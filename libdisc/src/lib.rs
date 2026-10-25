//! libdisc: raw optical disc model for the QEMU CD-ROM backend (docs 05
//! and 17).
//!
//! Sessions, tracks, indices and per-sector kinds over cue/bin, CloneCD and
//! plain ISO images; raw ⇄ cooked synthesis with EDC/ECC (verified, never
//! corrected); Q-channel synthesis; the MMC responders (`mmc`) and the C
//! API (`capi`) that QEMU's `cdimage` block driver and `atapi.c` use. Reads
//! are synchronous `pread`s on immutable files; a `Disc` is immutable once
//! opened and every method is safe to call from several threads.

#[allow(non_camel_case_types)]
pub mod capi;
pub mod ccd;
pub mod cue;
pub mod ecc;
pub mod iso;
pub mod isodir;
pub mod mds;
pub mod mmc;
pub mod msf;
pub mod sector;
pub mod subq;

use std::fmt;
use std::fs::File;
use std::path::{Path, PathBuf};

/// Raw sector payload size on CD (bytes).
pub const RAW_SECTOR_SIZE: usize = 2352;
/// Cooked Mode 1 / Mode 2 form 1 user-data size (bytes).
pub const COOKED_SECTOR_SIZE: usize = 2048;
/// Subchannel data per sector, deinterleaved P..W (bytes).
pub const SUBCHANNEL_SIZE: usize = 96;
/// The mandatory pause before a track's index 01, in sectors.
pub const PREGAP_SECTORS: i32 = 150;

/// Every failure a caller can see; the C API maps the variants to
/// `LIBDISC_E*` codes and atapi.c to sense keys (doc 17 §3).
#[derive(Debug)]
pub enum Error {
    /// LBA outside `[0, lead-out)`.
    Range,
    /// L-EC failed on a cooked read of a data sector.
    Medium,
    /// Wrong sector kind for the request (audio / gap / form 2 as cooked).
    Mode,
    /// Bad parameter, bad CDB field, or an image that does not parse.
    Invalid(String),
    /// A host file could not be read.
    Io(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Range => write!(f, "LBA out of range"),
            Error::Medium => write!(f, "L-EC uncorrectable error"),
            Error::Mode => write!(f, "illegal mode for this track"),
            Error::Invalid(s) => write!(f, "{s}"),
            Error::Io(s) => write!(f, "I/O error: {s}"),
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e.to_string())
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Track mode as recorded in the TOC / cue sheet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackMode {
    Audio,
    Mode1,
    /// Mode 2, form decided per sector from the subheader (XA).
    Mode2,
    Mode2Form1,
    Mode2Form2,
    /// Mode 2 without subheader interpretation (CD-I).
    Mode2Formless,
}

impl TrackMode {
    pub fn is_data(self) -> bool {
        self != TrackMode::Audio
    }
    /// Header mode byte for synthesized sectors.
    pub fn header_mode(self) -> u8 {
        match self {
            TrackMode::Audio => 0,
            TrackMode::Mode1 => 1,
            _ => 2,
        }
    }
    /// The kind of a synthesized (zero-filled or cooked-sourced) sector of this mode.
    pub fn default_kind(self) -> SectorKind {
        match self {
            TrackMode::Audio => SectorKind::Audio,
            TrackMode::Mode1 => SectorKind::Mode1,
            TrackMode::Mode2 | TrackMode::Mode2Form1 => SectorKind::Mode2Form1,
            TrackMode::Mode2Form2 => SectorKind::Mode2Form2,
            TrackMode::Mode2Formless => SectorKind::Mode2Formless,
        }
    }
}

/// What one sector is, as `libdisc_sector_info.kind` reports it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SectorKind {
    Audio,
    Mode1,
    Mode2Form1,
    Mode2Form2,
    Mode2Formless,
    /// Not covered by any track (between sessions).
    Gap,
}

impl SectorKind {
    /// `LIBDISC_KIND_*` value.
    pub fn code(self) -> u8 {
        match self {
            SectorKind::Audio => 0,
            SectorKind::Mode1 => 1,
            SectorKind::Mode2Form1 => 2,
            SectorKind::Mode2Form2 => 3,
            SectorKind::Mode2Formless => 4,
            SectorKind::Gap => 5,
        }
    }
    pub fn is_data(self) -> bool {
        !matches!(self, SectorKind::Audio | SectorKind::Gap)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SectorInfo {
    pub kind: SectorKind,
    pub track: u8,
    pub index: u8,
    /// Data sectors only: did EDC and parity verify.
    pub lec_ok: Option<bool>,
}

/// One raw TOC entry as a CloneCD `[Entry]` records it, replayed verbatim
/// by READ TOC format 2.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TocEntry {
    pub session: u8,
    pub point: u8,
    pub adr: u8,
    pub control: u8,
    pub min: u8,
    pub sec: u8,
    pub frame: u8,
    pub zero: u8,
    pub pmin: u8,
    pub psec: u8,
    pub pframe: u8,
}

/// How the sectors of an extent are stored in a payload file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layout {
    Cooked2048,
    Mode2_2336,
    Raw2352,
    /// Raw sectors with 96 bytes of interleaved subchannel appended.
    Raw2352Sub96,
}

impl Layout {
    pub fn stride(self) -> usize {
        match self {
            Layout::Cooked2048 => 2048,
            Layout::Mode2_2336 => 2336,
            Layout::Raw2352 => 2352,
            Layout::Raw2352Sub96 => 2448,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    File {
        file: usize,
        offset: u64,
        layout: Layout,
        /// Audio stored big-endian (`MOTOROLA`): swap sample bytes.
        swap: bool,
        /// The extent's last sector may run past the end of the file:
        /// zero-fill the remainder instead of failing. Set only by
        /// `isodir`, where a shared file is whatever length it is and
        /// the sector it ends in is padded, as on a burned disc; for an
        /// image file a short read is a truncated image and stays an error.
        eof_pad: bool,
    },
    /// Sectors of a generated volume held in memory: `blob` indexes the
    /// disc's blobs, `offset` is the byte offset of this extent's first
    /// sector (`isodir`'s descriptors, path tables and directory records).
    Mem {
        blob: usize,
        offset: u64,
    },
    /// Audio pregap / postgap: digital silence.
    Silence,
    /// Data pregap / postgap: zero-filled sectors of the track's mode with valid EDC/ECC.
    ZeroData,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SubSource {
    /// 96 bytes per sector, deinterleaved P..W (CloneCD `.sub`).
    File { file: usize, offset: u64 },
}

/// A contiguous run of sectors with one storage.
#[derive(Debug, Clone, Copy)]
pub struct Extent {
    pub lba: i32,
    pub count: u32,
    pub source: Source,
    pub sub: Option<SubSource>,
}

impl Extent {
    pub fn end(&self) -> i32 {
        self.lba + self.count as i32
    }
}

#[derive(Debug, Clone)]
pub struct Track {
    /// 1..=99, unique across sessions.
    pub number: u8,
    pub mode: TrackMode,
    /// Q control nibble: audio 0x0 (|1 pre-emphasis, |2 DCP, |8 4ch), data 0x4.
    pub control: u8,
    pub isrc: Option<[u8; 12]>,
    /// (index number, absolute LBA of its first sector), ascending; index 1 always present.
    pub indices: Vec<(u8, i32)>,
    /// Index 1.
    pub start_lba: i32,
    /// Exclusive.
    pub end_lba: i32,
    /// Contiguous coverage of `[first index .. end_lba)`.
    pub extents: Vec<Extent>,
}

impl Track {
    /// First sector of the track (index 0 when present, else index 1).
    pub fn first_lba(&self) -> i32 {
        self.indices.first().map(|i| i.1).unwrap_or(self.start_lba)
    }
    /// Index number the sector at `lba` belongs to (the caller knows it is inside the track).
    pub fn index_at(&self, lba: i32) -> u8 {
        let mut idx = self.indices[0].0;
        for &(n, start) in &self.indices {
            if lba >= start {
                idx = n;
            }
        }
        idx
    }
    pub fn index_start(&self, index: u8) -> Option<i32> {
        self.indices.iter().find(|i| i.0 == index).map(|i| i.1)
    }
}

#[derive(Debug, Clone)]
pub struct Session {
    pub number: u8,
    pub tracks: Vec<Track>,
    pub leadout_lba: i32,
}

/// A payload file named by an image (bin / img / sub / wav) or by a
/// shared folder (`isodir`), read by offset.
///
/// The handle is **not** kept: a folder disc names one payload per file
/// and a shared tree can hold thousands, where macOS' default limit is
/// 256 descriptors. `Disc` opens on demand into a small cache instead
/// (`payload_handle`), and re-checks length and mtime every time it
/// opens: a payload that changed underneath us is `Error::Medium` (the
/// read error a drive reports for a damaged sector) rather than a
/// silently torn read. A disc is a snapshot of the tree it was opened on.
pub struct Payload {
    pub path: PathBuf,
    pub len: u64,
    mtime: Option<std::time::SystemTime>,
}

/// How many payload handles a disc keeps open, most recently used first.
/// One is enough for an image (a `.bin` and its `.sub`); a folder reads
/// its files one after another, so a handful covers the interleaving
/// with the metadata blob.
const FD_CACHE: usize = 8;

/// A host file that failed a guest's read, said on stderr (the player's
/// log), the first few times only. A guest sees a failed sector as a sense
/// code, and CD audio as a play that stops, with nothing anywhere to say
/// which file or why: "CD music sometimes doesn't work, especially off a
/// Samba share" was all there was to go on.
fn report_file_error(msg: &str) {
    use std::sync::atomic::{AtomicU32, Ordering};
    static SAID: AtomicU32 = AtomicU32::new(0);
    if SAID.fetch_add(1, Ordering::Relaxed) < 32 {
        eprintln!("libdisc: {msg}");
    }
}

/// The disc.
pub struct Disc {
    /// 1-based session numbers, ascending.
    pub sessions: Vec<Session>,
    /// CATALOG, 13 ASCII digits.
    pub mcn: Option<[u8; 13]>,
    /// CloneCD `[Entry]` records verbatim; `None` = synthesize the raw TOC.
    pub raw_toc: Option<Vec<TocEntry>>,
    files: Vec<Payload>,
    /// Generated sectors held in memory (`Source::Mem`): `isodir`'s
    /// volume descriptors, path tables and directory records.
    blobs: Vec<Vec<u8>>,
    /// Open handles for `files`, most recently used first. The lock is
    /// the only mutable state on a disc; it is held to take a handle,
    /// never across the read itself.
    fds: std::sync::Mutex<Vec<(usize, std::sync::Arc<File>)>>,
}

impl fmt::Debug for Disc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Disc")
            .field("sessions", &self.sessions)
            .field("mcn", &self.mcn)
            .field("raw_toc", &self.raw_toc.as_ref().map(|t| t.len()))
            .field("files", &self.files.iter().map(|p| p.path.display().to_string()).collect::<Vec<_>>())
            .finish()
    }
}

impl Disc {
    pub(crate) fn new() -> Disc {
        Disc {
            sessions: Vec::new(),
            mcn: None,
            raw_toc: None,
            files: Vec::new(),
            blobs: Vec::new(),
            fds: std::sync::Mutex::new(Vec::new()),
        }
    }

    /// Register `path` as a payload file; returns its index for
    /// `Source::File`. The file is opened once here so that a missing or
    /// unreadable payload is an error at open time, as it always was,
    /// and its length and mtime are recorded for `payload_handle`; the
    /// handle itself is dropped (see `Payload`).
    pub(crate) fn add_file(&mut self, path: &Path) -> Result<usize> {
        let file = File::open(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
        let md = file.metadata()?;
        if md.is_dir() {
            return Err(Error::Invalid(format!("{}: is a directory, not a payload file", path.display())));
        }
        self.files.push(Payload { path: path.to_path_buf(), len: md.len(), mtime: md.modified().ok() });
        Ok(self.files.len() - 1)
    }

    /// Register generated sectors; returns their index for `Source::Mem`.
    pub(crate) fn add_blob(&mut self, data: Vec<u8>) -> usize {
        self.blobs.push(data);
        self.blobs.len() - 1
    }

    /// A handle for payload `index`, from the cache or freshly opened.
    /// The `Arc` keeps it alive for the caller's read even if another
    /// thread evicts it meanwhile.
    fn payload_handle(&self, index: usize) -> Result<std::sync::Arc<File>> {
        let mut cache = self.fds.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(at) = cache.iter().position(|(i, _)| *i == index) {
            let entry = cache.remove(at);
            let file = entry.1.clone();
            cache.insert(0, entry);
            return Ok(file);
        }
        let p = &self.files[index];
        let io = |e: std::io::Error| {
            let msg = format!("{}: reopening: {}", p.path.display(), e);
            report_file_error(&msg);
            Error::Io(msg)
        };
        let file = File::open(&p.path).map_err(io)?;
        let md = file.metadata().map_err(io)?;
        if md.len() != p.len || md.modified().ok() != p.mtime {
            /* The disc was laid out over what this file was; serving the
               new bytes at the old offsets would hand the guest a torn
               file with no sign that anything happened. */
            report_file_error(&format!(
                "{}: changed since the disc was opened (length {} -> {}, modified {:?} -> {:?}): reads fail as a changed medium",
                p.path.display(),
                p.len,
                md.len(),
                p.mtime,
                md.modified().ok()
            ));
            return Err(Error::Medium);
        }
        let file = std::sync::Arc::new(file);
        cache.insert(0, (index, file.clone()));
        cache.truncate(FD_CACHE);
        Ok(file)
    }

    /// `buf.len()` bytes of payload `index` at `offset`. With `eof_pad`
    /// a read that runs past the end of the file zero-fills the rest
    /// (the sector a shared file ends in); without it that is an error.
    fn read_payload(&self, index: usize, offset: u64, buf: &mut [u8], eof_pad: bool) -> Result<()> {
        let path = &self.files[index].path;
        let mut want = buf.len();
        if eof_pad {
            let have = self.files[index].len.saturating_sub(offset);
            want = want.min(have as usize);
            buf[want..].fill(0);
            if want == 0 {
                return Ok(());
            }
        }
        let file = self.payload_handle(index)?;
        let buf = &mut buf[..want];
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileExt;
            file.read_exact_at(buf, offset).map_err(|e| {
                let msg = format!("{}: {} bytes at {}: {}", path.display(), buf.len(), offset, e);
                report_file_error(&msg);
                Error::Io(msg)
            })
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::FileExt;
            let mut done = 0;
            while done < buf.len() {
                let n = file.seek_read(&mut buf[done..], offset + done as u64).map_err(|e| {
                    let msg = format!("{}: {} bytes at {}: {}", path.display(), buf.len() - done, offset + done as u64, e);
                    report_file_error(&msg);
                    Error::Io(msg)
                })?;
                if n == 0 {
                    let msg = format!("{}: short read at {}", path.display(), offset + done as u64);
                    report_file_error(&msg);
                    return Err(Error::Io(msg));
                }
                done += n;
            }
            Ok(())
        }
    }

    pub fn file(&self, index: usize) -> &Payload {
        &self.files[index]
    }

    /// Number of payload files the descriptor names (`file` indices are
    /// `0..file_count()`).
    pub fn file_count(&self) -> usize {
        self.files.len()
    }

    /// Open an image: `.cue`, `.ccd`, `.mds`, `.iso`, dispatching on the
    /// extension first and on the content when the extension says
    /// nothing. It also takes a **host directory**, with or without the `isodir:`
    /// prefix the QEMU block driver strips, as a generated ISO 9660 +
    /// Joliet volume over the tree (`isodir`, doc `m5-dirdisc`).
    pub fn open(path: &Path) -> Result<Disc> {
        let path = match path.to_str().and_then(|p| p.strip_prefix("isodir:")) {
            Some(dir) => Path::new(dir),
            None => path,
        };
        if path.is_dir() {
            return isodir::open(path);
        }
        let ext = path.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).unwrap_or_default();
        match ext.as_str() {
            "cue" => cue::open(path),
            "ccd" => ccd::open(path),
            "mds" => mds::open(path),
            "iso" => iso::open(path),
            _ => {
                let mut head = [0u8; 4096];
                let n = {
                    use std::io::Read;
                    let mut f = File::open(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
                    let mut got = 0;
                    while got < head.len() {
                        match f.read(&mut head[got..]) {
                            Ok(0) => break,
                            Ok(k) => got += k,
                            Err(e) => return Err(e.into()),
                        }
                    }
                    got
                };
                match sniff(&head[..n]) {
                    Some("cue") => cue::open(path),
                    Some("ccd") => ccd::open(path),
                    Some("mds") => mds::open(path),
                    Some("iso") => iso::open(path),
                    _ => Err(Error::Invalid(format!("{}: not a disc image libdisc reads", path.display()))),
                }
            }
        }
    }

    /// Lead-out LBA of the last session: the number of addressable sectors.
    pub fn sector_count(&self) -> u32 {
        self.sessions.last().map(|s| s.leadout_lba as u32).unwrap_or(0)
    }

    pub fn track_count(&self) -> u8 {
        self.sessions.iter().map(|s| s.tracks.len() as u8).sum()
    }

    pub fn tracks(&self) -> impl Iterator<Item = (&Session, &Track)> {
        self.sessions.iter().flat_map(|s| s.tracks.iter().map(move |t| (s, t)))
    }

    pub fn track(&self, number: u8) -> Option<(&Session, &Track)> {
        self.tracks().find(|(_, t)| t.number == number)
    }

    pub fn first_track(&self) -> u8 {
        self.tracks().next().map(|(_, t)| t.number).unwrap_or(1)
    }

    pub fn last_track(&self) -> u8 {
        self.tracks().last().map(|(_, t)| t.number).unwrap_or(1)
    }

    fn check_range(&self, lba: i32) -> Result<()> {
        if lba < 0 || lba >= self.sector_count() as i32 {
            Err(Error::Range)
        } else {
            Ok(())
        }
    }

    /// The session and track covering `lba`, with the index number; `None`
    /// inside a gap between sessions.
    pub fn locate(&self, lba: i32) -> Option<(&Session, &Track, u8)> {
        for s in &self.sessions {
            for t in &s.tracks {
                if lba >= t.first_lba() && lba < t.end_lba {
                    return Some((s, t, t.index_at(lba)));
                }
            }
        }
        None
    }

    /// The next track after the one covering `lba` (for the P-channel pause flag).
    fn track_after(&self, number: u8) -> Option<&Track> {
        let mut it = self.tracks().skip_while(|(_, t)| t.number != number);
        it.next();
        it.next().map(|(_, t)| t)
    }

    /// The extent covering `lba`. Extents are ascending and
    /// non-overlapping in every builder, so this is a binary search: a
    /// folder disc has one extent per shared file, and a linear scan
    /// would cost a comparison per file on every sector read.
    fn extent_at<'a>(&self, t: &'a Track, lba: i32) -> Option<&'a Extent> {
        let at = t.extents.partition_point(|e| e.lba <= lba);
        t.extents.get(at.checked_sub(1)?).filter(|e| lba < e.end())
    }

    /// The 2352 raw bytes of the sector at `lba`: stored, or synthesized
    /// from cooked data (with EDC/ECC), silence or zero data.
    pub fn read_raw(&self, lba: i32, out: &mut [u8; 2352]) -> Result<()> {
        self.check_range(lba)?;
        let Some((_, t, _)) = self.locate(lba) else {
            out.fill(0);
            return Ok(());
        };
        let e = self.extent_at(t, lba).ok_or_else(|| Error::Invalid(format!("track {} has no extent at LBA {lba}", t.number)))?;
        match e.source {
            Source::Silence => out.fill(0),
            Source::ZeroData => synth_from_cooked(t.mode, lba, &[0u8; 2048], out),
            Source::Mem { blob, offset } => {
                let off = (offset + (lba - e.lba) as u64 * 2048) as usize;
                let blob = &self.blobs[blob];
                let mut data = [0u8; 2048];
                let end = (off + 2048).min(blob.len());
                if off < end {
                    data[..end - off].copy_from_slice(&blob[off..end]);
                }
                synth_from_cooked(t.mode, lba, &data, out);
            }
            Source::File { file, offset, layout, swap, eof_pad } => {
                let stride = layout.stride();
                let off = offset + (lba - e.lba) as u64 * stride as u64;
                match layout {
                    Layout::Raw2352 | Layout::Raw2352Sub96 => {
                        self.read_payload(file, off, out, eof_pad)?;
                        if swap {
                            for p in out.chunks_exact_mut(2) {
                                p.swap(0, 1);
                            }
                        }
                    }
                    Layout::Cooked2048 => {
                        let mut data = [0u8; 2048];
                        self.read_payload(file, off, &mut data, eof_pad)?;
                        synth_from_cooked(t.mode, lba, &data, out);
                    }
                    Layout::Mode2_2336 => {
                        let mut body = [0u8; 2336];
                        self.read_payload(file, off, &mut body, eof_pad)?;
                        sector::build_mode2_2336(lba, &body, out);
                    }
                }
            }
        }
        Ok(())
    }

    /// The kind of the sector at `lba` without verifying L-EC (reads the
    /// sector only for tracks whose form is per sector).
    pub fn classify(&self, lba: i32) -> Result<(SectorKind, u8, u8)> {
        let mut raw = [0u8; 2352];
        self.classify_into(lba, &mut raw).map(|(c, _)| c)
    }

    /// `classify`, keeping the raw sector when it had to be read to tell
    /// the kind (`true` alongside the result): a stored raw track's
    /// sectors are classified from their own bytes, and a caller that
    /// wants those bytes too would otherwise read the sector twice.
    pub(crate) fn classify_into(&self, lba: i32, raw: &mut [u8; 2352]) -> Result<((SectorKind, u8, u8), bool)> {
        self.check_range(lba)?;
        let Some((_, t, index)) = self.locate(lba) else {
            return Ok(((SectorKind::Gap, 0, 0), false));
        };
        let e = self.extent_at(t, lba).ok_or_else(|| Error::Invalid(format!("track {} has no extent at LBA {lba}", t.number)))?;
        let stored_raw = matches!(e.source, Source::File { layout: Layout::Raw2352 | Layout::Raw2352Sub96 | Layout::Mode2_2336, .. });
        if t.mode == TrackMode::Audio || !stored_raw {
            return Ok(((t.mode.default_kind(), t.number, index), false));
        }
        self.read_raw(lba, raw)?;
        let kind = match (t.mode, sector::detect_kind(raw)) {
            (TrackMode::Mode2Formless, _) => SectorKind::Mode2Formless,
            (_, Some(k)) => k,
            (m, None) => m.default_kind(),
        };
        Ok(((kind, t.number, index), true))
    }

    /// Kind and raw bytes of the sector at `lba`, read once.
    pub fn classify_and_read(&self, lba: i32, raw: &mut [u8; 2352]) -> Result<(SectorKind, u8, u8)> {
        let (c, read) = self.classify_into(lba, raw)?;
        if !read {
            self.read_raw(lba, raw)?;
        }
        Ok(c)
    }

    /// Kind, track, index and L-EC state of the sector at `lba`.
    pub fn sector_info(&self, lba: i32) -> Result<SectorInfo> {
        let mut raw = [0u8; 2352];
        let ((kind, track, index), read) = self.classify_into(lba, &mut raw)?;
        let lec_ok = if kind.is_data() {
            if !read {
                self.read_raw(lba, &mut raw)?;
            }
            Some(sector::verify(&raw, kind).is_ok())
        } else {
            None
        };
        Ok(SectorInfo { kind, track, index, lec_ok })
    }

    /// The 2048 user bytes of a Mode 1 / Mode 2 form 1 sector after L-EC:
    /// errors the P/Q decoder can fix are corrected the way a drive's
    /// decoder does, `Err(Medium)` for what it cannot, `Err(Mode)` for
    /// audio, gap and form 2 sectors (what a drive answers to READ(10)).
    pub fn read_cooked(&self, lba: i32, out: &mut [u8; 2048]) -> Result<()> {
        let mut raw = [0u8; 2352];
        let (kind, _, _) = self.classify_and_read(lba, &mut raw)?;
        let kind = match kind {
            SectorKind::Mode1 | SectorKind::Mode2Form1 => kind,
            SectorKind::Audio | SectorKind::Gap | SectorKind::Mode2Form2 | SectorKind::Mode2Formless => return Err(Error::Mode),
        };
        sector::verify_or_correct(&mut raw, kind)?;
        let (off, _) = sector::user_data_range(kind).unwrap();
        out.copy_from_slice(&raw[off..off + 2048]);
        Ok(())
    }

    /// The 96 subchannel bytes (deinterleaved P..W) of the sector at
    /// `lba`: stored when the extent has a `.sub`, synthesized otherwise.
    pub fn read_sub(&self, lba: i32, out: &mut [u8; 96]) -> Result<()> {
        self.check_range(lba)?;
        if let Some((_, t, _)) = self.locate(lba) {
            if let Some(e) = self.extent_at(t, lba) {
                match e.sub {
                    Some(SubSource::File { file, offset }) => {
                        let off = offset + (lba - e.lba) as u64 * 96;
                        if off + 96 <= self.files[file].len {
                            return self.read_payload(file, off, out, false);
                        }
                    }
                    None => {
                        if let Source::File { file, offset, layout: Layout::Raw2352Sub96, .. } = e.source {
                            let off = offset + (lba - e.lba) as u64 * 2448 + 2352;
                            let mut inter = [0u8; 96];
                            self.read_payload(file, off, &mut inter, false)?;
                            subq::deinterleave(&inter, out);
                            return Ok(());
                        }
                    }
                }
            }
        }
        subq::synthesize(self, lba, out);
        Ok(())
    }

    /// P-channel pause flag for `lba`: inside index 00, or in the 150
    /// sectors before the next track's index 01.
    pub(crate) fn pause_at(&self, lba: i32) -> bool {
        match self.locate(lba) {
            Some((_, t, index)) => {
                if index == 0 {
                    return true;
                }
                match self.track_after(t.number) {
                    Some(n) => n.start_lba - lba <= PREGAP_SECTORS && lba < n.start_lba,
                    None => false,
                }
            }
            None => true,
        }
    }
}

/// A raw sector of `mode` around 2048 cooked bytes.
fn synth_from_cooked(mode: TrackMode, lba: i32, data: &[u8; 2048], out: &mut [u8; 2352]) {
    match mode {
        TrackMode::Audio => {
            out.fill(0);
        }
        TrackMode::Mode1 => sector::build_mode1(lba, data, out),
        TrackMode::Mode2Form2 => {
            let mut d = [0u8; 2324];
            d[..2048].copy_from_slice(data);
            sector::build_mode2_form2(lba, &[0, 0, 0x20, 0, 0, 0, 0x20, 0], &d, out)
        }
        _ => sector::build_mode2_form1(lba, &[0u8; 8], data, out),
    }
}

/// Content sniff for `Disc::open` and `libdisc_probe`: `"cue"`, `"ccd"`,
/// `"iso"` or `None`.
pub fn sniff(head: &[u8]) -> Option<&'static str> {
    let text = String::from_utf8_lossy(head);
    let t = text.to_ascii_uppercase();
    if t.trim_start().starts_with("[CLONECD]") {
        return Some("ccd");
    }
    if head.starts_with(b"MEDIA DESCRIPTOR") {
        return Some("mds");
    }
    if t.contains("TRACK ") && t.contains("FILE ") && t.contains("INDEX ") {
        return Some("cue");
    }
    if head.len() >= 16 * 2048 + 6 && &head[16 * 2048 + 1..16 * 2048 + 6] == b"CD001" {
        return Some("iso");
    }
    None
}

/// Resolve a payload path named by an image next to `base`: as written,
/// then case-insensitively in the same directory (dumps made on Windows).
/// A name that leaves the image's folder (absolute, or through `..`) is
/// taken by its leaf only: a downloaded image must not hand the guest
/// any file of the host's.
pub(crate) fn resolve_payload(base: &Path, name: &str) -> Result<PathBuf> {
    let dir = base.parent().unwrap_or_else(|| Path::new("."));
    let name = name.replace('\\', "/");
    let leaf = Path::new(&name).file_name().and_then(|s| s.to_str()).unwrap_or(&name).to_string();
    let inside = !name.contains(':')
        && Path::new(&name).components().all(|c| matches!(c, std::path::Component::Normal(_) | std::path::Component::CurDir));
    let direct = dir.join(if inside { name.as_str() } else { leaf.as_str() });
    if direct.is_file() {
        return Ok(direct);
    }
    let direct_leaf = dir.join(&leaf);
    if direct_leaf.is_file() {
        return Ok(direct_leaf);
    }
    if let Ok(rd) = std::fs::read_dir(dir) {
        for ent in rd.flatten() {
            if ent.file_name().to_string_lossy().eq_ignore_ascii_case(&leaf) && ent.path().is_file() {
                return Ok(ent.path());
            }
        }
    }
    Err(Error::Invalid(format!("{}: payload file not found: {}", base.display(), direct.display())))
}
