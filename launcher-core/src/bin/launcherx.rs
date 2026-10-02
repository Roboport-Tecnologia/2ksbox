//! `launcherx`: the launcher's toolkit-free verbs with no front end.
//!
//! Every verb that needs no GUI lives in `launcher_core::cli`, so
//! `launcher-mitsuami` and this binary answer them identically (doc 07,
//! ADR-014). This binary builds with nothing installed, while
//! `launcher-mitsuami` needs its platform toolkit. `scripts/test.sh`
//! drives the launcher through it for that reason. The suite runs before
//! every commit, on hosts with no GUI toolkit, and must not pay for a
//! front end to answer `--print-args`.
//!
//! It is a test and debugging tool. No packager installs it; the shipped
//! command is `2ksbox`, which is `launcher-mitsuami` (ADR-023). It cannot
//! do the headless screenshots of real windows, which need a toolkit and
//! belong to `launcher-mitsuami` (`LAUNCHER_SHOT` / `LAUNCHER_SCREEN`,
//! its `src/shot.rs`).

fn main() {
    // No `fatal::install` here, unlike the front end. It exists because a
    // windowed program on Windows has no stderr and a panic on the way to
    // the window would vanish. This is a console program, and installing
    // it would append to the launcher's own crash log 60 times per test
    // run.
    launcher_core::host_gpu::announce_driver();
    let mut args = std::env::args().skip(1);
    let Some(verb) = args.next() else {
        eprintln!("usage: launcherx <verb> [args]   (the launcher's toolkit-free verbs; see launcher-core/src/cli.rs)");
        std::process::exit(2);
    };
    match launcher_core::cli::run(&verb, &mut args) {
        Some(code) => std::process::exit(code),
        None => {
            eprintln!("launcherx: unknown verb {verb}");
            eprintln!("  (the headless window frame grabs are the front end's: `launcher-mitsuami`)");
            std::process::exit(2);
        }
    }
}
