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
//! (`D3DKMTWaitForVerticalBlankEvent`); on macOS a `CVDisplayLink` for the
//! window's `NSScreen`, its nominal period (a rational too) and its
//! callback; on Linux the CRTC that lights the screen, through DRM (its
//! mode's exact rate, and `DRM_IOCTL_WAIT_VBLANK`). Elsewhere neither is
//! read: the guest gets 60 Hz, which a 60 Hz screen and a 120 Hz ProMotion
//! one both divide, from its own timer.

use qemu_embed::Qemu;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use wgpu::rwh::{HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle};

/// A window's raw handles: the window's, and its display connection's
/// where it has one (Linux needs it to ask which screen the window is on).
fn raw(window: &(impl HasWindowHandle + HasDisplayHandle)) -> Option<(RawWindowHandle, Option<RawDisplayHandle>)> {
    let w = window.window_handle().ok()?.as_raw();
    Some((w, window.display_handle().ok().map(|d| d.as_raw())))
}

/// The refresh rate, in mHz, of the screen the window is on, or `None`
/// where it cannot be told.
pub fn refresh_mhz(window: &(impl HasWindowHandle + HasDisplayHandle)) -> Option<u32> {
    let (w, d) = raw(window)?;
    platform::refresh_mhz(w, d)
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
    /// The screen to wait on (its system name or number), `None` for none.
    screen: Mutex<Option<platform::Screen>>,
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
    pub fn follow(&self, window: &(impl HasWindowHandle + HasDisplayHandle), every: Option<u32>) {
        let screen = raw(window).and_then(|(w, d)| platform::screen_name(w, d));
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
    use wgpu::rwh::{RawDisplayHandle, RawWindowHandle};
    use windows_sys::Wdk::Graphics::Direct3D::*;
    use windows_sys::Win32::Devices::Display::*;
    use windows_sys::Win32::Graphics::Gdi::*;

    pub const HAS_VBLANK: bool = true;

    /// The monitor's GDI name.
    pub type Screen = Vec<u16>;

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

    pub fn screen_name(raw: RawWindowHandle, _display: Option<RawDisplayHandle>) -> Option<Vec<u16>> {
        monitor_device(raw).map(|d| d.to_vec())
    }

    pub fn refresh_mhz(raw: RawWindowHandle, _display: Option<RawDisplayHandle>) -> Option<u32> {
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

#[cfg(target_os = "macos")]
mod platform {
    use super::Shared;
    use objc2::runtime::AnyObject;
    use objc2::{class, msg_send};
    use qemu_embed::Qemu;
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::time::Duration;
    use wgpu::rwh::{RawDisplayHandle, RawWindowHandle};

    pub const HAS_VBLANK: bool = true;

    /// The screen's `CGDirectDisplayID`.
    pub type Screen = u32;

    type DisplayLink = *mut c_void;

    #[repr(C)]
    struct CVTime {
        time_value: i64,
        time_scale: i32,
        flags: i32,
    }

    /// kCVTimeIsIndefinite
    const TIME_IS_INDEFINITE: i32 = 1;

    type OutputCallback =
        extern "C" fn(DisplayLink, *const c_void, *const c_void, u64, *mut u64, *mut c_void) -> i32;

    #[link(name = "CoreVideo", kind = "framework")]
    extern "C" {
        fn CVDisplayLinkCreateWithCGDisplay(display: u32, link: *mut DisplayLink) -> i32;
        fn CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link: DisplayLink) -> CVTime;
        fn CVDisplayLinkSetOutputCallback(link: DisplayLink, callback: OutputCallback, ctx: *mut c_void) -> i32;
        fn CVDisplayLinkStart(link: DisplayLink) -> i32;
        fn CVDisplayLinkStop(link: DisplayLink) -> i32;
        fn CVDisplayLinkRelease(link: DisplayLink);
    }

    /// The number of the screen the window's view is on (its
    /// `deviceDescription`'s `NSScreenNumber`). On the main thread, as every
    /// caller of `follow` and `refresh_mhz` is.
    pub fn screen_name(raw: RawWindowHandle, _display: Option<RawDisplayHandle>) -> Option<u32> {
        let RawWindowHandle::AppKit(h) = raw else { return None };
        let view = h.ns_view.as_ptr() as *const AnyObject;
        objc2::rc::autoreleasepool(|_| unsafe {
            let window: *const AnyObject = msg_send![view, window];
            if window.is_null() {
                return None;
            }
            let screen: *const AnyObject = msg_send![window, screen];
            if screen.is_null() {
                return None;
            }
            let desc: *const AnyObject = msg_send![screen, deviceDescription];
            let key: *const AnyObject =
                msg_send![class!(NSString), stringWithUTF8String: c"NSScreenNumber".as_ptr()];
            let number: *const AnyObject = msg_send![desc, objectForKey: key];
            if number.is_null() {
                return None;
            }
            let id: u32 = msg_send![number, unsignedIntValue];
            Some(id)
        })
    }

    fn link(display: u32) -> Option<DisplayLink> {
        let mut link = std::ptr::null_mut();
        (unsafe { CVDisplayLinkCreateWithCGDisplay(display, &mut link) } == 0 && !link.is_null()).then_some(link)
    }

    /// The screen's nominal refresh period as Core Video gives it (a
    /// rational, e.g. 1/60 s), turned into a rate.
    pub fn refresh_mhz(raw: RawWindowHandle, display: Option<RawDisplayHandle>) -> Option<u32> {
        let link = link(screen_name(raw, display)?)?;
        let t = unsafe { CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link) };
        unsafe { CVDisplayLinkRelease(link) };
        (t.flags & TIME_IS_INDEFINITE == 0 && t.time_value > 0 && t.time_scale > 0)
            .then(|| (t.time_scale as i64 * 1000 / t.time_value) as u32)
    }

    /// What the display link's callback sees: on Core Video's thread, once
    /// a refresh.
    struct Ticks<'a> {
        vm: Qemu,
        shared: &'a Shared,
        count: AtomicU32,
    }

    extern "C" fn on_refresh(
        _link: DisplayLink,
        _now: *const c_void,
        _output: *const c_void,
        _flags: u64,
        _flags_out: *mut u64,
        ctx: *mut c_void,
    ) -> i32 {
        let t = unsafe { &*(ctx as *const Ticks) };
        let every = t.shared.every.load(Ordering::Relaxed);
        if every > 0 && (t.count.fetch_add(1, Ordering::Relaxed) + 1) % every == 0 {
            t.vm.vblank();
        }
        0
    }

    fn close(open: &mut Option<(u32, DisplayLink)>) {
        if let Some((_, link)) = open.take() {
            // returns once a callback under way has
            unsafe {
                CVDisplayLinkStop(link);
                CVDisplayLinkRelease(link);
            }
        }
    }

    /// The host-vblank thread: a display link for the window's screen,
    /// made again when the window moves to another; the ticks themselves
    /// come on Core Video's own thread.
    pub fn vblank_loop(shared: &Shared, vm: Qemu) {
        let ticks = Ticks { vm, shared, count: AtomicU32::new(0) };
        let mut open: Option<(u32, DisplayLink)> = None;
        while !shared.stop.load(Ordering::Relaxed) {
            let every = shared.every.load(Ordering::Relaxed);
            let want = shared.screen.lock().unwrap().filter(|_| every > 0);
            if open.map(|(d, _)| d) != want {
                close(&mut open);
                if let Some(link) = want.and_then(link) {
                    let ctx = &ticks as *const Ticks as *mut c_void;
                    if unsafe { CVDisplayLinkSetOutputCallback(link, on_refresh, ctx) } == 0
                        && unsafe { CVDisplayLinkStart(link) } == 0
                    {
                        open = Some((want.unwrap(), link));
                    } else {
                        unsafe { CVDisplayLinkRelease(link) };
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        close(&mut open);
    }
}

#[cfg(target_os = "linux")]
mod platform {
    //! DRM, under any compositor or X server: the CRTC that drives the
    //! screen, its mode's exact rate, and `DRM_IOCTL_WAIT_VBLANK` on the
    //! card's primary node, which needs no master (only the seat's access
    //! to `/dev/dri/card*`, which logind gives). Which screen the window is
    //! on is the window system's to say ([`where_shown`]): on Wayland the
    //! output a frame of the window's surface was last shown on, on X11 the
    //! RandR output under the window's middle; matched to a CRTC by the
    //! connector's name (`DP-1`, as `/sys/class/drm` and wlroots, KWin and
    //! Mutter name it) or else its EDID. Until it has said, the only lit
    //! screen is followed, with several none (the guest keeps its timer).
    //! `PLAYER_HOST_SCREEN` names the connector by hand.

    use super::Shared;
    use qemu_embed::Qemu;
    use std::fs::File;
    use std::os::fd::AsRawFd;
    use std::sync::atomic::Ordering;
    use std::time::Duration;
    use wgpu::rwh::{RawDisplayHandle, RawWindowHandle};

    pub const HAS_VBLANK: bool = true;

    /// A lit screen: its card's node, and its CRTC's index on that card.
    #[derive(Clone, PartialEq, Debug)]
    pub struct Screen {
        card: String,
        pipe: u32,
        mhz: u32,
    }

    const fn iowr(nr: u32, size: usize) -> libc::c_ulong {
        ((3 << 30) | ((size as u32) << 16) | ((b'd' as u32) << 8) | nr) as libc::c_ulong
    }

    #[repr(C)]
    #[derive(Default)]
    struct CardRes {
        fb_id_ptr: u64,
        crtc_id_ptr: u64,
        connector_id_ptr: u64,
        encoder_id_ptr: u64,
        count_fbs: u32,
        count_crtcs: u32,
        count_connectors: u32,
        count_encoders: u32,
        min_width: u32,
        max_width: u32,
        min_height: u32,
        max_height: u32,
    }

    #[repr(C)]
    #[derive(Default)]
    struct ModeInfo {
        clock: u32,
        hdisplay: u16,
        hsync_start: u16,
        hsync_end: u16,
        htotal: u16,
        hskew: u16,
        vdisplay: u16,
        vsync_start: u16,
        vsync_end: u16,
        vtotal: u16,
        vscan: u16,
        vrefresh: u32,
        flags: u32,
        kind: u32,
        name: [u8; 32],
    }

    #[repr(C)]
    #[derive(Default)]
    struct Crtc {
        set_connectors_ptr: u64,
        count_connectors: u32,
        crtc_id: u32,
        fb_id: u32,
        x: u32,
        y: u32,
        gamma_size: u32,
        mode_valid: u32,
        mode: ModeInfo,
    }

    #[repr(C)]
    #[derive(Default)]
    struct Connector {
        encoders_ptr: u64,
        modes_ptr: u64,
        props_ptr: u64,
        prop_values_ptr: u64,
        count_modes: u32,
        count_props: u32,
        count_encoders: u32,
        encoder_id: u32,
        connector_id: u32,
        connector_type: u32,
        connector_type_id: u32,
        connection: u32,
        mm_width: u32,
        mm_height: u32,
        subpixel: u32,
        pad: u32,
    }

    #[repr(C)]
    #[derive(Default)]
    struct Encoder {
        encoder_id: u32,
        encoder_type: u32,
        crtc_id: u32,
        possible_crtcs: u32,
        possible_clones: u32,
    }

    /// `union drm_wait_vblank`: the request, and the reply in its place.
    #[repr(C)]
    #[derive(Default)]
    struct WaitVBlank {
        kind: u32,
        sequence: u32,
        tval_sec: i64,
        tval_usec: i64,
    }

    const GETRESOURCES: libc::c_ulong = iowr(0xa0, std::mem::size_of::<CardRes>());
    const GETCRTC: libc::c_ulong = iowr(0xa1, std::mem::size_of::<Crtc>());
    const GETENCODER: libc::c_ulong = iowr(0xa6, std::mem::size_of::<Encoder>());
    const GETCONNECTOR: libc::c_ulong = iowr(0xa7, std::mem::size_of::<Connector>());
    const WAIT_VBLANK: libc::c_ulong = iowr(0x3a, std::mem::size_of::<WaitVBlank>());
    const VBLANK_RELATIVE: u32 = 0x1;
    const VBLANK_HIGH_CRTC_SHIFT: u32 = 1;
    const VBLANK_HIGH_CRTC_MASK: u32 = 0x3e;

    fn ioctl<T>(card: &File, request: libc::c_ulong, arg: &mut T) -> bool {
        loop {
            let r = unsafe { libc::ioctl(card.as_raw_fd(), request as _, arg as *mut T) };
            if r == 0 {
                return true;
            }
            let e = std::io::Error::last_os_error().raw_os_error();
            if e != Some(libc::EINTR) && e != Some(libc::EAGAIN) {
                return false;
            }
        }
    }

    /// The kernel's names for connector types (`drm_connector_enum_list`),
    /// for those a screen can be on.
    fn type_name(kind: u32) -> &'static str {
        match kind {
            1 => "VGA",
            2 => "DVI-I",
            3 => "DVI-D",
            4 => "DVI-A",
            7 => "LVDS",
            10 => "DisplayPort",
            11 => "HDMI-A",
            12 => "HDMI-B",
            14 => "eDP",
            15 => "Virtual",
            16 => "DSI",
            17 => "DPI",
            _ => "Unknown",
        }
    }

    /// The connector's name as the compositor gives it: `DP-1`, `HDMI-A-1`.
    fn connector_name(c: &Connector) -> String {
        let t = match type_name(c.connector_type) {
            "DisplayPort" => "DP",
            t => t,
        };
        format!("{t}-{}", c.connector_type_id)
    }

    /// Every lit screen on one card: its CRTC's index and rate, and the
    /// names of the connectors it drives.
    fn lit(path: &str) -> Vec<(Screen, Vec<String>)> {
        let Ok(card) = File::options().read(true).write(true).open(path) else { return vec![] };
        let mut res = CardRes::default();
        if !ioctl(&card, GETRESOURCES, &mut res) {
            return vec![];
        }
        let mut crtcs = vec![0u32; res.count_crtcs as usize];
        let mut connectors = vec![0u32; res.count_connectors as usize];
        let mut res = CardRes {
            crtc_id_ptr: crtcs.as_mut_ptr() as u64,
            connector_id_ptr: connectors.as_mut_ptr() as u64,
            count_crtcs: crtcs.len() as u32,
            count_connectors: connectors.len() as u32,
            ..Default::default()
        };
        if !ioctl(&card, GETRESOURCES, &mut res) {
            return vec![];
        }
        // which CRTC each connected connector is driven by
        let mut driven: Vec<(u32, String)> = vec![];
        for &id in connectors.iter().take(res.count_connectors as usize) {
            let mut c = Connector { connector_id: id, ..Default::default() };
            if !ioctl(&card, GETCONNECTOR, &mut c) || c.encoder_id == 0 {
                continue;
            }
            let mut e = Encoder { encoder_id: c.encoder_id, ..Default::default() };
            if ioctl(&card, GETENCODER, &mut e) && e.crtc_id != 0 {
                driven.push((e.crtc_id, connector_name(&c)));
            }
        }
        let mut out = vec![];
        for (pipe, &id) in crtcs.iter().take(res.count_crtcs as usize).enumerate() {
            let mut c = Crtc { crtc_id: id, ..Default::default() };
            if !ioctl(&card, GETCRTC, &mut c) || c.mode_valid == 0 {
                continue;
            }
            let m = &c.mode;
            let total = m.htotal as u64 * m.vtotal as u64;
            if m.clock == 0 || total == 0 {
                continue;
            }
            // the pixel clock is in kHz; the rate the exact quotient
            let mut mhz = m.clock as u64 * 1_000_000 / total;
            if m.flags & 0x10 != 0 {
                mhz *= 2; // DRM_MODE_FLAG_INTERLACE: a field a blank
            }
            if m.flags & 0x20 != 0 {
                mhz /= 2; // DRM_MODE_FLAG_DBLSCAN
            }
            let names = driven.iter().filter(|(c, _)| *c == id).map(|(_, n)| n.clone()).collect();
            out.push((Screen { card: path.to_string(), pipe: pipe as u32, mhz: mhz as u32 }, names));
        }
        out
    }

    /// The screen to follow: the one named by `PLAYER_HOST_SCREEN`, the
    /// one the window is on, or, while that is not known, the only one lit
    /// on any card. A window on an output no CRTC drives (a nested
    /// compositor's, a headless one) follows none.
    fn the_screen(on: Option<Output>) -> Option<Screen> {
        let mut cards: Vec<String> = std::fs::read_dir("/dev/dri")
            .ok()?
            .filter_map(|e| e.ok()?.file_name().into_string().ok())
            .filter(|n| n.strip_prefix("card").is_some_and(|d| d.parse::<u32>().is_ok()))
            .map(|n| format!("/dev/dri/{n}"))
            .collect();
        cards.sort();
        let all: Vec<_> = cards.iter().flat_map(|c| lit(c)).collect();
        let named = |want: &str| all.iter().find(|(_, names)| names.iter().any(|n| n == want)).map(|(s, _)| s.clone());
        if let Ok(want) = std::env::var("PLAYER_HOST_SCREEN") {
            return named(&want);
        }
        let Some(on) = on else {
            return (all.len() == 1).then(|| all[0].0.clone());
        };
        if let Some(s) = on.name.as_deref().and_then(named) {
            return Some(s);
        }
        // X drivers name outputs their own way (amdgpu's DisplayPort-0):
        // the EDID, when only one connector has it
        let edid = on.edid.filter(|e| !e.is_empty())?;
        let mut same = all.iter().filter(|(s, names)| {
            let card = s.card.trim_start_matches("/dev/dri/");
            names.iter().any(|n| std::fs::read(format!("/sys/class/drm/{card}-{n}/edid")).is_ok_and(|e| e == edid))
        });
        match (same.next(), same.next()) {
            (Some((s, _)), None) => Some(s.clone()),
            _ => None,
        }
    }

    pub fn screen_name(raw: RawWindowHandle, display: Option<RawDisplayHandle>) -> Option<Screen> {
        the_screen(where_shown(raw, display))
    }

    pub fn refresh_mhz(raw: RawWindowHandle, display: Option<RawDisplayHandle>) -> Option<u32> {
        the_screen(where_shown(raw, display)).map(|s| s.mhz)
    }

    /// An output as the window system names it.
    #[derive(Clone, PartialEq, Debug, Default)]
    struct Output {
        name: Option<String>,
        edid: Option<Vec<u8>>,
    }

    /// What answers for the window: kept from call to call (a Wayland
    /// answer comes with a later frame), made again for another window.
    enum Locator {
        Wayland(usize, wl::Watch),
        X11(x11::Watch),
        None(usize),
    }

    static LOCATOR: std::sync::Mutex<Option<Locator>> = std::sync::Mutex::new(None);
    static LAST: std::sync::Mutex<Option<Output>> = std::sync::Mutex::new(None);

    /// The output the window is on, as far as the window system has said
    /// (on Wayland the last frame shown, so a hidden window keeps its
    /// screen); logged when it changes.
    fn where_shown(raw: RawWindowHandle, display: Option<RawDisplayHandle>) -> Option<Output> {
        let mut locator = LOCATOR.lock().unwrap();
        let on = match (raw, display) {
            (RawWindowHandle::Wayland(w), Some(RawDisplayHandle::Wayland(d))) => {
                let key = w.surface.as_ptr() as usize;
                if !matches!(&*locator, Some(Locator::Wayland(k, _) | Locator::None(k)) if *k == key) {
                    // SAFETY: the window's live surface on its toolkit's
                    // live display, which outlive the window's handle
                    *locator = Some(match unsafe { wl::Watch::new(d.display.as_ptr(), w.surface.as_ptr()) } {
                        Some(watch) => Locator::Wayland(key, watch),
                        None => Locator::None(key),
                    });
                }
                match &mut *locator {
                    Some(Locator::Wayland(_, watch)) => watch.output(),
                    _ => None,
                }
            }
            (RawWindowHandle::Xcb(_) | RawWindowHandle::Xlib(_), _) => {
                let window = match raw {
                    RawWindowHandle::Xcb(h) => h.window.get(),
                    RawWindowHandle::Xlib(h) => h.window as u32,
                    _ => unreachable!(),
                };
                if !matches!(&*locator, Some(Locator::X11(_))) {
                    *locator = x11::Watch::new().map(Locator::X11);
                }
                match &*locator {
                    Some(Locator::X11(watch)) => watch.output(window),
                    _ => None,
                }
            }
            _ => None,
        };
        let mut last = LAST.lock().unwrap();
        if *last != on {
            match on.as_ref().map(|o| o.name.as_deref().unwrap_or("an unnamed output")) {
                Some(name) => eprintln!("[display] the window is on {name}"),
                None => eprintln!("[display] the window's screen is not known"),
            }
            *last = on.clone();
        }
        on
    }

    /// Wayland: `wp_presentation` feedback on the window's surface, whose
    /// `sync_output` names the output a frame was shown on (`wl_output`
    /// version 4's `name`). One request in flight at a time; the compositor
    /// answers with the surface's next frame, so a window dragged to
    /// another screen is placed when it next draws there. Events are read
    /// from the socket by the toolkit and filed under this queue.
    mod wl {
        use super::Output;
        use wayland_backend::client::{Backend, ObjectId};
        use wayland_client::globals::{GlobalListContents, registry_queue_init};
        use wayland_client::protocol::{wl_output, wl_registry, wl_surface};
        use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, delegate_noop};
        use wayland_protocols::wp::presentation_time::client::{wp_presentation, wp_presentation_feedback};

        pub struct Watch {
            conn: Connection,
            queue: EventQueue<State>,
            state: State,
            presentation: wp_presentation::WpPresentation,
            surface: wl_surface::WlSurface,
        }

        #[derive(Default)]
        struct State {
            /// Our bindings of the outputs: global name, proxy, its name.
            outputs: Vec<(u32, wl_output::WlOutput, Option<String>)>,
            asked: bool,
            /// The output named by the frame being answered for.
            synced: Option<String>,
            on: Option<String>,
        }

        impl Watch {
            /// # Safety
            ///
            /// `display` is the toolkit's live `wl_display`, `surface` a live
            /// `wl_surface` on it.
            pub unsafe fn new(display: *mut std::ffi::c_void, surface: *mut std::ffi::c_void) -> Option<Watch> {
                // SAFETY: the caller's.
                let conn = Connection::from_backend(unsafe { Backend::from_foreign_display(display.cast()) });
                let id = unsafe { ObjectId::from_ptr(wl_surface::WlSurface::interface(), surface.cast()) }.ok()?;
                let surface = wl_surface::WlSurface::from_id(&conn, id).ok()?;
                let (globals, mut queue) = registry_queue_init::<State>(&conn).ok()?;
                let qh = queue.handle();
                let presentation = globals.bind(&qh, 1..=1, ()).ok()?;
                let mut state = State::default();
                for g in globals.contents().clone_list() {
                    if g.interface == "wl_output" && g.version >= 4 {
                        state.outputs.push((g.name, globals.registry().bind(g.name, 4, &qh, g.name), None));
                    }
                }
                let _ = queue.roundtrip(&mut state);
                Some(Watch { conn, queue, state, presentation, surface })
            }

            /// The output the window's last answered frame was shown on.
            pub fn output(&mut self) -> Option<Output> {
                if let Err(e) = self.queue.dispatch_pending(&mut self.state) {
                    debug(format_args!("dispatch: {e}"));
                }
                if !self.state.asked {
                    debug(format_args!("asking"));
                    self.presentation.feedback(&self.surface, &self.queue.handle(), ());
                    self.state.asked = true;
                    let _ = self.conn.flush();
                }
                self.state.on.clone().map(|name| Output { name: Some(name), edid: None })
            }
        }

        impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
            fn event(
                state: &mut State,
                registry: &wl_registry::WlRegistry,
                event: wl_registry::Event,
                _: &GlobalListContents,
                _: &Connection,
                qh: &QueueHandle<State>,
            ) {
                match event {
                    wl_registry::Event::Global { name, interface, version } if interface == "wl_output" && version >= 4 => {
                        state.outputs.push((name, registry.bind(name, 4, qh, name), None));
                    }
                    wl_registry::Event::GlobalRemove { name } => {
                        if let Some(i) = state.outputs.iter().position(|(n, _, _)| *n == name) {
                            let (_, output, gone) = state.outputs.remove(i);
                            output.release();
                            // the window's screen unplugged: not known
                            // again until a frame is shown
                            if gone.is_some() && gone == state.on {
                                state.on = None;
                            }
                        }
                    }
                    _ => {}
                }
            }
        }

        impl Dispatch<wl_output::WlOutput, u32> for State {
            fn event(
                state: &mut State,
                output: &wl_output::WlOutput,
                event: wl_output::Event,
                _: &u32,
                _: &Connection,
                _: &QueueHandle<State>,
            ) {
                if let wl_output::Event::Name { name } = event {
                    if let Some(o) = state.outputs.iter_mut().find(|(_, p, _)| p == output) {
                        o.2 = Some(name);
                    }
                }
            }
        }

        impl Dispatch<wp_presentation_feedback::WpPresentationFeedback, ()> for State {
            fn event(
                state: &mut State,
                _: &wp_presentation_feedback::WpPresentationFeedback,
                event: wp_presentation_feedback::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<State>,
            ) {
                debug(format_args!("{event:?}"));
                match event {
                    // once for each of the client's bindings of the output
                    // (the toolkit's too): ours have names
                    wp_presentation_feedback::Event::SyncOutput { output } => {
                        if let Some((_, _, Some(name))) = state.outputs.iter().find(|(_, p, _)| *p == output) {
                            state.synced = Some(name.clone());
                        }
                    }
                    wp_presentation_feedback::Event::Presented { .. } => {
                        state.on = state.synced.take().or(state.on.take());
                        state.asked = false;
                    }
                    wp_presentation_feedback::Event::Discarded => {
                        state.synced = None;
                        state.asked = false;
                    }
                    _ => {}
                }
            }
        }

        delegate_noop!(State: ignore wp_presentation::WpPresentation);

        /// `PLAYER_SCREEN_LOG=1`: the feedback's events.
        fn debug(what: std::fmt::Arguments) {
            if std::env::var_os("PLAYER_SCREEN_LOG").is_some() {
                eprintln!("[screen] {what}");
            }
        }
    }

    /// X11: RandR's CRTC under the window's middle, its first output's name
    /// and EDID, on a connection of our own.
    mod x11 {
        use super::Output;
        use x11rb::connection::Connection;
        use x11rb::protocol::randr::ConnectionExt as _;
        use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _};
        use x11rb::rust_connection::RustConnection;

        pub struct Watch {
            conn: RustConnection,
            root: u32,
            edid: u32,
        }

        impl Watch {
            pub fn new() -> Option<Watch> {
                let (conn, screen) = x11rb::connect(None).ok()?;
                let root = conn.setup().roots.get(screen)?.root;
                let edid = conn.intern_atom(false, b"EDID").ok()?.reply().ok()?.atom;
                Some(Watch { conn, root, edid })
            }

            pub fn output(&self, window: u32) -> Option<Output> {
                let c = &self.conn;
                let geo = c.get_geometry(window).ok()?.reply().ok()?;
                let at = c.translate_coordinates(window, self.root, 0, 0).ok()?.reply().ok()?;
                let (mx, my) = (at.dst_x as i32 + geo.width as i32 / 2, at.dst_y as i32 + geo.height as i32 / 2);
                let res = c.randr_get_screen_resources_current(self.root).ok()?.reply().ok()?;
                for crtc in res.crtcs {
                    let Some(info) = c.randr_get_crtc_info(crtc, res.config_timestamp).ok().and_then(|r| r.reply().ok())
                    else {
                        continue;
                    };
                    let (x, y, w, h) = (info.x as i32, info.y as i32, info.width as i32, info.height as i32);
                    if info.mode == 0 || mx < x || my < y || mx >= x + w || my >= y + h {
                        continue;
                    }
                    let out = *info.outputs.first()?;
                    let name = c
                        .randr_get_output_info(out, res.config_timestamp)
                        .ok()
                        .and_then(|r| r.reply().ok())
                        .map(|o| String::from_utf8_lossy(&o.name).into_owned());
                    let edid = c
                        .randr_get_output_property(out, self.edid, AtomEnum::ANY, 0, 256, false, false)
                        .ok()
                        .and_then(|r| r.reply().ok())
                        .map(|p| p.data);
                    return Some(Output { name, edid });
                }
                None
            }
        }
    }

    /// The host-vblank thread: the screen's card, opened again when the
    /// screen changes, and its CRTC's vertical blank, one at a time.
    pub fn vblank_loop(shared: &Shared, vm: Qemu) {
        let mut open: Option<(Screen, File)> = None;
        let mut count = 0u32;
        while !shared.stop.load(Ordering::Relaxed) {
            let every = shared.every.load(Ordering::Relaxed);
            let want = shared.screen.lock().unwrap().clone().filter(|_| every > 0);
            let Some(want) = want else {
                open = None;
                std::thread::sleep(Duration::from_millis(100));
                continue;
            };
            if open.as_ref().is_none_or(|(s, _)| *s != want) {
                open = File::options().read(true).write(true).open(&want.card).ok().map(|f| (want.clone(), f));
                if open.is_none() {
                    std::thread::sleep(Duration::from_millis(500));
                    continue;
                }
            }
            let (screen, card) = open.as_ref().unwrap();
            let mut w = WaitVBlank {
                kind: VBLANK_RELATIVE | ((screen.pipe << VBLANK_HIGH_CRTC_SHIFT) & VBLANK_HIGH_CRTC_MASK),
                sequence: 1,
                ..Default::default()
            };
            if !ioctl(card, WAIT_VBLANK, &mut w) {
                // the screen is off (blanked, unplugged): again in a while,
                // the guest's own timer filling in meanwhile
                open = None;
                std::thread::sleep(Duration::from_millis(100));
                continue;
            }
            count = count.wrapping_add(1);
            if count % every == 0 {
                vm.vblank();
            }
        }
    }
}

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
mod platform {
    use super::Shared;
    use qemu_embed::Qemu;
    use wgpu::rwh::{RawDisplayHandle, RawWindowHandle};

    pub const HAS_VBLANK: bool = false;

    pub type Screen = ();

    pub fn refresh_mhz(_raw: RawWindowHandle, _display: Option<RawDisplayHandle>) -> Option<u32> {
        None
    }
    pub fn screen_name(_raw: RawWindowHandle, _display: Option<RawDisplayHandle>) -> Option<()> {
        None
    }
    pub fn vblank_loop(_shared: &Shared, _vm: Qemu) {}
}
