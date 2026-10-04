#!/usr/bin/env bash
# The Windows 7 WDDM driver for hosts that cannot build it (track M18).
# It builds only on Windows, with the EWDK (build-windows.sh's wddm
# stage), so the PC publishes its three files (d3dptkmd.sys,
# d3dptumd.dll, d3dptkmd.inf) as a GitHub release asset named by a hash
# of the sources they are built from, and Linux and macOS fetch the one
# that matches their checkout into build/wddm/x86, where the guest stage
# puts it on the ISO in WDDM\. A source change is a new name, so a fetch
# never gets a driver older than its checkout.
#
#   scripts/wddm-prebuilt.sh key        the hash of this checkout's driver sources
#   scripts/wddm-prebuilt.sh fetch      download the matching driver (Linux, macOS)
#   scripts/wddm-prebuilt.sh publish    upload build/wddm/x86 (the PC, after the
#                                       wddm stage; needs gh, logged in)
#
# fetch exits 0 when build/wddm/x86 holds this checkout's driver, 1 (with
# the reason) when it does not: nothing published for these sources, no
# network, or WDDM_PREBUILT=0. A driver fetched for older sources is
# removed first, so an ISO never carries one. Files put there by hand
# (no .key beside them) are left alone. WDDM_PREBUILT_REPO=owner/name
# reads and writes another repository's release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

REPO="${WDDM_PREBUILT_REPO:-davidrios/2ksbox}"
TAG=wddm-prebuilt
OUT=build/wddm/x86
FILES=(d3dptkmd.sys d3dptumd.dll d3dptkmd.inf)
# Everything the two projects compile or copy: the driver's own tree, the
# shared core the DLL compiles in, the protocol headers both include, and
# the script that builds them. .gitattributes keeps every file's bytes as
# committed, so the PC and Linux hash the same bytes.
SOURCES=(guest-tools/src/d3dptvid/wddm guest-tools/src/d3dptvid/core
         d3dpt/d3dpt_fb.h d3dpt/d3dpt_enc.h d3dpt/d3dpt_proto.h
         guest-tools/build-wddm.cmd)

if command -v sha256sum >/dev/null 2>&1; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

key() {
  local f
  git ls-files -- "${SOURCES[@]}" | LC_ALL=C sort | while IFS= read -r f; do
    printf '%s\n' "$f"; cat "$f"
  done | "${SHA[@]}" | cut -c1-16
}

have_all() { local f; for f in "${FILES[@]}"; do [ -f "$OUT/$f" ] || return 1; done; }

fetch() {
  local k cur url tmp
  k=$(key)
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  if have_all && [ "$cur" = "$k" ]; then
    echo "    the WDDM driver for these sources is in $OUT ($k)"; return 0
  fi
  if have_all && [ -z "$cur" ]; then
    echo "    $OUT holds a WDDM driver of unknown sources (no .key); leaving it" >&2; return 0
  fi
  if [ -n "$cur" ]; then
    echo "    removing the WDDM driver fetched for other sources ($cur)"
    rm -f "${FILES[@]/#/$OUT/}" "$OUT/.key" "$OUT/BUILT-FROM"
  fi
  if [ "${WDDM_PREBUILT:-1}" = 0 ]; then
    echo "    WDDM_PREBUILT=0: no WDDM driver fetched" >&2; return 1
  fi
  url="https://github.com/$REPO/releases/download/$TAG/wddm-$k.tar.gz"
  tmp=$(mktemp -d)
  if ! curl -fsSL --connect-timeout 10 -o "$tmp/w.tar.gz" "$url"; then
    rm -rf "$tmp"
    echo "    no WDDM driver published for these sources ($k), or no network;" >&2
    echo "    on the PC: scripts/build-windows.sh wddm --publish" >&2
    return 1
  fi
  mkdir -p "$OUT"
  tar -xzf "$tmp/w.tar.gz" -C "$OUT"
  rm -rf "$tmp"
  have_all || { echo "    wddm-$k.tar.gz lacks a driver file" >&2; rm -f "${FILES[@]/#/$OUT/}"; return 1; }
  printf '%s\n' "$k" > "$OUT/.key"
  echo "    fetched the WDDM driver for these sources ($k) into $OUT"
}

publish() {
  local k cur dirty tmp
  command -v gh >/dev/null 2>&1 || { echo "wddm-prebuilt.sh: publish needs gh (GitHub's CLI), logged in" >&2; exit 1; }
  have_all || { echo "wddm-prebuilt.sh: no driver in $OUT (scripts/build-windows.sh wddm)" >&2; exit 1; }
  k=$(key)
  cur=$(cat "$OUT/.key" 2>/dev/null || true)
  [ "$cur" = "$k" ] || {
    echo "wddm-prebuilt.sh: $OUT was not built from these sources (${cur:-no .key}, sources $k); run the wddm stage" >&2; exit 1; }
  # The name promises the committed sources, so a binary from edits that
  # were never committed must not take it.
  dirty=$(git status --porcelain -- "${SOURCES[@]}")
  [ -z "$dirty" ] || {
    printf 'wddm-prebuilt.sh: uncommitted changes in the driver sources:\n%s\n' "$dirty" >&2; exit 1; }
  if ! gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
    gh release create "$TAG" -R "$REPO" --prerelease --latest=false \
      --title "WDDM driver, prebuilt" \
      --notes "The Windows 7 WDDM driver for Linux and macOS guest-tools ISOs, one asset per source hash (scripts/wddm-prebuilt.sh). Not a release."
  fi
  if gh release view "$TAG" -R "$REPO" --json assets -q '.assets[].name' | grep -qx "wddm-$k.tar.gz"; then
    echo "    wddm-$k.tar.gz is already published"; return 0
  fi
  tmp=$(mktemp -d)
  printf 'sources %s\ncommit %s\n' "$k" "$(git rev-parse HEAD)" > "$tmp/BUILT-FROM"
  cp "${FILES[@]/#/$OUT/}" "$tmp/"
  tar -czf "$tmp/wddm-$k.tar.gz" -C "$tmp" "${FILES[@]}" BUILT-FROM
  gh release upload "$TAG" -R "$REPO" "$tmp/wddm-$k.tar.gz"
  rm -rf "$tmp"
  echo "    published wddm-$k.tar.gz"
}

case "${1:-}" in
  key) key ;;
  fetch) fetch ;;
  publish) publish ;;
  *) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
