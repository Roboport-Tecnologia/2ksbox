//! "Snapshots…" on a machine row, over `launcher_core::snaps::Snapshots`:
//! the tree of a machine's snapshots, and take, restore and delete.
//! Which source the list comes from (the monitor for a running machine,
//! `qemu-img` for a stopped one), the jobs and the tree are the core's.
//! This window owns when to poll a job and how a restore is confirmed: a
//! first click asks, a second one restores.
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

#[derive(Clone, Copy)]
pub struct Snaps {
    model: Signal<Snapshots>,
    /// The snapshot whose Restore was clicked once and now asks.
    confirm: Signal<Option<String>>,
}

impl Store for Snaps {
    fn create() -> Snaps {
        Snaps { model: signal(Snapshots::default()), confirm: signal(None) }
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
        self.run(|m| m.open_for_path(bundle, running));
    }

    /// For the headless `snapshots:<machine.toml>:ask=<name>` screen: the
    /// state a first click on a row's Restore leaves.
    pub fn ask(&self, name: &str) {
        self.confirm.set(Some(name.to_owned()));
    }

    fn close(&self) {
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
    let new_name = signal(String::new());
    let take = move || {
        let name = new_name.get_untracked();
        if !name.trim().is_empty() && !snaps.busy() {
            snaps.confirm.set(None);
            snaps.run(|m| m.take(&name));
            new_name.set(String::new());
        }
    };
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
                <Show when=move || snaps.read(Snapshots::running)>
                    <Text text_style=TextStyle::Caption>
                        "The machine is running, so a snapshot also saves its RAM and CPU state."
                    </Text>
                </Show>
                <Row padding_x=Spacing::Md gap=Spacing::Md>
                    <Text text_style=TextStyle::Headline grow=1.0>"Name"</Text>
                    <Text text_style=TextStyle::Headline width=TAKEN_W>"Taken"</Text>
                    <Text text_style=TextStyle::Headline width=STATE_W>"VM state"</Text>
                    // The width of a row's two buttons, so the columns line up.
                    <Row width=260/>
                </Row>
                <Show
                    when=move || snaps.read(|m| m.snapshots().is_empty())
                    fallback=|| view! { <SnapshotList/> }
                >
                    <Column grow=1.0 align=Align::Center justify=Justify::Center>
                        <Text>"No snapshots yet."</Text>
                    </Column>
                </Show>
                <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
                    <Text>"New snapshot"</Text>
                    <TextInput grow=1.0 a11y_label="New snapshot" bind=new_name @submit=take/>
                    <Button
                        enabled=move || !new_name.get().trim().is_empty() && !snaps.busy()
                        @click=take
                    >"Take snapshot"</Button>
                </Row>
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
            </Column>
        </Window>
    }
}

#[component]
fn SnapshotList() -> impl View {
    let snaps = use_store::<Snaps>();
    view! {
        <List
            each=move || snaps.read(|m| m.snapshots().iter().map(|s| s.id.clone()).collect::<Vec<_>>())
            key=|id: &String| id.clone()
            grow=1.0
            min_height=0
            let:id
        >
            <SnapshotRow id=id/>
        </List>
    }
}

/// One snapshot: indented under its parent, the "current" mark on the one
/// the disk descends from, and its two buttons.
#[component]
fn SnapshotRow(id: String) -> impl View {
    let snaps = use_store::<Snaps>();
    let id = std::rc::Rc::new(id);
    let field = {
        let id = id.clone();
        move |f: fn(&Snapshot) -> String| {
            let id = id.clone();
            move || snaps.field(&id, f)
        }
    };
    let name = field(|s| s.name.clone());
    let depth = {
        let id = id.clone();
        move || snaps.field(&id, |s| s.depth)
    };
    let current = {
        let id = id.clone();
        move || snaps.field(&id, |s| s.current)
    };
    let (label_name, depth2) = (name.clone(), depth.clone());
    let asking = {
        let name = name.clone();
        move || snaps.confirm.get().is_some_and(|c| c == name())
    };
    let restore = {
        let name = name.clone();
        move || {
            let name = name();
            if snaps.confirm.get_untracked().as_deref() == Some(name.as_str()) {
                snaps.confirm.set(None);
                snaps.run(|m| m.revert(&name));
            } else {
                snaps.confirm.set(Some(name));
            }
        }
    };
    let delete = move || {
        snaps.confirm.set(None);
        let name = name();
        snaps.run(|m| m.drop_snapshot(&name));
    };
    view! {
        <Row padding_x=Spacing::Md padding_y=Spacing::Xs gap=Spacing::Md align=Align::Center>
            <Row grow=1.0 gap=Spacing::Sm align=Align::Center>
                <Row width=move || Length::from(depth() as f32 * 18.0)/>
                <Text max_lines=1>
                    {move || format!("{}{}", if depth2() > 0 { "└ " } else { "" }, label_name())}
                </Text>
                <Show when=current>
                    <Text text_style=TextStyle::Caption>"current"</Text>
                </Show>
            </Row>
            <Text max_lines=1 width=TAKEN_W>{field(|s| s.date_label())}</Text>
            <Text max_lines=1 width=STATE_W>{field(|s| s.size_label())}</Text>
            <Row width=260 gap=Spacing::Sm justify=Justify::End>
                <Button enabled=move || !snaps.busy() @click=restore>
                    {move || if asking() { "Discard current state?" } else { "Restore" }.to_owned()}
                </Button>
                <Button enabled=move || !snaps.busy() @click=delete>"Delete"</Button>
            </Row>
        </Row>
    }
}
