#!/usr/bin/env bash
# A game on the d3dpt-vga display driver (doc 15), headless. The image must
# have the driver installed (tools/xp-driver-test.sh <image> install).
#
#   tools/xp-game-test.sh <image.qcow2> "<game dir on C:>" <exe> [name]
#   tools/xp-game-test.sh stacks <drwtsn32.log>          # print the newest report's thread stacks
#
# Boots a snapshot=on view of the image with bare qemu-system-i386 (no
# player), the discs in CDS on the IDE slots after the disk (the same
# letters the game sees under the player), and a FAT32 USB stick that
# carries RUN.BAT and receives everything the run produces. RUN.BAT is
# typed into the Run dialog once XP is up; it relaunches itself minimized
# (so no console sits in front of the game's dialogs), starts the game, and when the game is gone copies its logs, Dr. Watson's report
# and the crash minidump to the stick.
#
# Env:
#   CDS="a.iso:b.iso"   discs after the disk, colon-separated (at most 3; add the guest-tools ISO
#                       yourself when the game needs it)
#   PRE_CMD="cmd"       extra batch command(s) run in the game folder before the EXE starts
#                       (e.g. rewrite a config: PRE_CMD='echo D:\disk1\Levels> cd.ini')
#   KEYS="8:ret,25:esc" QMP keys sent after the first Direct3D context (delay in s, QKeyCode)
#   SHOTS=n             screendump (shot-<t>.png) every n s: dialogs, launchers and the
#                       game's own frames (the driver draws them into the adapter's VRAM)
#   PAGEHEAP=1          full page heap for the EXE (heap overruns fault where they happen)
#   DRW_AFTER=s         attach Dr. Watson to the game s seconds after start: every
#                       thread's stack in drwtsn32.log (the game is killed by it)
#   WAIT_MAX=s          give up waiting for the game to exit (default 300)
#   STICK_MB=n          USB stick size (default 64)
#   QEMU_EXTRA="args"    extra qemu arguments, word-split (e.g. QEMU_EXTRA="-trace ide_atapi_cmd_packet
#                       -D $PWD/atapi.log" to see every ATAPI command a protection issues)
#   NO_ATTACH=1         the run expects no Direct3D context: do not wait for one (a
#                       CD-protection run, where the game never creates a device)
#   VGA=cirrus          XP's own Cirrus driver instead of ours (the control for a crash)
#   ACCEL=kvm|tcg       default: kvm when /dev/kvm is writable
#   OUT=dir             default build/xp-game-test/<name>
#
# Output: OUT/qemu.log (the adapter's log), shot-*.png, EXIT.TXT (the
# game's exit code), TASKS.TXT, the game folder's *.log, drwtsn32.log, user.dmp.
# The summary at the end prints the device log tail, the exit code and the
# newest Dr. Watson report's main-thread stack when there is one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
. "$ROOT/tools/guestwait.sh"

stacks() {  # the newest report in a drwtsn32.log (UTF-16): module list + every thread's stack
  local txt; txt=$(iconv -f utf-16 -t utf-8 "$1" 2>/dev/null || iconv -f cp1252 -t utf-8 "$1")
  local start; start=$(printf '%s\n' "$txt" | grep -n '(pid=' | tail -1 | cut -d: -f1)
  [ -n "$start" ] || { echo "no report in $1"; return; }
  printf '%s\n' "$txt" | awk -v L="$start" 'NR>=L' | grep -E '\(pid=|^\(0*[0-9a-f]+ - |identificador do segmento|thread id|^eax=|^eip=|^[0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} \S|FAULT|FALHA' | grep -v 'Despejo simplificado'
}
[ "${1:-}" = stacks ] && { stacks "$2"; exit 0; }

IMG="${1:?image}"; GAMEDIR="${2:?game dir}"; EXE="${3:?exe}"; NAME="${4:-$(basename "$EXE" .exe)}"
OUT="${OUT:-build/xp-game-test/$NAME}"; rm -rf "$OUT"; mkdir -p "$OUT"
ACCEL="${ACCEL:-}"; if [ -z "$ACCEL" ]; then if [ -w /dev/kvm ]; then ACCEL=kvm; else ACCEL=tcg; fi; fi
CPU=(-cpu "${CPU:-$([ "$ACCEL" = kvm ] && echo host || echo pentium3)}")   # CPU=pentium3 under KVM masks the host features
for t in sfdisk mkfs.fat mcopy mdir; do command -v $t >/dev/null || { echo "needs $t"; exit 1; }; done

# the stick: one FAT32 partition, RUN.BAT on it
STICK="$OUT/stick.img"; truncate -s "${STICK_MB:-64}M" "$STICK"
printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$STICK" >/dev/null
mkfs.fat -F 32 --offset 2048 "$STICK" >/dev/null
FAT="$STICK@@1048576"
DRW='C:\Documents and Settings\All Users\Dados de aplicativos\Microsoft\Dr Watson'
[ -n "${DRW_DIR:-}" ] && DRW="$DRW_DIR"      # a non-Portuguese XP: "Application Data"
PRE="rem"
[ -n "${PRE_CMD:-}" ] && PRE="$PRE & $PRE_CMD"
# full page heap for the EXE (ntdll honours the IFEO flags without gflags.exe): every
# heap overrun faults at the guilty instruction instead of corrupting a neighbour
[ "${PAGEHEAP:-0}" = 1 ] && PRE="$PRE & reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\$EXE\" /v GlobalFlag /t REG_DWORD /d 0x02000000 /f > nul & reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\$EXE\" /v PageHeapFlags /t REG_DWORD /d 3 /f > nul"
WAITSTEP="ping -n ${WAIT_MAX:-300} 127.0.0.1 > nul"
if [ -n "${DRW_AFTER:-}" ]; then
  WAITSTEP="ping -n $DRW_AFTER 127.0.0.1 > nul & for /f \"tokens=2\" %%p in ('tasklist /fi \"imagename eq $EXE\" /nh') do drwtsn32 -p %%p & ping -n 15 127.0.0.1 > nul"
fi
{
  printf '@echo off\r\n'
  printf 'if not "%%1"=="min" (start /min "" cmd /c "%%~f0" min & exit)\r\n'
  printf 'set O=%%~d0\r\n'
  # the run is knocked on until this file turns up on the stick (a removable
  # drive, so XP writes it through instead of holding it back); the guard is
  # what keeps a second knock from starting the game twice
  printf 'if exist %%O%%\\STARTED.TXT exit\r\n'
  printf 'echo started > %%O%%\\STARTED.TXT\r\n'
  printf 'cd /d "%s"\r\n' "$GAMEDIR"
  printf '%s\r\n' "$PRE"
  printf 'ping -n 3 127.0.0.1 > nul\r\n'                # a key-up from the Run dialog must not reach the game
  printf 'start "" %s\r\n' "$EXE"
  printf 'ping -n 5 127.0.0.1 > nul\r\n'
  printf ':wait\r\n'
  printf 'tasklist /fi "imagename eq %s" /nh | find /i "%s" > nul\r\n' "$EXE" "$EXE"
  printf 'if errorlevel 1 goto gone\r\n'
  printf '%s\r\n' "$WAITSTEP"
  if [ -z "${DRW_AFTER:-}" ]; then printf 'goto wait\r\n'; fi
  printf ':gone\r\n'
  printf 'echo exit %%ERRORLEVEL%% > %%O%%\\EXIT.TXT\r\n'
  printf 'copy *.log %%O%%\\ > nul\r\n'
  printf 'copy "%s\\drwtsn32.log" %%O%%\\ > nul\r\n' "$DRW"
  printf 'copy "%s\\user.dmp" %%O%%\\ > nul\r\n' "$DRW"
  printf 'tasklist > %%O%%\\TASKS.TXT\r\n'
  printf 'echo done > %%O%%\\DONE.TXT\r\n'
} > "$OUT/RUN.BAT"
mcopy -i "$FAT" "$OUT/RUN.BAT" ::/RUN.BAT

DRIVES=(-drive "file=$IMG,if=ide,index=0,media=disk,snapshot=on")
n=0; IFS=: read -ra CDLIST <<< "${CDS:-}"; for cd in "${CDLIST[@]}"; do [ -n "$cd" ] || continue; DRIVES+=(-drive "file=$cd,media=cdrom"); n=$((n+1)); done
[ $n -le 3 ] || { echo "at most 3 discs (4 IDE units)"; exit 1; }
SOCK="/tmp/xp-game-test-$$.sock"; QLOG="$OUT/qemu.log"
echo "  $IMG (snapshot) accel $ACCEL, discs: ${CDS:-none}, game: $GAMEDIR\\$EXE -> $OUT"
VGA_ARGS=(-vga none -device d3dpt-vga); [ -n "${VGA:-}" ] && VGA_ARGS=(-vga "$VGA")
build/qemu/qemu-system-i386 -L qemu/pc-bios -accel "$ACCEL" "${CPU[@]}" -machine pc -m "${MEM:-1024}" \
  "${DRIVES[@]}" -drive "file=$STICK,format=raw,if=none,id=stick" -device usb-storage,drive=stick \
  "${VGA_ARGS[@]}" -net none -device AC97,audiodev=a0 -audiodev none,id=a0 -usb -device usb-tablet \
  -display none -serial none -monitor none -qmp "unix:$SOCK,server,nowait" \
  ${QEMU_EXTRA:+$QEMU_EXTRA} >"$QLOG" 2>&1 &
QEMU=$!
cleanup() { kill -0 $QEMU 2>/dev/null && { python3 tools/qmpc.py "$SOCK" json '{"execute":"quit"}' >/dev/null 2>&1 || kill $QEMU; }; rm -f "$SOCK"; }
trap cleanup EXIT
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
shot() { python3 tools/qmpc.py "$SOCK" screendump "$OUT/$1.png" >/dev/null 2>&1 || true; }
GW_PID=$QEMU
gw_wait_sock "$SOCK" || exit 1
t0=$(date +%s)
# No boot sleep: knock on the Run dialog from the start and let the guest
# say when it has the command (tools/guestwait.sh). RUN.BAT's own marker on
# the stick is the proof. The game's first Direct3D context comes later,
# and on a NO_ATTACH run it never comes at all.
gw_poke_until "$SOCK" xp 'cmd /c for %d in (D E F G H I) do if exist %d:\RUN.BAT %d:\RUN.BAT' "${BOOT_WAIT:-300}" \
  mcopy -n -i "$FAT" ::/STARTED.TXT "$OUT/STARTED.TXT" \
  || { echo "  the guest never ran RUN.BAT"; shot no-shell; }
if [ "${NO_ATTACH:-0}" = 1 ]; then
  echo "  RUN.BAT running after $(( $(date +%s) - t0 )) s, no Direct3D context expected"
elif gw_wait_log "$QLOG" "ddi: context " "${ATTACH_WAIT:-240}"; then
  echo "  first Direct3D context after $(( $(date +%s) - t0 )) s"
else
  echo "  no Direct3D context within ${ATTACH_WAIT:-240} s"; shot no-context
fi
IFS=, read -ra KS <<< "${KEYS:-}"
for k in "${KS[@]}"; do sleep "${k%%:*}"; echo "  key ${k#*:} at $(( $(date +%s) - t0 )) s"; qmp keys "${k#*:}"; done
n=0
while ! mcopy -n -i "$FAT" ::/DONE.TXT "$OUT/DONE.TXT" 2>/dev/null; do
  sleep 5; n=$((n+5))
  if [ -n "${SHOTS:-}" ] && [ $((n % SHOTS)) = 0 ]; then shot "shot-$n"; fi
  if [ $n -ge $(( ${WAIT_MAX:-300} + 60 )) ] || ! kill -0 $QEMU 2>/dev/null; then echo "  gave up after $n s"; shot "gave-up"; break; fi
done
shot final
for f in EXIT.TXT TASKS.TXT drwtsn32.log user.dmp; do mcopy -n -i "$FAT" "::/$f" "$OUT/" 2>/dev/null || true; done
mcopy -n -i "$FAT" '::/*.log' "$OUT/" 2>/dev/null || true
echo "---- adapter log (tail)"; grep -v -E '^info:|audio: Could not' "$QLOG" | sed 's/^qemu-system-i386: info: d3dpt-vga: //' | tail -12
echo "---- exit: $(cat "$OUT/EXIT.TXT" 2>/dev/null || echo '(none)')"
[ -f "$OUT/drwtsn32.log" ] && { echo "---- Dr. Watson, newest report (main thread first; full: $0 stacks $OUT/drwtsn32.log):"; stacks "$OUT/drwtsn32.log" | awk 'NR <= 40'; }
echo "---- files in $OUT:"; ls "$OUT" | tr '\n' ' '; echo
