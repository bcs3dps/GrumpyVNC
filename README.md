# GrumpyVNC

A touch UI for Klipper 3D printers, with a **built-in VNC server** so the panel
can be viewed and driven remotely. GrumpyVNC is a self-contained fork of
[grumpyscreen](https://github.com/pellcorp/grumpyscreen) (itself a fork of
GuppyScreen) that adds:

- **`vnc/`** — a small RFB 3.8 (RFC 6143) server built into the UI, with a DES
  engine for standard VNC Authentication. Built in by default; `GUPPY_NO_VNC=1`
  builds without it.
- **`crossbuild/`** — an ARMv6 cross-build for the target panels.
- **Centauri / C2 integration** for the Elegoo Centauri class of printers.

It is **fully self-contained**: LVGL, lv_drivers, libhv, fmt and wpa_supplicant
are vendored in full (trimmed to the source the build uses), so it builds from a
single clone with **no git submodules and no network**. See
[`PROVENANCE.md`](PROVENANCE.md) for the upstream sources and `LICENSE` for terms
— the project is **GPL-3.0**, and each vendored component keeps its own licence
file.

## Features

:white_check_mark: Setup Wifi
:white_check_mark: Print Status
:white_check_mark: Extrude/Retract
:white_check_mark: Temperature Control
:white_check_mark: Fans/LED/Move Control
:white_check_mark: Print Fine Tune (Speed, Flow, Baby Stepping, Pressure Advance)
:white_check_mark: Start a Print from Usb or Reprint
:white_check_mark: Exclude Object
:white_check_mark: Console Logging
:white_check_mark: Spoolman Integration
:white_check_mark: MMU support (AFC, Happy Hare)
:white_check_mark: **Built-in VNC server (remote view/control of the panel)**

## Building

`make` builds the panel binary; the Makefile carries the options
(`MMU_BACKENDS`, `GUPPY_SMALL_SCREEN`, `GUPPY_SUNXI_FB`, `GUPPY_SDL` desktop
preview, `GUPPY_NO_VNC`, and the draw-buffer tuning). For the ARMv6 panel
target, build through `crossbuild/`. The vendored `libhv` and `wpa_supplicant`
are built into static libraries by the top-level `make`, so no system packages
are required.

## Credits

- [Grumpyscreen](https://github.com/pellcorp/grumpyscreen)
- [Guppyscreen](https://github.com/ballaswag/guppyscreen)
- [OpenKE](https://github.com/coreflake1/guppyscreen)
- [Material Design Icons](https://pictogrammers.com/library/mdi/)
- [Material Design WebFont](https://github.com/Templarian/MaterialDesign-Webfont)
- [Moonraker](https://github.com/Arksine/moonraker)
- [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen)
- [Fluidd](https://github.com/fluidd-core/fluidd)

Full upstream and dependency provenance is in [`PROVENANCE.md`](PROVENANCE.md).
