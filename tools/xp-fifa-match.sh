#!/usr/bin/env bash
# FIFA 2000 into a real match on the d3dpt-vga HAL, headless, and a keyboard test there (doc 15 "FIFA 2000 on the HAL").
#
#   tools/xp-fifa-match.sh kvm|tcg <image.qcow2> [outdir]
#
# Boots bare QEMU with the FIFA disc as D: and the driver ISO as F:, stages
# tools/xp-fifa2000.bat (+ DINPUT\DINPUT.DLL when built) on the E: scratch
# disk, runs it, then drives the game over QMP: Esc skips the intro, the
# title is detected as the first static frame, AMISTOSO's ball icon and the
# arrows are long-held mouse clicks (the menus ignore 100 ms clicks), Left
# picks the side, Avançar starts the match, which kicks off by itself. In the
# match: F2 (tower camera), F1, Right held 2 s, Esc (pause menu), Esc, F12
# (exit dialog) as 100 ms taps, a screendump after each. Without the DINPUT
# shim the match ignores all of them; with it every one shows. Outputs in
# outdir (default build/xp-driver-test/fifa-match-<mode>): the screendumps,
# qemu.log (the executor's frames/s lines), and dinput_log.txt pulled from
# the image afterwards with 7z (the game never exits on its own).
# Env: FIFA_ISO (default /mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO),
# EXEC=wine (the executor in another process on Wine, M15: the A/B on a host
# with Vulkan).
set -u
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
MODE="${1:?kvm|tcg}"; IMG="${2:?image.qcow2}"; OUT="${3:-$ROOT/build/xp-driver-test/fifa-match-$MODE}"
FIFA_ISO="${FIFA_ISO:-/mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO}"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
mkdir -p "$OUT"
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
sed 's/\r$//; s/$/\r/' "$ROOT/tools/xp-fifa2000.bat" > "$OUT/RUN.BAT"
mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
# build-wrappers.sh upper-cases the whole staged tree for 8.3, so the shim is
# DINPUT.DLL there; the lower-case name only exists between the compile and
# that rename. Looking for one name only made this silently stage nothing and
# the match then ignores every key, which reads as a driver regression.
SHIM="$ROOT/guest-tools/out/iso/DINPUT/DINPUT.DLL"
[ -f "$SHIM" ] || SHIM="$ROOT/guest-tools/out/iso/DINPUT/dinput.dll"
if [ -f "$SHIM" ]; then
  mcopy -o -i "$SCRATCH@@1048576" "$SHIM" ::/DINPUT.DLL
  echo "DINPUT.DLL staged (the keyboard fix)"
  # this is the diagnostic harness, so ask the shim for its log too (RUN.BAT
  # turns D3DPT_DINPUT_LOG on when it finds this marker); a game folder gets
  # the silent shim
  : > "$OUT/DILOG"; mcopy -o -i "$SCRATCH@@1048576" "$OUT/DILOG" ::/DILOG
else
  echo "WARNING: no dinput shim in guest-tools/out/iso/DINPUT (run guest-tools/build-wrappers.sh)."
  echo "         The match will ignore every key — this harness's fault, not the HAL's."
fi
SOCK="/tmp/xp-fifa-$$.sock"; rm -f "$SOCK"      # short: a Unix socket path is limited to ~100 chars
trap 'rm -f "$SOCK"' EXIT
if [ "$MODE" = kvm ]; then ACCEL=(-accel kvm -cpu host); SLOW=1; else ACCEL=(-cpu pentium3); SLOW=3; fi
case "$(uname -s)" in Darwin) SO=dylib; DXVK_SO=0.dylib;; *) SO=so; DXVK_SO=so.0;; esac
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$DXVK_SO}"
# EXEC=wine: the executor in another process on Wine (M15, ADR-018). On a
# host with Vulkan it is the A/B, on one below the floor what the user gets
VGA_ARGS=(-vga none -device "d3dpt-vga${EXEC:+,exec=$EXEC}")
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$FIFA_ISO" -drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on" \
  "${VGA_ARGS[@]}" -net none -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$OUT/qemu.log" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
ev() { Q json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}" >/dev/null; }
key_hold() { ev "{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; sleep "$2"; ev "{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; }
lclick() {  # x y (of 640x480) wait name: a 1.5 s left click, the menus ignore short ones
  ev "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(( $1 * 32767 / 640 ))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(( $2 * 32767 / 480 ))}}"
  sleep 0.5; ev '{"type":"btn","data":{"down":true,"button":"left"}}'; sleep 1.5; ev '{"type":"btn","data":{"down":false,"button":"left"}}'
  sleep "$3"; Q screendump "$OUT/$4.png"
}
t0=$(date +%s); T() { echo "[$(( $(date +%s) - t0 ))s] $*"; }
GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# the adapter says when the display driver has the desktop; only the
# painting after it is a guess
gw_wait_log "$OUT/qemu.log" "linear mode on" $(( 150 * SLOW )) \
  || { echo "the desktop never came up on our driver:"; tail -5 "$OUT/qemu.log"; exit 1; }
sleep $(( 8 * SLOW )); T "desktop"
n0=$(grep -c "linear mode on (640x480" "$OUT/qemu.log")
Q keys esc; sleep 1; Q keys meta_l+r; sleep 2; Q type 'cmd /k E:\RUN.BAT'; Q keys ret
# bounded: QEMU can refuse to start at all (another process holding the
# image's write lock is the usual one) and this used to spin for ever
for _ in $(seq 1 $(( 60 * SLOW ))); do
  [ "$(grep -c "linear mode on (640x480" "$OUT/qemu.log")" -gt "$n0" ] && break
  kill -0 "$QPID" 2>/dev/null || { echo "QEMU exited before the game came up:"; sed -n '1,10p' "$OUT/qemu.log"; exit 1; }
  sleep 3
done
if [ "$(grep -c "linear mode on (640x480" "$OUT/qemu.log")" -le "$n0" ]; then
  echo "the game never switched to 640x480 within $(( 180 * SLOW )) s; last of qemu.log:"; tail -10 "$OUT/qemu.log"
  kill "$QPID" 2>/dev/null; exit 1
fi
T "game up"; sleep $(( 5 * SLOW )); Q keys esc          # skips the intro video
prev=""
for i in $(seq 1 40); do                                  # the title screen is the first static frame
  sleep 6; cur=$(Q screendump "$OUT/title.png" >/dev/null; md5sum < "$OUT/title.png")
  [ "$cur" = "$prev" ] && break; prev="$cur"
done
T "title (after $i polls)"
Q keys ret; sleep $(( 8 * SLOW )); Q screendump "$OUT/menu.png"
lclick 170 287 $(( 8 * SLOW )) teams; T "team select"
lclick 600 440 $(( 8 * SLOW )) side; T "side select"
Q keys left; sleep $(( 4 * SLOW )); Q screendump "$OUT/side-left.png"
lclick 600 440 80 match; T "match (80 s after Avançar: it kicks off by itself)"
grep "frames/s" "$OUT/qemu.log" | tail -1 | sed 's/qemu-system-i386: info: //'
Q screendump "$OUT/m0.png"
Q keys f2; sleep 4; Q screendump "$OUT/m1-f2.png"; T "after F2 (tower camera)"
Q keys f1; sleep 4; Q screendump "$OUT/m2-f1.png"; T "after F1 (TV camera)"
key_hold right 2; Q screendump "$OUT/m3-right.png"; T "after Right held 2 s"
Q keys esc; sleep 4; Q screendump "$OUT/m4-esc.png"; T "after Esc (pause menu)"
Q keys esc; sleep 4; Q screendump "$OUT/m5-esc.png"; T "after Esc again"
Q keys f12; sleep 8; Q screendump "$OUT/m6-f12.png"; T "after F12 (exit dialog)"
grep "frames/s" "$OUT/qemu.log" | tail -1 | sed 's/qemu-system-i386: info: //'
Q json '{"execute":"system_powerdown"}' >/dev/null
sleep 40; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
if command -v 7z >/dev/null; then
  7z e -y -so "$IMG" "Arquivos de programas/EA SPORTS/FIFA 2000/dinput_log.txt" 2>/dev/null | tr -d '\r' > "$OUT/dinput_log.txt"
  echo "---- dinput_log.txt: keys the game's DirectInput state reported in the match, and what Windows saw"
  awk '/shim loaded/{n++} {if (n) print}' "$OUT/dinput_log.txt" | grep -E "state DIK|async: VK|set from GetAsyncKeyState" | tail -20
fi
T "done: screendumps in $OUT"
