//! The disc shelf's model (doc 07: "disc shelf editing, one-click
//! guest-tools ISO attach").
//!
//! One object, two modes. Opened on its own it manages the shared shelf
//! (`disc_library.rs`): add, label, remove. Opened from a machine it also
//! carries that machine's two disc decisions: which disc is in the drive
//! at **boot** (a bundle edit, `Machine::disc`) and, while the machine
//! runs, which disc to **insert** now (a monitor command, `control.rs`).
//! The shelf is not filtered per machine: it is the user's collection,
//! and any disc can go in any drive.
//!
//! Library edits save as they are made, with no "Save" button, because a
//! shelf is a list of things you own, not a document being drafted. The
//! front end decides when they are written: a label field calls
//! `set_label` then `flush` once when its edit is finished, rather than
//! writing the file on every keystroke.
//!
//! The rows are in the shelf's own order (by label, `disc_library`'s
//! invariant), so a row number is only good until the next edit.
//!
//! **One drive.** To the person at the window a machine has one CD
//! drive, shown as a card above the shelf. `insert` and `eject` act on it
//! whether or not the machine is up: on a stopped machine they set the
//! disc it boots with, and on a running one they also swap the disc now,
//! so the drive still holds it after a restart. While the machine runs
//! the card shows what is in the drive at this moment (`refresh_live`),
//! which the guest can change too.

use crate::bundle::Machine;
use crate::control;
use crate::disc_library::{self, Disc, DiscKind, DiscLibrary};
use std::path::{Path, PathBuf};

/// What the window needs when it was opened for a particular machine.
struct MachineContext {
    bundle_path: PathBuf,
    name: String,
    /// The machine's current boot disc, mirrored here so a click updates
    /// the row markers before the library is rescanned.
    boot: Option<PathBuf>,
}

#[derive(Default)]
pub struct Shelf {
    /// Whether the window is up.
    pub open: bool,
    library_path: PathBuf,
    library: DiscLibrary,
    /// The shelf changed and needs writing back.
    dirty: bool,
    /// Set by `flush` when it actually wrote; see `take_saved`.
    saved: bool,
    /// `None` when the window was opened for the shelf itself rather
    /// than for one machine.
    machine: Option<MachineContext>,
    /// What the running machine's drive holds (`None` inside for an
    /// empty tray), or `None` when its monitor did not answer: a stopped
    /// machine, or one started outside the launcher.
    live: Option<Option<PathBuf>>,
    status: Option<String>,
    error: Option<String>,
}

impl Shelf {
    /// Open on the shared shelf alone, with no machine context.
    pub fn open_library(&mut self, library_path: &Path) {
        *self = Shelf { open: true, ..Default::default() };
        self.load(library_path);
    }

    /// Open for one machine: the same shelf, plus its boot-disc choice
    /// and (while running) live insert.
    pub fn open_for(&mut self, machine: &Machine, bundle_path: PathBuf, library_path: &Path) {
        *self = Shelf {
            open: true,
            machine: Some(MachineContext {
                bundle_path,
                name: machine.name.clone(),
                boot: machine.boot_disc().cloned(),
            }),
            ..Default::default()
        };
        self.load(library_path);
        self.refresh_live();
    }

    /// The same, from a bundle path, for a front end that addresses its
    /// windows by path rather than by a `Machine` it already holds. A
    /// bundle that won't load still opens the shelf (the shared half
    /// works), with the reason in `error`.
    pub fn open_for_path(&mut self, bundle_path: PathBuf, library_path: &Path) {
        match Machine::load(&bundle_path) {
            Ok(machine) => self.open_for(&machine, bundle_path, library_path),
            Err(e) => {
                let message = format!("{}: {e}", bundle_path.display());
                self.open_library(library_path);
                self.error = Some(message);
            }
        }
    }

    fn load(&mut self, library_path: &Path) {
        self.library_path = library_path.to_path_buf();
        match DiscLibrary::load(library_path) {
            Ok(library) => self.library = library,
            // A corrupt shelf is reported, never replaced with an empty
            // one, which the next save would write over it.
            Err(e) => self.error = Some(format!("{}: {e}", library_path.display())),
        }
    }

    /// The window's title: the machine's name when it has one.
    pub fn title(&self) -> String {
        match &self.machine {
            Some(m) => format!("Discs · {}", m.name),
            None => "Disc shelf".to_string(),
        }
    }

    pub fn for_machine(&self) -> bool {
        self.machine.is_some()
    }

    pub fn discs(&self) -> &[Disc] {
        &self.library.discs
    }

    /// Rename one row: the field reports a finished edit and this is what
    /// it calls.
    ///
    /// The row moves to where the new name belongs (the shelf is kept in
    /// order by label), so a caller holding row numbers must re-read
    /// them. A front end that re-reads the whole shelf after every
    /// operation already does.
    pub fn set_label(&mut self, row: usize, label: &str) {
        let Some(disc) = self.library.discs.get_mut(row) else { return };
        if disc.label == label {
            return;
        }
        disc.label = label.to_string();
        self.library.sort();
        self.dirty = true;
    }

    /// The machine's boot disc, or `None` for an empty tray.
    pub fn boot(&self) -> Option<&Path> {
        self.machine.as_ref().and_then(|m| m.boot.as_deref())
    }

    /// The label for it, ready to print.
    pub fn boot_label(&self) -> String {
        self.boot().map(disc_library::default_label).unwrap_or_else(|| "(empty tray)".to_string())
    }

    /// Ask the running machine what is in its drive. Called when the
    /// window opens and after every live change; a front end also calls
    /// it on a timer while the window is up, because the guest swaps and
    /// ejects discs on its own. A machine that does not answer is a
    /// stopped one, not an error.
    pub fn refresh_live(&mut self) {
        let probe = self.probe_live();
        self.apply_live(probe);
    }

    /// `refresh_live`'s question without its answer applied, so a front
    /// end polling it can leave the window alone when nothing changed
    /// (`live_changed`).
    pub fn probe_live(&self) -> LiveProbe {
        let Some(dir) = self.bundle_dir() else { return LiveProbe::default() };
        match control::Control::connect(&control::socket_path(dir)) {
            Err(_) => LiveProbe::default(),
            Ok(mut c) => match c.cd_medium() {
                Ok(disc) => LiveProbe { drive: Some(disc), error: None },
                Err(e) => LiveProbe { drive: None, error: Some(e) },
            },
        }
    }

    pub fn live_changed(&self, probe: &LiveProbe) -> bool {
        self.live != probe.drive || probe.error.is_some()
    }

    pub fn apply_live(&mut self, probe: LiveProbe) {
        self.live = probe.drive;
        if probe.error.is_some() {
            self.error = probe.error;
        }
    }

    /// Whether the machine is up, as far as the last `refresh_live` knows.
    pub fn running(&self) -> bool {
        self.live.is_some()
    }

    /// The disc in the drive: what the running machine holds now, or
    /// what a stopped one boots with.
    pub fn drive(&self) -> Option<&Path> {
        match &self.live {
            Some(disc) => disc.as_deref(),
            None => self.boot(),
        }
    }

    /// The card above the shelf. `None` when the window was opened on the
    /// shelf alone, which has no drive.
    pub fn drive_card(&self) -> Option<DriveCard> {
        self.machine.as_ref()?;
        Some(match self.drive() {
            Some(path) => DriveCard {
                kind: Some(DiscKind::of(path)),
                title: self.label_for(path),
                detail: disc_library::detail(path),
            },
            None => DriveCard {
                kind: None,
                title: "Tray empty".into(),
                detail: if self.running() {
                    "Insert a disc from the library below.".into()
                } else {
                    "The machine boots without a disc. Insert one from the library below.".into()
                },
            },
        })
    }

    /// A disc's name: its shelf label, or the file's for a disc that is
    /// not on the shelf (the guest put it in, or it was removed since).
    fn label_for(&self, path: &Path) -> String {
        match self.library.position(path) {
            Some(i) => self.library.discs[i].label.clone(),
            None => disc_library::default_label(path),
        }
    }

    /// How many discs, beside the shelf's heading.
    pub fn count_label(&self) -> String {
        match self.library.discs.len() {
            1 => "1 disc".into(),
            n => format!("{n} discs"),
        }
    }

    pub fn row_kind(&self, row: usize) -> Option<DiscKind> {
        self.library.discs.get(row).map(|d| DiscKind::of(&d.path))
    }

    /// The line under a row's label: "Disc image · ~/Games/zork.iso".
    pub fn row_detail(&self, row: usize) -> String {
        self.library.discs.get(row).map(|d| disc_library::detail(&d.path)).unwrap_or_default()
    }

    /// Whether this row is the disc in the drive, which shows as a mark
    /// in place of its Insert button.
    pub fn row_in_drive(&self, row: usize) -> bool {
        self.machine.is_some() && self.library.discs.get(row).is_some_and(|d| self.drive() == Some(d.path.as_path()))
    }

    /// Put `disc` in the drive: the boot disc, and on a running machine
    /// the disc in the tray now as well.
    pub fn insert(&mut self, disc: &Path) {
        let label = self.label_for(disc);
        if self.running() {
            self.insert_live(disc);
            if self.error.is_some() {
                return;
            }
            self.refresh_live();
        }
        if self.set_boot(Some(disc.to_path_buf())).is_some() {
            self.status = Some(format!("inserted {label}"));
        }
    }

    pub fn insert_row(&mut self, row: usize) {
        if let Some(path) = self.library.discs.get(row).map(|d| d.path.clone()) {
            self.insert(&path);
        }
    }

    /// Empty the drive, now and at the next boot.
    pub fn eject(&mut self) {
        if self.running() {
            self.eject_live();
            if self.error.is_some() {
                return;
            }
            self.refresh_live();
        }
        if self.set_boot(None).is_some() {
            self.status = Some("ejected".into());
        }
    }

    /// Write pending shelf edits now.
    pub fn flush(&mut self) -> std::io::Result<()> {
        if std::mem::take(&mut self.dirty) {
            self.library.save(&self.library_path)?;
            self.saved = true;
        }
        Ok(())
    }

    /// `flush`, with any failure folded into `error` rather than
    /// returned, for the end of a frame or a handler, where there is
    /// nobody to return an error to.
    pub fn flush_reporting(&mut self) {
        if let Err(e) = self.flush() {
            self.error = Some(e.to_string());
        }
    }

    /// Whether the shelf was written since this was last asked. If so,
    /// the caller republishes it to any running machine's drive, so the
    /// in-guest CDSHELF program sees a disc the moment it is added.
    pub fn take_saved(&mut self) -> bool {
        std::mem::take(&mut self.saved)
    }

    /// The directory of the bundle this window has open, so the caller
    /// can tell whether that machine is the running one.
    pub fn bundle_dir(&self) -> Option<&Path> {
        self.machine.as_ref().and_then(|m| m.bundle_path.parent())
    }

    /// Put a disc on the shelf.
    ///
    /// Both windows call this from more than one place, one of them the
    /// "Browse…" dialog itself. A file chosen there is on the shelf
    /// before the dialog closes, rather than landing in the text field
    /// for a second click on "Add to shelf"; a picker that seemed to do
    /// nothing was reported as a bug. The field and its button are for a
    /// typed path, and "Add folder…" adds immediately too. A disc added
    /// by mistake is one "Remove" away.
    pub fn add(&mut self, path: PathBuf) {
        let label = disc_library::default_label(&path);
        if self.library.add(path) {
            self.dirty = true;
            self.status = Some(format!("added {label}"));
            self.error = None;
        } else {
            // The same image added twice is one entry, not two rows that
            // then disagree about their labels.
            self.status = Some(format!("{label} is already on the shelf"));
        }
    }

    /// Doc 07's one-click guest-tools attach: no path to find, no
    /// browsing, just the driver/test ISO this checkout last built.
    pub fn add_guest_tools(&mut self) {
        match disc_library::guest_tools_iso() {
            Some(iso) => self.add(iso),
            None => self.error = Some("no guest-tools ISO built (guest-tools/build-wrappers.sh)".into()),
        }
    }

    /// Whether the guest-tools ISO `add_guest_tools` would add is on the
    /// shelf already, for a check mark on its menu item.
    pub fn has_guest_tools(&self) -> bool {
        disc_library::guest_tools_iso().is_some_and(|iso| self.library.position(&iso).is_some())
    }

    pub fn remove(&mut self, path: &Path) {
        if let Some(i) = self.library.position(path) {
            let disc = self.library.discs.remove(i);
            self.dirty = true;
            self.status = Some(format!("removed {}", disc.label));
            self.error = None;
        }
    }

    pub fn remove_row(&mut self, row: usize) {
        if let Some(path) = self.library.discs.get(row).map(|d| d.path.clone()) {
            self.remove(&path);
        }
    }

    /// Set (or clear) the open machine's boot disc, writing the bundle.
    /// Returns the bundle path on success so the caller can rescan.
    pub fn set_boot(&mut self, path: Option<PathBuf>) -> Option<PathBuf> {
        let machine = self.machine.as_mut()?;
        // Re-read rather than editing a `Machine` captured when the
        // window opened: the wizard may have saved the same bundle in
        // between, and only the boot disc is this window's to change.
        let result = Machine::load(&machine.bundle_path).and_then(|mut m| {
            m.disc = path.clone();
            m.discs.clear();
            m.save(&machine.bundle_path)
        });
        match result {
            Ok(()) => {
                machine.boot = path.clone();
                self.status = Some(match &path {
                    Some(p) => format!("boots with {}", disc_library::default_label(p)),
                    None => "boots with an empty tray".to_string(),
                });
                self.error = None;
                Some(self.machine.as_ref()?.bundle_path.clone())
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    pub fn set_boot_row(&mut self, row: usize) -> Option<PathBuf> {
        let path = self.library.discs.get(row).map(|d| d.path.clone())?;
        self.set_boot(Some(path))
    }

    /// Swap `disc` into the running machine's drive.
    pub fn insert_live(&mut self, disc: &Path) {
        let name = disc.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let disc = disc.to_path_buf();
        self.live(|c| c.insert_disc(&disc), &format!("inserted {name}"));
    }

    pub fn insert_live_row(&mut self, row: usize) {
        if let Some(path) = self.library.discs.get(row).map(|d| d.path.clone()) {
            self.insert_live(&path);
        }
    }

    pub fn eject_live(&mut self) {
        self.live(|c| c.eject_disc(), "ejected");
    }

    /// Republish the shelf to one running machine's drive file, so the
    /// in-guest CDSHELF program sees the current list.
    pub fn publish_to(&self, bundle_dir: &Path) {
        crate::machines::publish_shelf(&self.library_path, &control::shelf_path(bundle_dir));
    }

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// The last operation's result: the status line, or the error.
    pub fn last_result(&self) -> Result<Option<&str>, &str> {
        match &self.error {
            Some(e) => Err(e),
            None => Ok(self.status.as_deref()),
        }
    }

    /// Run one operation on the running machine's monitor. A fresh
    /// connection each time (jobs and block nodes are QEMU-global, so
    /// nothing is lost, and there is no half-open socket to keep).
    /// Failures land in the error line rather than a panic, because the
    /// guest may have shut down between the repaint that drew the button
    /// and the click on it.
    fn live(&mut self, op: impl FnOnce(&mut control::Control) -> Result<(), String>, done: &str) {
        let Some(dir) = self.bundle_dir() else {
            self.status = None;
            self.error = Some("no machine open".into());
            return;
        };
        let result = control::Control::connect(&control::socket_path(dir)).and_then(|mut c| op(&mut c));
        match result {
            Ok(()) => {
                self.status = Some(done.to_string());
                self.error = None;
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }
}

/// The card above the shelf that shows the machine's drive. Eject is
/// offered when `kind` is set.
pub struct DriveCard {
    /// `None` for an empty tray.
    pub kind: Option<DiscKind>,
    pub title: String,
    pub detail: String,
}

/// What the running machine's monitor said about its drive
/// (`Shelf::probe_live`).
#[derive(Default)]
pub struct LiveProbe {
    drive: Option<Option<PathBuf>>,
    error: Option<String>,
}
