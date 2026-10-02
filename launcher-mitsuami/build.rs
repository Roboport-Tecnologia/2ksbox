include!("../packaging/windows/win-icon.rs");

fn main() {
    // The icon Explorer shows for the launcher and its manifest: this crate
    // becomes 2ksbox.exe in the Windows package.
    embed_windows_resources();
}
