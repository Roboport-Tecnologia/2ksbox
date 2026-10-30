#!/usr/bin/env bash
# scripts/win.ps1's way into MINGW64: runs `bash "$@"` (a script and its
# arguments, or -i for a shell) with Rust's cargo on PATH. A login shell
# that is not interactive returns from ~/.bashrc before the line that
# puts it there (docs/build-windows.md, "Building on Windows", step 2).
set -euo pipefail
CARGO_BIN="$(cygpath "${CARGO_HOME:-$USERPROFILE/.cargo}")/bin"
case ":$PATH:" in *":$CARGO_BIN:"*) ;; *) PATH="$CARGO_BIN:$PATH" ;; esac
export PATH
exec bash "$@"
