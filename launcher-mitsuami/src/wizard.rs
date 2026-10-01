//! The machine form, "New machine" and "Edit…", over
//! `launcher_core::wizard::Form`: a settings window with a page per
//! section, as `WizardWindow.qml` draws it.
//!
//! The form is one signal. Every control reads it and every edit goes
//! through `Wizard::edit`, which is `Form`'s own method, so what a field
//! does to the ones under it (a family switch moving memory, the adapter,
//! the sound card) is the core's, as ADR-014 has it. Every label, list
//! and sentence comes from the core too.

use crate::machines::Library;
use crate::path_field::PathField;
use launcher_core::bundle::{Accel, Boot, CpuSpeed, Family, Optimization};
use launcher_core::shader_library::{self, ProfileEntry};
use launcher_core::wizard::{
    AccelNote, DISK_FILTER, FLOPPY_FILTER, Form, MEDIA_FILTER, SOUNDFONT_FILTER, Section,
};
use launcher_core::library;
use mitsuami::prelude::*;
use std::path::PathBuf;

/// The width of the labels in front of each control, so the controls on
/// a page line up.
const LABEL_W: f32 = 180.0;

#[derive(Clone, Copy)]
pub struct Wizard {
    form: Signal<Form>,
    profiles: Signal<Vec<ProfileEntry>>,
    /// The machine the window last showed and the page it was on, so
    /// reopening the same machine puts it back on that page (the core
    /// leaves that to the front end).
    last: Signal<(Option<PathBuf>, Section)>,
}

impl Store for Wizard {
    fn create() -> Wizard {
        Wizard { form: signal(Form::default()), profiles: signal(Vec::new()), last: signal((None, Section::General)) }
    }
}

impl Wizard {
    fn read<R>(&self, f: impl FnOnce(&Form) -> R) -> R {
        self.form.with(f)
    }

    fn edit(&self, f: impl FnOnce(&mut Form)) {
        self.form.update(f);
    }

    fn scan_profiles(&self) {
        self.profiles.set(shader_library::scan(&shader_library::default_dir()));
    }

    pub fn open_fresh(&self) {
        self.scan_profiles();
        self.edit(Form::open_fresh);
    }

    pub fn open_edit(&self, bundle: PathBuf) {
        self.scan_profiles();
        let (last, section) = self.last.get_untracked();
        let again = last.as_ref() == Some(&bundle);
        self.edit(|f| {
            f.open_edit_path(bundle);
            if again {
                f.choose_section(section);
            }
        });
    }

    /// `LAUNCHER_SCREEN=wizard[:<family>[:<page>]]`: open a fresh form
    /// on a family (`win98`, `xp`, `dos`, `other`) and a page (its index
    /// in the sidebar), for the headless shot.
    pub fn open_for_screen(&self, arg: &str) {
        self.open_fresh();
        let mut parts = arg.split(':');
        let family = match parts.next() {
            Some("xp") => Some(Family::Xp),
            Some("dos") => Some(Family::Dos),
            Some("other") => Some(Family::Other),
            Some("win98") => Some(Family::Win98),
            _ => None,
        };
        let section = parts.next().and_then(|p| p.parse::<usize>().ok()).and_then(|i| Section::ALL.get(i).copied());
        self.edit(|f| {
            if let Some(family) = family {
                f.choose_family(family);
            }
            if let Some(section) = section {
                f.choose_section(section);
            }
        });
    }

    /// `LAUNCHER_SCREEN=edit:<machine.toml>[:<page>]`: the form on a
    /// machine, as its Edit… button opens it.
    pub fn edit_for_screen(&self, arg: &str) {
        let (bundle, page) = match arg.rsplit_once(':') {
            Some((bundle, page)) if page.parse::<usize>().is_ok() => (bundle, page.parse().ok()),
            _ => (arg, None),
        };
        self.open_edit(PathBuf::from(bundle));
        if let Some(section) = page.and_then(|i: usize| Section::ALL.get(i).copied()) {
            self.edit(|f| f.choose_section(section));
        }
    }

    /// `LAUNCHER_SCREEN=create:<family>:<name>`: fill a fresh form on an
    /// existing (empty) disk and submit it, as Create does, for a check
    /// that the machine lands in the library.
    pub fn create_for_screen(&self, arg: &str, library: Library) {
        let (family, name) = arg.split_once(':').unwrap_or(("xp", arg));
        self.open_for_screen(family);
        self.edit(|f| {
            f.name = name.to_owned();
            f.existing_disk = true;
            f.disk_path = "/dev/null".to_owned();
        });
        self.submit(library);
        let (saved, error) = self.read(|f| (f.saved_path().map(|p| p.display().to_string()), f.error.clone()));
        eprintln!("[launcher] create: saved {saved:?}, error {error:?}, open {}", self.read(|f| f.open));
    }

    fn close(&self) {
        let last = self.read(|f| (f.bundle_path().map(PathBuf::from), f.section));
        self.last.set(last);
        self.edit(|f| f.open = false);
    }

    fn is_open(&self) -> bool {
        self.read(|f| f.open)
    }

    fn submit(&self, library: Library) {
        let mut saved = false;
        self.edit(|f| saved = f.submit(&library::default_dir()).is_some());
        if saved {
            library.refresh();
            self.close();
        }
    }
}

/// A read of the form, as a closure a view binds to.
fn get<R: 'static>(wiz: Wizard, f: impl Fn(&Form) -> R + 'static) -> impl Fn() -> R + 'static {
    move || wiz.read(&f)
}

fn labels<T: Copy>(all: &[T], label: impl Fn(T) -> &'static str) -> Vec<String> {
    all.iter().map(|x| label(*x).to_owned()).collect()
}

fn index_of<T: PartialEq>(all: &[T], value: T) -> usize {
    all.iter().position(|v| *v == value).unwrap_or(0)
}

fn joined(notes: &[&str]) -> String {
    notes.join("\n")
}

#[component]
pub fn WizardWindow() -> impl View {
    let wiz = use_store::<Wizard>();
    let library = use_store::<Library>();
    view! {
        <Window
            title=get(wiz, |f| f.title().to_owned())
            size=Size::new(820.0, 440.0)
            min_size=Size::new(640.0, 400.0)
            modal=Modality::Application
            open=move || wiz.is_open()
            @close_request=move || wiz.close()
        >
            // `min_height=0` on the column and the row: a flex item is at
            // least as tall as its content unless told otherwise (as in
            // CSS), so without them a long page grew the window's content
            // past the window and pushed the buttons out, instead of the
            // scroll view taking only the room left.
            <Column padding=Spacing::Lg gap=Spacing::Md grow=1.0 min_height=0>
                {crate::shot::arm(&["wizard", "edit"])}
                <Row gap=Spacing::Md grow=1.0 min_height=0>
                    <Sections/>
                    <ScrollView grow=1.0>
                        <Column gap=Spacing::Md padding_x=Spacing::Sm>
                            <Show when=on(wiz, Section::General)><GeneralPage/></Show>
                            <Show when=on(wiz, Section::System)><SystemPage/></Show>
                            <Show when=on(wiz, Section::Display)><DisplayPage/></Show>
                            <Show when=on(wiz, Section::Audio)><AudioPage/></Show>
                            <Show when=on(wiz, Section::Input)><InputPage/></Show>
                            <Show when=on(wiz, Section::Network)><NetworkPage/></Show>
                            <Show when=on(wiz, Section::Storage)><StoragePage/></Show>
                        </Column>
                    </ScrollView>
                </Row>
                <Show when=get(wiz, |f| f.error.is_some())>
                    <Text text_style=TextStyle::Callout>{get(wiz, |f| f.error.clone().unwrap_or_default())}</Text>
                </Show>
                <Row gap=Spacing::Sm justify=Justify::End shrink=0.0>
                    <Button role=ButtonRole::Cancel @click=move || wiz.close()>"Cancel"</Button>
                    <Button role=ButtonRole::Default @click=move || wiz.submit(library)>
                        {get(wiz, |f| if f.is_editing() { "Save" } else { "Create" }.to_owned())}
                    </Button>
                </Row>
            </Column>
        </Window>
    }
}

/// The sidebar: the form's pages, in the form's order.
#[component]
fn Sections() -> impl View {
    let wiz = use_store::<Wizard>();
    let selected = signal(vec![0usize]);
    // The page follows the form (every open starts on the first) and the
    // form follows a click; each side only writes when they differ.
    effect(move || {
        let at = index_of(&Section::ALL, wiz.read(|f| f.section));
        if selected.get_untracked() != [at] {
            selected.set(vec![at]);
        }
    });
    effect(move || {
        if let Some(&at) = selected.get().first() {
            let section = Section::ALL[at];
            if wiz.form.with_untracked(|f| f.section) != section {
                wiz.edit(|f| f.choose_section(section));
            }
        }
    });
    view! {
        <List
            each=|| (0..Section::ALL.len()).collect::<Vec<_>>()
            key=|i: &usize| *i
            selected=selected
            width=150
            min_width=150
            max_width=150
            let:i
        >
            <Row padding_x=Spacing::Md padding_y=Spacing::Sm>
                <Text>{Section::ALL[i].label()}</Text>
            </Row>
        </List>
    }
}

/// Whether a page is the one on show.
fn on(wiz: Wizard, section: Section) -> impl Fn() -> bool + 'static {
    move || wiz.read(|f| f.section == section)
}

/// A sentence under a control, from the core.
#[component]
fn Note(text: Value<String>) -> impl View {
    view! {
        <Show when={let text = text.clone(); move || !text.get().is_empty()}>
            <Text text_style=TextStyle::Caption>{text.clone()}</Text>
        </Show>
    }
}

/// A note that can be a warning (a machine that will refuse to start).
/// mitsuami has no text colour yet, so a warning is set in the stronger
/// style instead of the Qt window's amber.
#[component]
fn AccelLine(note: Value<(String, bool)>) -> impl View {
    let (n1, n2, n3) = (note.clone(), note.clone(), note);
    view! {
        <Show when=move || !n1.get().0.is_empty()>
            <Text text_style={let n2 = n2.clone(); move || if n2.get().1 { TextStyle::Callout } else { TextStyle::Caption }}>
                {let n3 = n3.clone(); move || n3.get().0}
            </Text>
        </Show>
    }
}

fn accel(note: AccelNote) -> (String, bool) {
    (note.text, note.warning)
}

/// A label, a picker over the core's list, and the "Default" button that
/// puts the family's own value back.
#[component]
fn Picker(
    #[prop(into)] label: String,
    options: Value<Vec<String>>,
    selected: Value<usize>,
    on_choose: Callback<usize>,
    /// Whether a "Default" button follows; the family and boot pickers
    /// have none.
    #[prop(default = true)] resettable: bool,
    #[prop(default = Value::Static(true))] is_default: Value<bool>,
    on_reset: Callback<()>,
) -> impl View {
    let reset = resettable.then(|| {
        view! {
            <Button enabled=move || !is_default.get() @click=move || on_reset.call(())>"Default"</Button>
        }
    });
    view! {
        <Row gap=Spacing::Sm align=Align::Center>
            <Text width=LABEL_W>{label.clone()}</Text>
            <Select label=label width=260 options=options selected=selected @change=move |i| on_choose.call(i)/>
            {reset}
        </Row>
    }
}

#[component]
fn GeneralPage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Picker
                label="Family"
                resettable=false
                options=labels(&Family::ALL, Family::label)
                selected=get(wiz, |f| index_of(&Family::ALL, f.family()))
                @choose=move |i| wiz.edit(|f| f.choose_family(Family::ALL[i]))
            />
            <Note text=get(wiz, |f| f.family_note().unwrap_or_default().to_owned())/>
            <Row gap=Spacing::Sm align=Align::Center>
                <Text width=LABEL_W>"Name"</Text>
                <TextInput grow=1.0 a11y_label="Name" value=get(wiz, |f| f.name.clone())
                    @input=move |s| wiz.edit(|f| f.name = s)/>
            </Row>
        </Column>
    }
}

#[component]
fn SystemPage() -> impl View {
    let wiz = use_store::<Wizard>();
    // Open from the start for a headless shot that ends in `:open`.
    let expanded = signal(std::env::var("LAUNCHER_SCREEN").is_ok_and(|s| s.ends_with(":open")));
    view! {
        <Column gap=Spacing::Md>
            <Row gap=Spacing::Sm align=Align::Center>
                <Text width=LABEL_W>"Memory (MB)"</Text>
                <NumberInput
                    label="Memory (MB)"
                    range_with=get(wiz, |f| {
                        let r = f.ram_range();
                        (*r.start() as i32, *r.end() as i32)
                    })
                    step=16
                    value=get(wiz, |f| f.ram_mb() as i32)
                    @change=move |mb| wiz.edit(|f| f.choose_ram_mb(mb.max(0) as u32))
                />
                <Button enabled=get(wiz, |f| !f.ram_is_default()) @click=move || wiz.edit(Form::reset_ram)>"Default"</Button>
            </Row>
            <Note text=get(wiz, |f| f.ram_note().unwrap_or_default().to_owned())/>
            <Show when=get(wiz, Form::cpu_speed_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Processor"
                        options=labels(&CpuSpeed::ALL, CpuSpeed::label)
                        selected=get(wiz, |f| index_of(&CpuSpeed::ALL, f.cpu_speed()))
                        @choose=move |i| wiz.edit(|f| f.choose_cpu_speed(CpuSpeed::ALL[i]))
                        is_default=get(wiz, Form::cpu_speed_is_default)
                        @reset=move |()| wiz.edit(Form::reset_cpu_speed)
                    />
                    <Note text=get(wiz, |f| joined(f.cpu_speed_notes()))/>
                </Column>
            </Show>
            <Picker
                label="Acceleration"
                options=labels(&Accel::ALL, Accel::label)
                selected=get(wiz, |f| index_of(&Accel::ALL, f.accel()))
                @choose=move |i| wiz.edit(|f| f.choose_accel(Accel::ALL[i]))
                is_default=get(wiz, Form::accel_is_default)
                @reset=move |()| wiz.edit(Form::reset_accel)
            />
            <AccelLine note=get(wiz, |f| accel(f.accel_note()))/>
            // A disclosure header: Qt Quick has none either, and the Qt window
            // builds its own from a tool button.
            <Row>
                <Button button_style=ButtonStyle::Borderless @click=move || expanded.update(|e| *e = !*e)>
                    {move || format!(
                        "{} Emulation optimizations ({})",
                        if expanded.get() { "▾" } else { "▸" },
                        wiz.read(Form::optimizations_summary),
                    )}
                </Button>
            </Row>
            <Show when=expanded>
                <Column gap=Spacing::Xs padding_x=Spacing::Lg>
                    <Note text=get(wiz, |f| f.optimizations_note().to_owned())/>
                    <For each=|| Optimization::ALL.to_vec() key=|o: &Optimization| o.label() let:opt>
                        <Column gap=Spacing::Xs>
                            <Checkbox
                                checked=get(wiz, move |f| f.optimization_enabled(opt))
                                @change=move |on| wiz.edit(|f| f.choose_optimization(opt, on))
                            >{opt.label()}</Checkbox>
                            <Text text_style=TextStyle::Caption>{opt.note()}</Text>
                        </Column>
                    </For>
                    <Row gap=Spacing::Sm>
                        <Button enabled=get(wiz, |f| !f.optimizations_all_off())
                            @click=move || wiz.edit(Form::disable_all_optimizations)>"Turn all off"</Button>
                        <Button enabled=get(wiz, |f| !f.optimizations_all_on())
                            @click=move || wiz.edit(Form::enable_all_optimizations)>"Turn all on"</Button>
                        <Button enabled=get(wiz, |f| !f.optimizations_are_default())
                            @click=move || wiz.edit(Form::reset_optimizations)>"All defaults"</Button>
                    </Row>
                </Column>
            </Show>
            <Row gap=Spacing::Sm align=Align::Center>
                <Text width=LABEL_W>"Extra QEMU arguments"</Text>
                <TextInput grow=1.0 a11y_label="Extra QEMU arguments"
                    placeholder="-global d3dpt-vga.ddflags=32768"
                    value=get(wiz, |f| f.extra_qemu_args.clone())
                    @input=move |s| wiz.edit(|f| f.extra_qemu_args = s)/>
            </Row>
            <AccelLine note=get(wiz, |f| accel(f.extra_qemu_args_note()))/>
        </Column>
    }
}

#[component]
fn DisplayPage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Show when=get(wiz, Form::video_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Display adapter"
                        options=get(wiz, |f| labels(f.video_choices(), |v| v.label()))
                        selected=get(wiz, |f| index_of(f.video_choices(), f.video()))
                        @choose=move |i| wiz.edit(|f| f.choose_video(f.video_choices()[i]))
                        is_default=get(wiz, Form::video_is_default)
                        @reset=move |()| wiz.edit(Form::reset_video)
                    />
                    <AccelLine note=get(wiz, |f| (f.video_warning().unwrap_or_default().to_owned(), true))/>
                    <Note text=get(wiz, |f| joined(f.video_notes()))/>
                </Column>
            </Show>
            <Show when=get(wiz, Form::d3d9_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Direct3D"
                        options=get(wiz, |f| labels(f.d3d9_choices(), |d| d.label()))
                        selected=get(wiz, |f| index_of(f.d3d9_choices(), f.d3d9()))
                        @choose=move |i| wiz.edit(|f| f.choose_d3d9(f.d3d9_choices()[i]))
                        is_default=get(wiz, Form::d3d9_is_default)
                        @reset=move |()| wiz.edit(Form::reset_d3d9)
                    />
                    <AccelLine note=get(wiz, |f| accel(f.d3d9_note()))/>
                </Column>
            </Show>
            <Show when=get(wiz, Form::voodoo2_applies)>
                <Column gap=Spacing::Md>
                    <Row gap=Spacing::Lg>
                        <Checkbox checked=get(wiz, Form::voodoo2) @change=move |on| wiz.edit(|f| f.choose_voodoo2(on))>
                            "3dfx Voodoo 2"
                        </Checkbox>
                        <Checkbox
                            enabled=get(wiz, Form::voodoo2_undither_enabled)
                            checked=get(wiz, Form::voodoo2_undither)
                            @change=move |on| wiz.edit(|f| f.choose_voodoo2_undither(on))
                        >"Voodoo3 undither filter"</Checkbox>
                    </Row>
                    <Note text=get(wiz, |f| joined(f.voodoo2_notes()))/>
                    <Note text=get(wiz, |f| joined(f.voodoo2_undither_notes()))/>
                </Column>
            </Show>
            <Picker
                label="Shader profile"
                options=move || wiz.profiles.with(|p| wiz.read(|f| f.shader_profile_labels(p)))
                selected=move || wiz.profiles.with(|p| wiz.read(|f| f.shader_profile_index(p)))
                @choose=move |i| wiz.profiles.with_untracked(|p| wiz.edit(|f| f.choose_shader_profile(p, i)))
                is_default=get(wiz, Form::shader_profile_is_default)
                @reset=move |()| wiz.edit(Form::reset_shader_profile)
            />
        </Column>
    }
}

#[component]
fn AudioPage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Picker
                label="Sound card"
                options=get(wiz, |f| labels(f.sound_choices(), |c| c.label()))
                selected=get(wiz, |f| index_of(f.sound_choices(), f.sound()))
                @choose=move |i| wiz.edit(|f| f.choose_sound(f.sound_choices()[i]))
                is_default=get(wiz, Form::sound_is_default)
                @reset=move |()| wiz.edit(Form::reset_sound)
            />
            <AccelLine note=get(wiz, |f| (f.sound_warning().unwrap_or_default().to_owned(), true))/>
            <Note text=get(wiz, |f| joined(f.sound_notes()))/>
            <Show when=get(wiz, Form::music_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Music (MIDI)"
                        options=get(wiz, |f| labels(f.music_choices(), |m| m.label()))
                        selected=get(wiz, |f| index_of(f.music_choices(), f.music()))
                        @choose=move |i| wiz.edit(|f| f.choose_music(f.music_choices()[i]))
                        is_default=get(wiz, Form::music_is_default)
                        @reset=move |()| wiz.edit(Form::reset_music)
                    />
                    <Note text=get(wiz, |f| joined(f.music_notes()))/>
                </Column>
            </Show>
            <Show when=get(wiz, Form::soundfont_applies)>
                <PathField
                label_width=LABEL_W
                    label_width=LABEL_W
                    label="SoundFont (optional)"
                    filter=SOUNDFONT_FILTER
                    value=get(wiz, |f| f.soundfont.clone())
                    @edit=move |p| wiz.edit(|f| f.soundfont = p)
                />
            </Show>
            <Show when=get(wiz, Form::mt32_roms_applies)>
                <PathField
                label_width=LABEL_W
                    label_width=LABEL_W
                    label="MT-32 ROMs"
                    folder=true
                    placeholder="Folder with your CM-32L control and PCM ROMs"
                    value=get(wiz, |f| f.mt32_roms.clone())
                    @edit=move |p| wiz.edit(|f| f.mt32_roms = p)
                />
            </Show>
        </Column>
    }
}

#[component]
fn InputPage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Show when=get(wiz, Form::pad_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Gamepad"
                        options=get(wiz, |f| labels(f.pad_choices(), |p| p.label()))
                        selected=get(wiz, |f| index_of(f.pad_choices(), f.pad()))
                        @choose=move |i| wiz.edit(|f| f.choose_pad(f.pad_choices()[i]))
                        is_default=get(wiz, Form::pad_is_default)
                        @reset=move |()| wiz.edit(Form::reset_pad)
                    />
                    <AccelLine note=get(wiz, |f| (f.pad_warning().unwrap_or_default().to_owned(), true))/>
                    <Note text=get(wiz, |f| joined(f.pad_notes()))/>
                </Column>
            </Show>
            <Checkbox checked=get(wiz, Form::seamless_mouse) @change=move |on| wiz.edit(|f| f.choose_seamless_mouse(on))>
                "Seamless mouse"
            </Checkbox>
            <Note text=get(wiz, |f| joined(f.seamless_mouse_notes()))/>
        </Column>
    }
}

#[component]
fn NetworkPage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Checkbox checked=get(wiz, Form::network) @change=move |on| wiz.edit(|f| f.choose_network(on))>
                "Networking"
            </Checkbox>
            <Note text=get(wiz, |f| joined(f.network_notes()))/>
        </Column>
    }
}

#[component]
fn StoragePage() -> impl View {
    let wiz = use_store::<Wizard>();
    view! {
        <Column gap=Spacing::Md>
            <Show when=move || !wiz.read(Form::is_editing)>
                <Checkbox checked=get(wiz, |f| f.existing_disk) @change=move |on| wiz.edit(|f| f.existing_disk = on)>
                    "Use an existing disk image"
                </Checkbox>
            </Show>
            <Show when=move || wiz.read(|f| f.is_editing() || f.existing_disk)>
                <PathField
                label_width=LABEL_W
                    label_width=LABEL_W
                    label="Disk path"
                    filter=DISK_FILTER
                    value=get(wiz, |f| f.disk_path.clone())
                    @edit=move |p| wiz.edit(|f| f.disk_path = p)
                />
            </Show>
            <Show when=move || wiz.read(|f| !f.is_editing() && !f.existing_disk)>
                <Row gap=Spacing::Sm align=Align::Center>
                    <Text width=LABEL_W>"New disk size (GB)"</Text>
                    <NumberInput
                        label="New disk size (GB)"
                        range_with=(1, 128)
                        value=get(wiz, |f| f.disk_size_gb as i32)
                        @change=move |gb| wiz.edit(|f| f.disk_size_gb = gb.max(1) as u32)
                    />
                </Row>
            </Show>
            <PathField
                label_width=LABEL_W
                label="Install media (optional)"
                filter=MEDIA_FILTER
                value=get(wiz, |f| f.install_media.clone())
                @edit=move |p| wiz.edit(|f| f.install_media = p)
            />
            <Show when=get(wiz, Form::floppy_applies)>
                <PathField
                    label_width=LABEL_W
                    label="Floppy (optional)"
                    filter=FLOPPY_FILTER
                    value=get(wiz, |f| f.floppy.clone())
                    @edit=move |p| wiz.edit(|f| f.floppy = p)
                />
            </Show>
            <Show when=get(wiz, Form::boot_applies)>
                <Column gap=Spacing::Md>
                    <Picker
                        label="Boot from"
                        resettable=false
                        options=labels(&Boot::ALL, Boot::label)
                        selected=get(wiz, |f| index_of(&Boot::ALL, f.boot))
                        @choose=move |i| wiz.edit(|f| f.boot = Boot::ALL[i])
                    />
                    <Note text=get(wiz, |f| f.boot_note().unwrap_or_default().to_owned())/>
                </Column>
            </Show>
        </Column>
    }
}
