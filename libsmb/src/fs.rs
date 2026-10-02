//! A share's folder on the host: names resolved under its root and never
//! above it, Windows' case-insensitive lookup on case-sensitive hosts,
//! and the FILE_* information an SMB client asks of a file.

use crate::status::*;
use crate::wire::{filetime, utf16, W};
use std::fs::{self, Metadata};
use std::path::{Component, Path, PathBuf};

pub const ATTR_READONLY: u32 = 0x01;
pub const ATTR_HIDDEN: u32 = 0x02;
pub const ATTR_DIRECTORY: u32 = 0x10;
pub const ATTR_ARCHIVE: u32 = 0x20;

/// Characters no Windows name may hold, and which a client therefore
/// never means as part of one (a `:` is a stream, unsupported here).
fn bad_char(c: char) -> bool {
    matches!(c, '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|') || (c as u32) < 0x20
}

/// A client's share-relative name (`dir\file.txt`, maybe with a leading
/// backslash) split into components, or OBJECT_NAME_INVALID.
pub fn components(name: &str) -> Result<Vec<String>, u32> {
    let mut out = Vec::new();
    for part in name.split('\\') {
        if part.is_empty() || part == "." {
            continue;
        }
        if part == ".." || part.chars().any(bad_char) {
            return Err(STATUS_OBJECT_NAME_INVALID);
        }
        out.push(part.to_string());
    }
    Ok(out)
}

/// One component under `dir`: the exact name if it exists, else the one
/// entry matching it case-insensitively, else the name as given (for a
/// create). Two case-insensitive matches and no exact one is ambiguous.
fn lookup(dir: &Path, name: &str) -> Result<PathBuf, u32> {
    let exact = dir.join(name);
    if fs::symlink_metadata(&exact).is_ok() {
        return Ok(exact);
    }
    let want = name.to_lowercase();
    let mut found = None;
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            if e.file_name().to_string_lossy().to_lowercase() == want {
                if found.is_some() {
                    return Err(STATUS_OBJECT_NAME_COLLISION);
                }
                found = Some(e.path());
            }
        }
    }
    Ok(found.unwrap_or(exact))
}

/// The host path a share-relative name means. Everything but the last
/// component must be an existing directory (else OBJECT_PATH_NOT_FOUND),
/// and the result, followed through any symlink, must stay under the root.
pub fn resolve(root: &Path, name: &str) -> Result<PathBuf, u32> {
    let parts = components(name)?;
    let mut p = root.to_path_buf();
    for (i, part) in parts.iter().enumerate() {
        p = lookup(&p, part)?;
        if i + 1 < parts.len() && !p.is_dir() {
            return Err(STATUS_OBJECT_PATH_NOT_FOUND);
        }
    }
    confine(root, &p)?;
    Ok(p)
}

/// Refuses a path that leaves `root` through a symlink. A path that does
/// not exist yet is judged by its parent.
pub fn confine(root: &Path, p: &Path) -> Result<(), u32> {
    let root = fs::canonicalize(root).map_err(|_| STATUS_OBJECT_PATH_NOT_FOUND)?;
    let real = match fs::canonicalize(p) {
        Ok(r) => r,
        Err(_) => match p.parent().map(fs::canonicalize) {
            Some(Ok(parent)) => parent,
            _ => return Err(STATUS_OBJECT_PATH_NOT_FOUND),
        },
    };
    if real.starts_with(&root) && !real.components().any(|c| c == Component::ParentDir) {
        Ok(())
    } else {
        Err(STATUS_ACCESS_DENIED)
    }
}

/// What every info class is built from.
#[derive(Clone)]
pub struct Info {
    pub created: u64,
    pub accessed: u64,
    pub written: u64,
    pub changed: u64,
    pub size: u64,
    pub alloc: u64,
    pub attrs: u32,
    pub is_dir: bool,
    pub id: u64,
    pub links: u32,
}

pub fn info(name: &str, m: &Metadata) -> Info {
    let written = m.modified().map(filetime).unwrap_or(0);
    let accessed = m.accessed().map(filetime).unwrap_or(written);
    let created = m.created().map(filetime).unwrap_or(written);
    #[cfg(unix)]
    let (changed, id, alloc, links) = {
        use std::os::unix::fs::MetadataExt;
        let ct = (m.ctime() as u64 + 11_644_473_600) * 10_000_000 + m.ctime_nsec() as u64 / 100;
        (ct, m.ino(), m.blocks() * 512, m.nlink() as u32)
    };
    #[cfg(not(unix))]
    let (changed, id, alloc, links) = (written, 0u64, (m.len() + 4095) & !4095, 1u32);
    let is_dir = m.is_dir();
    let mut attrs = 0;
    if is_dir {
        attrs |= ATTR_DIRECTORY;
    } else {
        attrs |= ATTR_ARCHIVE;
    }
    if m.permissions().readonly() && !is_dir {
        attrs |= ATTR_READONLY;
    }
    if name.starts_with('.') && name != "." && name != ".." {
        attrs |= ATTR_HIDDEN;
    }
    Info {
        created,
        accessed,
        written,
        changed,
        size: if is_dir { 0 } else { m.len() },
        alloc: if is_dir { 0 } else { alloc },
        attrs,
        is_dir,
        id,
        links,
    }
}

impl Info {
    fn times(&self, w: &mut W) {
        w.u64(self.created).u64(self.accessed).u64(self.written).u64(self.changed);
    }
    pub fn basic(&self, w: &mut W) {
        self.times(w);
        w.u32(self.attrs).u32(0);
    }
    pub fn standard(&self, w: &mut W, delete_pending: bool) {
        w.u64(self.alloc).u64(self.size).u32(self.links);
        w.u8(delete_pending as u8).u8(self.is_dir as u8).u16(0);
    }
    pub fn network_open(&self, w: &mut W) {
        self.times(w);
        w.u64(self.alloc).u64(self.size).u32(self.attrs).u32(0);
    }
}

/// One directory entry in a QUERY_DIRECTORY reply.
pub struct Entry {
    pub name: String,
    pub info: Info,
}

/// Directory information classes (MS-FSCC 2.4).
pub const FILE_DIRECTORY_INFORMATION: u8 = 1;
pub const FILE_FULL_DIRECTORY_INFORMATION: u8 = 2;
pub const FILE_BOTH_DIRECTORY_INFORMATION: u8 = 3;
pub const FILE_NAMES_INFORMATION: u8 = 12;
pub const FILE_ID_BOTH_DIRECTORY_INFORMATION: u8 = 0x25;
pub const FILE_ID_FULL_DIRECTORY_INFORMATION: u8 = 0x26;

/// One entry of `class`, starting with a zero NextEntryOffset the caller
/// fills in; `None` for a class this server does not produce.
pub fn dir_entry(class: u8, index: u32, e: &Entry) -> Option<Vec<u8>> {
    let name = utf16(&e.name);
    let i = &e.info;
    let mut w = W::new();
    w.u32(0).u32(index);
    if class == FILE_NAMES_INFORMATION {
        w.u32(name.len() as u32).bytes(&name);
        return Some(w.0);
    }
    i.times(&mut w);
    w.u64(i.size).u64(i.alloc).u32(i.attrs).u32(name.len() as u32);
    match class {
        FILE_DIRECTORY_INFORMATION => {}
        FILE_FULL_DIRECTORY_INFORMATION => {
            w.u32(0);
        }
        FILE_ID_FULL_DIRECTORY_INFORMATION => {
            w.u32(0).u32(0).u64(i.id);
        }
        FILE_BOTH_DIRECTORY_INFORMATION | FILE_ID_BOTH_DIRECTORY_INFORMATION => {
            // EaSize, no 8.3 name
            w.u32(0).u8(0).u8(0).zeros(24);
            if class == FILE_ID_BOTH_DIRECTORY_INFORMATION {
                w.u16(0).u64(i.id);
            }
        }
        _ => return None,
    }
    w.bytes(&name);
    Some(w.0)
}

/// `*`, `?` and DOS's `<`, `>`, `"`, matched case-insensitively.
pub fn wildcard(pattern: &str, name: &str) -> bool {
    fn m(p: &[char], n: &[char]) -> bool {
        match p.first() {
            None => n.is_empty(),
            Some('*') | Some('<') => (0..=n.len()).any(|k| m(&p[1..], &n[k..])),
            Some('?') => !n.is_empty() && m(&p[1..], &n[1..]),
            Some('>') => (!n.is_empty() && m(&p[1..], &n[1..])) || m(&p[1..], n),
            Some('"') => (n.first() == Some(&'.') && m(&p[1..], &n[1..])) || (n.is_empty() && m(&p[1..], n)),
            Some(c) => n.first() == Some(c) && m(&p[1..], &n[1..]),
        }
    }
    let p: Vec<char> = pattern.to_lowercase().chars().collect();
    let n: Vec<char> = name.to_lowercase().chars().collect();
    pattern == "*" || m(&p, &n)
}

/// The volume's size: (total, available) bytes.
pub fn space(root: &Path) -> (u64, u64) {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        let c = std::ffi::CString::new(root.as_os_str().as_bytes()).unwrap_or_default();
        let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
        if unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0 {
            let f = s.f_frsize as u64;
            return (s.f_blocks as u64 * f, s.f_bavail as u64 * f);
        }
    }
    let _ = root;
    (1 << 40, 1 << 39)
}

/// A self-relative security descriptor granting Everyone full access,
/// with the parts AdditionalInformation asked for.
pub fn security_descriptor(additional: u32, is_dir: bool) -> Vec<u8> {
    const EVERYONE: [u8; 12] = [1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0];
    let want_owner = additional & 1 != 0;
    let want_group = additional & 2 != 0;
    let want_dacl = additional & 4 != 0;
    let mut w = W::new();
    let control: u16 = 0x8000 | if want_dacl { 0x0004 } else { 0 };
    w.u8(1).u8(0).u16(control).zeros(16);
    let put = |w: &mut W, at: usize, data: &[u8]| {
        let off = w.len() as u32;
        w.bytes(data);
        w.put_u32(at, off);
    };
    if want_owner {
        put(&mut w, 4, &EVERYONE);
    }
    if want_group {
        put(&mut w, 8, &EVERYONE);
    }
    if want_dacl {
        let mut ace = W::new();
        let flags = if is_dir { 0x03 } else { 0 };
        ace.u8(0).u8(flags).u16(8 + EVERYONE.len() as u16).u32(0x001f_01ff).bytes(&EVERYONE);
        let mut acl = W::new();
        acl.u8(2).u8(0).u16(8 + ace.len() as u16).u16(1).u16(0).bytes(&ace.0);
        put(&mut w, 16, &acl.0);
    }
    w.0
}
