//! The player minus its window (doc 02): one running machine per process,
//! QEMU in-process, the picture through wgpu and the CRT chain, audio and
//! gamepads. A front end owns the window and its events and drives a
//! [`Session`] and a [`Gpu`]: `player/` on winit, `player-mitsuami/` on
//! mitsuami's `GpuSurface` (track M22). As with the launcher (ADR-014),
//! what one front end could do differently from another lives here.

pub mod audio;
pub mod companions;
#[cfg(target_os = "linux")]
pub mod dmabuf;
pub mod gpu;
pub mod input;
#[cfg(target_os = "macos")]
pub mod iosurface;
pub mod keys;
pub mod mode;
// The host end of a gamepad (M13). Polled on the UI thread, once per
// published guest frame (`Input::poll_pads`).
pub mod pad;
pub mod pattern;
pub mod qemu_vm;
pub mod qmp;
pub mod session;
pub mod sweep;

pub use gpu::{Gpu, MinSize};
pub use input::Input;
pub use qemu_embed::Qemu;
pub use session::Session;

/// The question a keyboard close asks before the player pulls the
/// machine's plug (Alt+F4, Cmd+Q), and what it says under it.
pub const CLOSE_QUESTION: &str = "Close the player?";
pub const CLOSE_DETAIL: &str = "The machine turns off at once and any unsaved work in it is lost.";

/// Whether a run starts with the host's shortcuts going to the guest
/// (`PLAYER_KEYBOARD_CAPTURE=0` starts it with them the host's).
pub fn keyboard_capture_at_start() -> bool {
    std::env::var("PLAYER_KEYBOARD_CAPTURE").as_deref() != Ok("0")
}

/// What the player was asked to run: `player [--shader <preset.slangp>]
/// [--shader-params <k=v,...>] [--pad <mode>] [--mode-sweep <dir>]
/// [--calib <bmp|dir>] [--] <qemu args...>`. No QEMU arguments is the
/// test pattern; `--mode-sweep` is doc 03's mode sweep, `--calib` shades
/// doc 09's calibration patterns. Those three run no guest.
#[derive(Default)]
pub struct Args {
    pub qemu: Vec<String>,
    pub shader: Option<std::path::PathBuf>,
    pub shader_params: Vec<(String, f32)>,
    /// What a host gamepad does for this machine (M13), written by
    /// `launcher-core` from `bundle::Pad`.
    pub pad_mode: pad::Mode,
    pub sweep: Option<std::path::PathBuf>,
    pub calib: Option<std::path::PathBuf>,
}

/// Everything a player does before its window: tell QEMU where the
/// package's companions are, ask Windows for 1 ms timers, answer the
/// verbs that need no window (`--companions`, `--pads`, `--pad-sweep`,
/// which exit), and read the rest of the command line. First thing in
/// `main`, before any thread: it edits the environment.
pub fn startup() -> Args {
    // An installed player tells QEMU where the package put the companions
    // its own dlopen searches would otherwise look for in a checkout.
    companions::announce();
    // Windows rounds every wait to its timer tick, 15.6 ms unless a process
    // asks for less, and QEMU's main loop waits for its timers: a guest's
    // 1 kHz timer (a MIDI sequencer's, a game's) fired a tick late and its
    // clock ran at 6 %. Patch 65 keeps those ticks; this keeps
    // them from arriving 15 at a time. For the life of the process, which
    // is what the request is scoped to since Windows 10 2004.
    #[cfg(windows)]
    unsafe {
        windows_sys::Win32::Media::timeBeginPeriod(1);
    }
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    // What the package put where, out of the binary that has to find it:
    // no window, no QEMU, nothing to clean up. The packagers' check.
    if args.first().map(String::as_str) == Some("--companions") {
        companions::report();
        std::process::exit(0);
    }
    // What gamepads this host can read, out of the binary that reads
    // them: the answer no one can get from outside the process.
    if args.first().map(String::as_str) == Some("--pads") {
        pad::report();
        std::process::exit(0);
    }
    // The host end of the gamepad against a scripted pad: no window, no
    // QEMU, nothing to clean up. The `pad` check.
    if args.first().map(String::as_str) == Some("--pad-sweep") && args.len() >= 2 {
        let frames: u64 = args[1].parse().unwrap_or(0);
        std::process::exit(pad::sweep(frames));
    }
    let mut shader: Option<std::path::PathBuf> =
        std::env::var("PLAYER_SHADER").ok().map(Into::into);
    let mut shader_params = std::env::var("PLAYER_SHADER_PARAMS")
        .ok()
        .map(|s| parse_shader_params(&s))
        .unwrap_or_default();
    if args.first().map(String::as_str) == Some("--shader") && args.len() >= 2 {
        shader = Some(args[1].clone().into());
        args.drain(0..2);
    }
    if args.first().map(String::as_str) == Some("--shader-params") && args.len() >= 2 {
        shader_params = parse_shader_params(&args[1]);
        args.drain(0..2);
    }
    let mut pad_cli: Option<String> = None;
    if args.first().map(String::as_str) == Some("--pad") && args.len() >= 2 {
        pad_cli = Some(args[1].clone());
        args.drain(0..2);
    }
    let pad_mode = pad::resolve_mode(pad_cli.as_deref());
    let mut sweep = None;
    if args.first().map(String::as_str) == Some("--mode-sweep") && args.len() >= 2 {
        sweep = Some(std::path::PathBuf::from(args[1].clone()));
        args.drain(0..2);
    }
    let mut calib = None;
    if args.first().map(String::as_str) == Some("--calib") && args.len() >= 2 {
        calib = Some(std::path::PathBuf::from(args[1].clone()));
        args.drain(0..2);
    }
    if args.first().map(String::as_str) == Some("--") {
        args.remove(0);
    }
    Args {
        qemu: args,
        shader,
        shader_params,
        pad_mode,
        sweep,
        calib,
    }
}

/// Exit without running atexit handlers. QEMU registers several
/// (`audio_cleanup`, `qemu_run_exit_notifiers`); running them on a thread
/// other than the QEMU thread, or while that thread is still alive, is a
/// crash. stderr is unbuffered so diagnostics already printed are safe.
pub fn hard_exit(code: i32) -> ! {
    #[cfg(unix)]
    unsafe {
        libc::_exit(code)
    }
    #[cfg(not(unix))]
    std::process::exit(code)
}

/// The next free `PLAYER_SHOT_DIR/2ksbox-NNNN.png` (the working directory
/// when unset), for both shots. `None`, said on stderr, when there is none.
pub fn shot_path() -> Option<std::path::PathBuf> {
    let dir = std::env::var_os("PLAYER_SHOT_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from("."));
    if let Err(e) = std::fs::create_dir_all(&dir) {
        eprintln!("[shot] {}: {e}", dir.display());
        return None;
    }
    // numbered rather than time-stamped: a number needs no time zone to
    // read, and the next free one is stable across runs
    let path = (1..10_000)
        .map(|n| dir.join(format!("2ksbox-{n:04}.png")))
        .find(|p| !p.exists());
    if path.is_none() {
        eprintln!("[shot] {}: no free file name", dir.display());
    }
    path
}

/// `PLAYER_SHOT_EVERY=<n>`: shoot the guest's own frame every n presented
/// guest frames (see `Gpu::screenshot`). Unset or 0 = never.
pub(crate) fn shot_every() -> Option<u64> {
    static EVERY: std::sync::OnceLock<Option<u64>> = std::sync::OnceLock::new();
    *EVERY.get_or_init(|| {
        std::env::var("PLAYER_SHOT_EVERY")
            .ok()
            .and_then(|v| v.parse().ok())
            .filter(|&n| n > 0)
    })
}

/// Debug: PLAYER_DUMP=<file.png> writes guest frame #PLAYER_DUMP_SEQ (default
/// 60) from the staging buffer, then exits. Lets CI/agents verify a boot.
pub fn maybe_dump(pixels: &[u32], w: usize, h: usize, seq: u64) {
    let Ok(path) = std::env::var("PLAYER_DUMP") else {
        return;
    };
    let want: u64 = std::env::var("PLAYER_DUMP_SEQ")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(60);
    if seq < want {
        return;
    }
    let mut rgb = Vec::with_capacity(w * h * 3);
    for p in pixels {
        rgb.extend_from_slice(&[(p >> 16) as u8, (p >> 8) as u8, *p as u8]);
    }
    let file = std::fs::File::create(&path).expect("dump file");
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w as u32, h as u32);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().unwrap().write_image_data(&rgb).unwrap();
    eprintln!("dumped {w}x{h} frame #{seq} to {path}");
    hard_exit(0);
}

/// Parse a `--shader-params`/`PLAYER_SHADER_PARAMS` value: comma-separated
/// `name=value` pairs (the launcher's shader-profile overrides), e.g.
/// `BRIGHTBOOST=1.2,GAMMA_INPUT=2.4`. A malformed entry is skipped with a
/// stderr line rather than failing the whole player over one typo.
fn parse_shader_params(s: &str) -> Vec<(String, f32)> {
    s.split(',')
        .filter(|entry| !entry.trim().is_empty())
        .filter_map(|entry| {
            let (name, value) = entry.split_once('=')?;
            match value.trim().parse::<f32>() {
                Ok(v) => Some((name.trim().to_string(), v)),
                Err(e) => {
                    eprintln!("[shader] bad --shader-params entry {entry:?}: {e}");
                    None
                }
            }
        })
        .collect()
}
