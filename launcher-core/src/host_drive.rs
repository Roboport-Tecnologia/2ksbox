//! The host's own optical drives (track M5h).
//!
//! A machine can have one as a second CD-ROM (`bundle::Machine::host_drive`),
//! apart from the drive the shelf fills: QEMU opens it as `hostcd:/dev/sr0`,
//! libdisc reads the disc in it through SCSI pass-through, and QEMU's
//! `hostcd` driver turns a disc put in or taken out of the real drive into
//! the guest's medium change (doc 17 §9).
//!
//! Linux only for now: libdisc's pass-through is `SG_IO`. Other hosts list
//! no drives.

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
