#!/usr/bin/env bash
# GTA Vice City (Rockstar North, 2003; DirectX 8 through
# RenderWare: XP's own d3d8.dll on the driver's DX8 DDI, no wrapper DLL) on
# the d3dpt-vga driver, headless. The workload behind protocol v9's
# video-memory vertex / index buffers (doc 15 "Vertex and index buffers in
# video memory"): its frames are 300–600 draws each, and before v9 every
# draw's vertices were copied through the command window by the guest.
#
#   tools/xp-vicecity.sh play <image.qcow2> [outdir]   # the game from the desktop into a new game: the play disc as D:, the
#                                                      #   legal / intro screens, Start Game -> New Game by mouse, the opening
#                                                      #   cutscenes skipped, then MEASURE seconds of the city (screendumps
#                                                      #   menu.png, cutscene.png, street.png, city*.png); the QEMU log's rate
#                                                      #   lines are the numbers (rates.txt: the in-city ones)
#   tools/xp-vicecity.sh vm <image.qcow2> [outdir]     # just boot with the disc as D: and start the game, QMP at /tmp/xp-vc.sock, detached
#   tools/xp-vicecity.sh attach <image.qcow2> [outdir] # the `play` flow on a VM already up (vm mode, or one whose game launch was
#                                                      #   lost): starts the game from the desktop and drives it as above
#   tools/xp-vicecity.sh stop                          # power that VM down
#
# The A/B for v9 is the same run with DDFLAGS=1048576 (ddflags 0x100000 =
# DDF_NO_HWVB: every buffer back in system memory, every draw's vertices
# copied into the record). For a throughput number add 0x8000 (the flip
# chain's vertical blank off: DDFLAGS=32768 / 1081344), else both runs sit
# at 60 fps under KVM; the game's own frame limiter (Options / Display
# Setup) caps it at 30 and must be off in the image's settings. NO_KVM=1
# runs it under TCG (-cpu pentium3), the Apple Silicon shape; every wait
# is then the log's rate lines, not the clock. Env: VC_ISO (the play disc;
# default the oldstuff folder's FLT-VCB ISO), CPU (KVM model, default
# pentium3: an era CPU for an era game), DDFLAGS, MEASURE (seconds in the
# city, default 60), DESKTOP_WAIT (seconds after the desktop's mode switch
# before the Run dialog; 15 under KVM, 120 under TCG).
# The game lives in winxp-m7g (C:\Arquivos de programas\Rockstar Games\Grand
# Theft Auto Vice City); run on an overlay of it, never on the user's image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
MODE="${1:?play|vm|attach|stop}"
SOCK=/tmp/xp-vc.sock
VC_ISO="${VC_ISO:-/mnt/data2/david/Downloads/oldstuff/FLT-VCB.iso01.iso}"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
t0=$(date +%s); T() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

if [ "$MODE" = stop ]; then
  Q json '{"execute":"system_powerdown"}'; exit 0
fi
IMG="${2:?image.qcow2}"; OUT="${3:-$ROOT/build/xp-driver-test/vc}"; mkdir -p "$OUT"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
[ -f "$VC_ISO" ] || { echo "no play disc $VC_ISO (VC_ISO=)"; exit 1; }
if [ "$MODE" != attach ]; then
  SCRATCH="$OUT/scratch.img"
  if [ ! -f "$SCRATCH" ]; then
    truncate -s 64M "$SCRATCH"
    printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
    mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
  fi
  sed 's/\r$//; s/$/\r/' "$ROOT/tools/xp-vicecity.bat" > "$OUT/RUN.BAT"
  mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
  rm -f "$SOCK"
  export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
  export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
  ACCEL=(-accel kvm -cpu "${CPU:-pentium3}")
  [ -n "${NO_KVM:-}" ] && ACCEL=(-cpu pentium3)
  # detached: the VM outlives the shell that started it (the harness kills long background tasks)
  setsid nohup "$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
    -hda "$IMG" -hdb "$SCRATCH" -cdrom "$VC_ISO" -drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on" \
    -vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}" -net none -usb -device usb-tablet -display none \
    -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$OUT/qemu.log" 2>&1 < /dev/null &
  disown
  # the desktop: its mode switch comes at the logon screen, the desktop itself
  # some seconds (KVM) or a couple of minutes (TCG) later. Keys typed before
  # it are lost (a TCG run can sit at the screen saver)
  gw_wait_log "$OUT/qemu.log" "linear mode on (800x600x" "${DESKTOP_WAIT_MAX:-300}" \
    || { echo "the desktop never came up on our driver:"; tail -5 "$OUT/qemu.log"; exit 1; }
  sleep "${DESKTOP_WAIT:-$([ -n "${NO_KVM:-}" ] && echo 120 || echo 15)}"; T "desktop"
else
  [ -S "$SOCK" ] || { echo "no VM at $SOCK (vm mode first)"; exit 1; }
  T "attaching to the VM at $SOCK"
fi
Q keys esc; sleep 1
Q keys meta_l+r; sleep 2; Q type 'E:\RUN.BAT'; Q keys ret
if [ "$MODE" = vm ]; then
  echo "VM started, game launching: log $OUT/qemu.log, QMP $SOCK (python3 tools/qmpc.py $SOCK screendump x.png)"; exit 0
fi
gw_wait_log "$OUT/qemu.log" "d3dptdisp: d3d context 0x" "${GAME_WAIT:-600}" \
  || { echo "the game never created a Direct3D context:"; tail -5 "$OUT/qemu.log"; exit 1; }
T "game up (a Direct3D context)"
# The legal screens and the intro movie run into the main menu on their
# own (Start Game / Options / Quit Game). The pointer sits on Options at
# the screen's centre and the highlight follows it, so the menus are driven
# by clicks, not keys (an Enter picks whatever is under the pointer). Every
# wait below reads the log's 5 s rate lines as draws per frame (draws /
# readbacks: the frame rate itself varies with the vertical blank and the
# accelerator): the intro draws hundreds a frame, the menu 2–10, the game
# hundreds again.
nlines() { grep -c "frames/s" "$OUT/qemu.log"; }
draws() { grep "frames/s" "$OUT/qemu.log" | tail -1 | sed 's/.*(\([0-9]*\) readbacks.* \([0-9]*\) draws.*/\2 \1/' | awk '{ print ($2 > 0) ? int($1 / $2) : 0 }'; }
wait_lines() { local n; n=$(nlines); until [ "$(nlines)" -ge $(( n + $1 )) ]; do sleep 2; done; }
wait_lines 1                                          # the game presents
for i in $(seq 1 120); do
  wait_lines 1; d=$(draws)
  [ "$d" -lt 40 ] && { wait_lines 1; d2=$(draws); [ "$d2" -lt 40 ] && break; }
done
T "main menu (draws/frame: $(draws))"; sleep 2; Q screendump "$OUT/menu.png" >/dev/null
Q click 400 242 800 600; sleep 3                     # Start Game
Q click 400 220 800 600; sleep 3                     # New Game
T "new game"
# the load (30 s under KVM, minutes under TCG), then the opening cutscenes
# (the deal in Sonny's office, then the street): each is skipped by Space
# once it plays; Space in the game itself is sprint, harmless
for i in $(seq 1 200); do wait_lines 1; d=$(draws); [ "$d" -gt 100 ] && break; done
T "cutscene (draws/frame: $d)"; sleep 3; Q screendump "$OUT/cutscene.png" >/dev/null
Q keys spc; wait_lines 2; Q screendump "$OUT/street.png" >/dev/null
Q keys spc; wait_lines 2; Q screendump "$OUT/city0.png" >/dev/null
Q keys spc; wait_lines 1
T "measuring ${MEASURE:-60} s in the city (the frame limiter is off in the image's settings)"
n0=$(nlines)
for i in $(seq 1 $(( ${MEASURE:-60} / 20 ))); do sleep 20; Q screendump "$OUT/city$i.png" >/dev/null; done
grep "frames/s" "$OUT/qemu.log" | tail -n +$(( n0 + 1 )) | sed 's/.*ddi: //' > "$OUT/rates.txt"
Q keys esc; sleep 2; Q keys alt+f4; sleep 5
Q json '{"execute":"system_powerdown"}' >/dev/null
for i in $(seq 1 90); do [ -S "$SOCK" ] || break; sleep 2; done
echo "-- in the city ($OUT/rates.txt):"; cat "$OUT/rates.txt"
grep -E "skipped|refused|unknown token|dp2 0x" "$OUT/qemu.log" | sed 's/qemu-system-i386: info: //' | head -5
T "done: screendumps in $OUT"
