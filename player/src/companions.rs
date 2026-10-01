//! Where an *installed* player's optional companions are, and how QEMU is
//! told about them.
//!
//! Several of the things a guest can use are `dlopen`ed by QEMU itself,
//! late, by a search that starts at `build/…` in the checkout this binary
//! was built from (`d3dpt/hw/d3dpt_exec_load.c`). A package has no
//! checkout, so each of them names an environment variable as its first
//! candidate, and this module fills those in:
//!
//! * `D3DPT_EXEC_LIB`, the Direct3D executor (doc 14).
//! * `D3DPT_DXVK_LIB`, the DXVK `d3d9` the executor runs on, which it
//!   `dlopen`s in turn and which is not named like the others.
//! * `D3DPT_EXEC_REMOTE_LIB`, the executor in another process, on Wine
//!   (ADR-018, M15): what QEMU's loader opens when DXVK finds no Vulkan
//!   device, and `D3DPT_EXEC_HOST` the Windows program that process runs
//!   (`lib/2ksbox/wine/d3dpt-exec-host.exe`, the executor's Windows
//!   build beside it). `D3DPT_WINE` is the Wine itself, which only the
//!   Flatpak's add-on ships (`lib/2ksbox/wine/bin/wine`, M15 step 7);
//!   every other package leaves it unset and the library finds one by
//!   its own rule (`PATH`, a Wine app), which the launcher's probe
//!   follows too for its verdict.
//! * `VK_DRIVER_FILES`, the Vulkan driver the executor needs. Stock macOS
//!   has no Vulkan at all, so a redistributable app carries a loader and
//!   an ICD of its own; on Linux the system's driver is the right one and
//!   nothing is set.
//! * `LIBSYNTH_SF2`, the General MIDI bank the `mpu401` device plays
//!   through (doc 20 §4). Not a `dlopen`, but the same problem. A
//!   machine says `synth=gm` and the file that answers it lives wherever
//!   this build was installed, which is not something to freeze into
//!   every machine's bundle. **Set in a checkout too**, unlike the ones
//!   above, because there QEMU has no search of its own to fall back on.
//!   A library is found beside the binary, a SoundFont is not.
//!
//! Only ever *when the caller left them unset*. A developer running the
//! packaged player with `D3DPT_EXEC_LIB=` pointing at a fresh build is
//! doing that deliberately, and an A/B that the package silently
//! overrode would be worse than useless. Anything missing is left unset,
//! and QEMU reports each absence in its own words ("d3dpt:
//! libd3dpt_exec not found … Direct3D pass-through off").
//!
//! The prefix rule is `launcher_core::paths`', deliberately duplicated
//! rather than depended on: the player links no launcher code, and the
//! rule is two `stat`s.

use std::path::{Path, PathBuf};

/// The install prefix this player is running under, or `None` in a
/// checkout. `share/2ksbox` is the marker, as it is for the launcher.
/// Inside a macOS `.app` that makes the prefix `Contents`, whose
/// `MacOS/` plays the part `bin/` plays elsewhere.
fn install_prefix() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    if cfg!(windows) {
        let dir = exe.parent()?;
        return dir.join("pc-bios").is_dir().then(|| dir.to_path_buf());
    }
    let prefix = exe.parent()?.parent()?;
    prefix.join("share").join("2ksbox").is_dir().then(|| prefix.to_path_buf())
}

/// A packaged file, flat on Windows and under a Unix prefix's
/// `lib`/`share` otherwise (`launcher_core::paths::in_prefix`).
fn in_prefix(prefix: &Path, installed: &str) -> PathBuf {
    if !cfg!(windows) {
        return prefix.join(installed);
    }
    for lead in ["share/2ksbox/", "lib/2ksbox/", "libexec/2ksbox/", "bin/"] {
        if let Some(rest) = installed.strip_prefix(lead) {
            return prefix.join(rest);
        }
    }
    prefix.join(installed)
}

/// The bank the packages ship (doc 20 §4, `soundfonts/README.md`). One
/// name in one place: the packagers stage this file and this is what
/// names it to QEMU.
pub const SOUNDFONT: &str = "TimGM6mb.sf2";

fn set_if_unset_and_present(var: &str, path: PathBuf) {
    if std::env::var_os(var).is_some() || !path.exists() {
        return;
    }
    // SAFETY: main() calls this before any thread exists. The event loop
    // and QEMU's own thread are both started after it returns.
    unsafe { std::env::set_var(var, path) };
}

/// The names `--companions` prints, in the order this module sets them.
const VARS: [(&str, &str); 7] = [
    ("d3dpt-exec", "D3DPT_EXEC_LIB"),
    ("dxvk", "D3DPT_DXVK_LIB"),
    ("d3dpt-remote", "D3DPT_EXEC_REMOTE_LIB"),
    ("wine-host", "D3DPT_EXEC_HOST"),
    ("wine", "D3DPT_WINE"),
    ("vulkan-icd", "VK_DRIVER_FILES"),
    ("soundfont", "LIBSYNTH_SF2"),
];

/// What `announce` resolved, one line each. It answers "did this
/// package ship the thing, and is the copy it found its own?". Called
/// after `announce`, so a name with a path is either the package's file or
/// the caller's own override, and a name without one is a companion this
/// build has none of. `scripts/package-linux.sh` asks the *staged* player
/// this instead of restating the layout, which is what catches a rule that
/// moved on one side only.
pub fn report() {
    match install_prefix() {
        Some(prefix) => println!("prefix         {}", prefix.display()),
        None => println!("prefix         (a checkout: QEMU's own searches find build/…)"),
    }
    for (name, var) in VARS {
        match std::env::var_os(var) {
            Some(value) => println!("{name:<14} {}", Path::new(&value).display()),
            None => println!("{name:<14} (not shipped)"),
        }
    }
}

/// Point QEMU's own `dlopen` searches at the package. A no-op in a
/// checkout, where those searches already find `build/…`.
pub fn announce() {
    // The bank first, because it is the one companion that also has to
    // be found in a checkout: `soundfonts/` in the source tree, the
    // package's own copy otherwise.
    match install_prefix() {
        Some(prefix) => set_if_unset_and_present(
            "LIBSYNTH_SF2",
            in_prefix(&prefix, &format!("share/2ksbox/soundfonts/{SOUNDFONT}")),
        ),
        None => set_if_unset_and_present(
            "LIBSYNTH_SF2",
            Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../soundfonts")).join(SOUNDFONT),
        ),
    }
    let Some(prefix) = install_prefix() else { return };
    let dylib = |stem: &str| {
        let ext = if cfg!(target_os = "macos") {
            "dylib"
        } else if cfg!(windows) {
            "dll"
        } else {
            "so"
        };
        let name = if cfg!(windows) { format!("{stem}.{ext}") } else { format!("lib{stem}.{ext}") };
        in_prefix(&prefix, &format!("lib/2ksbox/{name}"))
    };
    set_if_unset_and_present("D3DPT_EXEC_LIB", dylib("d3dpt_exec"));
    // The other process's library and program (never on Windows, whose
    // fallback is its own Direct3D 9 in process).
    if !cfg!(windows) {
        set_if_unset_and_present("D3DPT_EXEC_REMOTE_LIB", dylib("d3dpt_exec_remote"));
        set_if_unset_and_present("D3DPT_EXEC_HOST", in_prefix(&prefix, "lib/2ksbox/wine/d3dpt-exec-host.exe"));
        set_if_unset_and_present("D3DPT_WINE", in_prefix(&prefix, "lib/2ksbox/wine/bin/wine"));
    }
    // DXVK's own soname, which carries its major version rather than the
    // plain name the other three have. On Windows it is renamed in the
    // package, so that nothing can mistake it for the system's d3d9.dll.
    let dxvk = if cfg!(target_os = "macos") {
        "lib/2ksbox/libdxvk_d3d9.0.dylib"
    } else if cfg!(windows) {
        "bin/dxvk_d3d9.dll"
    } else {
        "lib/2ksbox/libdxvk_d3d9.so.0"
    };
    set_if_unset_and_present("D3DPT_DXVK_LIB", in_prefix(&prefix, dxvk));
    if cfg!(target_os = "macos") && std::env::var_os("VK_ICD_FILENAMES").is_none() {
        set_if_unset_and_present(
            "VK_DRIVER_FILES",
            in_prefix(&prefix, "share/2ksbox/vulkan/icd.d/driver.json"),
        );
    }
}
