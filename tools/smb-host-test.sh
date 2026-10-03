#!/usr/bin/env bash
# libsmb against a real SMB client on this host, no guest (track M23).
# smbserve serves a scratch folder on localhost TCP, once per dialect
# (2.1 with HMAC-SHA256 signing, 3.1.1 with AES-CMAC and the preauth
# hash); the host's client mounts it with signing required, reads,
# writes, copies, renames and deletes, and the server's log must show the
# login and no signature it could not verify. With macOS's client also
# leases: one is granted, and a read after the file changed on the host
# returns the new contents (the server broke the lease).
#
#   tools/smb-host-test.sh [out]     (default build/test/smb)
#
# The client: macOS's mount_smbfs (no root needed), else Samba's
# smbclient. Neither: exit 77 (a skip in scripts/test.sh).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/test/smb}"
BIN="$ROOT/target/release/smbserve"
PORT="${SMB_PORT:-14450}"
[ -x "$BIN" ] || { echo "no $BIN (cargo build --release -p libsmb)"; exit 77; }
if [ "$(uname)" = Darwin ]; then CLIENT=mount_smbfs
elif command -v smbclient >/dev/null; then CLIENT=smbclient
else echo "no SMB client (mount_smbfs or smbclient)"; exit 77; fi

fail=0
bad() { echo "FAIL: $*"; fail=1; }

one() { # dialect
  local d="$1" share="$OUT/share-$1" mnt="$OUT/mnt-$1" log="$OUT/server-$1.log"
  rm -rf "$share" && mkdir -p "$share/sub" "$mnt"
  echo "hello from the host" > "$share/hello.txt"
  echo nested > "$share/sub/inner.txt"
  "$BIN" --tcp "127.0.0.1:$PORT" --max-dialect "$d" -v "$share" >"$log" 2>&1 &
  local pid=$!
  for _ in $(seq 50); do grep -q listening "$log" && break; sleep 0.1; done
  echo "== dialect $d ($CLIENT)"
  if [ $CLIENT = mount_smbfs ]; then
    mount_smbfs -o nobrowse "//smb:smb@127.0.0.1:$PORT/host" "$mnt" || { bad "$d: mount"; kill $pid; return; }
    [ "$(cat "$mnt/hello.txt")" = "hello from the host" ] || bad "$d: read"
    [ "$(cat "$mnt/sub/inner.txt")" = nested ] || bad "$d: read in a folder"
    head -c 3000000 /dev/urandom > "$OUT/blob"
    cp "$OUT/blob" "$mnt/blob" && cmp -s "$OUT/blob" "$share/blob" || bad "$d: write 3 MB"
    cmp -s "$OUT/blob" "$mnt/blob" || bad "$d: read 3 MB back"
    mv "$mnt/blob" "$mnt/blob2" && [ -f "$share/blob2" ] && [ ! -e "$share/blob" ] || bad "$d: rename"
    rm "$mnt/blob2" && [ ! -e "$share/blob2" ] || bad "$d: delete"
    mkdir "$mnt/d" && rmdir "$mnt/d" && [ ! -e "$share/d" ] || bad "$d: mkdir / rmdir"
    ls "$mnt" | grep -qx sub || bad "$d: listing"
    smbutil statshares -m "$mnt" | tee "$OUT/statshares-$d.txt" | grep -q "SIGNING_ON *TRUE" || bad "$d: not signed"
    # a lease's cached copy does not outlive a change on the host: read
    # through one open, change the file under the share, read again
    echo one > "$share/live.txt"
    python3 - "$mnt/live.txt" "$share/live.txt" <<'PY' || bad "$d: a cached read outlived a change on the host"
import sys, time
f = open(sys.argv[1]); f.read(); f.seek(0); f.read()
open(sys.argv[2], "w").write("two\n")
time.sleep(3)
f.seek(0); sys.exit(0 if f.read() == "two\n" else 1)
PY
    umount "$mnt" || bad "$d: unmount"
  else
    local c=(smbclient "//127.0.0.1/host" -p "$PORT" -U smb%smb --option="client signing=required"
             --option="client max protocol=SMB$( [ "$d" = 2.1 ] && echo 2_10 || echo 3_11 )")
    "${c[@]}" -c 'get hello.txt '"$OUT/got.txt" && [ "$(cat "$OUT/got.txt")" = "hello from the host" ] || bad "$d: read"
    head -c 3000000 /dev/urandom > "$OUT/blob"
    "${c[@]}" -c "put $OUT/blob blob; rename blob blob2; get blob2 $OUT/blob.back; del blob2; mkdir d; rmdir d" \
      && cmp -s "$OUT/blob" "$OUT/blob.back" && [ ! -e "$share/blob2" ] && [ ! -e "$share/d" ] || bad "$d: write / rename / delete"
  fi
  kill $pid; wait $pid 2>/dev/null
  grep -q "logged in as" "$log" || bad "$d: no login in the server's log"
  grep -q "dialect 0x0$(echo "$d" | tr -d . | sed 's/^2$/202/;s/^21$/210/')" "$log" || bad "$d: not the dialect asked for"
  ! grep -q "bad signature" "$log" || bad "$d: a request's signature did not verify"
  if [ $CLIENT = mount_smbfs ]; then
    grep -q "read-caching on" "$log" || bad "$d: no lease granted"
    grep -q "changed on the host" "$log" || bad "$d: no lease broken for the host's change"
  fi
}

mkdir -p "$OUT"
one 2.1
one 3.1.1
[ $fail = 0 ] && echo "smb: both dialects pass"
exit $fail
