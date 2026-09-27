#!/usr/bin/env bash
# Wine's d3d8 / d3d9 tests (guest-tools/build-winetests.sh, the same EXEs
# the guest runs) under the host's Wine on DXVK's own 32-bit d3d9.dll /
# d3d8.dll: what DXVK itself fails, so a guest failure can be sorted into
# the driver's / executor's and DXVK's (track M16).
#
#   tools/winetest-dxvk.sh                     every file, into build/winetest/dxvk/
#   WT_TESTS="d3d9:visual" tools/winetest-dxvk.sh
#   WT_SAVE=reference/winetest/dxvk-wine.txt tools/winetest-dxvk.sh
#
# Then, for a guest run (tools/xp-driver-test.sh <image> winetest):
#   tools/winetest-summary.py <OUT>/winetest --baseline reference/winetest/dxvk-wine.txt \
#     --by-function build/winetest/wine-11.0
#
# Builds DXVK for i686 from third_party/dxvk into build/dxvk-win32 (mingw,
# meson, glslang; a few minutes once). The windows go to a headless sway
# with Xwayland when sway is installed, so nothing appears on the desktop;
# otherwise to $DISPLAY. The prefix is build/winetest/dxvk-prefix. The
# numbers depend on the host's Wine and GPU driver; the saved baseline's
# header says which.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WT="$ROOT/build/winetest/out"
OUT="$ROOT/build/winetest/dxvk"
BUILD="$ROOT/build/dxvk-win32"
[ -f "$WT/d3d9_test.exe" ] || { echo "no $WT/d3d9_test.exe: run guest-tools/build-winetests.sh"; exit 1; }
command -v wine >/dev/null || { echo "no wine on PATH"; exit 1; }
[ -n "${WT_SAVE:-}" ] && WT_SAVE=$(realpath -m "$WT_SAVE")

if [ ! -f "$BUILD/build.ninja" ]; then
  meson setup "$BUILD" "$ROOT/third_party/dxvk" --cross-file "$ROOT/third_party/dxvk/build-win32.txt" \
    --buildtype release -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false >/dev/null
fi
ninja -C "$BUILD" src/d3d9/d3d9.dll src/d3d8/d3d8.dll >/dev/null

mkdir -p "$OUT"
rm -f "$OUT"/*_test_*.txt
cp -f "$WT/d3d9_test.exe" "$WT/d3d8_test.exe" "$BUILD/src/d3d9/d3d9.dll" "$BUILD/src/d3d8/d3d8.dll" "$OUT/"
export WINEPREFIX="$ROOT/build/winetest/dxvk-prefix" WINEDEBUG=-all DXVK_LOG_LEVEL=none
export WINEDLLOVERRIDES="d3d9=n;d3d8=n"
# never a cached shader: a patch under test changes what they compile to
export DXVK_SHADER_CACHE=0

SWAY_PID=
if command -v sway >/dev/null; then
  # a sway of its own, headless, whose Xwayland display the tests draw on
  RUN=$(mktemp -d)
  printf '%s\n' 'xwayland enable' "exec sh -c 'echo \$DISPLAY > $RUN/display'" > "$RUN/config"
  env -u DISPLAY -u WAYLAND_DISPLAY WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$RUN/config" >"$RUN/sway.log" 2>&1 &
  SWAY_PID=$!
  trap 'kill $SWAY_PID 2>/dev/null; rm -rf "$RUN"' EXIT
  for _ in $(seq 100); do [ -s "$RUN/display" ] && break; sleep 0.1; done
  [ -s "$RUN/display" ] || { echo "the headless sway gave no display ($RUN/sway.log)"; tail -3 "$RUN/sway.log"; exit 1; }
  export DISPLAY; DISPLAY=$(cat "$RUN/display")
  unset WAYLAND_DISPLAY
fi
echo "Wine $(wine --version), DXVK $(git -C "$ROOT/third_party/dxvk" describe --tags --always 2>/dev/null || echo '?'), display $DISPLAY"

cd "$OUT"
for t in ${WT_TESTS:-d3d9:visual d3d9:stateblock d3d9:device d3d8:visual d3d8:stateblock d3d8:device}; do
  exe="${t%%:*}_test.exe"; name="${t%%:*}_test_${t#*:}"
  s=$(date +%s)
  rc=0; timeout "${WT_CAP:-1200}" wine "$exe" "${t#*:}" >"$name.txt" 2>&1 || rc=$?
  echo "$name: exit $rc in $(( $(date +%s) - s )) s"
done
python3 "$ROOT/tools/winetest-summary.py" "$OUT" ${WT_SAVE:+--save "$WT_SAVE"}
if [ -n "${WT_SAVE:-}" ]; then
  # the host the numbers belong to, under the tool's own header line
  gpu=$(DISPLAY= vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*deviceName\s*= //p' | head -1)
  sed -i "1a # DXVK on the host's Wine (tools/winetest-dxvk.sh): $(wine --version), ${gpu:-unknown GPU}" "$WT_SAVE"
fi
