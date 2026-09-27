#!/usr/bin/env bash
# 3DMark2001 SE's Benchmark on a Win98 machine, headless, end to end (docs/22 §6, the x87 PC=64 question of patch 47): boot a raw copy
# of the image through tools/win98-game-test.sh (the user's image is never
# written), start 3DMark from RUN.BAT, wait for its project window by pixels,
# click Benchmark over the PS/2 mouse, screendump every 5 s until the Overall
# Score dialog shows, click Show Details and screendump the per-test list
# (3DMark's own fps for every game test, low and high detail), then power off.
#
#   tools/w98-3dmark2001.sh <name>
#     env: IMG= (default: the base98-us machine, 3DMark2001 SE installed in
#     C:\PROGRA~1\MADONION.COM\3DMARK~1), CPU= (-cpu, e.g.
#     pentium3,x87-pc64-as-53=on), QEMU_TCG_OPTS=, DDFLAGS=, EXTRA=, TABLET=1
#     (a USB tablet and absolute clicks; off by default, see w98-3dmark.sh),
#     TDM_DIR= (3DMark's folder as 8.3; a pt-BR machine's is
#     C:\ARQUIV~1\MADONION.COM\3DMARK~1),
#     CAP= (seconds to wait for the score, 3000), passed through to
#     win98-game-test.sh where it takes them.
# Output: build/w98game/<name>/: score.png (the Overall Score dialog),
# details-NN.png (Show Details, a screendump per page: the list of tests
# with 3DMark's own fps; read the numbers off it), tests/t<secs>.png (a
# screendump every 5 s after the Benchmark click, so a rate line in
# qemu.log can be placed by test; the "Now Testing" splash names the test
# that is about to run), rates.txt (the executor's `ddi:` lines), qemu.log. Settings are the
# image's own (base98-us: 1024x768x32, compressed textures, pure hardware
# T&L). Local only: needs the image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1
O=$ROOT/build/w98game/$NAME
LOG=$ROOT/build/w98game/$NAME.run.log
mkdir -p "$ROOT/build/w98game"
cd "$ROOT"
TABLET=${TABLET:-0}
CAP=${CAP:-3000}
TDM_DIR=${TDM_DIR:-'C:\PROGRA~1\MADONION.COM\3DMARK~1'}
GUEST_CMD="cd \"$TDM_DIR\""$'\n3DMARK~3.EXE' TABLET=$TABLET RUN_SECS=$((CAP + 120)) SHOTS=${SHOTS:-0} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/Library/Application Support/2ksbox/machines/base98-us/disk.qcow2}" "$NAME" >"$LOG" 2>&1 &
H=$!
q() { python3 tools/qmpc.py "$O/qmp.sock" "$@"; }
c() { if [ "$TABLET" = 1 ]; then q click "$1" "$2" 800 600; else q relclick "$1" "$2"; fi; }
# pixel probes on the last screendump's ppm: window | score | other
probe() {
  python3 - "$1" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = (int(x) for x in d[1].split()[:2]); px = d[3]
def at(x, y): o = (y * w + x) * 3; return tuple(px[o:o + 3])
def blue(c): return c[2] >= 100 and c[0] < 40 and c[1] < 130   # the title bar is a gradient
def grey(c): return all(185 <= v <= 200 for v in c) and max(c) - min(c) <= 4
if (w, h) != (800, 600):
    print("other"); sys.exit()
# the Overall Score dialog: its title bar (y 131) blue across, the project
# window's own title bar (y 32) no longer the active blue
if blue(at(300, 131)) and blue(at(450, 131)) and grey(at(300, 300)):
    print("score")
# the project window: an active blue title bar at y 32 and the Benchmark
# button's grey face at (464, 424)
elif blue(at(300, 32)) and blue(at(500, 32)) and grey(at(464, 424)) and grey(at(400, 430)):
    print("window")
else:
    print("other")
PY
}
until grep -q "s of run" "$LOG"; do
  kill -0 $H 2>/dev/null || { echo "harness ended before the run"; tail "$LOG"; exit 1; }
  sleep 1
done
up=0
for i in $(seq 40); do
  sleep 3
  q screendump "$O/wait.png" >/dev/null 2>&1 || continue
  [ "$(probe "$O/wait.png.ppm")" = window ] && { up=1; break; }
done
[ $up = 1 ] || { echo "3DMark2001's project window never came up"; q json '{"execute":"system_powerdown"}'; wait $H; exit 1; }
sleep 3
c 464 424                       # Benchmark
T0=$(date +%s)
python3 -c 'import time; print("%.9f" % time.time())' > "$O/click.txt"
mkdir -p "$O/tests"
seen=0
while [ $(( $(date +%s) - T0 )) -lt "$CAP" ]; do
  sleep 5
  s=$(( $(date +%s) - T0 ))
  f="$O/tests/t$(printf '%04d' $s).png"
  q screendump "$f" >/dev/null 2>&1 || continue
  if [ "$(probe "$f.ppm")" = score ]; then
    cp "$f" "$O/score.png"; seen=$((seen + 1)); [ $seen -ge 2 ] && break
  fi
done
if [ $seen -ge 2 ]; then
  c 501 285; sleep 4             # Show Details
  # the list is pages long (user, project, display, the results, the
  # system) and its keyboard focus is not the list's (PgDn does nothing).
  # A click on the scrollbar's track below the thumb pages it
  # down, a screendump per page, until the last two pages are the same.
  # A page click scrolls one row past the page, so the row at each page
  # boundary is not on any shot (Game 2's high detail, on a pt-BR run)
  q screendump "$O/details-00.png" >/dev/null
  prev=$(cksum < "$O/details-00.png.ppm")
  for i in $(seq -w 1 24); do
    c 624 410 >/dev/null; sleep 2; q screendump "$O/details-$i.png" >/dev/null
    h=$(cksum < "$O/details-$i.png.ppm"); [ "$h" = "$prev" ] && { rm -f "$O/details-$i.png" "$O/details-$i.png.ppm"; break; }
    prev=$h
  done
else
  echo "the Overall Score dialog never showed within ${CAP}s -- look at $O/tests/"
fi
q json '{"execute":"system_powerdown"}' >/dev/null
wait $H
grep -E "ddi: [0-9.]+ frames" "$O/qemu.log" | sed 's/.*d3dpt-vga: //' > "$O/rates.txt"
echo "done: $O ($(wc -l < "$O/rates.txt") rate lines, score $( [ $seen -ge 2 ] && echo seen || echo NOT seen ))"
