//! A host's own optical drive as the disc (`hostcd:/dev/sr0`, track M5h,
//! doc 17 §9).
//!
//! The model is built from the drive's TOC when the disc is opened, like
//! a CloneCD image's from its `[Entry]` records, and every sector is read
//! from the drive when the guest asks for it, through SCSI pass-through:
//! `READ CD` for a CD's raw 2352 bytes and its raw P-W subchannel, `READ
//! (10)` for a DVD's 2048. So CD audio plays, raw reads return what is on
//! the disc, and a sector the drive cannot read fails as the drive failed
//! it, at its LBA, with no annotation anywhere: the real drive *is* the
//! drive model the images need EDC/ECC checks for.
//!
//! A disc in the drive is a snapshot, like an image: the opener reads the
//! TOC once. A disc taken out and another put in is a new `Disc`; the
//! QEMU driver watches the drive (`ready`) and swaps them (doc 17 §9).
//!
//! Linux only for now (`SG_IO`). Windows (`IOCTL_SCSI_PASS_THROUGH_DIRECT`
//! on `\\.\D:`) and macOS (IOKit's SCSITask, which needs the disc
//! unmounted) answer `Error::Invalid` until they are written.

use std::sync::Mutex;

use crate::{msf, Disc, Error, Extent, Result, Session, Source, TocEntry, Track, TrackMode};

/// The prefix a host drive is named with, everywhere a medium is a string.
pub const PREFIX: &str = "hostcd:";

/// Sectors one READ CD asks for: 75 KiB of audio, a guest's 64 KiB read
/// in one command. Every command is a USB round trip on a portable drive.
const READ_AHEAD: u32 = 32;

/// A sector read's timeout. A weak sector makes a drive retry for seconds
/// before it gives up, and that failure is the answer the guest wants.
const READ_TIMEOUT_MS: u32 = 60_000;

/// The result of one command that did not succeed.
#[derive(Debug, Clone, Copy)]
pub(crate) struct Sense {
    pub key: u8,
    pub asc: u8,
    pub ascq: u8,
}

impl Sense {
    fn error(self, what: &str) -> Error {
        match self.key {
            /* MEDIUM ERROR / HARDWARE ERROR: the sector, as the drive saw it. */
            0x3 | 0x4 => Error::Medium,
            /* NOT READY / UNIT ATTENTION: the disc went away or changed
               under us. The disc this model describes is not there. */
            0x2 | 0x6 => Error::Medium,
            /* ILLEGAL REQUEST on a sector: an LBA past the readable area
               (the run-out of a TAO track). */
            0x5 if self.asc == 0x21 || self.asc == 0x64 => Error::Medium,
            _ => Error::Io(format!("{what}: sense {:x}/{:02x}/{:02x}", self.key, self.asc, self.ascq)),
        }
    }
}

#[derive(Debug)]
pub(crate) enum CmdError {
    /// The drive answered CHECK CONDITION.
    Sense(Sense),
    /// The host could not send the command (no permission, gone).
    Host(String),
}

impl CmdError {
    fn error(self, what: &str) -> Error {
        match self {
            CmdError::Sense(s) => s.error(what),
            CmdError::Host(m) => Error::Io(format!("{what}: {m}")),
        }
    }
}

#[cfg(target_os = "linux")]
mod sys {
    use super::{CmdError, Sense};
    use std::fs::File;
    use std::os::fd::AsRawFd;
    use std::os::unix::fs::OpenOptionsExt;

    #[repr(C)]
    struct SgIoHdr {
        interface_id: i32,
        dxfer_direction: i32,
        cmd_len: u8,
        mx_sb_len: u8,
        iovec_count: u16,
        dxfer_len: u32,
        dxferp: *mut u8,
        cmdp: *const u8,
        sbp: *mut u8,
        timeout: u32,
        flags: u32,
        pack_id: i32,
        usr_ptr: *mut u8,
        status: u8,
        masked_status: u8,
        msg_status: u8,
        sb_len_wr: u8,
        host_status: u16,
        driver_status: u16,
        resid: i32,
        duration: u32,
        info: u32,
    }

    const SG_IO: std::ffi::c_ulong = 0x2285;
    const SG_DXFER_NONE: i32 = -1;
    const SG_DXFER_FROM_DEV: i32 = -3;
    const O_NONBLOCK: i32 = 0o4000;
    const CDROMEJECT: std::ffi::c_ulong = 0x5309;

    extern "C" {
        fn ioctl(fd: i32, request: std::ffi::c_ulong, ...) -> i32;
    }

    pub struct Handle(File);

    impl Handle {
        /// O_NONBLOCK: an empty drive opens instead of failing with ENOMEDIUM.
        pub fn open(path: &str) -> Result<Handle, String> {
            std::fs::OpenOptions::new()
                .read(true)
                .custom_flags(O_NONBLOCK)
                .open(path)
                .map(Handle)
                .map_err(|e| format!("{path}: {e}"))
        }

        pub fn command(&self, cdb: &[u8], buf: &mut [u8], timeout_ms: u32) -> Result<(), CmdError> {
            let mut sense = [0u8; 32];
            let mut h = SgIoHdr {
                interface_id: b'S' as i32,
                dxfer_direction: if buf.is_empty() { SG_DXFER_NONE } else { SG_DXFER_FROM_DEV },
                cmd_len: cdb.len() as u8,
                mx_sb_len: sense.len() as u8,
                iovec_count: 0,
                dxfer_len: buf.len() as u32,
                dxferp: buf.as_mut_ptr(),
                cmdp: cdb.as_ptr(),
                sbp: sense.as_mut_ptr(),
                timeout: timeout_ms,
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
            // SAFETY: `h` points at live buffers of the lengths it states for the call.
            if unsafe { ioctl(self.0.as_raw_fd(), SG_IO, &mut h as *mut SgIoHdr) } < 0 {
                return Err(CmdError::Host(std::io::Error::last_os_error().to_string()));
            }
            if h.status == 0 && h.host_status == 0 && (h.driver_status & 0xf) == 0 {
                return Ok(());
            }
            if h.sb_len_wr >= 3 {
                /* fixed (0x70/0x71) or descriptor (0x72/0x73) format */
                let s = if sense[0] & 0x7f >= 0x72 {
                    Sense { key: sense[1] & 0xf, asc: sense[2], ascq: sense[3] }
                } else {
                    Sense { key: sense[2] & 0xf, asc: sense[12], ascq: sense[13] }
                };
                return Err(CmdError::Sense(s));
            }
            Err(CmdError::Host(format!("status {:#x}, host {:#x}, driver {:#x}", h.status, h.host_status, h.driver_status)))
        }

        pub fn eject(&self) -> Result<(), String> {
            // SAFETY: CDROMEJECT takes no argument.
            if unsafe { ioctl(self.0.as_raw_fd(), CDROMEJECT, 0usize) } < 0 {
                return Err(std::io::Error::last_os_error().to_string());
            }
            Ok(())
        }
    }
}

#[cfg(not(target_os = "linux"))]
mod sys {
    use super::CmdError;

    pub struct Handle;

    impl Handle {
        pub fn open(path: &str) -> Result<Handle, String> {
            Err(format!("{path}: a host drive is not supported on this system yet"))
        }
        pub fn command(&self, _cdb: &[u8], _buf: &mut [u8], _timeout_ms: u32) -> Result<(), CmdError> {
            Err(CmdError::Host("not supported".into()))
        }
        pub fn eject(&self) -> Result<(), String> {
            Err("not supported".into())
        }
    }
}

/// Sectors read ahead, all from one track: `first..first + count`.
struct Window {
    first: i32,
    count: u32,
    stride: usize,
    data: Vec<u8>,
}

impl Window {
    fn get(&self, lba: i32) -> Option<&[u8]> {
        if lba >= self.first && lba < self.first + self.count as i32 {
            let at = (lba - self.first) as usize * self.stride;
            Some(&self.data[at..at + self.stride])
        } else {
            None
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Sub {
    /// READ CD's raw P-W (96 bytes, interleaved as on the disc).
    Raw,
    /// Formatted Q only (16 bytes: the 12 of the Q frame, CRC included).
    /// P and R-W are synthesized from the TOC.
    Q,
    /// Neither: all of it synthesized.
    None,
}

/// The drive behind a disc: the handle, what it can do, and the sectors read ahead.
pub struct Drive {
    handle: sys::Handle,
    /// What READ CD gives of the subchannel on this drive.
    sub: Sub,
    sectors: Mutex<Option<Window>>,
    subs: Mutex<Option<Window>>,
}

impl std::fmt::Debug for Drive {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Drive").field("sub", &self.sub).finish()
    }
}

fn cdb_lba(cdb: &mut [u8], at: usize, lba: i32) {
    cdb[at..at + 4].copy_from_slice(&(lba as u32).to_be_bytes());
}

/// READ CD: `count` sectors from `lba`, each `stride` bytes. `audio` asks
/// for CD-DA (2352 bytes of samples); otherwise any sector type with sync,
/// headers, user data and EDC/ECC (the raw 2352). `sub` = 1 appends raw P-W.
fn read_cd_cdb(lba: i32, count: u32, audio: bool, sub: u8) -> [u8; 12] {
    let mut cdb = [0u8; 12];
    cdb[0] = 0xBE;
    cdb[1] = if audio { 1 << 2 } else { 0 };
    cdb_lba(&mut cdb, 2, lba);
    cdb[6..9].copy_from_slice(&count.to_be_bytes()[1..]);
    cdb[9] = if audio { 0x10 } else { 0xF8 };
    cdb[10] = sub;
    cdb
}

impl Drive {
    fn read_window(&self, lba: i32, count: u32, audio: bool, sub: bool, cooked: bool) -> std::result::Result<Window, CmdError> {
        let sub_len = match self.sub { Sub::Raw => 96, _ => 16 };
        let stride = if sub { sub_len } else if cooked { 2048 } else { 2352 };
        let mut data = vec![0u8; count as usize * if sub { 2352 + sub_len } else { stride }];
        if cooked {
            let mut cdb = [0u8; 10];
            cdb[0] = 0x28;
            cdb_lba(&mut cdb, 2, lba);
            cdb[7..9].copy_from_slice(&(count as u16).to_be_bytes());
            self.handle.command(&cdb, &mut data, READ_TIMEOUT_MS)?;
        } else {
            let code = if !sub { 0 } else if self.sub == Sub::Raw { 1 } else { 2 };
            self.handle.command(&read_cd_cdb(lba, count, audio, code), &mut data, READ_TIMEOUT_MS)?;
        }
        if sub {
            /* keep only the subchannel bytes after each sector */
            let from = 2352 + sub_len;
            let mut subs = vec![0u8; count as usize * sub_len];
            for i in 0..count as usize {
                subs[i * sub_len..(i + 1) * sub_len].copy_from_slice(&data[i * from + 2352..(i + 1) * from]);
            }
            data = subs;
        }
        Ok(Window { first: lba, count, stride, data })
    }

    /// One sector of track `t` into `out` (`stride` bytes), through the read-ahead window.
    /// A run that fails is retried as the one sector, so a bad sector fails alone, at its LBA.
    fn read(&self, t: &Track, lba: i32, out: &mut [u8], sub: bool, cooked: bool) -> Result<()> {
        let cache = if sub { &self.subs } else { &self.sectors };
        let mut win = cache.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(s) = win.as_ref().and_then(|w| w.get(lba)) {
            out.copy_from_slice(s);
            return Ok(());
        }
        let audio = t.mode == TrackMode::Audio;
        let count = READ_AHEAD.min((t.end_lba - lba) as u32).max(1);
        let w = match self.read_window(lba, count, audio, sub, cooked) {
            Ok(w) => w,
            Err(_) if count > 1 => self.read_window(lba, 1, audio, sub, cooked).map_err(|e| e.error(&format!("sector {lba}")))?,
            Err(e) => return Err(e.error(&format!("sector {lba}"))),
        };
        out.copy_from_slice(w.get(lba).unwrap());
        *win = Some(w);
        Ok(())
    }

    pub(crate) fn read_raw(&self, t: &Track, lba: i32, out: &mut [u8; 2352]) -> Result<()> {
        self.read(t, lba, out, false, false)
    }

    pub(crate) fn read_cooked(&self, t: &Track, lba: i32, out: &mut [u8; 2048]) -> Result<()> {
        self.read(t, lba, out, false, true)
    }

    /// The subchannel of `lba` into `out` (deinterleaved P..W, as `Disc::read_sub`
    /// gives it) from a drive that returns raw P-W; `None` from any other, whose
    /// subchannel is synthesized from the TOC. A drive's formatted Q is not
    /// served: this project's first drive returns the next frame's Q, and some
    /// frames with bits flipped under a CRC that does not match them, where
    /// Q synthesized from the TOC is what a good drive reads. It does find
    /// pregaps (`find_pregap`), by the address in each frame.
    pub(crate) fn read_sub(&self, t: &Track, lba: i32, out: &mut [u8; 96]) -> Option<Result<()>> {
        if self.sub != Sub::Raw {
            return None;
        }
        let mut inter = [0u8; 96];
        Some(self.read(t, lba, &mut inter, true, false).map(|()| crate::subq::deinterleave(&inter, out)))
    }

    /// Open the tray (QEMU's eject, the guest's START STOP UNIT).
    pub fn eject(&self) -> Result<()> {
        self.handle.eject().map_err(Error::Io)
    }
}

/// Is there a readable disc in the drive at `path` (TEST UNIT READY)?
/// `Err` when the drive cannot be opened or asked at all. The QEMU
/// driver's watch calls it once a second.
pub fn ready(path: &str) -> Result<bool> {
    let h = sys::Handle::open(path).map_err(Error::Io)?;
    match h.command(&[0u8; 6], &mut [], 10_000) {
        Ok(()) => Ok(true),
        Err(CmdError::Sense(_)) => Ok(false),
        Err(CmdError::Host(m)) => Err(Error::Io(format!("{path}: {m}"))),
    }
}

/// Open the tray of the drive at `path`.
pub fn eject(path: &str) -> Result<()> {
    sys::Handle::open(path).map_err(Error::Io)?.eject().map_err(Error::Io)
}

/// The drive's current profile (GET CONFIGURATION): 0x08-0x0A a CD, 0x10 and up a DVD or above.
fn current_profile(h: &sys::Handle) -> Option<u16> {
    let mut buf = [0u8; 8];
    let mut cdb = [0u8; 10];
    cdb[0] = 0x46;
    cdb[1] = 0x01;
    cdb[8] = 8;
    h.command(&cdb, &mut buf, 10_000).ok()?;
    Some(u16::from_be_bytes([buf[6], buf[7]]))
}

fn read_capacity(h: &sys::Handle) -> Result<u32> {
    let mut buf = [0u8; 8];
    let mut cdb = [0u8; 10];
    cdb[0] = 0x25;
    h.command(&cdb, &mut buf, 10_000).map_err(|e| e.error("READ CAPACITY"))?;
    Ok(u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]).wrapping_add(1))
}

/// READ TOC format 2: the raw Q entries of every session, binary MSF.
fn full_toc(h: &sys::Handle) -> std::result::Result<Vec<TocEntry>, CmdError> {
    let mut buf = vec![0u8; 4 + 11 * 300];
    let mut cdb = [0u8; 10];
    cdb[0] = 0x43;
    cdb[1] = 0x02; /* MSF */
    cdb[2] = 0x02; /* format: full TOC */
    cdb[6] = 1; /* from session 1 */
    cdb[7..9].copy_from_slice(&(buf.len() as u16).to_be_bytes());
    h.command(&cdb, &mut buf, 30_000)?;
    let len = (u16::from_be_bytes([buf[0], buf[1]]) as usize + 2).min(buf.len());
    Ok(buf[4..len]
        .chunks_exact(11)
        .map(|d| TocEntry {
            session: d[0],
            adr: d[1] >> 4,
            control: d[1] & 0xf,
            point: d[3],
            min: d[4],
            sec: d[5],
            frame: d[6],
            zero: d[7],
            pmin: d[8],
            psec: d[9],
            pframe: d[10],
        })
        .collect())
}

fn entry_lba(e: &TocEntry) -> i32 {
    msf::Msf { m: e.pmin, s: e.psec, f: e.pframe }.to_lba()
}

/// Open the disc in the drive at `path` (`hostcd:` already stripped).
pub fn open(path: &str) -> Result<Disc> {
    let handle = sys::Handle::open(path).map_err(Error::Invalid)?;
    match handle.command(&[0u8; 6], &mut [], 30_000) {
        Ok(()) => {}
        Err(CmdError::Sense(s)) if s.key == 0x6 => {
            /* the change that made it ready: ask once more */
            handle.command(&[0u8; 6], &mut [], 30_000).map_err(|_| Error::Invalid(format!("{path}: no disc in the drive")))?;
        }
        Err(CmdError::Sense(_)) => return Err(Error::Invalid(format!("{path}: no disc in the drive"))),
        Err(CmdError::Host(m)) => return Err(Error::Invalid(format!("{path}: {m}"))),
    }
    let mut disc = Disc::new();
    if current_profile(&handle).is_some_and(|p| p >= 0x10) {
        /* A DVD or above: one data track of 2048-byte sectors. READ CD is a CD's command. */
        let count = read_capacity(&handle)?;
        if count == 0 || count as u64 > crate::isodir::MAX_SECTORS {
            return Err(Error::Invalid(format!("{path}: {count} sectors is not a disc libdisc models")));
        }
        let end = count as i32;
        disc.sessions.push(Session {
            number: 1,
            tracks: vec![Track {
                number: 1,
                mode: TrackMode::Mode1,
                control: 0x4,
                isrc: None,
                indices: vec![(1, 0)],
                start_lba: 0,
                end_lba: end,
                extents: vec![Extent { lba: 0, count, source: Source::Drive { raw: false }, sub: None }],
            }],
            leadout_lba: end,
        });
        disc.drive = Some(Drive { handle, sub: Sub::None, sectors: Mutex::new(None), subs: Mutex::new(None) });
        return Ok(disc);
    }

    let entries = full_toc(&handle).map_err(|e| e.error(&format!("{path}: READ TOC")))?;
    let mut drive = Drive { handle, sub: Sub::None, sectors: Mutex::new(None), subs: Mutex::new(None) };
    probe_raw_sub(&mut drive, &entries);
    let mut sessions: Vec<Session> = Vec::new();
    let mut session_numbers: Vec<u8> = entries.iter().map(|e| e.session).collect();
    session_numbers.sort_unstable();
    session_numbers.dedup();
    for &sn in &session_numbers {
        let in_session = |e: &&TocEntry| e.session == sn && e.adr == 1;
        let leadout = entries
            .iter()
            .filter(in_session)
            .find(|e| e.point == 0xA2)
            .map(entry_lba)
            .ok_or_else(|| Error::Invalid(format!("{path}: session {sn} has no lead-out in the TOC")))?;
        let mut points: Vec<&TocEntry> = entries.iter().filter(in_session).filter(|e| (1..=99).contains(&e.point)).collect();
        points.sort_by_key(|e| e.point);
        let mut tracks: Vec<Track> = Vec::new();
        for (i, e) in points.iter().enumerate() {
            let start = entry_lba(e);
            let end = points.get(i + 1).map(|n| entry_lba(n)).unwrap_or(leadout);
            let first = if sessions.is_empty() && i == 0 { 0 } else { start };
            if end <= start || first < sessions.last().map(|s| s.leadout_lba).unwrap_or(0) {
                return Err(Error::Invalid(format!("{path}: track {} spans {start} to {end}", e.point)));
            }
            let mut indices = vec![(1, start)];
            if first < start {
                indices.insert(0, (0, first));
            }
            let mut t = Track {
                number: e.point,
                mode: TrackMode::Audio,
                control: e.control,
                isrc: None,
                indices,
                start_lba: start,
                end_lba: end,
                extents: vec![Extent { lba: first, count: (end - first) as u32, source: Source::Drive { raw: true }, sub: None }],
            };
            if e.control & 0x4 != 0 {
                /* a data track's mode is in its sectors' headers, not the TOC */
                t.mode = TrackMode::Mode1;
                let mut raw = [0u8; 2352];
                if drive.read_raw(&t, start, &mut raw).is_ok() && raw[15] == 2 {
                    t.mode = TrackMode::Mode2;
                }
            }
            tracks.push(t);
        }
        if tracks.is_empty() {
            return Err(Error::Invalid(format!("{path}: session {sn} has no tracks")));
        }
        /* The TOC has index 1 only. A pregap (index 0) is in the Q subchannel
           before the track: move it from the track before into this one. Only
           where data meets audio, where the Red Book requires one: every
           search seeks the drive (a second each on a portable one), and
           between audio tracks a gap only changes which track its sound is
           counted in. */
        for i in 1..tracks.len() {
            if (tracks[i - 1].control ^ tracks[i].control) & 0x4 == 0 {
                continue;
            }
            let found = find_pregap(&drive, &tracks[i - 1], tracks[i].number, tracks[i].start_lba);
            if let Some(p) = found {
                let (a, b) = tracks.split_at_mut(i);
                let (prev, t) = (&mut a[i - 1], &mut b[0]);
                prev.end_lba = p;
                prev.extents[0].count = (p - prev.extents[0].lba) as u32;
                t.indices.insert(0, (0, p));
                t.extents[0] = Extent { lba: p, count: (t.end_lba - p) as u32, ..t.extents[0] };
            }
        }
        sessions.push(Session { number: sn, tracks, leadout_lba: leadout });
    }
    if sessions.is_empty() {
        return Err(Error::Invalid(format!("{path}: the TOC lists no session")));
    }
    *drive.subs.get_mut().unwrap() = None;
    *drive.sectors.get_mut().unwrap() = None;
    disc.sessions = sessions;
    disc.raw_toc = Some(entries);
    disc.drive = Some(drive);
    Ok(disc)
}

/// Raw P-W if this drive returns it for track 1 with a good Q CRC, else formatted Q, else none.
fn probe_raw_sub(drive: &mut Drive, entries: &[TocEntry]) {
    let Some(e) = entries.iter().find(|e| e.adr == 1 && e.point == 1) else { return };
    let start = entry_lba(e);
    let t = Track {
        number: 1,
        mode: if e.control & 0x4 != 0 { TrackMode::Mode1 } else { TrackMode::Audio },
        control: e.control,
        isrc: None,
        indices: vec![(1, start)],
        start_lba: start,
        end_lba: start + 1,
        extents: Vec::new(),
    };
    for sub in [Sub::Raw, Sub::Q] {
        drive.sub = sub;
        let ok = q_at(drive, &t, start).is_some();
        *drive.subs.get_mut().unwrap() = None;
        if ok {
            return;
        }
    }
    drive.sub = Sub::None;
}

/// (track, index, absolute LBA) from the Q subchannel read at `lba` as part of
/// `t`; `None` for a frame whose CRC fails or that carries no position (ADR 1).
/// The address is the frame's own: a drive's Q is not always the frame of the
/// sector asked for (this project's first drive returns the next one's).
fn q_at(drive: &Drive, t: &Track, lba: i32) -> Option<(u8, u8, i32)> {
    /* One sector, past the read-ahead: a run that crosses from data into an
       audio pregap fails whole, after the drive has retried it for seconds. */
    let w = drive.read_window(lba, 1, t.mode == TrackMode::Audio, true, false).ok()?;
    let mut de = [0u8; 96];
    match drive.sub {
        Sub::None => return None,
        Sub::Raw => crate::subq::deinterleave(w.data[..96].try_into().unwrap(), &mut de),
        Sub::Q => de[12..24].copy_from_slice(&w.data[..12]),
    }
    let q = &de[12..24];
    if !crate::subq::q_crc_ok(q) || q[0] & 0xf != 1 {
        return None;
    }
    let b = crate::sector::from_bcd;
    let abs = msf::Msf { m: b(q[7]), s: b(q[8]), f: b(q[9]) }.to_lba();
    Some((b(q[1]), b(q[2]), abs))
}

/// The first sector of track `n`'s index 0, searched in the 300 sectors
/// (4 s) before its index 1, or `None` when it has no pregap there or the
/// drive gives no subchannel.
fn find_pregap(drive: &Drive, prev: &Track, n: u8, start: i32) -> Option<i32> {
    let in_gap = |q: Option<(u8, u8, i32)>| q.is_some_and(|(t, i, _)| t == n && i == 0);
    let at_end = q_at(drive, prev, start - 2);
    if drive.sub == Sub::None || !in_gap(at_end) {
        return None;
    }
    /* reads from `lo` are before the pregap, the one at `hi` in it: find the edge */
    let (mut lo, mut hi, mut first) = ((start - 300).max(prev.start_lba), start - 2, at_end?.2);
    let q = q_at(drive, prev, lo);
    if in_gap(q) {
        return Some(q?.2);
    }
    while hi - lo > 1 {
        let mid = lo + (hi - lo) / 2;
        let q = q_at(drive, prev, mid);
        if in_gap(q) {
            hi = mid;
            first = q?.2;
        } else {
            lo = mid;
        }
    }
    /* a frame read at `lo` that is already in the gap moves the edge back */
    match q_at(drive, prev, lo) {
        Some((t, 0, abs)) if t == n => first = first.min(abs),
        _ => {}
    }
    (first > prev.start_lba && first < start).then_some(first)
}
