#!/usr/bin/env bash
# Build the firmware of a Windows 11 on Arm machine (track M20 step 4):
# EDK2's ArmVirtQemu for QEMU's aarch64 `virt` board, from the EDK2 commit
# QEMU's own `roms/edk2` submodule pins, with patches/edk2/ applied.
#
#   scripts/build-edk2.sh        build if anything changed, then install
#   scripts/build-edk2.sh -f     build again regardless
#
# Why not QEMU's prebuilt edk2-aarch64-code.fd (docs/tracks/m20-win11.md,
# "Step 4's results"):
#
#   - It has no Secure Boot, and Windows 11's setup refuses a PC that
#     cannot do it ("The PC must support Secure Boot"). Built here with
#     SECURE_BOOT_ENABLE: capable, off, no keys enrolled, as on x86_64.
#   - It has no AHCI driver, so it boots from nothing on AHCI, and the
#     disk would have to be on NVMe, which QEMU 9.2 cannot migrate (no
#     live snapshots). patches/edk2/01 adds the SATA and ATAPI drivers.
#   - Its ramfb offers 1024x768 at most and starts at 800x600, which is
#     the size Windows' Basic Display keeps. patches/edk2/02 adds sizes up
#     to 1920x1080; the board's preferred 1280x800 is now one of them.
#
# Everything else matches QEMU's own build of it (qemu/roms/edk2-build.config,
# `build.armvirt.aa64`): its options, its NX policy, RELEASE, silent.
#
# Out: build/edk2/2ksbox-aarch64-code.fd and 2ksbox-aarch64-vars.fd,
# padded to the 64 MiB the board's flash is, copied into qemu/pc-bios/ where
# the launcher's -L and the packagers look (bundle::Arch::efi_code_file).
#
# Needs clang and lld that can target aarch64 ELF (macOS: Homebrew's llvm
# and lld, build tools only; Linux: the distribution's), make, git, and a
# Python 3 (uv's 3.12, as for QEMU's configure). The first run fetches
# EDK2 and its submodules (~400 MB) into build/deps/src/edk2.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
FORCE=""
[ "${1:-}" = "-f" ] && FORCE=1

SRC=build/deps/src/edk2
OUT=build/edk2
PIN=$(git -C qemu ls-tree HEAD roms/edk2 | awk '{print $3}')
[ -n "$PIN" ] || { echo "build-edk2: qemu/ pins no roms/edk2 (is the qemu submodule checked out?)" >&2; exit 1; }

# The stamp: the pin, our patches and this script. Anything else changed
# is rebuilt with -f.
STAMP=$( { echo "$PIN"; cat patches/edk2/*.patch "$0"; } | shasum -a 256 | cut -c1-16)
if [ -z "$FORCE" ] && [ -f "$OUT/.stamp" ] && [ "$(cat "$OUT/.stamp")" = "$STAMP" ] \
   && [ -f "$OUT/2ksbox-aarch64-code.fd" ]; then
  echo "==> edk2: up to date ($PIN)"
else
  # --- tools ---------------------------------------------------------
  if [ "$(uname -s)" = Darwin ]; then
    # Apple's clang cannot link ELF; Homebrew's llvm and lld can
    LLVM=/opt/homebrew/opt/llvm/bin LLD=/opt/homebrew/opt/lld/bin
    [ -x "$LLVM/clang" ] && [ -x "$LLD/ld.lld" ] || {
      echo "build-edk2: needs Homebrew's llvm and lld (brew install llvm lld; build tools only)" >&2; exit 1; }
    export PATH="$LLVM:$LLD:$PATH" CLANGDWARF_BIN="$LLVM/"
  else
    command -v clang >/dev/null && command -v ld.lld >/dev/null || {
      echo "build-edk2: needs clang and lld" >&2; exit 1; }
    export CLANGDWARF_BIN="$(dirname "$(command -v clang)")/"
  fi
  PY="${QEMU_PYTHON:-$(uv python find 3.12 2>/dev/null || command -v python3)}"
  export PYTHON_COMMAND="$PY"

  # --- source --------------------------------------------------------
  if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)" != "$PIN" ]; then
    echo "==> edk2: fetching $PIN and its submodules"
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/tianocore/edk2.git
    git -C "$SRC" fetch -q --depth 1 origin "$PIN"
    git -C "$SRC" checkout -q FETCH_HEAD
    git -C "$SRC" submodule update -q --init --depth 1 --recommend-shallow
  fi
  # Our patches, onto the pinned tree every time (only tracked files are
  # restored: BaseTools' binaries and Build/ stay).
  git -C "$SRC" checkout -q -- .
  for p in patches/edk2/*.patch; do
    git -C "$SRC" apply "$ROOT/$p"
    echo "    $(basename "$p"): applied"
  done

  # --- BaseTools -----------------------------------------------------
  # The macOS SDK's stdint.h defines UINT8_MAX, which Decompress.c
  # defines again under -Werror (fixed upstream after this pin)
  if [ ! -x "$SRC/BaseTools/Source/C/bin/GenFw" ]; then
    echo "==> edk2: BaseTools"
    make -C "$SRC/BaseTools/Source/C" -j8 EXTRA_OPTFLAGS=-Wno-macro-redefined >/dev/null
  fi

  # --- ArmVirtQemu ---------------------------------------------------
  echo "==> edk2: ArmVirtQemu (AARCH64, CLANGDWARF, RELEASE, Secure Boot)"
  (
    cd "$SRC"
    export WORKSPACE="$PWD"
    # edksetup.sh reads unset variables
    set +u
    . ./edksetup.sh BaseTools >/dev/null
    build -q -a AARCH64 -t CLANGDWARF -b RELEASE -p ArmVirtPkg/ArmVirtQemu.dsc -n 8 \
      -D NETWORK_HTTP_BOOT_ENABLE=TRUE -D NETWORK_IP6_ENABLE=TRUE -D NETWORK_TLS_ENABLE=TRUE \
      -D NETWORK_ISCSI_ENABLE=TRUE -D NETWORK_ALLOW_HTTP_CONNECTIONS=TRUE \
      -D TPM2_ENABLE=TRUE -D TPM2_CONFIG_ENABLE=TRUE -D TPM1_ENABLE=TRUE -D CAVIUM_ERRATUM_27456=TRUE \
      -D DEBUG_PRINT_ERROR_LEVEL=0x80000000 \
      -D SECURE_BOOT_ENABLE=TRUE \
      --pcd PcdDxeNxMemoryProtectionPolicy=0xC000000000007FD1 --pcd PcdUninstallMemAttrProtocol=TRUE
  )
  FV="$SRC/Build/ArmVirtQemu-AARCH64/RELEASE_CLANGDWARF/FV"
  mkdir -p "$OUT"
  # The virt board's two flash devices are 64 MiB each, and QEMU wants
  # the image to be exactly that
  for pair in QEMU_EFI.fd:2ksbox-aarch64-code.fd QEMU_VARS.fd:2ksbox-aarch64-vars.fd; do
    cp "$FV/${pair%%:*}" "$OUT/${pair#*:}.part"
    truncate -s 64M "$OUT/${pair#*:}.part"
    mv "$OUT/${pair#*:}.part" "$OUT/${pair#*:}"
  done
  echo "$STAMP" > "$OUT/.stamp"
fi

# Next to QEMU's own firmware, every run: prepare-qemu.sh leaves untracked
# files in qemu/pc-bios alone, but a fresh qemu checkout has none.
for f in 2ksbox-aarch64-code.fd 2ksbox-aarch64-vars.fd; do
  cmp -s "$OUT/$f" "qemu/pc-bios/$f" || cp "$OUT/$f" "qemu/pc-bios/$f"
  echo "    qemu/pc-bios/$f"
done
