# How the 500-nit limit works, and how it was lifted

Notes from reading `drivers/gpu/drm/apple` in
[omacom/linux-aurora](https://github.com/omacom/linux-aurora) (7.2.11,
`integration/aurora`), the m1n1 DCP protocol definitions, and experiments on a
MacBook Pro 14" M1 Pro (`apple,j314s`, DCP firmware 13.5, Omarchy 4.5.0).

Evidence is labelled: **measured** means read back from the firmware or the
kernel, **by eye** means a person watched the screen (no light meter was
used), **untested** means exactly that.

## Summary

1. The limit was in the request, not the hardware. The driver only sends
   brightness values from a table that ends at 510 nits, and the device tree
   says 500. **By eye**, the backlight keeps getting brighter well beyond that
   when the request is larger, up to the top of what the firmware accepts.
2. The request field is a small float, not a DAC code, so extending the range
   is arithmetic, not new reverse engineering (**measured**, below).
3. HDR is a different problem and is **not solved**: surfaces tagged as PQ are
   decoded differently by the firmware, but the picture is not usable.

## The brightness request

The backlight level travels in each frame submission (swap, tag `A408`) as
`bl_value`, with `bl_power = 0x40` for on. The driver converts nits to
`bl_value` with two lookup tables captured from macOS traces (`dcp_backlight.c`);
the last entry is `0x7fe07fc0`.

The value is a float: the top 4 bits are an exponent `e`, the low 28 bits a
fraction `f / 2^28`, and

    nits = 2^(e + 1) * (1 + f / 2^28)

The firmware logs the level it reached in an `mNits` event (the driver clamps
it to the device tree maximum before exposing it). Overriding the request and
reading that event, **measured**:

| `bl_value` | firmware-reported nits | iDAC |
| --- | --- | --- |
| `0x7a4d3c25` | 420.827 | 49168 |
| `0x7f7d5f50` (driver's "500") | 503.835 | 59538 |
| `0x81300000` | 550.000 | 65404 |
| `0x84000000` | 640.000 | 65405 |
| `0x90000000` | 1024.000 | 65405 |
| `0xa0000000` | 1600.000 | 65405 |

The reported nits echo the request up to 1600, where the firmware caps it.
From 550 nits up the iDAC code stops moving and the firmware logs
`BrightnessLCD.cpp:803: nitsToDBV: iDAC out of range`. That message and that
code do **not** describe what the panel does: **by eye**, the screen kept
getting brighter at every step of 550, 640, 1024 and 1600, and the top looked
like the panel's full range. Our first conclusion, that 550 nits is the
ceiling, was wrong for that reason. The mini-LED backlight is probably driven
by a separate local-dimming path that the DAC code does not describe; this was
not investigated.

What was not measured: actual luminance at any level, power draw, panel
temperature, or whether 1600 is held or throttled.

## What the driver would need

- Replace the nits-to-`bl_value` tables with the float encoding above (the
  table is that curve, sampled).
- Let `apple,max-brightness` and `props.max_brightness` go above 500 for
  mini-LED panels (`dcp_backlight.c` caps at 509; `dcp.c` caps J313 at its own
  value).
- Decide a safe default ceiling. Apple rates this panel for 1000 nits sustained
  over the whole screen and 1600 for highlights.
- Answer the firmware's temperature query properly. DCP asks the OS for
  `PMUS/Temperature` on mini-LED panels and `dcpep_cb_get_uint_prop` answers
  `3029` every time (a TODO in the source). Whatever thermal protection the
  firmware derives from it is blind under Linux, which matters at high levels.

## HDR: what was tried

`struct dcp_surface` carries `xfer_func` and `colorspace`. The driver sends 13
(sRGB) and 12 (native) for RGB surfaces and defines 16 (CICP code for ST 2084
PQ) and 9 (BT.2020) without using them. The connector has no `Colorspace` or
`HDR_OUTPUT_METADATA` property.

Hyprland 0.56 can render PQ/BT.2020 at 10 bits on this panel (a monitor rule
with `cm = "hdr"`, `bitdepth = 10`, `supports_hdr = 1`; the output format
becomes `XRGB2101010`). With the probe module re-tagging those frames
`xfer_func = 16, colorspace = 9`, the firmware accepts them without errors, but
the picture is not usable, **by eye**:

| desktop white placed at | seen |
| --- | --- |
| 5 nits | washed out |
| 10 to 30 nits | mostly white, slight variation |
| 50 nits | washed out, bright |
| 100 to 1000 nits | about 99% white, only pure black visible |

No step looked normal. Tag 16 clearly changes the firmware's decode and makes
the panel brighter, but not into a correct PQ picture. Possible causes, not
separated: the firmware normalises by a reference white that macOS sets and
Linux leaves at zero (about 740 bytes of the swap record are unmapped, and a
per-swap headroom field could be among them); or tag 16 is not plain PQ on
this firmware. HLG (tag 18) changed nothing. Next step would be a calibrated
grey ramp and a systematic search of the unmapped swap bytes.

## Method

`probe/apple_xdr_probe.c` hooks `dcp_push` with a kprobe and edits the swap
request in place, with offsets generated from the running kernel's BTF
(`gen-layout.sh`), so it needs no driver rebuild and refuses other firmware
versions. It also hooks the firmware's property and `mNits` callbacks to read
back what the firmware reports. Overrides expire on their own. The raw logs of
the first experiment are in `docs/experiments/`.
