// The application icon and manifest, inside the .exe.
//
// `include!`d by the build script of every crate that produces a Windows
// binary someone sees in Explorer (`launcher-mitsuami`, `player-mitsuami`,
// `player`).
// An `include!` rather than a crate on purpose. A build-dependency would
// land in `Cargo.lock`, and the Flatpak's offline build declares every
// crate there with a checksum (`packaging/flatpak/cargo-sources.json`), so
// a Windows-only dependency would have to be vendored for a Linux build
// that never uses it.
//
// Explorer, the taskbar and Alt-Tab draw the *lowest-numbered* icon
// resource, so the .rc names it `1`. The manifest
// (`packaging/windows/app.manifest`) is resource 1 of type 24
// (RT_MANIFEST), the one CreateProcess reads; with it present, mingw's
// linker leaves out its default manifest, which declares no DPI
// awareness and makes the Windows App Certification Kit warn about the
// Store package. The resource script holds nothing else (no VERSIONINFO
// block; nobody maintains a version string yet).
//
// For a GNU-target binary mingw's `windres` turns the .rc into a COFF
// object and `rustc-link-arg-bins` hands it to the linker for every binary
// of the crate. A host with no windres gets a warning and a binary with no
// icon and the default manifest, not a failed build.
//
// The launcher and player-mitsuami are MSVC binaries (WinUI 3,
// `build-windows.sh mitsuami`). There windres writes a `.res`, which Microsoft's linker
// takes as an input file as it is, and the linker is told to make no
// manifest of its own, since ours is resource 1 already. The MSVC build
// runs in MSYS2's MINGW64 shell, whose `windres` is on PATH.
#[allow(dead_code)]
fn embed_windows_resources() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let ico = root.join("packaging/icon/2ksbox.ico");
    let manifest = root.join("packaging/windows/app.manifest");
    // The icon set is generated (`scripts/gen-icons.sh`) but checked in,
    // so this is a plain file read at build time on any machine.
    println!("cargo:rerun-if-changed={}", ico.display());
    println!("cargo:rerun-if-changed={}", manifest.display());
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    let rc = out.join("icon.rc");
    let msvc = std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");
    let obj = out.join(if msvc { "icon.res" } else { "icon.o" });
    // The paths go into the .rc quoted, and a backslash there is an
    // escape, so use forward slashes, which windres accepts everywhere.
    let ico_path = ico.display().to_string().replace('\\', "/");
    let manifest_path = manifest.display().to_string().replace('\\', "/");
    let script = format!("1 ICON \"{ico_path}\"\n1 24 \"{manifest_path}\"\n");
    if std::fs::write(&rc, script).is_err() {
        println!("cargo:warning=could not write {}: the .exe gets no icon", rc.display());
        return;
    }
    // The target-prefixed name first: a Linux host has both `windres`
    // (its own binutils, which cannot write COFF for this target) and
    // `x86_64-w64-mingw32-windres`, and only the second is right.
    let target = std::env::var("TARGET").unwrap_or_default();
    let prefixed = format!("{}-windres", target.replace("-pc-windows-gnu", "-w64-mingw32"));
    for windres in [prefixed.as_str(), "windres"] {
        match std::process::Command::new(windres)
            .args(["-I", &root.display().to_string()])
            .arg(&rc)
            .args(["-O", if msvc { "res" } else { "coff" }, "-o"])
            .arg(&obj)
            .status()
        {
            Ok(status) if status.success() => {
                println!("cargo:rustc-link-arg-bins={}", obj.display());
                if msvc {
                    println!("cargo:rustc-link-arg-bins=/MANIFEST:NO");
                }
                return;
            }
            Ok(status) => {
                println!("cargo:warning={windres} failed ({status}): the .exe gets no icon");
                return;
            }
            // Not installed under this name: try the next one.
            Err(_) => continue,
        }
    }
    println!("cargo:warning=no windres for {target}: the .exe gets no icon");
}
