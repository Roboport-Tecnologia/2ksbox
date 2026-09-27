#!/usr/bin/env bash
# SETUP.EXE from the guest-tools ISO, in a real Windows guest, headless. The installer decides what to do from the
# Windows it finds itself on, so the only honest test is to run it on both
# families and look at what ended up on the disk:
#
#   tools/setup-guest-test.sh ~/vms/winxp.qcow2  xp
#   tools/setup-guest-test.sh ~/vms/win98.qcow2  win98
#
# `SETUP /LIST` (the component list for this family), `SETUP /ALL` (install
# every one of them), `SETUP /GAME 3` and `/GAME 2 C:\2KSBOX` (the per-game
# file sets) and `/GAME 1` (retired: it must say so and copy nothing),
# then Windows' own `dir` on each thing that should now exist. That, and
# the MAPMEM service on NT, are the proof, not SETUP's own exit code.
# Output comes back over COM1; PASS/FAIL per check at the end.
#
# The XP machine boots on the paravirtual adapter (-vga none -device
# d3dpt-vga) because its display-driver component binds to the device as it
# installs; Win98's stages four files for PnP to pick up on the next boot
# and so needs nothing (doc 19 §16). It stays on doc 06's cirrus machine,
# where SETUP must still offer that component and land its four files.
#
# Needs a guest image, so it is run by hand and never from scripts/test.sh.
# The image is never written: everything goes to a qcow2 overlay under
# build/setup-test.
#
# Nothing here sleeps out a boot: the run waits for the guest to say it is
# there (tools/guestwait.sh) and BOOT_WAIT / WARMUP_WAIT are only the caps
# on that wait.
#
# On Win98 `SETUP /ALL` runs twice. The second time every display-driver
# file is already there, and SETUP must not overwrite a file of a driver
# Windows may be drawing with (a reinstall over the running driver
# blue-screened at the restart prompt). It stages each one beside
# its target and schedules the swap in WININIT.INI's [rename] section, so
# the run `type`s that file and `dir`s the staged copies. On cirrus the
# driver is not running, but the mechanism is the same.
#
# REBOOT=1 runs the other half of the installer instead: `SETUP /ALL`, then
# `SETUP /ALL /REBOOT`, and the proof is a second SeaBIOS banner on the
# debugcon, which means the machine really reset. It is its own mode because the
# restart lands in the middle of the `dir`s the normal run ends with, and
# because the 9x restart is a thing that has silently not worked (see
# below). On Win98 a second batch runs after the restart and asks Windows
# whether WININIT applied the swap: the staged copies must be gone and
# WININIT.BAK (what WININIT renames the INI to once it has run it) must
# name the driver files.
#
# VOODOO=1 puts the emulated Voodoo 2 on the machine (`-device voodoo2`,
# doc 21), whose driver is 3dfx's own and brings its Glide and, on 9x, a
# device mapper under the name ours has. With a 3dfx card present SETUP
# must leave those alone: before the install the batch writes a marker
# file under each of them (GLIDE2X.DLL and GLIDE3X.DLL in the system
# folder; on 9x FXMEMMAP.VXD and WINDOWS\GLIDE2X.OVL too) standing in for
# 3dfx's copies, and every marker must still be there after `/ALL`. No
# 3dfx driver is needed: Windows lists the card's devnode without one,
# which is what SETUP asks. On Win98 the card gets a null driver (an INF
# of ours, put into a raw copy of the overlay before the first boot), or
# its new-hardware wizard holds the boot before the shell. On Win98 the
# batch also plants 3dfx's login helper entry (`Voodoo2` in HKLM's Run key,
# regedit /s from the floppy) before the install, and after it exports both
# keys: the entry must have moved to HKLM\SOFTWARE\2ksbox\Voodoo2 and
# V2START.EXE taken its place (doc 21 §11), and the second `/ALL` must find
# it moved already.
#
# Env: OUT=dir (default build/setup-test), BOOT_WAIT=s (cap, 300),
# WARMUP_WAIT=s (cap, 300), NO_WARMUP=1, NO_KVM=1, FORCE_KVM=1 (Win98 under
# KVM), REBOOT=1, VOODOO=1, NO_NET=1 (Win98: no network card), NO_USB=1
# (no USB tablet), KEEP=1. A launcher machine's own image wants the knobs
# its bundle implies: base98-br is `NO_NET=1 NO_USB=1`.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; FAMILY="${2:-xp}"
OUT="${OUT:-$ROOT/build/setup-test}"; mkdir -p "$OUT"
OVL="$OUT/overlay-$FAMILY.qcow2"
FLOPPY="$OUT/run-$FAMILY.img"
LOG="$OUT/serial-$FAMILY.log"
QLOG="$OUT/qemu-$FAMILY.log"
# an AF_UNIX path is capped at 108 bytes, and a build/ path under a deep
# checkout is already close: keep the socket short and outside the tree
SOCK="${SOCK:-/tmp/2ks-setup-$FAMILY.sock}"
QEMU="$ROOT/build/qemu/qemu-system-i386"
ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }
[ -f "$ISO" ] || { echo "no guest-tools ISO: run guest-tools/build-wrappers.sh"; exit 1; }

# The CD's drive letter differs per image, and neither COMMAND.COM nor a
# fresh CMD.EXE can be relied on to set a variable inside a FOR. So every
# command is written once per candidate letter behind `if exist`, which
# both shells do the same thing with.
setup_line() {  # $1 = arguments to SETUP.EXE
  local d
  for d in D E F G; do printf 'if exist %s:\\SETUP.EXE %s:\\SETUP.EXE %s > COM1\n' "$d" "$d" "$1"; done
}
if [ -n "${REBOOT:-}" ]; then
  # /REBOOT only: SETUP returns as soon as it has handed the restart off, so
  # the batch runs to SETUPDONE and the machine goes down a few seconds
  # later. Anything after this line would run into the restart.
  {
    echo '@echo off'
    echo 'echo ==== install > COM1'
    setup_line '/ALL'
    echo 'echo ==== install again and restart > COM1'
    setup_line '/ALL /REBOOT'
    echo 'echo SETUPDONE > COM1'
  } > "$OUT/RUN.BAT"
  # after the restart (Win98): what WININIT did with the staged files
  {
    echo '@echo off'
    echo 'echo ==== after the restart > COM1'
    echo 'type %windir%\WININIT.BAK > COM1'
    # `if exist` rather than `dir`: a listing's format is not worth parsing,
    # and WININIT.BAK names the staged files too
    echo 'if exist %windir%\SYSTEM\D3DPT9X.DR_ echo STAGELEFT SYSTEM\D3DPT9X.DR_ > COM1'
    echo 'if not exist %windir%\SYSTEM\D3DPT9X.DR_ echo SWAPPED SYSTEM\D3DPT9X.DR_ > COM1'
    echo 'if exist %windir%\SYSTEM\D3DPT9V.VX_ echo STAGELEFT SYSTEM\D3DPT9V.VX_ > COM1'
    echo 'if not exist %windir%\SYSTEM\D3DPT9V.VX_ echo SWAPPED SYSTEM\D3DPT9V.VX_ > COM1'
    echo 'if exist %windir%\SYSTEM\D3DPT9HL.DL_ echo STAGELEFT SYSTEM\D3DPT9HL.DL_ > COM1'
    echo 'if not exist %windir%\SYSTEM\D3DPT9HL.DL_ echo SWAPPED SYSTEM\D3DPT9HL.DL_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9X.IN_ echo STAGELEFT INF\D3DPT9X.IN_ > COM1'
    echo 'if not exist %windir%\INF\D3DPT9X.IN_ echo SWAPPED INF\D3DPT9X.IN_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9X.DR_ echo STAGELEFT INF\D3DPT9X.DR_ > COM1'
    echo 'if not exist %windir%\INF\D3DPT9X.DR_ echo SWAPPED INF\D3DPT9X.DR_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9V.VX_ echo STAGELEFT INF\D3DPT9V.VX_ > COM1'
    echo 'if not exist %windir%\INF\D3DPT9V.VX_ echo SWAPPED INF\D3DPT9V.VX_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9HL.DL_ echo STAGELEFT INF\D3DPT9HL.DL_ > COM1'
    echo 'if not exist %windir%\INF\D3DPT9HL.DL_ echo SWAPPED INF\D3DPT9HL.DL_ > COM1'
    echo 'echo AFTERREBOOT > COM1'
  } > "$OUT/RUN2.BAT"
else
if [ "$FAMILY" = win98 ]; then SYSDIR='%windir%\SYSTEM'; else SYSDIR='%windir%\system32'; fi
# name|marker: what 3dfx's driver would have put there, as far as SETUP can tell
MARKS=("$SYSDIR\\GLIDE2X.DLL|MARK-GLIDE2X" "$SYSDIR\\GLIDE3X.DLL|MARK-GLIDE3X")
[ "$FAMILY" = win98 ] && MARKS+=("$SYSDIR\\FXMEMMAP.VXD|MARK-FXMEMMAP" '%windir%\GLIDE2X.OVL|MARK-OVL')
{
  echo '@echo off'
  if [ -n "${VOODOO:-}" ]; then
    echo "echo ==== 3dfx's files, as markers > COM1"
    for m in "${MARKS[@]}"; do echo "echo ${m#*|}> ${m%%|*}"; done
    if [ "$FAMILY" = win98 ]; then
      echo "echo ==== 3dfx's login helper entry > COM1"
      echo 'regedit /s A:\V2RUN.REG'
    fi
  fi
  echo 'echo ==== list > COM1'
  setup_line '/LIST'
  echo 'echo ==== install > COM1'
  setup_line '/ALL'
  if [ "$FAMILY" = win98 ]; then
    echo 'echo ==== install again, over the installed driver > COM1'
    setup_line '/ALL'
    echo 'echo ==== what SETUP scheduled for the restart > COM1'
    echo 'type %windir%\WININIT.INI > COM1'
    echo 'if exist %windir%\SYSTEM\D3DPT9X.DR_ echo STAGED SYSTEM\D3DPT9X.DR_ > COM1'
    echo 'if exist %windir%\SYSTEM\D3DPT9V.VX_ echo STAGED SYSTEM\D3DPT9V.VX_ > COM1'
    echo 'if exist %windir%\SYSTEM\D3DPT9HL.DL_ echo STAGED SYSTEM\D3DPT9HL.DL_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9X.IN_ echo STAGED INF\D3DPT9X.IN_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9X.DR_ echo STAGED INF\D3DPT9X.DR_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9V.VX_ echo STAGED INF\D3DPT9V.VX_ > COM1'
    echo 'if exist %windir%\INF\D3DPT9HL.DL_ echo STAGED INF\D3DPT9HL.DL_ > COM1'
  fi
  if [ "$FAMILY" = win98 ]; then
    echo 'echo ==== MS-DOS mode (/I 7), twice > COM1'
    setup_line '/I 7'
    setup_line '/I 7'
    echo 'dir C:\2KSBOX\UIDE.SYS > COM1'
    echo 'dir %windir%\DESKTOP\2KSBOX~1.PIF > COM1'
  fi
  echo 'echo ==== per-game set 3 (OpenGL) > COM1'
  setup_line '/GAME 3 C:\2KSBOX'
  echo 'echo ==== per-game set 2 (DirectInput), then the retired set 1 > COM1'
  setup_line '/GAME 2 C:\2KSBOX'
  setup_line '/GAME 1 C:\2KSBOX'
  if [ -n "${VOODOO:-}" ]; then
    # FIND prints a matching line only if the marker is still the file's
    # content; a copy of ours in its place prints just the file's name
    echo "echo ==== are 3dfx's files still there > COM1"
    for m in "${MARKS[@]}"; do echo "find \"${m#*|}\" ${m%%|*} > COM1"; done
    if [ "$FAMILY" = win98 ]; then
      # FIND /C prints "---------- <file>: <count>": the helper's DLL must be
      # named once under our key and no longer in the Run key.
      echo "echo ==== where 3dfx's login helper is now > COM1"
      echo 'regedit /e C:\RUNKEY.REG "HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run"'
      echo 'regedit /e C:\V2KEY.REG "HKEY_LOCAL_MACHINE\SOFTWARE\2ksbox\Voodoo2"'
      echo 'type C:\RUNKEY.REG > COM1'
      echo 'type C:\V2KEY.REG > COM1'
      echo 'find /c "3dfxv2ps" C:\RUNKEY.REG > COM1'
      echo 'find /c "3dfxv2ps" C:\V2KEY.REG > COM1'
      echo 'dir %windir%\V2START.EXE > COM1'
    fi
  fi
  echo 'echo ==== what is on the disk now > COM1'
  echo 'dir %windir%\CDSHELF.EXE > COM1'
  echo 'dir C:\2KSBOX\WGLGEARS.EXE > COM1'
  echo 'dir C:\2KSBOX\OPENGL32.DLL > COM1'
  if [ "$FAMILY" = win98 ]; then
    echo 'dir %windir%\SYSTEM\FXMEMMAP.VXD > COM1'
  else
    echo 'dir %windir%\system32\drivers\FXPTL.SYS > COM1'
    echo 'net start MAPMEM > COM1'
  fi
  echo 'echo SETUPDONE > COM1'
} > "$OUT/RUN.BAT"
fi
# perl, not `sed -i`: BSD sed takes the script as a backup suffix (macOS).
perl -pi -e 's/\r?\n\z/\r\n/' "$OUT/RUN.BAT"
[ -f "$OUT/RUN2.BAT" ] && perl -pi -e 's/\r?\n\z/\r\n/' "$OUT/RUN2.BAT"
rm -f "$FLOPPY"
rm -f "$FLOPPY"
if command -v mkfs.fat >/dev/null; then
  mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
else
  mformat -C -f 1440 -i "$FLOPPY" :: || { echo "need mkfs.fat or mformat"; exit 1; }
fi
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT
if [ -n "${VOODOO:-}" ] && [ "$FAMILY" = win98 ]; then
  # What 3dfx's VOODOO2.INF writes (its [Voodoo2.AddReg]), for SETUP to move.
  printf 'REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SoftWare\\Microsoft\\Windows\\CurrentVersion\\Run]\r\n"Voodoo2"="rundll32.exe 3dfxv2ps.dll,UpdateRegSettings"\r\n\r\n' \
    > "$OUT/V2RUN.REG"
  mcopy -o -i "$FLOPPY" "$OUT/V2RUN.REG" ::/V2RUN.REG
fi
[ -n "${REBOOT:-}" ] && mcopy -o -i "$FLOPPY" "$OUT/RUN2.BAT" ::/RUN2.BAT

rm -f "$OVL"
"$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"
DISK=(-hda "$OVL")
RAWV=
if [ -n "${VOODOO:-}" ] && [ "$FAMILY" = win98 ]; then
  # Win98 meets a card it has no driver for with "Add New Hardware" before
  # the shell starts. There is no taskbar or Run dialog, every knock's keys
  # work the wizard's buttons by their mnemonics, and cancelling it only
  # opens the next one. So the card gets a null driver
  # before the first boot: an INF of ours in WINDOWS\INF, which PnP matches
  # and installs with no clicks. The devnode is as present as ever, which
  # is all SETUP asks. mtools cannot write a qcow2, so the machine boots a
  # raw copy of the overlay with the INF put in it (never the image).
  RAWV="$OUT/voodoo-$FAMILY.raw"
  rm -f "$RAWV"; "$ROOT/build/qemu/qemu-img" convert -O raw "$OVL" "$RAWV"
  OFF=$(python3 -c "
import struct
m=open('$RAWV','rb').read(512)
for i in range(4):
    e=m[446+i*16:446+(i+1)*16]
    if e[4]: print(struct.unpack_from('<I',e,8)[0]*512); break")
  [ -n "$OFF" ] || { echo "no partition in $RAWV"; exit 1; }
  printf '%s\r\n' '[Version]' 'Signature="$CHICAGO$"' 'Class=Unknown' 'Provider=%P%' '' \
    '[Manufacturer]' '%P%=Models' '' '[Models]' '%D%=NoDriver,PCI\VEN_121A&DEV_0002' '' \
    '[NoDriver]' 'AddReg=NoDriver.AddReg' '' '[NoDriver.AddReg]' 'HKR,,NoDriver,,1' '' \
    '[Strings]' 'P="2ksbox tests"' 'D="Voodoo 2 (no driver, tools/setup-guest-test.sh)"' \
    > "$OUT/V2NULL.INF"
  mcopy -i "$RAWV@@$OFF" -o "$OUT/V2NULL.INF" ::/WINDOWS/INF/V2NULL.INF
  DISK=(-drive "file=$RAWV,format=raw,if=ide,index=0,media=disk")
fi

# Win98 re-detects its hardware on the first boot of a fresh overlay (the
# device set is not the one the image was last shut down with), and that
# boot regularly takes Explorer down with it: no taskbar, so no Start
# menu, so no Run dialog and no way in. So burn one boot first and shut it
# down over ACPI, which needs no shell; the second boot comes up settled.
warmup() {
  local pid
  "$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine "$MACHINE" "${HW[@]}" \
    "${DISK[@]}" -boot c -display none -qmp "unix:$SOCK,server,nowait" -monitor none \
    > "$OUT/warmup-$FAMILY.log" 2>&1 &
  pid=$!
  # The shell is the one thing this boot cannot be asked about (Explorer
  # dying is what it exists to absorb), so it waits on the disks instead.
  # The re-detection is disk-heavy from end to end, and when the
  # reads stop it is over, one way or the other (tools/guestwait.sh).
  GW_PID=$pid
  gw_wait_sock "$SOCK" && gw_wait_quiet "$SOCK" "${WARMUP_WAIT:-300}" 10 || true
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/$FAMILY-warmup.png" >/dev/null 2>&1 || true
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
  gw_wait_exit "$pid" 120 || true
  GW_PID=
  kill $pid 2>/dev/null || true
  wait $pid 2>/dev/null || true
  rm -f "$SOCK"
}

# Win98 is an emulated guest by decision (doc 06, and the launcher defaults
# its family to TCG): under KVM this image's Explorer dies at startup with
# "SHELL32.DLL is linked to missing export SHLWAPI.DLL:GetFileAttributesA"
# and there is then no Start menu to type into. XP uses KVM when there is a
# /dev/kvm. FORCE_KVM=1 runs Win98 under KVM anyway, to check that again.
ACCEL=(-cpu pentium3)
# Win98 as the launcher builds it: no HPET, which 98 has no driver for.
# An image installed that way meets one here as an "Unknown Device" and a
# new-hardware wizard that holds the boot before the shell (the
# launcher's `hpet` check).
MACHINE=pc
[ "$FAMILY" = win98 ] && MACHINE=pc,hpet=off
# NO_USB=1: an image that has never had a USB controller (a launcher machine
# with `seamless_mouse = false`); 98 wants its setup disc for one. Nothing
# here needs the pointer: the guest is driven by keyboard.
TABLET=(-usb -device usb-tablet)
[ -n "${NO_USB:-}" ] && TABLET=()
if [ "$FAMILY" != win98 ] || [ -n "${FORCE_KVM:-}" ]; then
  [ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
fi
if [ "$FAMILY" = win98 ]; then
  # doc 06's Win98 machine (the warm-up boot above absorbs the hardware
  # re-detection this device set triggers on a fresh overlay)
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0
      -audiodev none,id=a0 -device sb16,audiodev=a0)
  # NO_NET=1: an image installed with no network card (a launcher machine
  # with `network = false`, which the launcher boots `-nic none`: without
  # that QEMU adds a NIC of its own). Given one, its first boot installs
  # networking whose files are not on the disk: "vnetsup.vxd ... press any
  # key" in text mode, or a new-hardware wizard, before any shell.
  [ -n "${NO_NET:-}" ] && HW=(-m 256 -vga cirrus -nic none -audiodev none,id=a0 -device sb16,audiodev=a0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  # the adapter the display-driver component installs a driver for
  HW=(-m 512 -vga none -device d3dpt-vga -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
# the launcher's slot for the card (bundle.rs), after the sound card's
[ -n "${VOODOO:-}" ] && HW+=(-device voodoo2,addr=0x05)
if [ "$FAMILY" = win98 ] && [ -z "${NO_WARMUP:-}" ]; then warmup; fi

SEA="$OUT/seabios-$FAMILY.log"
DBG=()
if [ -n "${REBOOT:-}" ]; then
  # a screendump proves nothing about a reset (CLAUDE.md): the firmware's
  # own banner does
  rm -f "$SEA"
  DBG=(-chardev "file,path=$SEA,id=sea" -device isa-debugcon,iobase=0x402,chardev=sea)
fi
"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine "$MACHINE" "${HW[@]}" \
  "${DISK[@]}" -fda "$FLOPPY" -boot c -cdrom "$ISO" "${DBG[@]}" \
  "${TABLET[@]}" -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
banners() { grep -c '^SeaBIOS (version' "$SEA" 2>/dev/null || echo 0; }
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# Typing into the Run dialog is the only way in and it can miss (the shell
# may still be starting, or a message box may be in front of it), so keep
# knocking until the guest's own output turns up on COM1. That, and not a
# sleep, is what says the shell is there.
# before the install, not after: SETUP returns as soon as it has handed the
# restart off, so by the time SETUPDONE is on the wire the POST may already
# have been written and the count would never be seen to rise
base=$(banners)
gw_poke_until "$SOCK" "$FAMILY" "$SHELL_CMD" "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/$FAMILY-noshell.png" || true
  echo "the guest never ran anything: see $OUT/$FAMILY-noshell.png and $QLOG"
}
gw_wait_log "$LOG" SETUPDONE "${WAIT_SECS:-240}" || true
if [ -n "${REBOOT:-}" ]; then
  # SETUP hands the restart off and returns, so SETUPDONE comes first and
  # the POST follows; measured at ~20 s on the Air under TCG
  reset_seen=0
  for _ in $(seq 1 "${RESET_WAIT:-24}"); do
    [ "$(banners)" -gt "$base" ] && { reset_seen=1; break; }
    kill -0 $QPID 2>/dev/null || break
    sleep 5
  done
  # and it must come back up, not sit in ScanDisk or safe mode
  [ "$reset_seen" = 1 ] && gw_wait_quiet "$SOCK" "${WARMUP_WAIT:-300}" 10 || true
  # and then say what the boot did with the files SETUP staged
  if [ "$reset_seen" = 1 ] && [ "$FAMILY" = win98 ]; then
    gw_poke_until "$SOCK" "$FAMILY" 'command /c A:\RUN2.BAT' "${BOOT_WAIT:-300}" \
      grep -q AFTERREBOOT "$LOG" || true
  fi
fi
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with the ACPI power button, never a kill (CLAUDE.md);
  # the Start menu's keys were the English menu's (`u`), which a Portuguese
  # 98 ignores, and the run was killed after 90 s
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 90 || true
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 60 || true
fi
kill -0 $QPID 2>/dev/null && [ -z "${KEEP:-}" ] && kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
# the raw copy is gigabytes; KEEP=1 keeps it to look at
[ -n "$RAWV" ] && [ -z "${KEEP:-}" ] && rm -f "$RAWV" || true

echo "---- $LOG"
tr -d '\r' < "$LOG" || true
fails=0
want() {  # a line that must be in the output
  if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi
}
never() {  # a line that must NOT be there
  if grep -qF "$1" "$LOG"; then echo "FAIL  $2 (present: $1)"; fails=$((fails + 1)); else echo "PASS  $2"; fi
}
echo "----"
want "SETUPDONE" "the batch ran to the end"
want "2ksbox guest tools" "SETUP started"
want "Installed." "the install finished without errors"
if [ -n "${REBOOT:-}" ]; then
  want "restarting Windows" "SETUP said it was restarting"
  if [ "${reset_seen:-0}" = 1 ]; then
    echo "PASS  the machine really restarted ($base -> $(banners) POSTs)"
  else
    echo "FAIL  the machine never restarted (still $base POSTs, see $QLOG)"
    fails=$((fails + 1))
  fi
  if [ "$FAMILY" = win98 ]; then
    want "D3DPT9X.DRV: installed; the new copy replaces it on restart" "the second install staged the display driver instead of overwriting it"
    want "AFTERREBOOT" "the guest came back up and ran the second batch"
    want "SYSTEM\\D3DPT9X.DRV=" "WININIT ran the [rename] section (it is WININIT.BAK now)"
    never "STAGELEFT" "no staged copy is left beside its target"
    want "SWAPPED SYSTEM\D3DPT9X.DR_" "the restart swapped SYSTEM\D3DPT9X.DR_ into place"
    want "SWAPPED SYSTEM\D3DPT9V.VX_" "the restart swapped SYSTEM\D3DPT9V.VX_ into place"
    want "SWAPPED SYSTEM\D3DPT9HL.DL_" "the restart swapped SYSTEM\D3DPT9HL.DL_ into place"
    want "SWAPPED INF\D3DPT9X.IN_" "the restart swapped INF\D3DPT9X.IN_ into place"
    want "SWAPPED INF\D3DPT9X.DR_" "the restart swapped INF\D3DPT9X.DR_ into place"
    want "SWAPPED INF\D3DPT9V.VX_" "the restart swapped INF\D3DPT9V.VX_ into place"
    want "SWAPPED INF\D3DPT9HL.DL_" "the restart swapped INF\D3DPT9HL.DL_ into place"
  fi
  if [ "$fails" = 0 ]; then echo "setup guest test ($FAMILY, reboot): PASS"; exit 0; fi
  echo "setup guest test ($FAMILY, reboot): FAIL ($fails checks)"; exit 1
fi
if [ -n "${VOODOO:-}" ]; then
  want 'a 3dfx card is on this machine (PCI\VEN_121A&DEV_0002' "SETUP found the Voodoo 2"
  for m in "${MARKS[@]}"; do
    want "${m#*|}" "3dfx's ${m%%|*} (a marker) survived /ALL"
  done
  if [ "$FAMILY" = win98 ]; then
    want "FXMEMMAP.VXD: already there, left alone" "the mapper already there was not downgraded"
    want "3dfx's start-up entry \"rundll32.exe 3dfxv2ps.dll,UpdateRegSettings\" moved to HKLM" \
      "SETUP moved 3dfx's login helper out of the Run key"
    want "3dfx's start-up entry was moved by an earlier install" "the second /ALL found it moved already"
    want 'V2START.EXE -> ' "SETUP copied the Voodoo 2 start-up guard"
    want '"2ksbox Voodoo 2"="C:\\WINDOWS\\V2START.EXE"' "the guard is in the Run key (Windows' own export)"
    want 'RUNKEY.REG: 0' "3dfx's helper is no longer in the Run key (Windows' own export)"
    want 'V2KEY.REG: 1' "3dfx's helper command is kept under HKLM\SOFTWARE\2ksbox\Voodoo2"
  fi
else
  never "GLIDE2X.DLL ->" "SETUP installs no Glide (ADR-020)"
  never "a 3dfx card is on this machine" "no 3dfx card was seen on a machine without one"
  [ "$FAMILY" = win98 ] && want "no 3dfx card on this machine; nothing to do" "no Voodoo 2 guard without a 3dfx card"
fi
want "CDSHELF.EXE ->" "SETUP copied the disc shelf tool"
if [ "$FAMILY" = win98 ]; then
  want "UIDE.SYS -> C:\\2KSBOX" "the MS-DOS mode step copied the CD driver"
  want "2ksbox MS-DOS mode.pif: written" "the MS-DOS mode step wrote the desktop PIF"
  want "2KSBOX~1 PIF" "the PIF is on the desktop (Windows' own dir)"
fi
want "WGLGEARS.EXE" "the test programs are in C:\\2KSBOX (Windows' own dir)"
want "DINPUT.DLL ->" "set 2 copied DINPUT.DLL from DINPUT\\"
want "set 1: Direct3D 8/9 DLLs: retired" "set 1 says it is retired (M16 step 7)"
never "D3D9.DLL ->" "no Direct3D DLL is copied anywhere"
never "  1 Direct3D" "/LIST does not offer the retired set"
want "OPENGL32.DLL" "the per-game set landed in C:\\2KSBOX (Windows' own dir)"
if [ "$FAMILY" = win98 ]; then
  want "Windows 98" "the family was detected"
  [ -z "${VOODOO:-}" ] && want "FXMEMMAP.VXD ->" "the 9x device mapper was installed"
  # The 9x display driver is four files dropped where PnP will find them.
  # There is no installer to run and nothing to bind to until the next boot,
  # which is why this checks the copies and not the adapter. That the driver
  # then comes up is tools/win98-driver-test.sh's job, on a machine that has
  # the device; this one is doc 06's Win98 reference machine and keeps its
  # cirrus.
  want "D3DPT9X.INF ->" "the 9x display driver's INF is in WINDOWS\\INF"
  want "D3DPT9X.DRV ->" "the 9x display driver was staged"
  want "D3DPT9V.VXD ->" "the 9x mini-VDD was staged"
  # Named by the INF's CopyFiles: without it PnP stops and asks for the
  # disc.
  want "D3DPT9HL.DLL ->" "the 9x DirectDraw HAL was staged"
  # The second install: nothing overwritten in place, every file scheduled
  # for the restart in WININIT.INI, the staged copies beside the targets.
  want "D3DPT9X.DRV: installed; the new copy replaces it on restart" "a reinstall stages the display driver rather than overwriting it"
  want "D3DPT9V.VXD: installed; the new copy replaces it on restart" "a reinstall stages the mini-VDD rather than overwriting it"
  want "D3DPT9HL.DLL: installed; the new copy replaces it on restart" "a reinstall stages the DirectDraw HAL rather than overwriting it"
  want "D3DPT9X.INF: installed; the new copy replaces it on restart" "a reinstall stages the INF rather than overwriting it"
  want "[rename]" "WININIT.INI has a [rename] section"
  want "SYSTEM\\D3DPT9X.DRV=" "WININIT.INI renames the staged display driver into place"
  want "SYSTEM\\D3DPT9HL.DLL=" "WININIT.INI renames the staged HAL into place"
  want "STAGED SYSTEM\D3DPT9X.DR_" "the staged copy SYSTEM\D3DPT9X.DR_ is on the disk beside its target"
  want "STAGED SYSTEM\D3DPT9V.VX_" "the staged copy SYSTEM\D3DPT9V.VX_ is on the disk beside its target"
  want "STAGED SYSTEM\D3DPT9HL.DL_" "the staged copy SYSTEM\D3DPT9HL.DL_ is on the disk beside its target"
  want "STAGED INF\D3DPT9X.IN_" "the staged copy INF\D3DPT9X.IN_ is on the disk beside its target"
  want "STAGED INF\D3DPT9X.DR_" "the staged copy INF\D3DPT9X.DR_ is on the disk beside its target"
  want "STAGED INF\D3DPT9V.VX_" "the staged copy INF\D3DPT9V.VX_ is on the disk beside its target"
  want "STAGED INF\D3DPT9HL.DL_" "the staged copy INF\D3DPT9HL.DL_ is on the disk beside its target"
  # The SB16's wave names (doc 20 §5.3): on an image whose names fit (every
  # English one) the component says so and writes nothing; a name that does
  # not fit on a machine with no SB16.VXD devnode is the one way it fails.
  want "Sound Blaster 16 device names:" "SETUP ran the SB16 device-names component"
  never "no device here uses SB16.VXD" "the SB16 device-names component found the driver it renames"
  never "copy failed" "no copy failed"
else
  want "Windows XP" "the family was detected"
  want "drvinst: installed" "the display driver was installed"
  want "FXPTL.SYS ->" "the NT device mapper was installed"
  want "MAPMEM service: running" "the device-mapper service is running"
  grep -q "d3dptvid: adapter found" "$QLOG" && echo "PASS  the miniport came up (QEMU log)" \
    || { echo "FAIL  the miniport never logged (see $QLOG)"; fails=$((fails + 1)); }
fi
if [ "$fails" = 0 ]; then
  echo "setup guest test ($FAMILY): PASS"
else
  echo "setup guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
