#!/usr/bin/env bash
# Rebuild libdisc (the CD-ROM image model, a Rust staticlib) and relink the
# QEMU binaries that carry it. meson does not track liblibdisc.a as an
# input (cc.find_library), so after a change under libdisc/ a plain ninja
# would keep the old code inside qemu-system-i386 / qemu-img / the embed
# library: this removes those targets so ninja relinks them.
#   scripts/build-libdisc.sh            # cargo build --release -p libdisc + relink
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cargo build --release -p libdisc
if [ -f build/qemu/build.ninja ]; then
  case "$(uname -s)" in Darwin) SO=dylib;; *) SO=so;; esac
  rm -f build/qemu/qemu-system-i386 build/qemu/qemu-system-x86_64 build/qemu/qemu-img build/qemu/qemu-io \
        build/qemu/libqemu-embed-i386.$SO build/qemu/libqemu-embed-x86_64.$SO
  ninja -C build/qemu qemu-system-i386 qemu-img qemu-io libqemu-embed-i386.$SO
fi
