//! Headless screenshots of the real windows.
//!
//! Qt Quick already renders off-screen (`QT_QPA_PLATFORM=offscreen` with
//! the software backend, and `Item.grabToImage()`), so this is a handful
//! of environment variables read into properties and a few lines of QML.
//!
//! `LAUNCHER_QT_SHOT=<file.png>` arms it, `LAUNCHER_QT_SCREEN=<name>`
//! picks which window to open first, `LAUNCHER_QT_ARG=<value>` is that
//! window's argument (a bundle path, a preset),
//! `LAUNCHER_QT_DELAY=<ms>` is how long to let it settle, and
//! `LAUNCHER_QT_SIZE=<w>x<h>` opens the screen's window at that size
//! instead of its own, for a layout that only goes wrong when resized.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        /// Empty unless `LAUNCHER_QT_SHOT` is set. QML checks it to
        /// decide whether a grab is armed.
        #[qproperty(QString, shot_path)]
        /// "", "wizard", "optall", "closebox", "wizardscroll", "create",
        /// "clone", "adddisc", "pickdisc", "discs", "snapshots",
        /// "profiles", "saveprofile", "firstrun", "escfocus", "editor", "about"
        /// (`Main.qml`).
        #[qproperty(QString, screen)]
        #[qproperty(QString, arg)]
        #[qproperty(i32, delay_ms)]
        /// "" or "<w>x<h>" (`LAUNCHER_QT_SIZE`).
        #[qproperty(QString, size)]
        type Diag = super::DiagRust;

        /// Report what the grab did, so a failed `saveToFile` is visible
        /// in the terminal instead of producing a silent empty run.
        #[qinvokable]
        fn report(self: &Diag, ok: bool);

        /// Deliver a close event to the current modal window the way
        /// the window system does when its title bar's close button is
        /// clicked (`src/close_event.cpp`). Returns 1 if a modal window
        /// is still registered afterwards, 0 if none, -1 if there was
        /// none to close.
        #[qinvokable]
        fn close_modal_from_window_system(self: &Diag) -> i32;

        /// The title of the window that has the keyboard, "(none)" if
        /// none does. `src/focus_window.cpp` says why QML's own
        /// `Window.active` is no use for this.
        #[qinvokable]
        fn focus_window(self: &Diag) -> QString;

        /// A trace line from QML. Not `console.log`: that goes through
        /// Qt's categorised logging, which drops the `qml` category's
        /// debug output unless `QT_LOGGING_RULES` says otherwise. This
        /// always prints.
        #[qinvokable]
        fn note(self: &Diag, message: &QString);
    }
}

use crate::qs;
use cxx_qt_lib::QString;

pub struct DiagRust {
    shot_path: QString,
    screen: QString,
    arg: QString,
    delay_ms: i32,
    size: QString,
}

fn env(name: &str) -> QString {
    std::env::var(name).map(|v| qs(v)).unwrap_or_default()
}

impl Default for DiagRust {
    fn default() -> Self {
        DiagRust {
            shot_path: env("LAUNCHER_QT_SHOT"),
            screen: env("LAUNCHER_QT_SCREEN"),
            arg: env("LAUNCHER_QT_ARG"),
            delay_ms: std::env::var("LAUNCHER_QT_DELAY")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(600),
            size: env("LAUNCHER_QT_SIZE"),
        }
    }
}

unsafe extern "C" {
    fn launcher_qt_close_modal_from_window_system() -> i32;
    fn launcher_qt_focus_window() -> *const std::ffi::c_char;
}

impl ffi::Diag {
    fn close_modal_from_window_system(&self) -> i32 {
        unsafe { launcher_qt_close_modal_from_window_system() }
    }

    fn focus_window(&self) -> QString {
        let title = unsafe { std::ffi::CStr::from_ptr(launcher_qt_focus_window()) };
        qs(title.to_string_lossy())
    }

    fn note(&self, message: &QString) {
        eprintln!("[diag] {message}");
    }

    fn report(&self, ok: bool) {
        if ok {
            println!("[diag] wrote {}", self.shot_path);
        } else {
            eprintln!("[diag] failed to write {}", self.shot_path);
        }
    }
}
