//! The machine library grid's model: what is in the library, which of
//! them are running, and what "Play" does.
//!
//! The rows are `library::scan`'s entries and the running set is a
//! `bundle directory -> Child` map, where absence means "not running".
//! `reap` removes an entry the moment its child exits. A player process
//! cannot push that news, so the front end polls (`launcher-mitsuami`
//! from a task on the window, every half second).
//!
//! `play` publishes the shared shelf to the machine's drive before
//! spawning, so a disc added since the last run is on it. It derives the
//! monitor socket from the bundle directory rather than storing it,
//! which is how every other window finds it again (doc 07, "How the
//! launcher reaches a running machine").

use crate::bundle::{self, D3d9, Machine, Music, Video};
use crate::wizard::Section;
use crate::{control, disc_library, library, player, shader_library};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Child;

pub struct Machines {
    pub library_dir: PathBuf,
    /// The shared disc shelf's file (`disc_library.rs`). The shelf
    /// window re-reads it whenever it opens, so nothing here caches the
    /// discs themselves.
    pub disc_library_path: PathBuf,
    pub profiles_dir: PathBuf,
    entries: Vec<library::LibraryEntry>,
    profiles: Vec<shader_library::ProfileEntry>,
    running: HashMap<PathBuf, Child>,
}

impl Default for Machines {
    fn default() -> Self {
        Machines {
            library_dir: library::default_dir(),
            disc_library_path: disc_library::default_path(),
            profiles_dir: shader_library::default_dir(),
            entries: Vec::new(),
            profiles: Vec::new(),
            running: HashMap::new(),
        }
    }
}

impl Machines {
    /// A model on the default directories, already scanned.
    pub fn load() -> Machines {
        let mut machines = Machines::default();
        machines.refresh();
        machines
    }

    /// Rescan the library and the profile library from disk.
    pub fn refresh(&mut self) {
        self.entries = library::scan(&self.library_dir);
        self.profiles = shader_library::scan(&self.profiles_dir);
        import_legacy_discs(&self.entries, &self.disc_library_path);
    }

    /// Rescan only the profile library, after the profile manager saved
    /// or deleted one. That changes the grid's "Shader" column but not
    /// its rows.
    pub fn refresh_profiles(&mut self) {
        self.profiles = shader_library::scan(&self.profiles_dir);
    }

    pub fn entries(&self) -> &[library::LibraryEntry] {
        &self.entries
    }

    pub fn profiles(&self) -> &[shader_library::ProfileEntry] {
        &self.profiles
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn machine(&self, row: usize) -> Option<&Machine> {
        Some(&self.entries.get(row)?.machine)
    }

    pub fn dir(&self, row: usize) -> Option<&Path> {
        Some(&self.entries.get(row)?.dir)
    }

    /// The `machine.toml` of a row: how every other window is addressed,
    /// since they re-read the bundle rather than being handed a copy.
    pub fn bundle_path(&self, row: usize) -> Option<PathBuf> {
        Some(self.entries.get(row)?.dir.join(library::BUNDLE_FILE))
    }

    /// The label the "Shader" column shows: the profile's name if the
    /// machine names one that still exists, else a raw `shader`
    /// override's path, else the app default (named after the library's
    /// default profile when one is marked, `shader_library::default_label`).
    pub fn shader_label(&self, entry: &library::LibraryEntry) -> String {
        entry
            .machine
            .shader_profile
            .as_deref()
            .and_then(|id| self.profiles.iter().find(|e| shader_library::id_of(&e.path) == id))
            .map(|e| e.profile.name.clone())
            .or_else(|| entry.machine.shader.as_ref().map(|p| p.display().to_string()))
            .unwrap_or_else(|| shader_library::default_label_for(entry.machine.family, &self.profiles))
    }

    pub fn shader_label_at(&self, row: usize) -> String {
        self.entries.get(row).map(|e| self.shader_label(e)).unwrap_or_default()
    }

    /// The line under a machine's name in the list: its family, and
    /// whether it is running.
    pub fn subtitle(&self, row: usize) -> String {
        let Some(machine) = self.machine(row) else { return String::new() };
        let state = if self.is_running(row) { "Running" } else { "Stopped" };
        format!("{} · {state}", machine.family.label())
    }

    /// What the window shows of the selected machine, a group per page
    /// of the machine form: System, then Storage (what is in the drives
    /// is what is most often looked for, user), then the rest in the
    /// form's order. Only what the form would show for this family: no
    /// Direct3D row on a machine without our adapter.
    pub fn details(&self, row: usize) -> Vec<DetailGroup> {
        let Some(entry) = self.entries.get(row) else { return Vec::new() };
        details(&entry.machine, self.shader_label(entry))
    }

    pub fn is_running(&self, row: usize) -> bool {
        self.entries.get(row).map(|e| self.running.contains_key(&e.dir)).unwrap_or(false)
    }

    /// Whether the machine in a given bundle directory is up, for a
    /// per-machine window that knows its bundle, not its row.
    pub fn is_running_dir(&self, dir: &Path) -> bool {
        self.running.contains_key(dir)
    }

    pub fn running_dirs(&self) -> impl Iterator<Item = &PathBuf> {
        self.running.keys()
    }

    /// Start a machine's player. `Ok` carries the line to show, `Err`
    /// the reason it didn't start.
    /// The headline over `play`'s error, for a front end that shows it
    /// in an alert; the error is the line under it.
    pub fn start_failed(name: &str) -> String {
        format!("Couldn't start “{name}”")
    }

    pub fn play(&mut self, row: usize) -> Result<String, String> {
        let entry = self.entries.get(row).ok_or("no such machine")?;
        let dir = entry.dir.clone();
        let machine = entry.machine.clone();
        // The monitor socket is derived from the bundle directory, so
        // every window that wants live control finds it again without
        // the app storing it. The shelf is the file the guest's CDSHELF
        // program reads, refreshed here so a disc added since the last
        // run is on it.
        let socket = control::socket_path(&dir);
        let shelf = control::shelf_path(&dir);
        publish_shelf(&self.disc_library_path, &shelf);
        match player::spawn(&machine, Some(&socket), Some(&shelf)) {
            Ok(child) => {
                self.running.insert(dir, child);
                Ok(format!("started {}", machine.name))
            }
            Err(e) => Err(format!("{}: {e}", dir.display())),
        }
    }

    /// Reap any player that has exited, returning the rows whose running
    /// state just changed (a front end with a row-based view has to say
    /// which ones moved; one that redraws everything can ignore it).
    pub fn reap(&mut self) -> Vec<usize> {
        let mut ended: Vec<PathBuf> = Vec::new();
        self.running.retain(|dir, child| match child.try_wait() {
            Ok(None) => true,
            Ok(Some(status)) => {
                eprintln!("[launcher] {} exited: {status}", dir.display());
                ended.push(dir.clone());
                false
            }
            Err(e) => {
                eprintln!("[launcher] {}: {e}", dir.display());
                ended.push(dir.clone());
                false
            }
        });
        ended
            .iter()
            .filter_map(|dir| self.entries.iter().position(|e| &e.dir == dir))
            .collect()
    }

    /// Republish the shared shelf to every running machine's drive, so a
    /// disc added or renamed while a machine is up shows in the guest's
    /// CDSHELF listing without a restart.
    pub fn republish_shelf(&self) {
        for dir in self.running.keys() {
            publish_shelf(&self.disc_library_path, &control::shelf_path(dir));
        }
    }
}

/// One group of a machine's details: a page of the machine form, and its
/// settings as label and value.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct DetailGroup {
    pub title: &'static str,
    pub rows: Vec<DetailRow>,
}

/// One setting. A file's row shows the file's name and carries its whole
/// path, for a tooltip.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct DetailRow {
    pub label: &'static str,
    pub value: String,
    pub path: Option<PathBuf>,
}

impl From<(&'static str, String)> for DetailRow {
    fn from((label, value): (&'static str, String)) -> DetailRow {
        DetailRow { label, value, path: None }
    }
}

/// A machine's details, with the shader column's label for it. The
/// labels are the machine form's, so a row reads as the field it came
/// from; a file shows its name, the hard disk its name and two folders
/// (`short_path`), each with its whole path kept for a tooltip.
pub fn details(machine: &Machine, shader: String) -> Vec<DetailGroup> {
    let on_off = |on: bool| if on { "On" } else { "Off" }.to_owned();
    let file = |label: &'static str, path: Option<&PathBuf>| DetailRow {
        label,
        value: match path {
            Some(p) => p.file_name().map_or_else(|| p.display().to_string(), |n| n.to_string_lossy().into_owned()),
            None => "Empty".to_owned(),
        },
        path: path.cloned(),
    };
    let rows = |rows: Vec<(&'static str, String)>| rows.into_iter().map(DetailRow::from).collect::<Vec<_>>();
    let video = machine.effective_video();
    let mut display = Vec::new();
    if let Some(video) = video {
        display.push(("Display adapter", video.label().to_owned()));
    }
    if video == Some(Video::D3dpt) {
        display.push(("Direct3D", machine.d3d9.unwrap_or(D3d9::Auto).label().to_owned()));
    }
    let voodoo = match (machine.voodoo2, machine.voodoo2_undither) {
        (true, true) => "On, with the Voodoo3 undither filter".to_owned(),
        (on, _) => on_off(on),
    };
    display.push(("3dfx Voodoo 2", voodoo));
    display.push(("Shader profile", shader));
    let music = machine.effective_music();
    let mut audio = rows(vec![
        ("Sound card", machine.effective_sound().label().to_owned()),
        ("Music (MIDI)", music.label().to_owned()),
    ]);
    if music == Music::Gm && machine.soundfont.is_some() {
        audio.push(file("SoundFont", machine.soundfont.as_ref()));
    }
    let mut input = Vec::new();
    if bundle::pad_choices(machine.family).len() > 1 {
        input.push(("Gamepad", machine.effective_pad().label().to_owned()));
    }
    input.push(("Seamless mouse", on_off(machine.seamless_mouse)));
    let mut network = vec![("Networking", on_off(machine.network))];
    // Windows 11's sharing (M23), on the form's Network page too
    if machine.family.is_modern() {
        network.push(("Clipboard", on_off(machine.clipboard)));
    }
    let mut network = rows(network);
    if machine.family.is_modern() && machine.shared_folder.is_some() {
        network.push(file("Shared folder", machine.shared_folder.as_ref()));
    }
    vec![
        DetailGroup {
            title: Section::System.label(),
            rows: rows(vec![
                ("Memory", format!("{} MB", machine.ram_mb)),
                ("Processor", machine.effective_cpu_speed().label().to_owned()),
                ("Acceleration", machine.effective_accel().label().to_owned()),
            ]),
        },
        DetailGroup {
            title: Section::Storage.label(),
            rows: vec![
                DetailRow {
                    label: "Hard disk",
                    value: short_path(&machine.disk),
                    path: Some(machine.disk.clone()),
                },
                file("CD in drive", machine.boot_disc()),
                file("Floppy", machine.floppy.as_ref()),
                ("Boot from", machine.effective_boot().label().to_owned()).into(),
            ],
        },
        DetailGroup { title: Section::Display.label(), rows: rows(display) },
        DetailGroup { title: Section::Audio.label(), rows: audio },
        DetailGroup { title: Section::Input.label(), rows: rows(input) },
        DetailGroup { title: Section::Network.label(), rows: network },
    ]
}

/// A path cut to its file and the two folders above it, after `...`:
/// `.../machines/winxp/disk.qcow2`. Enough to tell one machine's disk
/// from another's without the whole path; a shorter path is shown whole.
fn short_path(path: &Path) -> String {
    let parts: Vec<_> = path.components().collect();
    if parts.len() <= 3 {
        return path.display().to_string();
    }
    let tail: PathBuf = parts[parts.len() - 3..].iter().collect();
    Path::new("...").join(tail).display().to_string()
}

/// Write the shared shelf out in the flat form a machine's ATAPI drive
/// reads (`cdshelf/cdshelf_proto.h`), so the in-guest CDSHELF program
/// sees the same discs the launcher does. Failing to publish is not
/// fatal: the machine still runs and its drive reports an empty shelf.
pub fn publish_shelf(library_path: &Path, shelf_path: &Path) {
    match disc_library::DiscLibrary::load(library_path) {
        Ok(library) => {
            if let Err(e) = disc_library::write_shelf_file(&library, shelf_path) {
                eprintln!("[discs] {}: {e}", shelf_path.display());
            }
        }
        Err(e) => eprintln!("[discs] {}: {e}", library_path.display()),
    }
}

/// Bundles written before the disc shelf became shared carry their own
/// per-machine `discs` list. Fold those onto the shared shelf so nothing
/// the user added is lost. `DiscLibrary::add` deduplicates by path, so
/// this is idempotent and runs on every rescan. The bundles themselves
/// migrate the next time anything saves them (`Machine::save` writes
/// `disc` and drops `discs`).
pub fn import_legacy_discs(entries: &[library::LibraryEntry], library_path: &Path) {
    match disc_library::DiscLibrary::load(library_path) {
        Ok(mut discs) => {
            let added = discs.import_legacy(entries);
            if added > 0 {
                match discs.save(library_path) {
                    Ok(()) => eprintln!("[discs] moved {added} disc(s) from machine bundles onto the shared shelf"),
                    Err(e) => eprintln!("[discs] {}: {e}", library_path.display()),
                }
            }
        }
        Err(e) => eprintln!("[discs] {}: {e}", library_path.display()),
    }
}
