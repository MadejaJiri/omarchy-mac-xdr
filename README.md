# omarchy-mac-xdr

Full dynamic range for the built-in mini-LED display of Apple Silicon MacBook
Pros running [Omarchy](https://omarchy.org) on the Asahi-derived
`linux-aurora` kernel: a desktop brighter than the 500-nit SDR limit, and
correct HDR playback.

**Status: brightness works.** The ordinary brightness control can be stretched
over a range you choose, from 2 nits up to the panel's 1600-nit peak, with a
top-bar widget to set it and an option to apply it at every login. Levels are
judged by eye, not measured with a meter. HDR content is not working yet.

## Install

```bash
bin/install
```

Builds an Arch package and installs it with pacman (asks for sudo). You get
**XDR Screen Control** in the app launcher, the `omarchy-xdr` command, and a
kernel module that is built for the running kernel and loaded at every boot
(`omarchy-xdr-probe.service`). `sudo pacman -R xdr-screen-control` removes it.

The app: the switch at the top turns the extended range on or off.
**Maximum** is what 100% brightness means (500 to 1600 nits). **Minimum** is
the bottom of the scale: at *Off* the lowest brightness step switches the
backlight off, as on macOS; otherwise it is that many nits. **Show icon in the
top bar** adds or removes the bar widget, and **Apply at login** restores your
choice after a reboot. The brightness keys and slider keep working as before,
now across the range you chose. The same from a terminal:
`omarchy-xdr enable | disable | max 1200 | min 0 | bar on | autostart on | status`.

Apple holds this panel to 1000 nits sustained full-screen and uses 1600 only
for HDR highlights, and under Linux the firmware does not get a real panel
temperature. Running the whole screen above 1000 nits for long is at your own
risk.

Linux on these machines stops at about 500 nits. The panel reaches 1000 nits
sustained and 1600 peak, but only through the display coprocessor's HDR path,
which the kernel driver does not use and nobody has documented.
[docs/research.md](docs/research.md) explains where the limit is and what has
to be found out. This repository currently holds the tool for finding out.

## What is here

| Path | What it is |
| --- | --- |
| `probe/` | `apple_xdr_probe`, a kernel module that hooks the running display driver. It can rewrite the backlight request, retag surfaces as PQ/BT.2020, patch unknown bytes of a frame submission, and read back the brightness the firmware reports. No driver rebuild, no reboot, no compositor restart. |
| `bin/xdr-probe` | Builds, loads and drives the module. |
| `bin/xdr-experiment` | Runs the first two experiments step by step and logs what the firmware reported and what you saw. |
| `bin/omarchy-xdr` | Saved brightness-range preference and the command that applies it. |
| `src/`, `xdr-screen-control.pro` | The XDR Screen Control app (Qt Quick). |
| `plugin/xdr.brightness/` | The top-bar widget. |
| `pkgbuild/` | Arch package: app, command, module sources, services, launcher entry. |
| `system/` | Boot-time module loader and systemd units. |
| `bin/xdr-hdr-test` | Hyprland HDR output with PQ-tagged frames at chosen SDR white levels. |
| `docs/research.md` | Findings so far, open questions, experiment log. |

Tested hardware: MacBook Pro 14" M1 Pro (`apple,j314s`), DCP firmware 13.5,
`linux-aurora` 7.2.11. The module refuses other firmware versions: it patches
driver structures by offset, read from the running kernel's BTF at build time.

## Running the probe

Needs `base-devel`, `pahole` and the headers of the running kernel
(`linux-aurora-headers`).

```bash
bin/xdr-probe load
```

```bash
bin/xdr-probe status
```

```bash
bin/xdr-probe set bl_raw=0x90000000
```

```bash
bin/xdr-probe unload
```

Overrides expire 20 seconds after the last write (`hold_ms`) and the panel
goes back to the level the driver last asked for.

**Risks.** A frame submission the firmware rejects can crash the display
coprocessor; the screen freezes until reboot. The module writes nothing to
disk, so a reboot undoes everything, but save your work first. Driving the
backlight far above 500 nits for long periods is outside what Linux has ever
done on this hardware, and the driver currently reports a constant panel
temperature to the firmware; see the Safety section of the research notes.

## Plan

1. Establish with the probe how DCP is asked for more than SDR brightness.
2. Turn that into a proper `appledrm` patch: HDR connector properties, PQ and
   extended-range surfaces, a backlight range above 500 nits, real panel
   temperature for the firmware.
3. Omarchy integration: an XDR brightness toggle and brightness keys that
   continue past 500 nits, plus Hyprland HDR configuration for HDR content.

## Licence

The kernel module is GPL-2.0-only, like the driver it hooks; the app and
scripts are MIT. See `LICENSE`.
