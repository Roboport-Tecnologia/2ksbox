include!("../packaging/windows/win-icon.rs");

fn main() {
    // The picture Explorer draws on 2ksbox-player.exe, and its manifest.
    embed_windows_resources();
    let target = std::env::var("TARGET").unwrap_or_default();
    let unix = target.contains("apple") || target.contains("linux");
    // An installed player is `<prefix>/bin/2ksbox-player` with the
    // embed library in `<prefix>/lib/2ksbox` (M6 step 6, doc 07's
    // install layout). Origin-relative, so the packaged tree can be
    // extracted anywhere. And *first*, so the same binary copied out of a
    // developer's `target/` is self-contained once packaged rather than
    // loading the library out of their build directory. In a checkout
    // that directory doesn't exist and the loader moves on to the
    // absolute one below.
    if unix {
        let relative = if target.contains("apple") {
            "@loader_path/../lib/2ksbox"
        } else {
            "$ORIGIN/../lib/2ksbox"
        };
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{relative}");
    }
    // rpath to libqemu-embed so `cargo run` / target/*/player find it in place
    if let Ok(dir) = std::env::var("DEP_QEMU_EMBED_LIBDIR") {
        if unix {
            println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{dir}");
        }
    }
}
