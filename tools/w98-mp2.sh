#!/usr/bin/env bash
# Max Payne 2 on the Win98 display driver, headless: the driver's
# benchmark (track M17). Boots a raw copy of the image through
# tools/win98-game-test.sh (the user's image is never written), clicks Play
# on the game's launcher, picks Resume Game on its menu, and lets the
# user's save stand in the hospital corridor: Max still, 800x600x32 with
# 4x MSAA and every setting at its maximum (the user's own settings), about
# 207 draws in 6 DrawPrimitives2 calls a frame. The level is steady from
# about 250 s after the desktop.
#
#   tools/w98-mp2.sh <name> [perf|dwarf]
#     env: IMG= (default: the base98-br machine, the game in
#     C:\ARQUIV~1\ROCKST~1\MAXPAY~1 with a save in Meus documentos),
#     RUN_SECS= (400), WAIT= (seconds after the desktop before the
#     measurement, 250), CAP=1 (keep the flip's vertical blank: the game
#     then stops at 60 frames/s, which hides a gain), EXTRA= (more QEMU
#     arguments), SHOTS= (0), and the executor's switches, which pass
#     through: D3DPT_DDI_FLUSH_DRAWS=n, D3DPT_DDI_FLUSH_AB=n (the flush
#     hint's A/B inside one run)
#     perf:  -perfmap, then a 30 s `perf record -g` of QEMU at WAIT, and
#            tools/guest-code-owner.py on it while the guest still runs:
#            the vCPU thread's samples by host library and by guest module
#     dwarf: an 8 s --call-graph dwarf profile at WAIT, for the executor's
#            inclusive costs (`perf report --children`); no -perfmap
# Output: build/w98game/<name>/: qemu.log, cpu.txt (every QEMU thread's CPU
# over 20 s at WAIT), rates.txt (tools/ddi-rate.py: the level's frame
# rate, the executor's share, the A/B), and perf.data, perf.map,
# owners.txt with perf. Never beside another guest: the script refuses to
# start while any QEMU runs (two TCG guests starve each other).
# Local only: needs the image and the game.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=${1:?usage: w98-mp2.sh <name> [perf|dwarf]}; MODE=${2:-}
O=$ROOT/build/w98game/$NAME
LOG=$ROOT/build/w98game/$NAME.run.log
cd "$ROOT"
[ -z "$(ps -C qemu-system-i386 -o pid=)" ] || { echo "a QEMU is running: two guests at once starve each other"; exit 1; }
mkdir -p "$ROOT/build/w98game"
WAIT=${WAIT:-250}
X="-device voodoo2,addr=0x05 ${EXTRA:-}"          # base98-br has the Voodoo 2, as the launcher runs it
[ "$MODE" = perf ] && X="$X -perfmap"
DDF=32768; [ "${CAP:-0}" = 1 ] && DDF=0
GUEST_CMD='C:
cd \ARQUIV~1\ROCKST~1\MAXPAY~1
MAXPAY~1.EXE
exit' EXTRA="$X" DDFLAGS=$DDF KEYS="60:ret,160:down,163:ret" RUN_SECS=${RUN_SECS:-400} SHOTS=${SHOTS:-0} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/.local/share/2ksbox/machines/base98-br/disk.qcow2}" "$NAME" >"$LOG" 2>&1 &
H=$!
until grep -q "t+0 =" "$LOG" 2>/dev/null; do
  kill -0 $H 2>/dev/null || { echo "the run ended before the desktop:"; tail "$LOG"; exit 1; }
  sleep 2
done
sleep "$WAIT"
QPID=$(ps -C qemu-system-i386 -o pid= | head -1 | tr -d ' ')   # the only one: checked at the start
snap() { for t in /proc/$QPID/task/*; do echo "$(basename "$t") $(tr ' ' _ < "$t/comm") $(awk '{print $14+$15}' "$t/stat")"; done; }
snap > "$O/cpu0.txt"; sleep 20; snap > "$O/cpu1.txt"
join <(sort "$O/cpu0.txt") <(sort "$O/cpu1.txt") | awk '{d=$5-$3; if (d>20) printf "%s %s %.1f%%\n", $1, $2, d/20}' | sort -k3 -rn > "$O/cpu.txt"
VT=$(awk 'NR==1{print $1}' "$O/cpu.txt")          # the busiest thread is the vCPU
case "$MODE" in
perf)
  # the PE files the code may come from, off the raw copy: the game's own
  # and the system's Direct3D, DirectDraw, sound and C runtimes
  RAW=${RAW:-$ROOT/build/w98game/guest.raw}
  OFF=$(python3 -c "import struct; e=open('$RAW','rb').read(512)[446:462]; print(struct.unpack_from('<I',e,8)[0]*512)")
  mkdir -p "$O/pe"
  G='::/Arquivos de programas/Rockstar Games/Max Payne 2'
  MTOOLS_SKIP_CHECK=1 mcopy -n -o -i "$RAW@@$OFF" "$G/*.dll" "$G/*.exe" "$G/e2driver/*.dll" "$O/pe/" 2>/dev/null
  for f in D3D8.DLL D3D9.DLL DDRAW.DLL DSOUND.DLL DINPUT8.DLL KERNEL32.DLL USER32.DLL GDI32.DLL WINMM.DLL MSVCRT.DLL d3dpt9hl.dll; do
    MTOOLS_SKIP_CHECK=1 mcopy -n -o -i "$RAW@@$OFF" "::/WINDOWS/SYSTEM/$f" "$O/pe/" 2>/dev/null
  done
  perf record -g -F 999 -p "$QPID" -o "$O/perf.data" -- sleep 30 >/dev/null 2>&1
  cp "/tmp/perf-$QPID.map" "$O/perf.map"
  python3 tools/guest-code-owner.py "$O" "$VT" "$O/qmp.sock" > "$O/owners.txt" 2>&1 ;;
dwarf)
  perf record --call-graph dwarf,16384 -F 499 -p "$QPID" -o "$O/perf.data" -- sleep 8 >/dev/null 2>&1 ;;
esac
wait $H
rm -f "/tmp/perf-$QPID.map"       # a line per translated instruction, never trimmed
python3 tools/ddi-rate.py "$O/qemu.log" > "$O/rates.txt"
cat "$O/cpu.txt" "$O/rates.txt"
[ -f "$O/owners.txt" ] && cat "$O/owners.txt"
exit 0
