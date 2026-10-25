//! A host directory served as a disc: an ISO 9660 + Joliet volume laid
//! out over the tree at open time and read lazily afterwards
//! (`isodir:/path`, M5g, `docs/tracks/m5-dirdisc.md`).
//!
//! Nothing is written anywhere. The volume descriptors, path tables and
//! directory records are built once into one in-memory blob
//! (`Source::Mem`); every file is an extent read straight from the host
//! file (`Source::File` with `Layout::Cooked2048`), so the rest of
//! `libdisc` synthesizes sync, header and EDC/ECC for the guest exactly
//! as it does for an `.iso`. The disc is a **snapshot of the tree**: the
//! layout is fixed at open, and a file that changes underneath it is
//! reported as a read error rather than served torn (`Payload`).
//!
//! Two directory trees over one set of file extents: the primary tree in
//! ISO 9660 level 1 (8.3, uppercase) which is what MSCDEX reads under
//! DOS, and a Joliet tree with the real names, which is what Windows
//! reads. A file's bytes are laid out once and both trees point at them.

use std::cmp::Ordering;
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use crate::{Disc, Error, Extent, Layout, Result, Session, Source, Track, TrackMode};

/// ISO 9660's logical block; our sector.
const BLOCK: usize = 2048;
/// Sectors before the first volume descriptor, all zero.
const SYSTEM_AREA: u32 = 16;
/// Zero sectors after the last file, as every mastering tool writes
/// them: guest drivers read ahead past the last extent, and a volume
/// that ends exactly at its last byte answers those reads with an error.
const TAIL_PAD: u32 = 150;
/// Longest name element in the Joliet tree, `;1` included: Joliet's own
/// limit (128 bytes of UCS-2) and what Windows expects.
const JOLIET_MAX: usize = 64;
/// ISO 9660's directory hierarchy limit. Deeper still works through the
/// Joliet tree on Windows; MSCDEX may not follow, so it is a warning.
const ISO_MAX_DEPTH: usize = 8;
/// Where we stop walking: a symlink loop `canonicalize` did not catch,
/// or a tree nobody meant to share.
const MAX_DEPTH: usize = 30;
/// A directory record addresses 32 bits of bytes and multi-extent files
/// are a per-Windows-version minefield, so a file this big is refused.
const MAX_FILE: u64 = 4 << 30;
/// Big enough that Win98 might not cope; shared anyway, with a warning.
const BIG_FILE: u64 = 2 << 30;
/// The path table's parent field is 16 bits.
const MAX_DIRS: usize = 65535;
/// Sectors a disc can hold: a dual-layer DVD-9, the largest medium the
/// drive we present a folder through could physically be. Up to an
/// 80-minute CD (`CD_MAX_SECTORS` in QEMU's `atapi.c`) the disc is a
/// CD-ROM; above it the device reports a DVD-ROM profile and MSF stops
/// meaning anything, which is why the CD's own ceiling
/// (`msf::MAX_LBA`, 99:59:74) is not the limit here. Past a DVD-9 there
/// is no medium left to claim to be, so the tree is refused.
const MAX_SECTORS: u64 = 4_173_824;

/// Serve `dir` as a disc.
pub fn open(dir: &Path) -> Result<Disc> {
    let mut b = Builder::new(dir)?;
    b.walk(0, &dir.to_path_buf(), 1, &mut Vec::new())?;
    b.name_tree();
    b.finish()
}

/// One name in the shared tree. `iso_id` / `jol_id` are the identifiers
/// as they appear in the two trees, already mangled and made unique
/// among their siblings; the layout fills in the rest.
struct Node {
    name: String,
    is_dir: bool,
    len: u64,
    date: [u8; 7],
    /// Payload index; `None` for directories and empty files, which own
    /// no sectors.
    payload: Option<usize>,
    children: Vec<usize>,
    parent: usize,
    depth: usize,
    iso_id: Vec<u8>,
    jol_id: Vec<u8>,
    /// File data extent (both trees share it); 0 for an empty file.
    lba: u32,
}

struct Builder {
    root: PathBuf,
    disc: Disc,
    nodes: Vec<Node>,
    dirs: usize,
    warnings: usize,
}

/// One tree's plan: the ordering the standard wants, the numbering that
/// follows from it, and the space each directory needs.
struct Tree {
    /// Directory node indices in path-table order: by level, then by
    /// parent number, then by identifier. Numbering follows the order,
    /// which is what makes a parent's number always ≤ its children's.
    order: Vec<usize>,
    /// 1-based directory number per node (0 for files).
    num: Vec<u16>,
    /// Children per node, sorted as the standard orders records.
    kids: Vec<Vec<usize>>,
    /// Directory extent per node, assigned by `Builder::finish`.
    lba: Vec<u32>,
    /// Directory data length in bytes (a whole number of sectors).
    size: Vec<u32>,
    /// Total bytes of one path table.
    pt_size: u32,
}

impl Builder {
    fn new(dir: &Path) -> Result<Builder> {
        let md = fs::metadata(dir).map_err(|e| Error::Invalid(format!("{}: {}", dir.display(), e)))?;
        if !md.is_dir() {
            return Err(Error::Invalid(format!("{}: not a directory", dir.display())));
        }
        let root = Node {
            name: String::new(),
            is_dir: true,
            len: 0,
            date: dir_date(md.modified().ok()),
            payload: None,
            children: Vec::new(),
            parent: 0,
            depth: 1,
            iso_id: vec![0],
            jol_id: vec![0],
            lba: 0,
        };
        Ok(Builder { root: dir.to_path_buf(), disc: Disc::new(), nodes: vec![root], dirs: 1, warnings: 0 })
    }

    /// Say what we are dropping and why. A shared folder is the user's
    /// own directory, not a mastered image: it will contain things that
    /// cannot go on a disc, and silence about them is how a file goes
    /// missing without anyone noticing.
    fn warn(&mut self, msg: String) {
        self.warnings += 1;
        if self.warnings <= 20 {
            eprintln!("libdisc: isodir: {msg}");
        } else if self.warnings == 21 {
            eprintln!("libdisc: isodir: (further warnings suppressed)");
        }
    }

    /// Recursive directory walk. `ancestors` holds the canonical paths
    /// of the directories we are inside, which is what catches a symlink
    /// pointing back up the tree.
    fn walk(&mut self, node: usize, path: &PathBuf, depth: usize, ancestors: &mut Vec<PathBuf>) -> Result<()> {
        if depth > MAX_DEPTH {
            return Err(Error::Invalid(format!("{}: directory nesting deeper than {MAX_DEPTH} levels", path.display())));
        }
        let here = fs::canonicalize(path).unwrap_or_else(|_| path.clone());
        if ancestors.contains(&here) {
            return Err(Error::Invalid(format!("{}: symlink loop", path.display())));
        }
        ancestors.push(here);

        let rd = fs::read_dir(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
        let mut entries: Vec<(String, PathBuf)> = Vec::new();
        for ent in rd {
            let ent = ent.map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
            match ent.file_name().into_string() {
                Ok(name) => entries.push((name, ent.path())),
                Err(raw) => self.warn(format!("{}: name is not valid UTF-8, skipped ({:?})", path.display(), raw)),
            }
        }
        // Deterministic before any mangling, so two runs over an
        // unchanged tree agree on which of two colliding names gets ~1.
        entries.sort_by(|a, b| a.0.cmp(&b.0));

        for (name, child) in entries {
            // Follow symlinks: what the user shared is what they see.
            let md = match fs::metadata(&child) {
                Ok(md) => md,
                Err(e) => {
                    self.warn(format!("{}: {e}, skipped", child.display()));
                    continue;
                }
            };
            let date = dir_date(md.modified().ok());
            if md.is_dir() {
                if self.dirs >= MAX_DIRS {
                    return Err(Error::Invalid(format!("{}: more than {MAX_DIRS} directories", self.root.display())));
                }
                let idx = self.push(Node {
                    name,
                    is_dir: true,
                    len: 0,
                    date,
                    payload: None,
                    children: Vec::new(),
                    parent: node,
                    depth: depth + 1,
                    iso_id: Vec::new(),
                    jol_id: Vec::new(),
                    lba: 0,
                });
                self.dirs += 1;
                self.nodes[node].children.push(idx);
                self.walk(idx, &child, depth + 1, ancestors)?;
            } else if md.is_file() {
                let len = md.len();
                if len >= MAX_FILE {
                    return Err(Error::Invalid(format!(
                        "{}: {} bytes, too large for one ISO 9660 extent (limit {} GiB)",
                        child.display(),
                        len,
                        MAX_FILE >> 30
                    )));
                }
                if len >= BIG_FILE {
                    self.warn(format!("{}: {} bytes; files this large are unreliable on Win98", child.display(), len));
                }
                let payload = if len == 0 {
                    None
                } else {
                    match self.disc.add_file(&child) {
                        Ok(p) => Some(p),
                        Err(e) => {
                            self.warn(format!("{e}, skipped"));
                            continue;
                        }
                    }
                };
                let idx = self.push(Node {
                    name,
                    is_dir: false,
                    len,
                    date,
                    payload,
                    children: Vec::new(),
                    parent: node,
                    depth: depth + 1,
                    iso_id: Vec::new(),
                    jol_id: Vec::new(),
                    lba: 0,
                });
                self.nodes[node].children.push(idx);
            } else {
                self.warn(format!("{}: not a regular file or directory, skipped", child.display()));
            }
        }
        ancestors.pop();
        Ok(())
    }

    fn push(&mut self, n: Node) -> usize {
        self.nodes.push(n);
        self.nodes.len() - 1
    }

    /// Give every node its two identifiers, unique among its siblings.
    fn name_tree(&mut self) {
        let mut deep = 0usize;
        for i in 0..self.nodes.len() {
            let kids = self.nodes[i].children.clone();
            if kids.is_empty() {
                continue;
            }
            if self.nodes[i].depth > ISO_MAX_DEPTH {
                deep += 1;
            }
            let mut iso_taken = HashSet::new();
            let mut jol_taken = HashSet::new();
            for k in kids {
                let (name, is_dir) = (self.nodes[k].name.clone(), self.nodes[k].is_dir);
                let iso = unique(iso_ident(&name, is_dir), &mut iso_taken, iso_suffix);
                let jol = unique(joliet_ident(&name, is_dir), &mut jol_taken, joliet_suffix);
                self.nodes[k].iso_id = iso;
                self.nodes[k].jol_id = jol;
            }
        }
        if deep > 0 {
            self.warn(format!(
                "{deep} directories are deeper than ISO 9660's {ISO_MAX_DEPTH} levels; Windows reads them through the Joliet tree, MSCDEX may not"
            ));
        }
    }

    /// Order, number and size one tree.
    fn plan(&self, joliet: bool, pad: u8) -> Tree {
        let n = self.nodes.len();
        let id = |i: usize| -> &[u8] {
            if joliet {
                &self.nodes[i].jol_id
            } else {
                &self.nodes[i].iso_id
            }
        };
        let mut kids: Vec<Vec<usize>> = vec![Vec::new(); n];
        for (i, node) in self.nodes.iter().enumerate() {
            let mut c = node.children.clone();
            c.sort_by(|&a, &b| id_cmp(id(a), id(b), pad));
            kids[i] = c;
        }
        // Path-table order is also the numbering order: pop the queue in
        // order, append each directory's child directories in record
        // order, and a parent's number can never exceed a child's.
        let mut order = Vec::new();
        let mut num = vec![0u16; n];
        let mut queue = vec![0usize];
        let mut at = 0;
        while at < queue.len() {
            let d = queue[at];
            at += 1;
            order.push(d);
            num[d] = order.len() as u16;
            for &k in &kids[d] {
                if self.nodes[k].is_dir {
                    queue.push(k);
                }
            }
        }
        let mut size = vec![0u32; n];
        let mut pt_size = 0u32;
        for &d in &order {
            // "." and ".." first, then the children, in record order.
            let mut lens = vec![dir_rec_len(1), dir_rec_len(1)];
            lens.extend(kids[d].iter().map(|&k| dir_rec_len(id(k).len())));
            size[d] = dir_data_len(&lens);
            pt_size += path_rec_len(if d == 0 { 1 } else { id(d).len() }) as u32;
        }
        Tree { order, num, kids, lba: vec![0; n], size, pt_size }
    }

    /// Assign every extent, build the metadata blob and the disc.
    fn finish(mut self) -> Result<Disc> {
        let mut iso = self.plan(false, b' ');
        let mut jol = self.plan(true, 0);

        // Descriptors, then the four path tables, then each tree's
        // directories, then the files: everything before the first file
        // extent is metadata and goes in one blob.
        let mut lba = SYSTEM_AREA + 3;
        let iso_pt_l = lba;
        lba += sectors_for(iso.pt_size);
        let iso_pt_m = lba;
        lba += sectors_for(iso.pt_size);
        let jol_pt_l = lba;
        lba += sectors_for(jol.pt_size);
        let jol_pt_m = lba;
        lba += sectors_for(jol.pt_size);
        for &d in &iso.order {
            iso.lba[d] = lba;
            lba += iso.size[d] / BLOCK as u32;
        }
        for &d in &jol.order {
            jol.lba[d] = lba;
            lba += jol.size[d] / BLOCK as u32;
        }
        let meta_sectors = lba;

        // How big the disc will be, before anything is laid out or
        // allocated: the sector numbers below are 32 bits and would wrap
        // on a tree of terabytes, and a folder nobody meant to share
        // whole (a downloads directory, a shelf of disc dumps) is
        // the ordinary way to arrive here.
        let data: u64 = self.nodes.iter().filter(|n| n.payload.is_some()).map(|n| sectors_for_len(n.len) as u64).sum();
        let want = meta_sectors as u64 + data + TAIL_PAD as u64;
        if want > MAX_SECTORS {
            return Err(Error::Invalid(format!(
                "{}: the folder holds {}, and a disc holds at most {} (a dual-layer DVD); share a folder that fits, or make an image of this one",
                self.root.display(),
                size_str(want),
                size_str(MAX_SECTORS)
            )));
        }

        // Files, in the primary tree's order: a directory's own files
        // land next to each other, which is the order a guest reads them.
        // An empty file owns no sectors but still needs a plausible
        // extent. Readers that meet one addressed at LBA 0, inside the
        // system area, have been known to drop the entry.
        let mut files: Vec<usize> = Vec::new();
        for &d in &iso.order {
            for &k in &iso.kids[d] {
                if self.nodes[k].is_dir {
                    continue;
                }
                self.nodes[k].lba = lba;
                if self.nodes[k].payload.is_some() {
                    lba += sectors_for_len(self.nodes[k].len);
                    files.push(k);
                }
            }
        }
        let last_data = lba;
        let total = last_data + TAIL_PAD;

        let mut blob = vec![0u8; meta_sectors as usize * BLOCK];
        let vol_date = volume_date(self.nodes[0].date);
        let label = volume_label(&self.root);
        write_descriptor(
            &mut blob[SYSTEM_AREA as usize * BLOCK..],
            Desc { joliet: false, label: &label, space: total, pt_size: iso.pt_size, pt_l: iso_pt_l, pt_m: iso_pt_m, root_lba: iso.lba[0], root_len: iso.size[0], root_date: self.nodes[0].date, vol_date },
        );
        write_descriptor(
            &mut blob[(SYSTEM_AREA as usize + 1) * BLOCK..],
            Desc { joliet: true, label: &label, space: total, pt_size: jol.pt_size, pt_l: jol_pt_l, pt_m: jol_pt_m, root_lba: jol.lba[0], root_len: jol.size[0], root_date: self.nodes[0].date, vol_date },
        );
        let term = &mut blob[(SYSTEM_AREA as usize + 2) * BLOCK..];
        term[0] = 255;
        term[1..6].copy_from_slice(b"CD001");
        term[6] = 1;

        for (tree, pt_l, pt_m, joliet) in [(&iso, iso_pt_l, iso_pt_m, false), (&jol, jol_pt_l, jol_pt_m, true)] {
            for (at, big) in [(pt_l, false), (pt_m, true)] {
                let out = &mut blob[at as usize * BLOCK..][..sectors_for(tree.pt_size) as usize * BLOCK];
                self.write_path_table(out, tree, joliet, big);
            }
            for &d in &tree.order {
                let out = &mut blob[tree.lba[d] as usize * BLOCK..][..tree.size[d] as usize];
                self.write_dir(out, tree, d, joliet);
            }
        }

        // One Mode 1 track: the metadata blob, every file, the padding.
        let mut extents = Vec::with_capacity(files.len() + 2);
        let blob_id = self.disc.add_blob(blob);
        extents.push(Extent { lba: 0, count: meta_sectors, source: Source::Mem { blob: blob_id, offset: 0 }, sub: None });
        for &k in &files {
            let n = &self.nodes[k];
            extents.push(Extent {
                lba: n.lba as i32,
                count: sectors_for_len(n.len),
                source: Source::File { file: n.payload.unwrap(), offset: 0, layout: Layout::Cooked2048, swap: false, eof_pad: true },
                sub: None,
            });
        }
        extents.push(Extent { lba: last_data as i32, count: TAIL_PAD, source: Source::ZeroData, sub: None });

        self.disc.sessions.push(Session {
            number: 1,
            tracks: vec![Track {
                number: 1,
                mode: TrackMode::Mode1,
                control: 0x4,
                isrc: None,
                indices: vec![(1, 0)],
                start_lba: 0,
                end_lba: total as i32,
                extents,
            }],
            leadout_lba: total as i32,
        });
        Ok(self.disc)
    }

    fn write_path_table(&self, out: &mut [u8], tree: &Tree, joliet: bool, big: bool) {
        let mut at = 0usize;
        for &d in &tree.order {
            let id: &[u8] = if d == 0 {
                &[0]
            } else if joliet {
                &self.nodes[d].jol_id
            } else {
                &self.nodes[d].iso_id
            };
            let rec = &mut out[at..at + path_rec_len(id.len())];
            rec[0] = id.len() as u8;
            rec[1] = 0;
            let lba = tree.lba[d];
            let parent = tree.num[self.nodes[d].parent];
            if big {
                rec[2..6].copy_from_slice(&lba.to_be_bytes());
                rec[6..8].copy_from_slice(&parent.to_be_bytes());
            } else {
                rec[2..6].copy_from_slice(&lba.to_le_bytes());
                rec[6..8].copy_from_slice(&parent.to_le_bytes());
            }
            rec[8..8 + id.len()].copy_from_slice(id);
            at += rec.len();
        }
    }

    fn write_dir(&self, out: &mut [u8], tree: &Tree, d: usize, joliet: bool) {
        let parent = self.nodes[d].parent;
        let mut recs: Vec<(&[u8], u32, u32, bool, [u8; 7])> = vec![
            (&[0], tree.lba[d], tree.size[d], true, self.nodes[d].date),
            (&[1], tree.lba[parent], tree.size[parent], true, self.nodes[parent].date),
        ];
        for &k in &tree.kids[d] {
            let n = &self.nodes[k];
            let id: &[u8] = if joliet { &n.jol_id } else { &n.iso_id };
            if n.is_dir {
                recs.push((id, tree.lba[k], tree.size[k], true, n.date));
            } else {
                recs.push((id, n.lba, n.len as u32, false, n.date));
            }
        }
        let mut at = 0usize;
        for (id, lba, len, is_dir, date) in recs {
            let l = dir_rec_len(id.len());
            // A record never straddles a sector: a driver reads one
            // sector and walks it, and a split record ends the walk.
            if at % BLOCK + l > BLOCK {
                at = (at / BLOCK + 1) * BLOCK;
            }
            let rec = &mut out[at..at + l];
            rec.fill(0);
            rec[0] = l as u8;
            both32(lba, &mut rec[2..10]);
            both32(len, &mut rec[10..18]);
            rec[18..25].copy_from_slice(&date);
            rec[25] = if is_dir { 0x02 } else { 0x00 };
            both16(1, &mut rec[28..32]);
            rec[32] = id.len() as u8;
            rec[33..33 + id.len()].copy_from_slice(id);
            at += l;
        }
    }
}

/// The fields of a volume descriptor that differ between the primary and
/// the Joliet one.
struct Desc<'a> {
    joliet: bool,
    label: &'a str,
    space: u32,
    pt_size: u32,
    pt_l: u32,
    pt_m: u32,
    root_lba: u32,
    root_len: u32,
    root_date: [u8; 7],
    vol_date: [u8; 17],
}

/// ECMA-119 §8.4 (primary) / §8.5 (supplementary). Byte offsets are
/// 0-based here; the standard's tables are 1-based, so everything is one
/// lower than the number printed there.
fn write_descriptor(out: &mut [u8], d: Desc) {
    let out = &mut out[..BLOCK];
    out.fill(0);
    out[0] = if d.joliet { 2 } else { 1 };
    out[1..6].copy_from_slice(b"CD001");
    out[6] = 1;
    if d.joliet {
        // Volume flags: no escape sequence outside the registered set.
        out[7] = 0;
        // UCS-2 level 3, the escape sequence Windows expects.
        out[88..91].copy_from_slice(b"%/E");
    }
    strfield(&mut out[8..40], "", d.joliet);
    strfield(&mut out[40..72], d.label, d.joliet);
    both32(d.space, &mut out[80..88]);
    both16(1, &mut out[120..124]);
    both16(1, &mut out[124..128]);
    both16(BLOCK as u16, &mut out[128..132]);
    both32(d.pt_size, &mut out[132..140]);
    out[140..144].copy_from_slice(&d.pt_l.to_le_bytes());
    out[148..152].copy_from_slice(&d.pt_m.to_be_bytes());
    // Root directory record, the one directory record that lives outside
    // a directory: identifier 0x00, one byte, so 34 bytes in all.
    let root = &mut out[156..190];
    root[0] = 34;
    both32(d.root_lba, &mut root[2..10]);
    both32(d.root_len, &mut root[10..18]);
    root[18..25].copy_from_slice(&d.root_date);
    root[25] = 0x02;
    both16(1, &mut root[28..32]);
    root[32] = 1;
    root[33] = 0;
    for range in [190..318, 318..446, 446..574, 574..702, 702..739, 739..776, 776..813] {
        strfield(&mut out[range], "", d.joliet);
    }
    for at in [813, 830, 847, 864] {
        out[at..at + 17].copy_from_slice(&d.vol_date);
    }
    out[881] = 1;
}

/// A text field: ASCII padded with spaces, or UCS-2 padded with them in
/// a Joliet descriptor, where every string field is UCS-2 too.
fn strfield(out: &mut [u8], s: &str, joliet: bool) {
    if joliet {
        let units: Vec<u16> = s.encode_utf16().collect();
        for (i, pair) in out.chunks_exact_mut(2).enumerate() {
            let u = units.get(i).copied().unwrap_or(0x20);
            pair.copy_from_slice(&u.to_be_bytes());
        }
    } else {
        out.fill(b' ');
        let b = s.as_bytes();
        let n = b.len().min(out.len());
        out[..n].copy_from_slice(&b[..n]);
    }
}

/// A volume label from the shared folder's own name: d-characters,
/// 32 at most, never empty.
fn volume_label(dir: &Path) -> String {
    let name = dir.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let mut s: String = name
        .chars()
        .map(|c| {
            let c = c.to_ascii_uppercase();
            if c.is_ascii_alphanumeric() || c == '_' {
                c
            } else {
                '_'
            }
        })
        .take(32)
        .collect();
    if s.trim_matches('_').is_empty() {
        s = "CDROM".into();
    }
    s
}

fn both32(v: u32, out: &mut [u8]) {
    out[..4].copy_from_slice(&v.to_le_bytes());
    out[4..8].copy_from_slice(&v.to_be_bytes());
}

fn both16(v: u16, out: &mut [u8]) {
    out[..2].copy_from_slice(&v.to_le_bytes());
    out[2..4].copy_from_slice(&v.to_be_bytes());
}

fn dir_rec_len(id_len: usize) -> usize {
    let l = 33 + id_len;
    l + (l & 1)
}

fn path_rec_len(id_len: usize) -> usize {
    let l = 8 + id_len;
    l + (l & 1)
}

/// Bytes a directory's records need, rounded to whole sectors, with no
/// record straddling one.
fn dir_data_len(lens: &[usize]) -> u32 {
    let mut sectors = 1u32;
    let mut used = 0usize;
    for &l in lens {
        if used + l > BLOCK {
            sectors += 1;
            used = 0;
        }
        used += l;
    }
    sectors * BLOCK as u32
}

fn sectors_for(bytes: u32) -> u32 {
    bytes.div_ceil(BLOCK as u32)
}

/// A sector count as the message about it should read.
fn size_str(sectors: u64) -> String {
    let mib = sectors * BLOCK as u64 / (1 << 20);
    if mib >= 1024 {
        format!("{:.1} GiB", mib as f64 / 1024.0)
    } else {
        format!("{mib} MiB")
    }
}

fn sectors_for_len(len: u64) -> u32 {
    len.div_ceil(BLOCK as u64) as u32
}

/// ISO 9660 orders records by identifier, comparing them as if padded to
/// the same length: with spaces in the primary tree, with zeros in the
/// UCS-2 of a Joliet one.
fn id_cmp(a: &[u8], b: &[u8], pad: u8) -> Ordering {
    for i in 0..a.len().max(b.len()) {
        let (x, y) = (*a.get(i).unwrap_or(&pad), *b.get(i).unwrap_or(&pad));
        if x != y {
            return x.cmp(&y);
        }
    }
    Ordering::Equal
}

/// ISO 9660 level 1: `NAME.EXT;1`, 8.3, uppercase d-characters. This is
/// the tree MSCDEX reads, which is why we do not simply use level 2.
fn iso_ident(name: &str, is_dir: bool) -> Vec<u8> {
    let dchar = |c: char| -> char {
        let c = c.to_ascii_uppercase();
        if c.is_ascii_alphanumeric() || c == '_' {
            c
        } else {
            '_'
        }
    };
    let (stem, ext) = match name.rfind('.') {
        Some(at) if !is_dir && at > 0 => (&name[..at], &name[at + 1..]),
        _ => (name, ""),
    };
    let mut base: String = stem.chars().map(dchar).take(8).collect();
    if base.is_empty() {
        base.push('_');
    }
    if is_dir {
        return base.into_bytes();
    }
    let ext: String = ext.chars().map(dchar).take(3).collect();
    format!("{base}.{ext};1").into_bytes()
}

/// Joliet: the real name in UCS-2, minus the characters the format
/// forbids, truncated to what Windows will read back.
fn joliet_ident(name: &str, is_dir: bool) -> Vec<u8> {
    let limit = if is_dir { JOLIET_MAX } else { JOLIET_MAX - 2 };
    let mut units: Vec<u16> = name
        .chars()
        .map(|c| match c {
            '*' | '/' | ':' | ';' | '?' | '\\' => '_',
            c if (c as u32) < 0x20 => '_',
            c => c,
        })
        .collect::<String>()
        .encode_utf16()
        .collect();
    truncate_utf16(&mut units, limit);
    if !is_dir {
        units.extend(";1".encode_utf16());
    }
    ucs2(&units)
}

/// Cut a UTF-16 string to `limit` code units without leaving half of a
/// surrogate pair behind.
fn truncate_utf16(units: &mut Vec<u16>, limit: usize) {
    if units.len() <= limit {
        return;
    }
    units.truncate(limit);
    if matches!(units.last(), Some(u) if (0xD800..0xDC00).contains(u)) {
        units.pop();
    }
}

fn ucs2(units: &[u16]) -> Vec<u8> {
    units.iter().flat_map(|u| u.to_be_bytes()).collect()
}

/// `~1`, `~2`, … applied to an identifier that a sibling already took.
/// The two trees mangle differently, so each gets its own rule.
fn iso_suffix(id: &[u8], n: u32) -> Vec<u8> {
    let s = String::from_utf8_lossy(id).into_owned();
    let (stem, rest) = match s.find('.') {
        Some(at) => (&s[..at], &s[at..]),
        None => (&s[..], ""),
    };
    let tag = format!("~{n}");
    let keep = 8usize.saturating_sub(tag.len()).min(stem.len());
    format!("{}{tag}{rest}", &stem[..keep]).into_bytes()
}

fn joliet_suffix(id: &[u8], n: u32) -> Vec<u8> {
    let mut units: Vec<u16> = id.chunks_exact(2).map(|p| u16::from_be_bytes([p[0], p[1]])).collect();
    let version = units.len() >= 2 && units[units.len() - 2] == ';' as u16;
    if version {
        units.truncate(units.len() - 2);
    }
    let tag: Vec<u16> = format!("~{n}").encode_utf16().collect();
    let limit = (if version { JOLIET_MAX - 2 } else { JOLIET_MAX }).saturating_sub(tag.len());
    truncate_utf16(&mut units, limit);
    units.extend(tag);
    if version {
        units.extend(";1".encode_utf16());
    }
    ucs2(&units)
}

/// Make `id` unique among `taken` (its siblings' identifiers, folded),
/// case-insensitively: two names that differ only in case are one name to
/// both trees' readers. A set, so a folder of thousands of names sharing
/// their first eight characters opens in a moment rather than minutes.
fn unique(id: Vec<u8>, taken: &mut HashSet<Vec<u8>>, suffix: fn(&[u8], u32) -> Vec<u8>) -> Vec<u8> {
    let fold = |v: &[u8]| -> Vec<u8> { v.iter().map(|b| b.to_ascii_uppercase()).collect() };
    let mut candidate = id;
    let mut n = 1u32;
    while taken.contains(&fold(&candidate)) {
        candidate = suffix(&candidate, n);
        n += 1;
    }
    taken.insert(fold(&candidate));
    candidate
}

/// A directory record's 7-byte date (ECMA-119 §9.1.5), in UTC: the
/// host's zone would make the same tree produce different images on two
/// machines, and nothing in a guest reads these but Explorer's date
/// column.
fn dir_date(t: Option<SystemTime>) -> [u8; 7] {
    let secs = t
        .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let (y, mo, d, h, mi, s) = civil(secs);
    [(y - 1900).clamp(0, 255) as u8, mo as u8, d as u8, h as u8, mi as u8, s as u8, 0]
}

/// A volume descriptor's 17-byte date (ECMA-119 §8.4.26.1): the same
/// instant in decimal digits.
fn volume_date(dir: [u8; 7]) -> [u8; 17] {
    let mut out = [b'0'; 17];
    let year = 1900 + dir[0] as i64;
    let digits = format!("{:04}{:02}{:02}{:02}{:02}{:02}00", year, dir[1], dir[2], dir[3], dir[4], dir[5]);
    out[..16].copy_from_slice(&digits.as_bytes()[..16]);
    out[16] = 0;
    out
}

/// Unix seconds → civil date in UTC (Howard Hinnant's days_from_civil,
/// inverted). Written out because `libdisc` has no dependencies and
/// keeps none: it links into QEMU.
fn civil(secs: i64) -> (i64, i64, i64, i64, i64, i64) {
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m, d, rem / 3600, rem / 60 % 60, rem % 60)
}
