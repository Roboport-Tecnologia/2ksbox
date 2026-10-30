#!/usr/bin/env bash
# Our viogpudo (track M24): virtio-win's display-only driver for
# virtio-gpu, from upstream's source at the pin below plus our patch queue
# in patches/viogpudo/, test signed in the guest by
# guest-tools/viogpudo/viogpudo-install.ps1 (64-bit Windows loads no
# unsigned kernel driver; Microsoft's signature is only on upstream's own
# builds).
#
#   scripts/build-viogpudo.sh source     upstream at the pin, our patches applied,
#                                        in build/viogpudo/src (any host)
#   scripts/build-viogpudo.sh build      source, then the ARM64 and x64 drivers
#                                        into build/viogpudo/{arm64,x64} (Windows,
#                                        MSYS2, Visual Studio 2022 with the WDK
#                                        component; the kit itself from NuGet)
#   scripts/build-viogpudo.sh iso        build/viogpudo/viogpudo-test.iso: both
#                                        drivers, the resolution service, the
#                                        installer and dwm-pace.ps1 (DWM's pace,
#                                        the measure), for a guest's CD drive
#                                        (needs xorriso)
#
# Hosts that cannot build it fetch the PC's build for their checkout,
# which the PC publishes: scripts/windows-drivers.sh fetch|publish viogpudo.
#
# The toolchain (Visual Studio 2022, the WDK and SDK from NuGet at their
# pins) is scripts/nuget-wdk.sh's, shared with the WDDM driver's Arm kernel
# driver. docs/build-windows.md "Our viogpudo".
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

UPSTREAM=https://github.com/virtio-win/kvm-guest-drivers-windows.git
# The last upstream commit before virtio-win 0.1.302's viogpudo
# (DriverVer 07/22/2026, 100.103.104.30200), the build our drivers disc
# carries, so an A/B against it is our patches alone.
PIN=fbcc19d763f92a8281abe364c7ea1de3de87e61d
OUT=build/viogpudo
SRC=$OUT/src
. scripts/nuget-wdk.sh

# Upstream at the pin, every patch in filename order, from a clean tree
# each time (a patch edited since the last run applies to the pristine
# file, never on top of its old self).
source_tree() {
  if ! [ -d "$SRC/.git" ]; then
    echo "==> viogpudo: fetching upstream"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$UPSTREAM"
  fi
  if ! git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null; then
    git -C "$SRC" fetch -q --depth 1 origin "$PIN"
  fi
  git -C "$SRC" checkout -q -f --detach "$PIN"
  git -C "$SRC" clean -q -fdx
  local p
  for p in patches/viogpudo/*.patch; do
    echo "==> viogpudo: $(basename "$p")"
    git -C "$SRC" apply --whitespace=nowarn "$ROOT/$p"
  done
}

build() {
  case "$(uname -s)" in MINGW*|MSYS*) ;; *)
    echo "build-viogpudo.sh: the drivers build on Windows (MSYS2); elsewhere: fetch" >&2; exit 1 ;;
  esac
  local vs arch dir pl pkgs
  vs=$(find_vs) || exit 1
  echo "==> Visual Studio at $vs"
  restore_wdk
  source_tree
  # MSBuild picks it up from the tree's root. Forward slashes: MSBuild
  # takes them, and no shell eats them.
  wdk_props "$SRC/Directory.Build.props" '$(MSBuildThisFileDirectory)../packages'
  # upstream's packaging step runs inf2cat by name; the catalog it makes is
  # not used (the installer writes and signs its own)
  PATH="$ROOT/$OUT/packages/Microsoft.Windows.WDK.x64.$WDK_VER/c/bin/${WDK_VER%.*}.0/x86:$PATH"
  # the solution, for its VirtioLib, which the driver links
  for pl in ARM64 x64; do
    "$vs/MSBuild/Current/Bin/amd64/MSBuild.exe" "$(cygpath -w "$SRC/viogpu/viogpu.sln")" \
      -t:viogpudo -nologo -v:minimal -m "-p:Configuration=Win11 Release" -p:Platform=$pl
  done
  for arch in arm64 x64; do
    case $arch in arm64) dir=ARM64 ;; x64) dir=amd64 ;; esac
    mkdir -p "$OUT/$arch"
    cp "$SRC/viogpu/Install/Win11/$dir/"{viogpudo.sys,viogpudo.inf,viogpudo.pdb,vgpusrv.exe,viogpuap.exe} \
       "$OUT/$arch/"
  done
  # the sources it was built from (scripts/windows-drivers.sh publishes by them)
  scripts/windows-drivers.sh key viogpudo > "$OUT/.key"
  echo "==> $OUT/arm64, $OUT/x64 (sources $(cat "$OUT/.key"))"
}

iso() {
  command -v xorriso >/dev/null || { echo "build-viogpudo.sh: iso needs xorriso" >&2; exit 1; }
  [ -f "$OUT/x64/viogpudo.sys" ] || {
    echo "build-viogpudo.sh: nothing in $OUT (build, or scripts/windows-drivers.sh fetch viogpudo)" >&2; exit 1; }
  local tmp
  tmp=$(mktemp -d)
  mkdir -p "$tmp/arm64" "$tmp/x64"
  cp "$OUT"/arm64/{viogpudo.sys,viogpudo.inf,vgpusrv.exe,viogpuap.exe} "$tmp/arm64/"
  cp "$OUT"/x64/{viogpudo.sys,viogpudo.inf,vgpusrv.exe,viogpuap.exe} "$tmp/x64/"
  # the installer and the measure from this checkout: guest scripts, not
  # the build, so editing one needs no new driver from the PC
  cp guest-tools/viogpudo/viogpudo-install.ps1 guest-tools/viogpudo/dwm-pace.ps1 "$tmp/"
  rm -f "$OUT/viogpudo-test.iso"
  xorriso -as mkisofs -quiet -J -R -V VIOGPUDO -o "$OUT/viogpudo-test.iso" "$tmp"
  rm -rf "$tmp"
  echo "==> $OUT/viogpudo-test.iso"
}

case "${1:-}" in
  source) source_tree ;;
  build) build ;;
  iso) iso ;;
  *) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
