//! Spawning `player` (doc 07: two separate binaries). Once spawned the
//! process is independent of the launcher: dropping the `Child` neither
//! waits for nor kills it (Rust's default). Closing the launcher, or the
//! grid forgetting a bundle, must never stop a running guest, because a
//! killed VM leaves a dirty FAT (CLAUDE.md). Only a guest-side shutdown
//! or the player's own window ends a run.

use crate::bundle::Machine;
use crate::shader_library;
use crate::shader_profile::ShaderProfile;
use std::path::PathBuf;
use std::process::Child;

/// The `player` binary: `bin/2ksbox-player` in an installed tree
/// (`paths.rs`), otherwise beside the launcher's own executable (the
/// workspace's binaries share `target/<profile>`), and failing that the
/// workspace's own `target/<profile>`. That last case is `launcher-qt`,
/// which is outside the root workspace (so `cargo build` never needs
/// Qt 6) and builds into `launcher-qt/target/<profile>`, with no player
/// beside it. `LAUNCHER_PLAYER_BIN` overrides all of it.
pub fn player_binary() -> PathBuf {
    if let Ok(p) = std::env::var("LAUNCHER_PLAYER_BIN") {
        return p.into();
    }
    if let Some(prefix) = crate::paths::install_prefix() {
        // Flat on Windows, `bin/` under a Unix prefix, `MacOS/` inside
        // an .app; `paths::bin_dir` knows which. `prefix` is bound only
        // because asking for it proved we are installed.
        let _ = prefix;
        let name = if cfg!(windows) { "2ksbox-player.exe" } else { "2ksbox-player" };
        return crate::paths::bin_dir().join(name);
    }
    let name = if cfg!(windows) { "player.exe" } else { "player" };
    let exe = std::env::current_exe().expect("current_exe");
    let dir = exe.parent().expect("executable has a parent directory");
    let beside = dir.join(name);
    if beside.exists() {
        return beside;
    }
    // The same profile, not a baked-in `release`: a debug launcher must
    // find the debug player. A cross-built tree
    // (`target/x86_64-pc-windows-gnu/release`) keeps its player beside
    // the launcher and so never reaches here.
    let profile = dir.file_name().unwrap_or_else(|| "release".as_ref());
    crate::paths::checkout("target").join(profile).join(name)
}

/// What this host's hardware acceleration is called, for the wizard's
/// picker and its hints: KVM on Linux, WHPX on Windows (doc 07's
/// "acceleration: …" indicator). macOS gets no name: the Apple Silicon
/// machines this project targets cannot run an x86 guest natively, so
/// there is nothing to offer.
pub fn hw_accel_label() -> Option<&'static str> {
    match () {
        _ if cfg!(target_os = "linux") => Some("KVM"),
        _ if cfg!(target_os = "windows") => Some("WHPX"),
        _ => None,
    }
}

/// Whether this host can actually give a guest hardware acceleration, for
/// the wizard to say so next to the acceleration picker. `bundle::qemu_args`
/// does not consult it and leaves the decision to QEMU's own
/// `accel=kvm:tcg` fallback; this is a hint for a human.
///
/// Linux: the device must open for writing, which a bare `exists()`
/// misses. The node is there on a host whose user is not in the `kvm`
/// group, the common way for KVM to be unavailable.
/// Windows: the Hypervisor Platform is asked whether a hypervisor is
/// present, because the feature can be installed and still turned off.
/// On a machine where Hyper-V or WSL2 already took the root partition,
/// this answer differs from the guess.
pub fn hw_accel_available() -> bool {
    #[cfg(target_os = "linux")]
    {
        std::fs::OpenOptions::new().read(true).write(true).open("/dev/kvm").is_ok()
    }
    #[cfg(target_os = "windows")]
    {
        whpx_present()
    }
    #[cfg(not(any(target_os = "linux", target_os = "windows")))]
    {
        false
    }
}

/// `WHvGetCapability(WHvCapabilityCodeHypervisorPresent)`, resolved at
/// run time so the launcher still starts on a Windows without the
/// Hypervisor Platform feature (where the DLL is simply absent).
#[cfg(target_os = "windows")]
fn whpx_present() -> bool {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryA(name: *const u8) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }
    type GetCapability =
        unsafe extern "system" fn(u32, *mut c_void, u32, *mut u32) -> i32;
    unsafe {
        let dll = LoadLibraryA(c"WinHvPlatform.dll".as_ptr() as *const u8);
        if dll.is_null() {
            return false;
        }
        let proc = GetProcAddress(dll, c"WHvGetCapability".as_ptr() as *const u8);
        if proc.is_null() {
            return false;
        }
        let get: GetCapability = std::mem::transmute(proc);
        let mut present: u32 = 0;
        let mut written: u32 = 0;
        // WHvCapabilityCodeHypervisorPresent = 0; the buffer is a BOOL.
        let hr = get(0, &mut present as *mut u32 as *mut c_void, 4, &mut written);
        hr >= 0 && written == 4 && present != 0
    }
}

/// QEMU's firmware directory (QEMU's `-L`): shipped as
/// `share/2ksbox/pc-bios` in an installed tree, `qemu/pc-bios` in
/// a checkout (`paths.rs`). `LAUNCHER_PC_BIOS_DIR` overrides both.
pub fn pc_bios_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_PC_BIOS_DIR") {
        return dir.into();
    }
    crate::paths::resource("share/2ksbox/pc-bios", "qemu/pc-bios")
}

/// Resolve `machine`'s shader setting into the preset+overrides the
/// player actually runs with: a named `shader_profile` (looked up in the
/// profile library) takes precedence, then the raw `shader` override,
/// then the library's default profile (`shader_library::default_id`,
/// what "(default)" means), then no shader at all. A modern machine
/// skips the library's default (ADR-024): a CRT over Windows 11 is
/// something to pick, not something to get. A `shader_profile`
/// naming a deleted profile falls through the same way rather than
/// failing the machine (see `shader_library::find`).
fn resolve_shader(machine: &Machine) -> Option<ShaderProfile> {
    let dir = shader_library::default_dir();
    if let Some(id) = &machine.shader_profile {
        if let Some(profile) = shader_library::find(&dir, id) {
            return Some(profile);
        }
    }
    if let Some(preset) = machine.shader.clone() {
        return Some(ShaderProfile::new(String::new(), preset));
    }
    if machine.family.is_modern() {
        return None;
    }
    shader_library::find_default(&dir)
}

/// The `--shader [path] [--shader-params k=v,...]` arguments `spawn`
/// passes to `player`, given `machine`'s resolved shader setting. Split
/// out so `cli.rs`'s `--print-shader-args` debug verb can show exactly
/// what a bundle resolves to without spawning anything.
pub fn shader_args(machine: &Machine) -> Vec<String> {
    let Some(profile) = resolve_shader(machine) else {
        return Vec::new();
    };
    let mut args = vec!["--shader".to_string(), profile.preset.display().to_string()];
    if let Some(params) = profile.params_arg() {
        args.push("--shader-params".to_string());
        args.push(params);
    }
    args
}

/// The `--pad <setting>` argument `spawn` passes to `player` for this
/// machine's gamepad setting (M13, `docs/tracks/m13-gamepads.md`).
///
/// Nothing for a machine with the pad off, the default for every machine
/// and for the player too. Split out beside [`shader_args`] for the same
/// reason: `--print-player-args` can show what a bundle resolves to
/// without spawning anything.
///
/// Only the setting crosses. The bindings are the shared `gamepad`
/// crate's `default_key_bindings`, which both sides read. When a machine
/// can carry its own map, this is where it will be written out.
pub fn pad_args(machine: &Machine) -> Vec<String> {
    match machine.effective_pad() {
        crate::bundle::Pad::None => Vec::new(),
        p => vec!["--pad".to_string(), p.name().to_string()],
    }
}

/// The `--target <qemu>` argument `spawn` passes to `player`: which
/// `libqemu-embed-<target>` runs the machine (M20). Nothing for the era's
/// machines, which run on the player's default (`i386`), so their command
/// line is the one it always was.
pub fn target_args(machine: &Machine) -> Vec<String> {
    match machine.qemu_target() {
        "i386" => Vec::new(),
        t => vec!["--target".to_string(), t.to_string()],
    }
}

/// What has to exist on disk before `machine` can start, made if it does
/// not: a modern machine's firmware variable store, a qcow2 copy of
/// EDK2's empty one (`bundle::EFI_VARS_TEMPLATE`). Its TPM needs nothing
/// made; a missing state file is a TPM libtpms manufactures on the first
/// start. `spawn` calls this, and `--prepare` for a script that runs
/// QEMU itself.
pub fn prepare(machine: &Machine) -> std::io::Result<()> {
    if !machine.family.is_modern() {
        return Ok(());
    }
    let vars = machine.effective_efi_vars();
    if vars.exists() {
        return Ok(());
    }
    let template = pc_bios_dir().join(crate::bundle::EFI_VARS_TEMPLATE);
    let bin = qemu_img_binary();
    let status = crate::console::command(&bin)
        .args(["convert", "-f", "raw", "-O", "qcow2"])
        .arg(&template)
        .arg(&vars)
        .status()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))?;
    if status.success() {
        Ok(())
    } else {
        Err(std::io::Error::other(format!(
            "qemu-img could not make {} from {} ({status})",
            vars.display(),
            template.display()
        )))
    }
}

/// Spawn `player` on `machine`. Inherits the launcher's stdout/stderr
/// when there is a terminal to inherit. When there is not (a
/// double-clicked launcher on Windows, which has no console and gives
/// its children none, so nothing flashes up), it writes them to
/// [`log_path`] instead. A player that dies during start-up says why on
/// its stderr, and on that platform nobody can rerun it from a terminal
/// to find out.
///
/// `qmp_socket`, when given, makes QEMU listen on that path for a second
/// monitor the launcher drives for live media and snapshot control
/// (`control.rs`). The player's own in-process monitor is untouched and
/// the player needs no change, since everything after `--` is passed
/// through to QEMU. `shelf` is the flat disc-shelf file the drive
/// answers the in-guest CDSHELF program from.
pub fn spawn(
    machine: &Machine,
    qmp_socket: Option<&std::path::Path>,
    shelf: Option<&std::path::Path>,
) -> std::io::Result<Child> {
    prepare(machine)?;
    let mut args = machine.qemu_args(&pc_bios_dir(), shelf);
    if let Some(extra) = qmp_socket.and_then(crate::control::qmp_args) {
        args.extend(extra);
    }
    let bin = player_binary();
    let mut argv: Vec<String> = shader_args(machine);
    argv.extend(pad_args(machine));
    argv.extend(target_args(machine));
    argv.push("--".into());
    argv.extend(args);
    // Log the command line before anything is spawned: it is the first
    // thing to ask for when a machine does not start. It goes to the
    // launcher's own log (the file `2ksbox-debug.bat` collects and the
    // one a Flatpak user can still read), the player's log beside it,
    // and the terminal when there is one.
    let line = command_line(&bin, &argv);
    crate::fatal::entry(&format!("[player] {line}"));
    if crate::console::inherits_output() {
        eprintln!("launcher: {line}");
    }
    let mut cmd = crate::console::command(&bin);
    cmd.args(&argv);
    if !crate::console::inherits_output() {
        if let Some(log) = open_log(&machine.name, &line) {
            let dup = log.try_clone();
            cmd.stdout(log);
            if let Ok(dup) = dup {
                cmd.stderr(dup);
            }
        }
    }
    cmd.spawn().map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))
}

/// The spawn as one line someone can paste into a shell. Half of these
/// arguments are QEMU option strings with commas and equals signs, and a
/// machine or disc image with a space in its name is ordinary, so every
/// argument that needs it is quoted: single quotes on Unix, double on
/// Windows, which is what each shell parses.
fn command_line(bin: &std::path::Path, argv: &[String]) -> String {
    let mut out = quote(&bin.display().to_string());
    for a in argv {
        out.push(' ');
        out.push_str(&quote(a));
    }
    out
}

fn quote(s: &str) -> String {
    let plain = !s.is_empty()
        && s.chars().all(|c| c.is_ascii_alphanumeric() || "-_=,.:/+@".contains(c));
    if plain {
        return s.to_string();
    }
    if cfg!(windows) {
        format!("\"{}\"", s.replace('"', "\\\""))
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

/// Where a windowless launcher puts the player's output: one file beside
/// the machine library, appended to, so the run before last is still
/// there when someone thinks to look.
pub fn log_path() -> Option<PathBuf> {
    crate::paths::data_dir().map(|d| d.join("player.log"))
}

fn open_log(machine: &str, command: &str) -> Option<std::fs::File> {
    use std::io::Write;
    let path = log_path()?;
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&path).ok()?;
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let _ = writeln!(f, "\n=== {machine} — player started, unix time {secs} ===");
    let _ = writeln!(f, "{command}");
    Some(f)
}

/// `qemu-img`, a QEMU build product rather than a workspace binary, so it
/// doesn't sit next to the launcher and player. In an installed tree it
/// is `libexec/2ksbox/qemu-img`, not `bin/`, because it is our patched
/// build and must neither shadow nor be shadowed by the system's own on
/// `PATH`. In a checkout it is `build/qemu/qemu-img`, the same path the
/// test scripts use. `LAUNCHER_QEMU_IMG_BIN` overrides both.
pub fn qemu_img_binary() -> PathBuf {
    if let Ok(bin) = std::env::var("LAUNCHER_QEMU_IMG_BIN") {
        return bin.into();
    }
    let exe = if cfg!(windows) { ".exe" } else { "" };
    crate::paths::resource(
        &format!("libexec/2ksbox/qemu-img{exe}"),
        &format!("build/qemu/qemu-img{exe}"),
    )
}

/// Create a new qcow2 disk image for the wizard's "new disk" path.
pub fn create_disk(path: &std::path::Path, size_gb: u32) -> std::io::Result<()> {
    let bin = qemu_img_binary();
    let status = crate::console::command(&bin)
        .args(["create", "-f", "qcow2"])
        .arg(path)
        .arg(format!("{size_gb}G"))
        .status()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))?;
    if status.success() {
        Ok(())
    } else {
        Err(std::io::Error::other(format!("qemu-img create exited with {status}")))
    }
}
