//! Bindings for `embed/libqemu_embed.h`. Hand-written: the API is ours and
//! small; `api_version()` guards against header/binding drift.
//!
//! Thread contract (see the header): [`Qemu::new`], [`Qemu::run`] and drop
//! happen on one dedicated thread; display callbacks fire on that thread
//! with the BQL held and must not block; everything else is `Send + Sync`.

use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;

pub const API_VERSION: u32 = 12;

/// The system emulator this build links (`qemu-x86_64` feature: Windows
/// 11, `qemu-aarch64`: Windows 11 on Arm; track M20), and so QEMU's own
/// name for itself.
pub const QEMU_NAME: &str = if cfg!(feature = "qemu-x86_64") {
    "qemu-system-x86_64"
} else if cfg!(feature = "qemu-aarch64") {
    "qemu-system-aarch64"
} else {
    "qemu-system-i386"
};
pub const FMT_XRGB8888: u32 = 1;

#[repr(C)]
pub struct RawDisplayCb {
    pub on_switch: Option<unsafe extern "C" fn(*mut c_void, *const u8, c_int, c_int, c_int, u32)>,
    pub on_update: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int, c_int)>,
    pub on_refresh_done: Option<unsafe extern "C" fn(*mut c_void)>,
    pub on_cursor:
        Option<unsafe extern "C" fn(*mut c_void, *const u32, c_int, c_int, c_int, c_int)>,
    pub on_mouse_set: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, bool)>,
    /// v4: qemu-3dfx pass-through on/off
    pub on_3d_active: Option<unsafe extern "C" fn(*mut c_void, bool)>,
    /// v4: presented 3D frame (XRGB8888 top-down, stride bytes), valid during the call
    pub on_3d_frame:
        Option<unsafe extern "C" fn(*mut c_void, *const u8, c_int, c_int, c_int)>,
    /// v5 (Linux): a ring slot's dma-buf is offered (fd owned by the callee); return nonzero to accept
    pub on_3d_dmabuf: Option<
        unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int, c_int, c_int, u32, u64) -> c_int,
    >,
    /// v5: the frame in the given slot is complete
    pub on_3d_frame_ready: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    /// v6 (macOS): a ring slot backed by an IOSurface (pointer stays valid while offered); return nonzero to accept
    pub on_3d_iosurface:
        Option<unsafe extern "C" fn(*mut c_void, c_int, *mut c_void, c_int, c_int) -> c_int>,
    /// v12: the updates just delivered were pushed by the device outside a
    /// refresh tick (a guest flush) and complete a frame
    pub on_flush: Option<unsafe extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct qemu_embed_t {
    _private: [u8; 0],
}

// On Windows rustc writes the imports itself (`raw-dylib`), so no import
// library is needed: mingw's `.dll.a` is one MSVC's link.exe would not
// take, and the player links QEMU's mingw DLL from either toolchain
// (ADR-026, doc 11 "The C runtime boundary"). Still a load-time import,
// never a run-time open (patch 63). Elsewhere build.rs links it.
#[cfg_attr(
    all(windows, not(any(feature = "qemu-x86_64", feature = "qemu-aarch64"))),
    link(name = "libqemu-embed-i386", kind = "raw-dylib")
)]
#[cfg_attr(
    all(windows, feature = "qemu-x86_64"),
    link(name = "libqemu-embed-x86_64", kind = "raw-dylib")
)]
#[cfg_attr(
    all(windows, feature = "qemu-aarch64", not(feature = "qemu-x86_64")),
    link(name = "libqemu-embed-aarch64", kind = "raw-dylib")
)]
extern "C" {
    fn qemu_embed_api_version() -> u32;
    fn qemu_embed_new(
        argc: c_int,
        argv: *mut *mut c_char,
        cb: *const RawDisplayCb,
        ud: *mut c_void,
    ) -> *mut qemu_embed_t;
    fn qemu_embed_run(e: *mut qemu_embed_t) -> c_int;
    fn qemu_embed_destroy(e: *mut qemu_embed_t, status: c_int);
    fn qemu_embed_vm_start(e: *mut qemu_embed_t);
    fn qemu_embed_vm_pause(e: *mut qemu_embed_t);
    fn qemu_embed_vm_reset(e: *mut qemu_embed_t);
    fn qemu_embed_vm_powerdown(e: *mut qemu_embed_t);
    fn qemu_embed_vm_shutdown(e: *mut qemu_embed_t);
    fn qemu_embed_vm_running(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_key(e: *mut qemu_embed_t, qcode: u32, down: bool);
    fn qemu_embed_atset1_to_qcode(atset1: u32) -> u32;
    fn qemu_embed_mouse_rel(e: *mut qemu_embed_t, dx: c_int, dy: c_int);
    fn qemu_embed_mouse_abs(e: *mut qemu_embed_t, x: c_int, y: c_int, w: c_int, h: c_int);
    fn qemu_embed_mouse_btn(e: *mut qemu_embed_t, button: u32, down: bool);
    fn qemu_embed_mouse_is_absolute(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_pad_state(e: *mut qemu_embed_t, axes: *const u8, hat: u32, buttons: u32);
    fn qemu_embed_pad_present(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_input_flush(e: *mut qemu_embed_t);
    fn qemu_embed_set_refresh_ms(e: *mut qemu_embed_t, ms: u32);
    fn qemu_embed_set_window_size(e: *mut qemu_embed_t, w: u32, h: u32, dpi: u32);
    fn qemu_embed_set_clipboard_cb(
        e: *mut qemu_embed_t,
        f: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ud: *mut c_void,
    );
    fn qemu_embed_clipboard_set_text(e: *mut qemu_embed_t, utf8: *const c_char, len: usize);
    fn qemu_embed_display_follows_window(e: *mut qemu_embed_t) -> bool;
    fn qemu_embed_set_audio_ring(
        base: *mut c_void,
        bytes: usize,
        wr_idx: *mut u32,
        rd_idx: *const u32,
    );
    fn qemu_embed_socket_to_fd(sock: u64) -> c_int;
    fn qemu_embed_setenv(name: *const c_char, value: *const c_char) -> bool;
}

/// A socket the caller owns, as the `fd=` of `-chardev socket,fd=N`.
///
/// Pass a raw fd on Unix and a raw `SOCKET` on Windows; the value that comes
/// back is what QEMU's own C runtime will resolve, and it owns the socket
/// afterwards. `None` if the conversion failed. The conversion lives in the
/// library because on Windows `fd=` is a CRT descriptor and the descriptor
/// table belongs to whichever CRT a module links.
pub fn socket_to_fd(sock: u64) -> Option<i32> {
    let fd = unsafe { qemu_embed_socket_to_fd(sock) };
    (fd >= 0).then_some(fd)
}

/// Set an environment variable where QEMU, our devices and the Direct3D
/// executor read it (v10), and in the process's environment as well.
///
/// Not `std::env::set_var`: they read it with their C runtime's
/// `getenv()`, which on Windows answers from a copy made when the process
/// started, and `set_var` (`SetEnvironmentVariableW`) never updates that
/// copy. The library sets it on its own runtime's side. Call before
/// [`Qemu::new`], while no other thread exists. `false` if it failed or a
/// string holds a NUL.
///
/// # Safety
/// As `std::env::set_var`: no other thread may be reading or writing the
/// environment.
pub unsafe fn setenv(name: &str, value: &std::ffi::OsStr) -> bool {
    let (Ok(name), Some(Ok(value))) = (CString::new(name), value.to_str().map(CString::new)) else {
        return false;
    };
    qemu_embed_setenv(name.as_ptr(), value.as_ptr())
}

/// Install the audio ring. Must be called BEFORE [`Qemu::new`] on any
/// thread; then configure `-audiodev embed,id=...,out.format=f32,...` (any
/// format works; the player uses f32 because s16 saturates an over-range mix).
///
/// # Safety
/// `base`/`wr`/`rd` must stay valid for the process lifetime; `bytes` is a
/// power of two; the consumer only writes `rd`, QEMU only writes `wr`.
pub unsafe fn set_audio_ring(base: *mut u8, bytes: usize, wr: *mut u32, rd: *const u32) {
    qemu_embed_set_audio_ring(base as *mut c_void, bytes, wr, rd)
}

/// Library API version; must equal [`API_VERSION`].
pub fn api_version() -> u32 {
    unsafe { qemu_embed_api_version() }
}

/// AT set-1 scancode (0xE0-prefixed as 0xE0xx) → QKeyCode, 0 if unmapped.
pub fn atset1_to_qcode(atset1: u32) -> u32 {
    unsafe { qemu_embed_atset1_to_qcode(atset1) }
}

/// Handle to the in-process VM. Cheap to copy; the underlying object lives
/// until [`Qemu::run`] returns and the owner calls [`Qemu::destroy`].
#[derive(Clone, Copy)]
pub struct Qemu(*mut qemu_embed_t);

// The C side is documented thread-safe for everything except new/run/destroy,
// which the owning thread performs via the `Owner` token.
unsafe impl Send for Qemu {}
unsafe impl Sync for Qemu {}

/// Proof that the caller is on the QEMU thread; only it can run/destroy.
pub struct Owner(Qemu);

impl Qemu {
    /// Initialize QEMU on the *current* thread. `args` is a qemu-system
    /// argument list without argv[0]. `cb`/`ud` receive display callbacks.
    ///
    /// # Safety
    /// `ud` must stay valid until `destroy`; callbacks must uphold the header
    /// contract. Fatal configuration errors terminate the process (QEMU).
    pub unsafe fn new(
        args: &[String],
        cb: &RawDisplayCb,
        ud: *mut c_void,
    ) -> Option<(Qemu, Owner)> {
        assert_eq!(
            api_version(),
            API_VERSION,
            "libqemu-embed API version mismatch"
        );
        let mut cargs: Vec<CString> = Vec::with_capacity(args.len() + 1);
        cargs.push(CString::new(QEMU_NAME).unwrap());
        for a in args {
            cargs.push(CString::new(a.as_str()).ok()?);
        }
        let mut argv: Vec<*mut c_char> = cargs.iter().map(|c| c.as_ptr() as *mut c_char).collect();
        argv.push(ptr::null_mut());
        let e = qemu_embed_new(cargs.len() as c_int, argv.as_mut_ptr(), cb, ud);
        if e.is_null() {
            return None;
        }
        let q = Qemu(e);
        Some((q, Owner(q)))
    }

    pub fn vm_start(&self) {
        unsafe { qemu_embed_vm_start(self.0) }
    }
    pub fn vm_pause(&self) {
        unsafe { qemu_embed_vm_pause(self.0) }
    }
    pub fn vm_reset(&self) {
        unsafe { qemu_embed_vm_reset(self.0) }
    }
    pub fn vm_powerdown(&self) {
        unsafe { qemu_embed_vm_powerdown(self.0) }
    }
    pub fn vm_shutdown(&self) {
        unsafe { qemu_embed_vm_shutdown(self.0) }
    }
    pub fn vm_running(&self) -> bool {
        unsafe { qemu_embed_vm_running(self.0) }
    }
    pub fn key(&self, qcode: u32, down: bool) {
        unsafe { qemu_embed_key(self.0, qcode, down) }
    }
    pub fn mouse_rel(&self, dx: i32, dy: i32) {
        unsafe { qemu_embed_mouse_rel(self.0, dx, dy) }
    }
    pub fn mouse_abs(&self, x: i32, y: i32, w: i32, h: i32) {
        unsafe { qemu_embed_mouse_abs(self.0, x, y, w, h) }
    }
    pub fn mouse_btn(&self, button: u32, down: bool) {
        unsafe { qemu_embed_mouse_btn(self.0, button, down) }
    }
    pub fn mouse_is_absolute(&self) -> bool {
        unsafe { qemu_embed_mouse_is_absolute(self.0) }
    }
    /// The whole gamepad at once (v8, M13 path A). Four axes (X, Y, Z,
    /// Rz, `0x80` centred), a hat of 0..7 clockwise from north (8 =
    /// released) and a bitmap of twelve buttons.
    ///
    /// Absolute state, not events, so a dropped update is corrected by
    /// the next one rather than leaving the guest holding a button. A
    /// no-op on a machine without `-device usb-gamepad`.
    pub fn pad_state(&self, axes: [u8; 4], hat: u8, buttons: u16) {
        unsafe { qemu_embed_pad_state(self.0, axes.as_ptr(), hat as u32, buttons as u32) }
    }
    /// Whether the machine has a `usb-gamepad` for [`Self::pad_state`] to
    /// reach, so the player can say "this machine has no gamepad
    /// device" rather than sending into nothing.
    pub fn pad_present(&self) -> bool {
        unsafe { qemu_embed_pad_present(self.0) }
    }
    pub fn input_flush(&self) {
        unsafe { qemu_embed_input_flush(self.0) }
    }
    /// Display refresh pull interval (ms); QEMU's default is 30.
    pub fn set_refresh_ms(&self, ms: u32) {
        unsafe { qemu_embed_set_refresh_ms(self.0, ms) }
    }
    /// The window's drawable size in pixels and its DPI (v9, M20), for an
    /// adapter that takes the window's size as its mode (virtio-gpu with
    /// Windows' viogpudo). Every other adapter ignores it.
    pub fn set_window_size(&self, w: u32, h: u32, dpi: u32) {
        unsafe { qemu_embed_set_window_size(self.0, w, h, dpi) }
    }
    /// Whether the adapter on show takes [`Self::set_window_size`], so
    /// the window should not be held to the guest's mode (v9).
    pub fn display_follows_window(&self) -> bool {
        unsafe { qemu_embed_display_follows_window(self.0) }
    }
    /// Hear the guest's clipboard text (v11, track M23), through QEMU's
    /// `qemu-vdagent`. `f` runs on QEMU's thread with the BQL held: it
    /// must not block. One handler per process, set once after `new`.
    pub fn set_clipboard_handler(&self, f: Box<dyn Fn(String) + Send + Sync>) {
        unsafe extern "C" fn tramp(ud: *mut c_void, p: *const c_char, len: usize) {
            let f = unsafe { &*(ud as *const Box<dyn Fn(String) + Send + Sync>) };
            let bytes = unsafe { std::slice::from_raw_parts(p as *const u8, len) };
            f(String::from_utf8_lossy(bytes).into_owned());
        }
        // one VM per process: the handler lives as long as it
        let ud = Box::into_raw(Box::new(f)) as *mut c_void;
        unsafe { qemu_embed_set_clipboard_cb(self.0, Some(tramp), ud) }
    }
    /// Offer the host's clipboard text to the guest (v11). Any thread.
    pub fn set_clipboard_text(&self, text: &str) {
        unsafe { qemu_embed_clipboard_set_text(self.0, text.as_ptr() as *const c_char, text.len()) }
    }
}

impl Owner {
    /// Run the main loop on this thread; returns QEMU's exit status.
    pub fn run(&self) -> i32 {
        unsafe { qemu_embed_run(self.0 .0) }
    }
    /// Tear down (one VM per process lifetime, QEMU cleanup is partial).
    pub fn destroy(self, status: i32) {
        unsafe { qemu_embed_destroy(self.0 .0, status) }
    }
}
