#!/usr/bin/env bash
# Our Windows drivers that build only on the PC, prebuilt for the hosts
# that cannot build them. Each is published as a GitHub release asset
# named by a hash of the sources it is built from, and Linux and macOS
# fetch the one that matches their checkout. A source change is a new
# name, so a fetch never gets a driver older than its checkout.
#
#   wddm      the WDDM display driver: Windows 7's (track M18) in
#             build/wddm/x86 and 64-bit Windows 11's (track M20) in
#             build/wddm/x64, which the guest-tools ISO carries in WDDM#             and WDDM64\. Built by build-windows.sh's wddm stage (the
#             EWDK 10.0.19041).
#   viogpudo  our viogpudo for Windows 11 (track M24): ARM64 and x64
#             viogpudo.{sys,inf} with the resolution service (vgpusrv,
#             viogpuap) in build/viogpudo, for build-viogpudo.sh iso, which
#             takes the installer from the source tree. Built by
#             build-viogpudo.sh build.
#
#   scripts/windows-drivers.sh key <driver>       the hash of its sources
#   scripts/windows-drivers.sh fetch [driver...]  download the matching
#                                                 builds (Linux, macOS; all
#                                                 by default)
#   scripts/windows-drivers.sh publish [driver...]
#                                                 build what is not built
#                                                 for these sources, then
#                                                 upload it (the PC; gh,
#                                                 logged in; all by default)
#
# fetch exits 0 when every driver asked for is there for this checkout,
# 1 (with the reason) when one is not: nothing published for these
# sources, no network, or WINDOWS_DRIVERS_PREBUILT=0 (WDDM_PREBUILT=0 too,
# its old name). A build fetched for older sources is removed first, so an
# ISO never carries one; files put there by hand (no .key beside them) are
# left alone. WINDOWS_DRIVERS_REPO=owner/name reads and writes another
# repository's release. docs/build-windows.md "Prebuilt drivers".
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

REPO="${WINDOWS_DRIVERS_REPO:-${WDDM_PREBUILT_REPO:-Roboport-Tecnologia/2ksbox}}"
TAG=windows-drivers
DRIVERS=(wddm viogpudo)

# Per driver: OUT, the files under it, the sources its hash covers (every
# file the build compiles or copies, and the script that builds it;
# .gitattributes keeps each file's bytes as committed, so the PC and the
# other hosts hash the same bytes), the release a fetch also tries (where
# it was published before this script), and how it is built.
setup() {
  case "$1" in
    wddm)
      NAME="the WDDM driver"
      OUT=build/wddm
      # Windows 7's pair, and 64-bit Windows 11's (track M20) with the
      # 32-bit user-mode driver for WoW64; the ISO's WDDM64\ takes its
      # .inf and installer from the source tree
      FILES=(x86/d3dptkmd.sys x86/d3dptumd.dll x86/d3dptkmd.inf
             x64/d3dptkmd.sys x64/d3dptumd.dll x64/d3dptumd32.dll)
      SOURCES=(guest-tools/src/d3dptvid/wddm guest-tools/src/d3dptvid/core
               d3dpt/d3dpt_fb.h d3dpt/d3dpt_enc.h d3dpt/d3dpt_proto.h
               guest-tools/build-wddm.cmd)
      # (wddm-prebuilt, the release before this script, has the x86 files
      # alone, flat: not a fallback for this layout)
      OLD_TAG=
      BUILD=(scripts/build-windows.sh wddm) ;;
    viogpudo)
      NAME="our viogpudo"
      OUT=build/viogpudo
      FILES=(arm64/viogpudo.sys arm64/viogpudo.inf arm64/vgpusrv.exe arm64/viogpuap.exe
             x64/viogpudo.sys x64/viogpudo.inf x64/vgpusrv.exe x64/viogpuap.exe)
      # the script holds upstream's pin, so the pin is in the hash; the
      # guest scripts (guest-tools/viogpudo) go on the ISO from the tree
      SOURCES=(patches/viogpudo scripts/build-viogpudo.sh)
      OLD_TAG=
      BUILD=(scripts/build-viogpudo.sh build) ;;
    *) echo "windows-drivers.sh: no driver '$1' (${DRIVERS[*]})" >&2; exit 2 ;;
  esac
}

if command -v sha256sum >/dev/null 2>&1; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

key() {
  local f
  git ls-files -- "${SOURCES[@]}" | LC_ALL=C sort | while IFS= read -r f; do
    printf '%s\n' "$f"; cat "$f"
  done | "${SHA[@]}" | cut -c1-16
}

have_all() { local f; for f in "${FILES[@]}"; do [ -f "$OUT/$f" ] || return 1; done; }

fetch_one() {
  local d=$1 k cur url tmp tag got=""
  k=$(key)
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  if have_all && [ "$cur" = "$k" ]; then
    echo "    $NAME for these sources is in $OUT ($k)"; return 0
  fi
  if have_all && [ -z "$cur" ]; then
    echo "    $OUT holds $NAME of unknown sources (no .key); leaving it" >&2; return 0
  fi
  if [ -n "$cur" ]; then
    echo "    removing $NAME fetched for other sources ($cur)"
    rm -f "${FILES[@]/#/$OUT/}" "$OUT/.key" "$OUT/BUILT-FROM"
  fi
  if [ "${WINDOWS_DRIVERS_PREBUILT:-${WDDM_PREBUILT:-1}}" = 0 ]; then
    echo "    WINDOWS_DRIVERS_PREBUILT=0: $NAME not fetched" >&2; return 1
  fi
  tmp=$(mktemp -d)
  for tag in "$TAG" $OLD_TAG; do
    url="https://github.com/$REPO/releases/download/$tag/$d-$k.tar.gz"
    if curl -fsSL --connect-timeout 10 -o "$tmp/d.tar.gz" "$url"; then got=1; break; fi
  done
  if [ -z "$got" ]; then
    rm -rf "$tmp"
    echo "    no $d published for these sources ($k), or no network;" >&2
    echo "    on the PC: scripts/windows-drivers.sh publish $d" >&2
    return 1
  fi
  mkdir -p "$OUT"
  tar -xzf "$tmp/d.tar.gz" -C "$OUT"
  rm -rf "$tmp"
  have_all || { echo "    $d-$k.tar.gz lacks a file" >&2; rm -f "${FILES[@]/#/$OUT/}"; return 1; }
  printf '%s\n' "$k" > "$OUT/.key"
  echo "    fetched $NAME for these sources ($k) into $OUT"
}

publish_one() {
  local d=$1 k cur dirty tmp
  k=$(key)
  # The name promises the committed sources, so a binary from edits that
  # were never committed must not take it.
  dirty=$(git status --porcelain -- "${SOURCES[@]}")
  [ -z "$dirty" ] || {
    printf 'windows-drivers.sh: uncommitted changes in the sources of %s:\n%s\n' "$d" "$dirty" >&2; exit 1; }
  if gh release view "$TAG" -R "$REPO" --json assets -q '.assets[].name' 2>/dev/null | grep -qx "$d-$k.tar.gz"; then
    echo "    $d-$k.tar.gz is already published"; return 0
  fi
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  if ! have_all || [ "$cur" != "$k" ]; then
    echo "==> $d: building ($k)"
    "${BUILD[@]}"
    cur=$(cat "$OUT/.key" 2>/dev/null || true)
    have_all && [ "$cur" = "$k" ] || {
      echo "windows-drivers.sh: ${BUILD[*]} left no build of these sources in $OUT (${cur:-no .key}, sources $k)" >&2; exit 1; }
  fi
  tmp=$(mktemp -d)
  printf 'sources %s\ncommit %s\n' "$k" "$(git rev-parse HEAD)" > "$tmp/BUILT-FROM"
  tar -czf "$tmp/$d-$k.tar.gz" -C "$OUT" "${FILES[@]}" -C "$tmp" BUILT-FROM
  gh release upload "$TAG" -R "$REPO" "$tmp/$d-$k.tar.gz"
  rm -rf "$tmp"
  echo "    published $d-$k.tar.gz"
}

cmd="${1:-}"
[ $# -gt 0 ] && shift
# the drivers named, or all of them
ds=("$@")
[ ${#ds[@]} -gt 0 ] || ds=("${DRIVERS[@]}")
case "$cmd" in
  key)
    [ $# -eq 1 ] || { echo "windows-drivers.sh: key <driver> (${DRIVERS[*]})" >&2; exit 2; }
    setup "$1"; key ;;
  fetch)
    rc=0
    for d in "${ds[@]}"; do setup "$d"; fetch_one "$d" || rc=1; done
    exit $rc ;;
  publish)
    # GitHub's installer puts gh where MSYS2's PATH does not look
    command -v gh >/dev/null 2>&1 || PATH="$PATH:/c/Program Files/GitHub CLI"
    command -v gh >/dev/null 2>&1 || { echo "windows-drivers.sh: publish needs gh (GitHub's CLI), logged in" >&2; exit 1; }
    case "$(uname -s)" in MINGW*|MSYS*) ;; *)
      echo "windows-drivers.sh: publish builds on the PC (Windows, MSYS2)" >&2; exit 1 ;;
    esac
    if ! gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
      gh release create "$TAG" -R "$REPO" --prerelease --latest=false \
        --title "Windows drivers, prebuilt" \
        --notes "Drivers that build only on Windows (the Windows 7 WDDM driver, our viogpudo), for Linux and macOS checkouts: one asset per driver and source hash (scripts/windows-drivers.sh). Not a release."
    fi
    for d in "${ds[@]}"; do setup "$d"; publish_one "$d"; done ;;
  *) sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
