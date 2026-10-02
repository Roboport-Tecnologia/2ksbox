#!/usr/bin/env bash
# The oldest macOS a Mac build of 2ksbox runs on, in the form
# MACOSX_DEPLOYMENT_TARGET takes:
#
#   scripts/macos-floor.sh                 12.0
#
# Why this number (docs/build-macos.md, "The floor"): the app carries
# nothing of the Mac's package manager any more (scripts/build-deps.sh
# builds QEMU's libraries from source, user decision 2026-09-23), so the
# floor is set by what those sources support. It was Qt 6.9's, the newest
# Qt line that still ran on macOS 12, while the launcher was Qt. The
# launcher is now launcher-mitsuami on AppKit (ADR-023), and 12 stands
# until a Mac that old has run it: mitsuami's own AppKit floor is not
# measured yet. QEMU, glib, pixman, libslirp, zstd, the LunarG loader,
# KosmicKrisp and Rust all go lower. `scripts/build.sh`
# builds everything for this target and `package-macos.sh` fails on any
# file above it. A preset MACOSX_DEPLOYMENT_TARGET wins everywhere, for a
# one-off build.
set -euo pipefail
case "${1:-}" in
  '') echo 12.0 ;;
  *) echo "usage: macos-floor.sh" >&2; exit 2 ;;
esac
