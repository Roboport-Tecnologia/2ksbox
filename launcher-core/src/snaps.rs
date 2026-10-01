//! The per-machine snapshot window's model (doc 07, "Snapshots"): which
//! source the list comes from, the in-flight job, and every operation
//! its buttons run.
//!
//! The rules, which are the reason this is one implementation:
//!
//! * A running machine is driven through its monitor (`control.rs`),
//!   because `qemu-img` writing to an image QEMU has open corrupts it,
//!   and even listing wants an image lock QEMU already holds.
//! * A stopped one goes through `qemu-img` (`snapshots.rs`).
//! * Starting or stopping under the window re-reads the list from the
//!   other source.
//! * Live save, load and delete are jobs, not synchronous commands
//!   (saving a 512 MB guest's RAM takes a visible moment), so the window
//!   polls rather than blocking the UI thread on QEMU's main loop.
//! * A load runs on a stopped VM and resumes afterwards only if the
//!   guest was running, so a machine the user had paused stays paused.
//! * `reload` never clears `error`. A failed operation reports and then
//!   re-reads, and a successful re-read must not wipe the report; when
//!   it did, a failed live restore looked like it had worked.
//! * The rows are a tree (`snapshots::Lineage`, the file beside the
//!   bundle): a take records the snapshot the disk descended from, a
//!   restore moves that pointer, and every read reconciles the file
//!   against the disk. A snapshot with no record sits at the top level.
//! * A Windows 11 machine's snapshot is more than its disk (track M20):
//!   the firmware's variable store, a qcow2 that takes the same snapshot
//!   by name, and the TPM's state file, copied into `tpm-snapshots/`
//!   (`tpm_copy`) after every take and put back by an offline restore.
//!   A live load needs no copy: the TPM's state rides in the snapshot's
//!   vmstate and the backend writes the file back itself (patch 75). A
//!   snapshot taken before any of this has neither, and restores the
//!   disk alone, as it always did.
//!
//! The front end still owns when `poll` is called (Qt runs a `Timer`
//! that stops when there is no job) and how a destructive restore is
//! confirmed.

use crate::bundle::Machine;
use crate::control::{self, Control};
use crate::snapshots::{arrange, create, delete, list, restore, Lineage, Snapshot};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// How often an in-flight job is actually asked about, however often
/// `poll` is called. A frame-driven caller would otherwise ask sixty
/// times a second.
const POLL_INTERVAL: Duration = Duration::from_millis(400);

#[derive(Default)]
pub struct Snapshots {
    /// Whether the window is up.
    pub open: bool,
    /// The bundle directory, so the caller can say whether this machine
    /// is the running one.
    bundle_dir: Option<PathBuf>,
    machine_name: String,
    disk: PathBuf,
    /// A modern machine's firmware variables and TPM state (`None` on
    /// the era's machines), which its snapshots carry too.
    efi_vars: Option<PathBuf>,
    tpm_state: Option<PathBuf>,
    /// The rows, in tree order (`snapshots::arrange`).
    list: Vec<Snapshot>,
    /// The tree's record, loaded with the window and saved on every
    /// change.
    lineage: Lineage,
    error: Option<String>,
    status: Option<String>,
    /// Whether this machine's player is up. Everything branches on it.
    running: bool,
    /// A `snapshot-save`/`-load`/`-delete` job in flight, and whether the
    /// guest has to be resumed once it finishes (a load runs on a
    /// stopped VM).
    job: Option<String>,
    /// What the job is doing (its QMP verb and tag), for the lineage
    /// once it has concluded.
    job_op: Option<(String, String)>,
    resume_after_job: bool,
    next_job: u64,
    last_poll: Option<Instant>,
}

impl Snapshots {
    pub fn open_for(&mut self, machine: &Machine, bundle_dir: PathBuf, running: bool) {
        let (lineage, lineage_err) = Lineage::load(&bundle_dir);
        *self = Snapshots {
            open: true,
            bundle_dir: Some(bundle_dir),
            machine_name: machine.name.clone(),
            disk: machine.disk.clone(),
            efi_vars: machine.family.is_modern().then(|| machine.effective_efi_vars()),
            tpm_state: machine.family.is_modern().then(|| machine.effective_tpm_state()),
            lineage,
            running,
            ..Default::default()
        };
        self.refresh();
        // A tree file that cannot be read is worth a line: the rows are
        // there, flat, and the next take starts a new record.
        if let Some(e) = lineage_err {
            self.error = Some(e);
        }
    }

    /// The same, from a bundle path, for a front end that addresses its
    /// windows by path rather than by a `Machine` it already holds.
    pub fn open_for_path(&mut self, bundle_path: &Path, running: bool) {
        let dir = bundle_path.parent().unwrap_or(Path::new(".")).to_path_buf();
        match Machine::load(bundle_path) {
            Ok(machine) => self.open_for(&machine, dir, running),
            Err(e) => {
                *self = Snapshots { open: true, bundle_dir: Some(dir), running, ..Default::default() };
                self.error = Some(format!("{}: {e}", bundle_path.display()));
            }
        }
    }

    pub fn title(&self) -> String {
        if self.machine_name.is_empty() {
            "Snapshots".to_string()
        } else {
            format!("Snapshots — {}", self.machine_name)
        }
    }

    pub fn bundle_dir(&self) -> Option<&Path> {
        self.bundle_dir.as_deref()
    }

    /// The rows, in tree order: each root followed by its descendants,
    /// with `depth` saying how far in to draw a row.
    pub fn snapshots(&self) -> &[Snapshot] {
        &self.list
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    pub fn running(&self) -> bool {
        self.running
    }

    /// Whether a live job is in flight: every button is disabled while
    /// one is (a second job on top of it is refused by QEMU anyway) and
    /// the poll timer runs.
    pub fn job_pending(&self) -> bool {
        self.job.is_some()
    }

    /// The machine started or stopped under the window: the same list
    /// now has to come from the other source.
    pub fn set_running(&mut self, running: bool) {
        if running == self.running {
            return;
        }
        self.running = running;
        self.job = None;
        self.job_op = None;
        self.refresh();
    }

    /// Connect to the running machine's monitor. A fresh connection per
    /// operation rather than one held across frames: jobs and block
    /// nodes are QEMU-global, not per-monitor, so nothing is lost, and
    /// there is no half-open socket to handle when a guest shuts down.
    fn control(&self) -> Result<Control, String> {
        let dir = self.bundle_dir.as_deref().ok_or("no machine open")?;
        Control::connect(&control::socket_path(dir))
    }

    /// Re-read the list from whichever source applies. Never touches
    /// `error`; this module's header says why.
    fn reload(&mut self) -> Result<(), String> {
        let result = if self.running {
            self.control().and_then(|mut c| c.disk_node(&self.disk)).map(|(_, snapshots)| snapshots)
        } else {
            list(&self.disk).map_err(|e| e.to_string())
        };
        match result {
            Ok(list) => {
                // The file follows the disk, never the other way round:
                // a snapshot deleted by hand takes its record with it.
                let changed = self.lineage.reconcile(&list);
                self.list = arrange(list, &self.lineage);
                if changed {
                    self.save_lineage()?;
                }
                Ok(())
            }
            Err(e) => {
                self.list.clear();
                Err(e)
            }
        }
    }

    fn save_lineage(&self) -> Result<(), String> {
        let dir = self.bundle_dir.as_deref().ok_or("no machine open")?;
        self.lineage.save(dir).map_err(|e| format!("saving the snapshot tree: {e}"))
    }

    /// Fold what an operation did into the tree, once the list has been
    /// re-read (the disk assigns the id and the date). `verb` is the QMP
    /// job's or, offline, the same word for the same operation.
    fn record(&mut self, verb: &str, name: &str) -> Result<(), String> {
        let snap = match verb {
            "snapshot-save" => Lineage::newest(&self.list, name),
            "snapshot-load" => Lineage::target(&self.list, name),
            // A delete is the reconciliation's: the record goes with
            // the snapshot and its children move up.
            _ => None,
        };
        let Some(snap) = snap.cloned() else { return Ok(()) };
        match verb {
            "snapshot-save" => self.lineage.took(&snap),
            _ => self.lineage.restored(&snap),
        }
        self.list = arrange(std::mem::take(&mut self.list), &self.lineage);
        self.save_lineage()
    }

    /// Re-read and make the result the window's current message, for
    /// opening the window or when the machine started or stopped.
    fn refresh(&mut self) {
        self.error = self.reload().err();
    }

    /// Run one offline operation and fold its result into the state,
    /// the tree included.
    fn run(&mut self, verb: &str, name: &str, what: &str, result: Result<(), String>) {
        match result {
            Ok(()) => {
                self.status = Some(what.to_string());
                self.error = self.reload().and_then(|()| self.record(verb, name)).err();
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    pub fn take(&mut self, name: &str) {
        let name = name.trim();
        if name.is_empty() {
            return;
        }
        if self.running {
            return self.start_job("snapshot-save", name, false);
        }
        let r = create(&self.disk, name)
            .map_err(|e| e.to_string())
            .and_then(|()| self.efi_vars_op(name, Op::Take))
            .and_then(|()| self.tpm_op(name, Op::Take));
        self.run("snapshot-save", name, &format!("took \u{201c}{name}\u{201d}"), r);
    }

    pub fn drop_snapshot(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-delete", name, false);
        }
        let r = delete(&self.disk, name)
            .map_err(|e| e.to_string())
            .and_then(|()| self.efi_vars_op(name, Op::Delete))
            .and_then(|()| self.tpm_op(name, Op::Delete));
        self.run("snapshot-delete", name, &format!("deleted \u{201c}{name}\u{201d}"), r);
    }

    pub fn revert(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-load", name, true);
        }
        let r = restore(&self.disk, name)
            .map_err(|e| e.to_string())
            .and_then(|()| self.efi_vars_op(name, Op::Restore))
            .and_then(|()| self.tpm_op(name, Op::Restore));
        self.run("snapshot-load", name, &format!("restored \u{201c}{name}\u{201d}"), r);
    }

    /// The offline side of a modern machine's snapshot on its variable
    /// store. A restore or delete of a snapshot the store does not have
    /// (one taken before it was covered) leaves the store alone.
    fn efi_vars_op(&self, name: &str, op: Op) -> Result<(), String> {
        let Some(vars) = &self.efi_vars else { return Ok(()) };
        if !vars.exists() {
            return Ok(());
        }
        let has = || list(vars).map(|l| l.iter().any(|s| s.name == name)).unwrap_or(false);
        let r = match op {
            Op::Take => create(vars, name),
            Op::Restore if has() => restore(vars, name),
            Op::Delete if has() => delete(vars, name),
            _ => Ok(()),
        };
        r.map_err(|e| format!("{}: {e}", vars.display()))
    }

    /// The TPM's side: its state file copied for a take, copied back for
    /// an offline restore, the copy removed with the snapshot. Nothing
    /// to copy back for a snapshot that has none.
    fn tpm_op(&self, name: &str, op: Op) -> Result<(), String> {
        let (Some(state), Some(dir)) = (&self.tpm_state, &self.bundle_dir) else { return Ok(()) };
        let copy = tpm_copy(dir, name);
        let r = match op {
            Op::Take if state.exists() => {
                std::fs::create_dir_all(copy.parent().unwrap_or(dir)).and_then(|()| std::fs::copy(state, &copy).map(|_| ()))
            }
            Op::Restore if copy.exists() => std::fs::copy(&copy, state).map(|_| ()),
            Op::Delete if copy.exists() => std::fs::remove_file(&copy),
            _ => Ok(()),
        };
        r.map_err(|e| format!("the TPM's state for \u{201c}{name}\u{201d}: {e}"))
    }

    /// Start a live snapshot job (`command` is the QMP verb) and leave
    /// the window polling it.
    fn start_job(&mut self, command: &str, tag: &str, stop_first: bool) {
        self.next_job += 1;
        let job_id = format!("launcher-{}", self.next_job);
        let disk = self.disk.clone();
        let vars = self.efi_vars.clone();
        let mut resume_after = false;
        let result = self.control().and_then(|mut c| {
            let (node, _) = c.disk_node(&disk)?;
            // A modern machine's variable store goes in the snapshot
            // too: always into a new one, and into a load or delete only
            // when it holds that snapshot (one taken before it was
            // covered does not, and QEMU refuses a device that lacks it).
            let mut also = Vec::new();
            if let Some(vars) = &vars {
                let (vars_node, snaps) = c.disk_node(vars)?;
                if command == "snapshot-save" || snaps.iter().any(|s| s.name == tag) {
                    also.push(vars_node);
                }
            }
            // A load replaces the guest's CPU/RAM state, which QEMU
            // requires the VM to be stopped for.
            if stop_first {
                resume_after = c.is_running()?;
                c.set_running(false)?;
            }
            c.start_snapshot_job(command, &job_id, tag, &node, &also)
        });
        match result {
            Ok(()) => {
                self.job = Some(job_id);
                self.job_op = Some((command.to_string(), tag.to_string()));
                self.resume_after_job = resume_after;
                self.error = None;
                self.status = Some(format!("{command} \u{201c}{tag}\u{201d}\u{2026}"));
            }
            Err(e) => {
                self.job_op = None;
                self.resume_after_job = false;
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    /// Check an in-flight job, at most a couple of times a second. A
    /// concluded job stays around until dismissed, which is where its
    /// error (if any) comes from.
    pub fn poll_job(&mut self) {
        if self.job.is_none() {
            return;
        }
        if self.last_poll.map(|t| t.elapsed() < POLL_INTERVAL).unwrap_or(false) {
            return;
        }
        self.last_poll = Some(Instant::now());
        self.poll_now();
    }

    /// Poll now, ignoring the throttle, for a caller driving a job from
    /// a loop rather than from frames or a timer (`cli`'s `--snapshots`).
    pub fn poll_job_now(&mut self) {
        self.last_poll = None;
        self.poll_job();
    }

    fn poll_now(&mut self) {
        let Some(job_id) = self.job.clone() else { return };
        let state = self.control().and_then(|mut c| c.job(&job_id));
        let (status, error) = match state {
            Ok(Some(state)) => state,
            // Gone already (or the monitor went away with the guest):
            // stop polling rather than spinning on a job that can't
            // report anything.
            Ok(None) => {
                self.job = None;
                self.refresh();
                return;
            }
            Err(e) => {
                self.job = None;
                self.error = Some(e);
                return;
            }
        };
        if status != "concluded" {
            return;
        }
        self.job = None;
        let op = self.job_op.take();
        let resume = std::mem::take(&mut self.resume_after_job);
        let result = self.control().and_then(|mut c| {
            c.dismiss_job(&job_id)?;
            if resume {
                c.set_running(true)?;
            }
            Ok(())
        });
        // The job's own error wins over anything the tidy-up hit.
        let outcome = error.map(Err).unwrap_or(result);
        let reload = self.reload();
        // The tree follows only a job that succeeded: a failed load
        // left the disk where it was.
        let recorded = match (&outcome, &op) {
            (Ok(()), Some((verb, tag))) => self.record(verb, tag).and_then(|()| match verb.as_str() {
                // The TPM's state as the snapshot left it, for an offline
                // restore later; a live load restored it from the vmstate.
                "snapshot-save" => self.tpm_op(tag, Op::Take),
                "snapshot-delete" => self.tpm_op(tag, Op::Delete),
                _ => Ok(()),
            }),
            _ => Ok(()),
        };
        match outcome.and(reload).and(recorded) {
            Ok(()) => {
                self.status = Some("done".into());
                self.error = None;
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    /// Wait out an in-flight job, for a caller with no frames and no
    /// timer (`cli`'s `--snapshots`).
    pub fn wait_for_job(&mut self, timeout: Duration) {
        let deadline = Instant::now() + timeout;
        while self.job_pending() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(200));
            self.poll_job_now();
        }
    }
}

#[derive(Clone, Copy)]
enum Op {
    Take,
    Restore,
    Delete,
}

/// Where a snapshot's copy of the TPM's state lives: `tpm-snapshots/`
/// in the bundle, the snapshot's name in hex so any name is a file name.
pub fn tpm_copy(bundle_dir: &Path, name: &str) -> PathBuf {
    let hex: String = name.bytes().map(|b| format!("{b:02x}")).collect();
    bundle_dir.join("tpm-snapshots").join(format!("{hex}.permall"))
}
