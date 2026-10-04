//! Leases (MS-SMB2 3.3.5.9.8 and 3.3.4.7): what lets a client cache a
//! file. Without one, Windows reads every few kilobytes over the wire,
//! however often it read the same bytes before.
//!
//! Only read caching is granted. Revoking it ("breaking" it) needs no
//! answer from the client, so the server never has to hold a conflicting
//! request while a client flushes, which write and handle caching would
//! need. A read lease is broken when:
//!
//! - another of the client's opens of the file (another lease key, or
//!   none) asks to write, or writes, resizes, renames or deletes it;
//! - the file changes under the share on the host, which the
//!   connection's watcher sees as a new size or modification time.

use crate::wire::{u16_at, u32_at, W};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

pub const READ_CACHING: u32 = 0x01;
const PARENT_LEASE_KEY_SET: u32 = 0x04;

/// What a client asked for in its `RqLs` create context.
pub struct Request {
    pub key: [u8; 16],
    pub state: u32,
    /// The v2 context (SMB 3.x): it carries an epoch and a parent key.
    pub v2: bool,
    pub flags: u32,
    pub parent: [u8; 16],
}

/// The `RqLs` context of a CREATE request's contexts, if there is one.
pub fn parse(contexts: &[u8]) -> Option<Request> {
    let mut at = 0usize;
    loop {
        let c = contexts.get(at..)?;
        let next = u32_at(c, 0) as usize;
        let name_off = u16_at(c, 4) as usize;
        let name_len = u16_at(c, 6) as usize;
        let data_off = u16_at(c, 10) as usize;
        let data_len = u32_at(c, 12) as usize;
        if c.get(name_off..name_off + name_len) == Some(b"RqLs") {
            let d = c.get(data_off..data_off + data_len)?;
            if d.len() < 32 {
                return None;
            }
            let mut key = [0u8; 16];
            key.copy_from_slice(&d[..16]);
            let mut parent = [0u8; 16];
            let v2 = d.len() >= 52;
            if v2 {
                parent.copy_from_slice(&d[32..48]);
            }
            return Some(Request { key, state: u32_at(d, 16), v2, flags: u32_at(d, 20), parent });
        }
        if next == 0 {
            return None;
        }
        at += next;
    }
}

/// The CREATE response's `RqLs` context for a granted lease.
pub fn response(req: &Request, state: u32, epoch: u16) -> Vec<u8> {
    let mut d = W::new();
    d.bytes(&req.key).u32(state).u32(req.flags & PARENT_LEASE_KEY_SET).u64(0);
    if req.v2 {
        d.bytes(&req.parent).u16(epoch).u16(0);
    }
    let mut w = W::new();
    w.u32(0).u16(16).u16(4).u16(0).u16(24).u32(d.len() as u32);
    w.bytes(b"RqLs").u32(0).bytes(&d.0);
    w.0
}

/// A file's identity across renames: its inode where there is one.
pub fn file_id(path: &Path, meta: &std::fs::Metadata) -> u64 {
    #[cfg(unix)]
    {
        let _ = path;
        std::os::unix::fs::MetadataExt::ino(meta)
    }
    #[cfg(not(unix))]
    {
        let _ = meta;
        use std::hash::{Hash, Hasher};
        let mut h = std::collections::hash_map::DefaultHasher::new();
        path.to_string_lossy().to_lowercase().hash(&mut h);
        h.finish()
    }
}

fn seen(path: &Path) -> Option<(u64, SystemTime)> {
    let m = std::fs::metadata(path).ok()?;
    Some((m.len(), m.modified().ok()?))
}

struct Lease {
    file: u64,
    path: PathBuf,
    state: u32,
    epoch: u16,
    v2: bool,
    opens: u32,
    /// The file's size and time as this server last left it.
    seen: Option<(u64, SystemTime)>,
}

/// A lease to revoke: sent as a lease break notification.
pub struct Break {
    pub key: [u8; 16],
    pub from: u32,
    pub epoch: u16,
    pub v2: bool,
}

#[derive(Default)]
pub struct Leases {
    map: HashMap<[u8; 16], Lease>,
}

impl Leases {
    /// A CREATE that asked for a lease: the state granted (read caching
    /// or nothing) and the lease's epoch.
    pub fn grant(&mut self, req: &Request, file: u64, path: &Path) -> (u32, u16) {
        let want = req.state & READ_CACHING;
        let l = self.map.entry(req.key).or_insert_with(|| Lease {
            file,
            path: path.to_path_buf(),
            state: 0,
            epoch: 0,
            v2: req.v2,
            opens: 0,
            seen: None,
        });
        l.opens += 1;
        l.path = path.to_path_buf();
        if l.state != want && want != 0 {
            l.state = want;
            l.epoch = l.epoch.wrapping_add(1);
        }
        l.seen = seen(path);
        (l.state, l.epoch)
    }

    /// An open with this lease key closed.
    pub fn release(&mut self, key: &[u8; 16]) {
        if let Some(l) = self.map.get_mut(key) {
            l.opens = l.opens.saturating_sub(1);
            if l.opens == 0 {
                self.map.remove(key);
            }
        }
    }

    /// Someone other than `except` writes `file` (or means to): every
    /// other lease on it goes. The writer's own lease keeps its state,
    /// with the file's new size and time as the ones it knows.
    pub fn conflict(&mut self, file: u64, except: Option<&[u8; 16]>) -> Vec<Break> {
        let mut out = Vec::new();
        for (key, l) in self.map.iter_mut() {
            if l.file != file {
                continue;
            }
            if Some(key) == except {
                l.seen = seen(&l.path);
                continue;
            }
            if l.state != 0 {
                out.push(Break { key: *key, from: l.state, epoch: l.epoch.wrapping_add(1), v2: l.v2 });
                l.state = 0;
                l.epoch = l.epoch.wrapping_add(1);
            }
        }
        out
    }

    /// The watcher's question, once a second: which leased files changed
    /// on the host since this server last left them.
    pub fn host_changes(&mut self) -> Vec<Break> {
        let mut out = Vec::new();
        for (key, l) in self.map.iter_mut() {
            if l.state != 0 && seen(&l.path) != l.seen {
                out.push(Break { key: *key, from: l.state, epoch: l.epoch.wrapping_add(1), v2: l.v2 });
                l.state = 0;
                l.epoch = l.epoch.wrapping_add(1);
            }
        }
        out
    }
}

/// A lease break notification: unsolicited (MessageId all ones, no
/// session), and to no state, so no acknowledgement is required.
pub fn break_message(b: &Break) -> Vec<u8> {
    let mut w = W::new();
    w.bytes(b"\xfeSMB").u16(64).u16(0).u32(0).u16(0x12).u16(0);
    w.u32(0x01).u32(0).u64(u64::MAX).u32(0).u32(0).u64(0).zeros(16);
    w.u16(44).u16(if b.v2 { b.epoch } else { 0 }).u32(0).bytes(&b.key);
    w.u32(b.from).u32(0).u32(0).u32(0).u32(0);
    w.0
}
