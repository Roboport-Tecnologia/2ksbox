//! The machine's CD drive from the player's Disc menu (track M22 step 5):
//! the shared disc shelf, the disc in the drive checked, Eject, and a disc
//! image or folder picked here, which goes on the shelf and in the drive
//! at once, or a game's installer, unpacked onto the shelf and its disc
//! put in the drive (`launcher_core::installer`). The model is `launcher_core::shelf`, the launcher's shelf
//! window's, opened for the bundle the launcher named (`--machine`): an
//! insert swaps the tray through the machine's monitor (the `-qmp` socket
//! the launcher started it with) and sets the disc it boots with.
//!
//! The menu follows changes as they happen, never on a timer (user,
//! 2026-10-19): the launcher writing the shelf or the bundle (`watch.rs`),
//! and the tray moving, whoever moved it (the guest's own eject, its
//! `CDSHELF`, the launcher's Insert), which QEMU says on the player's own
//! monitor (`DEVICE_TRAY_MOVED`, hooked up in `main.rs`). Everything that
//! asks the monitor runs on a thread of its own: QEMU answers from its
//! main loop, which a snapshot can hold for seconds, and the window must
//! not wait for it.

use crate::wake::Wake;
use crate::watch;
use launcher_core::browse;
use launcher_core::disc_library::{self, DISC_FILTER};
use launcher_core::installer;
use launcher_core::shelf::Shelf;
use mitsuami::prelude::*;
use std::path::{Path, PathBuf};
use std::sync::Arc;

#[derive(Clone, Copy)]
pub struct Discs {
    bundle: Signal<PathBuf>,
    model: Signal<Shelf>,
    /// Insert Game Installer… is unpacking one.
    unpacking: Signal<bool>,
}

/// The shelf opened for the machine, its drive asked of the monitor.
fn load(bundle: &Path) -> Shelf {
    let mut shelf = Shelf::default();
    shelf.open_for_path(bundle.to_path_buf(), &disc_library::default_path());
    shelf
}

impl Discs {
    /// The shelf for `bundle`, followed for as long as the window is up,
    /// and what to call when the tray moves.
    pub fn open(bundle: PathBuf) -> (Discs, Arc<dyn Fn() + Send + Sync>) {
        let discs = Discs { model: signal(Shelf::default()), bundle: signal(bundle.clone()), unpacking: signal(false) };
        let changed = Arc::new(Wake::default());
        let watch = watch::watch(&[bundle, disc_library::default_path()], &[], &changed);
        let tray = changed.notifier();
        spawn_local(async move {
            let _watch = watch;
            loop {
                discs.reload().await;
                changed.next().await;
            }
        });
        (discs, tray)
    }

    /// The menu changes only when what it lists does.
    async fn reload(self) {
        let bundle = self.bundle.get_untracked();
        let fresh = spawn_blocking(move || load(&bundle)).await;
        if fresh.menu_discs() != self.model.with_untracked(Shelf::menu_discs) {
            self.model.set(fresh);
        }
    }

    /// Insert or eject, on the shelf as it is now, written at once; the
    /// guest's `CDSHELF` lists a disc added here as soon as the shelf is
    /// written.
    fn change(self, f: impl FnOnce(&mut Shelf) + Send + 'static) {
        spawn_local(self.apply(f));
    }

    /// `change`, for a caller that waits for it to end.
    async fn apply(self, f: impl FnOnce(&mut Shelf) + Send + 'static) {
        let bundle = self.bundle.get_untracked();
        let shelf = spawn_blocking(move || {
            let mut s = load(&bundle);
            f(&mut s);
            s.flush_reporting();
            if s.take_saved() {
                if let Some(dir) = s.bundle_dir() {
                    s.publish_to(dir);
                }
            }
            s
        })
        .await;
        let error = shelf.error().map(str::to_owned);
        self.model.set(shelf);
        if let Some(error) = error {
            crate::report("The disc was not changed", error);
        }
    }

    /// A disc image or a folder, picked in the platform's dialog, on the
    /// shelf and in the drive as soon as the dialog closes.
    fn pick(self, folder: bool) {
        let mut request = if folder {
            OpenFile::new().title("Insert a folder as a disc").directories()
        } else {
            OpenFile::new()
                .title("Insert a disc image")
                .filter(FileFilter::new(DISC_FILTER.0, browse::extensions(DISC_FILTER)))
                .filter(FileFilter::all("All files"))
        };
        if let Some(start) = browse::browse_start("", None) {
            request = request.start_folder(start);
        }
        spawn_local(async move {
            let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) else { return };
            let path = browse::picked(&path);
            browse::remember(&path);
            self.change(move |s| s.add_and_insert(path));
        });
    }

    /// A game's installer, picked in the platform's dialog: unpacked on a
    /// thread, onto the shelf, its disc in the drive when it is done. The
    /// menu item says so meanwhile, and refuses a second one.
    fn pick_installer(self) {
        let mut request = OpenFile::new()
            .title(installer::PICK_TITLE)
            .filter(FileFilter::new(installer::FILTER.0, browse::extensions(installer::FILTER)))
            .filter(FileFilter::all("All files"));
        if let Some(start) = browse::browse_start("", None) {
            request = request.start_folder(start);
        }
        spawn_local(async move {
            let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) else { return };
            let path = browse::picked(&path);
            browse::remember(&path);
            self.unpacking.set(true);
            self.apply(move |s| s.unpack_and_insert(&path)).await;
            self.unpacking.set(false);
        });
    }

    pub fn menu(self) -> Menu {
        let menu = Menu::new("Disc")
            .children_with(move || {
                let rows = self.model.with(Shelf::menu_discs);
                if rows.is_empty() {
                    return vec![MenuItem::new("No discs yet.").enabled(false)];
                }
                rows.into_iter()
                    .map(|disc| {
                        let (path, in_drive) = (disc.path, disc.in_drive);
                        MenuItem::new(disc.label).checked(in_drive).on_select(move || {
                            if !in_drive {
                                let path = path.clone();
                                self.change(move |s| s.insert(&path));
                            }
                        })
                    })
                    .collect()
            })
            .separator()
            .item(
                MenuItem::new("Eject")
                    .enabled(move || self.model.with(|s| s.drive().is_some()))
                    .on_select(move || self.change(Shelf::eject)),
            );
        if !Shelf::menu_can_pick() {
            return menu;
        }
        menu.separator()
            .item(MenuItem::new("Insert Disc Image…").on_select(move || self.pick(false)))
            .item(MenuItem::new("Insert Folder as Disc…").on_select(move || self.pick(true)))
            .item(
                MenuItem::new(move || {
                    if self.unpacking.get() { installer::UNPACKING_ITEM } else { installer::INSERT_ITEM }.to_owned()
                })
                .enabled(move || !self.unpacking.get())
                .on_select(move || self.pick_installer()),
            )
    }
}
