#!/usr/bin/env bash
# Moto Racer 1997 into a practice race on a Win98 machine,
# headless, and its frame rate (docs/22 §6; the 9x counterpart of
# tools/xp-moto-race.sh): a raw copy of the image through
# tools/win98-game-test.sh, MOTO.EXE from RUN.BAT with the disc on ide.1,
# the menus walked state by state (every 6 s the screen is classified by
# tools/motoracer-state.py and the one action for that screen taken (Start
# on the title, the name accepted, Play Solo, Practice, Time Attack off,
# Continue (Speed Bay, 3 laps), Start in the showroom; Esc leaves the intro
# and anything unknown), because a fresh install and base98-us show the
# screens in different orders), the throttle held, and the rate read from the adapter: the
# `N page flips in 5.0 s` and `ddi: N frames/s` lines whose windows lie
# inside the throttle window (both are Moto Racer's own frames, presented
# through the driver's flip chain; the game has no frame cap of its own).
# The clicks walk the PS/2 pointer (no tablet on this machine); inside the
# game the pointer sprite is the game's, so the walk is blind from the
# top-left corner, which is why every step is checked.
#
#   tools/w98-moto.sh <name>
#     env: IMG= (base98-us), MOTO= (the .mds), CPU=, QEMU_TCG_OPTS=, EXTRA=,
#     SOFT=1 (Options: D3D off, the game's software renderer instead of
#     our Direct3D HAL, which is the workload docs/22 §6 has for it; the
#     rate then comes from tools/tcg-fps.py's distinct VGA frames, since the
#     software renderer flips nothing), RACE_DELAY= (seconds into the race before the window, 5), FPS= (window
#     seconds, 20), FRESH= (1), MOTO_DIR= (the game's folder, 8.3:
#     \PROGRA~1\MOTORA~1; a Portuguese Windows says \ARQUIV~1\MOTORA~1)
# Output: build/w98game/<name>/: fps.txt, step screendumps (demo, title,
# name, menu, mode, race, bike, loaded, racing), shots/, qemu.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1
O=$ROOT/build/w98game/$NAME
LOG=$ROOT/build/w98game/$NAME.run.log
mkdir -p "$ROOT/build/w98game"
cd "$ROOT"
MOTO=${MOTO:-$HOME/isos/Moto.Racer.1997.DSI.CD/MOTO_RACER.mds}
DIR=${MOTO_DIR:-'\PROGRA~1\MOTORA~1'}
FRESH=${FRESH:-1} GUEST_CMD="cd $DIR"$'\nMOTO.EXE' CDS="$MOTO" TABLET=0 RUN_SECS=${RUN_SECS:-500} SHOTS=${SHOTS:-5} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/Library/Application Support/2ksbox/machines/base98-us/disk.qcow2}" "$NAME" >"$LOG" 2>&1 &
H=$!
q() { python3 tools/qmpc.py "$O/qmp.sock" "$@"; }
shot() { q screendump "$O/$1.png" >/dev/null 2>&1; }
state() { python3 tools/motoracer-state.py "$O/$1.png.ppm" 2>/dev/null; }
# the D3D row of the Options screen: how far right its value's text reaches
# (bright text on the green checker). The cycle is FILTERED (x 370), OFF
# (331), ON (326): OFF is the middle width of the three
d3d_xmax() { python3 - "$O/$1.png.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w = int(d[1].split()[0]); px = d[3]
xs = [x for y in range(182, 199) for x in range(296, 440) if sum(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) > 400]
print(max(xs) if xs else 0)
PY
}
# SOFT=1: from the main menu, Options, the D3D row clicked once (every
# run starts from the image's saved FILTERED, and one click is OFF; a
# third click on the row does not always register, so the cycle is not
# walked, and the width check says whether OFF was reached), then Accept:
# the software renderer, which is the workload docs/22 §6 measured on XP
soft_off() {
  q relclick 315 440 >/dev/null; sleep 6; shot opt0
  q relclick 330 190 >/dev/null; sleep 3; shot opt1
  local c; c=$(d3d_xmax opt1)
  echo "options: D3D row text extent $(d3d_xmax opt0) -> $c $( [ "$c" = 331 ] && echo '(OFF)' || echo '(NOT OFF: 331 is OFF, 326 ON, 370 FILTERED)')"
  q relclick 565 435 >/dev/null; sleep 6      # Accept
}
softdone=0
ev() { q json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}" >/dev/null; }
key() { ev "{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; }
until grep -q "s of run" "$LOG"; do
  kill -0 $H 2>/dev/null || { echo "harness ended before the run"; tail "$LOG"; exit 1; }
  sleep 1
done
sleep 30; shot demo
# state-driven: the screens come in a different order on different installs
# (a fresh install asks for a name after Start; base98-us goes from the intro
# straight to the main menu), so every 6 s the screen is classified and the
# one action for that screen taken, until the showroom's Start has been
# clicked. "other" is the intro, the attract demo or a screen we do not
# know: Esc leaves all of them
n=0
while [ $n -lt 25 ]; do
  n=$((n + 1)); shot "s$(printf '%02d' $n)"; st=$(state "s$(printf '%02d' $n)")
  echo "screen $n: $st"
  case $st in
    title)       q relclick 315 445 >/dev/null ;;                       # Start
    name)        q keys ret >/dev/null ;;                               # the name as it is
    menu)        if [ "${SOFT:-0}" = 1 ] && [ $softdone = 0 ]; then softdone=1; soft_off
                 else q relclick 160 250 >/dev/null; fi ;;                # Play Solo
    mode)        q relclick 110 260 >/dev/null ;;                       # Practice
    race-select) q relclick 450 430 >/dev/null; sleep 3; q relclick 565 373 >/dev/null ;;  # Time Attack off, Continue
    showroom)    q relclick 565 390 >/dev/null; break ;;                # Start (Speed Bay, 3 laps)
    *)           q keys esc >/dev/null ;;
  esac
  sleep 6
done
[ "$st" = showroom ] || echo "never reached the showroom (last screen: $st)"
sleep 35; shot loaded                      # load + countdown
key up true
sleep "${RACE_DELAY:-5}"
T1=$(date -u +%Y-%m-%dT%H:%M:%S)
# the software renderer blits and flips nothing the adapter counts, so the
# distinct-frame probe runs over the same window (60 dumps/s: it saturates
# near that)
python3 tools/tcg-fps.py "$O/qmp.sock" "${FPS:-20}" 60 > "$O/tcgfps.txt" 2>&1
shot racing
T2=$(date -u +%Y-%m-%dT%H:%M:%S)
key up false
python3 - "$O/qemu.log" "$T1" "$T2" <<'PY' | tee "$O/fps.txt"
import datetime, re, sys
log, a, b = sys.argv[1:]
a = datetime.datetime.fromisoformat(a + "+00:00").timestamp(); b = datetime.datetime.fromisoformat(b + "+00:00").timestamp()
flips, ddi = [], []
for l in open(log, errors="replace"):
    m = re.search(r"(\d+) page flips in ([0-9.]+) s \(([0-9.]+)/s\)", l)
    n = re.search(r"ddi: ([0-9.]+) frames/s \(.* in ([0-9.]+) s\)", l)
    if not (m or n): continue
    end = datetime.datetime.fromisoformat(l.split()[0].replace("Z", "+00:00")).timestamp()
    if m and end - float(m.group(2)) >= a - 0.5 and end <= b + 0.5: flips.append(float(m.group(3)))
    if n and end - float(n.group(2)) >= a - 0.5 and end <= b + 0.5: ddi.append(float(n.group(1)))
f = lambda v: " ".join("%.1f" % x for x in v) or "-"
mean = lambda v: "%.1f" % (sum(v) / len(v)) if v else "-"
print("throttle window %.0f s: flips %s -> %s fps; ddi %s -> %s fps" % (b - a, f(flips), mean(flips), f(ddi), mean(ddi)))
PY
head -1 "$O/tcgfps.txt" | sed 's/^/distinct VGA frames: /' | tee -a "$O/fps.txt"
q keys esc >/dev/null; sleep 2
q json '{"execute":"system_powerdown"}' >/dev/null
wait $H
echo "done: $O"
