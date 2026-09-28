# Track M19: the launcher on mitsuami

Opened 2026-09-27 (user: "start the new mitsuami launcher"). ADR-023: a
front end on mitsuami, the user's own toolkit (platform widgets: AppKit,
WinUI 3, GTK 4, Kirigami), runs beside `launcher-qt` until it has every
window, then replaces it in the packages. Read `docs/00-status.md` first
for the track rules, doc 07 for what the launcher does, and M6's track
doc for the rules on working on the launcher (scratch libraries).

## Scope and files

`launcher-mitsuami/` (its own cargo workspace, like `launcher-qt`, so the
root build never needs GTK). The core stays M6's and ADR-014's: a
sentence or a rule a window needs goes into `launcher-core`, never here.
Nothing in `launcher-qt/` changes for this track until the flip.

## Building

```sh
cd launcher-mitsuami && cargo build              # GTK 4 (4.10+) on Linux
cargo build --no-default-features --features kde # Kirigami
```

mitsuami comes from a pinned `rev` in `Cargo.toml`. To work on both
repositories at once, override it in an uncommitted
`launcher-mitsuami/.cargo/config.toml`:

```toml
[patch."https://github.com/Roboport-Tecnologia/mitsuami"]
mitsuami = { path = "/path/to/mitsuami/crates/mitsuami" }
```

and bump `rev` to the pushed mitsuami commit before committing here.
Windows builds natively with MSVC and needs the Windows App Runtime 2.4.

## Test loop

The same scratch variables as `launcher-qt` (M6 "Rules for working on the
launcher"), then a shot on a private Broadway display, so nothing opens
on the desktop:

```sh
gtk4-broadwayd :7 &
LAUNCHER_LIBRARY_DIR=/tmp/lib LAUNCHER_DISC_LIBRARY=/tmp/discs.toml \
LAUNCHER_SHADER_PROFILES_DIR=/tmp/profiles \
GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 GTK_USE_PORTAL=0 \
LAUNCHER_SHOT=/tmp/main.png ./target/debug/launcher-mitsuami
```

`LAUNCHER_SHOT_TREE=1` also prints every node's kind and frame, which is
how a layout that comes out wrong is found. One such: **a container that
must fit its window needs `min_height=0`** (on the window's column and on
any row between it and a scroll view), because a flex item is at least as
tall as its content, as in CSS. Without it the form's long System page
pushed Cancel and Create out of the window instead of scrolling.

`LAUNCHER_SHOT` captures the window's content (not its title or toolbar)
after `LAUNCHER_SHOT_DELAY_MS` (800) and exits. `LAUNCHER_SCREEN` picks
the window and what it opens on:

| `LAUNCHER_SCREEN` | Shows |
|---|---|
| (unset) | the machine window |
| `wizard[:<family>[:<page>[:open]]]` | a fresh form, on `win98` / `xp` / `dos` / `other` and a page by its sidebar index; `:open` opens the optimizations list |
| `edit:<machine.toml>[:<page>]` | the form on a machine, as Edit… opens it |
| `clone:<machine.toml>[:same]` | the clone dialog on a machine, or sharing its disk |
| `clonego:<machine.toml>` | presses Clone, and shows the machine window once the copy has landed (writes into the library) |
| `snapshots:<machine.toml>[:ask=<name>]` | the snapshot tree, with a row's Restore asking |
| `shelf[:<disc>]` | the shared shelf, with a disc added through its Add field (writes the shelf) |
| `discs:<machine.toml>[:boot=<disc>]` | the shelf for a machine, with a disc ticked to boot with (writes the bundle) |
| `create:<family>:<name>` | fills a fresh form on an existing disk (`/dev/null`), submits it, prints `create: saved …`, and shows the machine window with the new row; writes into the library, so point `LAUNCHER_LIBRARY_DIR` at a scratch one | Broadway's screen is 1024
wide, so a 1060 window is cut at the right edge there. The debug verbs are
`launcher_core::cli`'s, as in every front end.

## Steps

1. **The machine window (done 2026-09-27).** The library as a platform
   list keyed by bundle directory (name, family, shader, Running / Play),
   the status line in the window's toolbar with its tooltip, the empty
   state, the 500 ms reap. Written with `view!`, `#[component]` and a
   `Store` (user: the easy macros). The buttons for windows not ported
   yet are there, disabled.
2. **The machine form (done 2026-09-27).** `src/wizard.rs`: one
   `Signal<Form>` in a `Wizard` store; every control reads it and every
   edit is `Form`'s own method, so what a field does to the ones under it
   stays the core's. An application-modal `Window` opened by New machine
   and Edit…, a platform list as the sidebar, one page per section, the
   core's lists, notes and warnings, the platform file dialog for the
   four path fields (the MT-32 ROMs pick a folder), `browse::picked` and
   `remember` on what it returns. Reopening the same machine returns to
   its page. Checked headless: every page, a DOS machine created (the
   core's DOS defaults in its `machine.toml`) and one opened for editing.
   Not yet driven by hand on a desktop. What mitsuami lacks for it:
   - **Text colour.** A warning is the Callout style, not the Qt
     window's amber: `Text` has no tone or colour.
   - **A dialog's start folder.** `OpenFile` has none, so Browse… opens
     where the platform likes, not at `browse::browse_start` (the field's
     own folder, or the last one picked).
   - The optimizations disclosure closes when the page changes (each page
     is rebuilt by its `Show`); the Qt window keeps it open.
3. **Clone, snapshots and the disc shelf (done 2026-09-27).** `clone.rs`,
   `snaps.rs`, `discs.rs`, each a store over its core model. Clone is a
   fit-height dialog that polls the copy's thread and puts the new row in
   the list. Snapshots is the tree, rows keyed by snapshot id (qcow2
   reuses an id once its snapshot is deleted, so a row reads its fields
   by key), Restore asking once, a poll while a live job runs. The shelf's
   rows are keyed by path (it is kept in label order); Boot is a
   checkbox (unticked: an empty tray), where Qt has a checkable button.
   Checked headless on a scratch machine with a real qcow2: a clone copied
   byte for byte, a three-snapshot tree built with `--snapshots` shown
   with its branch, discs added and one set to boot with. Not driven: a
   running machine's Insert / Eject and live snapshots, and typing a
   label. What mitsuami lacks for these:
   - **A text input's focus leaving.** A label is written on Enter, or
     when the window closes with the edit in it; Qt writes it when the
     field loses focus. Writing each keystroke would re-sort the row
     being typed in.
   - **An elide mode.** A disc's folder is cut at its end, where Qt cuts
     it at its start and keeps the useful part.
4. **Shader profiles and the editor with the live preview**
   (`launcher_core::preview` frames into an `Image`).
5. **First run** (the preset offer and its progress in the toolbar).
6. **What Qt does that mitsuami has no call for yet**: the app ID
   (Wayland's `app_id`, which the desktop entry is matched by; GTK takes
   the program name today), the window icon, a text colour for warnings,
   a file dialog's start folder (step 2), a text input's focus leaving
   and an elide mode (step 3).
7. **A `mitsuami` stage in `scripts/build.sh`, the offscreen shot in each
   packager, then the flip**: every package ships this as `2ksbox`,
   `launcher-qt` is deleted, ADR-015 is marked superseded, the Flatpak's
   runtime decided (GNOME, or `kde` on `org.kde.Platform`).
