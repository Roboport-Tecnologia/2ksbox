#!/usr/bin/env bash
# Build the VGA BIOSes 2ksbox ships in place of QEMU's prebuilt ones:
# SeaBIOS's VGA BIOS (qemu/roms/seabios, the version QEMU 9.2.4 pins) with
# our patch queue in patches/seabios/, for the two variants a 2ksbox
# machine loads, `stdvga` (-vga std, and d3dpt-vga, whose romfile it is)
# and `cirrus`. Output: firmware/vgabios-<variant>.bin, which is checked
# in, because SeaBIOS needs gcc and GNU ld for x86 and neither the Mac
# nor the Flatpak SDK is asked to have them. prepare-qemu.sh copies these
# over qemu/pc-bios/, which is what every package ships.
#
# Rerun after changing a patch in patches/seabios/ and commit the blobs
# with it. Needs a Linux host with gcc that can do -m32 (CROSS_PREFIX=
# x86_64-linux-gnu- on a non-x86 one) and python3. The guest-side proof is
# `VBEPAL=1 tools/vga-dirty-guest-test.py vesa` (the vbe-palette check in
# scripts/test.sh): a rebuild on another compiler is not byte-identical,
# so the check is what the ROM does, not what it hashes to.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEABIOS="$ROOT/qemu/roms/seabios"
WORK="$ROOT/build/vgabios"
VARIANTS="stdvga cirrus"

[ -f "$SEABIOS/Makefile" ] || { echo "qemu/roms/seabios missing (git -C qemu submodule update --init roms/seabios)"; exit 1; }
[ "$(uname -s)" = Linux ] || { echo "build-vgabios.sh: Linux only (SeaBIOS wants gcc -m32 and GNU ld); firmware/ is checked in"; exit 1; }

# A fresh copy of the pinned tree every run, so the submodule itself is
# never patched and a removed patch cannot linger. With no .git in it,
# SeaBIOS takes its version from .version, and with EXTRAVERSION set it
# calls the build clean, with no build time or host name in the version
# string. A guest can read that string in the ROM's banner (INT 10h
# 4F00h's OEM product revision does not carry it).
rm -rf "$WORK"
mkdir -p "$WORK/src"
git -C "$SEABIOS" archive --format=tar HEAD | tar -x -C "$WORK/src"
git -C "$SEABIOS" describe --tags --long --always > "$WORK/src/.version"

for p in "$ROOT"/patches/seabios/*.patch; do
  [ -e "$p" ] || continue
  # `patch`, not `git apply`: this copy sits inside our own work tree, where
  # git apply would resolve the paths against the repository root.
  patch -s -p1 -d "$WORK/src" --no-backup-if-mismatch < "$p"
  echo "    $(basename "$p"): applied"
done

for v in $VARIANTS; do
  out="$WORK/vga-$v"
  mkdir -p "$out"
  cp "$ROOT/qemu/roms/config.vga-$v" "$out/.config"
  mk=(make -s -C "$WORK/src" EXTRAVERSION=-2ksbox CROSS_PREFIX="${CROSS_PREFIX:-}"
      KCONFIG_CONFIG="$out/.config" OUT="$out/")
  "${mk[@]}" oldnoconfig >/dev/null 2>&1
  "${mk[@]}" "$out/vgabios.bin" >"$out/build.log" 2>&1 || { cat "$out/build.log"; exit 1; }
  mkdir -p "$ROOT/firmware"
  cp "$out/vgabios.bin" "$ROOT/firmware/vgabios-$v.bin"
  echo "    firmware/vgabios-$v.bin: $(wc -c < "$out/vgabios.bin") bytes, $(strings -n 8 "$out/vgabios.bin" | grep -m1 '^rel-')"
done
