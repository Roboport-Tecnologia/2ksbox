#!/usr/bin/env bash
# Build the firmware of a Windows 11 machine (track M20) from the EDK2
# commit QEMU's own `roms/edk2` submodule pins, with patches/edk2/ applied:
#
#   aarch64  (Arm hosts, step 4) ArmVirtQemu for QEMU's aarch64 `virt`
#            board, with clang and lld
#   x86_64   (Windows hosts, step 5) OvmfPkgX64 for the q35, with Visual
#            Studio (EDK2's VS2022 toolchain), MINGW64's nasm and an iasl
#            built here from ACPICA's pinned source
#
#   scripts/build-edk2.sh [-f] [aarch64|x86_64]
#       the host's own firmware by default (x86_64 on Windows, aarch64
#       elsewhere); build if anything changed, then install; -f builds
#       again regardless
#
# Why not QEMU's prebuilt aarch64 firmware (docs/tracks/m20-win11.md,
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
# Why not QEMU's prebuilt x86_64 firmware on a Windows host: its Secure
# Boot build requires SMM, which WHPX does not have ("System Management
# Mode not supported by this hypervisor"), and its other build has no
# Secure Boot. Built here with SECURE_BOOT_ENABLE and SMM_REQUIRE=FALSE:
# Secure Boot capable, its variables unprotected from the guest's own
# kernel, which nothing here needs.
#
# Everything else matches QEMU's own builds (qemu/roms/edk2-build.config,
# `build.armvirt.aa64` and `build.ovmf.x86_64.secure`): their options,
# RELEASE; Arm's NX policy and silence.
#
# Out: build/edk2/2ksbox-<arch>-code.fd and 2ksbox-<arch>-vars.fd (Arm's
# padded to the 64 MiB the board's flash is), copied into qemu/pc-bios/
# where the launcher's -L and the packagers look
# (bundle::Arch::efi_code_file).
#
# Needs, for aarch64: clang and lld that can target aarch64 ELF (macOS:
# Homebrew's llvm and lld, build tools only; Linux: the distribution's),
# make. For x86_64 (MSYS2's MINGW64 shell): Visual Studio's C++ tools,
# MINGW64's nasm, MSYS2's bison and flex (iasl's build; build-windows.sh
# --msys2-deps). Both: git and a Python 3 (uv's 3.12, as for QEMU's
# configure). The first run fetches EDK2 and its submodules (~400 MB) into
# build/deps/src/edk2.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
case "$(uname -s)" in MINGW*|MSYS*) WINDOWS=1 ;; *) WINDOWS="" ;; esac
FORCE="" ARCH=""
for a in "$@"; do
  case "$a" in
    -f) FORCE=1 ;;
    aarch64|x86_64) ARCH=$a ;;
    *) echo "usage: scripts/build-edk2.sh [-f] [aarch64|x86_64]" >&2; exit 2 ;;
  esac
done
[ -n "$ARCH" ] || { [ -n "$WINDOWS" ] && ARCH=x86_64 || ARCH=aarch64; }
if [ "$ARCH" = x86_64 ] && [ -z "$WINDOWS" ]; then
  echo "build-edk2: x86_64 is a Windows host's firmware (elsewhere QEMU's secure build runs)" >&2; exit 2
fi

SRC=build/deps/src/edk2
OUT=build/edk2
PIN=$(git -C qemu ls-tree HEAD roms/edk2 | awk '{print $3}')
[ -n "$PIN" ] || { echo "build-edk2: qemu/ pins no roms/edk2 (is the qemu submodule checked out?)" >&2; exit 1; }
# MSYS2 has sha256sum, not Perl's shasum
command -v shasum >/dev/null || shasum() { shift 2; sha256sum "$@"; }

# iasl for the x86_64 build (OvmfPkg's ACPI tables): ACPICA's own, pinned,
# built once with Visual Studio from its msvc2017 projects (retargeted),
# whose grammar steps run MSYS2's bison and flex
ACPICA=R2025_04_04
ACPICA_SHA=9991ec103b3660d17715780406ee7409f705cf87ac55e9a32374affe1a6f275a
IASL_DIR=build/deps/iasl-$ACPICA

# The stamp: the pin, our patches, this script (and for x86_64 iasl's
# version). Anything else changed is rebuilt with -f. Arm's keeps its old
# file name.
STAMP=$( { echo "$PIN $ARCH"; [ "$ARCH" = aarch64 ] || echo "$ACPICA"; cat patches/edk2/*.patch "$0"; } \
  | shasum -a 256 | cut -c1-16)
STAMPFILE=$OUT/.stamp; [ "$ARCH" = aarch64 ] || STAMPFILE=$OUT/.stamp-$ARCH
if [ -z "$FORCE" ] && [ -f "$STAMPFILE" ] && [ "$(cat "$STAMPFILE")" = "$STAMP" ] \
   && [ -f "$OUT/2ksbox-$ARCH-code.fd" ]; then
  echo "==> edk2: $ARCH up to date ($PIN)"
else
  # --- tools ---------------------------------------------------------
  if [ "$ARCH" = x86_64 ]; then
    NASM="${MINGW_PREFIX:-/mingw64}/bin/nasm.exe"
    [ -x "$NASM" ] || { echo "build-edk2: needs MINGW64's nasm (pacman -S mingw-w64-x86_64-nasm)" >&2; exit 1; }
    for t in bison flex; do
      [ -x "/usr/bin/$t.exe" ] || { echo "build-edk2: needs MSYS2's $t (pacman -S $t), for iasl" >&2; exit 1; }
    done
    MSBUILD="$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' \
      -find 'MSBuild\**\Bin\MSBuild.exe' 2>/dev/null | head -1 | tr -d '\r')"
    [ -n "$MSBUILD" ] || { echo "build-edk2: no Visual Studio with MSBuild (vswhere found none)" >&2; exit 1; }
    MSBUILD="$(cygpath -u "$MSBUILD")"
  elif [ "$(uname -s)" = Darwin ]; then
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
  if [ "$ARCH" = x86_64 ]; then
    # EDK2's Windows build wants a Windows Python (MINGW64's fails in
    # BaseTools' NmakeSubdirs.py): the Python launcher's 3.12. MSYS2's
    # login shell has none of Windows' PATH, so the launcher is looked
    # for where python.org's installer puts it too.
    PYL="$(command -v py || true)"
    for c in "$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")/Programs/Python/Launcher/py.exe" /c/Windows/py.exe; do
      [ -n "$PYL" ] || { [ -x "$c" ] && PYL="$c"; } || true
    done
    PY="$([ -z "$PYL" ] || "$PYL" -3.12 -c 'import sys; print(sys.executable)' 2>/dev/null | tr -d '\r')"
    [ -n "$PY" ] || { echo "build-edk2: needs a Windows Python 3.12 (py -3.12)" >&2; exit 1; }
  else
    PY="${QEMU_PYTHON:-$(uv python find 3.12 2>/dev/null || command -v python3)}"
  fi
  export PYTHON_COMMAND="$PY"

  # --- iasl (x86_64) -------------------------------------------------
  if [ "$ARCH" = x86_64 ] && [ ! -x "$IASL_DIR/iasl.exe" ]; then
    tgz=build/deps/src/acpica-$ACPICA.tar.gz
    if ! [ -f "$tgz" ] || [ "$(sha256sum "$tgz" | cut -d' ' -f1)" != "$ACPICA_SHA" ]; then
      echo "==> edk2: fetching ACPICA $ACPICA (iasl)"
      mkdir -p build/deps/src
      curl -fL --retry 3 -s -o "$tgz.part" "https://github.com/acpica/acpica/archive/refs/tags/$ACPICA.tar.gz"
      [ "$(sha256sum "$tgz.part" | cut -d' ' -f1)" = "$ACPICA_SHA" ] || {
        echo "build-edk2: ACPICA $ACPICA is not $ACPICA_SHA" >&2; rm -f "$tgz.part"; exit 1; }
      mv "$tgz.part" "$tgz"
    fi
    echo "==> edk2: iasl (ACPICA $ACPICA, Visual Studio)"
    rm -rf "build/deps/src/acpica-$ACPICA"
    tar -xzf "$tgz" -C build/deps/src
    # Its browse database step fails after the compiler is linked (an
    # absolute /acpica path), so the exe is what decides
    ( cd "build/deps/src/acpica-$ACPICA/generate/msvc2017" \
      && PATH="$PATH:/usr/bin" "$MSBUILD" AslCompiler.vcxproj -nologo -v:quiet -p:Configuration=Release \
           -p:Platform=Win32 -p:PlatformToolset=v143 -p:WindowsTargetPlatformVersion=10.0 >/dev/null 2>&1 || true )
    exe="build/deps/src/acpica-$ACPICA/generate/msvc2017/AslCompiler/AslCompiler.exe"
    [ -x "$exe" ] || { echo "build-edk2: iasl did not build (msbuild in build/deps/src/acpica-$ACPICA/generate/msvc2017)" >&2; exit 1; }
    mkdir -p "$IASL_DIR"
    cp "$exe" "$IASL_DIR/iasl.exe"
  fi

  # --- source --------------------------------------------------------
  # long paths: some of EDK2's submodules nest deep
  GIT=(git); [ -z "$WINDOWS" ] || GIT=(git -c core.longpaths=true)
  if [ "$("${GIT[@]}" -C "$SRC" rev-parse HEAD 2>/dev/null || true)" != "$PIN" ]; then
    echo "==> edk2: fetching $PIN and its submodules"
    rm -rf "$SRC"
    "${GIT[@]}" init -q "$SRC"
    [ -z "$WINDOWS" ] || git -C "$SRC" config core.longpaths true
    "${GIT[@]}" -C "$SRC" remote add origin https://github.com/tianocore/edk2.git
    "${GIT[@]}" -C "$SRC" fetch -q --depth 1 origin "$PIN"
    "${GIT[@]}" -C "$SRC" checkout -q FETCH_HEAD
    "${GIT[@]}" -C "$SRC" submodule update -q --init --depth 1 --recommend-shallow
  fi
  # Our patches, onto the pinned tree every time (only tracked files are
  # restored: BaseTools' binaries and Build/ stay).
  "${GIT[@]}" -C "$SRC" checkout -q -- .
  for p in patches/edk2/*.patch; do
    "${GIT[@]}" -C "$SRC" apply "$ROOT/$p"
    echo "    $(basename "$p"): applied"
  done

  if [ "$ARCH" = aarch64 ]; then
    # --- BaseTools ---------------------------------------------------
    # The macOS SDK's stdint.h defines UINT8_MAX, which Decompress.c
    # defines again under -Werror (fixed upstream after this pin)
    if [ ! -x "$SRC/BaseTools/Source/C/bin/GenFw" ]; then
      echo "==> edk2: BaseTools"
      make -C "$SRC/BaseTools/Source/C" -j8 EXTRA_OPTFLAGS=-Wno-macro-redefined >/dev/null
    fi

    # --- ArmVirtQemu -------------------------------------------------
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
  else
    # --- OvmfPkgX64 --------------------------------------------------
    # EDK2's Windows build runs in cmd: Visual Studio's x86 environment
    # (vcvars32: BaseTools are 32-bit programs, with Ia32's UINTN, and the
    # VS2022 toolchain finds its own X64 compilers for the firmware),
    # edksetup.bat (which builds BaseTools with nmake the first time),
    # then build. A batch file of ours does the three.
    # vcvars changes the directory, so every path it gets is absolute,
    # and EDK2's setup runs vswhere, which is not on PATH. EDK2's scripts
    # find each other in the current directory, which cmd skips when
    # NoDefaultCurrentDirectoryInExePath is set (as some shells do).
    echo "==> edk2: OvmfPkgX64 (X64, VS2022, RELEASE, Secure Boot without SMM)"
    VCVARS="$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' \
      -find 'VC\Auxiliary\Build\vcvars32.bat' 2>/dev/null | head -1 | tr -d '\r')"
    [ -n "$VCVARS" ] || { echo "build-edk2: no vcvars32.bat (Visual Studio's C++ tools)" >&2; exit 1; }
    REBUILD=""; [ -f "$SRC/BaseTools/Bin/Win32/GenFw.exe" ] || REBUILD=Rebuild
    mkdir -p "$OUT"
    # The values are written into the batch file: cmd /c strips the first
    # and last quote of a line with several quoted arguments.
    bat="$OUT/build-x64.bat"
    {
      echo '@echo off'
      echo "call \"$VCVARS\" >nul || exit /b 1"
      echo 'set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"'
      echo "set \"WORKSPACE=$(cygpath -aw "$SRC")\""
      echo "set \"PYTHON_COMMAND=$(cygpath -aw "$PY")\""
      echo "set \"NASM_PREFIX=$(cygpath -aw "$(dirname "$NASM")")\\\""
      echo "set \"IASL_PREFIX=$(cygpath -aw "$IASL_DIR")\\\""
      echo 'set NoDefaultCurrentDirectoryInExePath='
      echo 'cd /d "%WORKSPACE%" || exit /b 1'
      echo "call \"%WORKSPACE%\edksetup.bat\" $REBUILD VS2022 >nul || exit /b 1"
      echo 'call build -q -a X64 -t VS2022 -b RELEASE -p OvmfPkg/OvmfPkgX64.dsc -n 8 ^'
      echo '  -D NETWORK_HTTP_BOOT_ENABLE=TRUE -D NETWORK_IP6_ENABLE=TRUE -D NETWORK_TLS_ENABLE=TRUE ^'
      echo '  -D NETWORK_ISCSI_ENABLE=TRUE -D NETWORK_ALLOW_HTTP_CONNECTIONS=TRUE ^'
      echo '  -D TPM2_ENABLE=TRUE -D TPM2_CONFIG_ENABLE=TRUE -D TPM1_ENABLE=TRUE ^'
      echo '  -D SECURE_BOOT_ENABLE=TRUE -D SMM_REQUIRE=FALSE -D BUILD_SHELL=FALSE || exit /b 1'
    } | sed 's/$/\r/' > "$bat"
    MSYS2_ARG_CONV_EXCL='*' cmd /c "$(cygpath -aw "$bat")"
    FV="$SRC/Build/OvmfX64/RELEASE_VS2022/FV"
    cp "$FV/OVMF_CODE.fd" "$OUT/2ksbox-x86_64-code.fd"
    cp "$FV/OVMF_VARS.fd" "$OUT/2ksbox-x86_64-vars.fd"
  fi
  echo "$STAMP" > "$STAMPFILE"
fi

# Next to QEMU's own firmware, every run: prepare-qemu.sh leaves untracked
# files in qemu/pc-bios alone, but a fresh qemu checkout has none.
for f in "2ksbox-$ARCH-code.fd" "2ksbox-$ARCH-vars.fd"; do
  cmp -s "$OUT/$f" "qemu/pc-bios/$f" || cp "$OUT/$f" "qemu/pc-bios/$f"
  echo "    qemu/pc-bios/$f"
done
