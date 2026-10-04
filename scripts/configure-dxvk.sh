#!/usr/bin/env bash
# meson setup for the native DXVK d3d9 library (the D3D executor, doc 14 /
# ADR-007) into build/dxvk. Only d3d9 is built, and only against patch 04's
# window-less WSI (DXVK_WSI_DRIVER=Headless). Neither the executor nor the
# native oracles present, so no windowing toolkit is built or linked.
# Then: ninja -C build/dxvk
#   macOS: brew install vulkan-headers vulkan-loader glslang meson ninja
#          (+ the LunarG SDK for the KosmicKrisp ICD on macOS 26)
#   Arch:  pacman -S vulkan-headers vulkan-icd-loader glslang meson ninja
#
#   scripts/configure-dxvk.sh --windows   build/win/dxvk (d3d9.dll), in
#          MSYS2's MINGW64 shell on Windows (ADR-026), with patch 08's
#          headless WSI beside Win32, so the Windows executor runs the
#          same d3d9 as every other host. No cross file: meson's host is
#          already Windows and DXVK's meson.build takes its platform
#          flags from that
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "${1:-}" = "--windows" ]; then
  shift
  [ "${MSYSTEM:-}" = MINGW64 ] || {
    echo "configure-dxvk.sh --windows: in MSYS2's MINGW64 shell on Windows (ADR-026)" >&2; exit 1; }
  BUILD="${1:-$ROOT/build/win/dxvk}"
else
  BUILD="${1:-$ROOT/build/dxvk}"
fi
darwin=()
if [ "$(uname -s)" = Darwin ]; then
  export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  # The macOS every Mac build targets (scripts/macos-floor.sh), as flags
  # and not only the environment. A changed flag is a changed command
  # line, so ninja recompiles what a changed environment would have kept.
  T="${MACOSX_DEPLOYMENT_TARGET:-$("$ROOT/scripts/macos-floor.sh")}"
  darwin=(-Dc_args="-mmacosx-version-min=$T" -Dcpp_args="-mmacosx-version-min=$T"
          -Dc_link_args="-mmacosx-version-min=$T" -Dcpp_link_args="-mmacosx-version-min=$T")
fi
opts=(--buildtype release -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false
      -Dnative_sdl2=disabled -Dnative_glfw=disabled -Dnative_sdl3=disabled ${darwin[@]+"${darwin[@]}"})
# A meson build directory holds absolute paths and cannot be relocated. In
# a renamed or moved checkout its --reconfigure walks into directories that
# no longer exist ("[Errno 2] No such file or directory: <old
# checkout>/build/dxvk/meson-private/tmp..."). So when the reconfigure
# fails, for any reason, wipe and configure from scratch. A real error (a
# missing Vulkan header, say) fails again there with its own message.
if [ -f "$BUILD/build.ninja" ]; then
  meson setup --reconfigure "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}" || {
    echo "==> reconfigure failed; configuring $BUILD from scratch"
    rm -rf "$BUILD"
    meson setup "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}"
  }
else
  meson setup "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}"
fi
echo "==> ninja -C $BUILD"
