include!("../packaging/windows/win-icon.rs");

fn main() {
    // `sidebar`: the machines are the window's sidebar, the toolbar is
    // Start and the machine's windows, and the rest is in the menus (the
    // menu bar on macOS, the primary menu on GTK). Windows and Kirigami
    // keep the list and the details' buttons.
    println!("cargo::rustc-check-cfg=cfg(sidebar)");
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let feature = |f: &str| std::env::var_os(format!("CARGO_FEATURE_{f}")).is_some();
    if os == "macos" || (os == "linux" && feature("GTK") && !feature("KDE")) {
        println!("cargo::rustc-cfg=sidebar");
    }
    // The icon Explorer shows for the launcher and its manifest: this crate
    // becomes 2ksbox.exe in the Windows package.
    embed_windows_resources();
}
