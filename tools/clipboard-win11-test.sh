#!/usr/bin/env bash
# The clipboard and the shared folder between this host and Windows 11
# (track M23 steps 4-6), the way a user gets them: the player, with
# the launcher's `--share <dir>` (it serves the folder itself) and
# QEMU's qemu-vdagent; in the guest, the drivers disc's 2ksbox\install
# (vioser, the agent and its two logon tasks).
#
#   tools/clipboard-win11-test.sh [base]    base: an installed machine's
#                                           win11-spike.py OUT (build/w11d)
#
# On a Mac, Windows 11 on Arm under HVF (base build/w11d); on Linux, x64
# Windows 11 under KVM (base /mnt/data2/david/w11, step 1's install).
# Boots a fresh qcow2 overlay of base in OUT (build/w11s) in the player
# (tools/player-as-qemu.sh), whose window opens for the run. Checks, with
# tools/win11-spike/clip.ps1 in the guest:
#   1. the disc's installer registers the agent's tasks;
#   2. text on the host's clipboard at boot reaches the guest once the
#      agent is up;
#   3. text the guest sets reaches the host (pbpaste, wl-paste or xclip);
#   4. a second host text reaches the guest;
#   5. the agent maps the shared folder, and a host file reads through it.
# The host's clipboard is the user's own: its text is saved first and put
# back at the end (anything that was not text is lost).
#
# Needs: target/qemu-<aarch64|x86_64>/release/player (scripts/build.sh
# rust) and the drivers disc with the agent (scripts/build-virtio-win.sh).
# macOS on Apple Silicon, or Linux on x86_64 with KVM.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
case "$(uname -s)" in
  Darwin) ARCH=aarch64 DISC=arm64 DEFBASE=build/w11d ;;
  *) ARCH=x86_64 DISC=x64 DEFBASE=/mnt/data2/david/w11 ;;
esac
BASE="$(cd "${1:-$DEFBASE}" && pwd)"
OUT="${OUT:-build/w11s}"
PLAYER=target/qemu-$ARCH/release/player
DRIVERS=build/virtio-win/2ksbox-drivers-$DISC.iso
for f in "$PLAYER" "$DRIVERS" "$BASE/disk.qcow2"; do
  [ -e "$f" ] || { echo "clipboard-win11-test: no $f"; exit 77; }
done
# the host's clipboard, from a shell
if [ "$ARCH" = aarch64 ]; then
  paste() { pbpaste; }; copy() { pbcopy; }
elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
  # both leave a process serving the text: off this script's output, or
  # a pipe reading it never ends
  paste() { wl-paste -n 2>/dev/null; }; copy() { wl-copy >/dev/null 2>&1; }
else
  paste() { xclip -o -selection clipboard 2>/dev/null; }; copy() { xclip -i -selection clipboard >/dev/null 2>&1; }
fi

chmod -R u+w "$OUT" 2>/dev/null; rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BASE/vars.fd" "$BASE/tpm.permall" "$OUT/"
build/qemu/qemu-img create -q -f qcow2 -b "$BASE/disk.qcow2" -F qcow2 "$OUT/disk.qcow2" || exit 1
M1="from-host-$RANDOM$RANDOM"
dd if=/dev/zero of="$OUT/report.img" bs=1048576 count=32 2>/dev/null
mformat -i "$OUT/report.img" -v REPORT -F ::
printf '%s' "$M1" > "$OUT/expect.txt"
mcopy -i "$OUT/report.img" "$OUT/expect.txt" ::
# the folder the player shares, as the launcher's --share names it
mkdir -p "$OUT/share"
echo "hello from the host" > "$OUT/share/hello.txt"

OLD="$(paste)"
printf '%s' "$M1" | copy
restore() { printf '%s' "$OLD" | copy; }
trap restore EXIT

# the host's side of step 2 and 3: wait for the guest's text, then answer
( for _ in $(seq 900); do
    t="$(paste)"
    case "$t" in from-guest-*) printf '%s\n' "$t" > "$OUT/host-got.txt"; printf '%s' "$M1-again" | copy; exit 0 ;; esac
    sleep 1
  done ) &
WATCH=$!

PLAYER="$ROOT/$PLAYER" PLAYER_OPTS="--share $ROOT/$OUT/share" OUT="$OUT" TPM_PPI="${TPM_PPI:-off}" ARCH=$ARCH \
  TPM=libtpms DRIVERS="$ROOT/$DRIVERS" NET=1 CLIPBOARD=1 QEMU="$ROOT/tools/player-as-qemu.sh" \
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
