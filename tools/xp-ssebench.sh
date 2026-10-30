#!/usr/bin/env bash
# Run SSEBENCH.EXE (guest-tools ISO, doc 16) in an XP image
# headlessly, once per CPU configuration, and print the results.
#
#   tools/xp-ssebench.sh <image.qcow2> [cpu-config ...]
#   tools/xp-ssebench.sh ~/vms/winxp.qcow2 pentium3 pentium3,sse-fast=off pentium3,sse-fast=off,x87-fast=off
#
# Boots standalone QEMU (the XP image as a snapshot drive, not modified)
# with the newest guest-tools ISO (D:) and a fresh 1.44M floppy image (A:,
# made with mtools so it works on macOS too; NOT under -snapshot, or the
# guest's writes never reach the file), types the command over QMP,
# redirects the console into A:\SSEBENCH.TXT, polls the floppy image for
# the file, powers down and pulls it out with mcopy. Env: OUT=dir
# (default build/xp-ssebench), BOOT_WAIT=seconds (the cap on waiting for
# the guest's shell, which is knocked on rather than slept out: 300),
# BENCH_WAIT=max
# seconds to wait for the result (900), GUEST_ISO=path, ITER=n (SSEBENCH
# -iter n, default 1), ARGS='-only convert' (extra SSEBENCH arguments). Prints
# the SSE slow-path counters (info registers) before and after the run.
# Needs mtools + python3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; shift
CONFIGS=("$@"); [ ${#CONFIGS[@]} -gt 0 ] || CONFIGS=(pentium3 pentium3,sse-fast=off pentium3,sse-fast=off,x87-fast=off)
OUT="${OUT:-$ROOT/build/xp-ssebench}"; mkdir -p "$OUT"
ISO="${GUEST_ISO:-$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)}"
[ -f "${ISO:-}" ] || { echo "no guest-tools ISO: run guest-tools/build-wrappers.sh"; exit 1; }

for cfg in "${CONFIGS[@]}"; do
  tag="$(echo "$cfg" | tr ',=' '__')"
  FDD="$OUT/fdd-$tag.img"; rm -f "$FDD"
  mformat -C -f 1440 -i "$FDD" ::
  # a short path: AF_UNIX names cap at ~104 bytes and $OUT can sit deep in a worktree
  n=$(( ${n:-0} + 1 )); SOCK="/tmp/xpsb-$$-$n.sock"; rm -f "$SOCK"
  LOG="$OUT/qemu-$tag.log"
  "$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" -machine pc -cpu "$cfg" -m 512 \
    -drive "file=$IMG,if=ide,index=0,snapshot=on" -drive "file=$FDD,if=floppy,format=raw" \
    -cdrom "$ISO" -vga cirrus -net none -usb -device usb-tablet \
    -display none -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$LOG" 2>&1 &
  QPID=$!
  Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
  echo "== -cpu $cfg: booting"
  GW_PID=$QPID
  gw_wait_sock "$SOCK" || exit 1
  # No serial port on this machine, by decision. It is a benchmark, and the
  # measured machine stays the one the numbers were taken on. So the proof
  # that the shell is there is a marker on the same floppy the results come
  # back on: a removable drive, which XP writes through rather than holding
  # in its lazy writer (tools/guestwait.sh).
  gw_poke_until "$SOCK" xp 'cmd /c echo ready > A:\READY.TXT' "${BOOT_WAIT:-300}" \
    mcopy -n -o -i "$FDD" ::/READY.TXT "$OUT/.ready-$tag.txt" \
    || echo "   the guest never reached its shell (see $LOG)"
  Q keys esc; sleep 1; Q keys meta_l+r; sleep 4
  Q type ' '; Q keys backspace          # the first key after the chord is lost
  Q json '{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}' | grep -o 'SSE-fast[^\\]*' > "$OUT/stat-$tag-before.txt" || true
  # two passes per boot: macOS may park the vCPU thread on an efficiency core
  # for a whole run (uniform ~2x), so take the better of two
  Q type "cmd /k D:\\TESTS\\SSEBENCH.EXE -iter ${ITER:-1} ${ARGS:-} > A:\\SSEBENCH.TXT & D:\\TESTS\\SSEBENCH.EXE -iter ${ITER:-1} ${ARGS:-} > A:\\SSEBENC2.TXT"; Q keys ret
  t0=$(date +%s)
  while [ $(( $(date +%s) - t0 )) -lt "${BENCH_WAIT:-900}" ]; do
    sleep 15
    # the file exists from the start (the redirect); done when the last line is in
    mcopy -n -o -i "$FDD" ::/SSEBENC2.TXT "$OUT/.probe.txt" 2>/dev/null && grep -q '^done' "$OUT/.probe.txt" && { sleep 3; break; }
  done
  echo "   $(( $(date +%s) - t0 )) s until the output file appeared"
  Q screendump "$OUT/$tag.png" >/dev/null || true
  Q json '{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}' | grep -o 'SSE-fast[^\\]*' > "$OUT/stat-$tag-after.txt" || true
  echo "   slow paths before: $(cat "$OUT/stat-$tag-before.txt")"; echo "   slow paths after:  $(cat "$OUT/stat-$tag-after.txt")"
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 120 || true
  kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; rm -f "$SOCK"
  if mcopy -n -i "$FDD" ::/SSEBENCH.TXT "$OUT/ssebench-$tag.txt" 2>/dev/null; then
    mcopy -n -i "$FDD" ::/SSEBENC2.TXT "$OUT/ssebench-$tag-2.txt" 2>/dev/null || true
    echo "-- $OUT/ssebench-$tag.txt (pass 1; pass 2 in ssebench-$tag-2.txt; best of the two per kernel:)"
    python3 - "$OUT/ssebench-$tag.txt" "$OUT/ssebench-$tag-2.txt" <<'PY'
import sys, re
runs = [open(f).read().split('\n') for f in sys.argv[1:] if __import__('os').path.exists(f)]
print(runs[0][0])
print("%-32s %10s %9s %9s" % ("kernel", "ms", "ns/iter", "ns/op"))
for i, line in enumerate(runs[0][2:]):
    m = re.match(r'(.{32}) +([\d.]+) +([\d.]+) +([\d.]+) +(\S+)$', line)
    if not m:
        continue
    best = min((r[2 + i] for r in runs if len(r) > 2 + i), key=lambda l: float(l.split()[-4]))
    b = re.match(r'(.{32}) +([\d.]+) +([\d.]+) +([\d.]+) +(\S+)$', best)
    print("%s %10s %9s %9s" % (b.group(1), b.group(2), b.group(3), b.group(4)))
PY
  else
    echo "!! no SSEBENCH.TXT on the floppy for $cfg (see $OUT/$tag.png)"
  fi
done
