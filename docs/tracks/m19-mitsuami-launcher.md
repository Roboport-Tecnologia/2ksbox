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

`LAUNCHER_SHOT` captures the window's content (not its title, toolbar
or sidebar) after `LAUNCHER_SHOT_DELAY_MS` (800) and exits. To see a
whole window, sidebar and all, keep it open instead (no `LAUNCHER_SHOT`)
and photograph the screen it is on:

- GTK: Broadway serves its screen as a web page, `http://127.0.0.1:8080`
  plus the display number (`:18` is port 8098); open it in a browser and
  take a screenshot.
- KDE: Qt's VNC platform, `QT_QPA_PLATFORM=vnc:port=5917:size=1200x800`,
  then `tools/rfb-shot.py 5917 <out.png>`.

`LAUNCHER_SCREEN` picks the window and what it opens on:

| `LAUNCHER_SCREEN` | Shows |
|---|---|
| (unset) | the machine window |
| `select:<machine.toml>` | the machine window with that machine chosen, as a click on its row |
| `wizard[:<family>[:<page>[:open]]]` | a fresh form, on `win98` / `xp` / `dos` / `other` / `win11` and a page by its sidebar index; `:open` opens the optimizations list |
| `edit:<machine.toml>[:<page>]` | the form on a machine, as Edit… opens it |
| `clone:<machine.toml>[:same]` | the clone dialog on a machine, or sharing its disk |
| `clonego:<machine.toml>` | presses Clone, and shows the machine window once the copy has landed (writes into the library) |
| `snapshots:<machine.toml>` | the snapshot tree |
| `takesnapshot:<machine.toml>` | the snapshot tree with Take snapshot's name sheet open |
| `shelf[:<disc>]` | the shared shelf, with a disc added as the Add menu would (writes the shelf) |
| `discs:<machine.toml>[:insert=<disc>]` | the shelf for a machine, with a disc's ▶ pressed (writes the bundle) |
| `profiles` | the shader profile list |
| `saveprofile:<preset>` | a new profile "Probe profile" on a preset, saved through the core, and the list; prints `saveprofile: saved …` (writes into the profile directory) |
| `editor:<preset>[;<image>[;<param>=<value>]]` | the editor on a preset and a picture, the preview rendered, with one parameter overridden as its box and slider would (`LAUNCHER_SHOT_DELAY_MS=2500`: the first render makes a device) |
| `firstrun[:<answers>]` | the first-run offer with scripted answers (`yes`, `no`, `retry`, `cancel`, `ok`, comma-separated) in place of the platform's alerts: each prints `firstrun <Step>: <headline> \| <detail> [<buttons>]`, and a run ends with `firstrun settled: open=…`. With `LAUNCHER_SHADERS_DIR` on an empty folder it asks; `/proc/nowhere/shaders` makes the download fail at once, with no network |
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
   **Reworked 2026-10-01 (user: "more like UTM / VirtualBox"):** a
   platform list of machines down the leading side (an icon, the name,
   and the core's `subtitle`, "XP · Stopped"), selection by bundle
   directory with the first machine chosen until one is, and beside it a
   details pane: the name, with Start (the window's default button) and
   a More menu button (Settings, Discs, Snapshots, Clone) at its right on
   the same row since 2026-10-01 (user), and the core's `details` (a
   file's row one line, cut in the middle, its whole path the tooltip;
   the hard disk shows its whole path, user 2026-10-01), one
   `Group` per page of
   the form, rebuilt per group by a keyed `For`. Activating a row starts
   its machine. The toolbar keeps New, Shelf, Shaders (named so since
   2026-10-01, user; an empty library's button stays New machine),
   with platform icons, and the status line, which says only
   that a machine started: a start that fails is the platform's alert
   since 2026-10-01 (user), the core's `Machines::start_failed` headline
   over its error (checked on Broadway with `LAUNCHER_PLAYER_BIN` on a
   missing file; an alert's text can't be selected, mitsuami has no
   option for it); an empty library
   is a title, its folder and New machine. These buttons have no "…"
   (user): toolbar and action buttons go without it on every platform
   today, as UTM's and VirtualBox's do. `Sidebar` was not used: its
   items are fixed when it is built and have no second line. Checked
   headless on GTK and KDE with three machines (`select:` on the XP one
   shows its Direct3D row; DOS has none). On KDE Breeze's scroll bar is
   drawn over the details' right edge, as mitsuami leaves it no room.
2. **The machine form (done 2026-09-27).** `src/wizard.rs`: one
   `Signal<Form>` in a `Wizard` store; every control reads it and every
   edit is `Form`'s own method, so what a field does to the ones under it
   stays the core's. An application-modal `Window` opened by New machine
   and Edit…, the window's `Sidebar` for the pages (since 2026-10-01,
   user; a platform list before), each with the platform's icon (the
   window's 650 is its content's, the sidebar added to it; on KDE the
   column is narrowed to 10 grid units through `Sidebar::native`, from
   Kirigami's 20), one page per section, the
   core's lists, notes and warnings, the platform file dialog for the
   four path fields (the MT-32 ROMs pick a folder), `browse::picked` and
   `remember` on what it returns. Reopening the same machine returns to
   its page. Checked headless: every page, a DOS machine created (the
   core's DOS defaults in its `machine.toml`) and one opened for editing.
   Not yet driven by hand on a desktop. What mitsuami lacks for it:
   - Text colour: done in step 6 (mitsuami 0e474a9, `Color::Warning`).
   - A dialog's start folder: done in step 6 (mitsuami 6beec2e).
   - The optimizations disclosure closes when the page changes (each page
     is rebuilt by its `Show`); the Qt window keeps it open.
3. **Clone, snapshots and the disc shelf (done 2026-09-27).** `clone.rs`,
   `snaps.rs`, `discs.rs`, each a store over its core model. Clone is a
   dialog whose height follows its content (`FollowHeight`, as the Qt
   one's is bound to it: the warning and the progress bar grow it) that
   polls the copy's thread and puts the new row in the list. Snapshots is the tree, rows keyed by snapshot id (qcow2
   reuses an id once its snapshot is deleted, so a row reads its fields
   by key), Restore asking once, a poll while a live job runs. Since
   2026-10-01 (user) the tree is a `Table` (name indented under its
   parent with the "current" mark, taken, VM state, Restore and a trash
   button, tooltip "Delete") and Take snapshot is on the window's
   toolbar: it opens a sheet (`Modality::Window`) asking for the name,
   empty at each opening, Take snapshot its default button. Restore and
   Delete ask in the platform's alert, Cancel the default (the questions
   are the core's `Snapshots::restore_question` / `delete_question`), so
   the actions column is only as wide as Restore and the trash button;
   the Qt window still asks on Restore's button and deletes without
   asking. Checked by clicks through Broadway's page on a scratch tree: a
   snapshot taken from the sheet landed under the current one, Cancel
   took none; in each alert Cancel changed nothing, Restore moved the
   current snapshot, Delete removed the row. The shelf's
   rows are keyed by path (it is kept in label order).
   **The shelf redone 2026-10-01 (user's design):** opened on a machine
   it has the drive as a card on top, with no heading (user: it is
   plain what it is) (the disc's kind icon, its label, the
   core's detail line, Eject; or "Tray empty"), then "Library" with the
   core's count and an Add menu button (Disc image…, Folder as disc…,
   Guest tools ISO, checked once it is on the shelf), then the rows, and
   under them a `Group` of its own that takes dropped images and folders.
   The rows are a plain column in a `ScrollView`, not a `List`, so they
   sit on the window's background with no frame (user); the scroll view
   runs to the window's edges with the side padding inside it, so the
   scroll bar stays clear of the buttons (user). A row is its
   kind's icon, the label with a pencil floating past its end (absolute,
   taking no room; to show only on hover once mitsuami has hover, which
   the user is adding) that turns it into a field (written on Enter or
   focus leaving), the
   core's "Disc image · ~/path" line, ▶ (Insert) or "In drive", and the
   trash button. The rules are doc 07's "One drive": Insert sets the boot
   disc, and on a running machine swaps the disc now too; the card polls
   the running drive every 2 s and only touches the window when it
   changed (`probe_live` / `live_changed`). The typed-path field is gone.
   Checked headless on Broadway: the card with a disc and empty, five
   rows of the three kinds. Not driven: the pencil, a drop, the menu.
   Checked headless on a scratch machine with a real qcow2: a clone copied
   byte for byte, a three-snapshot tree built with `--snapshots` shown
   with its branch, discs added and one set to boot with. Not driven: a
   running machine's Insert / Eject and live snapshots, and typing a
   label. What mitsuami lacks for these:
   - A text input's focus leaving: done in step 6 (`@blur`).
   - An elide mode: done in step 6 (mitsuami 4cdac37, `Truncation`).
4. **Shader profiles and the editor with the live preview (done
   2026-09-27).** `shaders.rs`: the profile list (a `Table` since
   2026-10-01, user: name, preset, a `Switch` for the default, Edit, and
   a trash button, tooltip "Delete", that asks first in the platform's
   alert, Cancel the default (the question is the core's
   `shader_library::delete_question`); a row opens in the editor when
   activated; checked by clicks through Broadway's page: the switch moved
   the default, Cancel kept the file, Delete removed it; New and a clear
   icon, tooltip "No default", in the window's toolbar (user); the
   preset collection's download
   row) and the editor
   (name, preset, the parameters as box + slider + description, the
   preview image). The preview is the core's render path
   (`launcher_core::preview`) and its frame goes into an `Image` as
   pixels, with no BMP on disk as in Qt. Checked headless: the CRT
   Aperture preset over an XP screenshot, the same with BRIGHTNESS
   overridden to 0.3 (the preview renders again, darker), and a profile
   saved and listed. Not driven: dragging a slider, the download, an
   animated preset. Two things learned:
   - **The preview follows its area's size** (`use_size` on a
     `node_ref`, since step 6; a 30 ms tick before). One effect renders
     when the size changes or the inputs are stale; an animated preset
     sleeps one frame interval and marks the preview stale, so a still
     one runs no timer. That wake-up is spawned with `Ui::spawn_local`:
     the free `spawn_local` belongs to the effect's scope, which each
     run disposes, so the effect's own `stale = false` cancelled it and
     the animation stopped after one frame. The effect also reads
     `stale` before any `||`, or it stops following it.
   - **A picture bigger than the area is cut, never scaled down.** The
     core renders it at scale 1, larger than the area, and the caller
     shows its centre (the player crops the same way; the Qt window
     clips its rectangle). mitsuami has no clip, so `render` crops the
     frame's pixels to the area before they become the `Image`, which
     never pushes the layout. Checked headless: a 1600x900 picture shows
     its centre filling the preview on KDE. CRT Aperture on a 440-line
     picture at exactly 1x is black in the core too (`--preview-shader`):
     its `floor(OutputSize.y * SourceSize.w)` is 0 there, a float that
     falls just short of 1.0; the preset's, not ours.
   - **The preview's wgpu device must not be dropped at exit.** The
     reactive runtime is torn down with the main thread's thread-locals,
     and wgpu's queue, dropped then, touched a wgpu thread-local already
     gone: the process aborted after a shot was written. The device is
     dropped when the editor closes and held in a `ManuallyDrop`, so one
     still alive at exit is never dropped.
5. **First run (done 2026-09-27).** `firstrun.rs`: on a start with no
   preset collection and no `first-run.txt`, the core's question in the
   platform's alert (Yes / No); a yes shows the download in the machine
   window's toolbar (since 2026-10-01 only a small indeterminate progress
   bar, user; the core's headline is its label), then the outcome in
   another alert (OK, or Retry / Cancel on a failure); a collection that
   landed refreshes the profile manager and the Shader column. Every
   other headless screen runs without it. Checked headless with scripted
   answers, the Qt check's sequence: declining writes the marker, the
   next start asks nothing, a yes onto a download that can't succeed
   comes back as Retry / Cancel with the core's failure line. Not seen:
   the real alerts on a desktop. A download that runs was seen on
   Broadway on 2026-10-01 (the bar, ~3 s for the collection). The core's `firstrun::TITLE` has no place: a platform alert
   has a headline and a message, not a window title.
   - **Fixed after a report from the user: the KDE build never offered.**
     The offer asked while the machine window was still being built.
     Kirigami's alert lives in a window's overlay, and with no window it
     answers its last button at once, here "No", which the core recorded
     as the user's answer (GTK's alert needs no window, so GTK asked).
     The offer now waits on a 1 ms timer, which resumes only after a
     tick has committed the window, and names that window as the alert's
     parent. Checked on Qt's offscreen platform: the question shows over
     the window and no marker is written. A mitsuami issue too: an alert
     with no window to go in should wait for one, not answer.
6. **What Qt does that mitsuami has no call for yet**. The app ID, name
   and icon came with mitsuami cf35de4 (`App::id`, `name`, `icon` in
   `main.rs`: `paths::APP_ID` and the 256 px PNG `launcher-qt` uses);
   under Sway the window's `app_id` is `com._2ksbox.Launcher`. GTK shows
   only the theme's icon by that name, so a run from the build has none,
   as with Qt on Wayland. mitsuami 0e474a9 (2026-10-01) gave `Text` a
   colour, the platform's own where Qt fixes one: warnings are
   `Color::Warning` (amber on GTK, Breeze's orange on KDE), every error
   line `Color::Error` (they were the Callout style), the preview's
   placeholder `SecondaryLabel`, and since 2026-10-01 every note under a
   form control `SecondaryLabel` too (user), each at its note's size as
   Qt does
   (checked headless on `clone:<machine>:same`, which shows both). A disc label is now also written when its field loses
   focus (`@blur`, as Qt's `editingFinished`; not driven headless).
   mitsuami 4cdac37 closed the last two: `Truncation::Start` cuts a
   disc's folder and a profile's preset path at their start, as Qt's
   `ElideLeft` (checked headless on GTK and KDE with a long folder), and
   the shader preview follows its area through `use_size` (step 4;
   checked headless: a still preset renders once per size, CRT Beans VGA
   about 11 times a second, BRIGHTNESS 0.3 renders darker). What step 6
   asked of mitsuami is done.
   Widgets mitsuami won't have are ours, as `#[component]`s in their own
   module. The first is `src/path_field.rs`, `PathField` (the caption,
   the text input and Browse…, as `PathField.qml`): the form, the shader
   editor use it (the shelf's Add disc field until 2026-10-01). `@edit`
   gets a typed or picked path. What its dialog needed went into mitsuami 6beec2e
   (`OpenFile::start_folder`, `FileFilter::all`), so it does what the Qt
   field does: it opens at `browse::browse_start` (the field's folder,
   else `empty_dir`, which the preset field sets to the preset
   collection, else the last folder browsed), and "All files" follows the
   field's filter. The shelf's Add menu starts at the last folder too.
   `LAUNCHER_PICK=<label>=<path>` is the probe, as `pickdisc` is for Qt:
   the field with that caption prints the dialog it would open
   (`pick <label>: start …, filters […]`) and takes `<path>` as its
   answer.
7. **A `mitsuami` stage in `scripts/build.sh`, the offscreen shot in each
   packager, then the flip**: every package ships this as `2ksbox`,
   `launcher-qt` is deleted, ADR-015 is marked superseded, the Flatpak's
   runtime decided (GNOME, or `kde` on `org.kde.Platform`).
