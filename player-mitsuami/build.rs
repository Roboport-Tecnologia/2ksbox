// Where the binary finds libqemu-embed, as the winit player's build.rs
// says it: `<prefix>/lib/2ksbox` once installed (origin-relative, first),
// else the build directory it was linked from.
fn main() {
    let target = std::env::var("TARGET").unwrap_or_default();
    if !(target.contains("apple") || target.contains("linux")) {
        return;
    }
    let relative = if target.contains("apple") {
        "@loader_path/../lib/2ksbox"
    } else {
        "$ORIGIN/../lib/2ksbox"
    };
    println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{relative}");
    if let Ok(dir) = std::env::var("DEP_QEMU_EMBED_LIBDIR") {
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{dir}");
    }
}
