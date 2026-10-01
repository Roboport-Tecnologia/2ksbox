//! The guided creation wizard (doc 07), as a QObject over
//! `launcher_core::wizard::Form`.
//!
//! The form is the shared one: which defaults follow the family until
//! someone chooses otherwise, the per-family memory clamp, what
//! `build_machine` writes, what `submit` does differently when editing,
//! and the sentences under each row. This file is the projection onto
//! Q_PROPERTYs, in both directions:
//!
//! * **`publish`** reads the form and writes every property through its
//!   generated setter, never by direct field assignment (the header of
//!   `main.rs` has the trap).
//! * **`pull`** copies the plain, two-way-bound text fields back into
//!   the form. A QML `TextField` writes its property and nothing else,
//!   so the form is caught up before anything reads it (`submit`). The
//!   fields with a consequence never go this way: they have no writable
//!   property, only `choose_*`, so a new widget cannot forget the
//!   "…_chosen" rule.
//!
//! The combo boxes' labels come from the form's own enums
//! (`family_labels`, `accel_labels`, …) rather than being retyped in
//! QML, and so do the file dialog's name filters.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qstringlist.h");
        type QStringList = cxx_qt_lib::QStringList;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(bool, open)]
        #[qproperty(bool, editing)]
        #[qproperty(QString, title)]
        /// The bundle being edited, "" for a new machine: the form's
        /// identity, which the window compares with the last one it
        /// showed to decide whether to start at the top.
        #[qproperty(QString, bundle_path)]
        /// An index into `section_labels()`: the page on show.
        #[qproperty(i32, section)]
        /// An index into `family_labels()`, because that is what a QML
        /// `ComboBox` deals in. Same for `accel`, `cpu_speed` and `boot`.
        #[qproperty(i32, family)]
        /// Empty except on `Other`, which is the one family whose
        /// hardware isn't the reference machine doc 06 describes.
        #[qproperty(QString, family_note)]
        #[qproperty(QString, name)]
        #[qproperty(i32, ram_mb)]
        #[qproperty(i32, ram_min)]
        #[qproperty(i32, ram_max)]
        #[qproperty(QString, ram_note)]
        #[qproperty(bool, ram_is_default)]
        #[qproperty(i32, cpu_speed)]
        #[qproperty(QString, cpu_note)]
        #[qproperty(bool, cpu_is_default)]
        #[qproperty(i32, accel)]
        #[qproperty(QString, accel_note)]
        #[qproperty(bool, accel_warning)]
        #[qproperty(bool, accel_is_default)]
        /// The display adapter: an index into `video_labels`, which is
        /// a property and not an invokable like the other label lists,
        /// because this one changes with the family (Windows chooses
        /// between our adapter and the Cirrus, an `Other` machine
        /// between the two standard ones) and a combo box bound to an
        /// invokable would keep the list it was built with. Whether
        /// there is a choice at all is the model's answer too, so the
        /// row appears on whichever family has one.
        #[qproperty(i32, video)]
        #[qproperty(bool, video_applies)]
        #[qproperty(QStringList, video_labels)]
        #[qproperty(bool, video_is_default)]
        #[qproperty(QString, video_note)]
        /// Set only while editing a machine whose adapter has been
        /// changed: the guest will find new hardware on its next start.
        #[qproperty(QString, video_warning)]
        /// Which Direct3D 9 the host runs the pass-through's executor
        /// on: an index into `d3d9_labels`, a host question rather than
        /// a guest one (ADR-007's second amendment). The list never
        /// changes with the family, but `d3d9_applies` does with the
        /// adapter: only ours carries an executor.
        ///
        /// **Every one of these names its own `cxx_name`**, and they are
        /// the only properties here that do. `#[auto_cxx_name]` turns
        /// `d3d9_labels` into `d3D9Labels` (it capitalises the letter
        /// after a digit), and QML does not report a binding to a
        /// property that does not exist: the row drew with an empty
        /// combo box and nothing else wrong. The `wizard` diag screen
        /// prints what the combo shows, so the next such slip is a line
        /// in a log.
        #[qproperty(i32, d3d9, cxx_name = "d3d9")]
        #[qproperty(bool, d3d9_applies, cxx_name = "d3d9Applies")]
        #[qproperty(QStringList, d3d9_labels, cxx_name = "d3d9Labels")]
        #[qproperty(bool, d3d9_is_default, cxx_name = "d3d9IsDefault")]
        #[qproperty(QString, d3d9_note, cxx_name = "d3d9Note")]
        /// Orange: the host's Direct3D runs on software Vulkan.
        #[qproperty(bool, d3d9_warning, cxx_name = "d3d9Warning")]
        /// The sound card and what is on the MIDI port (doc 20 §6), the
        /// same shape as the adapter above and for the same reason:
        /// each family offers a different list. `soundfont` and
        /// `mt32_roms` are the two files only the user can supply, and
        /// `*_applies` says which of them this port needs.
        #[qproperty(i32, sound)]
        #[qproperty(QStringList, sound_labels)]
        #[qproperty(bool, sound_is_default)]
        #[qproperty(QString, sound_note)]
        #[qproperty(QString, sound_warning)]
        #[qproperty(i32, music)]
        #[qproperty(QStringList, music_labels)]
        #[qproperty(bool, music_is_default)]
        #[qproperty(QString, music_note)]
        #[qproperty(QString, soundfont)]
        #[qproperty(bool, soundfont_applies)]
        /// Rows a family may not have at all (Windows 11 has no period
        /// processor, Voodoo 2, floppy or MIDI port): the form says.
        #[qproperty(bool, cpu_speed_applies)]
        #[qproperty(bool, voodoo2_applies)]
        #[qproperty(bool, floppy_applies)]
        #[qproperty(bool, music_applies)]
        #[qproperty(bool, boot_applies)]
        #[qproperty(QString, mt32_roms)]
        #[qproperty(bool, mt32_roms_applies)]
        /// What a host gamepad does for this machine (M13): an index
        /// into `pad_labels`, a property for the same reason
        /// `video_labels` is: DOS has no USB stack and is offered no USB
        /// controller, so the list changes with the family.
        #[qproperty(i32, pad)]
        #[qproperty(bool, pad_applies)]
        #[qproperty(QStringList, pad_labels)]
        #[qproperty(bool, pad_is_default)]
        #[qproperty(QString, pad_note)]
        /// Set only while editing a machine that is gaining or losing
        /// the USB controller: a hardware change the guest will notice.
        #[qproperty(QString, pad_warning)]
        #[qproperty(bool, network)]
        #[qproperty(QString, network_note)]
        #[qproperty(bool, seamless_mouse)]
        #[qproperty(QString, seamless_mouse_note)]
        /// The 3dfx Voodoo 2 (doc 21) and the sentences under it, and
        /// beside it the card's own dither undone at scanout (doc 21
        /// §12), which applies only with the card.
        #[qproperty(bool, voodoo2)]
        #[qproperty(QString, voodoo2_note)]
        #[qproperty(bool, voodoo2_undither)]
        #[qproperty(bool, voodoo2_undither_enabled)]
        #[qproperty(QString, voodoo2_undither_note)]
        /// Arguments added to the end of QEMU's command line, as one
        /// line: two-way bound like `name`, and caught up by `pull`. The
        /// note is orange for a quote left open, which saving refuses.
        #[qproperty(QString, extra_qemu_args)]
        #[qproperty(QString, extra_qemu_args_note)]
        #[qproperty(bool, extra_qemu_args_warning)]
        /// Our own emulator fast paths, as a bit per `Optimization::ALL`
        /// entry, set meaning on. A bitmask rather than a list because a
        /// QML `CheckBox` needs a property to bind `checked` to (a
        /// `Q_INVOKABLE` would never re-evaluate), and fourteen bits in an
        /// `i32` is a property the generated setter already notifies on.
        #[qproperty(i32, optimizations_mask)]
        #[qproperty(QString, optimizations_summary)]
        #[qproperty(QString, optimizations_note)]
        #[qproperty(bool, optimizations_are_default)]
        #[qproperty(bool, optimizations_all_off)]
        #[qproperty(bool, optimizations_all_on)]
        #[qproperty(bool, existing_disk)]
        #[qproperty(QString, disk_path)]
        #[qproperty(i32, disk_size_gb)]
        #[qproperty(QString, install_media)]
        #[qproperty(QString, floppy)]
        #[qproperty(i32, boot)]
        #[qproperty(QString, boot_note)]
        /// The shader profile picker, the shape of the sound card's:
        /// the rows are the model's (`Form::shader_profile_labels`, the
        /// app default first), the index is which of them this machine
        /// names, and the id is published for anything that wants the
        /// value rather than the row ("" for the app default).
        #[qproperty(QStringList, shader_profile_labels)]
        #[qproperty(i32, shader_profile_index)]
        #[qproperty(bool, shader_profile_is_default)]
        #[qproperty(QString, shader_profile)]
        #[qproperty(QString, error)]
        type Wizard = super::WizardRust;

        /// Reset to a fresh "New machine" form and open it.
        #[qinvokable]
        fn open_fresh(self: Pin<&mut Wizard>);

        /// Open the form pre-filled from an existing bundle, to edit it
        /// in place instead of creating a new one.
        #[qinvokable]
        fn open_edit(self: Pin<&mut Wizard>, bundle_path: &QString);

        /// Cancel, or the window closed: put the form away. Through the
        /// model, never `wizard.open = false` on the property: that
        /// leaves the form's own flag up, and the next republish (a
        /// profile rescan when the profile list closes) raised the
        /// property again and showed the window (doc 07).
        #[qinvokable]
        fn dismiss(self: Pin<&mut Wizard>);

        /// Pick the family, moving whatever nobody has chosen (memory,
        /// the accelerator, the processor, the NIC) to that family's own
        /// default with it.
        #[qinvokable]
        fn choose_family(self: Pin<&mut Wizard>, family: i32);
        #[qinvokable]
        fn choose_section(self: Pin<&mut Wizard>, section: i32);

        /// Set the memory, clamped to the family's range, and remember
        /// that it was chosen so a later family switch can't rewrite it.
        #[qinvokable]
        fn choose_ram(self: Pin<&mut Wizard>, ram_mb: i32);

        /// Put the memory back on the family's default, un-choosing it.
        #[qinvokable]
        fn reset_ram(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_cpu_speed(self: Pin<&mut Wizard>, cpu_speed: i32);

        #[qinvokable]
        fn reset_cpu_speed(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_accel(self: Pin<&mut Wizard>, accel: i32);

        #[qinvokable]
        fn reset_accel(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_network(self: Pin<&mut Wizard>, network: bool);

        /// The pointer: the USB tablet (the host pointer walks in) or
        /// the PS/2 mouse alone (the window grabs it).
        #[qinvokable]
        fn choose_seamless_mouse(self: Pin<&mut Wizard>, seamless_mouse: bool);

        /// A 3dfx Voodoo 2 in the machine, or not (doc 21).
        #[qinvokable]
        fn choose_voodoo2(self: Pin<&mut Wizard>, voodoo2: bool);
        /// ... and that card's dither undone at scanout (doc 21 §12).
        #[qinvokable]
        fn choose_voodoo2_undither(self: Pin<&mut Wizard>, undither: bool);

        /// The extra-arguments field lost focus: take its text and say
        /// what the form makes of it (the note under it).
        #[qinvokable]
        fn commit_extra_qemu_args(self: Pin<&mut Wizard>);

        /// Turn one fast path on or off, by its index in
        /// `optimization_labels()`.
        #[qinvokable]
        fn choose_optimization(self: Pin<&mut Wizard>, index: i32, on: bool);

        /// Every fast path back on its shipped setting.
        #[qinvokable]
        fn reset_optimizations(self: Pin<&mut Wizard>);
        /// Every optimization off, and every one on: the control run and
        /// the way back, which are fourteen clicks each without them.
        /// Each needs `#[qinvokable]`: without it the method exists in
        /// Rust and QML's call to it is a TypeError, so both buttons did
        /// nothing (the `qt-wizard` check's `optall` probe covers it).
        #[qinvokable]
        fn disable_all_optimizations(self: Pin<&mut Wizard>);
        #[qinvokable]
        fn enable_all_optimizations(self: Pin<&mut Wizard>);

        /// The boot order. A plain field with no consequence beyond its
        /// own note, but an index like the other combos.
        #[qinvokable]
        fn choose_boot(self: Pin<&mut Wizard>, boot: i32);

        /// The display adapter, the same way: an index into
        /// `video_labels`, this machine's family's own list.
        #[qinvokable]
        fn choose_video(self: Pin<&mut Wizard>, video: i32);

        /// Put it back on the family's default.
        #[qinvokable]
        fn reset_video(self: Pin<&mut Wizard>);

        /// Which Direct3D 9 the executor runs on, and back to automatic.
        /// Named explicitly for the reason the properties above are.
        #[qinvokable]
        #[cxx_name = "chooseD3d9"]
        fn choose_d3d9(self: Pin<&mut Wizard>, d3d9: i32);
        #[qinvokable]
        #[cxx_name = "resetD3d9"]
        fn reset_d3d9(self: Pin<&mut Wizard>);

        /// The sound card and the MIDI port, as indices into their own
        /// family's list, with the same reset each.
        #[qinvokable]
        fn choose_sound(self: Pin<&mut Wizard>, sound: i32);
        #[qinvokable]
        fn reset_sound(self: Pin<&mut Wizard>);
        #[qinvokable]
        fn choose_music(self: Pin<&mut Wizard>, music: i32);
        #[qinvokable]
        fn reset_music(self: Pin<&mut Wizard>);

        /// The shader profile as a row of `shaderProfileLabels`, with
        /// the same reset. `refresh_profiles` rescans the library, for
        /// a profile saved or deleted while the form is open; opening
        /// the form scans it anyway.
        #[qinvokable]
        fn choose_shader_profile(self: Pin<&mut Wizard>, row: i32);
        #[qinvokable]
        fn reset_shader_profile(self: Pin<&mut Wizard>);
        #[qinvokable]
        fn refresh_profiles(self: Pin<&mut Wizard>);

        /// The two files behind the MIDI port. Invokable rather than a
        /// writable property for the reason `set_floppy_path` is: what
        /// the form shows under the picker depends on them.
        #[qinvokable]
        fn set_soundfont_path(self: Pin<&mut Wizard>, soundfont: &QString);
        #[qinvokable]
        fn set_mt32_roms_path(self: Pin<&mut Wizard>, romdir: &QString);
        /// The gamepad, the same way: an index into `pad_labels`.
        #[qinvokable]
        fn choose_pad(self: Pin<&mut Wizard>, pad: i32);

        /// Put it back on the family's default.
        #[qinvokable]
        fn reset_pad(self: Pin<&mut Wizard>);

        /// A floppy image was typed or browsed to: the boot note depends
        /// on it ("boot from floppy" with no image falls through to the
        /// hard disk), so this is the one text field with a consequence.
        #[qinvokable]
        fn set_floppy_path(self: Pin<&mut Wizard>, floppy: &QString);

        /// Create or save the machine. Returns true on success, having
        /// closed the form; on failure `error` says why and it stays open.
        #[qinvokable]
        fn submit(self: Pin<&mut Wizard>) -> bool;

        /// The `machine.toml` written by the last successful `submit`,
        /// for the caller to rescan around.
        #[qinvokable]
        fn saved_path(self: &Wizard) -> QString;

        /// The combo boxes' labels, from the bundle's own enums rather
        /// than retyped in QML.
        #[qinvokable]
        fn family_labels(self: &Wizard) -> QStringList;
        /// The form's pages, in order: the sidebar's entries.
        #[qinvokable]
        fn section_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn accel_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn cpu_speed_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn boot_labels(self: &Wizard) -> QStringList;

        /// The fast paths' checkbox labels and the sentence under each,
        /// in `Optimization::ALL` order, the same order as the bits of
        /// `optimizations_mask`. Fixed lists, so QML can call
        /// them once as a `Repeater` model.
        #[qinvokable]
        fn optimization_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn optimization_notes(self: &Wizard) -> QStringList;

        /// The file dialog's name filters, from the bundle's own
        /// constants.
        #[qinvokable]
        fn disk_filter(self: &Wizard) -> QString;

        #[qinvokable]
        fn media_filter(self: &Wizard) -> QString;

        #[qinvokable]
        fn floppy_filter(self: &Wizard) -> QString;

        /// The bank the General MIDI port plays through.
        #[qinvokable]
        fn soundfont_filter(self: &Wizard) -> QString;
    }

    // Publish the form's defaults from the constructor (see the impl
    // below): a doc comment here is an attribute, which cxx-qt refuses on
    // a trait impl.
    impl cxx_qt::Initialize for Wizard {}
}

use crate::{qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QString, QStringList};
use launcher_core::browse::name_filter;
use launcher_core::bundle::{Accel, Boot, CpuSpeed, Family, Optimization};
use launcher_core::library;
use launcher_core::shader_library::{self, ProfileEntry};
use launcher_core::wizard::{Form, Section, DISK_FILTER, FLOPPY_FILTER, MEDIA_FILTER, SOUNDFONT_FILTER};
use std::path::PathBuf;
use std::pin::Pin;

#[derive(Default)]
pub struct WizardRust {
    open: bool,
    editing: bool,
    title: QString,
    bundle_path: QString,
    section: i32,
    family: i32,
    family_note: QString,
    name: QString,
    ram_mb: i32,
    ram_min: i32,
    ram_max: i32,
    ram_note: QString,
    ram_is_default: bool,
    cpu_speed: i32,
    cpu_note: QString,
    cpu_is_default: bool,
    accel: i32,
    accel_note: QString,
    accel_warning: bool,
    accel_is_default: bool,
    video: i32,
    video_applies: bool,
    video_labels: QStringList,
    video_is_default: bool,
    video_note: QString,
    video_warning: QString,
    d3d9: i32,
    d3d9_applies: bool,
    d3d9_labels: QStringList,
    d3d9_is_default: bool,
    d3d9_note: QString,
    d3d9_warning: bool,
    sound: i32,
    sound_labels: QStringList,
    sound_is_default: bool,
    sound_note: QString,
    sound_warning: QString,
    music: i32,
    music_labels: QStringList,
    music_is_default: bool,
    music_note: QString,
    soundfont: QString,
    soundfont_applies: bool,
    cpu_speed_applies: bool,
    voodoo2_applies: bool,
    floppy_applies: bool,
    music_applies: bool,
    boot_applies: bool,
    mt32_roms: QString,
    mt32_roms_applies: bool,
    pad: i32,
    pad_applies: bool,
    pad_labels: QStringList,
    pad_is_default: bool,
    pad_note: QString,
    pad_warning: QString,
    network: bool,
    network_note: QString,
    seamless_mouse: bool,
    seamless_mouse_note: QString,
    voodoo2: bool,
    voodoo2_note: QString,
    voodoo2_undither: bool,
    voodoo2_undither_enabled: bool,
    voodoo2_undither_note: QString,
    extra_qemu_args: QString,
    extra_qemu_args_note: QString,
    extra_qemu_args_warning: bool,
    optimizations_mask: i32,
    optimizations_summary: QString,
    optimizations_note: QString,
    optimizations_are_default: bool,
    optimizations_all_off: bool,
    optimizations_all_on: bool,
    existing_disk: bool,
    disk_path: QString,
    disk_size_gb: i32,
    install_media: QString,
    floppy: QString,
    boot: i32,
    boot_note: QString,
    shader_profile_labels: QStringList,
    shader_profile_index: i32,
    shader_profile_is_default: bool,
    shader_profile: QString,
    error: QString,

    /// The form. Everything above is a projection of it.
    form: Form,
    /// The profile library as last scanned (opening the form, and
    /// `refresh_profiles`): the shader picker's rows are these, through
    /// `Form::shader_profile_labels`.
    profiles: Vec<ProfileEntry>,
}

/// Index <-> enum, in the order each enum's own `ALL` lists it, which is
/// also the order `*_labels()` hands QML.
fn index_of<T: PartialEq + Copy>(all: &[T], value: T) -> i32 {
    all.iter().position(|v| *v == value).unwrap_or(0) as i32
}

fn at<T: Copy>(all: &[T], index: i32) -> T {
    all[(index.max(0) as usize).min(all.len() - 1)]
}

fn labels(items: impl IntoIterator<Item = &'static str>) -> QStringList {
    let mut list = QStringList::default();
    for item in items {
        list.append(QString::from(item));
    }
    list
}

impl ffi::Wizard {
    fn open_fresh(mut self: Pin<&mut Self>) {
        self.as_mut().scan_profiles();
        self.as_mut().rust_mut().form.open_fresh();
        self.publish();
    }

    fn open_edit(mut self: Pin<&mut Self>, bundle_path: &QString) {
        let path = PathBuf::from(bundle_path.to_string());
        self.as_mut().scan_profiles();
        self.as_mut().rust_mut().form.open_edit_path(path);
        self.publish();
    }

    fn dismiss(self: Pin<&mut Self>) {
        self.edit(|form| form.open = false);
    }

    fn scan_profiles(self: Pin<&mut Self>) {
        self.rust_mut().profiles = shader_library::scan(&shader_library::default_dir());
    }

    /// A rescan while the form is open is an edit like any other: the
    /// republish it ends in must not write the form's stale name back
    /// over what was typed (`edit`).
    fn refresh_profiles(mut self: Pin<&mut Self>) {
        self.as_mut().scan_profiles();
        self.edit(|_| {});
    }

    fn choose_shader_profile(self: Pin<&mut Self>, row: i32) {
        let row = row.max(0) as usize;
        self.edit_with_profiles(|form, profiles| form.choose_shader_profile(profiles, row));
    }

    fn reset_shader_profile(self: Pin<&mut Self>) {
        self.edit(Form::reset_shader_profile);
    }

    /// Through `edit` like every other verb, although the page is not a
    /// field of the machine. It republishes the form, and a republish
    /// that has not caught up with the text fields writes the form's
    /// stale, empty name back over what was typed. Going around `pull`
    /// here lost the machine name on every page switch.
    fn choose_section(self: Pin<&mut Self>, section: i32) {
        let s = *Section::ALL.get(section.max(0) as usize).unwrap_or(&Section::General);
        self.edit(|form| form.choose_section(s));
    }

    fn choose_family(self: Pin<&mut Self>, family: i32) {
        let f = at(&Family::ALL, family);
        self.edit(|form| form.choose_family(f));
    }

    fn choose_ram(self: Pin<&mut Self>, ram_mb: i32) {
        self.edit(|form| form.choose_ram_mb(ram_mb.max(0) as u32));
    }

    fn reset_ram(self: Pin<&mut Self>) {
        self.edit(Form::reset_ram);
    }

    fn choose_cpu_speed(self: Pin<&mut Self>, cpu_speed: i32) {
        let s = at(&CpuSpeed::ALL, cpu_speed);
        self.edit(|form| form.choose_cpu_speed(s));
    }

    fn reset_cpu_speed(self: Pin<&mut Self>) {
        self.edit(Form::reset_cpu_speed);
    }

    fn choose_accel(self: Pin<&mut Self>, accel: i32) {
        let a = at(&Accel::ALL, accel);
        self.edit(|form| form.choose_accel(a));
    }

    fn reset_accel(self: Pin<&mut Self>) {
        self.edit(Form::reset_accel);
    }

    fn choose_network(self: Pin<&mut Self>, network: bool) {
        self.edit(|form| form.choose_network(network));
    }

    fn choose_seamless_mouse(self: Pin<&mut Self>, seamless_mouse: bool) {
        self.edit(|form| form.choose_seamless_mouse(seamless_mouse));
    }

    fn choose_voodoo2(self: Pin<&mut Self>, voodoo2: bool) {
        self.edit(|form| form.choose_voodoo2(voodoo2));
    }

    fn choose_voodoo2_undither(self: Pin<&mut Self>, undither: bool) {
        self.edit(|form| form.choose_voodoo2_undither(undither));
    }

    fn choose_optimization(self: Pin<&mut Self>, index: i32, on: bool) {
        let opt = at(&Optimization::ALL, index);
        self.edit(|form| form.choose_optimization(opt, on));
    }

    fn reset_optimizations(self: Pin<&mut Self>) {
        self.edit(Form::reset_optimizations);
    }

    fn disable_all_optimizations(self: Pin<&mut Self>) {
        self.edit(Form::disable_all_optimizations);
    }

    fn enable_all_optimizations(self: Pin<&mut Self>) {
        self.edit(Form::enable_all_optimizations);
    }

    fn choose_boot(self: Pin<&mut Self>, boot: i32) {
        let b = at(&Boot::ALL, boot);
        self.edit(|form| form.boot = b);
    }

    fn choose_video(self: Pin<&mut Self>, video: i32) {
        self.edit(|form| {
            // Into this family's own list, not `Video::ALL`: the combo
            // box and the model must be counting the same entries.
            let v = at(form.video_choices(), video);
            form.choose_video(v);
        });
    }

    fn choose_pad(self: Pin<&mut Self>, pad: i32) {
        self.edit(|form| {
            let p = at(form.pad_choices(), pad);
            form.choose_pad(p);
        });
    }

    fn reset_pad(self: Pin<&mut Self>) {
        self.edit(Form::reset_pad);
    }

    fn reset_video(self: Pin<&mut Self>) {
        self.edit(Form::reset_video);
    }

    fn choose_d3d9(self: Pin<&mut Self>, d3d9: i32) {
        self.edit(|form| {
            let d = at(form.d3d9_choices(), d3d9);
            form.choose_d3d9(d);
        });
    }

    fn reset_d3d9(self: Pin<&mut Self>) {
        self.edit(Form::reset_d3d9);
    }

    fn choose_sound(self: Pin<&mut Self>, sound: i32) {
        self.edit(|form| {
            let c = at(form.sound_choices(), sound);
            form.choose_sound(c);
        });
    }

    fn reset_sound(self: Pin<&mut Self>) {
        self.edit(Form::reset_sound);
    }

    fn choose_music(self: Pin<&mut Self>, music: i32) {
        self.edit(|form| {
            let m = at(form.music_choices(), music);
            form.choose_music(m);
        });
    }

    fn reset_music(self: Pin<&mut Self>) {
        self.edit(Form::reset_music);
    }

    fn commit_extra_qemu_args(self: Pin<&mut Self>) {
        // `pull` has already taken the text; publishing is the point.
        self.edit(|_| {});
    }

    fn set_soundfont_path(self: Pin<&mut Self>, soundfont: &QString) {
        let soundfont = soundfont.to_string();
        self.edit(|form| form.soundfont = soundfont);
    }

    fn set_mt32_roms_path(self: Pin<&mut Self>, romdir: &QString) {
        let romdir = romdir.to_string();
        self.edit(|form| form.mt32_roms = romdir);
    }

    fn set_floppy_path(self: Pin<&mut Self>, floppy: &QString) {
        let floppy = floppy.to_string();
        self.edit(|form| form.floppy = floppy);
    }

    fn submit(mut self: Pin<&mut Self>) -> bool {
        self.as_mut().pull();
        let library_dir = library::default_dir();
        let ok = self.as_mut().rust_mut().form.submit(&library_dir).is_some();
        self.publish();
        ok
    }

    fn saved_path(&self) -> QString {
        self.rust().form.saved_path().map(|p| qs(p.display())).unwrap_or_default()
    }

    fn section_labels(&self) -> QStringList {
        labels(Form::section_labels().into_iter())
    }

    fn family_labels(&self) -> QStringList {
        labels(Family::ALL.iter().map(|f| f.label()))
    }

    fn accel_labels(&self) -> QStringList {
        labels(Accel::ALL.iter().map(|a| a.label()))
    }

    fn cpu_speed_labels(&self) -> QStringList {
        labels(CpuSpeed::ALL.iter().map(|s| s.label()))
    }

    fn boot_labels(&self) -> QStringList {
        labels(Boot::ALL.iter().map(|b| b.label()))
    }

    fn optimization_labels(&self) -> QStringList {
        labels(Optimization::ALL.iter().map(|o| o.label()))
    }

    fn optimization_notes(&self) -> QStringList {
        labels(Optimization::ALL.iter().map(|o| o.note()))
    }

    fn disk_filter(&self) -> QString {
        qs(name_filter(DISK_FILTER))
    }

    fn media_filter(&self) -> QString {
        qs(name_filter(MEDIA_FILTER))
    }

    fn floppy_filter(&self) -> QString {
        qs(name_filter(FLOPPY_FILTER))
    }

    fn soundfont_filter(&self) -> QString {
        qs(name_filter(SOUNDFONT_FILTER))
    }
}

impl ffi::Wizard {
    /// Every verb that changes the form does the same three things, in
    /// this order. **The `pull` is not optional**: QML's text fields and
    /// check boxes write the property and nothing else, so a form that
    /// has not been caught up still holds what it was opened with, and
    /// the `publish` at the end writes that back over what the user
    /// typed. A machine name followed by any combo box change
    /// disappeared that way.
    fn edit(mut self: Pin<&mut Self>, change: impl FnOnce(&mut Form)) {
        self.as_mut().pull();
        change(&mut self.as_mut().rust_mut().form);
        self.publish();
    }

    /// `edit` for the verb whose answer is a row of the profile library.
    fn edit_with_profiles(mut self: Pin<&mut Self>, change: impl FnOnce(&mut Form, &[ProfileEntry])) {
        self.as_mut().pull();
        {
            let mut this = self.as_mut().rust_mut();
            let this = &mut *this;
            change(&mut this.form, &this.profiles);
        }
        self.publish();
    }

    /// The plain, two-way-bound text fields, back into the form. A QML
    /// `TextField` writes its property and nothing else, so this catches
    /// the form up before anything reads it.
    fn pull(mut self: Pin<&mut Self>) {
        let (name, disk_path, install_media, floppy, extra_qemu_args) = (
            self.name.to_string(),
            self.disk_path.to_string(),
            self.install_media.to_string(),
            self.floppy.to_string(),
            self.extra_qemu_args.to_string(),
        );
        let (existing_disk, disk_size_gb) = (self.existing_disk, self.disk_size_gb);
        let form = &mut self.as_mut().rust_mut().form;
        form.name = name;
        form.disk_path = disk_path;
        form.install_media = install_media;
        form.floppy = floppy;
        form.extra_qemu_args = extra_qemu_args;
        form.existing_disk = existing_disk;
        form.disk_size_gb = disk_size_gb.max(1) as u32;
    }

    /// The form, onto the properties, each through its own setter, so
    /// each notify fires for the values that moved. Read out first,
    /// written after: the setters take `&mut self`.
    ///
    /// **Order matters here, twice.** A control that clamps (the memory
    /// `SpinBox`) must get its range before its value, because Qt bounds
    /// the value against the range it has at that moment and does not
    /// revisit it when the range widens later. The memory field showed a
    /// fresh Win98 machine as 32 MB, the bottom of its range, because 256
    /// arrived while the range was still the model's initial 0..0. And
    /// `open` goes **last**, because `Main.qml` shows the window on it,
    /// so everything the first frame draws must already be current.
    fn publish(mut self: Pin<&mut Self>) {
        let (
            open,
            editing,
            title,
            bundle_path,
            section,
            family,
            family_note,
            name,
            ram_mb,
            ram_min,
            ram_max,
            ram_note,
            ram_is_default,
            cpu_speed,
            cpu_note,
            cpu_is_default,
        );
        let (accel, accel_note, accel_warning, accel_is_default, network, network_note);
        let (seamless_mouse, seamless_mouse_note);
        let (voodoo2, voodoo2_note);
        let (voodoo2_undither, voodoo2_undither_enabled, voodoo2_undither_note);
        let (extra_qemu_args, extra_qemu_args_note, extra_qemu_args_warning);
        let (video, video_applies, video_labels, video_is_default, video_note, video_warning);
        let (d3d9, d3d9_applies, d3d9_labels, d3d9_is_default, d3d9_note, d3d9_warning);
        let (sound, sound_labels, sound_is_default, sound_note, sound_warning);
        let (music, music_labels, music_is_default, music_note);
        let (soundfont, soundfont_applies, mt32_roms, mt32_roms_applies);
        let (cpu_speed_applies, voodoo2_applies, floppy_applies, music_applies, boot_applies);
        let (pad, pad_applies, pad_labels, pad_is_default, pad_note, pad_warning);
        let (optimizations_mask, optimizations_summary, optimizations_note, optimizations_are_default, optimizations_all_off, optimizations_all_on);
        let (existing_disk, disk_path, disk_size_gb, install_media, floppy, boot, boot_note);
        let (shader_profile_labels, shader_profile_index, shader_profile_is_default, shader_profile, error);
        {
            let f = &self.rust().form;
            let profiles = &self.rust().profiles;
            let range = f.ram_range();
            let note = f.accel_note();
            open = f.open;
            editing = f.is_editing();
            bundle_path = f.bundle_path().map(|p| qs(p.display())).unwrap_or_default();
            section = index_of(&Section::ALL, f.section);
            title = QString::from(f.title());
            family = index_of(&Family::ALL, f.family());
            family_note = qs_opt(f.family_note());
            name = qs(&f.name);
            ram_mb = f.ram_mb() as i32;
            ram_min = *range.start() as i32;
            ram_max = *range.end() as i32;
            ram_note = qs_opt(f.ram_note());
            ram_is_default = f.ram_is_default();
            cpu_speed = index_of(&CpuSpeed::ALL, f.cpu_speed());
            cpu_note = qs(f.cpu_speed_notes().join("\n"));
            cpu_is_default = f.cpu_speed_is_default();
            accel = index_of(&Accel::ALL, f.accel());
            accel_warning = note.warning;
            accel_note = qs(note.text);
            accel_is_default = f.accel_is_default();
            // Empty on a DOS or Other machine, neither of which has any
            // Direct3D to place.
            video = index_of(f.video_choices(), f.video());
            video_applies = f.video_applies();
            video_labels = labels(f.video_choices().iter().map(|v| v.label()));
            video_is_default = f.video_is_default();
            video_note = qs(f.video_notes().join("\n"));
            video_warning = qs_opt(f.video_warning());
            d3d9 = index_of(f.d3d9_choices(), f.d3d9());
            d3d9_applies = f.d3d9_applies();
            d3d9_labels = labels(f.d3d9_choices().iter().map(|d| d.label()));
            d3d9_is_default = f.d3d9_is_default();
            let n = f.d3d9_note();
            d3d9_note = qs(n.text);
            d3d9_warning = n.warning;
            sound = index_of(f.sound_choices(), f.sound());
            sound_labels = labels(f.sound_choices().iter().map(|c| c.label()));
            sound_is_default = f.sound_is_default();
            sound_note = qs(f.sound_notes().join("\n"));
            sound_warning = qs_opt(f.sound_warning());
            music = index_of(f.music_choices(), f.music());
            music_labels = labels(f.music_choices().iter().map(|m| m.label()));
            music_is_default = f.music_is_default();
            music_note = qs(f.music_notes().join("\n"));
            soundfont = qs(&f.soundfont);
            soundfont_applies = f.soundfont_applies();
            cpu_speed_applies = f.cpu_speed_applies();
            voodoo2_applies = f.voodoo2_applies();
            floppy_applies = f.floppy_applies();
            music_applies = f.music_applies();
            boot_applies = f.boot_applies();
            mt32_roms = qs(&f.mt32_roms);
            mt32_roms_applies = f.mt32_roms_applies();
            pad = index_of(f.pad_choices(), f.pad());
            pad_applies = f.pad_applies();
            pad_labels = labels(f.pad_choices().iter().map(|p| p.label()));
            pad_is_default = f.pad_is_default();
            pad_note = qs(f.pad_notes().join("\n"));
            pad_warning = qs_opt(f.pad_warning());
            network = f.network();
            network_note = qs(f.network_notes().join("\n"));
            seamless_mouse = f.seamless_mouse();
            seamless_mouse_note = qs(f.seamless_mouse_notes().join("\n"));
            voodoo2 = f.voodoo2();
            voodoo2_note = qs(f.voodoo2_notes().join("\n"));
            voodoo2_undither = f.voodoo2_undither();
            voodoo2_undither_enabled = f.voodoo2_undither_enabled();
            voodoo2_undither_note = qs(f.voodoo2_undither_notes().join("\n"));
            extra_qemu_args = qs(&f.extra_qemu_args);
            let args_note = f.extra_qemu_args_note();
            extra_qemu_args_note = qs(args_note.text);
            extra_qemu_args_warning = args_note.warning;
            optimizations_mask = Optimization::ALL
                .iter()
                .enumerate()
                .filter(|(_, opt)| f.optimization_enabled(**opt))
                .fold(0i32, |mask, (bit, _)| mask | (1 << bit));
            optimizations_summary = qs(f.optimizations_summary());
            optimizations_note = qs(f.optimizations_note());
            optimizations_are_default = f.optimizations_are_default();
            optimizations_all_off = f.optimizations_all_off();
            optimizations_all_on = f.optimizations_all_on();
            existing_disk = f.existing_disk;
            disk_path = qs(&f.disk_path);
            disk_size_gb = f.disk_size_gb as i32;
            install_media = qs(&f.install_media);
            floppy = qs(&f.floppy);
            boot = index_of(&Boot::ALL, f.boot);
            boot_note = qs_opt(f.boot_note());
            shader_profile_labels = f.shader_profile_labels(profiles)
                .into_iter()
                .fold(QStringList::default(), |mut l, s| { l.append(qs(s)); l });
            shader_profile_index = f.shader_profile_index(profiles) as i32;
            shader_profile_is_default = f.shader_profile_is_default();
            shader_profile = f.shader_profile.as_deref().map(qs).unwrap_or_default();
            error = qs_opt(f.error.as_deref());
        }
        self.as_mut().set_editing(editing);
        self.as_mut().set_title(title);
        self.as_mut().set_bundle_path(bundle_path);
        self.as_mut().set_section(section);
        self.as_mut().set_family(family);
        self.as_mut().set_family_note(family_note);
        self.as_mut().set_name(name);
        // The range first, then the value it has to fit in.
        self.as_mut().set_ram_min(ram_min);
        self.as_mut().set_ram_max(ram_max);
        self.as_mut().set_ram_mb(ram_mb);
        self.as_mut().set_ram_note(ram_note);
        self.as_mut().set_ram_is_default(ram_is_default);
        self.as_mut().set_cpu_speed(cpu_speed);
        self.as_mut().set_cpu_note(cpu_note);
        self.as_mut().set_cpu_is_default(cpu_is_default);
        self.as_mut().set_accel(accel);
        self.as_mut().set_accel_note(accel_note);
        self.as_mut().set_accel_warning(accel_warning);
        self.as_mut().set_accel_is_default(accel_is_default);
        self.as_mut().set_video_applies(video_applies);
        // The list before the index into it, like the memory range
        // before the value that has to fit in it.
        self.as_mut().set_video_labels(video_labels);
        self.as_mut().set_video(video);
        self.as_mut().set_video_is_default(video_is_default);
        self.as_mut().set_video_note(video_note);
        self.as_mut().set_video_warning(video_warning);
        self.as_mut().set_d3d9_applies(d3d9_applies);
        self.as_mut().set_d3d9_labels(d3d9_labels);
        self.as_mut().set_d3d9(d3d9);
        self.as_mut().set_d3d9_is_default(d3d9_is_default);
        self.as_mut().set_d3d9_note(d3d9_note);
        self.as_mut().set_d3d9_warning(d3d9_warning);
        // The lists before the indices into them, as everywhere else.
        self.as_mut().set_sound_labels(sound_labels);
        self.as_mut().set_sound(sound);
        self.as_mut().set_sound_is_default(sound_is_default);
        self.as_mut().set_sound_note(sound_note);
        self.as_mut().set_sound_warning(sound_warning);
        self.as_mut().set_music_labels(music_labels);
        self.as_mut().set_music(music);
        self.as_mut().set_music_is_default(music_is_default);
        self.as_mut().set_music_note(music_note);
        self.as_mut().set_soundfont(soundfont);
        self.as_mut().set_soundfont_applies(soundfont_applies);
        self.as_mut().set_cpu_speed_applies(cpu_speed_applies);
        self.as_mut().set_voodoo2_applies(voodoo2_applies);
        self.as_mut().set_floppy_applies(floppy_applies);
        self.as_mut().set_music_applies(music_applies);
        self.as_mut().set_boot_applies(boot_applies);
        self.as_mut().set_mt32_roms(mt32_roms);
        self.as_mut().set_mt32_roms_applies(mt32_roms_applies);
        self.as_mut().set_pad_applies(pad_applies);
        // The list before the index, for the same reason as the adapter's.
        self.as_mut().set_pad_labels(pad_labels);
        self.as_mut().set_pad(pad);
        self.as_mut().set_pad_is_default(pad_is_default);
        self.as_mut().set_pad_note(pad_note);
        self.as_mut().set_pad_warning(pad_warning);
        self.as_mut().set_network(network);
        self.as_mut().set_network_note(network_note);
        self.as_mut().set_seamless_mouse(seamless_mouse);
        self.as_mut().set_seamless_mouse_note(seamless_mouse_note);
        self.as_mut().set_voodoo2(voodoo2);
        self.as_mut().set_voodoo2_note(voodoo2_note);
        self.as_mut().set_voodoo2_undither(voodoo2_undither);
        self.as_mut().set_voodoo2_undither_enabled(voodoo2_undither_enabled);
        self.as_mut().set_voodoo2_undither_note(voodoo2_undither_note);
        self.as_mut().set_extra_qemu_args(extra_qemu_args);
        self.as_mut().set_extra_qemu_args_note(extra_qemu_args_note);
        self.as_mut().set_extra_qemu_args_warning(extra_qemu_args_warning);
        self.as_mut().set_optimizations_mask(optimizations_mask);
        self.as_mut().set_optimizations_summary(optimizations_summary);
        self.as_mut().set_optimizations_note(optimizations_note);
        self.as_mut().set_optimizations_are_default(optimizations_are_default);
        self.as_mut().set_optimizations_all_off(optimizations_all_off);
        self.as_mut().set_optimizations_all_on(optimizations_all_on);
        self.as_mut().set_existing_disk(existing_disk);
        self.as_mut().set_disk_path(disk_path);
        self.as_mut().set_disk_size_gb(disk_size_gb);
        self.as_mut().set_install_media(install_media);
        self.as_mut().set_floppy(floppy);
        self.as_mut().set_boot(boot);
        self.as_mut().set_boot_note(boot_note);
        self.as_mut().set_shader_profile_labels(shader_profile_labels);
        self.as_mut().set_shader_profile_index(shader_profile_index);
        self.as_mut().set_shader_profile_is_default(shader_profile_is_default);
        self.as_mut().set_shader_profile(shader_profile);
        self.as_mut().set_error(error);
        self.as_mut().set_open(open);
    }
}

/// Publish once at construction, so the window QML builds at start-up
/// binds to a form that means something instead of to the zeroes a
/// `#[derive(Default)]` leaves behind. Same reason as
/// `ShaderEditor`'s: a retained-mode property read before any verb has
/// run is read at its default, and here that default was a memory range
/// of 0..0 for the spin box to clamp against.
impl cxx_qt::Initialize for ffi::Wizard {
    fn initialize(self: Pin<&mut Self>) {
        self.publish();
    }
}
