#!/usr/bin/env bash
# A Windows 98 blue screen has to be *visible* on the d3dpt-vga adapter
# (doc 19 §29, M10).
#
#   tools/win98-bsod-test.sh ~/.local/share/2ksbox/machines/test98/disk.qcow2
#
# Why this is a test at all: a 9x blue screen (a fatal exception, a
# "Windows protection error", the Ctrl+Alt+Del screen) is drawn by the main
# VDD in VGA text mode, which it programs itself with no int 10h and no
# display driver drawing. Our adapter scans out the linear frame buffer
# while ENABLE is set, and until the mini-VDD hooked the VDD's screen switch
# nothing turned it off, so every blue screen was invisible. The machine
# "hung on a glitched desktop" while a message asking for a key sat in VRAM
# behind it (doc 19 §15 reads them out afterwards). The mini-VDD turns the
# linear frame buffer off on PRE_HIRES_TO_VGA and on
# SAVE_MESSAGE_MODE_STATE, and this is what proves a blue screen is seen.
#
# What it does: boots a raw copy of the image on our adapter with the
# freshly built driver (tools/win98-game-test.sh does the staging), makes
# Windows blue-screen from RUN.BAT by running BSOD.EXE (guest-tools/src/
# d3dptvid/w9x/bsod.c), which loads BSODVXD.VXD (bsodvxd.c), a dynamic VxD
# of ours whose init executes an invalid opcode in ring 0, so the VMM puts
# up "exception 06 in VxD BSODVXD(01)". (It tries the famous `C:\con\con`
# first; that is patched on the test image, and from a DOS box it only
# ends the DOS box anyway.) Then it reads the VGA text page out of VRAM
# while the screen is up, presses a key, and requires all of:
#
#   1. the VxD's `hi-res -> VGA` (or `message mode`) line after the desktop
#      came up and the device's `linear mode off` after it: a hook fired
#      and the adapter went back to its VGA core. Measured: a VxD's fatal
#      exception takes the ordinary screen switch (PRE_HIRES_TO_VGA + the
#      INT 2Fh notification); SAVE_MESSAGE_MODE_STATE is the DDK's door for
#      message screens that skip the switch, called once at boot as well;
#   2. a screendump taken meanwhile that is mostly the blue screen's blue,
#      which is what a person at the window would see (a screendump shows
#      what is scanned out: with ENABLE on it would show the desktop);
#   3. the text page naming a VxD or the 0028: selector, i.e. the blue
#      screen is a real exception screen and not some other text mode;
#   4. `linear mode on` again after the key, and a last screendump that is
#      not blue: "press any key to attempt to continue" continued, the
#      desktop came back, and the machine powers off on the button.
#
# WHEN=event is the other kind of blue screen: BSOD.EXE loads BSODTMR.VXD
# (the same source with -DBSOD_TIMER), whose init only arms a timer and
# whose callback faults a second later. A fault outside any VM's own
# execution gets its blue screen with no screen switch and no mini-VDD call
# at all, only the VMM's Begin_Message_Mode / End_Message_Mode control
# messages, which the mini-VDD answers (`message mode begins` / `ends` in
# the log). Without that, every blue screen of this kind is invisible (the
# patch-44 corruption's were). `NO_DRIVER=1` runs the image's own driver
# instead of the fresh build, the control.
#
# Env: RAW=, OUT=, BOOT_WAIT=, everything win98-game-test.sh takes;
# WHEN=init|event; TRIGGER= replaces the RUN.BAT body and STAGE= the file
# staged for it (a different way to blue-screen). The user's own player
# often holds the image's lock; RAW=build/w98game/guest.raw FRESH=0 reuses
# the copy the game harness made.
#
# Overlay/copy only, never the image. Local only (needs a guest image), not
# in scripts/test.sh.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

IMG="${1:?usage: win98-bsod-test.sh <image.qcow2>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export OUT="${OUT:-$ROOT/build/w98bsod/run}"
export RAW="${RAW:-$ROOT/build/w98bsod/guest.raw}"

# The blue screen comes up while the desktop is still settling (RUN.BAT
# starts with the shell), so the run clock's first shots are already of it;
# the key at 70 s is "press any key to attempt to continue", the text page
# is read at 40 s, and the last shot is the desktop that came back.
# WHEN=init (the default) faults in the VxD's init, which the VDD answers
# with its screen switch; WHEN=event faults from a timer callback a second
# later, which gets its blue screen with no switch and no mini-VDD call,
# only the VMM's Begin_Message_Mode.
case "${WHEN:-init}" in
  init)  BSOD_VXD=bsodvxd.vxd; BSOD_CMD='C:\BSOD.EXE' ;;
  event) BSOD_VXD=bsodtmr.vxd; BSOD_CMD='C:\BSOD.EXE C:\BSODTMR.VXD' ;;
  *) echo "WHEN= is init or event"; exit 1 ;;
esac
export STAGE="${STAGE:-$ROOT/guest-tools/out/driver9x/bsod.exe $ROOT/guest-tools/out/driver9x/$BSOD_VXD}"
export GUEST_CMD="${TRIGGER:-$BSOD_CMD}"
export SETTLE="${SETTLE:-20}" RUN_SECS="${RUN_SECS:-130}" SHOTS="${SHOTS:-10}"
export KEYS="${KEYS:-70:ret}" TEXT_AT="${TEXT_AT:-40}" FRESH="${FRESH:-1}"
export QMPC_HOLD="${QMPC_HOLD:-300}"

mkdir -p "$(dirname "$OUT")"
echo "==> win98-bsod-test: $IMG -> $OUT"
# The harness's exit status is not the verdict. The checks below are.
"$ROOT/tools/win98-game-test.sh" "$IMG" "$(basename "$OUT")" 2>&1 | tee "$OUT.log" || true
[ -f "$OUT/qemu.log" ] || { echo "FAIL  the harness produced no run (see $OUT.log)"; exit 1; }

fail=0
ok()   { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=1; }

# 1. the hooks and the device. A VxD's fatal exception reaches the adapter
# through the ordinary screen switch (`hi-res -> VGA`, the PRE_HIRES_TO_VGA
# hook), while SAVE_MESSAGE_MODE_STATE (`message mode`) is the DDK's door
# for message screens that skip the switch and is also called once at
# boot. So either counts *after* the desktop's first `linear mode on`, and
# the linear mode must then have gone off.
if awk '/linear mode on/{on=1} on && (/hi-res -> VGA/ || /d3dptvxd: message mode/){m=1} END{exit !m}' "$OUT/qemu.log"; then
  ok "the VDD told the mini-VDD it was taking the screen ($(awk '/linear mode on/{on=1} on && /hi-res -> VGA/{print "PRE_HIRES_TO_VGA"; exit} on && /message mode begins/{print "Begin_Message_Mode"; exit} on && /message mode/{print "SAVE_MESSAGE_MODE_STATE"; exit}' "$OUT/qemu.log"))"
else
  bad "no screen switch and no message mode after the desktop came up: nothing blue-screened, or the VDD used a door the mini-VDD does not hook"
fi
if awk '/linear mode on/{on=1} on && (/hi-res -> VGA/ || /d3dptvxd: message mode/){m=1} m && /linear mode off/{f=1} END{exit !f}' "$OUT/qemu.log"; then
  ok "the linear frame buffer went off for it"
else
  bad "the linear frame buffer stayed on: the blue screen was drawn behind the desktop"
fi

# 2. what a screendump (the window) showed meanwhile, and 4. after
# the key. A blue screen is white on (0,0,0xaa); ask the .ppm qmpc.py keeps
# beside every .png what fraction of the pixels are that blue.
blue_share() {
  python3 - "$1" <<'PYBLUE'
import sys
d = open(sys.argv[1], 'rb').read()
# P6 <w> <h> 255\n then rgb bytes
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
n = w * h; blue = 0
for i in range(0, n * 3, 3):
    r, g, b = px[i], px[i + 1], px[i + 2]
    if r < 0x30 and g < 0x30 and 0x80 <= b <= 0xc0: blue += 1
print("%d %dx%d" % (100 * blue // n, w, h))
PYBLUE
}
best=0; bestshot=
for s in "$OUT"/shots/t0*.png.ppm; do
  [ -f "$s" ] || continue
  read -r share dims < <(blue_share "$s")
  [ "$share" -gt "$best" ] && { best=$share; bestshot="$(basename "$s" .ppm) ($dims, ${share}% blue)"; }
done
if [ "$best" -ge 60 ]; then
  ok "a screendump during it is the blue screen: $bestshot"
else
  bad "no screendump in the first shots is mostly blue (best: ${bestshot:-none}) — the window showed the desktop"
fi

# 3. the text page, read at TEXT_AT
if grep -qE "^ *\| .*(VxD|0028:|CTRL\+ALT\+DEL|exce)" "$OUT.log"; then
  ok "the text page is an exception screen: $(grep -E '^ *\| .*(VxD|0028:)' "$OUT.log" | head -1 | sed 's/^ *| *//' | cut -c1-70)"
else
  bad "the VRAM text page at ${TEXT_AT}s is not a Windows exception screen (see $OUT.log)"
fi

# 4. the way back
if awk '/linear mode on/{on=1} on && (/hi-res -> VGA/ || /d3dptvxd: message mode/){m=1} m && /linear mode on/{f=1} END{exit !f}' "$OUT/qemu.log"; then
  ok "the linear frame buffer came back after the key"
else
  bad "the desktop never came back after the key (no 'linear mode on' after the blue screen)"
fi
last="$OUT/shots/zfinal.png.ppm"
if [ -f "$last" ]; then
  read -r share dims < <(blue_share "$last")
  if [ "$share" -lt 30 ]; then ok "the last screendump is not blue (${share}%): the desktop is back"
  else bad "the last screendump is still ${share}% blue"; fi
else
  bad "no final screendump"
fi
if [ -f "$OUT/hang.txt" ]; then
  bad "the machine did not power off after the round trip ($OUT/hang.txt)"
else
  ok "the machine powered off on the button afterwards"
fi

[ $fail = 0 ] && echo "win98-bsod-test: PASS" || { echo "win98-bsod-test: FAIL"; exit 1; }
