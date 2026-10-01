//! Bindings for `embed/libqemu_embed.h`. Hand-written: the API is ours and
//! small; `api_version()` guards against header/binding drift.
//!
//! **Which QEMU is a run-time choice** (track M20). Every system emulator
//! builds its own `libqemu-embed-<target>` with the same API: `i386` for
//! the era's machines, `x86_64` for Windows 11. The player is one binary
//! and opens the one its machine needs with [`load`], before anything
//! else here is called; nothing links a QEMU at build time.
//!
//! Thread contract (see the header): [`Qemu::new`], [`Qemu::run`] and drop
//! happen on one dedicated thread; display callbacks fire on that thread
//! with the BQL held and must not block; everything else is `Send + Sync`.

use std::ffi::{c_char, c_int, c_void, CString};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::OnceLock;

pub const API_VERSION: u32 = 8;
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
}

#[repr(C)]
pub struct qemu_embed_t {
    _private: [u8; 0],
}

type E = *mut qemu_embed_t;

/// The library's functions, resolved once by [`load`].
struct Api {
    target: String,
    api_version: unsafe extern "C" fn() -> u32,
    new: unsafe extern "C" fn(c_int, *mut *mut c_char, *const RawDisplayCb, *mut c_void) -> E,
    run: unsafe extern "C" fn(E) -> c_int,
    destroy: unsafe extern "C" fn(E, c_int),
    vm_start: unsafe extern "C" fn(E),
    vm_pause: unsafe extern "C" fn(E),
    vm_reset: unsafe extern "C" fn(E),
    vm_powerdown: unsafe extern "C" fn(E),
    vm_shutdown: unsafe extern "C" fn(E),
    vm_running: unsafe extern "C" fn(E) -> bool,
    key: unsafe extern "C" fn(E, u32, bool),
    atset1_to_qcode: unsafe extern "C" fn(u32) -> u32,
    mouse_rel: unsafe extern "C" fn(E, c_int, c_int),
    mouse_abs: unsafe extern "C" fn(E, c_int, c_int, c_int, c_int),
    mouse_btn: unsafe extern "C" fn(E, u32, bool),
    mouse_is_absolute: unsafe extern "C" fn(E) -> bool,
    pad_state: unsafe extern "C" fn(E, *const u8, u32, u32),
    pad_present: unsafe extern "C" fn(E) -> bool,
    input_flush: unsafe extern "C" fn(E),
    set_refresh_ms: unsafe extern "C" fn(E, u32),
    set_audio_ring: unsafe extern "C" fn(*mut c_void, usize, *mut u32, *const u32),
    socket_to_fd: unsafe extern "C" fn(u64) -> c_int,
}

static API: OnceLock<Api> = OnceLock::new();

fn api() -> &'static Api {
    API.get().expect("qemu_embed::load() was not called")
}

/// The library's file name for a target: `libqemu-embed-x86_64.so`.
pub fn library_name(target: &str) -> String {
    let ext = if cfg!(windows) {
        "dll"
    } else if cfg!(target_os = "macos") {
        "dylib"
    } else {
        "so"
    };
    format!("libqemu-embed-{target}.{ext}")
}

/// Where a library is looked for, in order:
///
/// 1. `QEMU_EMBED_LIB_DIR`, for a developer pointing at another build.
/// 2. `<exe>/../lib/2ksbox`, the installed layout on Linux and in the
///    macOS app (doc 07; `scripts/package-*.sh`).
/// 3. `<exe>`'s own directory, the Windows package's layout.
/// 4. The QEMU build directory this crate was compiled against
///    (`build.rs`), so a checkout's `target/release/player` runs in place.
///
/// The package directories come first, so a player copied out of a
/// developer's `target/` into a package never loads the build tree's.
pub fn search_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Ok(d) = std::env::var("QEMU_EMBED_LIB_DIR") {
        dirs.push(PathBuf::from(d));
    }
    if let Some(exe_dir) = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)) {
        dirs.push(exe_dir.join("../lib/2ksbox"));
        dirs.push(exe_dir);
    }
    dirs.push(PathBuf::from(env!("QEMU_EMBED_BUILD_DIR")));
    dirs
}

/// The first `libqemu-embed-<target>` in [`search_dirs`], the one [`load`]
/// would open.
pub fn find(target: &str) -> Option<PathBuf> {
    let name = library_name(target);
    search_dirs().into_iter().map(|d| d.join(&name)).find(|p| p.is_file())
}

/// Open `libqemu-embed-<target>` and resolve its functions. Call once,
/// before anything else in this crate; a second call with the same
/// target is a no-op, with another target an error (one QEMU per
/// process). The library stays loaded for the life of the process.
pub fn load(target: &str) -> Result<(), String> {
    if let Some(a) = API.get() {
        return if a.target == target {
            Ok(())
        } else {
            Err(format!("QEMU for {} already loaded, not {target}", a.target))
        };
    }
    let Some(path) = find(target) else {
        let tried: Vec<String> = search_dirs().iter().map(|d| d.display().to_string()).collect();
        return Err(format!("no {} in {}", library_name(target), tried.join(", ")));
    };
    // Never unloaded: QEMU's atexit handlers and threads point into it.
    let lib: &'static libloading::Library = Box::leak(Box::new(
        unsafe { libloading::Library::new(&path) }.map_err(|e| format!("{}: {e}", path.display()))?,
    ));
    macro_rules! sym {
        ($name:literal) => {
            *unsafe { lib.get(concat!($name, "\0").as_bytes()) }
                .map_err(|e| format!("{}: {}: {e}", path.display(), $name))?
        };
    }
    let a = Api {
        target: target.to_string(),
        api_version: sym!("qemu_embed_api_version"),
        new: sym!("qemu_embed_new"),
        run: sym!("qemu_embed_run"),
        destroy: sym!("qemu_embed_destroy"),
        vm_start: sym!("qemu_embed_vm_start"),
        vm_pause: sym!("qemu_embed_vm_pause"),
        vm_reset: sym!("qemu_embed_vm_reset"),
        vm_powerdown: sym!("qemu_embed_vm_powerdown"),
        vm_shutdown: sym!("qemu_embed_vm_shutdown"),
        vm_running: sym!("qemu_embed_vm_running"),
        key: sym!("qemu_embed_key"),
        atset1_to_qcode: sym!("qemu_embed_atset1_to_qcode"),
        mouse_rel: sym!("qemu_embed_mouse_rel"),
        mouse_abs: sym!("qemu_embed_mouse_abs"),
        mouse_btn: sym!("qemu_embed_mouse_btn"),
        mouse_is_absolute: sym!("qemu_embed_mouse_is_absolute"),
        pad_state: sym!("qemu_embed_pad_state"),
        pad_present: sym!("qemu_embed_pad_present"),
        input_flush: sym!("qemu_embed_input_flush"),
        set_refresh_ms: sym!("qemu_embed_set_refresh_ms"),
        set_audio_ring: sym!("qemu_embed_set_audio_ring"),
        socket_to_fd: sym!("qemu_embed_socket_to_fd"),
    };
    let version = unsafe { (a.api_version)() };
    if version != API_VERSION {
        return Err(format!(
            "{}: embed API {version}, this player wants {API_VERSION} (rebuild QEMU and the player together)",
            path.display()
        ));
    }
    let _ = API.set(a);
    Ok(())
}

/// The target [`load`] opened, if it has run.
pub fn loaded_target() -> Option<&'static str> {
    API.get().map(|a| a.target.as_str())
}

/// A socket the caller owns, as the `fd=` of `-chardev socket,fd=N`.
///
/// Pass a raw fd on Unix and a raw `SOCKET` on Windows; the value that comes
/// back is what QEMU's own C runtime will resolve, and it owns the socket
/// afterwards. `None` if the conversion failed. The conversion lives in the
/// library because on Windows `fd=` is a CRT descriptor and the descriptor
/// table belongs to whichever CRT a module links.
pub fn socket_to_fd(sock: u64) -> Option<i32> {
    let fd = unsafe { (api().socket_to_fd)(sock) };
    (fd >= 0).then_some(fd)
}

/// Install the audio ring. Must be called BEFORE [`Qemu::new`] on any
/// thread; then configure `-audiodev embed,id=...,out.format=f32,...` (any
/// format works; the player uses f32 because s16 saturates an over-range mix).
///
/// # Safety
/// `base`/`wr`/`rd` must stay valid for the process lifetime; `bytes` is a
/// power of two; the consumer only writes `rd`, QEMU only writes `wr`.
pub unsafe fn set_audio_ring(base: *mut u8, bytes: usize, wr: *mut u32, rd: *const u32) {
    (api().set_audio_ring)(base as *mut c_void, bytes, wr, rd)
}

/// Library API version; must equal [`API_VERSION`] ([`load`] checks it).
pub fn api_version() -> u32 {
    unsafe { (api().api_version)() }
}

/// AT set-1 scancode (0xE0-prefixed as 0xE0xx) → QKeyCode, 0 if unmapped.
pub fn atset1_to_qcode(atset1: u32) -> u32 {
    unsafe { (api().atset1_to_qcode)(atset1) }
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
        let a = api();
        let mut cargs: Vec<CString> = Vec::with_capacity(args.len() + 1);
        cargs.push(CString::new(format!("qemu-system-{}", a.target)).unwrap());
        for arg in args {
            cargs.push(CString::new(arg.as_str()).ok()?);
        }
        let mut argv: Vec<*mut c_char> = cargs.iter().map(|c| c.as_ptr() as *mut c_char).collect();
        argv.push(ptr::null_mut());
        let e = (a.new)(cargs.len() as c_int, argv.as_mut_ptr(), cb, ud);
        if e.is_null() {
            return None;
        }
        let q = Qemu(e);
        Some((q, Owner(q)))
    }

    pub fn vm_start(&self) {
        unsafe { (api().vm_start)(self.0) }
    }
    pub fn vm_pause(&self) {
        unsafe { (api().vm_pause)(self.0) }
    }
    pub fn vm_reset(&self) {
        unsafe { (api().vm_reset)(self.0) }
    }
    pub fn vm_powerdown(&self) {
        unsafe { (api().vm_powerdown)(self.0) }
    }
    pub fn vm_shutdown(&self) {
        unsafe { (api().vm_shutdown)(self.0) }
    }
    pub fn vm_running(&self) -> bool {
        unsafe { (api().vm_running)(self.0) }
    }
    pub fn key(&self, qcode: u32, down: bool) {
        unsafe { (api().key)(self.0, qcode, down) }
    }
    pub fn mouse_rel(&self, dx: i32, dy: i32) {
        unsafe { (api().mouse_rel)(self.0, dx, dy) }
    }
    pub fn mouse_abs(&self, x: i32, y: i32, w: i32, h: i32) {
        unsafe { (api().mouse_abs)(self.0, x, y, w, h) }
    }
    pub fn mouse_btn(&self, button: u32, down: bool) {
        unsafe { (api().mouse_btn)(self.0, button, down) }
    }
    pub fn mouse_is_absolute(&self) -> bool {
        unsafe { (api().mouse_is_absolute)(self.0) }
    }
    /// The whole gamepad at once (v8, M13 path A). Four axes (X, Y, Z,
    /// Rz, `0x80` centred), a hat of 0..7 clockwise from north (8 =
    /// released) and a bitmap of twelve buttons.
    ///
    /// Absolute state, not events, so a dropped update is corrected by
    /// the next one rather than leaving the guest holding a button. A
    /// no-op on a machine without `-device usb-gamepad`.
    pub fn pad_state(&self, axes: [u8; 4], hat: u8, buttons: u16) {
        unsafe { (api().pad_state)(self.0, axes.as_ptr(), hat as u32, buttons as u32) }
    }
    /// Whether the machine has a `usb-gamepad` for [`Self::pad_state`] to
    /// reach, so the player can say "this machine has no gamepad
    /// device" rather than sending into nothing.
    pub fn pad_present(&self) -> bool {
        unsafe { (api().pad_present)(self.0) }
    }
    pub fn input_flush(&self) {
        unsafe { (api().input_flush)(self.0) }
    }
    /// Display refresh pull interval (ms); QEMU's default is 30.
    pub fn set_refresh_ms(&self, ms: u32) {
        unsafe { (api().set_refresh_ms)(self.0, ms) }
    }
}

impl Owner {
    /// Run the main loop on this thread; returns QEMU's exit status.
    pub fn run(&self) -> i32 {
        unsafe { (api().run)(self.0 .0) }
    }
    /// Tear down (one VM per process lifetime, QEMU cleanup is partial).
    pub fn destroy(self, status: i32) {
        unsafe { (api().destroy)(self.0 .0, status) }
    }
}
