//! The launcher, minus the drawing.
//!
//! The front end over this crate is `launcher-qt/` (Qt 6 / QML through
//! cxx-qt, doc 07). `launcher-capi/` is the same crate as a C ABI, and
//! `launcherx` is its toolkit-free verbs with no front end at all. The
//! rule is that **everything a front end could get differently lives
//! here**, including the windows' own behaviour, not only the file
//! formats and the subprocesses. Which memory default follows the family
//! until someone picks a number, the exact sentence under the networking
//! checkbox, when a snapshot job is polled, whether a running machine is
//! driven through its monitor or through `qemu-img`: each has one
//! implementation here. A front end is the widgets that show it plus the
//! events that call in.
//!
//! The rule comes from two front ends that each held their own copy. An
//! egui build and the Qt one once shared only the file formats, and they
//! drifted. The Qt wizard had no processor, floppy or boot field, its
//! networking checkbox didn't follow the family, and saving a new shader
//! profile dropped the parameter overrides in one of them. The egui build
//! is gone (ADR-017), and the rule stays because a C front end over
//! `launcher-capi` would drift the same way.
//!
//! Three groups of modules:
//!
//! * **The data.** `bundle` (`machine.toml`), `library` (the machine
//!   library), `disc_library` (the shared disc shelf), `shader_profile` /
//!   `shader_library` / `shader_source` (profiles, their library, and
//!   fetching the preset collection), `paths` (where everything lives,
//!   installed or in a checkout).
//! * **The machinery.** `player` (spawning one, and `qemu-img`),
//!   `control` (QMP to a running machine), `snapshots` (`qemu-img`'s
//!   half of the same), `preview` (the shader chain on a still image).
//! * **The window models.** `machines`, `wizard`, `clone_machine`,
//!   `shelf`, `snaps`, `editor` and `firstrun`, one per window, each
//!   holding its whole state machine and the sentences it shows. `browse`
//!   is the one file-dialog decision that is not the dialog. `cli` is
//!   every debug verb that needs no toolkit, so both binaries answer the
//!   same ones identically.

pub mod about;
pub mod browse;
pub mod bundle;
pub mod cli;
// "Clone…": a new machine that is a whole copy of one, disk included.
pub mod clone_machine;
pub mod console;
pub mod control;
pub mod disc_library;
pub mod editor;
// The launcher's own last words: a start-up log and, on Windows, a
// message box, because a windowed program there has no stderr.
pub mod fatal;
// The one question a launcher with no shader presets asks on the way up.
pub mod firstrun;
// What this host's GPU can do for the Direct3D executor (ADR-013).
pub mod host_gpu;
pub mod library;
pub mod machines;
pub mod paths;
pub mod player;
pub mod preview;
pub mod shader_library;
pub mod shader_profile;
pub mod shader_source;
pub mod shelf;
pub mod snapshots;
pub mod snaps;
pub mod wizard;
