#!/usr/bin/env bash
# Build libd3dpt_exec (d3dpt/exec: the paravirtual Direct3D device's decoder
# + DXVK executor, doc 14) into build/d3dpt. Needs build/dxvk (headers only:
# the library dlopens libdxvk_d3d9 at runtime). Linux: .so, macOS: .dylib.
#
#   scripts/build-d3dpt-exec.sh             this host: the in-process library, the
#                                           out-of-process one (libd3dpt_exec_remote,
#                                           M15), and with mingw the PE pair it runs
#   scripts/build-d3dpt-exec.sh --wine      the PE pair only
#   scripts/build-d3dpt-exec.sh --windows   build/win/d3dpt/d3dpt_exec.dll, in MSYS2's
#                                           MINGW64 shell on Windows
#
# Under Rosetta on an Apple Silicon Mac (scripts/build.sh --x86_64, the
# Intel build; docs/build-macos.md "The Intel build") the output is
# build/x86_64/d3dpt, and only the out-of-process library and the PE pair
# are built: no Vulkan driver exists for an Intel Mac (ADR-019), so the
# in-process executor could never open a device there.
#
# --windows compiles the same two files with MSVC (cl, the C runtime
# static, as the launcher and the player) against the Windows SDK's own
# <windows.h> and <d3d9.h> instead of DXVK's native stand-ins for them, and
# loads DXVK's d3d9.dll at run time under the name the package gives it,
# `dxvk_d3d9.dll` (build/win/dxvk, scripts/configure-dxvk.sh --windows),
# never as Windows' own d3d9.dll. Natively, in MSYS2's MINGW64 shell
# (docs/build-windows.md), in the environment scripts/msvc-env.sh sets
# up. MSVC because DXVK is: DXVK throws C++ exceptions out of
# Direct3DCreate9 on a host with no Vulkan device, and only an executor
# built by the same compiler catches them (ADR-026's amendment). QEMU,
# which loads it, stays mingw; nothing but C crosses that edge.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = "--windows" ]; then
  OUT="$ROOT/build/win/d3dpt"; mkdir -p "$OUT"
  DLL="$OUT/d3dpt_exec.dll"   # not LIB: the MSVC environment has one
  [ "${MSYSTEM:-}" = MINGW64 ] || {
    echo "build-d3dpt-exec.sh --windows: in MSYS2's MINGW64 shell on Windows (ADR-026)" >&2; exit 1; }
  . "$ROOT/scripts/msvc-env.sh" || exit 1
  # /MT: the C runtime static, so the DLL needs no vcruntime DLL beside
  # it. /EHsc: what DXVK is built with (meson's default for MSVC).
  # NOMINMAX: <windows.h>'s min/max macros against <algorithm>'s.
  # Options with '-', not '/', which MSYS2 would take for paths.
  ( cd "$OUT" && cl -nologo -std:c++17 -O2 -EHsc -MT -W3 -LD -utf-8 \
      -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -Fe"$(cygpath -w "$DLL")" \
      "$(cygpath -w "$ROOT/d3dpt/exec/d3dpt_exec.cpp")" "$(cygpath -w "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp")" \
      user32.lib )
  echo "==> $DLL"
  exit 0
fi

# --wine: only the PE pair below (the Windows build of the executor and
# d3dpt-exec-host.exe), for a host that has mingw and wants just those.
WINE_ONLY=0; [ "${1:-}" = "--wine" ] && WINE_ONLY=1

OUT="$ROOT/build/d3dpt"
IN_PROCESS=1
if [ "$(uname -s)" = Darwin ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
  OUT="$ROOT/build/x86_64/d3dpt"; IN_PROCESS=0
fi
mkdir -p "$OUT"
DX="$ROOT/third_party/dxvk/include/native"
if [ "$(uname -s)" = Darwin ]; then LIB="$OUT/libd3dpt_exec.dylib"; RLIB="$OUT/libd3dpt_exec_remote.dylib"; SHARED=(-dynamiclib -install_name "$LIB"); RSHARED=(-dynamiclib -install_name "$RLIB"); else LIB="$OUT/libd3dpt_exec.so"; RLIB="$OUT/libd3dpt_exec_remote.so"; SHARED=(-shared); RSHARED=(-shared); fi
CXX="${CXX:-c++}"
CC="${CC:-cc}"
if [ $WINE_ONLY = 0 ]; then
  if [ $IN_PROCESS = 1 ]; then
    "$CXX" -std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wno-unused-function "${SHARED[@]}" -o "$LIB" \
      "$ROOT/d3dpt/exec/d3dpt_exec.cpp" "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp" -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl
    echo "==> $LIB"
  else
    echo "==> the Intel build: no in-process executor (no Vulkan on an Intel Mac)"
  fi
  # The same API over a child process on Wine (docs/tracks/m15-wine-executor.md,
  # ADR-018): what a host below DXVK's Vulkan 1.3 floor runs Direct3D on.
  # Plain C over POSIX, no DXVK headers; QEMU opens it after the in-process
  # library found no device (d3dpt_exec_load.c).
  "$CC" -O2 -fPIC -fvisibility=hidden -Wall "${RSHARED[@]}" -o "$RLIB" \
    "$ROOT/d3dpt/exec/d3dpt_exec_remote.c" -I"$ROOT/d3dpt/exec" -I"$ROOT/d3dpt" -ldl
  echo "==> $RLIB"
fi

# The pair that process runs: the Windows build of the executor (as the
# Windows package's, on Wine's own d3d9 instead of DXVK's) and the host
# program. mingw-w64 builds them on Linux and macOS alike; a host without it
# gets the in-process executor only, and says so.
WCXX="${WCXX:-x86_64-w64-mingw32-g++}"; WCC="${WCC:-x86_64-w64-mingw32-gcc}"
if command -v "$WCXX" >/dev/null && command -v "$WCC" >/dev/null; then
  WOUT="$OUT/wine"; mkdir -p "$WOUT"
  # -static as well: Homebrew's mingw links libwinpthread dynamically where
  # Fedora's does not, and the DLL must stand alone beside the .exe.
  "$WCXX" -std=c++17 -O2 -fvisibility=hidden -Wall -Wno-unused-function -shared \
    -D__USE_MINGW_ANSI_STDIO=1 -static -static-libgcc -static-libstdc++ -o "$WOUT/d3dpt_exec.dll" \
    "$ROOT/d3dpt/exec/d3dpt_exec.cpp" "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp"
  "$WCC" -O2 -Wall -static -o "$WOUT/d3dpt-exec-host.exe" "$ROOT/d3dpt/exec/d3dpt_exec_host.c" -I"$ROOT/d3dpt/exec"
  echo "==> $WOUT/d3dpt_exec.dll + d3dpt-exec-host.exe (the executor on Wine)"
else
  echo "==> no $WCXX: the executor on Wine (build/d3dpt/wine/) not built; install mingw-w64 for it"
  # A failure only when the pair was all that was asked for. (As the last
  # command of the script, a bare `[ … ] && exit 1` made the whole build
  # exit 1 on a host without mingw, which stopped the Flatpak's.)
  if [ $WINE_ONLY = 1 ]; then exit 1; fi
fi
