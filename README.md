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

The panel targets are cross-compiled for Linux/ARM, so **on Windows you build
inside WSL** (Ubuntu/Debian); the `native` target also builds on any Linux host.
Every build is **fully static** (the build script verifies it), so the target
device needs no libraries installed. The vendored dependencies (`libhv`,
`wpa_supplicant`) are compiled from source into static libraries by the build
and `fmt` is header-only, so **no dependency `-dev` packages are required** —
that is the point of the self-contained tree.

### 1. A Linux build host (Windows -> WSL)

In an elevated PowerShell:

```
wsl --install -d Ubuntu
```

Open the Ubuntu shell; every command below runs there. (On native Linux, skip
this step.)

### 2. Base build tools (all targets)

```
sudo apt-get update
sudo apt-get install -y build-essential make file binutils
```

`build-essential` supplies the host `gcc` / `g++` for the `native` target and
the C++17 toolchain; `make`, `file` and `binutils` (`readelf`) drive the build
and its verification. For the optional SDL desktop preview (`GUPPY_SDL=1`, runs
the UI in a window on the host) also install `libsdl2-dev pkg-config`.

### 3. Cross toolchain, per target

| Target | Runs on | Cross-compiler apt packages |
|---|---|---|
| `native` | the build host (x86-64 Linux / WSL) | none beyond `build-essential` |
| `armv7` | generic 32-bit ARM (ARMv7-A) | `gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf` |
| `sunxi` | Allwinner sunxi (disp2) panel — e.g. Elegoo Centauri / C2 | same as `armv7` |
| `aarch64` | 64-bit ARM (Pi Zero 2 W / 3 / 4 / 5) | `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu` |
| `armv6` | Pi 1 / Zero / Zero W (ARMv6 + VFPv2) | `gcc-12-arm-linux-gnueabihf g++-12-arm-linux-gnueabihf` **+ a sysroot (step 4)** |

Install only the targets you need, e.g. for the Centauri / C2 (`sunxi`) build:

```
sudo apt-get install -y gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

The 32-bit ARM binutils (`arm-linux-gnueabihf-ar`, `-readelf`, `-strip`, …) come
with those packages; the `armv6` build also uses them.

### 4. ARMv6 only — the sysroot

`-march=armv6` alone is **not** enough: Debian/Ubuntu armhf libraries are built
for ARMv7-A, so a static link pulls ARMv7 instructions in and the program dies
with `SIGILL` on an ARMv6 core. The `crossbuild/rpi-armv6-cc` wrapper therefore
compiles against a **Raspberry Pi OS (Raspbian) _bookworm_ armhf sysroot** and
links that sysroot's own ARMv6 `libc` / `libgcc` / `libstdc++` — which are
**gcc-12**'s, hence the `gcc-12-arm-linux-gnueabihf` packages above (the distro's
default gcc-13 cannot compile the sysroot's gcc-12 C++ headers).

Obtain a bookworm armhf sysroot (either way), a directory holding `lib/` and
`usr/`:

```
# from a running Pi (Raspberry Pi OS bookworm, 32-bit):
rsync -aL --delete pi@<pi-host>:/lib  /path/to/pi-sysroot-armhf/lib
rsync -aL --delete pi@<pi-host>:/usr  /path/to/pi-sysroot-armhf/usr
# or extract /lib and /usr from a Raspberry Pi OS Lite (bookworm, 32-bit) image.
```

It must contain `usr/lib/gcc/arm-linux-gnueabihf/12/libstdc++.a` and
`usr/lib/arm-linux-gnueabihf/libc.a` (the wrapper checks for these). Point the
build at it:

```
export PI_SYSROOT_ARMV6=/path/to/pi-sysroot-armhf
```

### 5. Build

From the repo root, one target at a time — each builds into its own
`build/<target>/` and is verified (static, no interpreter, correct ARM
architecture tag):

```
bash crossbuild/build.sh native
bash crossbuild/build.sh armv7
bash crossbuild/build.sh aarch64
bash crossbuild/build.sh sunxi        # Centauri / C2: sunxi framebuffer, no VNC, small draw buffer
PI_SYSROOT_ARMV6=/path/to/pi-sysroot-armhf bash crossbuild/build.sh armv6
```

The binary lands at `build/<target>/bin/grumpyscreen`. Anything after the target
is passed through to `make` (e.g. a `clean`, or an option override).

### 6. Build options (make variables)

- `GUPPY_NO_VNC=1` — leave the built-in VNC server out (smaller; panel-only builds).
- `MMU_BACKENDS="afc hh"` — MMU vendor drivers (default AFC + Happy Hare; `""` for none).
- `GUPPY_SMALL_SCREEN=1` — the 480x272 asset set and touch calibration.
- `GUPPY_SUNXI_FB=1` — the Allwinner disp2 framebuffer path (the `sunxi` target sets it).
- `GUPPY_DRAW_BUF_PX=<pixels>` / `GUPPY_DRAW_BUF_SINGLE=1` — LVGL draw-buffer size / single buffer, for tight RAM.
- `GUPPY_SDL=1` — build the desktop preview against SDL2 (needs `libsdl2-dev`).
- `COSMOS=<name>` — OpenCentauri Cosmos button text.

A changed flag needs a clean build — `make` does not track flag changes.

> The upstream Docker build (`build.sh`, using the `pellcorp/grumpydev` image)
> is kept from grumpyscreen but is **not** the path here: this fork builds
> self-contained with the toolchain above, no Docker image required.

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
