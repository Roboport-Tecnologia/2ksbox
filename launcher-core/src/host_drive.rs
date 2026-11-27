//! The host's own optical drives (track M5h).
//!
//! A machine can have one as a second CD-ROM (`bundle::Machine::host_drive`),
//! apart from the drive the shelf fills: QEMU opens it as `hostcd:/dev/sr0`,
//! libdisc reads the disc in it through SCSI pass-through, and QEMU's
//! `hostcd` driver turns a disc put in or taken out of the real drive into
//! the guest's medium change (doc 17 §9).
//!
//! Linux (`/sys/class/block/sr*`, `SG_IO`) and Windows (the CD-ROM drive
//! letters, `\\.\D:`, SPTI or the CD-ROM IOCTLs). Other hosts list no
//! drives.

use std::path::PathBuf;

/// The prefix QEMU's `hostcd` driver is named with (libdisc's `drive::PREFIX`).
pub const PREFIX: &str = "hostcd:";

/// The read speeds the form offers, in CD x, fastest first; before them
/// the drive's own fastest (`None`). A drive takes the nearest it has.
pub const SPEEDS: [u16; 7] = [48, 32, 24, 16, 8, 4, 2];

/// How a speed reads in the form and the machine's details.
pub fn speed_label(x: Option<u16>) -> String {
    x.map_or_else(|| "Fastest".to_string(), |x| format!("{x}x"))
}

/// A drive the host has.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HostDrive {
    /// The device: `/dev/sr0`, `\\.\D:`.
    pub path: PathBuf,
    /// The drive's model as it reports it (`DVDRAM GT50N`), or the device name.
    pub name: String,
}

/// The host's optical drives, in device order.
#[cfg(target_os = "linux")]
pub fn list() -> Vec<HostDrive> {
    let Ok(dir) = std::fs::read_dir("/sys/class/block") else { return Vec::new() };
    let mut drives: Vec<HostDrive> = dir
        .flatten()
        .filter_map(|e| {
            let dev = e.file_name().to_string_lossy().into_owned();
            /* the SCSI CD-ROM driver's devices: every optical drive, USB or SATA */
            dev.strip_prefix("sr")?.parse::<u32>().ok()?;
            let attr = |name: &str| {
                std::fs::read_to_string(e.path().join("device").join(name)).map(|s| s.trim().to_string()).unwrap_or_default()
            };
            let model = attr("model");
            Some(HostDrive { path: PathBuf::from(format!("/dev/{dev}")), name: if model.is_empty() { dev } else { model } })
        })
        .collect();
    drives.sort_by_key(|d| d.path.to_string_lossy().trim_start_matches("/dev/sr").parse::<u32>().unwrap_or(u32::MAX));
    drives
}

/// The drive letters Windows says are CD-ROM drives, as `\\.\D:`, named
/// `D: <vendor product>` (`IOCTL_STORAGE_QUERY_PROPERTY` on a handle opened
/// for queries only, which any user may; ported from PR #4).
#[cfg(windows)]
pub fn list() -> Vec<HostDrive> {
    use std::ffi::c_void;
    type H = *mut c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetLogicalDriveStringsW(len: u32, buf: *mut u16) -> u32;
        fn GetDriveTypeW(root: *const u16) -> u32;
        fn CreateFileW(name: *const u16, access: u32, share: u32, sa: *const c_void, disposition: u32, flags: u32, template: H) -> H;
        fn DeviceIoControl(h: H, code: u32, inb: *const c_void, inlen: u32, outb: *mut c_void, outlen: u32, got: *mut u32, ov: *mut c_void) -> i32;
        fn CloseHandle(h: H) -> i32;
    }
    const DRIVE_CDROM: u32 = 5;
    const IOCTL_STORAGE_QUERY_PROPERTY: u32 = 0x002D_1400;
    let wide = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();

    /* "vendor product" from STORAGE_DEVICE_DESCRIPTOR: its strings are
       offsets into the same buffer (vendor at 12, product at 16) */
    let model = |path: &str| -> String {
        let w = wide(path);
        // SAFETY: a NUL-terminated name; access 0 is a query-only handle.
        let h = unsafe { CreateFileW(w.as_ptr(), 0, 3, std::ptr::null(), 3, 0, std::ptr::null_mut()) };
        if h.is_null() || h == -1isize as H {
            return String::new();
        }
        let query = [0u8; 12]; // StorageDeviceProperty, PropertyStandardQuery
        let mut buf = vec![0u8; 1024];
        let mut got = 0u32;
        // SAFETY: buffers of the lengths given; our own handle, closed after.
        let ok = unsafe {
            let ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, query.as_ptr() as *const c_void, 12, buf.as_mut_ptr() as *mut c_void, 1024, &mut got, std::ptr::null_mut());
            CloseHandle(h);
            ok
        };
        let got = got as usize;
        if ok == 0 || got < 20 {
            return String::new();
        }
        let at = |field: usize| -> String {
            let off = u32::from_le_bytes(buf[field..field + 4].try_into().unwrap()) as usize;
            if off == 0 || off >= got {
                return String::new();
            }
            let end = buf[off..got].iter().position(|&b| b == 0).map_or(got, |n| off + n);
            String::from_utf8_lossy(&buf[off..end]).trim().to_string()
        };
        [at(12), at(16)].into_iter().filter(|s| !s.is_empty()).collect::<Vec<_>>().join(" ")
    };

    let mut buf = vec![0u16; 512];
    // SAFETY: a buffer of the length given.
    let n = unsafe { GetLogicalDriveStringsW(buf.len() as u32, buf.as_mut_ptr()) } as usize;
    buf[..n.min(buf.len())]
        .split(|&c| c == 0)
        .filter(|r| !r.is_empty())
        .map(String::from_utf16_lossy) // "D:\"
        // SAFETY: a NUL-terminated root path.
        .filter(|root| unsafe { GetDriveTypeW(wide(root).as_ptr()) } == DRIVE_CDROM)
        .map(|root| {
            let letter = root.trim_end_matches('\\').to_string();
            let path = format!(r"\\.\{letter}");
            let m = model(&path);
            HostDrive { name: if m.is_empty() { letter.clone() } else { format!("{letter} {m}") }, path: PathBuf::from(path) }
        })
        .collect()
}

#[cfg(not(any(target_os = "linux", windows)))]
pub fn list() -> Vec<HostDrive> {
    Vec::new()
}
