//! The host's keyboard shortcuts go to the guest while the player window
//! has focus: the Windows key opens the *guest's* Start menu, and Super+1
//! reaches the game instead of switching the host's workspace.
//!
//! winit has no keyboard grab, so this is one piece per windowing system:
//!
//! - **Wayland**: `zwp_keyboard_shortcuts_inhibit_manager_v1`. One
//!   inhibitor for the window's whole life. The compositor applies it only
//!   while the surface has keyboard focus, which is exactly the rule wanted,
//!   so focus changes need nothing from us. The compositor may still keep a
//!   binding of its own (sway's `bindsym --inhibited`, GNOME asks the user
//!   once), which is the user's way out and not ours to take.
//! - **X11**: an active `XGrabKeyboard` while focused. An active grab beats
//!   the window manager's passive ones on Super.
//! - **Windows**: the keyboard registered for raw input with
//!   `RIDEV_NOHOTKEYS`, which stops the shell acting on the two Windows keys
//!   (and so on every Win+ shortcut) while a window of this process is the
//!   foreground one. The key still arrives at the window as an ordinary
//!   `WM_KEYDOWN`, so winit delivers it and the guest gets it down the same
//!   path as every other key. This takes the key from the shell without
//!   taking it from us, and it needs no hook, no thread and no injection.
//!   **Ctrl+Esc comes with them.** It is a keyboard hotkey the shell acts
//!   on, so it stops with the rest and opens the *guest's* Start menu. It
//!   does not cover what Windows calls a *system* hotkey (Alt+Tab, Alt+Esc,
//!   Alt+F4, Alt+Space, Ctrl+Alt+Del, Win+L), which no program gets. Hence
//!   the player's Ctrl+Alt+Shift+D for Ctrl+Alt+Del, and Alt+F4 asking
//!   before it stops the machine.
//!
//!   **This used to be a `WH_KEYBOARD_LL` hook, and the hook never
//!   worked.** Measured on the user's PC, a low-level keyboard hook in the
//!   player is called for every key on the machine *except* while the
//!   player's own window is the foreground one, the only time it is
//!   wanted: zero calls, not late ones, with the hook installed and its
//!   thread answering. A hook in a third process saw those same keys, and a
//!   minimal program with a window and a hook of its own saw its own
//!   window's keys, so it is neither a Windows rule nor that window. What in
//!   this process does it was never found, and `RIDEV_NOHOTKEYS` makes the
//!   question moot. docs/03-display-pipeline.md, "Input path".
//! - **macOS**: the window server's hot key operating mode set to "all
//!   disabled except Universal Access" while the window has focus
//!   (`CGSSetGlobalHotKeyOperatingMode`, SkyLight's private interface, the
//!   call VirtualBox and UTM make for their keyboard capture; UTM ships it
//!   on the Mac App Store, and it works inside the App Sandbox): Cmd+Tab,
//!   Cmd+Space, Mission Control and the Spaces arrows (Ctrl+Up, which era
//!   games use), the screenshot chords and every other binding of the
//!   Keyboard settings pane stay out of the way and the keys arrive at the
//!   window as ordinary `keyDown:`s. The mode does not follow focus by
//!   itself, so the player sets it on focus and resets it on focus loss
//!   and on drop. The Universal Access chords are left on: accessibility
//!   is never the guest's. Two roads that do not work on macOS 26, both
//!   measured: the public Carbon `PushSymbolicHotKeyMode` is a stub (its
//!   own mode reads back as pushed while the window server's stays 0), and
//!   an active HID event tap with the Accessibility permission never sees
//!   these chords at all, sandboxed or not (the window server acts on them
//!   before any tap). `PLAYER_KEYBOARD_MAC=presentation` picks the public
//!   `NSApplicationPresentationOptions` instead, kept as the fallback
//!   should App Store review ever refuse the symbol: `disableProcessSwitching`
//!   (Cmd+Tab) and `disableHideApplication` (Cmd+H), which want the Dock
//!   auto-hidden while the app is active and cover nothing else. Cmd+H,
//!   Cmd+Opt+H and Cmd+Q are not hot keys but the app menu's key
//!   equivalents (winit's menu), which the menu would swallow before the
//!   window sees them; the capture blanks them for its life and gives them
//!   back on drop. **A Cmd+Q that reaches the window asks** (`main.rs`,
//!   like Alt+F4): a hand that meant Win+Q loses nothing, one that meant
//!   Quit gets the prompt. What stays the host's: Cmd+Opt+Esc (Force Quit)
//!   and the fn / Touch Bar media keys, which are not hot keys of the pane.
//!
//!   **Cmd+Q never `exit()`s under a live QEMU thread.** winit's Quit item
//!   is `terminate:`, and its delegate answers no `applicationShouldTerminate:`,
//!   so AppKit would `exit()` from inside the run loop while the QEMU thread
//!   is alive (the atexit race `main.rs` joins the thread to avoid). The
//!   player adds that method to winit's delegate class at start
//!   (`quit_closes_window`): it closes the key window instead, which is the
//!   title bar's close, and cancels the terminate. The Dock's Quit and the
//!   menu's take the same road.
//!
//! **Ctrl+Alt+K** hands them back to the host and, pressed again, to the
//! guest again. The player drops the `Capture` and builds a new one, so
//! "off" is exactly the state before it was made. `PLAYER_KEYBOARD_CAPTURE=0`
//! starts a run with them the host's, and **`PLAYER_KEYBOARD_LOG=1`** makes
//! the Windows side say what it did: that the registration was accepted, and
//! what winit and Windows each thought about focus at every change. A
//! shortcut that still reaches the host is nearly always a window that was
//! not in front when it was pressed.

use qemu_embed::Qemu;
#[cfg(all(unix, not(target_os = "macos")))]
use winit::raw_window_handle::RawDisplayHandle;
use winit::raw_window_handle::RawWindowHandle;
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use winit::window::Window;

pub enum Capture {
    #[cfg(all(unix, not(target_os = "macos")))]
    Wayland(wl::Inhibit),
    #[cfg(all(unix, not(target_os = "macos")))]
    X11(x11::Grab),
    #[cfg(windows)]
    Windows(win::NoHotkeys),
    #[cfg(target_os = "macos")]
    MacOS(mac::HotKeys),
}

impl Capture {
    /// `None` on a host this cannot do anything on (the reason is printed).
    pub fn new(window: &Window, vm: Qemu) -> Option<Capture> {
        let _ = &vm; // every platform's keys reach the guest the ordinary way
        let (w, d) = match (window.window_handle(), window.display_handle()) {
            (Ok(w), Ok(d)) => (w.as_raw(), d.as_raw()),
            _ => {
                eprintln!("[keyboard] host shortcuts stay the host's: the window has no handle yet");
                return None;
            }
        };
        let made: Result<Capture, String> = match (w, d) {
            #[cfg(all(unix, not(target_os = "macos")))]
            (RawWindowHandle::Wayland(w), RawDisplayHandle::Wayland(d)) => unsafe {
                wl::Inhibit::new(d.display.as_ptr(), w.surface.as_ptr()).map(Capture::Wayland)
            },
            #[cfg(all(unix, not(target_os = "macos")))]
            (RawWindowHandle::Xlib(w), RawDisplayHandle::Xlib(d)) => match d.display {
                Some(p) => x11::Grab::new(p.as_ptr(), w.window).map(Capture::X11),
                None => Err("no Xlib display".into()),
            },
            #[cfg(windows)]
            (RawWindowHandle::Win32(w), _) => win::NoHotkeys::new(w.hwnd.get()).map(Capture::Windows),
            #[cfg(target_os = "macos")]
            (RawWindowHandle::AppKit(_), _) => mac::HotKeys::new().map(Capture::MacOS),
            _ => Err("nothing to do on this windowing system".into()),
        };
        match made {
            Ok(c) => Some(c),
            Err(e) => {
                eprintln!("[keyboard] host shortcuts stay the host's: {e}");
                None
            }
        }
    }

    pub fn set_focused(&mut self, focused: bool) {
        match *self {
            #[cfg(all(unix, not(target_os = "macos")))]
            Capture::Wayland(ref mut i) => {
                let _ = focused; // the compositor follows focus itself
                i.poll();
            }
            #[cfg(all(unix, not(target_os = "macos")))]
            Capture::X11(ref mut g) => g.set(focused),
            #[cfg(windows)]
            Capture::Windows(ref mut h) => h.set(focused),
            #[cfg(target_os = "macos")]
            Capture::MacOS(ref mut h) => h.set(focused),
        }
    }
}

/// macOS: Cmd+Q (and the Dock's / the menu's Quit) closes the window
/// instead of terminating the process. Once per run, before the capture.
#[cfg(target_os = "macos")]
pub fn quit_closes_window() {
    if let Err(e) = mac::quit_closes_window() {
        eprintln!("[keyboard] Cmd+Q keeps terminating the process: {e}");
    }
}

#[cfg(all(unix, not(target_os = "macos")))]
mod wl {
    use std::ffi::c_void;
    use wayland_backend::client::{Backend, ObjectId};
    use wayland_client::globals::{registry_queue_init, GlobalListContents};
    use wayland_client::protocol::{wl_registry, wl_seat, wl_surface};
    use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
    use wayland_protocols::wp::keyboard_shortcuts_inhibit::zv1::client::{
        zwp_keyboard_shortcuts_inhibit_manager_v1::ZwpKeyboardShortcutsInhibitManagerV1 as Manager,
        zwp_keyboard_shortcuts_inhibitor_v1::{self as inhibitor, ZwpKeyboardShortcutsInhibitorV1 as Inhibitor},
    };

    #[derive(Default)]
    pub struct State {
        said: bool,
    }

    /// The inhibitor, on a queue of our own over winit's connection: the
    /// surface is winit's, the protocol objects are ours.
    pub struct Inhibit {
        conn: Connection,
        queue: EventQueue<State>,
        state: State,
        inhibitor: Inhibitor,
    }

    impl Inhibit {
        /// # Safety
        /// `display` and `surface` are the live `wl_display` / `wl_surface`
        /// of a window that outlives the returned value.
        pub unsafe fn new(display: *mut c_void, surface: *mut c_void) -> Result<Self, String> {
            let conn = Connection::from_backend(Backend::from_foreign_display(display.cast()));
            let (globals, queue) = registry_queue_init::<State>(&conn).map_err(|e| e.to_string())?;
            let qh = queue.handle();
            let manager: Manager = globals
                .bind(&qh, 1..=1, ())
                .map_err(|_| "the compositor offers no keyboard-shortcuts-inhibit protocol".to_string())?;
            let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=1, ()).map_err(|e| format!("no seat: {e}"))?;
            let id = ObjectId::from_ptr(wl_surface::WlSurface::interface(), surface.cast())
                .map_err(|e| format!("window surface: {e}"))?;
            let surface = wl_surface::WlSurface::from_id(&conn, id).map_err(|e| format!("window surface: {e}"))?;
            let inhibitor = manager.inhibit_shortcuts(&surface, &seat, &qh, ());
            conn.flush().map_err(|e| e.to_string())?;
            Ok(Inhibit { conn, queue, state: State::default(), inhibitor })
        }

        /// Handle what the compositor said: libwayland files the events
        /// under our queue when winit reads the socket.
        pub fn poll(&mut self) {
            let _ = self.queue.dispatch_pending(&mut self.state);
        }
    }

    impl Drop for Inhibit {
        fn drop(&mut self) {
            self.inhibitor.destroy();
            let _ = self.conn.flush();
        }
    }

    impl Dispatch<Inhibitor, ()> for State {
        fn event(state: &mut Self, _: &Inhibitor, event: inhibitor::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
            if let inhibitor::Event::Active = event {
                if !state.said {
                    state.said = true;
                    eprintln!("[keyboard] the compositor passes its shortcuts to the guest while the window has focus");
                }
            }
        }
    }

    impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
        fn event(
            _: &mut Self,
            _: &wl_registry::WlRegistry,
            _: wl_registry::Event,
            _: &GlobalListContents,
            _: &Connection,
            _: &QueueHandle<Self>,
        ) {
        }
    }

    impl Dispatch<wl_seat::WlSeat, ()> for State {
        fn event(_: &mut Self, _: &wl_seat::WlSeat, _: wl_seat::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
    }

    impl Dispatch<Manager, ()> for State {
        fn event(_: &mut Self, _: &Manager, _: <Manager as Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
    }
}

#[cfg(all(unix, not(target_os = "macos")))]
mod x11 {
    use std::ffi::{c_ulong, c_void};
    use x11_dl::xlib;

    pub struct Grab {
        xlib: xlib::Xlib,
        display: *mut xlib::Display,
        window: c_ulong,
        held: bool,
        said: bool,
    }

    impl Grab {
        pub fn new(display: *mut c_void, window: c_ulong) -> Result<Self, String> {
            let xlib = xlib::Xlib::open().map_err(|e| e.to_string())?;
            Ok(Grab { xlib, display: display.cast(), window, held: false, said: false })
        }

        pub fn set(&mut self, on: bool) {
            unsafe {
                if on && !self.held {
                    let r = (self.xlib.XGrabKeyboard)(
                        self.display,
                        self.window,
                        xlib::True,
                        xlib::GrabModeAsync,
                        xlib::GrabModeAsync,
                        xlib::CurrentTime,
                    );
                    self.held = r == xlib::GrabSuccess;
                    // AlreadyGrabbed: the window manager was mid-grab (an
                    // Alt+Tab) when focus arrived; the next focus tries again
                    if !self.held && !self.said {
                        self.said = true;
                        eprintln!("[keyboard] XGrabKeyboard refused ({r}): host shortcuts stay the host's this time");
                    }
                } else if !on && self.held {
                    (self.xlib.XUngrabKeyboard)(self.display, xlib::CurrentTime);
                    self.held = false;
                }
                (self.xlib.XFlush)(self.display);
            }
        }
    }

    impl Drop for Grab {
        fn drop(&mut self) {
            self.set(false);
        }
    }
}

#[cfg(windows)]
mod win {
    use windows_sys::Win32::Foundation::GetLastError;
    use windows_sys::Win32::UI::Input::{RegisterRawInputDevices, RAWINPUTDEVICE};
    use windows_sys::Win32::UI::WindowsAndMessaging::GetForegroundWindow;

    /// The HID usage a PC keyboard is: Generic Desktop page, Keyboard.
    const USAGE_PAGE_GENERIC: u16 = 0x01;
    const USAGE_KEYBOARD: u16 = 0x06;
    /// `RIDEV_NOHOTKEYS`: while a window of this process is the foreground
    /// one, the shell does not act on the keyboard's hotkeys.
    const RIDEV_NOHOTKEYS: u32 = 0x0000_0200;
    /// `RIDEV_REMOVE`: give the usage back. It wants a null target window.
    const RIDEV_REMOVE: u32 = 0x0000_0001;

    /// The keyboard registered for raw input with `RIDEV_NOHOTKEYS`.
    ///
    /// Nothing here follows focus: the flag is a property of *being the
    /// foreground window*, which is the rule this wants, applied by the
    /// window manager rather than by us. The key still arrives at the
    /// window as an ordinary `WM_KEYDOWN`, so winit delivers it and the
    /// guest gets it down the same path as every other key. This takes
    /// the key away from the shell without taking it away from us.
    pub struct NoHotkeys {
        trace: bool,
        window: isize,
    }

    impl NoHotkeys {
        pub fn new(hwnd: isize) -> Result<Self, String> {
            register(RIDEV_NOHOTKEYS, hwnd)?;
            let trace = std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1");
            if trace {
                eprintln!(
                    "[keyboard] the Windows keys are the guest's while window {hwnd:#x} is in front"
                );
            }
            Ok(NoHotkeys { trace, window: hwnd })
        }

        /// Focus changes nothing here; the line is worth printing because a
        /// shortcut that still reached the host is nearly always a window
        /// that was not in front when it was pressed.
        pub fn set(&mut self, focused: bool) {
            if self.trace {
                eprintln!(
                    "[keyboard] focus: winit says {focused}, foreground {:#x}, our window {:#x}",
                    unsafe { GetForegroundWindow() },
                    self.window,
                );
            }
        }
    }

    impl Drop for NoHotkeys {
        fn drop(&mut self) {
            let _ = register(RIDEV_REMOVE, 0);
        }
    }

    /// Register (or unregister) the keyboard usage for this process.
    ///
    /// It replaces winit's own registration of that usage, which winit made
    /// for `DeviceEvent::Key`; the player reads only `DeviceEvent::MouseMotion`,
    /// a different usage, and that one is left alone.
    fn register(flags: u32, hwnd: isize) -> Result<(), String> {
        let rid = RAWINPUTDEVICE {
            usUsagePage: USAGE_PAGE_GENERIC,
            usUsage: USAGE_KEYBOARD,
            dwFlags: flags,
            hwndTarget: hwnd,
        };
        let ok = unsafe {
            RegisterRawInputDevices(&rid, 1, std::mem::size_of::<RAWINPUTDEVICE>() as u32)
        };
        if ok == 0 {
            return Err(format!("RegisterRawInputDevices failed ({})", unsafe {
                GetLastError()
            }));
        }
        Ok(())
    }
}

#[cfg(target_os = "macos")]
mod mac {
    use objc2::rc::Retained;
    use objc2::runtime::{AnyClass, AnyObject, Sel};
    use objc2::{sel, MainThreadMarker, Message};
    use objc2_app_kit::{NSApplication, NSApplicationPresentationOptions as Opts, NSMenuItem};
    use objc2_foundation::{ns_string, NSString};

    // The window server's hot key operating mode, SkyLight's private
    // interface: the call VirtualBox (`DarwinDisableGlobalHotKeys`) and
    // UTM (`VMMetalView.captureMouse`, on the Mac App Store) make for
    // their keyboard capture. The public Carbon `PushSymbolicHotKeyMode`
    // no longer reaches it: on macOS 26 its mode reads back as pushed
    // while this one stays 0, and Cmd+Tab keeps switching. An event tap,
    // Accessibility granted, never sees these chords at all: the window
    // server acts on them first (both measured 2026-09-24).
    #[link(name = "ApplicationServices", kind = "framework")]
    extern "C" {
        fn CGSMainConnectionID() -> i32;
        fn CGSGetGlobalHotKeyOperatingMode(conn: i32, mode: *mut i32) -> i32;
        fn CGSSetGlobalHotKeyOperatingMode(conn: i32, mode: i32) -> i32;
    }
    const HOT_KEYS_ENABLED: i32 = 0;
    /// Every binding of the Keyboard settings pane off, the accessibility
    /// ones kept.
    const HOT_KEYS_DISABLED_EXCEPT_UNIVERSAL_ACCESS: i32 = 2;

    /// `NSApplicationTerminateReply`
    const NS_TERMINATE_CANCEL: usize = 0;
    const NS_TERMINATE_NOW: usize = 1;

    enum Way {
        /// The window server's mode, set while the window has focus and
        /// reset when it loses it (the mode does not follow focus itself).
        Cgs { conn: i32 },
        /// The public route, kept in case App Store review ever refuses
        /// the symbol (`PLAYER_KEYBOARD_MAC=presentation`):
        /// `disableProcessSwitching` needs the Dock auto-hidden and covers
        /// Cmd+Tab and Cmd+H only. AppKit applies presentation options
        /// while the app is active.
        Presentation { before: Opts },
    }

    /// The host's hot keys off for the capture's life, and the app menu's
    /// key equivalents blanked.
    pub struct HotKeys {
        way: Way,
        /// The menu items whose key equivalent was taken, with what to give back.
        taken: Vec<(Retained<NSMenuItem>, Retained<NSString>)>,
        trace: bool,
    }

    fn cgs_mode(conn: i32) -> i32 {
        let mut mode = -1;
        unsafe { CGSGetGlobalHotKeyOperatingMode(conn, &mut mode) };
        mode
    }

    impl HotKeys {
        pub fn new() -> Result<Self, String> {
            let mtm = MainThreadMarker::new().ok_or("not on the main thread")?;
            let trace = std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1");
            let app = NSApplication::sharedApplication(mtm);
            let way = if std::env::var("PLAYER_KEYBOARD_MAC").as_deref() != Ok("presentation") {
                let conn = unsafe { CGSMainConnectionID() };
                let mode = cgs_mode(conn);
                if mode != HOT_KEYS_ENABLED {
                    // sleep, the screen saver, or another app's capture
                    return Err(format!("the window server's hot key mode is {mode}, not enabled"));
                }
                Way::Cgs { conn }
            } else {
                let before = app.presentationOptions();
                let mut want = before | Opts::DisableProcessSwitching | Opts::DisableHideApplication;
                if !want.contains(Opts::HideDock) {
                    want |= Opts::AutoHideDock;
                }
                app.setPresentationOptions(want);
                Way::Presentation { before }
            };
            // The menu's key equivalents (winit's app menu: Cmd+H, Cmd+Opt+H,
            // Cmd+Q) are dispatched before the window's keyDown: and would
            // never reach the guest. Blank every one the menu has.
            let mut taken = Vec::new();
            if let Some(menu) = app.mainMenu() {
                for top in menu.itemArray().iter() {
                    let Some(sub) = top.submenu() else { continue };
                    for item in sub.itemArray().iter() {
                        let key = item.keyEquivalent();
                        if key.len() == 0 {
                            continue;
                        }
                        item.setKeyEquivalent(ns_string!(""));
                        taken.push((item.retain(), key));
                    }
                }
            }
            let mut made = HotKeys { way, taken, trace };
            if trace {
                eprintln!(
                    "[keyboard] {} while focused, {} menu key equivalents taken",
                    match made.way {
                        Way::Cgs { .. } => "the window server's hot keys off",
                        Way::Presentation { .. } => "process switching and hiding off (presentation options)",
                    },
                    made.taken.len()
                );
            }
            made.set(app.isActive());
            Ok(made)
        }

        pub fn set(&mut self, focused: bool) {
            if let Way::Cgs { conn } = self.way {
                let want = if focused { HOT_KEYS_DISABLED_EXCEPT_UNIVERSAL_ACCESS } else { HOT_KEYS_ENABLED };
                unsafe { CGSSetGlobalHotKeyOperatingMode(conn, want) };
            }
            if self.trace {
                let active = MainThreadMarker::new()
                    .map(|mtm| NSApplication::sharedApplication(mtm).isActive())
                    .unwrap_or(false);
                let mode = match self.way {
                    Way::Cgs { conn } => format!("hot key mode {}", cgs_mode(conn)),
                    Way::Presentation { .. } => "presentation options".to_string(),
                };
                eprintln!(
                    "[keyboard] focus: winit says {focused}, the app is {}, {mode}",
                    if active { "active" } else { "not active" },
                );
            }
        }
    }

    impl Drop for HotKeys {
        fn drop(&mut self) {
            for (item, key) in self.taken.drain(..) {
                item.setKeyEquivalent(&key);
            }
            match self.way {
                Way::Cgs { conn } => unsafe {
                    CGSSetGlobalHotKeyOperatingMode(conn, HOT_KEYS_ENABLED);
                },
                Way::Presentation { before } => {
                    if let Some(mtm) = MainThreadMarker::new() {
                        NSApplication::sharedApplication(mtm).setPresentationOptions(before);
                    }
                }
            }
        }
    }

    /// `applicationShouldTerminate:` for winit's delegate: close the key
    /// window (winit turns `windowShouldClose:` into `CloseRequested`, the
    /// title bar's close) and cancel the terminate, so the process never
    /// `exit()`s under the QEMU thread. With no window left there is
    /// nothing to stop and the terminate proceeds.
    unsafe extern "C-unwind" fn should_terminate(
        _this: *mut AnyObject,
        _cmd: Sel,
        _sender: *mut AnyObject,
    ) -> usize {
        let Some(mtm) = MainThreadMarker::new() else { return NS_TERMINATE_NOW };
        let app = NSApplication::sharedApplication(mtm);
        let window = app.keyWindow().or_else(|| app.windows().firstObject());
        if std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1") {
            eprintln!(
                "[keyboard] terminate asked: {}",
                if window.is_some() { "closing the window instead" } else { "no window, letting it" }
            );
        }
        match window {
            Some(w) => {
                w.performClose(None);
                NS_TERMINATE_CANCEL
            }
            None => NS_TERMINATE_NOW,
        }
    }

    pub fn quit_closes_window() -> Result<(), String> {
        let mtm = MainThreadMarker::new().ok_or("not on the main thread")?;
        let app = NSApplication::sharedApplication(mtm);
        let delegate = app.delegate().ok_or("the app has no delegate")?;
        let object: &AnyObject = (*delegate).as_ref();
        let class: &AnyClass = object.class();
        let imp: unsafe extern "C-unwind" fn(*mut AnyObject, Sel, *mut AnyObject) -> usize = should_terminate;
        // SAFETY: the IMP has the method's real signature, and the type
        // encoding says so (NSUInteger, self, _cmd, id).
        let added = unsafe {
            objc2::ffi::class_addMethod(
                class as *const AnyClass as *mut AnyClass,
                sel!(applicationShouldTerminate:),
                std::mem::transmute::<_, unsafe extern "C-unwind" fn()>(imp),
                c"Q@:@".as_ptr(),
            )
        };
        if !added.as_bool() {
            return Err(format!("{} already answers applicationShouldTerminate:", class.name().to_string_lossy()));
        }
        if std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1") {
            eprintln!("[keyboard] Cmd+Q and Quit close the window ({} answers applicationShouldTerminate:)", class.name().to_string_lossy());
        }
        Ok(())
    }
}
