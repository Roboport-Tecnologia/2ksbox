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
//!   double-clicked an icon will not find a log file on their own.
//!
//! The front end calls [`install`] first thing in `main`, so any panic
//! after it is reported. On Unix the log is written too (it costs
//! nothing, and a Flatpak's stderr is just as invisible) and the message
//! box is not compiled.

use std::io::Write;
use std::path::PathBuf;

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
/// writes to the same file.
pub fn install(front_end: &str) {
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

/// A modal box with an OK button, or nothing at all off Windows.
#[cfg(windows)]
fn box_up(title: &str, body: &str) {
    use std::ffi::c_void;
    const MB_OK: u32 = 0;
    const MB_ICONERROR: u32 = 0x10;
    #[link(name = "user32")]
    extern "system" {
        fn MessageBoxW(owner: *mut c_void, text: *const u16, caption: *const u16, kind: u32) -> i32;
    }
    let wide = |s: &str| s.encode_utf16().chain(std::iter::once(0)).collect::<Vec<u16>>();
    // The log's path is in the box because the box holds one message and
    // the log holds the milestones that lead up to it.
    let body = format!("{body}\n\nThe log is at\n{}", log_path().display());
    unsafe {
        MessageBoxW(std::ptr::null_mut(), wide(&body).as_ptr(), wide(title).as_ptr(), MB_OK | MB_ICONERROR);
    }
}

#[cfg(not(windows))]
fn box_up(_title: &str, _body: &str) {}

/// File a whole block of text in the log. `--diagnose` answers this way,
/// because a windowed program cannot answer on stdout and the verb's job
/// is to leave one file behind that says everything.
pub fn record(what: &str, block: &str) {
    append(&format!("--- {what} ---\n{}", block.trim_end()));
}
