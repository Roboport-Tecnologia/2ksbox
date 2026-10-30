#!/usr/bin/env bash
# Prepare the DXVK submodule tree: restore every tracked file our patch queue
# touches, then apply patches/dxvk/*.patch in filename order, and
# patches/dxvk/dxbc-spirv/*.patch inside its dxbc-spirv submodule. Deterministic
# and idempotent, like prepare-qemu.sh. Never `git checkout` files inside
# third_party/dxvk by hand between runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DXVK="$ROOT/third_party/dxvk"

[ -f "$DXVK/meson.build" ] || { echo "dxvk submodule missing (git submodule update --init third_party/dxvk)"; exit 1; }
# DXVK's own submodules (Vulkan/SPIR-V headers, dxbc-spirv, libdisplay-info)
if [ ! -f "$DXVK/include/vulkan/include/vulkan/vulkan.h" ] || [ ! -f "$DXVK/subprojects/dxbc-spirv/meson.build" ]; then
  echo "==> initialising dxvk's submodules"
  git -C "$DXVK" submodule update --init --recursive --depth 1
fi

# patches are plain `git diff` output (a/ b/ prefixes, applied with -p1)
patched_files() { sed -n 's|^+++ b/||p' "$@" | sort -u; }
created_files() { awk '/^--- \/dev\/null/ { getline; sub(/^\+\+\+ b\//, ""); print }' "$@" | sort -u; }

echo "==> restoring tracked files touched by patches/dxvk"
patched_files "$ROOT"/patches/dxvk/*.patch | while read -r f; do
  if git -C "$DXVK" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    git -C "$DXVK" checkout -q -- "$f"
  fi
done
created_files "$ROOT"/patches/dxvk/*.patch | while read -r f; do rm -f "$DXVK/$f"; done

echo "==> applying our patch queue (patches/dxvk/*.patch)"
for p in "$ROOT"/patches/dxvk/*.patch; do
  echo "    $(basename "$p")"
  git -C "$DXVK" apply "$p"
done

# dxbc-spirv (DXVK's SM1-3 compiler) is a submodule of DXVK's, so its
# patches are relative to it and applied inside it, restored the same way
SPV="$DXVK/subprojects/dxbc-spirv"
echo "==> restoring and applying patches/dxvk/dxbc-spirv"
patched_files "$ROOT"/patches/dxvk/dxbc-spirv/*.patch | while read -r f; do
  if git -C "$SPV" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    git -C "$SPV" checkout -q -- "$f"
  fi
done
for p in "$ROOT"/patches/dxvk/dxbc-spirv/*.patch; do
  echo "    dxbc-spirv/$(basename "$p")"
  git -C "$SPV" apply "$p"
done
echo "==> dxvk tree ready ($(git -C "$DXVK" rev-parse --short HEAD) + $(ls "$ROOT"/patches/dxvk/*.patch | wc -l | tr -d ' ') patches, dxbc-spirv + $(ls "$ROOT"/patches/dxvk/dxbc-spirv/*.patch | wc -l | tr -d ' '))"
