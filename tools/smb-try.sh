#!/usr/bin/env bash
# Try the shared folder by hand (track M23) before the launcher has the
# setting: Windows 11 on Arm in the player, with a host folder served at
# \\10.0.2.4\host (user smb, password smb).
#
#   tools/smb-try.sh <folder> [base]     base: an installed machine's
#                                        win11-spike.py OUT (build/w11d)
#
# Boots a fresh qcow2 overlay of base's disk in OUT (build/w11s), with
# copies of its firmware variables and TPM state, so base is never
# written; the folder is the one thing changed for real. The board is the
# launcher's (bundle::Machine::arm_args), with the TPM's PPI region off
# (QEMU 11.1's HVF aborts on it, M21) and the guest's 10.0.2.4:445
# forwarded to smbserve (QEMU patch 79). smbserve's log is OUT/smb.log.
# Mac on Apple Silicon (HVF); close the window or shut Windows down to end.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ $# -ge 1 ] || { sed -n '2,16p' "$0"; exit 2; }
DIR="$(cd "$1" && pwd)"
BASE="$(cd "${2:-build/w11d}" && pwd)"
OUT="$ROOT/${OUT:-build/w11s}"
PLAYER=target/qemu-aarch64/release/player
for f in "$PLAYER" target/release/smbserve build/qemu/qemu-img "$BASE/disk.qcow2"; do
  [ -e "$f" ] || { echo "smb-try: no $f"; exit 1; }
done

rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BASE/vars.fd" "$BASE/tpm.permall" "$OUT/"
build/qemu/qemu-img create -q -f qcow2 -b "$BASE/disk.qcow2" -F qcow2 "$OUT/disk.qcow2"

target/release/smbserve --unix "$OUT/smb.sock" --user smb --password smb -v "$DIR" >"$OUT/smb.log" 2>&1 &
SP=$!
trap 'kill $SP 2>/dev/null' EXIT
echo "smb-try: serving $DIR as \\\\10.0.2.4\\host (smb / smb); log $OUT/smb.log"

FW="$ROOT/qemu/pc-bios"
"$PLAYER" -- -L "$FW" -machine virt,gic-version=3 -accel hvf -cpu max -smp 4 -m 4096 \
  -drive "if=pflash,format=raw,unit=0,readonly=on,file=$FW/2ksbox-aarch64-code.fd" \
  -drive "if=pflash,format=raw,unit=1,file=$OUT/vars.fd" \
  -tpmdev "libtpms,id=tpm0,state=$OUT/tpm.permall" -device tpm-tis-device,tpmdev=tpm0,ppi=off \
  -rtc base=localtime \
  -device ich9-ahci,id=ide -drive "file=$OUT/disk.qcow2,if=none,id=disk0" -device ide-hd,bus=ide.0,drive=disk0 \
  -device ramfb -device virtio-gpu-pci \
  -device qemu-xhci -device usb-kbd -device usb-tablet \
  -device ich9-intel-hda -device hda-output,audiodev=embed0 \
  -netdev "user,id=n0,guestfwd=tcp:10.0.2.4:445-unix:$OUT/smb.sock" -device virtio-net-pci,netdev=n0
