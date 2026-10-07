#!/usr/bin/env bash
# A new Windows 11 machine starts setup from its disc with nobody at the
# keyboard (track M20): the firmware boots the disc before the EFI shell
# whatever its variables recorded, and the player answers the disc's
# "Press any key to boot from CD or DVD" (player-core/src/boot_prompt.rs).
#
#   tools/win11-boot-prompt-test.sh [iso]     default $W11_ISO, or
#                                             ~/Downloads/Windows11_Client_arm64_*.iso
#
# Makes a machine with launcherx in OUT (build/w11p), then two runs of
# the player on the launcher's own arguments, whose window opens for each:
#   1. with an empty drive, until the firmware gives up and starts the EFI
#      shell. That is how a user's first start goes, and it leaves the
#      disc out of the boot order the variables keep (it would come after
#      the shell);
#   2. with the disc in the drive (`launcherx --drive insert`): the
#      firmware must start the disc without starting the shell, the player
#      must answer, and setup's screen must come up (its background
#      fills the screen; the firmware's and the shell's are black).
#
# Needs: target/release/launcherx and target/qemu-aarch64/release/player
# (scripts/build.sh), Microsoft's Arm64 ISO (not in the repo; its download
# page refuses scripts). A Mac on Apple Silicon, under HVF: about 1.5 min.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) ;;
  *) echo "win11-boot-prompt-test: Windows 11 on Arm under HVF only, for now"; exit 77 ;;
esac
ISO="${1:-${W11_ISO:-$(ls "$HOME"/Downloads/Windows11_Client_arm64_*.iso 2>/dev/null | head -1)}}"
OUT="${OUT:-build/w11p}"
LX=target/release/launcherx
PLAYER=target/qemu-aarch64/release/player
for f in "$LX" "$PLAYER" "$ISO"; do
  [ -n "$f" ] && [ -e "$f" ] || { echo "win11-boot-prompt-test: no ${f:-Windows 11 Arm64 ISO}"; exit 77; }
done
ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"

rm -rf "$OUT" && mkdir -p "$OUT/library"
OUT="$(cd "$OUT" && pwd)"
export LAUNCHER_LIBRARY_DIR="$OUT/library" LAUNCHER_DISC_LIBRARY="$OUT/discs.toml" \
  LAUNCHER_QEMU_IMG_BIN="$ROOT/build/qemu/qemu-img"
bundle="$($LX --wizard-new win11 "Prompt check" 64 2>/dev/null | tail -1)"
[ -f "$bundle" ] || { echo "FAIL: --wizard-new made no bundle"; exit 1; }
$LX --prepare "$bundle" >/dev/null || { echo "FAIL: --prepare"; exit 1; }
# short: a deep path is "AF_UNIX path too long"
SOCK="/tmp/w11p-$$.sock"

# run <log>: the player on the bundle's arguments, in the background
run() {
  local -a argv
  read -r -a argv <<< "$($LX --print-args "$bundle")"
  "$PLAYER" -- "${argv[@]}" -qmp "unix:$SOCK,server,nowait" > "$1" 2>&1 &
  PID=$!
}
# wait_for <log> <pattern> <seconds>
wait_for() {
  local i
  for i in $(seq "$3"); do
    grep -q -e "$2" "$1" && return 0
    kill -0 "$PID" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}
quit() {
  python3 tools/qmpc.py "$SOCK" json '{"execute":"quit"}' >/dev/null 2>&1
  wait "$PID" 2>/dev/null
  rm -f "$SOCK"
}
# lit <ppm>: the share of the screen that is not black, in percent
lit() {
  python3 - "$1" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
head = d.split(b"\n", 3)
w, h = map(int, head[1].split())
px = head[3][: w * h * 3]
dark = sum(1 for i in range(0, len(px), 3 * 7) if px[i] + px[i + 1] + px[i + 2] < 48)
print(100 - dark * 100 // len(range(0, len(px), 3 * 7)))
EOF
}

echo "== 1. an empty drive, until the EFI shell"
run "$OUT/run1.log"
wait_for "$OUT/run1.log" 'starting Boot.*EFI Internal Shell' 90 \
  || { echo "FAIL: the firmware never started the shell"; grep -e '\[firmware\]' -e '\[boot\]' "$OUT/run1.log"; quit; exit 1; }
quit
grep '\[firmware\]' "$OUT/run1.log" | sed 's/^/  /'

echo "== 2. the disc in the drive"
o="$($LX --drive "$bundle" insert "$ISO" 2>&1)" || { echo "FAIL: --drive insert: $o"; exit 1; }
grep -q "^disc = \"$ISO\"" "$bundle" || { echo "FAIL: the bundle's disc is not the ISO"; grep '^disc' "$bundle"; exit 1; }
run "$OUT/run2.log"
rc=0
if wait_for "$OUT/run2.log" '\[boot\] the firmware starts a disc' 90; then
  lit_pct=0
  for i in $(seq 24); do
    sleep 5
    python3 tools/qmpc.py "$SOCK" json "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/screen.ppm\"}}" >/dev/null 2>&1
    [ -s "$OUT/screen.ppm" ] && lit_pct="$(lit "$OUT/screen.ppm")"
    [ "$lit_pct" -ge 60 ] && break
  done
  if [ "$lit_pct" -ge 60 ]; then
    python3 tools/qmpc.py "$SOCK" screendump "$OUT/setup.png" >/dev/null 2>&1
    echo "  setup's screen is up ($lit_pct % of it lit): $OUT/setup.png"
  else
    echo "FAIL: no setup screen within two minutes of the disc starting ($lit_pct % lit)"; rc=1
  fi
else
  echo "FAIL: the firmware never started the disc"; rc=1
fi
quit
grep -e '\[firmware\]' -e '\[boot\]' "$OUT/run2.log" | sed 's/^/  /'
if grep -q 'starting Boot.*EFI Internal Shell' "$OUT/run2.log"; then
  echo "FAIL: the firmware started the shell with the disc in the drive"; rc=1
fi
[ $rc = 0 ] && echo "PASS: setup started from the disc with no key pressed"
exit $rc
