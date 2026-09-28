//! "Shader profiles…": the profile list (`shader_library`) and the editor
//! (`launcher_core::editor::Editor`) with its live preview, which is the
//! core's own render path (`launcher_core::preview`, the player's integer
//! scale and letterbox on a windowless wgpu device). Its frame reaches the
//! window as pixels in an `Image`, where the Qt build goes through a BMP on
//! disk.
//!
//! The preview renders at the size of the area it sits in. mitsuami tells
//! no view its size, so the editor reads the area's frame (`Ui::frame`,
//! the node from `after_build`) on a short tick, and renders again when
//! the size, the parameters or the picture changed, or when an animated
//! shader's next frame is due.

use crate::machines::Library;
use launcher_core::browse::{self, Filter};
use launcher_core::editor::{Editor, IMAGE_FILTER, PRESET_FILTER, PresetState, Presets};
use launcher_core::preview::Preview;
use launcher_core::shader_library::{self, ProfileEntry};
use mitsuami::core::{NodeId, Ui};
use mitsuami::prelude::*;
use std::cell::RefCell;
use std::mem::ManuallyDrop;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::{Duration, Instant};

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

    /// Render the preview at `(w, h)` points, if the editor has a preset,
    /// its parameters and a picture.
    fn render(&self, w: u32, h: u32) {
        let job = self.editor.with_untracked(|e| {
            e.renderable().then(|| {
                (PathBuf::from(e.preset_path.trim()), e.effective(), PathBuf::from(e.preview_image_path.trim()))
            })
        });
        let Some((preset, params, image)) = job else { return };
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
        preview.update(&preset, &params, &image, w.max(1), h.max(1));
        if let Some(e) = preview.error() {
            self.preview_error.set(Some(e.to_owned()));
            return;
        }
        let Some((fw, fh, rgb)) = preview.read_frame() else {
            self.preview_error.set(Some("no frame rendered".to_owned()));
            return;
        };
        let rgba: Vec<u8> = rgb.chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]).collect();
        let (vw, vh) = preview.viewport();
        self.preview_error.set(None);
        self.frame.set(Some(Frame { width: vw as f32, height: vh as f32, pixels: Pixels::new(fw, fh, rgba) }));
    }

    fn frame_interval(&self) -> Option<Duration> {
        self.preview.get_untracked().borrow().as_ref().and_then(|p| p.frame_interval())
    }
}

/// A label, a path field and "Browse…" onto the platform's dialog.
#[component]
fn PathRow(#[prop(into)] label: String, value: Value<String>, on_edit: Callback<String>, filter: Filter<'static>) -> impl View {
    let (title, picked) = (label.clone(), on_edit.clone());
    let browse = move || {
        let request = OpenFile::new()
            .title(title.clone())
            .filter(FileFilter::new(filter.0, browse::extensions(filter)));
        let picked = picked.clone();
        spawn_local(async move {
            if let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) {
                let path = browse::picked(&path);
                browse::remember(&path);
                picked.call(path.display().to_string());
            }
        });
    };
    view! {
        <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
            <Text width=LABEL_W shrink=0.0>{label.clone()}</Text>
            <TextInput grow=1.0 a11y_label=label value=value @input=move |s| on_edit.call(s)/>
            <Button @click=browse>"Browse…"</Button>
        </Row>
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
                <Text text_style=TextStyle::Callout>
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
            size=Size::new(660.0, 340.0)
            min_size=Size::new(480.0, 240.0)
            modal=Modality::Application
            open=shaders.open
            @close_request=move || shaders.close_list(library)
        >
            <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
                {crate::shot::arm(&["profiles", "saveprofile"])}
                <Show
                    when=move || shaders.profiles.with(Vec::is_empty)
                    fallback=|| view! { <ProfileList/> }
                >
                    <Column grow=1.0 align=Align::Center justify=Justify::Center>
                        <Text>"No shader profiles yet."</Text>
                    </Column>
                </Show>
                <Row gap=Spacing::Sm shrink=0.0>
                    <Button @click=move || shaders.open_editor(Editor::new_profile)>"New profile…"</Button>
                    <Button
                        enabled=move || shaders.profiles.with(|p| p.iter().any(|e| e.is_default))
                        @click=move || {
                            if let Err(e) = shader_library::set_default(&Shaders::dir(), None) {
                                eprintln!("[shader-manager] clearing the default profile: {e}");
                            }
                            shaders.refresh();
                        }
                    >"No default"</Button>
                </Row>
                <PresetCollection/>
            </Column>
        </Window>
    }
}

#[component]
fn ProfileList() -> impl View {
    let shaders = use_store::<Shaders>();
    view! {
        <List
            each=move || shaders.profiles.with(|p| p.iter().map(|e| e.path.clone()).collect::<Vec<_>>())
            key=|p: &PathBuf| p.clone()
            grow=1.0
            min_height=0
            let:path
        >
            <ProfileRow path=path/>
        </List>
    }
}

/// One profile: its name, its preset, and what can be done to it.
#[component]
fn ProfileRow(path: PathBuf) -> impl View {
    let shaders = use_store::<Shaders>();
    let path = Rc::new(path);
    let field = {
        let path = path.clone();
        move |f: fn(&ProfileEntry) -> String| {
            let path = path.clone();
            move || shaders.profiles.with(|p| p.iter().find(|e| e.path == *path).map(f).unwrap_or_default())
        }
    };
    let is_default = {
        let path = path.clone();
        move || shaders.profiles.with(|p| p.iter().any(|e| e.path == *path && e.is_default))
    };
    let (d1, d2) = (is_default.clone(), is_default);
    let (p1, p2, p3) = (path.clone(), path.clone(), path);
    view! {
        <Row padding_x=Spacing::Md padding_y=Spacing::Xs gap=Spacing::Md align=Align::Center>
            <Text max_lines=1 width=170 shrink=0.0>{field(|e| e.profile.name.clone())}</Text>
            <Text text_style=TextStyle::Caption max_lines=1 grow=1.0 min_width=0>
                {field(|e| e.profile.preset.display().to_string())}
            </Text>
            <Show when=d1>
                <Text text_style=TextStyle::Headline>"default"</Text>
            </Show>
            <Show when=move || !d2()>
                <Button @click={let path = p1.clone(); move || {
                    let id = shader_library::id_of(&path);
                    if let Err(e) = shader_library::set_default(&Shaders::dir(), Some(&id)) {
                        eprintln!("[shader-manager] marking {id} as the default: {e}");
                    }
                    shaders.refresh();
                }}>"Use as default"</Button>
            </Show>
            <Button @click=move || {
                let path = p2.to_path_buf();
                shaders.open_editor(|e| e.edit_path(path));
            }>"Edit…"</Button>
            <Button @click=move || {
                if let Err(e) = shader_library::delete(&p3) {
                    eprintln!("[shader-manager] deleting {}: {e}", p3.display());
                }
                shaders.refresh();
            }>"Delete"</Button>
        </Row>
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
                <PathRow
                    label="Preset (.slangp)"
                    filter=PRESET_FILTER
                    value=move || shaders.read(|e| e.preset_path.clone())
                    @edit=move |p| shaders.edit(|e| {
                        e.preset_path = p;
                        e.reparse();
                    })
                />
                <PresetCollection/>
                <Show when=move || shaders.read(|e| e.parse_error().is_some())>
                    <Text text_style=TextStyle::Callout>
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
                        <PathRow
                            label="Preview image"
                            filter=IMAGE_FILTER
                            value=move || shaders.read(|e| e.preview_image_path.clone())
                            @edit=move |p| shaders.edit(|e| e.preview_image_path = p)
                        />
                        <PreviewArea/>
                    </Column>
                </Row>
                <Show when=move || shaders.read(|e| e.error.is_some())>
                    <Text text_style=TextStyle::Callout>{move || shaders.read(|e| e.error.clone().unwrap_or_default())}</Text>
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
            <Column gap=Spacing::Sm padding_x=Spacing::Xs>
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

/// One parameter: a box that overrides the preset's value, the slider,
/// and the preset's description of it.
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
    let description = {
        let row = row.clone();
        move || shaders.read(|e| row().and_then(|r| e.description(r)).unwrap_or_default().to_owned())
    };
    let value = meta(|e, r| e.value(r).unwrap_or_default() as f64);
    let (min, max) = (meta(|e, r| e.params()[r].minimum as f64), meta(|e, r| e.params()[r].maximum as f64));
    let step = meta(|e, r| e.params()[r].step as f64);
    let (value2, row2, row3) = (value.clone(), row.clone(), row);
    let (o1, o2, d1, d2) = (overridden.clone(), overridden, description.clone(), description);
    let id2 = id.clone();
    view! {
        <Column gap=Spacing::Xs>
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
                        {move || format!("{}  {:.3}", id2, value2())}
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
            <Show when=move || !d1().is_empty()>
                <Text text_style=TextStyle::Caption padding_x=Spacing::Xl>{d2.clone()}</Text>
            </Show>
        </Column>
    }
}

/// The preview: the frame, centred, at the size the core rendered it for
/// this area (the player's own scale and letterbox).
#[component]
fn PreviewArea() -> impl View {
    let shaders = use_store::<Shaders>();
    let area: Signal<Option<NodeId>> = signal(None);
    let ui = inject::<Ui>();
    // The render tick: the area's size, the editor's inputs and an
    // animated shader's clock, checked every 30 ms.
    spawn_local(async move {
        let mut last_size = (0, 0);
        let mut last_render = Instant::now();
        loop {
            sleep(Duration::from_millis(30)).await;
            let (Some(ui), Some(id)) = (ui.as_ref(), area.get_untracked()) else { continue };
            let Some(frame) = ui.frame(id) else { continue };
            let size = (frame.size.width.round() as u32, frame.size.height.round() as u32);
            let due = shaders.frame_interval().is_some_and(|i| last_render.elapsed() >= i);
            if size != last_size || shaders.stale.get_untracked() || due {
                last_size = size;
                last_render = Instant::now();
                shaders.stale.set(false);
                shaders.render(size.0, size.1);
            }
        }
    });
    let frame = move |f: fn(&Frame) -> f32| move || Length::from(shaders.frame.with(|x| x.as_ref().map_or(0.0, f)));
    let pixels = move || {
        let pixels = shaders.frame.with(|f| f.as_ref().map(|f| f.pixels.clone()));
        ImageSource::Pixels(pixels.unwrap_or_else(|| Pixels::new(1, 1, vec![0u8; 4])))
    };
    let mut column = Column::new()
        .grow(1.0)
        .min_height(0)
        .min_width(0)
        .align(Align::Center)
        .justify(Justify::Center)
        .children(view! {
            <Show when=move || shaders.read(|e| e.preview_image_path.trim().is_empty())>
                <Text text_style=TextStyle::Caption>"Pick a screenshot to preview the shader."</Text>
            </Show>
            <Show when=move || shaders.preview_error.get().is_some()>
                <Text text_style=TextStyle::Callout>{move || shaders.preview_error.get().unwrap_or_default()}</Text>
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
        });
    column.element().after_build(move |_, id| area.set(Some(id)));
    column
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
/// screen: the editor on a preset and a picture, as the Qt build's
/// `editPreset` does, with one parameter overridden as its box and slider
/// would (the preview must render again for it).
pub fn edit_preset(shaders: Shaders, arg: &str) {
    let mut parts = arg.split(';');
    let preset = parts.next().unwrap_or_default().to_owned();
    let image = parts.next().unwrap_or_default().to_owned();
    shaders.open_editor(|e| e.open_with(preset, image));
    if let Some((id, value)) = parts.next().and_then(|p| p.split_once('=')) {
        let value: f32 = value.parse().unwrap_or_default();
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
