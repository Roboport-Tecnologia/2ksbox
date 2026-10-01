//! The disc shelf, over `launcher_core::shelf::Shelf`: opened on its own
//! it manages the shared shelf (add, rename, remove); opened from a
//! machine's "Discs" it also shows that machine's drive as a card above
//! the shelf, and each row's ▶ puts that disc in it. The rules are the
//! core's: Insert and Eject set the disc the machine boots with and, while
//! it runs, swap the disc now as well, and the card shows what the drive
//! holds at this moment, polled, because the guest swaps discs too.
//!
//! Rows are keyed by the disc's path and read their fields by it: the
//! shelf is kept in label order, so a row number is only good until the
//! next edit.
//!
//! A label is edited behind the pencil beside it and written when Enter
//! is pressed, when the field loses focus, or when the window closes with
//! the edit still in it, as the Qt window's `editingFinished`: the core
//! re-sorts the shelf on every label change, so writing each keystroke
//! would move the row being typed in.

use crate::machines::Library;
use crate::machines::icons;
use launcher_core::browse;
use launcher_core::disc_library::{self, DISC_FILTER, DiscKind};
use launcher_core::shelf::Shelf;
use mitsuami::prelude::*;
use std::path::{Path, PathBuf};
use std::time::Duration;

#[derive(Clone, Copy)]
pub struct Discs {
    model: Signal<Shelf>,
    /// The disc whose label is being edited, and what is typed so far.
    editing: Signal<Option<(PathBuf, String)>>,
    /// Something droppable is over the shelf.
    drop_over: Signal<bool>,
}

impl Store for Discs {
    fn create() -> Discs {
        Discs { model: signal(Shelf::default()), editing: signal(None), drop_over: signal(false) }
    }
}

impl Discs {
    fn read<R>(&self, f: impl FnOnce(&Shelf) -> R) -> R {
        self.model.with(f)
    }

    /// A library edit, written straight away: a shelf is a list of things
    /// you own, not a document being drafted (`shelf.rs`).
    fn edit(&self, f: impl FnOnce(&mut Shelf)) {
        self.model.update(|shelf| {
            f(shelf);
            shelf.flush_reporting();
        });
    }

    pub fn open_library(&self, library: Library) {
        self.editing.set(None);
        let path = library.disc_library_path();
        self.model.update(|s| s.open_library(&path));
    }

    pub fn open_for(&self, bundle: PathBuf, library: Library) {
        self.editing.set(None);
        let path = library.disc_library_path();
        self.model.update(|s| s.open_for_path(bundle, &path));
    }

    pub fn add(&self, path: &Path) {
        let path = path.to_path_buf();
        self.edit(|s| s.add(path));
    }

    pub fn insert(&self, path: &Path) {
        self.edit(|s| s.insert(path));
    }

    /// What the running machine's drive holds, asked every two seconds
    /// while a machine's shelf is up. The window is only touched when the
    /// answer changed.
    async fn poll(self) {
        loop {
            sleep(Duration::from_secs(2)).await;
            if !self.model.with_untracked(|s| s.open && s.for_machine()) {
                continue;
            }
            let probe = self.model.with_untracked(Shelf::probe_live);
            if self.model.with_untracked(|s| s.live_changed(&probe)) {
                self.model.update(|s| s.apply_live(probe));
            }
        }
    }

    /// Write the label being edited, if any.
    fn commit_label(&self) {
        let Some((path, label)) = self.editing.get_untracked() else { return };
        self.editing.set(None);
        self.edit(|s| {
            if let Some(row) = s.discs().iter().position(|d| d.path == path) {
                s.set_label(row, label.trim());
            }
        });
    }

    /// Close the window: write a label still being typed, then tell the
    /// machine list what changed (the boot disc shows there, and a
    /// running machine's guest reads the shelf through its drive).
    fn close(&self, library: Library) {
        self.commit_label();
        let mut saved = false;
        self.model.update(|s| {
            s.open = false;
            saved = s.take_saved();
        });
        library.refresh();
        if saved {
            library.republish_shelf();
        }
    }

    /// Add a disc image or a folder, picked in the platform's dialog. It
    /// is on the shelf as soon as the dialog closes.
    fn pick(&self, folder: bool) {
        let mut request = if folder {
            OpenFile::new().title("Share a folder with the guest").directories()
        } else {
            OpenFile::new()
                .title("Add a disc image")
                .multiple()
                .filter(FileFilter::new(DISC_FILTER.0, browse::extensions(DISC_FILTER)))
                .filter(FileFilter::all("All files"))
        };
        if let Some(start) = browse::browse_start("", None) {
            request = request.start_folder(start);
        }
        let discs = *self;
        spawn_local(async move {
            for path in open_file(request).await.unwrap_or_default() {
                let path = browse::picked(&path);
                browse::remember(&path);
                discs.add(&path);
            }
        });
    }
}

fn kind_icon(kind: DiscKind) -> &'static str {
    match kind {
        DiscKind::Disc => icons::DISCS,
        DiscKind::Folder => icons::FOLDER,
        DiscKind::GuestTools => icons::TOOLS,
    }
}

#[component]
pub fn DiscShelfWindow() -> impl View {
    let discs = use_store::<Discs>();
    let library = use_store::<Library>();
    // The window's task, which ends with it.
    spawn_local(discs.poll());
    let has_guest_tools = disc_library::guest_tools_iso().is_some();
    let add_menu = move || {
        (
            MenuItem::new("Disc image…").on_select(move || discs.pick(false)),
            MenuItem::new("Folder as disc…").on_select(move || discs.pick(true)),
            MenuSeparator::new(),
            MenuItem::new("Guest tools ISO")
                .enabled(has_guest_tools)
                .on_select(move || discs.edit(Shelf::add_guest_tools)),
        )
    };
    view! {
        <Window
            title=move || discs.read(Shelf::title)
            size=Size::new(760.0, 620.0)
            min_size=Size::new(560.0, 420.0)
            modal=Modality::Application
            open=move || discs.read(|s| s.open)
            @close_request=move || discs.close(library)
        >
            <Column padding=Spacing::Lg gap=Spacing::Md grow=1.0 min_height=0>
                {crate::shot::arm(&["discs", "shelf"])}
                <Show when=move || discs.read(Shelf::for_machine)>
                    <DriveCard/>
                </Show>
                <Row gap=Spacing::Sm align=Align::Center>
                    <Text text_style=TextStyle::Headline>"Library"</Text>
                    <Text text_style=TextStyle::Caption color=Color::SecondaryLabel grow=1.0>
                        {move || discs.read(Shelf::count_label)}
                    </Text>
                    <MenuButton icon=icons::NEW menu=add_menu()>"Add"</MenuButton>
                </Row>
                <Group
                    grow=1.0
                    min_height=0
                    file_drop={FileDrop::extensions(DISC_FILTER.1.iter().copied()).and_folders()}
                    @drop={move |paths: Vec<PathBuf>| {
                        discs.drop_over.set(false);
                        for path in paths {
                            discs.add(&path);
                        }
                    }}
                    @drop_hover={move |over: bool| discs.drop_over.set(over)}
                >
                    <Show when=move || discs.read(|s| s.discs().is_empty()) fallback=|| view! { <DiscList/> }>
                        <Column grow=1.0 align=Align::Center justify=Justify::Center>
                            <Text color=Color::SecondaryLabel>"No discs yet."</Text>
                        </Column>
                    </Show>
                    <Text text_style=TextStyle::Caption color=Color::SecondaryLabel align_self=Align::Center>
                        {move || if discs.drop_over.get() {
                            "Release to add"
                        } else {
                            "Drop disc images or folders here"
                        }.to_owned()}
                    </Text>
                </Group>
                <Show when=move || discs.read(|s| s.error().is_some())>
                    <Text color=Color::Error>{move || discs.read(|s| s.error().unwrap_or_default().to_owned())}</Text>
                </Show>
            </Column>
        </Window>
    }
}

/// The machine's drive: the disc in it, or an empty tray, and Eject.
#[component]
fn DriveCard() -> impl View {
    let discs = use_store::<Discs>();
    let card = move || discs.read(Shelf::drive_card);
    let has_disc = move || card().is_some_and(|c| c.kind.is_some());
    view! {
        <Group title="CD drive">
            <Row gap=Spacing::Md align=Align::Center>
                <Icon
                    name=move || card().and_then(|c| c.kind).map_or(icons::DISCS, kind_icon).to_owned()
                    color=move || if has_disc() { Color::Accent } else { Color::SecondaryLabel }
                    icon_size=32.0
                />
                <Column gap=Spacing::Xs grow=1.0 min_width=0>
                    <Text text_style=TextStyle::Title max_lines=1>{move || card().map(|c| c.title).unwrap_or_default()}</Text>
                    <Text text_style=TextStyle::Caption color=Color::SecondaryLabel max_lines=1 truncation=Truncation::Middle>
                        {move || card().map(|c| c.detail).unwrap_or_default()}
                    </Text>
                </Column>
                <Show when=has_disc>
                    <Button icon=icons::EJECT @click=move || discs.edit(Shelf::eject)>"Eject"</Button>
                </Show>
            </Row>
        </Group>
    }
}

#[component]
fn DiscList() -> impl View {
    let discs = use_store::<Discs>();
    view! {
        <List
            each=move || discs.read(|s| s.discs().iter().map(|d| d.path.clone()).collect::<Vec<_>>())
            key=|p: &PathBuf| p.clone()
            grow=1.0
            min_height=0
            let:path
        >
            <DiscRow path=path/>
        </List>
    }
}

/// One disc: its kind, its label with a pencil to rename it, what it is
/// and where it lives, then ▶ (or "In drive") on a machine's shelf, and
/// Remove.
#[component]
fn DiscRow(path: PathBuf) -> impl View {
    let discs = use_store::<Discs>();
    let path = std::rc::Rc::new(path);
    let p = {
        let path = path.clone();
        move || path.clone()
    };
    let (p1, p2, p3, p4, p5, p6, p7) = (p(), p(), p(), p(), p(), p(), p());
    let row = move |path: &Path| discs.read(|s| s.discs().iter().position(|d| d.path == path));
    let field = move |path: &Path, f: fn(&Shelf, usize) -> String| discs.read(|s| row(path).map(|r| f(s, r))).unwrap_or_default();
    let editing = {
        let path = p1.clone();
        move || discs.editing.with(|e| e.as_ref().is_some_and(|(p, _)| p == path.as_ref()))
    };
    let in_drive = {
        let path = p2.clone();
        move || discs.read(|s| row(&path).is_some_and(|r| s.row_in_drive(r)))
    };
    view! {
        <Row padding_x=Spacing::Md padding_y=Spacing::Xs gap=Spacing::Md align=Align::Center>
            <Icon name={let path = p3.clone(); move || {
                discs.read(|s| row(&path).and_then(|r| s.row_kind(r))).map_or(icons::DISCS, kind_icon).to_owned()
            }} color=Color::SecondaryLabel icon_size=20.0 />
            <Column gap=Spacing::Xs grow=1.0 min_width=0>
                <Show when={editing.clone()} fallback={let path = p4.clone(); move || {
                    let path = path.clone();
                    let label = {
                        let path = path.clone();
                        move || field(&path, |s, r| s.discs()[r].label.clone())
                    };
                    view! {
                        <Row gap=Spacing::Xs align=Align::Center>
                            <Text max_lines=1 shrink=1.0 min_width=0>{label.clone()}</Text>
                            <Button
                                icon=icons::EDIT
                                icon_only=true
                                button_style=ButtonStyle::Borderless
                                tooltip="Rename"
                                @click=move || discs.editing.set(Some((path.to_path_buf(), label())))
                            >"Rename"</Button>
                        </Row>
                    }
                }}>
                    <TextInput
                        a11y_label="Label"
                        value=move || discs.editing.with(|e| e.as_ref().map(|(_, t)| t.clone()).unwrap_or_default())
                        @input=move |text| discs.editing.update(|e| {
                            if let Some((_, typed)) = e {
                                *typed = text;
                            }
                        })
                        @submit=move || discs.commit_label()
                        @blur=move || discs.commit_label()
                    />
                </Show>
                <Text text_style=TextStyle::Caption color=Color::SecondaryLabel max_lines=1 truncation=Truncation::Middle>
                    {let path = p5.clone(); move || field(&path, Shelf::row_detail)}
                </Text>
            </Column>
            <Show when=move || discs.read(Shelf::for_machine)>
                <Show when={in_drive.clone()} fallback={let path = p6.clone(); move || {
                    let path = path.clone();
                    view! {
                        <Button
                            icon=icons::START
                            icon_only=true
                            tooltip="Insert"
                            @click=move || discs.insert(&path)
                        >"Insert"</Button>
                    }
                }}>
                    <Text text_style=TextStyle::Caption color=Color::Accent max_lines=1 shrink=0.0>"In drive"</Text>
                </Show>
            </Show>
            <Button
                icon=icons::TRASH
                icon_only=true
                button_style=ButtonStyle::Borderless
                tooltip="Remove from shelf"
                @click=move || {
                    let path = p7.clone();
                    discs.editing.update(|e| {
                        if e.as_ref().is_some_and(|(p, _)| p == path.as_ref()) {
                            *e = None;
                        }
                    });
                    discs.edit(|s| s.remove(&path));
                }
            >"Remove"</Button>
        </Row>
    }
}
