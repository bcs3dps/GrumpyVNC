#!/usr/bin/env bash
# ----------------------------------------------------------------------------
#   build.sh - build GrumpyScreen for one target, then verify the binary
#
#   Usage, from anywhere (paths resolve relative to this script):
#
#       bash crossbuild/build.sh native            # this host (x86-64)
#       bash crossbuild/build.sh armv7             # generic 32-bit ARM, ARMv7-A
#       bash crossbuild/build.sh aarch64           # Pi Zero 2 W / 3 / 4 / 5, 64-bit OS
#       PI_SYSROOT_ARMV6=<sysroot> bash crossbuild/build.sh armv6
#                                                  # Pi 1 / Zero / Zero W, 32-bit OS
#       bash crossbuild/build.sh sunxi             # an Allwinner sunxi (disp2) panel
#                                                  # driven directly: ARMv7-A built with
#                                                  # GUPPY_SUNXI_FB=1 (FBDEV_SUNXI in the
#                                                  # framebuffer driver), no VNC, and one
#                                                  # small draw buffer
#
#   Anything after the target is passed to make unchanged (for example
#   MMU_BACKENDS="" or a single make target such as clean), after the
#   target's own make variables, so it overrides them: the sunxi build with
#   upstream's two large draw buffers is
#       bash crossbuild/build.sh sunxi GUPPY_DRAW_BUF_PX=131072 GUPPY_DRAW_BUF_SINGLE=
#   (a changed value needs a clean build: make does not track flags).
#
#   It drives the tree's own Makefile, which already takes a CROSS_COMPILE
#   prefix and passes it to the libhv and wpa_supplicant sub-builds.  What
#   this script adds is one BUILD_DIR and one DEPS_KEY PER TARGET -- the
#   Makefile's object directory is not keyed by architecture, so two targets
#   sharing it would link each other's objects -- plus the ARMv6 toolchain
#   and a check of every result.
#
#   ARMv6 needs a Raspberry Pi OS (Raspbian) bookworm armhf sysroot, named by
#   PI_SYSROOT_ARMV6; rpi-armv6-cc beside this script says why a -march flag
#   alone cannot do it.  The script never names a path outside the tree.
#
#   Exit status: 0 built and verified; 1 the build failed; 2 bad usage or a
#   missing toolchain / sysroot; 3 built, but the verification failed.
# ----------------------------------------------------------------------------

set -u

# ----------------------------------------------------------------------------
#   Locations: this script's folder, and the GrumpyScreen tree above it.
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TREE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ----------------------------------------------------------------------------
#   The target, and the settings that follow from it: the CROSS_COMPILE
#   prefix, the per-target build and dependency directories, the readelf that
#   can read the result, and the ARM architecture tag the binary must carry
#   (empty where there is no ARM attribute section to check).
# ----------------------------------------------------------------------------
if [ $# -lt 1 ]; then
    # No target given: say how to call it.
    echo "usage: bash crossbuild/build.sh native|armv7|aarch64|armv6|sunxi [make arguments...]" >&2
    exit 2
fi
TARGET="$1"
shift

# Each target builds into its own directory and keeps its own dependencies.
BUILD_DIR="build/$TARGET"
DEPS_KEY="$TARGET"

# Make variables a target adds of its own (none, for most).
MAKE_VARS=()

case "$TARGET" in
    native)
        # This host's own compiler: no prefix.
        CROSS=""
        READELF="readelf"
        WANT_ARCH=""
        ;;
    armv7)
        # The distribution's armhf cross compiler: ARMv7-A baseline.
        CROSS="arm-linux-gnueabihf-"
        READELF="arm-linux-gnueabihf-readelf"
        WANT_ARCH="v7"
        ;;
    aarch64)
        # The distribution's 64-bit ARM cross compiler.
        CROSS="aarch64-linux-gnu-"
        READELF="aarch64-linux-gnu-readelf"
        WANT_ARCH=""
        ;;
    armv6)
        # The armhf cross compiler wrapped onto the ARMv6 sysroot (below).
        CROSS=""
        READELF="arm-linux-gnueabihf-readelf"
        WANT_ARCH="v6"
        ;;
    sunxi)
        # The armhf cross compiler, as armv7, with the sunxi framebuffer code
        # switched on for a panel on an Allwinner disp2 display engine -- and
        # the VNC support left out (GUPPY_NO_VNC): this build drives the
        # panel it runs on, so the RFB server would only cost space.  RAM is
        # short on such a printer, so LVGL gets ONE draw buffer of 15960
        # pixels -- 30 rows of a 532-pixel panel, a tenth of its 300 rows,
        # LVGL's own recommended minimum -- about 62 KB where upstream's two
        # of 128K pixels take 1 MB.
        CROSS="arm-linux-gnueabihf-"
        READELF="arm-linux-gnueabihf-readelf"
        WANT_ARCH="v7"
        MAKE_VARS=(GUPPY_SUNXI_FB=1 GUPPY_NO_VNC=1 GUPPY_DRAW_BUF_PX=15960 GUPPY_DRAW_BUF_SINGLE=1)
        ;;
    *)
        # Anything else is a typo, not a new target.
        echo "build.sh: unknown target '$TARGET' (native, armv7, aarch64, armv6 or sunxi)" >&2
        exit 2
        ;;
esac

# ----------------------------------------------------------------------------
#   ARMv6 toolchain.  The sysroot must be named; then a tool directory under
#   the target's build directory gets the prefix names the Makefiles call:
#   <prefix>gcc and <prefix>g++ are the wrapper, every binutils name is the
#   distribution's own arm-linux-gnueabihf tool (binutils do not care which
#   ARM architecture version they handle).
# ----------------------------------------------------------------------------
if [ "$TARGET" = "armv6" ]; then
    if [ -z "${PI_SYSROOT_ARMV6:-}" ]; then
        # The wrapper would refuse every call; stop before make starts.
        echo "build.sh: armv6 needs PI_SYSROOT_ARMV6 set to a Raspberry Pi OS bookworm armhf sysroot" >&2
        exit 2
    fi
    export PI_SYSROOT_ARMV6

    TOOL_DIR="$TREE_DIR/$BUILD_DIR/toolbin"
    mkdir -p "$TOOL_DIR"
    ln -sf "$SCRIPT_DIR/rpi-armv6-cc" "$TOOL_DIR/rpi-armv6-gcc"
    ln -sf "$SCRIPT_DIR/rpi-armv6-cc" "$TOOL_DIR/rpi-armv6-g++"
    for TOOL in ar as ld nm objcopy objdump ranlib readelf strip; do
        # One binutils name per iteration, pointed at the distribution's tool.
        REAL="$(command -v "arm-linux-gnueabihf-$TOOL")"
        if [ -z "$REAL" ]; then
            # The armhf binutils are part of the cross toolchain this needs.
            echo "build.sh: arm-linux-gnueabihf-$TOOL not found" >&2
            exit 2
        fi
        ln -sf "$REAL" "$TOOL_DIR/rpi-armv6-$TOOL"
    done
    CROSS="$TOOL_DIR/rpi-armv6-"
fi

# ----------------------------------------------------------------------------
#   The build: the tree's Makefile, one target's directories, all cores.  A
#   native build passes no CROSS_COMPILE at all, because the Makefile tests
#   whether it is defined, not whether it is empty.
# ----------------------------------------------------------------------------
if [ -n "$CROSS" ]; then
    # Cross build: the prefix goes to make and on to the sub-builds.
    make -C "$TREE_DIR" CROSS_COMPILE="$CROSS" BUILD_DIR="$BUILD_DIR" \
         DEPS_KEY="$DEPS_KEY" "${MAKE_VARS[@]}" -j"$(nproc)" "$@"
else
    # Native build.
    make -C "$TREE_DIR" BUILD_DIR="$BUILD_DIR" DEPS_KEY="$DEPS_KEY" "${MAKE_VARS[@]}" -j"$(nproc)" "$@"
fi
RC=$?
if [ "$RC" -ne 0 ]; then
    # make has already said why.
    echo "build.sh: $TARGET build failed (make exit $RC)" >&2
    exit 1
fi

# ----------------------------------------------------------------------------
#   Verification.  Skipped when make was asked for something other than the
#   default build (clean, a dependency alone), since there is no binary then.
#   Otherwise: the binary exists, is fully static (no dynamic section, no
#   program interpreter -- so the target's glibc version cannot matter), and
#   on 32-bit ARM carries the architecture tag the target needs.  The tag is
#   the proof for ARMv6: a linker records the HIGHEST architecture of every
#   object it linked, so a single ARMv7 object anywhere would show as v7.
# ----------------------------------------------------------------------------
if [ $# -gt 0 ]; then
    # A specific make target was asked for; nothing to verify.
    exit 0
fi

BIN="$TREE_DIR/$BUILD_DIR/bin/grumpyscreen"
if [ ! -f "$BIN" ]; then
    # make succeeded but left no program: treat as a failed verification.
    echo "build.sh: $BIN was not produced" >&2
    exit 3
fi

# Say what was built, then hold it to the static and architecture rules.
file "$BIN"
echo "size: $(stat -c %s "$BIN") bytes"

if "$READELF" -d "$BIN" 2>/dev/null | grep -q 'NEEDED'; then
    # A shared-library dependency means the target's libraries would matter.
    echo "build.sh: $BIN is not fully static (it has NEEDED entries)" >&2
    exit 3
fi
if "$READELF" -l "$BIN" 2>/dev/null | grep -q 'INTERP'; then
    # A program interpreter means the target must supply a dynamic loader.
    echo "build.sh: $BIN requests a program interpreter" >&2
    exit 3
fi

if [ -n "$WANT_ARCH" ]; then
    # 32-bit ARM: read the architecture tag and hold it to the target's.
    GOT_ARCH="$("$READELF" -A "$BIN" | sed -n 's/^ *Tag_CPU_arch: *//p')"
    "$READELF" -A "$BIN" | grep -E 'Tag_CPU_arch:|Tag_FP_arch:|Tag_ABI_VFP_args:'
    case "$GOT_ARCH" in
        "$WANT_ARCH"|"$WANT_ARCH "*)
            # The expected architecture.
            ;;
        *)
            # Wrong architecture: for ARMv6 this is the SIGILL case.
            echo "build.sh: Tag_CPU_arch is '$GOT_ARCH', expected '$WANT_ARCH'" >&2
            exit 3
            ;;
    esac
fi

# Built, static, and the right architecture.
echo "build.sh: $TARGET OK -> $BUILD_DIR/bin/grumpyscreen"
exit 0
