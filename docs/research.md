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

Nothing run yet. `bin/xdr-experiment` writes its logs to `docs/experiments/`.
