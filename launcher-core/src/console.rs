//! The console a Windows GUI program has, or has not.
//!
//! The front end is a windowed program on Windows
//! (`windows_subsystem = "windows"`), because a console-subsystem binary
//! opens a black terminal window when someone double-clicks it and keeps
//! it for the life of the launcher. That choice costs two things, and
//! this module handles both:
//!
//! * A windowed program started from `cmd.exe` inherits no console, so
//!   `launcher --paths` would print into nowhere. [`attach_parent`]
//!   borrows the console it was launched from.
//! * A windowed program has no console to lend, so every console program
//!   it starts gets a new window of its own: one flash per `qemu-img`
//!   call and a permanent black rectangle behind the player. [`command`]
//!   is how this crate starts a subprocess, and it asks for no console.
//!
//! On every other platform `attach_parent` does nothing and `command` is
//! `Command::new`.

use std::path::Path;
use std::process::Command;

/// Start a subprocess without giving it a console window of its own.
///
/// Only when we have no console to lend it. Run the launcher from a
/// terminal and its children inherit that terminal, which is where a
/// developer wants the player's diagnostics. Run it from Explorer and
/// nothing flashes.
pub fn command(bin: &Path) -> Command {
    #[cfg_attr(not(windows), allow(unused_mut))]
    let mut cmd = Command::new(bin);
    #[cfg(windows)]
    if !has_console() {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// Attach to the console this process was launched from, if there is one
/// and nothing has already given us a standard output. A redirection
/// (`launcher --paths > file`, or a test harness reading a pipe) hands a
/// windowed program working handles, and those must be left alone.
///
/// Call it before writing anything, and only when the command line asks
/// for output. Attaching for a GUI run would put a console behind the
/// window, which this module exists to avoid.
pub fn attach_parent() {
    #[cfg(windows)]
    unsafe {
        use std::ffi::c_void;
        const ATTACH_PARENT_PROCESS: u32 = 0xFFFF_FFFF;
        const STD_OUTPUT_HANDLE: u32 = 0xFFFF_FFF5; // -11
        const STD_ERROR_HANDLE: u32 = 0xFFFF_FFF4; // -12
        const GENERIC_READ: u32 = 0x8000_0000;
        const GENERIC_WRITE: u32 = 0x4000_0000;
        const FILE_SHARE_WRITE: u32 = 2;
        const OPEN_EXISTING: u32 = 3;
        const INVALID_HANDLE_VALUE: *mut c_void = usize::MAX as *mut c_void;
        #[link(name = "kernel32")]
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
            fn GetStdHandle(which: u32) -> *mut c_void;
            fn SetStdHandle(which: u32, handle: *mut c_void) -> i32;
            fn CreateFileA(
                name: *const u8,
                access: u32,
                share: u32,
                sa: *mut c_void,
                disposition: u32,
                flags: u32,
                template: *mut c_void,
            ) -> *mut c_void;
        }
        // Redirected already: whoever started us said where output goes.
        let out = GetStdHandle(STD_OUTPUT_HANDLE);
        if !out.is_null() && out != INVALID_HANDLE_VALUE {
            return;
        }
        if AttachConsole(ATTACH_PARENT_PROCESS) == 0 {
            return; // no parent console: started from Explorer
        }
        // Attaching gives the process a console but not the three
        // handles; CONOUT$ is that console's screen buffer by name.
        let conout = CreateFileA(
            c"CONOUT$".as_ptr() as *const u8,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_WRITE,
            std::ptr::null_mut(),
            OPEN_EXISTING,
            0,
            std::ptr::null_mut(),
        );
        if conout != INVALID_HANDLE_VALUE && !conout.is_null() {
            SetStdHandle(STD_OUTPUT_HANDLE, conout);
            SetStdHandle(STD_ERROR_HANDLE, conout);
        }
    }
}

/// Whether a subprocess started by [`command`] inherits somewhere to
/// print. False only for a windowless Windows process, whose children
/// get no console. A caller with output worth keeping (the player's
/// start-up diagnostics) then redirects it to a file.
pub fn inherits_output() -> bool {
    #[cfg(windows)]
    {
        has_console()
    }
    #[cfg(not(windows))]
    {
        true
    }
}

/// Whether this process has a console attached at all.
/// End the process after a debug verb has answered, with `code`.
///
/// On Windows this is `TerminateProcess`, not `exit`: a verb ends
/// without running any global destructors, its own or a linked
/// library's. There is nothing to run at that point: the verb wrote
/// what it had, and this flushes what Rust still buffers. The player
/// ends its headless paths the same way, for a different atexit hazard.
///
/// The Qt launcher (retired 2026-10-02) is what made the rule. The QML
/// module compiled into that msvcrt-linked binary registered its units
/// in a `QGlobalStatic` whose destructor called back into `Qt6Qml.dll`.
/// The executable's destructors ran inside msvcrt's own
/// `DLL_PROCESS_DETACH`, which `LdrShutdownProcess` reached only after
/// it had detached the Qt DLLs loaded after msvcrt, so every verb ended
/// in `0xC0000005` after printing its whole answer. Today's launcher is
/// an MSVC binary with a static CRT, not that msvcrt arrangement, but a
/// verb still has no teardown worth the risk of a library that repeats
/// it.
///
/// Elsewhere `exit` orders the teardown correctly and is used as is.
pub fn exit_after_verb(code: i32) -> ! {
    use std::io::Write;
    let _ = std::io::stdout().flush();
    let _ = std::io::stderr().flush();
    #[cfg(windows)]
    unsafe {
        use std::ffi::c_void;
        #[link(name = "kernel32")]
        extern "system" {
            fn GetCurrentProcess() -> *mut c_void;
            fn TerminateProcess(process: *mut c_void, exit_code: u32) -> i32;
        }
        TerminateProcess(GetCurrentProcess(), code as u32);
    }
    std::process::exit(code)
}

#[cfg(windows)]
fn has_console() -> bool {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetConsoleWindow() -> *mut c_void;
    }
    unsafe { !GetConsoleWindow().is_null() }
}
