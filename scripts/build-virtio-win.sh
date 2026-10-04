#!/usr/bin/env bash
# The drivers disc of a Windows 11 machine (tracks M20 and M23), one per
# processor, cut from Red Hat's virtio-win ISO, pinned by version and
# sha256 here, plus the 2ksbox agent and its installer:
#
# - arm64, Windows 11 on Arm: the ARM64 Windows 11 builds of virtio-win's
#   network driver (NetKVM, for the board's virtio-net), display driver
#   (viogpudo, for virtio-gpu) and virtio-serial driver (vioser, the
#   clipboard's channel to QEMU's qemu-vdagent). Windows on Arm has none
#   of the three in the box.
# - x64, Windows 11 on a PC: vioser alone. The q35's network card
#   (e1000e) and display have drivers in the box; virtio-serial has none.
#
#   scripts/build-virtio-win.sh [-f] [arm64|x64]...
#       the discs named (default: the host's own processor's), each only
#       if something changed; -f builds them again regardless
#
# None of the drivers can be built here: Windows loads a kernel driver
# only with Microsoft's signature, and these carry it ("Microsoft Windows
# Hardware Compatibility Publisher" on each catalog). The binaries are
# under the ISO's own BSD-3-Clause license (virtio-win_license.txt),
# which each disc carries, as its redistribution clause asks.
#
# The drivers sit under `$WinPEDriver$` at the disc's root: Windows Setup
# looks for that folder on every drive and installs what is in it into the
# new system, so a machine installed with the disc in has its drivers
# without a click. On an installed Windows, 2ksbox\install.cmd adds them
# (and the agent).
#
# Out: build/virtio-win/2ksbox-drivers-<arm64|x64>.iso, which the
# launcher puts in a CD drive of its own on every Windows 11 machine of
# that processor (`disc_library::drivers_iso`, `bundle::Machine`'s
# `modern_args` and `arm_args`). The ISO they are cut from is kept in
# build/deps/src (877 MB, downloaded once).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION=0.1.302
SHA256=303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d
URL="https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-$VERSION-1/virtio-win-$VERSION.iso"
SRC=build/deps/src/virtio-win-$VERSION.iso
OUT=build/virtio-win

FORCE=""
ARCHES=()
for a in "$@"; do
  case "$a" in
    -f) FORCE=1 ;;
    arm64|x64) ARCHES+=("$a") ;;
    *) echo "usage: scripts/build-virtio-win.sh [-f] [arm64|x64]..." >&2; exit 2 ;;
  esac
done
if [ ${#ARCHES[@]} = 0 ]; then
  case "$(uname -m)" in arm64|aarch64) ARCHES=(arm64) ;; *) ARCHES=(x64) ;; esac
fi

# The agent and its installer (track M23) are on both discs, built here
# from guest-agent/ for x64 (Windows 11 on Arm runs it under emulation;
# rustup's x86_64-pc-windows-gnu and mingw-w64 build it).
AGENT=guest-agent/target/x86_64-pc-windows-gnu/release/2ksbox-agent.exe
STAMP=$( { echo "$VERSION $SHA256"; cat "$0"; cat guest-agent/Cargo.toml guest-agent/src/*.rs guest-agent/install.*; } \
  | shasum -a 256 | cut -c1-16)

TODO=()
for arch in "${ARCHES[@]}"; do
  if [ -z "$FORCE" ] && [ -f "$OUT/2ksbox-drivers-$arch.iso" ] && [ "$(cat "$OUT/.stamp-$arch" 2>/dev/null)" = "$STAMP" ]; then
    echo "==> virtio-win: $arch up to date ($VERSION)"
  else
    TODO+=("$arch")
  fi
done
[ ${#TODO[@]} = 0 ] && exit 0
command -v xorriso >/dev/null || { echo "build-virtio-win: needs xorriso" >&2; exit 1; }

if ! [ -f "$SRC" ] || [ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" != "$SHA256" ]; then
  echo "==> virtio-win: downloading $VERSION"
  mkdir -p "$(dirname "$SRC")"
  curl -fL --retry 3 -o "$SRC.part" "$URL"
  got=$(shasum -a 256 "$SRC.part" | cut -d' ' -f1)
  [ "$got" = "$SHA256" ] || { echo "build-virtio-win: $URL is $got, not $SHA256" >&2; rm -f "$SRC.part"; exit 1; }
  mv "$SRC.part" "$SRC"
fi

echo "==> the 2ksbox agent (x64)"
( cd guest-agent && cargo build --release --locked --target x86_64-pc-windows-gnu ) || {
  echo "build-virtio-win: the agent did not build (rustup target add x86_64-pc-windows-gnu; mingw-w64)" >&2; exit 1; }

# disc <arch>: the drivers (virtio-win folder:disc folder pairs, each with
# the files that must be there), the agent, a README, the ISO
disc() {
  local arch=$1 dir title drivers=() files=() readme
  case "$arch" in
    arm64)
      dir=ARM64; title="Windows 11 on Arm"
      drivers=(NetKVM viogpudo vioserial)
      files=(NetKVM/netkvm viogpudo/viogpudo vioserial/vioser)
      readme="  \$WinPEDriver\$\\NetKVM     the network card (Red Hat VirtIO Ethernet Adapter)
  \$WinPEDriver\$\\viogpudo   the display (Red Hat VirtIO GPU DOD controller)
  \$WinPEDriver\$\\vioserial  the clipboard's channel to the host (VirtIO Serial)" ;;
    x64)
      dir=amd64; title="Windows 11"
      drivers=(vioserial)
      files=(vioserial/vioser)
      readme="  \$WinPEDriver\$\\vioserial  the clipboard's channel to the host (VirtIO Serial)" ;;
  esac
  local iso=$OUT/2ksbox-drivers-$arch.iso tree=$OUT/tree-$arch
  echo "==> virtio-win: the $dir ${drivers[*]} for Windows 11"
  # what xorriso extracts keeps the disc's read-only modes
  [ -d "$tree" ] && chmod -R u+w "$tree"
  rm -rf "$tree"
  mkdir -p "$tree/\$WinPEDriver\$"
  local d pairs=(/virtio-win_license.txt:virtio-win_license.txt) pair
  for d in "${drivers[@]}"; do pairs+=("/$d/w11/$dir:\$WinPEDriver\$/$d"); done
  for pair in "${pairs[@]}"; do
    xorriso -osirrox on -indev "$SRC" -extract "${pair%%:*}" "$tree/${pair#*:}" 2>&1 \
      | grep -i -E "failure|sorry" >&2 && exit 1
    chmod -R u+w "$tree"
  done
  # Debug symbols: most of the size, and no use to anyone without a debugger
  find "$tree" -name '*.pdb' -delete
  local f ext
  for f in "${files[@]}"; do
    for ext in inf cat sys; do
      [ -f "$tree/\$WinPEDriver\$/$f.$ext" ] || { echo "build-virtio-win: virtio-win $VERSION has no $dir $f.$ext" >&2; exit 1; }
    done
  done
  mkdir -p "$tree/2ksbox"
  cp "$AGENT" guest-agent/install.ps1 "$tree/2ksbox/"
  # cmd.exe misreads a batch file's parenthesized blocks with LF line ends
  sed 's/$/\r/' guest-agent/install.cmd > "$tree/2ksbox/install.cmd"
  cat > "$tree/README.txt" <<EOF
2ksbox: drivers for $title

From virtio-win $VERSION (Red Hat), the $dir builds for Windows 11:

$readme

Windows Setup installs them by itself when this disc is in a drive.

2ksbox\\install.cmd installs the drivers on an installed Windows, and
2ksbox's agent: the clipboard shared with the host, and the host's
shared folder on a drive letter when the machine has one. Run it once.

License: virtio-win_license.txt (BSD-3-Clause).
EOF
  xorriso -as mkisofs -quiet -o "$iso.part" -V 2KSBOX_DRIVERS -J -joliet-long -r "$tree"
  mv "$iso.part" "$iso"
  chmod -R u+w "$tree"; rm -rf "$tree"
  echo "$STAMP" > "$OUT/.stamp-$arch"
  echo "    $iso ($(du -h "$iso" | cut -f1))"
}

mkdir -p "$OUT"
# the single stamp from before there were two discs
rm -f "$OUT/.stamp"
for arch in "${TODO[@]}"; do disc "$arch"; done
