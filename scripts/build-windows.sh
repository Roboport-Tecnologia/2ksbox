#!/usr/bin/env bash
# Build the Windows artefacts from a Linux host, in dependency order. This
# is the Windows counterpart of scripts/build.sh, which builds for the host
# it runs on. Everything runs inside the cross container
# (scripts/win-cross.sh, packaging/windows/Dockerfile) except the guest
# tools and the packaging step, which run on the host.
#
# In MSYS2's MINGW64 shell on Windows the same stages build natively, for
# debugging on the PC. They use the cross image's compilers, C runtime and
# Rust target, so a native build is the build that ships. The guest-tools
# ISO builds there too, with MSYS2's i686 toolchain
# (guest-tools/msys2-i686.sh), and so does the package
# (scripts/package-windows.sh, checked on Windows itself). Run what was
# built with scripts/win-run.sh.
#
#   scripts/build-windows.sh                everything this host can build
#   scripts/build-windows.sh qemu rust      only those stages
#   scripts/build-windows.sh --package      ... and then roll the zip
#   scripts/build-windows.sh --msys2-deps   (Windows) install what the build needs
#
# Stages, in the order they must run:
#
#   qemu    configure-qemu.sh --windows -> ninja: qemu-system-i386.exe,
#           qemu-img.exe, qemu-io.exe, libqemu-embed-i386.dll, into
#           build/win/qemu (with libdisc built for windows-gnu first)
#   rust    cargo build --release --target x86_64-pc-windows-gnu: the
#           player, launcher-core, discx. Runs after `qemu`, because the
#           player links the embed DLL from build/win/qemu.
#   qt      cargo build in launcher-qt/ (its own workspace): the Qt 6 /
#           QML launcher that every package ships (ADR-015). Cross-compiled
#           like the rest. The image carries the mingw Qt to link against
#           and a native Qt of the same version for moc, rcc and
#           qmltyperegistrar.
#   exec    DXVK's d3d9.dll into build/win/dxvk (configure-dxvk.sh
#           --windows), then build-d3dpt-exec.sh --windows: d3dpt_exec.dll,
#           the Direct3D executor (doc 14). The package ships DXVK as
#           dxvk_d3d9.dll, the executor's default. D3DPT_D3D9=system (or
#           auto, when DXVK opens no adapter) runs it on Windows' own
#           system32\d3d9.dll instead.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO. It is
#           32-bit guest code, the same file the Linux package ships, so
#           a default run builds it only when there is none yet. Naming
#           the stage rebuilds it (after a driver change).
#
# docs/build-windows.md is the prose; docs/tracks/m11-windows-host.md is
# the track. Nothing here writes to build/qemu or target/release, so a
# checkout holds a Linux build and a Windows build side by side.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Native on Windows (MSYS2's MINGW64 shell) or cross from Linux. The other
# MSYS2 shells are other C runtimes and C++ libraries than the package's
# msvcrt + libstdc++, so a build there would not be the one that ships.
NATIVE=""; HOW=cross
case "${MSYSTEM:-}" in
  "") ;;
  MINGW64) NATIVE=1; HOW=native ;;
  *) echo "build-windows.sh: this is MSYS2's $MSYSTEM shell; open the MINGW64 one" >&2; exit 1 ;;
esac

# Everything the native build needs from MSYS2, in one place: the cross
# image's list (packaging/windows/Dockerfile) under MSYS2's names, plus gdb,
# which is what building on the PC is for, and diffutils, which a bare MSYS2
# lacks: QEMU's meson requires `diff` (tests/qapi-schema) and prepare-qemu.sh
# keeps meson files' mtimes with `cmp`. The second half is the guest-tools
# ISO's: the i686 toolchain, gendef, and what qemu-3dfx's build calls
# (make, which, xxd from vim, shasum from perl, nasm), plus
# xorriso. Not here: Rust, which is rustup's own installer with the GNU
# host, and Open Watcom, which is a snapshot to unpack (both in
# docs/build-windows.md).
MSYS2_PACKAGES=(git rsync diffutils
  mingw-w64-x86_64-{gcc,clang,lld,gdb,ninja,meson,pkgconf,python,python-distlib}
  mingw-w64-x86_64-{glib2,pixman,zlib,libepoxy,libslirp}
  mingw-w64-x86_64-{glslang,qt6-base,qt6-declarative}
  mingw-w64-i686-gcc mingw-w64-x86_64-tools make which vim perl nasm xorriso zstd)

JOBS=(); PACKAGE=""; STAGES=(); EXPLICIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -p|--package) PACKAGE=1; shift ;;
    --msys2-deps)
      [ -n "$NATIVE" ] || { echo "build-windows.sh: --msys2-deps is for MSYS2's MINGW64 shell on Windows" >&2; exit 2; }
      pacman -S --needed "${MSYS2_PACKAGES[@]}"
      exit ;;
    -h|--help) sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    qemu|rust|qt|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build-windows.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done
if [ ${#STAGES[@]} -eq 0 ]; then STAGES=(qemu rust qt exec guest); else EXPLICIT=1; fi

BUILT=(); SKIPPED=(); T0=$SECONDS
want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build-windows.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"; SKIPPED+=("$1 ($2)"); return 1
}
if [ -n "$NATIVE" ]; then
  inw() { "$@"; }
  # MSYS2's compilers carry no target prefix: the host is the target.
  WCC=gcc WCXX=g++
else
  inw() { scripts/win-cross.sh "$@"; }
  WCC=x86_64-w64-mingw32-gcc WCXX=x86_64-w64-mingw32-g++
fi

if [ -n "$NATIVE" ]; then
  # A checkout with CRLF line endings fails far from here: every patch of
  # the queue "does not apply". Git for Windows converts by default.
  if [ -f qemu/configure ] && grep -q $'\r' qemu/configure; then
    echo "build-windows.sh: the checkout has CRLF line endings; clone again with core.autocrlf=false (docs/build-windows.md)" >&2
    exit 1
  fi
  missing=()
  for t in git rsync diff cmp cygpath gcc g++ clang ld.lld ninja meson pkg-config windres glslangValidator cargo rustc; do
    command -v "$t" >/dev/null || missing+=("$t")
  done
  want qt && ! command -v qmake6 >/dev/null && missing+=(qmake6)
  if [ ${#missing[@]} -gt 0 ]; then
    echo "build-windows.sh: not found: ${missing[*]} -- run scripts/build-windows.sh --msys2-deps" >&2
    exit 1
  fi
  # rustup's default on Windows is the MSVC toolchain, whose build scripts
  # need Microsoft's link.exe: the host has to be the GNU one.
  host="$(rustc -vV | sed -n 's/^host: //p')"
  [ "$host" = x86_64-pc-windows-gnu ] || {
    echo "build-windows.sh: rustc's host is $host; run: rustup default stable-x86_64-pc-windows-gnu" >&2; exit 1; }
  export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-gcc}"
  export QMAKE="${QMAKE:-qmake6}"
fi

if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/qemu-1/hw/mesa/meson.build ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# The patch queue is applied to the one qemu/ tree both builds compile
# from, so it is prepared here as scripts/build.sh does it. build.sh skips
# an unchanged queue by its stamp; here prepare is unconditional but cheap.
# A Windows build is not the inner loop, and a tree left half-prepared by
# an interrupted native build is the failure that costs an hour.
if want qemu; then
  say "qemu: prepare (overlay + patch queue)"
  scripts/prepare-qemu.sh

  # meson will not switch a build directory's compiler (patch 68 moved the
  # Windows QEMU from GCC to clang), so a different one configures afresh
  want_cc="${WIN_QEMU_CC:-clang}"
  if [ -f build/win/qemu/build.ninja ] && [ "$(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc)" != "$want_cc" ]; then
    echo "    build/win/qemu was built with $(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc), wanted $want_cc - configuring afresh"
    rm -rf build/win/qemu
  fi
  # Nor a directory built from another QEMU release (9.2 to 11.1, M21):
  # ninja's regeneration replays the old command line, which names options
  # the new release removed ("Unknown options: glusterfs"), and the old
  # release's thin archives are updated in place, which MSYS2's ar cannot
  # open ("ar: libqemuutil.a: No such file or directory"). A directory with
  # no record is from before the record, so 9.2's.
  want_qemu="$(cat qemu/VERSION)"
  if [ -f build/win/qemu/build.ninja ] && [ "$(cat build/win/qemu/.2ksbox-qemu 2>/dev/null || echo 9.2.4)" != "$want_qemu" ]; then
    echo "    build/win/qemu was built from QEMU $(cat build/win/qemu/.2ksbox-qemu 2>/dev/null || echo 9.2.4), the tree is $want_qemu - configuring afresh"
    rm -rf build/win/qemu
  fi
  # Configure again when the tree or the configure script changed since the
  # last configure, as scripts/build.sh does (a flag configure-qemu.sh
  # gained is otherwise not applied). configure starts from a clean
  # meson-private, so the objects stay.
  needs_configure=""
  [ -f build/win/qemu/build.ninja ] || needs_configure=1
  for f in qemu/meson.build qemu/hw/mesa/meson.build scripts/configure-qemu.sh; do
    if [ -f build/win/qemu/build.ninja ] && [ "$f" -nt build/win/qemu/build.ninja ]; then
      needs_configure=1
    fi
  done
  if [ -n "$needs_configure" ]; then
    say "qemu: configure (mingw-w64 $HOW, $want_cc)"
    inw scripts/configure-qemu.sh --windows
    echo "$want_cc" > build/win/qemu/.2ksbox-cc
    echo "$want_qemu" > build/win/qemu/.2ksbox-qemu
  else
    echo "    build/win/qemu is configured - skipping configure"
    # prepare re-applied the queue, so meson may need to regenerate; ninja
    # works that out itself from the mtimes it just saw change.
    :
  fi
  say "qemu: ninja"
  inw ninja -C build/win/qemu ${JOBS[@]+"${JOBS[@]}"} \
    qemu-system-i386.exe qemu-img.exe qemu-io.exe libqemu-embed-i386.dll
  BUILT+=(qemu)
fi

if want rust; then
  say "rust: cargo build --release --target x86_64-pc-windows-gnu"
  # Default members only (Cargo.toml): `launcher-capi` is left to the
  # native `scripts/build.sh`, which keeps it from rotting.
  inw cargo build --release --target x86_64-pc-windows-gnu ${JOBS[@]+"${JOBS[@]}"}
  BUILT+=(rust)
fi

# launcher-qt is its own cargo workspace (so a plain `cargo build` never
# needs Qt), which is why this is a separate stage with its own directory
# rather than another member of the one above.
# MSYS2 only. qt-build-utils runs moc and the other Qt tools with an empty
# environment, and MSYS2 keeps them in share/qt6/bin, away from the DLLs in
# bin/ they load. With no PATH none of them starts ("moc unexpectedly
# exited"). build/win/qt-host gets a copy of each tool beside its own DLL
# closure (ldd), and QMAKE becomes packaging/windows/qmake-host.c, which
# answers the tool-directory queries with that folder and passes everything
# else to qmake6. MSYS2's own tree is not touched.
msys2_qt_host() {
  local dir="$ROOT/build/win/qt-host" libexec tool dll real
  libexec="$(cygpath -u "$(qmake6 -query QT_HOST_LIBEXECS | tr -d '\r')")"
  rm -rf "$dir" && mkdir -p "$dir"
  for tool in moc rcc qmltyperegistrar qmlcachegen qtpaths; do
    [ -f "$libexec/$tool.exe" ] || continue
    cp "$libexec/$tool.exe" "$dir/"
    ldd "$libexec/$tool.exe" | awk '$3 ~ /^\/mingw64\// { print $3 }' | while read -r dll; do
      [ -f "$dir/$(basename "$dll")" ] || cp "$dll" "$dir/"
    done
  done
  # whether `command -v` says .exe or not, exactly one
  real="$(cygpath -m "$(command -v qmake6)")"; real="${real%.exe}.exe"
  printf '#define REAL_QMAKE "%s"\n#define HOST_TOOLS "%s"\n' "$real" "$(cygpath -m "$dir")" \
    > "$dir/qt-host-paths.h"
  gcc -O2 -Wall -static -I"$dir" -o "$dir/qmake-host.exe" packaging/windows/qmake-host.c
  export QMAKE="$(cygpath -m "$dir/qmake-host.exe")"
  # Run each the way qt-build-utils does, with no environment at all,
  # because its own failure is "could not find Qt" with the output thrown
  # away. MSYS2's `env -i` would not do, since it puts Windows' own
  # variables back for a native program. A native Python's env={} does not.
  python3 - "$QMAKE" "$real" "$(cygpath -m "$dir")" <<'PY' || exit 1
import os, subprocess, sys
qmake, real, tools = sys.argv[1:4]
def run(what, argv):
    try:
        r = subprocess.run(argv, env={}, capture_output=True, text=True)
    except OSError as e:
        print(f"    {what}: cannot start: {e}")
        return None
    if r.returncode != 0:
        print(f"    {what}: exit {r.returncode:#x}\n      stdout: {r.stdout.strip()!r}\n      stderr: {r.stderr.strip()!r}")
        return None
    return r.stdout.strip()
v = run("qmake-host -query QT_VERSION", [qmake, "-query", "QT_VERSION"])
if v is None:
    run("qmake6 itself, the same way", [real, "-query", "QT_VERSION"])
    print("build-windows.sh: the QMAKE wrapper does not answer with no environment (above)")
    sys.exit(1)
print(f"    qmake-host: Qt {v}, tools from {run('qmake-host -query QT_HOST_LIBEXECS', [qmake, '-query', 'QT_HOST_LIBEXECS'])}")
bad = [t for t in ("moc", "rcc", "qmltyperegistrar", "qmlcachegen", "qtpaths")
       if os.path.exists(os.path.join(tools, t + ".exe"))
       and run(t + " --help", [os.path.join(tools, t + ".exe"), "--help"]) is None]
if bad:
    print("build-windows.sh: these Qt tools do not start with no environment: " + " ".join(bad))
    sys.exit(1)
PY
}

if want qt; then
  if [ -z "$NATIVE" ] && ! scripts/win-cross.sh test -x /usr/bin/x86_64-w64-mingw32-qmake-qt6; then
    skip qt "the cross image has no mingw Qt 6 (rebuild it: scripts/win-cross.sh --build)" || true
  else
    [ -z "$NATIVE" ] || msys2_qt_host
    say "qt: cargo build --release --target x86_64-pc-windows-gnu (launcher-qt)"
    inw sh -c 'cd launcher-qt && exec cargo build --release --target x86_64-pc-windows-gnu'
    BUILT+=(qt)
  fi
fi

if want exec; then
  # The queue is applied on the host, like qemu's above. The DXVK tree is
  # shared with the native build, so this keeps scripts/build.sh's own
  # stamp (same file, same hash). A prepare hands both builds fresh
  # mtimes, so one that changed nothing would cost the native DXVK a full
  # rebuild.
  say "exec: DXVK d3d9.dll (prepare + mingw $HOW)"
  dxvk_stamp=$( { git -C third_party/dxvk rev-parse HEAD 2>/dev/null || echo none
                  find patches/dxvk scripts/prepare-dxvk.sh -type f | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat
                } | sha256sum | cut -d' ' -f1)
  if [ "$(cat build/.stamp-dxvk-prepare 2>/dev/null || true)" != "$dxvk_stamp" ]; then
    scripts/prepare-dxvk.sh
    mkdir -p build && printf '%s\n' "$dxvk_stamp" > build/.stamp-dxvk-prepare
  else
    echo "    patch queue and submodule unchanged - skipping prepare"
  fi
  if [ ! -f build/win/dxvk/build.ninja ]; then
    inw scripts/configure-dxvk.sh --windows
  fi
  inw ninja -C build/win/dxvk ${JOBS[@]+"${JOBS[@]}"} src/d3d9/d3d9.dll
  say "exec: d3dpt_exec.dll (the Direct3D decoder + executor)"
  inw scripts/build-d3dpt-exec.sh --windows
  # ... and the display driver's host test, which package-windows.sh runs
  # under wine against the staged pair: a frame through the Windows DLLs.
  inw "$WCXX" -std=c++17 -O2 -static -o build/win/d3dpt-dp2-test.exe tools/d3dpt-dp2-test.cpp
  # The WGL probe (tools/wgl-probe.c) rides along as one more compile. It
  # is the first thing to run on a Windows machine whose Win98 guest gets
  # no OpenGL.
  say "exec: wgl-probe.exe (the embed backend's WGL sequence, without QEMU)"
  inw "$WCC" -O1 -o build/win/wgl-probe.exe tools/wgl-probe.c \
    -lopengl32 -lgdi32 -luser32
  BUILT+=(exec)
fi

# The ISO is guest code and identical whatever host built it, so this
# stage exists to notice that there is none rather than to rebuild one.
if want guest; then
  if [ -z "$EXPLICIT" ] && ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1; then
    say "guest"
    echo "    guest-tools ISO present - skipping (scripts/build-windows.sh guest rebuilds it)"
  elif [ -n "$NATIVE" ]; then
    # msys2-i686.sh, sourced by the script, switches it to MSYS2's i686
    # toolchain and says what is missing
    say "guest: guest-tools ISO (MSYS2 i686)"
    guest-tools/build-wrappers.sh
    BUILT+=(guest)
  elif ! command -v i686-w64-mingw32-gcc >/dev/null; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! command -v xorriso >/dev/null && ! command -v genisoimage >/dev/null; then
    skip guest "needs xorriso (or genisoimage) for the ISO" || true
  else
    say "guest: guest-tools ISO"
    guest-tools/build-wrappers.sh
    BUILT+=(guest)
  fi
fi

say "summary after $((SECONDS - T0)) s"
[ ${#BUILT[@]} -gt 0 ] && printf '    built: %s\n' "${BUILT[*]}"
if [ ${#SKIPPED[@]} -gt 0 ]; then printf '    skipped:\n'; printf '      %s\n' "${SKIPPED[@]}"; fi

if [ -n "$PACKAGE" ]; then
  say "scripts/package-windows.sh"
  exec scripts/package-windows.sh
fi
echo
if [ -n "$NATIVE" ]; then
  echo "    next: scripts/win-run.sh launcher   (or player / qemu; GDB=1 runs it under gdb)"
  echo "          scripts/package-windows.sh    (the zip, checked on this PC)"
else
  echo "    next: scripts/package-windows.sh   (the zip, checked under wine)"
fi
