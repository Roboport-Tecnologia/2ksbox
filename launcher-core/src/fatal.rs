//! What a windowed program says when it dies.
//!
//! On Windows the launcher is `windows_subsystem = "windows"`
//! (`console.rs` says why), and a windowed process has no stderr. A
//! panic, or a toolkit that fails to load, prints into nothing and the
//! process disappears with no window and no message. The report that
//! comes back is "it didn't start, no error", which names no cause.
//!
//! So this module gives the launcher two outputs that need no toolkit:
//!
//! * A **log** beside the machine library (`launcher.log`, next to the
//!   `player.log` a windowless launcher already writes), appended to,
//!   with a header and a milestone per start-up step. A log that stops
//!   after "data dir" names the step that died.
//! * A **message box** on Windows for the last words, because a user who
//!   double-clicked an icon will not find a log file on their own. The
//!   front end hands [`install`] the toolkit's call for it (mitsuami's
//!   `fatal_error`, a Win32 message box that needs no toolkit running,
//!   DPI aware and with current controls), since this crate has none.
//!
//! The front end calls [`install`] first thing in `main`, so any panic
//! after it is reported. On Unix the log is written too (it costs
//! nothing, and a Flatpak's stderr is just as invisible) and no box is
//! shown: there the toolkit's error dialog runs a nested event loop, which
//! would re-enter a launcher that has just panicked inside its own.

use std::io::Write;
use std::path::PathBuf;
use std::sync::OnceLock;

/// The front end's message box (`install`), when it shows one.
static SHOW: OnceLock<fn(&str, &str)> = OnceLock::new();

/// Where the launcher's own start-up log goes: beside the library, like
/// `player.log`. Falls back to the temporary directory when the data
/// directory itself could not be worked out, since that failure is one
/// worth logging.
pub fn log_path() -> PathBuf {
    match crate::paths::data_dir() {
        Some(dir) => dir.join("launcher.log"),
        None => std::env::temp_dir().join("2ksbox-launcher.log"),
    }
}

fn append(line: &str) {
    let path = log_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) {
        let _ = writeln!(f, "{line}");
    }
}

/// One start-up milestone. The last one in the file is what matters: a
/// run that ends after `[start] library` died loading the library.
pub fn note(what: &str) {
    append(&format!("[start] {what}"));
}

/// One line for the log that is neither a start-up milestone nor a
/// death, but worth having in the file when a report comes back. The
/// player's command line is the main one ([`crate::player::spawn`]).
pub fn entry(line: &str) {
    append(line);
}

/// Say the last words: into the log always, and into a message box on
/// Windows, where there is no console and the user sees nothing else.
pub fn fatal(what: &str) {
    append(&format!("[fatal] {what}"));
    box_up("2ksbox could not start", what);
}

/// Install the panic hook and open the log with a header. Call it first
/// in `main`, before anything that can fail.
///
/// `front_end` names the binary in the header, because every front end
/// writes to the same file. `show` is the message box for the last
/// words, `None` for none (off Windows, see the module's note).
pub fn install(front_end: &str, show: Option<fn(&str, &str)>) {
    if let Some(show) = show {
        let _ = SHOW.set(show);
    }
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    append(&format!(
        "\n=== {} {} ({front_end}) started, unix time {secs} ===",
        crate::paths::NAME,
        env!("CARGO_PKG_VERSION"),
    ));
    if let Ok(exe) = std::env::current_exe() {
        note(&format!("exe = {}", exe.display()));
    }

    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        // The default hook first: it is what a developer running from a
        // terminal reads, and on Unix it is the whole report.
        previous(info);
        let what = format!(
            "{}\n\nat {}",
            info.payload()
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| info.payload().downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "panic".to_string()),
            info.location().map(|l| l.to_string()).unwrap_or_else(|| "unknown".into()),
        );
        append(&format!("[panic] {what}"));
        append(&format!("{}", std::backtrace::Backtrace::force_capture()));
        box_up("2ksbox stopped", &what);
    }));
}

/// The front end's message box, if `install` was given one.
fn box_up(title: &str, body: &str) {
    let Some(show) = SHOW.get() else { return };
    // The log's path is in the box because the box holds one message and
    // the log holds the milestones that lead up to it.
    show(title, &format!("{body}\n\nThe log is at\n{}", log_path().display()));
}

/// File a whole block of text in the log. `--diagnose` answers this way,
/// because a windowed program cannot answer on stdout and the verb's job
/// is to leave one file behind that says everything.
pub fn record(what: &str, block: &str) {
    append(&format!("--- {what} ---\n{}", block.trim_end()));
}
