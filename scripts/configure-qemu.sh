#!/usr/bin/env bash
# Configure the prepared QEMU tree with a uv-managed Python, so the build
# never depends on whichever interpreter wins the host PATH race.
# Python version pinned in .python-version (QEMU 11.1 supports <= 3.13,
# and 3.14 only with a real distlib: see QEMU_PYTHON below).
#
# Usage: scripts/configure-qemu.sh [--windows] [extra configure flags...]
#
# --windows builds for Windows x86_64 with mingw-w64 into build/win/qemu
# instead of build/qemu, against the Rust staticlib built for
# x86_64-pc-windows-gnu, in MSYS2's MINGW64 shell on Windows, where it is
# implied (below). The cross build from Linux was retired (ADR-026). The
# two build directories are independent, so one checkout holds a Linux
# build and a Windows build at once.
#
# On an Apple Silicon Mac the Intel build (scripts/build.sh --x86_64,
# docs/build-macos.md "The Intel build") runs this whole script under
# Rosetta, and that is how it is recognised (sysctl.proc_translated): it
# configures into build/x86_64/qemu against the Rust staticlibs built for
# x86_64-apple-darwin and the libraries under build/deps/x86_64, with an
# x86_64 Python from uv, so uname, the compiler and meson all answer
# x86_64 without being told. On an Intel Mac nothing runs translated and
# the build is the plain native one.
#
# On Windows, in MSYS2's MINGW64 shell (docs/build-windows.md, "Building
# on Windows"): --windows is implied, and MSYS2's own Python is used
# rather than uv's. A python.org
# interpreter makes a venv with `Scripts\` where QEMU's configure looks for
# `bin/`.
#
# WIN_QEMU_CC=msvc (Windows) builds QEMU against MSVC's runtime instead of
# mingw's, into build/win/qemu-msvc (docs/build-windows.md, "QEMU under
# MSVC"): MSYS2's clang targeting x86_64-pc-windows-msvc with Visual
# Studio's headers, the UCRT and the static C runtime, linked by lld-link,
# against the libraries scripts/build-deps.sh builds for it and the Rust
# staticlibs for x86_64-pc-windows-msvc. The mingw build stays where it is.
#
# QEMU_PYTHON=<interpreter> uses that one and never consults uv. It is for
# a sandbox that has a suitable Python and cannot fetch one (the Flatpak:
# no uv in the SDK, no network during the build). Its version is checked,
# because the failure is obscure where it bites. QEMU 11.1's mkvenv
# supports 3.9-3.13, and 3.14 works only with the real `distlib`
# installed, since pip >= 26 trimmed the vendored copy mkvenv falls back
# to (3.14 with distlib configures and builds; MSYS2 has no older Python).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

WINDOWS=""; NATIVE=""
if [ "${1:-}" = "--windows" ]; then WINDOWS=1; shift; fi
case "${MSYSTEM:-}" in
  "") [ -z "$WINDOWS" ] || {
        echo "configure-qemu.sh --windows: in MSYS2's MINGW64 shell on Windows (ADR-026)"; exit 1; } ;;
  MINGW64) WINDOWS=1; NATIVE=1 ;;
  # UCRT64 and CLANG64 are other C runtimes and C++ libraries than the
  # package's msvcrt + libstdc++, so a build there would not be the build
  # that ships.
  *) echo "MSYS2 $MSYSTEM shell: build from the MINGW64 one (docs/build-windows.md)"; exit 1 ;;
esac

ROSETTA=""
[ "$(uname -s)" = Darwin ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && ROSETTA=1
MSVC=""
[ -z "$NATIVE" ] || [ "${WIN_QEMU_CC:-}" != msvc ] || MSVC=1
if [ -n "$MSVC" ]; then
  BUILD="${WIN_QEMU_BUILD:-$ROOT/build/win/qemu-msvc}"
  CARGO_TARGET=x86_64-pc-windows-msvc
elif [ -n "$WINDOWS" ]; then
  BUILD="${WIN_QEMU_BUILD:-$ROOT/build/win/qemu}"
  CARGO_TARGET=x86_64-pc-windows-gnu
elif [ -n "$ROSETTA" ]; then
  BUILD="$ROOT/build/x86_64/qemu"
  CARGO_TARGET=x86_64-apple-darwin
else
  BUILD="$ROOT/build/qemu"
  CARGO_TARGET=""
fi
# Paths that end up inside meson's files are read by native Windows
# programs, which cannot resolve MSYS2's /c/... form: C:/... there.
MROOT="$ROOT"
[ -n "$NATIVE" ] && MROOT="$(cygpath -m "$ROOT")"
LIBDISC_DIR="$MROOT/target${CARGO_TARGET:+/$CARGO_TARGET}/release"
# libsynth's staticlib lands in the same directory. It has its own variable
# because the two meson options are separate and either can point
# elsewhere.
LIBSYNTH_DIR="$LIBDISC_DIR"

PYVER="$(cat "$ROOT/.python-version")"
check_python() {  # the variable that named it, for the message
  command -v "$PYTHON" >/dev/null || { echo "$1=$PYTHON is not executable"; exit 1; }
  "$PYTHON" -c '
import sys
v = sys.version_info[:2]
if v == (3, 14):
    import distlib.scripts, distlib.version
elif not (3, 9) <= v <= (3, 13):
    sys.exit(1)
' 2>/dev/null || {
    echo "$1=$PYTHON is $("$PYTHON" -V 2>&1); QEMU 11.1 needs 3.9–3.13, or 3.14 with the distlib package"
    exit 1; }
}
if [ -n "${QEMU_PYTHON:-}" ]; then
  PYTHON="$QEMU_PYTHON"
  check_python QEMU_PYTHON
elif [ -n "$ROSETTA" ]; then
  # The usual uv interpreter is an arm64 binary, and meson takes the
  # machine it runs on for the build machine, so an arm64 Python would
  # configure an arm64 QEMU into the Intel build's directory. uv installs
  # an x86_64 build of the same version by its full name.
  command -v uv >/dev/null || { echo "uv not found, and the Intel build's meson needs an x86_64 Python from it"; exit 1; }
  X86PY="cpython-$PYVER-macos-x86_64-none"
  uv python install "$X86PY" --quiet --no-bin
  PYTHON="$(uv python find "$X86PY")"
  [ "$("$PYTHON" -c 'import platform; print(platform.machine())')" = x86_64 ] || {
    echo "$PYTHON is not an x86_64 interpreter (uv python install $X86PY)"; exit 1; }
elif [ -n "$NATIVE" ]; then
  PYTHON=/mingw64/bin/python3
  [ -x "$PYTHON.exe" ] || [ -x "$PYTHON" ] || {
    echo "no $PYTHON: pacman -S mingw-w64-x86_64-python mingw-w64-x86_64-python-distlib"; exit 1; }
  check_python "MSYS2's python"
else
  command -v uv >/dev/null || {
    echo "uv not found — install it (https://docs.astral.sh/uv/), or set QEMU_PYTHON to a 3.8–3.13 interpreter"; exit 1; }
  # By its full name, this machine's architecture included: once the
  # Intel build has installed uv's x86_64 3.12 beside the native one,
  # a bare "3.12" can answer with either, and an x86_64 interpreter
  # here makes meson run the compiler as x86_64 under Rosetta, which
  # then "cannot find" every arm64 archive (libdisc, first).
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) NATIVEPY="cpython-$PYVER-macos-aarch64-none" ;;
    Darwin-x86_64) NATIVEPY="cpython-$PYVER-macos-x86_64-none" ;;
    *) NATIVEPY="$PYVER" ;;
  esac
  # --no-bin: uv would otherwise drop a python3.12 on ~/.local/bin, and
  # the Intel build's x86_64 one shadowed the shell's python3.12 once.
  uv python install "$NATIVEPY" --quiet --no-bin
  PYTHON="$(uv python find "$NATIVEPY")"
fi
echo "==> python: $PYTHON ($("$PYTHON" -V 2>&1))"

# libdisc (the CD-ROM image model, libdisc/): a Rust staticlib linked into
# qemu-system-* and libqemu-embed-* for block/cdimage.c (patch 50). The crate
# has no QEMU dependency, so no cycle with the player.
# Under MSVC through scripts/cargo-msvc.sh: rustup's MSVC toolchain and
# the static C runtime, as the rest of that build.
CARGO=(cargo build); [ -z "$MSVC" ] || CARGO=("$ROOT/scripts/cargo-msvc.sh" build)
echo "==> cargo build --release -p libdisc${CARGO_TARGET:+ --target $CARGO_TARGET}"
if [ -n "$MSVC" ]; then
  (cd "$ROOT" && "${CARGO[@]}" --release -p libdisc)
else
  (cd "$ROOT" && cargo build --release -p libdisc ${CARGO_TARGET:+--target "$CARGO_TARGET"})
fi

# libsynth (the music engines, libsynth/): the same arrangement for
# hw/audio/opl3.c and hw/audio/mpu401.c (patch 60, doc 20). Also no QEMU
# dependency, so no cycle with the player.
echo "==> cargo build --release -p libsynth${CARGO_TARGET:+ --target $CARGO_TARGET}"
if [ -n "$MSVC" ]; then
  (cd "$ROOT" && "${CARGO[@]}" --release -p libsynth)
else
  (cd "$ROOT" && cargo build --release -p libsynth ${CARGO_TARGET:+--target "$CARGO_TARGET"})
fi

mkdir -p "$BUILD"
cd "$BUILD"
# --disable-werror: a pinned release trips new-toolchain warnings (glibc const strstr)
# --extra-cflags: vendored Khronos GL headers (third_party/khronos/README.md)
# -fPIC: objects are also linked into libqemu-embed-<target> (shared)
EXTRA_CFLAGS="-I$ROOT/third_party/khronos -fPIC"
CFG=(-Db_staticpic=true)
if [ -n "$NATIVE" ]; then
  # Windows, in MSYS2's MINGW64 shell. clang, not GCC (patch 68): mingw
  # GCC 15 has only emulated TLS, a call on every __thread access, and
  # QEMU makes several on every device access (one VGA register read took
  # 121.6 ns against clang's 64.3; Linux: 52.7). Against GCC's mingw
  # runtime, linked by lld, named through meson's CC_LD. No TCG plugins:
  # lld has no --dynamic-list, and nothing here loads a plugin.
  # WIN_QEMU_CC=gcc builds the old way.
  EXTRA_CFLAGS="-I$MROOT/third_party/khronos"
  # Only the libraries the package ships: MSYS2 may have more (zstd and
  # friends arrive as other packages' dependencies), and QEMU links
  # whatever it detects, so each is pinned off. These are the ones the
  # retired cross build had none of.
  CFG=(--disable-zstd --disable-gnutls --disable-nettle --disable-gcrypt --disable-capstone
       --disable-libusb --disable-usb-redir --disable-lzo --disable-snappy --disable-smartcard
       --disable-libcbor --disable-lzfse)
  if [ -n "$MSVC" ]; then
    # MSVC's ABI and runtime (WIN_QEMU_CC=msvc above): clang's GNU driver,
    # since QEMU's flags are GCC's, with Visual Studio's environment for
    # the headers and libraries, and lld-link. The libraries are ours,
    # built for the same runtime (build-deps.sh on Windows); MSYS2's are
    # mingw's. The static C runtime, as every MSVC binary here (ADR-026).
    . "$ROOT/scripts/msvc-env.sh" || exit 1
    DEPS="$MROOT/build/deps/x86_64-msvc"
    [ -f "$DEPS/lib/pkgconfig/glib-2.0.pc" ] || {
      echo "no $DEPS/lib/pkgconfig/glib-2.0.pc: scripts/build-deps.sh first"; exit 1; }
    export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"
    unset PKG_CONFIG_PATH
    export CC_LD=lld-link CXX_LD=lld-link
    CFG+=(--cc="clang --target=x86_64-pc-windows-msvc" --cxx="clang++ --target=x86_64-pc-windows-msvc"
          --disable-plugins --disable-bzip2 -Db_vscrt=mt
          # qemu-ga links two resource objects, which lld-link refuses;
          # no package ships the guest agent
          --disable-guest-agent)
    # Visual Studio's header and library directories go into the build
    # directory, so a plain `ninja -C build/win/qemu-msvc` works in any
    # shell, with no msvc-env.sh (clang finds no UCRT on its own here).
    # configure splits these flags on spaces, so each directory is
    # written as its 8.3 short name, and as a word of its own: MSYS2
    # rewrites the /PROGRA~1 inside a joined -idirafterC:/PROGRA~1 as a
    # POSIX path. -idirafter, not clang-cl's -imsvc (the GNU driver has
    # none): searched after patch 83's -isystem include/msvc, whose
    # #include_next then finds the UCRT's. Libraries as lld-link's own
    # -libpath:, through -Wl: meson takes a lone -L apart from its
    # directory, and in its own link checks passes -L on as -Wl,-L, which
    # lld-link ignores (a regeneration then finds no pathcch.lib).
    msvc_short() {
      local s; s="$(cygpath -m -s "$1")"
      case "$s" in *" "*)
        echo "configure-qemu.sh: '$1' has no 8.3 short name (fsutil 8dot3name query)" >&2; return 1 ;; esac
      printf '%s' "$s"
    }
    MSVC_CFLAGS="" MSVC_LDFLAGS="" d=""
    IFS=';' read -ra dirs <<< "$INCLUDE"
    for d in "${dirs[@]}"; do [ -z "$d" ] || MSVC_CFLAGS="$MSVC_CFLAGS -idirafter $(msvc_short "$d")" || exit 1; done
    IFS=';' read -ra dirs <<< "$LIB"
    for d in "${dirs[@]}"; do [ -z "$d" ] || MSVC_LDFLAGS="$MSVC_LDFLAGS -Wl,-libpath:$(msvc_short "$d")" || exit 1; done
    EXTRA_CFLAGS="$EXTRA_CFLAGS$MSVC_CFLAGS"
    CFG+=(--extra-cxxflags="${MSVC_CFLAGS# }" --extra-ldflags="${MSVC_LDFLAGS# }")
    echo "==> MSVC runtime, libraries: $DEPS (static)"
    # WHPX (Windows 11 guests under the host's hypervisor, track M20)
    # needs the Windows SDK's VMX capability codes, which arrived after
    # 10.0.22000 (mingw-w64's headers have them). WHPX is required, as
    # in the mingw build: an older SDK stops the build here (user).
    WHVDEFS="$(cygpath -u "${WindowsSdkDir:-}")/Include/${WindowsSDKVersion%\\}/um/WinHvPlatformDefs.h"
    if ! grep -q WHvCapabilityCodeVmxBasic "$WHVDEFS" 2>/dev/null; then
      echo "configure-qemu.sh: the Windows SDK ${WindowsSDKVersion%\\} has no WHvCapabilityCodeVmxBasic, which WHPX needs: install the Windows 11 SDK 10.0.26100 (Visual Studio Installer, Individual components)" >&2
      exit 1
    fi
    CFG+=(--enable-whpx)
  elif [ "${WIN_QEMU_CC:-clang}" = clang ]; then
    command -v clang >/dev/null && command -v ld.lld >/dev/null || {
      echo "no clang/lld: pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld"; exit 1; }
    export CC_LD=lld CXX_LD=lld
    CFG+=(--cc=clang --cxx=clang++ --disable-plugins)
  fi
elif [ "$(uname -s)" = Darwin ]; then
  # Every Mac build targets the floor, the oldest macOS the app runs on
  # (scripts/macos-floor.sh; build.sh exports the same value, and a preset
  # one wins). It goes in as a flag as well as
  # the environment. A changed flag changes every command line, so a
  # reconfigure recompiles the tree, where a changed environment alone
  # would keep the objects built for the old target.
  # -Werror=unguarded-availability-new goes with it, because an API newer
  # than the target used without an @available check makes a binary that
  # dies on the floor's macOS. Since the flag reaches meson's own checks, a
  # function detected through its real declaration is only found when the
  # target has it (patch 46: strchrnul, 15.4).
  if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    export MACOSX_DEPLOYMENT_TARGET="$("$ROOT/scripts/macos-floor.sh")"
  fi
  EXTRA_CFLAGS="$EXTRA_CFLAGS -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Werror=unguarded-availability-new"
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
  # The libraries are ours (scripts/build-deps.sh, docs/build-macos.md "The
  # libraries"): glib, pixman, libslirp and zstd built from source for this
  # architecture and the floor, as static archives under build/deps/<arch>.
  # pkg-config sees that prefix and the SDK's own .pc files and nothing
  # else, so no Homebrew library is found by accident (libpng, say, which
  # auto-detection would link and the app would then have to carry). The
  # prefix holds archives only, and its .pc files carry their private
  # link lines publicly (build-deps.sh), so a plain `pkg-config --libs`
  # is the whole static link. Not meson's prefer_static: QEMU's
  # meson.build turns that into `-static`, which macOS cannot link
  # ("library 'crt0.o' not found").
  DEPS="$ROOT/build/deps/$(uname -m)"
  [ -f "$DEPS/lib/pkgconfig/glib-2.0.pc" ] || {
    echo "no $DEPS/lib/pkgconfig/glib-2.0.pc: scripts/build-deps.sh first (scripts/build.sh runs it)"; exit 1; }
  export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$(xcrun --show-sdk-path)/usr/lib/pkgconfig"
  unset PKG_CONFIG_PATH
  # The TPM 2.0 of a Windows 11 box: libtpms and its libcrypto, ours too
  # (patch 75, track M20). Asked for, so a missing one fails here.
  CFG+=(--enable-libtpms)
  echo "==> libraries: $DEPS (static)"
elif [ "$(uname -s)" = Linux ] && [ "${QEMU_DEPS:-}" != system ]; then
  # Linux: QEMU on a glib of its own (scripts/build-deps.sh on Linux,
  # which scripts/build.sh runs). QEMU's main loop iterates glib's global
  # default GMainContext on QEMU's thread; sharing the process's glib, a
  # toolkit that runs on that context (GTK; Qt's glib event dispatcher)
  # would have its sources dispatched there (docs/tracks/m22-mitsuami-player.md, "Why QEMU links a GLib of its own").
  # glib and libslirp, the one other library QEMU links that links glib,
  # come static from build/deps/<arch> ahead of the distribution's .pc
  # files; the rest stays the distribution's. Their symbols are hidden, so
  # QEMU's calls bind inside libqemu-embed and the process's own glib never
  # sees them. Smartcard goes: libcacard links the system glib, and no
  # machine the launcher writes has a CCID device. QEMU_DEPS=system links
  # the distribution's glib instead.
  DEPS="$ROOT/build/deps/$(uname -m)"
  [ -f "$DEPS/lib/pkgconfig/glib-2.0.pc" ] || {
    echo "no $DEPS/lib/pkgconfig/glib-2.0.pc: scripts/build-deps.sh first (scripts/build.sh runs it), or QEMU_DEPS=system"; exit 1; }
  export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  # libtpms and its libcrypto (patch 75, the TPM 2.0 of a Windows 11
  # box, track M20) come the same way, hidden too: a process that loads
  # its own OpenSSL must not have QEMU's calls bind to it, or the reverse.
  HIDE=libglib-2.0.a:libgio-2.0.a:libgobject-2.0.a:libgmodule-2.0.a:libpcre2-8.a:libslirp.a:libtpms.a:libcrypto.a
  CFG+=(--disable-smartcard --enable-libtpms --extra-ldflags="-Wl,--exclude-libs,$HIDE")
  echo "==> glib, libslirp and libtpms: $DEPS (static, hidden)"
fi
# spice-protocol's headers, and nothing of SPICE's server: they are what
# `qemu-vdagent` builds with, the chardev that carries the clipboard to a
# guest agent (track M23, doc 24 §3). build-deps.sh puts them in the prefix
# on macOS and Linux; Windows hosts come later (M23 step 7).
if [ -n "$WINDOWS" ]; then
  CFG+=(--disable-spice-protocol)
else
  CFG+=(--enable-spice-protocol)
fi
# No QEMU user interface at all. The player is the front end. It embeds
# QEMU, the embed library appends `-display none` itself
# (embed/libqemu_embed.c), and it brings its own 3D context provider
# (patch 30) and audio backend (patch 20). Every display QEMU can build was
# dead code each packager still had to carry: SDL2 (and, through
# sdl2-compat, SDL3) beside the player on Windows and macOS, GTK and its
# pango/cairo/gdk chain in libqemu-embed on Linux, spice's server, curses.
# Turning them off costs nothing we use and takes ~40 libraries off the
# Linux embed library alone.
#
# VNC stays. It needs no toolkit, and it is the only way left to *look at*
# a guest under a hand-run `qemu-system-i386`. With no local display
# compiled in and no `-display` given, `qemu_setup_display()` starts a VNC
# server on localhost:5900 (system/vl.c). Anything scripted passes
# `-display none` and gets neither.
#
# The host audio backends go for the same reason. The player's audio is
# patch 20's `embed` audiodev, an SPSC ring the embedding application owns
# (docs/11); every machine the launcher writes says `audiodev=embed0`, and
# every headless tool says `audiodev=none`. ALSA, PulseAudio, PipeWire,
# JACK, OSS, sndio, CoreAudio and DirectSound were compiled in and linked,
# and none was ever opened. `none` and `wav` are built unconditionally
# (audio/meson.build) and `embed` is ours, so what the tree uses is
# untouched. `tools/xp-cdimage-test.sh` still captures CD-DA through
# `-audiodev wav`.
#
# This is *not* `--audio-drv-list=`. That list only sets the default
# priority order; the per-driver feature options below, auto-detected,
# are what pull the libraries in.
#
# The same goes for the rest of QEMU's optional features that no machine
# the launcher writes can reach. Networking: every bundle says `-netdev
# user` and nothing else, so slirp stays and AF_XDP and vde go. So does
# libbpf, whose one consumer is `hw/net/virtio-net.c`'s eBPF RSS steering,
# a device the launcher never writes (pcnet on 98, rtl8139 on XP). Block:
# every drive is a local file (a qcow2, a raw floppy or one of doc 17's
# disc images through our own `cdimage` driver), so the network-storage
# drivers go. curl and libssh go because this host has them, and
# iscsi/nfs/rbd/gluster/blkio are *pinned off* because another host might.
# Auto-detection makes the build depend on which libraries the machine
# happened to have, which is how the Flatpak and the Mac would end up with
# a different libqemu-embed from this box's. brlapi is a braille chardev
# nothing here opens. libpng and libjpeg go too: the one PNG QEMU can
# write is `screendump`'s `format: png`, and every tool here takes the
# PPM and converts it itself (tools/qmpc.py), while VNC's JPEG encoding
# serves a viewer nothing scripted opens. Both were two more libraries in
# every package for nothing.
# The targets: i386 for the era's machines, x86_64 for Windows 11, and on
# an Arm host aarch64 too, Windows 11 on Arm under the host's hypervisor
# (HVF on a Mac, KVM on Linux; track M20 step 4). An x86 host has no use
# for it: emulated, Windows on Arm is slower than x64 Windows emulated.
# A Mac builds no x86_64: it runs Windows 11 on Arm only (user decision
# 2026-10-01, track M20), and nothing there links that QEMU.
TARGETS=i386-softmmu
[ "$(uname -s)" = Darwin ] || TARGETS="$TARGETS,x86_64-softmmu"
case "$(uname -m)" in arm64|aarch64) TARGETS="$TARGETS,aarch64-softmmu" ;; esac
"$ROOT/qemu/configure" \
  --python="$PYTHON" \
  --disable-werror \
  --disable-sdl \
  --disable-sdl-image \
  --disable-gtk \
  --disable-vte \
  --disable-cocoa \
  --disable-curses \
  --disable-spice \
  --disable-alsa \
  --disable-pa \
  --disable-pipewire \
  --disable-jack \
  --disable-oss \
  --disable-sndio \
  --disable-coreaudio \
  --disable-dsound \
  --disable-brlapi \
  --disable-af-xdp \
  --disable-vde \
  --disable-bpf \
  --disable-curl \
  --disable-libssh \
  --disable-libiscsi \
  --disable-libnfs \
  --disable-rbd \
  --disable-blkio \
  --disable-png \
  --disable-vnc-jpeg \
  --extra-cflags="$EXTRA_CFLAGS" \
  ${CFG[@]+"${CFG[@]}"} \
  --target-list="$TARGETS" \
  -Dlibdisc_dir="$LIBDISC_DIR" \
  -Dlibsynth_dir="$LIBSYNTH_DIR" \
  "$@"

# QEMU's configure writes `werror = true` into its native file for git
# checkouts on Linux/Windows; meson re-applies native-file options on
# auto-regeneration, overriding the -Dwerror=false from --disable-werror and
# breaking a pinned release on new toolchains. Strip it.
sed -i.bak '/^werror = true$/d' "$BUILD/config-meson.cross" && rm -f "$BUILD/config-meson.cross.bak"
# MSVC: the libraries' .pc files, in the file meson reads again when a
# plain `ninja` regenerates, where this script's PKG_CONFIG_LIBDIR is
# gone and MSYS2's pkg-config would answer with its mingw glib.
if [ -n "$MSVC" ] && ! grep -q '^pkg_config_libdir' "$BUILD/config-meson.cross"; then
  sed -i "s|^\[properties\]\$|[properties]\npkg_config_libdir = ['$DEPS/lib/pkgconfig']|" "$BUILD/config-meson.cross"
fi
# keep the edited native file from looking newer than build.ninja (spurious regen)
touch -r "$BUILD/build.ninja" "$BUILD/config-meson.cross"
