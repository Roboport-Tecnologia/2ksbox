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

use crate::about::{About, AboutWindow};
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
#[cfg(not(sidebar))]
const LIST_W: f32 = 260.0;
/// The machine sidebar's on macOS and GTK.
#[cfg(sidebar)]
const SIDEBAR_W: f64 = 250.0;
/// The details' width on GTK, beside the sidebar; narrower than half of
/// it, the sidebar collapses (`sidebar_width`).
pub const GTK_CONTENT_W: f32 = 620.0;
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
    /// so the window polls.
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
    let about = use_store::<About>();
    let stores = Stores { library, wizard, cloner, snaps, discs, shaders };
    crate::about::app_menu(about, command_menus(stores));
    // The window's task, which ends with it.
    spawn_local(library.poll());
    crate::shot::arm(&["", "select", "create", "clonego", "firstrun"]);
    // The first-run offer asks on a real start, and on
    // `firstrun[:<answers>]` with scripted answers; every other headless
    // screen runs without it.
    if std::env::var_os("LAUNCHER_SCREEN").is_none() {
        offer.start(library, shaders, None);
    }
    if let Some(answers) = crate::shot::screen("firstrun") {
        offer.start(library, shaders, Some(&answers));
    }
    if crate::shot::screen("about").is_some() {
        about.show();
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
    // On macOS (user) and GTK: Start, then the chosen machine's windows as
    // one group of icons (a capsule on macOS), each named by its tooltip;
    // New and the two libraries are the menus' (`command_menus`).
    #[cfg(sidebar)]
    let toolbar_buttons = {
        let stores = Stores::get();
        let tool = move |command: Command, icon: &'static str| {
            view! {
                <Button
                    icon=icon
                    icon_only=true
                    tooltip=command.tooltip()
                    enabled=move || command.enabled(stores, library.current().as_deref())
                    @click=move || command.run(stores, library.current().as_deref())
                >{command.label()}</Button>
            }
        };
        view! {
            <Button
                icon=icons::START
                tooltip=Command::Start.tooltip()
                enabled=move || Command::Start.enabled(stores, library.current().as_deref())
                @click=move || Command::Start.run(stores, library.current().as_deref())
            >"Start"</Button>
            <Row>
                {tool(Command::Settings, icons::SETTINGS)}
                {tool(Command::Discs, icons::DISCS)}
                {tool(Command::Snapshots, icons::SNAPSHOTS)}
                {tool(Command::Clone, icons::CLONE)}
            </Row>
        }
    };
    #[cfg(not(sidebar))]
    let shelf = move || {
        view! {
            <Button icon=icons::DISCS tooltip=Command::Shelf.tooltip() @click=move || discs.open_library(library)>"Shelf"</Button>
        }
    };
    #[cfg(not(sidebar))]
    let shaders_button = move || {
        view! {
            <Button icon=icons::SHADERS tooltip=Command::Shaders.tooltip() @click=move || shaders.open_list()>"Shaders"</Button>
        }
    };
    // Elsewhere New, Shelf and Shaders, two items the platform spaces as
    // its toolbars do.
    #[cfg(not(sidebar))]
    let toolbar_buttons = view! {
        <Button icon=icons::NEW tooltip=Command::New.tooltip() @click=move || wizard.open_fresh()>"New"</Button>
        {shelf()}
        {shaders_button()}
    };
    // About, at the toolbar's end. Not on macOS (user) or GTK: there it is
    // the application menu's or the primary menu's (`about::app_menu`).
    #[cfg(sidebar)]
    let about_button = ();
    #[cfg(not(sidebar))]
    let about_button = view! {
        <Button icon=icons::ABOUT icon_only=true tooltip="About 2ksbox" @click=move || about.show()>
            "About 2ksbox"
        </Button>
    };
    // On Windows and Kirigami the window takes the commands' keys itself:
    // there are no menus to carry them (`command_menus`).
    let mut root = Column::new().grow(1.0).min_height(0);
    if !cfg!(sidebar) {
        for command in Command::ALL {
            root = root.on_key(command.shortcut(), move || command.run(stores, library.current().as_deref()));
        }
    }
    root.children(view! {
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
                {toolbar_buttons}
                {about_button}
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
            <AboutWindow/>
    })
}

/// The library: the machines down the leading side, and the chosen one's
/// details beside them, as UTM and VirtualBox lay theirs out.
#[component]
fn MachineLibrary() -> impl View {
    let library = use_store::<Library>();
    view! {
        <Row grow=1.0 min_height=0>
            <MachineList/>
            <Show when=move || library.current().is_some()>
                <Details/>
            </Show>
        </Row>
    }
}

/// The machines on macOS and GTK: the window's sidebar (user), as UTM's,
/// so on macOS it is the system's sidebar glass and the details run up
/// under the toolbar, each machine its name, its family and state under
/// it, the list's menu and a double-click to start it.
#[cfg(sidebar)]
#[component]
fn MachineList() -> impl View {
    let library = use_store::<Library>();
    let stores = Stores::get();
    let chosen = signal(library.current());
    // The sidebar follows the library (the first machine while none is
    // chosen) and the library follows a click; each side only writes when
    // they differ.
    effect(move || {
        let current = library.current();
        if chosen.get_untracked() != current {
            chosen.set(current);
        }
    });
    effect(move || {
        let now: Vec<PathBuf> = chosen.get().into_iter().collect();
        if library.selected.get_untracked() != now {
            library.selected.set(now);
        }
    });
    Sidebar::new(chosen)
        .children_with(move || {
            library
                .dirs()
                .into_iter()
                .map(|dir| {
                    let name = library.field(&dir, |m, row| m.machine(row).map(|x| x.name.clone()));
                    let subtitle = library.field(&dir, |m, row| Some(m.subtitle(row)));
                    let (start, more) = (Rc::new(dir.clone()), Rc::new(dir.clone()));
                    SidebarItem::new(name, Some(dir)).icon(icons::MACHINE).subtitle(subtitle).context_menu((
                        Command::Start.item(stores, Rc::new(move || Some(start.to_path_buf()))),
                        MenuSeparator::new(),
                        machine_actions(Rc::new(move || more.to_path_buf())),
                    ))
                })
                .collect::<Vec<_>>()
        })
        .native(sidebar_width())
        .on_activate(move |dir: Option<PathBuf>| {
            if let Some(dir) = dir.filter(|d| !library.is_running(d)) {
                library.play(&dir);
            }
        })
}

/// 250, from AppKit's 140 (user), which the window grows by: the split
/// view is the window's, reached from the sidebar's table.
#[cfg(target_os = "macos")]
fn sidebar_width() -> Tweak<Sidebar<Option<PathBuf>>> {
    mitsuami::appkit::tweak(|table: &mitsuami::appkit::objc2_app_kit::NSTableView| {
        use mitsuami::appkit::objc2_app_kit::NSSplitViewController;
        let split = table.window().and_then(|w| w.contentViewController());
        let split = split.and_then(|c| c.downcast::<NSSplitViewController>().ok());
        if let Some(item) = split.and_then(|s| s.splitViewItems().firstObject()) {
            item.setMinimumThickness(SIDEBAR_W);
        }
    })
}

/// 250 wide as on macOS, from libadwaita's 180 (user), and rows as roomy as
/// the list's elsewhere: a 32 px icon, the name a size up, more padding
/// than GNOME Settings' rows. The split view is the window's, an ancestor
/// of the list once it is shown, in the window's breakpoint bin.
#[cfg(all(target_os = "linux", feature = "gtk", not(feature = "kde")))]
fn sidebar_width() -> Tweak<Sidebar<Option<PathBuf>>> {
    use mitsuami::gtk::gtk;
    use adw::prelude::*;
    const CSS: &str = "
        .machines > row { padding: 12px 12px; margin: 3px 6px; }
        .machines > row image { -gtk-icon-size: 32px; }
        .machines > row box > box > label:first-child { font-size: 1.1em; font-weight: bold; }
        .machines > row box > box { margin-left: 2px; }
    ";
    mitsuami::gtk::tweak(|list: &gtk::ListBox| {
        if list.has_css_class("machines") {
            return;
        }
        list.add_css_class("machines");
        // The tweak first runs before the window puts the sidebar in its
        // split view; showing the list comes after.
        // The details narrower than half their first width collapse the
        // split view into its two pages (user), before libadwaita's own
        // 400sp: a breakpoint on the window's breakpoint bin.
        list.connect_map(|list| {
            let split = std::iter::successors(list.parent(), |w| w.parent())
                .find_map(|w| w.downcast::<adw::NavigationSplitView>().ok());
            let Some(split) = split.filter(|s| s.min_sidebar_width() != SIDEBAR_W) else { return };
            split.set_min_sidebar_width(SIDEBAR_W);
            let bin = split.parent().and_then(|w| w.downcast::<adw::BreakpointBin>().ok());
            let width = SIDEBAR_W + f64::from(GTK_CONTENT_W) / 2.0;
            if let (Some(bin), Ok(condition)) = (bin, adw::BreakpointCondition::parse(&format!("max-width: {width}px"))) {
                let breakpoint = adw::Breakpoint::new(condition);
                breakpoint.add_setter(&split, "collapsed", Some(&true.to_value()));
                bin.add_breakpoint(breakpoint);
            }
        });
        let provider = gtk::CssProvider::new();
        provider.load_from_data(CSS);
        gtk::style_context_add_provider_for_display(
            &list.display(),
            &provider,
            gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
        );
    })
}

/// The machines elsewhere: a list beside the details, a line between.
#[cfg(not(sidebar))]
#[component]
fn MachineList() -> impl View {
    let library = use_store::<Library>();
    view! {
        <Row shrink=0.0>
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
        </Row>
    }
}

/// What can be done to a machine besides starting it, each opening its
/// window on it: the details' More menu (not on macOS), and the list's
/// context menu under Start. `dir` names the machine when the menu is used.
fn machine_actions(dir: Rc<dyn Fn() -> PathBuf>) -> impl mitsuami::core::services::MenuEntries {
    let stores = Stores::get();
    let dir: Rc<dyn Fn() -> Option<PathBuf>> = Rc::new(move || Some(dir()));
    (
        Command::Settings.item(stores, dir.clone()),
        Command::Discs.item(stores, dir.clone()),
        Command::Snapshots.item(stores, dir.clone()),
        Command::Clone.item(stores, dir),
    )
}

/// The stores the commands act through, `Copy` like each of them.
#[derive(Clone, Copy)]
struct Stores {
    library: Library,
    wizard: Wizard,
    cloner: Cloner,
    snaps: Snaps,
    discs: Discs,
    shaders: Shaders,
}

impl Stores {
    fn get() -> Stores {
        Stores {
            library: use_store::<Library>(),
            wizard: use_store::<Wizard>(),
            cloner: use_store::<Cloner>(),
            snaps: use_store::<Snaps>(),
            discs: use_store::<Discs>(),
            shaders: use_store::<Shaders>(),
        }
    }
}

/// The main window's commands and their keys, the platform's primary
/// modifier (`Shortcut::primary`: Command on macOS, Ctrl elsewhere) with a
/// letter. On macOS they are the menu bar's; elsewhere the window takes
/// the keys itself, and the More and context menus and the toolbar's
/// tooltips show them.
#[derive(Clone, Copy, PartialEq)]
enum Command {
    New,
    Start,
    Settings,
    Discs,
    Snapshots,
    Clone,
    Shelf,
    Shaders,
}

impl Command {
    const ALL: [Command; 8] = [
        Command::New,
        Command::Start,
        Command::Settings,
        Command::Discs,
        Command::Snapshots,
        Command::Clone,
        Command::Shelf,
        Command::Shaders,
    ];

    fn label(self) -> &'static str {
        match self {
            Command::New => "New Machine",
            Command::Start => "Start",
            Command::Settings => "Settings",
            Command::Discs => "Discs",
            Command::Snapshots => "Snapshots",
            Command::Clone => "Clone",
            Command::Shelf => "Disc Shelf",
            Command::Shaders => "Shader Profiles",
        }
    }

    /// ⌘D is Duplicate on macOS, so Clone; ⌘S would read as Save, so
    /// Snapshots takes Shift too.
    fn shortcut(self) -> Shortcut {
        let key = |c: char| Shortcut::primary(Key::Char(c));
        match self {
            Command::New => key('n'),
            Command::Start => key('r'),
            Command::Settings => key('i'),
            Command::Discs => key('e'),
            Command::Snapshots => key('s').shift(),
            Command::Clone => key('d'),
            Command::Shelf => key('d').shift(),
            Command::Shaders => key('p').shift(),
        }
    }

    /// The label and its keys as the platform writes them, for a button
    /// that runs the command: "Start (⌘R)", "Start (Ctrl+R)".
    fn tooltip(self) -> String {
        let s = self.shortcut();
        let Key::Char(c) = s.key else { return self.label().to_owned() };
        let c = c.to_ascii_uppercase();
        let keys = if cfg!(target_os = "macos") {
            format!("{}{}⌘{c}", if s.alt { "⌥" } else { "" }, if s.shift { "⇧" } else { "" })
        } else {
            format!("Ctrl+{}{}{c}", if s.alt { "Alt+" } else { "" }, if s.shift { "Shift+" } else { "" })
        };
        format!("{} ({keys})", self.label())
    }

    /// Whether it can run on the machine in `dir` (the chosen one, for the
    /// menu bar and the keys).
    fn enabled(self, s: Stores, dir: Option<&Path>) -> bool {
        match self {
            Command::New | Command::Shelf | Command::Shaders => true,
            Command::Start => dir.is_some_and(|d| !s.library.is_running(d)),
            Command::Clone => dir.is_some() && !s.cloner.busy(),
            Command::Settings | Command::Discs | Command::Snapshots => dir.is_some(),
        }
    }

    fn run(self, s: Stores, dir: Option<&Path>) {
        if !self.enabled(s, dir) {
            return;
        }
        let running = dir.is_some_and(|d| s.library.is_running(d));
        let bundle = dir.and_then(|d| s.library.bundle_path(d));
        match (self, bundle) {
            (Command::New, _) => s.wizard.open_fresh(),
            (Command::Shelf, _) => s.discs.open_library(s.library),
            (Command::Shaders, _) => s.shaders.open_list(),
            (Command::Start, _) => s.library.play(dir.expect("enabled")),
            (Command::Settings, Some(b)) => s.wizard.open_edit(b),
            (Command::Discs, Some(b)) => s.discs.open_for(b, s.library),
            (Command::Snapshots, Some(b)) => s.snaps.open_for(&b, running),
            (Command::Clone, Some(b)) => s.cloner.open_for(&b, running),
            (_, None) => {}
        }
    }

    /// The command as a menu item on the machine `dir` names.
    fn item(self, s: Stores, dir: Rc<dyn Fn() -> Option<PathBuf>>) -> MenuItem {
        let d = dir.clone();
        MenuItem::new(self.label())
            .shortcut(self.shortcut())
            .enabled(move || self.enabled(s, d().as_deref()))
            .on_select(move || self.run(s, dir().as_deref()))
    }
}

/// The menus' commands on macOS and GTK, on the chosen machine: File's New
/// Machine (AppKit's place for New) and the two libraries (user), and
/// Machine's; on GTK each menu is a section of the primary menu. Elsewhere
/// `None`: the window takes the keys.
fn command_menus(s: Stores) -> Option<MenuBar> {
    if !cfg!(sidebar) {
        return None;
    }
    let current: Rc<dyn Fn() -> Option<PathBuf>> = Rc::new(move || s.library.current());
    let item = |c: Command| c.item(s, current.clone());
    Some(
        MenuBar::new()
            .menu(
                Menu::new("File")
                    .item(item(Command::New))
                    .separator()
                    .item(item(Command::Shelf))
                    .item(item(Command::Shaders)),
            )
            .menu(
                Menu::new("Machine")
                    .item(item(Command::Start))
                    .separator()
                    .item(item(Command::Settings))
                    .item(item(Command::Discs))
                    .item(item(Command::Snapshots))
                    .item(item(Command::Clone)),
            ),
    )
}

/// One machine in the list: its name, and its family and state under it.
/// A right click offers what the details' Start and More do.
#[cfg(not(sidebar))]
#[component]
fn MachineRow(dir: PathBuf) -> impl View {
    let library = use_store::<Library>();
    let dir = Rc::new(dir);
    let (d1, d2, d3) = (dir.clone(), dir.clone(), dir.clone());
    let stores = Stores::get();
    let menu = (
        Command::Start.item(stores, Rc::new(move || Some(d3.to_path_buf()))),
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
    // Start and More beside the name; not on macOS or GTK, where they are
    // the toolbar's (user).
    #[cfg(sidebar)]
    let actions = ();
    #[cfg(not(sidebar))]
    let actions = {
            let running = move || library.is_running(&current());
            view! {
                <Button
                    role=ButtonRole::Default
                    icon=icons::START
                    enabled=move || !running()
                    tooltip=Command::Start.tooltip()
                    @click=move || library.play(&current())
                >{move || if running() { "Running" } else { "Start" }.to_owned()}</Button>
                <MenuButton menu=machine_actions(Rc::new(current))>"More"</MenuButton>
            }
    };
    // On Windows the details are a shade darker than the list beside them
    // (user): Fluent's secondary background, which follows the theme as the
    // window's base one does, set as a style so it switches with it.
    let background = platform! {
        windows => mitsuami::winui::tweak(|view: &mitsuami::winui::bindings::ScrollViewer| {
            use mitsuami::winui::bindings as w;
            use mitsuami::winui::windows_core::Interface;
            let markup = r#"<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ScrollViewer"><Setter Property="Background" Value="{ThemeResource SolidBackgroundFillColorSecondaryBrush}"/></Style>"#;
            view.cast::<w::IFrameworkElement>()?.SetStyle(&w::XamlReader::Load(markup)?.cast::<w::Style>()?)
        }),
        _ => Tweak::none(),
    };
    view! {
        <ScrollView grow=1.0 min_width=0 native=background>
            // Half the room above the name as at the other edges (user).
            <Column padding_x=Spacing::Xl padding_bottom=Spacing::Xl padding_top=Spacing::Md gap=Spacing::Lg>
                <Row gap=Spacing::Md align=Align::Center>
                    <Column gap=Spacing::Xs grow=1.0 min_width=0>
                        <Text text_style=TextStyle::LargeTitle max_lines=1>{field(|m, row| m.machine(row).map(|x| x.name.clone()))}</Text>
                        <Text color=Color::SecondaryLabel>{field(|m, row| Some(m.subtitle(row)))}</Text>
                    </Column>
                    {actions}
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
    // The shader list's No default; on macOS and Windows a slashed star,
    // not a clear mark (user): Segoe's Unfavorite on Windows.
    pub const CLEAR: &str = platform! {
        macos => "star.slash", gtk => "edit-clear-symbolic", kde => "edit-clear", windows => "\u{E8D9}",
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
    // The machine's Settings and Clone, the toolbar's on macOS and GTK.
    #[cfg_attr(not(sidebar), allow(dead_code))]
    pub const SETTINGS: &str = platform! {
        macos => "gearshape", gtk => "emblem-system-symbolic", kde => "configure", windows => "\u{E713}",
    };
    #[cfg_attr(not(sidebar), allow(dead_code))]
    pub const CLONE: &str = platform! {
        macos => "plus.square.on.square", gtk => "edit-copy-symbolic", kde => "edit-copy", windows => "\u{E8C8}",
    };
    // The Shaders toolbar button's; on macOS and GTK the File menu has them.
    #[cfg_attr(sidebar, allow(dead_code))]
    pub const SHADERS: &str = platform! {
        macos => "tv", gtk => "video-display-symbolic", kde => "video-display", windows => "\u{E7F4}",
    };
    // An "i" everywhere (user): the toolkits' About icon, Segoe's Info on
    // Windows. macOS and GTK have no toolbar button for it (the menus
    // have About), so macOS has no symbol.
    #[cfg_attr(sidebar, allow(dead_code))]
    pub const ABOUT: &str = platform! {
        macos => "", gtk => "help-about-symbolic", kde => "help-about", windows => "\u{E946}",
    };
}
