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
#                                        MSYS2: an EWDK for Windows 11 mounted)
#   scripts/build-viogpudo.sh publish    upload build/viogpudo as a release asset
#                                        named by the sources' hash (the PC, after
#                                        build; needs gh, logged in)
#   scripts/build-viogpudo.sh fetch      download the build for these sources
#                                        (Linux, macOS)
#   scripts/build-viogpudo.sh iso        build/viogpudo/viogpudo-test.iso: both
#                                        drivers and the installer, for a guest's
#                                        CD drive (needs xorriso)
#   scripts/build-viogpudo.sh key        the hash of the sources
#
# The EWDK is found on any mounted drive with a 10.0.2xxxx kit (Windows 11);
# EWDK11=<drive:> names it, EWDK11_ISO=<iso> mounts it first. Upstream
# builds this source with EWDK 26H1; 24H2 or later should do.
# docs/build-windows.md "Our viogpudo".
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
REPO="${VIOGPUDO_PREBUILT_REPO:-Roboport-Tecnologia/2ksbox}"
TAG=viogpudo-prebuilt
FILES=(arm64/viogpudo.sys arm64/viogpudo.inf x64/viogpudo.sys x64/viogpudo.inf
       viogpudo-install.ps1)
SOURCES=(patches/viogpudo guest-tools/viogpudo scripts/build-viogpudo.sh)

if command -v sha256sum >/dev/null 2>&1; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

key() {
  local f
  { echo "$PIN"
    git ls-files -- "${SOURCES[@]}" | LC_ALL=C sort | while IFS= read -r f; do
      printf '%s\n' "$f"; cat "$f"
    done; } | "${SHA[@]}" | cut -c1-16
}

have_all() { local f; for f in "${FILES[@]}"; do [ -f "$OUT/$f" ] || return 1; done; }

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

find_ewdk() {
  local d
  for d in D E F G H I J K L M N O P Q R S T U V W X Y Z; do
    [ -f "/${d,,}/BuildEnv/SetupBuildEnv.cmd" ] || continue
    if compgen -G "/${d,,}/Program Files/Windows Kits/10/Include/10.0.2*/km/dispmprt.h" >/dev/null; then
      echo "$d:"; return 0
    fi
  done
  return 1
}

build() {
  case "$(uname -s)" in MINGW*|MSYS*) ;; *)
    echo "build-viogpudo.sh: the drivers build on Windows (MSYS2); elsewhere: fetch" >&2; exit 1 ;;
  esac
  local ewdk="${EWDK11:-}" arch dir
  if [ -z "$ewdk" ]; then ewdk=$(find_ewdk || true); fi
  if [ -z "$ewdk" ] && [ -n "${EWDK11_ISO:-}" ]; then
    echo "==> mounting $EWDK11_ISO"
    powershell -NoProfile -Command "Mount-DiskImage -ImagePath '$(cygpath -w "$EWDK11_ISO")' | Out-Null"
    ewdk=$(find_ewdk || true)
  fi
  [ -n "$ewdk" ] || {
    echo "build-viogpudo.sh: no EWDK for Windows 11 mounted; mount it or set EWDK11_ISO (docs/build-windows.md \"Our viogpudo\")" >&2
    exit 1; }
  echo "==> EWDK at $ewdk"
  source_tree
  local proj
  proj=$(cygpath -w "$SRC/viogpu/viogpudo/viogpudo.vcxproj")
  # SetupBuildEnv refuses a shell that already ran it, hence the blank
  # WindowsSystemKit; one cmd builds both, the toolset picks each platform's
  # compiler.
  cat > "$OUT/build.cmd" <<EOF
@echo off
set WindowsSystemKit=
call "$ewdk\\BuildEnv\\SetupBuildEnv.cmd" >nul 2>nul
@echo off
if not defined WDKContentRoot exit /b 1
msbuild "$proj" -nologo -v:minimal "-p:Configuration=Win11 Release" -p:Platform=ARM64 || exit /b 1
msbuild "$proj" -nologo -v:minimal "-p:Configuration=Win11 Release" -p:Platform=x64 || exit /b 1
EOF
  cmd //c "$(cygpath -w "$OUT/build.cmd")"
  for arch in arm64 x64; do
    case $arch in arm64) dir=ARM64 ;; x64) dir=amd64 ;; esac
    mkdir -p "$OUT/$arch"
    cp "$SRC/viogpu/Install/Win11/$dir/viogpudo.sys" "$SRC/viogpu/Install/Win11/$dir/viogpudo.inf" "$OUT/$arch/"
    cp "$SRC/viogpu/viogpudo/Win11Release/$dir/viogpudo.pdb" "$OUT/$arch/" 2>/dev/null || true
  done
  cp guest-tools/viogpudo/viogpudo-install.ps1 "$OUT/"
  key > "$OUT/.key"
  echo "==> $OUT/arm64, $OUT/x64 (sources $(cat "$OUT/.key"))"
}

fetch() {
  local k cur url tmp
  k=$(key)
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  if have_all && [ "$cur" = "$k" ]; then
    echo "    our viogpudo for these sources is in $OUT ($k)"; return 0
  fi
  url="https://github.com/$REPO/releases/download/$TAG/viogpudo-$k.tar.gz"
  tmp=$(mktemp -d)
  if ! curl -fsSL --connect-timeout 10 -o "$tmp/v.tar.gz" "$url"; then
    rm -rf "$tmp"
    echo "    no viogpudo published for these sources ($k), or no network;" >&2
    echo "    on the PC: scripts/build-viogpudo.sh build && scripts/build-viogpudo.sh publish" >&2
    return 1
  fi
  mkdir -p "$OUT"
  tar -xzf "$tmp/v.tar.gz" -C "$OUT"
  rm -rf "$tmp"
  have_all || { echo "    viogpudo-$k.tar.gz lacks a file" >&2; return 1; }
  printf '%s\n' "$k" > "$OUT/.key"
  echo "    fetched our viogpudo for these sources ($k) into $OUT"
}

publish() {
  local k cur dirty tmp
  command -v gh >/dev/null 2>&1 || { echo "build-viogpudo.sh: publish needs gh (GitHub's CLI), logged in" >&2; exit 1; }
  have_all || { echo "build-viogpudo.sh: nothing built in $OUT (build first)" >&2; exit 1; }
  k=$(key)
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  [ "$cur" = "$k" ] || {
    echo "build-viogpudo.sh: $OUT was not built from these sources (${cur:-no .key}, sources $k); build again" >&2; exit 1; }
  dirty=$(git status --porcelain -- "${SOURCES[@]}")
  [ -z "$dirty" ] || {
    printf 'build-viogpudo.sh: uncommitted changes in the sources:\n%s\n' "$dirty" >&2; exit 1; }
  if ! gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
    gh release create "$TAG" -R "$REPO" --prerelease --latest=false \
      --title "viogpudo, prebuilt (test signed in the guest)" \
      --notes "Track M24's viogpudo for Linux and macOS test runs, one asset per source hash (scripts/build-viogpudo.sh). Not a release."
  fi
  if gh release view "$TAG" -R "$REPO" --json assets -q '.assets[].name' | grep -qx "viogpudo-$k.tar.gz"; then
    echo "    viogpudo-$k.tar.gz is already published"; return 0
  fi
  tmp=$(mktemp -d)
  printf 'sources %s\ncommit %s\nupstream %s\n' "$k" "$(git rev-parse HEAD)" "$PIN" > "$tmp/BUILT-FROM"
  tar -czf "$tmp/viogpudo-$k.tar.gz" -C "$OUT" "${FILES[@]}" -C "$tmp" BUILT-FROM
  gh release upload "$TAG" -R "$REPO" "$tmp/viogpudo-$k.tar.gz"
  rm -rf "$tmp"
  echo "    published viogpudo-$k.tar.gz"
}

iso() {
  command -v xorriso >/dev/null || { echo "build-viogpudo.sh: iso needs xorriso" >&2; exit 1; }
  have_all || { echo "build-viogpudo.sh: nothing in $OUT (build or fetch first)" >&2; exit 1; }
  local tmp
  tmp=$(mktemp -d)
  mkdir -p "$tmp/arm64" "$tmp/x64"
  cp "$OUT"/arm64/viogpudo.{sys,inf} "$tmp/arm64/"
  cp "$OUT"/x64/viogpudo.{sys,inf} "$tmp/x64/"
  cp "$OUT/viogpudo-install.ps1" "$tmp/"
  rm -f "$OUT/viogpudo-test.iso"
  xorriso -as mkisofs -quiet -J -R -V VIOGPUDO -o "$OUT/viogpudo-test.iso" "$tmp"
  rm -rf "$tmp"
  echo "==> $OUT/viogpudo-test.iso"
}

case "${1:-}" in
  source) source_tree ;;
  build) build ;;
  publish) publish ;;
  fetch) fetch ;;
  iso) iso ;;
  key) key ;;
  *) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
