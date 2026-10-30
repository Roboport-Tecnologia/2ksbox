#!/usr/bin/env bash
# Refresh voodoo/86box/ (86Box's Voodoo 1/2 emulation, vendored verbatim)
# from a given upstream commit and show what changed.
#
#   scripts/sync-86box-voodoo.sh <86Box commit>
#
# Fetches the files listed in voodoo/86box/UPSTREAM at that commit over
# GitHub's raw endpoint (no clone: 86Box's history is large), overwrites
# the copies, prints `git diff --stat` and reminds you to update UPSTREAM.
# The port itself (voodoo/shim, voodoo/*.c) is not touched; if upstream
# grew a new dependency on 86Box's platform, the build says so.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/voodoo/86box"
COMMIT="${1:?usage: $0 <86Box commit>}"
RAW="https://raw.githubusercontent.com/86Box/86Box/$COMMIT/src"

C_FILES="vid_voodoo.c vid_voodoo_blitter.c vid_voodoo_display.c vid_voodoo_fb.c
         vid_voodoo_fifo.c vid_voodoo_reg.c vid_voodoo_render.c vid_voodoo_setup.c
         vid_voodoo_texture.c"
H_FILES="vid_voodoo_banshee.h vid_voodoo_banshee_blitter.h vid_voodoo_blitter.h
         vid_voodoo_codegen_arm64.h vid_voodoo_codegen_x86-64.h vid_voodoo_common.h
         vid_voodoo_display.h vid_voodoo_dither.h vid_voodoo_fb.h vid_voodoo_fifo.h
         vid_voodoo_reg.h vid_voodoo_regs.h vid_voodoo_render.h vid_voodoo_setup.h
         vid_voodoo_texture.h"

for f in $C_FILES; do
  echo "==> $f"
  curl -fsSL "$RAW/video/$f" -o "$DEST/$f"
done
for f in $H_FILES; do
  echo "==> $f"
  curl -fsSL "$RAW/include/86box/$f" -o "$DEST/$f"
done

git -C "$ROOT" diff --stat -- voodoo/86box
echo
echo "Now set the commit and date in voodoo/86box/UPSTREAM to $COMMIT, and rebuild."
