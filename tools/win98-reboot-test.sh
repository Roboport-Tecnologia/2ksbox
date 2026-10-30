#!/usr/bin/env bash
# A Win98 guest must survive a restart.
#
#   tools/win98-reboot-test.sh ~/vms/win98.qcow2 [qmp|guest|both]
#
# The bug this guards (patch 22, patches/qemu/README.md): Win98 turns
# its local APIC off through IA32_APIC_BASE, which in QEMU also clears
# CPUID.01H:EDX.APIC; RESET puts the enable bit back but not the feature
# bit, so the next POST is told the CPU has no local APIC, SeaBIOS skips
# smp_setup() and never programs LINT0 as ExtINT, and the enabled APIC
# with a masked LINT0 swallows every i8259 interrupt. The guest then boots
# to its first wait on the BIOS tick counter at 0040:006C and spins there
# for ever: a frozen splash screen (or a boot menu whose countdown never
# moves) with a cursor that still blinks, because that blink is drawn by
# the VGA adapter on the host side and needs no guest at all.
#
# So the verdict is not "does the screen change". It is:
#
#   1. the machine really reset            a second SeaBIOS banner on the
#                                          debugcon, not a screendump
#   2. the guest made progress after it    disk reads, which a guest
#                                          spinning on the tick counter
#                                          does not issue (the frozen run
#                                          stops dead at ~400 of them)
#   3. LINT0 is ExtINT again               the mechanism itself, so a
#                                          regression is named, not just
#                                          observed
#
# Two ways in, because they are different code paths in QEMU and only the
# second is what a user does: `qmp` is a QMP system_reset, `guest` is the
# Start menu's Shut Down -> Restart typed over QMP. `guest` reports SKIP
# rather than FAIL when the menu could not be driven (no second banner):
# that is the script failing to click, not the machine failing to reboot.
#
# Win98 runs emulated by decision (CLAUDE.md: under KVM this image's
# Explorer dies at startup). The image is never written; everything goes
# to a qcow2 overlay under build/win98-reboot. Needs a guest image, so it is
# run by hand and never from scripts/test.sh.
#
# The first boot is not slept out: this test already counts the guest's
# disk reads, so it waits for them to stop instead (tools/guestwait.sh)
# and BOOT_WAIT is the cap on that.
#
# Env: OUT=dir, QEMU=binary (an A/B against another build), BIOS=0 (use the
# binary's own firmware instead of qemu/pc-bios, for a stock-QEMU control),
# BOOT_WAIT=s (the cap, default 300), WATCH=s (default 150), MIN_READS=n (default
# 5000: a full Win98 boot is ~60000, a frozen one ~400).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"
MODES="${2:-both}"
[ "$MODES" = both ] && MODES="qmp guest"
OUT="${OUT:-$ROOT/build/win98-reboot}"
QEMU="${QEMU:-$ROOT/build/qemu/qemu-system-i386}"
BOOT_WAIT="${BOOT_WAIT:-300}"
WATCH="${WATCH:-150}"
MIN_READS="${MIN_READS:-5000}"
# an AF_UNIX path is capped at 108 bytes and a build/ path under a deep
# checkout is already close: keep the socket short and outside the tree
SOCK="${SOCK:-/tmp/2ks-reboot.sock}"

[ -x "$QEMU" ] || { echo "no $QEMU: scripts/build.sh"; exit 1; }
[ -f "$IMG" ] || { echo "no $IMG"; exit 1; }
mkdir -p "$OUT"

qmp() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
hmp() {
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json \
    "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}" \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["return"])'
}
# every drive's read count: the honest "is the guest doing anything" probe
reads() { python3 - "$SOCK" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile("rwb", buffering=0)
f.readline(); f.write(b'{"execute":"qmp_capabilities"}\n'); f.readline()
f.write(b'{"execute":"query-blockstats"}\n')
while True:
    r = json.loads(f.readline())
    if "return" in r:
        break
print(sum(d["stats"]["rd_operations"] for d in r["return"]))
PY
}
lvt0() { hmp "info lapic" | sed -n 's/^LVT0\t *\([^ ]*\).*/\1/p'; }
# POSTs so far. A SeaBIOS built without debug output (any stock QEMU) writes
# nothing at all here, and then this evidence is not available (see where
# it is used).
banners() {
  local n
  n=$(grep -c "^SeaBIOS (version" "$D/seabios.log" 2>/dev/null)
  echo "${n:-0}"
}

fail=0
for MODE in $MODES; do
  D="$OUT/$MODE"; rm -rf "$D"; mkdir -p "$D"; rm -f "$SOCK"
  echo "==> $MODE: fresh overlay, booting"
  "$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$D/ovl.qcow2"

  # doc 06's Win98 machine, emulated (see the header)
  HW=(-cpu pentium3 -machine pc -m 256 -vga cirrus
      -netdev user,id=n0 -device pcnet,netdev=n0
      -audiodev none,id=a0 -device sb16,audiodev=a0 -accel tcg)
  [ "${BIOS:-1}" = 1 ] && HW=(-L "$ROOT/qemu/pc-bios" "${HW[@]}")
  "$QEMU" "${HW[@]}" -hda "$D/ovl.qcow2" -boot c -display none \
    -chardev "file,path=$D/seabios.log,id=sea" \
    -device isa-debugcon,iobase=0x402,chardev=sea \
    -qmp "unix:$SOCK,server,nowait" -monitor none > "$D/qemu.log" 2>&1 &
  QPID=$!
  trap 'kill $QPID 2>/dev/null || true; rm -f "$SOCK"' EXIT

  GW_PID=$QPID
  gw_wait_sock "$SOCK" || exit 1
  # the same reads this test counts, used the other way round: the boot is
  # over when the guest stops making them
  gw_wait_quiet "$SOCK" "$BOOT_WAIT" 10 || true
  qmp screendump "$D/desktop.png" >/dev/null || true
  before=$(reads); banners_before=$(banners)
  echo "    before: $before reads, LVT0 $(lvt0), $banners_before POST(s)"

  if [ "$MODE" = qmp ]; then
    qmp json '{"execute":"system_reset"}' >/dev/null
  else
    # Start -> Shut Down... -> the dialog's Restart radio button -> OK.
    # Escape first: a message box in front of the shell eats the chord.
    qmp keys esc >/dev/null || true; sleep 1
    qmp keys ctrl+esc >/dev/null || true; sleep 3
    qmp keys u >/dev/null || true; sleep 4
    qmp screendump "$D/shutdown-dialog.png" >/dev/null || true
    qmp keys down >/dev/null || true; sleep 1
    qmp keys ret >/dev/null || true
  fi

  echo "==> $MODE: watching ${WATCH}s"
  t=0; after=$before
  while [ "$t" -lt "$WATCH" ]; do
    sleep 10; t=$((t + 10))
    after=$(reads)
    printf '    t%3ds  %s reads (+%s)  LVT0 %s\n' \
      "$t" "$after" "$((after - before))" "$(lvt0)"
    [ "$((after - before))" -ge "$MIN_READS" ] && break
  done
  qmp screendump "$D/after.png" >/dev/null || true
  posts=$(banners); lv=$(lvt0)
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md) --
  # but a guest frozen on the tick counter cannot answer, so give it a
  # bounded try and then take the machine down over ACPI, which needs no shell
  qmp keys ctrl+esc >/dev/null 2>&1 || true; sleep 2
  qmp keys u >/dev/null 2>&1 || true; sleep 2
  qmp keys ret >/dev/null 2>&1 || true
  gw_wait_exit "$QPID" 40 || true
  if kill -0 $QPID 2>/dev/null; then
    qmp json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
    gw_wait_exit "$QPID" 40 || true
  fi
  kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
  trap - EXIT; rm -f "$SOCK"

  echo "    after:  $after reads (+$((after - before))), LVT0 $lv, $posts POST(s)"
  # Did the machine reset at all? The banners say so outright; with a
  # firmware that logs nothing (a stock QEMU's SeaBIOS) the fallback is that
  # a reset always re-reads the boot sector, so *some* reads must have
  # happened. The freeze this guards stops at a few hundred of them, well
  # above zero and well below MIN_READS.
  reset_seen=1
  if [ "$banners_before" -gt 0 ]; then
    [ "$posts" -gt "$banners_before" ] || reset_seen=0
  else
    [ "$((after - before))" -gt 0 ] || reset_seen=0
  fi
  if [ "$reset_seen" = 0 ]; then
    if [ "$MODE" = guest ]; then
      echo "SKIP $MODE — the machine never reset: the Start menu was not"
      echo "     driven (see $D/shutdown-dialog.png), not a reboot failure"
      continue
    fi
    echo "FAIL $MODE — no second POST: the machine never reset"; fail=1; continue
  fi
  if [ "$((after - before))" -lt "$MIN_READS" ]; then
    echo "FAIL $MODE — the machine reset and then stopped: only"
    echo "     $((after - before)) reads in ${WATCH}s (want $MIN_READS)."
    case "$lv" in
      0x00010000) echo "     LVT0 is masked — this is patch 22's bug: the"
                  echo "     firmware was told the CPU has no local APIC and"
                  echo "     never set LINT0 to ExtINT, so the i8259's"
                  echo "     interrupts are dropped. See $D/after.png." ;;
      *)          echo "     LVT0 is $lv, so it is something else; $D/qemu.log" ;;
    esac
    fail=1; continue
  fi
  echo "PASS $MODE — reset, then a whole boot: +$((after - before)) reads, LVT0 $lv"
done
exit $fail
