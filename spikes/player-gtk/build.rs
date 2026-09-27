// rpath to libqemu-embed, as the player's own build.rs bakes it
fn main() {
    if let Ok(dir) = std::env::var("DEP_QEMU_EMBED_I386_LIBDIR") {
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{dir}");
    }
}
