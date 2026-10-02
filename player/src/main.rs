//! 2ksbox player (doc 02): one running machine per process, in a winit
//! window.
//!
//! `player -- <qemu-system args>` boots QEMU in-process and presents the
//! guest framebuffer through wgpu and the CRT shader chain; keyboard,
//! mouse and pad are injected. No args → the test pattern. Everything but
//! the window is `player-core`'s, shared with the mitsuami player
//! (`player-mitsuami/`, track M22).

mod kbcapture;
mod keymap;
mod prompt;

use player_core::{Gpu, Input, MinSize, Qemu, Session};
use std::sync::Arc;
use winit::application::ApplicationHandler;
use winit::dpi::LogicalSize;
use winit::event::{
    DeviceEvent, DeviceId, ElementState, MouseButton, MouseScrollDelta, WindowEvent,
};
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};
use winit::keyboard::{KeyCode, ModifiersState, PhysicalKey};
use winit::window::{Cursor, CursorGrabMode, CustomCursor, Fullscreen, Window, WindowId};

struct App {
    args: player_core::Args,
    window: Option<Arc<Window>>,
    gpu: Option<Gpu>,
    session: Option<Session>,
    input: Input,
    modifiers: ModifiersState,
    grabbed: bool,
    /// Keys held whose host keymap moved them (`keymap::as_host_reads`):
    /// the physical key and the one the guest was sent for it.
    moved: Vec<(KeyCode, KeyCode)>,
    /// The host's shortcuts to the guest while the window has focus
    /// (`kbcapture`): the Windows key is the guest's.
    kbd: Option<kbcapture::Capture>,
    /// Ctrl+Alt+K turned that off (or `PLAYER_KEYBOARD_CAPTURE=0` started
    /// the run with it off): the host keeps its shortcuts.
    kbd_off: bool,
    /// The close prompt (`prompt.rs`) while a keyboard close waits for an
    /// answer; nothing reaches the guest meanwhile.
    confirm_close: Option<prompt::Prompt>,
    /// Wakes the event loop from the QEMU thread when a frame is published.
    proxy: Option<EventLoopProxy<()>>,
    /// The guest's hardware cursor (the d3dpt-vga driver, doc 15) as a host
    /// cursor: shown with the guest's shape while the pointer is over the
    /// image and the guest shows it, hidden when the guest hides it. With
    /// the USB tablet the host pointer is where the guest cursor is, so
    /// nothing is composited. A guest without one (a software pointer in
    /// the framebuffer) keeps the host cursor hidden over the image.
    guest_cursor: Option<CustomCursor>,
    guest_cursor_seq: u64,
    /// A fully transparent cursor, and the only way the player hides one.
    /// winit's own `set_cursor_visible(false)` builds its invisible cursor by
    /// decoding a 16x16 GIF, which on macOS is ImageIO, and ImageIO
    /// `dlopen`s its codecs by leaf name, so `/opt/homebrew/lib` on
    /// `DYLD_LIBRARY_PATH` (which is how a dev checkout finds the Vulkan
    /// loader) hands it Homebrew's `libgif` for its own `libGIF.dylib` on a
    /// case-insensitive filesystem: the decode then branches through a
    /// poisoned pointer and the player dies of SIGBUS on the first grab
    /// A cursor built from raw RGBA never reaches ImageIO.
    blank_cursor: Option<CustomCursor>,
    /// The pointer is over the image (CursorMoved inside the viewport).
    pointer_inside: bool,
    /// What the window's cursor was last set to (wakes come every frame).
    cursor_applied: HostCursor,
}

/// The host window's cursor state the player last applied.
#[derive(Default, PartialEq, Clone, Copy)]
enum HostCursor {
    #[default]
    Default,
    Hidden,
    /// the guest's shape of this sequence number
    Guest(u64),
}

impl App {
    fn vm(&self) -> Option<Qemu> {
        self.session.as_ref().and_then(Session::vm)
    }

    /// Release every key the guest still sees as held (focus loss).
    fn lift_all_keys(&mut self) {
        self.moved.clear();
        let vm = self.vm();
        self.input.lift_all(vm);
    }

    /// What the picture wants of the window's minimum size, applied
    /// (`Gpu::take_min_size`). Clamped to the monitor, or a mode larger
    /// than the screen would ask for a window that cannot be placed.
    fn apply_min_size(&mut self) {
        let (Some(gpu), Some(window)) = (self.gpu.as_mut(), self.window.as_ref()) else { return };
        let Some(min) = gpu.take_min_size() else { return };
        let (mut mw, mut mh) = match min {
            MinSize::Logical(w, h) => {
                window.set_min_inner_size(Some(LogicalSize::new(w, h)));
                return;
            }
            MinSize::Physical(w, h) => (w, h),
        };
        if let Some(mon) = window.current_monitor() {
            let s = mon.size();
            if s.width > 0 && s.height > 0 {
                mw = mw.min(s.width);
                mh = mh.min(s.height);
            }
        }
        window.set_min_inner_size(Some(winit::dpi::PhysicalSize::new(mw, mh)));
        // A window already smaller than the new floor is not grown by the
        // minimum alone on every platform; ask for it.
        let (w, h) = gpu.surface_px();
        if w < mw || h < mh {
            let _ = window.request_inner_size(winit::dpi::PhysicalSize::new(w.max(mw), h.max(mh)));
        }
    }

    /// The window's drawable size and DPI to the guest's adapter
    /// (`Session::tell_window_size`).
    fn tell_window_size(&self) {
        let (Some(session), Some(gpu), Some(window)) = (&self.session, &self.gpu, &self.window) else { return };
        let size = window.inner_size();
        session.tell_window_size(gpu, (size.width, size.height), window.scale_factor());
    }

    /// Alt+F4 and the like: ask before pulling the plug. Nothing reaches
    /// the guest while the question is up (the grab let go, the keys the
    /// guest holds, such as the Alt of Alt+F4, lifted) and the pointer is the
    /// host's, to click with.
    fn ask_to_close(&mut self) {
        self.set_grab(false);
        self.lift_all_keys();
        self.confirm_close = Some(prompt::Prompt::new());
        self.show_prompt();
        self.apply_cursor();
    }

    /// (Re)draw the prompt, at the size the window has now.
    fn show_prompt(&mut self) {
        let (Some(p), Some(gpu), Some(window)) = (self.confirm_close.as_mut(), self.gpu.as_mut(), &self.window) else {
            return;
        };
        p.fit(gpu.surface_px(), window.scale_factor());
        let (w, h) = p.size();
        gpu.set_overlay(Some((&p.render(), w, h)));
        window.request_redraw();
    }

    fn dismiss_prompt(&mut self) {
        self.confirm_close = None;
        if let Some(gpu) = self.gpu.as_mut() {
            gpu.set_overlay(None);
        }
        if let Some(window) = &self.window {
            window.request_redraw();
        }
        self.apply_cursor();
    }

    /// The prompt's button under a window position.
    fn prompt_button(&self, px: f64, py: f64) -> Option<prompt::Button> {
        let (p, gpu) = (self.confirm_close.as_ref()?, self.gpu.as_ref()?);
        let (x, y, _, _) = gpu.overlay_rect()?;
        p.hit(px - x as f64, py - y as f64)
    }

    /// Whether a keyboard close is worth a question: the guest has drawn
    /// something, so there may be work in it to lose.
    fn can_lose_work(&self) -> bool {
        self.gpu.as_ref().is_some_and(Gpu::has_frame) && self.vm().is_some()
    }

    /// Cmd+Q, and only on macOS: Super+Q is the guest's Win+Q elsewhere.
    fn mac_quit_chord(&self) -> bool {
        cfg!(target_os = "macos") && self.modifiers.super_key()
    }

    fn close_player(&mut self, event_loop: &ActiveEventLoop) {
        // before the VM handle goes: the Windows hook holds a copy
        self.kbd = None;
        if let Some(session) = self.session.as_mut() {
            session.shut_down();
        }
        event_loop.exit();
    }

    fn capture_keyboard(&mut self) {
        if let (Some(vm), Some(window)) = (self.vm(), self.window.as_ref()) {
            self.kbd = kbcapture::Capture::new(window, vm);
            if let Some(k) = self.kbd.as_mut() {
                k.set_focused(window.has_focus());
            }
        }
    }

    /// Ctrl+Alt+K. Off drops the capture (the inhibitor destroyed, the
    /// grab or the hook let go) and on makes a new one, so each state is
    /// what the other was built from.
    fn toggle_keyboard_capture(&mut self) {
        self.kbd_off = !self.kbd_off;
        if self.kbd_off {
            self.kbd = None;
            eprintln!("[keyboard] host shortcuts are the host's (Ctrl+Alt+K gives them to the guest)");
        } else {
            self.capture_keyboard();
            eprintln!("[keyboard] host shortcuts go to the guest (Ctrl+Alt+K gives them back)");
        }
        self.apply_title();
    }

    fn apply_title(&self) {
        let Some(window) = self.window.as_ref() else { return };
        let mut notes = Vec::new();
        if self.grabbed {
            notes.push("Ctrl+Alt+G releases the mouse");
        }
        if self.kbd_off {
            notes.push("Ctrl+Alt+K sends shortcuts to the guest");
        }
        let mut title = String::from("2ksbox player");
        if !notes.is_empty() {
            title.push_str(&format!(" ({})", notes.join(", ")));
        }
        window.set_title(&title);
    }

    fn set_grab(&mut self, on: bool) {
        let Some(window) = self.window.clone() else { return };
        if on {
            if window.set_cursor_grab(CursorGrabMode::Locked).is_err() {
                let _ = window.set_cursor_grab(CursorGrabMode::Confined);
            }
            self.hide_cursor(&window);
            self.cursor_applied = HostCursor::Hidden;
        } else {
            let _ = window.set_cursor_grab(CursorGrabMode::None);
        }
        self.grabbed = on;
        self.apply_title();
        // a hidden pointer is a shape now, so releasing the grab has to put
        // the right shape back rather than just turn the cursor on again
        if !on {
            self.apply_cursor();
        }
    }

    /// Hide the pointer over the window: our transparent shape, never
    /// `set_cursor_visible(false)` (see `blank_cursor`).
    fn hide_cursor(&self, window: &Window) {
        let Some(blank) = &self.blank_cursor else {
            window.set_cursor_visible(false);
            return;
        };
        window.set_cursor(Cursor::Custom(blank.clone()));
        window.set_cursor_visible(true);
    }

    /// Pick up a new guest cursor shape (a define or a clear) and turn it
    /// into a host cursor; needs the event loop, so it runs on wakes.
    fn update_guest_cursor(&mut self, event_loop: &ActiveEventLoop) {
        let Some(display) = self.session.as_ref().and_then(Session::display) else { return };
        let Some((seq, shape)) = display.cursor_if_newer(self.guest_cursor_seq) else { return };
        self.guest_cursor_seq = seq;
        self.guest_cursor = shape.and_then(|c| {
            let mut rgba = Vec::with_capacity(c.argb.len() * 4);
            for px in &c.argb {
                rgba.extend_from_slice(&[(px >> 16) as u8, (px >> 8) as u8, *px as u8, (px >> 24) as u8]);
            }
            match CustomCursor::from_rgba(rgba, c.width as u16, c.height as u16, c.hot_x as u16, c.hot_y as u16) {
                Ok(src) => Some(event_loop.create_custom_cursor(src)),
                Err(e) => {
                    eprintln!("[cursor] guest shape {}x{} refused: {e}", c.width, c.height);
                    None
                }
            }
        });
        self.apply_cursor();
    }

    /// The host pointer sits exactly where the guest's cursor is only with an
    /// absolute device (the USB tablet) and no grab: then the guest's shape
    /// can be the host cursor. Otherwise (a relative mouse, PS/2, whether
    /// grabbed or not) the sprite is composited into the frame.
    fn host_cursor_possible(&self) -> bool {
        !self.grabbed && self.vm().map(|v| v.mouse_is_absolute()).unwrap_or(false)
    }

    /// The host cursor over the window: the guest's shape while over the
    /// image (and visible per the guest), hidden over the image when the
    /// guest has no hardware cursor, the default elsewhere.
    fn apply_cursor(&mut self) {
        let Some(window) = &self.window else { return };
        if self.grabbed {
            return;
        }
        let visible = self.session.as_ref().and_then(Session::display).and_then(|d| d.cursor_visible());
        let want = if !self.pointer_inside || self.confirm_close.is_some() {
            HostCursor::Default
        } else if let (true, Some(_), Some(true)) = (self.host_cursor_possible(), &self.guest_cursor, visible) {
            HostCursor::Guest(self.guest_cursor_seq)
        } else {
            HostCursor::Hidden
        };
        if want == self.cursor_applied {
            return;
        }
        if std::env::var("PLAYER_CURSOR_LOG").is_ok() {
            eprintln!(
                "[cursor] host: {}",
                match &want {
                    HostCursor::Default => "default".to_string(),
                    HostCursor::Hidden => format!("hidden (shape {}, guest visible {:?})", self.guest_cursor.is_some(), visible),
                    HostCursor::Guest(s) => format!("the guest's shape #{s}"),
                }
            );
        }
        match &want {
            HostCursor::Default => {
                window.set_cursor(Cursor::default());
                window.set_cursor_visible(true);
            }
            HostCursor::Guest(_) => {
                window.set_cursor(Cursor::Custom(self.guest_cursor.clone().unwrap()));
                window.set_cursor_visible(true);
            }
            HostCursor::Hidden => self.hide_cursor(window),
        }
        self.cursor_applied = want;
    }

    /// Window pixel → guest framebuffer coordinates (None outside the image).
    fn to_guest(&self, px: f64, py: f64) -> Option<(i32, i32, i32, i32)> {
        self.gpu.as_ref()?.to_guest(px, py)
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.gpu.is_some() {
            return;
        }
        // Built here rather than in `main`: on macOS a HID source wants
        // the run loop, and `resumed` is the first point at which the UI
        // thread is running one.
        self.input = Input::new(self.args.pad_mode);

        let attrs = Window::default_attributes()
            .with_title("2ksbox player")
            .with_inner_size(LogicalSize::new(1280.0, 960.0));
        let window = Arc::new(event_loop.create_window(attrs).expect("create window"));
        // see `blank_cursor`: hiding the pointer is a shape of our own
        self.blank_cursor = CustomCursor::from_rgba(vec![0u8; 16 * 16 * 4], 16, 16, 0, 0)
            .ok()
            .map(|src| event_loop.create_custom_cursor(src));

        let size = window.inner_size();
        let mut gpu = Gpu::new(window.clone(), (size.width, size.height));
        let waker = self.proxy.clone().map(|p| {
            Arc::new(move || {
                let _ = p.send_event(());
            }) as Arc<dyn Fn() + Send + Sync>
        });
        self.session = Some(Session::start(&self.args, &mut gpu, waker));
        self.window = Some(window);
        self.gpu = Some(gpu);
        self.tell_window_size();
        #[cfg(target_os = "macos")]
        kbcapture::quit_closes_window();
        self.kbd_off = !player_core::keyboard_capture_at_start();
        if !self.kbd_off {
            self.capture_keyboard();
        }
        self.apply_title();
    }

    fn exiting(&mut self, _event_loop: &ActiveEventLoop) {
        // Every way out passes here, a guest power-off included, and the
        // windowing connection is still open: run_app() consumes the event
        // loop, so by the time App drops the wl_display / X Display is gone
        // and the inhibitor's destroy (or XUngrabKeyboard) touches freed
        // memory: a SIGSEGV after every power-off.
        self.kbd = None;
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => {
                // A close with Alt held came from the keyboard (Alt+F4, a
                // window manager's Alt binding) and may be a hand that meant
                // the guest: ask, and take a second one while asking as the
                // answer. On macOS the menu's Cmd+Q arrives here too
                // (`kbcapture::quit_closes_window`), with Cmd held. The
                // title bar's button is never an accident. A guest that has
                // drawn nothing yet has nothing to lose.
                let by_key = self.modifiers.alt_key() || (cfg!(target_os = "macos") && self.modifiers.super_key());
                if by_key && self.confirm_close.is_none() && self.can_lose_work() {
                    self.ask_to_close();
                    return;
                }
                self.close_player(event_loop);
            }
            WindowEvent::Resized(size) => {
                if let Some(gpu) = self.gpu.as_mut() {
                    gpu.resize(size.width, size.height);
                }
                self.tell_window_size();
                self.show_prompt();
            }
            WindowEvent::ModifiersChanged(m) => self.modifiers = m.state(),
            WindowEvent::KeyboardInput { event, .. } => {
                let PhysicalKey::Code(code) = event.physical_key else {
                    return;
                };
                let down = event.state == ElementState::Pressed;
                // the close prompt takes every key: Enter closes, Esc goes
                // back (as the host keymap reads them: caps:escape's Esc);
                // a second Cmd+Q on macOS is the answer, like a second Alt+F4
                if self.confirm_close.is_some() {
                    if down && !event.repeat {
                        match keymap::as_host_reads(&event.logical_key, event.location).unwrap_or(code) {
                            KeyCode::Enter | KeyCode::NumpadEnter => self.close_player(event_loop),
                            KeyCode::KeyQ if self.mac_quit_chord() => self.close_player(event_loop),
                            KeyCode::Escape => self.dismiss_prompt(),
                            _ => {}
                        }
                    }
                    return;
                }
                // macOS Cmd+Q, reaching the window because the capture took
                // it from the menu: ask first, as Alt+F4 does (Win+Q means
                // nothing to the guest, Quit means everything to the hand)
                if down && !event.repeat && code == KeyCode::KeyQ && self.mac_quit_chord() {
                    if self.can_lose_work() {
                        self.ask_to_close();
                    } else {
                        self.close_player(event_loop);
                    }
                    return;
                }
                // Ctrl+Alt+G: release the mouse grab (host-side hotkey)
                if down
                    && code == KeyCode::KeyG
                    && self.modifiers.control_key()
                    && self.modifiers.alt_key()
                {
                    self.set_grab(false);
                    return;
                }
                // Ctrl+Alt+S: shoot the guest's own frame, unscaled and
                // unshaded; with Shift, what the window shows (scaled and
                // through the CRT chain)
                if down
                    && code == KeyCode::KeyS
                    && self.modifiers.control_key()
                    && self.modifiers.alt_key()
                {
                    if let (false, Some(gpu)) = (event.repeat, self.gpu.as_ref()) {
                        if self.modifiers.shift_key() {
                            gpu.window_shot();
                        } else {
                            gpu.screenshot();
                        }
                    }
                    return;
                }
                // Ctrl+Alt+K: the host's shortcuts to the host, or back to
                // the guest (once per press: a held chord repeats)
                if down
                    && code == KeyCode::KeyK
                    && self.modifiers.control_key()
                    && self.modifiers.alt_key()
                {
                    if !event.repeat {
                        self.toggle_keyboard_capture();
                    }
                    return;
                }
                // Ctrl+Alt+Shift+F: windowed full screen (borderless, on the
                // window's own monitor), and back
                if down
                    && code == KeyCode::KeyF
                    && self.modifiers.control_key()
                    && self.modifiers.alt_key()
                    && self.modifiers.shift_key()
                {
                    if let (false, Some(window)) = (event.repeat, self.window.as_ref()) {
                        let full = window.fullscreen().is_none().then_some(Fullscreen::Borderless(None));
                        window.set_fullscreen(full);
                    }
                    return;
                }
                // Ctrl+Alt+Shift+D: Ctrl+Alt+Del in the guest, released with D
                if code == KeyCode::KeyD
                    && (self.input.cad_held()
                        || (down
                            && self.modifiers.control_key()
                            && self.modifiers.alt_key()
                            && self.modifiers.shift_key()))
                {
                    let vm = self.vm();
                    self.input.ctrl_alt_del(vm, down);
                    return;
                }
                // A key the host keymap moved goes as what the host reads
                // it as; the press's answer is kept for the release, which
                // has to let go of the same key in the guest.
                let key = match self.moved.iter().position(|m| m.0 == code) {
                    Some(i) if !down => self.moved.swap_remove(i).1,
                    Some(i) => self.moved[i].1, // key repeat
                    None => {
                        let k = keymap::as_host_reads(&event.logical_key, event.location).unwrap_or(code);
                        if down && k != code {
                            self.moved.push((code, k));
                        }
                        k
                    }
                };
                if let Some(sc) = keymap::atset1(key) {
                    let vm = self.vm();
                    self.input.key(vm, sc, down);
                }
            }
            WindowEvent::CursorMoved { position, .. } => {
                if self.confirm_close.is_some() {
                    let hover = self.prompt_button(position.x, position.y);
                    if let Some(p) = self.confirm_close.as_mut() {
                        if p.hover != hover {
                            p.hover = hover;
                            self.show_prompt();
                        }
                    }
                    return;
                }
                if let Some(vm) = self.vm() {
                    if vm.mouse_is_absolute() {
                        if self.grabbed {
                            // guest switched to a tablet: a relative grab is wrong now
                            self.set_grab(false);
                        }
                        let inside = self.to_guest(position.x, position.y);
                        // over the image the host cursor is the guest's hardware
                        // cursor, or hidden while the guest paints its own
                        if self.pointer_inside != inside.is_some() {
                            self.pointer_inside = inside.is_some();
                            self.apply_cursor();
                        }
                        if let Some((x, y, w, h)) = inside {
                            vm.mouse_abs(x, y, w, h);
                            vm.input_flush();
                        }
                    }
                }
            }
            WindowEvent::CursorLeft { .. } => {
                self.pointer_inside = false;
                self.apply_cursor();
            }
            WindowEvent::MouseInput { state, button, .. } => {
                let down = state == ElementState::Pressed;
                if let Some(hover) = self.confirm_close.as_ref().map(|p| p.hover) {
                    if down && button == MouseButton::Left {
                        match hover {
                            Some(prompt::Button::Close) => self.close_player(event_loop),
                            Some(prompt::Button::Back) => self.dismiss_prompt(),
                            None => {}
                        }
                    }
                    return;
                }
                if let Some(vm) = self.vm() {
                    if down && !self.grabbed && !vm.mouse_is_absolute() {
                        self.set_grab(true);
                    }
                    let b = match button {
                        MouseButton::Left => 0,
                        MouseButton::Middle => 1,
                        MouseButton::Right => 2,
                        MouseButton::Back => 5,
                        MouseButton::Forward => 6,
                        MouseButton::Other(_) => return,
                    };
                    vm.mouse_btn(b, down);
                    vm.input_flush();
                }
            }
            WindowEvent::MouseWheel { delta, .. } => {
                if self.confirm_close.is_some() {
                    return;
                }
                if let Some(vm) = self.vm() {
                    let y = match delta {
                        MouseScrollDelta::LineDelta(_, y) => y,
                        MouseScrollDelta::PixelDelta(p) => p.y as f32 / 40.0,
                    };
                    let b = if y > 0.0 {
                        3
                    } else if y < 0.0 {
                        4
                    } else {
                        return;
                    };
                    vm.mouse_btn(b, true);
                    vm.mouse_btn(b, false);
                    vm.input_flush();
                }
            }
            WindowEvent::Focused(focused) => {
                if let Some(k) = self.kbd.as_mut() {
                    k.set_focused(focused);
                }
                if !focused {
                    self.set_grab(false);
                    self.lift_all_keys();
                }
            }
            WindowEvent::RedrawRequested => {
                let sprite = !self.host_cursor_possible();
                let (Some(gpu), Some(session), Some(window)) = (self.gpu.as_mut(), self.session.as_mut(), &self.window)
                else {
                    return;
                };
                let frame = if session.offscreen() {
                    None
                } else {
                    match gpu.acquire() {
                        Some(f) => Some(f),
                        None => return,
                    }
                };
                let published = session.draw(gpu, frame.as_ref(), sprite);
                if let Some(frame) = frame {
                    window.pre_present_notify();
                    gpu.present(frame);
                }
                session.presented(gpu, published);
                if session.animates() {
                    window.request_redraw();
                }
                self.apply_min_size();
            }
            _ => {}
        }
    }

    fn user_event(&mut self, event_loop: &ActiveEventLoop, _ev: ()) {
        // the guest's cursor shape or visibility may have changed (a wake
        // comes for both; the shape needs the event loop to become a cursor)
        self.update_guest_cursor(event_loop);
        if self.pointer_inside {
            self.apply_cursor();
        }
        let (Some(gpu), Some(session)) = (self.gpu.as_mut(), self.session.as_mut()) else { return };
        // QEMU's main loop returned (guest power-off, `quit`): stop touching
        // the handle and leave; main() then releases it for qemu_cleanup.
        if session.wake(gpu) {
            event_loop.exit();
            return;
        }
        self.input.poll_pads(session);
        self.apply_min_size();
        // QEMU published a frame (multiple wakes coalesce into one redraw)
        if let Some(window) = &self.window {
            window.request_redraw();
        }
    }

    fn about_to_wait(&mut self, _el: &ActiveEventLoop) {
        if let (Some(gpu), Some(session)) = (self.gpu.as_mut(), self.session.as_mut()) {
            session.headless_tick(gpu);
        }
    }

    fn device_event(&mut self, _el: &ActiveEventLoop, _id: DeviceId, event: DeviceEvent) {
        if let DeviceEvent::MouseMotion { delta: (dx, dy) } = event {
            if !self.grabbed {
                return;
            }
            if let Some(vm) = self.vm() {
                if !vm.mouse_is_absolute() {
                    vm.mouse_rel(dx as i32, dy as i32);
                    vm.input_flush();
                }
            }
        }
    }
}

fn main() {
    let args = player_core::startup();
    let event_loop = EventLoop::<()>::with_user_event().build().expect("event loop");
    event_loop.set_control_flow(ControlFlow::Wait);
    let mut app = App {
        args,
        window: None,
        gpu: None,
        session: None,
        input: Input::default(),
        modifiers: ModifiersState::default(),
        grabbed: false,
        moved: Vec::new(),
        kbd: None,
        kbd_off: false,
        confirm_close: None,
        proxy: Some(event_loop.create_proxy()),
        guest_cursor: None,
        guest_cursor_seq: 0,
        blank_cursor: None,
        pointer_inside: false,
        cursor_applied: HostCursor::default(),
    };
    event_loop.run_app(&mut app).expect("run");
    let status = app.session.as_mut().map(Session::join).unwrap_or(0);
    // Return, don't exit(): QEMU's atexit handlers run here, after its
    // thread has already completed qemu_cleanup, the same order as
    // qemu-system's own main().
    if status != 0 {
        std::process::exit(status);
    }
}
