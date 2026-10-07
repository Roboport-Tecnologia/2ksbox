//! "Shader profiles…": the profile list (`shader_library`) and the editor
//! (`launcher_core::editor::Editor`) with its live preview, which is the
//! core's own render path (`launcher_core::preview`, the player's integer
//! scale and letterbox on a windowless wgpu device). Its frame reaches the
//! window as pixels in an `Image`, with no file on disk in between.
//!
//! The preview renders at the size of the area it sits in (`use_size`),
//! again when that size, the parameters or the picture changed, and, for
//! an animated shader, when its next frame is due.

use crate::machines::Library;
use crate::path_field::PathField;
use launcher_core::editor::{Editor, IMAGE_FILTER, PRESET_FILTER, PREVIEW_SCALE_MAX, PresetState, Presets};
use launcher_core::preview::Preview;
use launcher_core::shader_library::{self, ProfileEntry};
use mitsuami::core::{CurrentWindow, Ui};
use mitsuami::prelude::*;
use std::cell::{Cell, RefCell};
use std::mem::ManuallyDrop;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::Duration;

const LABEL_W: f32 = 150.0;

/// What the preview shows: the frame and its size in points.
#[derive(Clone, PartialEq)]
struct Frame {
    width: f32,
    height: f32,
    pixels: Pixels,
}

#[derive(Clone, Copy)]
pub struct Shaders {
    /// Whether the profile list is up.
    open: Signal<bool>,
    profiles: Signal<Vec<ProfileEntry>>,
    editor: Signal<Editor>,
    presets: Signal<Presets>,
    /// The preview's device, made on the first render and dropped when
    /// the editor closes. Never set: a `Copy` handle, as `Library`'s.
    ///
    /// `ManuallyDrop`, so a device still alive when the process ends is
    /// never dropped: the reactive runtime is torn down with the main
    /// thread's thread-locals, and wgpu's queue, dropped then, reached a
    /// wgpu thread-local already gone and aborted the process.
    preview: Signal<Rc<RefCell<Option<ManuallyDrop<Preview>>>>>,
    frame: Signal<Option<Frame>>,
    preview_error: Signal<Option<String>>,
    /// Something the preview depends on changed since it last rendered.
    stale: Signal<bool>,
}

impl Store for Shaders {
    fn create() -> Shaders {
        Shaders {
            open: signal(false),
            profiles: signal(Vec::new()),
            editor: signal(Editor::default()),
            presets: signal(Presets::default()),
            preview: signal(Rc::new(RefCell::new(None))),
            frame: signal(None),
            preview_error: signal(None),
            stale: signal(true),
        }
    }
}

impl Shaders {
    fn dir() -> PathBuf {
        shader_library::default_dir()
    }

    fn refresh(&self) {
        self.profiles.set(shader_library::scan(&Self::dir()));
    }

    /// A preset collection landed (the first-run download): look for it
    /// again, and for the starter profiles written with it.
    pub fn presets_changed(&self) {
        self.presets.update(Presets::forget);
        self.refresh();
    }

    pub fn open_list(&self) {
        self.refresh();
        self.presets.update(Presets::forget);
        self.open.set(true);
    }

    /// Close the list; the machine list's Shader column reads the library.
    fn close_list(&self, library: Library) {
        self.open.set(false);
        library.refresh();
    }

    fn edit(&self, f: impl FnOnce(&mut Editor)) {
        self.editor.update(f);
        self.stale.set(true);
    }

    pub fn open_editor(&self, f: impl FnOnce(&mut Editor)) {
        self.frame.set(None);
        self.preview_error.set(None);
        self.edit(f);
    }

    /// Close the editor and drop the preview's device with it.
    fn close_editor(&self) {
        self.editor.update(|e| e.open = false);
        self.frame.set(None);
        if let Some(preview) = self.preview.get_untracked().borrow_mut().take() {
            drop(ManuallyDrop::into_inner(preview));
        }
    }

    fn read<R>(&self, f: impl FnOnce(&Editor) -> R) -> R {
        self.editor.with(f)
    }

    fn preset_state(&self) -> PresetState {
        let mut state = None;
        self.presets.update(|p| state = Some(p.state()));
        state.expect("set by the update")
    }

    /// Start the preset download, and poll it until it ends: the row
    /// shows its megabytes, then the collection.
    fn download(&self) {
        self.presets.update(Presets::start_download);
        let shaders = *self;
        spawn_local(async move {
            loop {
                sleep(Duration::from_millis(300)).await;
                let running = matches!(shaders.preset_state(), PresetState::Downloading(_));
                if !running {
                    break;
                }
            }
        });
    }

    /// Render the preview into `(w, h)` physical pixels, shown at
    /// `scale` pixels to the point, if the editor has a preset, its
    /// parameters and a picture.
    fn render(&self, w: u32, h: u32, scale: f32) {
        let job = self.editor.with_untracked(|e| {
            e.renderable().then(|| {
                let (preset, image) = (PathBuf::from(e.preset_path.trim()), PathBuf::from(e.preview_image_path.trim()));
                (preset, e.effective(), image, e.preview_scale())
            })
        });
        let Some((preset, params, image, fixed)) = job else { return };
        let preview = self.preview.get_untracked();
        let mut preview = preview.borrow_mut();
        if preview.is_none() {
            match Preview::headless() {
                Ok(p) => *preview = Some(ManuallyDrop::new(p)),
                Err(e) => {
                    self.preview_error.set(Some(e));
                    return;
                }
            }
        }
        let preview = preview.as_mut().expect("made above");
        preview.set_scale(fixed);
        preview.update(&preset, &params, &image, w.max(1), h.max(1));
        if let Some(e) = preview.error() {
            self.preview_error.set(Some(e.to_owned()));
            return;
        }
        let Some((fw, fh, rgb)) = preview.read_frame() else {
            self.preview_error.set(Some("no frame rendered".to_owned()));
            return;
        };
        // A picture bigger than the area renders at scale 1, bigger than
        // the area: show its centre and cut what overflows, as the player
        // does. Never scaled down.
        let (vw, vh) = preview.viewport();
        let (cw, ch) = (vw.min(w.max(1)).min(fw), vh.min(h.max(1)).min(fh));
        let (x0, y0) = ((fw - cw) / 2, (fh - ch) / 2);
        let mut rgba = Vec::with_capacity((cw * ch * 4) as usize);
        for row in rgb.chunks_exact(fw as usize * 3).skip(y0 as usize).take(ch as usize) {
            let row = &row[x0 as usize * 3..(x0 + cw) as usize * 3];
            rgba.extend(row.chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]));
        }
        self.preview_error.set(None);
        let pixels = Pixels::new(cw, ch, rgba).scale(scale);
        self.frame.set(Some(Frame { width: cw as f32 / scale, height: ch as f32 / scale, pixels }));
    }

    fn frame_interval(&self) -> Option<Duration> {
        self.preview.get_untracked().borrow().as_ref().and_then(|p| p.frame_interval())
    }
}

/// Whether a preset collection is installed, and the download when not.
#[component]
fn PresetCollection() -> impl View {
    let shaders = use_store::<Shaders>();
    let state = move || match shaders.preset_state() {
        PresetState::Ready(_) => (0, String::new(), String::new()),
        PresetState::Missing { install_dir, size } => (1, size.to_owned(), install_dir.display().to_string()),
        PresetState::Downloading(mb) => (2, format!("{mb:.1}"), String::new()),
        PresetState::Failed(e) => (3, e, String::new()),
    };
    let state = Rc::new(state);
    let (s1, s2, s3, s4, s5, s6) = (state.clone(), state.clone(), state.clone(), state.clone(), state.clone(), state);
    view! {
        <Column gap=Spacing::Xs shrink=0.0>
            <Show when=move || s1().0 == 2>
                <Row gap=Spacing::Sm align=Align::Center>
                    <Spinner label="Downloading"/>
                    <Text>{let s = s2.clone(); move || format!("Downloading shader presets… {} MB", s().1)}</Text>
                </Row>
            </Show>
            <Show when={let s = s3.clone(); move || s().0 == 3}>
                <Text color=Color::Error>
                    {let s = s3.clone(); move || format!("Couldn't download the shader presets: {}", s().1)}
                </Text>
            </Show>
            <Show when={let s = s4.clone(); move || s().0 == 1}>
                <Text text_style=TextStyle::Caption>
                    "No shader presets installed. A profile is built on a .slangp preset."
                </Text>
            </Show>
            <Show when={let s = s5.clone(); move || matches!(s().0, 1 | 3)}>
                <Row>
                    <Button @click=move || shaders.download()>
                        {let s = s5.clone(); move || {
                            let (kind, size, _) = s();
                            if kind == 3 { "Try again".to_owned() } else { format!("Download presets ({size})") }
                        }}
                    </Button>
                </Row>
            </Show>
            <Show when={let s = s6.clone(); move || s().0 == 1}>
                <Text text_style=TextStyle::Caption>
                    {let s = s6.clone(); move || format!("libretro's slang-shaders, installed into {}", s().2)}
                </Text>
            </Show>
        </Column>
    }
}

#[component]
pub fn ShaderProfilesWindow() -> impl View {
    let shaders = use_store::<Shaders>();
    let library = use_store::<Library>();
    view! {
        <Window
            title="Shader profiles"
            size=Size::new(720.0, 340.0)
            min_size=Size::new(480.0, 240.0)
            modal=Modality::Application
            open=shaders.open
            @close_request=move || shaders.close_list(library)
        >
            <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
                {crate::shot::arm(&["profiles", "saveprofile"])}
                <Toolbar>
                    <Button icon=crate::machines::icons::NEW @click=move || shaders.open_editor(Editor::new_profile)>
                        "New"
                    </Button>
                    <Button
                        icon=crate::machines::icons::CLEAR
                        icon_only=true
                        tooltip="No default"
                        enabled=move || shaders.profiles.with(|p| p.iter().any(|e| e.is_default))
                        @click=move || {
                            if let Err(e) = shader_library::set_default(&Shaders::dir(), None) {
                                eprintln!("[shader-manager] clearing the default profile: {e}");
                            }
                            shaders.refresh();
                        }
                    >"No default"</Button>
                </Toolbar>
                <Show
                    when=move || shaders.profiles.with(Vec::is_empty)
                    fallback=|| view! { <ProfileTable/> }
                >
                    <Column grow=1.0 align=Align::Center justify=Justify::Center>
                        <Text>"No shader profiles yet."</Text>
                    </Column>
                </Show>
                <PresetCollection/>
            </Column>
        </Window>
    }
}

/// The profiles under column headers: name, preset, a switch for the
/// default, and what can be done to it. Rows are keyed by the profile's
/// file and read their fields by it, so an edit shows in place. Activating
/// a row (double-click, Return) opens it in the editor; the trash button
/// asks before it deletes.
#[component]
fn ProfileTable() -> impl View {
    let shaders = use_store::<Shaders>();
    let ui = inject::<Ui>().expect("a window's component");
    let window = inject::<CurrentWindow>().map(|CurrentWindow(w)| w);
    let field = move |path: &PathBuf, f: fn(&ProfileEntry) -> String| {
        let path = path.clone();
        move || shaders.profiles.with(|p| p.iter().find(|e| e.path == path).map(f).unwrap_or_default())
    };
    let is_default = move |path: &PathBuf| {
        let path = path.clone();
        move || shaders.profiles.with(|p| p.iter().any(|e| e.path == path && e.is_default))
    };
    let columns = vec![
        TableColumn::new("Name", move |path: PathBuf| {
            // In from the frame by about what the actions column leaves
            // after the trash button (user).
            let name = field(&path, |e| e.profile.name.clone());
            view! {
                <Row padding_start=Spacing::Sm min_width=0>
                    <Text max_lines=1>{name}</Text>
                </Row>
            }
        })
        .width(170),
        TableColumn::new("Preset", move |path: PathBuf| {
            Text::new(field(&path, |e| e.profile.preset.display().to_string()))
                .text_style(TextStyle::Caption)
                .color(Color::SecondaryLabel)
                .max_lines(1)
                .truncation(Truncation::Start)
                .grow(1.0)
                .shrink(1.0)
                .basis(0)
        })
        .expand(),
        // On makes this profile the default (the library has one, so the
        // others go off); off leaves none. On macOS the small switch, as
        // dense lists there have (user).
        TableColumn::new("Default", move |path: PathBuf| {
            let id = shader_library::id_of(&path);
            let small = platform! {
                macos => mitsuami::appkit::tweak(|s: &mitsuami::appkit::objc2_app_kit::NSSwitch| {
                    s.setControlSize(mitsuami::appkit::objc2_app_kit::NSControlSize::Small)
                }),
                _ => Tweak::none(),
            };
            Switch::new("Default").checked(is_default(&path)).native(small).on_change(move |on| {
                let chosen = on.then_some(id.as_str());
                if let Err(e) = shader_library::set_default(&Shaders::dir(), chosen) {
                    eprintln!("[shader-manager] setting the default profile to {chosen:?}: {e}");
                }
                shaders.refresh();
            })
        })
        .width(80),
        TableColumn::new("", move |path: PathBuf| {
            let path = Rc::new(path);
            let (p2, p3) = (path.clone(), path);
            let ui = ui.clone();
            view! {
                <Row gap=Spacing::Sm padding_y=Spacing::Xs align=Align::Center>
                    <Button @click=move || {
                        let path = p2.to_path_buf();
                        shaders.open_editor(|e| e.edit_path(path));
                    }>"Edit"</Button>
                    <Button icon=crate::machines::icons::TRASH icon_only=true tooltip="Delete" @click=move || {
                        let path = p3.to_path_buf();
                        let name = shaders.profiles.with_untracked(|p| {
                            p.iter().find(|e| e.path == path).map(|e| e.profile.name.clone()).unwrap_or_default()
                        });
                        let (headline, detail) = shader_library::delete_question(&name);
                        // Cancel first: the first is the default, and Return
                        // should not delete.
                        let alert = Alert::new(headline)
                            .message(detail)
                            .style(AlertStyle::Critical)
                            .button("Cancel")
                            .button("Delete");
                        let asker = ui.clone();
                        ui.spawn_local(async move {
                            if asker.alert(window, alert).await != 1 {
                                return;
                            }
                            if let Err(e) = shader_library::delete(&path) {
                                eprintln!("[shader-manager] deleting {}: {e}", path.display());
                            }
                            shaders.refresh();
                        });
                    }>"Delete"</Button>
                </Row>
            }
        })
        .width(124),
    ];
    view! {
        <Table
            each=move || shaders.profiles.with(|p| p.iter().map(|e| e.path.clone()).collect::<Vec<_>>())
            key=|p: &PathBuf| p.clone()
            columns=columns
            list_style=ListStyle::Framed
            @activate=move |path: PathBuf| shaders.open_editor(|e| e.edit_path(path))
            grow=1.0
            min_height=0
        />
    }
}

#[component]
pub fn ShaderEditorWindow() -> impl View {
    let shaders = use_store::<Shaders>();
    let save = move || {
        let mut saved = false;
        shaders.editor.update(|e| saved = e.save(&Shaders::dir()));
        if saved {
            shaders.close_editor();
            shaders.refresh();
        }
    };
    view! {
        <Window
            title="Shader profile"
            size=Size::new(1200.0, 780.0)
            min_size=Size::new(720.0, 420.0)
            modal=Modality::Application
            open=move || shaders.read(|e| e.open)
            @close_request=move || shaders.close_editor()
        >
            <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
                {crate::shot::arm(&["editor"])}
                <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
                    <Text width=LABEL_W shrink=0.0>"Name"</Text>
                    <TextInput
                        grow=1.0
                        a11y_label="Name"
                        value=move || shaders.read(|e| e.name.clone())
                        @input=move |s| shaders.editor.update(|e| e.name = s)
                    />
                </Row>
                <PathField
                    label_width=LABEL_W
                    label="Preset (.slangp)"
                    filter=PRESET_FILTER
                    // Presets live where nobody would navigate by hand.
                    empty_dir=move || match shaders.preset_state() {
                        PresetState::Ready(dir) => Some(dir),
                        _ => None,
                    }
                    value=move || shaders.read(|e| e.preset_path.clone())
                    @edit=move |p| shaders.edit(|e| {
                        e.preset_path = p;
                        e.reparse();
                    })
                />
                <PresetCollection/>
                <Show when=move || shaders.read(|e| e.parse_error().is_some())>
                    <Text color=Color::Error>
                        {move || format!(
                            "Couldn't read this preset's parameters: {}",
                            shaders.read(|e| e.parse_error().unwrap_or_default().to_owned()),
                        )}
                    </Text>
                </Show>
                <Row gap=Spacing::Md grow=1.0 min_height=0>
                    <Column width=320 shrink=0.0 gap=Spacing::Xs min_height=0>
                        <Show
                            when=move || shaders.read(|e| e.params().is_empty())
                            fallback=|| view! { <ParamList/> }
                        >
                            <Text text_style=TextStyle::Caption>"Pick a preset to see its parameters."</Text>
                        </Show>
                    </Column>
                    <Column grow=1.0 gap=Spacing::Sm min_height=0>
                        <PathField
                            label_width=LABEL_W
                            label="Preview image"
                            filter=IMAGE_FILTER
                            value=move || shaders.read(|e| e.preview_image_path.clone())
                            @edit=move |p| shaders.edit(|e| e.preview_image_path = p)
                        />
                        <Row gap=Spacing::Sm align=Align::Center>
                            <Text width=90 shrink=0.0>{move || shaders.read(Editor::preview_scale_label)}</Text>
                            <Slider
                                label="Preview scale"
                                grow=1.0
                                range_with=(0.0, PREVIEW_SCALE_MAX as f64)
                                step=1.0
                                value=move || shaders.read(|e| e.preview_scale as f64)
                                @change=move |v: f64| shaders.edit(|e| e.preview_scale = v.round() as u32)
                            />
                        </Row>
                        <PreviewArea/>
                    </Column>
                </Row>
                <Show when=move || shaders.read(|e| e.error.is_some())>
                    <Text color=Color::Error>{move || shaders.read(|e| e.error.clone().unwrap_or_default())}</Text>
                </Show>
                <Row gap=Spacing::Sm shrink=0.0>
                    <Button role=ButtonRole::Default @click=save>"Save"</Button>
                    <Button role=ButtonRole::Cancel @click=move || shaders.close_editor()>"Cancel"</Button>
                </Row>
            </Column>
        </Window>
    }
}

#[component]
fn ParamList() -> impl View {
    let shaders = use_store::<Shaders>();
    view! {
        <ScrollView grow=1.0 min_height=0>
            <Column gap=Spacing::Lg padding_x=Spacing::Xs>
                <For
                    each=move || shaders.read(|e| e.params().iter().map(|p| p.id.clone()).collect::<Vec<_>>())
                    key=|id: &String| id.clone()
                    let:id
                >
                    <ParamRow id=id/>
                </For>
            </Column>
        </ScrollView>
    }
}

/// One parameter: a box that overrides the preset's value, and the
/// slider under its name and value.
#[component]
fn ParamRow(id: String) -> impl View {
    let shaders = use_store::<Shaders>();
    let id = Rc::new(id);
    let row = {
        let id = id.clone();
        move || shaders.read(|e| e.params().iter().position(|p| p.id == *id))
    };
    let meta = {
        let row = row.clone();
        move |f: fn(&Editor, usize) -> f64| {
            let row = row.clone();
            move || shaders.read(|e| row().map(|r| f(e, r)).unwrap_or_default())
        }
    };
    let overridden = {
        let row = row.clone();
        move || shaders.read(|e| row().is_some_and(|r| e.is_overridden(r)))
    };
    let label = {
        let row = row.clone();
        move || shaders.read(|e| row().and_then(|r| e.label(r)).unwrap_or_default())
    };
    let value = meta(|e, r| e.value(r).unwrap_or_default() as f64);
    let (min, max) = (meta(|e, r| e.params()[r].minimum as f64), meta(|e, r| e.params()[r].maximum as f64));
    let step = meta(|e, r| e.params()[r].step as f64);
    let (row2, row3) = (row.clone(), row);
    let (o1, o2) = (overridden.clone(), overridden);
    view! {
        <Row gap=Spacing::Sm align=Align::Center>
            <Checkbox
                a11y_label=id.to_string()
                checked=o1
                @change=move |on| {
                    if let Some(r) = row2() {
                        shaders.edit(|e| e.set_override(r, on));
                    }
                }
            />
            <Column grow=1.0 gap=Spacing::Xs>
                <Text text_style=TextStyle::Caption max_lines=1>
                    {label}
                </Text>
                <Slider
                    label=id.to_string()
                    enabled=o2
                    range_with=move || (min(), max())
                    step=step
                    value=value
                    @change=move |v| {
                        if let Some(r) = row3() {
                            shaders.edit(|e| e.set_value(r, v as f32));
                        }
                    }
                />
            </Column>
        </Row>
    }
}

/// The preview: the frame, centred, at the size the core rendered it for
/// this area (the player's own scale and letterbox).
#[component]
fn PreviewArea() -> impl View {
    let shaders = use_store::<Shaders>();
    let area = node_ref();
    let size = use_size(area);
    let last_size = Cell::new((0, 0));
    // An animated shader's next frame: one wake-up at a time, which marks
    // the preview stale. On the `Ui` itself, as a task the effect spawned
    // would be cancelled by its next run (each run disposes the last's).
    let ui = inject::<Ui>().expect("a window's component");
    let waking = Rc::new(Cell::new(false));
    effect(move || {
        // Both read on every run, so the effect follows both.
        let stale = shaders.stale.get();
        // In physical pixels, as the player's surface is: the frame then
        // shows pixel for pixel, never stretched to the points (blurred on
        // a 2x screen).
        let scale = ui.metrics().scale_factor.max(1.0);
        let size = size.get();
        let size = ((size.width * scale).round() as u32, (size.height * scale).round() as u32);
        if size == (0, 0) || (size == last_size.get() && !stale) {
            return;
        }
        last_size.set(size);
        shaders.stale.set(false);
        shaders.render(size.0, size.1, scale);
        if let Some(interval) = shaders.frame_interval()
            && !waking.replace(true)
        {
            let (waking, wait) = (waking.clone(), ui.sleep(interval));
            ui.spawn_local(async move {
                wait.await;
                waking.set(false);
                shaders.stale.set(true);
            });
        }
    });
    let frame = move |f: fn(&Frame) -> f32| move || Length::from(shaders.frame.with(|x| x.as_ref().map_or(0.0, f)));
    let pixels = move || {
        let pixels = shaders.frame.with(|f| f.as_ref().map(|f| f.pixels.clone()));
        ImageSource::Pixels(pixels.unwrap_or_else(|| Pixels::new(1, 1, vec![0u8; 4])))
    };
    Column::new()
        .node_ref(area)
        .grow(1.0)
        .min_height(0)
        .min_width(0)
        .align(Align::Center)
        .justify(Justify::Center)
        .children(view! {
            <Show when=move || shaders.read(|e| e.preview_image_path.trim().is_empty())>
                <Text text_style=TextStyle::Caption color=Color::SecondaryLabel>"Pick a screenshot to preview the shader."</Text>
            </Show>
            <Show when=move || shaders.preview_error.get().is_some()>
                <Text color=Color::Error>{move || shaders.preview_error.get().unwrap_or_default()}</Text>
            </Show>
            <Show when=move || shaders.frame.with(Option::is_some)>
                <Image
                    source=pixels
                    label="Preview"
                    fit=ImageFit::Stretch
                    width=frame(|f| f.width)
                    height=frame(|f| f.height)
                    shrink=0.0
                />
            </Show>
        })
}

/// For the headless `saveprofile:<preset>` screen: a new profile named
/// "Probe profile" on a preset, saved as Save does, and the list opened.
pub fn save_probe(shaders: Shaders, preset: &str) {
    shaders.open_list();
    let preset = preset.to_owned();
    shaders.open_editor(|e| {
        e.new_profile();
        e.name = "Probe profile".to_owned();
        e.preset_path = preset;
        e.reparse();
    });
    let mut saved = false;
    shaders.editor.update(|e| saved = e.save(&Shaders::dir()));
    let error = shaders.editor.with_untracked(|e| e.error.clone());
    shaders.close_editor();
    shaders.refresh();
    eprintln!("[launcher] saveprofile: saved {saved}, error {error:?}, list {}", shaders.profiles.with_untracked(Vec::len));
}

/// For the headless `editor:<preset>[;<image>[;<param>=<value>]]`
/// screen: the editor on a preset and a picture, with one parameter overridden as its box and slider
/// would (the preview must render again for it), or `scale=<n>` the
/// preview's scale slider moved.
pub fn edit_preset(shaders: Shaders, arg: &str) {
    let mut parts = arg.split(';');
    let preset = parts.next().unwrap_or_default().to_owned();
    let image = parts.next().unwrap_or_default().to_owned();
    shaders.open_editor(|e| e.open_with(preset, image));
    if let Some((id, value)) = parts.next().and_then(|p| p.split_once('=')) {
        let value: f32 = value.parse().unwrap_or_default();
        if id == "scale" {
            shaders.edit(|e| e.preview_scale = value as u32);
            return;
        }
        let row = shaders.editor.with_untracked(|e| e.params().iter().position(|p| p.id == id));
        match row {
            Some(row) => shaders.edit(|e| {
                e.set_override(row, true);
                e.set_value(row, value);
            }),
            None => eprintln!("[launcher] editor: no parameter {id}"),
        }
    }
}
