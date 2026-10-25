//! The machine library (doc 07): a directory of bundle subdirectories,
//! each holding one `machine.toml`. "A plain, documented directory layout
//! the user can back up" (doc 07's settings taxonomy). No database, no
//! hidden index; the grid is a scan.

use crate::bundle::{Family, Machine};
use std::path::{Path, PathBuf};

pub const BUNDLE_FILE: &str = "machine.toml";

/// Help's item that opens `default_dir()` in the file manager.
pub const OPEN_FOLDER: &str = "Open Machines Folder";

/// Help's item that picks the machines folder again (`choose_dir`).
pub const CHOOSE_FOLDER: &str = "Choose Machines Folder…";

/// The library directory: the folder the user chose (`chosen_dir`), else
/// (`platform_dir`) the platform data dir (`~/.local/share` on Linux, `~/Library/Application
/// Support` on macOS, `%APPDATA%` on Windows) plus `machines`.
/// `LAUNCHER_LIBRARY_DIR` overrides both for development and tests, like
/// the player's `PLAYER_*` knobs.
pub fn default_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_LIBRARY_DIR") {
        return dir.into();
    }
    chosen_dir().unwrap_or_else(platform_dir)
}

/// The folder a launcher uses before one is chosen: `machines` in the
/// platform data dir.
fn platform_dir() -> PathBuf {
    crate::paths::data_dir().map(|d| d.join("machines")).unwrap_or_else(|| PathBuf::from("machines"))
}

/// The machines folder the user chose (`choose_dir`), if it is still there.
///
/// Every launcher asks once, at start (`needs_dir`, user decision
/// 2026-10-18), and Help's `CHOOSE_FOLDER` asks again, so machines can
/// live on another disk or in a folder the user backs up. Outside the
/// sandbox "Use Default Folder" (`keep_platform_dir`) answers it with the
/// folder used so far, so an upgrade keeps its machines where they were.
///
/// The App Store build has no such default. Its data dir is inside the
/// app's sandbox container, and macOS's open panel never shows a folder
/// in there: a machine kept in the container could not have its disk
/// browsed to, and "Browse…" opened somewhere else (2026-10-18). So it
/// offers only "Later", keeps the container's folder for that run and
/// asks again at the next start; a pick is kept with a security-scoped
/// bookmark (`grants`). Nothing is moved into a chosen folder (user
/// decision).
pub fn chosen_dir() -> Option<PathBuf> {
    let text = std::fs::read_to_string(chosen_file()?).ok()?;
    let dir = PathBuf::from(text.trim_end_matches(['\r', '\n']));
    (!dir.as_os_str().is_empty() && dir.is_dir()).then_some(dir)
}

/// `<data dir>/library.txt`: the one line `choose_dir` writes.
fn chosen_file() -> Option<PathBuf> {
    crate::paths::data_dir().map(|d| d.join("library.txt"))
}

/// Whether to ask at start: until a folder is chosen (and again if it
/// went away), unless `LAUNCHER_LIBRARY_DIR` names the library.
pub fn needs_dir() -> bool {
    std::env::var_os("LAUNCHER_LIBRARY_DIR").is_none() && chosen_dir().is_none()
}

/// Keep `dir`, picked in a folder dialog, as the library from now on,
/// with its access for later runs in the sandbox.
pub fn choose_dir(dir: &Path) -> std::io::Result<()> {
    let file = chosen_file().ok_or_else(|| std::io::Error::other("no data directory"))?;
    if let Some(parent) = file.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(&file, format!("{}\n", dir.display()))?;
    crate::grants::keep(dir);
    Ok(())
}

/// The question's other answer: outside the sandbox, keep the folder
/// used so far (made if new) and return it; in the sandbox ("Later")
/// nothing is kept and `None` comes back.
pub fn keep_platform_dir() -> std::io::Result<Option<PathBuf>> {
    if crate::grants::sandboxed() {
        return Ok(None);
    }
    let dir = platform_dir();
    std::fs::create_dir_all(&dir)?;
    choose_dir(&dir)?;
    Ok(Some(dir))
}

/// Where the folder dialog opens: beside the chosen folder, so it shows
/// in the list, else the user's real home folder. In the macOS sandbox
/// `$HOME` is the container, which the dialog does not show.
pub fn choose_start() -> Option<PathBuf> {
    if let Some(parent) = chosen_dir().as_deref().and_then(Path::parent) {
        return Some(parent.to_path_buf());
    }
    #[cfg(unix)]
    {
        // SAFETY: getpwuid's record is read at once, on this thread, and
        // pw_dir checked for null.
        let home = unsafe {
            let pw = libc::getpwuid(libc::getuid());
            if pw.is_null() || (*pw).pw_dir.is_null() {
                return None;
            }
            std::ffi::CStr::from_ptr((*pw).pw_dir).to_string_lossy().into_owned()
        };
        Some(PathBuf::from(home))
    }
    #[cfg(not(unix))]
    {
        directories::BaseDirs::new().map(|d| d.home_dir().to_path_buf())
    }
}

/// The question at start (`needs_dir`), and the folder dialog's prompt.
/// The words are here, not in a front end, like `firstrun`'s.
pub struct Question {
    pub headline: &'static str,
    pub detail: String,
    /// The buttons: pick a folder now, or the other answer
    /// (`keep_platform_dir`).
    pub choose: &'static str,
    pub other: &'static str,
    /// The folder dialog's message line.
    pub prompt: &'static str,
    /// The alert's headline when the pick cannot be kept.
    pub failed: &'static str,
}

pub const PROMPT: &str = "Choose or make the folder for your 2ksbox machines";
pub const FAILED: &str = "Couldn't use that folder for your machines.";

pub fn question() -> Question {
    let lead = "2ksbox keeps each machine's settings and disk in a folder, one per machine, all inside \
                the machines folder. Choose one, or make a new one, for example \"2ksbox\" in your \
                home folder or on another disk.";
    let (detail, other) = if crate::grants::sandboxed() {
        (
            format!("{lead}\n\nUntil you do, new machines are kept inside the app's own storage, which file dialogs can't open."),
            "Later",
        )
    } else {
        (
            format!("{lead}\n\nThe default folder is {}.", platform_dir().display()),
            "Use Default Folder",
        )
    };
    Question { headline: "Choose a folder for your machines", detail, choose: "Choose Folder…", other, prompt: PROMPT, failed: FAILED }
}

pub struct LibraryEntry {
    pub dir: PathBuf,
    pub machine: Machine,
}

/// A directory name from a machine name: lowercase, non-alphanumerics
/// collapsed to `-`, deduplicated against what's already in `dir`.
fn slug(dir: &Path, name: &str) -> String {
    let mut base: String = name
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while base.contains("--") {
        base = base.replace("--", "-");
    }
    let base = base.trim_matches('-');
    let base = if base.is_empty() { "machine" } else { base };
    let mut candidate = base.to_string();
    let mut n = 2;
    while dir.join(&candidate).exists() {
        candidate = format!("{base}-{n}");
        n += 1;
    }
    candidate
}

/// Reserve a fresh bundle subdirectory for `name` under `dir` (creating
/// both) without writing a `machine.toml` yet. The wizard needs the
/// directory first, to create a disk image inside it before the bundle
/// that references that disk can be written.
///
/// The folder is one this call made (`create_dir`, not `create_dir_all`):
/// a caller that fails removes it whole, which must never be another
/// machine's that a second launcher reserved under the same name.
pub fn reserve_dir(dir: &Path, name: &str) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    loop {
        let machine_dir = dir.join(slug(dir, name));
        match std::fs::create_dir(&machine_dir) {
            Ok(()) => return Ok(machine_dir),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
}

/// Create a new bundle under `dir` from doc 06's reference defaults,
/// returning the path to its `machine.toml`.
pub fn create(dir: &Path, family: Family, name: String, disk: PathBuf) -> std::io::Result<PathBuf> {
    let machine_dir = reserve_dir(dir, &name)?;
    let bundle_path = machine_dir.join(BUNDLE_FILE);
    Machine::reference(family, name, disk).save(&bundle_path)?;
    Ok(bundle_path)
}

/// Every bundle directly under `dir` (one level, not recursive). A
/// subdirectory without a readable `machine.toml` is skipped, so one
/// corrupt bundle doesn't take down the whole grid.
pub fn scan(dir: &Path) -> Vec<LibraryEntry> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut entries: Vec<LibraryEntry> = read
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .filter_map(|e| {
            let dir = e.path();
            match Machine::load(&dir.join(BUNDLE_FILE)) {
                Ok(machine) => Some(LibraryEntry { dir, machine }),
                Err(err) => {
                    eprintln!("[library] skipping {}: {err}", dir.display());
                    None
                }
            }
        })
        .collect();
    // By name, ignoring case ("doom" beside "Diablo", not after "Zork");
    // names equal but for case keep a fixed order.
    entries.sort_by(|a, b| {
        let (a, b) = (&a.machine.name, &b.machine.name);
        a.to_lowercase().cmp(&b.to_lowercase()).then_with(|| a.cmp(b))
    });
    entries
}
