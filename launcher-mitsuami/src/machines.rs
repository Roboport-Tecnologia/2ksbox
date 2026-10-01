//! The machine library window, over `launcher_core::machines::Machines`:
//! the scan, the running-player map, what "Start" does (publish the shelf,
//! derive the monitor socket from the bundle directory, spawn) and the
//! reap that notices a player exiting.
//!
//! The model is the `Library` store's. It is not itself a signal's value:
//! `reap` needs it mutably every half second, and a signal's `update`
//! would redraw every row each time whether a player exited or not. It
//! sits in a `RefCell` beside a version signal that moves only when the
//! model did, and every view reads it through `Library::read`, which
//! tracks that version.
//!
//! The list is keyed by bundle directory, and a keyed row stays mounted
//! while its key does, so a row reads its fields through the model by
//! that key rather than holding a copy: a player exiting changes only its
//! row's state line. Beside the list, the chosen machine's details (the
//! core's `details`), as UTM and VirtualBox show theirs.

use crate::clone::{CloneWindow, Cloner};
use crate::firstrun::Offer;
use crate::shaders::{ShaderEditorWindow, ShaderProfilesWindow, Shaders};
use crate::discs::{DiscShelfWindow, Discs};
use crate::snaps::{Snaps, SnapshotsWindow};
use crate::wizard::{Wizard, WizardWindow};
use launcher_core::machines::{DetailGroup, Machines};
use mitsuami::core::{CurrentWindow, Ui};
use mitsuami::prelude::*;
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;

/// The machine list's width, beside the details.
const LIST_W: f32 = 260.0;
/// The details' label column.
const LABEL_W: f32 = 150.0;

#[derive(Clone, Copy)]
pub struct Library {
    /// Never set: a `Copy` handle on the model, so the store is `Copy`.
    model: Signal<Rc<RefCell<Machines>>>,
    version: Signal<u64>,
    /// A start that failed, as the alert's headline and the core's error,
    /// until the window shows it.
    failure: Signal<Option<(String, String)>>,
    /// The list's selection, by bundle directory; `current` is what the
    /// details show.
    selected: Signal<Vec<PathBuf>>,
}

impl Store for Library {
    fn create() -> Library {
        Library {
            model: signal(Rc::new(RefCell::new(Machines::load()))),
            version: signal(0),
            failure: signal(None),
            selected: signal(Vec::new()),
        }
    }
}

impl Library {
    /// Read the model, as a dependency of the calling view.
    fn read<R>(&self, f: impl FnOnce(&Machines) -> R) -> R {
        self.version.get();
        self.model.with_untracked(|m| f(&m.borrow()))
    }

    /// Change the model, and say so if `f` reports that it did.
    fn write(&self, f: impl FnOnce(&mut Machines) -> bool) {
        if self.model.with_untracked(|m| f(&mut m.borrow_mut())) {
            self.version.update(|v| *v += 1);
        }
    }

    /// Rescan the library from disk, after a window wrote a bundle.
    pub fn refresh(&self) {
        self.write(|m| {
            m.refresh();
            true
        });
    }

    /// The shared disc shelf's file.
    pub fn disc_library_path(&self) -> PathBuf {
        self.model.with_untracked(|m| m.borrow().disc_library_path.clone())
    }

    /// Hand the shelf to every running machine's drive again, after the
    /// shelf window wrote it.
    pub fn republish_shelf(&self) {
        self.model.with_untracked(|m| m.borrow().republish_shelf());
    }

    fn bundle_path(&self, dir: &Path) -> Option<PathBuf> {
        self.model.with_untracked(|m| row_of(&m.borrow(), dir).and_then(|row| m.borrow().bundle_path(row)))
    }

    fn dirs(&self) -> Vec<PathBuf> {
        self.read(|m| m.entries().iter().map(|e| e.dir.clone()).collect())
    }

    /// One of a row's text fields, by its bundle directory.
    fn field(&self, dir: &Path, f: impl FnOnce(&Machines, usize) -> Option<String>) -> String {
        self.read(|m| row_of(m, dir).and_then(|row| f(m, row)).unwrap_or_default())
    }

    /// The machine the details show: the selected one, or the first while
    /// none is (at start, or after the selected one went).
    fn current(&self) -> Option<PathBuf> {
        let dirs = self.dirs();
        let chosen = self.selected.with(|s| s.first().cloned()).filter(|d| dirs.contains(d));
        chosen.or_else(|| dirs.first().cloned())
    }

    /// A machine's details, by its bundle directory.
    fn details(&self, dir: &Path) -> Vec<DetailGroup> {
        self.read(|m| row_of(m, dir).map(|row| m.details(row)).unwrap_or_default())
    }

    fn is_running(&self, dir: &Path) -> bool {
        self.read(|m| m.is_running_dir(dir))
    }

    fn play(&self, dir: &Path) {
        self.write(|m| {
            let Some(row) = row_of(m, dir) else { return false };
            match m.play(row) {
                Ok(_) => true,
                Err(e) => {
                    let name = m.machine(row).map(|x| x.name.clone()).unwrap_or_default();
                    self.failure.set(Some((Machines::start_failed(&name), e)));
                    false
                }
            }
        })
    }

    /// Reap any player that exited. A child process cannot push that news,
    /// so the window polls, as the Qt window's `Timer` does.
    async fn poll(self) {
        loop {
            sleep(Duration::from_millis(500)).await;
            self.write(|m| !m.reap().is_empty());
        }
    }
}

fn row_of(machines: &Machines, dir: &Path) -> Option<usize> {
    machines.entries().iter().position(|e| e.dir == dir)
}

#[component]
pub fn MachinesWindow() -> impl View {
    let library = use_store::<Library>();
    let wizard = use_store::<Wizard>();
    let cloner = use_store::<Cloner>();
    let snaps = use_store::<Snaps>();
    let discs = use_store::<Discs>();
    let shaders = use_store::<Shaders>();
    let offer = use_store::<Offer>();
    // The window's task, which ends with it.
    spawn_local(library.poll());
    crate::shot::arm(&["", "select", "create", "clonego", "firstrun"]);
    // The first-run offer asks on a real start, and on
    // `firstrun[:<answers>]` with scripted answers; every other headless
    // screen runs without it, as the Qt build's do.
    if std::env::var_os("LAUNCHER_SCREEN").is_none() {
        offer.start(library, shaders, None);
    }
    if let Some(answers) = crate::shot::screen("firstrun") {
        offer.start(library, shaders, Some(&answers));
    }
    if let Some(arg) = crate::shot::screen("wizard") {
        wizard.open_for_screen(&arg);
    }
    if let Some(arg) = crate::shot::screen("edit") {
        wizard.edit_for_screen(&arg);
    }
    // `clone:<machine.toml>[:same]` shows the dialog on a machine (sharing
    // its disk); `clonego:<machine.toml>` presses Clone and shows this
    // window once the copy has landed.
    if let Some(arg) = crate::shot::screen("clone") {
        let (bundle, same) = match arg.strip_suffix(":same") {
            Some(bundle) => (bundle.to_owned(), true),
            None => (arg, false),
        };
        cloner.open_for(Path::new(&bundle), false);
        cloner.set_same_disk(same);
    }
    // `snapshots:<machine.toml>`: the window on a machine.
    if let Some(bundle) = crate::shot::screen("snapshots") {
        snaps.open_for(Path::new(&bundle), false);
    }
    // `takesnapshot:<machine.toml>`: the window on a machine, with Take
    // snapshot's name sheet open.
    if let Some(bundle) = crate::shot::screen("takesnapshot") {
        snaps.open_for(Path::new(&bundle), false);
        snaps.ask_name();
    }
    // `shelf[:<disc>]`: the shared shelf, with a disc added as the Add
    // menu would; `discs:<machine.toml>[:insert=<disc>]`: the shelf for a
    // machine, with a disc's ▶ pressed.
    if let Some(add) = crate::shot::screen("shelf") {
        discs.open_library(library);
        if !add.is_empty() {
            discs.add(Path::new(&add));
        }
    }
    if let Some(arg) = crate::shot::screen("discs") {
        let (bundle, insert) = match arg.split_once(":insert=") {
            Some((bundle, disc)) => (bundle.to_owned(), Some(PathBuf::from(disc))),
            None => (arg, None),
        };
        discs.open_for(PathBuf::from(bundle), library);
        if let Some(disc) = insert {
            discs.insert(&disc);
        }
    }
    // `profiles`: the profile list; `editor:<preset>[;<image>[;<p>=<v>]]`:
    // the editor on a preset and a picture, the preview rendered.
    if crate::shot::screen("profiles").is_some() {
        shaders.open_list();
    }
    if let Some(preset) = crate::shot::screen("saveprofile") {
        crate::shaders::save_probe(shaders, &preset);
    }
    if let Some(arg) = crate::shot::screen("editor") {
        crate::shaders::edit_preset(shaders, &arg);
    }
    if let Some(bundle) = crate::shot::screen("clonego") {
        cloner.open_for(Path::new(&bundle), false);
        cloner.submit(library);
    }
    if let Some(arg) = crate::shot::screen("create") {
        wizard.create_for_screen(&arg, library);
    }
    // `select:<machine.toml>`: the window with that machine chosen, as a
    // click on its row would.
    if let Some(bundle) = crate::shot::screen("select") {
        let dir = Path::new(&bundle).parent().map(Path::to_path_buf).unwrap_or_default();
        library.selected.set(vec![dir]);
    }
    // A start that failed says why in the platform's alert, over this
    // window.
    let ui = inject::<Ui>().expect("a window's component");
    let window = inject::<CurrentWindow>().map(|CurrentWindow(w)| w);
    effect(move || {
        let Some((headline, detail)) = library.failure.get() else { return };
        library.failure.set(None);
        let alert = Alert::new(headline).message(detail).style(AlertStyle::Critical).button("OK");
        let asker = ui.clone();
        ui.spawn_local(async move {
            asker.alert(window, alert).await;
        });
    });
    // The list shows what the details do: the first machine until one is
    // chosen, and the first again when the chosen one goes.
    effect(move || {
        let shown: Vec<PathBuf> = library.current().into_iter().collect();
        if library.selected.with_untracked(|s| *s != shown) {
            library.selected.set(shown);
        }
    });
    view! {
        <Column grow=1.0 min_height=0>
            <Toolbar>
                <Show when=move || offer.progress.get().is_some()>
                    // Only a bar, on the toolbar with no capsule: the size
                    // isn't known until it ends, and the headline is its
                    // label for a screen reader.
                    <Progress
                        label=move || offer.progress.get().unwrap_or_default()
                        indeterminate=true
                        width=160
                    />
                </Show>
                <Button icon=icons::NEW @click=move || wizard.open_fresh()>"New"</Button>
                // One item, so one capsule on macOS 26.
                <Row>
                    <Button icon=icons::DISCS @click=move || discs.open_library(library)>"Shelf"</Button>
                    <Button icon=icons::SHADERS @click=move || shaders.open_list()>"Shaders"</Button>
                </Row>
            </Toolbar>
            <Show when=move || library.read(Machines::is_empty) fallback=|| view! { <MachineLibrary/> }>
                <Column grow=1.0 gap=Spacing::Md align=Align::Center justify=Justify::Center padding=Spacing::Xl>
                    <Text text_style=TextStyle::Title>"No machines yet"</Text>
                    <Text color=Color::SecondaryLabel>{move || library.read(|m| m.library_dir.display().to_string())}</Text>
                    <Button role=ButtonRole::Default @click=move || wizard.open_fresh()>"New machine"</Button>
                </Column>
            </Show>
            <WizardWindow/>
            <CloneWindow/>
            <SnapshotsWindow/>
            <DiscShelfWindow/>
            <ShaderProfilesWindow/>
            <ShaderEditorWindow/>
        </Column>
    }
}

/// The library: the machines down the leading side, and the chosen one's
/// details beside them, as UTM and VirtualBox lay theirs out.
#[component]
fn MachineLibrary() -> impl View {
    let library = use_store::<Library>();
    view! {
        <Row grow=1.0 min_height=0>
            <List
                each=move || library.dirs()
                key=|d: &PathBuf| d.clone()
                selection_mode=SelectionMode::Single
                selected=library.selected
                list_style=ListStyle::Plain
                width=LIST_W
                shrink=0.0
                @activate=move |dir: PathBuf| {
                    if !library.is_running(&dir) {
                        library.play(&dir);
                    }
                }
                let:dir
            >
                <MachineRow dir=dir/>
            </List>
            <Separator orientation=Orientation::Vertical/>
            <Show when=move || library.current().is_some()>
                <Details/>
            </Show>
        </Row>
    }
}

/// What can be done to a machine besides starting it, each opening its
/// window on it: the details' More menu, and the list's context menu
/// under Start. `dir` names the machine when the menu is used.
fn machine_actions(dir: Rc<dyn Fn() -> PathBuf>) -> impl mitsuami::core::services::MenuEntries {
    let library = use_store::<Library>();
    let wizard = use_store::<Wizard>();
    let cloner = use_store::<Cloner>();
    let snaps = use_store::<Snaps>();
    let discs = use_store::<Discs>();
    let bundle = {
        let dir = dir.clone();
        move || library.bundle_path(&dir())
    };
    let running = move || library.is_running(&dir());
    let (b1, b2, b3, b4) = (bundle.clone(), bundle.clone(), bundle.clone(), bundle);
    let r1 = running.clone();
    (
        MenuItem::new("Settings").on_select(move || {
            if let Some(bundle) = b1() {
                wizard.open_edit(bundle);
            }
        }),
        MenuItem::new("Discs").on_select(move || {
            if let Some(bundle) = b2() {
                discs.open_for(bundle, library);
            }
        }),
        MenuItem::new("Snapshots").on_select(move || {
            if let Some(bundle) = b3() {
                snaps.open_for(&bundle, r1());
            }
        }),
        MenuItem::new("Clone").enabled(move || !cloner.busy()).on_select(move || {
            if let Some(bundle) = b4() {
                cloner.open_for(&bundle, running());
            }
        }),
    )
}

/// One machine in the list: its name, and its family and state under it.
/// A right click offers what the details' Start and More do.
#[component]
fn MachineRow(dir: PathBuf) -> impl View {
    let library = use_store::<Library>();
    let dir = Rc::new(dir);
    let (d1, d2, d3, d4) = (dir.clone(), dir.clone(), dir.clone(), dir.clone());
    let menu = (
        MenuItem::new("Start").enabled(move || !library.is_running(&d3)).on_select(move || library.play(&d4)),
        MenuSeparator::new(),
        machine_actions(Rc::new(move || dir.to_path_buf())),
    );
    view! {
        <Row padding_x=Spacing::Lg padding_y=Spacing::Md gap=Spacing::Md align=Align::Center context_menu=menu>
            <Icon name=icons::MACHINE icon_size=32.0/>
            <Column min_width=0 grow=1.0 gap=Spacing::Xs>
                <Text text_style=TextStyle::Headline max_lines=1>
                    {move || library.field(&d1, |m, row| m.machine(row).map(|x| x.name.clone()))}
                </Text>
                <Text text_style=TextStyle::Caption color=Color::SecondaryLabel max_lines=1>
                    {move || library.field(&d2, |m, row| Some(m.subtitle(row)))}
                </Text>
            </Column>
        </Row>
    }
}

/// The chosen machine: its name, what can be done to it, and its
/// settings, a group per page of the machine form.
#[component]
fn Details() -> impl View {
    let library = use_store::<Library>();
    let current = move || library.current().unwrap_or_default();
    let field = move |f: fn(&Machines, usize) -> Option<String>| move || library.field(&current(), f);
    let running = move || library.is_running(&current());
    view! {
        <ScrollView grow=1.0 min_width=0>
            <Column padding=Spacing::Xl gap=Spacing::Lg>
                <Row gap=Spacing::Md align=Align::Center>
                    <Column gap=Spacing::Xs grow=1.0 min_width=0>
                        <Text text_style=TextStyle::LargeTitle max_lines=1>{field(|m, row| m.machine(row).map(|x| x.name.clone()))}</Text>
                        <Text color=Color::SecondaryLabel>{field(|m, row| Some(m.subtitle(row)))}</Text>
                    </Column>
                    <Button
                        role=ButtonRole::Default
                        icon=icons::START
                        enabled=move || !running()
                        @click=move || library.play(&current())
                    >{move || if running() { "Running" } else { "Start" }.to_owned()}</Button>
                    <MenuButton menu=machine_actions(Rc::new(current))>"More"</MenuButton>
                </Row>
                <For
                    each=move || library.details(&current())
                    key=|g: &DetailGroup| g.clone()
                    let:group
                >
                    <DetailBox group=group/>
                </For>
            </Column>
        </ScrollView>
    }
}

/// One page's settings, label and value a row each. A file's value is
/// one line, cut in the middle to fit, with its whole path as the tooltip.
#[component]
fn DetailBox(group: DetailGroup) -> impl View {
    let rows: Vec<_> = group
        .rows
        .into_iter()
        .map(|row| {
            let value = match &row.path {
                Some(path) => view! {
                    <Text grow=1.0 min_width=0 max_lines=1 truncation=Truncation::Middle tooltip={path.display().to_string()}>
                        {row.value}
                    </Text>
                },
                None => view! { <Text grow=1.0 min_width=0>{row.value}</Text> },
            };
            view! {
                <Row gap=Spacing::Md>
                    <Text color=Color::SecondaryLabel width=LABEL_W shrink=0.0>{row.label}</Text>
                    {value}
                </Row>
            }
        })
        .collect();
    // Room inside the box, past the platform's own inset (user).
    view! {
        <Group title=group.title>
            <Column padding_x=Spacing::Md padding_y=Spacing::Sm gap=Spacing::Sm>{rows}</Column>
        </Group>
    }
}

/// The platform's own icons: an SF Symbol, a symbolic GTK theme icon, a
/// Breeze icon, a Segoe Fluent Icons glyph.
pub(crate) mod icons {
    use mitsuami::prelude::platform;

    pub const MACHINE: &str = platform! {
        macos => "desktopcomputer", gtk => "computer-symbolic", kde => "computer", windows => "\u{E977}",
    };
    pub const NEW: &str = platform! {
        macos => "plus", gtk => "list-add-symbolic", kde => "list-add", windows => "\u{E710}",
    };
    pub const START: &str = platform! {
        macos => "play.fill", gtk => "media-playback-start-symbolic", kde => "media-playback-start", windows => "\u{E768}",
    };
    pub const DISCS: &str = platform! {
        macos => "opticaldisc", gtk => "media-optical-symbolic", kde => "media-optical", windows => "\u{E958}",
    };
    pub const SNAPSHOTS: &str = platform! {
        macos => "camera", gtk => "camera-photo-symbolic", kde => "camera-photo", windows => "\u{E722}",
    };
    pub const CLEAR: &str = platform! {
        macos => "xmark.circle", gtk => "edit-clear-symbolic", kde => "edit-clear", windows => "\u{E894}",
    };
    pub const FOLDER: &str = platform! {
        macos => "folder", gtk => "folder-symbolic", kde => "folder", windows => "\u{E8B7}",
    };
    pub const TOOLS: &str = platform! {
        macos => "wrench.and.screwdriver", gtk => "applications-engineering-symbolic", kde => "tools", windows => "\u{E90F}",
    };
    pub const EDIT: &str = platform! {
        macos => "pencil", gtk => "document-edit-symbolic", kde => "document-edit", windows => "\u{E70F}",
    };
    /// Segoe Fluent Icons has no eject glyph: the button shows its caption.
    pub const EJECT: &str = platform! {
        macos => "eject", gtk => "media-eject-symbolic", kde => "media-eject", windows => "",
    };
    pub const DOWNLOAD: &str = platform! {
        macos => "square.and.arrow.down", gtk => "folder-download-symbolic", kde => "download", windows => "\u{E896}",
    };
    pub const TRASH: &str = platform! {
        macos => "trash", gtk => "user-trash-symbolic", kde => "edit-delete", windows => "\u{E74D}",
    };
    pub const SHADERS: &str = platform! {
        macos => "tv", gtk => "video-display-symbolic", kde => "video-display", windows => "\u{E7F4}",
    };
}
