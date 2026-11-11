# Where the 500-nit limit lives

Notes from reading `drivers/gpu/drm/apple` in
[omacom/linux-aurora](https://github.com/omacom/linux-aurora) (7.2.11,
`integration/aurora`) and the m1n1 DCP protocol definitions, on a MacBook Pro
14" M1 Pro (`apple,j314s`) with DCP firmware 13.5.

Everything under "Established" was read from source or from the running
system. Everything under "Open" is a hypothesis the probe exists to test.

## Established

**The panel is driven by a coprocessor.** The display, its mini-LED backlight
and local dimming are run by DCP, an RTKit firmware loaded by iBoot. Linux
does not program the backlight; it sends DCP a *swap* (a frame presentation)
that may carry a backlight request, and DCP does the rest.

**Three separate things hold brightness at ~500 nits.**

1. Device tree: `panel { apple,max-brightness = <500>; }`.
2. Driver: `props.max_brightness = min(dcp->brightness.maximum, 509)`
   (`dcp_backlight.c`), so raising the device tree value alone gives 509.
3. The request encoding itself. The driver converts nits to the swap's
   `bl_value` with a lookup table captured from macOS traces. The table covers
   2..510 nits and its last entry is `0x7fe07fc`, sent as `0x7fe07fc0`: within
   0.1% of `S32_MAX`. macOS was never seen to send more, because macOS never
   asks for more than ~500 nits *through this field* either.

Point 3 is the important one. There is no "slightly higher number" to send
within the known range: the SDR backlight request is at the top of its scale
at 510 nits. Whether values above `0x7fffffff` mean anything is unknown.

**macOS gets past 500 nits through the HDR path, not the backlight slider.**
On the XDR panel, SDR white tops out at 500 nits and everything above it is
EDR: surfaces carry values brighter than SDR white and DCP raises the
backlight zones to show them (1000 nits sustained, 1600 peak). "Brighter
desktop" tools on macOS work by putting SDR content into that EDR range.

**The driver has the surface vocabulary for HDR but never uses it.**

- `struct dcp_surface` has `xfer_func` and `colorspace`. RGB planes always get
  `xfer_func = 13`, `colorspace = 12`. The values match CICP code points
  (13 = sRGB, 12 = P3-D65), and `DCP_XFER_FUNC_HDR = 16` (CICP 16 = ST 2084 PQ)
  and `DCP_COLORSPACE_BG_BT2020 = 9` are defined but unused for RGB.
- `DRM_FORMAT_ABGR16161616F` maps to DCP's half-float format, the format
  macOS uses for EDR surfaces. `ARGB2101010` is supported too.
- The connector exposes no `HDR_OUTPUT_METADATA` or `Colorspace` property, so
  no compositor can ask for HDR today.
- Colour mode selection only ever picks an SDR-gamma mode
  (`set_digital_out_mode ... eotf:0`).

**Large parts of the swap record are unmapped.** `unk_110[48]`,
`unk_190[312]`, `unk_2cc[20]`, `unk_2f3[45]`, `unk_320[319]`: about 740 bytes
the driver zero-fills. EDR headroom, if it is passed per swap, is in there.

**The firmware reports the level it reached.** DCP publishes property 15
(nits x `Brightness_Scale`, 65536) after a backlight change. The driver clamps
it to the device tree maximum before exposing it as `actual_brightness`, so a
level above 500 is invisible through sysfs. The probe reads it unclamped.
This is the measurement used below: no light meter needed.

**The firmware's thermal input is a constant.** DCP asks the OS for
`PMUS/Temperature` on mini-LED panels; the driver answers `3029` every time
(`iomfb_template.c`, marked TODO). Whatever protection DCP derives from panel
temperature is therefore blind under Linux. This matters once the backlight
runs far above 500 nits for long periods. See "Safety".

**Asahi upstream has not done this.** HDR is listed as not yet supported; the
February 2026 progress report says the DCP driver needs cleanup before HDR is
shippable. There is no published description of the EDR fields.

## Open questions, in the order the probe tests them

1. **Does `bl_value` above `0x7fffffff` raise the backlight?** If the field is
   an unsigned scale that macOS simply never exceeds, this alone gives a
   brighter SDR desktop and the fix is a longer table. Test: `bl_raw` at
   `0x80000000`, `0x90000000`, ... and read property 15.
2. **Does DCP decode a PQ surface?** Retag the desktop surface `xfer=16`,
   `cspace=9`. An sRGB desktop read as PQ turns very bright and washed out if
   the firmware honours it (sRGB mid-grey lands near 100 nits, white at the
   panel peak). That is ugly but unambiguous, and property 15 says how far the
   backlight went.
3. **How is the SDR white level set in PQ mode?** Whether `bl_value` still
   scales the picture or PQ is absolute decides how the brightness keys must
   work.
4. **Do half-float surfaces with values above 1.0 show as EDR** without any
   further swap fields? Needs a compositor path; later.
5. **Which unknown swap bytes carry headroom**, if 2 and 4 fail without them.

## Safety

- The probe's overrides expire 20 s after the last write and the driver's own
  level is re-sent. A black or blinding screen recovers without input.
- A swap the firmware rejects can crash DCP. The screen then freezes until
  reboot; nothing is written to disk by the probe, so a reboot is a full
  recovery. Save work before experimenting.
- Apple's own limits for this panel are 1000 nits sustained full-screen and
  1600 nits peak on part of the screen. Nothing here should hold the whole
  panel above 1000 nits, and long runs above 500 should feed DCP a real
  temperature (`temp` parameter) instead of the driver's constant.

## Experiment log

### 2026-11-12, j314s, firmware 13.5 (`docs/experiments/20261112-014059.log`)

**`bl_value` is a logarithm of nits, and its range ends just above today's
maximum.** Overriding the request and reading the firmware's own report
(`actual_brightness`, valid below the 500 clamp):

| asked (formula below) | firmware reported |
| --- | --- |
| 100 | 105 |
| 200 | 210 |
| 300 | 314 |
| 400 | 420 |

So `bl_value = log2(nits / 2) * 2^28` to within 5%: Q4.28 fixed point, not a
DAC code. The lookup table in the driver is this curve sampled from traces.

Above the table the firmware logs `BrightnessLCD.cpp:803: nitsToDBV: iDAC out
of range`. Bisecting on that message: `0x81300000` is accepted,
`0x81310000` is out of range. The driver's maximum today is `0x7f7d5f50`, so
the plain backlight path has `2^((0x81300000 - 0x7f7d5f50) / 2^28)` = 1.076,
under 8%, of headroom left. Out-of-range requests looked brighter than the
normal maximum to the eye, but by how much is not measured yet; the firmware
most likely clamps at the top of the range.

Conclusion: question 1 is answered. A longer table buys a few percent. It is
not the way to 1000 nits.

**DCP decodes PQ surfaces on the built-in panel.** Retagging the unchanged
sRGB desktop as `xfer_func = 16`:

| tag | seen |
| --- | --- |
| PQ + BT.2020 | washed out, nearly all white, clearly brighter than SDR maximum |
| PQ + native primaries | same |
| HLG (18) + BT.2020 | no change |
| PQ + BT.2020, backlight request at table top | washed out, bright |

That is what an sRGB signal read as PQ should look like, so the firmware
honours transfer function 16 with no further swap fields and no HDR colour
mode. HLG is ignored or unsupported. No firmware errors were logged.

Conclusion: question 2 is answered yes. The route to XDR brightness is PQ
surfaces, with the compositor placing SDR white wherever the user wants it.

Still open: how bright PQ white actually gets (peak and sustained), whether
the backlight request still scales a PQ picture (question 3), and the thermal
behaviour.

### 2026-11-12, second session: unclamped firmware readback

The probe now hooks the firmware's `mNits` events (level reached, iDAC code).

| `bl_value` | firmware nits | iDAC |
| --- | --- | --- |
| `0x7a4d3c25` | 420.827 | 49168 |
| `0x7f7d5f50` (driver's "500") | 503.835 | 59538 |
| `0x81300000` | 550.000 | 65404 |
| `0x84000000` | 640.000 | 65405 |
| `0x90000000` | 1024.000 | 65405 |
| `0xa0000000` | 1600.000 | 65405 |

- The encoding is exact, not approximate: the top 4 bits are an exponent `e`
  and the low 28 a fraction `f`, and `nits = 2^(e+1) * (1 + f)`. (The earlier
  log2 reading was this curve seen through too few points.) Every row above
  fits to three decimals.
- The backlight DAC saturates at 65405, which is 550 nits. Above that the
  firmware echoes the request (capped at 1600) but the iDAC code does not
  move. **The SDR backlight ceiling of this panel is 550 nits**; the driver
  stops at 504. That is 9% the driver could offer with no HDR at all.
- `mNits` reports the backlight setting only. It does not change while PQ
  surfaces are shown, so it cannot measure HDR output.

**Hyprland renders PQ and the firmware accepts it.** A runtime rule
(`bitdepth = 10, cm = "hdr", supports_hdr = 1, sdr_max_luminance = N`) switches
the output to `XRGB2101010` with a PQ/BT.2020 picture; with the probe tagging
those frames `xfer_func = 16, colorspace = 9` the firmware took about 70
frames a second for 25 seconds with SDR white stepped from 203 to 1000 nits,
logged no errors, and went back to SDR cleanly. `bin/xdr-hdr-test` repeats
this. What it looked like is recorded below once observed.
