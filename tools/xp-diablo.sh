#!/usr/bin/env bash
# Diablo (1996, DirectDraw 640x480x8 with palette animation) on the
# d3dpt-vga driver: the 8 bpp palettized-mode title (doc 15 "8 bpp
# palettized modes").
#
#   tools/xp-diablo.sh install <image.qcow2> [outdir]   # D:\SETUP.EXE -> C:\Diablo (three clicks), then the game as below
#   tools/xp-diablo.sh play <image.qcow2> [outdir]      # C:\Diablo\Diablo.exe: Esc past the intro, Single Player, a new
#                                                       #   Warrior, into Tristram; screendumps title.png / menu.png / town.png
#                                                       #   / walk.png / char.png; alt+F4 out, power-down; ~4 min under KVM
#   tools/xp-diablo.sh vm <image.qcow2> [outdir]        # just boot with the disc as D:, QMP at /tmp/xp-diablo.sock, detached
#   tools/xp-diablo.sh stop                             # power that VM down
#
# The Diablo disc is D:, the driver ISO F:, a FAT scratch disk E:. Diablo's
# menus ignore QMP mouse clicks (like FIFA's) but take the keyboard; in the
# game the clicks work (walk.png). The pass: town.png shows Tristram with the
# right colours (the QMP screendump is the device's palette conversion) and the
# QEMU log has `linear mode on (640x480x8`. Env: DIABLO_ISO (default the
# oldstuff folder's), CPU (KVM model, default host).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
MODE="${1:?install|play|vm|stop}"
SOCK=/tmp/xp-diablo.sock
DIABLO_ISO="${DIABLO_ISO:-/mnt/data2/david/Downloads/oldstuff/Diablo.iso}"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
t0=$(date +%s); T() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

if [ "$MODE" = stop ]; then
  Q json '{"execute":"system_powerdown"}'; exit 0
fi
IMG="${2:?image.qcow2}"; OUT="${3:-$ROOT/build/xp-driver-test/diablo}"; mkdir -p "$OUT"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  truncate -s 64M "$SCRATCH"
  printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
  mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
fi
rm -f "$SOCK"
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
# detached: the VM outlives the shell that started it (the harness kills long background tasks)
setsid nohup "$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" -accel kvm -cpu "${CPU:-host}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$DIABLO_ISO" -drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on" \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$OUT/qemu.log" 2>&1 < /dev/null &
disown
if [ "$MODE" = vm ]; then
  echo "VM started: log $OUT/qemu.log, QMP $SOCK (python3 tools/qmpc.py $SOCK screendump x.png)"; exit 0
fi

gw_wait_log "$OUT/qemu.log" "linear mode on (800x600x32" "${DESKTOP_WAIT:-300}" \
  || { echo "the desktop never came up on our driver:"; tail -5 "$OUT/qemu.log"; exit 1; }
sleep 12; T "desktop"
Q keys esc; Q keys meta_l+r; sleep 2
if [ "$MODE" = install ]; then
  Q type 'D:\SETUP.EXE'; Q keys ret; sleep 8; Q screendump "$OUT/setup.png"
  Q click 240 100 800 600; sleep 5                 # Install & Play Diablo
  Q click 480 435 800 600; sleep 10                # OK: C:\Diablo (the copy takes a few seconds)
  Q screendump "$OUT/setup-done.png"
  Q click 358 354 800 600                          # "DirectX 2.0 or better cannot be detected (sound) — play anyway?": Sim
else
  Q type 'C:\Diablo\Diablo.exe'; Q keys ret
fi
gw_wait_log "$OUT/qemu.log" "linear mode on (640x480x8" "${GAME_WAIT:-300}" \
  || { echo "the game never switched to 640x480x8:"; tail -5 "$OUT/qemu.log"; exit 1; }
T "game up at 640x480x8"; sleep 12
Q keys esc; sleep 6                                # skips the intro cinematic
prev=""
for i in $(seq 1 8); do                            # the title menu: its flames animate, so settle for "mostly static"
  Q screendump "$OUT/title.png" >/dev/null; cur=$(md5sum < "$OUT/title.png"); [ "$cur" = "$prev" ] && break; prev="$cur"; sleep 3
done
T "title menu"
Q keys ret; sleep 4; Q screendump "$OUT/menu.png"    # Single Player -> New Single Player Hero (Warrior highlighted)
Q keys ret; sleep 3; Q type 'Palette'; Q keys ret    # class, then the name
sleep 20; Q screendump "$OUT/town.png"; T "town"
Q click 450 250; sleep 4; Q screendump "$OUT/walk.png"
Q keys c; sleep 2; Q screendump "$OUT/char.png"; Q keys c
Q keys alt+f4; sleep 4                             # the game closes on WM_CLOSE
Q json '{"execute":"system_powerdown"}' >/dev/null
for i in $(seq 1 45); do [ -S "$SOCK" ] || break; sleep 2; done
grep -E "linear mode on \(640x480x8|batch .*error|dp2" "$OUT/qemu.log" | sed 's/qemu-system-i386: info: //' | sort | uniq -c | head
T "done: screendumps in $OUT"
