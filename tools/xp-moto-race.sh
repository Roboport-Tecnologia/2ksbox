#!/usr/bin/env bash
# Moto Racer 1997 into a practice race, headless, and its
# frame rate from outside (M9 track: the before/after number for the
# software renderer under TCG that the vCPU's % split cannot give).
#
#   tools/xp-moto-race.sh <image.qcow2> <name> [qemu-system-i386]
#
# Boots through tools/tcg-profile.sh (KEEP=1, the player's disc layout:
# CDS=MOTO_RACER.mds:FIFA2000.ISO, the M7 adapter), lets the attract-mode demo
# start, quits it (Esc, "Quit demo"), clicks Start on the title, accepts the
# name, Play Solo -> Practice -> Time Attack off -> Continue (Speed Bay, 3 laps)
# -> Start, waits
# for the load and the countdown, then holds the throttle while
# tools/tcg-fps.py counts distinct VGA frames for FPS seconds (15).
# Screendumps of every step and `fps.txt` land in build/tcg-profile/<name>/.
# Env: MOTO (the .mds), FIFA (the .iso), FPS, FPS_RATE (dumps per second, 25),
# RACE_SAMPLE=<s> (macOS: sample the process for that long *in the race* before the
# fps probe, report in <out>/race/; the runner's own sample is of the demo;
# DFILTER= on the runner still applies, tcg-hot.py <out>/race --dlog <out>/qemu-d.log),
# RACE_MEMSAVE=<addr:size,...> (guest-virtual ranges saved twice, 1 s apart, to
# <out>/race/mem-<addr>-{a,b}.bin: which bytes the game patches),
# RACE_BRAKE=1 (throttle and brake alternated and *measured apart*: braking
# emits tyre smoke, which is where the game nearly stops, and an average over a
# lap hides it: per phase the guest's frames and QEMU's TB invalidations and
# translated bytes, to phases.txt; RACE_ACCEL=<s> throttle per cycle (6),
# RACE_BRAKE_HOLD=<s> brake per cycle (4), RACE_CYCLES=<n> (3), brake1.png),
# RACE_WATCH=<s> (one row per second of that many seconds: TB invalidations,
# bytes translated, a screendump kept for the worst of them: which *frame* costs,
# which an average over a lap hides; RACE_CYCLE=4:3 cycles throttle and brake
# under it, RACE_SAMPLE=<s> then samples one brake phase and RACE_TRACE=1 traces
# the translations of one phase of each kind, and with RACE_STAGE=demo the attract demo is watched instead of a
# race, and the menus are skipped),
# RACE_DELAY=<s>
# (seconds into the race before the fps probe: the standing start and mid-race
# differ), PERFMAP=0 (the runner's knob: fps without the perf map's cost), plus the
# runner's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; NAME="${2:?name}"; BIN="${3:-$ROOT/build/qemu/qemu-system-i386}"
MOTO="${MOTO:-$HOME/vms/Moto.Racer.1997.DSI.CD/MOTO_RACER.mds}"; FIFA="${FIFA:-$HOME/vms/FIFA2000.ISO}"
OUT="$ROOT/build/tcg-profile/$NAME"; mkdir -p "$OUT"
KEEP=1 QEMU_BIN="$BIN" CDS="$MOTO:$FIFA" VGA=d3dpt MEM=1024 BOOT_WAIT="${BOOT_WAIT:-300}" WARM=25 SECS=1 \
  "$ROOT/tools/tcg-profile.sh" "$IMG" "$NAME" 'cmd /k cd /d C:\Arquiv~1\MotoRacer & MOTO.EXE' > "$OUT/run.log" 2>&1
SOCK=$(grep -o '/tmp/tcgprof-[0-9]*.sock' "$OUT/run.log" | head -1)
QPID=$(grep -o 'pid [0-9]*' "$OUT/run.log" | head -1 | cut -d' ' -f2)
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
ev() { Q json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}" >/dev/null; }
click() {  # x y in the 640x480 frame, a 200 ms press
  ev "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(( $1 * 32767 / 640 ))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(( $2 * 32767 / 480 ))}}"
  sleep 0.5; ev '{"type":"btn","data":{"down":true,"button":"left"}}'; sleep 0.2; ev '{"type":"btn","data":{"down":false,"button":"left"}}'
}
key() { ev "{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; }
shot() { Q screendump "$OUT/$1.png" >/dev/null || true; }
HMP() { Q json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("return",""))'; }
trap 'kill ${BRAKE_PID:-} 2>/dev/null || true; Q json "{\"execute\":\"system_powerdown\"}" >/dev/null 2>&1 || true; sleep 8; kill $QPID 2>/dev/null || true' EXIT
sleep 10; shot demo                       # the attract demo is running by now (the runner waited 25 s)
if [ "${RACE_STAGE:-race}" = demo ]; then  # the attract demo rides the lap properly, brakes into the corners included
  python3 "$ROOT/tools/moto-watch.py" "$SOCK" "${RACE_WATCH:-40}" "$OUT/watch" | tee "$OUT/watch.txt"
  echo "== $NAME: the demo watched, $OUT/watch"; exit 0
fi
Q keys esc >/dev/null; sleep 2; Q keys down >/dev/null; sleep 1; Q keys ret >/dev/null; sleep 6; shot title
click 315 445; sleep 6; shot name         # Start
Q keys ret >/dev/null; sleep 6; shot menu # the name as it is
click 160 250; sleep 6; shot mode         # Play Solo
click 110 260; sleep 6; shot race         # Practice
click 450 430; sleep 3                    # Time Attack off: no time limit ending the race in the window
click 565 373; sleep 6; shot bike         # Continue (Speed Bay, 3 laps)
click 565 390; sleep 35; shot loaded      # Start; load + countdown
key up true
if [ -n "${RACE_WATCH:-}" ]; then       # second by second: which second costs, and its screendump
  WATCH_SAMPLE_PID="${RACE_SAMPLE:+$QPID}" WATCH_SAMPLE_SECS="${RACE_SAMPLE:-3}" \
  WATCH_TRACE_LOG="${RACE_TRACE:+$OUT/qemu.log}" WATCH_MEMSAVE="${RACE_MEMSAVE:-}" \
    python3 "$ROOT/tools/moto-watch.py" "$SOCK" "$RACE_WATCH" "$OUT/watch" ${RACE_CYCLE:+"$RACE_CYCLE"} | tee "$OUT/watch.txt"
fi
if [ -n "${RACE_BRAKE:-}" ]; then       # the tyre smoke: a brake needs speed first, and it stops the bike
  # one measurement per phase, so the smoke is not averaged with the rest of the
  # lap: the guest's frames (tcg-fps.py) and QEMU's TB work over the same window
  jit_snap() { HMP "info jit" | awk -F'[ /]+' '/gen code size/{g=$4} /TB invalidate count/{i=$4} END{print g, i}'; }
  phase() {  # <label> <seconds>
    local g0 i0 g1 i1 s0 s1 fps
    read -r g0 i0 <<< "$(jit_snap)"; s0=$(date +%s)
    fps=$(python3 "$ROOT/tools/tcg-fps.py" "$SOCK" "$2" "${FPS_RATE:-25}" | head -1)
    read -r g1 i1 <<< "$(jit_snap)"; s1=$(date +%s)
    awk -v l="$1" -v f="$fps" -v g0="$g0" -v g1="$g1" -v i0="$i0" -v i1="$i1" -v d="$((s1 - s0))" \
      'BEGIN { if (d < 1) d = 1
               printf "%-8s %s  invalidations/s %7.0f  translated %6.1f MiB/s\n", l, f, (i1-i0)/d, (g1-g0)/1048576/d }'
  }
  sleep "${RACE_ACCEL:-6}"                 # up to speed: braking from a standstill emits nothing
  : > "$OUT/phases.txt"
  for i in $(seq "${RACE_CYCLES:-3}"); do
    key down false; key up true; phase throttle "${RACE_ACCEL:-6}" | tee -a "$OUT/phases.txt"
    key up false; key down true
    [ "$i" = 1 ] && { sleep 0.7; shot brake1; }   # the smoke on screen, once
    phase brake "${RACE_BRAKE_HOLD:-4}" | tee -a "$OUT/phases.txt"
  done
  key down false; key up true
fi
if [ -n "${RACE_SAMPLE:-}" ] && [ -z "${RACE_WATCH:-}" ]; then   # where the vCPU's time goes in the race itself
                                        # (with RACE_WATCH the watcher takes it, timed to one phase)
  mkdir -p "$OUT/race"; HMP "info jit" > "$OUT/race/info-jit-before.txt" || true
  sample "$QPID" "$RACE_SAMPLE" 1 -mayDie -file "$OUT/race/sample.txt" >/dev/null
  cp "/tmp/perf-$QPID.map" "$OUT/race/perf.map"; HMP "info jit" > "$OUT/race/info-jit-after.txt" || true
  python3 "$ROOT/tools/tcg-profile.py" "$OUT/race" > "$OUT/race/report.txt"
fi
if [ -n "${RACE_MEMSAVE:-}" ] && [ -z "${RACE_WATCH:-}" ]; then
  mkdir -p "$OUT/race"; for pass in a b; do IFS=, read -ra RANGES <<< "$RACE_MEMSAVE"
    for r in "${RANGES[@]}"; do a="${r%%:*}"; n="${r##*:}"
      Q json "{\"execute\":\"memsave\",\"arguments\":{\"val\":$((a)),\"size\":$((n)),\"filename\":\"$OUT/race/mem-$a-$pass.bin\"}}" >/dev/null || true
    done; sleep 1; done
  HMP "info registers" > "$OUT/race/info-registers.txt" || true
fi
[ -n "${RACE_DELAY:-}" ] && sleep "$RACE_DELAY"
python3 "$ROOT/tools/tcg-fps.py" "$SOCK" "${FPS:-15}" "${FPS_RATE:-25}" | tee "$OUT/fps.txt"
shot racing; kill ${BRAKE_PID:-} 2>/dev/null || true; key up false; key down false
echo "== $NAME: $(cat "$OUT/fps.txt")  ($OUT)"
