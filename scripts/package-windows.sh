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
# Everything in it is MSVC with a static C runtime (ADR-026's third
# amendment), so it ships no runtime DLL. `2ksbox.exe` is
# `launcher-mitsuami`, the WinUI 3 launcher (ADR-023), and
# `2ksbox-player.exe` is `player-mitsuami` (track M22), both from
# `build-windows.sh mitsuami`; QEMU (`libqemu-embed-i386.dll`,
# `qemu-img.exe`) is the build against MSVC's runtime in
# build/win/qemu-msvc (`build-windows.sh qemu-msvc`), its libraries linked
# in. The launcher and the player run on the Windows App Runtime 2.4 or
# later, a
# framework Microsoft installs once per PC; the zip cannot carry it, and
# the Store's MSIX declares it as a dependency instead. So the package
# comes from a Windows PC, where they are built.
#
# It runs in MSYS2's MINGW64 shell on Windows, after a native build, and
# builds nothing (scripts/build-windows.sh does that, and this script says
# so if an artefact is missing). Windows packages are made only there
# (ADR-026; the cross build from Linux and its wine checks are retired).
# The mingw runtime and binutils are MSYS2's own (/mingw64), and the
# checks run the package itself, with nothing but Windows on PATH and the
# launcher's data in a scratch directory (LAUNCHER_DATA_DIR), not the
# user's %APPDATA%. Every check is a verdict, because this is the target.
#
# A Windows package is one folder, not a Unix prefix. The executables sit
# at the top with every DLL beside them, which is where the loader looks
# (no rpath, no PATH), and the data directories under them. The launcher
# knows both shapes (launcher-core/src/paths.rs).
#
#   2ksbox.exe                  the launcher
#   2ksbox-player.exe           the player (player-mitsuami)
#   2ksbox-player-x86_64.exe    the same player for Windows 11 (track M20)
#   qemu-img.exe                ours, patched
#   libqemu-embed-i386.dll      QEMU as a library, what the player runs
#   libqemu-embed-x86_64.dll    ... and the Windows 11 player's (WHPX)
#   d3dpt_exec.dll              the Direct3D executor (doc 14)
#   dxvk_d3d9.dll               DXVK's d3d9, the executor's default,
#                               renamed so it is never mistaken for
#                               Windows' own d3d9.dll (D3DPT_D3D9=system)
#   pc-bios\                    QEMU firmware
#   soundfonts\                 the General MIDI bank (doc 20)
#   guest-tools\                the guest-tools ISO
#   drivers\                    Windows 11's drivers disc (x64), with the
#                               answer file that skips setup's TPM and
#                               Secure Boot checks (build-virtio-win.sh)
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
    -h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package-windows.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# MSYS2's MINGW64 shell only: the shell build-windows.sh builds in (the
# package itself is MSVC), and the Windows that runs its checks. As in build-windows.sh.
case "${MSYSTEM:-}" in
  MINGW64) ;;
  "") echo "package-windows.sh: Windows packages are made on Windows, in MSYS2's MINGW64 shell (ADR-026)" >&2; exit 1 ;;
  *) echo "package-windows.sh: this is MSYS2's $MSYSTEM shell; open the MINGW64 one" >&2; exit 1 ;;
esac

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="2ksbox-$VERSION-windows-x86_64"
STAGE="$OUT/$NAME"
LAUNCHER="$ROOT/launcher-mitsuami/target/release/launcher-mitsuami.exe"
PLAYER="$ROOT/player-mitsuami/target/release/player-mitsuami.exe"
PLAYER64="$ROOT/player-mitsuami/target/qemu-x86_64/release/player-mitsuami.exe"
Q="$ROOT/build/win/qemu-msvc"

need() { [ -e "$1" ] || { echo "package-windows.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need "$Q/libqemu-embed-i386.dll" "scripts/build-windows.sh qemu-msvc"
need "$Q/libqemu-embed-x86_64.dll" "scripts/build-windows.sh qemu-msvc"
need "$Q/qemu-img.exe"           "scripts/build-windows.sh qemu-msvc"
need "$LAUNCHER"                 "scripts/build-windows.sh mitsuami, on Windows"
need "$PLAYER"                   "scripts/build-windows.sh mitsuami, on Windows"
need "$PLAYER64"                 "scripts/build-windows.sh mitsuami, on Windows"
need qemu/pc-bios                "scripts/prepare-qemu.sh"
need qemu/pc-bios/edk2-x86_64-code.fd "scripts/prepare-qemu.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE/doc"

install -m755 "$LAUNCHER" "$STAGE/2ksbox.exe"
install -m755 "$PLAYER" "$STAGE/2ksbox-player.exe"
install -m755 "$PLAYER64" "$STAGE/2ksbox-player-x86_64.exe"
install -m755 "$Q/qemu-img.exe" "$STAGE/"
install -m755 "$Q/libqemu-embed-i386.dll" "$Q/libqemu-embed-x86_64.dll" "$STAGE/"
cp -a qemu/pc-bios "$STAGE/pc-bios"

# The Direct3D executor is optional at run time (the device says "no
# executor" and the guest falls back), so a package without it is not a
# failure. Say so anyway, because "3D does nothing" is hard to trace back
# to a packaging step.
#
# Both or neither, as on the other hosts. DXVK's d3d9 is the executor's default;
# Windows' own system32 d3d9 is only the fallback below the Vulkan floor
# (D3DPT_D3D9). Both are MSVC (ADR-026's amendment), whose symbols stay
# in the PDB beside the build and are not shipped.
if [ -f build/win/d3dpt/d3dpt_exec.dll ] && [ -f build/win/dxvk/src/d3d9/d3d9.dll ]; then
  install -m755 build/win/d3dpt/d3dpt_exec.dll "$STAGE/"
  install -m755 build/win/dxvk/src/d3d9/d3d9.dll "$STAGE/dxvk_d3d9.dll"
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

# Windows 11's drivers disc (`disc_library::drivers_iso`): its answer file
# lets setup past the TPM and Secure Boot checks a Windows host's machine
# cannot pass, so a package without it installs no Windows 11.
drivers=build/virtio-win/2ksbox-drivers-x64.iso
if [ -f "$drivers" ]; then
  install -Dm644 "$drivers" "$STAGE/drivers/2ksbox-drivers-x64.iso"
else
  echo "package-windows.sh: no $drivers (scripts/build-windows.sh virtio); packaging without it, so Windows 11 setup stops at its TPM check"
fi

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

# --- no mingw DLLs ---------------------------------------------------
# Everything in the package is MSVC with a static C runtime (ADR-026's
# third amendment): QEMU from build/win/qemu-msvc, whose libraries are
# linked in statically (build-deps.sh), the launcher and the player, the
# Direct3D pair and the tools. So the package carries no runtime DLL at
# all, and every DLL a staged binary imports is Windows' own. A mingw
# binary slipping back in (the mingw QEMU, a tool built on the GNU
# target) would want MSYS2's libraries beside it, the classic Windows
# failure: a dialog naming a DLL before a single line of ours runs. The
# import tables, walked with objdump, are the source of truth, and any
# name MSYS2's /mingw64/bin has fails the package.
#
# Seeded with what is Windows' even where MSYS2 carries a copy: the
# Vulkan loader, which DXVK and the executor name, comes with the GPU
# driver and must not ship ("vulkan (the system's loader)" in the
# launcher's --paths).
SYSROOT=${WIN_SYSROOT:-/mingw64/bin}
OBJDUMP=${WIN_OBJDUMP:-objdump}
command -v "$OBJDUMP" >/dev/null || { echo "package-windows.sh: no $OBJDUMP (WIN_OBJDUMP=)"; exit 1; }

imports() { "$OBJDUMP" -p "$1" | sed -n 's/^\tDLL Name: //p'; }

# Every binary in the package is a root, not just the ones at the top
# (tools\wgl-probe.exe sits in a subdirectory).
staged_binaries() { find "$STAGE" \( -name '*.dll' -o -name '*.exe' \) -type f; }

# A DLL that is *loaded* rather than imported is invisible to an import
# table (Fedora's sdl2-compat once LoadLibrary'd SDL3.dll and a packaged
# player died on "Failed loading SDL3 library"), so every DLL name in a
# staged binary's strings is held to the same test.
runtime_deps() { strings -a "$1" | grep -oiE '[A-Za-z0-9_.+-]+\.dll' | sort -u; }
command -v strings >/dev/null || { echo "package-windows.sh: no strings(1)"; exit 1; }

mingw=0
while read -r file; do
  [ -n "$file" ] || continue
  while read -r how dll; do
    [ -n "$dll" ] || continue
    key=$(printf '%s' "$dll" | tr 'A-Z' 'a-z')
    [ "$key" != vulkan-1.dll ] || continue
    if [ -e "$SYSROOT/$dll" ] || [ -e "$SYSROOT/$key" ]; then
      echo "package-windows.sh: ${file#"$STAGE"/} $how $dll, a mingw DLL (build it with MSVC: scripts/build-windows.sh)" >&2
      mingw=1
    fi
  done < <(imports "$file" | sed 's/^/imports /'; runtime_deps "$file" | sed 's/^/names /')
done < <(staged_binaries)
[ "$mingw" = 0 ] || exit 1
echo "imports        Windows' own DLLs only, no runtime DLL shipped"

# --- the check --------------------------------------------------------
# The staged binaries, run as Windows binaries, from outside the checkout,
# with an empty environment, so no LAUNCHER_*/PLAYER_* knob from this
# shell can make them work and nothing may resolve back into the build
# tree.
fail=0
{
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' EXIT
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

  # The checks run *in* the package folder, and whatever a program there
  # writes into its working directory would ship: a DXVK log, and with no
  # %ProgramData% NVIDIA's driver's `NVIDIA Corporation\umdlogs`. Both go
  # to the scratch directory (with %LOCALAPPDATA%, DXVK's shader cache),
  # and the tree is compared afterwards.
  find "$STAGE" | sort > "$scratch/stage-before"

  # runpkg [VAR=value ...] program [args ...]: run a staged program from
  # the package folder with an empty environment, so no LAUNCHER_* or
  # PLAYER_* knob from this shell can make it work and nothing may resolve
  # back into the build tree. T= bounds it (default 300 s), and IN= runs
  # it from another directory.
  runpkg() {
    local vars=() prog
    while [[ "${1:-}" == *=* ]]; do vars+=("$1"); shift; done
    case "$1" in /*) prog=$1 ;; *) prog=$STAGE/$1 ;; esac
    (cd "${IN:-$STAGE}" && timeout "${T:-300}" env -i SYSTEMROOT="$WINDOWS" WINDIR="$WINDOWS" \
        PATH="$WINPATH" TEMP="$(winpath "$scratch")" TMP="$(winpath "$scratch")" \
        LOCALAPPDATA="$(winpath "$scratch")" PROGRAMDATA="$(winpath "$scratch")" \
        DXVK_LOG_PATH="$(winpath "$scratch")" \
        LAUNCHER_DATA_DIR="$(winpath "$DATA")" "${vars[@]}" "$prog" "${@:2}")
  }

  resolved=$(runpkg 2ksbox.exe --paths 2>/dev/null || true)
  if [ -z "$resolved" ]; then
    # A launcher that dies before `main` answers nothing, so silence
    # fails.
    echo "package-windows.sh: the staged launcher printed nothing for --paths" >&2
    fail=1
  else
    printf '%s\n' "$resolved"
    # Every companion must resolve inside the package; compare on the
    # path's tail, without the drive.
    while read -r what path; do
      case "$what" in player|player-x86_64|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
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
  # The profile from Windows itself (CSIDL_PROFILE), as the launcher
  # asks for it: USERPROFILE is not always in an MSYS2 environment.
  home=$(cygpath -u "$(cygpath -F 40)")/2ksbox
  if [ ! -e "$home" ]; then made=$home
  elif [ -f "$home/launcher.log" ]; then kept=$home/launcher.log; cp -p "$kept" "$scratch/kept.log"
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
  # A PC with no Vulkan device skips (77).
  if [ -f "$STAGE/dxvk_d3d9.dll" ] && [ -f build/win/d3dpt-dp2-test.exe ]; then
    cp build/win/d3dpt-dp2-test.exe "$scratch/"
    rc=0
    runpkg D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_DXVK_LIB=dxvk_d3d9.dll \
       "$scratch/d3dpt-dp2-test.exe" "$(winpath "$scratch/dp2.bmp")" > "$scratch/dp2.log" 2>&1 || rc=$?
    ok=$(grep -c '^ok:' "$scratch/dp2.log" || true); bad=$(grep -c '^FAIL' "$scratch/dp2.log" || true)
    if [ "$rc" = 77 ]; then
      echo "direct3d       SKIP: no Vulkan device"
    elif [ "$rc" = 0 ] && [ "$bad" = 0 ] && [ "$ok" -gt 0 ]; then
      echo "direct3d       $ok checks through the staged d3dpt_exec.dll + dxvk_d3d9.dll"
    else
      echo "package-windows.sh: the staged Direct3D executor failed its host test (exit $rc, $bad failed):" >&2
      { grep '^FAIL' "$scratch/dp2.log"; grep '^exec: \|^dlopen\|^bad \|mismatch' "$scratch/dp2.log"; } | head -20 >&2
      fail=1
    fi
    # ... and the same records on the *other* backend, the system
    # Direct3D 9 the executor runs on on a real Windows host below the
    # Vulkan 1.3 floor: this PC's own system32 d3d9, the real thing, so
    # a failure fails the package.
    rc=0
    runpkg D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system \
       "$scratch/d3dpt-dp2-test.exe" "$(winpath "$scratch/dp2-system.bmp")" > "$scratch/dp2-system.log" 2>&1 || rc=$?
    sysbad=$(grep -c '^FAIL' "$scratch/dp2-system.log" || true)
    sysok=$(grep -c '^ok:' "$scratch/dp2-system.log" || true)
    if [ "$rc" = 0 ] && [ "$sysbad" = 0 ] && [ "$sysok" -gt 0 ]; then
      echo "direct3d/sys   $sysok checks on this PC's system32 d3d9"
    else
      echo "package-windows.sh: the staged executor failed on the system Direct3D 9 (exit $rc, $sysbad failed):" >&2
      { grep '^FAIL' "$scratch/dp2-system.log"; grep '^exec: \|^dlopen\|^bad \|mismatch' "$scratch/dp2-system.log"; } | head -20 >&2
      fail=1
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

  # The C runtime, which the mingw check above cannot see: an MSVC
  # binary that imports vcruntime140.dll (or a ucrt redistributable DLL)
  # needs a Visual C++ redistributable no Windows comes with, and a PC
  # without it shows a loader dialog before any code of ours runs.
  # `build-windows.sh mitsuami` links it statically, and so do QEMU's
  # (`-Db_vscrt=mt`), the Direct3D pair's (`build-windows.sh exec`, /MT and
  # DXVK's own b_vscrt) and the WGL probe's; this keeps it so.
  for exe in 2ksbox.exe 2ksbox-player.exe 2ksbox-player-x86_64.exe \
             libqemu-embed-i386.dll libqemu-embed-x86_64.dll qemu-img.exe \
             d3dpt_exec.dll dxvk_d3d9.dll tools/wgl-probe.exe; do
    [ -f "$STAGE/$exe" ] || continue        # the Direct3D pair: not built here; warned above
    if imports "$STAGE/$exe" | grep -qiE '^(vcruntime|msvcp)[0-9]+'; then
      echo "package-windows.sh: $exe imports $(imports "$STAGE/$exe" | grep -iE '^(vcruntime|msvcp)[0-9]+' | tr '\n' ' ')(link its C runtime statically: scripts/build-windows.sh mitsuami / qemu-msvc / exec)" >&2
      fail=1
    else
      echo "c runtime      $exe links its C runtime statically"
    fi
  done
  # A window, which `--paths` never opens: WinUI 3 and the Windows App
  # Runtime start only then. The launcher's own headless grab
  # (`LAUNCHER_SHOT`, launcher-mitsuami/src/shot.rs) opens the machine
  # window, draws it into a PNG and exits (it shows for a moment on this
  # desktop).
  shot="$scratch/window.png"
  T=90 runpkg LAUNCHER_SHOT="$(winpath "$shot")" 2ksbox.exe >/dev/null 2>&1 || true
  if [ -s "$shot" ]; then
    echo "window         drawn by the staged launcher on WinUI 3"
    rm -f "$shot"
  else
    echo "package-windows.sh: the staged launcher drew no window (LAUNCHER_SHOT); is the Windows App Runtime 2.4+ installed?" >&2
    fail=1
  fi
  # The player, which --companions above starts but never runs a machine
  # in: natively, a machine with a General MIDI port (the Win98 and DOS
  # default) to the BIOS screen and a clean exit, through QMP's quit once
  # the guest has drawn. That is WinUI 3, the surface's Direct3D 12, QEMU's
  # DLL, the bank the player names from the package, and `-netdev user`,
  # which every machine the launcher writes asks for (bundle.rs). That
  # backend is compiled in through libslirp, now linked statically, so no
  # import table shows it; without it the first machine started on a real
  # PC once died on "network backend 'user' is not compiled into this
  # binary" with every check here green. It runs from the scratch
  # directory: from the
  # package's own, QEMU's relative soundfonts\ would find the bank anyway,
  # and outside it QEMU refused such a machine until the embed library set
  # the variable (doc 11, "The C runtime boundary"). The window shows for
  # a moment.
  if IN="$scratch" T=90 runpkg PLAYER_QMP_EXEC='{"execute":"quit"}' 2ksbox-player.exe -- \
       -L "$(winpath "$STAGE/pc-bios")" -M pc -m 32 -device mpu401,audiodev=embed0,synth=gm \
       -netdev user,id=net0 -device rtl8139,netdev=net0 \
       > "$scratch/player.txt" 2>&1; then
    echo "player         ran a machine to its BIOS and quit (WinUI 3, the embed DLL, the bank, -netdev user)"
  else
    echo "package-windows.sh: the staged player did not run a machine to its BIOS and quit:" >&2
    grep -v '^\[audio\]' "$scratch/player.txt" | tail -5 >&2
    fail=1
  fi
  # ... and Windows 11's player on the x86_64 DLL, which nothing above
  # loads: the same BIOS run on a q35 board under TCG (the launcher asks
  # for WHPX, which this PC need not have enabled).
  if IN="$scratch" T=90 runpkg PLAYER_QMP_EXEC='{"execute":"quit"}' 2ksbox-player-x86_64.exe -- \
       -L "$(winpath "$STAGE/pc-bios")" -M q35 -accel tcg -m 128 \
       -netdev user,id=net0 -device e1000e,netdev=net0 \
       > "$scratch/player64.txt" 2>&1; then
    echo "player-x86_64  ran a q35 machine to its BIOS and quit (libqemu-embed-x86_64.dll)"
  else
    echo "package-windows.sh: the staged x86_64 player did not run a machine to its BIOS and quit:" >&2
    grep -v '^\[audio\]' "$scratch/player64.txt" | tail -5 >&2
    fail=1
  fi

  # The bundle-creating path end to end: the staged launcher runs the
  # staged qemu-img to make a disk, and turns the result into a command
  # line pointing at the staged firmware.
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
  # machine that runs the package; a user runs tools\wgl-probe.exe on
  # theirs for the real answer.
  if [ -f "$STAGE/tools/wgl-probe.exe" ]; then
    if out=$(runpkg tools/wgl-probe.exe 2>/dev/null); then
      echo "wgl-probe      $(printf '%s\n' "$out" | tail -1)"
    else
      echo "wgl-probe      no offscreen GL: $(printf '%s\n' "$out" | tail -1)"
    fi
  fi
  if ! find "$STAGE" | sort | diff "$scratch/stage-before" - > "$scratch/stage-diff"; then
    echo "package-windows.sh: the checks changed the staged tree (new entries removed):" >&2
    grep '^[<>]' "$scratch/stage-diff" | head -20 >&2
    sed -n 's/^> //p' "$scratch/stage-diff" | sort -r | while IFS= read -r f; do rm -rf "$f"; done
    fail=1
  fi
}
[ "$fail" = 0 ] || exit 1
echo "checks passed"

du -sh "$STAGE" | sed 's/^/staged  /'
if [ "$ZIP" = 1 ]; then
  # A zip, because that is what a Windows user is handed. MSYS2 may have
  # no `zip`; Python's zipfile makes the same archive and is always
  # there, so it is the fallback.
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
# contents and no more, so the zip's evidence is its evidence.
if [ "$MSIX" = 1 ]; then
  scripts/package-msix.sh "$STAGE" --out "$OUT"
fi
echo "package: $STAGE"
