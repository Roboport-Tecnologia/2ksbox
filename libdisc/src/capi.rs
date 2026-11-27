//! The C API (`libdisc/libdisc.h`, doc 17 §3): `#[no_mangle] extern "C"`
//! functions over `Disc`. A handle is a `Box<Disc>`; every body runs under
//! `catch_unwind` and reports a panic as `LIBDISC_EIO`. Nothing unwinds
//! into QEMU.

use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

use crate::{Disc, Error, SectorKind};

pub const LIBDISC_API_VERSION: u32 = 4;

pub const LIBDISC_OK: i32 = 0;
pub const LIBDISC_ERANGE: i32 = -1;
pub const LIBDISC_EMEDIUM: i32 = -2;
pub const LIBDISC_EMODE: i32 = -3;
pub const LIBDISC_EINVAL: i32 = -4;
pub const LIBDISC_EIO: i32 = -5;
/// `libdisc_read_raw_nowait`: a host drive is still fetching the sector.
pub const LIBDISC_EAGAIN: i32 = -6;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct LibdiscSectorInfo {
    pub kind: u8,
    pub track: u8,
    pub index: u8,
    pub lec: u8,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct LibdiscTrackInfo {
    pub number: u8,
    pub session: u8,
    pub control: u8,
    pub mode: u8,
    pub start_lba: i32,
    pub pregap_lba: i32,
    pub end_lba: i32,
}

pub fn code(e: &Error) -> i32 {
    match e {
        Error::Range => LIBDISC_ERANGE,
        Error::Medium => LIBDISC_EMEDIUM,
        Error::Mode => LIBDISC_EMODE,
        Error::Invalid(_) => LIBDISC_EINVAL,
        Error::Io(_) => LIBDISC_EIO,
    }
}

fn guard<F: FnOnce() -> i32>(f: F) -> i32 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(LIBDISC_EIO)
}

/// # Safety
/// `d` must be a handle from `libdisc_open` that has not been closed.
unsafe fn disc<'a>(d: *const Disc) -> Option<&'a Disc> {
    if d.is_null() {
        None
    } else {
        Some(&*d)
    }
}

fn put_err(err: *mut c_char, errlen: usize, msg: &str) {
    if err.is_null() || errlen == 0 {
        return;
    }
    let bytes: Vec<u8> = msg.bytes().filter(|b| b.is_ascii() && *b != 0).take(errlen - 1).collect();
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, bytes.len());
        *err.add(bytes.len()) = 0;
    }
}

/// Copy a reply into a C buffer of `cap` bytes; the reply length, or EINVAL when it does not fit.
fn reply(out: *mut u8, cap: usize, data: &[u8]) -> i32 {
    if out.is_null() || data.len() > cap {
        return LIBDISC_EINVAL;
    }
    unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), out, data.len()) };
    data.len() as i32
}

#[no_mangle]
pub extern "C" fn libdisc_api_version() -> u32 {
    LIBDISC_API_VERSION
}

/// # Safety
/// `head` points to `len` readable bytes; `filename` is a NUL-terminated string or NULL.
#[no_mangle]
pub unsafe extern "C" fn libdisc_probe(head: *const u8, len: usize, filename: *const c_char) -> i32 {
    guard(|| {
        let head = if head.is_null() { &[][..] } else { std::slice::from_raw_parts(head, len) };
        let name = if filename.is_null() { String::new() } else { CStr::from_ptr(filename).to_string_lossy().to_ascii_lowercase() };
        let sniffed = crate::sniff(head);
        // GOG names a DOSBox game's cue sheet `game.ins` (or `.inst`),
        // beside its `game.gog` data track.
        let cue_name = name.ends_with(".cue") || name.ends_with(".ins") || name.ends_with(".inst");
        if cue_name && sniffed == Some("cue") {
            100
        } else if name.ends_with(".ccd") && sniffed == Some("ccd") {
            100
        } else if name.ends_with(".mds") && sniffed == Some("mds") {
            100
        } else {
            0
        }
    })
}

/// # Safety
/// `path` is a NUL-terminated string; `err` points to `errlen` writable bytes or is NULL.
#[no_mangle]
pub unsafe extern "C" fn libdisc_open(path: *const c_char, err: *mut c_char, errlen: usize) -> *mut Disc {
    let r = catch_unwind(AssertUnwindSafe(|| {
        if path.is_null() {
            put_err(err, errlen, "libdisc_open: NULL path");
            return std::ptr::null_mut();
        }
        let p = CStr::from_ptr(path).to_string_lossy().into_owned();
        match Disc::open(Path::new(&p)) {
            Ok(d) => std::sync::Arc::into_raw(std::sync::Arc::new(d)) as *mut Disc,
            Err(e) => {
                put_err(err, errlen, &e.to_string());
                std::ptr::null_mut()
            }
        }
    }));
    match r {
        Ok(p) => p,
        Err(_) => {
            put_err(err, errlen, "libdisc_open: internal error (panic)");
            std::ptr::null_mut()
        }
    }
}

fn drive_path(path: *const c_char) -> Option<String> {
    if path.is_null() {
        return None;
    }
    // SAFETY: the caller passes a NUL-terminated string.
    let p = unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned();
    Some(p.strip_prefix(crate::drive::PREFIX).map(str::to_owned).unwrap_or(p))
}

/// 1 when the host drive at `path` (`/dev/sr0`, with or without `hostcd:`)
/// has a readable disc, 0 when it has none (or is still spinning one up),
/// `LIBDISC_EIO` when the drive cannot be opened or asked.
///
/// # Safety
/// `path` is NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn libdisc_drive_ready(path: *const c_char) -> i32 {
    guard(|| match drive_path(path).map(|p| crate::drive::ready(&p)) {
        Some(Ok(r)) => r as i32,
        Some(Err(e)) => code(&e),
        None => LIBDISC_EINVAL,
    })
}

/// Open the tray of the host drive at `path`.
///
/// # Safety
/// `path` is NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn libdisc_drive_eject(path: *const c_char) -> i32 {
    guard(|| match drive_path(path).map(|p| crate::drive::eject(&p)) {
        Some(Ok(())) => LIBDISC_OK,
        Some(Err(e)) => code(&e),
        None => LIBDISC_EINVAL,
    })
}

/// Drop one reference to the disc (`libdisc_open`'s or a `libdisc_ref`'s);
/// the last one closes it.
///
/// # Safety
/// `d` is a handle from `libdisc_open` (or NULL), and the caller drops a
/// reference it holds.
#[no_mangle]
pub unsafe extern "C" fn libdisc_close(d: *mut Disc) {
    if !d.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(std::sync::Arc::from_raw(d as *const Disc))));
    }
}

/// One more reference to the disc, for a read that may outlive the
/// caller's: QEMU reads a host drive on a worker thread while the disc can
/// be swapped out of the drive (doc 17 §9). `libdisc_close` drops it.
///
/// # Safety
/// `d` is an open handle.
#[no_mangle]
pub unsafe extern "C" fn libdisc_ref(d: *const Disc) -> *const Disc {
    if !d.is_null() {
        std::sync::Arc::increment_strong_count(d);
    }
    d
}

/// 1 when the disc is a host's drive (`hostcd:`), whose reads can take
/// seconds and belong off QEMU's main loop. `LIBDISC_DRIVE_SYNC=1` answers
/// 0, so QEMU reads the drive on its main loop as before patch 102: the
/// A/B for a freeze.
///
/// # Safety
/// `d` is an open handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn libdisc_is_host_drive(d: *const Disc) -> i32 {
    static SYNC: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let sync = *SYNC.get_or_init(|| std::env::var_os("LIBDISC_DRIVE_SYNC").is_some_and(|v| v == "1"));
    (!sync && disc(d).is_some_and(|d| d.is_host_drive())) as i32
}

/// The sector's kind from the layout alone (`LIBDISC_KIND_*`; a data
/// track's own mode, gap between sessions), never reading it: what a
/// command checks before it starts, where a host drive's read would wait
/// out a seek on QEMU's main loop.
///
/// # Safety
/// `d` is an open handle.
#[no_mangle]
pub unsafe extern "C" fn libdisc_sector_kind(d: *const Disc, lba: u32) -> i32 {
    guard(|| {
        let Some(d) = disc(d) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        match d.layout_kind(lba as i32) {
            Ok(k) => k.code() as i32,
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle.
#[no_mangle]
pub unsafe extern "C" fn libdisc_sector_count(d: *const Disc) -> u32 {
    disc(d).map(|d| d.sector_count()).unwrap_or(0)
}

/// # Safety
/// `d` is an open handle.
#[no_mangle]
pub unsafe extern "C" fn libdisc_session_count(d: *const Disc) -> u8 {
    disc(d).map(|d| d.sessions.len() as u8).unwrap_or(0)
}

/// # Safety
/// `d` is an open handle.
#[no_mangle]
pub unsafe extern "C" fn libdisc_track_count(d: *const Disc) -> u8 {
    disc(d).map(|d| d.track_count()).unwrap_or(0)
}

/// # Safety
/// `d` is an open handle; `out` points to a writable `LibdiscTrackInfo`.
#[no_mangle]
pub unsafe extern "C" fn libdisc_track_info(d: *const Disc, track: u8, out: *mut LibdiscTrackInfo) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        match d.track(track) {
            Some((s, t)) => {
                *out = LibdiscTrackInfo {
                    number: t.number,
                    session: s.number,
                    control: t.control,
                    mode: t.mode.default_kind().code(),
                    start_lba: t.start_lba,
                    pregap_lba: t.first_lba(),
                    end_lba: t.end_lba,
                };
                LIBDISC_OK
            }
            None => LIBDISC_ERANGE,
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to a writable `LibdiscSectorInfo`.
#[no_mangle]
pub unsafe extern "C" fn libdisc_sector_info(d: *const Disc, lba: u32, out: *mut LibdiscSectorInfo) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        match d.sector_info(lba as i32) {
            Ok(i) => {
                *out = LibdiscSectorInfo { kind: i.kind.code(), track: i.track, index: i.index, lec: if i.lec_ok == Some(false) { 0 } else { 1 } };
                LIBDISC_OK
            }
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to 2048 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_read_cooked(d: *const Disc, lba: u32, out: *mut u8) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        let buf = &mut *(out as *mut [u8; 2048]);
        match d.read_cooked(lba as i32, buf) {
            Ok(()) => LIBDISC_OK,
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to 2352 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_read_raw(d: *const Disc, lba: u32, out: *mut u8) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        let buf = &mut *(out as *mut [u8; 2352]);
        match d.read_raw(lba as i32, buf) {
            Ok(()) => LIBDISC_OK,
            Err(e) => code(&e),
        }
    })
}

/// `libdisc_read_raw` that never waits for a host drive: `LIBDISC_EAGAIN`
/// while the drive is still fetching the sector (asked for now; call again).
///
/// # Safety
/// `d` is an open handle; `out` points to 2352 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_read_raw_nowait(d: *const Disc, lba: u32, out: *mut u8) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        let buf = &mut *(out as *mut [u8; 2352]);
        match d.read_raw_nowait(lba as i32, buf) {
            Ok(true) => LIBDISC_OK,
            Ok(false) => LIBDISC_EAGAIN,
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to 96 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_read_sub(d: *const Disc, lba: u32, out: *mut u8) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        let buf = &mut *(out as *mut [u8; 96]);
        match d.read_sub(lba as i32, buf) {
            Ok(()) => LIBDISC_OK,
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_mmc_read_toc(d: *const Disc, format: u8, msf: i32, start: u8, out: *mut u8, cap: usize) -> i32 {
    guard(|| {
        let Some(d) = disc(d) else { return LIBDISC_EINVAL };
        let mut v = Vec::with_capacity(1024);
        match crate::mmc::read_toc(d, format, msf != 0, start, &mut v) {
            Ok(()) => reply(out, cap, &v),
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_mmc_read_subchannel(
    d: *const Disc,
    pos_lba: u32,
    msf: i32,
    subq: i32,
    format: u8,
    track: u8,
    audio_status: u8,
    out: *mut u8,
    cap: usize,
) -> i32 {
    guard(|| {
        let Some(d) = disc(d) else { return LIBDISC_EINVAL };
        let mut v = Vec::with_capacity(32);
        match crate::mmc::read_subchannel(d, pos_lba.min(i32::MAX as u32) as i32, msf != 0, subq != 0, format, track, audio_status, &mut v) {
            Ok(()) => reply(out, cap, &v),
            Err(e) => code(&e),
        }
    })
}

/// # Safety
/// `d` is an open handle; `out` points to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_mmc_read_disc_information(d: *const Disc, out: *mut u8, cap: usize) -> i32 {
    guard(|| {
        let Some(d) = disc(d) else { return LIBDISC_EINVAL };
        let mut v = Vec::with_capacity(34);
        match crate::mmc::read_disc_information(d, &mut v) {
            Ok(()) => reply(out, cap, &v),
            Err(e) => code(&e),
        }
    })
}

#[no_mangle]
pub extern "C" fn libdisc_mmc_read_cd_length(expected_type: u8, byte9: u8, byte10: u8) -> i32 {
    guard(|| match crate::mmc::read_cd_length(expected_type, byte9, byte10) {
        Ok(n) => n as i32,
        Err(e) => code(&e),
    })
}

/// # Safety
/// `d` is an open handle; `out` points to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn libdisc_mmc_read_cd_sector(d: *const Disc, lba: u32, expected_type: u8, byte9: u8, byte10: u8, out: *mut u8, cap: usize) -> i32 {
    guard(|| {
        let (Some(d), false) = (disc(d), out.is_null()) else { return LIBDISC_EINVAL };
        if lba > i32::MAX as u32 {
            return LIBDISC_ERANGE;
        }
        let buf = std::slice::from_raw_parts_mut(out, cap);
        match crate::mmc::read_cd_sector(d, lba as i32, expected_type, byte9, byte10, buf) {
            Ok(n) => n as i32,
            Err(e) => code(&e),
        }
    })
}

/// Kind code of a `SectorKind` (for callers that compare with the header's constants).
pub fn kind_code(k: SectorKind) -> u8 {
    k.code()
}
