# 10. Decision records

The project's architecture decisions, oldest first. Each record keeps
its date, status, decision, reasons, rejected options and costs;
amendments stay inside the record they change, dated. The mechanisms
live in the design docs named in each record; this doc keeps the *why*.
The roadmap is doc 08.

| ADR | Decision | Status |
|---|---|---|
| 001 | QEMU is the base | accepted |
| 002 | QEMU runs in-process with the display | accepted |
| 003 | A libretro core is the front end | **superseded by 005** |
| 004 | Rust where possible | accepted |
| 005 | A standalone player; RetroArch dropped | accepted |
| 006 | Direct3D 8/9 through our own paravirtual device | accepted |
| 007 | The executor is DXVK everywhere; macOS on KosmicKrisp | accepted, amended 2026-09-17 and 2026-09-21 (Windows) |
| 008 | A real guest display driver, staged after the DLL device | accepted |
| 009 | The launcher and `shader-chain` are GPL-2.0-or-later | accepted, with an addendum |
| 010 | The player ships despite the GPLv2 / Apache-2.0 conflict | accepted |
| 011 | The product is 2ksbox, app ID `com._2ksbox.Launcher` | accepted, amended 2026-09-06 |
| 012 | Win98 gets the XP driver over a shared core | accepted |
| 013 | Below the Vulkan 1.3 floor, no DXVK device; no second executor | accepted, amended; points 1 and 3 **superseded by 018** |
| 014 | One launcher library, front ends draw it | accepted; "two front ends" **ended by 017** |
| 015 | The Qt front end is the shipped one | accepted; "keep `launcher/`" **reversed by 017** |
| 016 | The Voodoo 2 is emulated beside the Glide pass-through | accepted; "beside" superseded by 020 |
| 017 | The egui front end is retired | accepted |
| 018 | Below the Vulkan floor, the executor runs on Wine on the host; WineD3D-in-guest retired | accepted, retirement done 2026-09-23 |
| 019 | Two macOS builds: App Store 26+, community at Homebrew's floor | accepted |
| 020 | The Glide pass-through is removed; the Voodoo 2 is the only Glide | accepted |
| 021 | The driver is a DirectX 9 driver; no per-game graphics DLLs | accepted; done for Direct3D, the OpenGL ICD left open (2026-09-27) |
| 022 | Aero on Windows 7 through a WDDM driver of our own, beside the XP one | accepted, work in M18 |
| 023 | The launcher moves to mitsuami | accepted, work in M19 |
| 024 | A general-purpose VM manager, best at vintage boxes | accepted, work in M20 |
| 025 | The player moves to mitsuami, over a shared `player-core` | accepted, work in M22 |

## ADR-001: QEMU as the base (2026-08-31)

The only open-source option covering Linux, Windows and macOS on Apple
Silicon with a working guest-3D path for both Win98 and XP (qemu-3dfx).

**Rejected.** VMware (closed source, no ARM Mac). VirtualBox (no 3D for
pre-Win7 guests since 6.1). 86Box as a base (it already does
authentic-hardware emulation; ADR-016 later borrows its Voodoo 2 as a
device).

**Amendment (2026-10-01, track M21): QEMU 11.1, and qemu-3dfx's patch is
ours.** The user moved the base to the newest release, v11.1.2 ("upgrade
to latest qemu version and just backport the qemu-3dfx part"). qemu-3dfx
publishes one patch per QEMU release and has none past 9.2, so its patch
is carried in our tree, ported to 11.1: `patches/qemu-3dfx/
00-qemu111x-mesa.patch`, applied by `prepare-qemu.sh` before our queue.
Only its OpenGL half (ADR-020 retired the Glide pass-through). The device
sources (`hw/mesa`), `sign_commit` and the guest wrappers still come from
the pinned `third_party/qemu-3dfx` submodule, so a qemu-3dfx bump stays
possible as long as its `hw/mesa` compiles on our QEMU. Not a fork of
qemu-3dfx: one patch file is less to keep than a repository.

## ADR-002: QEMU runs in-process with the display (2026-08-31)

For gaming latency, framebuffer, input and audio must not cross a
process boundary, so QEMU gets UTM-style embed patches. The costs are
one VM per process and GPL-2.0 for anything that links QEMU. ADR-010
later measured a process boundary as affordable; the decision stands
for the work a split would take.

## ADR-003: A libretro core is the front end (2026-08-31), superseded by ADR-005

A RetroArch core would have supplied CRT shaders, frame pacing, input,
audio, disc control and packaging, with a companion launcher for
machines, snapshots and the disc shelf. The embed layer was kept
front-end-agnostic so a standalone player stayed possible.

## ADR-004: Rust where possible (2026-08-31)

All original code is Rust unless a hard boundary forces C:

| Component | Language | Why |
|---|---|---|
| Embed-API bindings, player, launcher | Rust | greenfield, consumer side |
| Disc model and format parsers (`libdisc`) | Rust | parser-heavy; QEMU accepts Rust (experimental since 9.2), so upstreaming is plausible |
| Tooling | Rust | greenfield |
| QEMU patches: embed API, ATAPI glue | C | inside QEMU; thin shims over libdisc |
| qemu-3dfx host patches | C | third-party, version-coupled |
| Guest wrappers and drivers | C (era toolchains) | Rust cannot target Win9x/XP |

So libmirage is a behavioural reference (doc 05), not a linked
dependency, which also drops its glib dependency.

## ADR-005: Standalone player is the front end; RetroArch dropped (2026-09-02)

Supersedes ADR-003. After hands-on time the user judged RetroArch too
buggy to build on ("the thing is so full of bugs it's not even worth
it"). The front end is a **standalone Rust player** (winit, wgpu,
librashader, cpal) with QEMU in-process (ADR-002), plus a separate
**launcher**.

- We own the presentation pipeline (doc 03).
- qemu-3dfx's target becomes "guest GL output → wgpu texture" (Spike A,
  `docs/spikes/spike-a-macos.md`) instead of a libretro hw-render
  context.
- Spike B (`docs/spikes/spike-b-libretro-crate.md`) is void and the M0
  core is deleted. The embed API stays front-end-agnostic; we will not
  maintain a libretro shell.

## ADR-006: Direct3D 8/9 for XP through our own paravirtual device (2026-09-03)

**Decision.** Thin guest `d3d9.dll` / `d3d8.dll` serialize the API into
a shared-memory command stream; a host executor runs the same D3D9
semantics natively (doc 14). Guest-side WineD3D (JHRobotics' wine9x)
stays as the fallback and the DirectDraw / D3D ≤ 7 path *(retired by
ADR-018; the DirectDraw / D3D ≤ 7 path is ADR-012's driver)*.

**Why.** After two days of WineD3D-in-guest on XP (FIFA 2000), every fix
was in a 2015 fork with no upstream, and WineD3D translates D3D → GL
inside the guest at TCG speed, then ships GL calls through the FIFO one
by one. A paravirtual device moves the translation to native host code:
on Apple Silicon, the difference between "works" and "plays". No
community D3D9 paravirt device exists (VirtualBox's is Vista+, VMware's
closed and Win7+).

**Rejected.** Proton (no Windows guest, no macOS). DXVK in the guest (no
Vulkan driver for XP). Fixing WineD3D 1.7.55 further.

**Licensing.** Wine is LGPL-2.1+, so copying its D3D behaviour and code
into the guest DLLs and the executor is allowed (those parts carry the
LGPL and ship with source). DXVK is zlib, d3d8to9 BSD-2; all combine
with GPL-2.0 QEMU. Nothing from Microsoft's SDK ships; headers come from
mingw-w64.

**Consequences.** M4 becomes the device. It reuses qemu-3dfx's FIFO/MMIO
model, the FXPTL/MAPMEM guest driver, the embed frame path and the
player's 3D layer. C++ enters the host side behind a C shim; guest DLLs
are C.

## ADR-007: The host executor is DXVK everywhere; macOS runs it over KosmicKrisp (2026-09-03, amended 2026-09-17 and 2026-09-21)

**Decision.** The executor is DXVK's d3d9 built natively as a library
beside QEMU. On macOS the Vulkan under it is Mesa's **KosmicKrisp**
(Vulkan on Metal 4, in the LunarG SDK), which needs **macOS 26**;
MoltenVK is not supported. DXVK is a submodule (`third_party/dxvk`)
plus a patch queue (`patches/dxvk/`), like QEMU.

**Why.** Spike C (`docs/spikes/spike-c-dxvk-native-macos.md`): DXVK
refuses MoltenVK outright, with five required features missing, two of
them (robustBufferAccess2, nullDescriptor) unimplementable on Metal.
KosmicKrisp has all but geometry shaders, which d3d9 never uses, so a
one-line "required → optional" patch is enough. On the Air the
reference frame matches the rig as closely as on RADV (2026-09-03).

**Rejected.** Implementing the features in MoltenVK (harder). DXVK on
MoltenVK with dummy resources (fights DXVK's descriptor design on every
rebase). A native D3D9-on-Metal executor or a wgpu translator (~40 k
lines, and a second executor to keep bug-for-bug equal).

**Consequences.** The Direct3D path needs macOS 26 (ADR-013, ADR-018 and
ADR-019 cover a pre-26 Mac). A window-less WSI backend (patch 04)
replaces SDL2. The rig goldens (`reference/d3d`) are the acceptance test
on every driver.

**Amendment, 2026-09-17: Windows runs DXVK too** (user decision,
reversing M11's choice of the system d3d9). The executor's first run on
Windows' own d3d9 drew black frames, and a second rasteriser is what
this ADR rejected for macOS. The Windows package carries DXVK's d3d9 as
`dxvk_d3d9.dll` (patch 08).

**Second amendment, 2026-09-21: Windows' own Direct3D 9 as the
fallback** (user decision: "the wine path is just not very good"). DXVK
stays the default on every host and the only rasteriser goldens are
compared against. A **Windows host below the Vulkan 1.3 floor**
(ADR-013's table) runs the same executor on `system32\d3d9.dll` instead
of WineD3D in the guest, because those cards all run D3D9 natively at
full speed.

- The black frames came from nothing being adapted to it; what the
  system d3d9 refuses and DXVK allows is handled in `Exec::native`
  (doc 14, "The system Direct3D 9").
- The form's **Direct3D** row writes `-device d3dpt-vga,d3d9=…`, and the
  launcher's probe resolves `auto`, since only it can tell software
  Vulkan from real (a card's own d3d9 beats lavapipe). `D3DPT_D3D9=
  dxvk|system` forces either (doc 07).
- The cost is a second rasteriser's driver gaps. What makes it
  defensible is an oracle: `d3dpt-dp2-test.exe` and
  `d3dpt-exec-test.exe` pass on both backends on the user's PC, with
  frames byte-identical between DXVK and NVIDIA's Direct3D 9.

ADR-018 is the same answer for Linux and macOS, which have no system
d3d9.

## ADR-008: A real guest display driver is the long-term shape; staged after the DLL device (2026-09-04)

**Decision.** Keep ADR-006's DLL device, and grow a **real Windows
display driver** on the same transport and executor as milestone M7
(doc 15). The driver replaces the per-game DLLs and the emulated Cirrus,
and is the only road to Vista+ acceleration should it ever be wanted.

**Why.** Once the DLL device matched the rig, the question was how games
reach it. DLLs have structural limits:

- **Per-game installs.** A `D3D9.DLL` must sit beside every EXE
  (Windows File Protection fights a system-wide one), and games that
  load `system32\d3d9.dll` by path, use DirectDraw 7 or check the
  DirectX version never see it.
- **Re-implementing the runtime** (the managed pool, lost devices,
  state blocks, software vertex processing). With a driver, Microsoft's
  runtime stays in the guest and talks to us through the Direct3D DDI,
  on 2000/XP already a serialized stream (DP2 token buffers).
- **The desktop becomes ours.** Any mode the CRT shader wants, no Cirrus
  limits, DirectDraw and D3D 7 for free. VMware and VirtualBox prove the
  shape, and its cost.

**Kept.** The DLL device (Win98's path until ADR-012, and the executor's
harness). WineD3D in the guest as the DX7 fallback *(retired by
ADR-018)*.

**Rejected.** WDDM (Vista/7/10) now: a D3D11 UMD is VBoxDX-sized.

**Stages.** M7a framebuffer driver, M7b DirectDraw DDI, M7c Direct3D DDI
(the doc 04 matrix through Microsoft's own d3d8/d3d9 with no DLL in the
game folder), M7d WDDM if ever.

**Toolchain and licensing.** mingw-w64 ships the DDI headers and the
cross toolchain. ReactOS (GPL-2.0) is the reference for DDI behaviour.
Nothing from Microsoft's DDK ships.

**Consequences.** The record protocol stays driver-neutral
(guest-chosen handles, host-object resources, nothing about COM). The
executor grows DDI-shaped operations when a stage needs them.

## ADR-009: The launcher and `shader-chain` are GPL-2.0-or-later (2026-09-05)

**Decision.** The launcher crates and `shader-chain` declare
`GPL-2.0-or-later`. Everything that links QEMU (`player`, `qemu-embed`)
and `libdisc`, which is compiled into QEMU, stays `GPL-2.0-only`.

**Why.** The launcher's dependency tree has **Apache-2.0-only** crates
(then egui's `ab_glyph` and `glutin`, winit's `dpi`, `ring` under
`ureq`'s rustls). Apache-2.0 is incompatible with GPLv2 (the
patent-termination clause) and fine with GPLv3, so "or later" resolves
it. The launcher can do this because it **links no QEMU code**.
`shader-chain` moves with it because both binaries link it.

**Rejected.** Moving the preset download out of process (leaves the
other crates). `aws-lc-rs` or `native-tls` for `ring` (OpenSSL's
licence instead). Shipping the 80 MB preset collection in packages.

**Addendum, 2026-09-05: permissive dual-licensing rejected.** `MIT OR
Apache-2.0` for our own code was tried and reverted the same day. It
fixes nothing (the player is GPLv2 because QEMU is), and the most
reusable piece, the guest serializer DLLs, is transient once M7c lands.
Copyleft keeps the novel work (the D3D protocol, the display drivers,
the CD model) open, and its likeliest reusers (86Box, DOSBox-X) are GPL
already. Permissive release is a one-way door.

## ADR-010: The player ships as a binary despite the GPLv2 / Apache-2.0 conflict (2026-09-05)

**Decision.** The player links GPL-2.0-only QEMU and has Apache-2.0-only
code in its tree, which on the FSF's reading cannot combine. **We ship
player binaries anyway**, state the position in the README and
`THIRD-PARTY-NOTICES.md` (with `COPYING`), and distribute complete
source and build scripts.

**Why the player cannot follow the launcher.**

1. *QEMU pins it to v2.* Of what an i386 softmmu build compiles, 411
   files are v2-or-later, 605 have no header, and **35 are v2-only**
   (`util/bitmap.c`, `util/qemu-sockets.c`, `hw/audio/ac97.c`, …).
   Re-check with `tools/gpl-scan.py` after a QEMU bump.
2. *The crates are not swappable.* `codespan-reporting` comes with naga
   and `rspirv` with librashader, so a clean tree means dropping wgpu
   and librashader, and the CRT chain is the product.

**Why it is defensible.** Every GPLv2 program on the modern Rust GUI
stack is in this position (`winit` alone settles it), and our source is
complete and buildable, which is what GPL enforcement protects. The
residual risk is a strict distribution declining to package the player,
or a QEMU copyright holder objecting.

**Rejected: `dlopen` instead of linking.** The FSF treats static and
dynamic linking alike, and our coupling is deep (one address space,
callbacks on QEMU's vCPU threads under the BQL, a shared audio ring).
The contrary reading is settled nowhere, and an "everything open
source" project should not lean on it.

**Kept on the table: QEMU in its own process**, the structurally clean
answer. ADR-002's latency premise, measured
(`tools/ipc-latency-spike.c`, x86-64 rig, 16 cores):

| | idle | all cores busy |
|---|---|---|
| frame notify round trip, hot loop | p50 8 µs, p99 11 µs | p50 11 µs, p99 15 µs |
| frame notify @60 Hz, cold receiver | p50 18 µs, p99 226 µs, max 459 µs | p50 17 µs, p99 35 µs, max 2.0 ms |
| buffer fd over `SCM_RIGHTS`, once per ring slot | 12–43 µs | 17–2800 µs |
| in-process callback (today) | 0.02 µs | 0.02 µs |

That is 0.1 % of a frame typically and ~1.4 % at p99, so **latency is
not what stops it**. The work does: the VGA surface through shared
memory, IOSurface handles over a mach port on macOS, and the lifecycle
rules, headless dumps and guest harnesses that assume one process.
`player-core/src/qemu_vm.rs` is the only module touching the embed API, so
it would be a track, not a rewrite. Re-run the spike on the Air first.
A distribution refusing the player is the signal to open that track.

## ADR-011: The product is 2ksbox; the application ID is `com._2ksbox.Launcher` (2026-09-05, amended 2026-09-06)

**Decision.** The project is **2ksbox** (the user registered
`2ksbox.com`); `win98-xp-virt` was a working name. At first only the
packaged identity moved. **Amended 2026-09-06**: the working name is
gone everywhere, including the repository
(`github.com/davidrios/2ksbox`), the checkout, the docs and the user's
data directory.

**Why the underscore.** No segment of a D-Bus-style name may start with
a digit, and `flatpak build-init` refuses `com.2ksbox.Launcher` ("Name
segment can't start with 2"). Escaping the digit is the established fix
(`org._7zip`).

**Which name goes where.** `2ksbox` (`launcher-core/src/paths.rs::NAME`)
names the commands `2ksbox` and `2ksbox-player`, the resource
directories (`share/2ksbox`, `lib/2ksbox`, `libexec/2ksbox`), the
tarball and the window title. `com._2ksbox.Launcher` (`paths.rs::APP_ID`)
names the desktop entry, the icon, the Wayland `app_id`, and the
Flatpak and AppStream ID.

**The data directory.** The library is `~/.local/share/2ksbox`, runtime
files under `$XDG_RUNTIME_DIR/2ksbox`. (On Windows `%APPDATA%\2ksbox\data`,
and from an installed MSIX `%USERPROFILE%\2ksbox`, outside the
`AppData` an uninstall deletes; 2026-09-23, `build-windows.md` "The
Store package".) `paths.rs::data_dir()` migrates
the old directory once, by a **rename in the same parent** (atomic),
**only when the new name is absent** (two present get a stderr line and
neither is touched), and **never fatally** (a failed move is a warning
and an empty library, fixed with one `mv`). The runtime directory is not
migrated.

## ADR-012: Win98 gets the same display driver as XP, over one shared core (2026-09-06)

**Decision.** Win98/Me gets a native display driver for `d3dpt-vga`, with
desktop modes from our table, a DirectDraw HAL and a Direct3D HAL on the
same protocol and executor. First, XP's driver is split into an
**OS-independent core plus a thin per-OS layer**, with XP rebuilt on it.
Track M10, design in doc 19.

**Why a driver.** The Glide + WineD3D stack needs per-game files and
cannot accelerate the desktop. The driver needs nothing beside a game
(98's own `ddraw.dll` / `d3dim.dll` drive it), and the DirectX 3–7
behaviour 98 titles depend on is what M7 already built. Step 0 confirmed the DDI structures and DP2 opcodes match NT's
(doc 19); on 9x the HAL is a ring-3 DLL in the game's process, so the
core may call no OS service.

**Why the core, and first.** About three quarters of `d3dptdisp.c`
(3,708 lines) states facts about our adapter and protocol, not NT. A
second copy would double every protocol bump and drift at the first
one-sided fix. The split lands alone, proven a refactor by the M7 suite
being byte-identical across it, before the 9x layer starts ("a 9x bug on
top of an unproven refactor is two bugs wearing one coat").

**Unchanged.** The M4 DLL device stays the path wherever the driver is
not installed. If 9x needs an adapter change, that is a finding, not a
licence to fork the register set.

## ADR-013: Below the Vulkan 1.3 floor, no DXVK device; no second executor (2026-09-06, amended the same day, 2026-09-21 and 2026-09-22)

**Status.** Amended three times:

- *2026-09-06.* Software Vulkan counts as available (below).
- *2026-09-21.* A **Windows** host below the floor runs the same
  executor on its own `d3d9.dll` (ADR-007's second amendment).
- *2026-09-22.* A **Linux or macOS** host below the floor runs it on
  Wine on the host, and WineD3D-in-guest is retired in M15's last step
  (ADR-018). That supersedes points 1 and 3 below; point 2 stands.

What this ADR says about the guest-side stack is history: ADR-018
retired it, and M15 step 6 removed it on 2026-09-23.

**Decision as written.** The paravirtual device needs a **Vulkan 1.3
device** on the host, because DXVK asks for exactly that
(`third_party/dxvk` v3.1 enforces `VK_API_VERSION_1_3` at instance
creation and per adapter). Hosts below it are still supported and get
qemu-3dfx's OpenGL pass-through with the Glide wrappers and
**WineD3D-in-guest**, which needs no Vulkan.

1. **WineD3D is not retired by M10.** The ISO's `WINED3D\` and `SETUP
   /GAME`'s renames stay. *(Superseded by ADR-018.)*
2. **The launcher probes and says so**, rather than a machine silently
   having no 3D.
3. **No second executor is built.** Doc 14 P0b's escape hatch (host
   WineD3D over GL, or a wgpu translator) stays open and unbuilt.
   *(ADR-018 takes the hatch with the one executor on Wine's d3d9.)*

**Who misses it.** Good hosts for this project, not antiques. A
2012-era x86 laptop runs KVM at the right speed for these guests:

| Host | Vulkan | Why it misses |
|---|---|---|
| Intel pre-Broadwell (HD 3000/4000) | none | Mesa's `anv` starts at Gen8 |
| Nvidia Kepler (GTX 600/700) | 1.2 | the 470 legacy branch; NVK starts at Turing |
| Nvidia Fermi, AMD TeraScale (HD 5000/6000) | none | no driver |
| macOS before 26; every Intel Mac | none usable | MoltenVK unsupported, KosmicKrisp needs 26 on Apple Silicon (ADR-007) |

All have OpenGL 2.1 or better, which is all the GL pass-through wants.

**Software Vulkan is used, and warned about** (the first amendment).
The first version refused lavapipe on the user's behalf, a judgement
about someone else's hardware. DXVK ranks a `CPU` device last but never
excludes it, so the probe counts it and says "available, in software
(slow)". Which path wins on a box is that box's measurement.

**Rejected.** Pinning DXVK 1.10.3 (the last Vulkan 1.1 release): a
second DXVK branch and patch queue for fewer features. Lowering DXVK's
bar: the 1.3 features are load-bearing. A GL executor now: a second
implementation of D3D9 semantics on a guess.

**Consequences.** The probe is `launcher-core/src/host_gpu.rs`,
`launcherx --host-check` prints its report, and the form's sentence is
`wizard::Form::d3d9_note()` in the shared model (doc 07). With no
executor the device reports it and the machine boots normally.

## ADR-014: One launcher library; front ends draw it (2026-09-06)

**Status.** The line between core and front end stands. "Two maintained
front ends" was ended by ADR-017; which one ships is ADR-015.

**Decision.** The launcher is **`launcher-core`** plus front ends that
draw it (then `launcher/` on egui and `launcher-qt/` on Qt 6 / QML via
cxx-qt), and `launcher-capi/` is the same core as a C ABI. A front end
owns the widgets, when to redraw, the file dialog and how it confirms
something destructive. **Everything else is the core**: each window's
state machine, its derived labels, and the sentences it shows.

**Why at behaviour, not at file formats.** For a few hours the Qt build
shared only the toolkit-free modules, and four divergences appeared that
no compiler could see: no processor field (a DOS machine came out
unthrottled), a network checkbox that did not follow the family, a note
saying "Windows won't see a card" on DOS machines, and a shader profile
that kept its overrides in one build and dropped them in the other. Each
was a rule in a `show()` function, copied once. The fix is having
nowhere to put the second copy.

**What it buys.** Every toolkit-free debug verb is `launcher_core::cli`,
so every caller answers `--paths`, `--discs`, `--wizard-new` and the
rest with one implementation. `launcherx` is `cli::run` alone, and the
test suite drives the models through it with no toolkit built.

**Rejected.** A Rust-only core. `launcher-capi` is a thin C ABI that
Swift can import directly; `launcher-capi/examples/smoke.c` (the `capi`
check) is a miniature front end that fails when a model's default
changes.

**Consequences.** Nothing a second front end could get differently goes
in a front end: not a default that follows the family, not a note under
a checkbox, not a combo box's labels. `launcher-qt` has its own cargo
workspace, so a root `cargo build` never needs Qt 6.

## ADR-015: The Qt front end is the one the packages ship (2026-09-07)

**Status.** Accepted. Its "keep `launcher/` as an unshipped second
front end" was reversed by ADR-017.

**Decision.** `launcher-qt` is **the** launcher. Every packager (Linux,
the Flatpak, macOS, Windows) installs it as `2ksbox`.

**Why one, and why Qt.** A product has one launcher, one set of
screenshots, one name in a bug report. With the front ends equal on
behaviour by construction, Qt won on the rest: real windows, the
platform's file dialog, decorations, HiDPI, colour scheme,
accessibility and input methods. It already ran as the Windows launcher
(M11), and it idles, where egui drew every 16 ms.

**Costs.** The Linux tarball depends on the host's `qt6-base` +
`qt6-declarative`; the Flatpak moved to `org.kde.Platform` 6.10; macOS
and Windows carry Qt. The shader preview reads back through a second
wgpu device (doc 07).

**Rejected.** Bundling ~38 MB of Qt in the tarball that every
distribution ships (the Flatpak is for a host without Qt).

**Consequences.** `scripts/build.sh` has a `qt` stage; a host with no Qt
6 builds everything else and rolls no package. Every packager opens a
**real window offscreen** and requires a PNG, because Qt resolves its
platform plugin and QML modules by name at run time, invisible to every
import-table check (doc 07, "Shipping Qt").

## ADR-016: The Voodoo 2 is emulated, beside the Glide pass-through, not instead of it (2026-09-12)

**Decision** (user decision). 2ksbox carries a real 3dfx Voodoo 2 on the
PCI bus: 86Box's emulation (PCem's rasterizer, 86Box's recompilers;
GPL-2.0+), vendored **verbatim** under `voodoo/` and wrapped as `-device
voodoo2` through a shim of 86Box's platform headers, with no edits to
the vendored files (doc 21).

**Why.** Completeness. The guest runs 3dfx's own drivers and the game's
own Glide, so Glide 3, LFB tricks and statically linked Glide 2 work
with no translation for a game to find a hole in.

**What it does not replace** *(as written; the Glide half of this
paragraph is superseded by ADR-020)*. qemu-3dfx's Glide half (`hw/3dfx`
+ OpenGLide) stays as the fast path, because the host GPU draws, where
the chip is a software rasterizer on host cores. The machine form picks.
Its OpenGL half (`hw/mesa`) serves GLQuake, Quake II, Half-Life GL and
wglgears, which a Voodoo 2 reaches only through 3dfx's MiniGL at
rasterizer speed. Never propose retiring `hw/mesa` on the strength of
this device. Direct3D (docs 14/15) is a third thing.

**Costs accepted.** Every register write is an MMIO trap with a BQL
round trip; mitigations only where the profile asks. The command FIFO in
guest RAM (`ramfifo=on`, doc 21 §9) was taken; a BQL-free region was
not, since what still traps is mostly status polls. SLI and the Voodoo
Graphics type are not offered.

## ADR-017: The egui front end is retired (2026-09-13)

**Decision** (user decision). `launcher/`, the egui/eframe front end, is
deleted. `launcher-qt` is the only front end, and `launcher-core` keeps
every rule. `launcher-capi` (with `smoke.c`) and `launcherx` remain as
the core's other callers. This reverses ADR-015's "keep `launcher/`" and
ends ADR-014's two front ends; ADR-014's line is unchanged.

**Why.** Installed by nothing, opened by no check and kept alive only by
`cargo check`, it no longer proved the boundary, while costing ~2,300
lines, ~70 crates only it used (eframe, accesskit, harfrust, icu) and a
Flatpak vendoring all of them. `launcherx` through `scripts/test.sh` and
`smoke.c` keep the boundary checked.

**What went with it.** The `--diag-*-frame` and `--pick-file` /
`--pick-folder` verbs (the Qt build's offscreen screens,
`LAUNCHER_QT_SCREEN`, are the headless frame grabs now), and the core
API only egui called.

**Consequences.** `launcher-capi` is the root workspace's one
non-default member, kept compiling by `cargo check --workspace`. A
second native front end goes over `launcher-capi` or `launcher-core`,
never a revived `launcher/`.

## ADR-018: Below the Vulkan floor, the executor runs on Wine on the host; WineD3D-in-guest is retired (2026-09-22)

**Decision** (user decision: "running an old unsupported Wine version in
the guest is bad UX and doesn't make sense"). On a **Linux or macOS**
host below DXVK's Vulkan 1.3 bar (ADR-013's table), the paravirtual
device keeps running (same guest driver, protocol and executor) with the
D3D9 supplied by **Wine's `d3d9.dll` (WineD3D over OpenGL) on the
host**. Wine's d3d9 exists only inside a Wine process, so the executor's
Windows build runs out of process under Wine. The order is in-process
DXVK wherever Vulkan 1.3 exists, then Wine, then `D3DPT_STATUS_NO_EXEC`.
A Windows host's second choice is its own `system32\d3d9.dll` in process
(ADR-007's second amendment), so the two platforms get one answer.
Design: doc 14 §"The executor on Wine, in another process"; steps:
`docs/tracks/m15-wine-executor.md`.

**WineD3D-in-guest is retired**, and was removed on 2026-09-23 (M15
step 6, one commit) once the host path had drawn the reference scene
within the rig budget and run a real game on a real macOS 15: the ISO's
`WINED3D\` folders, `SETUP /GAME 4`/`5`, `/I 7` with `D3DPRE.EXE` and
the `DDRAWME`/`DDSYS` switcher, the wine9x build and its patch queue,
the two test scripts, and the launcher's advice pointing at them. A
below-floor host with no Wine now has no Direct3D pass-through; the
driver keeps its DirectDraw half, OpenGL still passes through and the
Voodoo 2 still works.

**Why.** UX, entirely. The fallback asked the user to copy a 2015 Wine
(1.7.55) beside every game from a CD folder, an unsupported copy of
what the host runs current. It was cheap enough to decide without
ADR-013's "measure the users first": the executor already ran on
foreign d3d9s, its API was built for a process boundary (five calls,
four callbacks, every pointer the window or VRAM), and Wine's d3d9 does
not refuse what Windows' did.

**Supersedes** ADR-013's point 1 (by the sequenced retirement) and
point 3 (one executor on a second D3D9, not a second executor). ADR-013's
point 2 gains a third answer, "Direct3D through Wine on the host", and
"install Wine" where there is none. The Vulkan 1.3 floor stays the floor
of the DXVK back end; the GL path and the Voodoo 2 are untouched.

**Rejected.** A native wined3d `.so` (there is none; it is a PE
module). An executor of our own over GL/Metal/wgpu (a second D3D9
implementation, which WineD3D is, twenty years in). Native arm64 Wine on
macOS (`winemac.drv` has no OpenGL there), so the Wine process is x86_64
under Rosetta.

**Consequences.** Wine is a *runtime* companion, reported by `player
--companions` and `launcher --paths`. No package ships a Wine, the
form's note says which to install. The Flatpak is the exception: the
sandbox cannot run the host's Wine, and Flathub lists no second app, so
its Wine is an add-on extension of the app, `com._2ksbox.Launcher.Wine`
(user decision, 2026-09-23; M15 step 7). `-global d3dpt-vga.exec=wine` is the A/B on a host
with both back ends, and `no-exec=on` still means no executor at all.

## ADR-019: Two macOS builds, App Store on macOS 26+ Apple Silicon and community at Homebrew's floor with the Wine executor, Intel permitted (2026-09-22)

**Decision.** One `scripts/package-macos.sh` rolls two apps:

1. **App Store.** macOS 26+, Apple Silicon only. DXVK with the
   KosmicKrisp ICD and nothing of Wine (no x86_64 helper, no Rosetta
   prompt), so its sandbox story is QEMU's JIT entitlement alone (UTM
   is the precedent).
2. **Community** (`--community`). Homebrew's floor
   (`scripts/macos-floor.sh`; 15.0 since Homebrew dropped Sonoma on
   2026-09-10, and the number follows `brew update`), the M15 Wine
   executor pair in. A Developer ID-signed, notarized DMG on the GitHub
   release, with the from-source build as the alternative. It **permits
   Intel Macs**, because the Wine is x86_64 on both architectures and
   only this build starts it.

The App Store never gets a pre-26 version: it keeps one current version
per app, and two listings with different minimums are reviewed as
duplicates (guideline 4.3). The pre-26 path lives on the release page.
Both apps stage the Vulkan loader and KosmicKrisp ICD, so the community
app on a 26 Mac still takes DXVK (`build-macos.md`, "Two builds").

**Why.** Mac hosts split exactly on the executor's line. On 26,
KosmicKrisp gives DXVK its Vulkan 1.3. Below it, the executor runs on
Wine under Rosetta, a payload and a prompt the store build should not
carry. One build with a run-time choice would put Wine in every 26
user's download for nothing. Splitting costs one packager flag.

**Intel Macs**, out of scope since 2026-09-12, are reopened by the user
*for the community build only*: **permitted, untested.** Homebrew 7.0.0
(2026-09-13) put Intel at tier 3 (bottles frozen, support ending
September 2027). No row claims Intel until an Intel Mac runs the
reference scene.

*Addendum (2026-09-23, user: "the intel version is obviously macos 15
only").* The Intel app is made on the Apple Silicon Mac under Rosetta
(`scripts/build.sh --x86_64`, `package-macos.sh --x86_64`;
`build-macos.md` "The Intel build"). It is the community build and
nothing else, and it carries **no Vulkan**: KosmicKrisp exists only as
arm64 and MoltenVK is refused, so no DXVK and no in-process executor
either. Its Direct3D is the executor on native x86_64 Wine, and a Mac
with no Wine has none.

*Second addendum (2026-09-23, user: "depending on homebrew at all was a
mistake").* Homebrew's installer refuses Intel Macs since 7.0, which
would have ended the Intel build before it started, and Homebrew's
bottle floor was the app's floor. So **nothing the app carries comes
from Homebrew any more**: `scripts/build-deps.sh` builds QEMU's
libraries (static) and Qt 6.9.3 from pinned upstream tarballs for the
floor and either architecture (`build-macos.md` "The libraries"), and
**the community build's floor is macOS 12**, the lowest the newest Qt
line that reaches it allows (`scripts/macos-floor.sh`; 6.5 would reach
11, but its open-source line ended in 2023). Homebrew remains a source
of build tools and of recipes to crib from.

**What stays open.** The store build adds the sandbox and whatever
review asks. The store's licensing question (GPL-2 QEMU and 86Box under
store terms; UTM ships there, and the FSF says it cannot) is the user's
to weigh. The data directory and disc shelf under the sandbox
(security-scoped bookmarks) are M6's work.

## ADR-020: The Glide pass-through is removed; the emulated Voodoo 2 is the only Glide (2026-09-23)

**Decision** (user decision: "remove the glide passthrough. running the
emulated voodoo 2 card is a much better experience"). qemu-3dfx's Glide
pass-through is gone from 2ksbox: the OpenGLide submodule and its patch
queue, `glidept/` (the window-less platform layer), `scripts/build-glide.sh`
and the `glide` build stage, patch 33 (the host-ops handshake), the embed
provider's Glide half, the guest `GLIDE*.DLL` and the DOS `GLIDE2X.OVL`,
SETUP's Glide sets, `GLIDETEST.EXE`, the `glide-host` check and
`tools/glide-guest-test.sh`, and the packagers' `libglide2x` staging.
Patch 74 takes `hw/3dfx` out of the QEMU build and off the PC machine
(the directory is still overlaid because qemu-3dfx's `sign_commit` stamps
a file in it). **A Glide game, Windows or DOS, runs on the emulated
Voodoo 2 with 3dfx's own driver and its own Glide** (ADR-016, doc 21).
The OpenGL pass-through (`hw/mesa`) and the device mapper it and the
Direct3D DLLs share are untouched; the ISO's `GLIDE\` folder is now
`MAPPER\`, SETUP's component 2 is "The device mapper", and the Glide
game sets are gone (the sets are 1–3).

**Why.** The card is complete by construction (Glide 2 and 3, static
links, every LFB trick) where the wrapper covered Glide 2 alone and
needed a patch per title; the user plays on the card and never had the
wrapper work by hand; and the wrapper's cost was real: a submodule with
a patch queue, a platform layer, a QEMU patch, two checks, a build stage
on three platforms (none of them Windows) and a folder of guest DLLs
whose names collide with 3dfx's own. What the wrapper had over the chip
was speed on the host GPU, which the user did not miss.

**Supersedes** ADR-016's "beside the Glide pass-through, not instead of
it" and doc 12 §5; ADR-016's device and its verbatim-vendoring rule
stand. The stacks the OpenGLide patch README listed as not taken (a
guest-side Glide→OpenGL wrapper over the GL pass-through, nGlide /
dgVoodoo2 over our Direct3D, a Glide 3 layer in OpenGLide) stay not
taken, for the same reasons and now also because the card covers them.

**Rejected.** Keeping `hw/3dfx` compiled with no wrapper (a device that
only refuses, and a `glidept` MMIO region on every machine for nothing).
Keeping the OpenGLide checkout for its Glide SDK header alone:
`DITHTEST.EXE`, the Voodoo 2's dither probe, now compiles against
`guest-tools/src/glide2sdk.h`, a subset of the Glide 2.4 ABI written from
the public reference.

## ADR-021: The display driver becomes a DirectX 9 driver; no per-game graphics DLLs (2026-09-25)

**Decision** (user decision: "let's start the holy grail, our native
directx 9 driver. no more custom dlls anywhere"). The `d3dpt-vga`
driver answers Microsoft's `d3d9.dll` as a DirectX 9 driver with shader
model 3.0, on XP and Win98, through the same core, protocol and executor
as its DirectX 8 DDI. When it does, the ISO's per-game graphics DLLs go:
`D3D8.DLL`, `D3D9.DLL` and `DDRAW.DLL` (`D3DPT\`), then `OPENGL32.DLL`
(`OPENGL\`), which an OpenGL ICD installed by the driver's INF replaces.
The SysBus `-device d3dpt` and the device mapper leave with the last DLL
that uses them. `DINPUT.DLL` is input, not graphics, and stays per game
(user: "leave dinput for now"). A component Windows loads because the
driver's INF installed it (the display DLL, the 9x HAL, the ICD) is part
of the driver, not a custom DLL. The work is track M16
(`docs/tracks/m16-dx9-ddi.md`); Wine's `d3d8` / `d3d9` test suites,
run on the reference rig and in the guest, are its conformance check.

**Why.** The DLLs bypass the runtime, so every API corner the runtime
would have handled is ours to write, and doc 14's stub list is what
remained unwritten. They are copied per game, which a user must know to
do. On 9x a DLL beside a game reaches only the session's first
DirectDraw program (doc 19 §42). The DX8 DDI already showed the other
route works: XP's `d3d8.dll` runs Vice City and Max Payne with no DLL.
SM3 rather than SM2 first: the bytecode passes through to DXVK, so the
claimed version is caps, and SM3's additions (vertex texture fetch,
instancing) sit beside the SM2 work rather than changing it. A flag caps
the claim at 2.0 for A/B.

**Done for Direct3D** (M16 step 7, 2026-09-27): the three DLLs, their
sources and the SysBus device are gone; SETUP's file set 1 is a retired
placeholder so set 3 keeps its number, and `DINPUT.DLL` moved to its own
`DINPUT\` folder.

**Amended 2026-09-27: the OpenGL ICD is left open** (user decision, "I
don't think that's worth it, at least for the time being"). M16 closed
with step 7; `OPENGL32.DLL` stays the per-game copy from `OPENGL\`, and
the device mapper stays with it. The rejection below of a wrapper that
stays per game is suspended for OpenGL until the ICD is taken up.

**Amends** ADR-006 and ADR-008: the paravirtual device and its executor
stand, and the guest DLLs that ADR-008 staged the driver after are
retired rather than kept as the 9x per-game path (doc 14 "The guest
DLLs").

**Rejected.** Claiming SM2 first and SM3 in a later milestone (the same
DDI work, tested twice). Keeping the DLLs as a fallback beside the
driver: two paths to keep equal, and the DLL path is the one with the
stubs. An OpenGL wrapper that stays per game after the Direct3D DLLs go:
the point is a game folder nobody touches.

## ADR-022: Aero on Windows 7 through a WDDM driver of our own, beside the XP one (2026-09-27)

**Decision** (user: "ultimately what I want is to run full Aero on
windows 7"). Windows 7's compositor runs only on a WDDM driver, so the
guest gets a second display driver: a WDDM 1.1 kernel-mode driver and a
Direct3D 9 user-mode driver over the same `d3dpt-vga` device, protocol,
executor and DXVK. The XP-model driver stays: it is XP's and Win98's
driver, and Windows 7's fallback (it already runs there, track M18 step
1). **The WDDM headers are our own** (user decision): the subset we use,
written from Microsoft's public documentation, with a small shim for
`DxgkInitialize`, so the driver keeps the Linux cross build and the
open-source rule. **32-bit first** (user decision): the user's Windows 7
is 32-bit and loads an unsigned driver after a prompt; 64-bit and its
signing are a later step.

**Why.** DWM composes through Direct3D 9Ex surfaces shared between
processes, which only WDDM's video memory manager provides; no XDDM driver
gets Aero. The host side already executes a Direct3D 9 stream, so the
new work is the guest's half and the device's interrupt, fence and DMA.

**Rejected.** Building the WDDM parts with the WDK on the user's PC: faster
to start, but it ties the driver to a Windows build machine and to the
WDK's licence. Stretching the XP-model driver: no path to DWM exists
there.


## ADR-023: The launcher moves to mitsuami, native widgets on every platform (2026-09-27)

**Status.** Accepted; supersedes ADR-015 once track M19 flips what the
packages ship. Since 2026-10-02 (user) `launcher-mitsuami` is the
official launcher and `launcher-qt` is deprecated: no more changes go
into it, though the packages still build it until M19 moves them over.

**Decision** (user decision). The launcher's next front end is
`launcher-mitsuami/`, on mitsuami (`github.com/Roboport-Tecnologia/mitsuami`,
the user's own Rust toolkit): AppKit on macOS, WinUI 3 on Windows, GTK 4
on Linux by default, and Kirigami with the `kde` feature. It is a second
thin view over `launcher-core` until it has every window, then it replaces
`launcher-qt` in every package, as egui went (ADR-017). It depends on
mitsuami by a **pinned git revision** (user decision), bumped by hand.

**Why.** Each platform's own controls, not Qt Quick's approximation of
them; no Qt to carry on macOS and Windows; the toolkit is written against
this launcher's needs (its `ARCHITECTURE.md` §10 lists what 2ksbox
drove: `NumberInput`, `Image`, tooltips, modal windows, the toolbar,
lists).

**Costs.** The Windows launcher needs MSVC and the Windows App Runtime
2.4 (user: a native Windows build is fine), so it leaves the Linux cross
build; the player and QEMU stay on it (separate processes). The Linux
build needs GTK 4.10+ development files; the Flatpak either moves to a
GNOME runtime or builds with `kde` on `org.kde.Platform`, decided when
it is packaged.

**Unchanged.** ADR-014: every rule and every sentence stays in
`launcher-core`. wgpu stays in `launcher-core` for the preview (mitsuami
takes its pixels in an `Image`); the player keeps winit (a mitsuami player
was a separate question, answered by ADR-025).

## ADR-024: A general-purpose VM manager, best at vintage boxes (2026-10-01)

**Status.** Accepted (user decision: "lets be a general purpose vm
manager, that just so happens to also be awesome at emulating vintage
boxes"). Removes doc 01's "modern guests" non-goal. Work in track M20.

**Decision.** 2ksbox runs current guests as well as era ones. The first
is Windows 11 (track M20), asked for by the user so people who need it
for work can run it on a Mac. A modern box uses the same launcher,
bundle format, snapshots, player and in-process QEMU as a vintage one,
but none of the era devices: no `d3dpt-vga`, no Voodoo 2, no
guest-tools ISO, no CRT shader by default. On Apple Silicon a modern
guest runs as ARM64 under Hypervisor.framework (`aarch64-softmmu`,
`-accel hvf`). Emulating a current x86 system under TCG is too slow for
work there. Vintage boxes stay x86 under TCG.

**Why.** Everything a modern box needs from us outside QEMU already
exists for the vintage ones: the machine library, the wizard, snapshots,
the disc shelf, packaging on three platforms. A second app for modern
guests would duplicate all of it. UTM covers the Mac only and is not our
stack.

**Unchanged.** Every locked decision: QEMU as the base (ADR-001),
in-process (ADR-002), Rust where possible (ADR-004), one launcher
library (ADR-014), everything open source, nothing shipped from
Homebrew. Vintage work keeps its own tracks and its own order in doc 00.

**Costs.**

- The player links one QEMU (`libqemu-embed-i386`), and has to keep
  linking it (patch 63, doc 22 §5.0). So each target is a player binary
  of its own: `2ksbox-player-x86_64` for a modern PC guest (M20 step 3),
  aarch64 on a Mac later. Packages grow by a player and a QEMU library
  per target.
- The trimmed QEMU (`no-optionals`, the patches README) gains what a
  modern guest needs: the TPM and its backend, NVMe, virtio, ramfb, HVF.
- New libraries to build from pinned sources: libtpms and OpenSSL's
  libcrypto (M20). EDK2 firmware, which QEMU's `pc-bios/` already
  carries.
- Doc 01's integration features (clipboard, shared folders, the guest
  resizing its screen to the window) stay later work, but a modern
  guest's users will expect them sooner than a game player does.

## ADR-025: The player moves to mitsuami, over a shared `player-core` (2026-10-01)

**Status.** Accepted (user decision: "now lets make the mitsuami
player"). Work in track M22. Until it runs on every host, `player/` (winit)
ships.

**Decision.** The player gets a mitsuami front end, `player-mitsuami/`,
its picture on mitsuami's `GpuSurface` (a desync Wayland subsurface or an
X11 child window on Linux, a `CAMetalLayer` view on macOS, a child HWND on
Windows) and the platform's menus over it. Everything but the window moves
into `player-core/`, which both front ends drive, as `launcher-core` holds
the launcher's rules (ADR-014): the QEMU thread, the picture and its CRT
chain, audio, gamepads, what the guest holds, the command line. The user
chose this shape over a copy of the modules or a rewrite of `player/` in
place, GTK on Wayland as the first host, and a menu bar over a window that
is otherwise only the picture.

**Why.** The player's window then has the platform's menus, alerts and
full screen, and what the winit player built by hand per windowing system
(the pointer lock's raw motion, the guest's cursor, keys a keymap moved)
is the toolkit's. The latency question was measured first (2026-09-27,
the spike in `tracks/m22-mitsuami-player.md`): a desync subsurface
presented to in Mailbox matched the winit window at the median, where
GTK's own offload waited a refresh for its frame clock. Its other
precondition, QEMU's main loop off the toolkit's `GMainContext`, is the
private GLib `libqemu-embed` links on Linux since 2026-09-27.

**Costs.** The player is still linked to QEMU, never opened at run time
(patch 63), so it stays one binary per embed target, now per toolkit too
while both front ends exist. On Windows it needs the native MSVC build
mitsuami needs (ADR-023). The keyboard grab mitsuami gives is weaker on
Windows and macOS than the winit player's capture (doc 03 "Input path"),
so those hosts wait for it to match.

