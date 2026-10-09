//! The host screen's refresh rate, and the guest's that follows it (track
//! M24, step 4). The guest's vertical blank (our viogpudo) at a rate that
//! divides the screen's keeps every guest frame the same number of host
//! refreshes: 72 on a 144 Hz screen was the user's "very smooth", where 60
//! there alternates two and three refreshes a frame (step A). And the host
//! screen's own blank raises the guest's (step B, [`HostVBlank`]), so a
//! guest frame never slides across the host's: a guest timer, however
//! exact, free-runs against the screen's real rate.
//!
//! The screen's rate and blank are read where the system gives them: on
//! Windows the display path of the window's monitor (`QueryDisplayConfig`,
//! a rational such as 143.981 Hz) and its adapter's vertical blank event
//! (`D3DKMTWaitForVerticalBlankEvent`). Elsewhere neither is read yet: the
//! guest gets 60 Hz, which a 60 Hz screen and a 120 Hz ProMotion one both
//! divide, from its own timer.

use qemu_embed::Qemu;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use wgpu::rwh::HasWindowHandle;

/// The refresh rate, in mHz, of the screen the window is on, or `None`
/// where it cannot be told.
pub fn refresh_mhz(window: &impl HasWindowHandle) -> Option<u32> {
    let raw = window.window_handle().ok()?.as_raw();
    platform::refresh_mhz(raw)
}

/// How many host refreshes make a guest frame: the smallest whole number
/// that brings the screen's rate to the cap or under (90 Hz,
/// `PLAYER_GUEST_HZ_MAX`), so 144 → 2, 120 → 2, 165 → 2, 240 → 3 and
/// 60 → 1; `None` when the screen's rate is not known.
pub fn divisor(host_mhz: Option<u32>) -> Option<u32> {
    let cap = std::env::var("PLAYER_GUEST_HZ_MAX")
        .ok()
        .and_then(|v| v.parse::<f64>().ok())
        .filter(|hz| *hz >= 1.0)
        .map_or(90_000, |hz| (hz * 1000.0) as u32);
    host_mhz.filter(|&m| m >= 1000).map(|host| host.div_ceil(cap))
}

/// The guest's rate, in mHz, for a host screen's: the screen's over
/// [`divisor`] (144 → 72, 120 → 60, 165 → 82.5, 240 → 80, 60 → 60); 60 Hz
/// when the screen's is not known.
pub fn guest_refresh_mhz(host_mhz: Option<u32>) -> u32 {
    match (host_mhz, divisor(host_mhz)) {
        (Some(host), Some(n)) => host / n,
        _ => 60_000,
    }
}

/// The host screen's vertical blank, every Nth one passed to QEMU as a
/// guest frame's start (`Qemu::vblank`, embed API v16; virtio-gpu's
/// `host-vblank`, QEMU patch 90). A thread of its own, which waits on the
/// blank of the screen [`HostVBlank::follow`] last named; it stops when
/// this is dropped. Where the blank cannot be waited on, nothing runs and
/// the guest keeps its own timer.
pub struct HostVBlank {
    shared: Arc<Shared>,
}

struct Shared {
    /// The screen to wait on (its system name), `None` for none.
    screen: Mutex<Option<Vec<u16>>>,
    /// Host blanks per guest frame; 0 for no ticks.
    every: AtomicU32,
    stop: AtomicBool,
}

impl HostVBlank {
    pub fn start(vm: Qemu) -> HostVBlank {
        let shared = Arc::new(Shared {
            screen: Mutex::new(None),
            every: AtomicU32::new(0),
            stop: AtomicBool::new(false),
        });
        if platform::HAS_VBLANK {
            let s = shared.clone();
            std::thread::Builder::new()
                .name("host-vblank".into())
                .spawn(move || platform::vblank_loop(&s, vm))
                .expect("spawn the host-vblank thread");
        }
        HostVBlank { shared }
    }

    /// The screen the window is on, and every how many of its blanks a
    /// guest frame starts ([`divisor`]; `None` for no ticks).
    pub fn follow(&self, window: &impl HasWindowHandle, every: Option<u32>) {
        let screen = window.window_handle().ok().and_then(|h| platform::screen_name(h.as_raw()));
        *self.shared.screen.lock().unwrap() = screen;
        self.shared.every.store(every.unwrap_or(0), Ordering::Relaxed);
    }
}

impl Drop for HostVBlank {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
    }
}

#[cfg(windows)]
mod platform {
    use super::Shared;
    use qemu_embed::Qemu;
    use std::sync::atomic::Ordering;
    use std::time::Duration;
    use wgpu::rwh::RawWindowHandle;
    use windows_sys::Wdk::Graphics::Direct3D::*;
    use windows_sys::Win32::Devices::Display::*;
    use windows_sys::Win32::Graphics::Gdi::*;

    pub const HAS_VBLANK: bool = true;

    /// The GDI name (\\.\DISPLAYn) of the window's monitor, as 32 UTF-16
    /// units with a terminating 0.
    fn monitor_device(raw: RawWindowHandle) -> Option<[u16; 32]> {
        let RawWindowHandle::Win32(h) = raw else { return None };
        let mut mi: MONITORINFOEXW = unsafe { std::mem::zeroed() };
        mi.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
        let monitor = unsafe { MonitorFromWindow(h.hwnd.get() as _, MONITOR_DEFAULTTONEAREST) };
        if unsafe { GetMonitorInfoW(monitor, &mut mi as *mut MONITORINFOEXW as *mut MONITORINFO) } == 0 {
            return None;
        }
        Some(mi.szDevice)
    }

    fn trimmed(name: &[u16]) -> &[u16] {
        &name[..name.iter().position(|&c| c == 0).unwrap_or(name.len())]
    }

    pub fn screen_name(raw: RawWindowHandle) -> Option<Vec<u16>> {
        monitor_device(raw).map(|d| d.to_vec())
    }

    pub fn refresh_mhz(raw: RawWindowHandle) -> Option<u32> {
        // the active display path whose source is the monitor's
        let device = monitor_device(raw)?;
        let (mut np, mut nm) = (0u32, 0u32);
        if unsafe { GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut np, &mut nm) } != 0 {
            return None;
        }
        let mut paths = vec![unsafe { std::mem::zeroed::<DISPLAYCONFIG_PATH_INFO>() }; np as usize];
        let mut modes = vec![unsafe { std::mem::zeroed::<DISPLAYCONFIG_MODE_INFO>() }; nm as usize];
        let r = unsafe {
            QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS,
                &mut np,
                paths.as_mut_ptr(),
                &mut nm,
                modes.as_mut_ptr(),
                std::ptr::null_mut(),
            )
        };
        if r != 0 {
            return None;
        }
        paths.truncate(np as usize);
        paths.iter().find_map(|p| {
            let mut name: DISPLAYCONFIG_SOURCE_DEVICE_NAME = unsafe { std::mem::zeroed() };
            name.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size = std::mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            name.header.adapterId = p.sourceInfo.adapterId;
            name.header.id = p.sourceInfo.id;
            if unsafe { DisplayConfigGetDeviceInfo(&mut name.header) } != 0 {
                return None;
            }
            if trimmed(&name.viewGdiDeviceName) != trimmed(&device) {
                return None;
            }
            let rate = p.targetInfo.refreshRate;
            (rate.Denominator != 0)
                .then(|| (rate.Numerator as u64 * 1000 / rate.Denominator as u64) as u32)
        })
    }

    struct Open {
        name: Vec<u16>,
        adapter: u32,
        source: u32,
    }

    fn close(open: &mut Option<Open>) {
        if let Some(o) = open.take() {
            let c = D3DKMT_CLOSEADAPTER { hAdapter: o.adapter };
            unsafe { D3DKMTCloseAdapter(&c) };
        }
    }

    /// The host-vblank thread: the monitor's adapter, opened again when the
    /// window moves to another screen, and its vertical blank event.
    pub fn vblank_loop(shared: &Shared, vm: Qemu) {
        let mut open: Option<Open> = None;
        let mut count = 0u32;
        while !shared.stop.load(Ordering::Relaxed) {
            let every = shared.every.load(Ordering::Relaxed);
            let want = shared.screen.lock().unwrap().clone();
            let Some(want) = want.filter(|_| every > 0) else {
                close(&mut open);
                std::thread::sleep(Duration::from_millis(100));
                continue;
            };
            if open.as_ref().is_some_and(|o| trimmed(&o.name) != trimmed(&want)) {
                close(&mut open);
            }
            if open.is_none() {
                let mut a: D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME = unsafe { std::mem::zeroed() };
                let n = want.len().min(31);
                a.DeviceName[..n].copy_from_slice(&want[..n]);
                if unsafe { D3DKMTOpenAdapterFromGdiDisplayName(&mut a) } != 0 {
                    std::thread::sleep(Duration::from_millis(500));
                    continue;
                }
                open = Some(Open { name: want, adapter: a.hAdapter, source: a.VidPnSourceId });
            }
            let o = open.as_ref().unwrap();
            let w = D3DKMT_WAITFORVERTICALBLANKEVENT { hAdapter: o.adapter, hDevice: 0, VidPnSourceId: o.source };
            if unsafe { D3DKMTWaitForVerticalBlankEvent(&w) } != 0 {
                // the monitor went (unplugged, the adapter reset): again
                close(&mut open);
                std::thread::sleep(Duration::from_millis(100));
                continue;
            }
            count = count.wrapping_add(1);
            if count % every == 0 {
                vm.vblank();
            }
        }
        close(&mut open);
    }
}

#[cfg(not(windows))]
mod platform {
    use super::Shared;
    use qemu_embed::Qemu;
    use wgpu::rwh::RawWindowHandle;

    pub const HAS_VBLANK: bool = false;

    pub fn refresh_mhz(_raw: RawWindowHandle) -> Option<u32> {
        None
    }
    pub fn screen_name(_raw: RawWindowHandle) -> Option<Vec<u16>> {
        None
    }
    pub fn vblank_loop(_shared: &Shared, _vm: Qemu) {}
}
