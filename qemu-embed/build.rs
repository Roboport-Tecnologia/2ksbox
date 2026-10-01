//! Names the QEMU build directory to the crate (`QEMU_EMBED_BUILD_DIR`),
//! the last place `load` looks for libqemu-embed-<target>, so a checkout's
//! `target/release/player` runs in place. Nothing is linked: the library
//! is opened at run time, one per machine (`src/lib.rs`).
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // The Windows cross build keeps its own QEMU build directory so one
    // checkout can hold both (scripts/win-cross.sh, docs/build-windows.md),
    // and so does the Intel build made on an Apple Silicon Mac
    // (scripts/build.sh --x86_64, docs/build-macos.md "The Intel build").
    // On an Intel Mac itself the x86_64 target is the native one.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let default = manifest.join(if target_os == "windows" {
        "../build/win/qemu"
    } else if target_os == "macos" && target_arch == "x86_64" && cfg!(target_arch = "aarch64") {
        "../build/x86_64/qemu"
    } else {
        "../build/qemu"
    });
    let dir = std::env::var("QEMU_EMBED_LIB_DIR")
        .map(PathBuf::from)
        .unwrap_or(default);
    let dir = dir.canonicalize().map(strip_verbatim).unwrap_or(dir);
    println!("cargo:rerun-if-env-changed=QEMU_EMBED_LIB_DIR");
    warn_if_overlay_stale(&manifest);
    println!("cargo:rustc-env=QEMU_EMBED_BUILD_DIR={}", dir.display());
}

/// `canonicalize` on a Windows host answers `\\?\C:\...`, a verbatim path in
/// which `/` is not a separator, and the linker joins `/libqemu-embed-…`
/// onto the search directory, so a native Windows build (MSYS2, docs/
/// build-windows.md) would not find the library. A drive path loses the
/// prefix; anything else (a UNC share) is left as it was.
fn strip_verbatim(dir: PathBuf) -> PathBuf {
    let s = dir.to_string_lossy();
    match s.strip_prefix(r"\\?\") {
        Some(rest) if rest.as_bytes().get(1) == Some(&b':') => PathBuf::from(rest),
        _ => dir,
    }
}

/// `qemu/embed/` is an rsync copy of `embed/` made by `scripts/prepare-qemu.sh`;
/// after a `git pull` that touches the embed API the copy (and the library
/// built from it) lags behind the bindings, which surfaces as an undefined
/// `qemu_embed_*` symbol at link time. Say so up front.
fn warn_if_overlay_stale(manifest: &std::path::Path) {
    let root = manifest.join("..");
    for f in ["libqemu_embed.h", "libqemu_embed.c", "embedaudio.c"] {
        let src = root.join("embed").join(f);
        let copy = root.join("qemu/embed").join(f);
        println!("cargo:rerun-if-changed={}", src.display());
        println!("cargo:rerun-if-changed={}", copy.display());
        let (Ok(a), Ok(b)) = (std::fs::read(&src), std::fs::read(&copy)) else {
            continue;
        };
        if a != b {
            println!(
                "cargo:warning=qemu/embed/{f} is stale vs embed/{f}: run \
                 scripts/prepare-qemu.sh && scripts/configure-qemu.sh, then rebuild \
                 libqemu-embed-<target> (ninja) before linking the player"
            );
            return;
        }
    }
}
