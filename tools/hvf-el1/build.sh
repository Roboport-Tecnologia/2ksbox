#!/bin/sh
# Builds the hvf-el1 probe: the bare-metal EL1 payload (Rust,
# aarch64-unknown-none, flat binary) and the Hypervisor.framework host
# (signed ad hoc with the hypervisor entitlement). Outputs in build/hvf-el1/.
#   tools/hvf-el1/build.sh && build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT="$ROOT/build/hvf-el1"
mkdir -p "$OUT"
SYSROOT=$(rustc --print sysroot)
OBJCOPY=$(ls "$SYSROOT"/lib/rustlib/*/bin/llvm-objcopy | head -1)
rustup target list --installed | grep -q aarch64-unknown-none || rustup target add aarch64-unknown-none
cargo build --release --manifest-path payload/Cargo.toml --target aarch64-unknown-none --target-dir "$OUT/payload"
"$OBJCOPY" -O binary "$OUT/payload/aarch64-unknown-none/release/payload" "$OUT/payload.bin"
cargo build --release --manifest-path host/Cargo.toml --target-dir "$OUT/host"
cp "$OUT/host/release/hvf-el1" "$OUT/hvf-el1"
codesign --entitlements hv.entitlements --force -s - "$OUT/hvf-el1"
echo "built: $OUT/hvf-el1 $OUT/payload.bin ($(stat -f %z "$OUT/payload.bin") bytes)"
