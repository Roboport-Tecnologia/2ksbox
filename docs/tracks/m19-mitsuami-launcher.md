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

`LAUNCHER_SHOT` captures the window's content (not its title or toolbar)
after `LAUNCHER_SHOT_DELAY_MS` (800) and exits. `LAUNCHER_SCREEN` picks
the window and what it opens on:

| `LAUNCHER_SCREEN` | Shows |
|---|---|
| (unset) | the machine window |
| `wizard[:<family>[:<page>]]` | a fresh form, on `win98` / `xp` / `dos` / `other` and a page by its sidebar index |
| `edit:<machine.toml>[:<page>]` | the form on a machine, as Edit… opens it |
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
3. **Disc shelf, snapshots, clone** (the per-machine windows).
4. **Shader profiles and the editor with the live preview**
   (`launcher_core::preview` frames into an `Image`).
5. **First run** (the preset offer and its progress in the toolbar).
6. **What Qt does that mitsuami has no call for yet**: the app ID
   (Wayland's `app_id`, which the desktop entry is matched by; GTK takes
   the program name today), the window icon, a text colour for warnings
   and a file dialog's start folder (step 2).
7. **A `mitsuami` stage in `scripts/build.sh`, the offscreen shot in each
   packager, then the flip**: every package ships this as `2ksbox`,
   `launcher-qt` is deleted, ADR-015 is marked superseded, the Flatpak's
   runtime decided (GNOME, or `kde` on `org.kde.Platform`).
