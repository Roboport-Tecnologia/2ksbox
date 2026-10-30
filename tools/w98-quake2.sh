#!/usr/bin/env bash
# Quake II's software renderer timedemo on a Win98 machine,
# headless (docs/22 §6): a raw copy of the image through
# tools/win98-game-test.sh, the disc on ide.1 (a minimal install reads its
# pak files off the CD: quake2.exe scans the drives for INSTALL\DATA), and
# from RUN.BAT `quake2.exe +set vid_ref soft +set sw_mode 3 +set logfile 2
# +timedemo 1 +map demo1.dm2`: 640x480 software rendering, the console
# logged and flushed to BASEQ2\QCONSOLE.LOG, demo1 played as fast as the
# machine can. The number is the game's own "N frames, S seconds: F fps"
# line, pulled off the disk afterwards; the screendumps every 5 s show the
# demo running. Quake II is the CPU benchmark of its day, and its software
# renderer is x87 and integer code with no 3D device in the path.
#
#   tools/w98-quake2.sh <name>
#     env: IMG= (base98-us), Q2= (the .iso), CPU=, QEMU_TCG_OPTS=, EXTRA=,
#     DEMO= (demo1.dm2), RUN_SECS= (200), FRESH= (1)
# Output: build/w98game/<name>/: QCONSOLE.LOG, fps.txt, shots/, qemu.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1
O=$ROOT/build/w98game/$NAME
cd "$ROOT"
FRESH=${FRESH:-1} CDS="${Q2:-$HOME/isos/Q2.iso}" TABLET=0 RUN_SECS=${RUN_SECS:-200} SHOTS=${SHOTS:-5} \
  GUEST_CMD=$'cd \\QUAKE2\nQUAKE2.EXE +set vid_ref soft +set sw_mode 3 +set vid_fullscreen 1 +set logfile 2 +timedemo 1 +map '"${DEMO:-demo1.dm2}" \
  PULL='QUAKE2\BASEQ2\QCONSOLE.LOG' \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/Library/Application Support/2ksbox/machines/base98-us/disk.qcow2}" "$NAME" \
  > "$ROOT/build/w98game/$NAME.run.log" 2>&1
grep -a -i -E 'frames, .* seconds' "$O/QCONSOLE.LOG" | tee "$O/fps.txt" || echo "no timedemo line in $O/QCONSOLE.LOG"
