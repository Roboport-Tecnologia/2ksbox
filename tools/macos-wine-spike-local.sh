#!/usr/bin/env bash
# The M15 spike on a *second macOS on this Mac* (ADR-019's community build's
# user: a pre-26 macOS, no Vulkan 1.3, the real GPU), run from a Terminal
# booted into that macOS, not over ssh. One machine runs one macOS at a
# time, and this one mounts the other volume's checkout under /Volumes.
# tools/macvm-wine-spike.sh is the same idea for a VM, but a VM has no
# accelerated OpenGL for Wine at all, so this is the real test.
#
#   cd "/Volumes/Macintosh HD - Data/Users/<you>/work/win-98-xp-virt"   # the 26 volume's checkout
#   tools/macos-wine-spike-local.sh
#
# Needs, in that checkout, what scripts/build.sh made on the other macOS
# (nothing here links Homebrew, so it all runs on a bare install):
#   build/d3dpt-dp2-test                                  the host test
#   build/d3dpt/libd3dpt_exec_remote.dylib               QEMU's side of the transport
#   build/d3dpt/wine/d3dpt_exec.dll + d3dpt-exec-host.exe   the pair the Wine process runs
#   build/wine/wine-staging-*-osx64.tar.xz               WineHQ's macOS build (x86_64)
#   build/test/dp2-test.bmp                               the DXVK frame (the oracle)
# and, on this macOS, Rosetta: `softwareupdate --install-rosetta --agree-to-license`
# (a fresh install has none; no sudo needed). A bare install has no python3
# either (/usr/bin/python3 is the Command Line Tools' stub, which asks to
# install them), so the diff runs on `$PYTHON`, else a python3 that answers,
# else uv's own CPython from the checkout owner's home on the other volume.
#
# It unpacks Wine beside the tarball if that has not been done, keeps its
# prefix in build/wine-prefix-<macOS version>, runs the host test through
# the executor in the Wine process with wined3d's GL renderer, and diffs
# the frame. The two answers this macOS gives and the other cannot: the
# GL_RENDERER line (the real GPU's GL, for a Rosetta process) and the fps.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ver="$(sw_vers -productVersion)"
echo "macOS $ver, $(uname -m)"
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "no Rosetta: softwareupdate --install-rosetta --agree-to-license"; exit 1; }
for f in build/d3dpt-dp2-test build/d3dpt/libd3dpt_exec_remote.dylib \
         build/d3dpt/wine/d3dpt_exec.dll build/d3dpt/wine/d3dpt-exec-host.exe \
         build/test/dp2-test.bmp; do
  [ -f "$f" ] || { echo "missing $f: scripts/build.sh and scripts/test.sh host on the other macOS first"; exit 1; }
done
wine=""
for w in build/wine/Wine*.app/Contents/Resources/wine/bin/wine; do [ -x "$w" ] && wine="$w"; done
if [ -z "$wine" ]; then
  tar=$(ls build/wine/wine-*-osx64.tar.xz 2>/dev/null | head -1)
  [ -n "$tar" ] || { echo "no Wine in build/wine/ (the tarball from Gcenx's macOS_Wine_builds releases)"; exit 1; }
  echo "unpacking $tar"; (cd build/wine && tar xJf "$(basename "$tar")")
  for w in build/wine/Wine*.app/Contents/Resources/wine/bin/wine; do [ -x "$w" ] && wine="$w"; done
fi
py="${PYTHON:-}"
if [ -z "$py" ]; then
  if python3 -c '' 2>/dev/null; then py=python3
  else for p in "$ROOT"/../../.local/share/uv/python/cpython-3.1*-macos-aarch64-none/bin/python3; do
    if [ -x "$p" ] && "$p" -c "" 2>/dev/null; then py="$p"; fi; done
  fi
fi
[ -n "$py" ] || { echo "no working python3 (the CLT stub does not count): PYTHON=<interpreter>"; exit 1; }
OUT="build/macos-$ver"; mkdir -p "$OUT"
export D3DPT_WINE="$PWD/$wine" D3DPT_EXEC_LIB="$PWD/build/d3dpt/libd3dpt_exec_remote.dylib"
export D3DPT_WINEPREFIX="$PWD/build/wine-prefix-$ver" D3DPT_REMOTE_DIR="$PWD/$OUT"
export WINEDEBUG="-all,+d3d"       # the GL_RENDERER line, and any refusal, in the child's stderr
echo "wine: $wine"; echo "prefix: $D3DPT_WINEPREFIX (a fresh one costs ~20 s)"
rc=0
build/d3dpt-dp2-test "$OUT/dp2.bmp" > "$OUT/dp2.log" 2>&1 || { echo "dp2 test failed:"; tail -5 "$OUT/dp2.log"; rc=1; }
grep -ho 'GL_RENDERER "[^"]*"' "$OUT"/*.log | sort -u | head -2
grep -h "Using the .* renderer\|d3dpt-exec-host: exec: d3d9\|suitable pixel format\|frames," "$OUT"/*.log | sort -u | head -6
[ -f "$OUT/dp2.bmp" ] && "$py" tools/bmpdiff.py build/test/dp2-test.bmp "$OUT/dp2.bmp" --tolerance 8 -o "$OUT/dp2-diff.bmp" || rc=1
[ $rc = 0 ] && echo "PASS on macOS $ver: the frame matches the DXVK frame" || echo "FAIL on macOS $ver (logs and diff images in $OUT)"
exit $rc
