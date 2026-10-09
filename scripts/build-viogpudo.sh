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
# The WDK and SDK are Microsoft's NuGet packages at the versions below,
# restored into build/viogpudo/packages by a nuget.exe fetched next to them;
# Visual Studio 2022 supplies the compilers and, through its "Windows Driver
# Kit" component, the kernel-driver toolset (the components it needs are
# named when one is missing). WDK 10.0.26100 is the last for Visual Studio
# 2022. docs/build-windows.md "Our viogpudo".
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
WDK_VER=10.0.26100.6584
# the SDK the WDK packages depend on, at their floor (what NuGet resolves)
SDK_VER=10.0.26100.1
NUGET_URL=https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe
VS_COMPONENTS=(Component.Microsoft.Windows.DriverKit
               Microsoft.VisualStudio.Component.VC.Tools.ARM64
               Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre
               Microsoft.VisualStudio.Component.VC.Runtimes.ARM64.Spectre)

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

# Visual Studio 2022 with every component in VS_COMPONENTS, or a sentence
# naming what to add.
find_vs() {
  local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" vs
  [ -x "$vswhere" ] || { echo "build-viogpudo.sh: no Visual Studio installer (vswhere)" >&2; return 1; }
  vs=$("$vswhere" -version '[17.0,18.0)' -products '*' -requires "${VS_COMPONENTS[@]}" \
       -property installationPath | tr -d '\r' | head -1)
  if [ -z "$vs" ]; then
    echo "build-viogpudo.sh: needs Visual Studio 2022 with these components (Visual Studio Installer > Modify, or setup.exe modify --add <id>):" >&2
    printf '    %s\n' "${VS_COMPONENTS[@]}" >&2
    return 1
  fi
  cygpath -u "$vs"
}

# The WDK and SDK packages, at the pins, in $OUT/packages.
restore_wdk() {
  local nuget="$OUT/tools/nuget.exe" p
  if ! [ -f "$nuget" ]; then
    mkdir -p "$OUT/tools"
    curl -fsSL -o "$nuget" "$NUGET_URL"
  fi
  for p in "Microsoft.Windows.SDK.CPP $SDK_VER" "Microsoft.Windows.SDK.CPP.x64 $SDK_VER" \
           "Microsoft.Windows.SDK.CPP.arm64 $SDK_VER" \
           "Microsoft.Windows.WDK.x64 $WDK_VER" "Microsoft.Windows.WDK.ARM64 $WDK_VER"; do
    set -- $p
    [ -d "$OUT/packages/$1.$2" ] && continue
    echo "==> NuGet: $1 $2"
    "$nuget" install "$1" -Version "$2" -OutputDirectory "$(cygpath -w "$OUT/packages")" \
      -NonInteractive -Verbosity quiet
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
  # What the WDK's NuGet packages ask of a project (Microsoft's
  # Windows-driver-samples do the same); MSBuild picks it up from the tree's
  # root. Forward slashes: MSBuild takes them, and no shell eats them.
  pkgs='$(MSBuildThisFileDirectory)../packages'
  {
    echo '<Project>'
    echo "  <Import Project=\"$pkgs/Microsoft.Windows.WDK.x64.$WDK_VER/build/native/Microsoft.Windows.WDK.x64.props\" Condition=\"'\$(Platform)' == 'x64'\"/>"
    echo "  <Import Project=\"$pkgs/Microsoft.Windows.WDK.ARM64.$WDK_VER/build/native/Microsoft.Windows.WDK.ARM64.props\" Condition=\"'\$(Platform)' == 'ARM64'\"/>"
    echo "  <Import Project=\"$pkgs/Microsoft.Windows.SDK.CPP.x64.$SDK_VER/build/native/Microsoft.Windows.SDK.cpp.x64.props\" Condition=\"'\$(Platform)' == 'x64'\"/>"
    echo "  <Import Project=\"$pkgs/Microsoft.Windows.SDK.CPP.arm64.$SDK_VER/build/native/Microsoft.Windows.SDK.cpp.arm64.props\" Condition=\"'\$(Platform)' == 'ARM64'\"/>"
    echo "  <Import Project=\"$pkgs/Microsoft.Windows.SDK.CPP.$SDK_VER/build/native/Microsoft.Windows.SDK.cpp.props\"/>"
    echo '</Project>'
  } > "$SRC/Directory.Build.props"
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
  cp guest-tools/viogpudo/viogpudo-install.ps1 "$OUT/"
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
  cp "$OUT/viogpudo-install.ps1" "$tmp/"
  # the measure, from this checkout (a guest script, not part of the build)
  cp guest-tools/viogpudo/dwm-pace.ps1 "$tmp/"
  rm -f "$OUT/viogpudo-test.iso"
  xorriso -as mkisofs -quiet -J -R -V VIOGPUDO -o "$OUT/viogpudo-test.iso" "$tmp"
  rm -rf "$tmp"
  echo "==> $OUT/viogpudo-test.iso"
}

case "${1:-}" in
  source) source_tree ;;
  build) build ;;
  iso) iso ;;
  *) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
