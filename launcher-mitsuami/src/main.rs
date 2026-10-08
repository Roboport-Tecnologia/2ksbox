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
    launcher_core::fatal::install("mitsuami");
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
    // derives from the icon the packages install.
    let icon = include_bytes!("../../packaging/icon/2ksbox-256.png");
    // The toolbar in the title bar beside the caption buttons, as Windows
    // 11's own apps have it (user), rather than on a row of its own under it.
    #[cfg(windows)]
    {
        use mitsuami::winui::{ToolbarAlign, ToolbarPlace, set_toolbar_place};
        set_toolbar_place(ToolbarPlace::InTitleBar(ToolbarAlign::End));
    }
    App::new()
        .id(launcher_core::paths::APP_ID)
        .name("2ksbox")
        .icon(AppIcon::bytes(icon.as_slice()))
        // The machine list and its details, with room for a long path; 50
        // wider and taller on Windows and GTK (user). On macOS the list is
        // the window's sidebar (250), which the window adds to the details'
        // size.
        .window(
            "2ksbox",
            if cfg!(target_os = "macos") {
                Size::new(540.0, 560.0)
            } else if cfg!(any(windows, all(target_os = "linux", feature = "gtk", not(feature = "kde")))) {
                Size::new(820.0, 610.0)
            } else {
                Size::new(770.0, 560.0)
            },
            || view! { <machines::MachinesWindow/> },
        )
        .run();
}
