//! The launcher (doc 07) on mitsuami: the platform's own widgets (AppKit,
//! WinUI 3, GTK 4, or Kirigami with the `kde` feature) over
//! `launcher-core`, which holds the bundle format, the machine library,
//! the disc shelf, the snapshot state machine, the wizard's form, the
//! shader profile editor, the preview's render path and every debug verb
//! that needs no toolkit (ADR-014).
//!
//! So this crate is only view code: signals that mirror a core model, and
//! a `view!` per window. Nothing here decides anything about machines,
//! discs or shaders. It runs beside `launcher-qt` until it reaches parity,
//! and then replaces it in the packages (ADR-023, track M19).
//!
//! It is **not** in the root workspace, like `launcher-qt`: build it from
//! this directory, so the root `cargo build` never needs GTK.

// A windowed program on Windows, because a console-subsystem binary opens
// a black terminal on every double-click. The debug verbs still print:
// `main` borrows the console it was launched from when there is one.
#![cfg_attr(windows, windows_subsystem = "windows")]

mod clone;
mod discs;
mod firstrun;
mod machines;
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
    // Debug verbs first, before a window exists: `launcher_core::cli`'s,
    // so this binary answers every one `launcherx` and `launcher-qt` do.
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
    // Wide enough for a row's five buttons with room to spare, as the Qt
    // window is.
    App::new().window("2ksbox", Size::new(1060.0, 560.0), || view! { <machines::MachinesWindow/> }).run();
}
