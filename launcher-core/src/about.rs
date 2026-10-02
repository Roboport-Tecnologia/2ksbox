//! The About window's words: the version, one line on what 2ksbox is,
//! and the projects it is built on, grouped by what they do for it.
//! Here and not in a front end (ADR-014), so every launcher thanks the
//! same people in the same order. `launcherx --about` prints it.
//!
//! The list is the projects the app runs or ships, plus Wine, which a
//! host below DXVK's floor brings itself. A library one of these pulls
//! in on its own is credited by that project, not here.

/// One project, as the window shows it.
pub struct Credit {
    pub name: &'static str,
    /// What it does for 2ksbox, in a few words.
    pub what: &'static str,
    /// Its licence as an SPDX expression, or a phrase when it has none.
    pub license: &'static str,
    pub url: &'static str,
}

/// A heading and the projects under it.
pub struct Group {
    pub title: &'static str,
    pub credits: &'static [Credit],
}

pub const NAME: &str = "2ksbox";
pub const VERSION: &str = env!("CARGO_PKG_VERSION");
pub const TAGLINE: &str = "Old Windows and DOS machines that play like native apps.";
pub const LICENSE: &str = "Free software under the GNU GPL, version 2.";
pub const URL: &str = "https://github.com/davidrios/2ksbox";
pub const THANKS: &str = "Built on the work of these projects. Thank you.";

const fn c(name: &'static str, what: &'static str, license: &'static str, url: &'static str) -> Credit {
    Credit { name, what, license, url }
}

pub const GROUPS: &[Group] = &[
    Group {
        title: "Emulation",
        credits: &[
            c("QEMU", "The emulator every machine runs on", "GPL-2.0", "https://www.qemu.org"),
            c("qemu-3dfx", "OpenGL pass-through for the guest", "GPL-2.0", "https://github.com/kjliew/qemu-3dfx"),
            c("86Box", "The Voodoo 2", "GPL-2.0", "https://86box.net"),
            c("SeaBIOS", "The PC BIOS", "LGPL-3.0", "https://www.seabios.org"),
            c("EDK II", "UEFI firmware for Windows 11", "BSD-2-Clause-Patent", "https://www.tianocore.org"),
            c("libtpms", "The TPM for Windows 11", "BSD-3-Clause", "https://github.com/stefanberger/libtpms"),
            c("libslirp", "Guest networking", "BSD-3-Clause", "https://gitlab.freedesktop.org/slirp/libslirp"),
            c("GLib", "QEMU's support library", "LGPL-2.1", "https://gitlab.gnome.org/GNOME/glib"),
        ],
    },
    Group {
        title: "Graphics",
        credits: &[
            c("DXVK", "Direct3D 9 on Vulkan", "Zlib", "https://github.com/doitsujin/dxvk"),
            c("Wine", "Direct3D on hosts without Vulkan 1.3", "LGPL-2.1", "https://www.winehq.org"),
            c("Mesa", "Vulkan on macOS (KosmicKrisp)", "MIT", "https://mesa3d.org"),
            c("wgpu", "The player's renderer", "MIT OR Apache-2.0", "https://wgpu.rs"),
            c("librashader", "Runs the CRT shaders", "MPL-2.0 OR GPL-3.0", "https://github.com/SnowflakePowered/librashader"),
            c("libretro slang-shaders", "The CRT shaders", "Various", "https://github.com/libretro/slang-shaders"),
        ],
    },
    Group {
        title: "Sound",
        credits: &[
            c("Nuked-OPL3", "The OPL3 FM chip", "LGPL-2.1", "https://github.com/nukeykt/Nuked-OPL3"),
            c("Munt and moont", "The Roland MT-32", "LGPL-2.1", "https://github.com/munt/munt"),
            c("RustySynth", "SoundFont music", "MIT", "https://github.com/sinshu/rustysynth"),
            c("cpal", "Audio output", "Apache-2.0", "https://github.com/RustAudio/cpal"),
        ],
    },
    Group {
        title: "Apps",
        credits: &[
            c("Qt", "The launcher's interface", "LGPL-3.0", "https://www.qt.io"),
            c("CXX-Qt", "Rust and Qt together", "MIT OR Apache-2.0", "https://github.com/KDAB/cxx-qt"),
            c("winit", "The player's window", "Apache-2.0", "https://github.com/rust-windowing/winit"),
            c("gilrs", "Game controllers", "MIT OR Apache-2.0", "https://gitlab.com/gilrs-project/gilrs"),
            c("UIDE", "The DOS CD-ROM driver", "Free with source", "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/pkg-html/uide.html"),
        ],
    },
];

/// The whole window as plain text, for `launcherx --about`.
pub fn text() -> String {
    use std::fmt::Write;
    let mut s = String::new();
    let _ = writeln!(s, "{NAME} {VERSION}\n{TAGLINE}\n{LICENSE}\n{URL}\n\n{THANKS}");
    for group in GROUPS {
        let _ = writeln!(s, "\n{}", group.title);
        for c in group.credits {
            let _ = writeln!(s, "  {:<24} {:<40} {:<20} {}", c.name, c.what, c.license, c.url);
        }
    }
    s
}
