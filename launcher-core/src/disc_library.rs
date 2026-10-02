//! The disc shelf (doc 07): the user's collection of disc images, shared
//! by every machine rather than owned by one.
//!
//! It started per machine (`Machine::discs`), which was wrong. A rip of
//! Blood disc 2 belongs to the person, not to the XP box that installed
//! it first. Two machines wanting the same disc had to list it twice, and
//! a disc added while setting one machine up was invisible to the next.
//! So the shelf is one flat `discs.toml` next to the machine and
//! shader-profile libraries, and a machine keeps only `Machine::disc`,
//! the disc in its drive at boot. Anything else is inserted at runtime
//! (`control.rs`).
//!
//! Entries have labels because a shelf of `d1.cue`, `disc2.cue`,
//! `cd1.iso` is not a library. The label defaults to the file name and is
//! editable.
//!
//! **The shelf is kept in order by label**, not in the order discs were
//! added, because people look a title up. The order is an invariant of
//! `DiscLibrary` rather than a sort each view does, so every consumer
//! agrees: the GUI, the C ABI, the `--discs` verb, and the flat file the
//! in-guest CDSHELF program lists. That file is written straight out of
//! this list and addressed by slot number, so a view that sorted for
//! itself would show a disc under one number and load another.

use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::path::{Path, PathBuf};

/// What a file dialog should offer when picking a disc, as a (label,
/// extensions) pair. It is the shelf's definition of a disc image, so it
/// lives here, shared, rather than in a front end's file picker.
pub const DISC_FILTER: (&str, &[&str]) = ("Disc images", &["iso", "cue", "ccd", "mds"]);

/// How QEMU is told to open a shelf entry.
///
/// A **directory** is a disc too. `isodir` generates an ISO 9660 + Joliet
/// volume over the tree as the guest reads it (M5g,
/// `docs/tracks/m5-dirdisc.md`), which is how someone hands a pile of
/// files to a machine with no useful networking. It is reached by prefix
/// because a directory can be neither probed nor opened as a file, so the
/// choice is made here, once, for everything that names a medium to
/// QEMU: the boot drive (`bundle.rs`), a live insert (`control.rs`) and
/// the flat shelf file the guest's own CDSHELF reads.
///
/// Decided from the path each time rather than remembered, so a folder
/// deleted since is reported as a missing file rather than a folder disc
/// QEMU cannot generate.
pub fn qemu_medium(path: &Path) -> String {
    if path.is_dir() {
        format!("isodir:{}", path.display())
    } else {
        path.display().to_string()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Disc {
    /// What to call it in a list. Defaults to the file name.
    pub label: String,
    pub path: PathBuf,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiscLibrary {
    #[serde(default, rename = "disc")]
    pub discs: Vec<Disc>,
}

/// `<platform data dir>/discs.toml`, beside `machines/` and
/// `shader-profiles/` (doc 07: "bundles live in a plain, documented
/// directory layout the user can back up"). `LAUNCHER_DISC_LIBRARY`
/// overrides it.
pub fn default_path() -> PathBuf {
    if let Ok(path) = std::env::var("LAUNCHER_DISC_LIBRARY") {
        return path.into();
    }
    crate::paths::data_dir().map(|d| d.join("discs.toml")).unwrap_or_else(|| PathBuf::from("discs.toml"))
}

/// A label for a disc that has none: the file name without its
/// extension, which is what a rip is usually named after. A shared folder
/// keeps its whole name, extension and all (`Patch 1.3` is a name,
/// `Patch 1` a mangling of one).
pub fn default_label(path: &Path) -> String {
    let name = if is_folder(path) { path.file_name() } else { path.file_stem() };
    name.map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| path.display().to_string())
}

/// Whether a shelf entry is a shared folder, for its icon and label. A
/// disc image's extension answers without asking the filesystem: a disc
/// on a network share that has gone idle (or a mapped drive whose server
/// is off) blocks every `is_dir` for seconds while Windows reconnects,
/// and the windows ask once per row each time the shelf changes.
/// `qemu_medium` still asks, once, when the disc is handed to QEMU.
fn is_folder(path: &Path) -> bool {
    let image = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| DISC_FILTER.1.iter().any(|x| x.eq_ignore_ascii_case(e)));
    !image && path.is_dir()
}

/// What a shelf entry is, for its icon and the word under its label.
/// Image formats are not told apart: an `.iso`, a `.cue` and an `.mds`
/// are all a disc to the person picking one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiscKind {
    Disc,
    /// A host folder served as a generated disc (`qemu_medium`).
    Folder,
    /// One of our guest-tools ISOs, any revision (`guest_tools_iso`).
    GuestTools,
}

impl DiscKind {
    /// Decided from the path each time, like `qemu_medium` (`is_folder`).
    pub fn of(path: &Path) -> DiscKind {
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if is_folder(path) {
            DiscKind::Folder
        } else if name.starts_with("guest-tools-") && name.ends_with(".iso") {
            DiscKind::GuestTools
        } else {
            DiscKind::Disc
        }
    }

    /// A stable name for a front end to pick an icon by.
    pub fn key(self) -> &'static str {
        match self {
            DiscKind::Disc => "disc",
            DiscKind::Folder => "folder",
            DiscKind::GuestTools => "tools",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            DiscKind::Disc => "Disc image",
            DiscKind::Folder => "Folder",
            DiscKind::GuestTools => "Guest tools",
        }
    }
}

/// A path as a row shows it: the home directory as `~`.
pub fn display_path(path: &Path) -> String {
    let home = directories::BaseDirs::new().map(|b| b.home_dir().to_path_buf());
    match home.as_deref().and_then(|h| path.strip_prefix(h).ok()) {
        Some(rest) => Path::new("~").join(rest).display().to_string(),
        None => path.display().to_string(),
    }
}

/// The line under a disc's label: what it is and where it lives.
pub fn detail(path: &Path) -> String {
    format!("{} · {}", DiscKind::of(path).label(), display_path(path))
}

/// The shelf's order: by label, the way a shelf of discs is looked
/// through.
///
/// Case-insensitive, because nobody thinks of `blood2.cue` as coming
/// after `Zork`, and **digit runs compare as numbers**, because disc sets
/// are numbered and a plain string sort puts `Disc 10` between `Disc 1`
/// and `Disc 2`. Equal labels are broken by path, so the order is total
/// and two rips with the same name keep a stable position.
pub fn compare(a: &Disc, b: &Disc) -> Ordering {
    label_cmp(&a.label, &b.label).then_with(|| a.path.cmp(&b.path))
}

fn label_cmp(a: &str, b: &str) -> Ordering {
    let (mut x, mut y) = (a.chars().peekable(), b.chars().peekable());
    loop {
        let (ca, cb) = match (x.peek().copied(), y.peek().copied()) {
            (None, None) => return Ordering::Equal,
            (None, Some(_)) => return Ordering::Less,
            (Some(_), None) => return Ordering::Greater,
            (Some(ca), Some(cb)) => (ca, cb),
        };
        let ordering = if ca.is_ascii_digit() && cb.is_ascii_digit() {
            let (na, nb) = (digits(&mut x), digits(&mut y));
            // Compared by value without parsing: a leading-zero-trimmed
            // run is ordered by length first, so an eleven-digit number
            // in a label can't overflow anything.
            let (ta, tb) = (na.trim_start_matches('0'), nb.trim_start_matches('0'));
            ta.len().cmp(&tb.len()).then_with(|| ta.cmp(tb))
        } else {
            x.next();
            y.next();
            lower(ca).cmp(&lower(cb))
        };
        if ordering != Ordering::Equal {
            return ordering;
        }
    }
}

fn digits(it: &mut std::iter::Peekable<std::str::Chars>) -> String {
    let mut run = String::new();
    while let Some(c) = it.peek().copied().filter(char::is_ascii_digit) {
        run.push(c);
        it.next();
    }
    run
}

fn lower(c: char) -> char {
    c.to_lowercase().next().unwrap_or(c)
}

impl DiscLibrary {
    /// Put the shelf back in order. Called by everything that can
    /// disturb it (a load, an add, a rename), so a caller never has to
    /// remember to.
    pub fn sort(&mut self) {
        self.discs.sort_by(compare);
    }

    /// Read the shelf. A missing file is an empty shelf (a fresh
    /// install), not an error. A corrupt one is an error, so a hand-edit
    /// gone wrong is reported instead of the collection being overwritten
    /// with an empty one.
    pub fn load(path: &Path) -> std::io::Result<DiscLibrary> {
        match std::fs::read_to_string(path) {
            // Sorted on the way in, so a hand-edited file (or one an
            // older launcher wrote in the order discs were added) comes
            // up in order like any other.
            Ok(text) => toml::from_str::<DiscLibrary>(&text).map_err(std::io::Error::other).map(|mut l| {
                // A shelf written by a launcher in a Flatpak before
                // `browse::picked` holds document-portal paths; each
                // becomes the file's own here and is saved so on the
                // next change.
                for d in &mut l.discs {
                    d.path = crate::browse::picked(&d.path);
                }
                l.sort();
                l
            }),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(DiscLibrary::default()),
            Err(e) => Err(e),
        }
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let text = toml::to_string_pretty(self).map_err(std::io::Error::other)?;
        std::fs::write(path, text)
    }

    pub fn position(&self, path: &Path) -> Option<usize> {
        self.discs.iter().position(|d| d.path == path)
    }

    /// Put a disc on the shelf. Returns false if it was already there:
    /// the same image added twice is one entry, not two rows that then
    /// disagree about their labels.
    pub fn add(&mut self, path: PathBuf) -> bool {
        if self.position(&path).is_some() {
            return false;
        }
        self.discs.push(Disc { label: default_label(&path), path });
        self.sort();
        true
    }

    /// Pull `machine.toml`'s legacy per-machine `discs` lists onto the
    /// shared shelf, so nothing a user added before this change is lost.
    /// Idempotent (`add` deduplicates by path), so it can run after every
    /// library scan. Returns how many were new.
    pub fn import_legacy(&mut self, machines: &[crate::library::LibraryEntry]) -> usize {
        let mut added = 0;
        for entry in machines {
            for disc in &entry.machine.discs {
                if self.add(disc.clone()) {
                    added += 1;
                }
            }
        }
        added
    }
}

/// Write the shelf in the flat form QEMU's ATAPI handler reads
/// (`cdshelf/cdshelf_proto.h`): one `<label>\t<path>` line per disc.
///
/// Not `discs.toml` itself, because QEMU's side of this is C and a
/// tab-separated line file needs only a short parser. Tabs and newlines
/// in a label are replaced with spaces rather than rejected, so a stray
/// tab never loses a disc. A path containing a newline is skipped
/// instead: this format cannot hold it, and truncating it would point
/// the guest at the wrong file.
pub fn write_shelf_file(library: &DiscLibrary, path: &Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut out = String::new();
    for disc in library.discs.iter().take(crate::disc_library::MAX_SHELF_ENTRIES) {
        let path_str = qemu_medium(&disc.path);
        if path_str.contains('\n') {
            eprintln!("[discs] skipping {path_str:?}: a newline in a path can't be written to the shelf file");
            continue;
        }
        let label = disc.label.replace(['\t', '\n', '\r'], " ");
        out.push_str(&label);
        out.push('\t');
        out.push_str(&path_str);
        out.push('\n');
    }
    std::fs::write(path, out)
}

/// Matches `CDSHELF_FILE_MAX_ENTRIES` in `cdshelf/cdshelf_proto.h`: the
/// guest walks the reply with a fixed stride and a bounded buffer.
pub const MAX_SHELF_ENTRIES: usize = 256;

/// Windows 11 on Arm's drivers disc (`scripts/build-virtio-win.sh`):
/// virtio-win's ARM64 network and display drivers, which Windows on Arm
/// has none of in the box. Shipped as `share/2ksbox/drivers/`, built
/// into `build/virtio-win` in a checkout; `LAUNCHER_ARM_DRIVERS_ISO`
/// overrides both. `None` when it is not there, and the machine then
/// starts without it (`player::prepare` says so).
pub fn arm_drivers_iso() -> Option<PathBuf> {
    let path = match std::env::var("LAUNCHER_ARM_DRIVERS_ISO") {
        Ok(path) => PathBuf::from(path),
        Err(_) => crate::paths::resource("share/2ksbox/drivers", "build/virtio-win").join(ARM_DRIVERS_ISO),
    };
    path.is_file().then(|| crate::paths::canonical(&path).unwrap_or(path))
}

/// The drivers disc's file name, in either place.
pub const ARM_DRIVERS_ISO: &str = "2ksbox-drivers-arm64.iso";

/// The newest guest-tools ISO (`guest-tools/build-wrappers.sh` writes
/// `guest-tools/out/guest-tools-3dfx-<rev>.iso`), for doc 07's
/// "one-click guest-tools ISO attach". Shipped as
/// `share/2ksbox/guest-tools/` in an installed tree, built into
/// `guest-tools/out` in a checkout (`paths.rs`, like
/// `player::pc_bios_dir`). `LAUNCHER_GUEST_TOOLS_ISO` overrides both with
/// an explicit path.
pub fn guest_tools_iso() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("LAUNCHER_GUEST_TOOLS_ISO") {
        return Some(path.into());
    }
    let dir = crate::paths::resource("share/2ksbox/guest-tools", "guest-tools/out");
    let mut candidates: Vec<(std::time::SystemTime, PathBuf)> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(|e| e.ok())
        .filter(|e| {
            let name = e.file_name();
            let name = name.to_string_lossy();
            name.starts_with("guest-tools-") && name.ends_with(".iso")
        })
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    candidates.sort();
    // Canonicalized because this path is stored. The build-time anchor
    // is `<manifest>/../guest-tools/out`, and a `launcher-core/../guest-tools`
    // on the shelf would be correct but hard to read.
    candidates.pop().map(|(_, path)| crate::paths::canonical(&path).unwrap_or(path))
}
