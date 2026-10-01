//! "Snapshots…" on a machine row, over `launcher_core::snaps::Snapshots`:
//! the tree of a machine's snapshots as a table, and take, restore and
//! delete. Take snapshot is on the window's toolbar and asks for the name
//! in a sheet. Which source the list comes from (the monitor for a running
//! machine, `qemu-img` for a stopped one), the jobs and the tree are the
//! core's. This window owns when to poll a job and how a restore is
//! confirmed: a first click asks, a second one restores.
//!
//! Rows are keyed by snapshot id and read their fields by it: qcow2 reuses
//! an id once its snapshot is deleted, and a keyed row stays mounted while
//! its key does, so a row holding a copy would show the old snapshot.

use launcher_core::snaps::Snapshots;
use launcher_core::snapshots::Snapshot;
use mitsuami::prelude::*;
use std::path::Path;
use std::time::Duration;

const TAKEN_W: f32 = 180.0;
const STATE_W: f32 = 80.0;
/// Restore while it asks ("Discard current state?") and the trash button.
const ACTIONS_W: f32 = 260.0;

#[derive(Clone, Copy)]
pub struct Snaps {
    model: Signal<Snapshots>,
    /// The snapshot whose Restore was clicked once and now asks.
    confirm: Signal<Option<String>>,
    /// Take snapshot's name sheet is open.
    naming: Signal<bool>,
}

impl Store for Snaps {
    fn create() -> Snaps {
        Snaps { model: signal(Snapshots::default()), confirm: signal(None), naming: signal(false) }
    }
}

impl Snaps {
    fn read<R>(&self, f: impl FnOnce(&Snapshots) -> R) -> R {
        self.model.with(f)
    }

    /// Run an operation, then poll the job it started, if any, until it
    /// ends (a running machine's are jobs; a stopped one's are done).
    fn run(&self, f: impl FnOnce(&mut Snapshots)) {
        self.model.update(f);
        if !self.model.with_untracked(Snapshots::job_pending) {
            return;
        }
        let snaps = *self;
        spawn_local(async move {
            while snaps.model.with_untracked(Snapshots::job_pending) {
                sleep(Duration::from_millis(400)).await;
                snaps.model.update(Snapshots::poll_job);
            }
        });
    }

    pub fn open_for(&self, bundle: &Path, running: bool) {
        self.confirm.set(None);
        self.naming.set(false);
        self.run(|m| m.open_for_path(bundle, running));
    }

    /// For the headless `snapshots:<machine.toml>:ask=<name>` screen: the
    /// state a first click on a row's Restore leaves.
    pub fn ask(&self, name: &str) {
        self.confirm.set(Some(name.to_owned()));
    }

    /// For the headless `takesnapshot:<machine.toml>` screen: Take
    /// snapshot clicked, its name sheet open.
    pub fn ask_name(&self) {
        self.naming.set(true);
    }

    fn close(&self) {
        self.naming.set(false);
        self.model.update(|m| m.open = false);
    }

    fn busy(&self) -> bool {
        self.read(Snapshots::job_pending)
    }

    /// One of a row's fields, by snapshot id.
    fn field<R: Default>(&self, id: &str, f: impl FnOnce(&Snapshot) -> R) -> R {
        self.read(|m| m.snapshots().iter().find(|s| s.id == id).map(f).unwrap_or_default())
    }
}

#[component]
pub fn SnapshotsWindow() -> impl View {
    let snaps = use_store::<Snaps>();
    view! {
        <Window
            title=move || snaps.read(|m| m.title())
            size=Size::new(800.0, 500.0)
            min_size=Size::new(640.0, 320.0)
            modal=Modality::Application
            open=move || snaps.read(|m| m.open)
            @close_request=move || snaps.close()
        >
            <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
                {crate::shot::arm(&["snapshots"])}
                <Toolbar>
                    <Button
                        icon=crate::machines::icons::SNAPSHOTS
                        enabled=move || !snaps.busy()
                        @click=move || {
                            snaps.confirm.set(None);
                            snaps.naming.set(true);
                        }
                    >"Take snapshot"</Button>
                </Toolbar>
                <Show when=move || snaps.read(Snapshots::running)>
                    <Text text_style=TextStyle::Caption>
                        "The machine is running, so a snapshot also saves its RAM and CPU state."
                    </Text>
                </Show>
                <Show
                    when=move || snaps.read(|m| m.snapshots().is_empty())
                    fallback=|| view! { <SnapshotTable/> }
                >
                    <Column grow=1.0 align=Align::Center justify=Justify::Center>
                        <Text>"No snapshots yet."</Text>
                    </Column>
                </Show>
                <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
                    <Show when=move || snaps.busy()>
                        <Spinner label="Working"/>
                    </Show>
                    <Text text_style=TextStyle::Caption>
                        {move || snaps.read(|m| m.status().unwrap_or_default().to_owned())}
                    </Text>
                </Row>
                <Show when=move || snaps.read(|m| m.error().is_some())>
                    <Text color=Color::Error>
                        {move || snaps.read(|m| m.error().unwrap_or_default().to_owned())}
                    </Text>
                </Show>
                <TakeSnapshotWindow/>
            </Column>
        </Window>
    }
}

/// Take snapshot's question, a sheet on the snapshots window: the new
/// snapshot's name, empty at each opening.
#[component]
fn TakeSnapshotWindow() -> impl View {
    let snaps = use_store::<Snaps>();
    let name = signal(String::new());
    let can_take = move || !name.get().trim().is_empty() && !snaps.busy();
    let take = move || {
        let name = name.get_untracked();
        if !name.trim().is_empty() && !snaps.busy() {
            snaps.naming.set(false);
            snaps.run(|m| m.take(&name));
        }
    };
    view! {
        <Window
            title="Take snapshot"
            size=WindowSize::FitHeight(420.0)
            resizable=false
            modal=Modality::Window
            open=snaps.naming
            @open=move || name.set(String::new())
            @close_request=move || snaps.naming.set(false)
        >
            <Column padding=Spacing::Lg gap=Spacing::Md>
                {crate::shot::arm(&["takesnapshot"])}
                <Row gap=Spacing::Sm align=Align::Center>
                    <Text>"Name"</Text>
                    <TextInput grow=1.0 a11y_label="Name" bind=name @submit=take/>
                </Row>
                <Row gap=Spacing::Sm justify=Justify::End>
                    <Button role=ButtonRole::Cancel @click=move || snaps.naming.set(false)>"Cancel"</Button>
                    <Button role=ButtonRole::Default enabled=can_take @click=take>"Take snapshot"</Button>
                </Row>
            </Column>
        </Window>
    }
}

/// The snapshots under column headers: the name, indented under its parent
/// with the "current" mark on the one the disk descends from, when it was
/// taken, its VM state, and its two buttons.
#[component]
fn SnapshotTable() -> impl View {
    let snaps = use_store::<Snaps>();
    let field = move |id: &str, f: fn(&Snapshot) -> String| {
        let id = id.to_owned();
        move || snaps.field(&id, f)
    };
    let columns = vec![
        TableColumn::new("Name", move |id: String| {
            let name = field(&id, |s| s.name.clone());
            let (d1, d2) = (id.clone(), id);
            let depth = move || snaps.field(&d1, |s| s.depth);
            let indent = depth.clone();
            view! {
                <Row gap=Spacing::Sm align=Align::Center>
                    <Row width=move || Length::from(indent() as f32 * 18.0)/>
                    <Text max_lines=1>
                        {move || format!("{}{}", if depth() > 0 { "└ " } else { "" }, name())}
                    </Text>
                    <Show when=move || snaps.field(&d2, |s| s.current)>
                        <Text text_style=TextStyle::Caption color=Color::SecondaryLabel>"current"</Text>
                    </Show>
                </Row>
            }
        })
        .expand(),
        TableColumn::new("Taken", move |id: String| Text::new(field(&id, |s| s.date_label())).max_lines(1))
            .width(TAKEN_W),
        TableColumn::new("VM state", move |id: String| Text::new(field(&id, |s| s.size_label())).max_lines(1))
            .width(STATE_W),
        TableColumn::new("", move |id: String| {
            let name = field(&id, |s| s.name.clone());
            let (n1, n2) = (name.clone(), name.clone());
            let asking = move || snaps.confirm.get().is_some_and(|c| c == n1());
            let restore = move || {
                let name = n2();
                if snaps.confirm.get_untracked().as_deref() == Some(name.as_str()) {
                    snaps.confirm.set(None);
                    snaps.run(|m| m.revert(&name));
                } else {
                    snaps.confirm.set(Some(name));
                }
            };
            let delete = move || {
                snaps.confirm.set(None);
                let name = name();
                snaps.run(|m| m.drop_snapshot(&name));
            };
            view! {
                <Row gap=Spacing::Sm align=Align::Center>
                    <Button enabled=move || !snaps.busy() @click=restore>
                        {move || if asking() { "Discard current state?" } else { "Restore" }.to_owned()}
                    </Button>
                    <Button
                        icon=crate::machines::icons::TRASH
                        icon_only=true
                        tooltip="Delete"
                        enabled=move || !snaps.busy()
                        @click=delete
                    >"Delete"</Button>
                </Row>
            }
        })
        .width(ACTIONS_W),
    ];
    view! {
        <Table
            each=move || snaps.read(|m| m.snapshots().iter().map(|s| s.id.clone()).collect::<Vec<_>>())
            key=|id: &String| id.clone()
            columns=columns
            list_style=ListStyle::Framed
            grow=1.0
            min_height=0
        />
    }
}
