#!/usr/bin/env bash
# Drive an XP image through the d3dpt-vga driver tests headlessly
# (doc 15). Boots standalone QEMU (KVM when /dev/kvm exists)
# with the driver ISO and a FAT scratch disk, types commands over QMP, and
# pulls the guest logs out of the scratch disk with mtools.
#
#   tools/xp-driver-test.sh <image.qcow2> install      # DRVINST from the ISO, reboot, desktop on the driver
#   tools/xp-driver-test.sh <image.qcow2> ddtest       # DDTEST 640x480x8 (palette) / x16 / x32 / windowed, logs + BMP
#   tools/xp-driver-test.sh <image.qcow2> modes        # SETMODE 1024x768x32@85, 800x600x16@75, list
#   tools/xp-driver-test.sh <image.qcow2> vesa         # the inbox VGA driver's VESA mode change (no driver installed): SETMODE
#                                                       # 800x600x32 on vga.sys, then the screen must be 800x600 and not black; PASS/FAIL
#   tools/xp-driver-test.sh <image.qcow2> d3d7         # D3D7TEST: the DX7 HAL scene, diffed against the host test's frame
#   tools/xp-driver-test.sh <image.qcow2> d3dgame8     # D3DGAME8 through XP's own d3d8.dll on the DX8 DDI (no wrapper DLL),
#                                                       # its frame diffed against the native d3d9 oracle of scripts/test.sh
#   tools/xp-driver-test.sh <image.qcow2> d3dgame9     # D3DGAME9 the same way through XP's own d3d9.dll (the DX9 DDI, M16)
#   tools/xp-driver-test.sh <image.qcow2> d3dfeat9     # D3DFEAT9 through d3d9.dll: its frame and query / getter lines against
#                                                       # the native run's (build/test/f9-native.bmp, D3DFEAT9.LOG), byte for byte
#   tools/xp-driver-test.sh <image.qcow2> shtest       # SHTEST: vertex / pixel shaders 1.x through d3d8.dll on the DX8 DDI,
#                                                       # every draw read back in the guest; PASS = "0 failed" in shtest.log
#   tools/xp-driver-test.sh <image.qcow2> cktest       # CKTEST: palettized textures + colour keying through the DX7 HAL,
#                                                       # every draw read back in the guest; PASS = "0 failed" in cktest.log
#   tools/xp-driver-test.sh <image.qcow2> cubetest     # CUBETEST: cube textures through d3d8.dll on the DX8 DDI (protocol v11),
#                                                       # every draw read back in the guest; PASS = "0 failed" in cubetest.log
#   tools/xp-driver-test.sh <image.qcow2> gamma        # GAMMATEST: a gamma ramp through d3d8.dll (register set v5),
#                                                       # checked in two screendumps (ramp held, ramp back); PASS/FAIL
#   tools/xp-driver-test.sh <image.qcow2> probe VOLTEST  # one DX8 feature probe (d3d8probe.h: CUBETEST STRMTEST VOLTEST FMTTEST
#                                                       # BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST): PASS, NOT OFFERED (the caps say
#                                                       # the driver has no such feature) or FAIL, from the probe's last line
#   tools/xp-driver-test.sh <image.qcow2> probes       # all eight in one boot, a verdict each
#   tools/xp-driver-test.sh <image.qcow2> ebtest       # EBTEST: the DirectX 3 path (IDirect3D v1, execute buffers, texture
#                                                       # handles, viewport Clear) on the HAL; PASS = "0 failed" in ebtest.log
#   tools/xp-driver-test.sh <image.qcow2> caps         # DX8CAPS + DX9CAPS: what d3d8.dll / d3d9.dll make of the driver (device
#                                                       # types, depth formats, CreateDevice as Wine's tests call it, the caps)
#   tools/xp-driver-test.sh <image.qcow2> winetest     # Wine's d3d8 / d3d9 conformance tests (M16) through XP's own runtime:
#                                                       # guest-tools/build-winetests.sh first; a table per test file, and with
#                                                       # WT_BASELINE=reference/winetest/<f>.txt a verdict (tools/winetest-summary.py).
#                                                       # WT_TESTS="d3d9:visual" a subset, WT_CAP=s the cap per file (1200),
#                                                       # WT_SAVE=<f> writes the run as a baseline
#   tools/xp-driver-test.sh <image.qcow2> cmd 'D:\DRIVER\SETMODE.EXE'   # any guest command line
#   tools/xp-driver-test.sh <image.qcow2> bat run.bat                   # a batch file, staged as E:\RUN.BAT (long command lines)
#
# Env: QEMU_EXTRA='-audiodev none,id=snd0 -device AC97,audiodev=snd0' (more QEMU arguments: a
# sound card), VGA=cirrus (XP's inbox driver instead of ours: the control for a crash), DDFLAGS=N (-device d3dpt-vga,ddflags=N), FBVER=N (-device d3dpt-vga,fb-version=N: the adapter reports another register set version, so a newer one checks the installed driver accepts a QEMU update), EXEC=wine (-device d3dpt-vga,exec=wine: the executor in another process on Wine's own d3d9, ADR-018 / M15. It is what a Linux or macOS host below the floor runs, and on a host with Vulkan the A/B; D3DPT_WINE names the wine binary), NO_EXEC=1 (-device d3dpt-vga,no-exec=on: the adapter reports no Direct3D executor, as a host with none does, and the run shows what such a user gets: the DirectDraw half alone, no HAL, the runtime's software device), OUT=dir for screendumps
# and logs (default build/xp-driver-test), NO_KVM=1, CPU=pentium3 (the KVM CPU model), GAME_ISO=game.iso (the
# game disc takes the CD-ROM drive the game was installed from, D:; the
# driver ISO moves to the next drive, F: after the E: scratch), and for `cmd` / `bat`:
# CMD_WAIT=s (the cap on waiting for the command to say it is done over
# COM1, default 300; BOOT_WAIT=s is the cap on finding the shell at all,
# REBOOT_WAIT=s the cap on `install`'s restart; none of the three is a
# sleep, see tools/guestwait.sh, except that `cmd` / `bat` sleep CMD_WAIT
# (30) unless UNTIL=word names what the command echoes to COM1 at its end)
# or SHOTS=n SHOT_EVERY=s (n screendumps
# cmd-01.png … every s seconds, for watching a game start; SHOT_KEYS="26:esc"
# presses a key right before screendump n). KEEP=1 leaves the machine
# running at the end (QMP at OUT/qmp.sock; kill it yourself) for a look
# at a guest that went wrong: `info registers` twice, `info pic`, a
# second Run dialog typing `cmd /c type C:\WINDOWS\setupapi.log > COM1`.
# `install` writes DRVINST's own lines to COM1 (serial-install.log) and a
# screendump every 20 s (install-NN.png) while it waits. Needs
# guest-tools/build-driver.sh run first (guest-tools/out/d3dpt-driver.iso),
# mtools + python3, and mkfs.fat + sfdisk where they exist (the Mac has
# neither: mformat then builds the scratch disk). Ends every run with a clean
# power-down. Keys typed while a full-screen DirectDraw window is up are
# lost, so each test is ONE chained "cmd /k a & b & c" command line.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
if [ "$(uname -s)" = Darwin ]; then
  # scripts/test.sh's macOS run environment: DXVK dlopens the Vulkan loader
  # by leaf name, and a DYLD_* variable handed to this script is stripped by
  # SIP at the #!/usr/bin/env exec. Without it the first Direct3D context
  # takes QEMU down (DXVK calls the loader it never found: SIGSEGV in
  # LibraryFn, "vkGetInstanceProcAddr not found" just before). The loader's
  # own keg only, never all of /opt/homebrew/lib (doc 00's ImageIO gotcha).
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi
IMG="${1:?image.qcow2}"; MODE="${2:?install|ddtest|modes|vesa|d3d7|d3dgame8|d3dgame9|d3dfeat9|shtest|cktest|cubetest|probe|probes|ebtest|gamma|caps|winetest|cmd|bat}"; shift 2
OUT="${OUT:-$ROOT/build/xp-driver-test}"; mkdir -p "$OUT"
# DRIVER_ISO= another build's driver ISO: the A/B against an older driver
# (built from `git archive <sha>` into a scratch tree, never over this one's)
ISO="${DRIVER_ISO:-$ROOT/guest-tools/out/d3dpt-driver.iso}"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  dd if=/dev/null of="$SCRATCH" bs=1 seek=$((64 * 1048576)) 2>/dev/null
  if command -v sfdisk >/dev/null && command -v mkfs.fat >/dev/null; then
    printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
    mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
  else
    # the Mac has neither: the partition table by hand and mtools inside it,
    # tools/xp-cdimage-test.sh's recipe (-H 2048, the hidden-sectors field =
    # the partition's start, or XP does not mount the volume at all)
    python3 - "$SCRATCH" <<'MBR'
import struct, sys
start, total = 2048, 64 * 2048
mbr = bytearray(512)
mbr[0x1be:0x1be + 16] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', start, total - start)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'r+b') as f:
    f.write(bytes(mbr))
MBR
    mformat -i "$SCRATCH@@1048576" -F -H 2048 -T $((64 * 2048 - 2048)) ::
  fi
fi
stage_bat() {  # the Run dialog truncates long lines: stage a batch file on the scratch disk (before the guest mounts it)
  sed 's/\r$//; s/$/\r/' "$1" > "$OUT/RUN.BAT"
  mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
}
[ "$MODE" = bat ] && stage_bat "${1:?batch file}"
if [ "$MODE" = ddtest ]; then
  # every guest program writes its log and BMP to C:\2KSBOX now (guestlog.h),
  # not to whatever directory it was started from, so the copies name it
  printf '%s\n' '@echo off' 'cd /d E:\' 'for %%b in (8 16 32) do (' \
    '  D:\DRIVER\DDTEST.EXE 640 480 %%b 300' '  copy C:\2KSBOX\DDTEST.LOG E:\dd%%b.log > nul' '  copy C:\2KSBOX\DDTEST.BMP E:\dd%%b.bmp > nul' ')' \
    'D:\DRIVER\DDTEST.EXE 640 480 32 200 -windowed' 'copy C:\2KSBOX\DDTEST.LOG E:\ddwin.log > nul' 'echo DDDONE > COM1' > "$OUT/ddtest.bat"
  stage_bat "$OUT/ddtest.bat"
fi
case "$MODE" in                                    # the scene's EXE and its files' tag (G8.BMP, g8.log, G8DONE)
  d3dgame8) GEXE=D3DGAME8; GN=G8 ;; d3dgame9) GEXE=D3DGAME9; GN=G9 ;; d3dfeat9) GEXE=D3DFEAT9; GN=F9 ;; *) GN= ;;
esac
gl=$(echo "$GN" | tr A-Z a-z)
if [ -n "$GN" ]; then
  # the reference scene's DX8 / DX9 build from the guest-tools ISO (TESTS\), copied out alone
  # into a folder of its own: XP's own runtime on our DDI (DX9's since M16)
  FULL_ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1)"
  [ -f "$FULL_ISO" ] || { echo "no guest-tools ISO (TESTS\\$GEXE.EXE): run guest-tools/build-wrappers.sh"; exit 1; }
  ISO="$FULL_ISO"
  printf '%s\n' '@echo off' "mkdir E:\\$GN" "copy D:\\TESTS\\$GEXE.EXE E:\\$GN\\ > nul" "cd /d E:\\$GN" "del C:\\2KSBOX\\$GEXE.LOG 2> nul" \
    "$GEXE.EXE -frames 600 -dump 300 E:\\$GN.BMP" "copy C:\\2KSBOX\\$GEXE.LOG E:\\$gl.log > nul" "echo done > E:\\${GN}DONE.TXT" "echo ${GN}DONE > COM1" > "$OUT/$gl.bat"
  stage_bat "$OUT/$gl.bat"
fi
if [ "$MODE" = winetest ]; then
  # Wine's d3d8 / d3d9 tests (guest-tools/build-winetests.sh) staged on the
  # scratch disk with their runner; each test file runs in its own process
  # under WTRUN's time cap, and the output lands on E:\WT\WINETEST
  WT="${WT_DIR:-$ROOT/build/winetest/out}"
  [ -f "$WT/wtrun.exe" ] || { echo "no $WT/wtrun.exe: run guest-tools/build-winetests.sh"; exit 1; }
  mdel -i "$SCRATCH@@1048576" '::/WT/WINETEST/*' 2>/dev/null || true
  mmd -i "$SCRATCH@@1048576" ::/WT 2>/dev/null || true
  mcopy -o -i "$SCRATCH@@1048576" "$WT/wtrun.exe" "$WT/d3d8_test.exe" "$WT/d3d9_test.exe" ::/WT/
  { printf '%s\n' '@echo off' 'set BOXLOG=E:\WT'
    # WT_TESTS="d3d9:visual d3d8:device": a subset; the default is every file
    # `device` last: its fullscreen tests can leave the desktop without Direct3D (M16 finding 5)
    for t in ${WT_TESTS:-d3d9:visual d3d9:stateblock d3d9:d3d9ex d3d8:visual d3d8:stateblock d3d9:device d3d8:device}; do
      printf 'E:\\WT\\WTRUN.EXE %s E:\\WT\\%s_TEST.EXE %s\n' "${WT_CAP:-1200}" "$(echo "${t%%:*}" | tr a-z A-Z)" "${t#*:}"
    done
    printf '%s\n' 'echo WTALL > COM1'; } > "$OUT/winetest.bat"
  stage_bat "$OUT/winetest.bat"
fi
PROBES="CUBETEST STRMTEST VOLTEST FMTTEST BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST MGDTEST"
if [ "$MODE" = probes ]; then
  # the DX8 feature probes one after the other; each writes C:\2KSBOX\<name>.LOG
  { printf '%s\n' '@echo off'
    for p in $PROBES; do
      printf '%s\n' "D:\\DRIVER\\$p.EXE" \
        "copy C:\\2KSBOX\\$p.LOG E:\\$(echo "$p" | tr 'A-Z' 'a-z').log > nul"
    done
    printf '%s\n' 'echo PRDONE > COM1'; } > "$OUT/probes.bat"
  stage_bat "$OUT/probes.bat"
fi
SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu "${CPU:-host}")   # CPU=pentium3: Max Payne's JPEG decoder mis-decodes on a modern family
LOG="$OUT/qemu-$MODE.log"
# The guest's own line out. The scratch disk cannot be asked anything while
# the guest is running (XP's lazy writer can hold a small FAT write for
# minutes), so every
# command this script types ends by echoing a marker to COM1, and the run
# waits for the marker instead of sleeping for as long as the command has
# ever taken.
SER="$OUT/serial-$MODE.log"; rm -f "$SER"
VGA_ARGS=(-vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}${FBVER:+,fb-version=$FBVER}${NO_EXEC:+,no-exec=on}${EXEC:+,exec=$EXEC}")   # EXEC=wine: the executor in another process on Wine (M15), the A/B on a host that has Vulkan
[ -n "${VGA:-}" ] && VGA_ARGS=(-vga "$VGA")     # VGA=cirrus: the control run on XP's inbox driver (no Direct3D)
CD2=()
CD1="$ISO"
if [ -n "${GAME_ISO:-}" ]; then CD1="$GAME_ISO"; CD2=(-drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on"); fi
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m "${MEM:-512}" \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$CD1" "${CD2[@]}" "${VGA_ARGS[@]}" \
  -net none -usb -device usb-tablet -display none -qmp "unix:$SOCK,server,nowait" \
  -serial "file:$SER" -monitor none ${QEMU_EXTRA:-} > "$LOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
run() {  # one chained guest command line in a console that stays open
  Q keys esc; sleep 1; Q keys meta_l+r; sleep 2
  Q type ' '; Q keys backspace          # the first key after the chord is lost, and Run opens on its last command
  Q type "cmd /k $1"; Q keys ret
}
run_until() {  # <marker> <cap> <command line>: run it, and wait for it to say it is done
  local mark=$1 cap=$2; shift 2
  run "$* & echo $mark > COM1"
  gw_wait_log "$SER" "$mark" "$cap" || true
}
pull() { mcopy -n -i "$SCRATCH@@1048576" "::/$1" "$OUT/$1" 2>/dev/null && echo "-- $1" && cat "$OUT/$1"; }
probe_verdict() {  # <NAME>: a DX8 feature probe's last line as PASS, NOT OFFERED or FAIL (d3d8probe.h); no log is a FAIL
  local l="$OUT/$(echo "$1" | tr A-Z a-z).log"
  rm -f "$l"
  mcopy -n -i "$SCRATCH@@1048576" "::/$(basename "$l")" "$l" 2>/dev/null || true
  if grep -q 'failed (not offered' "$l" 2>/dev/null; then
    echo "-- $1: NOT OFFERED ($(grep -o 'not offered: [^)]*' "$l" | tail -1 | cut -c14-))"
  elif grep -qE ': [1-9][0-9]* cases, 0 failed' "$l" 2>/dev/null; then
    echo "-- $1: PASS ($(grep -oE '[0-9]+ cases' "$l" | tail -1))"
  else
    echo "-- $1: FAIL (see $l and the device log)"
  fi
}
finish() {
  Q screendump "$OUT/$MODE-end.png" || true
  if [ "${KEEP:-0}" = 1 ]; then   # KEEP=1: leave the machine up for a look over QMP ($SOCK); kill it yourself
    echo "KEEP=1: the guest is still running (pid $QPID, QMP $SOCK)"
    echo "---- $LOG (device side)"; grep -v "^WARNING" "$LOG" | sed 's/qemu-system-i386: info: //' | tail -40
    return 0
  fi
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 90 || true
  kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
  echo "---- $LOG (device side)"; grep -v "^WARNING" "$LOG" | sed 's/qemu-system-i386: info: //' | tail -40
}

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# No fixed boot sleep: knock on the Run dialog until the guest runs
# something and says so on COM1 (tools/guestwait.sh). `install` is the mode
# where the driver is not in the image yet, so there is no adapter line to
# wait for either. The knocking works on every mode.
gw_poke_until "$SOCK" xp 'cmd /c ver > COM1 & echo SHELLUP > COM1' "${BOOT_WAIT:-300}" grep -q SHELLUP "$SER" || {
  Q screendump "$OUT/$MODE-noshell.png" || true
  echo "the guest never reached its shell: see $OUT/$MODE-noshell.png and $LOG"
}
# Windows 7 (track M18) runs the same checks. What differs is the install:
# DRVINST needs an administrator, and UAC gives one only to a program
# started so. Win7's Run box has no Ctrl+Shift+Enter (Windows 10 added it),
# its Start menu search box has, and UAC's prompt then takes Alt+Y. The
# unsigned-driver prompt that follows ("Windows can't verify the publisher")
# never gets the keyboard, so it is clicked, on a USB tablet: "Install this
# driver software anyway" sits 18 px right of and 48 px below the screen's
# centre, the prompt being centred.
NT6=0; grep -q 'Version 6\.' "$SER" 2>/dev/null && NT6=1
elevated() {  # an administrator's console, the command typed into it
  Q keys esc; sleep 1; Q keys meta_l; sleep 2; Q type 'cmd'; sleep 2
  Q keys ctrl+shift+ret; sleep 5; Q keys alt+y; sleep 5
  Q type "$1"; Q keys ret
}
click_install_anyway() {  # the unsigned-driver prompt's second choice, clicked until the installer answers
  local k geom w h
  for k in 1 2 3 4 5 6; do
    sleep 10; grep -q DRVDONE "$SER" && return 0
    Q screendump "$OUT/unsigned-$k.png" >/dev/null 2>&1 || true
    geom=$(head -c 20 "$OUT/unsigned-$k.png.ppm" 2>/dev/null | sed -n 2p); w=${geom% *}; h=${geom#* }
    [ -n "$w" ] && Q click $((w / 2 + 18)) $((h / 2 + 48)) "$w" "$h" >/dev/null || true
  done
}
case "$MODE" in
  install)
    # a shorter cap than the rest: if DRVINST ever turns out not to return,
    # the restart below still works, since cmd buffers the typed line until
    # it does.
    # DRVINST's own words go to COM1 (the serial log on the host, written
    # the moment they are printed), because an install that blocks (a
    # dialog, a devnode restart that never comes back) shows a black
    # screendump and nothing else, and its console window dies with the
    # guest. A screendump every 20 s meanwhile (install-NN.png), because the
    # one at the end shows where it ended, not what it went through.
    ( i=0; while sleep 20; do i=$((i+1)); Q screendump "$OUT/install-$(printf %02d "$i").png" >/dev/null 2>&1 || true; done ) & SHOTPID=$!
    if [ "$NT6" = 1 ]; then
      elevated 'D:\DRIVER\DRVINST.EXE > COM1 & echo DRVDONE > COM1'
      click_install_anyway
      gw_wait_log "$SER" DRVDONE "${CMD_WAIT:-300}" || true
    else
      run_until DRVDONE "${CMD_WAIT:-300}" 'D:\DRIVER\DRVINST.EXE > COM1'
    fi
    Q screendump "$OUT/install-done.png"
    # the count to beat: the machine has to program the desktop mode once
    # more, after the restart, and that (not a screendump of a desktop
    # that looks the same either way) is the proof it came back on our
    # driver (nothing at all here on the first install: the count is 0)
    seen=$(grep -c "linear mode on" "$LOG" 2>/dev/null || true)
    want=$(( ${seen:-0} + 1 ))
    if [ "$NT6" = 1 ]; then run 'shutdown -r -t 0'; else Q type 'shutdown -r -t 0'; Q keys ret; fi
    gw_wait_count "$LOG" "linear mode on" "$want" "${REBOOT_WAIT:-300}" || true
    Q screendump "$OUT/install-rebooted.png"
    kill $SHOTPID 2>/dev/null || true
    finish
    echo "---- $SER (the installer's own lines)"; tr -d '\r' < "$SER" | grep -v '^$' | tail -12 ;;
  ddtest)
    run 'E:\RUN.BAT'                                        # 8 / 16 / 32 bpp chains, then windowed (staged above)
    sleep 5; Q screendump "$OUT/ddtest-fullscreen.png"      # the 8 bpp chain: the palette shows in the dump
    gw_wait_log "$SER" DDDONE "${CMD_WAIT:-300}" || true
    finish
    pull dd8.log; pull dd16.log; pull dd32.log; pull ddwin.log
    for b in 8 16 32; do mcopy -n -i "$SCRATCH@@1048576" "::/dd$b.bmp" "$OUT/dd$b.bmp" 2>/dev/null || true; done ;;
  modes)
    run_until MODESDONE "${CMD_WAIT:-180}" 'D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE 800 600 16 75 & D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE > E:\modes.log'
    finish
    pull modes.log ;;
  vesa)
    # The inbox VGA driver (a fresh install, no driver of ours) changes to a
    # VESA mode through the VGA BIOS: XP resets to text mode 3 and sets the
    # mode with int10 4F02h, each an int10 call from V86 mode around a mov
    # cr3. Patch 44's retired TLB table came back across the VGA window's
    # topology flush and left the screen black or in an empty text mode
    # (2026-09-24, the user's fresh basexp-us). The command is typed until
    # the guest confirms it on COM1: a fresh install's Found New Hardware
    # wizard takes the first tries' keys. VGA=std is the control.
    ok=0
    for try in 1 2 3; do
      run "D:\\DRIVER\\SETMODE.EXE 800 600 32 > COM1 & echo VESADONE$try > COM1"
      if gw_wait_log "$SER" "VESADONE$try" 45; then ok=1; break; fi
    done
    sleep 6; Q screendump "$OUT/vesa.png"
    geom=$(head -c 20 "$OUT/vesa.png.ppm" | sed -n 2p)
    lit=$(tail -c +16 "$OUT/vesa.png.ppm" | LC_ALL=C tr -d '\0' | wc -c | tr -d ' ')
    if [ "$ok" = 1 ] && [ "$geom" = "800 600" ] && [ "$lit" -gt 100000 ]; then
      echo "-- vesa: PASS (800x600, $lit non-black bytes)"
    else
      echo "-- vesa: FAIL (confirmed=$ok screen=$geom non-black=$lit; the A/B is QEMU_EXTRA='-accel tcg,tlb-retire=off')"
    fi
    finish ;;
  d3d7)
    run 'D:\DRIVER\D3D7TEST.EXE 640 480 32 300 & copy C:\2KSBOX\D3D7TEST.LOG E:\d3d7.log & copy C:\2KSBOX\D3D7TEST.BMP E:\d3d7.bmp & echo D3D7DONE > COM1'
    sleep 8; Q screendump "$OUT/d3d7-fullscreen.png"
    gw_wait_log "$SER" D3D7DONE "${CMD_WAIT:-300}" || true
    finish
    pull d3d7.log
    mcopy -n -i "$SCRATCH@@1048576" ::/d3d7.bmp "$OUT/d3d7.bmp" 2>/dev/null || true
    # the same scene through the executor without a guest (tools/d3dpt-dp2-test.cpp): the frames must agree
    if [ -f "$OUT/d3d7.bmp" ] && [ -x "$ROOT/build/d3dpt-dp2-test" ]; then
      ( cd "$ROOT" && D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}" build/d3dpt-dp2-test "$OUT/d3d7-host.bmp" >"$OUT/d3d7-host.log" 2>&1 ) || true
      python3 "$ROOT/tools/bmpdiff.py" "$OUT/d3d7-host.bmp" "$OUT/d3d7.bmp" --tolerance 2 --max-over 0 -o "$OUT/d3d7-diff.bmp" && echo "-- d3d7: guest frame == host frame" || echo "-- d3d7: FRAMES DIFFER ($OUT/d3d7-diff.bmp)"
    fi ;;
  d3dgame8|d3dgame9|d3dfeat9)
    run 'E:\RUN.BAT'
    # COM1, not G8DONE.TXT on the scratch disk: the file is written the
    # moment the run ends but XP's lazy writer decides when the host sees it
    gw_wait_log "$SER" "${GN}DONE" "${CMD_WAIT:-300}" || true
    sleep 2; Q screendump "$OUT/$MODE-end.png" || true
    finish
    pull "$gl.log"
    mcopy -n -i "$SCRATCH@@1048576" "::/$GN.BMP" "$OUT/$GN.BMP" 2>/dev/null || true
    if [ "$GN" = F9 ]; then
      # D3DFEAT9's oracle is exact, as scripts/test.sh's guest-F9=native: the
      # native frame (build/test, the host stage) and its query / getter lines
      if [ -f "$OUT/F9.BMP" ] && [ -f "$ROOT/build/test/f9-native.bmp" ]; then
        if cmp -s "$ROOT/build/test/f9-native.bmp" "$OUT/F9.BMP"; then echo "-- d3dfeat9: frame byte-identical to the native frame"
        else python3 "$ROOT/tools/bmpdiff.py" "$ROOT/build/test/f9-native.bmp" "$OUT/F9.BMP" -o "$OUT/f9-diff.bmp" || true
             echo "-- d3dfeat9: FRAME DIFFERS ($OUT/f9-diff.bmp)"; fi
        grep -h "occlusion query\|getters" "$ROOT/build/test/D3DFEAT9.LOG" | sort > "$OUT/f9-native.lines"
        tr -d '\r' < "$OUT/f9.log" | grep "occlusion query\|getters" | sort > "$OUT/f9-guest.lines"
        diff "$OUT/f9-native.lines" "$OUT/f9-guest.lines" && echo "-- d3dfeat9: query / getter lines as native" || echo "-- d3dfeat9: LINES DIFFER"
      else echo "-- d3dfeat9: no frame ($OUT/F9.BMP) or no native oracle (build/test/f9-native.bmp: run scripts/test.sh host)"; fi
    # the native d3d9 frame of the same scene (scripts/test.sh host writes it): the HUD masked, the rig budget
    elif [ -f "$OUT/$GN.BMP" ] && [ -f "$ROOT/build/test/g9-native.bmp" ]; then
      python3 "$ROOT/tools/bmpdiff.py" "$ROOT/build/test/g9-native.bmp" "$OUT/$GN.BMP" --mask 0,368,270,112 --tolerance 8 --max-over 1200 -o "$OUT/$gl-diff.bmp" \
        && echo "-- $MODE: frame within budget of the native d3d9 frame" || echo "-- $MODE: FRAME DIFFERS ($OUT/$gl-diff.bmp)"
    else echo "-- $MODE: no frame ($OUT/$GN.BMP) or no native oracle (build/test/g9-native.bmp: run scripts/test.sh host)"; fi ;;
  shtest)
    run 'D:\DRIVER\SHTEST.EXE & copy C:\2KSBOX\SHTEST.LOG E:\shtest.log & echo SHDONE > COM1'
    sleep 8; Q screendump "$OUT/shtest-window.png" || true
    gw_wait_log "$SER" SHDONE "${CMD_WAIT:-300}" || true
    finish
    pull shtest.log
    if grep -q 'shtest: [1-9][0-9]* cases, 0 failed' "$OUT/shtest.log" 2>/dev/null; then echo "-- shtest: PASS"; else echo "-- shtest: FAIL (see $OUT/shtest.log and the device log)"; fi ;;
  cubetest|probe)
    p=CUBETEST; [ "$MODE" = probe ] && p="$(echo "${1:?probe name, e.g. VOLTEST}" | tr a-z A-Z)"
    lp="$(echo "$p" | tr A-Z a-z)"
    run_until PRDONE "${CMD_WAIT:-300}" "D:\\DRIVER\\$p.EXE & copy C:\\2KSBOX\\$p.LOG E:\\$lp.log"
    finish
    pull "$lp.log" || true
    probe_verdict "$p" ;;
  probes)
    run 'E:\RUN.BAT'                                        # the eight probes (staged above)
    gw_wait_log "$SER" PRDONE "${CMD_WAIT:-900}" || true
    finish
    for p in $PROBES; do probe_verdict "$p"; done ;;
  gamma)
    # GAMMATEST: a gamma ramp through d3d8.dll's SetGammaRamp, which the
    # adapter applies where it makes the picture (register set v5). So the
    # evidence is the screen, not the probe: a screendump while each ramp is
    # held (the adapter says `gamma ramp on` / `off` when it takes one), and
    # the centre pixel of each, mid grey with blue at 3/4 and then unchanged
    run "D:\\DRIVER\\GAMMATEST.EXE & copy C:\\2KSBOX\\GAMMATEST.LOG E:\\gammatest.log & echo GMDONE > COM1"
    if gw_wait_log "$LOG" "gamma ramp on" "${CMD_WAIT:-300}"; then Q screendump "$OUT/gamma-on.png" || true; fi
    if gw_wait_log "$LOG" "gamma ramp off" "${CMD_WAIT:-300}"; then Q screendump "$OUT/gamma-off.png" || true; fi
    gw_wait_log "$SER" GMDONE "${CMD_WAIT:-300}" || true
    finish
    pull gammatest.log || true
    grep "gamma" "$LOG" | sed 's/^/   /' | head -8
    python3 - "$OUT/gamma-on.png.ppm" "$OUT/gamma-off.png.ppm" "$OUT/gammatest.log" <<'PY'
import sys
def centre(path):
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    o = ((h // 2) * w + w // 2) * 3
    return tuple(parts[3][o:o + 3])
on, off = centre(sys.argv[1]), centre(sys.argv[2])
grey = lambda v: 0x78 <= v <= 0x8c
ok_on = on is not None and grey(on[0]) and grey(on[1]) and 0x56 <= on[2] <= 0x6c
ok_off = off is not None and grey(off[0]) and grey(off[1]) and grey(off[2])
try:
    ok_log = any(l.startswith("gammatest:") and "cases, 0 failed" in l for l in open(sys.argv[3]))
except OSError:
    ok_log = False
print("   ramp held: centre %s (want ~80 80 60), ramp back: %s (want ~80 80 80), gammatest.log %s" %
      (on and "%02x %02x %02x" % on, off and "%02x %02x %02x" % off, "0 failed" if ok_log else "missing or failed"))
print("-- gamma: " + ("PASS" if ok_on and ok_off and ok_log else "FAIL (see gamma-on.png / gamma-off.png and the device log)"))
PY
    ;;
  cktest)
    run 'D:\DRIVER\CKTEST.EXE & copy C:\2KSBOX\CKTEST.LOG E:\cktest.log & copy C:\2KSBOX\CK*.BMP E:\ & echo CKDONE > COM1'
    sleep 8; Q screendump "$OUT/cktest-fullscreen.png" || true
    gw_wait_log "$SER" CKDONE "${CMD_WAIT:-300}" || true
    finish
    pull cktest.log
    for n in 1 2 3 4 5 6; do mcopy -n -i "$SCRATCH@@1048576" "::/CK$n.BMP" "$OUT/ck$n.bmp" 2>/dev/null || true; done
    if grep -q 'cktest: [1-9][0-9]* cases, 0 failed' "$OUT/cktest.log" 2>/dev/null; then echo "-- cktest: PASS"; else echo "-- cktest: FAIL (see $OUT/cktest.log and the device log)"; fi ;;
  ebtest)
    run "D:\\DRIVER\\EBTEST.EXE ${*:-} & copy C:\\2KSBOX\\EBTEST.LOG E:\\ebtest.log & copy C:\\2KSBOX\\EB*.BMP E:\\ & echo EBDONE > COM1"    # extra args: e.g. -rgb, the software-device control
    sleep 8; Q screendump "$OUT/ebtest-fullscreen.png" || true
    gw_wait_log "$SER" EBDONE "${CMD_WAIT:-300}" || true
    finish
    pull ebtest.log
    for n in 1 2 3 4 5 6; do mcopy -n -i "$SCRATCH@@1048576" "::/EB$n.BMP" "$OUT/eb$n.bmp" 2>/dev/null || true; done
    if grep -q 'ebtest: [1-9][0-9]* cases, 0 failed' "$OUT/ebtest.log" 2>/dev/null; then echo "-- ebtest: PASS"; else echo "-- ebtest: FAIL (see $OUT/ebtest.log and the device log)"; fi ;;
  caps)
    run_until CAPSDONE "${CMD_WAIT:-180}" 'D:\DRIVER\DX8CAPS.EXE > nul & D:\DRIVER\DX9CAPS.EXE > nul & copy C:\2KSBOX\DX8CAPS.LOG E:\dx8caps.log & copy C:\2KSBOX\DX9CAPS.LOG E:\dx9caps.log'
    finish
    pull dx8caps.log || true; pull dx9caps.log || true ;;
  winetest)
    run 'E:\RUN.BAT'
    gw_wait_log "$SER" WTALL "${CMD_WAIT:-14400}" || true
    finish
    echo "---- $SER (WTRUN's lines)"; tr -d '\r' < "$SER" | grep '^wtrun:'
    rm -rf "$OUT/winetest"; mkdir -p "$OUT/winetest"
    mcopy -n -i "$SCRATCH@@1048576" '::/WT/WINETEST/*' "$OUT/winetest/" 2>/dev/null || true
    python3 "$ROOT/tools/winetest-summary.py" "$OUT/winetest" ${WT_SAVE:+--save "$WT_SAVE"} ${WT_BASELINE:+--baseline "$WT_BASELINE"} || true ;;
  cmd|bat)
    if [ "$MODE" = bat ]; then run 'E:\RUN.BAT'; else run "${1:?guest command line}"; fi
    if [ -n "${SHOTS:-}" ]; then
      for i in $(seq -f '%02g' 1 "$SHOTS"); do
        sleep "${SHOT_EVERY:-5}"
        # SHOT_KEYS="12:esc,14:ret": press the key (qmpc.py names) just before that screendump
        SK="${SHOT_KEYS:-}"; for k in ${SK//,/ }; do [ "${k%%:*}" = "${i#0}" ] && { Q keys "${k#*:}" || true; sleep 1; }; done
        Q screendump "$OUT/cmd-$i.png" || true
      done
    elif [ -n "${UNTIL:-}" ]; then
      gw_wait_log "$SER" "$UNTIL" "${CMD_WAIT:-300}" || true   # the command echoes UNTIL to COM1 when it is done
    else
      sleep "${CMD_WAIT:-30}"
    fi
    finish ;;
  *) echo "unknown mode $MODE"; kill $QPID; exit 1 ;;
esac
