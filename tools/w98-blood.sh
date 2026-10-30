#!/usr/bin/env bash
# Blood (the 1997 DOS Build-engine game) in a Windows 98 DOS
# box, headless, and its frame rate (docs/tracks/m9-tcg-aarch64.md "Blood",
# docs/22 §6): a raw copy of the image through tools/win98-game-test.sh,
# `BLOOD.EXE -quick -map e1m1` from RUN.BAT (-quick skips the intro, -map
# loads the level: the crypt is up with no menu to drive), a screendump every
# 5 s, and QEMU's `vga_vbe_write` trace on. Blood flips pages by writing the
# VBE display-start index (9) once per frame, so the trace's index-9 writes
# per second *are* its frame rate (tools/tcg-fps.py cannot count a still
# camera: Build draws the same frame again and the probe sees one).
#
#   tools/w98-blood.sh <name>
#     env: IMG= (base98-us), CPU=, QEMU_TCG_OPTS=, EXTRA=, KEYS="40:left,..."
#     (turn the view: the rate depends on what is in front of the camera),
#     RUN_SECS= (100), FRESH= (1: a fresh raw copy every run, because a DOS
#     box does not answer the power button, so the copy is left dirty)
# Output: build/w98game/<name>/: fps.txt (frames per second of the run, one
# row a second, with the shot nearest each), shots/t<secs>.png, qemu.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1
O=$ROOT/build/w98game/$NAME
cd "$ROOT"
FRESH=${FRESH:-1} GUEST_CMD=$'cd \\BLOOD\nBLOOD.EXE -quick -map e1m1' \
  EXTRA="-trace vga_vbe_write${EXTRA:+ $EXTRA}" SHOTS=${SHOTS:-5} RUN_SECS=${RUN_SECS:-100} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/Library/Application Support/2ksbox/machines/base98-us/disk.qcow2}" "$NAME" \
  > "$ROOT/build/w98game/$NAME.run.log" 2>&1
python3 tools/w98-blood-fps.py "$O" | tee "$O/fps.txt"
