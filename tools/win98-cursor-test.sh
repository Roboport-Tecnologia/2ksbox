#!/usr/bin/env bash
# The guest's standard cursors become the host's (player-core's
# cursor_shape.rs, doc 15 "Player"): CURSORS.EXE sets each IDC_* in turn on
# Windows 98, in the player, and every one the host has a cursor for must
# be recognised by its pixels, in order. IDC_UPARROW has none and must stay
# the guest's.
#
#   tools/win98-cursor-test.sh ~/.local/share/2ksbox/machines/base98-br/disk.qcow2
#
# The player runs inside a private headless sway (nothing opens on this
# desktop), through tools/win98-game-test.sh PLAYER=1, on its raw copy
# (RAW=, kept; the image is never written). TCG, about 5 minutes.
#
# Env: RAW= (default build/w98cursor/guest.raw), OUT= (build/w98cursor).
# Needs: sway, mtools, player-mitsuami/target/release/player-mitsuami, build/cursors/cursors.exe
# (guest-tools/build-wrappers.sh builds it into the ISO tree; this builds
# its own copy when i686-w64-mingw32-gcc is there).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMG="${1:?usage: $0 <win98 image>}"
OUT="${OUT:-build/w98cursor}"
RAW="${RAW:-build/w98cursor/guest.raw}"
EXE=build/cursors/cursors.exe
command -v sway >/dev/null || { echo "needs sway"; exit 77; }
if command -v i686-w64-mingw32-gcc >/dev/null; then
  mkdir -p build/cursors
  i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
    -mwindows -o "$EXE" guest-tools/src/cursors.c || exit 1
fi
[ -f "$EXE" ] || { echo "no $EXE"; exit 77; }

# The harness, copied: an instance breaks when its script is edited.
mkdir -p "$OUT"
cp tools/win98-game-test.sh build/.win98-cursor-run.sh

RUN="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
printf 'output HEADLESS-1 resolution 1280x960@60Hz\ndefault_border none\n' > "$OUT/sway.conf"
before=$(ls "$RUN" | grep '^wayland-[0-9]*$' | sort)
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c "$OUT/sway.conf" > "$OUT/sway.log" 2>&1 &
SWAY=$!
trap 'kill $SWAY 2>/dev/null; wait $SWAY 2>/dev/null' EXIT
sock=""
for _ in $(seq 1 100); do
  sock=$(comm -13 <(echo "$before") <(ls "$RUN" | grep '^wayland-[0-9]*$' | sort) | head -1)
  [ -n "$sock" ] && break
  sleep 0.1
done
[ -n "$sock" ] || { echo "sway did not start ($OUT/sway.log)"; exit 1; }
export WAYLAND_DISPLAY=$sock GDK_BACKEND=wayland GTK_USE_PORTAL=0
unset DISPLAY

PLAYER=1 PLAYER_CURSOR_LOG=1 STAGE="$EXE" GUEST_CMD='C:\CURSORS.EXE' UNTIL='CURSORS DONE' RUN_SECS=240 SHOTS=0 \
  RAW="$RAW" OUT="$OUT/run" timeout 600 bash build/.win98-cursor-run.sh "$IMG" cursors > "$OUT/harness.log" 2>&1
grep -q "CURSORS DONE" "$OUT/run/com1.log" 2>/dev/null || { echo "CURSORS.EXE never finished ($OUT/harness.log)"; exit 1; }

# What the player made of each shape after the program's first, in order
# (the login's own arrow and busy cursors come before it).
got=$(grep -o '\[cursor\] guest shape #[0-9]* .*' "$OUT/run/qemu.log" | sed -n 's/.*(\(.*\))$/\1/p' \
  | awk 'f||/^Text$/{f=1} f' | head -13 | tr '\n' ' ')
want="Text Wait Crosshair not a standard one ResizeNorthWestSouthEast ResizeNorthEastSouthWest ResizeEastWest ResizeNorthSouth Move NotAllowed Pointer Progress Help "
echo "recognised: $got"
[ "$got" = "$want" ] || { echo "wanted:     $want"; echo "FAIL"; exit 1; }
echo "PASS: every standard cursor recognised, IDC_UPARROW left the guest's"
