#!/usr/bin/env bash
# XP reads a cdimage disc through the real OS driver (doc 17 §6.3):
#
#   tools/xp-cdimage-test.sh <winxp.qcow2> <disc.cue|.ccd|.iso|isodir:dir> <reference dir> [outdir]
#
# `isodir:<dir>` serves a host directory as a generated disc (M5g); pass the
# same directory as the reference and the run is the round trip: the folder
# through libdisc, the ATAPI drive and XP's own cdrom.sys, back out again.
#
# Boots the XP image read-only (snapshot=on) with the disc as the IDE CD-ROM
# (-cdrom: the probe path, so a .cue/.ccd must reach the cdimage driver by
# itself) and a fresh FAT32 scratch disk (E:) carrying RUN.BAT, which copies
# the whole CD (xcopy /S /E, DMA through cdrom.sys) to E:\CD and lists it,
# then powers XP down over QMP and compares every file with <reference dir>
# (the files the image was made from, e.g. the ISO extracted with bsdtar).
# Prints PASS/FAIL, the copied file count, DIR output head and the QEMU log's
# cdimage lines. Exit 1 on any difference. Env: TEST_ACCEL=kvm|tcg,
# BOOT_TIMEOUT (300 s), TEST_KEEP=1 leaves XP running on failure.
# CDTEST=<path to CDTEST.EXE>: also CD audio (doc 17 §5.4). The drive gets
# `-device ide-cd,audiodev=` on a `-audiodev wav` so the tone the guest plays
# through MCI (CDTEST.EXE from the scratch disk after the copy) lands in
# <outdir>/cd.wav, whose loudest second must be a 1 kHz tone (the selftest
# disc's / `discx convert --audio tone.wav`'s track 2); cdtest.log is pulled.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?xp image}"; DISC="${2:?disc image}"; REF="${3:?reference dir}"; OUT="${4:-$ROOT/build/test/cdimage-xp}"
# the paths QEMU reads inside its own arguments, as Windows writes them
# there (tools/guestwait.sh); `isodir:<folder>` keeps its prefix
IMG="$(gw_path "$IMG")"; OUT="$(gw_path "$OUT")"
case "$DISC" in isodir:*) DISC="isodir:$(gw_path "${DISC#isodir:}")";; *) DISC="$(gw_path "$DISC")";; esac
mkdir -p "$OUT"
for t in mcopy mmd; do command -v $t >/dev/null || { echo "needs $t"; exit 2; }; done
# The scratch disk is built with sfdisk + mkfs.fat where they exist (the
# Linux box) and with mtools alone where they do not (the Mac has neither,
# nor truncate, nor du -sb, nor stat -c). Same image either way: one FAT32
# partition of type 0x0c starting at LBA 2048, which is XP's E:.
have_gnu_fat=0
command -v sfdisk >/dev/null && command -v mkfs.fat >/dev/null && have_gnu_fat=1
[ "$have_gnu_fat" = 1 ] || command -v mformat >/dev/null || { echo "needs mkfs.fat + sfdisk, or mformat"; exit 2; }
# GNU stat and BSD stat spell the same question differently, and GNU's -f
# means something else entirely, so decide once rather than per call.
if stat -c%s /dev/null >/dev/null 2>&1; then fsize() { stat -c%s "$1"; }; else fsize() { stat -f%z "$1"; }; fi
make_scratch() { # path, size in MiB
  local path="$1" mb="$2"
  rm -f "$path"
  dd if=/dev/null of="$path" bs=1 seek=$((mb * 1048576)) 2>/dev/null
  if [ "$have_gnu_fat" = 1 ]; then
    printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$path" >/dev/null
    mkfs.fat -F 32 --offset 2048 "$path" >/dev/null
  else
    # mtools formats inside the partition (@@offset) but writes no partition
    # table for a disk it did not create, so put one there ourselves.
    python3 - "$path" "$mb" <<'MBR'
import struct, sys
path, mb = sys.argv[1], int(sys.argv[2])
start, total = 2048, mb * 1024 * 1024 // 512
mbr = bytearray(512)
mbr[0x1be:0x1be + 16] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0c,
                                    b'\xfe\xff\xff', start, total - start)
mbr[510:512] = b'\x55\xaa'
with open(path, 'r+b') as f:
    f.write(bytes(mbr))
MBR
    # -H 2048: the BPB's hidden-sectors field must be the partition's own
    # start LBA. mformat defaults it to 0, and XP then does not mount the
    # volume at all: no E:, and a test that only says the batch file
    # never ran.
    mformat -i "$path@@1048576" -F -H 2048 -T $((mb * 2048 - 2048)) :: || return 1
  fi
}
[ -x "$GW_QDIR/qemu-system-i386" ] || { echo "no $GW_QDIR/qemu-system-i386"; exit 2; }
accel="${TEST_ACCEL:-}"; if [ -z "$accel" ]; then if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi; fi
cpu=(-cpu pentium3); [ "$accel" = kvm ] && cpu=(-cpu host)

# scratch FAT32 sized for the disc: the reference's bytes + 64 MB (sparse)
need=$(( $(du -sk "$REF" | cut -f1) / 1024 + 64 ))
scratch="$OUT/scratch.img"
make_scratch "$scratch" "$need" || { echo "could not build the scratch disk"; exit 2; }
fat="$scratch@@1048576"
printf '@echo off\r\nmkdir E:\\OUT\r\necho started > E:\\OUT\\STARTED.TXT\r\necho started > COM1\r\ndir D:\\ /S > E:\\OUT\\DIR.TXT\r\nxcopy D:\\ E:\\CD\\ /I /Y /S /E /H > E:\\OUT\\XCOPY.TXT\r\necho %%ERRORLEVEL%% > E:\\OUT\\XCOPYRC.TXT\r\necho done > E:\\OUT\\DONE.TXT\r\necho done > COM1\r\n' > "$OUT/RUN.BAT"
mcopy -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT
cdargs=(-cdrom "$DISC")
if [ -n "${CDTEST:-}" ]; then
  [ -f "$CDTEST" ] || { echo "CDTEST=$CDTEST not found"; exit 2; }
  mcopy -i "$fat" "$CDTEST" ::/CDTEST.EXE
  printf '@echo off\r\nmkdir E:\\OUT\r\necho started > E:\\OUT\\STARTED.TXT\r\necho started > COM1\r\ndir D:\\ /S > E:\\OUT\\DIR.TXT\r\nxcopy D:\\ E:\\CD\\ /I /Y /S /E /H > E:\\OUT\\XCOPY.TXT\r\necho %%ERRORLEVEL%% > E:\\OUT\\XCOPYRC.TXT\r\ncd /d E:\\OUT\r\nset BOXLOG=E:\\OUT\r\nE:\\CDTEST.EXE D 2 > E:\\OUT\\CDTEST.TXT\r\necho done > E:\\OUT\\DONE.TXT\r\necho done > COM1\r\n' > "$OUT/RUN.BAT"
  mcopy -o -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT
  rm -f "$OUT/cd.wav"
  cdargs=(-audiodev "wav,id=cd0,path=$OUT/cd.wav" -drive "if=none,id=cd0,media=cdrom,file=$DISC" -device "ide-cd,bus=ide.1,drive=cd0,audiodev=cd0")
fi

SOCK="$(gw_qmp_addr "$OUT")"; rm -f "$OUT/qmp.sock"; qlog="$OUT/qemu.log"; slog="$OUT/serial.log"; rm -f "$slog"
# RUN.BAT says where it is over COM1 as well as on the scratch disk,
# because the two are not equally visible: XP's lazy writer can hold a
# small file for minutes, so the host sees E:\OUT appear and stay empty
# while the batch file is already halfway through the copy (measured on
# macOS). The serial port is a host file written as the guest writes it,
# so it is what the waiting below actually watches; the FAT is the
# fallback, and everything is read off it after the shutdown flushes.
started() { grep -q started "$slog" 2>/dev/null || mcopy -n -o -i "$fat" ::/OUT/STARTED.TXT "$OUT/STARTED.TXT" 2>/dev/null; }
finished() { grep -q done "$slog" 2>/dev/null || mcopy -n -o -i "$fat" ::/OUT/DONE.TXT "$OUT/DONE.TXT" 2>/dev/null; }
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
echo "XP: $IMG (snapshot), disc: $DISC, accel: $accel"
"$GW_QDIR/qemu-system-i386" -L qemu/pc-bios -accel "$accel" "${cpu[@]}" -machine pc -m 512 \
  -drive "file=$IMG,if=ide,index=0,media=disk,snapshot=on" -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" \
  "${cdargs[@]}" -vga cirrus -net none -usb -device usb-tablet -display none -serial "file:$slog" -monitor none \
  -qmp "$(gw_qmp_opt "$SOCK")" >"$qlog" 2>&1 &
QEMU_PID=$!
teardown() {
  kill -0 "$QEMU_PID" 2>/dev/null || return 0
  if [ "${TEST_KEEP:-0}" = 1 ] && [ "$1" = fail ]; then echo "TEST_KEEP=1: XP left running, QMP at $SOCK (pid $QEMU_PID)"; return 0; fi
  qmp json '{"execute":"system_powerdown"}' 2>/dev/null
  for _ in $(seq 60); do kill -0 "$QEMU_PID" 2>/dev/null || return 0; sleep 2; done
  echo "XP did not power down, killing"; kill "$QEMU_PID" 2>/dev/null
}
fail() { echo "FAIL: $*"; teardown fail; exit 1; }
for _ in $(seq 50); do gw_listening "$SOCK" && break; sleep 0.2; done
gw_listening "$SOCK" || { tail -5 "$qlog"; fail "no QMP socket"; }
t0=$(date +%s); GW_PID=$QEMU_PID
# no head start: the knocking begins at once and stops the moment the guest
# answers (tools/guestwait.sh), which on this image is around half a minute
gw_poke_until "$SOCK" xp 'E:\RUN.BAT' "${BOOT_TIMEOUT:-300}" started \
  || { tail -5 "$qlog"; fail "RUN.BAT did not start within the timeout"; }
echo "RUN.BAT started after $(( $(date +%s) - t0 )) s"
deadline=$(( $(date +%s) + 600 ))
while ! finished; do
  if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then tail -5 "$qlog"; fail "the copy did not finish within the timeout"; fi
  sleep 3
done
echo "copy done after $(( $(date +%s) - t0 )) s; shutting XP down"
teardown ok
rm -rf "$OUT/cd" "$OUT/DIR.TXT" "$OUT/XCOPY.TXT" "$OUT/XCOPYRC.TXT"; mkdir -p "$OUT/cd"
mcopy -n -i "$fat" ::/OUT/DIR.TXT ::/OUT/XCOPY.TXT ::/OUT/XCOPYRC.TXT "$OUT/" 2>/dev/null
mcopy -s -n -i "$fat" ::/CD/* "$OUT/cd/" 2>/dev/null
echo "xcopy: $(LC_ALL=C tr -d '\r' < "$OUT/XCOPY.TXT" | tail -1), rc $(LC_ALL=C tr -d '\r\n ' < "$OUT/XCOPYRC.TXT")"
echo "DIR D:\\ head:"; LC_ALL=C tr -d '\r' < "$OUT/DIR.TXT" | head -8 | sed 's/^/    /'
grep -i 'cdimage\|libdisc' "$qlog" | sed 's/^/    qemu: /'

# every reference file present with the same bytes (names case-insensitively: FAT/ISO 9660)
rc=0; n=0
while IFS= read -r -d '' f; do
  rel="${f#"$REF"/}"
  got="$(find "$OUT/cd" -ipath "$OUT/cd/$rel" -type f | head -1)"
  if [ -z "$got" ]; then echo "  missing: $rel"; rc=1; continue; fi
  if ! cmp -s "$f" "$got"; then echo "  differs: $rel ($(fsize "$f") vs $(fsize "$got") bytes)"; rc=1; fi
  n=$((n+1))
done < <(find "$REF" -type f -print0)
extra=$(( $(find "$OUT/cd" -type f | wc -l) - n ))
[ "$extra" -ne 0 ] && echo "  $extra file(s) on the copy that are not in the reference"
[ "$n" -gt 0 ] || { echo "FAIL: no reference files under $REF (extract the ISO with bsdtar, or xorriso -osirrox on -indev x.iso -extract / dir)"; rc=1; }
if [ -n "${CDTEST:-}" ]; then
  mcopy -n -i "$fat" ::/OUT/CDTEST.LOG "$OUT/cdtest.log" 2>/dev/null || mcopy -n -i "$fat" ::/OUT/cdtest.log "$OUT/cdtest.log" 2>/dev/null
  echo "cdtest.log:"; LC_ALL=C tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | grep -E 'tracks|play|position|mode|RESULT|error' | head -30 | sed 's/^/    /'
  # the verdict: MCI saw the play running with advancing positions, and the tone is in the wav; the
  # script's last line is reported but not required (XP's mcicda has ended CDTEST.EXE silently after
  # a resume past the track's end)
  grep -q 'RESULT ok' "$OUT/cdtest.log" 2>/dev/null || echo "  note: CDTEST.EXE ended before its last line (last: $(LC_ALL=C tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | tail -1))"
  npos=$(LC_ALL=C tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | grep -c 'status cd position" -> "02:00:0[1-9]')
  if grep -q '"playing"' "$OUT/cdtest.log" 2>/dev/null && [ "${npos:-0}" -ge 1 ] && python3 - "$OUT/cd.wav" <<'PY'
import struct, sys, math
p = sys.argv[1]
d = open(p, 'rb').read()
i = d.find(b'data'); assert i > 0, "no data chunk"
n = struct.unpack('<I', d[i+4:i+8])[0]; pcm = d[i+8:i+8+n]
rate = struct.unpack('<I', d[24:28])[0]; ch = struct.unpack('<H', d[22:24])[0]
frames = len(pcm) // (2 * ch)
print("wav: %d frames, %d Hz, %d ch, %.1f s" % (frames, rate, ch, frames / rate))
best = (0, 0)
for sec in range(int(frames / rate)):
    s = pcm[sec * rate * 2 * ch:(sec + 1) * rate * 2 * ch]
    v = struct.unpack('<%dh' % (len(s) // 2), s)[::ch]
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    if rms > best[0]: best = (rms, sec)
rms, sec = best
print("loudest second: %d (rms %.0f)" % (sec, rms))
assert rms > 500, "silence"
s = pcm[sec * rate * 2 * ch:(sec + 1) * rate * 2 * ch]
v = struct.unpack('<%dh' % (len(s) // 2), s)[::ch][:4096]
N = len(v); bestf = (0, 0)
for k in range(50, 3000, 10):
    re = sum(v[t] * math.cos(2 * math.pi * k * t / rate) for t in range(N))
    im = sum(v[t] * math.sin(2 * math.pi * k * t / rate) for t in range(N))
    m = re * re + im * im
    if m > bestf[0]: bestf = (m, k)
print("dominant frequency ~%d Hz" % bestf[1])
assert 950 <= bestf[1] <= 1050, "not the 1 kHz tone"
PY
  then echo "PASS: CD audio: the 1 kHz tone reached the audiodev"; else echo "FAIL: CD audio (cdtest.log / cd.wav under $OUT)"; rc=1; fi
  # and the drive must stop when it is told to: mcicda's Stop is a START STOP
  # UNIT, so a drive that ends playback on STOP PLAY/SCAN alone answers the
  # mode query right after it with "playing" and plays the track out.
  stopmode=$(LC_ALL=C tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | grep -A1 '"stop cd"' | grep -m1 'status cd mode' || true)
  if [ -z "$stopmode" ]; then echo "  note: CDTEST.EXE never reached its stop step, nothing to check"
  elif printf '%s' "$stopmode" | grep -q '"playing"'; then echo "FAIL: the drive played on after MCI stopped it ($stopmode)"; rc=1
  else echo "PASS: the drive stopped when MCI stopped it ($stopmode)"; fi
fi
if [ $rc -eq 0 ]; then echo "PASS: $n files copied from the disc match the reference"; else echo "FAIL: differences above ($n reference files)"; fi
exit $rc
