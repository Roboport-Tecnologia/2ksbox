//! The host's own optical drives, as discs on the shelf (track M5h).
//!
//! A drive goes on the shelf like an image, by its device path
//! (`/dev/sr0`), and QEMU opens it as `hostcd:/dev/sr0`
//! (`disc_library::qemu_medium`): libdisc reads the disc in it through
//! SCSI pass-through, and QEMU's `hostcd` driver turns a disc put in or
//! taken out of the real drive into the guest's medium change (doc 17 §9).
//! So the shelf entry is the drive, not the disc in it.
//!
//! Linux only for now: libdisc's pass-through is `SG_IO`. Other hosts list
//! no drives.

use std::path::{Path, PathBuf};

/// The prefix QEMU's `hostcd` driver is named with (libdisc's `drive::PREFIX`).
pub const PREFIX: &str = "hostcd:";

/// A drive the host has.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HostDrive {
    /// The device: `/dev/sr0`.
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

#[cfg(not(target_os = "linux"))]
pub fn list() -> Vec<HostDrive> {
    Vec::new()
}

/// Whether a shelf entry is a host drive: a block device under `/dev`.
/// The prefix is checked first, so an image on an idle network share is
/// never asked.
pub fn is_host_drive(path: &Path) -> bool {
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::fs::FileTypeExt;
        path.starts_with("/dev") && std::fs::metadata(path).is_ok_and(|m| m.file_type().is_block_device())
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = path;
        false
    }
}

/// The label a drive gets on the shelf: its model.
pub fn label(path: &Path) -> Option<String> {
    list().into_iter().find(|d| d.path == path).map(|d| d.name)
}
