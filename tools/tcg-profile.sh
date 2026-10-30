#!/usr/bin/env bash
# Where does the emulated CPU's time go under TCG? (M9 track)
#
#   tools/tcg-profile.sh <image.qcow2> <name> ['guest command line']
#   tools/tcg-profile.sh ~/vms/winxp.qcow2 idle
#   tools/tcg-profile.sh ~/vms/winxp.qcow2 7zip '"C:\Program Files\7-Zip\7z.exe" b -mmt1 20'
#   CDROM=~/vms/FIFA2000.ISO VGA=d3dpt tools/tcg-profile.sh ~/vms/winxp-m7.qcow2 fifa \
#       '"C:\Arquivos de programas\EA SPORTS\FIFA 2000\fifa2000.exe"'
#
# Boots the image headless as a snapshot (nothing is written to it) with
# `-perfmap` (QEMU writes /tmp/perf-<pid>.map: one line per translated guest
# instruction with its host code range), optionally types a command into the
# Run dialog, waits WARM seconds for it to settle, then samples the whole
# QEMU process for SECS seconds (macOS: `sample` at 1 ms, every thread, the
# call tree with self counts, generated code as unknown addresses; Linux:
# `perf record -g -p`). Saves the perf map, `info jit`, a screendump
# and the QEMU log next to the sample, then prints the report
# (tools/tcg-profile.py): time per thread, the vCPU thread split into
# generated code / helpers / softmmu slow path / translation / interrupts /
# other, the generated-code samples mapped to guest addresses (kernel vs
# user, hot pages, hot instructions).
#
# Env: OUT (build/tcg-profile/<name>), BOOT_WAIT (the cap on waiting for
# the desktop, 300 s; the boot is not slept out, the run waits for the
# guest's disks to go quiet, and on VGA=d3dpt for the adapter's mode line),
# WARM (20 s after the command), SECS (30 s of sampling), CPU (pentium3;
# add ,x87-fast=off etc.), MEM (512), VGA (cirrus | d3dpt = -vga none
# -device d3dpt-vga with the executor, for the M7 images), DDFLAGS (that
# adapter's bisection register: 32 = no Direct3D, which puts a title on its
# own software renderer), CDROM (a disc as
# D:, default the newest guest-tools ISO), CDS='a.mds:b.iso' (more discs
# after it, each an ide-cd with CD audio through the AC97 card, so a game's
# disc sits where the player puts it; .mds/.cue/.ccd go through the cdimage
# driver), SND=1 (the AC97 card, implied by CDS; the M7 images have it),
# KEEP=1 (leave the guest running,
# QMP socket printed), KEYS='alt+c,down,ret' (chords sent 1.5 s apart,
# KEYS_WAIT (15 s) after the command: menus and dialogs of a GUI program,
# `keys.png` shows the result), DFILTER='0x80501000..0x80502000,...' (second pass:
# also log the guest disassembly, the optimized TCG ops and the host code
# of every TB starting in those ranges to $OUT/qemu-d.log, from the
# `tcg-profile.py --hot` line of a first pass, for tools/tcg-hot.py),
# QEMU_EXTRA='-global ...' (more QEMU arguments, e.g. an experiment's switch),
# QEMU_BIN (another qemu-system-i386, e.g. a baseline kept aside for an A/B),
# FPS=<s> (after the sample, tools/tcg-fps.py counts the guest's distinct VGA
# frames for that many seconds -> fps.txt: an absolute number next to the %).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; NAME="${2:?name}"; GUEST_CMD="${3:-}"
OUT="${OUT:-$ROOT/build/tcg-profile/$NAME}"; mkdir -p "$OUT"
OS="$(uname -s)"
CDROM="${CDROM:-$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)}"
CPU="${CPU:-pentium3}"; MEM="${MEM:-512}"
PERFMAP="${PERFMAP:-1}"; [ "$PERFMAP" = 0 ] && PERFMAP=   # PERFMAP=0: no -perfmap (its writer costs ~12 % of the vCPU on a retranslation-bound game; fps runs)
BOOT_WAIT="${BOOT_WAIT:-300}"; WARM="${WARM:-20}"; SECS="${SECS:-30}"

VGA_ARGS=(-vga cirrus)
# DDFLAGS: the adapter's bisection register (d3dpt/hw/d3dpt_vga.c); 32 = DDF_NO_D3D,
# i.e. a DirectDraw-only driver, which is how a title is put on its own software renderer
[ "${VGA:-cirrus}" = d3dpt ] && VGA_ARGS=(-vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}${EXEC:+,exec=$EXEC}")   # EXEC=wine: the executor in another process (M15), the A/B
# the game discs and the sound card, as under the player (tools/xp-game-test.sh's CDS=)
SND_ARGS=(-audiodev none,id=snd0); [ "${SND:-${CDS:+1}}" = 1 ] && SND_ARGS+=(-device AC97,audiodev=snd0)
# IDE slots: the disk is ide.0/0, CDROM (-cdrom = index 2) ide.1/0; the CDS discs take the two slave slots
CD_ARGS=(); n=0; IFS=: read -ra CDLIST <<< "${CDS:-}"
for cd in "${CDLIST[@]}"; do [ -n "$cd" ] || continue
  [ $n -lt 2 ] || { echo "CDS: at most two discs"; exit 1; }
  CD_ARGS+=(-drive "file=$cd,media=cdrom,id=cd$n,if=none" -device "ide-cd,bus=ide.$n,unit=1,drive=cd$n,audiodev=snd0"); n=$((n+1))
done
# the Direct3D executor (the M4 device is always on the pc machine; the M7
# adapter with VGA=d3dpt): same environment as scripts/test.sh
case "$OS" in Darwin) SO=dylib;; *) SO=so;; esac
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)}"
if [ "$OS" = Darwin ]; then  # docs/build-macos.md: Homebrew's loader, the LunarG KosmicKrisp ICD
  # the loader's own keg, never all of `/opt/homebrew/lib`, which shadows
  # ImageIO's codecs by leaf name (docs/build-macos.md)
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi

SOCK="/tmp/tcgprof-$$.sock"; rm -f "$SOCK"
LOG="$OUT/qemu.log"
"${QEMU_BIN:-$ROOT/build/qemu/qemu-system-i386}" -L "$ROOT/qemu/pc-bios" -machine pc -cpu "$CPU" -m "$MEM" \
  -drive "file=$IMG,if=ide,index=0,snapshot=on" ${CDROM:+-cdrom "$CDROM"} \
  "${CD_ARGS[@]}" "${VGA_ARGS[@]}" "${SND_ARGS[@]}" -net none -usb -device usb-tablet ${PERFMAP:+-perfmap} \
  ${DFILTER:+-d in_asm,op_opt,out_asm -dfilter "$DFILTER" -D "$OUT/qemu-d.log"} ${QEMU_EXTRA:-} \
  -display none -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$LOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
HMP() { Q json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("return",""))'; }
finish() {
  if [ "${KEEP:-0}" = 1 ]; then echo "KEEP=1: guest left running, pid $QPID, QMP $SOCK"; return; fi
  Q json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
  gw_wait_exit "$QPID" 90 || true
  kill $QPID 2>/dev/null || true
}
trap finish EXIT

echo "== $NAME: pid $QPID, booting"
GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# This machine has no serial line and (unless VGA=d3dpt) no adapter of ours
# to ask, and the command below must be typed exactly once (starting a game
# twice would wreck the sample), so the boot is not knocked on. It ends when
# the guest stops reading its disk, which is as close to "the desktop is
# settled" as a machine that cannot answer questions gets (tools/guestwait.sh).
[ "${VGA:-cirrus}" = d3dpt ] && { gw_wait_log "$LOG" "linear mode on" "$BOOT_WAIT" || true; }
gw_wait_quiet "$SOCK" "$BOOT_WAIT" 8 || true
Q screendump "$OUT/desktop.png" >/dev/null || true
if [ -n "$GUEST_CMD" ]; then
  Q keys esc; sleep 1; Q keys meta_l+r; sleep 3
  Q type ' '; Q keys backspace           # the first key after the chord is lost
  Q type "$GUEST_CMD"; Q keys ret
  echo "== typed: $GUEST_CMD; settling ($WARM s)"
  if [ -n "${KEYS:-}" ]; then           # KEYS='alt+c,down,ret' after the program is up: menus, dialogs
    sleep "${KEYS_WAIT:-15}"
    for k in ${KEYS//,/ }; do Q keys "$k"; sleep 1.5; done
    Q screendump "$OUT/keys.png" >/dev/null || true
  fi
  sleep "$WARM"
fi
Q screendump "$OUT/before.png" >/dev/null || true
HMP "info jit" > "$OUT/info-jit-before.txt" || true

echo "== sampling $SECS s"
case "$OS" in
  Darwin)
    sample "$QPID" "$SECS" 1 -mayDie -file "$OUT/sample.txt" >/dev/null
    ;;
  *)
    perf record -g -F 1000 -p "$QPID" -o "$OUT/perf.data" -- sleep "$SECS" >/dev/null 2>&1
    perf report -i "$OUT/perf.data" --stdio --no-children --sort sym > "$OUT/perf-report.txt" 2>/dev/null || true
    ;;
esac
[ -n "$PERFMAP" ] && cp "/tmp/perf-$QPID.map" "$OUT/perf.map"
if [ -n "${FPS:-}" ]; then
  python3 "$ROOT/tools/tcg-fps.py" "$SOCK" "$FPS" | tee "$OUT/fps.txt"
fi
HMP "info jit" > "$OUT/info-jit-after.txt" || true
HMP "info registers" > "$OUT/info-registers.txt" || true
Q screendump "$OUT/after.png" >/dev/null || true
echo "== done: $OUT"
[ "$OS" = Darwin ] && python3 "$ROOT/tools/tcg-profile.py" "$OUT" | tee "$OUT/report.txt"
