//! Where the launcher's companions are: the `player` binary, our
//! `qemu-img`, QEMU's `pc-bios` firmware, the guest-tools ISO, a shipped
//! shader collection. An installed launcher has no checkout to find them
//! in.
//!
//! So there are two layouts, asked in this order. An explicit
//! `LAUNCHER_*` environment variable wins over both; each caller checks
//! its own first.
//!
//! 1. **Installed.** The layout `scripts/package-linux.sh` stages and
//!    doc 07 documents, found relative to the running executable
//!    (`<exe dir>/..`), so the whole tree can be moved or extracted
//!    anywhere and still work. `share/2ksbox` is the marker: a launcher
//!    that merely sits in some `bin/` is not installed.
//! 2. **A source checkout.** `CARGO_MANIFEST_DIR`, baked in at compile
//!    time (like `qemu-embed/build.rs`'s own default). Not the current
//!    working directory, which a bare relative path would use and which
//!    isn't always the workspace root; running the launcher from
//!    elsewhere gave "No such file or directory".
//!
//! It is one or the other, never a mixture. An installed launcher answers
//! only with its own prefix, even for a file the package left out.
//! Falling through to the checkout would let a package tested on a
//! developer's machine work there and fail everywhere else, which is the
//! bug packaging exists to catch. A missing file inside the prefix is
//! reported as missing by whichever window wanted it.

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// The product name: the resource directories inside a prefix
/// (`share/2ksbox`, `lib/2ksbox`, …), the launcher's installed executable
/// and the user's own data directory. The old working name,
/// `win98-xp-virt`, survives only in `migrate_data_dir` below.
pub const NAME: &str = "2ksbox";

/// The application ID: the desktop entry's filename, the icon's name, the
/// Wayland `app_id` the compositor matches between the two, and the
/// Flatpak/AppStream ID. Reverse-DNS of `2ksbox.com`, with the leading
/// digit escaped as `_2ksbox` because a name segment may not start with
/// one (`flatpak build-init` rejects `com.2ksbox.…`; the same convention
/// gives `7-zip.org` `org._7zip.…`). Not the macOS bundle ID: Apple
/// forbids the underscore, so the app is `com.2ksbox.2ksbox` (`-community` for that build, ADR-011).
pub const APP_ID: &str = "com._2ksbox.Launcher";

/// The prefix this launcher is installed under, or `None` when it is a
/// binary in a checkout's `target/`. Computed once: it is a couple of
/// `stat`s, but callers ask per frame.
pub fn install_prefix() -> Option<&'static Path> {
    static PREFIX: OnceLock<Option<PathBuf>> = OnceLock::new();
    PREFIX.get_or_init(detect_prefix).as_deref()
}

fn detect_prefix() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    if cfg!(windows) {
        // A Windows package is one folder the user unzips and opens: the
        // executables at the top, the DLLs beside them (where the loader
        // looks), the data directories under it. So the prefix is the
        // executable's own directory, and `pc-bios` is the marker. Every
        // package has it, and nothing else would put it next to a stray
        // copy of the launcher.
        let dir = exe.parent()?;
        return dir.join("pc-bios").is_dir().then(|| dir.to_path_buf());
    }
    let prefix = exe.parent()?.parent()?;
    prefix.join("share").join(NAME).is_dir().then(|| prefix.to_path_buf())
}

/// The directory inside the prefix that holds executables: `bin` under a
/// Unix prefix, the flat prefix itself on Windows, and `MacOS` inside a
/// macOS `.app`, the one directory Launch Services starts a program from
/// (`scripts/package-macos.sh`). Detected from the running executable
/// rather than from the prefix's shape, because a plain tarball extracted
/// on a Mac is still an ordinary Unix prefix with a `bin`.
pub fn bin_dir() -> PathBuf {
    let exe = std::env::current_exe().ok();
    let Some(prefix) = install_prefix() else {
        // A checkout: the workspace's binaries share one directory, the
        // running executable's own. That is right for a debug build as
        // well as a release one, which a baked-in `target/release` would
        // not be.
        return exe.and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_else(|| checkout("."));
    };
    if cfg!(windows) {
        return prefix.to_path_buf();
    }
    let in_bundle = exe
        .and_then(|e| e.parent().map(|d| d.file_name() == Some("MacOS".as_ref())))
        .unwrap_or(false);
    prefix.join(if in_bundle { "MacOS" } else { "bin" })
}

/// A companion's place inside the prefix. Unix keeps the
/// `bin`/`lib`/`libexec`/`share` split doc 07 documents; a Windows
/// package is flat, so the same name loses the directory that only
/// keeps a Unix prefix tidy (`share/2ksbox/pc-bios` → `pc-bios`). Done
/// once here rather than at every call site, so one string describes
/// both layouts.
fn in_prefix(installed: &str) -> &str {
    if !cfg!(windows) {
        return installed;
    }
    for lead in ["share/2ksbox/", "lib/2ksbox/", "libexec/2ksbox/", "bin/"] {
        if let Some(rest) = installed.strip_prefix(lead) {
            return rest;
        }
    }
    installed
}

/// A companion at `installed` under the install prefix, or at `checkout`
/// in the source tree, whichever layout this binary is running in.
/// Always returns a path. Whether it exists is the caller's concern,
/// since for most of these "missing" is a state the UI already reports
/// (no guest-tools ISO built, no preset collection yet).
pub fn resource(installed: &str, checkout_rel: &str) -> PathBuf {
    match install_prefix() {
        Some(prefix) => prefix.join(in_prefix(installed)),
        None => checkout(checkout_rel),
    }
}

/// A file the package shipped, or `None` in a checkout or a package
/// built without it. For the companions that exist only as a package's
/// own copy (the macOS app's Vulkan loader and driver, which stock macOS
/// lacks), where a checkout uses the system's.
pub fn shipped(installed: &str) -> Option<PathBuf> {
    let path = install_prefix()?.join(in_prefix(installed));
    path.is_file().then_some(path)
}

/// `rel` in the workspace checkout this binary was built from. A Windows
/// binary built from a Linux checkout (docs/build-windows.md) finds
/// QEMU's own artefacts under `build/win/qemu`, since that checkout holds
/// both builds at once.
pub fn checkout(rel: &str) -> PathBuf {
    let root = Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/.."));
    if cfg!(windows) {
        if let Some(rest) = rel.strip_prefix("build/qemu") {
            return root.join(format!("build/win/qemu{rest}"));
        }
    }
    root.join(rel)
}

/// `std::fs::canonicalize`, minus the verbatim prefix it puts on every
/// Windows path (`\\?\C:\...`, `\\?\UNC\server\share\...`). A path that is
/// stored (a machine's disk, a qcow2's backing file, a disc on the shelf)
/// or handed to QEMU takes the plain form, which every Windows program
/// reads and the user recognises. Elsewhere it is canonicalize itself.
pub fn canonical(path: &Path) -> std::io::Result<PathBuf> {
    let c = std::fs::canonicalize(path)?;
    let Some(s) = c.to_str() else { return Ok(c) };
    if let Some(unc) = s.strip_prefix(r"\\?\UNC\") {
        return Ok(PathBuf::from(format!(r"\\{unc}")));
    }
    match s.strip_prefix(r"\\?\") {
        Some(rest) if rest.as_bytes().get(1) == Some(&b':') => Ok(PathBuf::from(rest)),
        _ => Ok(c),
    }
}

/// The user's own directory: `machines/`, `discs.toml`, `shader-profiles/`
/// and a downloaded preset collection (`~/.local/share/2ksbox` on Linux,
/// `~/Library/Application Support/2ksbox` on macOS, `%APPDATA%\2ksbox\data`
/// on Windows, and `%USERPROFILE%\2ksbox` when the launcher runs as an
/// installed MSIX, below). `None` only where the platform has no home
/// directory at all, which every caller answers with a bare relative path.
///
/// It was `win98-xp-virt` until the repository took the product's name
/// (ADR-011), so an existing library is **moved once** here, the first
/// time anything asks: a plain rename inside the same parent directory,
/// atomic, and only when the new name does not exist yet. A user who
/// upgrades finds their machines where they left them. One who has both
/// directories (two versions run side by side) keeps both and is told
/// which one is in use; nothing is merged.
///
/// **A packaged launcher keeps its library outside `AppData`.** Windows
/// virtualises a packaged app's `%APPDATA%`: every write lands in the
/// package's private copy, and an uninstall deletes that copy, which
/// here would be the user's machines and their disks. The Store refuses
/// the capability that turns the virtualisation off, so the library goes
/// to the profile root instead, `%USERPROFILE%\2ksbox`, as VirtualBox
/// keeps its `VirtualBox VMs` there: neither virtualised nor synced by
/// OneDrive, and untouched by an uninstall. The zip's library is not
/// adopted (a rename out of a virtualised directory is not a rename);
/// `docs/build-windows.md` "The Store package" says what to copy.
///
/// `LAUNCHER_DATA_DIR` overrides all of it (an empty value is unset). It
/// is how `scripts/package-windows.sh` keeps its checks out of the user's
/// own library on Windows, where Windows' known folders, not the
/// environment, place `%APPDATA%`, and no wine prefix stands in between.
pub fn data_dir() -> Option<&'static Path> {
    static DIR: OnceLock<Option<PathBuf>> = OnceLock::new();
    DIR.get_or_init(|| {
        if let Some(dir) = std::env::var_os("LAUNCHER_DATA_DIR").filter(|d| !d.is_empty()) {
            return Some(PathBuf::from(dir));
        }
        if packaged() {
            return Some(directories::BaseDirs::new()?.home_dir().join(NAME));
        }
        let dir = directories::ProjectDirs::from("", "", NAME)?.data_dir().to_path_buf();
        migrate_data_dir(&dir);
        Some(dir)
    })
    .as_deref()
}

/// Whether this process runs with package identity, i.e. out of an
/// installed MSIX (`docs/build-windows.md`, "The Store package"). Asked
/// of Windows through `GetCurrentPackageFullName`, which answers
/// `APPMODEL_ERROR_NO_PACKAGE` for a plain executable and a length (or
/// `ERROR_INSUFFICIENT_BUFFER`, since no buffer is offered) for a
/// packaged one. `LAUNCHER_PACKAGED=1` answers yes without a package, so
/// the packaged layout can be checked from a checkout or under wine.
/// Never true elsewhere.
pub fn packaged() -> bool {
    static PACKAGED: OnceLock<bool> = OnceLock::new();
    *PACKAGED.get_or_init(|| {
        if std::env::var_os("LAUNCHER_PACKAGED").is_some_and(|v| v == "1") {
            return true;
        }
        package_identity()
    })
}

#[cfg(windows)]
fn package_identity() -> bool {
    // Declared by hand rather than through a Windows bindings crate: one
    // kernel32 call is not worth a dependency the Flatpak's offline build
    // would have to vendor for a Linux build that never makes it.
    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentPackageFullName(length: *mut u32, name: *mut u16) -> i32;
    }
    const ERROR_SUCCESS: i32 = 0;
    const ERROR_INSUFFICIENT_BUFFER: i32 = 122;
    let mut len: u32 = 0;
    // SAFETY: a null buffer with a zero length is the documented way to
    // ask only for the required length; nothing is written.
    let rc = unsafe { GetCurrentPackageFullName(&mut len, std::ptr::null_mut()) };
    // Anything else (APPMODEL_ERROR_NO_PACKAGE, 15700, above all) is a
    // plain executable.
    rc == ERROR_INSUFFICIENT_BUFFER || rc == ERROR_SUCCESS
}

#[cfg(not(windows))]
fn package_identity() -> bool {
    false
}

/// The `win98-xp-virt` → `2ksbox` move, done once. Every failure is only
/// a warning: the launcher still starts, on an empty library, which is
/// recoverable by hand. Refusing to run would not be.
fn migrate_data_dir(new: &Path) {
    let Some(old) = directories::ProjectDirs::from("", "", "win98-xp-virt").map(|d| d.data_dir().to_path_buf()) else {
        return;
    };
    if old == new || !old.is_dir() {
        return;
    }
    if new.exists() {
        eprintln!("launcher: both {} and {} exist; using the latter", old.display(), new.display());
        return;
    }
    if let Some(parent) = new.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    match std::fs::rename(&old, new) {
        Ok(()) => eprintln!("launcher: moved {} to {}", old.display(), new.display()),
        Err(e) => eprintln!("launcher: cannot move {} to {}: {e}", old.display(), new.display()),
    }
}

/// The platform runtime directory (`/run/user/<uid>/2ksbox` on Linux) for
/// this machine's monitor socket, or the temp dir where there is none.
/// Nothing is migrated: what lives here belongs to a running process.
pub fn runtime_dir() -> PathBuf {
    directories::ProjectDirs::from("", "", NAME)
        .and_then(|d| d.runtime_dir().map(Path::to_path_buf))
        .unwrap_or_else(std::env::temp_dir)
}
