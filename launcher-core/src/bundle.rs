//! The machine bundle format (doc 07): a declarative `machine.toml` the
//! launcher reads and writes. "Hand-written bundles + the player binary is
//! a fully supported path" (doc 07). `qemu_args` is the one place that
//! translates a bundle into a real QEMU command line (`qemu-system-i386`,
//! or `qemu-system-x86_64` for Windows 11: `Machine::qemu_target`); no
//! user-visible QEMU command line exists anywhere else.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Which reference guest configuration (doc 06) a machine is modeled on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Family {
    Win98,
    Xp,
    /// Windows 7, 32-bit (track M18): the era PC with a BIOS like XP's,
    /// and the machine Aero runs on. Our adapter with its interrupt
    /// (`irq=on`), which the WDDM display driver needs and which SETUP
    /// installs it on, setting DWM's composition policy with it; Windows
    /// 7's own HD Audio and Intel PRO/1000 drivers for the rest; the CPU
    /// the Windows 7 work was proved on (`-cpu max`), since Windows 7's
    /// software wants SSE2 and a Pentium III has none. 64-bit Windows 7
    /// loads no unsigned kernel driver, so it would get no Aero here.
    Win7,
    /// A DOS machine:MS-DOS or FreeDOS on the same i440FX PC, with the
    /// SB16 the Win98 family already carries "for DOS boxes/games" (doc
    /// 06) and no network card. What makes it a DOS machine is
    /// `CpuSpeed`: the era's software paces itself by how fast the CPU
    /// is, and emulation is far too fast for it (doc 06).
    Dos,
    /// Anything else of the era on the same PC: BeOS, a period Linux,
    /// OS/2. Defined by what it doesn't get: our paravirtual adapter
    /// (`d3dpt-vga`) needs a Windows display driver and the 3D
    /// pass-through is a set of Windows DLLs, so none of it is reachable
    /// here. What is left is hardware every one of these systems shipped
    /// a driver for in the nineties: the Bochs/standard VGA with VBE 2.0,
    /// an RTL8139 and an ES1370. It gets 2D, the CRT shader chain and the
    /// real CD-ROM model, like the DOS family but with PCI cards.
    Other,
    /// Windows 11 (ADR-024, track M20): the first modern box, on a
    /// different PC. A q35 with UEFI (EDK2's secure build, its variable
    /// store a file of the machine's own), a TPM 2.0 (libtpms inside
    /// QEMU, its state another file of the machine's), the disk on AHCI,
    /// HD Audio, the standard VGA and, with networking, an e1000e: every
    /// one of them a device Windows 11 drives in the box. 64-bit, so it
    /// runs on `qemu-system-x86_64`; hardware virtualization by default,
    /// since nothing of the era work applies and emulation is far too
    /// slow for it (M20 step 1: 15x slower to the desktop than KVM).
    Win11,
}

impl Family {
    /// In the order a picker should offer them: the Windows the project is
    /// built around first, oldest to newest, Windows 11 last of them,
    /// then DOS and the era's catch-all.
    pub const ALL: [Family; 6] = [Family::Win98, Family::Xp, Family::Win7, Family::Win11, Family::Dos, Family::Other];

    pub fn label(self) -> &'static str {
        match self {
            Family::Win98 => "Windows 98",
            Family::Xp => "Windows XP",
            Family::Win7 => "Windows 7",
            Family::Dos => "DOS",
            Family::Other => "Other (BeOS, Linux, …)",
            Family::Win11 => "Windows 11",
        }
    }

    /// The era families' machine, an i440FX PC with a BIOS, against the
    /// modern one's q35 with UEFI and a TPM.
    pub fn is_modern(self) -> bool {
        matches!(self, Family::Win11)
    }
}

/// A modern machine's processor, which is the Windows it can install:
/// x64 Windows 11 on an x86_64 guest, Windows 11 on Arm on an aarch64
/// one (track M20). A machine keeps the one it was made with, because its
/// installed disk only boots on that. A new one gets the host's own,
/// which the host's hypervisor runs (`Arch::native`); the other would be
/// emulated. The era's families are always i386 and have no field.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Arch {
    /// The default for a bundle that does not say, which is every
    /// Windows 11 machine made before the Arm one existed.
    #[default]
    X86_64,
    /// Windows 11 on Arm on QEMU's `virt` machine (M20 step 4).
    Aarch64,
}

impl Arch {
    /// The host's own architecture, which its hypervisor (KVM, HVF,
    /// WHPX) can run.
    pub fn native() -> Arch {
        if cfg!(target_arch = "aarch64") {
            Arch::Aarch64
        } else {
            Arch::X86_64
        }
    }

    /// Whether this host's hypervisor runs a machine on QEMU target
    /// `target` (`Machine::qemu_target`). KVM and WHPX run the era's
    /// i386 on an x86_64 host too (QEMU 11.1's i386 WHPX needs patch 84);
    /// a Mac's HVF runs only its own architecture, so there it is
    /// Windows 11 on Arm alone (M20 step 4).
    pub fn hypervisor_runs(target: &str) -> bool {
        let native = Arch::native().qemu_target();
        if cfg!(target_os = "macos") {
            target == native
        } else {
            target == native || (native == "x86_64" && target == "i386")
        }
    }

    /// QEMU's name for it: `qemu-system-<target>`, `libqemu-embed-<target>`.
    pub fn qemu_target(self) -> &'static str {
        match self {
            Arch::X86_64 => "x86_64",
            Arch::Aarch64 => "aarch64",
        }
    }

    /// EDK2's code for it in `pc-bios`: QEMU's secure build on x86_64
    /// (unpacked there by `scripts/prepare-qemu.sh`); on aarch64 our own
    /// build (`scripts/build-edk2.sh`), since QEMU's has no Secure Boot,
    /// which Windows 11's setup requires, and no AHCI driver.
    pub fn efi_code_file(self) -> &'static str {
        match self {
            Arch::X86_64 => "edk2-x86_64-secure-code.fd",
            Arch::Aarch64 => "2ksbox-aarch64-code.fd",
        }
    }

    /// The empty variable store a machine's own is made from (the x86_64
    /// firmware uses QEMU's i386 template; aarch64 our build's own).
    pub fn efi_vars_template(self) -> &'static str {
        match self {
            Arch::X86_64 => "edk2-i386-vars.fd",
            Arch::Aarch64 => "2ksbox-aarch64-vars.fd",
        }
    }
}

/// How fast the guest's CPU is allowed to be, named after the machine it
/// feels like rather than after the knob underneath.
///
/// DOS-era software calibrates delay loops against the CPU it finds and
/// then trusts the answer forever, so on a fast machine it runs wrong:
/// unplayable games, Turbo Pascal's "runtime error 200", music at double
/// speed. Our TCG runs a DOS guest at around 610 million instructions/s
/// on the Linux box (a tight loop under `-cpu pentium3`), which is
/// Pentium III territory; KVM is far beyond that.
///
/// QEMU's only rate control is `-icount`, whose `shift` sets one
/// instruction per 2^shift ns, so the rates below are powers of two and
/// the labels name the real machine each is closest to. The UI states
/// both consequences: it needs `align=on` to pace against the host at
/// all (without it the guest only believes it is slow), and it cannot
/// coexist with KVM, so a throttled machine runs emulated whatever its
/// `accel` says.
///
/// The cap is exact where it matters. Measured on the Linux box with a
/// 100M-instruction loop: 30.6 MIPS asked 31.25, 7.9 asked 7.8. Above
/// ~30 MIPS the alignment only corrects the guest when it falls behind,
/// so the fast entries are a ceiling the host may miss or overshoot.
/// That is why a 1993 game should get one of the two slowest entries.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CpuSpeed {
    /// No throttle: as fast as this host emulates (or KVM, if the machine
    /// asked for it). Right for Windows, wrong for most DOS software.
    #[default]
    #[serde(rename = "unthrottled")]
    Unthrottled,
    #[serde(rename = "pentium-133")]
    Pentium133,
    #[serde(rename = "pentium-75")]
    Pentium75,
    #[serde(rename = "486dx2-66")]
    Dx266,
    #[serde(rename = "486sx-25")]
    Sx25,
    #[serde(rename = "386dx-33")]
    Dx33,
    #[serde(rename = "286-12")]
    At286,
}

impl CpuSpeed {
    /// In the order a combo box should offer them: fastest first, because
    /// full speed is right for everything but a DOS game, and the list
    /// then reads downwards through the eras.
    pub const ALL: [CpuSpeed; 7] = [
        CpuSpeed::Unthrottled,
        CpuSpeed::Pentium133,
        CpuSpeed::Pentium75,
        CpuSpeed::Dx266,
        CpuSpeed::Sx25,
        CpuSpeed::Dx33,
        CpuSpeed::At286,
    ];

    /// `-icount shift=`, or `None` for no throttle at all.
    pub fn icount_shift(self) -> Option<u32> {
        match self {
            CpuSpeed::Unthrottled => None,
            CpuSpeed::Pentium133 => Some(3), // 125 M instructions/s
            CpuSpeed::Pentium75 => Some(4),  // 62.5
            CpuSpeed::Dx266 => Some(5),      // 31.25
            CpuSpeed::Sx25 => Some(6),       // 15.6
            CpuSpeed::Dx33 => Some(7),       // 7.8
            CpuSpeed::At286 => Some(8),      // 3.9
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            CpuSpeed::Unthrottled => "Full speed (no throttle)",
            CpuSpeed::Pentium133 => "Pentium 133 (~125 M instructions/s)",
            CpuSpeed::Pentium75 => "Pentium 75 (~62 M)",
            CpuSpeed::Dx266 => "486DX2-66 (~31 M)",
            CpuSpeed::Sx25 => "486SX-25 (~16 M)",
            CpuSpeed::Dx33 => "386DX-33 (~8 M)",
            CpuSpeed::At286 => "286-12 (~4 M)",
        }
    }
}

/// The display adapter, on the families that have a choice of one.
///
/// There are two valid answers and nothing here can pick between them.
/// On Windows it is our adapter and driver (docs 15, 19) against the
/// in-box driver Windows already has. Ours carries the whole display
/// path (the mode table, the linear frame buffer the player scans out,
/// the page flips that pace a game, the Direct3D DDI). The Cirrus is the
/// fallback when the driver is not installed yet, for an A/B, or when a
/// title misbehaves on ours. On `Other` there is no driver of ours, so it
/// is one standard adapter against another, and only the person
/// installing the guest knows which has a driver in the box.
///
/// Every entry is an adapter with a real VGA BIOS, so any of them boots
/// anything; the difference is what the guest finds a driver for.
/// Which entries a family offers is `video_choices`, and the **first of
/// them is that family's default**.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Video {
    /// `d3dpt-vga`, our own paravirtual adapter, driven by our display
    /// driver from the guest-tools ISO (docs 15, 19). Windows only, since
    /// there is no driver for it anywhere else. A guest without the
    /// driver comes up on the plain VGA the device also provides.
    #[serde(rename = "d3dpt")]
    D3dpt,
    /// QEMU's standard VGA: the Bochs adapter, VBE 2.0 and a linear
    /// frame buffer. What a period VESA driver wants, what a modern
    /// Linux binds `bochs-drm` to, and the later of the two VESA BIOSes
    /// a DOS title can find. **No XP driver at all** (XP falls back to
    /// 800×600×4 vga.sys), so the Windows families do not offer it.
    Std,
    /// Cirrus Logic GD5446. A chip that really existed, so a guest of
    /// the era is likely to have a native driver for it: Windows 98 and
    /// XP both have one in the box, and so do BeOS R5 and XFree86.
    Cirrus,
}

impl Video {
    /// Every variant, for serde round-trips and label lookups. **Not what
    /// a picker offers**; that is `video_choices(family)`, because some
    /// of these are wrong on any given family.
    pub const ALL: [Video; 3] = [Video::D3dpt, Video::Std, Video::Cirrus];

    pub fn label(self) -> &'static str {
        match self {
            Video::D3dpt => "2ksbox adapter (d3dpt-vga)",
            Video::Std => "Standard VGA (Bochs, VBE 2.0)",
            Video::Cirrus => "Cirrus Logic GD5446",
        }
    }

    /// The arguments that put this adapter on the machine. Our own is a
    /// `-device` on a machine with no `-vga` at all, and it carries the
    /// same PCI address the plain `-vga` adapters are given by the
    /// machine itself (0x02, measured), so the cards pinned below it do
    /// not move when the adapter is changed under an installed guest.
    fn args(self) -> [&'static str; 2] {
        match self {
            Video::D3dpt => ["-device", "d3dpt-vga,addr=0x02"],
            Video::Std => ["-vga", "std"],
            Video::Cirrus => ["-vga", "cirrus"],
        }
    }
}

/// The adapters a family offers, **first one its default**, or empty
/// where there is nothing to choose.
///
/// The Windows families choose between our adapter and the one Windows
/// has an in-box driver for. They are not offered the standard VGA,
/// which has no XP driver at all. Both start on ours. `Other` chooses
/// between the two standard adapters, since nothing of ours runs there.
/// DOS chooses between those same two for a reason unrelated to drivers:
/// its titles program the adapter themselves, so what changes is
/// **which VESA BIOS the game finds**, the Bochs one's VBE 2.0 with its
/// linear frame buffer or the Cirrus's of the period.
pub fn video_choices(family: Family) -> &'static [Video] {
    match family {
        // XP starts on ours: the driver is the whole display path there
        // and every game the M4/M7 tracks were built on runs through it.
        Family::Xp => &[Video::D3dpt, Video::Cirrus],
        // Win98 starts on ours too (user decision). The Cirrus stays one
        // pick away as the in-box driver and the A/B.
        Family::Win98 => &[Video::D3dpt, Video::Cirrus],
        // Windows 7 starts on ours, with its interrupt: the WDDM driver
        // and Aero (track M18). The other choice is the standard VGA,
        // which Windows 7's own VGA driver runs through VBE: Windows 7
        // has no Cirrus driver.
        Family::Win7 => &[Video::D3dpt, Video::Std],
        Family::Other => &[Video::Std, Video::Cirrus],
        // DOS starts on the standard VGA (user decision): its VBE 2.0
        // and linear frame buffer are the fuller of the two VESA BIOSes
        // a title can find. The Cirrus (what a DOS machine got while the
        // adapter was hardcoded) is the other half of an A/B that only a
        // title whose modes come out wrong on one BIOS can settle. Our
        // own adapter is not offered, since it has no DOS driver.
        Family::Dos => &[Video::Std, Video::Cirrus],
        // Windows 11's Basic Display Adapter drives the standard VGA's
        // linear frame buffer at any resolution; nothing of ours runs
        // there (an XP-model driver does not load past Windows 7).
        Family::Win11 => &[Video::Std],
    }
}

/// Which Direct3D 9 implementation the host runs the paravirtual
/// device's executor on (ADR-007 and its second amendment). It is a
/// property of the host, not the guest (every guest sees the same device
/// either way). It is on the machine form because a host can have both,
/// and a user whose game draws wrong on one wants the other without a
/// rebuild or an environment variable.
///
/// Only Windows has two: DXVK everywhere, and there also the system's own
/// `d3d9.dll`, which a host below DXVK's Vulkan 1.3 floor (ADR-013:
/// pre-Broadwell Intel, Kepler and older, TeraScale) has instead of
/// nothing. On Linux and macOS the executor refuses `system` with a line
/// in the log, so the picker does not offer it there (`d3d9_choices`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum D3d9 {
    /// DXVK, and on Windows the system's own Direct3D 9 when this host
    /// cannot run DXVK: no Vulkan 1.3, or only a software Vulkan device,
    /// where a real card's D3D9 driver is the faster of the two.
    Auto,
    /// DXVK or no pass-through at all. The frames every golden in
    /// `reference/d3d` was taken with.
    Dxvk,
    /// Windows' own Direct3D 9. On a host that has both, this is the A/B
    /// between the two rasterisers.
    System,
}

impl D3d9 {
    pub const ALL: [D3d9; 3] = [D3d9::Auto, D3d9::Dxvk, D3d9::System];

    pub fn label(self) -> &'static str {
        match self {
            D3d9::Auto => "Automatic",
            D3d9::Dxvk => "DXVK (needs Vulkan 1.3)",
            D3d9::System => "This PC's own Direct3D 9",
        }
    }

    /// The one line under the picker: what this entry is for, on the
    /// host this launcher runs on and no other (user decision: a note
    /// that talks about Linux to someone on Windows is noise). Not a
    /// front end's to write (ADR-014).
    pub fn note(self) -> &'static str {
        match self {
            D3d9::Auto if cfg!(windows) => {
                "DXVK, or this PC's own Direct3D 9 when it has no Vulkan 1.3 GPU."
            }
            D3d9::Auto => "DXVK, or through Wine on this host when it has no Vulkan 1.3 GPU.",
            D3d9::Dxvk => "The tested path. Without a Vulkan 1.3 GPU there is no Direct3D at all.",
            D3d9::System => {
                "This PC's own Direct3D 9, whatever the card. Older cards draw well here; not the tested path."
            }
        }
    }
}

/// What the picker offers: only what this host can run (user decision:
/// an entry for another OS is noise), so the system Direct3D 9 is
/// listed on Windows alone. A machine file is still
/// portable: a bundle saying `system` opened on a Linux or macOS host
/// shows as Automatic and keeps its value until something else is
/// picked (`effective_video`'s rule, one level up), and the executor
/// falls back to DXVK for it there either way.
pub fn d3d9_choices() -> &'static [D3d9] {
    if cfg!(windows) {
        &D3d9::ALL
    } else {
        &[D3d9::Auto, D3d9::Dxvk]
    }
}

/// What a host gamepad does for this machine (M13,
/// `docs/tracks/m13-gamepads.md`).
///
/// All three guest-facing paths exist now: a USB HID gamepad (patch 26)
/// for the families with a USB stack, a gameport at 0x201 (patch 27) for
/// the ones without, and the key mapping for everything else. Which of
/// them a family is offered is `pad_choices`, and a path is offered only
/// where this project can say what the guest needs, the same rule the
/// display-adapter picker follows for an adapter a family has no driver
/// for.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Pad {
    /// A controller plugged into the host does nothing. The default for
    /// a reason: with `Keys` a resting stick that drifts past the
    /// threshold holds an arrow key down, and on a desktop that is a
    /// cursor sliding across the screen with no visible cause. Someone
    /// who wants a pad says so.
    #[default]
    None,
    /// A real USB HID gamepad on the machine (`-usb -device
    /// usb-gamepad`, patch 26). Two sticks, an 8-way hat and twelve
    /// buttons, bound by the guest's own in-box HID stack; DirectInput
    /// and `joy.cpl` see it on the first boot after it is added. This is
    /// the entry a game of the era can use: it enumerates as a
    /// controller, and the sticks are analog.
    ///
    /// **Confirmed with a real controller** on XP and on Windows 98 SE,
    /// both showing it in the Game Controllers panel. The wizard tells
    /// them apart: XP needs nothing, and 98 SE binds its own driver but
    /// asks for the Windows 98 source files the first time (the CD, or
    /// the CAB folder on the disk). Without that warning the user faces
    /// an unexplained file-copy dialog.
    ///
    /// Not offered on DOS, which has no USB stack; path B's gameport
    /// covers it. Windows 98 first edition is still in doubt: its USB
    /// support predates a reliable HID class, and it may want the USB
    /// supplement. Untried.
    Usb,
    /// The analog joystick port at 0x200-0x207 (`-device gameport`, patch
    /// 27): four one-shots and four buttons, which is the whole of what
    /// the hardware ever had. The **only** path that reaches DOS, where a
    /// game reads the port itself and there is no USB stack for path A,
    /// and the period-correct one: the sticks of the era plugged into
    /// this connector.
    ///
    /// The guest has work to do, and the wizard says so: the port is not
    /// Plug and Play, so Windows 9x wants Add New Hardware and then a
    /// calibration pass in the Game Controllers panel. DOS needs neither.
    ///
    /// Nobody here has done that, and the wizard steers away from it. On
    /// 98 a Windows game gets its joystick from [`Pad::Usb`] through both
    /// APIs it can call (DirectInput, and winmm's `joyGetPosEx` on top of
    /// VJOYD, measured by the `pad-guest-98` check), so the driver half of
    /// this port was dropped from M13. This variant is for DOS, which has
    /// no USB stack, and a DOS box under Windows 98, which reads 0x201
    /// itself.
    ///
    /// Not offered on XP: `gameenum.sys` is still in the box, but nothing
    /// enumerates a non-PnP port and Microsoft was already retiring
    /// analog sticks, so XP gets path A. Nor on `Other`, an OS this
    /// project cannot name a driver step for.
    Gameport,
    /// The pad presses keys: the player maps its controls onto the key
    /// events it already sends, against `gamepad::default_key_bindings`.
    /// Reaches **every** guest (DOS, Win98 FE, XP, `Other`), because
    /// there is no device for the guest to support. The cost is that it
    /// is a mapping, not a controller: nothing analog, and a game that
    /// enumerates DirectInput or reads 0x201 still finds nothing.
    Keys,
}

impl Pad {
    pub const ALL: [Pad; 4] = [Pad::None, Pad::Usb, Pad::Gameport, Pad::Keys];

    pub fn label(self) -> &'static str {
        match self {
            Pad::None => "No gamepad",
            Pad::Usb => "USB gamepad",
            Pad::Gameport => "Gameport joystick",
            Pad::Keys => "Gamepad as keyboard keys",
        }
    }

    /// The name this serializes to. Must agree with the `rename_all`
    /// above: `pad_lenient` reads through this, so a disagreement would
    /// make every bundle's `pad` field silently fall back to the default.
    /// The `pad` check in `scripts/test.sh` writes a machine and reads it
    /// back to prove they still agree.
    pub fn name(self) -> &'static str {
        match self {
            Pad::None => "none",
            Pad::Usb => "usb",
            Pad::Gameport => "gameport",
            Pad::Keys => "keys",
        }
    }

    pub fn from_name(name: &str) -> Option<Pad> {
        Pad::ALL.into_iter().find(|p| p.name() == name)
    }
}

/// Reads `pad` **leniently**: a value this build has never heard of
/// becomes `None` (the family default) instead of failing the whole
/// bundle.
///
/// Unlike every other enum in this file, `Pad` was known to be gaining
/// variants (M13's paths A and B added `usb` and `gameport` after the
/// field shipped), and the rule stays. Otherwise a machine made in a
/// newer build and opened in an older one would not load at all, and
/// the user would lose sight of its disk, discs and shader profile over
/// a controller setting.
fn pad_lenient<'de, D>(d: D) -> Result<Option<Pad>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Ok(Option::<String>::deserialize(d)?.and_then(|s| Pad::from_name(&s)))
}

/// What each family offers, **first one its default**.
///
/// The two asymmetries are the two devices, and each is a driver
/// question rather than a preference. `Usb` needs a guest with a USB
/// stack, which DOS has not got at all. `Gameport` needs a guest that
/// will talk to a port nothing enumerates: DOS reads it directly and 9x
/// has "Standard Game Port" for it, while XP would need a driver
/// installed by hand for a class of device it was already dropping, and
/// the `Other` family is an OS this project cannot name a step for. So
/// DOS is offered the gameport and not the HID pad, XP and `Other` the
/// HID pad and not the gameport, and Win98, which has both stacks, both.
///
/// Every family starts on `None`. A machine nobody asked for a pad on
/// should not grow a device in its Device Manager. Networking follows the
/// same rule: it is off on new machines because a guest that waits on
/// DHCP at boot is worse than one with no network. A pad is one pick
/// away either way.
pub fn pad_choices(family: Family) -> &'static [Pad] {
    match family {
        // No USB stack, so no HID gamepad. Nothing to warn about: the
        // entry is not offered, as the display-adapter picker does not
        // offer an adapter a family has no driver for.
        Family::Dos => &[Pad::None, Pad::Gameport, Pad::Keys],
        Family::Win98 => &[Pad::None, Pad::Usb, Pad::Gameport, Pad::Keys],
        Family::Xp | Family::Win7 | Family::Other => &[Pad::None, Pad::Usb, Pad::Keys],
        Family::Win11 => &[Pad::None, Pad::Usb, Pad::Keys],
    }
}

/// The pad setting a family starts on. Always the first of
/// `pad_choices`, so the list and the default cannot disagree.
pub fn default_pad(family: Family) -> Pad {
    pad_choices(family).first().copied().unwrap_or(Pad::None)
}

/// The digital sound card and, for the cards that carried one, the FM
/// chip that comes with it (doc 20 §6).
///
/// The era's split is why this is a choice: a Sound Blaster is what a
/// DOS game knows how to find and what Windows 98 has a driver for in
/// the box, and an AC'97 is what a machine of 2001 has and sounds
/// better. Neither is right for both families.
///
/// **The FM chip is not in this list, on purpose.** A card either had
/// one or it did not: an SB16 carries a YMF262 at 0x388 and at its own
/// base, an AdLib is one, and an AC'97, an ES1370 and a Gravis have
/// none. So picking a card decides whether the machine has FM, the way
/// buying one did, and a game that only knows AdLib music finds it on
/// exactly the machines where it would have.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Sound {
    /// Sound Blaster 16 at 0x220 (IRQ 5, DMA 1/5) **and its OPL3**.
    /// What `BLASTER=A220 I5 D1 H5 T6` describes, and the card every
    /// DOS title of the CD-ROM era has a driver for.
    Sb16,
    /// The Intel AC'97 codec: 2001's card, and XP's in-box driver. No
    /// FM at all, so a DOS box inside such a machine has no music.
    Ac97,
    /// Ensoniq AudioPCI (ES1370): doc 06's card for the `Other`
    /// family, the one BeOS R5 and a period Linux both drive in the box.
    Es1370,
    /// Gravis Ultrasound: a wavetable card, so its music is its own (no
    /// MPU-401 and no FM involved), and the guest needs Gravis's own
    /// drivers and its `ULTRASND` line before anything comes out of it.
    Gus,
    /// An AdLib and nothing else: the OPL3 at 0x388, no digital audio at
    /// all. The 1990 machine, for a title that predates sampled sound.
    Adlib,
    /// No sound card. The machine keeps whatever the music picker gives
    /// it, which is a real configuration: an MPU-401 and a module was how
    /// music was done before cards could play samples.
    None,
    /// Intel HD Audio (the ICH9's controller and a duplex codec), the
    /// Windows 11 machine's card: in-box since Vista.
    Hda,
}

impl Sound {
    /// Every variant, for serde round-trips and label lookups. **Not
    /// what a picker offers**; that is `sound_choices(family)`.
    pub const ALL: [Sound; 7] =
        [Sound::Sb16, Sound::Ac97, Sound::Es1370, Sound::Gus, Sound::Adlib, Sound::Hda, Sound::None];

    pub fn label(self) -> &'static str {
        match self {
            Sound::Sb16 => "Sound Blaster 16 (with FM)",
            Sound::Ac97 => "Intel AC'97",
            Sound::Es1370 => "Ensoniq AudioPCI (ES1370)",
            Sound::Gus => "Gravis Ultrasound",
            Sound::Adlib => "AdLib (FM only)",
            Sound::Hda => "Intel HD Audio",
            Sound::None => "No sound card",
        }
    }

    /// The name a bundle and the debug verbs use.
    pub fn key(self) -> &'static str {
        match self {
            Sound::Sb16 => "sb16",
            Sound::Ac97 => "ac97",
            Sound::Es1370 => "es1370",
            Sound::Gus => "gus",
            Sound::Adlib => "adlib",
            Sound::Hda => "hda",
            Sound::None => "none",
        }
    }

    /// The devices this card is. The PCI cards carry their address for
    /// the reason every pinned address here exists: removing the NIC
    /// above them must not slide them into its slot, which an installed
    /// guest would see as a swapped sound card.
    fn args(self) -> Vec<String> {
        let device = |spec: &str| vec!["-device".to_string(), spec.to_string()];
        match self {
            // Two devices, because the card is two chips: the SB16 for
            // digital audio and the OPL3 that sits at 0x388 and is
            // mirrored at the card's own base, where an SB-aware driver
            // looks for it.
            Sound::Sb16 => {
                let mut v = device("sb16,audiodev=embed0");
                v.extend(device("opl3,audiodev=embed0,sbbase=0x220"));
                v
            }
            Sound::Adlib => device("opl3,audiodev=embed0"),
            Sound::Ac97 => device("AC97,audiodev=embed0,addr=0x04"),
            Sound::Es1370 => device("ES1370,audiodev=embed0,addr=0x04"),
            Sound::Gus => device("gus,audiodev=embed0"),
            // The controller and its codec, two devices, the controller
            // at the ICH9's own address.
            Sound::Hda => {
                let mut v = device("ich9-intel-hda,addr=0x1b");
                v.extend(device("hda-duplex,audiodev=embed0"));
                v
            }
            Sound::None => Vec::new(),
        }
    }
}

/// What is behind the machine's MIDI port (doc 20 §6): an MPU-401 at
/// 0x330 and the synthesizer that plays what the guest writes to it.
///
/// This is the music half of a period machine's audio, which stock QEMU
/// lacks: it has no MPU-401 device, so a game offering "General MIDI" or
/// "Roland MT-32" in its setup program had nothing to talk to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Music {
    /// A SoundFont General MIDI synthesizer, on the bank we ship unless
    /// `soundfont` names another. The bank matters more to how the music
    /// sounds than anything else on this screen.
    Gm,
    /// A Roland CM-32L (the MT-32 family), which needs the user's own
    /// ROM images in `mt32_roms`. What a 1990 title means by "Roland".
    Mt32,
    /// No MIDI port on the machine at all, rather than a port that
    /// swallows notes: a game that found one would pick it and play to
    /// nobody.
    None,
}

impl Music {
    pub const ALL: [Music; 3] = [Music::Gm, Music::Mt32, Music::None];

    pub fn label(self) -> &'static str {
        match self {
            Music::Gm => "General MIDI (SoundFont)",
            Music::Mt32 => "Roland MT-32 / CM-32L",
            Music::None => "No MIDI port",
        }
    }

    pub fn key(self) -> &'static str {
        match self {
            Music::Gm => "gm",
            Music::Mt32 => "mt32",
            Music::None => "none",
        }
    }
}

/// The cards a family offers, **first one its default**.
///
/// Every family keeps the card it already had as its first entry, so no
/// existing machine changes hardware by being opened: 98 and DOS on the
/// Sound Blaster, XP on the AC'97, `Other` on the Ensoniq. The rest are
/// one pick away: a Gravis in a 98 or DOS machine for the games written for one, an AdLib for
/// a 1990 title, and no card at all.
pub fn sound_choices(family: Family) -> &'static [Sound] {
    match family {
        // Windows has the SB16 driver in the box and a DOS box inside 98
        // finds the card it expects. No AC'97 (user, 2026-10-05): DOS
        // games inside 98 get nothing from it.
        Family::Win98 => &[Sound::Sb16, Sound::Gus, Sound::None],
        Family::Dos => &[Sound::Sb16, Sound::Gus, Sound::Adlib, Sound::None],
        // XP's own driver, and the card doc 06 has always given it.
        Family::Xp => &[Sound::Ac97, Sound::Sb16, Sound::None],
        // Windows 7's own HD Audio driver; it has none for the AC'97 or
        // the SB16.
        Family::Win7 => &[Sound::Hda, Sound::None],
        // Standard hardware only, as everywhere else in this family:
        // both of these had a driver in the box on BeOS R5 and on a
        // period Linux, where nothing of ours can be installed after.
        Family::Other => &[Sound::Es1370, Sound::Ac97, Sound::None],
        Family::Win11 => &[Sound::Hda, Sound::None],
    }
}

/// What is on the MIDI port, **first one the family's default**.
///
/// The two families with no synthesizer of their own get one: a DOS
/// machine has nothing but the card's FM otherwise, and Windows 98's
/// own MIDI output is that same FM chip. XP ships a wavetable
/// synthesizer with the operating system and `Other` is a family we add
/// no drivers to, so both start with no port. It is one pick away when a
/// game wants a real MT-32.
pub fn music_choices(family: Family) -> &'static [Music] {
    match family {
        Family::Win98 | Family::Dos => &[Music::Gm, Music::Mt32, Music::None],
        Family::Xp | Family::Other => &[Music::None, Music::Gm, Music::Mt32],
        // An MPU-401 at 0x330 is an ISA device of the era; Windows 11
        // has its own synthesizer and no driver for the port. Windows 7
        // has its own synthesizer too, and the port is not tried there.
        Family::Win7 | Family::Win11 => &[Music::None],
    }
}

/// The card a family starts on. Always the first of `sound_choices`.
pub fn default_sound(family: Family) -> Sound {
    sound_choices(family).first().copied().unwrap_or(Sound::None)
}

/// What a family's MIDI port starts as. Always the first of
/// `music_choices`.
pub fn default_music(family: Family) -> Music {
    music_choices(family).first().copied().unwrap_or(Music::None)
}

/// Which drive the machine boots from. `Auto` leaves the order to QEMU,
/// which tries the hard disk, then the floppy, then the CD. That is right
/// both for an installed Windows and for the wizard's "boot the
/// installer from the CD because the new disk is blank" case.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Boot {
    #[default]
    Auto,
    Disk,
    /// The floppy first, the disk if there is no disk in it: a DOS boot
    /// disk, or a Windows machine being repaired from one.
    Floppy,
    /// The CD first: reinstalling over a disk that still boots.
    Cd,
}

impl Boot {
    pub const ALL: [Boot; 4] = [Boot::Auto, Boot::Disk, Boot::Floppy, Boot::Cd];

    pub fn label(self) -> &'static str {
        match self {
            Boot::Auto => "Automatic",
            Boot::Disk => "Hard disk",
            Boot::Floppy => "Floppy, then hard disk",
            Boot::Cd => "CD, then hard disk",
        }
    }

    fn order(self) -> Option<&'static str> {
        match self {
            Boot::Auto => None,
            Boot::Disk => Some("c"),
            Boot::Floppy => Some("ac"),
            Boot::Cd => Some("dc"),
        }
    }
}

/// How the guest's instructions are executed. Kept in the bundle rather
/// than decided at spawn time, because a user can want to pin it: an era
/// CPU under TCG is the reference behaviour the project is tuned for
/// (docs 13 and 16's x87/SSE fast paths only exist there), while KVM is
/// what makes an XP game playable on a Linux host.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Accel {
    /// Hardware acceleration when the host has it, emulation otherwise.
    /// QEMU itself picks from the `kvm:tcg` (Windows: `whpx:tcg`) list,
    /// so no host probing here can get it wrong. On macOS an era machine
    /// is emulation (`Machine::accel_args`).
    #[default]
    Auto,
    /// Hardware acceleration only: the machine refuses to start without
    /// it, which is what makes it worth choosing over `Auto`. Named `kvm`
    /// in the bundle on every host. The field says what the user asked
    /// for, and each host spells it its own way (`whpx` on Windows), so a
    /// machine directory copied between hosts keeps its meaning.
    Kvm,
    /// Emulation only. The right choice for Win98: KVM runs the guest at
    /// host speed, and Win9x has real fast-CPU bugs (doc 06) that the
    /// `pentium3` model does not protect against, since the speed trips
    /// them.
    Tcg,
}

impl Accel {
    pub const ALL: [Accel; 3] = [Accel::Auto, Accel::Kvm, Accel::Tcg];

    /// The label a front end shows. Here rather than inline in a combo
    /// box, like `Family::label` and `Boot::label`, so every front end
    /// offers the same wording.
    pub fn label(self) -> &'static str {
        match self {
            Accel::Auto => "Automatic",
            // Offered only where the host has it
            // (`wizard::Form::accel_choices`), so it needs no "required"
            // beside "Automatic"; the note under the picker names this
            // host's kind (`crate::player::hw_accel_label`) (user).
            Accel::Kvm => "Hardware virtualization",
            Accel::Tcg => "Emulation",
        }
    }
}

/// One of the QEMU fast paths this project carries as its own patches
/// (`patches/qemu/README.md`, docs 13 and 16, the M8/M9 tracks), with
/// the off switch the patch already gave it.
///
/// Each one shortcuts how the emulator runs or translates guest code, so
/// each arrived with an off switch that serves as the oracle. When a
/// guest computes the wrong number or a game stops drawing, one run with
/// one of these off says whether a fast path did it, instead of bisecting
/// a patch queue against a Windows install.
///
/// They only exist under emulation. A machine on KVM executes on the
/// host CPU directly and none of these is reachable; the form says so
/// rather than showing switches that do nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Optimization {
    X87Fast,
    SseFast,
    SimdFast,
    RepFast,
    X87Pc64As53,
    SmcSameValue,
    SoftImm,
    InlineLookup,
    TbInvalidateFast,
    TlbFloor,
    TlsHotPaths,
    JumpCacheKeep,
    EobChain,
    TlbRetire,
}

/// Where an optimization's switch goes on the command line: a property
/// of the guest CPU (`-cpu pentium3,x87-fast=off`) or of the TCG
/// accelerator itself (`-accel tcg,smc-same-value=off`). The two are not
/// interchangeable, since QEMU looks each name up on a different object,
/// and the accelerator half is why `qemu_args` spells the accelerator as
/// `-accel` rather than `-machine accel=`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Knob {
    Cpu,
    Tcg,
}

impl Optimization {
    /// In the order the form lists them: the arithmetic fast paths
    /// first, in the order they were written (the inexact one last), then
    /// the ones about translation.
    ///
    /// Patch 21's `pinned-regs` is not here (user decision): it crashed
    /// guests and its gain was too small to pursue. The patch keeps its
    /// accelerator property, off by default. A bundle that still has the
    /// entry keeps it in the table, never on the command line, until
    /// "All defaults" (`Optimizations::RETIRED`).
    pub const ALL: [Optimization; 14] = [
        Optimization::X87Fast,
        Optimization::SseFast,
        Optimization::SimdFast,
        Optimization::RepFast,
        Optimization::X87Pc64As53,
        Optimization::SmcSameValue,
        Optimization::SoftImm,
        Optimization::InlineLookup,
        Optimization::TbInvalidateFast,
        Optimization::TlbFloor,
        Optimization::TlsHotPaths,
        Optimization::JumpCacheKeep,
        Optimization::EobChain,
        Optimization::TlbRetire,
    ];

    /// The QEMU property name, which is also the key in `machine.toml`:
    /// one name for the thing, so a bundle can be read against
    /// `patches/qemu/README.md` without a translation table.
    pub fn key(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87-fast",
            Optimization::SseFast => "sse-fast",
            Optimization::SimdFast => "simd-fast",
            Optimization::RepFast => "rep-fast",
            Optimization::X87Pc64As53 => "x87-pc64-as-53",
            Optimization::SmcSameValue => "smc-same-value",
            Optimization::SoftImm => "soft-imm",
            Optimization::InlineLookup => "inline-lookup",
            Optimization::TbInvalidateFast => "tb-invalidate-fast",
            Optimization::TlbFloor => "tlb-floor",
            Optimization::TlsHotPaths => "tls-hot-paths",
            Optimization::JumpCacheKeep => "jump-cache-keep",
            Optimization::EobChain => "eob-chain",
            Optimization::TlbRetire => "tlb-retire",
        }
    }

    fn knob(self) -> Knob {
        match self {
            Optimization::X87Fast
            | Optimization::SseFast
            | Optimization::SimdFast
            | Optimization::RepFast
            | Optimization::X87Pc64As53 => Knob::Cpu,
            Optimization::SmcSameValue
            | Optimization::SoftImm
            | Optimization::InlineLookup
            | Optimization::TbInvalidateFast
            | Optimization::TlbFloor
            | Optimization::TlsHotPaths
            | Optimization::JumpCacheKeep
            | Optimization::EobChain
            | Optimization::TlbRetire => Knob::Tcg,
        }
    }

    /// Whether a machine that says nothing has it on. Everything is on
    /// (turning one off is a diagnosis, not a preference) except
    /// `x87-pc64-as-53`, the one switch that changes what the guest
    /// computes rather than how fast.
    pub fn default_on(self) -> bool {
        !matches!(self, Optimization::X87Pc64As53)
    }

    /// The checkbox's label: what the fast path does, not what it is
    /// called in the patch queue.
    pub fn label(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87 floating point on the host FPU",
            Optimization::SseFast => "SSE floating point on the host",
            Optimization::SimdFast => "MMX and SSE integer instructions inline",
            Optimization::RepFast => "Block string moves as whole-page copies",
            Optimization::X87Pc64As53 => "Run x87 extended precision as double (not exact)",
            Optimization::SmcSameValue => "Skip retranslation when code is rewritten unchanged",
            Optimization::SoftImm => "Read patched operands from the guest's code as it runs",
            Optimization::InlineLookup => "Find the next block without leaving generated code",
            Optimization::TbInvalidateFast => "Skip the block walk for writes that can't hit code",
            Optimization::TlbFloor => "Keep the address-translation cache from shrinking",
            Optimization::TlsHotPaths => "Take the memory-tracking locks once per run, not per write",
            Optimization::JumpCacheKeep => "Keep the block cache across the guest's context switches",
            Optimization::EobChain => "Chain past segment loads, sti and popf when no interrupt waits",
            Optimization::TlbRetire => "Keep the address cache across the guest's own TLB flushes",
        }
    }

    /// The sentence under it: what it buys, and for the one that is off
    /// by default, why.
    pub fn note(self) -> &'static str {
        match self {
            Optimization::X87Fast => {
                "Runs the guest's x87 maths on the host FPU instead of simulating it. Super PI 1M on an \
                 M1 Air: 9:49 off, 1:57 on. Turn it off if a program's arithmetic looks wrong."
            }
            Optimization::SseFast => "Runs SSE/SSE2 floating point on the host's vector unit: 3 to 12x faster.",
            Optimization::SimdFast => {
                "Runs MMX and SSE integer instructions on the host's vector unit: 2 to 4x on software renderers."
            }
            Optimization::RepFast => {
                "Copies a page at a time for REP MOVS/STOS instead of one element per loop: about 30x on \
                 screen blits."
            }
            Optimization::X87Pc64As53 => {
                "Runs code that asks for 64-bit x87 precision at 53 bits on the host FPU. Faster, but the \
                 last bits can differ from a real FPU: games rarely notice, benchmarks may. Off by default."
            }
            Optimization::SmcSameValue => {
                "Skips retranslation when self-modifying code writes back the bytes already there. \
                 Moto Racer: 7 to 22 fps."
            }
            Optimization::SoftImm => {
                "Reads the operands a program patches into its own code at run time instead of \
                 retranslating. Moto Racer: 41 to 58 fps."
            }
            Optimization::InlineLookup => {
                "Finds the next code block after a return or indirect jump without leaving generated \
                 code: 7 to 12% on 7-Zip."
            }
            Optimization::TbInvalidateFast => {
                "Remembers where the code on a page really is, so a write to the data around it returns \
                 at once instead of walking every block."
            }
            Optimization::TlbFloor => {
                "Keeps the address-translation cache at 4096 entries. Windows' flushes shrank it to 64, \
                 where pages collide constantly."
            }
            Optimization::TlsHotPaths => {
                "Takes the memory-tracking locks once per run instead of once per access. Up to 8% of a \
                 game's emulation thread on macOS."
            }
            Optimization::JumpCacheKeep => {
                "Keeps the jump cache across the guest's context switches, checking each entry against \
                 its page instead of emptying it."
            }
            Optimization::EobChain => {
                "Chains straight to the next block after a segment load, sti or popf when no interrupt \
                 is pending."
            }
            Optimization::TlbRetire => {
                "Keeps address translations across the guest's own TLB flushes, rechecking the page \
                 tables they came from."
            }
        }
    }
}

/// A machine's optimization settings. **Only what differs from
/// `Optimization::default_on` is stored**, so a bundle that says nothing
/// runs on the defaults, and an optimization added to the patch queue
/// later arrives on in every bundle that already exists.
///
/// Keyed by the QEMU property name rather than by the enum, so a bundle
/// written by a launcher that knows an optimization this build does not
/// survives a load and a save with the entry intact instead of being
/// refused as an unknown variant.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Optimizations(BTreeMap<String, bool>);

impl Optimizations {
    pub fn enabled(&self, opt: Optimization) -> bool {
        self.0.get(opt.key()).copied().unwrap_or_else(|| opt.default_on())
    }

    pub fn is_default(&self, opt: Optimization) -> bool {
        self.enabled(opt) == opt.default_on()
    }

    /// Turn one on or off. Putting it back on its own default removes
    /// the entry rather than writing it out, so a `machine.toml` holds
    /// only what someone changed.
    pub fn set(&mut self, opt: Optimization, on: bool) {
        if on == opt.default_on() {
            self.0.remove(opt.key());
        } else {
            self.0.insert(opt.key().to_string(), on);
        }
    }

    /// Switches the form offered once and no longer does. Nothing reads
    /// them; "All defaults" removes them with the rest.
    const RETIRED: [&'static str; 1] = ["pinned-regs"];

    /// Every optimization back on its default. Only the ones this build
    /// knows about, and the ones it retired. This build cannot judge an
    /// entry a newer launcher wrote.
    pub fn reset(&mut self) {
        for opt in Optimization::ALL {
            self.0.remove(opt.key());
        }
        for key in Self::RETIRED {
            self.0.remove(key);
        }
    }

    pub fn all_default(&self) -> bool {
        Optimization::ALL.iter().all(|opt| self.is_default(*opt))
    }

    /// Every optimization this build knows about turned **off**: the
    /// one-click control run for "did one of ours break this guest".
    /// Written as explicit `false` entries for the ones whose default is
    /// on, as unticking each box would, so the bundle says what it means
    /// and `--print-args` shows the whole line. Unlike `reset`, which
    /// removes entries.
    ///
    /// **This is not a pristine QEMU.** Some patches of the queue have no
    /// runtime switch (`patches/qemu/README.md` names them), so a guest
    /// that is still wrong with everything here off has cleared only
    /// these switches, not our whole tree.
    pub fn disable_all(&mut self) {
        for opt in Optimization::ALL {
            self.set(opt, false);
        }
    }

    /// Every optimization this build knows about turned **on**, the
    /// other end of the same shortcut. `x87-pc64-as-53` comes on with
    /// it: the switch means what it says, and the warning lives in that
    /// optimization's own note.
    pub fn enable_all(&mut self) {
        for opt in Optimization::ALL {
            self.set(opt, true);
        }
    }

    /// Whether every optimization this build knows about is off, for a
    /// front end deciding whether its "Turn everything off" is worth
    /// offering.
    pub fn all_off(&self) -> bool {
        Optimization::ALL.iter().all(|opt| !self.enabled(*opt))
    }

    /// The same at the other end.
    pub fn all_on(&self) -> bool {
        Optimization::ALL.iter().all(|opt| self.enabled(*opt))
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// What a collapsed section says about itself, so someone who never
    /// opens it still sees that something in there has been changed.
    /// A count rather than "all on", because one of them ships off.
    pub fn summary(&self) -> String {
        let on = Optimization::ALL.iter().filter(|opt| self.enabled(**opt)).count();
        format!("{on} of {} on", Optimization::ALL.len())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Machine {
    pub name: String,
    pub family: Family,
    pub ram_mb: u32,
    /// How to execute the guest, or `None` for "whatever this family
    /// runs as" (`default_accel`). Absent rather than defaulted, so a
    /// bundle written before this field existed follows its family
    /// instead of silently acquiring KVM, which for a Win98 machine would
    /// change how it had been running. Anything this launcher saves
    /// carries an explicit value, since the form always has one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub accel: Option<Accel>,
    /// Whether the machine has a network adapter at all (doc 06's
    /// per-family NIC on QEMU's user-mode NAT). `false` gives the guest
    /// no adapter rather than an unplugged one: "no networking" should
    /// mean Windows never sees a card, never asks for its driver and
    /// never waits on a network at boot.
    ///
    /// Defaults to `false` when the field is absent, like a new machine
    /// (`default_network`). An absent field used to mean on; the user
    /// decided networking is off by default for every machine. The
    /// wizard has always written the field, so only a hand-written or
    /// very old bundle loses its card.
    #[serde(default)]
    pub network: bool,
    /// Whether the machine gets the USB tablet: an absolute pointing
    /// device, so the host pointer and the guest cursor are the same
    /// pointer and the window never has to grab anything (doc 03's
    /// pointer model, doc 06's "Pointer" rows). `false` leaves the
    /// machine the PS/2 mouse the chipset already gives it, which is
    /// relative: the player grabs on a click and Ctrl+Alt+G gives the
    /// pointer back. Mouselook needs that, and it is the only thing a DOS
    /// mouse driver can read.
    ///
    /// Defaults to `true` when the field is absent, which is how every
    /// bundle written before it existed ran (the tablet was
    /// unconditional). A newer launcher must not change a machine's
    /// pointer by reading it.
    #[serde(default = "seamless_mouse_default")]
    pub seamless_mouse: bool,
    /// A 3dfx Voodoo 2 on the PCI bus (`-device voodoo2`: 86Box's
    /// emulation of the chip, doc 21, M14) beside whatever 2D adapter
    /// the machine has. It borrows the monitor from that adapter's
    /// console, as the card borrowed it through a cable. It is the one
    /// Glide on a 2ksbox machine (ADR-020): a game draws on 3dfx's own
    /// `glide2x.dll`, and the guest needs 3dfx's own Voodoo2 driver. Off
    /// unless picked, on
    /// every family, so no machine grows a card by being read by a newer
    /// launcher.
    #[serde(default)]
    pub voodoo2: bool,
    /// The card's dither reconstructed away at scanout
    /// (`-device voodoo2,undither=on`, doc 21 §12). A Voodoo stores
    /// RGB565 through an ordered dither, and this puts back the colour
    /// the rasterizer had by inverting that table. It is exact over a
    /// 4x4 window, and where no single colour could have produced a
    /// window the pixel is left as it was, so an edge is never blurred. Off
    /// unless picked, and meaningless without [`Machine::voodoo2`]: the
    /// form keeps the two together and `--print-args` writes the
    /// property only with the card.
    #[serde(default)]
    pub voodoo2_undither: bool,
    /// Primary IDE hard disk (qcow2).
    pub disk: PathBuf,
    /// The disc in the CD-ROM drive when the machine boots, if any. Just
    /// one: the collection of discs is the shared shelf
    /// (`disc_library.rs`), not a per-machine list, and any other disc is
    /// swapped in at runtime through the monitor (`control.rs`).
    #[serde(default)]
    pub disc: Option<PathBuf>,
    /// Superseded by `disc` + the shared shelf. Read so bundles written
    /// before the shelf became shared still boot the disc they named
    /// (see `boot_disc`), and so their other entries can be imported
    /// (`DiscLibrary::import_legacy`); `save` drops it.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub discs: Vec<PathBuf>,
    /// A named shader profile (`shader_library`) to run this machine
    /// with, by id; `None` uses the app default. Takes precedence over
    /// `shader` when both are set.
    #[serde(default)]
    pub shader_profile: Option<String>,
    /// Raw shader preset override, bypassing the profile manager (doc 07
    /// settings taxonomy: the hand-written-bundle escape hatch).
    /// `None` uses the app default.
    #[serde(default)]
    pub shader: Option<PathBuf>,

    /// A floppy image in the machine's A: drive, if it has one. Doc 06
    /// lists a floppy on the Win98 machine ("driver/utility sneakernet,
    /// boot disks") and doc 07 lists floppy images among the media the
    /// launcher handles; a DOS machine may boot from one. Absent means
    /// no disk in the drive. The controller is there either way, as on a
    /// real PC of the era.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub floppy: Option<PathBuf>,

    /// Which drive to boot from. Absent = `Boot::Auto`, which is what
    /// every bundle written before this field existed was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub boot: Option<Boot>,

    /// How fast the CPU is allowed to run (`CpuSpeed`). Absent =
    /// unthrottled, which is what every earlier bundle was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cpu_speed: Option<CpuSpeed>,

    /// The display adapter (`video_choices`). Absent = that family's
    /// default, and a value the family does not offer falls back to it
    /// (`effective_video`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub video: Option<Video>,

    /// Which Direct3D 9 the host runs the executor on (`D3d9`). Absent =
    /// `Auto`, so a bundle written before this field existed keeps the
    /// command line it had. Read only where the machine has our own
    /// adapter, which is the device that carries the executor.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub d3d9: Option<D3d9>,

    /// What a host gamepad does for this machine (`Pad`). Absent = that
    /// family's default, which is `Pad::None` everywhere, so a bundle
    /// written before this field existed still ignores a gamepad.
    #[serde(
        default,
        deserialize_with = "pad_lenient",
        skip_serializing_if = "Option::is_none"
    )]
    pub pad: Option<Pad>,

    /// The sound card (`Sound`). Absent = the family's default, which
    /// is the card that family already had, so a bundle written before
    /// this field existed describes the same machine it always did.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sound: Option<Sound>,

    /// What is on the MIDI port (`Music`). Absent = the family's
    /// default: a General MIDI synthesizer on the two families with no
    /// synthesizer of their own, no port at all on the other two.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub music: Option<Music>,

    /// A SoundFont bank of the user's own, for `Music::Gm`. Absent =
    /// the one the package ships, which the player names to QEMU
    /// (`LIBSYNTH_SF2`, `player-core/src/companions.rs`) rather than the
    /// bundle, so where an installed tree keeps its resources is never
    /// frozen into a machine's file.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub soundfont: Option<PathBuf>,

    /// The directory holding the user's own Roland ROMs, for
    /// `Music::Mt32`. There is no default and there never will be:
    /// nothing of Roland's is redistributable (doc 20 §4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mt32_roms: Option<PathBuf>,

    /// Arguments added to the end of QEMU's command line, one list
    /// entry per argument, exactly as typed into the form's "Extra QEMU
    /// arguments" field. The escape hatch for what the form has no field
    /// for: a device property (`-global d3dpt-vga.ddflags=32768`), a
    /// trace, a debug knob. Nothing checks them; a machine QEMU refuses
    /// says so at start. Absent from every bundle that has none.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub extra_qemu_args: Vec<String>,
    /// QEMU's versioned board, `pc-i440fx-<QEMU version>`. A machine keeps
    /// the board it was created on: a newer QEMU's unversioned `pc`
    /// changes CPUID bits and device state, and a live snapshot saved on
    /// the old board may not load on the new one (track M21). Absent
    /// means the bundle predates the field, when every machine ran 9.2's
    /// `pc`, so it gets `LEGACY_BOARD` (`effective_board`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board: Option<String>,
    /// Windows 11 (M20): the guest's processor (`Arch`). Absent = x86_64,
    /// what every Windows 11 machine was before Windows 11 on Arm; a new
    /// machine is written with the host's own.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub arch: Option<Arch>,
    /// Windows 11 (M20): the firmware's variable store, a qcow2 the size
    /// of EDK2's (boot entries, Secure Boot keys). A qcow2 rather than
    /// the raw file EDK2 ships so live snapshots can hold it, as they
    /// hold the disk. Made from QEMU's template when the machine first
    /// starts (`player::prepare`). Absent = `efivars.qcow2` beside the
    /// disk (`effective_efi_vars`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub efi_vars: Option<PathBuf>,
    /// Windows 11 (M20): the TPM's permanent state, the one file the
    /// libtpms backend keeps (patch 75). Missing means a TPM that was
    /// never made, which the first start makes. Losing it is a new TPM
    /// to Windows: BitLocker asks for its recovery key. Absent =
    /// `tpm.permall` beside the disk.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tpm_state: Option<PathBuf>,
    /// Windows 11 (M23): a host folder the guest sees as
    /// `\\10.0.2.4\host`, served by the player itself (libsmb) and
    /// reached through the machine's network, so only with `network`
    /// (`share_folder`). Absent = none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub shared_folder: Option<PathBuf>,
    /// Windows 11 (M23): the clipboard shared with the host, through
    /// QEMU's `qemu-vdagent` and our agent in the guest (doc 24 §3-4).
    #[serde(default)]
    pub clipboard: bool,
    /// Which of our own emulator fast paths this machine runs with
    /// (`Optimization`), holding only what differs from each one's
    /// default. Absent means all of them at their shipped setting.
    ///
    /// **Last in the struct on purpose.** It is the only field that
    /// serializes to a TOML table, and a table swallows every key-value
    /// line that follows it: written anywhere else, the fields after it
    /// would be read back as part of `[optimizations]`.
    #[serde(default, skip_serializing_if = "Optimizations::is_empty")]
    pub optimizations: Optimizations,
}

/// The board a new machine is created on: the versioned name of the
/// QEMU this launcher ships (`Machine::board`). Moves with the `qemu`
/// submodule.
pub const CURRENT_BOARD: &str = "pc-i440fx-11.1";

/// The board of a bundle written before `Machine::board` existed: 9.2's
/// `pc`, which every machine ran then.
pub const LEGACY_BOARD: &str = "pc-i440fx-9.2";

/// How a family runs unless the machine says otherwise.
///
/// **Win98 is emulated by default.** KVM runs the guest at host speed,
/// and doc 06's `pentium3` model does not protect against Win9x's
/// fast-CPU bugs, since the speed trips them, not the CPUID. TCG is also
/// the path this project's x87/SSE fast paths (docs 13, 16) exist for,
/// so it is the configuration Win98 is tuned and tested on here. XP has
/// none of those problems and wants the speed.
pub fn default_accel(family: Family) -> Accel {
    match family {
        // A DOS machine is throttled by default and a throttle needs TCG
        // (`-icount` and KVM cannot coexist), so this is the only
        // truthful default; `Auto` would promise KVM and not deliver it.
        Family::Win98 | Family::Dos => Accel::Tcg,
        // Nothing here is tuned for an era Linux or BeOS, and neither
        // has Win9x's fast-CPU bugs: take the host's speed when it is
        // there, emulate when it isn't.
        Family::Xp | Family::Win7 | Family::Other => Accel::Auto,
        // Hardware virtualization where the host has it. A host without
        // it still starts the machine, at a speed step 1 measured as
        // unusable for work, rather than refusing it.
        Family::Win11 => Accel::Auto,
    }
}

/// Whether a new machine of this family gets a card. **None of them
/// do** (user decision): these guests stopped getting security fixes
/// twenty years ago, so none should be on a network before anyone asked.
/// The checkbox is in the wizard for the machine that wants one, and
/// turning it on later is a card appearing, which Windows handles far
/// better than one disappearing. DOS never got one anyway: it reaches a
/// network only through a packet driver the user installs by hand, so
/// the card would be an unused device the guest still enumerates.
///
/// The family is still the argument, because that is what a default here
/// may depend on and one of them may want a card again.
///
/// A bundle with no `network` field has no card either
/// (`Machine::network`).
pub fn default_network(_family: Family) -> bool {
    false
}

/// The tablet is on unless a bundle says otherwise: it is what every
/// machine had before the field existed, and desktop mousing without a
/// grab is the right first impression of a machine.
fn seamless_mouse_default() -> bool {
    true
}

/// Whether a *new* machine of this family gets the tablet. Two families
/// don't. DOS's mouse drivers talk to the PS/2 controller, so a tablet
/// would leave the guest with a pointer it cannot see. `Other` is off
/// for the weaker version of the same reason: an absolute USB pointer
/// needs the guest's USB HID stack and its windowing system to agree it
/// is absolute, which an era Linux (XFree86 wants an explicit
/// `evdev`/`usbtablet` input section) and BeOS do not do out of the box,
/// and unlike the Windows families there is no guest-tools install that
/// would fix it. The PS/2 mouse works everywhere, so a machine we cannot
/// test starts with it; the checkbox turns the tablet on for a guest
/// that handles it. (An existing bundle with no
/// `seamless_mouse` field is unaffected: taking a pointing device away
/// from a machine that has been running with one is a hardware change,
/// not a default.)
pub fn default_seamless_mouse(family: Family) -> bool {
    !matches!(family, Family::Dos | Family::Other) && seamless_mouse_default()
}

/// The speed a family runs at unless the machine says otherwise. Only
/// DOS is throttled: a 486DX2-66 is the machine most of the CD-ROM era
/// was written for, and it is inside the range where the cap is exact
/// (see `CpuSpeed`). Windows machines are unthrottled: 9x and XP read
/// the clock instead of counting instructions, and a throttle would only
/// make them slow.
pub fn default_cpu_speed(family: Family) -> CpuSpeed {
    match family {
        Family::Dos => CpuSpeed::Dx266,
        Family::Win98 | Family::Xp | Family::Win7 | Family::Other | Family::Win11 => CpuSpeed::Unthrottled,
    }
}

/// The adapter a family starts on, or `None` when it has no choice to
/// make. Always the first of `video_choices`, so the list and the
/// default cannot disagree: our own adapter on both Windows families,
/// where the whole display path is built on it, and the standard VGA on
/// DOS and `Other`, a VESA path every guest can fall back on when it has
/// no native driver.
pub fn default_video(family: Family) -> Option<Video> {
    video_choices(family).first().copied()
}

/// The size a new machine's disk is offered at, in GB (a qcow2, so
/// only what the guest writes is taken on the host). Enough for the OS
/// and the era's games installed in full: a Windows 98 install is a few
/// hundred MB and a big CD game another few hundred, XP itself wants
/// ~1.5 GB, and DOS's largest games fit on a single CD.
pub fn default_disk_size_gb(family: Family) -> u32 {
    match family {
        Family::Win98 | Family::Other => 10,
        Family::Xp => 20,
        // Setup wants 16 GB; room for the updates and the games.
        Family::Win7 => 40,
        Family::Dos => 2,
        // Setup refuses a disk under 64 GB.
        Family::Win11 => 64,
    }
}

/// doc 06's RAM default for a family.
pub fn default_ram_mb(family: Family) -> u32 {
    match family {
        Family::Win98 => 256, // doc 06: 256 MB default, ≤512 MB hard cap
        Family::Xp => 512,    // doc 06: 512 MB-1 GB default
        // What the Windows 7 work ran with (track M18); Aero is
        // comfortable in it, and setup wants 1 GB.
        Family::Win7 => 2048,
        // DOS itself uses the first megabyte; the rest is XMS for the
        // extenders a mid-90s game ships with, and more of it buys
        // nothing. 64 MB is generous for the era and stays inside what
        // MS-DOS 6.22's own HIMEM.SYS manages.
        Family::Dos => 64,
        // No family default to inherit. An era Linux desktop or BeOS R5
        // is comfortable in 512 MB and neither needs more.
        Family::Other => 512,
        // Setup's minimum.
        Family::Win11 => 4096,
    }
}

/// A value inside a QEMU option string. Options are separated by commas
/// there, so a comma in a value is written twice. Otherwise a disk in
/// `~/Games/Doom, Quake and friends/` becomes an unknown option and QEMU
/// refuses the whole line. Paths come from a file picker, and a comma in
/// a directory name is ordinary.
fn opt_value(s: &str) -> String {
    s.replace(',', ",,")
}

/// What the UI lets a user ask for. The Win98 ceiling is doc 06's hard
/// cap, not a guess: Win9x fails to boot with much more than 512 MB (its
/// VCACHE sizing overflows), so offering 2 GB there would only produce a
/// machine that does not start. XP's is the practical 32-bit limit,
/// below the 3.5 GB where PCI space starts eating into RAM.
pub fn ram_mb_range(family: Family) -> std::ops::RangeInclusive<u32> {
    match family {
        Family::Win98 => 32..=512,
        Family::Xp => 64..=3072,
        // Setup's 1 GB at the bottom, XP's 32-bit ceiling at the top.
        Family::Win7 => 1024..=3072,
        Family::Dos => 4..=256,
        // We don't know what guest is going in, so the only limits are
        // the machine's. The bottom is where a 1995 kernel still boots,
        // the top is XP's 32-bit ceiling. BeOS R5 has a lower ceiling of
        // its own (1 GB), which the wizard states rather than enforces.
        Family::Other => 16..=3072,
        // Setup refuses less than 4 GB; the top is a guess at what a
        // host that runs it has to spare.
        Family::Win11 => 4096..=32768,
    }
}

/// How many processors the machine has. One on the era's families,
/// which is all their software expected; Windows 11 refuses fewer than
/// two, and gets four of the host's (all of them on a host with fewer).
pub fn default_cpus(family: Family) -> u32 {
    if family.is_modern() {
        let host = std::thread::available_parallelism().map(|n| n.get() as u32).unwrap_or(2);
        host.clamp(2, 4)
    } else {
        1
    }
}

impl Machine {
    /// A new machine from doc 06's reference defaults for `family`.
    pub fn reference(family: Family, name: String, disk: PathBuf) -> Self {
        Machine {
            name,
            family,
            ram_mb: default_ram_mb(family),
            accel: Some(default_accel(family)),
            network: default_network(family),
            seamless_mouse: default_seamless_mouse(family),
            voodoo2: false,
            voodoo2_undither: false,
            disk,
            disc: None,
            discs: Vec::new(),
            shader_profile: None,
            shader: None,
            floppy: None,
            boot: None,
            cpu_speed: Some(default_cpu_speed(family)),
            video: default_video(family),
            d3d9: Some(D3d9::Auto),
            sound: Some(default_sound(family)),
            music: Some(default_music(family)),
            soundfont: None,
            mt32_roms: None,
            shared_folder: None,
            clipboard: family.is_modern(),
            pad: Some(default_pad(family)),
            extra_qemu_args: Vec::new(),
            board: Some(CURRENT_BOARD.to_string()),
            arch: family.is_modern().then(Arch::native),
            efi_vars: None,
            tpm_state: None,
            optimizations: Optimizations::default(),
        }
    }

    /// The disc in the drive at boot: `disc`, or the first entry of a
    /// pre-shared-shelf bundle's `discs` (which is exactly what the old
    /// `qemu_args` attached).
    pub fn boot_disc(&self) -> Option<&PathBuf> {
        self.disc.as_ref().or_else(|| self.discs.first())
    }

    pub fn load(path: &Path) -> std::io::Result<Machine> {
        let text = std::fs::read_to_string(path)?;
        toml::from_str(&text).map_err(std::io::Error::other)
    }

    /// Writes the bundle in the current format, which also migrates a
    /// legacy one: the boot disc moves to `disc` and the old per-machine
    /// `discs` list is dropped. Its entries aren't lost: the library
    /// scan imports them onto the shared shelf
    /// (`DiscLibrary::import_legacy`) before anything here can rewrite a
    /// bundle.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let mut out = self.clone();
        out.disc = self.boot_disc().cloned();
        out.discs.clear();
        let text = toml::to_string_pretty(&out).map_err(std::io::Error::other)?;
        std::fs::write(path, text)
    }

    /// The accelerator, as `-accel` options. `Auto` is expressed as
    /// QEMU's own fallback list rather than by probing `/dev/kvm` here:
    /// a probe's answer can still be wrong at spawn time (permissions, a
    /// module unloaded since), and QEMU's list already means "KVM if you
    /// can, emulation otherwise". Two `-accel` options are tried in order
    /// and the first that initializes wins, the same code path
    /// `accel=kvm:tcg` took. `kvm` is only offered where it exists: on
    /// macOS the name is not a registered accelerator, and listing it
    /// would print a warning on every boot.
    ///
    /// **`-accel`, not `-machine accel=`, and the two cannot be mixed**
    /// ("The -accel and \"-machine accel=\" options are incompatible",
    /// `system/vl.c`). The TCG-side optimizations are properties of the
    /// accelerator object, and `-accel` is the only spelling that has
    /// somewhere to put them.
    fn accel_args(&self) -> Vec<String> {
        let mut tcg = "tcg".to_string();
        tcg.push_str(&self.optimization_props(Knob::Tcg));
        // A Mac's hypervisor is asked for only on the host's own target,
        // Windows 11 on Arm's, since QEMU builds `hvf` only into that one
        // (M20 step 4): asking the era's i386 QEMU for it would print
        // "invalid accelerator" on every boot. Windows' WHPX runs the
        // era's i386 too (patch 84).
        let hyp = (cfg!(target_os = "macos") || cfg!(target_os = "windows"))
            && Arch::hypervisor_runs(self.qemu_target());
        let hyp_name = if cfg!(target_os = "windows") { "whpx" } else { "hvf" };
        match self.effective_accel() {
            // "Hardware acceleration, required", spelled the way this
            // host spells it (`whpx` on Windows, `hvf` on a Mac), so a
            // machine directory copied between hosts keeps its meaning.
            // An era machine set to it there does not start, as the
            // wizard's note says.
            Accel::Kvm if cfg!(target_os = "windows") || cfg!(target_os = "macos") => {
                vec!["-accel".into(), hyp_name.into()]
            }
            Accel::Kvm => vec!["-accel".into(), "kvm".into()],
            Accel::Auto if hyp => vec!["-accel".into(), hyp_name.into(), "-accel".into(), tcg],
            Accel::Auto if cfg!(target_os = "linux") => {
                vec!["-accel".into(), "kvm".into(), "-accel".into(), tcg]
            }
            Accel::Auto | Accel::Tcg => vec!["-accel".into(), tcg],
        }
    }

    /// The `,name=on`/`,name=off` tail for one kind of switch, **only
    /// what differs from the property's own default in our QEMU**. A
    /// machine that has changed nothing produces the command line it
    /// always produced, and one run against a QEMU without these patches
    /// still starts.
    fn optimization_props(&self, knob: Knob) -> String {
        let mut out = String::new();
        for opt in Optimization::ALL {
            if opt.knob() == knob && !self.optimizations.is_default(opt) {
                let value = if self.optimizations.enabled(opt) { "on" } else { "off" };
                out.push_str(&format!(",{}={value}", opt.key()));
            }
        }
        out
    }

    /// What this machine runs as: its own setting, or its family's
    /// (`default_accel`) when the bundle doesn't say. **Except** that a
    /// throttled CPU forces emulation, because QEMU refuses `-icount`
    /// together with KVM ("cannot enable icount when KVM is enabled"),
    /// and starting matters more than the accelerator. The wizard says
    /// so next to the field.
    pub fn effective_accel(&self) -> Accel {
        if self.effective_cpu_speed().icount_shift().is_some() {
            return Accel::Tcg;
        }
        self.accel.unwrap_or_else(|| default_accel(self.family))
    }

    /// The CPU speed this machine runs at: its own setting, or its
    /// family's default (only DOS has a throttled one).
    pub fn effective_cpu_speed(&self) -> CpuSpeed {
        // A modern machine has no period processor to be (the form does
        // not offer one), so a stray field cannot force it to TCG.
        if self.family.is_modern() {
            return CpuSpeed::Unthrottled;
        }
        self.cpu_speed.unwrap_or_else(|| default_cpu_speed(self.family))
    }

    /// The adapter this machine runs on, or `None` where the family
    /// settles it. A `video` naming an adapter this family does not
    /// offer falls back to its default rather than being obeyed:
    /// `video = "std"` on an XP machine would leave the guest with no
    /// driver at all (there is none for the Bochs adapter on XP), and a
    /// stray bundle field should not be able to do that.
    pub fn effective_video(&self) -> Option<Video> {
        let choices = video_choices(self.family);
        let default = *choices.first()?;
        Some(match self.video {
            Some(v) if choices.contains(&v) => v,
            _ => default,
        })
    }

    /// What this machine does with a host gamepad: its own setting, or
    /// its family's default. A `pad` naming a setting this family does
    /// not offer falls back the way `effective_video` does. That is also
    /// where a bundle from a later launcher lands: `pad_lenient` has
    /// already turned its unknown `usb` or `gameport` into `None`, and
    /// this turns `None` into the family's default.
    pub fn effective_pad(&self) -> Pad {
        let choices = pad_choices(self.family);
        match self.pad {
            Some(p) if choices.contains(&p) => p,
            _ => default_pad(self.family),
        }
    }

    /// The `-vga` / `-device` pair that puts the machine's adapter on it.
    /// `-vga none` first for every choice, so the machine never gets the
    /// default adapter as well as the one it asked for.
    ///
    /// `retrace=precise` rides on the `-vga` option whichever adapter is
    /// picked (it is a QEMU-wide setting every VGA-derived device reads
    /// at init, `d3dpt-vga` included). QEMU's default, `dumb`, answers
    /// each read of the input status register (port 3DAh) by flipping
    /// the vertical-retrace bit, so a program's wait-for-retrace loop
    /// ends on its second read and a game paced by the retrace (Mortal
    /// Kombat 3, and most of the DOS era) runs unbounded whatever the
    /// processor combo says. `precise` derives the bit from the CRTC
    /// timing and the virtual clock, which is a 70 Hz retrace in mode
    /// 13h and the game's own pace.
    fn video_args(&self) -> Vec<String> {
        const RETRACE: &str = ",retrace=precise";
        let mut args = vec!["-vga".to_string(), format!("none{RETRACE}")];
        if let Some(video) = self.effective_video() {
            let [flag, value] = video.args();
            // `-vga <name>` replaces the `none` above rather than adding
            // to it; our own adapter is a `-device` and keeps it.
            if flag == "-vga" {
                args = vec![flag.to_string(), format!("{value}{RETRACE}")];
            } else {
                let mut dev = value.to_string();
                // Which Direct3D 9 the executor runs on, on the one
                // adapter that carries it. Said only when it is not
                // `auto`, so a machine nobody has touched writes the
                // command line it always did.
                if let Some(which) = self.d3d9_arg() {
                    dev.push_str(",d3d9=");
                    dev.push_str(which);
                }
                // Windows 7's WDDM driver needs the adapter's interrupt:
                // dxgkrnl starts no adapter without one (track M18). XP
                // and 98 keep the PCI config they were installed on.
                if self.family == Family::Win7 {
                    dev.push_str(",irq=on");
                }
                args.extend([flag.to_string(), dev]);
            }
        }
        args
    }

    /// The value of the adapter's `d3d9=` property, or `None` to say
    /// nothing, which is what a machine on [`D3d9::Auto`] writes on a
    /// host where auto means what the executor's own auto means.
    ///
    /// `Auto` is resolved **here**, by the host's Vulkan probe, and only
    /// on Windows. The executor can tell a DXVK that opens no adapter
    /// from one that opens a real one, but not a software Vulkan device
    /// from a hardware one, and on a Windows host a real card's own
    /// Direct3D 9 beats a software Vulkan rasteriser every time
    /// (ADR-007's second amendment, ADR-013's floor).
    fn d3d9_arg(&self) -> Option<&'static str> {
        match self.d3d9.unwrap_or(D3d9::Auto) {
            D3d9::Dxvk => Some("dxvk"),
            D3d9::System => Some("system"),
            D3d9::Auto if cfg!(windows) => {
                let gpu = crate::host_gpu::cached().gpu;
                (!gpu.d3d_available() || gpu.is_slow()).then_some("system")
            }
            D3d9::Auto => None,
        }
    }

    /// The card this machine has. One the family does not offer falls
    /// back to its default rather than being obeyed, for the reason
    /// `effective_video` does: a stray field should not be able to
    /// produce a machine whose guest has no driver.
    pub fn effective_sound(&self) -> Sound {
        let choices = sound_choices(self.family);
        match self.sound {
            Some(s) if choices.contains(&s) => s,
            _ => default_sound(self.family),
        }
    }

    /// What is on this machine's MIDI port.
    pub fn effective_music(&self) -> Music {
        let choices = music_choices(self.family);
        match self.music {
            Some(m) if choices.contains(&m) => m,
            _ => default_music(self.family),
        }
    }

    /// The sound card and the MIDI port, as devices (doc 20 §6).
    ///
    /// The bank for a General MIDI port is named here **only when the
    /// user chose one**. The one we ship is a companion of the player's,
    /// found by the player's own rule like the Direct3D executor, so a
    /// package that moves does not invalidate every machine file in the
    /// library.
    fn audio_args(&self) -> Vec<String> {
        let mut args = self.effective_sound().args();
        let music = self.effective_music();
        if music != Music::None {
            let mut spec = format!("mpu401,audiodev=embed0,synth={}", music.key());
            match music {
                Music::Gm => {
                    if let Some(sf2) = &self.soundfont {
                        spec.push_str(&format!(",soundfont={}", opt_value(&sf2.display().to_string())));
                    }
                }
                Music::Mt32 => {
                    if let Some(dir) = &self.mt32_roms {
                        spec.push_str(&format!(",romdir={}", opt_value(&dir.display().to_string())));
                    }
                }
                Music::None => {}
            }
            args.extend(["-device".to_string(), spec]);
        }
        args
    }

    pub fn effective_boot(&self) -> Boot {
        self.boot.unwrap_or_default()
    }

    pub fn effective_board(&self) -> &str {
        self.board.as_deref().unwrap_or(LEGACY_BOARD)
    }

    /// The `qemu-system-i386` arguments the player expects on its own
    /// command line (`player -- <these>`), per doc 06's reference tables.
    /// `pc_bios_dir` is `qemu/pc-bios` (see README's `-L`); `shelf`, when
    /// given, is the flat disc-shelf file the drive answers the in-guest
    /// `CDSHELF` program from (`cdshelf/cdshelf_proto.h`).
    pub fn qemu_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        if self.family.is_modern() {
            return self.modern_args(pc_bios_dir, shelf);
        }
        // Windows 98 has no driver for an HPET (`PNP0103` is in none of
        // 98 SE's INFs) and never uses one (it times off the PIT), so on
        // 98 it is an "Unknown Device" with a yellow mark in Device
        // Manager and nothing else. QEMU's fw_cfg (`QEMU0002`) is the one
        // other device 98 has no driver for, but its `_STA` says "not
        // shown in UI", and 98 obeys that.
        let hpet = if matches!(self.family, Family::Win98) { ",hpet=off" } else { "" };
        let machine = format!("{}{hpet}", self.effective_board());
        let mut args = vec!["-L".into(), pc_bios_dir.display().to_string(), "-machine".into(), machine];
        args.extend(self.accel_args());
        args.extend([
            "-m".into(),
            self.ram_mb.to_string(),
            // doc 06's floor for the era families: avoids the fast-CPU
            // Win9x bugs and CPUID-dispatched guest code that mis-decodes
            // under -cpu host (the Max Payne JPEG decoder gotcha).
            // Windows 7 gets `max`, what its work was proved on (track
            // M18): its software wants SSE2, which a Pentium III lacks.
            "-cpu".into(),
            format!(
                "{}{}",
                if self.family == Family::Win7 { "max" } else { "pentium3" },
                self.optimization_props(Knob::Cpu)
            ),
            "-drive".into(),
            format!("file={},if=ide,index=0,media=disk", opt_value(&self.disk.display().to_string())),
        ]);
        // The pointer (doc 03). The USB tablet is an absolute device: it
        // reports where the pointer is rather than how far it moved, so
        // the host pointer is the guest cursor and nothing has to be
        // grabbed. Without it the machine keeps only the PS/2 mouse
        // (relative, grabbed on a click).
        // The gamepad (M13 path A) needs the same USB controller. `-usb`
        // goes on once for both. A machine whose pointer is turned off
        // must still get a controller for its pad rather than a
        // `-device usb-gamepad` with no bus to attach to.
        let pad_usb = self.effective_pad() == Pad::Usb;
        if self.seamless_mouse || pad_usb {
            args.push("-usb".into());
        }
        if self.seamless_mouse {
            args.extend(["-device".into(), "usb-tablet".into()]);
        }
        if pad_usb {
            args.extend(["-device".into(), "usb-gamepad".into()]);
        }
        // Path B's gameport (patch 27) needs no controller: it is an ISA
        // device at 0x200-0x207, and every machine this project makes
        // has an ISA bus. It is the only pad device DOS can use, so that
        // family is offered it and not the USB one.
        if self.effective_pad() == Pad::Gameport {
            args.extend(["-device".into(), "gameport".into()]);
        }
        // The Voodoo 2 (doc 21): a PCI card of its own in the slot after
        // the sound card's, on whichever 2D adapter the machine has.
        if self.voodoo2 {
            let mut dev = String::from("voodoo2,addr=0x05");

            if self.voodoo2_undither {
                dev.push_str(",undither=on");
            }
            args.extend(["-device".into(), dev]);
        }
        // The CPU rate, when the machine asks for one. `align=on` is what
        // throttles: `-icount shift=N` on its own only makes the guest's
        // clock a function of instructions retired, so the guest believes
        // it is slow while the host runs it as fast as it likes. Measured:
        // a run meant to be throttled finished in less wall-clock time
        // than the unthrottled one, because the guest's idle waits
        // collapse too.
        if let Some(shift) = self.effective_cpu_speed().icount_shift() {
            args.extend(["-icount".into(), format!("shift={shift},align=on")]);
        }
        // The floppy: `format=raw` because a floppy image has no header
        // to probe and QEMU otherwise both warns and refuses writes to
        // the boot sector.
        if let Some(floppy) = &self.floppy {
            args.extend(["-drive".into(), format!("file={},if=floppy,index=0,format=raw", opt_value(&floppy.display().to_string()))]);
        }
        if let Some(order) = self.effective_boot().order() {
            args.extend(["-boot".into(), format!("order={order}")]);
        }
        // Doc 06's per-family NIC on QEMU's user-mode NAT, or no adapter
        // at all rather than an unplugged cable. A card that is present
        // still makes Windows enumerate it, ask for its driver on a fresh
        // install and wait on it at boot.
        //
        // `-nic none` is what turns it off. QEMU creates a NIC of its own
        // when the command line asks for no networking at all: leaving
        // out the `-netdev` only replaces ours with an e1000 in the slot
        // below (`query-pci` shows it).
        //
        // The Windows devices carry explicit PCI addresses because
        // removing the NIC would otherwise slide the card below it up
        // into its slot, and a card that moves is a hardware change an
        // installed Windows re-detects. These are the addresses those
        // devices already get from their `-device` order, so pinning them
        // changes nothing for an existing machine; it only keeps them
        // still when the NIC comes and goes. DOS needs none of it: its
        // SB16 is ISA, so its NIC is the only PCI card in the sequence.
        if !self.network {
            args.extend(["-nic".into(), "none".into()]);
        }
        match self.family {
            // The adapter is a choice (`video_choices`): `d3dpt-vga` with
            // our own display driver (doc 19, M10), which carries the
            // linear frame buffer the player scans out, the mode table,
            // the page flips that pace a game and Direct3D, and is where
            // this family starts. Or the Cirrus and the in-box driver
            // Windows already has, which has none of that.
            //
            // **Changing it is a hardware change to an installed guest.**
            // The guest finds an unknown adapter, comes up in plain VGA,
            // and wants a driver (ours from the guest-tools ISO, `SETUP`,
            // doc 19 §16, or Windows' own for the Cirrus) before it has
            // its desktop back. The machine boots either way.
            Family::Win98 => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box 98 driver
                    args.extend(["-device".into(), "pcnet,netdev=n0,addr=0x03".into()]);
                }
                args.extend(self.audio_args());
            }
            // The 1994 PC: the same chipset and the SB16 doc 06 already
            // puts on the Win98 machine "for DOS boxes/games", one of the
            // two standard adapters for its VGA and VESA modes, and
            // nothing else. 3D reaches DOS through the Voodoo 2 card
            // above, with the game's own GLIDE2X.OVL.
            //
            // The adapter is a choice here too (`video_choices`), and the
            // only family where it is not a driver question: a DOS title
            // programs the registers itself, so a different adapter
            // changes which VESA BIOS it finds. It starts on the standard
            // VGA, the fuller of the two.
            Family::Dos => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    args.extend(["-device".into(), "pcnet,netdev=n0".into()]);
                }
                args.extend(self.audio_args());
            }
            Family::Xp => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box XP driver
                    args.extend(["-device".into(), "rtl8139,netdev=n0,addr=0x03".into()]);
                }
                args.extend(self.audio_args());
            }
            // XP's machine with Windows 7's in-box cards: the Intel
            // PRO/1000 (e1000) for the network, HD Audio for sound
            // (`sound_choices`). Our adapter carries its interrupt
            // (`video_args`).
            Family::Win7 => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box Windows 7 driver
                    args.extend(["-device".into(), "e1000,netdev=n0,addr=0x03".into()]);
                }
                args.extend(self.audio_args());
            }
            // Everything else of the era: standard hardware only, chosen
            // for having had a driver in the box on BeOS R5 and on a
            // period Linux alike, because there is no guest-tools install
            // to add one afterwards.
            //
            // Its two adapters are the two standard ones, never
            // `d3dpt-vga`, which needs our display driver and so exists
            // for Windows only. So this family has no 3D path through the
            // adapter whichever is picked.
            //
            // The PCI addresses are pinned for the same reason the
            // Windows families pin theirs: removing the NIC would slide
            // the sound card up into its slot, and a card that moves is a
            // hardware change a guest re-detects. Every adapter here
            // takes 0x02 (measured), so these two follow it.
            Family::Other => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box on BeOS R5 and on Linux since 2.2 (8139too)
                    args.extend(["-device".into(), "rtl8139,netdev=n0,addr=0x03".into()]);
                }
                // The Ensoniq AudioPCI, not the AC'97 the Windows
                // families get: it is the PCI card of the period both
                // these guests drive in the box (BeOS ships an ensoniq
                // add-on, Linux `snd-ens1370`), where AC'97 needs a
                // driver an era install may not have.
                args.extend(self.audio_args());
            }
            Family::Win11 => unreachable!("a modern family's machine is modern_args"),
        }
        args.extend(self.cdrom_args(shelf));
        // Last, so an option given twice is the user's: QEMU takes the
        // later of most repeated options.
        args.extend(self.extra_qemu_args.iter().cloned());
        args
    }

    /// The CD-ROM drive, on every machine. It is always attached, empty
    /// tray and all: a real machine of the era has one, and the
    /// launcher's live disc swap (`control.rs`) needs a device to put a
    /// disc into. A drive that only existed when the bundle named a disc
    /// couldn't be loaded later. The id is what a medium change
    /// addresses (`control::CDROM_ID`). `bus=ide.1` is the i440FX's
    /// secondary IDE channel and the q35's second AHCI port alike.
    fn cdrom_args(&self, shelf: Option<&Path>) -> Vec<String> {
        let mut drive = "if=none,id=cd0,media=cdrom".to_string();
        if let Some(disc) = self.boot_disc() {
            // `qemu_medium`, not the path: a shared folder is a disc too,
            // and it is named to QEMU with the `isodir:` prefix (M5g).
            drive.push_str(&format!(",file={}", opt_value(&crate::disc_library::qemu_medium(disc))));
        }
        let mut cd = format!("ide-cd,bus=ide.1,id={},drive=cd0,audiodev=embed0", crate::control::CDROM_ID);
        if self.family.is_modern() {
            // after the disk (`boot_prompt_args`): the firmware's own
            // order is whatever drives held media on its first start
            cd.push_str(",bootindex=1");
        }
        if let Some(shelf) = shelf {
            // The drive answers the in-guest CDSHELF program from this
            // file (patch 52). Without it the vendor command reports
            // "no shelf" and the drive is an ordinary CD-ROM, which is
            // also what a hand-written bundle run straight through
            // `player` gets.
            cd.push_str(&format!(",shelf={}", opt_value(&shelf.display().to_string())));
        }
        vec!["-drive".into(), drive, "-device".into(), cd]
    }

    /// Which QEMU runs this machine: `i386` for the era's families,
    /// the machine's `Arch` for Windows 11. Each player binary links one
    /// (`player::player_binary_for`).
    pub fn qemu_target(&self) -> &'static str {
        if self.family.is_modern() {
            self.effective_arch().qemu_target()
        } else {
            "i386"
        }
    }

    /// A modern machine's processor: its own, or x86_64 for a bundle
    /// that does not say (`arch`).
    pub fn effective_arch(&self) -> Arch {
        self.arch.unwrap_or_default()
    }

    /// The firmware's variable store (`efi_vars`), or `efivars.qcow2`
    /// beside the disk for a bundle that does not say.
    pub fn effective_efi_vars(&self) -> PathBuf {
        self.efi_vars.clone().unwrap_or_else(|| self.disk.with_file_name(EFI_VARS_FILE))
    }

    /// The TPM's state file (`tpm_state`), or `tpm.permall` beside the
    /// disk.
    pub fn effective_tpm_state(&self) -> PathBuf {
        self.tpm_state.clone().unwrap_or_else(|| self.disk.with_file_name(TPM_STATE_FILE))
    }

    /// Windows 11's machine (M20): a q35 with UEFI and a TPM 2.0.
    ///
    /// * The firmware is EDK2's secure build (Secure Boot available,
    ///   off until keys are enrolled; Windows 11 wants it possible, not
    ///   on), which needs SMM and its flash marked secure. Its code is
    ///   read-only from `pc-bios`, its variables the machine's own.
    /// * The TPM is libtpms inside QEMU (patch 75) on the CRB interface
    ///   Windows uses.
    /// * The disk is on the q35's AHCI controller, not NVMe: QEMU 9.2's
    ///   NVMe cannot be migrated, so a machine on it could take no live
    ///   snapshot. Windows drives both in the box.
    /// * `-cpu max`: the host's own CPU under KVM, everything TCG has
    ///   under emulation, and valid for both, which `Auto`'s fallback
    ///   list needs.
    /// * The RTC in local time, which is what Windows reads it as.
    /// * The PS/2 keyboard the q35 has; the tablet and a pad on xHCI.
    /// * The drivers disc (`disc_library::drivers_iso`: virtio-win's x64
    ///   serial driver, the clipboard's, and the 2ksbox agent) in a CD
    ///   drive of its own on `ide.2`, as on Arm.
    fn modern_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        if self.effective_arch() == Arch::Aarch64 {
            return self.arm_args(pc_bios_dir, shelf);
        }
        let bios = |name: &str| opt_value(&pc_bios_dir.join(name).display().to_string());
        let mut args = vec!["-L".into(), pc_bios_dir.display().to_string(), "-machine".into(), "q35,smm=on".into()];
        args.extend(self.accel_args());
        args.extend([
            "-m".into(),
            self.ram_mb.to_string(),
            "-smp".into(),
            default_cpus(self.family).to_string(),
            "-cpu".into(),
            format!("max{}", self.optimization_props(Knob::Cpu)),
            "-global".into(),
            "driver=cfi.pflash01,property=secure,value=on".into(),
            "-drive".into(),
            format!("if=pflash,format=raw,unit=0,readonly=on,file={}", bios(Arch::X86_64.efi_code_file())),
            "-drive".into(),
            format!(
                "if=pflash,format=qcow2,unit=1,file={}",
                opt_value(&self.effective_efi_vars().display().to_string())
            ),
            "-tpmdev".into(),
            format!("libtpms,id=tpm0,state={}", opt_value(&self.effective_tpm_state().display().to_string())),
            "-device".into(),
            "tpm-crb,tpmdev=tpm0".into(),
            "-rtc".into(),
            "base=localtime".into(),
            "-drive".into(),
            format!("file={},if=none,id=disk0", opt_value(&self.disk.display().to_string())),
            "-device".into(),
            "ide-hd,bus=ide.0,drive=disk0,bootindex=0".into(),
        ]);
        let pad_usb = self.effective_pad() == Pad::Usb;
        if self.seamless_mouse || pad_usb {
            args.extend(["-device".into(), "qemu-xhci".into()]);
        }
        if self.seamless_mouse {
            args.extend(["-device".into(), "usb-tablet".into()]);
        }
        if pad_usb {
            args.extend(["-device".into(), "usb-gamepad".into()]);
        }
        args.extend(self.video_args());
        if self.network {
            args.extend(["-netdev".into(), "user,id=n0".into()]);
            args.extend(["-device".into(), "e1000e,netdev=n0".into()]);
        } else {
            args.extend(["-nic".into(), "none".into()]);
        }
        args.extend(self.clipboard_args());
        args.extend(self.audio_args());
        args.extend(self.cdrom_args(shelf));
        args.extend(boot_prompt_args());
        args.extend(drivers_disc_args(Arch::X86_64));
        args.extend(self.extra_qemu_args.iter().cloned());
        args
    }

    /// Windows 11 on Arm's machine (M20 step 4): QEMU's `virt` board, the
    /// host's own CPU under its hypervisor (HVF on a Mac), UEFI and a
    /// TPM 2.0, and only devices Windows on Arm drives in the box.
    ///
    /// * `virt` with a GICv3, which Windows on Arm requires. The board
    ///   has no IDE, PS/2 or VGA of its own.
    /// * Our own EDK2 build (`scripts/build-edk2.sh`: Secure Boot
    ///   available, off; AHCI; ramfb up to 1920x1080) and the machine's
    ///   own variables, as on x86_64.
    /// * The TPM on `tpm-tis-device`, the `virt` board's TPM interface;
    ///   QEMU writes its ACPI table.
    /// * An ICH9 AHCI controller named `ide`, so the disk and the CD-ROM
    ///   sit on `ide.0` and `ide.1` as on the q35 (`cdrom_args`), and live
    ///   snapshots work (NVMe cannot be migrated, as on x86_64). Windows
    ///   on Arm drives it in the box; QEMU's own aarch64 EDK2 does not,
    ///   which is one reason the firmware is ours.
    /// * The screen is `ramfb`: EDK2 draws on it and Windows keeps it
    ///   through its Basic Display driver, at the size the firmware set
    ///   (1280x800; another from the firmware's setup screen).
    /// * Keyboard and pointer on xHCI, since there is no PS/2. The
    ///   tablet, or a relative mouse with the seamless pointer off.
    /// * With networking, `virtio-net`. Windows on Arm has no driver for
    ///   it (nor for an e1000e); the drivers disc does.
    /// * The drivers disc (`disc_library::drivers_iso`, virtio-win's
    ///   ARM64 NetKVM, viogpudo and vioser, and the 2ksbox agent) in a CD drive of its own on `ide.2`,
    ///   always: Windows Setup installs what it finds under the disc's
    ///   `$WinPEDriver$`, and an installed Windows can be pointed at it.
    ///   The user's own discs keep `ide.1` and the shelf.
    /// * `-cpu max`: the host's CPU under HVF, all of TCG's otherwise.
    fn arm_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        let arch = Arch::Aarch64;
        let bios = |name: &str| opt_value(&pc_bios_dir.join(name).display().to_string());
        let mut args =
            vec!["-L".into(), pc_bios_dir.display().to_string(), "-machine".into(), "virt,gic-version=3".into()];
        args.extend(self.accel_args());
        args.extend([
            "-m".into(),
            self.ram_mb.to_string(),
            "-smp".into(),
            default_cpus(self.family).to_string(),
            "-cpu".into(),
            "max".into(),
            "-drive".into(),
            format!("if=pflash,format=raw,unit=0,readonly=on,file={}", bios(arch.efi_code_file())),
            "-drive".into(),
            format!(
                "if=pflash,format=qcow2,unit=1,file={}",
                opt_value(&self.effective_efi_vars().display().to_string())
            ),
            "-tpmdev".into(),
            format!("libtpms,id=tpm0,state={}", opt_value(&self.effective_tpm_state().display().to_string())),
            "-device".into(),
            "tpm-tis-device,tpmdev=tpm0".into(),
            "-rtc".into(),
            "base=localtime".into(),
            "-device".into(),
            "ich9-ahci,id=ide".into(),
            "-drive".into(),
            format!("file={},if=none,id=disk0", opt_value(&self.disk.display().to_string())),
            "-device".into(),
            "ide-hd,bus=ide.0,drive=disk0,bootindex=0".into(),
            // Two screens, and the player shows the live one (embed's
            // `embed_live_console`): the firmware, Windows setup and its
            // recovery draw on ramfb, an installed Windows on virtio-gpu
            // through viogpudo from the drivers disc. ramfb first, so it
            // is the default console. `sync-ctrl` (QEMU patch 86) runs
            // virtio-gpu's queue inside the guest's notify, so each of
            // viogpudo's presents is copied before it draws the next.
            "-device".into(),
            "ramfb".into(),
            "-device".into(),
            "virtio-gpu-pci,sync-ctrl=on".into(),
            "-device".into(),
            "qemu-xhci".into(),
            "-device".into(),
            "usb-kbd".into(),
            "-device".into(),
            if self.seamless_mouse { "usb-tablet" } else { "usb-mouse" }.into(),
        ]);
        if self.effective_pad() == Pad::Usb {
            args.extend(["-device".into(), "usb-gamepad".into()]);
        }
        if self.network {
            args.extend(["-netdev".into(), "user,id=n0".into()]);
            args.extend(["-device".into(), "virtio-net-pci,netdev=n0".into()]);
        } else {
            args.extend(["-nic".into(), "none".into()]);
        }
        args.extend(self.clipboard_args());
        args.extend(self.audio_args());
        args.extend(self.cdrom_args(shelf));
        args.extend(boot_prompt_args());
        args.extend(drivers_disc_args(arch));
        args.extend(self.extra_qemu_args.iter().cloned());
        args
    }
}

/// A Windows 11 machine's firmware console on a serial port the player
/// supplies (`player_core::boot_prompt`): when the firmware starts the
/// install disc, the player answers its "Press any key to boot from CD or
/// DVD", which nobody sees on the new machine's black screen. With the
/// disk first in the boot order (`bootindex=0`, the disc 1) the firmware
/// only starts the disc while the disk has no system, whatever order
/// its variables recorded: a machine first started with an empty drive
/// lists the disc after the EFI shell.
fn boot_prompt_args() -> Vec<String> {
    vec!["-serial".into(), "chardev:fwcon".into()]
}

/// A Windows 11 machine's drivers disc in a CD drive of its own on
/// `ide.2`, after the user's own on `ide.1`; nothing when it was not
/// built or shipped.
fn drivers_disc_args(arch: Arch) -> Vec<String> {
    match crate::disc_library::drivers_iso(arch) {
        Some(iso) => vec![
            "-drive".into(),
            format!("if=none,id=drivers0,media=cdrom,readonly=on,file={}", opt_value(&iso.display().to_string())),
            "-device".into(),
            "ide-cd,bus=ide.2,drive=drivers0".into(),
        ],
        None => Vec::new(),
    }
}

impl Machine {
    /// The clipboard's channel (M23, doc 24 §3): QEMU's `qemu-vdagent` on
    /// a virtio-serial port named as the SPICE agent's, which the guest's
    /// agent opens (virtio-win's vioser). The player joins QEMU's
    /// clipboard when it sees the chardev. Modern machines only.
    pub fn clipboard_args(&self) -> Vec<String> {
        if !self.clipboard || !self.family.is_modern() {
            return Vec::new();
        }
        [
            "-chardev",
            "qemu-vdagent,id=vda,clipboard=on,mouse=off",
            "-device",
            "virtio-serial-pci",
            "-device",
            "virtserialport,chardev=vda,name=com.redhat.spice.0",
        ]
        .map(String::from)
        .to_vec()
    }

    /// The folder the player shares (M23): only on a modern machine with
    /// its network, which the guest reaches it through.
    pub fn share_folder(&self) -> Option<&Path> {
        self.shared_folder.as_deref().filter(|_| self.network && self.family.is_modern())
    }
}

/// A modern machine's variable store, in its bundle directory.
pub const EFI_VARS_FILE: &str = "efivars.qcow2";
/// A modern machine's TPM state, in its bundle directory.
pub const TPM_STATE_FILE: &str = "tpm.permall";

/// One line of arguments, as the form's "Extra QEMU arguments" field
/// takes them, into a list. Whitespace separates; single or double quotes
/// group, and are removed, wherever they sit in a word (`-name "a b"`,
/// `file="C:\My Games\x.img"`). There is no escape character, so a
/// Windows path's backslashes stay what they are. `Err` for a quote that
/// is never closed.
pub fn split_args(line: &str) -> Result<Vec<String>, &'static str> {
    let mut args = Vec::new();
    let mut word: Option<String> = None;
    let mut quote: Option<char> = None;
    for c in line.chars() {
        match quote {
            Some(q) if c == q => quote = None,
            Some(_) => word.get_or_insert_with(String::new).push(c),
            None if c == '"' || c == '\'' => {
                quote = Some(c);
                word.get_or_insert_with(String::new);
            }
            None if c.is_whitespace() => args.extend(word.take()),
            None => word.get_or_insert_with(String::new).push(c),
        }
    }
    if quote.is_some() {
        return Err("A quote in the extra QEMU arguments is never closed.");
    }
    args.extend(word);
    Ok(args)
}

/// The inverse of [`split_args`]: a list back into the one line the form
/// shows, quoting only what needs it, so `split_args(&join_args(a)) == a`.
pub fn join_args(args: &[String]) -> String {
    let words: Vec<String> = args
        .iter()
        .map(|arg| {
            if !arg.is_empty() && !arg.chars().any(|c| c.is_whitespace() || c == '"' || c == '\'') {
                return arg.clone();
            }
            // Double quotes around everything, and a double quote itself
            // as '"', the one thing a double-quoted run cannot hold.
            let mut out = String::from("\"");
            for c in arg.chars() {
                if c == '"' {
                    out.push_str("\"'\"'\"");
                } else {
                    out.push(c);
                }
            }
            out.push('"');
            out
        })
        .collect();
    words.join(" ")
}
