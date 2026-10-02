# Track M6: the launcher

The machine library, the settings form, the disc shelf, snapshots, shader
profiles and the packages. M6 is done (doc 08), but the launcher keeps
changing and a session working on it starts here. The design is doc 07
(player vs. launcher, the core/front-end split, every form field, the
install layout, what any front end owes); the decisions are ADR-009
(licence), ADR-011 (names), ADR-014 (one core), ADR-017 (egui deleted)
and ADR-023 (mitsuami, which superseded ADR-015's Qt). Since 2026-10-02
the one front end, `launcher-mitsuami/`, is track M19's
(`docs/tracks/m19-mitsuami-launcher.md`); M6 keeps the core under it and
the packages (user: "remove the other launchers code completely. only
mitsuami launcher will be used now"). Every tool below is in
`docs/testing.md`.

## Scope and files

| Owned | What it is |
|---|---|
| `launcher-core/` | everything the launcher decides: the bundle (`bundle.rs`, `machine.toml`), library, disc shelf, shader profiles and sources, paths, spawning the player, QMP control, snapshots, preview, clone, host GPU probe, and one model per window (`machines`, `wizard`, `shelf`, `snaps`, `editor`, `firstrun`) with its sentences; `cli.rs` holds every toolkit-free debug verb, `src/bin/launcherx.rs` their binary |
| `launcher-capi/` | the same models as a C ABI (`include/launcher_core.h`); `examples/smoke.c` is a test |
| `shader-chain/` | the librashader (slang) filter chain on wgpu, linked into the player *and* the launcher's preview, so rebuild and retest both |
| `packaging/`, `scripts/package-{linux,flatpak,macos,windows}.sh`, `scripts/gen-flatpak-cargo-sources.sh`, `scripts/gen-icons.sh` | the packages and their self-checks |

Shared, so edit minimally and say so in the commit: `launcher-mitsuami/`
(M19's; a sentence or rule it needs comes here, to the core),
`player/build.rs`'s rpath, the root `Cargo.toml`, `scripts/test.sh`.
`guest-tools/src/cdshelf.{c,asm}` and patch 52 are the shelf's in-guest
half.

The egui front end `launcher/` was deleted (ADR-017), and the Qt one,
`launcher-qt/`, on 2026-10-02 (ADR-023). Don't bring either back or cite
their verbs and probes (`--diag-*-frame`, `--pick-file`,
`LAUNCHER_QT_SCREEN`, the `qt-*` checks).

## Building

```sh
cargo build --release              # launcher-core + launcherx (root workspace)
cargo check --release --workspace  # keeps launcher-capi compiling
scripts/build.sh mitsuami          # the launcher; GTK 4.10+ dev files on Linux
```

`launcher-mitsuami` sits outside the root workspace so a plain `cargo
build` never needs GTK; on Windows it is built only on a PC, with
`scripts/build-windows.sh mitsuami` (track M19, "Building"). A
dependency change in either lock file means running
`scripts/gen-flatpak-cargo-sources.sh`, or the offline Flatpak build
breaks.

## Test loop

No clicking and no unit tests. Three layers, all in `scripts/test.sh host`:

- **The models, without a toolkit.** `launcherx` verbs (`--new`,
  `--wizard-new`, `--wizard-edit <bundle> [fields…]` with `-` keeping a
  field, `--print-args`, `--print-player-args`, `--discs`, `--boot-disc`,
  `--snapshots [--live]`, `--insert-disc`, `--clone`, `--first-run`,
  `--default-profiles`, `--preview-shader`, `--browse-start`,
  `--optimizations`, `--host-check`, `--paths`, `--diagnose`, …; usage in
  `cli.rs`). The launcher answers the same verbs from the same code.
  Checks: `optimizations`, `pointer`, `extra-args`, `display-adapter`,
  `d3d9`, `voodoo2`, `music`, `pad`, `hpet`, `family-other`,
  `host-check`, `shelforder`, `dirshelf`, `clone`, `shader-defaults`,
  `preview-anim`, and `capi` (the C smoke). Most end with our own
  `qemu-system-i386` accepting the exact line `--print-args` wrote.
- **The real window, headless.** `LAUNCHER_SCREEN=<screen>` picks the
  window and what it does (the table is in track M19's "Test loop"), and
  `LAUNCHER_SHOT=<png>` draws it into a PNG and exits; on Linux on a
  private Broadway display (`gtk4-broadwayd`, `GDK_BACKEND=broadway`),
  so nothing opens on the desktop. The `mitsuami` check grabs the main
  window, creates a machine through the form (`create:xp:Probe box`),
  answers the first-run offer No (`firstrun:no`: the model's headline,
  and the marker written) and grabs About.
- **The packages.** The `package` check runs `package-linux.sh --no-tar`
  (`package-macos.sh` on a Mac), which asks the *staged* launcher and
  player, in a scrubbed environment, to resolve every companion, and
  requires the staged launcher's `LAUNCHER_SHOT` PNG. `icons` is
  `gen-icons.sh --check`. `scripts/package-flatpak.sh` runs its own
  in-sandbox checks and is not in the suite.

Beyond the suite: `tools/dos-guest-test.py` drives `launcherx` for the DOS
family end to end; `tools/cdshelf-guest-test.sh <image> xp` is the
in-guest shelf on a real XP.

## Rules for working on the launcher

- **A decision goes in `launcher-core`, never in the front end**,
  including family defaults, labels, notes and which rows apply. The C
  smoke and the launcher's window must not be able to disagree
  (ADR-014).
- **Test against a scratch library.** `LAUNCHER_LIBRARY_DIR`,
  `LAUNCHER_DISC_LIBRARY`, `LAUNCHER_SHADER_PROFILES_DIR` (and
  `LAUNCHER_SHADERS_DIR`, `LAUNCHER_BROWSE_MEMORY`) move everything the
  launcher writes; the `LAUNCHER_SCREEN` probes `create:`, `clonego:`,
  `shelf:`, `discs:…:insert=`, `saveprofile:` and the verbs write for
  real. `LAUNCHER_PLAYER_BIN`, `LAUNCHER_QEMU_IMG_BIN`,
  `LAUNCHER_PC_BIOS_DIR` and `LAUNCHER_GUEST_TOOLS_ISO` override the
  companions.
- **Ask the window, not the model, for a window's bug.** Every bug the
  Qt launcher had of its own came with a correct model, so a check that
  asks the model passes on the broken build. Drive the window through
  `LAUNCHER_SCREEN` and read what it prints, draws (`LAUNCHER_SHOT`) or
  writes. The lessons behind this are doc 07's "What is in the core".
- **Verify with a real QEMU.** `--print-args` is not the machine: the NIC
  QEMU adds when none is asked for, and a sound card sliding into the
  NIC's PCI slot, showed only through `query-pci` on a running binary.
- **Sentences in a window are short and plain** (user decision, doc 07).
  The *why* stays in comments and docs.
- **The launcher never stops a running machine**: a killed guest leaves a
  dirty FAT.

## Open

- **Library thumbnails** (a machine's last frame) and **bundle
  import/export**, in doc 07's launcher list, not built.
- **The player's own overlay** (pause, snapshot, disc swap, doc 07's
  Player section); today these live in the launcher only.
- **Packaging.** Flathub (metainfo screenshots need hosting on
  2ksbox.com, and the manifest's sources should be a repository, not a
  local directory), the AppImage the user asked for (not started), and a
  Windows installer beside the zip. Windows live control (AF_UNIX) is
  M11's item. The Linux, macOS and Flatpak packagers have not yet run
  with the mitsuami launcher (track M19, "Left").
- **Clone cannot be cancelled once copying.** `std::fs::copy` keeps the
  kernel's fast paths (reflinks, `copy_file_range`) and cannot stop
  mid-file, so Cancel is off during a copy.
- **`CDSHELF.EXE` on Win98.** Used by hand by the user; no scripted
  `tools/cdshelf-guest-test.sh <image> win98` pass recorded (the first
  image tried had a broken shell).
- A shader-pack release and a docs site from these documents were in the
  original M6 plan; neither needs code, neither is started.
