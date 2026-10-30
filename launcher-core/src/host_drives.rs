//! This computer's own drives, passed through to a machine: a real CD/DVD
//! drive as the guest's second CD-ROM drive, and a USB stick, card or
//! external disk as its second hard disk (`Machine::host_optical`,
//! `Machine::host_disk`). The guest sees ordinary IDE devices, so
//! Windows 95/98/XP need no driver; QEMU reads the device itself
//! (`host_cdrom` / `host_device`).
//!
//! Only removable and external disks are offered. The disk the host runs
//! from is never in the list: a guest writing to a disk the host has
//! mounted wrecks both file systems.
//!
//! The scan is synchronous and cheap (sysfs on Linux, a few
//! `DeviceIoControl`s on Windows, `diskutil` on macOS), and the form runs
//! it when it opens and on Refresh, never per frame.

use std::path::Path;

/// One drive the host has, as the form lists it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostDrive {
    /// What QEMU opens: `/dev/sr0`, `\\.\D:`, `\\.\PhysicalDrive2`,
    /// `/dev/disk4`. What the bundle stores.
    pub path: String,
    /// What the picker shows: `D: HL-DT-ST DVDRAM GP65NB60 (/dev/sr0)`.
    pub label: String,
}

/// The CD, DVD and Blu-ray drives this computer has, empty or not.
pub fn optical_drives() -> Vec<HostDrive> {
    imp::optical_drives()
}

/// The removable and external disks this computer has: USB sticks and
/// disks, memory cards. Never the disk the host itself runs from.
pub fn removable_disks() -> Vec<HostDrive> {
    imp::removable_disks()
}

/// Whether a stored device is there now, for `player::spawn`'s refusal:
/// a machine whose USB stick was pulled out says so instead of QEMU
/// failing to open it in the middle of start-up.
pub fn is_present(path: &str) -> bool {
    imp::is_present(path)
}

/// The QEMU block driver for a host device of this kind. On Linux a CD
/// is `host_cdrom`, which follows the tray: a disc swapped in the real
/// drive is a disc swapped in the guest's. Windows' QEMU has only
/// `host_device`, which recognises `\\.\D:` as a CD itself, and a Mac's
/// optical drive is a plain disk node to it; neither sees a swap, so
/// there the disc goes in before the machine starts (`notes`).
pub fn qemu_driver(optical: bool) -> &'static str {
    if optical && cfg!(target_os = "linux") { "host_cdrom" } else { "host_device" }
}

/// Whether a disc swapped in the real drive reaches a running guest
/// (`qemu_driver`).
pub fn follows_tray() -> bool {
    cfg!(target_os = "linux")
}

/// Whether the app runs in a Flatpak, whose sandbox has no `/dev/sr*` or
/// `/dev/sd*` unless the user grants it (`notes`).
pub fn in_flatpak() -> bool {
    Path::new("/.flatpak-info").exists()
}

fn human_size(bytes: u64) -> String {
    const GB: f64 = 1_000_000_000.0;
    let b = bytes as f64;
    if b >= 1000.0 * GB {
        format!("{:.1} TB", b / (1000.0 * GB))
    } else if b >= GB {
        format!("{:.1} GB", b / GB)
    } else {
        format!("{} MB", bytes / 1_000_000)
    }
}

#[cfg(target_os = "linux")]
mod imp {
    use super::{HostDrive, human_size};
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

    /// The block devices behind `/`, `/boot` and `/home`, by kernel name
    /// (`nvme0n1`, `sda`), so the host's own disk is never offered even
    /// when it is a USB disk the system booted from.
    fn system_disks() -> Vec<String> {
        let mounts = fs::read_to_string("/proc/mounts").unwrap_or_default();
        let mut out = Vec::new();
        for line in mounts.lines() {
            let mut f = line.split_whitespace();
            let (Some(dev), Some(at)) = (f.next(), f.next()) else { continue };
            if !matches!(at, "/" | "/boot" | "/boot/efi" | "/home" | "/usr" | "/var") {
                continue;
            }
            let Ok(real) = fs::canonicalize(dev) else { continue };
            let Some(part) = real.file_name().map(|n| n.to_string_lossy().into_owned()) else { continue };
            // a partition's sysfs entry sits under its disk's
            for (disk, sys) in block_devices() {
                if disk == part || sys.join(&part).exists() {
                    out.push(disk);
                }
            }
        }
        out
    }

    pub fn removable_disks() -> Vec<HostDrive> {
        let system = system_disks();
        block_devices()
            .into_iter()
            .filter(|(name, _)| {
                ["sd", "mmcblk", "nvme", "vd"].iter().any(|p| name.starts_with(p)) && !system.contains(name)
            })
            .filter(|(_, sys)| {
                // a USB disk often says removable=0, so its bus decides too
                let usb = fs::canonicalize(sys).is_ok_and(|p| p.to_string_lossy().contains("/usb"));
                read(&sys.join("removable")) == "1" || usb
            })
            .filter_map(|(name, sys)| {
                let bytes = read(&sys.join("size")).parse::<u64>().ok()? * 512;
                // a card reader with no card in it
                (bytes > 0).then(|| HostDrive {
                    path: format!("/dev/{name}"),
                    label: format!("{} – {} (/dev/{name})", model(&sys), human_size(bytes)),
                })
            })
            .collect()
    }

    pub fn is_present(path: &str) -> bool {
        Path::new(path).exists()
    }
}

#[cfg(target_os = "windows")]
mod imp {
    use super::{HostDrive, human_size};
    use std::ffi::c_void;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, GetDriveTypeW, GetLogicalDriveStringsW,
        GetVolumeInformationW, OPEN_EXISTING,
    };
    use windows_sys::Win32::System::IO::DeviceIoControl;

    const DRIVE_CDROM: u32 = 5;
    const IOCTL_STORAGE_QUERY_PROPERTY: u32 = 0x002D_1400;
    const IOCTL_DISK_GET_LENGTH_INFO: u32 = 0x0007_405C;
    /// `STORAGE_BUS_TYPE`: the buses a removable or external disk is on.
    const BUS_1394: u32 = 4;
    const BUS_USB: u32 = 7;
    const BUS_SD: u32 = 12;
    const BUS_MMC: u32 = 13;

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

    fn length(h: HANDLE) -> Option<u64> {
        let mut buf = [0u8; 8];
        ioctl(h, IOCTL_DISK_GET_LENGTH_INFO, &[], &mut buf)?;
        Some(u64::from_le_bytes(buf))
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

    pub fn removable_disks() -> Vec<HostDrive> {
        let mut out = Vec::new();
        for n in 0..32 {
            let path = format!(r"\\.\PhysicalDrive{n}");
            let Some(h) = open_query(&path) else { continue };
            let desc = describe(h);
            let size = length(h);
            unsafe { CloseHandle(h) };
            let Some((bus, model)) = desc else { continue };
            // the disk Windows runs from is SATA, NVMe or RAID, never these
            if !matches!(bus, BUS_USB | BUS_SD | BUS_MMC | BUS_1394) {
                continue;
            }
            let Some(size) = size.filter(|&s| s > 0) else { continue };
            let model = if model.is_empty() { "Disk".to_string() } else { model };
            out.push(HostDrive { label: format!("{model} \u{2013} {} (disk {n})", human_size(size)), path });
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
    use super::{HostDrive, human_size};
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

    pub fn removable_disks() -> Vec<HostDrive> {
        whole_disks()
            .into_iter()
            .filter_map(|disk| {
                let i = info(&disk);
                if get(&i, "Optical Drive Type").is_some() {
                    return None;
                }
                let external = get(&i, "Device Location") == Some("External")
                    || get(&i, "Removable Media") == Some("Removable");
                if !external {
                    return None;
                }
                let name = get(&i, "Device / Media Name").unwrap_or("Disk");
                // "Disk Size: 16.0 GB (16008609792 Bytes) ..."
                let bytes = get(&i, "Disk Size")
                    .and_then(|s| s.split('(').nth(1))
                    .and_then(|s| s.split_whitespace().next())
                    .and_then(|s| s.parse::<u64>().ok());
                let size = bytes.map(|b| format!(" \u{2013} {}", human_size(b))).unwrap_or_default();
                Some(HostDrive { label: format!("{name}{size} ({disk})"), path: disk })
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
    pub fn removable_disks() -> Vec<HostDrive> {
        Vec::new()
    }
    pub fn is_present(path: &str) -> bool {
        std::path::Path::new(path).exists()
    }
}
