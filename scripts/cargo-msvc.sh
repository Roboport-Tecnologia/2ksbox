#!/usr/bin/env bash
# cargo for Rust's MSVC target, from MSYS2's MINGW64 shell on Windows
# (ADR-026): what build-windows.sh and test.sh run for the MSVC Rust
# builds that live in the root workspace (the tools: launcherx, discx,
# synthx). Arguments go to `cargo build` and friends unchanged; the
# output lands in target/x86_64-pc-windows-msvc.
#
#   scripts/cargo-msvc.sh build --release -p libdisc --bin discx
#
# The toolchain is rustup's stable-x86_64-pc-windows-msvc, which finds
# Visual Studio's linker itself. MSYS2's /usr/bin (and /bin, the same
# directory) comes off PATH: its coreutils `link` is found before
# Microsoft's link.exe ("link: extra operand"). `+crt-static` keeps
# vcruntime140.dll out of the import table, as for the launcher.
set -euo pipefail

MSVC=stable-x86_64-pc-windows-msvc
TARGET=x86_64-pc-windows-msvc
if ! rustup run "$MSVC" rustc -V >/dev/null 2>&1; then
  echo "cargo-msvc.sh: no $MSVC toolchain (rustup toolchain install $MSVC; needs Visual Studio's C++ tools)" >&2
  exit 1
fi
cargo=$(command -v cargo)
nolink=""; IFS=: read -ra dirs <<< "$PATH"
for d in "${dirs[@]}"; do case "$d" in /usr/bin|/bin) ;; *) nolink="${nolink:+$nolink:}$d" ;; esac; done

# the subcommand first, then the target, then the caller's arguments
sub="$1"; shift
PATH="$nolink" CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS="-C target-feature=+crt-static" \
  exec "$cargo" "+$MSVC" "$sub" --target "$TARGET" "$@"
