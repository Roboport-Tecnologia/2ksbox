#!/usr/bin/env bash
# CD-DA through a guest's own MCI, on both Windows families (doc 17 §5.4,
# §6.3):
#
#   tools/cdaudio-guest-test.sh ~/vms/win98.qcow2 win98
#   tools/cdaudio-guest-test.sh ~/vms/winxp.qcow2 xp
#
# `CDTEST.EXE` lists the tracks, plays track 2, pauses, resumes and **stops**,
# and asks MCI for the mode after each. The point of this tool is the last
# one: *how* a family's `mcicda` stops a drive is not the same command on 9x
# and NT, and a drive that answers one of them and not the other plays on
# after the Stop button. So the run is always traced (`CDIMAGE_TRACE=1`) and
# prints the packets around the stop: the guest's own words, rather than an
# assumption about them.
#
# The drive's audiodev is a wav file, so what the guest played is on the host
# afterwards. The disc is the selftest's mixed-mode one (track 1 data, tracks
# 2-3 audio, a 1 kHz tone in track 2) unless DISC= names another; the guest
# writes CDTEST.LOG to the floppy it booted the batch from (BOXLOG), which is how it
# comes back on a machine with no writable disk of its own.
#
# The image is never written: everything goes to a qcow2 overlay under
# build/cdaudio-guest. Local only (needs a guest image), so not in
# scripts/test.sh. The boot is not slept out (tools/guestwait.sh).
# Env: OUT=dir, DISC=cue, DRIVE=D, PLAY=6 (seconds of track 2),
#      BOOT_WAIT=s (the cap, 300), NO_KVM=1, KEEP=1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; FAMILY="${2:-win98}"
OUT="${OUT:-$ROOT/build/cdaudio-guest}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"; FLOPPY="$OUT/tools.img"; WAV="$OUT/cd-$FAMILY.wav"
LOG="$OUT/serial-$FAMILY.log"; QLOG="$OUT/qemu-$FAMILY.log"; SOCK="$OUT/q.sock"
QEMU="$ROOT/build/qemu/qemu-system-i386"
CDTEST="${CDTEST:-$ROOT/guest-tools/out/iso/TESTS/CDTEST.EXE}"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }
[ -f "$CDTEST" ] || { echo "no $CDTEST: guest-tools/build-wrappers.sh first"; exit 1; }

# The disc: the selftest's mixed-mode one, which is where the 1 kHz tone in
# track 2 comes from. `discx selftest` writes it as a side effect of running,
# so its own verdict is reported and then ignored.
DISC="${DISC:-}"
if [ -z "$DISC" ]; then
  "$ROOT/target/release/discx" selftest "$OUT/discs" > "$OUT/discx.log" 2>&1 ||
    { echo "discx selftest failed: $OUT/discx.log"; exit 1; }
  DISC="$OUT/discs/mixed.cue"
fi
[ -f "$DISC" ] || { echo "no disc at $DISC"; exit 1; }
echo "==> disc: $DISC"
"$ROOT/target/release/discx" info "$DISC" | sed 's/^/    /'

# CDTEST writes its log to C:\2KSBOX like every guest program of ours
# (guest-tools/src/guestlog.h); BOXLOG puts it on the floppy instead, which
# is the medium this script can read back out afterwards.
{
  echo '@echo off'
  echo 'set BOXLOG=A:\'
  echo 'echo CDAUDIOSTART > COM1'
  echo 'A:'
  echo 'cd \'
  echo "A:\\CDTEST.EXE ${DRIVE:-D} ${PLAY:-6} > COM1"
  echo 'echo CDAUDIODONE > COM1'
} > "$OUT/RUN.BAT"
python3 - "$OUT/RUN.BAT" <<'CRLF'
import sys
p = sys.argv[1]
text = open(p, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
open(p, "w", newline="").write(text)
CRLF
rm -f "$FLOPPY"
if command -v mkfs.fat >/dev/null; then
  mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
else
  mformat -C -f 1440 -i "$FLOPPY" ::
fi
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT
mcopy -o -i "$FLOPPY" "$CDTEST" ::/CDTEST.EXE

rm -f "$OVL"; "$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG" "$WAV"
# Win98 runs under TCG by decision, without having to be asked: under
# `-accel kvm` the image loses Explorer at startup (CLAUDE.md), so there is
# no Run dialog to knock on and the run fails as if the drive had.
ACCEL=(-cpu pentium3)
if [ "$FAMILY" != win98 ] && [ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ]; then
  ACCEL=(-accel kvm -cpu pentium3)
fi
if [ "$FAMILY" = win98 ]; then
  HW=(-m 256 -vga cirrus)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  HW=(-m 512 -vga std)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
# The trace is the deliverable, not a debugging aid: it is the only place the
# guest says which command it used to stop the drive.
CDIMAGE_TRACE=1 "$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c -net none \
  -audiodev "wav,id=cd0,path=$WAV" \
  -drive "if=none,id=cd0,media=cdrom,file=$DISC" \
  -device "ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=cd0" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
gw_poke_until "$SOCK" "$FAMILY" "$SHELL_CMD" "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/$FAMILY-noshell.png" || true
  echo "the guest never ran anything: see $OUT/$FAMILY-noshell.png and $QLOG"
}
gw_wait_log "$LOG" CDAUDIODONE "${WAIT_SECS:-180}" || true
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a scripted Win98 run ends with the ACPI power button, never a kill: a
  # modal dialog eats keystrokes and a dirty FAT boots into safe mode next
  # time, which reads exactly like the thing under test having failed.
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 120 || true
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 60 || true
fi
kill -0 $QPID 2>/dev/null && { [ -n "${KEEP:-}" ] || kill $QPID 2>/dev/null || true; }
wait $QPID 2>/dev/null || true

mcopy -n -i "$FLOPPY" ::/cdtest.log "$OUT/cdtest-$FAMILY.log" 2>/dev/null ||
  mcopy -n -i "$FLOPPY" ::/CDTEST.LOG "$OUT/cdtest-$FAMILY.log" 2>/dev/null || true
CD="$OUT/cdtest-$FAMILY.log"
echo "---- cdtest.log ($FAMILY)"
[ -s "$CD" ] && LC_ALL=C tr -d '\r' < "$CD" | sed 's/^/    /' || echo "    (nothing came back)"

# What the guest sent to stop the drive, in its own words. 4e STOP PLAY/SCAN,
# 1b START STOP UNIT (the stop half), 4b PAUSE/RESUME (byte 8 bit 0 = resume).
echo "---- the audio commands this guest used"
LC_ALL=C grep -o "packet ([0-9]*): \(45\|47\|48\|4b\|4e\|1b\|42\) [0-9a-f ]*" "$QLOG" 2>/dev/null |
  sed 's/packet ([0-9]*): //' | awk '
    { op = $1
      name = (op == "45") ? "PLAY AUDIO(10)" : (op == "47") ? "PLAY AUDIO MSF" :
             (op == "48") ? "PLAY AUDIO TRACK/INDEX" : (op == "4b") ? "PAUSE/RESUME" :
             (op == "4e") ? "STOP PLAY/SCAN" : (op == "1b") ? "START STOP UNIT" : "READ SUB-CHANNEL"
      if (op == "4b") name = name ($9 % 2 ? " (resume)" : " (pause)")
      if (op == "1b") name = name ($5 % 2 ? " (start)" : " (stop)")
      if (op != "42") print "    " $0 "   " name }' | uniq -c |
  sed 's/^ *\([0-9]*\) /    x\1 /' || true

fails=0
want() { if grep -qF "$1" "$CD" 2>/dev/null; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi; }
echo "----"
grep -qF CDAUDIODONE "$LOG" 2>/dev/null && echo "PASS  the batch ran to the end" ||
  { echo "FAIL  the batch ran to the end"; fails=$((fails + 1)); }
want '"playing"' "MCI saw the drive playing"
# The one this tool exists for. mcicda's Stop is not the same command on the
# two families, and the mode query right after it is where a drive that
# ignored the one it was sent says so.
stopmode=$(LC_ALL=C tr -d '\r' < "$CD" 2>/dev/null | grep -A1 '"stop cd"' | grep -m1 'status cd mode' || true)
if [ -z "$stopmode" ]; then
  echo "FAIL  MCI believes the drive stopped (CDTEST never reached its stop step)"; fails=$((fails + 1))
elif printf '%s' "$stopmode" | grep -q '"playing"'; then
  echo "FAIL  MCI believes the drive played on after it stopped it ($stopmode)"; fails=$((fails + 1))
else
  echo "PASS  MCI believes the drive stopped ($stopmode)"
fi

# ... and now the two that can see through it. mcicda answers `status mode`
# from the state it *commanded*, so on 9x it said "stopped" for a year while
# the drive played the disc out. Ask the drive and the speaker instead.
last=$(grep -o "reply (16): 00 .." "$QLOG" 2>/dev/null | tail -1 | awk '{print $4}')
case "${last:-none}" in
  11) echo "FAIL  the drive was still playing when the guest was done (last audio status 0x11)"
      fails=$((fails + 1)) ;;
  none) echo "FAIL  the guest never asked the drive for an audio status" ; fails=$((fails + 1)) ;;
  *)  echo "PASS  the drive was not playing when the guest was done (last audio status 0x$last)" ;;
esac
# The speaker: everything the drive actually put out is in the wav, so a
# drive that ignored the stop has minutes of audio in it for a few seconds
# of play. 44100 Hz, 16-bit stereo, 44 bytes of header.
if [ -s "$WAV" ]; then
  secs=$(( ($(stat -c %s "$WAV") - 44) / (44100 * 4) ))
  if [ "$secs" -gt $(( ${PLAY:-6} + 10 )) ]; then
    echo "FAIL  the drive put out ${secs}s of audio for a ${PLAY:-6}s play: it did not stop when it was told"
    fails=$((fails + 1))
  else
    echo "PASS  the drive put out ${secs}s of audio for a ${PLAY:-6}s play ($WAV)"
  fi
else
  echo "FAIL  no audio reached the audiodev at all"; fails=$((fails + 1))
fi
if [ "$fails" = 0 ]; then
  echo "cd audio guest test ($FAMILY): PASS"
else
  echo "cd audio guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png, $CD and $QLOG"
  exit 1
fi
