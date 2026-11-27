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

use std::collections::VecDeque;
use std::sync::{mpsc, Arc, Mutex};

use crate::{msf, Disc, Error, Extent, Result, Session, Source, TocEntry, Track, TrackMode};

/// The prefix a host drive is named with, everywhere a medium is a string.
pub const PREFIX: &str = "hostcd:";

/// Sectors one read asks for on a data track: a guest's 64 KiB read in one
/// command. Every command is a USB round trip on a portable drive.
const READ_AHEAD: u32 = 32;

/// Sectors one read asks for on an audio track: two seconds of CD audio.
/// Music is read at 75 sectors a second, and each read can cost a seek back
/// from the game's data track (0.2 s on the first drive) and a change of
/// spin speed (2.7 s). `read_window` splits it into commands the host takes.
const AUDIO_AHEAD: u32 = 150;

/// Windows the sector cache keeps, most recently used first: the music's
/// and the game's reads must not evict each other (each eviction is a seek
/// back, seconds on a slim USB drive, which froze the VM with the music).
const CACHE_WINDOWS: usize = 16;

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
    const BLKSECTGET: std::ffi::c_ulong = 0x1267;

    extern "C" {
        fn ioctl(fd: i32, request: std::ffi::c_ulong, ...) -> i32;
    }

    pub struct Handle(File);

    impl Handle {
        /// O_NONBLOCK: an empty drive opens instead of failing with
        /// ENOMEDIUM. Read-write when this user may: the kernel passes
        /// SET CD SPEED only on a writable handle (EPERM on a read-only
        /// one; learnt from PR #4). Nothing is ever written to the disc.
        pub fn open(path: &str) -> Result<Handle, String> {
            let open = |write: bool| {
                std::fs::OpenOptions::new().read(true).write(write).custom_flags(O_NONBLOCK).open(path)
            };
            open(true).or_else(|_| open(false)).map(Handle).map_err(|e| format!("{path}: {e}"))
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

        /// The most bytes one command may move: the block queue's
        /// `max_sectors`, 120 KiB on the first drive (USB). A larger
        /// transfer fails with EIO before it reaches the drive.
        pub fn max_transfer(&self) -> usize {
            let mut sectors: u16 = 0;
            // SAFETY: BLKSECTGET writes one unsigned short.
            if unsafe { ioctl(self.0.as_raw_fd(), BLKSECTGET, &mut sectors as *mut u16) } < 0 || sectors == 0 {
                return 64 * 1024;
            }
            sectors as usize * 512
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
        pub fn max_transfer(&self) -> usize {
            64 * 1024
        }
        pub fn eject(&self) -> Result<(), String> {
            Err("not supported".into())
        }
    }
}

/// The drive a disc is read from: the host's (`sys`), or a simulated one
/// answering the same commands from an image (`sim:<image>`).
pub(crate) enum Dev {
    Os(sys::Handle),
    Sim(sim::Sim),
}

impl Dev {
    fn open(path: &str) -> std::result::Result<Dev, String> {
        match path.strip_prefix("sim:") {
            Some(image) => Ok(Dev::Sim(sim::Sim::open(image))),
            None => sys::Handle::open(path).map(Dev::Os),
        }
    }

    fn command(&self, cdb: &[u8], buf: &mut [u8], timeout_ms: u32) -> std::result::Result<(), CmdError> {
        match self {
            Dev::Os(h) => h.command(cdb, buf, timeout_ms),
            Dev::Sim(s) => s.command(cdb, buf),
        }
    }

    fn max_transfer(&self) -> usize {
        match self {
            Dev::Os(h) => h.max_transfer(),
            Dev::Sim(_) => sim::MAX_TRANSFER,
        }
    }

    fn eject(&self) -> std::result::Result<(), String> {
        match self {
            Dev::Os(h) => h.eject(),
            Dev::Sim(_) => Ok(()),
        }
    }
}

/// A simulated drive (`hostcd:sim:<image>`, learnt from PR #4): the SCSI
/// commands `Drive` sends, answered by `mmc` from an image, so the whole
/// path (TOC parsing, raw reads in runs split at the transfer limit, the
/// pregap search, the read-ahead worker, and in QEMU the watch's swaps)
/// runs on a host with no drive. The disc is in the drive while the image
/// file exists: renaming it away is taking the disc out, and back is
/// putting it in (the `hostcd-sim` check).
mod sim {
    use super::{CmdError, Sense};
    use crate::{mmc, Disc, Error};
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Mutex};

    /// What one command may move: a USB drive's 120 KiB, a little less,
    /// so long runs are split as on the first real drive.
    pub const MAX_TRANSFER: usize = 64 * 1024;

    pub struct Sim {
        image: PathBuf,
        disc: Mutex<Option<Arc<Disc>>>,
    }

    fn sense(key: u8, asc: u8, ascq: u8) -> CmdError {
        CmdError::Sense(Sense { key, asc, ascq })
    }

    fn error(e: Error) -> CmdError {
        match e {
            Error::Range => sense(5, 0x21, 0),
            Error::Medium => sense(3, 0x11, 5),
            Error::Mode => sense(5, 0x64, 0),
            Error::Invalid(_) => sense(5, 0x24, 0),
            Error::Io(_) => sense(4, 0x08, 0),
        }
    }

    fn put(buf: &mut [u8], v: &[u8]) {
        let n = v.len().min(buf.len());
        buf[..n].copy_from_slice(&v[..n]);
    }

    fn be32(b: &[u8]) -> u32 {
        u32::from_be_bytes([b[0], b[1], b[2], b[3]])
    }

    impl Sim {
        pub fn open(image: &str) -> Sim {
            Sim { image: PathBuf::from(image), disc: Mutex::new(None) }
        }

        /// The disc while the image is there, opened once per insertion.
        fn disc(&self) -> Result<Arc<Disc>, CmdError> {
            let mut d = self.disc.lock().unwrap_or_else(|e| e.into_inner());
            if !Path::new(&self.image).exists() {
                *d = None;
                return Err(sense(2, 0x3A, 0));
            }
            if d.is_none() {
                *d = Some(Arc::new(Disc::open(&self.image).map_err(|_| sense(3, 0x30, 0))?));
            }
            Ok(d.clone().unwrap())
        }

        pub fn command(&self, cdb: &[u8], buf: &mut [u8]) -> Result<(), CmdError> {
            let d = match cdb[0] {
                0xBB | 0x1B => return Ok(()), /* SET CD SPEED, START STOP UNIT */
                _ => self.disc()?,
            };
            let mut out = Vec::new();
            match cdb[0] {
                0x00 => {}
                0x46 => {
                    /* GET CONFIGURATION: the current profile, a CD or a DVD-ROM */
                    let profile: u16 = if d.sector_count() > 360_000 { 0x10 } else { 0x08 };
                    let mut r = [0u8; 8];
                    r[6..8].copy_from_slice(&profile.to_be_bytes());
                    put(buf, &r);
                }
                0x25 => {
                    let mut r = [0u8; 8];
                    r[..4].copy_from_slice(&d.sector_count().saturating_sub(1).to_be_bytes());
                    r[4..].copy_from_slice(&2048u32.to_be_bytes());
                    put(buf, &r);
                }
                0x43 => {
                    mmc::read_toc(&d, cdb[2] & 0x0F, cdb[1] & 2 != 0, cdb[6], &mut out).map_err(error)?;
                    put(buf, &out);
                }
                0x42 => {
                    mmc::read_subchannel(&d, 0, cdb[1] & 2 != 0, cdb[2] & 0x40 != 0, cdb[3], cdb[6], 0x15, &mut out)
                        .map_err(error)?;
                    put(buf, &out);
                }
                0x28 => {
                    let (lba, n) = (be32(&cdb[2..6]) as i32, u16::from_be_bytes([cdb[7], cdb[8]]) as usize);
                    for i in 0..n {
                        let mut c = [0u8; 2048];
                        d.read_cooked(lba + i as i32, &mut c).map_err(error)?;
                        buf[i * 2048..(i + 1) * 2048].copy_from_slice(&c);
                    }
                }
                0xBE => {
                    let lba = be32(&cdb[2..6]) as i32;
                    let n = u32::from_be_bytes([0, cdb[6], cdb[7], cdb[8]]) as usize;
                    let (ty, b9, b10) = ((cdb[1] >> 2) & 7, cdb[9], cdb[10]);
                    let len = mmc::read_cd_length(ty, b9, b10).map_err(error)?;
                    for i in 0..n {
                        mmc::read_cd_sector(&d, lba + i as i32, ty, b9, b10, &mut buf[i * len..(i + 1) * len])
                            .map_err(error)?;
                    }
                }
                _ => return Err(sense(5, 0x20, 0)),
            }
            Ok(())
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

/// The sectors read ahead: windows most recently used first, and the
/// ranges the worker is fetching.
#[derive(Default)]
struct Cache {
    windows: VecDeque<Window>,
    pending: Vec<(i32, u32)>,
    /// First sectors of fetches that failed, for `try_read_raw` to report once.
    failed: Vec<i32>,
}

impl Cache {
    fn get(&mut self, lba: i32) -> Option<&Window> {
        let at = self.windows.iter().position(|w| w.get(lba).is_some())?;
        if at > 0 {
            let w = self.windows.remove(at).unwrap();
            self.windows.push_front(w);
        }
        self.windows.front()
    }

    fn insert(&mut self, w: Window) {
        self.windows.push_front(w);
        self.windows.truncate(CACHE_WINDOWS);
    }

    fn pending(&self, lba: i32) -> bool {
        self.pending.iter().any(|&(f, n)| lba >= f && lba < f + n as i32)
    }
}

/// A read for the worker thread: `count` sectors from `first`, all one track's.
struct Fetch {
    first: i32,
    count: u32,
    audio: bool,
}

/// The drive behind a disc: the handle, what it can do, and the sectors read ahead.
pub struct Drive {
    handle: Arc<Dev>,
    /// What READ CD gives of the subchannel on this drive.
    sub: Sub,
    sectors: Arc<Mutex<Cache>>,
    subs: Mutex<Option<Window>>,
    /// The worker that reads ahead for `try_read_raw` (CD audio), started on
    /// its first use; it ends when the drive is dropped and this goes with it.
    worker: Mutex<Option<mpsc::Sender<Fetch>>>,
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

/// `count` sectors from `lba` in one command: READ CD's raw 2352 (or the
/// subchannel alone, `sub`), or READ(10)'s 2048 (`cooked`).
fn read_window(handle: &Dev, sub_mode: Sub, lba: i32, count: u32, audio: bool, sub: bool, cooked: bool) -> std::result::Result<Window, CmdError> {
    let sub_len = match sub_mode { Sub::Raw => 96, _ => 16 };
    let stride = if sub { sub_len } else if cooked { 2048 } else { 2352 };
    /* what one sector moves over the wire, and how many fit in a command */
    let wire = if sub { 2352 + sub_len } else { stride };
    let per_cmd = (handle.max_transfer() / wire).max(1) as u32;
    let mut data = vec![0u8; count as usize * stride];
    let mut done = 0u32;
    while done < count {
        let n = per_cmd.min(count - done);
        let at = lba + done as i32;
        let mut buf = vec![0u8; n as usize * wire];
        if cooked {
            let mut cdb = [0u8; 10];
            cdb[0] = 0x28;
            cdb_lba(&mut cdb, 2, at);
            cdb[7..9].copy_from_slice(&(n as u16).to_be_bytes());
            handle.command(&cdb, &mut buf, READ_TIMEOUT_MS)?;
        } else {
            let code = if !sub { 0 } else if sub_mode == Sub::Raw { 1 } else { 2 };
            handle.command(&read_cd_cdb(at, n, audio, code), &mut buf, READ_TIMEOUT_MS)?;
        }
        for i in 0..n as usize {
            /* the subchannel bytes after each sector, or the sector itself */
            let from = if sub { i * wire + 2352 } else { i * wire };
            let to = (done as usize + i) * stride;
            data[to..to + stride].copy_from_slice(&buf[from..from + stride]);
        }
        done += n;
    }
    Ok(Window { first: lba, count, stride, data })
}

/// `read_window`, and a run that fails retried as its first sector alone, so
/// a bad sector fails alone, at its LBA.
fn read_run(handle: &Dev, sub_mode: Sub, lba: i32, count: u32, audio: bool, sub: bool, cooked: bool) -> Result<Window> {
    match read_window(handle, sub_mode, lba, count, audio, sub, cooked) {
        Ok(w) => Ok(w),
        Err(_) if count > 1 => read_window(handle, sub_mode, lba, 1, audio, sub, cooked).map_err(|e| e.error(&format!("sector {lba}"))),
        Err(e) => Err(e.error(&format!("sector {lba}"))),
    }
}

/// How many sectors to read from `lba` of track `t` in one command.
fn run_length(t: &Track, lba: i32) -> u32 {
    let ahead = if t.mode == TrackMode::Audio { AUDIO_AHEAD } else { READ_AHEAD };
    ahead.min((t.end_lba - lba) as u32).max(1)
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

impl Drive {
    fn new(handle: Dev) -> Drive {
        Drive {
            handle: Arc::new(handle),
            sub: Sub::None,
            sectors: Arc::default(),
            subs: Mutex::new(None),
            worker: Mutex::new(None),
        }
    }

    fn read_window(&self, lba: i32, count: u32, audio: bool, sub: bool, cooked: bool) -> std::result::Result<Window, CmdError> {
        read_window(&self.handle, self.sub, lba, count, audio, sub, cooked)
    }

    /// One sector of track `t` into `out` (`stride` bytes), through the cache.
    /// The drive is asked without the cache's lock held, so the worker's
    /// reads and this one only wait for each other at the drive.
    fn read(&self, t: &Track, lba: i32, out: &mut [u8], sub: bool, cooked: bool) -> Result<()> {
        let audio = t.mode == TrackMode::Audio;
        if sub {
            let mut win = lock(&self.subs);
            if let Some(s) = win.as_ref().and_then(|w| w.get(lba)) {
                out.copy_from_slice(s);
                return Ok(());
            }
            let w = read_run(&self.handle, self.sub, lba, READ_AHEAD.min((t.end_lba - lba) as u32).max(1), audio, true, false)?;
            out.copy_from_slice(w.get(lba).unwrap());
            *win = Some(w);
            return Ok(());
        }
        if let Some(w) = lock(&self.sectors).get(lba).filter(|w| w.stride == out.len()) {
            out.copy_from_slice(w.get(lba).unwrap());
            return Ok(());
        }
        let w = read_run(&self.handle, self.sub, lba, run_length(t, lba), audio, false, cooked)?;
        out.copy_from_slice(w.get(lba).unwrap());
        lock(&self.sectors).insert(w);
        Ok(())
    }

    /// Queue a read of the run from `first` for the worker, unless it is
    /// being fetched already. Starts the worker on first use.
    fn fetch(&self, cache: &mut Cache, t: &Track, first: i32) {
        if cache.pending(first) {
            return;
        }
        let f = Fetch { first, count: run_length(t, first), audio: t.mode == TrackMode::Audio };
        let mut worker = lock(&self.worker);
        let tx = worker.get_or_insert_with(|| {
            let (tx, rx) = mpsc::channel::<Fetch>();
            let (handle, sub, cache) = (self.handle.clone(), self.sub, self.sectors.clone());
            std::thread::Builder::new()
                .name("libdisc-drive".into())
                .spawn(move || {
                    for f in rx {
                        let r = read_run(&handle, sub, f.first, f.count, f.audio, false, false);
                        let mut c = lock(&cache);
                        c.pending.retain(|&(p, _)| p != f.first);
                        match r {
                            Ok(w) => c.insert(w),
                            Err(_) => c.failed.push(f.first),
                        }
                    }
                })
                .expect("start the drive's read-ahead thread");
            tx
        });
        if tx.send(Fetch { ..f }).is_ok() {
            cache.pending.push((f.first, f.count));
        }
    }

    /// The raw sector at `lba` of track `t` if it is read already: `Ok(true)`
    /// with it in `out`, `Ok(false)` when the drive is still fetching it (a
    /// fetch is queued). Never waits for the drive, so CD audio, which
    /// QEMU plays from its main loop, never stalls the machine: past the
    /// middle of a window the next one is fetched, so music stays a window
    /// ahead. A fetch that failed is reported once, as an I/O error (CD
    /// audio plays it as silence and goes on).
    pub(crate) fn try_read_raw(&self, t: &Track, lba: i32, out: &mut [u8; 2352]) -> Result<bool> {
        let mut c = lock(&self.sectors);
        if let Some(w) = c.get(lba).filter(|w| w.stride == 2352) {
            out.copy_from_slice(w.get(lba).unwrap());
            let (first, count) = (w.first, w.count);
            let next = first + count as i32;
            if lba - first >= count as i32 / 2 && next < t.end_lba && c.windows.iter().all(|w| w.get(next).is_none()) {
                self.fetch(&mut c, t, next);
            }
            return Ok(true);
        }
        if let Some(at) = c.failed.iter().position(|&f| f == lba) {
            c.failed.swap_remove(at);
            return Err(Error::Io(format!("sector {lba}: the drive could not read it")));
        }
        self.fetch(&mut c, t, lba);
        Ok(false)
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

/// What follows `hostcd:` in a medium's name: an optional read speed
/// (`8x:`, `max:`), then the device. `(None, name)` when there is no speed,
/// which reads at the drive's fastest.
pub fn split_speed(name: &str) -> (Option<u16>, &str) {
    if let Some((speed, dev)) = name.split_once(':') {
        if speed == "max" {
            return (None, dev);
        }
        if let Some(x) = speed.strip_suffix(['x', 'X']).and_then(|n| n.parse::<u16>().ok()).filter(|&x| x > 0) {
            return (Some(x), dev);
        }
    }
    (None, name)
}

/// SET CD SPEED: `x` times 1x (176 kB/s), or the drive's fastest. A
/// drive takes the nearest speed it has; one that refuses, or a host that
/// will not pass the command (a read-only handle), keeps its own.
fn set_speed(h: &Dev, x: Option<u16>) {
    let kbps = x.map_or(0xFFFF, |x| x.saturating_mul(176));
    let mut cdb = [0u8; 12];
    cdb[0] = 0xBB;
    cdb[2..4].copy_from_slice(&kbps.to_be_bytes());
    cdb[4..6].copy_from_slice(&[0xFF, 0xFF]);
    let _ = h.command(&cdb, &mut [], 10_000);
}

/// Is there a readable disc in the drive at `path` (TEST UNIT READY)?
/// `Err` when the drive cannot be opened or asked at all. The QEMU
/// driver's watch calls it once a second.
pub fn ready(path: &str) -> Result<bool> {
    let (_, path) = split_speed(path);
    let h = Dev::open(path).map_err(Error::Io)?;
    match h.command(&[0u8; 6], &mut [], 10_000) {
        Ok(()) => Ok(true),
        Err(CmdError::Sense(_)) => Ok(false),
        Err(CmdError::Host(m)) => Err(Error::Io(format!("{path}: {m}"))),
    }
}

/// Open the tray of the drive at `path`.
pub fn eject(path: &str) -> Result<()> {
    let (_, path) = split_speed(path);
    Dev::open(path).map_err(Error::Io)?.eject().map_err(Error::Io)
}

/// The drive's current profile (GET CONFIGURATION): 0x08-0x0A a CD, 0x10 and up a DVD or above.
fn current_profile(h: &Dev) -> Option<u16> {
    let mut buf = [0u8; 8];
    let mut cdb = [0u8; 10];
    cdb[0] = 0x46;
    cdb[1] = 0x01;
    cdb[8] = 8;
    h.command(&cdb, &mut buf, 10_000).ok()?;
    Some(u16::from_be_bytes([buf[6], buf[7]]))
}

fn read_capacity(h: &Dev) -> Result<u32> {
    let mut buf = [0u8; 8];
    let mut cdb = [0u8; 10];
    cdb[0] = 0x25;
    h.command(&cdb, &mut buf, 10_000).map_err(|e| e.error("READ CAPACITY"))?;
    Ok(u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]).wrapping_add(1))
}

/// READ TOC format 2: the raw Q entries of every session, binary MSF.
fn full_toc(h: &Dev) -> std::result::Result<Vec<TocEntry>, CmdError> {
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

/// Open the disc in the drive at `path` (`hostcd:` already stripped; an
/// optional speed before the device, `split_speed`). The speed is set on
/// every open, so a disc swapped in keeps it and choosing the fastest
/// again undoes a slower one.
pub fn open(path: &str) -> Result<Disc> {
    let (speed, path) = split_speed(path);
    let handle = Dev::open(path).map_err(Error::Invalid)?;
    match handle.command(&[0u8; 6], &mut [], 30_000) {
        Ok(()) => {}
        Err(CmdError::Sense(s)) if s.key == 0x6 => {
            /* the change that made it ready: ask once more */
            handle.command(&[0u8; 6], &mut [], 30_000).map_err(|_| Error::Invalid(format!("{path}: no disc in the drive")))?;
        }
        Err(CmdError::Sense(_)) => return Err(Error::Invalid(format!("{path}: no disc in the drive"))),
        Err(CmdError::Host(m)) => return Err(Error::Invalid(format!("{path}: {m}"))),
    }
    set_speed(&handle, speed);
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
        disc.drive = Some(Drive::new(handle));
        return Ok(disc);
    }

    let entries = full_toc(&handle).map_err(|e| e.error(&format!("{path}: READ TOC")))?;
    let mut drive = Drive::new(handle);
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
                match drive.read_raw(&t, start, &mut raw) {
                    Ok(()) if raw[15] == 2 => t.mode = TrackMode::Mode2,
                    Ok(()) => {}
                    Err(_) => {
                        /* A drive that refuses a raw read of a data sector it
                           reads cooked: the track is read cooked (READ(10)),
                           its raw bytes synthesized as for an .iso (PR #4's
                           probe). Mode 1, which every PC disc is. */
                        let mut cooked = [0u8; 2048];
                        if drive.read_cooked(&t, start, &mut cooked).is_ok() {
                            t.extents[0].source = Source::Drive { raw: false };
                        }
                    }
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
    *lock(&drive.sectors) = Cache::default();
    disc.mcn = read_mcn(&drive.handle);
    disc.sessions = sessions;
    disc.raw_toc = Some(entries);
    disc.drive = Some(drive);
    Ok(disc)
}

/// The disc's catalogue number (MCN, READ SUB-CHANNEL format 2), when it
/// has one: instant on the first drive (PR #4 reads it, and each track's
/// ISRC too). ISRCs are not read: format 3 took that drive 0.65 s a track,
/// eight seconds added to every swap of a 12-track disc, for codes a game
/// CD seldom carries.
fn read_mcn(h: &Dev) -> Option<[u8; 13]> {
    let mut buf = [0u8; 24];
    h.command(&[0x42, 0, 0x40, 2, 0, 0, 0, 0, 24, 0], &mut buf, 10_000).ok()?;
    let mcn: [u8; 13] = buf[9..22].try_into().ok()?;
    (buf[8] & 0x80 != 0 && mcn.iter().all(u8::is_ascii_digit) && mcn != [b'0'; 13]).then_some(mcn)
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
    /* The first Q read after a spin-up can fail its CRC: a cold open of the
       first drive missed the pregap that the next open found. A few frames
       back, a few tries. */
    let at_end = [2, 3, 4].into_iter().find_map(|back| q_at(drive, prev, start - back));
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
