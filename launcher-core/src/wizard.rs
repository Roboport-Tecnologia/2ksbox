//! The guided creation form (doc 07): family, name, memory, processor,
//! acceleration, networking, the pointer, disk and install media, then a
//! bundle written from doc 06's reference defaults. The same form edits
//! an existing bundle (`open_edit`), where `submit` writes back in place
//! instead of reserving a new library directory. It never builds a QEMU
//! command line.
//!
//! **Everything except the widgets is here**, including the sentences.
//! A front end reads `ram_note()`, `accel_note()`, `network_notes()` and
//! `seamless_mouse_notes()` and prints them; it never composes its own.
//! When two front ends each composed them, they told the user different
//! things about the same checkbox ("Windows won't see a card" against
//! "the guest won't see a card"), and the Qt one (retired) also lacked
//! the processor, floppy and boot fields.
//!
//! A retained-mode front end has a property setter and no
//! before/after pair to compare. So a field with a consequence (family,
//! memory, acceleration, networking, the processor) is private, with a
//! `choose_*` that applies the consequence and a `reset_*` that puts it
//! back on the family's default. The plain fields (a name, a path, a
//! checkbox with nothing behind it) are public and a front end writes
//! them directly.

use crate::browse::Filter;
use crate::bundle::{
    self, Accel, Boot, CpuSpeed, D3d9, Family, Machine, Music, Optimization, Optimizations, Pad,
    Sound, Video,
};
use crate::disc_library::DISC_FILTER;
use crate::shader_library::{self, ProfileEntry};
use crate::{host_gpu, library, player};
use std::path::{Path, PathBuf};

/// The shader picker's first row, and the "Shader" column's word for a
/// machine that names no profile (`machines::Machines::shader_label`).
pub const SHADER_DEFAULT_LABEL: &str = "(default)";

/// The Voodoo 2 checkbox (`Form::voodoo2`), and the machine details' row
/// for the same field (`machines::details`).
pub const VOODOO2_LABEL: &str = "3dfx Voodoo 2";
/// The checkbox beside it (`Form::voodoo2_undither`).
pub const VOODOO2_UNDITHER_LABEL: &str = "Voodoo3 undither filter";

pub const DISK_FILTER: Filter<'static> = ("Disk images", &["qcow2", "img", "raw"]);
pub const FLOPPY_FILTER: Filter<'static> = ("Floppy images", &["img", "ima", "vfd", "flp"]);
/// A General MIDI bank for the machine's MIDI port (doc 20 §4). SF2
/// only: the engine reads SoundFont 2, and an `.sf3`'s samples are
/// Vorbis-compressed.
pub const SOUNDFONT_FILTER: Filter<'static> = ("SoundFont banks", &["sf2"]);
/// Re-exported so a front end drawing this form needs one import for its
/// three file fields; the constant itself belongs to the shelf.
pub const MEDIA_FILTER: Filter<'static> = DISC_FILTER;

/// The pages the form is shown on, as a settings window's sidebar like
/// VirtualBox's and UTM's (one long form had outgrown its window). Which
/// field sits on which page is the view's business. The pages, their
/// names and their order are the form's, so every front end offers the
/// same ones in the same order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Section {
    #[default]
    General,
    System,
    Display,
    Audio,
    Input,
    Network,
    Storage,
}

impl Section {
    pub const ALL: [Section; 7] = [
        Section::General,
        Section::System,
        Section::Display,
        Section::Audio,
        Section::Input,
        Section::Network,
        Section::Storage,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Section::General => "General",
            Section::System => "System",
            Section::Display => "Display",
            Section::Audio => "Audio",
            Section::Input => "Input",
            Section::Network => "Network",
            Section::Storage => "Storage",
        }
    }
}

/// What editing an existing bundle needs to preserve: the fields this
/// form doesn't expose, so a quick edit can't silently discard them.
struct EditTarget {
    bundle_path: PathBuf,
    shader: Option<PathBuf>,
    /// The adapter the bundle had when it was opened, so the form can
    /// warn that changing it is a hardware change to a guest that is
    /// already installed (`video_warning`).
    video: Video,
    /// The same, for the sound card: a guest that already has a driver
    /// for one card finds another on its next start (`sound_warning`).
    sound: Sound,
    /// The gamepad setting the bundle had when it was opened, for the
    /// same reason: gaining or losing the USB controller is a hardware
    /// change (`pad_warning`).
    pad: Pad,
    /// The board the bundle was created on, kept through an edit: the
    /// form has no field for it (`Machine::board`).
    board: Option<String>,
    /// A Windows 11 machine's processor, firmware variables and TPM
    /// state, which the form has no fields for and must not lose.
    arch: Option<bundle::Arch>,
    efi_vars: Option<PathBuf>,
    tpm_state: Option<PathBuf>,
}

/// The acceleration hint under the picker, and whether it is a warning
/// (a machine that will refuse to start) rather than a note.
pub struct AccelNote {
    pub text: String,
    pub warning: bool,
}

/// What the acceleration note says of a modern guest emulated: Windows
/// 11 takes minutes to start, and runs no better after.
const TOO_SLOW: &str = "CPU emulation is too slow to run this OS.";

/// A sentence's first letter in upper case.
fn capitalized(text: &str) -> String {
    let mut chars = text.chars();
    chars.next().map(|c| c.to_uppercase().chain(chars).collect()).unwrap_or_default()
}

pub struct Form {
    /// Whether the window is up. A front end owns the window; this is
    /// the model's own answer to "should it be".
    pub open: bool,
    pub name: String,
    /// A floppy image in A:, or empty for an empty drive.
    pub floppy: String,
    pub boot: Boot,
    /// The display adapter, on a family that has a choice of one
    /// (`video_applies`). Private, unlike `boot`, because it is one of
    /// the four fields whose list changes with the family. Windows
    /// chooses between our adapter and the Cirrus, `Other` and DOS
    /// between the two standard ones, so a value carried across a family
    /// switch can be one the new family does not offer, and
    /// `choose_family` has to put it back.
    video: Video,
    /// Which Direct3D 9 the host runs the executor on (`D3d9`). No
    /// `_chosen` flag, because it does not depend on the family: it is a
    /// property of the host, with the same three entries on every
    /// machine that has our adapter.
    d3d9: D3d9,
    /// Whether the adapter in the field is one somebody picked, the way
    /// `ram_chosen` works for the memory. Until it is, switching family
    /// moves it to the new family's default. Once it is, it survives the
    /// switch, unless the new family does not offer it at all. Without
    /// this flag, a new machine switched from 98 to XP kept the Cirrus,
    /// 98's default at the time and not XP's; the same holds for the
    /// three fields below.
    video_chosen: bool,
    /// The sound card, and what is on the MIDI port (doc 20 §6).
    /// Private for the same reason `video` is: each family offers a
    /// different list, so a value carried across a family switch can be
    /// one the new family does not have.
    sound: Sound,
    sound_chosen: bool,
    music: Music,
    music_chosen: bool,
    /// A SoundFont bank of the user's own, or empty for the one we
    /// ship. A plain file field, like the floppy.
    pub soundfont: String,
    /// The directory holding the user's own Roland ROMs. There is no
    /// default: `Music::Mt32` without it is a machine that refuses to
    /// start, so `submit` says so instead (doc 20 §4).
    pub mt32_roms: String,
    /// Windows 11 (M23): a folder of this computer's the guest sees as
    /// `\\10.0.2.4\host`, empty for none. A plain path field; what it
    /// takes (the machine's network) is in `shared_folder_notes`.
    pub shared_folder: String,
    /// Windows 11 (M23): the clipboard shared with the host. A plain
    /// checkbox; what it needs in the guest is in `clipboard_notes`.
    pub clipboard: bool,
    /// What a host gamepad does for this machine (M13). Private for the
    /// same reason `video` is: `Usb` is offered on the Windows families
    /// and on `Other` but never on DOS, which has no USB stack, so a
    /// value carried across a family switch can be one the new family
    /// does not offer and `choose_family` has to put it back.
    pad: Pad,
    pad_chosen: bool,
    pub existing_disk: bool,
    pub disk_path: String,
    /// The new disk's size. Public, since it is a plain number field, so
    /// there is no `_chosen` flag: `choose_family` moves it to the new
    /// family's default only while it still holds the old family's.
    pub disk_size_gb: u32,
    pub install_media: String,
    /// The "Extra QEMU arguments" line, as typed. Split into the
    /// bundle's list only when the machine is written
    /// (`bundle::split_args`), so a quote still being typed is not an
    /// error until then.
    pub extra_qemu_args: String,
    /// A shader profile id (`shader_library`), or `None` for the app
    /// default, which is the library's default profile when one is marked
    /// (`shader_library::default_id`). Independent of `EditTarget::shader`
    /// (see `bundle::Machine`).
    pub shader_profile: Option<String>,
    /// What the last `submit` failed with, for the form to show.
    pub error: Option<String>,
    /// The bundle the last successful `submit` wrote.
    saved_path: Option<PathBuf>,

    family: Family,
    ram_mb: u32,
    /// Whether the memory field holds a value someone chose. Until it
    /// does, switching family moves it to that family's own default (doc
    /// 06), which is what picking "Windows XP" after "Windows 98" means;
    /// once a number has been set, a later family switch must not throw it
    /// away.
    ram_chosen: bool,
    accel: Accel,
    /// Same as `ram_chosen`, for the accelerator: until someone picks
    /// one, the family's own default follows the family (Win98 is
    /// emulated, XP is automatic; `bundle::default_accel`).
    accel_chosen: bool,
    /// Whether the machine gets a network adapter at all. Follows the
    /// family until someone touches it, like memory and the accelerator
    /// (`bundle::default_network`, currently off for every family). The
    /// follow keeps the form and `Machine::reference` agreeing about a
    /// new machine.
    network: bool,
    network_chosen: bool,
    /// Whether the host pointer walks into this machine (the USB tablet)
    /// or the window grabs it (the PS/2 mouse alone). Follows the family
    /// like the fields above: DOS cannot read a tablet at all.
    seamless_mouse: bool,
    seamless_mouse_chosen: bool,
    /// A 3dfx Voodoo 2 beside the adapter (doc 21). Off for every
    /// family unless picked, so there is nothing to follow the family
    /// with and no `_chosen` flag.
    voodoo2: bool,
    /// The card's dither undone at scanout (doc 21 §12). Off unless
    /// picked, like the card itself, and only meaningful with it. The
    /// form turns it off with the card rather than keeping a setting
    /// that reaches no device.
    voodoo2_undither: bool,
    /// The CPU the guest should feel like, with the same rule. It is the
    /// field that makes a DOS machine a DOS machine, so switching family
    /// to DOS must bring it along.
    cpu_speed: CpuSpeed,
    cpu_speed_chosen: bool,
    /// Which of our own emulator fast paths this machine runs with.
    /// Private like the fields above, though nothing here follows the
    /// family: a checkbox that is off means someone is in the middle of
    /// a diagnosis. `choose_optimization` is the only way to set one,
    /// which keeps the "only the difference is stored" rule
    /// (`Optimizations::set`) out of the front ends.
    optimizations: Optimizations,

    /// Whether this host can give a guest KVM. Read once when the form
    /// opens rather than per frame: it opens `/dev/kvm` to find out (a
    /// bare `exists()` misses the "not in the `kvm` group" case), and
    /// the answer cannot change while a form is on screen.
    have_kvm: bool,
    /// The same, for the guest's 3D (`host_gpu`, ADR-013): a probe is a
    /// whole `VkInstance`, and the answer cannot change while a form is
    /// on screen. `cached`, so one session pays for it once.
    host_gpu: host_gpu::HostGpu,
    editing: Option<EditTarget>,
    /// The page on show. Every open starts on the first; a front end
    /// that puts a reopened machine back on its page does so itself.
    pub section: Section,
}

impl Default for Form {
    fn default() -> Self {
        Form {
            open: false,
            section: Section::General,
            name: String::new(),
            floppy: String::new(),
            boot: Boot::default(),
            video: bundle::default_video(Family::Win98).unwrap_or(Video::Std),
            video_chosen: false,
            d3d9: D3d9::Auto,
            sound: bundle::default_sound(Family::Win98),
            sound_chosen: false,
            music: bundle::default_music(Family::Win98),
            music_chosen: false,
            soundfont: String::new(),
            mt32_roms: String::new(),
            shared_folder: String::new(),
            clipboard: true,
            pad: bundle::default_pad(Family::Win98),
            pad_chosen: false,
            existing_disk: false,
            disk_path: String::new(),
            disk_size_gb: bundle::default_disk_size_gb(Family::Win98),
            install_media: String::new(),
            shader_profile: None,
            error: None,
            saved_path: None,
            family: Family::Win98,
            ram_mb: bundle::default_ram_mb(Family::Win98),
            ram_chosen: false,
            accel: bundle::default_accel(Family::Win98),
            accel_chosen: false,
            network: bundle::default_network(Family::Win98),
            network_chosen: false,
            seamless_mouse: bundle::default_seamless_mouse(Family::Win98),
            seamless_mouse_chosen: false,
            voodoo2: false,
            voodoo2_undither: false,
            extra_qemu_args: String::new(),
            cpu_speed: bundle::default_cpu_speed(Family::Win98),
            cpu_speed_chosen: false,
            optimizations: Optimizations::default(),
            have_kvm: player::hw_accel_available(),
            host_gpu: host_gpu::cached().gpu,
            editing: None,
        }
    }
}

// --- opening -------------------------------------------------------

impl Form {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Form { open: true, ..Default::default() };
    }

    /// The same, starting on a given family, with everything that
    /// follows from it (memory, the accelerator, the processor, the NIC).
    pub fn open_new(&mut self, family: Family) {
        self.open_fresh();
        self.choose_family(family);
    }

    /// Open the form pre-filled from an existing bundle, to edit it in
    /// place instead of creating a new one.
    pub fn open_edit(&mut self, machine: &Machine, bundle_path: PathBuf) {
        *self = Form {
            open: true,
            name: machine.name.clone(),
            family: machine.family,
            ram_mb: machine.ram_mb,
            // An existing machine's memory is a chosen value, whatever
            // it came from: changing family must not rewrite it. Same
            // for the fields below.
            ram_chosen: true,
            accel: machine.effective_accel(),
            accel_chosen: true,
            network: machine.network,
            network_chosen: true,
            seamless_mouse: machine.seamless_mouse,
            seamless_mouse_chosen: true,
            voodoo2: machine.voodoo2,
            voodoo2_undither: machine.voodoo2_undither,
            extra_qemu_args: bundle::join_args(&machine.extra_qemu_args),
            cpu_speed: machine.effective_cpu_speed(),
            cpu_speed_chosen: true,
            optimizations: machine.optimizations.clone(),
            floppy: machine.floppy.as_ref().map(|f| f.display().to_string()).unwrap_or_default(),
            boot: machine.effective_boot(),
            // An existing machine's adapter, card, MIDI port and pad are
            // chosen values for the same reason its memory is: whatever
            // they came from, switching family must not rewrite them.
            video: machine.effective_video().unwrap_or(Video::Std),
            video_chosen: true,
            d3d9: machine.d3d9.unwrap_or(D3d9::Auto),
            sound: machine.effective_sound(),
            sound_chosen: true,
            music: machine.effective_music(),
            music_chosen: true,
            soundfont: machine.soundfont.as_ref().map(|f| f.display().to_string()).unwrap_or_default(),
            mt32_roms: machine.mt32_roms.as_ref().map(|d| d.display().to_string()).unwrap_or_default(),
            shared_folder: machine.shared_folder.as_ref().map(|d| d.display().to_string()).unwrap_or_default(),
            clipboard: machine.clipboard,
            pad: machine.effective_pad(),
            pad_chosen: true,
            existing_disk: true,
            disk_path: machine.disk.display().to_string(),
            install_media: machine.boot_disc().map(|d| d.display().to_string()).unwrap_or_default(),
            shader_profile: machine.shader_profile.clone(),
            editing: Some(EditTarget {
                bundle_path,
                shader: machine.shader.clone(),
                video: machine.effective_video().unwrap_or(Video::Std),
                sound: machine.effective_sound(),
                pad: machine.effective_pad(),
                board: machine.board.clone(),
                arch: machine.arch,
                efi_vars: machine.efi_vars.clone(),
                tpm_state: machine.tpm_state.clone(),
            }),
            ..Default::default()
        };
    }

    /// The same, from a bundle path, for a front end that addresses
    /// windows by path (one where every window re-reads the bundle
    /// rather than being handed a copy). The load error, if any, lands
    /// in `error` and the form does not open.
    pub fn open_edit_path(&mut self, bundle_path: PathBuf) {
        match Machine::load(&bundle_path) {
            Ok(machine) => self.open_edit(&machine, bundle_path),
            Err(e) => {
                self.open = false;
                self.error = Some(format!("{}: {e}", bundle_path.display()));
            }
        }
    }

    /// Headless construction with the same fields the widgets would have
    /// set, so `submit`'s real disk-creation and save logic can be
    /// exercised without a GUI click (`cli`'s `--wizard-new`).
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Form {
        let mut form = Form { name, ..Default::default() };
        form.choose_family(family);
        // After the family, which would otherwise move a size that
        // happens to equal Win98's default along to the new family's.
        form.disk_size_gb = disk_size_gb;
        form
    }

    pub fn is_editing(&self) -> bool {
        self.editing.is_some()
    }

    /// The pages, in order, for a sidebar.
    pub fn section_labels() -> Vec<&'static str> {
        Section::ALL.iter().map(|s| s.label()).collect()
    }

    pub fn choose_section(&mut self, section: Section) {
        self.section = section;
    }

    /// The bundle being edited, `None` for a new machine. This is the
    /// form's identity, for a front end that keeps something per machine
    /// (a window that keeps its scroll position while the same machine
    /// is reopened and starts at the top for another).
    pub fn bundle_path(&self) -> Option<&Path> {
        self.editing.as_ref().map(|e| e.bundle_path.as_path())
    }

    /// "Edit machine" or "New machine", the window's own title.
    pub fn title(&self) -> &'static str {
        if self.is_editing() {
            "Edit machine"
        } else {
            "New machine"
        }
    }
}

// --- the fields with a consequence ----------------------------------

impl Form {
    pub fn family(&self) -> Family {
        self.family
    }

    /// Pick the family, moving whatever nobody has chosen to that
    /// family's own default with it.
    pub fn choose_family(&mut self, family: Family) {
        if self.disk_size_gb == bundle::default_disk_size_gb(self.family) {
            self.disk_size_gb = bundle::default_disk_size_gb(family);
        }
        self.family = family;
        if !self.ram_chosen {
            self.ram_mb = bundle::default_ram_mb(family);
        }
        if !self.accel_chosen {
            self.accel = bundle::default_accel(family);
        }
        if !self.cpu_speed_chosen {
            self.cpu_speed = bundle::default_cpu_speed(family);
        }
        if !self.network_chosen {
            self.network = bundle::default_network(family);
        }
        if !self.seamless_mouse_chosen {
            self.seamless_mouse = bundle::default_seamless_mouse(family);
        }
        // The adapter, the card, the MIDI port and the gamepad take the
        // new family's default on either of two counts, where the fields
        // above have only the first. Either nobody picked one, so it
        // follows the family like everything else (a new machine moved
        // from 98 to XP has to arrive on XP's AC'97, not keep 98's SB16),
        // or somebody did and the new family does not offer it at all.
        // Our own adapter is not offered for BeOS, a Gravis not to XP, an
        // ES1370 not to DOS, and a `Usb` pad needs a USB stack DOS lacks.
        if !self.sound_chosen || !bundle::sound_choices(family).contains(&self.sound) {
            self.sound = bundle::default_sound(family);
        }
        if !self.music_chosen || !bundle::music_choices(family).contains(&self.music) {
            self.music = bundle::default_music(family);
        }
        if !self.video_chosen || !bundle::video_choices(family).contains(&self.video) {
            if let Some(default) = bundle::default_video(family) {
                self.video = default;
            }
        }
        if !self.pad_chosen || !bundle::pad_choices(family).contains(&self.pad) {
            self.pad = bundle::default_pad(family);
        }
        // The new family's ceiling may be below the memory already in
        // the field (Win98 stops at 512 MB), so the clamp is part of the
        // switch rather than something the drawing code remembers to do.
        self.ram_mb = self.ram_mb.clamp(*self.ram_range().start(), *self.ram_range().end());
    }

    /// What picking this family means, under the picker. Only `Other`
    /// has anything to say. The other three are the reference machines
    /// doc 06 describes. `Other` is defined by what it does not get (our
    /// display driver and the 3D pass-through, both Windows-only) and by
    /// hardware chosen for guests we cannot test here, and without the
    /// note someone would find that out by installing an OS onto it.
    /// Windows 11 has its own: what the machine is, where its installer
    /// comes from, and that it needs the host's hardware virtualization.
    /// Windows 7 says it is the 32-bit one (64-bit loads no unsigned
    /// driver, so it would get no Aero) and what gives it Aero.
    pub fn family_note(&self) -> Option<&'static str> {
        match self.family {
            Family::Win7 => Some(
                "Windows 7, 32-bit. Install from a 32-bit Windows 7 ISO.\n\
                 Then run SETUP from the guest tools: it installs the display driver Aero needs. \
                 Confirm the unsigned driver prompt.",
            ),
            Family::Other => Some(
                "For an era OS other than Windows or DOS: BeOS, a period Linux, OS/2. \
                 Standard hardware these systems have drivers for: a VESA VGA, an RTL8139 network card \
                 and an ES1370 sound card.\n\
                 No 3D: the display driver and the Direct3D and OpenGL pass-through are Windows-only.",
            ),
            // Not on a Windows host yet: QEMU has no TPM there (track
            // M20). On an Arm host, Windows 11 on Arm (step 4).
            Family::Win11 if !cfg!(target_os = "windows") && self.arch() == bundle::Arch::Aarch64 => Some(
                "Windows 11 on Arm: UEFI and a TPM 2.0. \
                 Install from Microsoft's Windows 11 ISO for Arm64.\n\
                 A second CD drive holds the network and display drivers, and setup installs them.\n\
                 Most x64 apps run, through Windows' own emulation. Drivers must be built for Arm64.",
            ),
            // No x64 Windows 11 on a Mac (user decision 2026-10-01): an
            // x86_64 machine there (an Intel Mac's, or one copied from
            // Linux) gets the refusal the player gives.
            Family::Win11 if cfg!(target_os = "macos") => Some(crate::player::mac_x64_refusal()),
            Family::Win11 if cfg!(target_os = "linux") => Some(
                "A current PC: UEFI with Secure Boot available, and a TPM 2.0. \
                 Install from Microsoft's Windows 11 ISO (x64).",
            ),
            Family::Win11 => Some("Windows 11 machines don't run on this computer yet."),
            _ => None,
        }
    }

    pub fn ram_mb(&self) -> u32 {
        self.ram_mb
    }

    pub fn ram_range(&self) -> std::ops::RangeInclusive<u32> {
        bundle::ram_mb_range(self.family)
    }

    pub fn ram_is_default(&self) -> bool {
        self.ram_mb == bundle::default_ram_mb(self.family)
    }

    /// Set the memory, clamped to the family's range. A value outside it
    /// is corrected here rather than refused at save time, so the form
    /// cannot produce a Win98 machine with more memory than Win98 can
    /// boot with.
    pub fn choose_ram_mb(&mut self, ram_mb: u32) {
        let range = self.ram_range();
        self.ram_mb = ram_mb.clamp(*range.start(), *range.end());
        self.ram_chosen = true;
    }

    pub fn reset_ram(&mut self) {
        self.ram_mb = bundle::default_ram_mb(self.family);
        self.ram_chosen = false;
    }

    pub fn ram_note(&self) -> Option<&'static str> {
        match self.family {
            Family::Win98 if self.ram_mb >= *self.ram_range().end() => {
                Some("512 MB is the most Windows 98 can boot with.")
            }
            // The range cannot enforce this one: it depends on which OS
            // is going in, which this family does not know. A period
            // Linux is happy with 3 GB.
            Family::Other if self.ram_mb > 1024 => {
                Some("BeOS R5 won't boot with more than 1 GB. Most other systems of the era are fine.")
            }
            _ => None,
        }
    }

    pub fn cpu_speed(&self) -> CpuSpeed {
        self.cpu_speed
    }

    pub fn cpu_speed_is_default(&self) -> bool {
        self.cpu_speed == bundle::default_cpu_speed(self.family)
    }

    pub fn choose_cpu_speed(&mut self, cpu_speed: CpuSpeed) {
        self.cpu_speed = cpu_speed;
        self.cpu_speed_chosen = true;
    }

    pub fn reset_cpu_speed(&mut self) {
        self.cpu_speed = bundle::default_cpu_speed(self.family);
        self.cpu_speed_chosen = false;
    }

    /// What follows from the chosen processor, said before the machine
    /// is created rather than after it behaves oddly.
    /// Whether the processor picker is a question here. A period
    /// processor means nothing to Windows 11, whose machine always runs
    /// at full speed.
    pub fn cpu_speed_applies(&self) -> bool {
        !self.family.is_modern()
    }

    pub fn cpu_speed_notes(&self) -> &'static [&'static str] {
        if self.cpu_speed == CpuSpeed::Unthrottled {
            &["Full speed. Right for Windows. Most DOS games of the 486 era need a slower processor."]
        } else {
            &[
                "DOS-era software runs as fast as the CPU lets it, so a period processor keeps a game playable.",
                "A chosen processor means the machine is emulated: KVM can't slow a CPU down.",
            ]
        }
    }

    pub fn accel(&self) -> Accel {
        self.accel
    }

    pub fn accel_is_default(&self) -> bool {
        self.accel == bundle::default_accel(self.family)
    }

    pub fn choose_accel(&mut self, accel: Accel) {
        self.accel = accel;
        self.accel_chosen = true;
    }

    pub fn reset_accel(&mut self) {
        self.accel = bundle::default_accel(self.family);
        self.accel_chosen = false;
    }

    /// Whether this host has hardware acceleration, so "Automatic" in
    /// the picker can say what it means here.
    pub fn have_kvm(&self) -> bool {
        self.hw_accel()
    }

    /// The acceleration picker's entries. Hardware acceleration alone is
    /// offered only where this host can run the machine with it, so a
    /// machine that would not start is never one pick away. A machine
    /// already set to it keeps the entry, so the picker can show what it
    /// is, under the note that says it won't start here.
    pub fn accel_choices(&self) -> Vec<Accel> {
        Accel::ALL.into_iter().filter(|&a| a != Accel::Kvm || self.hw_accel() || self.accel == Accel::Kvm).collect()
    }

    /// Whether this host's hardware acceleration can run this machine
    /// (`bundle::Arch::hypervisor_runs`): on a Mac Windows 11 on Arm
    /// alone, never the era's i386.
    fn hw_accel(&self) -> bool {
        let target = if self.family.is_modern() { self.arch().qemu_target() } else { "i386" };
        self.have_kvm && bundle::Arch::hypervisor_runs(target)
    }

    /// A Windows 11 machine's processor: the edited machine's own, the
    /// host's for a new one (`bundle::Machine::reference`).
    fn arch(&self) -> bundle::Arch {
        match &self.editing {
            Some(edit) => edit.arch.unwrap_or_default(),
            None => bundle::Arch::native(),
        }
    }

    pub fn accel_note(&self) -> AccelNote {
        // "Hardware virtualization (KVM)" on Linux, WHPX on Windows, HVF
        // on macOS: the picker's entry says only "Hardware
        // virtualization", and the note names what this host has (user).
        let hw = match player::hw_accel_label() {
            Some(kind) => format!("hardware virtualization ({kind})"),
            None => "hardware virtualization".to_owned(),
        };
        // The host has it, but not for this machine (an era one on a Mac,
        // `hw_accel`): say that rather than "no hypervisor".
        let not_for_this = self.have_kvm && !self.hw_accel();
        let mut text = match (self.accel, self.hw_accel()) {
            (Accel::Auto, true) => format!("{} is available and will be used.", capitalized(&hw)),
            (Accel::Auto, false) if not_for_this => {
                format!("{} runs only Windows 11 here, so this machine will be emulated.", capitalized(&hw))
            }
            (Accel::Auto, false) => format!("No {hw} on this host, so the machine will be emulated."),
            (Accel::Kvm, true) => format!("{} is available.", capitalized(&hw)),
            (Accel::Kvm, false) if not_for_this => {
                format!("{} runs only Windows 11 here. This machine won't start.", capitalized(&hw))
            }
            (Accel::Kvm, false) => format!("No {hw} on this host. This machine won't start."),
            (Accel::Tcg, _) if self.family.is_modern() => TOO_SLOW.to_string(),
            // Windows 7 runs `-cpu max` (bundle::qemu_args): about three
            // minutes to its desktop emulated (track M18's boots).
            (Accel::Tcg, _) if self.family == Family::Win7 => {
                "Emulated CPU. Windows 7 runs, slowly: a few minutes to the desktop.".to_string()
            }
            // Every other era family runs `-cpu pentium3`.
            (Accel::Tcg, _) => "Emulated Pentium 3 equivalent CPU. Suitable for older OSes.".to_string(),
        };
        // A modern guest emulated, picked or by default: a warning (user).
        let too_slow = self.family.is_modern() && !self.will_use_kvm();
        if too_slow && self.accel == Accel::Auto {
            text.push('\n');
            text.push_str(TOO_SLOW);
        }
        if self.family == Family::Win98 && self.accel != Accel::Tcg && self.hw_accel() {
            text.push('\n');
            text.push_str(&format!("Under {hw} Windows 98 runs at full host speed, which triggers its fast-CPU bugs."));
        }
        let wont_start = matches!((self.accel, self.hw_accel()), (Accel::Kvm, false));
        AccelNote { text, warning: wont_start || too_slow }
    }

    /// What this host will give the guest's 3D, under the acceleration
    /// row (ADR-013). Not a picker, because the host settles it; the note
    /// exists so nobody finds out after the machine exists. The two
    /// families with no Direct3D get no line: DOS, and `Other`, whose
    /// guests cannot reach the pass-through at all because its guest half
    /// is a set of Windows DLLs (`family_note` says so once, where the
    /// choice is made).
    ///
    /// `warning` is true only for a software Vulkan driver: the executor
    /// runs there, slowly, and that is the one case that will disappoint
    /// the user. A host below Vulkan 1.3 gets a plain note: Windows runs
    /// the executor on its own Direct3D 9, Linux and macOS on Wine's when
    /// a Wine and the executor's Windows build are there
    /// (`host_gpu::D3dBackend`). Only a host with none of these
    /// (`D3dBackend::None`) has no executor, and its guest has no
    /// Direct3D pass-through at all.
    ///
    /// On that last host with our adapter picked, the note also says to
    /// keep it. The guest driver asks the host for an executor before it
    /// offers Direct3D, so only that half goes. The Cirrus has no
    /// Direct3D either and loses the flip chain's pacing, 8 bpp palettes
    /// and the cursor. Switching adapters would cost a driver install for
    /// nothing, and the image may later move to a host that has Vulkan.
    pub fn graphics_note(&self) -> Option<AccelNote> {
        if matches!(self.family, Family::Dos | Family::Other | Family::Win11) {
            return None;
        }
        let mut text = format!("3D: {}", self.host_gpu.d3d_headline());
        if self.host_gpu.backend() == host_gpu::D3dBackend::None && self.video == Video::D3dpt {
            text.push_str("\nKeep the 2ksbox adapter anyway. Only its Direct3D needs Vulkan.");
        }
        if let Some(advice) = self.host_gpu.d3d_advice() {
            text.push('\n');
            text.push_str(&advice);
        }
        Some(AccelNote { text, warning: self.host_gpu.is_slow() })
    }

    pub fn network(&self) -> bool {
        self.network
    }

    pub fn choose_network(&mut self, network: bool) {
        self.network = network;
        self.network_chosen = true;
    }

    /// One checkbox for one question: does this machine have a network
    /// card. When it does, it gets QEMU's user-mode NAT (doc 06's
    /// per-family NIC), which needs no host privileges and lets nothing
    /// on the network in. The note says so, since "networking" otherwise
    /// sounds like the guest is put on the LAN, and says once, next to
    /// the switch, that these guests stopped getting security fixes
    /// twenty years ago.
    pub fn network_notes(&self) -> &'static [&'static str] {
        if self.network && self.family.is_modern() {
            &["Outbound only, through the host (NAT). Nothing on the network can reach the guest."]
        } else if self.network && self.family == Family::Win7 {
            &[
                "Outbound only, through the host (NAT). Nothing on the network can reach the guest.",
                "Windows 7 hasn't had security updates since 2020. Don't browse the web on it.",
            ]
        } else if self.network {
            &[
                "Outbound only, through the host (NAT). Nothing on the network can reach the guest.",
                "These systems haven't had security updates in twenty years. Don't browse the web on them.",
            ]
        } else {
            &["No network adapter. The guest won't see a card or ask for a driver."]
        }
    }

    pub fn seamless_mouse(&self) -> bool {
        self.seamless_mouse
    }

    pub fn choose_seamless_mouse(&mut self, seamless_mouse: bool) {
        self.seamless_mouse = seamless_mouse;
        self.seamless_mouse_chosen = true;
    }

    pub fn voodoo2(&self) -> bool {
        self.voodoo2
    }

    /// Whether the Voodoo 2 checkbox is a question here: a PCI card of
    /// 1998 with drivers for 9x and XP, so not on Windows 11.
    pub fn voodoo2_applies(&self) -> bool {
        !self.family.is_modern()
    }

    /// Whether the shared folder and the clipboard are questions here:
    /// both need our agent, which is for Windows 11 (M23; the era's
    /// machines keep the folder disc).
    pub fn integration_applies(&self) -> bool {
        self.family.is_modern()
    }

    /// Under the "Shared folder" field.
    pub fn shared_folder_notes(&self) -> &'static [&'static str] {
        match (self.shared_folder.trim().is_empty(), self.network) {
            (true, _) => &["No folder of this computer's is shared with the guest."],
            (false, false) => &["Shared only with Networking on: the guest reaches the folder through the machine's network."],
            (false, true) => &[
                "Windows sees it as \\\\10.0.2.4\\host (user 2ksbox, password 2ksbox), and the 2ksbox agent puts it on a drive letter.",
                "The folder is shared with read and write permissions.",
            ],
        }
    }

    /// Under the "Clipboard" checkbox: what the guest needs for it.
    pub fn clipboard_notes(&self) -> &'static [&'static str] {
        if self.clipboard {
            &[
                "Text copied on either side can be pasted on the other.",
                "Needs the 2ksbox agent in Windows: run 2ksbox\\install.cmd from the drivers disc once.",
            ]
        } else {
            &["Copy and paste stay inside the machine."]
        }
    }

    /// Whether the floppy field is: Windows 11's machine (a q35) has no
    /// floppy controller.
    pub fn floppy_applies(&self) -> bool {
        !self.family.is_modern()
    }

    /// Under the "Extra QEMU arguments" field: what it is for, or a
    /// quote left open, reported while the form is still there to fix it
    /// (saving refuses it too).
    pub fn extra_qemu_args_note(&self) -> AccelNote {
        match bundle::split_args(&self.extra_qemu_args) {
            Err(e) => AccelNote { text: e.to_string(), warning: true },
            Ok(_) => AccelNote {
                text: "Added to the end of QEMU's command line, for testing. Quote an argument that has spaces. \
                       A wrong one can stop the machine from starting."
                    .to_string(),
                warning: false,
            },
        }
    }

    pub fn choose_voodoo2(&mut self, voodoo2: bool) {
        self.voodoo2 = voodoo2;
        if !voodoo2 {
            // The setting belongs to the card: with no card it would
            // reach no device, and leaving it ticked would say otherwise.
            self.voodoo2_undither = false;
        }
    }

    pub fn voodoo2_undither(&self) -> bool {
        self.voodoo2_undither
    }

    /// Only answerable with the card, so the front end shows it beside
    /// the Voodoo 2's own box and disabled without it.
    pub fn voodoo2_undither_enabled(&self) -> bool {
        self.voodoo2
    }

    pub fn choose_voodoo2_undither(&mut self, undither: bool) {
        self.voodoo2_undither = undither && self.voodoo2;
    }

    /// The sentence under the pair. The box is named after the Voodoo3's
    /// "22-bit" scanout filter, the thing a user of the era knows, so the
    /// first sentence says what that filter was for and the second how
    /// this one differs: not a blur but the dither arithmetically undone,
    /// so nothing drawn sharp comes back soft, at a main-loop cost per
    /// frame, which is why it is not always on. Without the card there
    /// is no sentence: the greyed box beside the card's own says enough.
    pub fn voodoo2_undither_notes(&self) -> &'static [&'static str] {
        if !self.voodoo2 {
            &[]
        } else if self.voodoo2_undither {
            &[
                "Like the Voodoo3's 22-bit filter, this removes the 16-bit dither the chip stores its picture with, so skies and shading come out smooth instead of speckled.",
                "Unlike that filter it is exact, not a blur: the dither pattern is inverted, so textures and edges stay as drawn. Costs about 1.4 ms per frame on the host.",
            ]
        } else {
            &["The picture is shown as a Voodoo 2 showed it: the chip's 16-bit dither pattern, unfiltered."]
        }
    }

    /// One checkbox: is there a 3dfx Voodoo 2 in the machine (doc 21).
    /// The sentences carry what the name does not say: the card is a
    /// second display device that does nothing until a real 3dfx Voodoo2
    /// driver is installed in the guest (the user's own download; the
    /// guest tools disc carries none, doc 21 §10), and it is the only
    /// Glide a machine has (ADR-020). The driver line is its own sentence
    /// on its own line, so it is read before the first boot rather than
    /// found after a game shows no card. With the box off, they say
    /// Glide games have no card.
    pub fn voodoo2_notes(&self) -> &'static [&'static str] {
        if self.voodoo2 {
            &[
                "Adds a 3dfx Voodoo 2 next to the display adapter. Glide games then render on the emulated chip, in software on the host CPU, at 640×480 to 800×600.",
                "The card does nothing until a real Voodoo 2 driver is installed in the guest: 3dfx's own Voodoo2 driver, your own download. The guest tools disc does not carry one.",
            ]
        } else {
            &["No Voodoo 2. Glide games (3dfx) find no card on this machine. Turn it on for them, then install 3dfx's own Voodoo2 driver in the guest."]
        }
    }

    /// One checkbox for one question: does the host pointer walk into
    /// this machine, or does the window take it. A desktop wants the
    /// first, and a game that turns the view with the mouse needs the
    /// second. So the sentences carry the hotkey (a grabbed pointer with
    /// no way out is the worst thing this form can produce) and the
    /// symptom that sends someone back here: mouselook against an
    /// absolute device sticks instead of turning.
    ///
    /// DOS is caught before the machine exists: its mouse drivers read
    /// the PS/2 controller, so a tablet leaves such a guest with no
    /// pointer at all. `Other` has the tablet off by default for a milder
    /// version of that (`default_seamless_mouse`) and says what to look
    /// for if someone turns it on.
    pub fn seamless_mouse_notes(&self) -> &'static [&'static str] {
        match (self.seamless_mouse, self.family) {
            (true, Family::Dos) => &[
                "The host pointer moves straight into the guest. No grab, no hotkey.",
                "DOS mouse drivers read the PS/2 mouse, so with the tablet a DOS guest has no pointer at all.",
            ],
            (true, Family::Other) => &[
                "The host pointer moves straight into the guest. No grab, no hotkey. The guest gets a USB tablet, which reports absolute positions.",
                "Whether it works depends on the guest: BeOS and an old XFree86 need setting up for an absolute pointer. Turn this off if the cursor doesn't move.",
            ],
            (true, _) => &[
                "The host pointer moves straight into the guest. No grab, no hotkey. The guest gets a USB tablet, which reports absolute positions.",
            ],
            (false, _) => &[
                "PS/2 mouse only. Click the window to take the pointer, Ctrl+Alt+G to release it.",
                "Mouselook needs this. If a game's view sticks instead of turning, turn seamless mouse off.",
            ],
        }
    }

    pub fn optimizations(&self) -> &Optimizations {
        &self.optimizations
    }

    pub fn optimization_enabled(&self, opt: Optimization) -> bool {
        self.optimizations.enabled(opt)
    }

    pub fn choose_optimization(&mut self, opt: Optimization, on: bool) {
        self.optimizations.set(opt, on);
    }

    /// Every fast path back on its shipped setting: the way out of a
    /// diagnosis session.
    pub fn reset_optimizations(&mut self) {
        self.optimizations.reset();
    }

    pub fn optimizations_are_default(&self) -> bool {
        self.optimizations.all_default()
    }

    /// Every switch off at once: the control run for "did one of ours
    /// break this guest", otherwise fourteen clicks and easy to get
    /// half-right.
    pub fn disable_all_optimizations(&mut self) {
        self.optimizations.disable_all();
    }

    /// The other end of the same shortcut.
    pub fn enable_all_optimizations(&mut self) {
        self.optimizations.enable_all();
    }

    /// Whether they are all off already, so a front end can grey the
    /// button rather than offer a no-op.
    pub fn optimizations_all_off(&self) -> bool {
        self.optimizations.all_off()
    }

    pub fn optimizations_all_on(&self) -> bool {
        self.optimizations.all_on()
    }

    /// What the collapsed section says about itself, so a machine with
    /// something turned off says so without being opened.
    pub fn optimizations_summary(&self) -> String {
        self.optimizations.summary()
    }

    /// Whether the switches are worth showing: only where the machine
    /// will be emulated, since they are speed-ups in the emulator (user).
    pub fn optimizations_apply(&self) -> bool {
        !self.will_use_kvm()
    }

    /// The line above the switches. On a machine that will run on KVM
    /// they all do nothing, because the host CPU executes the guest's
    /// instructions and no emulator is in the path.
    pub fn optimizations_note(&self) -> &'static str {
        if self.will_use_kvm() {
            "This machine runs on KVM, where these have no effect: they are speed-ups in the emulator. \
             Choose Emulation above (or a processor, which forces it) to use them."
        } else if self.optimizations.all_off() {
            "All off: the plain emulator with none of our speed-ups. Use this as the control when a guest \
             misbehaves. Expect it to be slow."
        } else {
            "Our own speed-ups to QEMU's emulator. If a guest computes wrong numbers or stops drawing, \
             turn these off one at a time, or all at once, to find out whether one of them is the cause."
        }
    }

    /// Whether this machine, as the form stands, will run on KVM: what
    /// `effective_accel` decides, plus what this host has. `Automatic` on
    /// a box without `/dev/kvm` is emulation.
    fn will_use_kvm(&self) -> bool {
        self.cpu_speed.icount_shift().is_none()
            && match self.accel {
                Accel::Kvm => true,
                Accel::Auto => self.hw_accel(),
                Accel::Tcg => false,
            }
    }

    pub fn video(&self) -> Video {
        self.video
    }

    /// Which adapters this machine's family offers, its default first.
    /// A picker fills itself from this, so no front end has to know that
    /// Windows is offered a different pair than `Other`.
    pub fn video_choices(&self) -> &'static [Video] {
        bundle::video_choices(self.family)
    }

    /// Whether there is an adapter to choose at all, so a front end
    /// shows or hides the row without knowing which family that is.
    /// Every family offers a pair today, so this is always true. It
    /// stays because the answer is `video_choices`'s to give, and a
    /// family that gains a fixed adapter should not need a front end
    /// changed.
    pub fn video_applies(&self) -> bool {
        !self.video_choices().is_empty()
    }

    pub fn video_is_default(&self) -> bool {
        Some(self.video) == bundle::default_video(self.family)
    }

    pub fn d3d9(&self) -> D3d9 {
        self.d3d9
    }

    /// The three entries, in the order a picker shows them (`Auto`
    /// first, because it is what every machine starts on).
    pub fn d3d9_choices(&self) -> &'static [D3d9] {
        bundle::d3d9_choices()
    }

    /// Whether there is anything to pick here: only our own adapter
    /// carries the executor, so on a Cirrus or a standard VGA machine
    /// the row is not a question at all and a front end hides it.
    pub fn d3d9_applies(&self) -> bool {
        self.video == Video::D3dpt
    }

    pub fn d3d9_is_default(&self) -> bool {
        self.d3d9 == D3d9::Auto
    }

    pub fn choose_d3d9(&mut self, d3d9: D3d9) {
        self.d3d9 = d3d9;
    }

    pub fn reset_d3d9(&mut self) {
        self.d3d9 = D3d9::Auto;
    }

    /// The line under the picker: what the entry in the field means.
    /// For `Auto`, the only entry that asks the host anything, it is
    /// only what this host will do with it, the host's own headline and
    /// advice (`host_gpu`), not the paths it might take elsewhere. That line belongs beside this picker, the one it
    /// answers; `graphics_note()` carries it for a front end with no
    /// Direct3D picker. A warning only for the software Vulkan driver,
    /// the case that runs and disappoints.
    pub fn d3d9_note(&self) -> AccelNote {
        let mut text = self.d3d9.note().to_string();
        let mut warning = false;
        // Automatic is whatever this host runs, so its note is that, not
        // every path it could take (user).
        if self.d3d9 == D3d9::Auto {
            text = self.host_gpu.d3d_headline();
            if self.host_gpu.backend() == host_gpu::D3dBackend::None {
                text.push_str("\nKeep the 2ksbox adapter anyway. Only its Direct3D needs Vulkan.");
            }
            if let Some(advice) = self.host_gpu.d3d_advice() {
                text.push('\n');
                text.push_str(&advice);
            }
            warning = self.host_gpu.is_slow();
        }
        AccelNote { text, warning }
    }

    /// An adapter this family does not offer is refused rather than
    /// stored: the list is per family and a front end may be a frame
    /// behind on it.
    pub fn choose_video(&mut self, video: Video) {
        if self.video_choices().contains(&video) {
            self.video = video;
            self.video_chosen = true;
        }
    }

    /// Back to this family's default, and back to following the family,
    /// so a later switch moves it again. "Default" means the field was
    /// never really touched.
    pub fn reset_video(&mut self) {
        if let Some(default) = bundle::default_video(self.family) {
            self.video = default;
            self.video_chosen = false;
        }
    }

    /// What the chosen adapter means for this family. The same Cirrus is
    /// Windows' in-box driver on one machine and a period XFree86 driver
    /// on another, and it costs different things: on Windows, picking it
    /// gives up our whole display path (docs 15, 19).
    pub fn video_notes(&self) -> &'static [&'static str] {
        match (self.video, self.family) {
            (Video::D3dpt, Family::Win7) => &[
                "2ksbox's own display adapter and driver: Aero, the full mode table and Direct3D through the driver.",
                "Needs the driver from the guest-tools ISO (SETUP). Until it's installed, Windows uses a basic VGA driver.",
            ],
            (Video::Std, Family::Win7) => &[
                "Windows drives it with its own VGA driver. 2D only: no Aero and no Direct3D through a driver.",
            ],
            (Video::D3dpt, _) => &[
                "2ksbox's own display adapter and driver: the full mode table, page flips that pace games, and Direct3D through the driver.",
                "Needs the driver from the guest-tools ISO. Until it's installed, the guest sees a plain VGA.",
            ],
            (Video::Cirrus, Family::Win98 | Family::Xp) => &[
                "A Cirrus Logic GD5446, which Windows has a driver for. 2D only: no paced page flips and no Direct3D through the driver.",
                "Use it before our driver is installed, or to check whether a game misbehaves on ours.",
            ],
            (Video::Cirrus, Family::Dos) => &[
                "A Cirrus Logic GD5446 with its period VGA/VESA BIOS. This is what a DOS machine here always had.",
                "Nothing to install either way. If a game's modes come out wrong, try the standard VGA.",
            ],
            (Video::Std, Family::Dos) => &[
                "The Bochs adapter: VBE 2.0 with a linear frame buffer, the fuller of the two VESA BIOSes.",
                "Try it when a game's high-resolution modes are wrong or missing on the Cirrus. Some games know the Cirrus and not this one.",
            ],
            (Video::Cirrus, _) => &[
                "A real chip of the era, so the guest probably has a native driver for it. BeOS R5 and XFree86 both do.",
                "Its VESA BIOS is the weaker of the two. Try it when the standard VGA leaves the guest in plain VGA.",
            ],
            (Video::Std, Family::Win11) => &[
                "Windows drives it with its own basic display driver. No 3D.",
            ],
            (Video::Std, _) => &[
                "The Bochs adapter: VBE 2.0 with a linear frame buffer. Works with period VESA drivers and with a modern Linux (bochs-drm).",
                "The safe choice: a guest with no native driver still gets its VESA modes.",
            ],
        }
    }

    // --- the sound card and the MIDI port (doc 20 §6) ---------------

    pub fn sound(&self) -> Sound {
        self.sound
    }

    /// The cards this family offers. Never empty: "no sound card" is an
    /// entry, so every family has a picker.
    pub fn sound_choices(&self) -> &'static [Sound] {
        bundle::sound_choices(self.family)
    }

    pub fn sound_is_default(&self) -> bool {
        self.sound == bundle::default_sound(self.family)
    }

    pub fn choose_sound(&mut self, sound: Sound) {
        if self.sound_choices().contains(&sound) {
            self.sound = sound;
            self.sound_chosen = true;
        }
    }

    pub fn reset_sound(&mut self) {
        self.sound = bundle::default_sound(self.family);
        self.sound_chosen = false;
    }

    pub fn music(&self) -> Music {
        self.music
    }

    pub fn music_choices(&self) -> &'static [Music] {
        bundle::music_choices(self.family)
    }

    pub fn music_is_default(&self) -> bool {
        self.music == bundle::default_music(self.family)
    }

    pub fn choose_music(&mut self, music: Music) {
        if self.music_choices().contains(&music) {
            self.music = music;
            self.music_chosen = true;
        }
    }

    pub fn reset_music(&mut self) {
        self.music = bundle::default_music(self.family);
        self.music_chosen = false;
    }

    /// The shader picker's rows: the app default first (named after the
    /// library's default profile when one is marked, or "None" on a
    /// modern machine, `shader_library::default_label_for`), then every profile of the
    /// library by name, in `shader_library::scan`'s order. A front end hands the same `profiles` to the three verbs
    /// below, so a row is a profile and nothing in the widget translates
    /// between an index and an id. When the Qt launcher's window (retired
    /// 2026-10-02) did that itself it needed a delegate of its own, and
    /// its combo box looked unlike the others in the form.
    pub fn shader_profile_labels(&self, profiles: &[ProfileEntry]) -> Vec<String> {
        std::iter::once(shader_library::default_label_for(self.family, profiles))
            .chain(profiles.iter().map(|e| e.profile.name.clone()))
            .collect()
    }

    /// The row the form's `shader_profile` is on: 0 for the app default,
    /// and 0 too for an id no profile of `profiles` has any more (a
    /// profile deleted after a machine picked it plays as the default,
    /// `shader_library::find`, so that is what the picker shows).
    pub fn shader_profile_index(&self, profiles: &[ProfileEntry]) -> usize {
        self.shader_profile
            .as_deref()
            .and_then(|id| profiles.iter().position(|e| shader_library::id_of(&e.path) == id))
            .map_or(0, |i| i + 1)
    }

    pub fn shader_profile_is_default(&self) -> bool {
        self.shader_profile.is_none()
    }

    /// Row `index` of `shader_profile_labels(profiles)`; a row past the
    /// end is ignored, as `choose_sound` ignores a card the family lacks.
    pub fn choose_shader_profile(&mut self, profiles: &[ProfileEntry], index: usize) {
        if index == 0 {
            self.shader_profile = None;
        } else if let Some(entry) = profiles.get(index - 1) {
            self.shader_profile = Some(shader_library::id_of(&entry.path));
        }
    }

    pub fn reset_shader_profile(&mut self) {
        self.shader_profile = None;
    }

    /// Whether the MIDI port is a question at all: Windows 11's family
    /// offers none (`bundle::music_choices`).
    pub fn music_applies(&self) -> bool {
        self.music_choices().len() > 1
    }

    /// Whether the SoundFont field is worth showing at all.
    pub fn soundfont_applies(&self) -> bool {
        self.music == Music::Gm
    }

    /// Whether the ROM directory field is, and it is not optional there.
    pub fn mt32_roms_applies(&self) -> bool {
        self.music == Music::Mt32
    }

    /// What the card does for the guest's music, the part of this screen
    /// that is not obvious: whether the machine has FM at all, and what
    /// the guest has to do before it hears anything.
    pub fn sound_notes(&self) -> &'static [&'static str] {
        match (self.sound, self.family) {
            (Sound::Sb16, Family::Dos) => &[
                "The card DOS games look for, with an OPL3 for AdLib music. Nothing to install.",
                "For AUTOEXEC.BAT: BLASTER=A220 I5 D1 H5 P330 T6. P330 is the MIDI port.",
            ],
            (Sound::Sb16, _) => &[
                "Windows has the driver built in, and DOS programs inside the guest find the card they expect.",
                "Includes the OPL3, so games with AdLib-only music still play it.",
            ],
            (Sound::Ac97, Family::Win98) => &[
                "A more modern sound card, but it doesn't work well for DOS games.",
            ],
            (Sound::Ac97, _) => &[
                "XP has the driver built in. This is the card XP machines always had.",
                "No FM chip. Nothing of this era needs one.",
            ],
            (Sound::Es1370, _) => &[
                "A PCI card that BeOS R5 and a period Linux both have drivers for.",
            ],
            (Sound::Gus, _) => &[
                "A wavetable card. Games written for it sound better on it than on anything else.",
                "The guest needs Gravis's own drivers and the ULTRASND line before it makes any sound.",
                "No FM chip, and no Sound Blaster compatibility except through Gravis's own emulation.",
            ],
            (Sound::Adlib, _) => &[
                "FM music only, no digital audio. Speech and sound effects will be silent.",
            ],
            (Sound::Hda, _) => &[
                "Windows has the driver built in.",
            ],
            (Sound::None, _) => &[
                "No sound card. The machine can still have a MIDI port for music.",
            ],
        }
    }

    /// What is behind the MIDI port, and what it needs.
    pub fn music_notes(&self) -> &'static [&'static str] {
        match (self.music, self.family) {
            (Music::Gm, Family::Win98) => &[
                "An MPU-401 at 0x330 with a General MIDI synthesizer behind it. Much better than the FM chip Windows 98 plays MIDI on otherwise.",
                "Windows finds it after you add \"MPU-401 Compatible\" in Add New Hardware and pick it under Multimedia.",
                "Leave the bank empty to use the built-in one. Your own SoundFont changes how everything sounds.",
            ],
            (Music::Gm, _) => &[
                "An MPU-401 at 0x330 with a General MIDI synthesizer behind it: what a game's setup calls \"General MIDI\" or \"MPU-401\".",
                "Leave the bank empty to use the built-in one. Your own SoundFont changes how everything sounds.",
            ],
            (Music::Mt32, _) => &[
                "A Roland MT-32 family module, what a 1990 game means by \"Roland\". Sounds nothing like General MIDI.",
                "Needs your own CM-32L ROM images. Nothing of Roland's is included, and the machine won't start without them.",
            ],
            (Music::None, _) => &[
                "No MIDI port. A game set to General MIDI or Roland finds nothing and falls back to FM or digital music.",
            ],
        }
    }

    /// The same warning the adapter carries, for the same reason: a
    /// guest that is already installed re-detects a card that changed.
    pub fn sound_warning(&self) -> Option<&'static str> {
        let changed = match &self.editing {
            Some(edit) => edit.sound != self.sound,
            None => false,
        };
        changed.then_some(
            "This machine already exists. Changing its sound card means the guest finds new hardware on its \
             next start, and games inside it need to be set up for the new card.",
        )
    }

    /// The warning above the picker rather than under one of its entries:
    /// changing this on a machine that already has an OS installed is a
    /// hardware change, and the guest will say so.
    ///
    /// Except on DOS, where nothing is installed for an adapter and the
    /// machine simply boots. A game's own setup can still be stale: a
    /// title that already ran its SETUP wrote down a video mode, and the
    /// two adapters do not offer the same list. DOS is told that instead.
    pub fn video_warning(&self) -> Option<&'static str> {
        if !self.is_editing() || self.video_is_default_for_machine() {
            return None;
        }
        Some(match self.family {
            Family::Dos => {
                "This machine already exists. It boots on the new adapter with nothing to install, but a game \
                 that already ran its setup may have saved a video mode this adapter doesn't offer."
            }
            _ => {
                "This machine already exists. Changing its adapter means the guest finds new hardware on its \
                 next start and asks for a driver before the desktop comes back."
            }
        })
    }

    /// Whether the adapter is still the one the bundle was opened with.
    fn video_is_default_for_machine(&self) -> bool {
        match &self.editing {
            Some(edit) => edit.video == self.video,
            None => true,
        }
    }

    pub fn pad(&self) -> Pad {
        self.pad
    }

    /// What this family offers a gamepad, its default first. A picker
    /// fills itself from this.
    pub fn pad_choices(&self) -> &'static [Pad] {
        bundle::pad_choices(self.family)
    }

    /// Whether there is anything to choose. True on every family (even
    /// DOS has `None` against `Keys`), but asked of the model so a front
    /// end's row does not depend on that staying true.
    pub fn pad_applies(&self) -> bool {
        self.pad_choices().len() > 1
    }

    pub fn pad_is_default(&self) -> bool {
        self.pad == bundle::default_pad(self.family)
    }

    /// A setting this family does not offer is refused rather than
    /// stored, the same way `choose_video` refuses an adapter.
    pub fn choose_pad(&mut self, pad: Pad) {
        if self.pad_choices().contains(&pad) {
            self.pad = pad;
            self.pad_chosen = true;
        }
    }

    pub fn reset_pad(&mut self) {
        self.pad = bundle::default_pad(self.family);
        self.pad_chosen = false;
    }

    /// What the chosen setting means. `Keys` states its limitation in the
    /// picker. Otherwise someone who turns it on for a Direct3D game
    /// concludes the pad is broken, when the game asked DirectInput and
    /// found no controller.
    pub fn pad_notes(&self) -> &'static [&'static str] {
        match self.pad {
            Pad::None => &[
                "A controller plugged into the host does nothing. Keyboard and mouse are unaffected.",
            ],
            Pad::Usb => &[
                "A USB controller in the machine: two analog sticks, an 8-way hat and twelve buttons, visible to DirectInput and the Game Controllers panel.",
                "Windows XP installs its own driver on the next start. Windows 98 SE does too, but asks for its install files the first time: keep the CD in the drive, or point it at the CAB folder on the disk.",
            ],
            Pad::Gameport => &[
                "The classic joystick port at 0x201: two axes and two buttons per stick, two sticks. No hat and no extra buttons. The d-pad drives the first two axes.",
                "The only controller DOS can use. A DOS game reads the port directly with nothing to install, in a DOS box under Windows 98 too.",
                "For Windows games on 98, pick the USB controller instead: Windows finds it by itself. This port needs Add New Hardware and a calibration pass in Game Controllers first.",
            ],
            Pad::Keys => &[
                "The pad presses keys: the d-pad and left stick are the arrow keys, the four face buttons are Ctrl, Alt, Space and Enter.",
                "A key mapping, not a controller: no analog steering, and a game that asks DirectInput for a joystick finds none. For games that only read the keyboard.",
            ],
        }
    }

    /// The warning above the picker: adding or removing a controller
    /// device on a machine that already has an OS installed is a hardware
    /// change the guest notices on its next start, as with the adapter.
    ///
    /// The sentence depends on the device. Windows finds a USB pad by
    /// itself and does **not** find a gameport at all: the port was never
    /// Plug and Play, so it needs Add New Hardware and then a calibration
    /// pass. Telling someone Windows would handle it is worse than saying
    /// nothing.
    ///
    /// Both sentences point at the USB pad on 98, because a USB pad
    /// reaches a Windows game there through DirectInput and winmm's
    /// `joyGetPosEx` (measured, `pad-guest-98`), so the port's install
    /// steps buy a Windows game nothing. The port is still the right
    /// answer for DOS, including a DOS box under Windows, and the
    /// picker's own note says so.
    pub fn pad_warning(&self) -> Option<&'static str> {
        /// `Keys` and `None` are the same thing to the guest: no device.
        fn device(pad: Pad) -> Option<Pad> {
            matches!(pad, Pad::Usb | Pad::Gameport).then_some(pad)
        }
        let edit = self.editing.as_ref()?;
        if device(edit.pad) == device(self.pad) {
            return None;
        }
        Some(match self.pad {
            Pad::Usb => {
                "This machine already exists. Adding the USB controller means the guest finds new hardware \
                 on its next start. Windows installs its own driver for it."
            }
            Pad::Gameport => {
                "This machine already exists, and the joystick port isn't Plug and Play. Add \"Standard Game \
                 Port\" in Add New Hardware, then calibrate the stick in Game Controllers. DOS needs neither. \
                 For Windows games on 98 the USB controller is the easier choice."
            }
            // Was a device, now is not.
            _ => {
                "This machine already exists. Removing the controller is a hardware change the guest will \
                 notice on its next start."
            }
        })
    }

    /// Whether the boot picker is a question: a modern machine's UEFI
    /// keeps its own boot order (Windows' boot manager, then the disc
    /// when the disk is empty), which a BIOS boot order does not reach.
    pub fn boot_applies(&self) -> bool {
        !self.family.is_modern()
    }

    /// The boot picker's one non-obvious case: a machine told to boot
    /// from a floppy it hasn't got.
    pub fn boot_note(&self) -> Option<&'static str> {
        (self.boot == Boot::Floppy && self.floppy.trim().is_empty())
            .then_some("No floppy image, so the machine will boot from the hard disk.")
    }
}

// --- what it writes -------------------------------------------------

impl Form {
    /// The `Machine` the current field values describe, given the disk
    /// path to use (a fresh disk's path isn't known until it's created,
    /// so callers that might still need to do that pass it in).
    pub fn build_machine(&self, disk: PathBuf) -> Machine {
        let mut machine = match &self.editing {
            Some(edit) => Machine {
                name: self.name.clone(),
                family: self.family,
                ram_mb: self.ram_mb,
                accel: Some(self.accel),
                network: self.network,
                seamless_mouse: self.seamless_mouse,
                voodoo2: self.voodoo2,
                voodoo2_undither: self.voodoo2_undither,
                extra_qemu_args: Vec::new(),
                disk,
                disc: None,
                discs: Vec::new(),
                shader_profile: None,
                shader: edit.shader.clone(),
                floppy: None,
                boot: None,
                cpu_speed: None,
                video: None,
                d3d9: None,
                sound: None,
                music: None,
                soundfont: None,
                mt32_roms: None,
                shared_folder: None,
                clipboard: false,
                pad: None,
                board: edit.board.clone(),
                arch: edit.arch,
                efi_vars: edit.efi_vars.clone(),
                tpm_state: edit.tpm_state.clone(),
                optimizations: Optimizations::default(),
            },
            None => Machine::reference(self.family, self.name.clone(), disk),
        };
        // The form owns these for a new machine too, where `reference`
        // has just filled in the family defaults. The `*_chosen` flag
        // decides, not the field's current contents: a form built without
        // the family picker (`with_new_disk`, the headless verb) never
        // moved the default along, and once wrote Win98's 256 MB into an
        // XP machine.
        machine.ram_mb = if self.ram_chosen { self.ram_mb } else { bundle::default_ram_mb(self.family) };
        // Written out explicitly either way: what the form showed is
        // what the machine gets, even when it is the family's default.
        machine.accel = Some(if self.accel_chosen { self.accel } else { bundle::default_accel(self.family) });
        machine.network = if self.network_chosen { self.network } else { bundle::default_network(self.family) };
        machine.seamless_mouse = if self.seamless_mouse_chosen {
            self.seamless_mouse
        } else {
            bundle::default_seamless_mouse(self.family)
        };
        machine.voodoo2 = self.voodoo2;
        machine.voodoo2_undither = self.voodoo2_undither;
        // `write` refuses a line that does not split before it gets here.
        machine.extra_qemu_args = bundle::split_args(&self.extra_qemu_args).unwrap_or_default();
        machine.cpu_speed =
            Some(if self.cpu_speed_chosen { self.cpu_speed } else { bundle::default_cpu_speed(self.family) });
        // Only what someone turned off is in here, so this is a clone
        // of "nothing" for almost every machine (`Optimizations`).
        machine.optimizations = self.optimizations.clone();
        machine.boot = Some(self.boot);
        // Written only when the family offers it, so a family switch
        // cannot leave behind a `video` the new family ignores.
        machine.video = bundle::video_choices(self.family).contains(&self.video).then_some(self.video);
        machine.d3d9 = Some(self.d3d9);
        // Written out explicitly, like the accelerator: what the form
        // showed is what the machine gets, even when it is the family's
        // default. A bundle that names its card is not changed under the
        // user by a later change to that default.
        machine.sound = Some(self.sound);
        machine.music = Some(self.music);
        // Only the user's own files, and only where they mean anything:
        // no bank is kept on a machine whose port was turned off.
        machine.soundfont = (self.music == Music::Gm)
            .then(|| Some(self.soundfont.trim()).filter(|f| !f.is_empty()).map(PathBuf::from))
            .flatten();
        machine.mt32_roms = (self.music == Music::Mt32)
            .then(|| Some(self.mt32_roms.trim()).filter(|d| !d.is_empty()).map(PathBuf::from))
            .flatten();
        // Same rule as `video`: only a setting this family offers is
        // written. DOS offers no `Usb` and XP/`Other` no `Gameport`
        // (`bundle::pad_choices`), so a family switch must not keep one.
        machine.pad = bundle::pad_choices(self.family).contains(&self.pad).then_some(self.pad);
        machine.floppy = Some(self.floppy.trim()).filter(|f| !f.is_empty()).map(PathBuf::from);
        // Only where the family offers them (Windows 11, M23).
        let modern = self.family.is_modern();
        machine.shared_folder =
            Some(self.shared_folder.trim()).filter(|d| modern && !d.is_empty()).map(PathBuf::from);
        machine.clipboard = modern && self.clipboard;
        machine.shader_profile = self.shader_profile.clone();
        // The single slot this form has is the machine's boot disc;
        // everything else lives on the shared shelf (`disc_library.rs`).
        machine.disc = Some(self.install_media.trim()).filter(|m| !m.is_empty()).map(PathBuf::from);
        machine
    }

    /// Create or save the machine, closing the form on success. `None`
    /// leaves it open with `error` saying why.
    pub fn submit(&mut self, library_dir: &Path) -> Option<PathBuf> {
        match self.write(library_dir) {
            Ok(path) => {
                self.error = None;
                self.saved_path = Some(path.clone());
                self.open = false;
                Some(path)
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    /// The `machine.toml` the last successful `submit` wrote.
    pub fn saved_path(&self) -> Option<&Path> {
        self.saved_path.as_deref()
    }

    fn write(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
        if self.name.trim().is_empty() {
            return Err(std::io::Error::other("A name is required."));
        }
        // The one field with no default and no fallback: an MT-32
        // machine with no ROMs fails to start, and failing here says so
        // while there is still a form to fix it in (doc 20 §4).
        if self.music == Music::Mt32 && self.mt32_roms.trim().is_empty() {
            return Err(std::io::Error::other(
                "The Roland MT-32 needs a folder with your own CM-32L ROM images.",
            ));
        }
        if let Err(e) = bundle::split_args(&self.extra_qemu_args) {
            return Err(std::io::Error::other(e));
        }
        if let Some(edit) = &self.editing {
            let bundle_path = edit.bundle_path.clone();
            self.build_machine(PathBuf::from(&self.disk_path)).save(&bundle_path)?;
            return Ok(bundle_path);
        }
        let dir = library::reserve_dir(library_dir, &self.name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        let disk_path = if self.existing_disk {
            PathBuf::from(&self.disk_path)
        } else {
            let disk_path = dir.join("disk.qcow2");
            player::create_disk(&disk_path, self.disk_size_gb)?;
            disk_path
        };
        let mut machine = self.build_machine(disk_path);
        // A modern machine's firmware variables and TPM live in its own
        // folder whatever disk it has; the first start makes them
        // (`player::prepare`).
        if machine.family.is_modern() {
            machine.efi_vars = Some(dir.join(bundle::EFI_VARS_FILE));
            machine.tpm_state = Some(dir.join(bundle::TPM_STATE_FILE));
        }
        machine.save(&bundle_path)?;
        Ok(bundle_path)
    }
}
