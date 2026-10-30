#!/usr/bin/env bash
# The benchmark programs of
# docs/22-tcg-evaluation.md, cross-built for the XP guest, plus their inputs
# and the ISO the guest runs them from (build/specbench/sb.iso).
#
# The suite (docs/22 §4): programs whose hot paths are the ones the patch
# queue touches, each with a fixed, deterministic workload:
#
#   nbench 2.2.3 (BYTEmark)   ten self-timed kernels: seven integer, three x87 double
#                             (Fourier, neural net, LU decomposition); prints iterations/s
#   7-Zip 26.03 `7zr b 2 -mmt1 -md=22`  LZMA compress + decompress, one thread, two passes at dictionary 22:
#                             integer, `ret`-heavy, memory-bound; prints MIPS ratings
#   Super PI mod 1.5 XS, 1M   x87 at 53-bit precision, the classic; on the image
#                             (SUPERPI_DIR), keyed from the host; time = the job's CPU time
#   SSEBENCH.EXE              our own: the SSE / x87 kernels a Direct3D game runs
#                             (guest-tools ISO, doc 16); ns per op
#
# SPEC=1 also builds the open-source ancestors of five SPEC CINT2006
# benchmarks (bzip2, GNU Go, HMMER, Sjeng, libquantum) with fixed inputs.
# Measured once (default 1.07x geomean over pristine QEMU) and left out of
# the paper because none of the patches target compiled integer code;
# kept for the upstream-interest question.
#
# 7zr.exe (public domain, https://www.7-zip.org/a/7zr.exe) and SSEBENCH.EXE
# (from the newest guest-tools ISO) are placed in build/specbench/extra by
# this script. Everything cross-built here uses the guest-tools toolchain
# flags (msvcrt, since XP has no UCRT, with _USE_32BIT_TIME_T so time_t
# matches what msvcrt.dll's own ftime()/time() fill; -march=pentium3 for
# SSE1, doubles on x87 and the era's codegen; -O2) and gnu89 so 1990s C
# gets through GCC 16. Tarballs are fetched into build/specbench/src and checked
# against the hashes below.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/specbench"; SRC="$B/src"; OUT="$B/out"; IN="$B/inputs"; NAT="$B/native"
mkdir -p "$SRC" "$OUT" "$IN" "$NAT/bin" "$B/shim/sys"
for t in i686-w64-mingw32-gcc xorriso python3 cc; do command -v $t >/dev/null || { echo "needs $t"; exit 1; }; done

fetch() {  # url sha256
  local f="$SRC/$(basename "$1")"
  [ -f "$f" ] || curl -sSL -o "$f" "$1"
  echo "$2  $f" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $f"; exit 1; }
}
mkdir -p "$B/extra"
[ -f "$B/extra/7zr.exe" ] || curl -sSL -o "$B/extra/7zr.exe" https://www.7-zip.org/a/7zr.exe
GTISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
[ -f "$GTISO" ] || { echo "no guest-tools ISO (SSEBENCH.EXE): run guest-tools/build-wrappers.sh"; exit 1; }
xorriso -osirrox on -indev "$GTISO" -extract /TESTS/SSEBENCH.EXE "$B/extra/SSEBENCH.EXE" >/dev/null 2>&1
chmod u+w "$B/extra/SSEBENCH.EXE"
if [ -n "${SPEC:-}" ]; then
fetch https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
fetch https://www.sjeng.org/ftp/Sjeng-Free-11.2.tar.gz aaf584d12e79f8e366710d40cc02a27a7927dd924234421107b2fb8e84cfd727
fetch http://deb.debian.org/debian/pool/main/libq/libquantum/libquantum_1.1.1.orig.tar.gz d8e3c4407076558f87640f1e618501ec85bc5f4c5a84db4117ceaec7105046e5
fetch http://eddylab.org/software/hmmer/hmmer-2.3.2.tar.gz d20e1779fcdff34ab4e986ea74a6c4ac5c5f01da2993b14e92c94d2f076828b4
fetch https://ftp.gnu.org/gnu/gnugo/gnugo-3.8.tar.gz da68d7a65f44dcf6ce6e4e630b6f6dd9897249d34425920bfdd4e07ff1866a72
fi
fetch https://www.math.utah.edu/~mayer/linux/nbench-byte-2.2.3.tar.gz 723dd073f80e9969639eb577d2af4b540fc29716b6eafdac488d8f5aed9101ac

# the cross compiler, as guest-tools/build-wrappers.sh sets it up
CC="$B/cc.sh"
cat > "$CC" <<'EOF'
#!/usr/bin/env bash
exec i686-w64-mingw32-gcc -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -D_USE_32BIT_TIME_T -D_WIN32_WINNT=0x0501 -march=pentium3 -mtune=generic -O2 -std=gnu89 -fcommon \
  -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types \
  -Wno-return-mismatch -Wno-declaration-missing-parameter-type "$@"
EOF
chmod +x "$CC"
# mingw has no <sys/times.h>; HMMER's stopwatch wants times() and sysconf(_SC_CLK_TCK)
cat > "$B/shim/sys/times.h" <<'EOF'
#ifndef SHIM_SYS_TIMES_H
#define SHIM_SYS_TIMES_H
#include <time.h>
struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
static clock_t times(struct tms *t) { clock_t c = clock(); t->tms_utime = c; t->tms_stime = 0; t->tms_cutime = 0; t->tms_cstime = 0; return c; }
#define _SC_CLK_TCK 2
#define sysconf(x) ((long)CLOCKS_PER_SEC)
#endif
EOF

unpack() { [ -d "$SRC/$1" ] || tar xzf "$SRC/$2" -C "$SRC"; }
if [ -n "${SPEC:-}" ]; then
unpack bzip2-1.0.8 bzip2-1.0.8.tar.gz
unpack Sjeng-Free-11.2 Sjeng-Free-11.2.tar.gz
unpack libquantum-1.1.1 libquantum_1.1.1.orig.tar.gz
unpack hmmer-2.3.2 hmmer-2.3.2.tar.gz
unpack gnugo-3.8 gnugo-3.8.tar.gz
fi
unpack nbench-byte-2.2.3 nbench-byte-2.2.3.tar.gz

if [ -n "${SPEC:-}" ]; then
echo "==> bzip2"
(cd "$SRC/bzip2-1.0.8" && "$CC" -o "$OUT/bzip2.exe" blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c bzip2.c
 cc -O2 -w -o "$NAT/bin/bzip2" blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c bzip2.c)

echo "==> Sjeng (no gdbm book: the benchmark plays fixed positions)"
(cd "$SRC/Sjeng-Free-11.2"
 cat > config.h <<'EOF'
#define HAVE_FTIME 1
#define HAVE_STRSTR 1
#define HAVE_SYS_TIMEB_H 1
#define HAVE_LIBM 1
#define STDC_HEADERS 1
#define VERSION "11.2"
#define PACKAGE "Sjeng"
EOF
 cat > nobook.c <<'EOF'
#include "sjeng.h"
#include "protos.h"
#include "extvars.h"
#include <string.h>
unsigned long bookidx;
move_s choose_binary_book_move(void) { move_s m; memset(&m, 0, sizeof m); return m; }
void book_learning(int result) { (void)result; }
void build_book(void) {}
EOF
 # interrupt() on Windows peeks stdin during a search and treats the script's next
 # line as an interruption (180 nodes per move): the benchmark build never polls
 python3 - <<'PY'
s = open("utils.c").read(); k = "int interrupt(void)\n{"
if "SPECBENCH_NO_INTERRUPT" not in s: open("utils.c", "w").write(s.replace(k, k + "\n#ifdef SPECBENCH_NO_INTERRUPT\n  return 0;\n#endif"))
# and no learning file across runs (Learn/LoadLearn): every run must do the same search
s = open("learn.c").read()
if "SPECBENCH_NO_INTERRUPT" not in s:
    for k in ("void Learn(int score, int best, int depth)\n{", "void LoadLearn(void)\n{"):
        s = s.replace(k, k + "\n#ifdef SPECBENCH_NO_INTERRUPT\n  return;\n#endif")
    open("learn.c", "w").write(s)
PY
 S="attacks.c book.c crazy.c draw.c ecache.c epd.c eval.c learn.c leval.c moves.c neval.c partner.c probe.c proof.c rcfile.c search.c see.c segtb.c seval.c sjeng.c ttable.c utils.c nobook.c"
 "$CC" -DHAVE_CONFIG_H -DSPECBENCH_NO_INTERRUPT -o "$OUT/sjeng.exe" $S -lm
 cc -O2 -std=gnu89 -w -DHAVE_CONFIG_H -DSPECBENCH_NO_INTERRUPT -o "$NAT/bin/sjeng" $S -lm)

echo "==> libquantum (shor)"
(cd "$SRC/libquantum-1.1.1"
 [ -f config.h ] || ./configure --host=i686-w64-mingw32 CC="$CC" >/dev/null 2>&1
 # cross configure cannot run its probes: the real type and the complex pointer come out empty
 sed -i.bak 's/#define REAL_FLOAT $/#define REAL_FLOAT float/' types.h
 sed -i.bak2 's/quantum_matrix H,  \*\*w/quantum_matrix H, COMPLEX_FLOAT **w/' quantum.h
 # shor.c seeds rand() from the clock and its measurement decides the work: pin the seed
 sed -i.bak3 's/srand(time(0));/srand(20060101);/' shor.c
 make -j4 >/dev/null 2>&1 || true
 "$CC" -o "$OUT/shor.exe" shor.c -I. .libs/libquantum.a -lm)
(rm -rf "$NAT/libquantum-1.1.1"; tar xzf "$SRC/libquantum_1.1.1.orig.tar.gz" -C "$NAT"; cd "$NAT/libquantum-1.1.1"
 ./configure >/dev/null 2>&1; sed -i.bak 's/#define REAL_FLOAT $/#define REAL_FLOAT float/' types.h
 sed -i.bak3 's/srand(time(0));/srand(20060101);/' shor.c
 make >/dev/null 2>&1 || true; cc -O2 -std=gnu89 -w -o "$NAT/bin/shor" shor.c -I. .libs/libquantum.a -lm)

echo "==> HMMER (hmmsearch)"
(cd "$SRC/hmmer-2.3.2"
 [ -f src/config.h ] || ./configure --host=i686-w64-mingw32 --build=x86_64-pc-linux-gnu CC="$CC" CFLAGS="-I$B/shim" >/dev/null 2>&1
 make -k -j4 >/dev/null 2>&1 || true; cp src/hmmsearch.exe "$OUT/")
(rm -rf "$NAT/hmmer-2.3.2"; tar xzf "$SRC/hmmer-2.3.2.tar.gz" -C "$NAT"; cd "$NAT/hmmer-2.3.2"
 ./configure >/dev/null 2>&1 || ./configure --build=x86_64-pc-linux-gnu >/dev/null 2>&1; make -k -j8 >/dev/null 2>&1 || true; cp src/hmmsearch "$NAT/bin/")

echo "==> GNU Go (native first: its pattern compilers run at build time)"
(rm -rf "$NAT/gnugo-3.8"; tar xzf "$SRC/gnugo-3.8.tar.gz" -C "$NAT"; cd "$NAT/gnugo-3.8"
 ./configure --without-curses --without-readline CFLAGS="-O2 -fcommon -w" >/dev/null 2>&1; make -j8 >/dev/null 2>&1)
(cd "$SRC/gnugo-3.8"
 [ -f config.h ] || ./configure --host=i686-w64-mingw32 --build=x86_64-pc-linux-gnu CC="$CC" --without-curses --without-readline >/dev/null 2>&1
 for t in mkeyes mkpat joseki mkmcpat uncompress_fuseki; do
   printf '#!/bin/sh\nexec "%s" "$@"\n' "$NAT/gnugo-3.8/patterns/$t" > patterns/$t; chmod +x patterns/$t
 done
 make -j4 >/dev/null 2>&1; cp interface/gnugo.exe "$OUT/")

fi

echo "==> nbench"
(cd "$SRC/nbench-byte-2.2.3" && touch pointer.h && cp sysinfo.c.template sysinfo.c && cp sysinfoc.c.template sysinfoc.c
 "$CC" -o "$OUT/nbench.exe" nbench0.c nbench1.c emfloat.c misc.c hardware.c sysspec.c -lm)

echo "==> specrun (the guest runner)"
"$CC" -o "$OUT/specrun.exe" "$ROOT/tools/specbench/specrun.c"

if [ -n "${SPEC:-}" ]; then
echo "==> inputs"
(cd "$IN"
 # a 15 MB text corpus: every source file of the suite in a fixed order, plus one PDF
 (find "$SRC/gnugo-3.8" "$SRC/hmmer-2.3.2" "$SRC/Sjeng-Free-11.2" "$SRC/libquantum-1.1.1" "$SRC/bzip2-1.0.8" -type f \( -name '*.c' -o -name '*.h' -o -name '*.db' -o -name '*.txt' -o -name '*.sgf' \) | LC_ALL=C sort | xargs cat; cat "$SRC/hmmer-2.3.2/Userguide.pdf") > text.src 2>/dev/null
 "$NAT/bin/bzip2" -9 -c text.src > text.bz2
 python3 - <<'EOF'
import random
random.seed(20060101)
aa = "ACDEFGHIKLMNPQRSTVWY"
with open("db.fa", "w") as f:
    for i in range(16000):
        n = random.randint(120, 700)
        f.write(">seq%05d synthetic\n" % i)
        s = "".join(random.choice(aa) for _ in range(n))
        for j in range(0, n, 60): f.write(s[j:j+60] + "\n")
EOF
 cp "$SRC/hmmer-2.3.2/tutorial/rrm.hmm" .
 { printf 'boardsize 19\nclear_board\nlevel 10\n'; for i in $(seq 1 9); do printf 'genmove black\ngenmove white\n'; done; printf 'quit\n'; } > go.gtp
 printf 'xboard\nprotover 2\nsd 8\nst 100000\nnew\nsetboard r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4\ngo\nsetboard r2q1rk1/ppp2ppp/2np1n2/2b1p1B1/2B1P1b1/2NP1N2/PPP2PPP/R2Q1RK1 w - - 0 8\ngo\nsetboard 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1\ngo\nsetboard r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1\ngo\nquit\n' > sjeng.in)

fi

echo "==> the ISO"
rm -rf "$B/iso"; mkdir -p "$B/iso/SB"
cp "$OUT"/{specrun,nbench}.exe "$SRC/nbench-byte-2.2.3/NNET.DAT" "$B/iso/SB/"
cp "$B/extra/7zr.exe" "$B/extra/SSEBENCH.EXE" "$B/iso/SB/"
# Super PI mod 1.5 XS is on the image (SUPERPI_DIR); the batch file starts it and
# waits for pi_data.txt, which it writes when the 1M run is done, and then for
# the file to stop growing, because the digits take a while to write under TCG
# and a kill mid-write leaves a truncated file (a CRC that differs for no
# computational reason). The keys that pick 1M come from the host driver
# (run.sh). Its time is the job's CPU time.
cat > "$B/iso/SB/SUPERPI.BAT" <<BAT
@echo off
cd /d ${SUPERPI_DIR:-C:\Docume~1\David\Desktop\SUPER_~1.5}
if exist pi_data.txt del pi_data.txt
start "" super_pi_mod.exe
:w
ping -n 2 127.0.0.1 > nul
if not exist pi_data.txt goto w
:s
for %%f in (pi_data.txt) do set S1=%%~zf
ping -n 3 127.0.0.1 > nul
for %%f in (pi_data.txt) do set S2=%%~zf
if not "%S1%"=="%S2%" goto s
tskill super_pi_mod
type pi_data.txt
BAT
# tab-separated: name, command (through cmd.exe /c), stdin file, extra file to CRC
printf 'superpi\tC:\\SB\\SUPERPI.BAT\t-\t-\r\n' > "$B/iso/SB/run.lst"
printf '7zip\t7zr.exe b 2 -mmt1 -md=22\t-\t-\r\n' >> "$B/iso/SB/run.lst"
printf 'ssebench\tSSEBENCH.EXE -iter 20\t-\t-\r\n' >> "$B/iso/SB/run.lst"
printf 'nbench\tnbench.exe\t-\t-\r\n' > "$B/iso/SB/once.lst"
if [ -n "${SPEC:-}" ]; then
  cp "$OUT"/{bzip2,shor,hmmsearch,gnugo,sjeng}.exe "$IN"/{text.src,text.bz2,db.fa,rrm.hmm,go.gtp,sjeng.in} "$B/iso/SB/"
  printf 'bzip2-c\tbzip2.exe -9 -c text.src\t-\t-\r\n' >> "$B/iso/SB/run.lst"
  printf 'bzip2-d\tbzip2.exe -dc text.bz2 & bzip2.exe -dc text.bz2 & bzip2.exe -dc text.bz2\t-\t-\r\n' >> "$B/iso/SB/run.lst"
  printf 'shor\tshor.exe 391 12\t-\t-\r\n' >> "$B/iso/SB/run.lst"
  printf 'hmmer\thmmsearch.exe rrm.hmm db.fa\t-\t-\r\n' >> "$B/iso/SB/run.lst"
  printf 'gnugo\tgnugo.exe --mode gtp --seed 1\tgo.gtp\t-\r\n' >> "$B/iso/SB/run.lst"
  # Sjeng prints its own timings: keep only the moves and node counts, which must be identical everywhere
  printf 'sjeng\tsjeng.exe | findstr /r /c:"^move" /c:"^Nodes"\tsjeng.in\t-\r\n' >> "$B/iso/SB/run.lst"
fi
cat > "$B/iso/SB/GO.BAT" <<'BAT'
@echo off
echo ready > A:\READY.TXT
if exist C:\SB rd /s /q C:\SB
xcopy D:\SB C:\SB /E /I /Y /Q > nul
cd /d C:\SB
specrun.exe run.lst A:\RESULTS.TXT C:\SB 2
specrun.exe once.lst A:\RESULTS.TXT C:\SB 1
for %%f in (nbench superpi 7zip ssebench sjeng shor hmmer gnugo) do if exist C:\SB\%%f.out copy C:\SB\%%f.out A:\%%f.TXT > nul
echo finished >> A:\RESULTS.TXT
BAT
sed -i.bak 's/$/\r/' "$B/iso/SB/GO.BAT" "$B/iso/SB/SUPERPI.BAT" && rm -f "$B/iso/SB/"*.bak
xorriso -as mkisofs -quiet -J -R -V SPECBENCH -o "$B/sb.iso" "$B/iso"
ls -la "$B/sb.iso"
