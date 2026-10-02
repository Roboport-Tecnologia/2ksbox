#!/usr/bin/env bash
# Run the native Windows build from the checkout, in MSYS2's MINGW64
# shell (docs/build-windows.md, "Building on Windows"). A package finds
# everything because it is one folder. A checkout keeps the same files in
# build/win/ and target/, so this script points each program at them.
#
#   scripts/win-run.sh launcher [args...]   the launcher, launcher-mitsuami (2ksbox.exe in a
#                                           package; its MSVC release build)
#   scripts/win-run.sh player [args...]     the player (2ksbox-player.exe)
#   scripts/win-run.sh qemu [args...]       qemu-system-i386.exe: no window of its
#                                           own, -display vnc=:0 to look at a guest
#   GDB=1 scripts/win-run.sh player ...     any of them under gdb
#
# What it sets, each only when the caller has not:
#   PATH                  build/win/qemu first, for libqemu-embed-i386.dll.
#                         The mingw runtime comes from /mingw64/bin, already
#                         on this shell's PATH
#   LAUNCHER_PLAYER_BIN   the launcher is built into its own target/ and
#                         looks beside itself and in target/<profile>, where
#                         a --target build never puts the player
#   D3DPT_EXEC_LIB        QEMU's own search is relative to the working directory
#   D3DPT_DXVK_LIB        the executor loads DXVK only by the package's name for
#                         it, dxvk_d3d9.dll (never plain d3d9.dll, which is
#                         Windows' own), so the build is copied to that name
#
# The launcher is a windowed program and writes nothing to this terminal.
# Its log is %APPDATA%\2ksbox\data\launcher.log, and a machine's is
# player.log beside it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ "${MSYSTEM:-}" = MINGW64 ] || { echo "win-run.sh: run it in MSYS2's MINGW64 shell" >&2; exit 1; }

REL="$ROOT/target/x86_64-pc-windows-gnu/release"
case "${1:-}" in
  launcher|mitsuami) BIN="$ROOT/launcher-mitsuami/target/release/launcher-mitsuami.exe"; STAGE=mitsuami ;;
  player)   BIN="$REL/player.exe"; STAGE=rust ;;
  qemu)     BIN="$ROOT/build/win/qemu/qemu-system-i386.exe"; STAGE=qemu ;;
  *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
shift
if [ ! -x "$BIN" ]; then
  echo "win-run.sh: no $BIN (scripts/build-windows.sh $STAGE)" >&2
  exit 1
fi

# Native programs read these, so Windows paths rather than MSYS2's /c/...
win() { cygpath -w "$1"; }

export PATH="$ROOT/build/win/qemu:$REL:$PATH"
[ -n "${LAUNCHER_PLAYER_BIN:-}" ] || export LAUNCHER_PLAYER_BIN="$(win "$REL/player.exe")"

EXEC="$ROOT/build/win/d3dpt/d3dpt_exec.dll"
DXVK="$ROOT/build/win/dxvk/src/d3d9/d3d9.dll"
if [ -f "$EXEC" ] && [ -f "$DXVK" ]; then
  RENAMED="$ROOT/build/win/d3dpt/dxvk_d3d9.dll"
  cmp -s "$DXVK" "$RENAMED" || cp "$DXVK" "$RENAMED"
  [ -n "${D3DPT_EXEC_LIB:-}" ] || export D3DPT_EXEC_LIB="$(win "$EXEC")"
  [ -n "${D3DPT_DXVK_LIB:-}" ] || export D3DPT_DXVK_LIB="$(win "$RENAMED")"
else
  echo "win-run.sh: no Direct3D executor built (scripts/build-windows.sh exec); XP's 3D is off" >&2
fi

cd "$ROOT"
if [ -n "${GDB:-}" ]; then
  exec gdb --args "$BIN" "$@"
fi
exec "$BIN" "$@"
