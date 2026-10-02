#!/usr/bin/env bash
# Windows 11 on Arm against libsmb, scripted (track M23 step 3): Explorer's
# copy engine both ways over a tree with awkward names and a 3000-file
# folder, a 512 MB file timed, editing in place (append, truncate,
# ReplaceFile, a case-only rename), the drive's size, change notification
# of a file the host adds, and Explorer's window on the share for the
# closing screenshot.
#
#   tools/smb-win11-test.sh [base]     base: an installed machine's
#                                      win11-spike.py OUT (build/w11d)
#
# Boots a fresh qcow2 overlay of base's disk in OUT (build/w11s), with
# copies of its firmware variables and TPM state: base is never written.
# smbserve serves OUT/share on OUT/smb.sock, which QEMU patch 79 forwards
# the guest's 10.0.2.4:445 to; tools/win11-spike/smb-explorer.ps1 runs
# elevated on the desktop. Then the host checks the guest's upload against
# the hashes the guest wrote, and lists what the server did not support.
#
# Environment: OUT, TPM_PPI (off: QEMU 11.1's HVF aborts on the TPM's PPI
# region, M21), MAX_DIALECT (3.1.1). macOS on Apple Silicon only for now.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BASE="$(cd "${1:-build/w11d}" && pwd)"
OUT="${OUT:-build/w11s}"
SHARE="$OUT/share"
BIN=target/release/smbserve
[ -x "$BIN" ] || { echo "no $BIN (cargo build --release -p libsmb)"; exit 77; }
[ -f "$BASE/disk.qcow2" ] || { echo "no installed machine in $BASE"; exit 77; }

rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BASE/vars.fd" "$BASE/tpm.permall" "$OUT/"
build/qemu/qemu-img create -q -f qcow2 -b "$BASE/disk.qcow2" -F qcow2 "$OUT/disk.qcow2" || exit 1

# The host's tree, and its hashes for the guest to check its copy against.
python3 - "$SHARE" <<'EOF'
import hashlib, os, random, sys
root = sys.argv[1]
src = os.path.join(root, "hostsrc")
rnd = random.Random(11)
def put(rel, data):
    p = os.path.join(src, *rel.split("\\"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
put("café 日本語.txt", "unicode\r\n".encode())
put("spaces and.dots.in name.txt", b"spaces\r\n")
put("x" * 200 + ".txt", b"long\r\n")
put("\\".join("d%d" % i for i in range(10)) + "\\deep.txt", b"deep\r\n")
put("empty.txt", b"")
put("five-mb.bin", rnd.randbytes(5 << 20))
for i in range(3000):
    put("many\\f%04d.txt" % i, b"%d\r\n" % i)
lines = []
for d, _, files in os.walk(src):
    for n in files:
        p = os.path.join(d, n)
        rel = os.path.relpath(p, src).replace(os.sep, "\\")
        lines.append("%s\t%s" % (hashlib.sha256(open(p, "rb").read()).hexdigest().upper(), rel))
with open(os.path.join(root, "hostsrc.sha256"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("host tree: %d files" % len(lines))
EOF

# the host's side of the change-notification check: a new file every 3 s
mkdir -p "$SHARE/hostwatch"
( i=0; while sleep 3; do i=$((i + 1)); echo "$i" > "$SHARE/hostwatch/tick-$i.txt"; done ) &
TICK=$!

"$BIN" --unix "$OUT/smb.sock" --user smb --password smb --max-dialect "${MAX_DIALECT:-3.1.1}" -v "$SHARE" >"$OUT/smb.log" 2>&1 &
SP=$!
trap 'kill $SP $TICK 2>/dev/null' EXIT
sleep 1

OUT="$OUT" TPM_PPI="${TPM_PPI:-off}" ARCH=aarch64 NET=1 SMB="$ROOT/$OUT/smb.sock" \
  PROBE=1 PROBE_PS1=tools/win11-spike/smb-explorer.ps1 PROBE_WAIT=900 SETTLE=20 \
  python3 tools/win11-spike.py boot >"$OUT/run.out" 2>&1
kill $SP $TICK 2>/dev/null

fail=0
grep -o 'W11-PROBE .*' "$OUT/run.out" | sed 's/^W11-PROBE //' | tee "$OUT/probe.txt"
grep -q '^done' "$OUT/probe.txt" || { echo "FAIL: the probe never finished ($OUT/run.out, $OUT/shots)"; fail=1; }
grep -q '^FAIL' "$OUT/probe.txt" && fail=1

# The guest's upload, as it landed on the host.
python3 - "$SHARE" <<'EOF' || fail=1
import hashlib, os, sys
root = sys.argv[1]
man = os.path.join(root, "gsrc.sha256")
if not os.path.exists(man):
    print("FAIL host-check: no gsrc.sha256"); sys.exit(1)
bad = []
for line in open(man, encoding="utf-8-sig"):
    line = line.rstrip("\r\n")
    if not line:
        continue
    h, rel = line.split("\t", 1)
    p = os.path.join(root, "gsrc", *rel.split("\\"))
    if not os.path.exists(p) or hashlib.sha256(open(p, "rb").read()).hexdigest().upper() != h:
        bad.append(rel)
print("%s host-check: the guest's upload on the host%s" % ("FAIL" if bad else "PASS", " bad=" + ",".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
EOF

echo "== what the server refused or did not support"
grep -E "not supported|refused|bad signature|failed" "$OUT/smb.log" | sed 's/^smbserve: //;s/\[unix#[0-9]*\] //' | sort | uniq -c | sort -rn
grep -q "bad signature" "$OUT/smb.log" && { echo "FAIL: a signature did not verify"; fail=1; }
echo "requests: $(grep -c 'mid=' "$OUT/smb.log"); screenshot: $OUT/shots/desktop-hvf.png"
[ $fail = 0 ] && echo "smb-win11: pass"
exit $fail
