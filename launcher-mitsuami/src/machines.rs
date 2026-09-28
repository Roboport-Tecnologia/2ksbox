//! The machine library window, over `launcher_core::machines::Machines`:
//! the scan, the running-player map, what "Play" does (publish the shelf,
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
//! row's "Running" label.

use crate::clone::{CloneWindow, Cloner};
use crate::firstrun::Offer;
use crate::shaders::{ShaderEditorWindow, ShaderProfilesWindow, Shaders};
use crate::discs::{DiscShelfWindow, Discs};
use crate::snaps::{Snaps, SnapshotsWindow};
use crate::wizard::{Wizard, WizardWindow};
use launcher_core::machines::Machines;
use mitsuami::prelude::*;
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;

/// The widths of the three text columns, shared by the header and every
/// row so they line up.
const NAME_W: f32 = 190.0;
const FAMILY_W: f32 = 80.0;
const SHADER_W: f32 = 170.0;

#[derive(Clone, Copy)]
pub struct Library {
    /// Never set: a `Copy` handle on the model, so the store is `Copy`.
    model: Signal<Rc<RefCell<Machines>>>,
    version: Signal<u64>,
    /// The line at the end of the toolbar: what "Play" last did.
    pub status: Signal<String>,
}

impl Store for Library {
    fn create() -> Library {
        Library {
            model: signal(Rc::new(RefCell::new(Machines::load()))),
            version: signal(0),
            status: signal(String::new()),
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

    fn is_running(&self, dir: &Path) -> bool {
        self.read(|m| m.is_running_dir(dir))
    }

    fn play(&self, dir: &Path) {
        self.write(|m| {
            let Some(row) = row_of(m, dir) else { return false };
            let (line, started) = match m.play(row) {
                Ok(line) => (line, true),
                Err(e) => (e, false),
            };
            self.status.set(line);
            started
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
    crate::shot::arm(&["", "create", "clonego", "firstrun"]);
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
    // `snapshots:<machine.toml>[:ask=<name>]`: the window on a machine,
    // with a row's Restore asking.
    if let Some(arg) = crate::shot::screen("snapshots") {
        let (bundle, ask) = match arg.rsplit_once(":ask=") {
            Some((bundle, name)) => (bundle.to_owned(), Some(name.to_owned())),
            None => (arg, None),
        };
        snaps.open_for(Path::new(&bundle), false);
        if let Some(name) = ask {
            snaps.ask(&name);
        }
    }
    // `shelf[:<disc>]`: the shared shelf, with a disc added through its
    // Add field; `discs:<machine.toml>[:boot=<disc>]`: the shelf for a
    // machine, with a disc ticked to boot with.
    if let Some(add) = crate::shot::screen("shelf") {
        discs.open_library(library);
        discs.add(&add);
    }
    if let Some(arg) = crate::shot::screen("discs") {
        let (bundle, boot) = match arg.split_once(":boot=") {
            Some((bundle, disc)) => (bundle.to_owned(), Some(PathBuf::from(disc))),
            None => (arg, None),
        };
        discs.open_for(PathBuf::from(bundle), library, false);
        if boot.is_some() {
            discs.set_boot(boot);
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
    view! {
        <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
            <Toolbar>
                <Show when=move || offer.progress.get().is_some()>
                    <Row gap=Spacing::Sm align=Align::Center>
                        <Spinner label="Downloading"/>
                        <Text>{move || offer.progress.get().unwrap_or_default()}</Text>
                    </Row>
                </Show>
                <Text max_lines=1 max_width=320 tooltip=library.status>{library.status}</Text>
            </Toolbar>
            <Text text_style=TextStyle::Title>"Machines"</Text>
            <Row padding_x=Spacing::Md gap=Spacing::Md>
                <Text text_style=TextStyle::Headline width=NAME_W>"Name"</Text>
                <Text text_style=TextStyle::Headline width=FAMILY_W>"Family"</Text>
                <Text text_style=TextStyle::Headline width=SHADER_W>"Shader"</Text>
            </Row>
            <Show when=move || library.read(Machines::is_empty) fallback=|| view! { <MachineList/> }>
                <Column grow=1.0 align=Align::Center justify=Justify::Center>
                    <Text>{move || library.read(|m| format!("No machines yet.\n{}", m.library_dir.display()))}</Text>
                </Column>
            </Show>
            <Row gap=Spacing::Sm>
                <Button @click=move || wizard.open_fresh()>"New machine…"</Button>
                <Button @click=move || discs.open_library(library)>"Disc shelf…"</Button>
                <Button @click=move || shaders.open_list()>"Shader profiles…"</Button>
            </Row>
            <WizardWindow/>
            <CloneWindow/>
            <SnapshotsWindow/>
            <DiscShelfWindow/>
            <ShaderProfilesWindow/>
            <ShaderEditorWindow/>
        </Column>
    }
}

#[component]
fn MachineList() -> impl View {
    let library = use_store::<Library>();
    view! {
        <List each=move || library.dirs() key=|d: &PathBuf| d.clone() grow=1.0 let:dir>
            <MachineRow dir=dir/>
        </List>
    }
}

/// One machine: its name, family and shader, and what can be done to it.
#[component]
fn MachineRow(dir: PathBuf) -> impl View {
    let library = use_store::<Library>();
    let wizard = use_store::<Wizard>();
    let cloner = use_store::<Cloner>();
    let snaps = use_store::<Snaps>();
    let discs = use_store::<Discs>();
    let dir = Rc::new(dir);
    let (d1, d2, d3, d4, d5, d6, d7, d8, d9) = (
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir.clone(),
        dir,
    );
    view! {
        <Row padding_x=Spacing::Md padding_y=Spacing::Xs gap=Spacing::Md align=Align::Center>
            <Text max_lines=1 width=NAME_W>
                {move || library.field(&d1, |m, row| m.machine(row).map(|x| x.name.clone()))}
            </Text>
            <Text max_lines=1 width=FAMILY_W>
                {move || library.field(&d2, |m, row| m.machine(row).map(|x| x.family.label().to_string()))}
            </Text>
            <Text max_lines=1 width=SHADER_W grow=1.0>
                {move || library.field(&d3, |m, row| Some(m.shader_label_at(row)))}
            </Text>
            <Show when=move || library.is_running(&d4) fallback=move || {
                let dir = d5.clone();
                view! { <Button width=60 @click=move || library.play(&dir)>"Play"</Button> }
            }>
                <Text width=60>"Running"</Text>
            </Show>
            <Button @click=move || {
                if let Some(bundle) = library.bundle_path(&d6) {
                    wizard.open_edit(bundle);
                }
            }>"Edit…"</Button>
            <Button @click=move || {
                if let Some(bundle) = library.bundle_path(&d9) {
                    discs.open_for(bundle, library, library.is_running(&d9));
                }
            }>"Discs…"</Button>
            <Button @click=move || {
                if let Some(bundle) = library.bundle_path(&d8) {
                    snaps.open_for(&bundle, library.is_running(&d8));
                }
            }>"Snapshots…"</Button>
            <Button enabled=move || !cloner.busy() @click=move || {
                if let Some(bundle) = library.bundle_path(&d7) {
                    cloner.open_for(&bundle, library.is_running(&d7));
                }
            }>"Clone…"</Button>
        </Row>
    }
}
