//! Snapshots (doc 07: "QEMU internal snapshots via in-proc QMP, surfaced
//! in the overlay and the launcher").
//!
//! This is the offline half. A machine that isn't running has no monitor
//! to ask, so the launcher works on the qcow2 directly with `qemu-img`,
//! the same snapshot table QEMU's own `savevm`/`loadvm` use. Listing goes
//! through `qemu-img info --output=json` rather than `snapshot -l`'s
//! column layout: the JSON is a stable interface, while the table is
//! formatted for humans and has no escaping for a tag with a space in it.
//!
//! Restoring is `qemu-img snapshot -a`, which rolls the disk back and
//! leaves the saved CPU/RAM state in the image for a later `loadvm`, the
//! same as a cold boot into a snapshot. Reverting a running machine is
//! the live half (`control.rs`).
//!
//! **The tree is the launcher's own record.** A qcow2 snapshot has an
//! id, a name, a date and a size and nothing about where it came from:
//! restoring one and taking another leaves two snapshots that read as
//! consecutive when they are siblings. So the window writes what it did
//! to `snapshots.toml` beside the bundle (`Lineage`): each snapshot it
//! took with the snapshot the disk was descended from at the time, and
//! which snapshot the disk's present state descends from now, which is
//! where the next one goes. A snapshot with no record — taken by hand
//! with `qemu-img`, or before the launcher kept the file — is a root,
//! and the window says so rather than guess. The file is reconciled
//! against the disk on every read: a record whose snapshot is gone is
//! dropped and its children move up to its parent, which is also what
//! deleting a snapshot in the middle of a branch does to the branch.
//!
//! **Nothing here knows about a toolkit.** The window's model is
//! `snaps.rs`, kept separate so `control.rs` imports nothing that pulls
//! in a toolkit.

use crate::player;
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone)]
pub struct Snapshot {
    pub id: String,
    pub name: String,
    /// Size of the saved CPU/RAM state, 0 for a disk-only snapshot
    /// (`qemu-img snapshot -c` makes those; `savevm` makes the other kind).
    pub vm_state_size: u64,
    /// Unix seconds when it was taken, as qcow2 records it.
    pub date_sec: u64,
    /// How far down the tree it sits: 0 for a root. Set by `arrange`;
    /// a list straight from the disk is flat.
    pub depth: usize,
    /// Whether the disk's present state descends from this one — where
    /// the next snapshot taken would go.
    pub current: bool,
    /// Whether the launcher has a record of its parent. Without one it
    /// is shown as a root, which is not the same as being one.
    pub recorded: bool,
}

/// The file beside the bundle that holds the tree.
pub const LINEAGE_FILE: &str = "snapshots.toml";

/// One snapshot the launcher took, keyed by everything the disk says
/// about it: an id is reused once its snapshot is deleted, and a name
/// may be taken twice, so a record matches a snapshot only when all
/// three agree.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Record {
    pub id: String,
    pub name: String,
    pub date_sec: u64,
    /// The `id` of the snapshot it was taken from; a root has none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent: Option<String>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Lineage {
    /// The `id` of the snapshot the disk's present state descends from:
    /// the last one taken or restored. None after a fresh disk, or when
    /// the launcher cannot tell.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub current: Option<String>,
    #[serde(default, rename = "snapshot")]
    pub records: Vec<Record>,
}

impl Lineage {
    /// The bundle directory's record, or an empty one when there is no
    /// file. A file that cannot be parsed is an empty record too, with
    /// the error returned beside it: the tree degrades to a flat list
    /// rather than the window refusing to open.
    pub fn load(bundle_dir: &Path) -> (Lineage, Option<String>) {
        let path = bundle_dir.join(LINEAGE_FILE);
        match std::fs::read_to_string(&path) {
            Ok(text) => match toml::from_str::<Lineage>(&text) {
                Ok(l) => (l, None),
                Err(e) => (Lineage::default(), Some(format!("{}: {e}", path.display()))),
            },
            Err(_) => (Lineage::default(), None),
        }
    }

    pub fn save(&self, bundle_dir: &Path) -> std::io::Result<()> {
        let path = bundle_dir.join(LINEAGE_FILE);
        if self.records.is_empty() && self.current.is_none() {
            // Nothing to say: leave no file rather than an empty one.
            return match std::fs::remove_file(&path) {
                Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
                _ => Ok(()),
            };
        }
        let text = toml::to_string_pretty(self).map_err(std::io::Error::other)?;
        let text = format!(
            "# The snapshot tree the launcher keeps for this machine (doc 07):\n\
             # which snapshot each one was taken from, and which one the disk's\n\
             # present state descends from. The disk itself records no parent.\n\n{text}"
        );
        std::fs::write(path, text)
    }

    fn find(&self, id: &str) -> Option<&Record> {
        self.records.iter().find(|r| r.id == id)
    }

    /// The record matching a snapshot on the disk, if the launcher took it.
    fn record_of(&self, snap: &Snapshot) -> Option<&Record> {
        self.records.iter().find(|r| r.id == snap.id && r.name == snap.name && r.date_sec == snap.date_sec)
    }

    /// Drop every record whose snapshot is no longer on the disk (or is
    /// another snapshot under the same id), moving its children up to
    /// its parent, and `current` with them. Returns whether anything
    /// changed, so a caller knows to save.
    pub fn reconcile(&mut self, disk: &[Snapshot]) -> bool {
        let alive = |r: &Record| disk.iter().any(|s| s.id == r.id && s.name == r.name && s.date_sec == r.date_sec);
        let dead: Vec<Record> = self.records.iter().filter(|r| !alive(r)).cloned().collect();
        let mut changed = !dead.is_empty();
        for gone in &dead {
            let up = gone.parent.clone();
            for r in &mut self.records {
                if r.parent.as_deref() == Some(gone.id.as_str()) {
                    r.parent = up.clone();
                }
            }
            if self.current.as_deref() == Some(gone.id.as_str()) {
                self.current = up;
            }
            self.records.retain(|r| r.id != gone.id);
        }
        // A parent that never had a record at all (a file edited by
        // hand) is no parent.
        let ids: Vec<String> = self.records.iter().map(|r| r.id.clone()).collect();
        for r in &mut self.records {
            if r.parent.as_ref().is_some_and(|p| !ids.contains(p)) {
                r.parent = None;
                changed = true;
            }
        }
        if self.current.as_ref().is_some_and(|c| !ids.contains(c)) {
            self.current = None;
            changed = true;
        }
        changed
    }

    /// The snapshot `qemu-img snapshot -a <name>` / `snapshot-load`
    /// would act on: by id first, then the first of that name, which is
    /// QEMU's own lookup order.
    pub fn target<'a>(disk: &'a [Snapshot], name: &str) -> Option<&'a Snapshot> {
        disk.iter().find(|s| s.id == name).or_else(|| disk.iter().find(|s| s.name == name))
    }

    /// The snapshot a take of `name` just made: the newest of that
    /// name, since a name can be taken twice.
    pub fn newest<'a>(disk: &'a [Snapshot], name: &str) -> Option<&'a Snapshot> {
        disk.iter().rev().find(|s| s.name == name)
    }

    /// A snapshot was taken: it descends from `current`, and the disk
    /// now descends from it. `snap` is the snapshot as the disk reports
    /// it after the fact, since the id and the date are the disk's.
    pub fn took(&mut self, snap: &Snapshot) {
        self.records.retain(|r| r.id != snap.id);
        self.records.push(Record {
            id: snap.id.clone(),
            name: snap.name.clone(),
            date_sec: snap.date_sec,
            parent: self.current.clone(),
        });
        self.current = Some(snap.id.clone());
    }

    /// A snapshot was restored: the disk now descends from it. One the
    /// launcher has no record of gets one now, as a root, so that the
    /// snapshots taken from here on hang under it.
    pub fn restored(&mut self, snap: &Snapshot) {
        if self.record_of(snap).is_none() {
            self.records.retain(|r| r.id != snap.id);
            self.records.push(Record {
                id: snap.id.clone(),
                name: snap.name.clone(),
                date_sec: snap.date_sec,
                parent: None,
            });
        }
        self.current = Some(snap.id.clone());
    }
}

/// The disk's snapshots as a tree, flattened for a list: every root and
/// its descendants before the next root, children under their parent
/// in the order they were taken, each row carrying its `depth`. Roots
/// keep the disk's own order, so a snapshot with no record sits where
/// the flat list had it. A record's parent that reconciliation missed
/// (a cycle from a hand-edited file) leaves its snapshots as roots
/// rather than dropping them.
pub fn arrange(disk: Vec<Snapshot>, lineage: &Lineage) -> Vec<Snapshot> {
    let n = disk.len();
    let parent_of: Vec<Option<usize>> = disk
        .iter()
        .map(|s| {
            let rec = lineage.record_of(s)?;
            let pid = rec.parent.as_deref()?;
            let prec = lineage.find(pid)?;
            disk.iter().position(|p| p.id == prec.id && p.name == prec.name && p.date_sec == prec.date_sec)
        })
        .collect();
    let mut out = Vec::with_capacity(n);
    let mut placed = vec![false; n];
    fn visit(i: usize, depth: usize, parent_of: &[Option<usize>], placed: &mut [bool], out: &mut Vec<(usize, usize)>) {
        if placed[i] {
            return;
        }
        placed[i] = true;
        out.push((i, depth));
        for (c, p) in parent_of.iter().enumerate() {
            if *p == Some(i) {
                visit(c, depth + 1, parent_of, placed, out);
            }
        }
    }
    let mut order = Vec::with_capacity(n);
    for i in 0..n {
        if parent_of[i].is_none() {
            visit(i, 0, &parent_of, &mut placed, &mut order);
        }
    }
    // Whatever a cycle kept out: roots, at the end.
    for i in 0..n {
        if !placed[i] {
            visit(i, 0, &parent_of, &mut placed, &mut order);
        }
    }
    let current = lineage.current.as_deref().and_then(|id| lineage.find(id)).cloned();
    let mut rows: Vec<Option<Snapshot>> = disk.into_iter().map(Some).collect();
    for (i, depth) in order {
        let mut s = rows[i].take().expect("each index placed once");
        s.depth = depth;
        s.recorded = lineage.record_of(&s).is_some();
        s.current = current.as_ref().is_some_and(|c| c.id == s.id && c.name == s.name && c.date_sec == s.date_sec);
        out.push(s);
    }
    out
}

impl Snapshot {
    /// `2026-09-05 14:03 UTC`: a label in a list, never a parsed value.
    pub fn date_label(&self) -> String {
        let secs = self.date_sec as i64;
        // No chrono/time dependency for one label: civil-from-days
        // (Howard Hinnant's algorithm), in UTC. A label a few hours off
        // local time is not worth a timezone database.
        let days = secs.div_euclid(86_400);
        let rem = secs.rem_euclid(86_400);
        let z = days + 719_468;
        let era = z.div_euclid(146_097);
        let doe = z.rem_euclid(146_097);
        let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        let y = yoe + era * 400;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let d = doy - (153 * mp + 2) / 5 + 1;
        let m = if mp < 10 { mp + 3 } else { mp - 9 };
        let y = if m <= 2 { y + 1 } else { y };
        format!("{y:04}-{m:02}-{d:02} {:02}:{:02} UTC", rem / 3600, (rem % 3600) / 60)
    }

    /// "12.4 MB" / "—" for a disk-only snapshot.
    pub fn size_label(&self) -> String {
        if self.vm_state_size == 0 {
            return "—".into();
        }
        let mb = self.vm_state_size as f64 / (1024.0 * 1024.0);
        if mb >= 1024.0 {
            format!("{:.1} GB", mb / 1024.0)
        } else {
            format!("{mb:.1} MB")
        }
    }
}

fn qemu_img(args: &[&str], disk: &Path) -> std::io::Result<std::process::Output> {
    let bin = player::qemu_img_binary();
    crate::console::command(&bin)
        .args(args)
        .arg(disk)
        .output()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))
}

/// Fail with `qemu-img`'s own stderr rather than a bare exit code. Its
/// messages ("Could not find snapshot 'x'", "Permission denied") are
/// what the window should show.
pub(crate) fn check(what: &str, out: &std::process::Output) -> std::io::Result<()> {
    if out.status.success() {
        return Ok(());
    }
    let err = String::from_utf8_lossy(&out.stderr);
    let err = err.trim();
    Err(std::io::Error::other(if err.is_empty() {
        format!("qemu-img {what}: exited with {}", out.status)
    } else {
        format!("qemu-img {what}: {err}")
    }))
}

/// Every internal snapshot in `disk`, newest last (qcow2 order). An image
/// with no snapshot table has none, which is not an error.
pub fn list(disk: &Path) -> std::io::Result<Vec<Snapshot>> {
    let out = qemu_img(&["info", "--output=json"], disk)?;
    check("info", &out)?;
    let info: serde_json::Value = serde_json::from_slice(&out.stdout).map_err(std::io::Error::other)?;
    Ok(parse(info.get("snapshots")))
}

/// The `snapshots` array of an `ImageInfo`, whichever way it arrived:
/// `qemu-img info --output=json` (offline) and QMP
/// `query-named-block-nodes` (live) return the same shape, so the window
/// shows one kind of row either way.
pub fn parse(list: Option<&serde_json::Value>) -> Vec<Snapshot> {
    let Some(list) = list.and_then(|s| s.as_array()) else {
        return Vec::new();
    };
    list.iter()
        .map(|s| Snapshot {
            id: s["id"].as_str().unwrap_or_default().to_string(),
            name: s["name"].as_str().unwrap_or_default().to_string(),
            vm_state_size: s["vm-state-size"].as_u64().unwrap_or(0),
            date_sec: s["date-sec"].as_u64().unwrap_or(0),
            depth: 0,
            current: false,
            recorded: false,
        })
        .collect()
}

pub fn create(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -c", &qemu_img(&["snapshot", "-c", name], disk)?)
}

pub fn delete(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -d", &qemu_img(&["snapshot", "-d", name], disk)?)
}

pub fn restore(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -a", &qemu_img(&["snapshot", "-a", name], disk)?)
}
