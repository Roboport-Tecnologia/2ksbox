#!/usr/bin/env bash
# The aarch64 player taking a qemu-system command line, so a harness that
# starts QEMU (tools/win11-spike.py, QEMU=<this>) runs the machine in the
# player instead: its embed-library features (the clipboard peer, M23)
# are then in the run. The player opens its window.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "${PLAYER:-$ROOT/target/qemu-aarch64/release/player}" -- "$@"
