//! "Clone…" on a grid row: a new machine that is a complete copy of an
//! existing one under a name the user picks. It gets the settings, its
//! own copy of the disk (internal snapshots included, because they live
//! inside the qcow2), and whatever else sits in the bundle directory.
//!
//! The rules, which are why this is one implementation:
//!
//! * **The disk is always copied**, wherever it is. A bundle made by the
//!   wizard keeps `disk.qcow2` in its own directory, but "Use an existing
//!   disk" can point anywhere (`~/vms/win98.qcow2`), and two machines on
//!   one image corrupt it the day both run. A disk outside the bundle
//!   lands in the clone's directory under its own file name. Anything
//!   else the bundle names inside its own directory (a floppy image, say)
//!   is copied with the tree and renamed into the clone. What it names
//!   outside it (discs on the shared shelf, a shader, a SoundFont) is
//!   shared media and stays shared.
//! * **Unless the user asks for the same disk** (`same_disk`): then the
//!   clone names the original's disk by its absolute path and only the
//!   rest of the bundle is copied. The window warns that only one of the
//!   two may run at a time; nothing enforces it beyond QEMU's own image
//!   lock.
//! * **A running machine is refused** when its disk is copied. Its disk is being written, and a
//!   copy taken under QEMU is not a disk that boots. "Running" is the
//!   grid's own player map or a monitor socket something is listening
//!   on, which also catches a player started by `--play` or by another
//!   launcher.
//! * **The copy runs on a thread**, since a Windows disk is gigabytes,
//!   and the front end polls it like a snapshot job. Progress is what has
//!   arrived in the new directory against what the old one holds.
//! * **The new `machine.toml` is written last**, so a clone that is
//!   still copying, or that failed, is never in the grid (`library::scan`
//!   skips a directory without one). A failure removes the directory,
//!   which this model created and nobody else has touched.
//! * **A relative qcow2 backing file is made absolute** in the copy
//!   (`qemu-img rebase -u`): the copy sits in another directory, where
//!   the relative name points at nothing.
//! * **A Windows 11 machine's TPM is copied too, unless the user asks for
//!   a new one** (`new_tpm`, user decision 2026-10-01). A copy keeps
//!   BitLocker and Windows Hello working on the clone, but both machines
//!   then answer with one TPM identity (one endorsement key). A new TPM
//!   is the state file left out, with the snapshots' copies of it
//!   (`tpm-snapshots/`), so the clone's first start manufactures one.

use crate::bundle::Machine;
use crate::{library, player};
use std::path::{Path, PathBuf};
use std::sync::mpsc;

pub struct CloneMachine {
    /// Whether the window is up.
    pub open: bool,
    /// The name field. A front end writes it as the user types.
    pub name: String,
    /// "Use the same hard disk": the clone boots the original's disk
    /// instead of a copy. A front end writes it from its checkbox.
    pub same_disk: bool,
    /// "Give the copy a new TPM" (Windows 11 only, `tpm_applies`). A
    /// front end writes it from its checkbox.
    pub new_tpm: bool,
    library_dir: PathBuf,
    source: Option<Source>,
    /// Whether the machine is up, the one thing that makes the window
    /// refuse outright.
    running: bool,
    error: Option<String>,
    job: Option<Job>,
    saved_path: Option<PathBuf>,
    /// The line the grid shows once a clone has landed.
    status: Option<String>,
}

struct Source {
    dir: PathBuf,
    machine: Machine,
    /// Every file the clone copies, and where in the new directory it
    /// goes. Worked out when the window opens, so the note can name the
    /// size before anything is copied.
    files: Vec<(PathBuf, PathBuf)>,
    /// Which of `files` is the disk.
    disk: usize,
    /// Whether the disk came from outside the bundle directory.
    disk_outside: bool,
    bytes: u64,
}

struct Job {
    dest_dir: PathBuf,
    /// Where each file lands, for the progress count.
    dests: Vec<PathBuf>,
    total: u64,
    done: mpsc::Receiver<Result<PathBuf, String>>,
}

impl Default for CloneMachine {
    fn default() -> Self {
        CloneMachine {
            open: false,
            name: String::new(),
            same_disk: false,
            new_tpm: false,
            library_dir: library::default_dir(),
            source: None,
            running: false,
            error: None,
            job: None,
            saved_path: None,
            status: None,
        }
    }
}

impl CloneMachine {
    /// Open the window on a bundle. `running` is the grid's own answer;
    /// a listening monitor socket counts too (see the header).
    pub fn open_for_path(&mut self, bundle_path: &Path, running: bool) {
        let dir = bundle_path.parent().unwrap_or(Path::new(".")).to_path_buf();
        let library_dir = std::mem::take(&mut self.library_dir);
        *self = CloneMachine { open: true, library_dir, ..Default::default() };
        self.running = running || monitor_listening(&dir);
        let machine = match Machine::load(bundle_path) {
            Ok(machine) => machine,
            Err(e) => {
                self.error = Some(format!("{}: {e}", bundle_path.display()));
                return;
            }
        };
        // The name is offered even when the copy cannot go ahead: it
        // depends on the library, not on the disk.
        self.name = default_name(&machine.name, &taken_names(&self.library_dir));
        match plan(&dir, &machine) {
            Ok(source) => self.source = Some(source),
            Err(e) => self.error = Some(e),
        }
    }

    /// The library the clone goes into: `library::default_dir()` unless
    /// a caller holds another one.
    pub fn set_library_dir(&mut self, dir: PathBuf) {
        self.library_dir = dir;
    }

    pub fn title(&self) -> String {
        match &self.source {
            Some(s) => format!("Clone \u{2014} {}", s.machine.name),
            None => "Clone machine".to_string(),
        }
    }

    /// The checkbox that shares the disk instead of copying it.
    pub fn same_disk_label(&self) -> &'static str {
        "Use the same hard disk instead of copying it"
    }

    /// Whether the TPM checkbox is a question: only a modern machine has
    /// a TPM.
    pub fn tpm_applies(&self) -> bool {
        self.source.as_ref().is_some_and(|s| s.machine.family.is_modern())
    }

    /// The checkbox that starts the clone on a new TPM.
    pub fn new_tpm_label(&self) -> &'static str {
        "Give the copy a new TPM"
    }

    /// What a clone is, with the size of what will be copied.
    pub fn note(&self) -> String {
        let Some(s) = &self.source else { return String::new() };
        if self.same_disk {
            return format!(
                "A new machine with the same settings, on the same disk ({}).{}",
                s.machine.disk.display(),
                self.tpm_note()
            );
        }
        let mut note = format!(
            "A new machine with the same settings and its own copy of the disk ({}), \
             snapshots included.",
            size_label(s.bytes)
        );
        if s.disk_outside {
            note.push_str(&format!(
                " The disk is copied from {} into the new machine's folder.",
                s.machine.disk.display()
            ));
        }
        note.push_str(self.tpm_note());
        note
    }

    /// The TPM's sentence, on a machine that has one.
    fn tpm_note(&self) -> &'static str {
        match (self.tpm_applies(), self.new_tpm) {
            (false, _) => "",
            (true, false) => " The TPM is copied too, so both machines have the same TPM.",
            (true, true) => {
                " The copy gets a new TPM. If the original uses BitLocker, the copy will ask for its recovery key."
            }
        }
    }

    /// Drawn as a warning: the shared disk's rule, or why Clone is off
    /// when it is not a matter of the name (the machine is running and
    /// its disk would be copied).
    pub fn warning(&self) -> Option<&'static str> {
        if self.same_disk {
            Some("Only one machine at a time may run on this disk. Two running at once will corrupt it.")
        } else {
            self.refused()
        }
    }

    fn refused(&self) -> Option<&'static str> {
        (self.running && !self.same_disk).then_some(
            "This machine is running. Shut it down before cloning it: its disk is being written, \
             and a copy taken now wouldn't boot.",
        )
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    pub fn busy(&self) -> bool {
        self.job.is_some()
    }

    pub fn can_submit(&self) -> bool {
        self.source.is_some() && self.refused().is_none() && !self.busy() && !self.name.trim().is_empty()
    }

    /// Copied so far and in all, while a copy runs.
    pub fn progress(&self) -> Option<(u64, u64)> {
        let job = self.job.as_ref()?;
        let copied: u64 = job.dests.iter().filter_map(|p| std::fs::metadata(p).ok()).map(|m| m.len()).sum();
        Some((copied.min(job.total), job.total))
    }

    pub fn progress_label(&self) -> String {
        match self.progress() {
            Some((done, total)) => format!("Copying\u{2026} {} of {}", size_label(done), size_label(total)),
            None => String::new(),
        }
    }

    /// The `machine.toml` the last clone wrote.
    pub fn saved_path(&self) -> Option<&Path> {
        self.saved_path.as_deref()
    }

    /// Start the copy. A refusal lands in `error` and nothing starts.
    pub fn submit(&mut self) {
        if self.busy() {
            return;
        }
        self.error = None;
        if let Err(e) = self.start() {
            self.error = Some(e);
        }
    }

    fn start(&mut self) -> Result<(), String> {
        if let Some(refusal) = self.refused() {
            return Err(refusal.to_string());
        }
        let source = self.source.as_ref().ok_or("No machine to clone.")?;
        let name = self.name.trim().to_string();
        if name.is_empty() {
            return Err("A name is required.".into());
        }
        if taken_names(&self.library_dir).iter().any(|n| n == &name) {
            return Err(format!("There is already a machine called \u{201c}{name}\u{201d}."));
        }
        let dest_dir = library::reserve_dir(&self.library_dir, &name).map_err(|e| e.to_string())?;
        let mut files: Vec<(PathBuf, PathBuf)> =
            source.files.iter().map(|(from, rel)| (from.clone(), dest_dir.join(rel))).collect();
        let mut machine = source.machine.clone();
        machine.name = name;
        remap(&mut machine, &source.dir, &dest_dir);
        // Set here, from the plan's own record of which file is the disk,
        // and never by comparing spellings of a path: a miss there would
        // leave the clone booting the original's disk when it should not,
        // or a copy when it should.
        let disk = if self.same_disk {
            let (original, _) = files.remove(source.disk);
            machine.disk = crate::paths::canonical(&original).unwrap_or(original);
            None
        } else {
            machine.disk = files[source.disk].1.clone();
            Some(source.disk)
        };
        // A new TPM: the state file and the snapshots' copies of it stay
        // behind. The disk is found again by where it goes, since the
        // list just shrank.
        let disk = if self.new_tpm && source.machine.family.is_modern() {
            let disk_to = disk.map(|i| files[i].1.clone());
            let state = std::fs::canonicalize(source.machine.effective_tpm_state()).ok();
            let copies = source.dir.join("tpm-snapshots");
            files.retain(|(from, _)| std::fs::canonicalize(from).ok() != state && !from.starts_with(&copies));
            disk_to.and_then(|to| files.iter().position(|(_, t)| t == &to))
        } else {
            disk
        };
        let total = files.iter().filter_map(|(from, _)| std::fs::metadata(from).ok()).map(|m| m.len()).sum();
        let dests = files.iter().map(|(_, to)| to.clone()).collect();
        let (tx, rx) = mpsc::channel();
        let thread_dir = dest_dir.clone();
        std::thread::spawn(move || {
            let result = copy_all(&files, disk, &machine, &thread_dir);
            if result.is_err() {
                let _ = std::fs::remove_dir_all(&thread_dir);
            }
            let _ = tx.send(result);
        });
        self.job = Some(Job { dest_dir, dests, total, done: rx });
        Ok(())
    }

    /// Ask the copy whether it has finished. Safe to call as often as a
    /// front end likes; returns true on the call that saw it end.
    pub fn poll(&mut self) -> bool {
        let Some(job) = &self.job else { return false };
        let result = match job.done.try_recv() {
            Ok(result) => result,
            Err(mpsc::TryRecvError::Empty) => return false,
            Err(mpsc::TryRecvError::Disconnected) => {
                let _ = std::fs::remove_dir_all(&job.dest_dir);
                Err("The copy stopped unexpectedly.".into())
            }
        };
        self.job = None;
        match result {
            Ok(path) => {
                let from = self.source.as_ref().map(|s| s.machine.name.as_str()).unwrap_or_default();
                self.status = Some(format!("cloned {from} as {}", self.name.trim()));
                self.saved_path = Some(path);
                self.open = false;
            }
            Err(e) => self.error = Some(e),
        }
        true
    }

    /// Wait out a copy, for a caller with no timer (`cli`, the C ABI).
    pub fn wait(&mut self) {
        while self.busy() {
            if !self.poll() {
                std::thread::sleep(std::time::Duration::from_millis(100));
            }
        }
    }
}

/// "Win98 (copy)", or "Win98 (copy 2)" and up when that is taken too.
pub fn default_name(original: &str, taken: &[String]) -> String {
    let mut candidate = format!("{original} (copy)");
    let mut n = 2;
    while taken.iter().any(|t| t == &candidate) {
        candidate = format!("{original} (copy {n})");
        n += 1;
    }
    candidate
}

fn taken_names(library_dir: &Path) -> Vec<String> {
    library::scan(library_dir).into_iter().map(|e| e.machine.name).collect()
}

/// What gets copied: every regular file under the bundle directory but
/// its `machine.toml` (written afresh, last), plus the disk if it lives
/// somewhere else.
fn plan(dir: &Path, machine: &Machine) -> Result<Source, String> {
    // Windows answers `metadata` on a device (`NUL`, `\\.\PhysicalDrive0`)
    // with an error, though it opens: that is a disk that is not a file.
    let disk_meta = std::fs::metadata(&machine.disk);
    if disk_meta.is_err() && std::fs::File::open(&machine.disk).is_ok() {
        return Err(format!("The disk {} is not a file.", machine.disk.display()));
    }
    let disk_meta = disk_meta.map_err(|e| format!("the disk {}: {e}", machine.disk.display()))?;
    if !disk_meta.is_file() {
        return Err(format!("The disk {} is not a file.", machine.disk.display()));
    }
    let mut files = Vec::new();
    walk(dir, Path::new(""), &mut files).map_err(|e| format!("{}: {e}", dir.display()))?;
    files.retain(|(_, rel)| rel != Path::new(library::BUNDLE_FILE));
    // The disk is found among the files by canonical path, not by how
    // the bundle spells it.
    let disk_canon = std::fs::canonicalize(&machine.disk).ok();
    let inside = files.iter().position(|(from, _)| std::fs::canonicalize(from).ok() == disk_canon);
    let (disk, disk_outside) = match inside {
        Some(i) => (i, false),
        None => {
            let file_name = machine.disk.file_name().map(PathBuf::from).unwrap_or_else(|| "disk.qcow2".into());
            let mut rel = file_name.clone();
            let mut n = 2;
            while rel == Path::new(library::BUNDLE_FILE) || files.iter().any(|(_, r)| r == &rel) {
                rel = PathBuf::from(format!("disk-{n}-{}", file_name.display()));
                n += 1;
            }
            files.push((machine.disk.clone(), rel));
            (files.len() - 1, true)
        }
    };
    let bytes = files.iter().filter_map(|(from, _)| std::fs::metadata(from).ok()).map(|m| m.len()).sum();
    Ok(Source { dir: dir.to_path_buf(), machine: machine.clone(), files, disk, disk_outside, bytes })
}

fn walk(dir: &Path, rel: &Path, out: &mut Vec<(PathBuf, PathBuf)>) -> std::io::Result<()> {
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let rel = rel.join(entry.file_name());
        // Followed, so a symlinked disk is copied as the disk it names.
        match std::fs::metadata(&path) {
            Ok(m) if m.is_dir() => walk(&path, &rel, out)?,
            Ok(m) if m.is_file() => out.push((path, rel)),
            _ => {}
        }
    }
    Ok(())
}

/// Point every path the bundle names inside its own directory at the
/// same file in the clone's. The disk is the exception; `start` places
/// it itself.
fn remap(machine: &mut Machine, from: &Path, to: &Path) {
    for path in [
        &mut machine.floppy,
        &mut machine.disc,
        &mut machine.shader,
        &mut machine.soundfont,
        &mut machine.mt32_roms,
        &mut machine.efi_vars,
        &mut machine.tpm_state,
    ]
    .into_iter()
    .flatten()
    {
        if let Ok(rel) = path.strip_prefix(from) {
            *path = to.join(rel);
        }
    }
}

/// `disk` is which of `files` is the disk, `None` when the clone shares
/// the original's.
fn copy_all(files: &[(PathBuf, PathBuf)], disk: Option<usize>, machine: &Machine, dest_dir: &Path) -> Result<PathBuf, String> {
    for (from, to) in files {
        if let Some(parent) = to.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        std::fs::copy(from, to).map_err(|e| format!("copying {}: {e}", from.display()))?;
    }
    if let Some(disk) = disk {
        absolute_backing(&files[disk].0, &files[disk].1)?;
    }
    let bundle_path = dest_dir.join(library::BUNDLE_FILE);
    machine.save(&bundle_path).map_err(|e| format!("{}: {e}", bundle_path.display()))?;
    Ok(bundle_path)
}

/// The backing file named in a qcow2 header, if any. Read straight out
/// of the header (magic, version, then the name's offset and length), so
/// a disk with none needs no `qemu-img` at all.
fn qcow2_backing(disk: &Path) -> std::io::Result<Option<String>> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(disk)?;
    let mut header = [0u8; 20];
    if f.read_exact(&mut header).is_err() || &header[..4] != b"QFI\xfb" {
        return Ok(None);
    }
    let offset = u64::from_be_bytes(header[8..16].try_into().unwrap());
    let len = u32::from_be_bytes(header[16..20].try_into().unwrap()) as usize;
    if offset == 0 || len == 0 || len > 4096 {
        return Ok(None);
    }
    let mut name = vec![0u8; len];
    f.seek(SeekFrom::Start(offset))?;
    f.read_exact(&mut name)?;
    Ok(Some(String::from_utf8_lossy(&name).into_owned()))
}

/// A relative backing file resolves against the image's own directory,
/// which for the copy is somewhere else: point the copy at the original
/// backing file by its absolute path, header only (`rebase -u`).
fn absolute_backing(original: &Path, copy: &Path) -> Result<(), String> {
    let backing = match qcow2_backing(copy) {
        Ok(Some(b)) => b,
        Ok(None) => return Ok(()),
        Err(e) => return Err(format!("{}: {e}", copy.display())),
    };
    if Path::new(&backing).is_absolute() {
        return Ok(());
    }
    let base = original.parent().unwrap_or(Path::new(".")).join(&backing);
    let base = crate::paths::canonical(&base).unwrap_or(base);
    let bin = player::qemu_img_binary();
    // The backing file's format rides along when the original says what
    // it is; `rebase` without one would have to probe it.
    let format = crate::console::command(&bin)
        .args(["info", "--output=json"])
        .arg(original)
        .output()
        .ok()
        .and_then(|out| serde_json::from_slice::<serde_json::Value>(&out.stdout).ok())
        .and_then(|info| info["backing-filename-format"].as_str().map(str::to_string));
    let mut cmd = crate::console::command(&bin);
    cmd.args(["rebase", "-u", "-b"]).arg(&base);
    if let Some(format) = &format {
        cmd.args(["-F", format]);
    }
    let out = cmd.arg(copy).output().map_err(|e| format!("running {}: {e}", bin.display()))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(format!("qemu-img rebase: {}", String::from_utf8_lossy(&out.stderr).trim()))
    }
}

/// Whether something is listening on the bundle's monitor socket, which
/// means a player is up, whoever started it.
fn monitor_listening(dir: &Path) -> bool {
    crate::control::listening(&crate::control::socket_path(dir))
}

fn size_label(bytes: u64) -> String {
    let mb = bytes as f64 / (1024.0 * 1024.0);
    if mb >= 1024.0 {
        format!("{:.1} GB", mb / 1024.0)
    } else {
        format!("{mb:.1} MB")
    }
}
