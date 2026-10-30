#!/usr/bin/env bash
# 3DMark 99 Max on a Win98 machine, headless, end to end
# (docs/tracks/m9-tcg-aarch64.md, "Win98 3D"): boot a raw copy of the image
# through tools/win98-game-test.sh (the user's image is never written), wait
# for 3DMark's welcome dialog by its pixels (a fixed sleep missed it once the
# boot got faster), click New Benchmark and Benchmark over a USB tablet,
# screendump the score dialog every 15 s from 150 s after the click, power off.
#
#   tools/w98-3dmark.sh <name> [perf|whole]
#     env: IMG= (default: the claude98 machine, 3DMark installed in
#     C:\ARQUIV~1\3DMARK~1), SHOTS=, DDFLAGS= (32768: vertical blank off),
#     EXTRA= (-perfmap, -d out_asm ...), QEMU_BIN= (a wrapper, e.g. the
#     memtrace preload), all passed through to win98-game-test.sh
#     TABLET=0: no USB tablet; the clicks walk the PS/2 pointer instead
#            (qmpc.py relclick), for a machine that has never bound a
#            tablet and would stop in the New Hardware wizard (base98-us)
#     perf:  a --call-graph dwarf profile for PERF_SECS from PERF_AT s after
#            the Benchmark click (the first-person test is ~40-56 s)
#     whole: a -F 199 profile of the whole run (PERF_SECS, default 200);
#            the run's perf-<pid>.map is copied beside perf.data so that
#            tools/tcg-perf-cut.py can name the generated code later
#     JIT_SNAPS="43 53": `info jit` at those seconds after the click into
#            jit-<s>.txt (two inside one test and the difference is that
#            test's TLB flushes, refills, jump-cache hits and the rest)
#     PAGES="0158e000 ...": memsave those guest pages at the first snapshot
#            + 5 s into pages/<page>.bin, for tools/tcg-form-weights.py
#     QEMU_TCG_OPTS=tlb-retire=off: an accelerator switch for the A/B,
#            through win98-game-test.sh
# Output: build/w98game/<name>/: score.png (the score dialog; read the
# two scores off it), **tests.txt** (every `ddi:` and page-flip rate line
# placed by test, from a screendump every 5 s in tests/; that is where a
# game test's frame rate is read, never off a bare rate line, whose window
# can be a loading screen or the CPU 3D Speed test, see the track doc),
# qemu.log, click.txt (the click's epoch, to
# place profile windows: take a test's seconds from tests.txt), perf.data.
# Settings are the image's own (the user's: 800x600x16, triple buffer,
# Pentium III optimizations). Local only: needs the image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1; MODE=${2:-}
O=$ROOT/build/w98game/$NAME
LOG=$ROOT/build/w98game/$NAME.run.log
mkdir -p "$ROOT/build/w98game"   # the log's directory, on a checkout's first run
cd "$ROOT"
# TDM_DIR=: 3DMark's 8.3 folder. claude98 is a Portuguese Windows
# ("Arquivos de programas"); an English one is \PROGRA~1\3DMARK~1.
TDM_DIR=${TDM_DIR:-'\ARQUIV~1\3DMARK~1'}
TABLET=${TABLET:-1}
GUEST_CMD="cd $TDM_DIR"$'\n3DMARK.EXE' TABLET=$TABLET RUN_SECS=900 SHOTS=${SHOTS:-0} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/.local/share/2ksbox/machines/claude98/disk.qcow2}" "$NAME" >"$LOG" 2>&1 &
H=$!
q() { python3 tools/qmpc.py "$O/qmp.sock" "$@"; }
c() { if [ "$TABLET" = 1 ]; then q click "$1" "$2" 800 600; else q relclick "$1" "$2"; fi; }
until grep -q "s of run" "$LOG"; do
  kill -0 $H 2>/dev/null || { echo "harness ended before the run"; tail "$LOG"; exit 1; }
  sleep 1
done
# 3DMark's welcome dialog: the "New 3DMark Benchmark" button's face is the
# dialog grey at (300,199) and the desktop behind it is not; wait for it
up=0
for i in $(seq 60); do
  sleep 3
  q screendump "$O/wait.png" >/dev/null 2>&1 || continue
  python3 - "$O/wait.png.ppm" <<'PY' && { up=1; break; }
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w = int(d[1].split()[0]); px = d[3]
def at(x, y): o = (y * w + x) * 3; return tuple(px[o:o + 3])
# the button face (Windows grey, 192 or its RGB565 round trip) and the
# dialog title bar's dark blue, right of the title text
g = at(300, 199); t = at(450, 91)
grey = all(185 <= c <= 200 for c in g) and max(g) - min(g) <= 4
# (the title's blue reads (8, 85, 181) on claude98's desktop and Windows'
# standard navy (0, 0, 128) on an English 98's: dark, blue, no red)
sys.exit(0 if grey and t[2] >= 100 and t[0] < 40 and t[1] < 100 else 1)
PY
done
[ $up = 1 ] || { echo "3DMark's welcome dialog never came up"; q json '{"execute":"system_powerdown"}'; wait $H; exit 1; }
sleep 5
c 368 199; sleep 8
q screendump "$O/project.png" >/dev/null
c 448 457
T0=$(date +%s)
python3 -c 'import time; print("%.9f" % time.time())' > "$O/click.txt"   # BSD date has no %N
if [ -n "${JIT_SNAPS:-}" ]; then
  ( for at in $JIT_SNAPS; do
      while [ $(( $(date +%s) - T0 )) -lt "$at" ]; do sleep 1; done
      q json '{"execute":"human-monitor-command","arguments":{"command-line":"info jit"}}' > "$O/jit-$at.txt"
      if [ -n "${PAGES:-}" ]; then
        sleep 5; mkdir -p "$O/pages"
        for pg in $PAGES; do   # a relative name: HMP's memsave takes no path with a slash
          q json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"memsave 0x$pg 4096 pages_$pg.bin\"}}" >/dev/null 2>&1
          [ -e "pages_$pg.bin" ] && mv "pages_$pg.bin" "$O/pages/$pg.bin"
        done
        PAGES=
      fi
    done ) &
fi
if [ "$MODE" = perf ]; then
  sleep "${PERF_AT:-8}"
  P=$(ps -C qemu-system-i386 -o pid= | head -1)
  perf record -F 499 --call-graph dwarf,16384 -p $P -o "$O/perf.data" -- sleep "${PERF_SECS:-10}" 2>&1 | tail -1
elif [ "$MODE" = whole ]; then
  # the whole benchmark, frame-pointer stacks, wall-clock stamped so that
  # `perf report --time` can cut it at the tests' boundaries (qemu.log's
  # timestamped ddi: lines); in the background so the score wait still runs
  P=$(ps -C qemu-system-i386 -o pid= | head -1)
  perf record -F 199 -p $P -o "$O/perf.data" -- sleep "${PERF_SECS:-200}" \
    >"$O/perf.log" 2>&1 &
fi
# a screendump every 5 s after the click into tests/t<secs>.png: the game
# tests are told from 3DMark's "Now testing" splash by pixels and every
# rate line is placed by test (tools/w98-3dmark-tests.py -> tests.txt),
# because a `ddi:` line's window can span a test's loading screen or the
# CPU 3D Speed test and read as a frame rate that is not one. The score
# dialog is the last shot (score.png); the loop ends 10 s after it shows,
# or at 420 s (the benchmark takes ~170 s here with vsync on, the Air is
# slower).
mkdir -p "$O/tests"
seen=0
while [ $(( $(date +%s) - T0 )) -lt 420 ]; do
  sleep 5
  s=$(( $(date +%s) - T0 ))
  f="$O/tests/t$(printf '%03d' $s).png"
  q screendump "$f" >/dev/null 2>&1 || continue
  if [ "$(python3 tools/w98-3dmark-tests.py classify "$f.ppm" 2>/dev/null)" = score ]; then
    cp "$f" "$O/score.png"; seen=$((seen + 1)); [ $seen -ge 2 ] && break
  fi
done
q json '{"execute":"system_powerdown"}' >/dev/null
[ "$MODE" = whole ] && cp /tmp/perf-*.map "$O/" 2>/dev/null
wait $H
grep -E "ddi: [0-9.]+ frames" "$O/qemu.log" | sed 's/.*d3dpt-vga: //' > "$O/rates.txt"
python3 tools/w98-3dmark-tests.py report "$O" || echo "tests.txt: the game tests or the score were not seen on screen -- look at $O/tests/"
echo "done: $O ($(wc -l < "$O/rates.txt") rate lines)"
