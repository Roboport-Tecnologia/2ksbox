#!/usr/bin/env bash
# Moto Racer (Delphine, 1997; a DirectX 3 title: execute buffers,
# palettized and colour-keyed textures, the two caps that made it run its
# software rasterizer on the driver before protocol v8, doc 15
# "When a title falls back to its software renderer" and "Execute buffers")
# on the d3dpt-vga driver, headless.
#
#   tools/xp-motoracer.sh install <image.qcow2> [outdir]   # D:\SETUP.EXE (InstallShield: Install, Next x3, no DirectX 3) into
#                                                          #   C:\Arquivos de programas\MotoRacer; then the game as below
#   tools/xp-motoracer.sh play <image.qcow2> [outdir]      # the desktop at 800x600x16 (the game insists on 16 bpp), MOTO.EXE, the
#                                                          #   title, a name, Play Solo / Practice / Continue / Start into a race; each
#                                                          #   menu recognised by tools/motoracer-state.py from a screendump and retried;
#                                                          #   screendumps title.png / name.png / menu.png / bike*.png / race*.png; alt+F4, power-down
#   tools/xp-motoracer.sh vm <image.qcow2> [outdir]        # just boot with the disc as D:, QMP at /tmp/xp-moto.sock, detached
#   tools/xp-motoracer.sh stop                             # power that VM down
#
# The disc (an Alcohol .mds/.mdf, a mixed-mode CD: one data track + 12 CD
# audio tracks) is D: through the cdimage block driver (doc 17), the driver
# ISO F:, a FAT scratch disk E:. The game is a DirectX 3 title: execute
# buffers, texture handles, a 16 bpp mode (doc 15 "Execute buffers"). The
# menus take the mouse only (QMP clicks), the title screen the keyboard.
# The pass for `play`: `d3dptdisp: d3d context` in the QEMU log (the game
# took the HAL), `ddi: N frames/s (... draws ...)` lines with draws while
# the race runs, and race screendumps with the track and the bikes on the
# panorama. Env: MOTO_MDS (default the oldstuff folder's), CPU (KVM model,
# default host), DDFLAGS (the device's bisection knob).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
MODE="${1:?install|play|vm|stop}"
SOCK=/tmp/xp-moto.sock
MOTO_MDS="${MOTO_MDS:-/mnt/data2/david/Downloads/oldstuff/Moto.Racer.1997.DSI.CD/MOTO_RACER.mds}"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
t0=$(date +%s); T() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

if [ "$MODE" = stop ]; then
  Q json '{"execute":"system_powerdown"}'; exit 0
fi
IMG="${2:?image.qcow2}"; OUT="${3:-$ROOT/build/xp-driver-test/moto}"; mkdir -p "$OUT"
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
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$MOTO_MDS" -drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on" \
  -vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}" -net none -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$OUT/qemu.log" 2>&1 < /dev/null &
disown
if [ "$MODE" = vm ]; then
  echo "VM started: log $OUT/qemu.log, QMP $SOCK (python3 tools/qmpc.py $SOCK screendump x.png)"; exit 0
fi

gw_wait_log "$OUT/qemu.log" "linear mode on (800x600x" "${DESKTOP_WAIT:-300}" \
  || { echo "the desktop never came up on our driver:"; tail -5 "$OUT/qemu.log"; exit 1; }   # the desktop, 32 or 16 bpp
sleep 12; T "desktop"
Q keys esc
if [ "$MODE" = install ]; then
  Q keys meta_l+r; sleep 2; Q type 'D:\SETUP.EXE'; Q keys ret; sleep 8; Q screendump "$OUT/setup.png"
  Q click 608 182 800 600; sleep 8                   # Install (the autorun front end)
  Q click 496 459 800 600; sleep 4                   # Welcome: Next
  Q click 496 459 800 600; sleep 4                   # Destination: Next
  Q click 496 459 800 600; sleep 30                  # Typical: Next -> the copy (~20 s), then the program group opens
  Q screendump "$OUT/setup-done.png"
  Q keys alt+f4; sleep 2                             # the program-group explorer window
  Q click 402 350 800 600; sleep 4                   # "Setup is complete": OK
  Q click 449 387 800 600; sleep 4                   # "install DirectX 3 now?": Nao
  Q click 608 382 800 600; sleep 3                   # Exit the front end
  T "installed"
fi
# the game wants a 16 bpp desktop ("16 bit screen mode required!" otherwise)
Q keys meta_l+r; sleep 2; Q type 'F:\DRIVER\SETMODE.EXE 800 600 16'; Q keys ret; sleep 5
Q keys meta_l+r; sleep 2; Q type 'cmd /c cd /d "C:\Arquivos de programas\MotoRacer" & MOTO.EXE'; Q keys ret
gw_wait_log "$OUT/qemu.log" "linear mode on (640x480x16" "${GAME_WAIT:-300}" \
  || { echo "the game never switched to 640x480x16:"; tail -5 "$OUT/qemu.log"; exit 1; }
T "game up at 640x480x16"
# The menus, driven by what the screen shows (tools/motoracer-state.py
# classifies a screendump): the title takes an Enter (a click on it starts
# the attract demo), the name screen a click on its return glyph (an Enter
# there presses whichever letter the cursor was left on), the 2D menus
# clicks. The title runs into the attract demo when left alone (a loading
# screen and a demo both classify as "other": after 20 s of it the demo's
# Esc menu is taken to Quit demo), so every step is checked and retried.
state() { Q screendump "$1" >/dev/null; python3 "$ROOT/tools/motoracer-state.py" "$1.ppm"; }
other=0
for i in $(seq 1 80); do
  s=$(state "$OUT/step.png")
  case "$s" in
    title)        cp "$OUT/step.png" "$OUT/title.png"; Q keys ret; sleep 8 ;;
    name)         cp "$OUT/step.png" "$OUT/name.png"; Q click 550 345; sleep 6 ;;
    menu)         cp "$OUT/step.png" "$OUT/menu.png"; Q click 165 255; sleep 6 ;;         # Play Solo
    mode)         cp "$OUT/step.png" "$OUT/mode.png"; Q click 110 260; sleep 6 ;;         # Practice
    race-select)  cp "$OUT/step.png" "$OUT/select.png"; Q click 565 372; sleep 8 ;;       # Continue (Speed Bay, 3 laps)
    showroom)     cp "$OUT/step.png" "$OUT/bike.png"; break ;;
    *)            other=$((other + 1)); sleep 2
                  if [ "$other" -ge 10 ]; then other=0; T "a demo? Esc menu, Quit demo"; Q keys esc; sleep 4; Q keys down; sleep 3; Q keys ret; sleep 8; fi ;;
  esac
  [ "$s" = other ] || other=0
done
[ "$s" = showroom ] || { T "never reached the showroom (last screen: $s)"; }
T "showroom"; sleep 6; Q screendump "$OUT/bike2.png"  # the showroom's bike turns under the spotlights
Q click 565 390; sleep 20                            # Start -> the race (the countdown, then the AI rides on its own)
for i in 1 2 3; do Q screendump "$OUT/race$i.png"; sleep 8; done
T "race"
Q keys esc; sleep 2; Q keys alt+f4; sleep 4
Q json '{"execute":"system_powerdown"}' >/dev/null
for i in $(seq 1 45); do [ -S "$SOCK" ] || break; sleep 2; done
grep -E "d3d context 0x|frames/s|legacy opcode|unknown token|batch .*error|dp2 0x|page flips" "$OUT/qemu.log" | sed 's/qemu-system-i386: info: //' | sort | uniq -c | sort -rn | head -12
T "done: screendumps in $OUT"
