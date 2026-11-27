//! The launcher (doc 07) on mitsuami: the platform's own widgets (AppKit,
//! WinUI 3, GTK 4, or Kirigami with the `kde` feature) over
//! `launcher-core`, which holds the bundle format, the machine library,
//! the disc shelf, the snapshot state machine, the wizard's form, the
//! shader profile editor, the preview's render path and every debug verb
//! that needs no toolkit (ADR-014).
//!
//! So this crate is only view code: signals that mirror a core model, and
//! a `view!` per window. Nothing here decides anything about machines,
//! discs or shaders. It is the launcher, the one every package ships as
//! `2ksbox` (ADR-023).
//!
//! It is **not** in the root workspace: build it from this directory, so
//! the root `cargo build` never needs GTK.

// A windowed program on Windows, because a console-subsystem binary opens
// a black terminal on every double-click. The debug verbs still print:
// `main` borrows the console it was launched from when there is one.
#![cfg_attr(windows, windows_subsystem = "windows")]

mod about;
mod clone;
mod discs;
mod firstrun;
mod machines;
mod path_field;
mod shaders;
mod shot;
mod snaps;
mod wizard;

use mitsuami::prelude::*;

fn main() {
    // First: a windowed program on Windows has no stderr, so the log's
    // milestones are what say how far a start that vanished got
    // (`launcher_core::fatal`).
    // On Windows the last words also go in a message box, mitsuami's.
    launcher_core::fatal::install("mitsuami", cfg!(windows).then_some(mitsuami::fatal_error as fn(&str, &str)));
    // The package's own Vulkan driver, named to the loader before a thread
    // exists (`host_gpu::announce_driver`'s one rule).
    launcher_core::host_gpu::announce_driver();
    // In the macOS App Sandbox, the files picked in earlier runs
    // (`launcher_core::grants`), before anything opens one: a verb, a
    // window or a player this process starts.
    launcher_core::grants::restore();
    // Debug verbs first, before a window exists: `launcher_core::cli`'s,
    // so this binary answers every one `launcherx` does.
    let mut args = std::env::args().skip(1);
    if let Some(verb) = args.next() {
        launcher_core::console::attach_parent();
        if let Some(code) = launcher_core::cli::run(&verb, &mut args) {
            launcher_core::console::exit_after_verb(code);
        }
        eprintln!("unknown option {verb}");
        launcher_core::console::exit_after_verb(2);
    }

    launcher_core::fatal::note("the event loop");
    // The app's identity, given to the toolkit: the desktop-entry
    // name a Wayland compositor matches a window to its launcher by (and
    // the themed icon GTK and KDE look up under it), and the picture for
    // the platforms that take one (the Dock outside a bundle, Windows'
    // title bars). The PNG is one of the sizes `scripts/gen-icons.sh`
    // derives: the monitor with no plate on Windows and Linux, as the
    // packages' icon there, and the plated icon on Apple's grid on macOS.
    #[cfg(not(target_os = "macos"))]
    let icon = include_bytes!("../../packaging/icon/2ksbox-256.png");
    #[cfg(target_os = "macos")]
    let icon = include_bytes!("../../packaging/icon/macos/2ksbox-256.png");
    // The menu bar in the title bar before the title, as the player's
    // (user), and the toolbar (only the first-run download's bar) at its
    // end beside the caption buttons, rather than rows of their own.
    #[cfg(windows)]
    {
        use mitsuami::winui::{MenuBarPlace, ToolbarAlign, ToolbarPlace, set_menu_bar_place, set_toolbar_place};
        set_menu_bar_place(MenuBarPlace::InTitleBarStart);
        set_toolbar_place(ToolbarPlace::InTitleBar(ToolbarAlign::End));
    }
    App::new()
        .id(launcher_core::paths::APP_ID)
        .name("2ksbox")
        .icon(AppIcon::bytes(icon.as_slice()))
        // The machine list and its details, with room for a long path; 50
        // wider and taller on Windows and GTK (user). On macOS and GTK the
        // list is the window's sidebar (250 on macOS), which the window adds
        // to the details' size.
        .open(main_window)
        .run();
}

/// Whether the machines window opened at its remembered frame, which an
/// empty library's wider window (`machines`) already is.
pub static OPENED_REMEMBERED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// The machines window, opened as it was left (where, how large, maximized
/// or not; `launcher_core::window_state`), else at its first size where
/// the platform puts a new window. A headless shot neither reads nor
/// writes the remembered frame, so a photograph is the same on every
/// machine.
fn main_window() -> Window {
    use launcher_core::window_state::{Keeper, WindowState};
    use std::{cell::RefCell, rc::Rc};
    let headless = std::env::var_os("LAUNCHER_SHOT").is_some() || std::env::var_os("LAUNCHER_SCREEN").is_some();
    let keeper = launcher_core::window_state::launcher_file().filter(|_| !headless).map(Keeper::new);
    let remembered = keeper.as_ref().and_then(Keeper::load);
    let frame = signal(remembered.map(|s| WindowFrame {
        position: s.position.map(|(x, y)| Point::new(x, y)),
        size: Size::new(s.size.0, s.size.1),
    }));
    let maximized = signal(remembered.is_some_and(|s| s.maximized));
    OPENED_REMEMBERED.store(remembered.is_some(), std::sync::atomic::Ordering::Relaxed);
    let keeper = Rc::new(RefCell::new(keeper));
    Window::new("2ksbox")
        .size(if cfg!(target_os = "macos") {
            Size::new(540.0, 560.0)
        } else if cfg!(sidebar) {
            Size::new(machines::GTK_CONTENT_W, 610.0)
        } else if cfg!(windows) {
            Size::new(820.0, 610.0)
        } else {
            Size::new(770.0, 560.0)
        })
        .frame(frame)
        .maximized(maximized)
        .bind(signal(true))
        .content(move || {
            let ui = inject::<Ui>().expect("a window's content");
            let keeper = keeper.clone();
            effect(move || {
                let (Some(f), maximized) = (frame.get(), maximized.get()) else { return };
                let state = WindowState {
                    position: f.position.map(|pt| (pt.x, pt.y)),
                    size: (f.size.width, f.size.height),
                    maximized,
                };
                if !keeper.borrow_mut().as_mut().is_some_and(|k| k.changed(state)) {
                    return;
                }
                let (waiter, keeper) = (ui.clone(), keeper.clone());
                ui.spawn_local(async move {
                    waiter.sleep(Keeper::SETTLE).await;
                    keeper.borrow_mut().as_mut().map(Keeper::settled);
                });
            });
            view! { <machines::MachinesWindow/> }
        })
}
