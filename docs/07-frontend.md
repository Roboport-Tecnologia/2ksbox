# 7. Front end: player + launcher

There are two programs (ADR-005). The **player** runs one machine in one
window, and the **launcher** manages the library and spawns a player per
machine. This document covers both, the split between the launcher's
core and its mitsuami front end, the C ABI, and the package layout. The
display pipeline and input model are doc 03, the embed API doc 11, the
machine definitions per family doc 06, and the build and packaging
commands `docs/development.md` and `docs/build-macos.md` /
`build-windows.md`. Every check named here is in `docs/testing.md`.

## Player

- **Two front ends over one core** (ADR-025, track M22): `player-core/`
  is everything but the window, under `player/` (winit) and
  `player-mitsuami/` (the launcher's default when built; the Windows
  packages ship it as `2ksbox-player.exe`).
- **What it runs is a QEMU command line**, not a bundle:
  `player [--shader <preset>] [--shader-params k=v,…] [--pad <mode>]
  [--share <dir>] -- <qemu args>` (in that order; `docs/development.md`
  has every option). The launcher translates a bundle into that line
  (`launcherx --print-player-args <bundle>`), so everything a bundle
  means lives in `launcher-core`. The player boots QEMU in-process (doc
  11) and renders through doc 03's pipeline, one machine per process
  because QEMU's cleanup is incomplete upstream.
- **Window.** The shaded display fills it, aspect-correct with black
  bars. Ctrl+Alt+Shift+F is borderless full screen.
- **Input** is doc 03's model (tablet or PS/2 grab, Ctrl+Alt+G,
  Ctrl+Alt+K, Ctrl+Alt+Shift+D for Ctrl+Alt+Del, Ctrl+Alt+S for a shot
  of the guest's frame, Ctrl+Alt+Shift+S for one of the window's). Alt+F4 asks first. The winit
  player draws the question over the picture (`player/src/prompt.rs`),
  since it has no toolkit and Linux has no message box that works inside
  the Flatpak and over a full-screen window; the mitsuami player uses the
  platform's alert. A gamepad (M13, `docs/tracks/m13-gamepads.md`)
  works whether or not the pointer is grabbed and never changes the grab.
- **Audio.** QEMU's mixer writes f32 into a lock-free ring drained by
  cpal (CoreAudio / WASAPI / PipeWire), and the player limits the sum
  rather than letting it clip. Pacing is doc 11 ("The audio driver and
  its pacing"); CD-DA mixes QEMU-side (doc 17 §5.4).
- **Not built: an overlay** for pause, snapshot, disc swap and the
  shader preset. Snapshots and disc swaps are the launcher's (live, over
  QMP, below), and a disc swap from inside the guest is `CDSHELF`.

## Launcher

The launcher is optional. `launcherx --play <bundle>` or a hand-typed
player line runs a machine with nothing else.

### The library

- The library shows each machine's family and running state, and Start
  spawns a player. "Running" is the launcher's own child *or* a
  listening monitor socket, so a player started by `--play` counts too.
  The launcher only observes (`try_wait`); a spawned player outlives it.
  Machines are in name order, ignoring case (`library::scan`, user,
  2026-10-02).
- **The library is a list beside the chosen machine's details**, as
  UTM and VirtualBox lay theirs out (user, 2026-10-01).
  The machines run down the leading side, each with its name and
  `Machines::subtitle` (family and state); double-click or Return starts
  one. Beside them: the chosen machine's name, Start and a More menu of
  its windows (Settings, Discs, Snapshots, Clone), then its settings in a group
  per page of the form, Storage second (`Machines::details`, `launcherx
  --machine-details`; user: the drives are what is most often looked
  for). The details show only what the form shows for
  that family (no Direct3D row without our adapter), and a path shows
  its file name. The toolbar keeps what is not about one machine: New,
  Shelf, Shaders and About; no button label there ends in "…" (user).
- **About 2ksbox** (2026-10-02) shows the version, the licence and
  the projects 2ksbox is built on, grouped by what they do for it, each
  a link with its licence. The list is `launcher_core::about` (the
  projects the app runs or ships, plus Wine; a library one of them pulls
  in is theirs to credit), and `launcherx --about` prints it. mitsuami
  (`launcher-mitsuami/src/about.rs`) opens it from an info icon at the end of
  the toolbar, except on macOS (user), where it is the application
  menu's About item: the app's menu bar is set on macOS only, one item
  with `MenuRole::About`, which AppKit moves into the application menu.
  The `mitsuami` check grabs it (`LAUNCHER_SCREEN=about`).
- **The launcher has no Stop or Kill**, on purpose. A killed guest
  leaves a dirty FAT, so a run ends from the guest or the player window.
- **Every Start is logged with the line it ran**, quoted to paste back
  into a shell, as `[player] …` in `launcher.log`, at the head of
  `player.log`, and on the terminal. The line is derived at spawn
  (family devices, shelf, QMP socket, shader profile), so a bundle alone
  does not say what ran.
- **The player is found** at `LAUNCHER_PLAYER_BIN`, else the installed
  prefix's `2ksbox-player`, else, in a checkout, the mitsuami player
  (M22, the default) when `player-mitsuami/target/<same profile>/` has
  it, else the winit player beside the launcher, else the root
  workspace's `target/<same profile>/player` (`launcher-mitsuami`
  builds into its own `target/`, where no player sits).
  "Same profile" falls back to `release`: a debug launcher runs the
  release player `scripts/build.sh` made when no debug one is built.
  Another target's player follows the same order under
  `qemu-<target>/`. `--paths` prints the players it will use.
- **Clone** (`launcher-core/src/clone_machine.rs`) makes a new machine
  with the same settings and **its own copy of the disk**, wherever that
  disk is, since two machines on one image corrupt it the day both run.
  Internal snapshots come along inside the qcow2. Files inside the
  bundle folder are copied; what it names outside (shelf discs, a
  shader, a SoundFont) stays shared. A relative backing file is made
  absolute (`qemu-img rebase -u`). A running machine is refused. The
  copy runs on a thread and writes `machine.toml` last, so a clone in
  progress or failed never shows in the list. It cannot be cancelled
  mid-file: `std::fs::copy` keeps the kernel's fast paths (reflinks,
  `copy_file_range`). **"Use the same hard disk instead of copying it"**
  (user request, 2026-09-26) makes the clone name the original's disk by
  its absolute path and copy only the rest of the bundle; the window
  warns that only one machine at a time may run on that disk. Nothing
  enforces it beyond QEMU's own image lock. Nothing of the disk is read,
  so such a clone of a running machine goes ahead. `launcherx --clone
  <machine.toml> [--same-disk] [--new-tpm] [name]` and `lc_machines_clone` (its
  `same_disk` argument) are the same model.
  A Windows 11 machine's TPM is copied with the rest (the clone has the
  same TPM identity, and BitLocker keeps working) unless **"Give the
  copy a new TPM"** is ticked (user decision, 2026-10-01): then its state
  file and the snapshots' copies of it stay behind, and the clone's
  first start makes a new TPM.
- **Not built:** last-frame thumbnails in the list, and bundle
  import/export.

### The bundle

A machine is a directory with `machine.toml` (`launcher-core/src/
bundle.rs`), usually its disk, and references to shelf discs:

- **Most settings are optional fields.** An absent `accel`, `video`,
  `sound`, `music` or `pad` follows the family. An absent field that
  predates the family rules means what every bundle before it ran
  (`seamless_mouse` on, `voodoo2` off, `network` off).
  `[optimizations]` stores only the switches that differ from the
  shipped setting, keyed by QEMU property name, so a new switch arrives
  at its default in every existing bundle, and an entry a newer launcher
  wrote survives a load and save in an older one.
- **`[optimizations]` must stay the last field of `Machine`.** It is the
  one field that serialises as a TOML table, and a table swallows every
  key after it.
- **Editing never renames the bundle directory**, so outside references
  and the disk inside stay valid. New directories are slugs of the name,
  deduplicated (`xp-test-box`, `xp-test-box-2`).
- A bundle is validated before it is written. A corrupt `machine.toml`
  in the library is skipped with a `[library]` log line, never fatal.
- **Commas in paths are doubled** in every QEMU option string the
  bundle writes (disk, floppy, disc, shelf), since QEMU splits options
  on commas; the `dirshelf` check hands QEMU such a path. A live insert
  passes a JSON string and needs no doubling.
- Legacy per-machine shelves (`discs = [...]`) are still read:
  `DiscLibrary::import_legacy` folds them onto the shared shelf at
  start-up (deduplicated by path), `boot_disc()` falls back to the first
  entry, and `save` drops the field.
- The acceleration is written host-neutrally (below), so a directory
  copied between hosts keeps its meaning.

### The settings form

One window with a sidebar of sections, laid out as VirtualBox and UTM
lay theirs out (user request): General (family, name), System (memory,
processor, acceleration, emulation optimizations, extra QEMU
arguments), Display (adapter, Direct3D, the Voodoo 2, the shader
profile), Audio (sound card, music, SoundFont, MT-32 ROMs), Input
(gamepad, pointer), Network, Storage (disk, CD in drive, floppy, boot
order). The sections and their order are the model's (`wizard::Section`,
`lc_wizard_label(LC_LABEL_SECTION, …)`); which field sits on which page
is the front end's. The form opens on its first page for a new or
different machine, and where it was left when the same one is reopened.

The same form creates and edits. It never shows a QEMU command line and
has no raw-TOML box (user decision).

**Every per-family field follows the family until someone picks it.**
Memory, the accelerator, the processor, the NIC and the pointer do, and
so do the four fields whose *list* is per family: adapter, sound card,
music port and pad. Each has a `*_chosen` flag in `wizard::Form`.
`choose_family` moves every unchosen field to the new family's default;
`reset_*` clears the flag, so "Default" follows the family again rather
than pinning the value; `open_edit` sets every flag, because an
existing machine's values are deliberate. A field with a consequence
has no setter, only `choose_*`. The `capi` smoke asserts both
directions and "Default" (except the pad, which the C ABI has no row
for yet). The disk size follows the same rule
(`bundle::default_disk_size_gb`: 10 GB Win98 and Other, 20 GB XP, 40 GB
Windows 7, 64 GB Windows 11, 2 GB DOS; user decision).

The fields, and why each is what it is:

- **Family.** Windows 98, Windows XP, Windows 7, Windows 11, DOS and
  Other (doc 06), in that order and with those labels (`Family::ALL`,
  `Family::label`; user, 2026-10-04). Windows 7, Windows 11 and Other
  have a sentence under the picker (`family_note()`): Windows 7's says
  it is the 32-bit one and that SETUP installs the driver Aero needs;
  Windows 11's says what the machine is and where its installer comes
  from, or that it does not run on this host. Other gets none of our
  adapter, the 3D pass-through or the Windows components, and its
  hardware is chosen for guests nothing here tests (BeOS, a period
  Linux, OS/2). Its note names what it has (a VESA VGA, an RTL8139, an
  ES1370) and says there is no 3D.
- **Memory** is bounded per family (`bundle::ram_mb_range`: Win98
  32–512, since more will not boot; XP 64–3072, Windows 7 1024–3072,
  Windows 11 4096–32768, DOS 4–256, Other 16–3072). BeOS R5's 1 GB ceiling is stated, not enforced.
- **Acceleration** is Automatic / Hardware virtualization / Emulation,
  `accel = "auto" | "kvm" | "tcg"` on every host. `kvm` means "hardware
  acceleration, required", spelled `whpx` on Windows at spawn, and
  refuses to start without it. The picker says "Hardware virtualization";
  the note under it names this host's kind, "Hardware virtualization
  (KVM)" (WHPX on Windows, HVF on macOS; user). The picker offers it
  only where this host can run the machine with it
  (`Form::accel_choices`; user, 2026-10-01: "(required)" beside
  Automatic read as nonsense), so on a host without it the list is
  Automatic and Emulation; a machine already set to it keeps the entry,
  under the note that it won't start. `launcherx --kvm [machine.toml]`
  prints the list. *Automatic* is QEMU's own fallback list (`-accel kvm -accel tcg`,
  `whpx` then `tcg` on Windows), not a probe of ours that could be stale
  by spawn time. On macOS the hypervisor is Windows 11 on Arm's alone
  and an era machine is emulated (`-accel tcg`: HVF runs only the host's
  own architecture), and the note says the hypervisor "runs only
  Windows 11 here" rather than that the host has none
  (`Arch::hypervisor_runs`). Windows' WHPX runs era machines too (patch
  84 put it back into QEMU 11.1's i386 target; it was Windows 11's alone
  from 2026-10-02 to 10-04). `player::hw_accel_available()`
  backs only the hint beside the picker: Linux opens `/dev/kvm` for
  *writing* (a bare `exists()` misses a user outside the `kvm` group),
  Windows asks `WHvGetCapability` (the feature can be installed and
  still off or held by Hyper-V/WSL2). **Win98 and DOS default to
  emulation, every other family to Automatic.** Under KVM Win9x runs at host
  speed into its own fast-CPU bugs, which `-cpu pentium3` does not
  prevent, and Win98 is tuned on the fast paths of docs 13 and 16. A DOS
  throttle needs TCG.
- **The processor** is a combo of named machines (`cpu_speed`,
  `bundle::CpuSpeed`): *Full speed (no throttle)*, then *Pentium 133*,
  *Pentium 75*, *486DX2-66*, *486SX-25*, *386DX-33* and *286-12*. DOS-era software times itself
  against the CPU it finds, so this field decides whether a game is
  playable (doc 06 has the measurements). Every era family offers it (a
  Win98 DOS box runs DOS games too; Windows 11 has no such field); only DOS defaults to a throttled
  one (486DX2-66). A throttle is `-icount`, which cannot run under KVM,
  so `effective_accel()` returns TCG whenever one is chosen, and the
  form says so.
- **Emulation optimizations** have one checkbox per switch of our QEMU
  patch queue (`bundle::Optimization::ALL`; the switches and their
  `-cpu` / `-accel tcg` properties are in `patches/qemu/README.md`).
  **They are exposed because the switch is the oracle**: one run with
  one box clear says whether a fast path made a guest compute the wrong
  number, with no bisecting of a patch queue. All ship on except
  `x87-pc64-as-53`, the one that changes what the guest computes. The
  section is a disclosure headed "Emulation optimizations (N of M on)",
  gives each switch's measured gain, and says they do nothing under KVM.
  The form shows the section only where the machine will be emulated
  (`Form::optimizations_apply`; user, 2026-10-01).
  "All defaults", "Turn all off" and "Turn all on" sit above them
  (`Form::*_all_optimizations`), and the note says which of the three
  states the machine is in. Patch 21's `pinned-regs` is not offered (user decision:
  it crashed guests for too small a gain); a bundle still carrying it
  keeps the entry but never emits it, until "All defaults" removes it
  (`Optimizations::RETIRED`). The accelerator is spelled `-accel kvm
  -accel tcg,…` rather than `-machine accel=kvm:tcg`, because the
  properties need somewhere to live and QEMU refuses the two spellings
  together.
- **Extra QEMU arguments** (user request) is one line, the escape hatch
  for what the form has no field for (`-global d3dpt-vga.ddflags=32768`,
  a trace). Whitespace separates, single or double quotes group and are
  removed, and there is no escape character, so a Windows path's
  backslashes stay as typed (`bundle::split_args` / `join_args`). It is
  stored as `extra_qemu_args` and appended **last**, so a repeated
  option is the user's. Nothing validates it; an unclosed quote is an
  orange note while typing and a refusal at save.
- **The display adapter** is the one picker whose *list* changes with
  the family (`bundle::video_choices`, doc 06): Windows chooses between
  our adapter and the Cirrus, Other between the two standard adapters,
  DOS between `std` and `cirrus` (which VESA BIOS a title finds). A new
  machine starts on the first entry, ours on both Windows families. An
  adapter a family does not offer is refused rather than stored (`std`
  on XP would leave the guest no driver). Changing an existing machine's
  adapter draws an orange line: the guest will find new hardware.
- **Direct3D** picks which Direct3D 9 the executor runs on
  (`bundle::D3d9`, ADR-007's second amendment). It shows only with our
  adapter (`d3d9_applies()`) and offers only what this host can run
  (`bundle::d3d9_choices`): Automatic and DXVK everywhere, plus "This
  PC's own Direct3D 9" on Windows, where the launcher's Vulkan probe
  resolves Automatic (`d3d9=system` with no Vulkan 1.3 GPU or only a
  software one). Elsewhere Automatic writes nothing, and the device's
  own `exec=auto` takes Wine on the host when DXVK finds no device
  (ADR-018). It is a picker because a host can have both and the user
  sees whether a game draws right; the non-Automatic entries are an A/B.
  A bundle saying `system` opened on another host shows Automatic and
  keeps its value, and so does a machine moved to another adapter.
- **What this host gives the guest's Direct3D is stated, not chosen**
  (ADR-013), in the note under that picker (`d3d9_note()`, which on
  Automatic is only this host's answer, not the paths it takes
  elsewhere, user 2026-10-01;
  `graphics_note()` for a front end with no picker, the C smoke among
  them), so nobody finds out after the machine exists. `launcher-core/src/host_gpu.rs` loads the Vulkan
  loader dynamically (no libvulkan is a report, not a crash), creates
  an instance at `min(loader, 1.3)` with
  `VK_KHR_portability_enumeration` when offered, classifies every
  device by type and `apiVersion`, and (`HostGpu::backend`,
  `d3d_headline`, `d3d_advice`) gives one of these answers:
  - a Vulkan 1.3 GPU: DXVK;
  - a **software** Vulkan driver: available, drawn orange (the only
    orange case), expected very slow, and the other stack may beat it.
    With a Wine present too it suggests `-global d3dpt-vga.exec=wine`
    in the extra arguments;
  - Windows below the floor: "runs on this PC's own Direct3D 9";
  - Linux or macOS below the floor with a Wine (`D3DPT_WINE`, the Mac
    apps, `PATH`) and the executor's Windows build shipped
    (`lib/2ksbox/wine/d3dpt-exec-host.exe`): "runs through Wine on this
    host", slower than a Vulkan GPU;
  - neither: a plain note naming the Wine to install (inside the
    Flatpak's sandbox, where the host's Wine is out of reach, the app's
    own add-on `com._2ksbox.Launcher.Wine`, M15 step 7); that guest has
    no Direct3D pass-through.

  While our adapter is picked on a host with no executor, the note adds
  "Keep the 2ksbox adapter anyway. Only its Direct3D needs Vulkan.".
  The Cirrus has no Direct3D either and loses the flip chain's vertical
  blank, the 8 bpp modes, gamma and the cursor. The adapter is never
  picked from the host: an image moves between hosts, and changing its
  adapter is a driver install. DOS and Other get
  no note (the guest half of the pass-through is Windows DLLs).
  `launcherx --host-check` is the full answer for a script or support
  question; it exits non-zero only with no Direct3D at all (software
  Vulkan and Wine count), and the `host-check` check holds it. `--paths`
  prints the Wine and the pair as `wine` and `wine-host`, and the Vulkan
  the probe ran on as `vulkan` and `vulkan-icd`. The probe runs on the
  package's own Vulkan where there is one; in the macOS app that is
  `host_gpu::shipped_loader` by full path, with the ICD named by
  `host_gpu::announce_driver`, every front end's first call in `main`
  (`lc_announce_driver` in the C API; `build-macos.md` "The app" has
  why).
- **The Voodoo 2** ("3dfx Voodoo 2", `voodoo2`; doc 21) sits
  under the adapter. When on, the machine gets `-device
  voodoo2,addr=0x05` and nothing else changes. It is off unless picked,
  on every era family (Windows 11 has no such field), because a card with no guest driver is a New Hardware
  wizard on every boot. **"Voodoo3 undither filter"** (`voodoo2_undither`,
  doc 21 §12) is on the same line: disabled without the card
  (`voodoo2_undither_enabled()`), turned off with it, written only where
  there is a device. It is named after the Voodoo3's "22-bit" scanout
  filter, the thing a user of the era knows, and its notes
  (`voodoo2_undither_notes()`) say how it differs: exact, not a blur
  (user rename, 2026-09-23). It costs ~1.4 ms of the main loop per
  presented frame, hence a choice. The notes are `voodoo2_notes()`; with the box on
  the second line says the card does nothing until a real Voodoo 2
  driver (3dfx's own, the user's download; the disc carries none) is
  installed in the guest (user request, 2026-09-23).
- **Sound card and music** are per-family lists (`bundle::Sound`,
  `bundle::Music`, doc 20 §6). The FM chip comes with the card that
  carried one.
- **Seamless mouse** (`seamless_mouse`) is on for the Windows families and off
  for DOS (its mouse drivers read the PS/2 controller) and Other (an
  absolute pointer needs a guest USB stack nobody here vouches for). On
  gives `-usb -device usb-tablet` and the window never grabs; off leaves
  the PS/2 mouse, grabbed on a click (doc 03). A game whose view sticks
  instead of turning needs it off. An absent field means on.
- **The gamepad** is `bundle::Pad` (M13).
- **Networking** is one checkbox, **off for every new machine**, and an
  absent `network` field means off too (user decisions). These are
  unpatched systems, and ticking it later is a card *appearing*, which
  Windows handles far better than one disappearing. On gives doc 06's
  per-family NIC on user-mode NAT (outbound only). Off emits `-nic
  none`, because QEMU otherwise adds a NIC. XP's PCI devices carry
  explicit addresses, so the NIC's absence does not slide the sound card
  into its slot and make an installed guest re-detect hardware.
- **Clipboard and Shared folder** (`clipboard`, `shared_folder`), the
  first on the Input page and the second on the Network page (user,
  2026-10-03), Windows 11 only (M23, doc 24). The clipboard is on for a
  new Windows 11 machine: it adds QEMU's `qemu-vdagent` on a
  virtio-serial port (`Machine::clipboard_args`), which the player joins,
  and does nothing until the guest has the agent (`2ksbox\install.cmd`
  on the drivers disc, as `clipboard_notes` says). The shared folder is
  the player's `--share <dir>` (`player::share_args`), and only with
  Networking on, which the guest reaches it through (`share_folder`,
  `shared_folder_notes`). An absent field means off and none.
- **A floppy and a boot order** (`floppy`, `boot`), not on Windows 11. *Boot from* is
  Automatic / Hard disk / Floppy, then hard disk / CD, then hard disk. Automatic emits no `-boot`, which
  is what booting a blank new disk's installer from the CD relies on.

### The disc shelf

- **One shelf serves every machine** (`discs.toml` beside the machine
  and profile libraries), because a rip belongs to the person, not the
  machine that installed it first. A machine keeps only which disc is in
  its drive at boot.
- **One drive** (2026-10-01, user): a machine's shelf shows its CD drive
  as a card above the list, and a row's Insert (▶) and the card's Eject
  act on it whether the machine is up or not. Stopped, they set the boot
  disc; running, they swap the disc now and set the boot disc too, so
  the drive still holds it after a restart. While the machine runs the
  card is what the drive holds this moment, read with `query-block`
  (`control::cd_medium`) and polled, because `CDSHELF` and the guest's
  own eject change it too; the matching row reads "In drive". A disc's
  kind is disc, folder or guest tools (`DiscKind`), not its image
  format. `launcherx --drive` prints the card and rows; the `drive`
  check runs it against a paused QEMU.
- **Sorted by label**, case-insensitively, with **digit runs compared as
  numbers** (`disc 10` after `disc 2`). It is an invariant of
  `DiscLibrary`, not a sort each view does, because the flat file the
  in-guest `CDSHELF` lists is addressed by slot number, and a view that
  sorted for itself would offer one disc and load another. So a row
  index is good only until the next edit, and a rename in progress must
  not re-sort (the label is written on Enter or when its field loses
  focus). The `shelforder` check covers it.
- **Adding a disc puts it on the shelf** the moment the dialog closes
  (user report). The shelf's Add menu (Disc image…, Folder as disc…,
  Guest tools ISO) starts at the last folder browsed, and the shelf
  takes dropped images and folders too; there is no typed-path field
  (2026-10-01).
- **A host folder is a disc too** ("Folder as disc…"): the drive gets
  `isodir:<path>`, an ISO 9660 + Joliet volume generated over the tree
  (doc 17 §8). That is how a patch, a save game or a folder of
  installers reaches a guest without networking or mastering an image.
  It is read-only, a snapshot of the tree as the tray closed; sizes
  and limits are doc 17 §8, and a refusal shows on the row's error
  line. A folder's label is its whole name (`patch1.3` keeps its `.3`).
- **`disc_library::qemu_medium(path)` is the one place a medium is named
  to QEMU**: `isodir:<path>` for a directory, the path for a file. It
  serves the boot drive, a live insert and the flat shelf file, and
  decides from the path each time, so a deleted folder is just a missing
  file.
- **The CD-ROM drive is always attached**, empty tray and all, with the
  fixed id `ide1-cd0` (`control::CDROM_ID`). A drive that existed only
  when the bundle had a disc could never be loaded later.
- **Insert and Eject force the tray.** QMP's `blockdev-change-medium`
  and `eject` otherwise *ask* a guest that has locked the medium (XP
  locks it for every open handle), and the click takes effect minutes
  later. The user's click on this machine's own shelf is all the
  authority `force` needs. Insert passes no `format`, so a `.cue`/`.ccd`
  still probes to the `cdimage` driver.
- **The one-click guest-tools disc** is the newest
  `guest-tools/out/guest-tools-*.iso` in a checkout or
  `share/2ksbox/guest-tools/` installed (`LAUNCHER_GUEST_TOOLS_ISO`
  overrides), canonicalised because it is written into a bundle.
- **The shelf from inside the guest** (`CDSHELF` on the guest-tools
  ISO) answers a disc-2 prompt without leaving the game: a window on
  Windows, a key-per-disc menu in DOS, and verbs for scripts. In the
  window Insert is grey while a disc is in the drive (user decision); the
  verbs swap in one step. The drive names the disc in it from the medium
  itself, so the boot disc and one the launcher inserted count too. The channel is a vendor ATAPI command on the
  machine's own drive (opcode 0xD0, patch 52, `cdshelf/cdshelf_proto.h`),
  which DOS, Win98 and XP can all send (PIO, ASPI, SPTI) with no extra
  driver; a machine with no shelf answers ILLEGAL REQUEST. The launcher publishes the
  shelf to a flat file beside the monitor socket at spawn and on every
  edit, and `launcherx --discs publish <bundle dir>` does it by hand.

### How the launcher reaches a running machine

Snapshots and disc swaps on a running machine need its monitor, and the
player's own is a socketpair inside its process (doc 11). Rather than
give either binary an IPC interface, **the launcher adds `-qmp
unix:<runtime dir>/…,server,nowait` to the player's arguments and speaks
QMP to it itself** (QEMU allows several monitors, and everything after
`--` reaches QEMU unchanged). The socket is derived from the bundle
directory, in an owner-only directory, since a monitor is complete
control of the machine. A stale one left by a killed player is removed
before spawn, since QEMU will not bind over it. **Windows uses the same
Unix-domain socket**: QEMU's Windows build binds `unix:` addresses and
the launcher's client is Winsock AF_UNIX (`control.rs`). A loopback port
would let any local process in, and a named pipe's chardev waits for its
one client inside machine start-up. A host that cannot bind one (no
AF_UNIX, Wine, a path longer than `sun_path`) is found by a trial bind
and runs without live control.

### Snapshots

- **Live** snapshots are QMP jobs (`snapshot-save` / `-load` /
  `-delete`), polled through `query-jobs` so the window never blocks
  while QEMU writes a guest's RAM. Buttons grey while one runs. A
  restore resumes the VM **only if it was running**. The snapshot node
  is looked up at run time (QEMU names it like `#block136`) and must be
  the one with `drv == "qcow2"`: a qcow2 shows as two nodes with the
  same filename, and only the format node holds snapshots.
- **Offline**, the launcher runs `qemu-img snapshot` on the qcow2 (the
  same snapshots `savevm` writes) and lists them with `qemu-img info
  --output=json`, since the table form cannot escape a tag with a space.
  `qemu-img`'s stderr is the window's error text.
- **A Windows 11 machine's snapshot is three things** (track M20): the
  disk, its firmware variable store (a qcow2 that takes the same
  snapshot by name, live and offline; a live load or delete includes it
  only when it holds that snapshot), and its TPM state, copied to
  `tpm-snapshots/` after every take and put back by an offline restore
  (a live load restores it from the vmstate). A snapshot taken before
  these were covered restores the disk alone.
- Each mode is refused in the other, because `qemu-img` writing an
  image QEMU has open corrupts it. Restore asks for confirmation (it has
  no undo, and it sits beside Delete).
- **The list is a tree, and the tree is the launcher's own record**
  (2026-09-23). A qcow2 snapshot carries an id, a name, a date and a
  size and nothing about what it was taken from, so "restore A, take C"
  leaves A, B, C reading as a line when C is B's sibling. The window
  therefore writes what it did to `snapshots.toml` beside the bundle
  (`snapshots::Lineage`): every snapshot it took, with the snapshot the
  disk descended from at the time, and which snapshot the disk's present
  state descends from now: the last one taken or restored, marked
  *current* in the list, and where the next one goes. Rows come in tree
  order (each root, then its descendants; siblings in the order they
  were taken) with a depth per row, so a front end draws the tree by
  indenting names. The file follows the disk, never the other way: on
  every read a record whose snapshot is gone is dropped and its children
  move up to its parent, which is also what deleting a snapshot in the
  middle of a branch does. A snapshot with **no record** (taken by
  hand with `qemu-img`, or before the launcher kept the file) sits at
  the top level, with no parent guessed (and no notice: it is simply a
  row); restoring one gives it a record as a root, so the tree grows
  from there. A record matches a snapshot by id, name *and* date, since
  qcow2 reuses an id once its snapshot is deleted. The clone copies the
  file with the rest of the bundle. The `snapshot-tree` check drives
  all of it through `launcherx --snapshots`.

### Shader profiles, presets and the preview

- **A profile** is a name, a `.slangp` path and a **sparse** override
  table. Only parameters someone moved are stored, so a profile follows
  the preset's own retuning and survives new parameters. A machine's
  `shader_profile` wins over its raw `shader` path (the hand-edit escape
  hatch). Both become the player's `--shader` / `--shader-params` at
  spawn, and the player skips an unknown parameter with a log line.
- **The form's picker has the same shape as every other picker.**
  `Form::shader_profile_labels` gives its rows (the app default,
  `SHADER_DEFAULT_LABEL`, then the library by name),
  `shader_profile_index` the current row (0 for a profile that no
  longer exists, which is what the machine plays with), and
  `choose_shader_profile` / `reset_shader_profile` are the verbs.
- **The library has a default profile** (user decision 2026-09-24): one
  file beside the profiles, `default-profile.txt`, naming a profile id
  (`shader_library::default_id` / `set_default`; one file rather than a
  flag in each profile, so an editor rewriting a profile cannot drop the
  mark and there is never a second default). A machine on the app
  default (`shader_profile = None`, the picker's first row) plays
  through it: `player::resolve_shader` goes named profile, raw `shader`
  path, the library's default, nothing. The picker's first row and the
  library's "Shader" column say which ("(default) CRT Aperture",
  `shader_library::default_label`; bare "(default)" with none marked).
  The profile window's rows carry a "Default" switch, and "No
  default" clears it; deleting the default
  profile clears it too, and a file naming a profile that is gone reads
  as none. **The first download marks CRT Aperture**
  (`create_defaults`, when the library has no default yet; a default
  the user chose is never moved). `launcherx --default-shader-profile
  [<id>|(none)]` reads or sets it; `--print-shader-args` shows what a
  machine resolves to.
- **Where presets come from** (`shader_source::presets_dir()`):
  `LAUNCHER_SHADERS_DIR` if set (then nothing else), else the checkout's
  `third_party/slang-shaders`, else a downloaded copy in the data
  directory. "Has presets" means a `.slangp` within two levels, so an
  empty or half-unpacked directory reads as none.
- **The download** is upstream's `master` tarball (overrides are by
  name, so a newer tree is additive) over HTTPS with `ureq`/rustls,
  streamed through `flate2` + `tar` on its own thread, showing MB so far
  (codeload sends no `Content-Length`). It unpacks into a `.part` sibling and renames
  over the old collection only once the result has presets, so an
  interrupted download never reads as installed. Symlinks, special
  entries and `..` paths are skipped.
- **The first run** (`launcher_core::firstrun`) offers the download once
  at start-up, as a modal question over the machine window, when no collection
  exists anywhere (a button two windows deep is where a new user never
  looks); the profile manager has the button too. Answering either way
  writes `first-run.txt` into the *profile* directory (a download
  replaces the preset directory by a rename and would take a marker
  with it). A yes writes the starter profiles
  (`shader_source::DEFAULT_PROFILES`: CRT Aperture, CRT Royale, Apple
  II, at the presets' defaults) once the collection lands, never a name
  already held. The model holds the words at every step (`Message {
  step, headline, detail }`); a front end chooses only the buttons,
  which is why the size and destination are in the question. Accepting
  calls `editor::Presets::forget`, or the profile manager's cached
  "none" would go on offering what just arrived. `launcherx
  --first-run` and `--default-profiles` are the flow without a toolkit.
- **"Browse…" starts** at the field's own directory, else a suggestion
  (the collection, for a preset), else **the last directory any dialog
  browsed** (`browse::remember`, one line in `<data dir>/
  last-browse.txt`, `LAUNCHER_BROWSE_MEMORY` overrides), else the OS
  default. The memory exists because a dialog given no folder opened in
  the working directory (the Qt launcher's did). `launcherx
  --browse-start` prints the answer, and `LAUNCHER_PICK=<label>=<path>`
  makes the launcher's field with that caption print the dialog it
  would open and take `<path>` as the answer (track M19).
- **A path a dialog hands back goes through `browse::picked`** (the
  launcher's `PathField` and shelf, and the disc library on load). Inside the
  Flatpak the portal's dialog returns a document-portal path,
  `$XDG_RUNTIME_DIR/doc/<id>/<name>`, even though the app can reach the
  host: only the picked file is there, so a `.cue`'s tracks are not
  beside it, and its FUSE filesystem answers QEMU's lock test with EIO
  (user report, 2026-09-23: a disc inserted while the guest ran "failed"
  with `Failed to get "consistent read" lock`). The portal's
  `user.document-portal.host-path` attribute names the real file, which
  is what is kept. `launcherx --picked` prints the answer and
  `package-flatpak.sh` checks it in the sandbox with a file it exported
  through the portal.
- **The preview is the player's picture.** The scale is the player's
  `floor(min(area/image)).max(1.0)`, integer, letterboxed and cropped
  like a window smaller than the mode. The `.max(1.0)` is load-bearing:
  slang CRT presets assume they upscale, and some divide by zero when
  asked to shrink (`crt-aperture`'s `floor(OutputSize.y /
  SourceSize.y)`) and draw black. A source over 1600×1200 is resized on
  the CPU first (`MAX_SOURCE_W/H`).
- **The preview moves when the preset does** (interlacing, phosphor
  decay, NTSC shimmer). `preview::Preview::frame_interval` says how
  often to draw (`None` for a still preset) and the front end obeys
  (the editor sleeps one interval and marks the preview stale, so a
  still preset runs no timer). The frame number comes from a clock at `FRAME_RATE`
  (60/s), not a count of renders, so the effect runs at the player's
  speed and drops frames rather than running slow.
  `shader_chain::preset_is_animated` looks for a *use* of `FrameCount`
  (1131 slang-shaders passes declare it, 271 read it) or a
  history/feedback texture, and errs towards animating. The headless
  verbs pin one frame (`PREVIEW_FRAME`, default 0) so their PNGs are
  reproducible. The `preview-anim` check covers it.

## Settings taxonomy

- **Per app:** the shader profiles and presets, the disc shelf, default
  hotkeys. There is no telemetry.
- **Per machine:** everything in the bundle (hardware, RAM, media,
  shader profile, pointer, the emulator's fast paths). The fast paths
  are per machine on purpose: turning one off diagnoses *one guest* and
  must not slow every other machine meanwhile.
- Bundles live in a plain, documented directory layout the user can
  back up.

## One front end over a core

The launcher is **one front end over one library** (ADR-014).
`launcher-mitsuami/` (ADR-023, track M19) is a view over
`launcher-core/` in mitsuami's platform widgets: AppKit on macOS,
WinUI 3 on Windows, GTK 4 on Linux, or Kirigami with the `kde` feature.
Since 2026-10-02 it is the only launcher and every package ships it as
`2ksbox` (user: "remove the other launchers code completely"). The
core has two more callers with no window: `launcher-capi/` (the same
models as a C ABI) and `launcherx` (every toolkit-free debug verb,
`launcher_core::cli`, which the launcher answers identically). Each
window is a `#[component]` reading a `Store` that holds the core model;
a keyed platform `List` keeps a row mounted while its key lives, so rows
read their fields from the model by key instead of holding copies.
`launcher-mitsuami` is its own cargo workspace, outside the root one, so
a plain `cargo build` never needs GTK; build it with `scripts/build.sh
mitsuami` (GTK 4.10+ development files on Linux, nothing on macOS) and
on Windows with `scripts/build-windows.sh mitsuami`. The Qt 6 / QML
launcher that shipped before it (ADR-015, superseded by ADR-023) was
deleted on 2026-10-02, and the egui one before that (ADR-017).

### What is in the core, and why all of it

`launcher-core/` is **everything the launcher does that is not
drawing**, a line deliberately far into what usually counts as UI:

- the data: `bundle`, `library`, `disc_library`, `shader_profile` /
  `shader_library` / `shader_source`, `paths`;
- the machinery: `player` (spawning, `qemu-img`), `control` (QMP),
  `snapshots`, `preview`, `clone_machine`, `host_gpu`;
- **each window's own behaviour**: `machines`, `wizard`, `shelf`,
  `snaps`, `editor`, `firstrun`, down to the sentences they show, plus
  `browse` for the file-dialog decisions that are not a dialog, and
  `cli`.

The sentences are short and plain (user request): one or two per note,
in a user's words, with no dates, doc numbers, patch names or benchmark
stories. A front end prints `ram_note()`, `accel_note()` and
`network_notes()`, fills a combo from `Family::ALL` / `CpuSpeed::ALL`
and their `label()`s, and a field with a consequence has only
`choose_*`, so the drift ADR-014 records cannot be expressed.

**What the core does not protect against.** Every bug the Qt launcher
had of its own came with a correct model: the window showed something
else. The lessons outlived it and hold for any front end:

1. **Ask the window, not the model.** A check that asks the model
   passes on a broken window, so the launcher's checks drive the real
   window (`LAUNCHER_SCREEN`, below) and read what it shows or wrote.
2. **A value that lives in two places needs one rule for which way it
   flows.** A verb that republished the form while someone typed wrote
   older text back into the field. Every edit is the model's own method,
   and a window reads the model rather than keeping a copy.
3. **A window puts itself away through the model.** Hiding the window
   and leaving the model's "open" flag up showed it again on the next
   republish, over nothing.
4. **A rule typed into a view drifts.** A label, a range or which row
   applies, written in the view, came out different from the core's
   (ADR-014's record); the view only asks.
5. **A control's state is not a checkbox when it is "showing / hidden"**:
   a tick before "Emulation optimizations" said clearing it turns them
   off. A disclosure is a disclosure.
6. **File dialog filters are case-sensitive on Linux**, so `*.cue` hid
   `GAME.CUE`; `browse::extensions` gives each extension in both cases
   (not `[cC]`, which Windows and macOS dialogs do not take).

### What the front end still owns

These are the toolkit's, and any other front end owes them too:

| | mitsuami (`launcher-mitsuami/`) |
|---|---|
| the file dialog | the platform's (`OpenFile`, started at `browse::browse_start`; the answer through `browse::picked`) |
| when to redraw | a reactive `Store`; timers only for what is watched (the 500 ms reap, the drive's 2 s poll, a live snapshot job) |
| "the list changed" | a keyed `List` / `For`, rows read by key |
| a destructive restore | the platform's alert, Cancel the default |
| the preview frame | the core's RGB pixels into an `Image`, cropped to its area, no file on disk |
| secondary screens | real top-level windows, and sheets |
| a headless frame | `LAUNCHER_SHOT=<png>` (`launcher-mitsuami/src/shot.rs`) |

The shader manager is **two windows** (list and editor), not one that
resizes between modes, because a mapped window's size is the window
manager's to ignore.

### Proving the core is the one implementation

- `--preview-shader` is `launcher_core::preview` on a headless device,
  the same code the launcher's preview runs.
- `launcherx`, `launcher-core`'s own binary, is what `scripts/test.sh`
  and `tools/dos-guest-test.py` drive, so the suite builds no GUI
  toolkit to ask `--print-args` a question.
- `LAUNCHER_SCREEN=create:<family>:<name>` fills the real form on a
  fresh machine and submits it, and its `machine.toml` is the model's.
  The `mitsuami` check does it for XP, beside the main window's shot,
  `firstrun:no` (the model's headline, and the marker written) and
  `about`; on Linux on a private Broadway display. The screens are
  listed in `docs/tracks/m19-mitsuami-launcher.md`.

## A second caller: `launcher-core` as a library

`launcher-capi/` is the C ABI that lets a front end in another language
be a view over the same models, shaped for a native macOS app in Swift,
which imports a C header with no bridge crate.

- `launcher-capi/include/launcher_core.h` is hand-written beside the
  code.
- Each window is an **opaque handle** (`lc_wizard_new` /
  `lc_wizard_free` …). Rows are addressed by index, one field at a time,
  as a platform list reads them.
- Strings out are owned by the caller (`lc_string_free`) and never
  `NULL` for empty, so `NULL` means only "no such row".
- Nothing blocks on a guest. The long operations poll
  (`lc_snapshots_poll` while `lc_snapshots_job_pending`,
  `lc_editor_preset_state` during a download).
- It adds **no behaviour**; every function is a thin wrapper.

It is a workspace member but not a default one (a `cdylib` and a
`staticlib` of the whole launcher), kept compiling by `scripts/build.sh`'s
`cargo check --release --workspace`. `launcher-capi/examples/smoke.c`
is the smallest front end and a test (the `capi` check). Another front
end owes the table above, and `lc_editor_read_frame` hands over RGB8
for the preview.

## Shipping the launcher

The launcher brings no toolkit of its own: it is the platform's widgets,
so what each package carries for it is the platform's to supply:

| package | what the launcher needs there |
|---|---|
| Linux tarball (`scripts/package-linux.sh`) | the host's GTK 4, not carried; `install.sh` names the distribution's package when it is missing |
| Flatpak (`packaging/flatpak/`) | the runtime: `org.gnome.Platform` 49, which ships GTK 4 (user decision, 2026-10-02) |
| macOS (`scripts/package-macos.sh`) | nothing: AppKit is the system's |
| Windows (`scripts/package-windows.sh`, and its MSIX through `package-msix.sh`) | the Windows App Runtime 2.4+ (WinUI 3); the launcher is MSVC like the whole package, with the static C runtime, and the MSIX declares the App Runtime as a `PackageDependency` |

**Every packager opens the staged launcher's window and requires a
PNG**, since a package can pass every other check and still open
nothing (the Qt launcher's lesson: a toolkit's pieces found by name at
run time). The grab is the launcher's own, `LAUNCHER_SHOT=<png>`
(`launcher-mitsuami/src/shot.rs`), which draws the window into a PNG
and exits; `LAUNCHER_SCREEN` picks the window. On Linux and in the
Flatpak it runs on a private Broadway display (`gtk4-broadwayd`,
`GDK_BACKEND=broadway`), so nothing opens on the desktop; on macOS and
Windows the window shows briefly, on macOS under
`DYLD_PRINT_LIBRARIES=1`.

## Platform packaging

### The names

The product is **2ksbox** and the application ID
**`com._2ksbox.Launcher`** (ADR-011, which says which name goes where
and why the underscore); the macOS app's bundle ID is
`com.2ksbox.2ksbox`, since Apple forbids the underscore. The data directory `~/.local/share/2ksbox` was
moved once from `win98-xp-virt` (`launcher-core/src/paths.rs::data_dir`).
On Windows it is `%APPDATA%\2ksbox\data`, except from an installed MSIX,
where Windows would virtualise `AppData` and delete it on uninstall: a
launcher with package identity (`paths::packaged()`) keeps the library
at `%USERPROFILE%\2ksbox` (`build-windows.md`, "The Store package").

### The install layout

The launcher decides whether it is installed by looking at its own
executable (`launcher-core/src/paths.rs`): if `<exe dir>/..` contains
`share/2ksbox`, it is installed. Otherwise it finds everything in the
checkout it was built from (`target/`, `build/qemu`, `qemu/pc-bios`,
`guest-tools/out`, `third_party/`).

```
<prefix>/bin/2ksbox                            the launcher
<prefix>/bin/2ksbox-player                     the player (era machines)
<prefix>/bin/2ksbox-player-x86_64              the player for Windows 11
<prefix>/bin/2ksbox-player-aarch64             ... for Windows 11 on Arm (Arm hosts; the Mac app)
<prefix>/lib/2ksbox/libqemu-embed-i386.so      the QEMU each links
<prefix>/lib/2ksbox/libqemu-embed-x86_64.so
<prefix>/lib/2ksbox/…                          D3D executor + DXVK, wine/
<prefix>/libexec/2ksbox/qemu-img               ours, patched, kept off PATH
<prefix>/share/2ksbox/pc-bios/                 QEMU firmware (the player's -L)
<prefix>/share/2ksbox/guest-tools/             the guest-tools ISO
<prefix>/share/2ksbox/drivers/                 Windows 11's drivers disc, the host's processor's
<prefix>/share/2ksbox/shaders/                 presets, when a package ships them
<prefix>/share/2ksbox/desktop/                 .desktop + metainfo, for install.sh
<prefix>/share/icons/hicolor/<n>x<n>/apps/     the application icon, every size
<prefix>/share/doc/2ksbox/                     COPYING, notices, README
```

Three rules hold it together:

- **Everything is relative to the executable**, so an extracted tarball
  works where it lands. The player finds `libqemu-embed` through an
  `$ORIGIN/../lib/2ksbox` rpath (`@loader_path` on macOS) ordered
  *before* the build-directory one, so a packaged binary never loads a
  developer's library. The packaged player names the dlopened
  companions to QEMU itself (`player-core/src/companions.rs`,
  `player --companions`).
- **One layout or the other, never a mixture.** An installed launcher
  answers only with its own prefix, even for a file the package left
  out; a checkout fallback would let a broken package pass on the
  machine that built it. `LAUNCHER_*` overrides win over both.
- `qemu-img` is ours (patch 50's `cdimage` driver), so it lives in
  `libexec/`, where it neither shadows nor is shadowed by the system's.

On macOS the `.app`'s `Contents` is the prefix, with `MacOS/` doing
`bin/`'s job (`paths::bin_dir()`). Windows is flat. The launcher's
window carries the same identity through mitsuami's `App::id`, `name`
and `icon` in `launcher-mitsuami/src/main.rs` (`paths::APP_ID`, "2ksbox"
and the 256 px PNG), so under Wayland its `app_id` is
`com._2ksbox.Launcher`.

**The icon is one master and one generator.** `packaging/icon/
2ksbox.png` is padded to 512×512, and every size (16–512 PNGs and a
four-size `.ico`) is a downscale made by `scripts/gen-icons.sh`. All
are checked in, because nothing that needs one (an offline Flatpak, a
build without ImageMagick, `install.sh`) can draw it.
`gen-icons.sh --check` is the `icons` check. Linux
installs the set under `share/icons/hicolor/` and writes one absolute
path into the desktop entry's `Icon=` (a prefix outside `XDG_DATA_DIRS`
cannot resolve a theme name). macOS builds its `.icns` from the same
PNGs. On Windows the `.ico` goes *inside* every .exe as a resource, the
only thing Explorer reads: `packaging/windows/win-icon.rs` is
`include!`d by the build scripts of `launcher-mitsuami`, `player` and
`player-mitsuami` (a
build-dependency would have to be vendored into the Flatpak's offline
sources), writes a two-line `.rc` (the icon, and the application
manifest `packaging/windows/app.manifest`, which declares per-monitor
DPI awareness for the Store's certification kit) and runs `windres`.
For the MinGW player it links the object; for an MSVC binary (the
launcher, the mitsuami player) windres writes a `.res`, which Microsoft's linker takes as it is, told
to make no manifest of its own. A host without windres gets a warning
and an icon-less binary with the default manifest. The loose `.ico`
ships too, for shortcuts.

**AppStream metadata** (`com._2ksbox.Launcher.metainfo.xml`, into
`share/metainfo`) carries a deliberately **empty** OARS rating: it
rates 2ksbox itself, which has no chat, purchasing or user-to-user
content, and the software someone runs in a guest is their own (the
reading other emulators apply). `appstreamcli validate --no-net` runs on
every package and fails on errors only; the one warning (no
screenshots) needs somewhere to host them.

### Per platform

- **Linux.** `scripts/package-linux.sh` stages the layout, asks the
  staged launcher and player where everything resolves with a scrubbed
  environment (`--paths`, `--companions`, a machine created and
  translated to a command line, the window grabbed on Broadway), and
  rolls a tarball whose `install.sh` copies the tree into a prefix and
  names the GTK 4 package to install when the host has none. **The
  Flatpak** (`packaging/flatpak/`,
  `scripts/package-flatpak.sh`) is the primary Linux target (user
  decision: Flatpak first, then an AppImage), for distribution rather
  than sandboxing: no distro `qemu` can replace our patch queue, and
  Flathub is where a stranger finds a Linux app. The sandbox is mostly
  nominal (`/dev/kvm`, the GPU, the network, and `--filesystem=host`,
  because bundles store absolute paths to discs and disks). Its runtime
  is `org.gnome.Platform` 49, for GTK 4 (user decision, 2026-10-02; it
  was `org.kde.Platform` while the launcher was Qt). It builds from
  source (host binaries need a newer
  glibc than the runtime's), offline, through `package-linux.sh --prefix
  /app`, with QEMU's GLib, libslirp and libtpms from `build-deps.sh`'s
  declared tarballs and a build-only `distlib`. Wine and the KDE
  launcher are add-ons (`docs/development.md` "Flatpak"). Its crates, mitsuami's git checkout
  among them, are listed in `packaging/flatpak/cargo-sources.json`
  (`scripts/gen-flatpak-cargo-sources.sh`).
- **macOS.** A signed, notarized .app with the JIT entitlement, native
  on Apple Silicon, carrying its whole non-system dylib closure (the Mac
  that runs it has no Homebrew and no Vulkan) and no toolkit (the
  launcher is AppKit), in **two builds (ADR-019)**. Recipe and
  reasoning: `docs/build-macos.md` ("The app", "The floor").
- **Windows.** A portable zip (`scripts/package-windows.sh`,
  `docs/build-windows.md`) and the same tree as an MSIX for the
  Microsoft Store (`scripts/package-msix.sh`,
  `packaging/windows/AppxManifest.xml.in`; `build-windows.md` "The Store
  package"). The launcher is built only on a PC (`build-windows.sh
  mitsuami`, MSVC, WinUI 3, needing the Windows App Runtime 2.4+), so the
  package is rolled on the PC. Hardware acceleration is WHPX,
  stated beside the picker, with TCG as the fallback.

**Open:** Flathub (hosted screenshots on 2ksbox.com, and the manifest's
sources as git rather than a local directory), the AppImage (asked for,
not started), a Windows installer for users outside the Store, and the
Store upload itself (a Partner Center identity).

## Out of scope for v1

Drag-and-drop, USB passthrough, multi-monitor guests, and
recording/streaming helpers. (Shared folders and the clipboard are track
M23 since 2026-10-02, doc 24.) Recording pairs
with the shader pipeline and is the first post-v1 candidate.
