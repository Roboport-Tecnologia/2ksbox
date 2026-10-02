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
# `2ksbox.exe` is `launcher-qt`, the Qt 6 / QML launcher (ADR-015), and
# the package carries the Qt runtime it needs: the DLLs, the platform
# plugin and the QML module trees, none of which Windows has.
#
# Run it on the host, not inside scripts/win-cross.sh, because the checks
# need wine and the cross image does not carry it. It builds nothing.
# scripts/build-windows.sh does that, and this script says so if an
# artefact is missing.
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
#   *.dll                       the mingw and Qt 6 runtimes those need
#   plugins\                    Qt's platform plugin and friends
#   qml\                        the QtQuick module trees the views import
#   qt.conf                     where Qt looks for those two
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
    -h|--help) sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package-windows.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="2ksbox-$VERSION-windows-x86_64"
STAGE="$OUT/$NAME"
TARGET="$ROOT/target/x86_64-pc-windows-gnu/release"
QT_TARGET="$ROOT/launcher-qt/target/x86_64-pc-windows-gnu/release"
Q="$ROOT/build/win/qemu"

need() { [ -e "$1" ] || { echo "package-windows.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need "$Q/libqemu-embed-i386.dll" "scripts/build-windows.sh qemu"
need "$Q/qemu-img.exe"           "scripts/build-windows.sh qemu"
need "$QT_TARGET/launcher-qt.exe" "scripts/build-windows.sh qt"
need "$TARGET/player.exe"        "scripts/build-windows.sh rust"
need qemu/pc-bios                "scripts/prepare-qemu.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE/doc"

install -m755 "$QT_TARGET/launcher-qt.exe" "$STAGE/2ksbox.exe"
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
  STRIP=${WIN_STRIP:-x86_64-w64-mingw32-strip}
  if command -v "$STRIP" >/dev/null; then "$STRIP" --strip-debug "$STAGE/dxvk_d3d9.dll"
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

# --- the Qt runtime ---------------------------------------------------
# Qt needs more than its DLLs: a platform plugin (there is no window
# without `platforms/qwindows.dll`) and the QML modules the views import,
# neither of which is in any import table. Fedora's mingw packages have no
# cross `windeployqt`, so this is that step written out: the plugin
# directories, the three QML module trees `qml/*.qml`
# imports (QtQuick pulls Controls, Layouts, Dialogs, Templates and
# Effects with it), and a `qt.conf` so Qt resolves both relative to the
# executable instead of to the build machine's absolute paths.
QTROOT=${WIN_QTROOT:-/usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6}
if [ ! -d "$QTROOT" ]; then
  QTROOT="$ROOT/build/win/qt6"
  echo "==> copying the mingw Qt runtime out of the cross image"
  rm -rf "$QTROOT"      # for the same reason as the sysroot copy above
  mkdir -p "$QTROOT"
  scripts/win-cross.sh bash -c \
    "cp -a /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/plugins \
           /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/qml '$QTROOT/'"
fi
need "$QTROOT/plugins/platforms" "the cross image's mingw Qt 6"
mkdir -p "$STAGE/plugins" "$STAGE/qml"
for d in platforms imageformats iconengines styles tls; do
  [ -d "$QTROOT/plugins/$d" ] && cp -a "$QTROOT/plugins/$d" "$STAGE/plugins/"
done
for m in QtQuick QtQml QtCore; do
  [ -d "$QTROOT/qml/$m" ] && cp -a "$QTROOT/qml/$m" "$STAGE/qml/"
done
cat > "$STAGE/qt.conf" <<'EOF'
; Qt's own paths, relative to this executable. Without it a deployed
; build looks for its plugins and QML modules where they were on the
; machine that compiled Qt.
[Paths]
Prefix = .
Plugins = plugins
Qml2Imports = qml
EOF

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
SYSROOT=${WIN_SYSROOT:-/usr/x86_64-w64-mingw32/sys-root/mingw/bin}
OBJDUMP=${WIN_OBJDUMP:-x86_64-w64-mingw32-objdump}
if [ ! -d "$SYSROOT" ]; then
  # The sysroot lives in the cross container, so ask it for a copy every
  # time. A kept copy is a snapshot of an older image; one from before Qt
  # was in the image once quietly packaged a launcher with no Qt6Core.dll.
  SYSROOT="$ROOT/build/win/sysroot-bin"
  echo "==> copying the mingw runtime out of the cross image"
  rm -rf "$SYSROOT"
  mkdir -p "$SYSROOT"
  scripts/win-cross.sh bash -c \
    "cp -a /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll '$SYSROOT/'"
fi
command -v "$OBJDUMP" >/dev/null || { echo "package-windows.sh: no $OBJDUMP (WIN_OBJDUMP=)"; exit 1; }

imports() { "$OBJDUMP" -p "$1" | sed -n 's/^\tDLL Name: //p'; }

# Every binary in the package is a root, not just the ones at the top. A
# Qt platform plugin or a QML module's DLL sits in a subdirectory, imports
# half of Qt, and is loaded by name at run time, so nothing above it names
# what it needs. Its imports resolve from the executable's own directory,
# which is where the closure puts everything.
staged_binaries() { find "$STAGE" \( -name '*.dll' -o -name '*.exe' \) -type f; }

declare -A seen=()
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

if command -v wine >/dev/null; then
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' EXIT
  export WINEPREFIX="$scratch/wine" WINEDEBUG=-all
  # One prefix, created once, so every check below runs in the same one.
  wine wineboot -i >/dev/null 2>&1 || true

  runw() { (cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" \
                WINEDEBUG=-all PATH="$PATH" wine "$@" 2>/dev/null); }

  resolved=$(runw 2ksbox.exe --paths || true)
  if [ -z "$resolved" ]; then
    # The Qt launcher once could not answer, while its `std::call_once`
    # died before `main` (M11). That bug is fixed, so silence fails.
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
  # is the launcher's own switch for answering as a packaged build.
  packaged=$(cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all \
      PATH="$PATH" LAUNCHER_PACKAGED=1 wine 2ksbox.exe --paths 2>/dev/null | awk '$1 == "library" { print }' || true)
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
  companions=$(runw 2ksbox-player.exe --companions || true)
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
    (cd "$STAGE" && WINEDEBUG=-all D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_DXVK_LIB=dxvk_d3d9.dll \
       timeout 300 wine "$scratch/d3dpt-dp2-test.exe" "$scratch/dp2.bmp" > "$scratch/dp2.log" 2>&1) || rc=$?
    ok=$(grep -c '^ok:' "$scratch/dp2.log" || true); bad=$(grep -c '^FAIL' "$scratch/dp2.log" || true)
    if [ "$rc" = 77 ]; then
      echo "direct3d       SKIP: no Vulkan device under wine"
    elif [ "$rc" = 0 ] && [ "$bad" = 0 ] && [ "$ok" -gt 0 ]; then
      echo "direct3d       $ok checks through the staged d3dpt_exec.dll + dxvk_d3d9.dll"
    else
      echo "package-windows.sh: the staged Direct3D executor failed its host test (exit $rc, $bad failed):" >&2
      grep '^FAIL\|^exec: \|^dlopen\|^bad \|mismatch' "$scratch/dp2.log" | head -20 >&2
      fail=1
    fi
    # ... and the same records on the *other* backend, the system
    # Direct3D 9 the executor runs on on a real Windows host below the
    # Vulkan 1.3 floor. Under wine that name is wine's own d3d9 over
    # WineD3D, a third implementation rather than the user's, so it is
    # reported and never fails the package. It needs a display and GL,
    # which are often absent where a package is rolled.
    rc=0
    (cd "$STAGE" && WINEDEBUG=-all D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system \
       timeout 300 wine "$scratch/d3dpt-dp2-test.exe" "$scratch/dp2-system.bmp" > "$scratch/dp2-system.log" 2>&1) || rc=$?
    sysbad=$(grep -c '^FAIL' "$scratch/dp2-system.log" || true)
    sysok=$(grep -c '^ok:' "$scratch/dp2-system.log" || true)
    if [ "$rc" = 0 ] && [ "$sysbad" = 0 ] && [ "$sysok" -gt 0 ]; then
      echo "direct3d/sys   $sysok checks on wine's own d3d9 (the system-Direct3D-9 backend)"
    else
      echo "direct3d/sys   not run here (exit $rc, $sysbad failed): wine's d3d9 needs a display and GL — the real check is on Windows"
    fi
  fi

  # The package has to be able to say why it failed
  # (`launcher-core/src/fatal.rs`). A windowed program's start-up failure
  # has no stdout, so it goes into a log. Here the staged binary writes
  # that log in its own prefix. It is the one file a user is asked for when
  # nothing appeared on screen, and it must hold both the start-up
  # milestones and `--diagnose`'s answers.
  runw 2ksbox.exe --diagnose >/dev/null 2>&1 || true
  llog=$(find "$WINEPREFIX/drive_c/users" -name launcher.log 2>/dev/null | head -1)
  if [ -n "$llog" ] && grep -q -- '--- --diagnose ---' "$llog" && grep -q '\[start\] exe = ' "$llog"; then
    echo "launcher.log   start-up milestones and --diagnose, written by the staged launcher"
  else
    echo "package-windows.sh: the staged launcher wrote no launcher.log" >&2
    fail=1
  fi

  # A window, which `--paths` never opens. Qt finds its platform plugin
  # and its QML modules by name at run time, out of `plugins\` and
  # `qml\` beside the executable, and no import table says so. A package
  # that answers every question above can still show nothing on a real PC.
  # Under wine the grab is a report, not a verdict (wine's Qt is not the
  # target's), but the staged files below are a verdict: without
  # `qwindows.dll` the package cannot open a window anywhere.
  if [ -f "$STAGE/plugins/platforms/qwindows.dll" ] && [ -f "$STAGE/qml/QtQuick/qmldir" ]; then
    echo "qt runtime     platforms\\qwindows.dll and the QtQuick modules are staged"
  else
    echo "package-windows.sh: no plugins\\platforms\\qwindows.dll or no qml\\QtQuick: the launcher would open no window" >&2
    fail=1
  fi
  # Z: is wine's view of /, so the grab lands in the same scratch
  # directory everything else here uses.
  shot="$scratch/window.png"
  winshot="Z:$(printf '%s' "$scratch" | tr '/' '\\')\\window.png"
  # `timeout`: the grab is the one call here that opens a Qt window, and a
  # Qt window under wine with the offscreen plugin and no display can
  # never come back. Without the bound the script hangs here instead of
  # taking the "no offscreen grab" branch below.
  (cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all \
      PATH="$PATH" QT_QPA_PLATFORM=offscreen LAUNCHER_QT_SHOT="$winshot" \
      LAUNCHER_QT_DELAY=2000 timeout 90 wine 2ksbox.exe >/dev/null 2>&1) || true
  if [ -s "$shot" ]; then
    echo "window         grabbed offscreen under wine: QML, plugins and all"
    rm -f "$shot"
  else
    echo "window         (no offscreen grab under wine; the real answer is 2ksbox-debug.bat on a PC)"
  fi

  # The bundle-creating path end to end: the staged launcher runs the
  # staged qemu-img to make a disk, and turns the result into a command
  # line pointing at the staged firmware. This is also what proves the
  # DLL closure: qemu-img.exe cannot start without every DLL beside it.
  runw 2ksbox.exe --wizard-new xp "Package check" 1 >/dev/null || true
  disk=$(find "$scratch" "$WINEPREFIX/drive_c/users" -name disk.qcow2 2>/dev/null | head -1)
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
  # invisible window, which the others' `env -i` makes impossible.
  runw_display() { (cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" \
        WINEDEBUG=-all PATH="$PATH" DISPLAY="${DISPLAY:-}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        wine "$@" 2>/dev/null); }
  if [ -f "$STAGE/tools/wgl-probe.exe" ]; then
    if out=$(runw_display tools/wgl-probe.exe); then
      echo "wgl-probe      $(printf '%s\n' "$out" | tail -1)"
    else
      echo "wgl-probe      no offscreen GL under wine: $(printf '%s\n' "$out" | tail -1)"
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
