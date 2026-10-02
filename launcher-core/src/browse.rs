//! The part of "Browse…" that is not a dialog.
//!
//! The file dialog itself is the toolkit's (mitsuami's is GTK's
//! `FileDialog`, onto the XDG portal, or `QtQuick.Dialogs`' under
//! Kirigami on Linux, `NSOpenPanel` on macOS and `FileOpenPicker` on
//! Windows). This module holds the decision any front
//! end has to get right: which extensions a field offers, and which
//! directory the dialog opens in. A `.slangp` lives somewhere nobody
//! would navigate to by hand, so a wrong start directory leaves the user
//! in their home directory with no idea where to go.

use std::path::{Path, PathBuf};

/// One extension filter for a dialog (e.g. `("Disk images", &["qcow2"])`).
/// A plain pair rather than a toolkit type, so each front end turns it
/// into whatever its own dialog wants (for mitsuami, a `FileFilter` over
/// [`extensions`]; for a dialog that takes name filters, a
/// `"Disk images (*.qcow2)"` string from [`name_filter`]).
pub type Filter<'a> = (&'a str, &'a [&'a str]);

/// The extensions a dialog is handed: each one in lower and upper case.
/// The constants are lower case, and on Linux every backend (the XDG
/// portal, GTK, KDE's dialog) matches the glob case-sensitively, so a
/// `GAME.CUE` written by a DOS-era tool was hidden from the disc shelf's
/// "Browse…". Both spellings rather than a `*.[cC][uU][eE]` class,
/// because Windows' and macOS's dialogs take no classes and a backend
/// that case-folds the globs itself would mangle one. A mixed-case
/// `.Cue` is the one spelling this misses.
pub fn extensions(filter: Filter) -> Vec<String> {
    let mut out = Vec::new();
    for e in filter.1 {
        for v in [e.to_ascii_lowercase(), e.to_ascii_uppercase()] {
            if !out.contains(&v) {
                out.push(v);
            }
        }
    }
    out
}

/// A `"Disk images (*.qcow2 *.QCOW2 *.img *.IMG)"` name filter, for a
/// dialog that takes its filters as one string per entry, so such a front
/// end uses these constants instead of repeating the extension lists by
/// hand.
pub fn name_filter(filter: Filter) -> String {
    let globs: Vec<String> = extensions(filter).iter().map(|e| format!("*.{e}")).collect();
    format!("{} ({})", filter.0, globs.join(" "))
}

/// The directory a path field's "Browse…" should open in: the value's own
/// directory if it names one (a file inside it, or the directory itself),
/// `None` if the field is empty or names a bare filename. Hand the dialog
/// this directory, never the file path, which breaks it.
pub fn start_dir(value: &str) -> Option<PathBuf> {
    if value.is_empty() {
        return None;
    }
    let path = Path::new(value);
    if path.is_dir() {
        return Some(path.to_path_buf());
    }
    path.parent().filter(|p| !p.as_os_str().is_empty()).map(|p| p.to_path_buf())
}

/// Where "Browse…" opens: the field's own value if it points somewhere
/// (`start_dir`), else the caller's suggestion for an empty field (the
/// preset collection, for the shader editor's preset field), else the
/// directory the last dialog was browsing (`last_dir`), else the OS
/// default. It is its own function so `cli`'s `--browse-start` verb can
/// check the choice without a modal dialog only a human could answer.
///
/// **Only the shader editor's preset field passes an `empty_dir`.** A
/// `.slangp` lives in a checkout's `third_party/` or a downloaded copy
/// under the platform data directory, and nobody navigates there by
/// hand. The user already knows where their disk images, install ISOs,
/// floppies, discs and screenshots are, so those fields open where the
/// user last browsed. Two "Browse…" buttons in one window opening in
/// different places looks like a bug, which is why the rule is written
/// here.
///
/// The launcher keeps the last-used location itself because no platform
/// picker did. The Qt launcher's `FileDialog` (retired 2026-10-02),
/// handed an empty folder, opened in the working directory, so every
/// empty field (a new machine's, and the disc shelf's adder, which
/// empties itself after every disc) started over from there.
pub fn browse_start(value: &str, empty_dir: Option<&Path>) -> Option<PathBuf> {
    start_dir(value).or_else(|| empty_dir.map(|d| d.to_path_buf())).or_else(last_dir)
}

/// `<platform data dir>/last-browse.txt`: the one line `remember` writes.
/// `LAUNCHER_BROWSE_MEMORY` overrides it, so a scripted run leaves the
/// user's own alone.
pub fn memory_path() -> PathBuf {
    if let Ok(path) = std::env::var("LAUNCHER_BROWSE_MEMORY") {
        return path.into();
    }
    crate::paths::data_dir()
        .map(|d| d.join("last-browse.txt"))
        .unwrap_or_else(|| PathBuf::from("last-browse.txt"))
}

/// The directory the last "Browse…" was showing, if it is still there.
/// Shared by every field and both dialogs (file and folder), and kept
/// across launcher runs.
pub fn last_dir() -> Option<PathBuf> {
    let text = std::fs::read_to_string(memory_path()).ok()?;
    let dir = PathBuf::from(text.trim_end_matches(['\r', '\n']));
    dir.is_dir().then_some(dir)
}

/// The path to keep for a file or folder a dialog handed back.
///
/// Inside a Flatpak the dialog is the XDG portal's, and what comes back
/// is not the file but a copy of its name under the document portal's
/// FUSE mount (`$XDG_RUNTIME_DIR/doc/<id>/<name>`), even for an app
/// that can reach the whole host. That path fails two ways here: the
/// portal exports the one file picked, so the `.bin` tracks behind a
/// `.cue` (the `.mdf` behind a `.mds`, the `.img` behind a `.ccd`) are
/// not beside it, and its filesystem answers QEMU's `F_OFD_GETLK` lock
/// test with EIO ("Failed to get \"consistent read\" lock"). The
/// portal records the real path on every exported file as the extended
/// attribute `user.document-portal.host-path`; this returns that path
/// when the file is reachable, which `--filesystem=host` makes it. Any
/// other path comes back as it is. `launcherx --picked` prints the
/// answer, and `package-flatpak.sh` asks it inside the sandbox.
pub fn picked(path: &Path) -> PathBuf {
    match host_path_of_portal_document(path) {
        Some(real) if real.exists() => real,
        _ => path.to_path_buf(),
    }
}

#[cfg(target_os = "linux")]
fn host_path_of_portal_document(path: &Path) -> Option<PathBuf> {
    use std::os::unix::ffi::OsStrExt;
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")?;
    if !path.starts_with(Path::new(&runtime).join("doc")) {
        return None;
    }
    let c_path = std::ffi::CString::new(path.as_os_str().as_bytes()).ok()?;
    let name = c"user.document-portal.host-path";
    let mut buf = vec![0u8; 4096];
    // SAFETY: both strings are NUL-terminated and the buffer is as long as
    // it says; getxattr writes at most that many bytes.
    let n = unsafe { libc::getxattr(c_path.as_ptr(), name.as_ptr(), buf.as_mut_ptr().cast(), buf.len()) };
    if n <= 0 {
        return None;
    }
    buf.truncate(n as usize);
    Some(PathBuf::from(std::ffi::OsStr::from_bytes(&buf)))
}

#[cfg(not(target_os = "linux"))]
fn host_path_of_portal_document(_path: &Path) -> Option<PathBuf> {
    None
}

/// A dialog handed back `picked` (a file, or a folder from a folder
/// dialog). Remember the directory it was picked in, which is where the
/// dialog was browsing. A failure to write is only a warning; the next
/// dialog then opens where it would have anyway.
pub fn remember(picked: &Path) {
    let Some(dir) = picked.parent().filter(|p| !p.as_os_str().is_empty()) else {
        return;
    };
    let path = memory_path();
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Err(e) = std::fs::write(&path, format!("{}\n", dir.display())) {
        eprintln!("launcher: cannot remember the browse directory in {}: {e}", path.display());
    }
}
