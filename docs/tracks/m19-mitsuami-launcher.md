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
after `LAUNCHER_SHOT_DELAY_MS` (800) and exits. Broadway's screen is 1024
wide, so a 1060 window is cut at the right edge there. The debug verbs are
`launcher_core::cli`'s, as in every front end.

## Steps

1. **The machine window (done 2026-09-27).** The library as a platform
   list keyed by bundle directory (name, family, shader, Running / Play),
   the status line in the window's toolbar with its tooltip, the empty
   state, the 500 ms reap. Written with `view!`, `#[component]` and a
   `Store` (user: the easy macros). The buttons for windows not ported
   yet are there, disabled.
2. **The machine form** (`WizardWindow.qml`, the biggest): a settings
   window, a page per section, over `wizard::Form`; opened by New
   machine and Edit.
3. **Disc shelf, snapshots, clone** (the per-machine windows).
4. **Shader profiles and the editor with the live preview**
   (`launcher_core::preview` frames into an `Image`).
5. **First run** (the preset offer and its progress in the toolbar).
6. **What Qt does that mitsuami has no call for yet**: the app ID
   (Wayland's `app_id`, which the desktop entry is matched by; GTK takes
   the program name today) and the window icon.
7. **A `mitsuami` stage in `scripts/build.sh`, the offscreen shot in each
   packager, then the flip**: every package ships this as `2ksbox`,
   `launcher-qt` is deleted, ADR-015 is marked superseded, the Flatpak's
   runtime decided (GNOME, or `kde` on `org.kde.Platform`).
