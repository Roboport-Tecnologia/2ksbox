//! The disc shelf, over `launcher_core::shelf::Shelf`: opened on its own
//! it manages the shared shelf (add, label, remove); opened from a
//! machine's "Discs…" it also sets the disc that machine boots with and,
//! while it runs, swaps discs in its drive. The rules are the core's.
//!
//! Rows are keyed by the disc's path and read their fields by it: the
//! shelf is kept in label order, so a row number is only good until the
//! next edit.
//!
//! A label is written when Enter is pressed in its field, when the field
//! loses focus, or when the window closes with the edit still in it, as
//! the Qt window's `editingFinished`: the core re-sorts the shelf on every
//! label change, so writing each keystroke would move the row being typed
//! in.

use crate::machines::Library;
use crate::path_field::PathField;
use launcher_core::browse;
use launcher_core::disc_library::{self, DISC_FILTER, Disc};
use launcher_core::shelf::Shelf;
use mitsuami::prelude::*;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

const LABEL_W: f32 = 190.0;
const NAME_W: f32 = 170.0;

#[derive(Clone, Copy)]
pub struct Discs {
    model: Signal<Shelf>,
    /// Whether the machine the shelf was opened for is up: Insert and
    /// Eject are live then.
    running: Signal<bool>,
    /// Labels typed and not written yet, by disc.
    drafts: Signal<HashMap<PathBuf, String>>,
    /// The "Add disc" field.
    adding: Signal<String>,
}

impl Store for Discs {
    fn create() -> Discs {
        Discs {
            model: signal(Shelf::default()),
            running: signal(false),
            drafts: signal(HashMap::new()),
            adding: signal(String::new()),
        }
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

    fn reset(&self, running: bool) {
        self.running.set(running);
        self.drafts.set(HashMap::new());
        self.adding.set(String::new());
    }

    pub fn open_library(&self, library: Library) {
        self.reset(false);
        let path = library.disc_library_path();
        self.model.update(|s| s.open_library(&path));
    }

    pub fn open_for(&self, bundle: PathBuf, library: Library, running: bool) {
        self.reset(running);
        let path = library.disc_library_path();
        self.model.update(|s| s.open_for_path(bundle, &path));
    }

    pub fn add(&self, path: &str) {
        let path = path.trim();
        if !path.is_empty() {
            let path = PathBuf::from(path);
            self.edit(|s| s.add(path));
        }
    }

    /// The Boot checkbox: this disc in the drive when the machine starts,
    /// or an empty tray.
    pub fn set_boot(&self, path: Option<PathBuf>) {
        self.edit(|s| {
            s.set_boot(path);
        });
    }

    fn field<R: Default>(&self, path: &Path, f: impl FnOnce(&Shelf, &Disc) -> R) -> R {
        self.read(|s| s.discs().iter().find(|d| d.path == path).map(|d| f(s, d)).unwrap_or_default())
    }

    fn row_of(shelf: &Shelf, path: &Path) -> Option<usize> {
        shelf.discs().iter().position(|d| d.path == path)
    }

    /// Write one disc's typed label, if it has one.
    fn commit_label(&self, path: &Path) {
        let Some(label) = self.drafts.with_untracked(|d| d.get(path).cloned()) else { return };
        self.drafts.update(|d| {
            d.remove(path);
        });
        self.edit(|s| {
            if let Some(row) = Self::row_of(s, path) {
                s.set_label(row, label.trim());
            }
        });
    }

    /// Close the window: write what is still typed, then tell the machine
    /// list what changed (the boot disc shows there, and a running
    /// machine's guest reads the shelf through its drive).
    fn close(&self, library: Library) {
        let typed: Vec<PathBuf> = self.drafts.with_untracked(|d| d.keys().cloned().collect());
        for path in typed {
            self.commit_label(&path);
        }
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

    /// A folder to share, picked in the platform's dialog. A disc file is
    /// the Add disc field's `PathField`.
    fn share_folder(&self) {
        let mut request = OpenFile::new().title("Share a folder with the guest").directories();
        if let Some(start) = browse::browse_start("", None) {
            request = request.start_folder(start);
        }
        let discs = *self;
        spawn_local(async move {
            if let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) {
                let path = browse::picked(&path);
                browse::remember(&path);
                discs.add(&path.display().to_string());
            }
        });
    }
}

#[component]
pub fn DiscShelfWindow() -> impl View {
    let discs = use_store::<Discs>();
    let library = use_store::<Library>();
    let guest_tools = disc_library::guest_tools_iso();
    let has_guest_tools = guest_tools.is_some();
    let guest_tools_tip = match &guest_tools {
        Some(iso) => iso.display().to_string(),
        None => "None built yet (guest-tools/build-wrappers.sh)".to_owned(),
    };
    view! {
        <Window
            title=move || discs.read(Shelf::title)
            size=Size::new(880.0, 600.0)
            min_size=Size::new(620.0, 360.0)
            modal=Modality::Application
            open=move || discs.read(|s| s.open)
            @close_request=move || discs.close(library)
        >
            <Column padding=Spacing::Lg gap=Spacing::Sm grow=1.0 min_height=0>
                {crate::shot::arm(&["discs", "shelf"])}
                <Show when=move || !discs.read(Shelf::for_machine)>
                    <Text text_style=TextStyle::Caption>
                        "Discs any machine can use. Pick one to boot with; swap the others in while the machine runs."
                    </Text>
                </Show>
                <Show when=move || discs.read(Shelf::for_machine)>
                    <Row gap=Spacing::Sm align=Align::Center>
                        <Text>{move || format!("Boots with: {}", discs.read(Shelf::boot_label))}</Text>
                        <Button
                            enabled=move || discs.read(|s| s.boot().is_some())
                            @click=move || discs.edit(|s| {
                                s.set_boot(None);
                            })
                        >"Boot with empty tray"</Button>
                    </Row>
                </Show>
                <Show when=discs.running>
                    <Row gap=Spacing::Sm align=Align::Center>
                        <Text text_style=TextStyle::Caption grow=1.0>
                            "The machine is running. Insert swaps the disc now, and the boot choice applies on the next start."
                        </Text>
                        <Button @click=move || discs.model.update(Shelf::eject_live)>"Eject"</Button>
                    </Row>
                </Show>
                <Show when=move || discs.read(|s| s.discs().is_empty()) fallback=|| view! { <DiscList/> }>
                    <Column grow=1.0 align=Align::Center justify=Justify::Center>
                        <Text>"No discs yet."</Text>
                    </Column>
                </Show>
                // A disc chosen in the dialog goes on the shelf at once.
                <PathField
                    label="Add disc"
                    filter=DISC_FILTER
                    value=discs.adding
                    @edit=move |p| discs.adding.set(p)
                    @pick=move |p: String| {
                        discs.add(&p);
                        discs.adding.set(String::new());
                    }
                    @submit=move |()| {
                        discs.add(&discs.adding.get_untracked());
                        discs.adding.set(String::new());
                    }
                />
                <Row gap=Spacing::Sm shrink=0.0>
                    <Button
                        enabled=move || !discs.adding.get().trim().is_empty()
                        @click=move || {
                            discs.add(&discs.adding.get_untracked());
                            discs.adding.set(String::new());
                        }
                    >"Add to shelf"</Button>
                    <Button tooltip="Share a folder with the guest as a disc" @click=move || discs.share_folder()>
                        "Add folder…"
                    </Button>
                    <Button
                        enabled=has_guest_tools
                        tooltip=guest_tools_tip.clone()
                        @click=move || discs.edit(Shelf::add_guest_tools)
                    >"Add guest-tools ISO"</Button>
                </Row>
                <Show when=move || discs.read(|s| s.status().is_some())>
                    <Text text_style=TextStyle::Caption>{move || discs.read(|s| s.status().unwrap_or_default().to_owned())}</Text>
                </Show>
                <Show when=move || discs.read(|s| s.error().is_some())>
                    <Text color=Color::Error>{move || discs.read(|s| s.error().unwrap_or_default().to_owned())}</Text>
                </Show>
            </Column>
        </Window>
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

/// One disc: Insert (while the machine runs), Boot (for a machine),
/// Remove, its label, its file name and its folder.
#[component]
fn DiscRow(path: PathBuf) -> impl View {
    let discs = use_store::<Discs>();
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| path.display().to_string());
    let dir = path.parent().map(|d| d.display().to_string()).unwrap_or_default();
    let full = path.display().to_string();
    let path = std::rc::Rc::new(path);
    let p = {
        let path = path.clone();
        move || path.clone()
    };
    let (p1, p2, p3, p4, p5, p6, p7) = (p(), p(), p(), p(), p(), p(), p());
    let label = move || {
        discs.drafts.with(|d| d.get(p1.as_path()).cloned()).unwrap_or_else(|| discs.field(&p1, |_, d| d.label.clone()))
    };
    view! {
        <Row padding_x=Spacing::Md padding_y=Spacing::Xs gap=Spacing::Sm align=Align::Center>
            <Show when=discs.running>
                <Button @click={let path = p2.clone(); move || {
                    let path = path.clone();
                    discs.model.update(|s| s.insert_live(&path));
                }}>"Insert"</Button>
            </Show>
            <Show when=move || discs.read(Shelf::for_machine)>
                <Checkbox
                    tooltip="Put this disc in the drive when the machine starts"
                    checked={let path = p3.clone(); move || discs.field(&path, |s, d| s.boot() == Some(d.path.as_path()))}
                    @change={let path = p4.clone(); move |on| {
                        discs.set_boot(on.then(|| path.to_path_buf()));
                    }}
                >"Boot"</Checkbox>
            </Show>
            <Button @click=move || {
                let path = p5.clone();
                discs.drafts.update(|d| {
                    d.remove(path.as_path());
                });
                discs.edit(|s| s.remove(&path));
            }>"Remove"</Button>
            <TextInput
                width=LABEL_W
                shrink=0.0
                a11y_label="Label"
                value=label
                @input={let path = p6.clone(); move |text| discs.drafts.update(|d| {
                    d.insert(path.to_path_buf(), text);
                })}
                @submit=move || discs.commit_label(&p7)
                @blur=move || discs.commit_label(&path)
            />
            <Text max_lines=1 width=NAME_W shrink=0.0>{name}</Text>
            // Cut at its end: mitsuami's Text has no elide mode, and the Qt
            // window cuts a folder at its start (track doc, step 3).
            <Text text_style=TextStyle::Caption max_lines=1 grow=1.0 min_width=0 tooltip=full>{dir}</Text>
        </Row>
    }
}
