#!/bin/bash
# The mitsuami player (track M22) on a private headless sway, so nothing
# opens on the desktop. Three checks, no guest:
#
#   sweep    doc 03's mode sweep, run by the mitsuami player: player-core's
#            session driven from mitsuami's loop (it exits 0 when every mode
#            passed)
#   picture  the test pattern on the GpuSurface, read back from the
#            compositor with grim: the middle of the window is a colour bar,
#            not black (the surface presented) nor GTK's background (it is
#            over the window)
#   key      a key from a virtual keyboard (wtype) reaches the surface as
#            the key it is (`PLAYER_INPUT_LOG`)
#
# The headless seat has no pointer, so the pointer is not checked here.
#
# usage: tools/player-mitsuami-test.sh [out-dir]
# needs: player-mitsuami/target/release/player-mitsuami (built from its own
# directory), sway, grim, wtype; a Vulkan device
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/test/player-mitsuami}"
BIN="$ROOT/player-mitsuami/target/release/player-mitsuami"
PRESET="$ROOT/third_party/slang-shaders/crt/crt-guest-advanced.slangp"
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "no $BIN (cd player-mitsuami && cargo build --release)"; exit 1; }
for tool in sway grim wtype python3; do
  command -v $tool >/dev/null || { echo "needs $tool"; exit 1; }
done
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
rc=0

# sweep: offscreen, so the window's size does not matter; it exits itself
if [ -f "$PRESET" ]; then
  if timeout 120 "$BIN" --shader "$PRESET" --mode-sweep "$OUT/sweep" > "$OUT/sweep.log" 2>&1 \
      && grep -q "modes OK" "$OUT/sweep.log"; then
    echo "  ok   sweep: $(grep 'modes OK' "$OUT/sweep.log")"
  else
    echo "  FAIL sweep ($OUT/sweep.log)"; tail -5 "$OUT/sweep.log"; rc=1
  fi
else
  echo "  skip sweep (no slang-shaders submodule)"
fi

# picture and key: the pattern, then a key, then a shot
PLAYER_INPUT_LOG=1 "$BIN" > "$OUT/pattern.log" 2>&1 &
P=$!
sleep 3
# Each wtype run is a new virtual keyboard, and a key sent while the
# client is still taking its keymap can be lost: a Shift first, then the
# key under test on the same keyboard, and again if it did not arrive.
for _ in 1 2 3; do
  wtype -P Shift_L -p Shift_L -s 500 -P Super_L -s 100 -p Super_L
  sleep 1
  grep -q "Key { code: MetaLeft.*pressed: false" "$OUT/pattern.log" && break
done
grim -t ppm "$OUT/pattern.ppm"
kill $P 2>/dev/null
for _ in $(seq 1 25); do kill -0 $P 2>/dev/null || break; sleep 0.2; done
kill -9 $P 2>/dev/null

if python3 - "$OUT/pattern.ppm" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
# P6 <w> <h> <max>\n then RGB
parts, i = [], 0
while len(parts) < 4:
    while data[i:i+1].isspace(): i += 1
    j = i
    while not data[j:j+1].isspace(): j += 1
    parts.append(data[i:j]); i = j
i += 1
w, h = int(parts[1]), int(parts[2])
def px(x, y):
    o = i + (y * w + x) * 3
    return tuple(data[o:o+3])
# the pattern's seven bars across the middle of the picture: at least five
# distinct, saturated colours on the window's middle row
row = h // 2 + 20
seen = {px(x, row) for x in range(w // 8, w * 7 // 8, 8)}
bright = {c for c in seen if max(c) > 150}
print(f"  picture: {len(bright)} bright colours across the middle")
sys.exit(0 if len(bright) >= 5 else 1)
EOF
then echo "  ok   picture"
else echo "  FAIL picture ($OUT/pattern.ppm)"; rc=1
fi

if grep -q "Key { code: MetaLeft.*pressed: true" "$OUT/pattern.log" \
    && grep -q "Key { code: MetaLeft.*pressed: false" "$OUT/pattern.log"; then
  echo "  ok   key: MetaLeft down and up"
else
  echo "  FAIL key ($OUT/pattern.log)"; rc=1
fi
exit $rc
