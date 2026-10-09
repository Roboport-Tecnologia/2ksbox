//! The host screen's refresh rate, and the guest's that follows it (track
//! M24, step 4 A). The guest's vertical blank (our viogpudo) at a rate that
//! divides the screen's keeps every guest frame the same number of host
//! refreshes: 72 on a 144 Hz screen was the user's "very smooth", where 60
//! there alternates two and three refreshes a frame.
//!
//! The screen's rate is read where the system says it exactly: on Windows
//! the display path of the window's monitor (`QueryDisplayConfig`, a
//! rational such as 143.981 Hz). Elsewhere it is not read yet and the guest
//! gets 60 Hz, which a 60 Hz screen and a 120 Hz ProMotion one both divide.

use wgpu::rwh::HasWindowHandle;

/// The refresh rate, in mHz, of the screen the window is on, or `None`
/// where it cannot be told.
pub fn refresh_mhz(window: &impl HasWindowHandle) -> Option<u32> {
    let raw = window.window_handle().ok()?.as_raw();
    platform::refresh_mhz(raw)
}

/// The guest's rate, in mHz, for a host screen's: the screen's divided by
/// the smallest whole number that brings it to the cap or under (90 Hz,
/// `PLAYER_GUEST_HZ_MAX`), so 144 → 72, 120 → 60, 165 → 82.5, 240 → 80 and
/// 60 → 60; 60 Hz when the screen's is not known.
pub fn guest_refresh_mhz(host_mhz: Option<u32>) -> u32 {
    let cap = std::env::var("PLAYER_GUEST_HZ_MAX")
        .ok()
        .and_then(|v| v.parse::<f64>().ok())
        .filter(|hz| *hz >= 1.0)
        .map_or(90_000, |hz| (hz * 1000.0) as u32);
    match host_mhz.filter(|&m| m >= 1000) {
        Some(host) => host / host.div_ceil(cap),
        None => 60_000,
    }
}

#[cfg(windows)]
mod platform {
    use wgpu::rwh::RawWindowHandle;
    use windows_sys::Win32::Devices::Display::*;
    use windows_sys::Win32::Graphics::Gdi::*;

    pub fn refresh_mhz(raw: RawWindowHandle) -> Option<u32> {
        let RawWindowHandle::Win32(h) = raw else { return None };
        // the monitor's GDI name (\\.\DISPLAYn), then the active display
        // path whose source has that name
        let mut mi: MONITORINFOEXW = unsafe { std::mem::zeroed() };
        mi.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
        let monitor = unsafe { MonitorFromWindow(h.hwnd.get() as _, MONITOR_DEFAULTTONEAREST) };
        if unsafe { GetMonitorInfoW(monitor, &mut mi as *mut MONITORINFOEXW as *mut MONITORINFO) } == 0 {
            return None;
        }
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
        let device = &mi.szDevice[..mi.szDevice.iter().position(|&c| c == 0).unwrap_or(32)];
        paths.iter().find_map(|p| {
            let mut name: DISPLAYCONFIG_SOURCE_DEVICE_NAME = unsafe { std::mem::zeroed() };
            name.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size = std::mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            name.header.adapterId = p.sourceInfo.adapterId;
            name.header.id = p.sourceInfo.id;
            if unsafe { DisplayConfigGetDeviceInfo(&mut name.header) } != 0 {
                return None;
            }
            let n = &name.viewGdiDeviceName;
            if &n[..n.iter().position(|&c| c == 0).unwrap_or(32)] != device {
                return None;
            }
            let rate = p.targetInfo.refreshRate;
            (rate.Denominator != 0)
                .then(|| (rate.Numerator as u64 * 1000 / rate.Denominator as u64) as u32)
        })
    }
}

#[cfg(not(windows))]
mod platform {
    pub fn refresh_mhz(_raw: wgpu::rwh::RawWindowHandle) -> Option<u32> {
        None
    }
}
