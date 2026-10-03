#!/usr/bin/env bash
# The drivers disc of a Windows 11 on Arm machine (track M20): the ARM64
# Windows 11 builds of virtio-win's network driver (NetKVM, for the
# board's virtio-net), display driver (viogpudo, for virtio-gpu) and
# virtio-serial driver (vioser, the clipboard's channel to QEMU's
# qemu-vdagent, track M23), taken from Red Hat's virtio-win ISO, pinned by
# version and sha256 here.
#
#   scripts/build-virtio-win.sh       build if anything changed
#   scripts/build-virtio-win.sh -f    build again regardless
#
# Windows on Arm has no driver in the box for any of the three, and none
# can be built here: Windows loads a kernel driver only with Microsoft's
# signature, and these carry it ("Microsoft Windows Hardware
# Compatibility Publisher" on each catalog). The binaries are under the
# ISO's own BSD-3-Clause license (virtio-win_license.txt), which the disc
# carries, as its redistribution clause asks.
#
# The drivers sit under `$WinPEDriver$` at the disc's root: Windows Setup
# looks for that folder on every drive and installs what is in it into the
# new system, so a machine installed with the disc in has its network
# without a click. On an installed Windows, Device Manager's "Browse my
# computer for drivers" on the disc (subfolders included) finds them too.
#
# Out: build/virtio-win/2ksbox-drivers-arm64.iso, which the launcher puts
# in a second CD drive on every Windows 11 on Arm machine
# (`disc_library::arm_drivers_iso`, `bundle::Machine::arm_args`). The
# ISO it is cut from is kept in build/deps/src (877 MB, downloaded once).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION=0.1.302
SHA256=303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d
URL="https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-$VERSION-1/virtio-win-$VERSION.iso"
SRC=build/deps/src/virtio-win-$VERSION.iso
OUT=build/virtio-win
ISO=$OUT/2ksbox-drivers-arm64.iso

# The agent and its installer (track M23) are on the disc too, built here
# from guest-agent/ for x64 (Windows 11 on Arm runs it under emulation;
# rustup's x86_64-pc-windows-gnu and mingw-w64 build it).
AGENT=guest-agent/target/x86_64-pc-windows-gnu/release/2ksbox-agent.exe
STAMP=$( { echo "$VERSION $SHA256"; cat "$0"; cat guest-agent/Cargo.toml guest-agent/src/*.rs guest-agent/install.*; } \
  | shasum -a 256 | cut -c1-16)
if [ "${1:-}" != "-f" ] && [ -f "$ISO" ] && [ "$(cat "$OUT/.stamp" 2>/dev/null)" = "$STAMP" ]; then
  echo "==> virtio-win: up to date ($VERSION)"
  exit 0
fi
command -v xorriso >/dev/null || { echo "build-virtio-win: needs xorriso" >&2; exit 1; }

if ! [ -f "$SRC" ] || [ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" != "$SHA256" ]; then
  echo "==> virtio-win: downloading $VERSION"
  mkdir -p "$(dirname "$SRC")"
  curl -fL --retry 3 -o "$SRC.part" "$URL"
  got=$(shasum -a 256 "$SRC.part" | cut -d' ' -f1)
  [ "$got" = "$SHA256" ] || { echo "build-virtio-win: $URL is $got, not $SHA256" >&2; rm -f "$SRC.part"; exit 1; }
  mv "$SRC.part" "$SRC"
fi

echo "==> virtio-win: the ARM64 NetKVM, viogpudo and vioser for Windows 11"
TREE=$OUT/tree
# what xorriso extracts keeps the disc's read-only modes
[ -d "$TREE" ] && chmod -R u+w "$TREE"
rm -rf "$TREE"
mkdir -p "$TREE/\$WinPEDriver\$"
for pair in /NetKVM/w11/ARM64:\$WinPEDriver\$/NetKVM /viogpudo/w11/ARM64:\$WinPEDriver\$/viogpudo \
            /vioserial/w11/ARM64:\$WinPEDriver\$/vioserial \
            /virtio-win_license.txt:virtio-win_license.txt; do
  xorriso -osirrox on -indev "$SRC" -extract "${pair%%:*}" "$TREE/${pair#*:}" 2>&1 \
    | grep -i -E "failure|sorry" >&2 && exit 1
  chmod -R u+w "$TREE"
done
# Debug symbols: 18 of the 20 MB, and no use to anyone without a debugger
find "$TREE" -name '*.pdb' -delete
for f in NetKVM/netkvm.inf NetKVM/netkvm.cat NetKVM/netkvm.sys viogpudo/viogpudo.inf viogpudo/viogpudo.cat viogpudo/viogpudo.sys \
         vioserial/vioser.inf vioserial/vioser.cat vioserial/vioser.sys; do
  [ -f "$TREE/\$WinPEDriver\$/$f" ] || { echo "build-virtio-win: virtio-win $VERSION has no ARM64 $f" >&2; exit 1; }
done
echo "==> the 2ksbox agent (x64)"
( cd guest-agent && cargo build --release --locked --target x86_64-pc-windows-gnu ) || {
  echo "build-virtio-win: the agent did not build (rustup target add x86_64-pc-windows-gnu; mingw-w64)" >&2; exit 1; }
mkdir -p "$TREE/2ksbox"
cp "$AGENT" guest-agent/install.ps1 "$TREE/2ksbox/"
# cmd.exe misreads a batch file's parenthesized blocks with LF line ends
sed 's/$/\r/' guest-agent/install.cmd > "$TREE/2ksbox/install.cmd"
cat > "$TREE/README.txt" <<EOF
2ksbox: drivers for Windows 11 on Arm

From virtio-win $VERSION (Red Hat), the ARM64 builds for Windows 11:

  \$WinPEDriver\$\\NetKVM     the network card (Red Hat VirtIO Ethernet Adapter)
  \$WinPEDriver\$\\viogpudo   the display (Red Hat VirtIO GPU DOD controller)
  \$WinPEDriver\$\\vioserial  the clipboard's channel to the host (VirtIO Serial)

Windows Setup installs them by itself when this disc is in a drive.

2ksbox\\install.cmd installs the drivers on an installed Windows, and
2ksbox's agent: the clipboard shared with the host, and the host's
shared folder on a drive letter when the machine has one. Run it once.

License: virtio-win_license.txt (BSD-3-Clause).
EOF
mkdir -p "$OUT"
xorriso -as mkisofs -quiet -o "$ISO.part" -V 2KSBOX_DRIVERS -J -joliet-long -r "$TREE"
mv "$ISO.part" "$ISO"
chmod -R u+w "$TREE"; rm -rf "$TREE"
echo "$STAMP" > "$OUT/.stamp"
echo "    $ISO ($(du -h "$ISO" | cut -f1))"
