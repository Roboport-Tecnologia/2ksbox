#!/usr/bin/env bash
# CDSHELF.EXE against a real Windows guest (doc 07's
# disc shelf, patch 52, protocol cdshelf/cdshelf_proto.h). The DOS build is
# covered by tools/atapi-guest-test.py, which needs no guest image; this is
# the Windows half, and it needs one, so it is run by hand rather than from
# scripts/test.sh.
#
#   tools/cdshelf-guest-test.sh ~/vms/winxp.qcow2 xp
#   tools/cdshelf-guest-test.sh ~/vms/win98.qcow2 win98
#
# The machine boots with the test ISO in the drive, the way a bundle with a
# boot disc starts, and a shelf of two discs: a generated
# ISO with two files on it, and a path that does not exist. The guest then
# lists the shelf, loads the ISO, reads its files through Windows' own file
# system driver (`dir` and `type`, the proof the tray really changed, not
# a status byte), refuses the missing one, ejects, and lists
# again. Output comes back over COM1; PASS/FAIL per check at the end.
#
# The image is never written: everything goes to a qcow2 overlay under
# build/cdshelf-test. CDSHELF.EXE rides in on a floppy so its drive is A: on
# both families, and the guest is driven through the Run dialog over QMP.
#
# Nothing here sleeps out the boot: the run knocks on the Run dialog
# until the guest answers over COM1 (tools/guestwait.sh), and BOOT_WAIT
# is only the cap on that wait.
#
# Env: OUT=dir (default build/cdshelf-test), BOOT_WAIT=s (cap, 300),
# NO_KVM=1, KEEP=1 (leave the VM running on failure).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; FAMILY="${2:-xp}"
OUT="${OUT:-$ROOT/build/cdshelf-test}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"
FLOPPY="$OUT/tools.img"
SHELF="$OUT/shelf.txt"
ISO="$OUT/shelfdisc.iso"
LOG="$OUT/serial-$FAMILY.log"
QLOG="$OUT/qemu-$FAMILY.log"
SOCK="$OUT/q.sock"
QEMU="$ROOT/build/qemu/qemu-system-i386"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }

# the disc the guest will load, with two files it can read back
rm -rf "$OUT/isoroot" && mkdir -p "$OUT/isoroot"
printf 'the disc shelf works\r\n' > "$OUT/isoroot/HELLO.TXT"
printf 'second file\r\n' > "$OUT/isoroot/DISC2.TXT"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V SHELFTEST -J -r "$OUT/isoroot" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V SHELFTEST -J -r "$OUT/isoroot" >/dev/null 2>&1
else
  echo "need xorriso or genisoimage"; exit 1
fi
# ...and a *folder*, which is a disc too: `isodir:` makes the drive
# generate an ISO 9660 + Joliet volume over the tree as the guest reads it
# (M5g). This is the prefix the launcher writes into the shelf file
# (`disc_library::qemu_medium`), so loading slot 2 is the whole path: the
# shelf line, patch 52's medium change, and Windows' own file system
# driver reading what libdisc generated.
rm -rf "$OUT/folderdisc" && mkdir -p "$OUT/folderdisc/Patch Notes"
printf 'a folder is a disc\r\n' > "$OUT/folderdisc/FOLDER.TXT"
printf 'long names survive\r\n' > "$OUT/folderdisc/Patch Notes/Read Me First.txt"
{ printf 'Shelf test disc\t%s\n' "$ISO"
  printf 'Not on the host\t%s\n' "$OUT/no-such-disc.iso"
  printf 'Shared folder\tisodir:%s\n' "$OUT/folderdisc"; } > "$SHELF"
rm -f "$OUT/no-such-disc.iso"

# -mwindows because the program's normal face is a window; its command-line
# verbs (the ones below) still write to a redirected stdout, which is how
# their output gets out of the guest over COM1.
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mwindows -I"$ROOT/cdshelf" -o "$OUT/CDSHELF.EXE" \
  "$ROOT/guest-tools/src/cdshelf.c"

# One batch for both families: COMMAND.COM (98) and CMD.EXE (XP) both take
# it, and every line's output goes to COM1, which needs no writable disk in
# the guest. The CD-ROM's own letter differs per image, so the disc is read
# from both D: and E: and one of the two prints a "path not found". That
# noise is cheaper than teaching this script every image's letters.
cat > "$OUT/RUN.BAT" <<'BAT'
@echo off
A:
cd \
echo ==== list, the boot disc in the drive > COM1
CDSHELF.EXE list > COM1
echo ==== load 0 > COM1
CDSHELF.EXE 0 > COM1
dir /b D:\ > COM1
dir /b E:\ > COM1
type D:\HELLO.TXT > COM1
type E:\HELLO.TXT > COM1
echo ==== load 1, missing on the host > COM1
CDSHELF.EXE 1 > COM1
echo ==== eject > COM1
CDSHELF.EXE E > COM1
CDSHELF.EXE list > COM1
echo ==== load 2, a host folder > COM1
CDSHELF.EXE 2 > COM1
dir /b D:\ > COM1
dir /b E:\ > COM1
type D:\FOLDER.TXT > COM1
type E:\FOLDER.TXT > COM1
dir /b "D:\Patch Notes" > COM1
dir /b "E:\Patch Notes" > COM1
echo SWAPTEST > COM1
CDSHELF.EXE 0 > COM1
dir /b D:\ > COM1
dir /b E:\ > COM1
type D:\HELLO.TXT > COM1
type E:\HELLO.TXT > COM1
echo CDSHELFDONE > COM1
BAT
# CRLF for DOS, without depending on which sed this is: BSD's -i wants an
# argument and GNU's refuses one.
python3 - "$OUT/RUN.BAT" <<'CRLF'
import sys
p = sys.argv[1]
text = open(p, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
open(p, "w", newline="").write(text)
CRLF
rm -f "$FLOPPY"
# mkfs.fat on the Linux box, mtools where there is none (a Mac has
# neither mkfs.fat nor sfdisk); the same 1.44 MB FAT12 floppy either way.
if command -v mkfs.fat >/dev/null; then
  mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
else
  mformat -C -f 1440 -i "$FLOPPY" :: || { echo "need mkfs.fat or mformat"; exit 1; }
fi
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT
mcopy -o -i "$FLOPPY" "$OUT/CDSHELF.EXE" ::/CDSHELF.EXE

# never write the image itself
rm -f "$OVL"
"$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"

ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
if [ "$FAMILY" = win98 ]; then
  # doc 06's Win98 machine, so Windows does not re-detect hardware on the
  # first boot of the overlay (which can take Explorer down with it)
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0
      -audiodev none,id=a0 -device sb16,audiodev=a0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  HW=(-m 512 -vga std -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c \
  -drive "if=none,id=cd0,media=cdrom,file=$ISO" \
  -device "ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,shelf=$SHELF" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# Typing into the Run dialog is the only way in, and it can miss: the shell
# may still be starting, or (Win98, first boot of a fresh overlay) an
# "illegal operation" box may be in front of it. So keep knocking until the
# guest's own output turns up on COM1. That says the shell is there the
# second it is true, rather than after a sleep long enough for the
# slowest machine anyone has run this on.
gw_poke_until "$SOCK" "$FAMILY" "$SHELL_CMD" "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/$FAMILY-noshell.png" || true
  echo "the guest never ran anything: see $OUT/$FAMILY-noshell.png and $QLOG"
}
gw_wait_log "$LOG" CDSHELFDONE "${WAIT_SECS:-150}" || true
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
  Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
  gw_wait_exit "$QPID" 90 || true
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 60 || true
fi
if kill -0 $QPID 2>/dev/null; then
  [ -n "${KEEP:-}" ] || { kill $QPID 2>/dev/null || true; }
fi
wait $QPID 2>/dev/null || true

echo "---- $LOG"
cat "$LOG" || true
fails=0
want() {  # a line that must be in the output
  if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi
}
after() { sed -n "/$1/,\$p" "$LOG"; }
before() { sed -n "1,/$1/p" "$LOG"; }
want_before() {  # marker, needle, name: only what came before the marker counts
  if before "$1" | grep -qF "$2"; then echo "PASS  $3"; else echo "FAIL  $3 (missing before $1: $2)"; fails=$((fails + 1)); fi
}
want_after() {  # marker, needle, name: only what came after the marker counts
  if after "$1" | grep -qF "$2"; then echo "PASS  $3"; else echo "FAIL  $3 (missing after $1: $2)"; fails=$((fails + 1)); fi
}
unwanted_after() {
  if after "$1" | grep -qF "$2"; then echo "FAIL  $3 (still there after $1: $2)"; fails=$((fails + 1)); else echo "PASS  $3"; fi
}
echo "----"
want "CDSHELFDONE" "the batch ran to the end"
want "Shelf test disc" "the shelf was listed"
# the disc the machine booted with was never LOADed by the guest; the
# listing must still say it is the one in the drive
want_before "==== load 0" "Shelf test disc  [in the drive]" "the boot disc is shown in the drive"
want "[missing on the host]" "the unreachable disc is flagged"
want "loading slot 0: Shelf test disc" "the load names the disc"
want "the disc is in the drive." "the drive reported the new medium"
want "HELLO.TXT" "Windows read the loaded disc's directory"
want "the disc shelf works" "Windows read a file off the loaded disc"
want "the host cannot reach that disc image" "the missing disc was refused"
want "the drive is empty." "the eject was accepted"
want "loading slot 2: Shared folder" "the folder was loaded as a disc"
want "a folder is a disc" "Windows read a file out of the shared folder"
want "Read Me First.txt" "long names survived into the guest (Joliet)"
# The swap a user actually does: load a disc while another is already in
# the drive, with no eject in between. Everything above loads into an
# empty tray or ejects first, which is why this case went untested.
want_after SWAPTEST "the disc shelf works" "a disc swapped into a full drive is the one that gets read"
unwanted_after SWAPTEST "FOLDER.TXT" "the folder that was in the drive is gone after the swap"
if [ "$fails" = 0 ]; then
  echo "cdshelf guest test ($FAMILY): PASS"
else
  echo "cdshelf guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
