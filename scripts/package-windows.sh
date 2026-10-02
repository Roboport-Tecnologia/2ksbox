#!/usr/bin/env bash
# Build the Windows package (docs/build-windows.md). Stage everything a
# user needs into one folder, check that the staged launcher resolves its
# companions inside that folder, and roll a zip.
#
#   scripts/package-windows.sh                 # stage, check, zip
#   scripts/package-windows.sh --no-zip        # leave the staged tree only
#   scripts/package-windows.sh --with-shaders  # include the preset collection
#   scripts/package-windows.sh --msix          # ... and the Store's MSIX layout
#                                              #     (scripts/package-msix.sh)
#   scripts/package-windows.sh --out DIR       # default build/win/package
#
# `2ksbox.exe` is `launcher-mitsuami`, the WinUI 3 launcher (ADR-023),
# the one MSVC binary here (`build-windows.sh mitsuami`, Windows only),
# with a static C runtime. It needs no DLL of ours, but it runs on the
# Windows App Runtime 2.4 or later, a framework Microsoft installs once
# per PC; the zip cannot carry it, and the Store's MSIX declares it as a
# dependency instead. So the package comes from a Windows PC, where that
# launcher is built.
#
# It runs in two places, and builds nothing in either
# (scripts/build-windows.sh does that, and this script says so if an
# artefact is missing):
#
#   - a Linux host, not inside scripts/win-cross.sh, given a launcher
#     built on Windows in launcher-mitsuami/target/release: the checks need
#     wine, which the cross image does not carry, and the mingw sysroot and
#     strip come out of that image (podman) when the host has none;
#   - MSYS2's MINGW64 shell on Windows, after a native build: the sysroot
#     and binutils are MSYS2's own (/mingw64), no container, and the
#     checks run the package itself, with nothing but Windows on PATH and
#     the launcher's data in a scratch directory (LAUNCHER_DATA_DIR), not
#     the user's %APPDATA%. There the window grab and the system-Direct3D
#     run are verdicts, because this is the target.
#
# A Windows package is one folder, not a Unix prefix. The executables sit
# at the top with every DLL beside them, which is where the loader looks
# (no rpath, no PATH), and the data directories under them. The launcher
# knows both shapes (launcher-core/src/paths.rs).
#
#   2ksbox.exe                  the launcher
#   2ksbox-player.exe           the player
#   qemu-img.exe                ours, patched
#   libqemu-embed-i386.dll      QEMU as a library, what the player runs
#   d3dpt_exec.dll              the Direct3D executor (doc 14)
#   dxvk_d3d9.dll               DXVK's d3d9, the executor's default,
#                               renamed so it is never mistaken for
#                               Windows' own d3d9.dll (D3DPT_D3D9=system)
#   *.dll                       the mingw runtime those need
#   pc-bios\                    QEMU firmware
#   soundfonts\                 the General MIDI bank (doc 20)
#   guest-tools\                the guest-tools ISO
#   shaders\                    presets, with --with-shaders
#   doc\                        COPYING, notices, README
#   2ksbox.ico                  the application icon, for a shortcut
#                               (the .exes carry it as a resource too)
#   2ksbox-debug.bat            runs the launcher from a console and
#                               keeps its exit code, the only trace a
#                               silent start-up failure leaves
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ZIP=1 SHADERS=0 MSIX=0 OUT="$ROOT/build/win/package"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-zip) ZIP=0; shift ;;
    --with-shaders) SHADERS=1; shift ;;
    --msix) MSIX=1; shift ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,56p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package-windows.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# Native on Windows (MSYS2's MINGW64 shell, the C runtime and libstdc++
# the package ships) or on a Linux host. As in build-windows.sh.
NATIVE=""
case "${MSYSTEM:-}" in
  "") ;;
  MINGW64) NATIVE=1 ;;
  *) echo "package-windows.sh: this is MSYS2's $MSYSTEM shell; open the MINGW64 one" >&2; exit 1 ;;
esac

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="2ksbox-$VERSION-windows-x86_64"
STAGE="$OUT/$NAME"
TARGET="$ROOT/target/x86_64-pc-windows-gnu/release"
LAUNCHER="$ROOT/launcher-mitsuami/target/release/launcher-mitsuami.exe"
Q="$ROOT/build/win/qemu"

need() { [ -e "$1" ] || { echo "package-windows.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need "$Q/libqemu-embed-i386.dll" "scripts/build-windows.sh qemu"
need "$Q/qemu-img.exe"           "scripts/build-windows.sh qemu"
need "$LAUNCHER"                 "scripts/build-windows.sh mitsuami, on Windows"
need "$TARGET/player.exe"        "scripts/build-windows.sh rust"
need qemu/pc-bios                "scripts/prepare-qemu.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE/doc"

install -m755 "$LAUNCHER" "$STAGE/2ksbox.exe"
install -m755 "$TARGET/player.exe" "$STAGE/2ksbox-player.exe"
install -m755 "$Q/qemu-img.exe" "$STAGE/"
install -m755 "$Q/libqemu-embed-i386.dll" "$STAGE/"
cp -a qemu/pc-bios "$STAGE/pc-bios"

# The Direct3D executor is optional at run time (the device says "no
# executor" and the guest falls back), so a package without it is not a
# failure. Say so anyway, because "3D does nothing" is hard to trace back
# to a packaging step.
#
# Both or neither, as on Linux. DXVK's d3d9 is the executor's default;
# Windows' own system32 d3d9 is only the fallback below the Vulkan floor
# (D3DPT_D3D9). DXVK's release build keeps its symbols, 21 MB of them, so
# the staged copy is stripped.
if [ -f build/win/d3dpt/d3dpt_exec.dll ] && [ -f build/win/dxvk/src/d3d9/d3d9.dll ]; then
  install -m755 build/win/d3dpt/d3dpt_exec.dll "$STAGE/"
  install -m755 build/win/dxvk/src/d3d9/d3d9.dll "$STAGE/dxvk_d3d9.dll"
  STRIP=${WIN_STRIP:-$([ -n "$NATIVE" ] && echo strip || echo x86_64-w64-mingw32-strip)}
  if command -v "$STRIP" >/dev/null; then "$STRIP" --strip-debug "$STAGE/dxvk_d3d9.dll"
  elif [ -n "$NATIVE" ]; then echo "package-windows.sh: no $STRIP (pacman -S mingw-w64-x86_64-binutils)" >&2; exit 1
  else scripts/win-cross.sh x86_64-w64-mingw32-strip --strip-debug "$STAGE/dxvk_d3d9.dll"; fi
else
  echo "package-windows.sh: no build/win/d3dpt/d3dpt_exec.dll and build/win/dxvk/src/d3d9/d3d9.dll (scripts/build-windows.sh exec); packaging without Direct3D pass-through"
fi

# The diagnostic that answers "why does my Win98 guest get no OpenGL" on
# the machine it happens on, rather than in a VM (tools/wgl-probe.c).
if [ -f build/win/wgl-probe.exe ]; then
  mkdir -p "$STAGE/tools"
  install -m755 build/win/wgl-probe.exe "$STAGE/tools/"
fi

# The General MIDI bank (doc 20 §4). Flat, like everything else here:
# `paths::in_prefix` drops the `share/2ksbox/` a Unix prefix uses.
mkdir -p "$STAGE/soundfonts"
install -m644 soundfonts/TimGM6mb.sf2 "$STAGE/soundfonts/"

iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$STAGE/guest-tools"
  install -m644 "$iso" "$STAGE/guest-tools/"
else
  echo "package-windows.sh: no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

if [ "$SHADERS" = 1 ]; then
  need third_party/slang-shaders "git submodule update --init third_party/slang-shaders"
  mkdir -p "$STAGE/shaders"
  tar -c --exclude='.git*' -C third_party/slang-shaders . | tar -x -C "$STAGE/shaders"
fi

install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$STAGE/doc/"
# The application icon (`scripts/gen-icons.sh`), the same artwork the
# Linux and macOS packages install. Every staged .exe already carries it
# as a resource (`packaging/windows/win-icon.rs`). This loose copy is for
# what takes a path instead: a pinned shortcut, an installer, a folder's
# own icon.
install -m644 packaging/icon/2ksbox.ico "$STAGE/2ksbox.ico"

# --- what to double-click when nothing happens ------------------------
# A windowed program that dies before `main` (a DLL the loader cannot
# find, a static initialiser that faults) says nothing anywhere. There is
# no window, no console and no launcher log, because no code of ours has
# run yet (launcher-core/src/fatal.rs writes that log from the first line
# of `main`). Only the **exit code** tells those cases apart, and only a
# console shows it, so the package carries a console to run the launcher
# from. Ask for its log first when a report is "it didn't start".
cat > "$STAGE/2ksbox-debug.bat" <<'BAT'
@echo off
rem  Run the launcher from a console and keep what it says.  Send the
rem  developers the 2ksbox-debug.log this writes: when no window ever
rem  appeared, its exit codes and the launcher's own log are the whole
rem  of the evidence.
rem
rem  Every run goes through `start "" /b /wait`, because cmd does not
rem  wait for a windows-subsystem program and would otherwise record no
rem  exit code at all.  The launcher's *output* comes from its own log
rem  rather than this console: a program started by double-click has no
rem  stdout, so `--diagnose` files its answers instead of printing them.
setlocal
cd /d "%~dp0"
set LOG=%APPDATA%\2ksbox\data\launcher.log
set PLOG=%APPDATA%\2ksbox\data\player.log
set OUT=%~dp02ksbox-debug.log
rem  player.log is appended to by every machine ever started, so only the
rem  lines this run adds are kept: count them now, skip them at the end.
set PSKIP=0
if exist "%PLOG%" for /f %%n in ('type "%PLOG%" ^| find /c /v ""') do set PSKIP=%%n
echo === 2ksbox debug run, %DATE% %TIME% === > "%OUT%"
echo Asking the launcher what it can see ...
start "" /b /wait 2ksbox.exe --diagnose
echo [--diagnose exit %ERRORLEVEL%] >> "%OUT%"
echo.
echo Starting the launcher.  Close its window when you have seen enough.
start "" /b /wait 2ksbox.exe
echo [launcher exit %ERRORLEVEL%] >> "%OUT%"
echo. >> "%OUT%"
if exist "%LOG%" (
  echo --- %%APPDATA%%\2ksbox\data\launcher.log --- >> "%OUT%"
  type "%LOG%" >> "%OUT%"
) else (
  echo (no launcher.log: nothing of ours ran, so it died in the loader^) >> "%OUT%"
)
echo. >> "%OUT%"
if exist "%PLOG%" (
  echo --- %%APPDATA%%\2ksbox\data\player.log, this run --- >> "%OUT%"
  more +%PSKIP% "%PLOG%" >> "%OUT%"
) else (
  echo (no player.log: no machine was started^) >> "%OUT%"
)
echo.
type "%OUT%"
echo.
echo Send this file: "%OUT%"
pause
BAT
chmod 644 "$STAGE/2ksbox-debug.bat"

# --- the DLL closure --------------------------------------------------
# Everything our four binaries import, transitively, that is not a
# Windows system DLL. A missing one of these is the classic Windows
# failure: a dialog naming a DLL, before a single line of ours runs. The
# import tables, walked with objdump, are the source of truth. A closure
# guessed from a package list goes stale.
#
# The system set is matched by name. Anything under the mingw sysroot is
# ours to ship. Anything else (kernel32, d3d9, opengl32, the api-ms-win-*
# API sets) is Windows' own and must NOT be shipped; a system DLL copied
# into the folder makes an app that only runs on the machine that built
# it.
#
# Natively the sysroot is MSYS2's /mingw64/bin, where the build linked
# against, and "not ours" is the same test: Windows' own DLLs are not in
# it.
if [ -n "$NATIVE" ]; then
  SYSROOT=${WIN_SYSROOT:-/mingw64/bin}
  OBJDUMP=${WIN_OBJDUMP:-objdump}
else
  SYSROOT=${WIN_SYSROOT:-/usr/x86_64-w64-mingw32/sys-root/mingw/bin}
  OBJDUMP=${WIN_OBJDUMP:-x86_64-w64-mingw32-objdump}
fi
if [ ! -d "$SYSROOT" ] && [ -z "$NATIVE" ]; then
  # The sysroot lives in the cross container, so ask it for a copy every
  # time. A kept copy is a snapshot of an older image; one from before Qt
  # was in the image once quietly packaged the old Qt launcher with no
  # Qt6Core.dll.
  SYSROOT="$ROOT/build/win/sysroot-bin"
  echo "==> copying the mingw runtime out of the cross image"
  rm -rf "$SYSROOT"
  mkdir -p "$SYSROOT"
  scripts/win-cross.sh bash -c \
    "cp -a /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll '$SYSROOT/'"
fi
command -v "$OBJDUMP" >/dev/null || { echo "package-windows.sh: no $OBJDUMP (WIN_OBJDUMP=)"; exit 1; }

imports() { "$OBJDUMP" -p "$1" | sed -n 's/^\tDLL Name: //p'; }

# Every binary in the package is a root, not just the ones at the top
# (tools\wgl-probe.exe sits in a subdirectory). Its imports resolve from
# the executable's own directory, which is where the closure puts
# everything.
staged_binaries() { find "$STAGE" \( -name '*.dll' -o -name '*.exe' \) -type f; }

# Seeded with what is Windows' even where a sysroot carries a copy:
# MSYS2's /mingw64/bin has the Vulkan loader, and DXVK and the executor
# name vulkan-1.dll, but the one to load is the system's, which comes
# with the GPU driver and matches it ("vulkan (the system's loader)" in
# the launcher's --paths).
declare -A seen=([vulkan-1.dll]=1)
copied=0
again=1
while [ "$again" = 1 ]; do
  again=0
  while read -r file; do
    [ -n "$file" ] || continue
    while read -r dll; do
      [ -n "$dll" ] || continue
      key=$(printf '%s' "$dll" | tr 'A-Z' 'a-z')
      [ -n "${seen[$key]:-}" ] && continue
      seen[$key]=1
      src=$(ls "$SYSROOT/$dll" 2>/dev/null || ls "$SYSROOT"/"$key" 2>/dev/null || true)
      [ -n "$src" ] || continue        # a Windows system DLL: not ours
      install -m755 "$src" "$STAGE/$(basename "$src")"
      copied=$((copied + 1))
      again=1
    done < <(imports "$file")
  done < <(staged_binaries)
done

# A DLL that is *loaded* rather than imported is invisible to the walk
# above. Fedora's mingw64-SDL2 is sdl2-compat, an SDL2.dll that
# LoadLibrary's SDL3.dll at run time, and a package with only the imported
# DLLs had a player that died with "Failed loading SDL3 library" on a PC
# with no SDL of its own. QEMU is now built --disable-sdl and neither DLL
# is staged, but the pass stays as the net for the next such library.
#
# Every staged binary is searched for names of DLLs that exist in the
# mingw sysroot and are not staged yet, and those ship too. It is broader
# than an import table on purpose, so the next run-time load is caught
# here instead of by a user.
runtime_deps() { strings -a "$1" | grep -oiE '[A-Za-z0-9_.+-]+\.dll' | sort -u; }
if command -v strings >/dev/null; then
  again=1
  while [ "$again" = 1 ]; do
    again=0
    while read -r file; do
      [ -n "$file" ] || continue
      while read -r dll; do
        [ -n "$dll" ] || continue
        key=$(printf '%s' "$dll" | tr 'A-Z' 'a-z')
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        src=$(ls "$SYSROOT/$dll" 2>/dev/null || ls "$SYSROOT"/"$key" 2>/dev/null || true)
        [ -n "$src" ] || continue        # Windows' own, or not a real name
        install -m755 "$src" "$STAGE/$(basename "$src")"
        echo "               + $(basename "$src") (loaded at run time, not imported)"
        copied=$((copied + 1))
        again=1
      done < <(runtime_deps "$file")
    done < <(staged_binaries)
  done
else
  echo "package-windows.sh: no strings(1); run-time-loaded DLLs not checked for" >&2
fi
echo "runtime DLLs   $copied copied from $(basename "$SYSROOT")"

# --- the check --------------------------------------------------------
# The staged binaries, run as Windows binaries, from outside the checkout,
# with an empty environment, so no LAUNCHER_*/PLAYER_* knob from this
# shell can make them work and nothing may resolve back into the build
# tree. Wine is the only Windows on a Linux build host. It is not the
# target, so a failure here is investigated rather than trusted, but "the
# launcher starts and answers about itself" and "the packaged qemu-img
# writes a qcow2" are what a broken package fails at.
fail=0
# The network backend every machine the launcher writes asks for
# (`-netdev user`, bundle.rs) must exist in the QEMU beside it. It is a
# *compiled-in* backend, through libslirp, which Fedora does not package
# for mingw. Without it the first machine started on a real PC died on
# "network backend 'user' is not compiled into this binary" with every
# check here green. The import table answers rather than a running QEMU,
# because the package holds no qemu-system-*.exe (QEMU is in-process,
# inside libqemu-embed-i386.dll) and wine hangs in the player that would
# load it. net/slirp.c is libslirp's only consumer, so the import is the
# backend.
if imports "$STAGE/libqemu-embed-i386.dll" | grep -qi '^libslirp'; then
  echo "qemu           -netdev user is compiled in (libslirp)"
else
  echo "package-windows.sh: the embed library does not link libslirp, so it has no" >&2
  echo "  'user' network backend -- and every machine the launcher writes asks for one" >&2
  echo "  (packaging/windows/Dockerfile builds it; Fedora has no mingw package)" >&2
  fail=1
fi

RUN=""
if [ -n "$NATIVE" ]; then RUN=native
elif command -v wine >/dev/null; then RUN=wine
fi
if [ -n "$RUN" ]; then
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' EXIT
  if [ "$RUN" = wine ]; then
    export WINEPREFIX="$scratch/wine" WINEDEBUG=-all
    # One prefix, created once, so every check below runs in the same one.
    wine wineboot -i >/dev/null 2>&1 || true
    # Where the staged launcher keeps its data: the prefix's AppData.
    DATA="$WINEPREFIX/drive_c/users"
    # Z: is wine's view of /.
    winpath() { printf 'Z:%s' "$1" | tr '/' '\\'; }
  else
    # Windows' known folders place %APPDATA%, so the environment cannot
    # move it; LAUNCHER_DATA_DIR does (paths::data_dir). Without it every
    # check below would write into the user's own library and log.
    DATA="$scratch/data"
    winpath() { cygpath -w "$1"; }
    # Windows and nothing else on PATH: a DLL the package lacks must fail
    # here, not be found in /mingw64/bin.
    WINDOWS=$(cygpath -W)              # from Windows, not the environment
    sysroot_u=$(cygpath -u "$WINDOWS")
    WINPATH="$sysroot_u/System32:$sysroot_u:$sysroot_u/System32/Wbem"
  fi

  # runpkg [VAR=value ...] program [args ...]: run a staged program from
  # the package folder with an empty environment, so no LAUNCHER_* or
  # PLAYER_* knob from this shell can make it work and nothing may resolve
  # back into the build tree. T= bounds it (default 300 s). D=1 passes the
  # display through to wine, which a real window or a Vulkan device needs
  # there; natively Windows' own session is always there.
  runpkg() {
    local vars=() prog
    while [[ "${1:-}" == *=* ]]; do vars+=("$1"); shift; done
    if [ "$RUN" = native ]; then
      case "$1" in /*) prog=$1 ;; *) prog=./$1 ;; esac
      (cd "$STAGE" && timeout "${T:-300}" env -i SYSTEMROOT="$WINDOWS" WINDIR="$WINDOWS" \
          PATH="$WINPATH" TEMP="$(winpath "$scratch")" TMP="$(winpath "$scratch")" \
          LAUNCHER_DATA_DIR="$(winpath "$DATA")" "${vars[@]}" "$prog" "${@:2}")
    else
      local disp=()
      [ -z "${D:-}" ] || disp=(DISPLAY="${DISPLAY:-}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}"
                               XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}")
      (cd "$STAGE" && timeout "${T:-300}" env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" \
          WINEDEBUG=-all PATH="$PATH" "${disp[@]}" "${vars[@]}" wine "$@")
    fi
  }

  resolved=$(runpkg 2ksbox.exe --paths 2>/dev/null || true)
  if [ -z "$resolved" ]; then
    # A launcher that dies before `main` answers nothing, so silence
    # fails.
    echo "package-windows.sh: the staged launcher printed nothing for --paths" >&2
    fail=1
  else
    printf '%s\n' "$resolved"
    # Every companion must resolve inside the package. Wine reports them
    # as Z:\... paths for a Unix directory, so compare on the tail.
    while read -r what path; do
      case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
      case "$path" in "("*) continue ;; esac
      win=$(printf '%s' "$path" | tr '\\' '/' | sed 's|^[A-Za-z]:||')
      case "$win" in
        *"/$NAME"/*|*"/$NAME") ;;
        *) echo "package-windows.sh: $what resolved outside the package: $path" >&2; fail=1 ;;
      esac
    done <<< "$resolved"
  fi

  # The library an *installed MSIX* would use (docs/build-windows.md,
  # "The Store package"): outside AppData, at the profile root, or an
  # uninstall takes the user's machines with it. `LAUNCHER_PACKAGED=1`
  # is the launcher's own switch for answering as a packaged build, and
  # LAUNCHER_DATA_DIR is emptied so the real answer shows. Natively that
  # answer is the user's own profile, where the launcher writes its log,
  # so the run leaves it as it found it: a %USERPROFILE%\2ksbox this run
  # made is removed again, and an existing launcher.log there is put back.
  made="" kept=""
  if [ "$RUN" = native ]; then
    # The profile from Windows itself (CSIDL_PROFILE), as the launcher
    # asks for it: USERPROFILE is not always in an MSYS2 environment.
    home=$(cygpath -u "$(cygpath -F 40)")/2ksbox
    if [ ! -e "$home" ]; then made=$home
    elif [ -f "$home/launcher.log" ]; then kept=$home/launcher.log; cp -p "$kept" "$scratch/kept.log"
    fi
  fi
  packaged=$(runpkg LAUNCHER_PACKAGED=1 LAUNCHER_DATA_DIR= 2ksbox.exe --paths 2>/dev/null \
      | awk '$1 == "library" { print }' || true)
  [ -z "$made" ] || rm -rf "$made"
  [ -z "$kept" ] || cp -p "$scratch/kept.log" "$kept"
  case "$packaged" in
    *"\\2ksbox (packaged)")
      echo "library        packaged build: $(printf '%s' "$packaged" | sed 's/^library *//')" ;;
    *)
      echo "package-windows.sh: a packaged launcher would keep its library at ${packaged:-(nothing printed)}, not <profile>\\2ksbox" >&2
      fail=1 ;;
  esac

  # The libraries QEMU `LoadLibrary`s by name rather than through an
  # import table: here, the Direct3D executor and the DXVK `d3d9` it runs
  # on. Nothing above can see them, which is how the Linux packages once
  # shipped without them. The staged *player* knows where they should be
  # (`player-core/src/companions.rs`), so ask it.
  companions=$(runpkg 2ksbox-player.exe --companions 2>/dev/null || true)
  if [ -n "$companions" ]; then
    printf '%s\n' "$companions"
    while read -r what file; do
      [ -f "$STAGE/$file" ] || continue          # not built here; warned above
      got=$(printf '%s\n' "$companions" | awk -v w="$what" '$1 == w { print $2 }')
      win=$(printf '%s' "$got" | tr '\\' '/' | sed 's|^[A-Za-z]:||')
      case "$win" in
        *"/$NAME/$file") ;;
        *) echo "package-windows.sh: $file is staged but the player answered ${got:-nothing}" >&2; fail=1 ;;
      esac
    done <<EOF
d3dpt-exec  d3dpt_exec.dll
dxvk        dxvk_d3d9.dll
EOF
  else
    echo "package-windows.sh: the staged player printed nothing for --companions" >&2
    fail=1
  fi

  # A frame through the staged pair: the display driver's host test
  # (tools/d3dpt-dp2-test.cpp, built by `build-windows.sh exec`) loads the
  # package's own d3dpt_exec.dll and dxvk_d3d9.dll and checks the pixels
  # it reads back. The Windows executor's first real run drew black with
  # every check here green, because nothing had put a batch through it.
  # Under wine this reaches the host GPU through winevulkan; a host with no
  # Vulkan device skips (77).
  if [ -f "$STAGE/dxvk_d3d9.dll" ] && [ -f build/win/d3dpt-dp2-test.exe ]; then
    cp build/win/d3dpt-dp2-test.exe "$scratch/"
    rc=0
    D=1 runpkg D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_DXVK_LIB=dxvk_d3d9.dll \
       "$scratch/d3dpt-dp2-test.exe" "$(winpath "$scratch/dp2.bmp")" > "$scratch/dp2.log" 2>&1 || rc=$?
    ok=$(grep -c '^ok:' "$scratch/dp2.log" || true); bad=$(grep -c '^FAIL' "$scratch/dp2.log" || true)
    if [ "$rc" = 77 ]; then
      echo "direct3d       SKIP: no Vulkan device$([ "$RUN" = wine ] && echo ' under wine')"
    elif [ "$rc" = 0 ] && [ "$bad" = 0 ] && [ "$ok" -gt 0 ]; then
      echo "direct3d       $ok checks through the staged d3dpt_exec.dll + dxvk_d3d9.dll"
    else
      echo "package-windows.sh: the staged Direct3D executor failed its host test (exit $rc, $bad failed):" >&2
      grep '^FAIL\|^exec: \|^dlopen\|^bad \|mismatch' "$scratch/dp2.log" | head -20 >&2
      fail=1
    fi
    # ... and the same records on the *other* backend, the system
    # Direct3D 9 the executor runs on on a real Windows host below the
    # Vulkan 1.3 floor. Natively that is this PC's own system32 d3d9, the
    # real thing, so a failure fails the package. Under wine that name is
    # wine's own d3d9 over WineD3D, a third implementation rather than the
    # user's, so it is reported and never fails the package. It needs a
    # display and GL, which are often absent where a package is rolled.
    rc=0
    D=1 runpkg D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system \
       "$scratch/d3dpt-dp2-test.exe" "$(winpath "$scratch/dp2-system.bmp")" > "$scratch/dp2-system.log" 2>&1 || rc=$?
    sysbad=$(grep -c '^FAIL' "$scratch/dp2-system.log" || true)
    sysok=$(grep -c '^ok:' "$scratch/dp2-system.log" || true)
    if [ "$rc" = 0 ] && [ "$sysbad" = 0 ] && [ "$sysok" -gt 0 ]; then
      echo "direct3d/sys   $sysok checks on $([ "$RUN" = wine ] && echo "wine's own d3d9 (the system-Direct3D-9 backend)" || echo "this PC's system32 d3d9")"
    elif [ "$RUN" = native ]; then
      echo "package-windows.sh: the staged executor failed on the system Direct3D 9 (exit $rc, $sysbad failed):" >&2
      grep '^FAIL\|^exec: \|^dlopen\|^bad \|mismatch' "$scratch/dp2-system.log" | head -20 >&2
      fail=1
    else
      echo "direct3d/sys   not run here (exit $rc, $sysbad failed): wine's d3d9 needs a display and GL — the real check is on Windows"
    fi
  fi

  # The package has to be able to say why it failed
  # (`launcher-core/src/fatal.rs`). A windowed program's start-up failure
  # has no stdout, so it goes into a log. Here the staged binary writes
  # that log in its own prefix (natively, LAUNCHER_DATA_DIR). It is the
  # one file a user is asked for when nothing appeared on screen, and it
  # must hold both the start-up milestones and `--diagnose`'s answers.
  runpkg 2ksbox.exe --diagnose >/dev/null 2>&1 || true
  llog=$(find "$DATA" -name launcher.log 2>/dev/null | head -1)
  if [ -n "$llog" ] && grep -q -- '--- --diagnose ---' "$llog" && grep -q '\[start\] exe = ' "$llog"; then
    echo "launcher.log   start-up milestones and --diagnose, written by the staged launcher"
  else
    echo "package-windows.sh: the staged launcher wrote no launcher.log" >&2
    fail=1
  fi

  # The launcher's C runtime, which the closure above cannot see: an MSVC
  # binary that imports vcruntime140.dll (or a ucrt redistributable DLL)
  # needs a Visual C++ redistributable no Windows comes with, and a PC
  # without it shows a loader dialog before any code of ours runs.
  # `build-windows.sh mitsuami` links it statically; this keeps it so.
  if imports "$STAGE/2ksbox.exe" | grep -qiE '^(vcruntime|msvcp)[0-9]+'; then
    echo "package-windows.sh: 2ksbox.exe imports $(imports "$STAGE/2ksbox.exe" | grep -iE '^(vcruntime|msvcp)[0-9]+' | tr '\n' ' ')(build it with +crt-static: scripts/build-windows.sh mitsuami)" >&2
    fail=1
  else
    echo "c runtime      2ksbox.exe links its C runtime statically"
  fi
  # A window, which `--paths` never opens: WinUI 3 and the Windows App
  # Runtime start only then. The launcher's own headless grab
  # (`LAUNCHER_SHOT`, launcher-mitsuami/src/shot.rs) opens the machine
  # window, draws it into a PNG and exits. Natively that is a verdict (it
  # shows for a moment on this desktop). Wine has no WinUI, so there it is
  # not tried, and the real answer is 2ksbox-debug.bat on a PC.
  shot="$scratch/window.png"
  if [ "$RUN" = native ]; then
    T=90 runpkg LAUNCHER_SHOT="$(winpath "$shot")" 2ksbox.exe >/dev/null 2>&1 || true
    if [ -s "$shot" ]; then
      echo "window         drawn by the staged launcher on WinUI 3"
      rm -f "$shot"
    else
      echo "package-windows.sh: the staged launcher drew no window (LAUNCHER_SHOT); is the Windows App Runtime 2.4+ installed?" >&2
      fail=1
    fi
  else
    echo "window         (WinUI 3 does not run under wine; the real answer is 2ksbox-debug.bat on a PC)"
  fi

  # The bundle-creating path end to end: the staged launcher runs the
  # staged qemu-img to make a disk, and turns the result into a command
  # line pointing at the staged firmware. This is also what proves the
  # DLL closure: qemu-img.exe cannot start without every DLL beside it.
  runpkg 2ksbox.exe --wizard-new xp "Package check" 1 >/dev/null 2>&1 || true
  disk=$(find "$scratch" "$DATA" -name disk.qcow2 2>/dev/null | head -1)
  if [ -n "$disk" ] && [ -s "$disk" ]; then
    echo "qemu-img       created $(du -h "$disk" | cut -f1) of qcow2"
  else
    echo "package-windows.sh: the packaged qemu-img did not create a disk" >&2
    fail=1
  fi
  # Offscreen GL, which is what a Win98 guest's 3D needs (the embed
  # library's WGL backend). Reported, never fatal. It is a property of the
  # machine that runs the package, and wine's GL is not the target's; a
  # Windows user runs tools\wgl-probe.exe there for the real answer.
  # Unlike the checks above, this one needs a display. It opens a real,
  # invisible window, which wine with no DISPLAY makes impossible.
  if [ -f "$STAGE/tools/wgl-probe.exe" ]; then
    if out=$(D=1 runpkg tools/wgl-probe.exe 2>/dev/null); then
      echo "wgl-probe      $(printf '%s\n' "$out" | tail -1)"
    else
      echo "wgl-probe      no offscreen GL$([ "$RUN" = wine ] && echo ' under wine'): $(printf '%s\n' "$out" | tail -1)"
    fi
  fi
else
  echo "checks         (no wine on this host; the package was not run)"
fi
[ "$fail" = 0 ] || exit 1
echo "checks passed"

du -sh "$STAGE" | sed 's/^/staged  /'
if [ "$ZIP" = 1 ]; then
  # A zip, because that is what a Windows user is handed. `zip` is not on
  # every Linux (this project's own host has none); Python's zipfile makes
  # the same archive and is always there, so it is the fallback.
  archive="$OUT/$NAME.zip"
  rm -f "$archive"
  if command -v zip >/dev/null; then
    (cd "$OUT" && zip -qr "$archive" "$NAME")
  else
    python3 -c 'import shutil,sys; shutil.make_archive(sys.argv[1], "zip", sys.argv[2], sys.argv[3])' \
      "${archive%.zip}" "$OUT" "$NAME"
  fi
  du -h "$archive" | sed 's/^/zip     /'
fi
# The same tree as an MSIX layout for the Store (docs/build-windows.md,
# "The Store package"). Only after the checks: the MSIX is the zip's
# contents and no more, so the zip's evidence is its evidence. A Linux
# host has no makeappx, so this stops at the layout and says how to
# finish on a PC.
if [ "$MSIX" = 1 ]; then
  scripts/package-msix.sh "$STAGE" --out "$OUT"
fi
echo "package: $STAGE"
