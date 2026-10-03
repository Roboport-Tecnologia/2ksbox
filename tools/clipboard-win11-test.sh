#!/usr/bin/env bash
# The clipboard between this Mac and Windows 11 on Arm (track M23 step 5):
# the player (whose embed library is the host's end of QEMU's
# qemu-vdagent), virtio-win's vioser and our agent in the guest.
#
#   tools/clipboard-win11-test.sh [base]    base: an installed machine's
#                                           win11-spike.py OUT (build/w11d)
#
# Boots a fresh qcow2 overlay of base in OUT (build/w11s) in the player
# (tools/player-as-qemu.sh), whose window opens for the run. Checks, with
# tools/win11-spike/clip.ps1 in the guest:
#   1. text on the host's clipboard at boot reaches the guest once the
#      agent is up;
#   2. text the guest sets reaches the host (pbpaste);
#   3. a second host text reaches the guest.
# The host's clipboard is the user's own: its text is saved first and put
# back at the end (anything that was not text is lost).
#
# Needs: target/qemu-aarch64/release/player (scripts/build.sh rust), the
# agent (cd guest-agent && cargo build --release --target
# x86_64-pc-windows-gnu), and virtio-win's ISO in build/deps/src
# (scripts/build-virtio-win.sh fetches it). macOS on Apple Silicon.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BASE="$(cd "${1:-build/w11d}" && pwd)"
OUT="${OUT:-build/w11s}"
AGENT=guest-agent/target/x86_64-pc-windows-gnu/release/2ksbox-agent.exe
ISO=$(ls build/deps/src/virtio-win-*.iso 2>/dev/null | tail -1)
for f in target/qemu-aarch64/release/player "$AGENT" "$ISO" "$BASE/disk.qcow2"; do
  [ -e "$f" ] || { echo "clipboard-win11-test: no $f"; exit 77; }
done

chmod -R u+w "$OUT" 2>/dev/null; rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BASE/vars.fd" "$BASE/tpm.permall" "$OUT/"
build/qemu/qemu-img create -q -f qcow2 -b "$BASE/disk.qcow2" -F qcow2 "$OUT/disk.qcow2" || exit 1
# xorriso, as scripts/build-virtio-win.sh: bsdtar refuses the ISO's
# hard links ("Skipping hardlink pointing to itself")
xorriso -osirrox on -indev "$ISO" -extract /vioserial/w11/ARM64 "$OUT/vioser" >/dev/null 2>&1
chmod -R u+w "$OUT/vioser"
rm -f "$OUT"/vioser/*.pdb
[ -f "$OUT/vioser/vioser.inf" ] || { echo "clipboard-win11-test: no vioser in $ISO"; exit 1; }

M1="from-host-$RANDOM$RANDOM"
dd if=/dev/zero of="$OUT/report.img" bs=1m count=32 2>/dev/null
mformat -i "$OUT/report.img" -v REPORT -F ::
printf '%s' "$M1" > "$OUT/expect.txt"
mcopy -i "$OUT/report.img" "$OUT/expect.txt" "$AGENT" ::
mmd -i "$OUT/report.img" ::vioser
mcopy -i "$OUT/report.img" "$OUT"/vioser/* ::vioser/

OLD="$(pbpaste)"
printf '%s' "$M1" | pbcopy
restore() { printf '%s' "$OLD" | pbcopy; }
trap restore EXIT

# the host's side of step 2 and 3: wait for the guest's text, then answer
( for _ in $(seq 900); do
    t="$(pbpaste)"
    case "$t" in from-guest-*) printf '%s\n' "$t" > "$OUT/host-got.txt"; printf '%s' "$M1-again" | pbcopy; exit 0 ;; esac
    sleep 1
  done ) &
WATCH=$!

OUT="$OUT" TPM_PPI="${TPM_PPI:-off}" ARCH=aarch64 CLIPBOARD=1 QEMU="$ROOT/tools/player-as-qemu.sh" \
  PROBE=1 PROBE_PS1=tools/win11-spike/clip.ps1 PROBE_WAIT=300 SETTLE=10 \
  python3 tools/win11-spike.py boot >"$OUT/run.out" 2>&1
kill $WATCH 2>/dev/null

fail=0
grep -o 'W11-PROBE .*' "$OUT/run.out" | sed 's/^W11-PROBE //' | tee "$OUT/probe.txt"
grep -q '^done' "$OUT/probe.txt" || { echo "FAIL: the probe never finished ($OUT/run.out)"; fail=1; }
grep -q '^FAIL' "$OUT/probe.txt" && fail=1
sent="$(sed -n 's/^guest-set //p' "$OUT/probe.txt")"
got="$(cat "$OUT/host-got.txt" 2>/dev/null)"
if [ -n "$sent" ] && [ "$sent" = "$got" ]; then echo "PASS guest-to-host $got"
else echo "FAIL guest-to-host: the guest set '$sent', the host saw '$got'"; fail=1; fi
[ $fail = 0 ] && echo "clipboard-win11: pass"
exit $fail
