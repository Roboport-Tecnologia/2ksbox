#!/usr/bin/env bash
# The CPU-benchmark matrix of docs/22-tcg-evaluation.md, headless: one XP boot per emulator configuration, the whole benchmark ISO
# run inside it by SPECRUN.EXE, timings and output CRCs pulled off a floppy.
# A GUI program on the list (Super PI) is keyed from here: SPECRUN announces
# each start on COM1 and the host sends that name's key sequence.
#
#   tools/specbench/run.sh <image.qcow2> [config ...]
#   tools/specbench/run.sh ~/vms/winxp.qcow2 stock off default
#   tools/specbench/run.sh ~/vms/winxp.qcow2 all        # every configuration below
#
# Configurations (a name → a QEMU binary and its -cpu / -accel line):
#   stock      pristine QEMU v9.2.4 (build/qemu-stock, scripts in the paper), the baseline
#   stock-pic  the same pristine tree built with our -fPIC/-Db_staticpic flags
#              (build/qemu-stock-pic): the diagnostic that separates the cost of
#              PIC objects (ours are linked into the embed library too) from
#              the cost of the patches' off paths; not part of `all`
#   off        our QEMU with every switch off (the un-switchable patches alone)
#   default    our QEMU as the launcher writes it
#   no-<sw>    default with one switch off: x87-fast sse-fast simd-fast rep-fast
#              smc-same-value soft-imm inline-lookup tb-invalidate-fast tlb-floor
#              tls-hot-paths jump-cache-keep eob-chain tlb-retire
#   pinned     default + pinned-regs=on (patch 21, opt-in)
#   pc64as53   default + x87-pc64-as-53=on (patch 47, opt-in, inexact)
#
# Env: QEMU_EXTRA='...' (appended to the QEMU line: a -plugin, a -d),
# OUT=dir (build/specbench/runs), ISO=path (build/specbench/sb.iso, from
# build-guest.sh), BOOT_WAIT=s (cap on the knock, 300), RUN_WAIT=s (cap on
# the whole list, 5400), MEM=MB (512). The image boots snapshot=on and is
# never written. Never run two of these at once (CLAUDE.md: two TCG guests
# starve each other). Results: $OUT/<config>/results.txt (RESULT lines),
# qemu.log, serial.log, jit-after.txt (info jit, our binary only).
#
# Resumable, one configuration at a time: a configuration whose results.txt
# already ends in `finished` is skipped (FORCE=1 redoes it), so `all` can be
# re-run after an interruption and only the missing ones run; `--status`
# prints done/pending per configuration; PAUSE=1 asks on the terminal before
# each one (Enter runs it, s skips, q quits), for interleaving the ~12-minute
# runs with other work on the machine. A benchmark taken beside another
# load is not a benchmark.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; shift
OUT="${OUT:-$ROOT/build/specbench/runs}"; mkdir -p "$OUT"
ISO="${ISO:-$ROOT/build/specbench/sb.iso}"
[ -f "$ISO" ] || { echo "no benchmark ISO at $ISO: run tools/specbench/build-guest.sh"; exit 1; }
OURS="$ROOT/build/qemu/qemu-system-i386"; OURS_BIOS="$ROOT/qemu/pc-bios"
# Every launch through the no-ASLR launcher (WRAP= to override, WRAP="" for none): a launch
# whose executable lands at an odd multiple of 16 KiB runs helper-heavy code ~35 % slower
# on this Mac (docs/22 §5: 8 of 8 sampled launches, and dyld ignores segment
# alignment when it picks the slide), and a benchmark must not be a coin toss
NOASLR="$ROOT/build/specbench/noaslr"; [ -x "$NOASLR" ] || NOASLR=""
STOCK="${STOCK_BIN_OVERRIDE:-$ROOT/build/qemu-stock/qemu-system-i386}"; STOCK_BIOS="$ROOT/build/qemu-stock-src/pc-bios"
STOCK_PIC="$ROOT/build/qemu-stock-pic/qemu-system-i386"
CPU_SW="x87-fast sse-fast simd-fast rep-fast"
ACCEL_SW="smc-same-value soft-imm inline-lookup tb-invalidate-fast tlb-floor tls-hot-paths jump-cache-keep eob-chain tlb-retire"
ALL="stock off default"; for s in $CPU_SW $ACCEL_SW; do ALL="$ALL no-$s"; done; ALL="$ALL pinned pc64as53"
CONFIGS=("$@"); [ ${#CONFIGS[@]} -gt 0 ] || CONFIGS=(default)
[ "${CONFIGS[0]}" = all ] && read -r -a CONFIGS <<< "$ALL"
if [ "${CONFIGS[0]}" = --status ]; then
  for c in $ALL; do
    if grep -q '^finished' "$OUT/$c/results.txt" 2>/dev/null; then echo "done     $c"
    elif [ -d "$OUT/$c" ]; then echo "partial  $c"; else echo "pending  $c"; fi
  done; exit 0
fi

config_args() {  # sets BIN BIOS CPU ACCEL for a configuration name
  local c=$1 s; BIN=$OURS; BIOS=$OURS_BIOS; CPU="pentium3"; ACCEL="tcg"
  case "$c" in
    stock) BIN=$STOCK; BIOS=$STOCK_BIOS ;;
    stock-pic) BIN=$STOCK_PIC; BIOS=$STOCK_BIOS ;;
    default) ;;
    off) for s in $CPU_SW; do CPU="$CPU,$s=off"; done; for s in $ACCEL_SW; do ACCEL="$ACCEL,$s=off"; done ;;
    pinned) ACCEL="tcg,pinned-regs=on" ;;
    pc64as53) CPU="pentium3,x87-pc64-as-53=on" ;;
    no-*) s=${c#no-}
      case " $CPU_SW " in *" $s "*) CPU="pentium3,$s=off" ;; *)
      case " $ACCEL_SW " in *" $s "*) ACCEL="tcg,$s=off" ;; *) echo "unknown switch $s"; return 1 ;; esac ;; esac ;;
    *) echo "unknown configuration $c"; return 1 ;;
  esac
  [ -x "$BIN" ] || { echo "no emulator at $BIN"; return 1; }
}

n=0
for cfg in "${CONFIGS[@]}"; do
  config_args "$cfg" || exit 1; [ -n "${ACCEL_EXTRA:-}" ] && ACCEL="$ACCEL,$ACCEL_EXTRA"
  D="$OUT/$cfg"
  if [ -z "${FORCE:-}" ] && grep -q '^finished' "$D/results.txt" 2>/dev/null; then
    echo "== $cfg: done already (FORCE=1 to redo)"; continue
  fi
  if [ -n "${PAUSE:-}" ]; then
    read -r -p "run $cfg now? [Enter = run, s = skip, q = quit] " a </dev/tty || a=q
    case "$a" in s|S) echo "   skipped"; continue ;; q|Q) echo "   stopped before $cfg"; break ;; esac
  fi
  rm -rf "$D"; mkdir -p "$D"
  FDD="$D/fdd.img"; mformat -C -f 1440 -i "$FDD" ::
  n=$((n+1)); SOCK="/tmp/spb-$$-$n.sock"; rm -f "$SOCK"
  echo "== $cfg: $BIN -cpu $CPU -accel $ACCEL"
  ${WRAP-$NOASLR} "$BIN" -L "$BIOS" -machine pc -accel "$ACCEL" -cpu "$CPU" -m "${MEM:-512}" \
    -drive "file=$IMG,if=ide,index=0,snapshot=on" -drive "file=$FDD,if=floppy,format=raw" \
    -cdrom "$ISO" -vga cirrus -net none -usb -device usb-tablet \
    -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$D/serial.log" -monitor none \
    ${QEMU_EXTRA:-} > "$D/qemu.log" 2>&1 &
  QPID=$!; GW_PID=$QPID
  Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
  t_boot=$(date +%s)
  gw_wait_sock "$SOCK" 120 || { echo "   QEMU never came up: $(tail -3 "$D/qemu.log")"; exit 1; }
  # GO.BAT writes A:\READY.TXT first thing, which is the proof the shell took the knock
  gw_poke_until "$SOCK" xp 'cmd /c D:\SB\GO.BAT' "${BOOT_WAIT:-300}" \
    mcopy -n -o -i "$FDD" ::/READY.TXT "$D/ready.txt" \
    || { echo "   the guest never reached its shell (see $D/qemu.log)"; kill $QPID; exit 1; }
  echo "   shell after $(( $(date +%s) - t_boot )) s; running the list"
  t0=$(date +%s); keyed=0; tick=0
  while :; do
    sleep 2; tick=$((tick+1))
    # a GUI program the runner just started: send its keys once the window is up
    starts=$(grep -c '^START superpi' "$D/serial.log" 2>/dev/null || echo 0)
    if [ "$starts" -gt "$keyed" ]; then
      keyed=$starts; sleep 6
      Q keys alt+c; for i in 1 2 3 4 5 6; do Q keys down; done; Q keys ret; sleep 1; Q keys ret
      echo "   superpi rep $keyed keyed at $(( $(date +%s) - t0 )) s"
    fi
    [ $((tick % 5)) -eq 0 ] && { mcopy -n -o -i "$FDD" ::/RESULTS.TXT "$D/results.txt" 2>/dev/null || true; }
    grep -q '^finished' "$D/results.txt" 2>/dev/null && break
    gw_dead && { echo "   QEMU exited mid-run"; break; }
    [ $(( $(date +%s) - t0 )) -ge "${RUN_WAIT:-5400}" ] && { echo "   gave up after ${RUN_WAIT:-5400} s"; break; }
  done
  echo "   list done in $(( $(date +%s) - t0 )) s"
  Q json '{"execute":"human-monitor-command","arguments":{"command-line":"info jit"}}' > "$D/jit-after.txt" 2>/dev/null || true
  Q json '{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}' > "$D/registers-after.txt" 2>/dev/null || true
  Q screendump "$D/end.png" >/dev/null 2>&1 || true
  Q json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
  gw_wait_exit "$QPID" 120 || true
  kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; rm -f "$SOCK"
  mcopy -n -o -i "$FDD" ::/RESULTS.TXT "$D/results.txt" 2>/dev/null || true
  for f in NBENCH SUPERPI 7ZIP SSEBENCH; do
    mcopy -n -o -i "$FDD" "::/$f.TXT" "$D/$(echo $f | tr A-Z a-z).txt" 2>/dev/null || true
  done
  grep '^RESULT' "$D/results.txt" | sed 's/^/   /' || echo "   no RESULT lines"
done
python3 "$ROOT/tools/specbench/report.py" "$OUT" || true
