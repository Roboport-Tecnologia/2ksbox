//! The player (doc 02) in a mitsuami window (track M22): one running
//! machine per process, its picture on a `GpuSurface`, and the platform's
//! own menus over it. Everything but the window is `player-core`'s, shared
//! with the winit player (`player/`): the QEMU thread, the picture and its
//! CRT chain, audio, gamepads, the keys the guest holds, the command line.
//!
//! What the surface gives this player that the winit one does for itself:
//! the keyboard grab (the host's shortcuts to the guest, `kbcapture.rs`
//! there), the pointer lock with raw motion, the guest's cursor as the
//! surface's, keys a host keymap moved read as the host reads them, full
//! screen and the minimum size capped at the screen. The close question is
//! the platform's alert, where the winit player draws its own.
//!
//! The picture is drawn on the UI thread when QEMU publishes (`wake.rs`),
//! as the winit player draws on its event loop's wake. The surface is a
//! desync Wayland subsurface on GTK, presented to in Mailbox: the spike
//! (`track/player-gtk-spike`) measured that route at the winit window's
//! latency, where GTK's own offload waited a refresh for its frame clock.
//!
//! It is **not** in the root workspace, like `launcher-mitsuami`: build it
//! from this directory, so the root `cargo build` never needs GTK.

mod wake;

use mitsuami::core::{CurrentWindow, Ui};
use mitsuami::prelude::*;
use player_core::{Gpu, Input, MinSize, Qemu, Session};
use std::cell::RefCell;
use std::sync::Arc;
use std::time::Duration;

/// The machine and its picture. Kept out of the reactive graph: nothing
/// here is shown by a widget, and the draw runs on every published frame.
struct Player {
    args: player_core::Args,
    gpu: Option<Gpu>,
    session: Option<Session>,
    input: Input,
    /// The surface's scale: its pixels per point.
    scale: f32,
    /// The pointer is over the guest's image, not the bars around it.
    pointer_inside: bool,
    /// The guest's hardware cursor (the d3dpt-vga driver, doc 15), as the
    /// surface's cursor, and the sequence number it came with.
    guest_cursor: Option<Cursor>,
    guest_cursor_seq: u64,
    /// What the surface's cursor was last set to.
    cursor_applied: HostCursor,
    /// Raw moves have come while locked: `Motion` is then ignored, or the
    /// guest would get each move twice.
    raw_motion: bool,
    /// The part of the relative motion not yet sent: the guest takes whole
    /// counts, and raw motion can come in fractions (Wayland's relative
    /// pointer), so rounding each move would lose every slow one.
    motion_rest: (f32, f32),
    /// The close question is up.
    asking: bool,
    /// The window, for sizing it to the picture (`fit_window`).
    window: Option<(Ui, NodeId)>,
    /// The content size in points the window opened at or last
    /// remembered (`--window-state`): a resize to another one is the
    /// user's, and is written (`remember_size`).
    remembered: Option<(f32, f32)>,
    /// When the surface first had a real size: the sizes of the next
    /// moments are the platform's opening, not the user's.
    opened_at: Option<std::time::Instant>,
}

#[derive(Default, PartialEq, Clone, Copy)]
enum HostCursor {
    #[default]
    Default,
    Hidden,
    /// the guest's shape of this sequence number
    Guest(u64),
}

thread_local! {
    static PLAYER: RefCell<Option<Player>> = const { RefCell::new(None) };
}

fn with<R>(f: impl FnOnce(&mut Player) -> R) -> Option<R> {
    PLAYER.with(|p| p.borrow_mut().as_mut().map(f))
}

fn vm() -> Option<Qemu> {
    with(|p| p.session.as_ref().and_then(Session::vm)).flatten()
}

/// The window's signals: what the platform shows, and what it changes back
/// (the lock and the grab end when the window stops being the active one).
#[derive(Clone, Copy)]
struct Window_ {
    open: Signal<bool>,
    full: Signal<bool>,
    min: Signal<Size>,
    /// the pointer is locked to the surface (a relative mouse)
    locked: Signal<bool>,
    /// the keyboard is grabbed: the host's shortcuts go to the guest
    grabbed: Signal<bool>,
    /// the user wants that (Ctrl+Alt+K turns it off and on); the platform
    /// ends the grab on every focus loss, and the next key or click takes
    /// it again
    want_grab: Signal<bool>,
    cursor: Signal<Cursor>,
    paused: Signal<bool>,
    /// View > Scale: the picture held at a whole scale, in the screen's
    /// real pixels per scanline (every scale the largest fit can land on),
    /// or `None` for the largest that fits
    scale: Signal<Option<u32>>,
    /// the surface's pixels per point: the Scale menu lists as many real
    /// scales as 4 points per scanline takes (8 on a Retina screen)
    backing: Signal<f32>,
    /// there is a picture to fit the window to: a GPU, and a guest that
    /// doesn't take the window's size; kept by `draw`, since neither is a
    /// signal the menu could follow
    fits: Signal<bool>,
}

fn main() {
    let args = player_core::startup();
    let guest = !args.qemu.is_empty() && args.sweep.is_none() && args.calib.is_none();
    PLAYER.with(|p| {
        *p.borrow_mut() = Some(Player {
            args,
            gpu: None,
            session: None,
            input: Input::default(),
            scale: 1.0,
            pointer_inside: false,
            guest_cursor: None,
            guest_cursor_seq: 0,
            cursor_applied: HostCursor::Default,
            raw_motion: false,
            motion_rest: (0.0, 0.0),
            asking: false,
            window: None,
            remembered: None,
            opened_at: None,
        })
    });
    // The launcher's identity: a compositor matches the player's window to
    // 2ksbox's desktop entry and icon by it. mitsuami makes no
    // single-instance application of it, so players run side by side.
    let icon = include_bytes!("../../packaging/icon/2ksbox-256.png");
    // The menu bar in the title bar, after the title, rather than on a row
    // of its own that the picture sits under (user, 2026-10-08).
    // ... and every machine's window opening in the middle of the screen,
    // not a step further down Windows' cascade each time; in full screen
    // the menus drop over the picture while the released pointer is at the
    // top edge (user, 2026-10-08).
    #[cfg(windows)]
    {
        use mitsuami::winui::{
            FullScreenMenuBar, MenuBarPlace, WindowPlacement, set_full_screen_menu_bar, set_menu_bar_place,
            set_window_placement,
        };
        set_menu_bar_place(MenuBarPlace::InTitleBar);
        set_window_placement(WindowPlacement::Centred);
        set_full_screen_menu_bar(FullScreenMenuBar::AtTopEdge);
    }
    App::new()
        .id("com._2ksbox.Launcher")
        .name("2ksbox")
        .icon(AppIcon::bytes(icon.as_slice()))
        .open(move || player_window(guest))
        .run();
    // The window is gone: QEMU's thread must finish `qemu_cleanup` before
    // the process exits (CLAUDE.md). The device is never dropped: the
    // reactive runtime is gone with the window, and wgpu's teardown at
    // exit has nothing to give back to a process that is ending.
    let status = PLAYER
        .with(|p| p.borrow_mut().take())
        .map(|mut p| {
            let status = p.session.as_mut().map(Session::join).unwrap_or(0);
            std::mem::forget(p.gpu.take());
            status
        })
        .unwrap_or(0);
    // QEMU's atexit handlers run here, after its thread has completed
    // qemu_cleanup, as after the winit player's return from main.
    std::process::exit(status);
}

fn player_window(guest: bool) -> Window {
    let w = Window_ {
        open: signal(true),
        full: signal(false),
        min: signal(Size::new(320.0, 240.0)),
        locked: signal(false),
        grabbed: signal(false),
        want_grab: signal(guest && player_core::keyboard_capture_at_start()),
        cursor: signal(Cursor::Default),
        paused: signal(false),
        scale: signal(None),
        backing: signal(1.0),
        fits: signal(false),
    };
    let title = move || {
        let mut notes = Vec::new();
        if w.locked.get() {
            notes.push(format!("{} releases the mouse", keys('G', false)));
        }
        if guest && !w.want_grab.get() {
            notes.push(format!("{} sends shortcuts to the guest", keys('K', false)));
        }
        let mut title = String::from("2ksbox player");
        if !notes.is_empty() {
            title.push_str(&format!(" ({})", notes.join(", ")));
        }
        title
    };
    // the machine's remembered size (M22), else 1280x960 points
    let remembered = with(|p| p.args.window_state.as_deref().and_then(player_core::window_state::load)).flatten();
    let (width, height) = remembered.unwrap_or((1280.0, 960.0));
    eprintln!("[window] opens at {width}x{height} points{}", if remembered.is_some() { " (remembered)" } else { "" });
    Window::new(title)
        .size(Size::new(width, height))
        .min_size(w.min)
        .full_screen(w.full)
        .open(w.open)
        .on_close_request(move || close_request(w))
        .content(move || content(w))
}

fn content(w: Window_) -> impl View {
    let ui = inject::<Ui>().expect("a window's content");
    if let Some(CurrentWindow(id)) = inject::<CurrentWindow>() {
        with(|p| p.window = Some((ui.clone(), id)));
    }
    set_menu(menus(w));
    // The wake: everything that follows a publish, on the UI thread.
    let wake = Arc::new(wake::Wake::default());
    {
        let wake = wake.clone();
        spawn_local(async move {
            loop {
                wake.next().await;
                on_wake(w);
            }
        });
    }
    // The keyboard grab follows the user's wish; the platform ending it
    // leaves the wish alone.
    effect(move || {
        let want = w.want_grab.get();
        if w.grabbed.get_untracked() != want {
            w.grabbed.set(want);
        }
    });
    // A lock ended (by the platform, or Ctrl+Alt+G): the pointer is the
    // host's again, with the right shape.
    effect(move || {
        let _ = w.locked.get();
        apply_cursor(w);
    });
    let ready = move |handle: SurfaceHandle| {
        let size = handle.size();
        // On Windows the surface is a child window of the XAML window, and
        // frames Vulkan presents there never show (acquired and presented
        // without an error, the window empty); Direct3D 12's do.
        let backends = cfg!(windows).then_some(player_core::wgpu::Backends::DX12);
        let mut gpu = Gpu::with_backends(handle.clone(), (size.width.max(1), size.height.max(1)), backends);
        let started = with(|p| {
            p.input = Input::new(p.args.pad_mode);
            p.scale = if size.scale > 0.0 { size.scale } else { 1.0 };
            w.backing.set(p.scale);
            let session = Session::start(&p.args, &mut gpu, Some(wake.notifier()));
            let animates = session.animates();
            let offscreen = session.offscreen();
            session.tell_window_size(&gpu, (size.width, size.height), p.scale as f64);
            p.session = Some(session);
            p.gpu = Some(gpu);
            (animates, offscreen)
        });
        // The test pattern at 60 Hz; the sweep and the calibration as fast
        // as they go (each step exits the process when it is done).
        if let Some((true, offscreen)) = started {
            let period = Duration::from_millis(if offscreen { 1 } else { 16 });
            let ticker = ui.clone();
            ui.spawn_local(async move {
                loop {
                    draw(w);
                    ticker.sleep(period).await;
                }
            });
        }
        if w.want_grab.get_untracked() {
            w.grabbed.set(true);
        }
    };
    let resized = move |size: SurfaceSize| {
        if std::env::var_os("PLAYER_SURFACE_LOG").is_some() {
            eprintln!("[surface] {}x{} at {}x", size.width, size.height, size.scale);
        }
        let backing = if size.scale > 0.0 { size.scale } else { 1.0 };
        if w.backing.get_untracked() != backing {
            w.backing.set(backing);
        }
        let full = w.full.get_untracked();
        with(|p| {
            p.scale = backing;
            remember_size(p, (size.width as f32 / p.scale, size.height as f32 / p.scale), full);
            if let (Some(gpu), Some(session)) = (p.gpu.as_mut(), p.session.as_ref()) {
                gpu.resize(size.width, size.height);
                session.tell_window_size(gpu, (size.width, size.height), p.scale as f64);
            }
        });
        // at the new size at once, never the old frame stretched
        draw(w);
    };
    view! {
        <Column grow=1.0>
            <GpuSurface label="Machine" grow=1.0 @ready=ready @resize=resized
                @input=move |input| on_input(w, input)
                pointer_lock=w.locked keyboard_grab=w.grabbed cursor=w.cursor/>
        </Column>
    }
}

/// A new content size, in points, written to the machine's
/// `--window-state` file when it is the user's: not a size of the first
/// two seconds (0x0 comes first, then the size asked for, then, on macOS,
/// that size shrunk to fit a screen too small for it), not full screen's,
/// and not the size it already had (a move to a screen of another scale
/// reports the same points).
const OPENING: Duration = Duration::from_secs(2);

fn remember_size(p: &mut Player, size: (f32, f32), full: bool) {
    let Some(path) = p.args.window_state.as_deref() else { return };
    if size.0 < 1.0 || size.1 < 1.0 {
        return;
    }
    let opened = *p.opened_at.get_or_insert_with(std::time::Instant::now);
    let Some((w, h)) = p.remembered.filter(|_| opened.elapsed() >= OPENING) else {
        p.remembered = Some(size);
        return;
    };
    if full || ((w - size.0).abs() < 1.0 && (h - size.1).abs() < 1.0) {
        return;
    }
    player_core::window_state::save(path, size.0, size.1);
    p.remembered = Some(size);
}

/// The menu bar. The chords are the winit player's: the window takes them
/// first, and while the keyboard is grabbed they reach the surface, which
/// answers the same ones (`chord`).
fn menus(w: Window_) -> MenuBar {
    let primary_alt = |c: char| Shortcut::primary(Key::Char(c)).alt();
    // AppKit puts its own Enter Full Screen (Ctrl+Cmd+F) in a menu named
    // View, and mitsuami keeps `w.full` in step with it, so a second item
    // there would only repeat it; the grabbed chord still toggles it.
    let mut view = Menu::new("View");
    if !cfg!(target_os = "macos") {
        view = view
            .item(MenuItem::new("Full Screen").bind(w.full).shortcut(primary_alt('f').shift()))
            .separator();
    }
    // Nothing to fit while the guest takes the window's size (the
    // picture is the window), nor in full screen.
    let sized = move || !w.full.get() && w.fits.get();
    // Every whole scale in the screen's real pixels, the ones the largest
    // fit lands on: 1x is one pixel per scanline on any screen, and a
    // Retina screen lists up to 8x (4 points per scanline).
    let scale = Menu::new("Scale")
        .item(
            MenuItem::new("Largest That Fits")
                .radio((w.scale, None))
                .shortcut(primary_alt('0')),
        )
        .children_with(move || {
            let backing = w.backing.get();
            (1..=((4.0 * backing).round() as u32).max(4))
                .map(|n| {
                    let key = char::from_digit(n, 10).filter(|_| n <= 4);
                    MenuItem::new(format!("{n}x"))
                        .radio((w.scale, Some(n)))
                        .shortcut(key.map(primary_alt))
                })
                .collect::<Vec<_>>()
        });
    // A scale chosen is the picture's, whatever the window's size: what
    // overflows the window is cropped. Fit Window to Picture is the way
    // to the window around it.
    effect(move || {
        let held = w.scale.get();
        with(|p| p.gpu.as_mut().map(|g| g.set_fixed_scale(held)));
        draw(w);
    });
    view = view
        .submenu(scale)
        .item(
            MenuItem::new("Fit Window to Picture")
                .enabled(sized)
                .shortcut(primary_alt('0').shift())
                .on_select(move || fit_window(w)),
        )
        .separator();
    MenuBar::new()
        .menu(
            Menu::new("Machine")
                // Its chord (`chord`) holds Ctrl+Alt+Del for as long as D
                // is; the menu's shortcut, which the window takes first
                // while the keyboard isn't grabbed, taps it, as the item
                // does (user, 2026-10-08).
                .item(
                    MenuItem::new("Send Ctrl+Alt+Del")
                        .shortcut(primary_alt('d').shift())
                        .on_select(send_ctrl_alt_del),
                )
                .item(MenuItem::new("Send Shortcuts to Guest").bind(w.want_grab).shortcut(primary_alt('k')))
                .item(
                    MenuItem::new("Release Mouse")
                        .enabled(move || w.locked.get())
                        .shortcut(primary_alt('g'))
                        .on_select(move || w.locked.set(false)),
                )
                .separator()
                .item(
                    MenuItem::new("Pause")
                        .checked(move || w.paused.get())
                        .shortcut(primary_alt('p').shift())
                        .on_select(move || toggle_pause(w)),
                )
                .item(MenuItem::new("Reset").on_select(|| {
                    if let Some(vm) = vm() {
                        vm.vm_reset();
                    }
                }))
                .item(MenuItem::new("Power Button").on_select(|| {
                    if let Some(vm) = vm() {
                        vm.vm_powerdown();
                    }
                }))
                .separator()
                .item(
                    MenuItem::new("Close")
                        .role(MenuRole::Quit)
                        .shortcut(Shortcut::primary(Key::Char('q')))
                        .on_select(move || close_request(w)),
                ),
        )
        .menu(
            view.item(MenuItem::new("Save Screenshot").shortcut(primary_alt('s')).on_select(|| {
                    with(|p| p.gpu.as_ref().map(Gpu::screenshot));
                }))
                .item(MenuItem::new("Save Screenshot as Shown").shortcut(primary_alt('s').shift()).on_select(|| {
                    with(|p| p.gpu.as_ref().map(Gpu::window_shot));
                })),
        )
}

/// Pause the guest, or let a paused one run again.
/// Pausing lets go of the mouse, and a click doesn't take it again until
/// the guest runs.
fn toggle_pause(w: Window_) {
    let Some(vm) = vm() else { return };
    let pause = !w.paused.get_untracked();
    if pause {
        w.locked.set(false);
        vm.vm_pause();
    } else {
        vm.vm_start();
    }
    w.paused.set(pause);
}

/// Whether the platform's primary modifier is held: Command on macOS,
/// Ctrl elsewhere, as `Shortcut::primary` means it in the menus.
fn primary(m: Modifiers) -> bool {
    if cfg!(target_os = "macos") { m.meta } else { m.control }
}

/// A player chord as the platform writes it, for the title's notes:
/// "⌥⌘G" (with Shift "⌥⇧⌘G") on macOS, "Ctrl+Alt+G" elsewhere; the
/// menus show the same chords the platform's own way.
fn keys(key: char, shift: bool) -> String {
    if cfg!(target_os = "macos") {
        format!("⌥{}⌘{key}", if shift { "⇧" } else { "" })
    } else {
        format!("Ctrl+Alt+{}{key}", if shift { "Shift+" } else { "" })
    }
}

/// Ctrl+Alt+Del from the menu: nothing is held, so the chord presses the
/// modifiers too, and lets go a tenth of a second later rather than in the
/// same instant, which a guest can miss.
fn send_ctrl_alt_del() {
    let vm = vm();
    with(|p| p.input.ctrl_alt_del(vm, true));
    spawn_local(async move {
        sleep(Duration::from_millis(100)).await;
        let vm = self::vm();
        with(|p| p.input.ctrl_alt_del(vm, false));
    });
}

/// A close: the window's close button, Alt+F4, or the menu's Close (and
/// its shortcut). Each asks first when the guest has drawn something to
/// lose (user, 2026-10-07: the title bar's button too, which once closed
/// at once).
fn close_request(w: Window_) {
    let Some((can_lose, asking)) = with(|p| {
        let can_lose = p.gpu.as_ref().is_some_and(Gpu::has_frame) && p.session.as_ref().and_then(Session::vm).is_some();
        (can_lose, p.asking)
    }) else {
        return;
    };
    if asking {
        return;
    }
    if !can_lose {
        close(w);
        return;
    }
    // Nothing reaches the guest while the question is up: the lock let
    // go, the keys it holds (the Alt of Alt+F4) lifted.
    w.locked.set(false);
    let vm = vm();
    with(|p| {
        p.asking = true;
        p.input.lift_all(vm);
    });
    let (Some(ui), window) = (inject::<Ui>(), inject::<CurrentWindow>().map(|CurrentWindow(id)| id)) else {
        close(w);
        return;
    };
    let alert = Alert::new(player_core::CLOSE_QUESTION)
        .message(player_core::CLOSE_DETAIL)
        .style(AlertStyle::Warning)
        .button("Cancel")
        .button("Close");
    let asker = ui.clone();
    ui.spawn_local(async move {
        let answer = asker.alert(window, alert).await;
        with(|p| p.asking = false);
        if answer == 1 {
            close(w);
        }
    });
}

/// Pull the plug and close the window; `main` then joins QEMU's thread.
fn close(w: Window_) {
    with(|p| {
        if let Some(session) = p.session.as_mut() {
            session.shut_down();
        }
    });
    w.open.set(false);
}

/// QEMU published a frame, or the guest's cursor changed.
fn on_wake(w: Window_) {
    let stopped = with(|p| {
        let (Some(gpu), Some(session)) = (p.gpu.as_mut(), p.session.as_mut()) else { return false };
        // QEMU's main loop returned (guest power-off, `quit`): stop
        // touching the handle and leave; `main` joins it.
        if session.wake(gpu) {
            return true;
        }
        p.input.poll_pads(session);
        session.headless_tick(gpu);
        false
    })
    .unwrap_or(false);
    if stopped {
        w.open.set(false);
        return;
    }
    update_guest_cursor(w);
    draw(w);
}

/// Milliseconds since the first call, for the input log.
fn t_ms() -> f64 {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_secs_f64() * 1000.0
}

/// One frame: the newest picture through the chain onto the surface.
fn draw(w: Window_) {
    let sprite = !host_cursor_possible(w);
    let min = with(|p| {
        let scale = p.scale;
        let (Some(gpu), Some(session)) = (p.gpu.as_mut(), p.session.as_mut()) else { return None };
        let frame = if session.offscreen() {
            None
        } else {
            match gpu.acquire() {
                Some(f) => Some(f),
                None => return gpu.take_min_size().map(|m| (m, scale)),
            }
        };
        let published = session.draw(gpu, frame.as_ref(), sprite);
        if let Some(frame) = frame {
            gpu.present(frame);
        }
        session.presented(gpu, published);
        gpu.take_min_size().map(|m| (m, scale))
    })
    .flatten();
    let fits = with(|p| p.gpu.as_ref().is_some_and(|g| !g.follows_window())).unwrap_or(false);
    if w.fits.get_untracked() != fits {
        w.fits.set(fits);
    }
    // What the picture wants of the window's size; mitsuami caps it at the
    // screen and grows a window smaller than it.
    if let Some((min, scale)) = min {
        let size = match min {
            MinSize::Logical(width, height) => Size::new(width as f32, height as f32),
            MinSize::Physical(width, height) => Size::new(width as f32 / scale, height as f32 / scale),
        };
        w.min.set(size);
    }
}

/// Size the window's content to the picture with no bars around it: at
/// the scale it is held to, or the one on show. Not in full screen, and
/// not while the guest takes the window's size (the picture is the
/// window then).
fn fit_window(w: Window_) {
    if w.full.get_untracked() {
        return;
    }
    let ask = with(|p| {
        let (pw, ph) = p.gpu.as_ref()?.picture_px()?;
        let (ui, window) = p.window.clone()?;
        Some((ui, window, Size::new(pw as f32 / p.scale, ph as f32 / p.scale)))
    })
    .flatten();
    if let Some((ui, window, size)) = ask {
        ui.set_window_size(window, size);
    }
}

/// The host pointer sits exactly where the guest's cursor is only with an
/// absolute device (the USB tablet) and no lock: then the guest's shape
/// can be the host cursor. Otherwise (a relative mouse, PS/2, locked or
/// not) the sprite is composited into the frame.
fn host_cursor_possible(w: Window_) -> bool {
    !w.locked.get_untracked() && vm().is_some_and(|v| v.mouse_is_absolute())
}

/// Pick up a new guest cursor shape (a define or a clear).
fn update_guest_cursor(w: Window_) {
    let changed = with(|p| {
        let display = p.session.as_ref().and_then(Session::display)?;
        let (seq, shape) = display.cursor_if_newer(p.guest_cursor_seq)?;
        p.guest_cursor_seq = seq;
        let scale = p.scale;
        p.guest_cursor = shape.map(|c| {
            let mut rgba = Vec::with_capacity(c.argb.len() * 4);
            for px in &c.argb {
                rgba.extend_from_slice(&[(px >> 16) as u8, (px >> 8) as u8, *px as u8, (px >> 24) as u8]);
            }
            // one guest pixel to one of the screen's, as the picture's 1x
            Cursor::Image {
                pixels: Pixels::new(c.width, c.height, rgba).scale(scale),
                hotspot: Point::new(c.hot_x as f32 / scale, c.hot_y as f32 / scale),
            }
        });
        Some(())
    })
    .flatten();
    if changed.is_some() || with(|p| p.pointer_inside).unwrap_or(false) {
        apply_cursor(w);
    }
}

/// The cursor over the surface: the guest's shape while over the image
/// (and visible per the guest), hidden over the image when the guest has
/// no hardware cursor, the platform's elsewhere and while the close
/// question is up.
fn apply_cursor(w: Window_) {
    if w.locked.get_untracked() {
        return; // the lock hides it
    }
    let possible = host_cursor_possible(w);
    let set = with(|p| {
        let visible = p.session.as_ref().and_then(Session::display).and_then(|d| d.cursor_visible());
        let want = if !p.pointer_inside || p.asking {
            HostCursor::Default
        } else if let (true, Some(_), Some(true)) = (possible, &p.guest_cursor, visible) {
            HostCursor::Guest(p.guest_cursor_seq)
        } else {
            HostCursor::Hidden
        };
        if want == p.cursor_applied {
            return None;
        }
        if std::env::var("PLAYER_CURSOR_LOG").is_ok() {
            eprintln!(
                "[cursor] host: {}",
                match &want {
                    HostCursor::Default => "default".to_string(),
                    HostCursor::Hidden => format!("hidden (shape {}, guest visible {visible:?})", p.guest_cursor.is_some()),
                    HostCursor::Guest(s) => format!("the guest's shape #{s}"),
                }
            );
        }
        p.cursor_applied = want;
        Some(match want {
            HostCursor::Default => Cursor::Default,
            HostCursor::Hidden => Cursor::Hidden,
            HostCursor::Guest(_) => p.guest_cursor.clone().unwrap_or(Cursor::Hidden),
        })
    })
    .flatten();
    if let Some(cursor) = set {
        w.cursor.set(cursor);
    }
}

/// Input on the surface: the host's chords, then the guest's keys and
/// pointer.
fn on_input(w: Window_, input: SurfaceInput) {
    if std::env::var_os("PLAYER_INPUT_LOG").is_some() {
        eprintln!("[input] {:.1} {input:?} (locked {}, grabbed {})", t_ms(), w.locked.get_untracked(), w.grabbed.get_untracked());
    }
    if with(|p| p.asking).unwrap_or(true) {
        return;
    }
    match input {
        SurfaceInput::Key { code, pressed, repeat, modifiers, .. } => {
            // the platform ended the grab when the window lost focus: a key
            // here means it has focus again
            if pressed && w.want_grab.get_untracked() && !w.grabbed.get_untracked() {
                w.grabbed.set(true);
            }
            if chord(w, code, pressed, repeat, modifiers) {
                return;
            }
            let Some(sc) = player_core::keys::atset1(code.name()) else { return };
            let vm = vm();
            with(|p| p.input.key(vm, sc, pressed));
        }
        SurfaceInput::PointerMoved { position, .. } => {
            let Some(vm) = vm() else { return };
            if !vm.mouse_is_absolute() {
                return;
            }
            if w.locked.get_untracked() {
                // the guest switched to a tablet: a relative lock is wrong now
                w.locked.set(false);
            }
            let inside = with(|p| {
                let s = p.scale as f64;
                let at = p.gpu.as_ref()?.to_guest(position.x as f64 * s, position.y as f64 * s);
                let changed = p.pointer_inside != at.is_some();
                p.pointer_inside = at.is_some();
                Some((at, changed))
            })
            .flatten();
            if let Some((at, changed)) = inside {
                if changed {
                    apply_cursor(w);
                }
                if let Some((x, y, gw, gh)) = at {
                    vm.mouse_abs(x, y, gw, gh);
                    vm.input_flush();
                }
            }
        }
        SurfaceInput::PointerLeft => {
            with(|p| p.pointer_inside = false);
            apply_cursor(w);
        }
        SurfaceInput::Button { button, pressed, .. } => {
            if pressed && w.want_grab.get_untracked() && !w.grabbed.get_untracked() {
                w.grabbed.set(true);
            }
            let Some(vm) = vm() else { return };
            if pressed && !w.locked.get_untracked() && !w.paused.get_untracked() && !vm.mouse_is_absolute() {
                with(|p| (p.raw_motion, p.motion_rest) = (false, (0.0, 0.0)));
                w.locked.set(true);
            }
            let b = match button {
                MouseButton::Primary => 0,
                MouseButton::Middle => 1,
                MouseButton::Secondary => 2,
                MouseButton::Back => 5,
                MouseButton::Forward => 6,
                MouseButton::Other(_) => return,
            };
            vm.mouse_btn(b, pressed);
            vm.input_flush();
        }
        SurfaceInput::Scroll { delta, .. } => {
            let Some(vm) = vm() else { return };
            // positive is towards the end (down); the guest's wheel up is 3
            let y = match delta {
                ScrollDelta::Lines { y, .. } => y,
                ScrollDelta::Points { y, .. } => y / 40.0,
            };
            let b = if y < 0.0 {
                3
            } else if y > 0.0 {
                4
            } else {
                return;
            };
            vm.mouse_btn(b, true);
            vm.mouse_btn(b, false);
            vm.input_flush();
        }
        // The guest accelerates a relative mouse itself, so it gets the
        // device's own counts where the platform has them, and the
        // cursor's moves only where it has none (X11 without XInput 2).
        // Except on macOS: there the raw counts are `GCMouse`'s, which
        // ignore the pointer's speed setting (the speed felt off, and a
        // drag with the trackpad pressed much faster, user), so the guest
        // gets the cursor's moves, as the winit player does.
        SurfaceInput::RawMotion { dx, dy } if !cfg!(target_os = "macos") => {
            with(|p| p.raw_motion = true);
            relative(w, dx, dy);
        }
        SurfaceInput::RawMotion { .. } => {}
        SurfaceInput::Motion { dx, dy } => {
            if !with(|p| p.raw_motion).unwrap_or(false) {
                relative(w, dx, dy);
            }
        }
    }
}

fn relative(w: Window_, dx: f32, dy: f32) {
    if !w.locked.get_untracked() {
        return;
    }
    let Some(vm) = vm() else { return };
    if vm.mouse_is_absolute() {
        return;
    }
    let Some((x, y)) = with(|p| {
        let (x, y) = (p.motion_rest.0 + dx, p.motion_rest.1 + dy);
        let (sx, sy) = (x.trunc(), y.trunc());
        p.motion_rest = (x - sx, y - sy);
        (sx as i32, sy as i32)
    }) else {
        return;
    };
    if x != 0 || y != 0 {
        vm.mouse_rel(x, y);
        vm.input_flush();
    }
}

/// The winit player's chords, for when the keyboard is grabbed and the
/// menus' shortcuts reach the surface: Ctrl+Alt+G lets go of the mouse,
/// Ctrl+Alt+S shoots the guest's frame (with Shift, the window's),
/// Ctrl+Alt+K gives the host its shortcuts back, Ctrl+Alt+Shift+F is full
/// screen, Ctrl+Alt+Shift+P pauses, Ctrl+Alt+1 to 4 hold the picture at
/// that scale and Ctrl+Alt+0 lets it go back to the largest that fits,
/// Ctrl+Alt+Shift+0 fits the window to the picture, and Ctrl+Alt+Shift+D
/// is Ctrl+Alt+Del in the guest for as long as D is held. Ctrl is the platform's primary
/// modifier (`primary`): Command on macOS, as in the menus. True when the
/// key was the host's.
fn chord(w: Window_, code: KeyCode, pressed: bool, repeat: bool, m: Modifiers) -> bool {
    let held = with(|p| p.input.cad_held()).unwrap_or(false);
    if code == KeyCode::KeyD && (held || (pressed && primary(m) && m.alt && m.shift)) {
        let vm = vm();
        with(|p| p.input.ctrl_alt_del(vm, pressed));
        return true;
    }
    if !(pressed && primary(m) && m.alt) {
        return false;
    }
    // once per press: a held chord repeats
    match code {
        KeyCode::KeyG => w.locked.set(false),
        KeyCode::KeyS if !repeat => {
            with(|p| p.gpu.as_ref().map(|g| if m.shift { g.window_shot() } else { g.screenshot() }));
        }
        KeyCode::KeyK if !repeat => w.want_grab.set(!w.want_grab.get_untracked()),
        KeyCode::KeyF if m.shift && !repeat => w.full.set(!w.full.get_untracked()),
        KeyCode::KeyP if m.shift && !repeat => toggle_pause(w),
        KeyCode::Digit0 if m.shift && !repeat => fit_window(w),
        KeyCode::Digit0 if !repeat => w.scale.set(None),
        KeyCode::Digit1 | KeyCode::Digit2 | KeyCode::Digit3 | KeyCode::Digit4 if !m.shift && !repeat => {
            w.scale.set(Some(match code {
                KeyCode::Digit1 => 1,
                KeyCode::Digit2 => 2,
                KeyCode::Digit3 => 3,
                _ => 4,
            }))
        }
        KeyCode::KeyS | KeyCode::KeyK => {}
        KeyCode::KeyF | KeyCode::KeyP if m.shift => {}
        KeyCode::Digit0 => {}
        KeyCode::Digit1 | KeyCode::Digit2 | KeyCode::Digit3 | KeyCode::Digit4 if !m.shift => {}
        _ => return false,
    }
    true
}
