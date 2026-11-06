//! A real CD/DVD drive as a disc: `hostcd:<speed>:<device>` (doc 17 §9).
//!
//! The drive is asked what a dumping tool asks it: the full TOC (READ TOC
//! format 2), every sector raw with its 96 bytes of subchannel (READ CD,
//! byte 9 = 0xF8, byte 10 = raw P..W), and, where raw reads are not to be
//! had (a DVD, a drive that refuses them), the cooked 2048 bytes (READ(12)).
//! The model built from those answers is the one an image gets, so CD
//! audio, multi-session TOCs, subchannel and the protection band's read
//! errors reach the guest exactly as they do from a `.ccd` dump: `mmc`
//! answers from it and `atapi.c` never knows the difference.
//!
//! Three ways to the drive (`Transport`):
//!
//! * **SCSI pass-through** (`Scsi`): Linux's `SG_IO`, Windows' SPTI. Every
//!   command above, so everything above works.
//! * **Windows' CD-ROM IOCTLs** (`win::Ioctl`), when SPTI is refused (it
//!   wants an administrator on most installs): the full TOC
//!   (`IOCTL_CDROM_READ_TOC_EX`), audio sectors (`IOCTL_CDROM_RAW_READ`)
//!   and cooked data (`ReadFile`). CD audio works; a data sector's raw
//!   bytes and the subchannel are synthesized, as for an `.iso`, so a
//!   protection that reads its band raw does not see it.
//! * **A simulated drive** (`sim:<image>`): the same SCSI commands answered
//!   by `mmc` from an image, so `discx hostcd` checks the whole path, TOC
//!   parsing to raw reads, on a machine without a drive.
//!
//! Reads go through a one-chunk read-ahead: a guest reads up to 64
//! sectors per command and libdisc asks one at a time, and one SCSI
//! command per sector is slower than the drive. A failed chunk is retried
//! sector by sector, so one unreadable sector (a protection band) fails
//! alone, as `Error::Medium`, which is what the drive itself said.

use crate::msf::Msf;
use crate::{Disc, Error, Extent, Result, Session, Source, TocEntry, Track, TrackMode};
use std::sync::Mutex;

/// The prefix of a drive's medium name, as QEMU's `hostcd` driver and
/// `Disc::open` take it.
pub const PREFIX: &str = "hostcd:";

/// 1x for CD in the units SET CD SPEED takes (kB/s, MMC's 176.4 rounded
/// the way drives round it).
pub const CD_1X_KBPS: u16 = 176;

/// `hostcd:<speed>:<device>`: the speed (`max`, or `<n>x`) and the device.
/// `None` for anything else.
pub fn parse_medium(name: &str) -> Option<(Speed, &str)> {
    let rest = name.strip_prefix(PREFIX)?;
    let (speed, dev) = rest.split_once(':')?;
    let speed = match speed {
        "max" | "" => Speed::Max,
        s => Speed::Times(s.trim_end_matches(['x', 'X']).parse().ok()?),
    };
    (!dev.is_empty()).then_some((speed, dev))
}

/// The medium name for `dev` at `speed`, the inverse of `parse_medium`.
pub fn medium(dev: &str, speed: Speed) -> String {
    match speed {
        Speed::Max => format!("{PREFIX}max:{dev}"),
        Speed::Times(n) => format!("{PREFIX}{n}x:{dev}"),
    }
}

/// What speed to ask the drive for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Speed {
    /// The drive's fastest (SET CD SPEED 0xFFFF).
    Max,
    /// A CD speed, `n` x 176 kB/s; a DVD drive takes its nearest.
    Times(u16),
}

impl Speed {
    fn kbps(self) -> u16 {
        match self {
            Speed::Max => 0xFFFF,
            Speed::Times(n) => n.saturating_mul(CD_1X_KBPS),
        }
    }
}

/// How much of a disc reaches the guest through this drive.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Access {
    /// SCSI pass-through: raw sectors, subchannel, read errors. Audio and
    /// copy protection as from a dump.
    Full,
    /// Windows' CD-ROM IOCTLs only (no administrator): data and CD audio,
    /// no raw data sectors or subchannel.
    Basic,
}

/// What `query` finds out about a drive without reading a disc.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DriveInfo {
    pub access: Access,
    /// The fastest read the drive reports (MODE SENSE page 2A), kB/s;
    /// `None` when it does not say.
    pub max_kbps: Option<u16>,
}

/// Open `dev` long enough to see how it can be read and how fast. Fast:
/// no disc is read, so a sleeping drive is not spun up. `Err` when this
/// platform or this user cannot reach the drive this way at all; the
/// caller then falls back to QEMU's own data-only host device.
pub fn query(dev: &str) -> Result<DriveInfo> {
    let t = open_transport(dev)?;
    Ok(DriveInfo {
        access: t.access(),
        max_kbps: t.max_speed(),
    })
}

/// Whether `dev` has a readable disc in it now (its TOC answers).
pub fn has_disc(dev: &str) -> bool {
    open_transport(dev).is_ok_and(|t| t.toc(0).is_ok())
}

/// The speeds a picker offers for a drive whose fastest is `max_kbps`:
/// the usual CD speeds up to it, slowest last. Unknown: the usual ones up
/// to 52x.
pub fn speed_choices(max_kbps: Option<u16>) -> Vec<u16> {
    let top = max_kbps.map_or(52, |k| (k / CD_1X_KBPS).max(1));
    [52u16, 48, 40, 32, 24, 16, 8, 4, 2, 1]
        .into_iter()
        .filter(|&x| x <= top)
        .collect()
}

// ---------------------------------------------------------------- transport

/// One way of talking to the drive. Every method is one request; the
/// caller (`Drive`) does the caching and the retries.
pub(crate) trait Transport: Send + Sync {
    fn access(&self) -> Access;
    /// The READ TOC reply, format 0 (LBA) or 2 (full TOC, session 1 on).
    fn toc(&self, format: u8) -> Result<Vec<u8>>;
    /// Number of readable sectors (READ CAPACITY's last LBA + 1).
    fn capacity(&self) -> Result<u32>;
    /// `n` raw sectors from `lba`, 2352 bytes each, then 96 of raw P..W
    /// each when `sub` (2448). Audio sectors through any transport;
    /// data sectors only when `access()` is `Full`.
    fn read_cd(&self, lba: i32, n: u32, sub: bool, out: &mut [u8]) -> Result<()>;
    /// `n` cooked 2048-byte data sectors from `lba`.
    fn read_data(&self, lba: i32, n: u32, out: &mut [u8]) -> Result<()>;
    fn set_speed(&self, kbps: u16) -> Result<()>;
    fn max_speed(&self) -> Option<u16>;
    /// READ SUB-CHANNEL's catalog number (format 2) or a track's ISRC
    /// (format 3): the 13 or 12 characters after the valid bit, or `None`.
    fn code(&self, _format: u8, _track: u8) -> Option<Vec<u8>> {
        None
    }
}

/// A SCSI command channel: one CDB, data in (or none), the bytes that came.
pub(crate) trait Scsi: Send + Sync {
    fn cmd(&self, cdb: &[u8], data: &mut [u8], timeout_s: u32) -> Result<usize>;
}

fn be16(b: &[u8]) -> u16 {
    u16::from_be_bytes([b[0], b[1]])
}
fn be32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

/// The commands, over any `Scsi`.
struct ScsiDrive<S: Scsi>(S);

impl<S: Scsi> Transport for ScsiDrive<S> {
    fn access(&self) -> Access {
        Access::Full
    }

    fn toc(&self, format: u8) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; 4096];
        let msf = if format == 2 { 0x02 } else { 0 };
        let len = buf.len() as u16;
        let cdb = [
            0x43,
            msf,
            format,
            0,
            0,
            0,
            1,
            (len >> 8) as u8,
            len as u8,
            0,
        ];
        let got = self.0.cmd(&cdb, &mut buf, 20)?;
        if got < 4 {
            return Err(Error::Invalid("READ TOC: short reply".into()));
        }
        let want = (be16(&buf) as usize + 2).min(got);
        buf.truncate(want);
        Ok(buf)
    }

    fn capacity(&self) -> Result<u32> {
        let mut buf = [0u8; 8];
        self.0
            .cmd(&[0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], &mut buf, 20)?;
        Ok(be32(&buf).wrapping_add(1))
    }

    fn read_cd(&self, lba: i32, n: u32, sub: bool, out: &mut [u8]) -> Result<()> {
        let l = (lba as u32).to_be_bytes();
        let cdb = [
            0xBE,
            0,
            l[0],
            l[1],
            l[2],
            l[3],
            (n >> 16) as u8,
            (n >> 8) as u8,
            n as u8,
            0xF8,
            sub as u8,
            0,
        ];
        let want = n as usize * if sub { 2448 } else { 2352 };
        let got = self.0.cmd(&cdb, &mut out[..want], 30)?;
        if got < want {
            return Err(Error::Medium);
        }
        Ok(())
    }

    fn read_data(&self, lba: i32, n: u32, out: &mut [u8]) -> Result<()> {
        let l = (lba as u32).to_be_bytes();
        let c = n.to_be_bytes();
        let cdb = [
            0xA8, 0, l[0], l[1], l[2], l[3], c[0], c[1], c[2], c[3], 0, 0,
        ];
        let want = n as usize * 2048;
        let got = self.0.cmd(&cdb, &mut out[..want], 30)?;
        if got < want {
            return Err(Error::Medium);
        }
        Ok(())
    }

    fn set_speed(&self, kbps: u16) -> Result<()> {
        let k = kbps.to_be_bytes();
        self.0
            .cmd(
                &[0xBB, 0, k[0], k[1], 0xFF, 0xFF, 0, 0, 0, 0, 0, 0],
                &mut [],
                10,
            )
            .map(|_| ())
    }

    fn max_speed(&self) -> Option<u16> {
        let mut buf = [0u8; 64];
        let cdb = [0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, buf.len() as u8, 0];
        let got = self.0.cmd(&cdb, &mut buf, 10).ok()?;
        let page = 8 + be16(&buf[6..8]) as usize;
        if got < page + 10 || buf[page] & 0x3F != 0x2A {
            return None;
        }
        Some(be16(&buf[page + 8..page + 10])).filter(|&k| k > 0)
    }

    fn code(&self, format: u8, track: u8) -> Option<Vec<u8>> {
        let mut buf = [0u8; 24];
        self.0
            .cmd(
                &[0x42, 0, 0x40, format, 0, 0, track, 0, 24, 0],
                &mut buf,
                10,
            )
            .ok()?;
        let n = if format == 2 { 13 } else { 12 };
        (buf[8] & 0x80 != 0).then(|| buf[9..9 + n].to_vec())
    }
}

/// The transport for a device name: `sim:<image>`, else the platform's.
fn open_transport(dev: &str) -> Result<Box<dyn Transport>> {
    if let Some(image) = dev.strip_prefix("sim:") {
        return Ok(Box::new(ScsiDrive(sim::Sim::open(image)?)));
    }
    platform_transport(dev)
}

#[cfg(target_os = "linux")]
fn platform_transport(dev: &str) -> Result<Box<dyn Transport>> {
    let sg = linux::SgIo::open(dev)?;
    // INQUIRY answers with no disc in: it proves pass-through works for
    // this user before anything is built on it
    sg.cmd(&[0x12, 0, 0, 0, 36, 0], &mut [0u8; 36], 10)?;
    Ok(Box::new(ScsiDrive(sg)))
}

#[cfg(windows)]
fn platform_transport(dev: &str) -> Result<Box<dyn Transport>> {
    win::open(dev)
}

#[cfg(not(any(target_os = "linux", windows)))]
fn platform_transport(dev: &str) -> Result<Box<dyn Transport>> {
    Err(Error::Invalid(format!(
        "{dev}: reading a drive directly is not supported on this platform"
    )))
}

// -------------------------------------------------------------------- Drive

/// How many sectors one read-ahead asks for: under 64 KiB either way,
/// which every pass-through path takes in one transfer.
const RAW_CHUNK: u32 = 24; // x 2448
const COOKED_CHUNK: u32 = 32; // x 2048

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Raw,
    Cooked,
}

struct Chunk {
    kind: Kind,
    lba: i32,
    n: u32,
    stride: usize,
    data: Vec<u8>,
}

/// The drive behind a `Disc` opened from `hostcd:`. Held by the disc;
/// every read of a `Source::Drive` extent comes here.
pub struct Drive {
    t: Mutex<Box<dyn Transport>>,
    /// READ CD works for data sectors (else they are read cooked).
    raw: bool,
    /// READ CD's raw P..W works (else the subchannel is synthesized).
    sub: bool,
    name: String,
    chunk: Mutex<Option<Chunk>>,
}

impl std::fmt::Debug for Drive {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Drive({}, raw={}, sub={})",
            self.name, self.raw, self.sub
        )
    }
}

/// What one sector came back as.
// 2352 against 2048 bytes, returned by value once per sector: boxing
// would allocate on every read to save 304 bytes of stack
#[allow(clippy::large_enum_variant)]
pub(crate) enum Sector {
    Raw([u8; 2352]),
    Cooked([u8; 2048]),
}

impl Drive {
    pub fn raw_reads(&self) -> bool {
        self.raw
    }

    pub fn subchannel(&self) -> bool {
        self.sub
    }

    /// Sector `lba` of a track that ends at `end` (exclusive): raw when it
    /// is audio or the drive reads data raw, cooked otherwise.
    pub(crate) fn read(&self, lba: i32, end: i32, audio: bool) -> Result<Sector> {
        if audio || self.raw {
            let mut out = [0u8; 2352];
            self.cached(Kind::Raw, lba, end, |c, at| {
                out.copy_from_slice(&c[at..at + 2352])
            })?;
            Ok(Sector::Raw(out))
        } else {
            let mut out = [0u8; 2048];
            self.cached(Kind::Cooked, lba, end, |c, at| {
                out.copy_from_slice(&c[at..at + 2048])
            })?;
            Ok(Sector::Cooked(out))
        }
    }

    /// The 96 interleaved subchannel bytes the drive read with `lba`;
    /// `None` when it does not deliver them.
    pub(crate) fn read_sub(&self, lba: i32, end: i32) -> Result<Option<[u8; 96]>> {
        if !self.sub {
            return Ok(None);
        }
        let mut out = [0u8; 96];
        self.cached(Kind::Raw, lba, end, |c, at| {
            out.copy_from_slice(&c[at + 2352..at + 2448])
        })?;
        Ok(Some(out))
    }

    fn stride(&self, kind: Kind) -> usize {
        match kind {
            Kind::Cooked => 2048,
            Kind::Raw if self.sub => 2448,
            Kind::Raw => 2352,
        }
    }

    /// Run `take(bytes, offset)` on sector `lba` from the read-ahead,
    /// reading a new chunk when it is not there.
    fn cached(
        &self,
        kind: Kind,
        lba: i32,
        end: i32,
        take: impl FnOnce(&[u8], usize),
    ) -> Result<()> {
        let mut chunk = self.chunk.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(c) = chunk
            .as_ref()
            .filter(|c| c.kind == kind && lba >= c.lba && lba < c.lba + c.n as i32)
        {
            take(&c.data, (lba - c.lba) as usize * c.stride);
            return Ok(());
        }
        let stride = self.stride(kind);
        let max = if kind == Kind::Raw {
            RAW_CHUNK
        } else {
            COOKED_CHUNK
        };
        // never across the end of the track: a drive asked for audio and
        // data in one READ CD may refuse the lot
        let n = ((end - lba).max(1) as u32).min(max);
        let mut data = vec![0u8; n as usize * stride];
        let t = self.t.lock().unwrap_or_else(|e| e.into_inner());
        let read = |t: &dyn Transport, at: i32, k: u32, buf: &mut [u8]| match kind {
            Kind::Raw => t.read_cd(at, k, self.sub, buf),
            Kind::Cooked => t.read_data(at, k, buf),
        };
        let n = match read(t.as_ref(), lba, n, &mut data) {
            Ok(()) => n,
            Err(_) => {
                // one bad sector in the chunk: read just this one, so it
                // fails alone and the ones around it still read
                data.truncate(stride);
                if let Err(e) = read(t.as_ref(), lba, 1, &mut data) {
                    report(&format!("{}: LBA {lba}: {e}", self.name));
                    return Err(Error::Medium);
                }
                1
            }
        };
        take(&data, 0);
        *chunk = Some(Chunk {
            kind,
            lba,
            n,
            stride,
            data,
        });
        Ok(())
    }
}

fn report(msg: &str) {
    use std::sync::atomic::{AtomicU32, Ordering};
    static SAID: AtomicU32 = AtomicU32::new(0);
    if SAID.fetch_add(1, Ordering::Relaxed) < 32 {
        eprintln!("libdisc: {msg}");
    }
}

// ---------------------------------------------------------------- the model

/// `Disc::open` for `hostcd:<speed>:<device>`.
pub fn open(name: &str) -> Result<Disc> {
    let (speed, dev) = parse_medium(name)
        .ok_or_else(|| Error::Invalid(format!("{name}: expected {PREFIX}<speed>:<device>")))?;
    let t = open_transport(dev)?;
    // before the TOC: a speed change can spin the disc down and up again
    let _ = t.set_speed(speed.kbps());
    let (sessions, raw_toc) =
        layout(t.as_ref()).map_err(|e| Error::Invalid(format!("{dev}: no readable disc ({e})")))?;
    let mut disc = Disc::new();

    // What the drive will do, tried on the disc itself: a raw read of
    // the first data sector (a DVD, or a drive or transport that refuses,
    // reads cooked), and the subchannel beside it.
    let first_data = sessions
        .iter()
        .flat_map(|s| &s.tracks)
        .find(|t| t.mode.is_data())
        .map(|t| t.start_lba);
    let probe_lba = first_data
        .or_else(|| {
            sessions
                .first()
                .and_then(|s| s.tracks.first())
                .map(|t| t.start_lba)
        })
        .unwrap_or(0);
    let mut buf = vec![0u8; 2448];
    let raw =
        t.access() == Access::Full && t.read_cd(probe_lba, 1, false, &mut buf[..2352]).is_ok();
    let sub = t.access() == Access::Full && t.read_cd(probe_lba, 1, true, &mut buf).is_ok();
    let raw = raw && first_data.is_some();

    let drive = Drive {
        t: Mutex::new(t),
        raw,
        sub,
        name: dev.to_string(),
        chunk: Mutex::new(None),
    };

    // A data track's mode from its own first sector's header, when the
    // drive reads raw; Mode 1 otherwise, which every PC disc is.
    let mut sessions = sessions;
    for s in &mut sessions {
        for tr in &mut s.tracks {
            if tr.mode.is_data() && raw {
                if let Ok(Sector::Raw(r)) = drive.read(tr.start_lba, tr.end_lba, false) {
                    tr.mode = match r[15] {
                        2 => TrackMode::Mode2,
                        _ => TrackMode::Mode1,
                    };
                }
            }
        }
    }
    if drive.sub {
        find_pregaps(&drive, &mut sessions);
    }
    {
        let t = drive.t.lock().unwrap_or_else(|e| e.into_inner());
        disc.mcn = t
            .code(2, 0)
            .and_then(|v| v.try_into().ok())
            .filter(|c: &[u8; 13]| c.iter().all(u8::is_ascii_digit));
        for s in &mut sessions {
            for tr in s.tracks.iter_mut().filter(|t| !t.mode.is_data()) {
                tr.isrc = t
                    .code(3, tr.number)
                    .and_then(|v| v.try_into().ok())
                    .filter(|c: &[u8; 12]| c.iter().all(u8::is_ascii_alphanumeric));
            }
        }
    }
    disc.sessions = sessions;
    disc.raw_toc = raw_toc;
    disc.drive = Some(drive);
    Ok(disc)
}

/// `(track, index)` from the Q channel of a sector, when it is a
/// position frame (ADR 1) whose CRC holds.
fn q_position(drive: &Drive, lba: i32, end: i32) -> Option<(u8, u8)> {
    let inter = drive.read_sub(lba, end).ok()??;
    let mut de = [0u8; 96];
    crate::subq::deinterleave(&inter, &mut de);
    let q = &de[12..24];
    let bcd = |b: u8| (b >> 4) * 10 + (b & 0x0F);
    (q[0] & 0x0F == 1 && crate::subq::q_crc_ok(q)).then(|| (bcd(q[1]), bcd(q[2])))
}

/// The TOC gives each track's index 1 only; the pause before it (index
/// 0, the two seconds of silence before an audio track) is in the
/// subchannel. Where the sector before a track says "this track, index
/// 0", the pregap's start is found the way a ripper finds it, and the
/// sectors move from the track before into this one. Without that a
/// pregap would read as the end of the previous track: a data track's
/// for the first audio track, which is the wrong sector kind.
fn find_pregaps(drive: &Drive, sessions: &mut [Session]) {
    for s in sessions.iter_mut() {
        for i in 1..s.tracks.len() {
            let (prev_start, start, number) = (
                s.tracks[i - 1].start_lba,
                s.tracks[i].start_lba,
                s.tracks[i].number,
            );
            let in_pregap = |lba: i32| q_position(drive, lba, start) == Some((number, 0));
            if !in_pregap(start - 1) {
                continue;
            }
            // the first sector of the run of (this track, index 0) before
            // index 1, at most ten seconds back and never into the track
            // before's own first sector
            let (mut lo, mut hi) = ((start - 750).max(prev_start + 1), start - 1);
            while lo < hi {
                let mid = lo + (hi - lo) / 2;
                if in_pregap(mid) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            let gap = hi;
            let prev = &mut s.tracks[i - 1];
            prev.end_lba = gap;
            // from its own first sector: its index 0, when it has one
            let first = prev.first_lba();
            prev.extents = vec![Extent {
                lba: first,
                count: (gap - first) as u32,
                source: Source::Drive,
                sub: None,
            }];
            let t = &mut s.tracks[i];
            t.indices = vec![(0, gap), (1, t.start_lba)];
            t.extents = vec![Extent {
                lba: gap,
                count: (t.end_lba - gap) as u32,
                source: Source::Drive,
                sub: None,
            }];
        }
    }
}

/// The sessions and tracks from the drive's TOC: the full TOC when it has
/// one (a CD), else the plain track list and READ CAPACITY (a DVD).
fn layout(t: &dyn Transport) -> Result<(Vec<Session>, Option<Vec<TocEntry>>)> {
    if let Ok(reply) = t.toc(2) {
        if let Ok(v) = from_full_toc(&reply) {
            return Ok(v);
        }
    }
    let reply = t.toc(0)?;
    let cap = t.capacity().ok();
    Ok((from_track_list(&reply, cap)?, None))
}

fn track(number: u8, control: u8, start: i32) -> Track {
    Track {
        number,
        mode: if control & 0x4 != 0 {
            TrackMode::Mode1
        } else {
            TrackMode::Audio
        },
        control,
        isrc: None,
        indices: vec![(1, start)],
        start_lba: start,
        end_lba: start,
        extents: Vec::new(),
    }
}

/// Close each track at the next one's start (the last at the session's
/// lead-out) and give it its one extent on the drive.
fn finish(sessions: &mut [Session]) -> Result<()> {
    for s in sessions.iter_mut() {
        s.tracks.sort_by_key(|t| t.start_lba);
        let ends: Vec<i32> = s
            .tracks
            .iter()
            .skip(1)
            .map(|t| t.start_lba)
            .chain([s.leadout_lba])
            .collect();
        for (t, end) in s.tracks.iter_mut().zip(ends) {
            if end <= t.start_lba {
                return Err(Error::Invalid(format!(
                    "track {} ends before it starts",
                    t.number
                )));
            }
            t.end_lba = end;
            t.extents = vec![Extent {
                lba: t.start_lba,
                count: (end - t.start_lba) as u32,
                source: Source::Drive,
                sub: None,
            }];
        }
    }
    if sessions.iter().all(|s| s.tracks.is_empty()) {
        return Err(Error::Invalid("the TOC lists no tracks".into()));
    }
    Ok(())
}

/// READ TOC format 2: 11-byte descriptors, A0 / A1 / A2 per session and
/// a point per track, MSF in binary. Kept verbatim for the guest's own
/// format 2 (`raw_toc`), as a CloneCD dump keeps it.
fn from_full_toc(reply: &[u8]) -> Result<(Vec<Session>, Option<Vec<TocEntry>>)> {
    let mut entries = Vec::new();
    for d in reply.get(4..).unwrap_or_default().chunks_exact(11) {
        entries.push(TocEntry {
            session: d[0],
            adr: d[1] >> 4,
            control: d[1] & 0x0F,
            point: d[3],
            min: d[4],
            sec: d[5],
            frame: d[6],
            zero: d[7],
            pmin: d[8],
            psec: d[9],
            pframe: d[10],
        });
    }
    let mut sessions: Vec<Session> = Vec::new();
    for e in entries.iter().filter(|e| e.adr == 1) {
        let at = match sessions.iter().position(|s| s.number == e.session) {
            Some(i) => i,
            None => {
                sessions.push(Session {
                    number: e.session,
                    tracks: Vec::new(),
                    leadout_lba: 0,
                });
                sessions.len() - 1
            }
        };
        let lba = Msf {
            m: e.pmin,
            s: e.psec,
            f: e.pframe,
        }
        .to_lba();
        match e.point {
            1..=99 => sessions[at].tracks.push(track(e.point, e.control, lba)),
            0xA2 => sessions[at].leadout_lba = lba,
            _ => {}
        }
    }
    sessions.retain(|s| !s.tracks.is_empty());
    sessions.sort_by_key(|s| s.number);
    if sessions.iter().any(|s| s.leadout_lba == 0) {
        return Err(Error::Invalid("full TOC without a lead-out".into()));
    }
    finish(&mut sessions)?;
    Ok((sessions, Some(entries)))
}

/// READ TOC format 0 (LBA): 8-byte descriptors, AA the lead-out. One
/// session; a DVD's.
fn from_track_list(reply: &[u8], capacity: Option<u32>) -> Result<Vec<Session>> {
    let mut s = Session {
        number: 1,
        tracks: Vec::new(),
        leadout_lba: 0,
    };
    for d in reply.get(4..).unwrap_or_default().chunks_exact(8) {
        let lba = be32(&d[4..8]) as i32;
        match d[2] {
            1..=99 => s.tracks.push(track(d[2], d[1] & 0x0F, lba)),
            0xAA => s.leadout_lba = lba,
            _ => {}
        }
    }
    if let Some(c) = capacity.filter(|&c| c > 0) {
        // a DVD's TOC lead-out can be anything; READ CAPACITY is not
        if s.leadout_lba <= s.tracks.last().map_or(0, |t| t.start_lba) || s.tracks.len() == 1 {
            s.leadout_lba = c as i32;
        }
    }
    let mut v = vec![s];
    finish(&mut v)?;
    Ok(v)
}

// -------------------------------------------------------------- simulated

/// `sim:<image>`: an image answering the SCSI commands a drive gets, from
/// `mmc`'s own responders, for `discx hostcd`.
mod sim {
    use super::{be32, Scsi};
    use crate::{mmc, Disc, Error, Result};
    use std::path::Path;

    pub struct Sim(Disc);

    impl Sim {
        pub fn open(image: &str) -> Result<Sim> {
            Ok(Sim(Disc::open(Path::new(image))?))
        }
    }

    impl Scsi for Sim {
        fn cmd(&self, cdb: &[u8], data: &mut [u8], _timeout_s: u32) -> Result<usize> {
            let d = &self.0;
            let put = |data: &mut [u8], v: &[u8]| {
                let n = v.len().min(data.len());
                data[..n].copy_from_slice(&v[..n]);
                n
            };
            match cdb[0] {
                0x12 => Ok(put(data, b"\x05\x80\x05\x32\x1f\0\0\0libdisc sim drive")),
                0x43 => {
                    let mut out = Vec::new();
                    mmc::read_toc(d, cdb[2] & 0x0F, cdb[1] & 2 != 0, cdb[6], &mut out)?;
                    Ok(put(data, &out))
                }
                0x25 => {
                    let last = d.sector_count().saturating_sub(1);
                    let mut out = [0u8; 8];
                    out[..4].copy_from_slice(&last.to_be_bytes());
                    out[4..].copy_from_slice(&2048u32.to_be_bytes());
                    Ok(put(data, &out))
                }
                0xBE => {
                    let lba = be32(&cdb[2..6]) as i32;
                    let n = u32::from_be_bytes([0, cdb[6], cdb[7], cdb[8]]);
                    let len = mmc::read_cd_length(cdb[1] >> 2 & 7, cdb[9], cdb[10])?;
                    for i in 0..n as usize {
                        mmc::read_cd_sector(
                            d,
                            lba + i as i32,
                            cdb[1] >> 2 & 7,
                            cdb[9],
                            cdb[10],
                            &mut data[i * len..(i + 1) * len],
                        )?;
                    }
                    Ok(n as usize * len)
                }
                0xA8 => {
                    let lba = be32(&cdb[2..6]) as i32;
                    let n = be32(&cdb[6..10]) as usize;
                    for i in 0..n {
                        let mut s = [0u8; 2048];
                        d.read_cooked(lba + i as i32, &mut s)?;
                        data[i * 2048..(i + 1) * 2048].copy_from_slice(&s);
                    }
                    Ok(n * 2048)
                }
                0x42 => {
                    let mut out = Vec::new();
                    mmc::read_subchannel(
                        d,
                        0,
                        cdb[1] & 2 != 0,
                        cdb[2] & 0x40 != 0,
                        cdb[3],
                        cdb[6],
                        0x15,
                        &mut out,
                    )?;
                    Ok(put(data, &out))
                }
                0xBB => Ok(0),
                0x5A => {
                    // a 40x drive: header, then page 2A with max read speed at +8
                    let mut out = [0u8; 8 + 20];
                    out[8] = 0x2A;
                    out[9] = 18;
                    out[16..18].copy_from_slice(&(40u16 * super::CD_1X_KBPS).to_be_bytes());
                    Ok(put(data, &out))
                }
                op => Err(Error::Invalid(format!(
                    "sim drive: command {op:#04x} not modelled"
                ))),
            }
        }
    }
}

// -------------------------------------------------------------------- Linux

#[cfg(target_os = "linux")]
mod linux {
    use super::Scsi;
    use crate::{Error, Result};
    use std::ffi::{c_int, c_uint, c_ulong, c_void};
    use std::fs::{File, OpenOptions};
    use std::os::unix::fs::OpenOptionsExt;
    use std::os::unix::io::AsRawFd;

    extern "C" {
        fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    }

    const SG_IO: c_ulong = 0x2285;
    const SG_DXFER_NONE: c_int = -1;
    const SG_DXFER_FROM_DEV: c_int = -3;
    const O_NONBLOCK: i32 = 0o4000;

    /// `struct sg_io_hdr` (scsi/sg.h).
    #[repr(C)]
    struct SgIoHdr {
        interface_id: c_int,
        dxfer_direction: c_int,
        cmd_len: u8,
        mx_sb_len: u8,
        iovec_count: u16,
        dxfer_len: c_uint,
        dxferp: *mut c_void,
        cmdp: *const u8,
        sbp: *mut u8,
        timeout: c_uint,
        flags: c_uint,
        pack_id: c_int,
        usr_ptr: *mut c_void,
        status: u8,
        masked_status: u8,
        msg_status: u8,
        sb_len_wr: u8,
        host_status: u16,
        driver_status: u16,
        resid: c_int,
        duration: c_uint,
        info: c_uint,
    }

    pub struct SgIo(File);

    impl SgIo {
        /// Read-write when allowed (SET CD SPEED is a write-class command
        /// to some kernels), read-only otherwise; non-blocking, so a drive
        /// with its tray open still opens.
        pub fn open(dev: &str) -> Result<SgIo> {
            let open = |write: bool| {
                OpenOptions::new()
                    .read(true)
                    .write(write)
                    .custom_flags(O_NONBLOCK)
                    .open(dev)
            };
            open(true)
                .or_else(|_| open(false))
                .map(SgIo)
                .map_err(|e| Error::Invalid(format!("{dev}: {e}")))
        }
    }

    impl Scsi for SgIo {
        fn cmd(&self, cdb: &[u8], data: &mut [u8], timeout_s: u32) -> Result<usize> {
            let mut sense = [0u8; 32];
            let mut h = SgIoHdr {
                interface_id: b'S' as c_int,
                dxfer_direction: if data.is_empty() {
                    SG_DXFER_NONE
                } else {
                    SG_DXFER_FROM_DEV
                },
                cmd_len: cdb.len() as u8,
                mx_sb_len: sense.len() as u8,
                iovec_count: 0,
                dxfer_len: data.len() as c_uint,
                dxferp: data.as_mut_ptr() as *mut c_void,
                cmdp: cdb.as_ptr(),
                sbp: sense.as_mut_ptr(),
                timeout: timeout_s * 1000,
                flags: 0,
                pack_id: 0,
                usr_ptr: std::ptr::null_mut(),
                status: 0,
                masked_status: 0,
                msg_status: 0,
                sb_len_wr: 0,
                host_status: 0,
                driver_status: 0,
                resid: 0,
                duration: 0,
                info: 0,
            };
            let rc = unsafe { ioctl(self.0.as_raw_fd(), SG_IO, &mut h as *mut SgIoHdr) };
            if rc < 0 {
                return Err(Error::Io(format!(
                    "SG_IO: {}",
                    std::io::Error::last_os_error()
                )));
            }
            if h.info & 1 != 0 || h.status != 0 {
                let (key, asc, ascq) = if h.sb_len_wr >= 14 {
                    (sense[2] & 0x0F, sense[12], sense[13])
                } else {
                    (0, 0, 0)
                };
                return Err(Error::Io(format!(
                    "command {:#04x}: status {:#x}, sense {key:x}/{asc:02x}/{ascq:02x}",
                    cdb[0], h.status
                )));
            }
            Ok(data.len().saturating_sub(h.resid.max(0) as usize))
        }
    }
}

// ------------------------------------------------------------------ Windows

/// Windows: SPTI when this user may (an administrator, on most installs),
/// the CD-ROM IOCTLs otherwise. Declared by hand, as the rest of libdisc
/// has no dependencies; with `--cfg libdisc_wincheck` this module also
/// compiles (unlinked) on another host, which is how it is type-checked
/// away from a PC.
#[cfg(any(windows, libdisc_wincheck))]
pub(crate) mod win {
    use super::{Access, Scsi, ScsiDrive, Transport};
    use crate::{Error, Result};
    use std::ffi::c_void;

    type Handle = *mut c_void;
    const INVALID: Handle = -1isize as Handle;

    #[cfg_attr(windows, link(name = "kernel32"))]
    extern "system" {
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            sa: *const c_void,
            disposition: u32,
            flags: u32,
            template: Handle,
        ) -> Handle;
        fn CloseHandle(h: Handle) -> i32;
        fn DeviceIoControl(
            h: Handle,
            code: u32,
            inb: *const c_void,
            inlen: u32,
            outb: *mut c_void,
            outlen: u32,
            got: *mut u32,
            ov: *mut c_void,
        ) -> i32;
        fn ReadFile(h: Handle, buf: *mut c_void, len: u32, got: *mut u32, ov: *mut c_void) -> i32;
        fn SetFilePointerEx(h: Handle, to: i64, new: *mut i64, how: u32) -> i32;
        fn GetLastError() -> u32;
    }

    const GENERIC_READ: u32 = 0x8000_0000;
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const SHARE_RW: u32 = 3;
    const OPEN_EXISTING: u32 = 3;
    const IOCTL_SCSI_PASS_THROUGH_DIRECT: u32 = 0x4D014;
    const IOCTL_CDROM_READ_TOC_EX: u32 = 0x24054;
    const IOCTL_CDROM_RAW_READ: u32 = 0x2403E;
    const IOCTL_CDROM_SET_SPEED: u32 = 0x24060;
    const IOCTL_DISK_GET_LENGTH_INFO: u32 = 0x7405C;

    /// A device handle, closed with its owner.
    struct Dev(Handle, String);
    unsafe impl Send for Dev {}
    unsafe impl Sync for Dev {}
    impl Drop for Dev {
        fn drop(&mut self) {
            unsafe { CloseHandle(self.0) };
        }
    }

    impl Dev {
        fn open(path: &str, access: u32) -> Result<Dev> {
            let w: Vec<u16> = path.encode_utf16().chain(Some(0)).collect();
            let h = unsafe {
                CreateFileW(
                    w.as_ptr(),
                    access,
                    SHARE_RW,
                    std::ptr::null(),
                    OPEN_EXISTING,
                    0,
                    std::ptr::null_mut(),
                )
            };
            if h == INVALID || h.is_null() {
                return Err(Error::Invalid(format!(
                    "{path}: cannot open (Windows error {})",
                    unsafe { GetLastError() }
                )));
            }
            Ok(Dev(h, path.to_string()))
        }

        fn ioctl(&self, code: u32, inb: &[u8], out: &mut [u8]) -> Result<usize> {
            let mut got = 0u32;
            let ok = unsafe {
                DeviceIoControl(
                    self.0,
                    code,
                    inb.as_ptr() as *const c_void,
                    inb.len() as u32,
                    out.as_mut_ptr() as *mut c_void,
                    out.len() as u32,
                    &mut got,
                    std::ptr::null_mut(),
                )
            };
            if ok == 0 {
                return Err(Error::Io(format!(
                    "{}: IOCTL {code:#x}: Windows error {}",
                    self.1,
                    unsafe { GetLastError() }
                )));
            }
            Ok(got as usize)
        }
    }

    /// An aligned transfer buffer: devices want 2- or 4-byte alignment,
    /// some more; 16 covers every one.
    fn aligned(len: usize) -> Vec<u128> {
        vec![0u128; len.div_ceil(16).max(1)]
    }
    fn bytes(v: &mut [u128]) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(v.as_mut_ptr() as *mut u8, v.len() * 16) }
    }

    /// `SCSI_PASS_THROUGH_DIRECT` with its sense buffer after it.
    #[repr(C)]
    struct Sptd {
        length: u16,
        scsi_status: u8,
        path_id: u8,
        target_id: u8,
        lun: u8,
        cdb_length: u8,
        sense_info_length: u8,
        data_in: u8,
        data_transfer_length: u32,
        timeout_value: u32,
        data_buffer: *mut c_void,
        sense_info_offset: u32,
        cdb: [u8; 16],
    }
    #[repr(C)]
    struct SptdSense {
        sptd: Sptd,
        sense: [u8; 32],
    }

    struct Spti(Dev);

    impl Scsi for Spti {
        fn cmd(&self, cdb: &[u8], data: &mut [u8], timeout_s: u32) -> Result<usize> {
            let mut buf = aligned(data.len());
            let mut p = SptdSense {
                sptd: Sptd {
                    length: std::mem::size_of::<Sptd>() as u16,
                    scsi_status: 0,
                    path_id: 0,
                    target_id: 0,
                    lun: 0,
                    cdb_length: cdb.len() as u8,
                    sense_info_length: 32,
                    data_in: if data.is_empty() { 2 } else { 1 }, // UNSPECIFIED / DATA_IN
                    data_transfer_length: data.len() as u32,
                    timeout_value: timeout_s,
                    data_buffer: if data.is_empty() {
                        std::ptr::null_mut()
                    } else {
                        buf.as_mut_ptr() as *mut c_void
                    },
                    sense_info_offset: std::mem::offset_of!(SptdSense, sense) as u32,
                    cdb: [0; 16],
                },
                sense: [0; 32],
            };
            p.sptd.cdb[..cdb.len()].copy_from_slice(cdb);
            let size = std::mem::size_of::<SptdSense>();
            let raw = unsafe {
                std::slice::from_raw_parts_mut(&mut p as *mut SptdSense as *mut u8, size)
            };
            let mut got = 0u32;
            let ok = unsafe {
                DeviceIoControl(
                    self.0 .0,
                    IOCTL_SCSI_PASS_THROUGH_DIRECT,
                    raw.as_ptr() as *const c_void,
                    size as u32,
                    raw.as_mut_ptr() as *mut c_void,
                    size as u32,
                    &mut got,
                    std::ptr::null_mut(),
                )
            };
            if ok == 0 {
                return Err(Error::Io(format!(
                    "{}: SCSI pass-through: Windows error {}",
                    self.0 .1,
                    unsafe { GetLastError() }
                )));
            }
            if p.sptd.scsi_status != 0 {
                return Err(Error::Io(format!(
                    "command {:#04x}: status {:#x}, sense {:x}/{:02x}/{:02x}",
                    cdb[0],
                    p.sptd.scsi_status,
                    p.sense[2] & 0x0F,
                    p.sense[12],
                    p.sense[13]
                )));
            }
            let n = (p.sptd.data_transfer_length as usize).min(data.len());
            data[..n].copy_from_slice(&bytes(&mut buf)[..n]);
            Ok(n)
        }
    }

    /// SPTI when it answers INQUIRY, the IOCTLs otherwise.
    pub(super) fn open(dev: &str) -> Result<Box<dyn Transport>> {
        if let Ok(d) = Dev::open(dev, GENERIC_READ | GENERIC_WRITE) {
            let s = Spti(d);
            if s.cmd(&[0x12, 0, 0, 0, 36, 0], &mut [0u8; 36], 10).is_ok() {
                return Ok(Box::new(ScsiDrive(s)));
            }
        }
        Ok(Box::new(Ioctl(Dev::open(dev, GENERIC_READ)?)))
    }

    /// The CD-ROM class driver's own IOCTLs: no administrator needed.
    struct Ioctl(Dev);

    impl Transport for Ioctl {
        fn access(&self) -> Access {
            Access::Basic
        }

        fn toc(&self, format: u8) -> Result<Vec<u8>> {
            // CDROM_READ_TOC_EX: Format in the low nibble, Msf in bit 7
            // (the full TOC is MSF whatever this says; some drivers want it
            // set for it), SessionTrack 1
            let mut out = vec![0u8; 4096];
            let tries: &[u8] = if format == 2 { &[0x82, 0x02] } else { &[0x00] };
            let mut last = None;
            for &b0 in tries {
                match self
                    .0
                    .ioctl(IOCTL_CDROM_READ_TOC_EX, &[b0, 1, 0, 0], &mut out)
                {
                    Ok(n) if n >= 4 => {
                        let want = (u16::from_be_bytes([out[0], out[1]]) as usize + 2).min(n);
                        out.truncate(want);
                        return Ok(out);
                    }
                    Ok(_) => last = Some(Error::Invalid("READ TOC: short reply".into())),
                    Err(e) => last = Some(e),
                }
            }
            Err(last.unwrap())
        }

        fn capacity(&self) -> Result<u32> {
            let mut out = [0u8; 8];
            self.0.ioctl(IOCTL_DISK_GET_LENGTH_INFO, &[], &mut out)?;
            Ok((i64::from_le_bytes(out) / 2048) as u32)
        }

        fn read_cd(&self, lba: i32, n: u32, sub: bool, out: &mut [u8]) -> Result<()> {
            if sub {
                return Err(Error::Mode);
            }
            // RAW_READ_INFO: DiskOffset (bytes, as if cooked), SectorCount, TrackMode CDDA
            let mut info = [0u8; 16];
            info[..8].copy_from_slice(&(lba as i64 * 2048).to_le_bytes());
            info[8..12].copy_from_slice(&n.to_le_bytes());
            info[12..16].copy_from_slice(&2i32.to_le_bytes());
            let want = n as usize * 2352;
            let mut buf = aligned(want);
            let got = self
                .0
                .ioctl(IOCTL_CDROM_RAW_READ, &info, &mut bytes(&mut buf)[..want])?;
            if got < want {
                return Err(Error::Medium);
            }
            out[..want].copy_from_slice(&bytes(&mut buf)[..want]);
            Ok(())
        }

        fn read_data(&self, lba: i32, n: u32, out: &mut [u8]) -> Result<()> {
            let want = n as usize * 2048;
            let mut buf = aligned(want);
            let mut got = 0u32;
            let ok = unsafe {
                SetFilePointerEx(self.0 .0, lba as i64 * 2048, std::ptr::null_mut(), 0) != 0
                    && ReadFile(
                        self.0 .0,
                        buf.as_mut_ptr() as *mut c_void,
                        want as u32,
                        &mut got,
                        std::ptr::null_mut(),
                    ) != 0
            };
            if !ok || (got as usize) < want {
                return Err(Error::Medium);
            }
            out[..want].copy_from_slice(&bytes(&mut buf)[..want]);
            Ok(())
        }

        fn set_speed(&self, kbps: u16) -> Result<()> {
            // CDROM_SET_SPEED: RequestType CdromSetSpeed, ReadSpeed,
            // WriteSpeed, RotationControl CdromDefaultRotation
            let mut req = [0u8; 12];
            req[4..6].copy_from_slice(&kbps.to_le_bytes());
            req[6..8].copy_from_slice(&0xFFFFu16.to_le_bytes());
            self.0
                .ioctl(IOCTL_CDROM_SET_SPEED, &req, &mut [])
                .map(|_| ())
        }

        fn max_speed(&self) -> Option<u16> {
            None
        }
    }
}
