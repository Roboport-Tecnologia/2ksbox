//! cxx-qt's cargo-only build: no CMake, no Corrosion. `CxxQtBuilder`
//! finds Qt through `qmake6`, runs `moc` and `qmltyperegistrar` over the
//! QObjects the bridges declare, compiles the generated C++ and links it
//! into this binary, so `cargo build` is the whole build command, as
//! everywhere else in the tree.
//!
//! The QML files are compiled into the binary as a Qt resource, hence
//! the `qrc:/qt/qml/<uri as a path>/…` URL `main.rs` loads: an installed
//! launcher has no `qml/` directory beside it.

use cxx_qt_build::{CxxQtBuilder, QmlModule};
use qt_build_utils::QResourceFile;

include!("../packaging/windows/win-icon.rs");

fn main() {
    // The icon Explorer shows for the launcher and its manifest: this crate becomes
    // 2ksbox.exe in the Windows package.
    embed_windows_resources();
    // On a Mac the Qt is ours (scripts/build-deps.sh, docs/build-macos.md
    // "The libraries"), under build/deps/<arch>, and cxx-qt-build finds
    // Qt through QMAKE or a qmake6 on PATH. A `cargo build` run by hand
    // without QMAKE once found Homebrew's qmake6 and linked the launcher
    // against a Qt the package then failed on (built for macOS 26, with
    // QtDBus and brotli in its closure). So when QMAKE is unset and our
    // Qt is built, name it here; a preset QMAKE still wins.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos")
        && std::env::var_os("QMAKE").is_none()
    {
        let arch = match std::env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
            Ok("aarch64") => "arm64",
            Ok(other) => other,
            Err(_) => "arm64",
        }
        .to_string();
        let qmake = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../build/deps")
            .join(&arch)
            .join("bin/qmake");
        println!("cargo:rerun-if-env-changed=QMAKE");
        println!("cargo:rerun-if-changed={}", qmake.display());
        if qmake.is_file() {
            // SAFETY: single-threaded build script, before anything reads it.
            unsafe { std::env::set_var("QMAKE", &qmake) };
        }
    }
    // `appearance.cpp` calls `QQuickStyle`, and it is compiled into the
    // generated archive that the linker reaches after the Qt import
    // libraries `qt_module` names. ELF doesn't care, but a PE import
    // library only satisfies symbols already undefined when the linker
    // walks past it, so the Windows build names it again at the end with
    // a `rustc-link-arg` (M11).
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        println!("cargo::rustc-link-arg=-lQt6QuickControls2");
    }
    CxxQtBuilder::new_qml_module(
        // The same reverse-DNS identity as the rest of the product
        // (ADR-011): `com._2ksbox.…`, the leading digit escaped because
        // neither a QML module URI nor a D-Bus name may start with one.
        QmlModule::new("com._2ksbox.launcher").qml_files([
            "qml/Main.qml",
            "qml/AboutWindow.qml",
            "qml/MacMenu.qml",
            "qml/Disclosure.qml",
            "qml/FirstRunDialog.qml",
            "qml/FirstRunResultDialog.qml",
            "qml/PathField.qml",
            "qml/PresetCollection.qml",
            "qml/WizardWindow.qml",
            "qml/CloneWindow.qml",
            "qml/DiscShelfWindow.qml",
            "qml/SnapshotsWindow.qml",
            "qml/ShaderProfilesWindow.qml",
            "qml/ShaderEditorWindow.qml",
        ]),
    )
    // Windows: `std::call_once` in cxx-qt's generated crate initialiser
    // reaches a `__once_proxy` in `libstdc++-6.dll` that reads its
    // argument from a different emutls registry than this binary writes
    // it to, and calls the NULL it finds before `main`, so the launcher
    // never started on Windows (M11). This defines the proxy locally,
    // where the two halves agree. The file compiles to nothing elsewhere.
    .cpp_file("src/once_proxy.cpp")
    // The window icon: one call into QGuiApplication that cxx-qt-lib
    // does not bind (it has QImage, not QIcon).
    .cpp_file("src/window_icon.cpp")
    // The headless probe's stand-in for the title bar's close button
    // (`src/close_event.cpp`): a close event, which nothing in
    // cxx-qt-lib can send.
    .cpp_file("src/close_event.cpp")
    // The probe's "which window has the keyboard"
    // (`src/focus_window.cpp`), which QML's `Window.active` cannot answer.
    .cpp_file("src/focus_window.cpp")
    // Which Quick Controls style and colour scheme (`src/appearance.cpp`).
    // It calls `QQuickStyle`, so that module is linked as well as the
    // ones the QML imports pull in.
    .cpp_file("src/appearance.cpp")
    .qt_module("QuickControls2")
    // The About window's picture, at the module's own `qrc:` prefix as
    // `icon/2ksbox-128.png`.
    .qrc_resources([QResourceFile::new("../packaging/icon/2ksbox-128.png").alias("icon/2ksbox-128.png")])
    .files([
        "src/qt/about.rs",
        "src/qt/diag.rs",
        "src/qt/browse.rs",
        "src/qt/clone_machine.rs",
        "src/qt/discs.rs",
        "src/qt/firstrun.rs",
        "src/qt/machines.rs",
        "src/qt/shaders.rs",
        "src/qt/snaps.rs",
        "src/qt/wizard.rs",
    ])
    .build();
}
