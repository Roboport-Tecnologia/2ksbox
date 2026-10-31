//! This computer's own CD/DVD drives, passed through to a machine: a real
//! drive as the guest's second CD-ROM drive (`Machine::host_optical`), or
//! in its first drive from the player's Disc menu (`shelf`).
//!
//! Where the platform lets this user send the drive SCSI commands (Linux's
//! SG_IO, Windows' SPTI), the drive is read through libdisc the way a
//! ripping program reads it (`hostcd:<speed>:<device>`, doc 17 §9): CD
//! audio, every track, subchannel and copy protection reach the guest as
//! from an image. Windows without an administrator still gets data and CD
//! audio through its CD-ROM IOCTLs. Anywhere else (macOS, a Linux user
//! without access) QEMU reads the device itself, data only
//! (`host_cdrom` / `host_device`). `reach` says which.
//!
//! The drive list is cheap (sysfs on Linux, a few IOCTLs on Windows,
//! `diskutil` on macOS), and the form scans it when it opens and on
//! Refresh, never per frame.

use std::path::Path;

pub use libdisc::drive::CD_1X_KBPS;

/// One drive the host has, as the form lists it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostDrive {
    /// The device: `/dev/sr0`, `\\.\D:`, `/dev/disk4`. What the bundle stores.
    pub path: String,
    /// What the picker shows: `D: HL-DT-ST DVDRAM GP65NB60 – “GAME_CD1”`.
    pub label: String,
}

/// The CD, DVD and Blu-ray drives this computer has, empty or not.
pub fn optical_drives() -> Vec<HostDrive> {
    imp::optical_drives()
}

/// Whether a stored device is there now, for `player::spawn`'s refusal:
/// a machine whose USB DVD drive was unplugged says so instead of QEMU
/// failing to open it in the middle of start-up.
pub fn is_present(path: &str) -> bool {
    imp::is_present(path)
}

/// How much of a disc in this drive reaches the guest.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reach {
    /// Read raw through libdisc: data, CD audio, subchannel, protection.
    Full,
    /// Windows' CD-ROM IOCTLs through libdisc: data and CD audio.
    Audio,
    /// QEMU's own host device: data only.
    DataOnly,
}

/// What this user can get from `dev`, and the drive's fastest speed in
/// CD x when it says (`libdisc::drive::query`: no disc is read).
pub fn reach(dev: &str) -> (Reach, Option<u16>) {
    match libdisc::drive::query(dev) {
        Ok(info) => (
            match info.access {
                libdisc::drive::Access::Full => Reach::Full,
                libdisc::drive::Access::Basic => Reach::Audio,
            },
            info.max_kbps.map(|k| (k / CD_1X_KBPS).max(1)),
        ),
        Err(_) => (Reach::DataOnly, None),
    }
}

/// The speeds a picker offers for a drive whose fastest is `max_x`, in CD
/// x, fastest first.
pub fn speed_choices(max_x: Option<u16>) -> Vec<u16> {
    libdisc::drive::speed_choices(max_x.map(|x| x.saturating_mul(CD_1X_KBPS)))
}

/// The medium name QEMU opens `dev` by, through libdisc, at `speed` (CD
/// x; `None` = the drive's fastest).
pub fn medium(dev: &str, speed: Option<u16>) -> String {
    libdisc::drive::medium(dev, speed.map_or(libdisc::drive::Speed::Max, libdisc::drive::Speed::Times))
}

/// Whether there is a disc in `dev` to read now.
pub fn has_disc(dev: &str) -> bool {
    libdisc::drive::has_disc(dev)
}

/// The device a medium QEMU reports is on: `/dev/sr0` for
/// `hostcd:8x:/dev/sr0` or `/dev/sr0` itself; `None` for an image.
pub fn device_of(medium: &str) -> Option<String> {
    if let Some((_, dev)) = libdisc::drive::parse_medium(medium) {
        return Some(dev.to_string());
    }
    (medium.starts_with("/dev/") || medium.starts_with(r"\\.\")).then(|| medium.to_string())
}

/// The QEMU block driver for a drive QEMU reads itself (`Reach::DataOnly`).
/// On Linux `host_cdrom`, which follows the tray; Windows' QEMU has only
/// `host_device`, which recognises `\\.\D:` as a CD, and a Mac's optical
/// drive is a plain disk node to it.
pub fn qemu_driver() -> &'static str {
    if cfg!(target_os = "linux") { "host_cdrom" } else { "host_device" }
}

/// Whether the app runs in a Flatpak, whose sandbox has no `/dev/sr*`
/// unless the user grants it (`notes`).
pub fn in_flatpak() -> bool {
    Path::new("/.flatpak-info").exists()
}

#[cfg(target_os = "linux")]
mod imp {
    use super::HostDrive;
    use std::fs;
    use std::path::Path;

    fn read(path: &Path) -> String {
        fs::read_to_string(path).map(|s| s.trim().to_string()).unwrap_or_default()
    }

    /// `vendor model` from sysfs, the way `lsblk -o VENDOR,MODEL` has it.
    fn model(dev: &Path) -> String {
        let vendor = read(&dev.join("device/vendor"));
        let model = read(&dev.join("device/model"));
        let name = read(&dev.join("device/name")); // mmcblk
        let s = [vendor, model, name].into_iter().filter(|s| !s.is_empty()).collect::<Vec<_>>().join(" ");
        if s.is_empty() { "Drive".to_string() } else { s }
    }

    fn block_devices() -> Vec<(String, std::path::PathBuf)> {
        let mut out: Vec<_> = fs::read_dir("/sys/block")
            .into_iter()
            .flatten()
            .flatten()
            .map(|e| (e.file_name().to_string_lossy().into_owned(), e.path()))
            .collect();
        out.sort();
        out
    }

    pub fn optical_drives() -> Vec<HostDrive> {
        block_devices()
            .into_iter()
            .filter(|(name, _)| name.starts_with("sr"))
            .map(|(name, sys)| HostDrive {
                path: format!("/dev/{name}"),
                label: format!("{} (/dev/{name})", model(&sys)),
            })
            .collect()
    }

    pub fn is_present(path: &str) -> bool {
        Path::new(path).exists()
    }
}

#[cfg(target_os = "windows")]
mod imp {
    use super::HostDrive;
    use std::ffi::c_void;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, GetDriveTypeW, GetLogicalDriveStringsW,
        GetVolumeInformationW, OPEN_EXISTING,
    };
    use windows_sys::Win32::System::IO::DeviceIoControl;

    const DRIVE_CDROM: u32 = 5;
    const IOCTL_STORAGE_QUERY_PROPERTY: u32 = 0x002D_1400;

    #[repr(C)]
    struct StoragePropertyQuery {
        property_id: u32, // StorageDeviceProperty = 0
        query_type: u32,  // PropertyStandardQuery = 0
        additional: [u8; 1],
    }

    /// The head of `STORAGE_DEVICE_DESCRIPTOR`; the strings it points
    /// at are offsets into the same buffer.
    #[repr(C)]
    struct StorageDeviceDescriptor {
        version: u32,
        size: u32,
        device_type: u8,
        device_type_modifier: u8,
        removable_media: u8,
        command_queueing: u8,
        vendor_id_offset: u32,
        product_id_offset: u32,
        product_revision_offset: u32,
        serial_number_offset: u32,
        bus_type: u32, // an enum, an int in C
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    /// The device opened for queries only (no read or write access, so
    /// no administrator is needed and nothing is locked).
    fn open_query(path: &str) -> Option<HANDLE> {
        let w = wide(path);
        let h = unsafe {
            CreateFileW(w.as_ptr(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, std::ptr::null(), OPEN_EXISTING, 0, std::ptr::null_mut())
        };
        (h != INVALID_HANDLE_VALUE && !h.is_null()).then_some(h)
    }

    fn ioctl(h: HANDLE, code: u32, input: &[u8], out: &mut [u8]) -> Option<usize> {
        let mut got = 0u32;
        let ok = unsafe {
            DeviceIoControl(
                h,
                code,
                input.as_ptr() as *const c_void,
                input.len() as u32,
                out.as_mut_ptr() as *mut c_void,
                out.len() as u32,
                &mut got,
                std::ptr::null_mut(),
            )
        };
        (ok != 0).then_some(got as usize)
    }

    /// `(bus type, "vendor product")` of a device.
    fn describe(h: HANDLE) -> Option<(u32, String)> {
        let q = StoragePropertyQuery { property_id: 0, query_type: 0, additional: [0] };
        let input = unsafe {
            std::slice::from_raw_parts(&q as *const _ as *const u8, std::mem::size_of::<StoragePropertyQuery>())
        };
        let mut buf = vec![0u8; 1024];
        let got = ioctl(h, IOCTL_STORAGE_QUERY_PROPERTY, input, &mut buf)?;
        if got < std::mem::size_of::<StorageDeviceDescriptor>() {
            return None;
        }
        let d = unsafe { std::ptr::read_unaligned(buf.as_ptr() as *const StorageDeviceDescriptor) };
        let string_at = |off: u32| -> String {
            let off = off as usize;
            if off == 0 || off >= got {
                return String::new();
            }
            let end = buf[off..got].iter().position(|&b| b == 0).map_or(got, |n| off + n);
            String::from_utf8_lossy(&buf[off..end]).trim().to_string()
        };
        let name = [string_at(d.vendor_id_offset), string_at(d.product_id_offset)]
            .into_iter()
            .filter(|s| !s.is_empty())
            .collect::<Vec<_>>()
            .join(" ");
        Some((d.bus_type, name))
    }

    fn letters() -> Vec<String> {
        let mut buf = vec![0u16; 512];
        let n = unsafe { GetLogicalDriveStringsW(buf.len() as u32, buf.as_mut_ptr()) } as usize;
        buf[..n.min(buf.len())]
            .split(|&c| c == 0)
            .filter(|s| !s.is_empty())
            .map(String::from_utf16_lossy) // "D:\"
            .collect()
    }

    fn volume_label(root: &str) -> String {
        let w = wide(root);
        let mut name = vec![0u16; 261];
        let ok = unsafe {
            GetVolumeInformationW(
                w.as_ptr(),
                name.as_mut_ptr(),
                name.len() as u32,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                0,
            )
        };
        if ok == 0 {
            return String::new();
        }
        let end = name.iter().position(|&c| c == 0).unwrap_or(name.len());
        String::from_utf16_lossy(&name[..end])
    }

    pub fn optical_drives() -> Vec<HostDrive> {
        let mut out = Vec::new();
        for root in letters() {
            let w = wide(&root);
            if unsafe { GetDriveTypeW(w.as_ptr()) } != DRIVE_CDROM {
                continue;
            }
            let letter = root.trim_end_matches('\\').to_string(); // "D:"
            let path = format!(r"\\.\{letter}");
            let model = open_query(&path)
                .map(|h| {
                    let m = describe(h).map(|(_, m)| m).unwrap_or_default();
                    unsafe { CloseHandle(h) };
                    m
                })
                .unwrap_or_default();
            let disc = volume_label(&root);
            let mut label = letter.clone();
            if !model.is_empty() {
                label.push_str(&format!(" {model}"));
            }
            if !disc.is_empty() {
                label.push_str(&format!(" \u{2013} \u{201c}{disc}\u{201d}"));
            }
            out.push(HostDrive { path, label });
        }
        out
    }

    pub fn is_present(path: &str) -> bool {
        match open_query(path) {
            Some(h) => {
                unsafe { CloseHandle(h) };
                true
            }
            None => false,
        }
    }
}

#[cfg(target_os = "macos")]
mod imp {
    use super::HostDrive;
    use std::process::Command;

    /// `diskutil info <disk>` as key/value pairs.
    fn info(disk: &str) -> Vec<(String, String)> {
        let Ok(out) = Command::new("/usr/sbin/diskutil").args(["info", disk]).output() else { return Vec::new() };
        String::from_utf8_lossy(&out.stdout)
            .lines()
            .filter_map(|l| l.split_once(':'))
            .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
            .collect()
    }

    fn get<'a>(info: &'a [(String, String)], key: &str) -> Option<&'a str> {
        info.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    /// Every whole physical disk (`/dev/disk4`), from `diskutil list`.
    fn whole_disks() -> Vec<String> {
        let Ok(out) = Command::new("/usr/sbin/diskutil").args(["list", "physical"]).output() else {
            return Vec::new();
        };
        String::from_utf8_lossy(&out.stdout)
            .lines()
            .filter(|l| l.starts_with("/dev/disk"))
            .filter_map(|l| l.split_whitespace().next().map(str::to_string))
            .collect()
    }

    pub fn optical_drives() -> Vec<HostDrive> {
        whole_disks()
            .into_iter()
            .filter_map(|disk| {
                let i = info(&disk);
                get(&i, "Optical Drive Type")?;
                let name = get(&i, "Device / Media Name").unwrap_or("Optical drive");
                Some(HostDrive { label: format!("{name} ({disk})"), path: disk })
            })
            .collect()
    }

    pub fn is_present(path: &str) -> bool {
        std::path::Path::new(path).exists()
    }
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
mod imp {
    use super::HostDrive;
    pub fn optical_drives() -> Vec<HostDrive> {
        Vec::new()
    }
    pub fn is_present(path: &str) -> bool {
        std::path::Path::new(path).exists()
    }
}
