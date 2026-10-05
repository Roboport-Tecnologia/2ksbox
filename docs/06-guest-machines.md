# 6. Guest machines: the families

The launcher ships its era machine families with tested defaults; users
supply their own OS media and licences. The project is built around
Win98, XP and DOS; **Windows 7** (32-bit, track M18) is XP's PC with
Aero on our WDDM driver. **Other** is the catch-all for any other era OS
(BeOS, a period Linux, OS/2), with standard hardware and nothing of
ours. **Windows 11** (ADR-024, track M20) is the one modern family: a
different PC (q35 or Arm's `virt`, UEFI, a TPM), below. The defaults
live in `launcher-core/src/bundle.rs`
(`video_choices`, `sound_choices`, `music_choices`, `pad_choices`,
`default_*`); each choice list's first entry is the family's default.
The machine form is doc 07, the sound and music devices doc 20, the
display drivers docs 15 and 19, the Voodoo 2 doc 21.

**Common to the era families** (all but Windows 11). i440FX + PIIX on
a versioned board (`pc-i440fx-<ver>`, "Windows 98 SE" below),
`-cpu pentium3` (`max` on Windows 7; with the form's optimization switches as its
properties), IDE disk
(qcow2) and our ATAPI CD (doc 17). **No network card** on a new machine
or a bundle that doesn't ask for one, so an unpatched guest stays off
the network (`-nic none`; QEMU otherwise adds an e1000). **No gamepad**
until one is picked (`bundle::Pad`, M13). A Voodoo 2 checkbox, off
(`voodoo2,addr=0x05`). The display adapter is always PCI `0x02` and the
NIC and sound card are pinned at `0x03` and `0x04`, so turning
networking off or changing the adapter never slides a card into
another's slot; an installed guest re-detects a card that moves.

## Windows 98 SE

Modeled as a ~1998–2000 consumer PC.

| Component | Default (alternatives) | Why |
|---|---|---|
| Machine | `pc-i440fx-<ver>,hpet=off` | The board is versioned and kept per bundle (`Machine::board`): a machine stays on the QEMU version it was created on, so a QEMU upgrade cannot break its live snapshots; a new one gets `pc-i440fx-11.1`, and a bundle with no `board` predates the field and gets `pc-i440fx-9.2`. 98 has no HPET (`PNP0103`) driver and never uses one; it showed as an Unknown Device (the `hpet` check). fw_cfg (`QEMU0002`) has no driver either, but its `_STA` hides it |
| Accel | TCG | under KVM Explorer dies at startup (*SHELL32.DLL is linked to missing export SHLWAPI.DLL:GetFileAttributesA*): no Start menu |
| CPU | `pentium3` | avoids CPUID features and fast-CPU bugs 9x mishandles; the floor, as our guest wrappers are built `-march=pentium3` |
| RAM | 256 MB (32–512) | 9x VCACHE sizing overflows much above 512 MB |
| Video | `d3dpt-vga` + our driver (`cirrus`) | the whole display path (doc 19): mode table, desktop from VRAM, paced page flips, Direct3D. Default by user decision; the Cirrus (Windows' in-box driver) is the A/B |
| Sound | SB16 + OPL3 (AC'97, Gravis Ultrasound, none) | in the box, and what a DOS box inside 98 expects. The AC'97 needs a driver 98 lacks and the ISO doesn't carry yet; the Gravis needs Gravis's own |
| Music | MPU-401 at 0x330, General MIDI (CM-32L, none) | 98 has no wavetable synth; its MIDI output is the FM chip. Add "MPU-401 Compatible" in Add New Hardware to get the port |
| Net | PCnet when on | in-box driver |
| Pointer | USB tablet (`seamless_mouse`) | absolute, nothing grabbed; off leaves PS/2 relative mode for mouselook (doc 03) |
| Gamepad | none (USB HID pad, gameport, key mapping) | the one family offered both devices (below) |
| Floppy | enabled | drivers, utilities, boot disks |

**Gamepads.** 98 SE binds its in-box HID driver to the USB pad (asking
for the Windows 98 source files the first time); a Windows game reads it
through DirectInput **and** winmm's `joyGetPosEx` over VJOYD (the
`pad-guest-98` check measures both). "Standard Game Port" gives a
Windows game nothing, so M13 doesn't install it. The gameport at 0x201
is for a DOS box under 98 and 1995 titles; it is not Plug and Play (Add
New Hardware, then calibrate), and the form says so.

**QEMU-side traps** (`patches/qemu/README.md`):

- 98 faulted with exception 0D on first boot under 9.2.4's TCG until
  the upstream LSS fix (issue 2987); QEMU 11.1 has it, so patch 01 was
  dropped in M21.
- TCG has faulted RUNDLL32 in Display Properties since 7.2 (issue 1964).
  The user has not seen it on our build for a long time (2026-09-23);
  the cause of the change is unknown. Cosmetic; KVM/WHPX were never
  affected.
- The OpenGL pass-through (qemu-3dfx's `hw/mesa`) needs a context
  provider, which the player registers and a bare `qemu-system-i386`
  does not (doc 12).

**Guest setup.** Install from the user's CD image, then the guest-tools
ISO (`SETUP.EXE`, `guest-tools/README.md`). Still to document:
DOS-compatibility-mode storage regressions.

### The install must come out ACPI

A PnP-BIOS install leaves the PCI bus un-enumerated: "Plug and Play
BIOS" with a yellow ! and no PCI device found (no USB tablet, AC'97 or
NIC). Setup's `DetectACPIBIOS` (`sysdetmg.dll`, PRECOPY1.CAB) decides
from the **legacy BIOS date at F000:FFF5** against the `ACPICheckDate`
its own `machine.inf` writes:

```
; machine.inf, [BaseWinOptions] → ACPI_BASE
HKLM,Software\Microsoft\Windows\CurrentVersion\Detect,ACPICheckDate,,"12/01/99"
```

A BIOS at least that new is believed; an older one only if it matches
`BIOSINFO.INF`'s `[GoodACPIBios]` (four 1998 machines, by ACPI OEM id).
SeaBIOS says **06/23/99** and QEMU's tables say `BOCHS `/`BXPC`, so setup
fell back to PnP-BIOS. (`BadACPIBios` never matches us; the `ASL
Compiler version < 1.0` gate applies only to `MSFT`-created tables.)

`SETUP /p j` forces ACPI, but a launcher cannot type that at a DOS
prompt, so **`scripts/prepare-qemu.sh` stamps every `pc-bios/bios*.bin`
to `12/31/99`** (eight ASCII bytes ending three from the end of each
image; 1999 because the comparison is on a two-digit year). Nothing else
reads the field: `BIOSINFO.INF`'s `date=` quirks all want exact 1994–96
days, and the guest's clock comes from the RTC. The `bios-date` check
asks a running QEMU what a guest reads there. A plain `SETUP` from the
98 SE CD comes out ACPI (user-confirmed). Repair an image installed
before the stamp through Device Manager (PnP-BIOS → PCI Bus,
`docs/build-macos.md`) rather than reinstalling.

Kept in reserve: `-machine pc,x-oem-id=COMPAQ,x-oem-table-id="CAPONE  "`
matches `[CompaqCapone]` (its rules want FACP OEM revision ≥ 1 and RSDT
creator revision ≥ 0, both hardcoded to 1 in `hw/acpi/aml-build.c`) with
no firmware change, at the price of a vendor's name on every table.

## Windows XP

Modeled as a ~2002–2005 PC.

| Component | Default (alternatives) | Why |
|---|---|---|
| Machine | `pc-i440fx-<ver>` | best compatibility with XP-era drivers; q35 unnecessary. Versioned as on 98 |
| Accel | Automatic (KVM or WHPX with TCG behind it; TCG on macOS) | prefer an era CPU model for games: KVM `-cpu host` breaks Max Payne's level loading |
| RAM | 512 MB (64–3072) | the top is the practical 32-bit limit, below where PCI space eats RAM |
| Video | `d3dpt-vga` + our driver (`cirrus`) | doc 15: the host's mode table (640×480…1600×1200, 16/32 bpp, 60/75/85 Hz), desktop from VRAM, Direct3D up to 9 through Microsoft's own runtime (track M16). Plain VGA (vga.sys, 800×600×4) until the driver is installed. The Cirrus is XP's in-box 2D driver (up to 1024×768×16). The standard VGA has **no** XP driver and is not offered |
| Sound | AC'97 (SB16 + OPL3, none) | XP's in-box AC'97 driver; the SB16 for a DOS-era title |
| Music | none (General MIDI, CM-32L) | XP ships a wavetable synth and wouldn't use the port |
| Net | RTL8139 when on | in-box driver |
| Pointer | USB tablet (`seamless_mouse`) | as on 98 |
| Gamepad | none (USB HID pad, key mapping) | XP binds `hidusb.sys` on first start and shows the pad to DirectInput and `joy.cpl`; nothing to install. No gameport: nothing enumerates a non-PnP port here |

SP3 recommended; activation is the user's affair with their own licence.
Uniprocessor ACPI HAL; SMP under TCG is a measured decision for later.

## Windows 7

32-bit Windows 7 (track M18, since 2026-10-04): XP's PC with Windows 7's
in-box cards, and the machine Aero runs on.

| Component | Default (alternatives) | Why |
|---|---|---|
| Machine | `pc-i440fx-<ver>` | as XP; Windows 7 drives the PIIX and the HPET in the box |
| Accel | Automatic | as XP: KVM where the host has it, emulated on a Windows or macOS host (about three minutes to the desktop under TCG) |
| CPU | `max` | what all of M18's Windows 7 work ran on. Windows 7's own software wants SSE2, which `pentium3` lacks |
| RAM | 2048 MB (1024–3072) | setup wants 1 GB; the top is XP's 32-bit ceiling |
| Disk | 40 GB | setup wants 16 GB |
| Video | `d3dpt-vga,irq=on` + our WDDM driver (`std`) | the interrupt is what dxgkrnl starts a WDDM adapter on; SETUP installs the WDDM driver on such a machine and sets DWM's `CompositionPolicy=2`, so Aero composes (track M18 findings 11, 12). The standard VGA through Windows 7's VBE driver is the 2D fallback; Windows 7 has no Cirrus driver |
| Sound | HD Audio, ICH9 + `hda-duplex` at `0x1b` (none) | Windows 7's in-box driver; it has none for the AC'97 or the SB16 |
| Music | none | Windows 7 has its own synthesizer; the MPU-401 is not tried there |
| Net | Intel PRO/1000 (e1000) when on | in-box driver |
| Pointer | USB tablet (`seamless_mouse`) | as on XP |
| Gamepad | none (USB HID pad, key mapping) | as on XP |

Aero needs an edition that has it: Home Premium, Professional, Ultimate
or Enterprise; Starter and Home Basic say "This edition of Windows doesn't
support themes". On a fresh install, SETUP from the guest-tools disc and
one restart are all it takes (track M18 finding 14): the desktop comes
back at 1024x768 on the WDDM driver, composed.

64-bit Windows 7 boots on the same machine but loads no unsigned kernel
driver, so it gets no Aero; SETUP says so and installs the XP-model
driver instead. An XP-family bundle holding a Windows 7 install (the
user's `win7`, from before this family) has no interrupt on its adapter,
so SETUP gives it the XP-model driver too.

## Windows 11

The modern family (ADR-024, track M20): 64-bit, on its own QEMU target
(`Machine::modern_args`, `arm_args`), with none of the era work: no
`d3dpt-vga`, no Voodoo 2, no guest-tools ISO, no MIDI port, no gameport.

| Component | x64 | Arm (`arch = "aarch64"`) |
|---|---|---|
| QEMU | `qemu-system-x86_64` | `qemu-system-aarch64` |
| Machine | `q35,smm=on` | `virt,gic-version=3` |
| Accel | Automatic: KVM with TCG behind it (a Windows host runs no Windows 11 yet: no TPM there) | Automatic: HVF on a Mac (KVM on an Arm Linux host) with TCG behind it |
| CPU | `max`, 2 to 4 vCPUs (`default_cpus`) | `max`, the same count |
| Firmware | EDK2's secure build from `pc-bios`, its variables the machine's own qcow2 (`efi_vars`) | our own EDK2 (`scripts/build-edk2.sh`), the same |
| TPM 2.0 | libtpms in QEMU (patch 75) on `tpm-crb`, state in `tpm_state` | the same on `tpm-tis-device` |
| RAM / disk | 4096 MB (4096 to 32768) / 64 GB | the same |
| Disk, CD | AHCI: disk on `ide.0`, the user's CD on `ide.1`, the drivers disc on `ide.2` | `ich9-ahci,id=ide`, the same layout |
| Video | `std` (Basic Display Adapter) | `ramfb` plus `virtio-gpu-pci` (viogpudo from the drivers disc) |
| Sound | HD Audio (none) | HD Audio (none) |
| Net | e1000e when on | `virtio-net-pci` when on (NetKVM from the drivers disc) |
| Input | PS/2 keyboard; tablet and pad on `qemu-xhci` | `usb-kbd`, `usb-tablet` (or `usb-mouse`) on `qemu-xhci` |

A new machine gets the host's own architecture (`Arch::native`). A Mac
runs only the Arm one. With `clipboard`, a `qemu-vdagent` serial port
carries the clipboard; `shared_folder` (with networking) is the
player's SMB share (track M23, doc 24). Host support and measurements:
`docs/tracks/m20-win11.md`.

## DOS

Modeled as a ~1994 PC: the same board, a Sound Blaster, and nothing else.

| Component | Default (alternatives) | Why |
|---|---|---|
| CPU model | `pentium3` | unchanged on purpose: a 486 is its rate, not its CPUID string |
| **CPU rate** | **486DX2-66** (`cpu_speed`) | what makes this a DOS machine (below) |
| Accel | TCG | forced: `-icount` and KVM cannot coexist |
| RAM | 64 MB (4–256) | the rest of the first megabyte is XMS for a mid-90s extender; inside what MS-DOS 6.22's HIMEM.SYS manages |
| Video | `std` (`cirrus`) | a DOS title programs the adapter itself, so the choice is **which VESA BIOS it finds**: the Bochs adapter's VBE 2.0 with a linear frame buffer (the default, user decision) or the Cirrus's period one. The A/B for a game whose modes come out wrong. No `d3dpt-vga`: it has no DOS driver |
| Sound | SB16 + OPL3 (Gravis Ultrasound, AdLib alone, none) | what DOS software talks to (`BLASTER=A220 I5 D1 H5 P330 T6` names the MIDI port too); the Gravis for games written for one; the bare AdLib is the 1990 machine |
| Music | MPU-401 at 0x330, General MIDI (CM-32L, none) | what a setup screen means by "General MIDI", "MPU-401" or "Roland" |
| Net | PCnet when on (off by default, as everywhere) | DOS networks only through a packet driver installed by hand |
| Pointer | PS/2 only (`seamless_mouse = false`) | a DOS mouse driver talks to the PS/2 controller and would find no tablet. A click grabs, Ctrl+Alt+G releases |
| Gamepad | none (gameport at 0x201, key mapping) | no USB stack. A DOS game reads the port by arming four one-shots and counting, so the count depends on guest speed; unpaced, an untouched axis wanders by half. Another reason this family is paced |
| Boot | floppy + `boot` on the machine | a DOS machine usually boots from one |

**3D.** No OpenGL or Direct3D (Windows DLLs). The Voodoo 2 checkbox
gives a DOS Glide game a real card for its own `GLIDE2X.OVL` (doc 21;
untried on this family).

### Why a rate control, and what it costs

Era software calibrates a delay loop against the CPU it finds and trusts
the answer forever, so on a fast machine it runs *wrong*, not just fast.
Our TCG runs a DOS guest at ~660 M instructions/s on the Linux box,
Pentium III territory; KVM is far beyond.

QEMU's only rate control is `-icount`, whose `shift` gives one
instruction per 2^shift ns, so the offered processors are powers of two
(`bundle::CpuSpeed`).

- **`align=on` is the whole thing.** `-icount shift=N` alone only ties
  the guest's clock to instructions retired: the guest believes it is
  slow while the host runs it flat out, and its idle waits collapse too
  (a "throttled" run finished faster than the unthrottled one). The
  machine line is `-icount shift=N,align=on`.
- **A throttled machine is emulated.** `effective_accel()` returns TCG
  whenever a processor is chosen.

Measured in FreeDOS, 200 M instructions of loop (`tools/dos-guest-test.py`):

| `cpu_speed` | asks for | measured | loop |
|---|---|---|---|
| `unthrottled` | — | 663 M/s | 0.30 s |
| `pentium-133` | 125 M/s | a ceiling (below) | |
| `486dx2-66` | 31.25 M/s | **31.3 M/s** | 6.39 s |
| `386dx-33` | 7.8 M/s | **7.8 M/s** | 25.63 s |

Above ~30 M/s alignment only corrects a guest that has fallen *behind*,
so the fast settings are a ceiling the host may overshoot; the era
settings a 1993 game wants are accurate. Boot time barely moves
(2.4 → 3.5 s): booting is mostly waiting.

### DOS games in a Win98 DOS box: the timer is Windows' business

A DOS game whose clock misbehaves only in a Win98 DOS box is not chased
(user decision). DOS games run on pure DOS, the DOS family or 98's
"Restart in MS-DOS mode", where `TESTS\QCLOCK.COM` reads 100 % since
patch 34. That mode has no CD-ROM drive and no `BLASTER` until the guest
tools' MS-DOS mode component (`SETUP /I 7`, off by default) writes
"2ksbox MS-DOS mode.pif" on the desktop: a PIF with its own CONFIG.SYS
(UIDE.SYS as a CD-only driver) and AUTOEXEC.BAT (MSCDEX, `SET
BLASTER`), as Windows 98's own "MS-DOS mode for games" samples are
built; opening it restarts there and EXIT boots Windows again
(`guest-tools/README-ISO.txt`, `DOSMODE\`). Not a CONFIG.SYS line: a
real-mode CD driver under Windows puts every drive in compatibility
mode (IOS.LOG "UnKnown int 13 hooker"). Mortal Kombat 3 is the title
that needs it: it dies on Start Game in a DOS box on any host and plays
there, proven headless from the PIF (2026-09-25). Inside a DOS box about 17 of 18 ticks still read one period
backward. The IRQ arrives on time, so the lag is between VTD/VPICD
taking the interrupt and the VM's reflected INT 8 updating 0040:006C;
VTD's trapped counter reads are already current. DOS Quake there runs
steadily fast. (Before patch 34 the DOS box got about 12 ticks a second
and Windows repaid the shortfall in bursts of up to 153 ticks.) Left
untried on purpose: measuring the VM's tick lag, `[386Enh]
TrapTimerPorts=Off`, and the rate VTD programs the PIT to.

## Other

For an era OS that is neither Windows nor DOS. Nothing is tested against
a specific guest; every choice is the one likeliest to have had an
in-box driver on a nineties system.

| Component | Default (alternatives) | Why |
|---|---|---|
| CPU rate | unthrottled | these OSes read the clock rather than count a delay loop |
| Accel | Automatic | none has Win9x's fast-CPU bugs |
| RAM | 512 MB (16–3072) | the machine's own limits; BeOS R5's lower 1 GB ceiling is said by the form, not enforced |
| Video | `std` (`cirrus`) | the Bochs VGA with VBE 2.0 and a linear frame buffer, which a period VESA driver wants and modern Linux's `bochs-drm` binds. The Cirrus is a real chip an era guest may have a *native* driver for (BeOS R5, XFree86). Not `d3dpt-vga`: its driver is Windows-only |
| Sound | ES1370 (AC'97, none) | the period PCI card both BeOS (`ensoniq`) and Linux (`snd-ens1370`) drive in the box; no FM chip, no MIDI port |
| Net | RTL8139 when on | in-box on BeOS R5 and Linux since 2.2 (`8139too`) |
| Pointer | PS/2 only | an absolute USB pointer needs the guest's HID stack *and* its windowing system to agree, which an unconfigured era XFree86 or BeOS doesn't, and no guest tools can fix; the checkbox turns it on |
| Gamepad | none (USB HID pad, key mapping) | driven by the guest's own USB stack; no gameport, whose address we can't tell an unknown OS |

No 3D and no guest tools: the Direct3D and OpenGL pass-through and our
display drivers are Windows components, and `SETUP.EXE` is
a Win32 console program. The `family-other` check holds this family's
machine line.

## The display adapter

Every era family offers a choice (`bundle::Video`, the bundle's
`video`). x64 Windows 11 has the standard VGA only, and Windows 11 on
Arm a fixed `ramfb` plus `virtio-gpu-pci` whatever `video` says:

| Family | Offers | Default |
|---|---|---|
| XP | `d3dpt` (our adapter + driver) / `cirrus` (in-box driver) | `d3dpt` |
| Win98 | `d3dpt` / `cirrus` | `d3dpt` |
| Windows 7 | `d3dpt` (with `irq=on`) / `std` (no Cirrus driver) | `d3dpt` |
| Windows 11 | `std` | `std` |
| Other | `std` (Bochs VGA, VBE 2.0) / `cirrus` | `std` |
| DOS | `std` / `cirrus` (period VESA BIOS) | `std` |

On Windows the Cirrus is for a machine whose driver isn't installed yet,
for A/B-ing a title that misbehaves on ours, and for the test tools that
exercise the in-box driver. On Other only the person installing knows
which standard adapter the guest has a driver for.

- **Every adapter carries `retrace=precise`** (`Machine::video_args`,
  on the `-vga` option, which QEMU parses for the whole machine, so
  `d3dpt-vga` gets it too: it is built on the same VGA core). QEMU's
  default answers each read of the input status register (port 3DAh) by
  flipping the vertical-retrace bit, so a wait-for-retrace loop ends on
  its second read and a title paced by the retrace runs unbounded
  whatever the processor combo says: Mortal Kombat 3's intro went by in
  under two seconds, and its character select was unusable. `precise`
  derives the bit from the CRTC timing on the virtual clock, a 70 Hz
  retrace in mode 13h, and the intro plays at its own pace with no
  throttle at all (2026-09-24). A CPU rate is still what a title that
  paces on instructions wants.
- **An adapter a family doesn't offer falls back to its default**
  (`Machine::effective_video`) and the form refuses it
  (`Form::choose_video`). `video = "std"` on XP would leave the guest
  with no display driver.
- **Changing it under an installed Windows or Other guest is a hardware
  change.** The guest finds an unknown adapter, drops to plain VGA and
  wants a driver before the desktop is back. The form says so in orange,
  only when the adapter actually changed (`Form::video_warning`). DOS
  gets a different note: the machine boots, and only a game's own setup,
  which may have recorded a mode the other adapter lacks, can be stale.
  A DOS bundle with no `video` field (from before the picker, when DOS
  was hardcoded to the Cirrus) moves to `std` on its next start.
- **`d3d9 = "auto" | "dxvk" | "system"`** rides on our adapter and picks
  the *host's* Direct3D 9 library for the executor (ADR-007's second
  amendment, doc 14). `auto` (the default, and what an older bundle
  means) is DXVK, or on a Windows host below DXVK's Vulkan 1.3 floor
  that host's own d3d9, resolved by the launcher's Vulkan probe. It is
  read only where the machine has our adapter, kept when the machine
  moves off it, and reaches QEMU as `-device d3dpt-vga,d3d9=…` (the
  `d3d9` check).

The `display-adapter` check holds the table (defaults, switches both
ways, our adapter *gone* rather than beside the Cirrus, the NIC at
`0x03`, refusals, our QEMU accepting every line). The `capi` check holds
the family switch (untouched fields follow the new family's default, a
picked one survives unless the new family lacks it, "Default" puts a
field back to following the family). Both are in `docs/testing.md`.

## Performance expectations

| Host | Win98 | XP |
|---|---|---|
| Linux/Windows x86 (KVM/WHPX) | vastly faster than period hardware | near-native |
| Apple Silicon (TCG) | faster than a period PC | usable. Against the rig's P4 1.7 (M1 Air, `reference/benchmarks/`) it boots as fast, integer is 1.3–2× faster (7-Zip 0.996/1.511 vs 0.742/0.776 GIPS), x87 reaches the P4 with patch 06 (Super PI 1M 1:57 vs 2:02; 1:25.3 with patch 14; 9:49 unpatched), and SSE/SIMD inline TCG (patches 11/12) matches or beats it on key kernels |

Expectations are percentages of the reference rig's (doc 09) benchmark
scores. Doc 22 has the full evaluation.

## Snapshots and storage

- qcow2 with named snapshots ("fresh install", "drivers installed",
  "pre-game-X") in the launcher; the retro workflow is reinstall-heavy.
- Machines are declarative bundles (`machine.toml`) in the library,
  turned into QEMU arguments by the launcher (`launcherx --print-args`).
  No user-visible QEMU command line except the form's "Extra QEMU
  arguments" field, for testing.
