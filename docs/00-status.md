# 0. Status and how to resume (updated 2026-10-24)

The handoff for a new session: the tracks and what each owns, where each
area stands, the everyday commands, open threads, next steps and the
gotchas that cost a day each. Current state only; a fixed thing moves to
its design doc and the commit log. Decisions are in doc 10, the
milestone plan in doc 08, test tools in `docs/testing.md`, build stages,
player options and logs in `docs/development.md`.

## Tracks (pick one per session)

Work runs as parallel tracks, one session each. Each track doc has its
scope, owned files, state, test loop and ordered next steps. This table
is the index.

| Track | Doc | Owns | State · next |
|---|---|---|---|
| **M4** paravirtual Direct3D device | `tracks/m4-d3d-device.md` | `d3dpt/exec/`, `scripts/test.sh`, doc 14 | Done; its guest DLLs and SysBus device retired by M16 step 7 (2026-09-27). The executor and its host harness stay |
| **M5** CD-ROM backend | `tracks/m5-cdrom-backend.md` | `libdisc/`, patches 50–59, `tools/atapi-guest-test.py`, `guest-tools/src/cdtest.c`, docs 05, 17 | Done (steps 1–8) · FIFA 2002's no-match, a second SafeDisc 2 title, SecuROM, multisession, CHD |
| **M5g** a host folder as a CD (`isodir:`) | `tracks/m5-dirdisc.md` | `libdisc/src/isodir.rs`, `libdisc/qemu/cdimage.c` | Done |
| **M6** launcher and packaging | `tracks/m6-launcher.md` | `launcher-core/`, `launcher-capi/`, `shader-chain/`, `scripts/package-*.sh`, doc 07 | Shipped (the front end is M19's since 2026-10-02, ADR-023), continues on `main`; the AppImage and an installer dropped (ADR-028) · a snapshot from the player (its menus have pause, the disc and the shader since M22), thumbnails, bundle import/export, `CDSHELF.EXE`'s Win98 run |
| **M7** XP display driver | `tracks/m7-display-driver.md` | `d3dpt/hw/d3dpt_vga.c`, `d3dpt/hw/d3dpt_exec_load.[ch]`, `d3dpt/d3dpt_fb.h`, `d3dpt/exec/d3dpt_exec_ddi.cpp`, `guest-tools/src/d3dptvid/nt/`, `tools/xp-*.sh`, `tools/d3dpt-dp2-test.cpp`, doc 15 | Done through protocol v13 · a title for each probe-only DX8 feature, more 8 bpp titles, a driver stage in `scripts/test.sh` |
| **M8** x87 / SSE fast paths | `tracks/m8-tcg-fastpaths.md` | patches 05, 06, 11, 12, `tools/x87-*`, `tools/sse-guest-test.py`, docs 13, 16 | Done · a real Direct3D workload with and without `*-fast=off` |
| **M9** TCG on Apple Silicon | `tracks/m9-tcg-aarch64.md` | `tools/tcg-profile.*`, `tools/tcg-hot.py`, the TCG patches from 13 on | Done; optimization closed by user decision (2026-09-12); the game tests uncapped on the Air in doc 22 §6.2 (2026-09-16) · binary32 at PC=24 slower than PC=53 on aarch64, not profiled |
| **M10** Win98 display driver | `tracks/m10-win98-driver.md` | `guest-tools/src/d3dptvid/core/` and `w9x/`, `guest-tools/build-driver*.sh`, `setup.c`'s 9x role, `tools/win98-*.sh`, doc 19, ADR-012 | Active; steps 0–4 done, step 5 (real titles) under way; Age of Empires' intro videos froze Win98 until the DCI primary became a flat pointer (2026-10-11, doc 19 §46, `gdiprobe.exe`'s DCI case) · the ACPI standby resume, the HAL's `cmd_lock` after a fault (§36, accepted for v1) |
| **M11** Windows host | `tracks/m11-windows-host.md` | `packaging/windows/`, `build-windows.sh`, `package-windows.sh`, `package-msix.sh`, `win-run.sh`, `embed/mglcntx_embed.c`'s WGL half, `build-windows.md` | Done; Windows builds natively in MSYS2 (ADR-026), the package is all MSVC, runs guests on the user's PC and packs as a Store MSIX (Partner Center's identity, certified 2026-10-17; an installer dropped, ADR-028) · Moto Racer's speed there, live control over AF_UNIX, the Store submission (the user's), zero-copy frames |
| **M12** music | `tracks/m12-music.md` | `libsynth/`, patches 60–61, `soundfonts/`, `bundle::Sound` / `Music`, `tools/midi-guest-test.py`, doc 20 | All stages landed · capture Win98's failing MIDI run, a host MIDI port |
| **M13** gamepads | `tracks/m13-gamepads.md` | `player-core/src/pad.rs`, `gamepad/`, patches 26–27, `bundle::Pad`, `tools/pad-guest-test.py` | Done · a real controller on the key mapping, the USB pad on Win98 FE / Me |
| **M14** Voodoo 2 device | `tracks/m14-voodoo2.md` | `voodoo/`, patch 62 and the Voodoo patches after it, `tools/voodoo-guest-test.py`, `scripts/sync-86box-voodoo.sh`, doc 21 | Active on `main`; the Air done 2026-10-04 (Quake II 147.9 fps, UT 39.9, as on Linux) · a second Glide game after one has quit, a client left on a dead ring, DxDiag's Direct3D 7 `GetDC`, the Windows build (no run on the MSVC QEMU), Diablo II measured, patches 64 and 71 upstream |
| **M15** Direct3D executor on Wine | `tracks/m15-wine-executor.md` | `d3dpt/exec/d3dpt_exec_host.c`, `d3dpt_exec_remote.c`, `d3dpt_remote.h`, the loader's library choice, `build-d3dpt-exec.sh --wine`, `player-core/src/companions.rs`, `launcher-core/src/host_gpu.rs` | Steps 1–7 done: the community app passed on a real macOS 15, WineD3D-in-guest was removed, and the Flatpak's Wine add-on `com._2ksbox.Launcher.Wine` was built and checked in the sandbox (2026-09-23) · a game through the add-on on a below-floor host; the spike's host tests on the rig's Linux Wine |
| **M16** DirectX 9 driver, no custom DLLs | `tracks/m16-dx9-ddi.md` | the DX9 DDI in `guest-tools/src/d3dptvid/` (with M10 / M7's files), its decoder cases in `d3dpt/exec/d3dpt_exec_ddi.cpp`, the Wine test suites' build, runner and baselines, doc 15's DX9 section | Closed 2026-09-27 (user), steps 0–7: XP's and Win98's own `d3d9.dll` / `d3d8.dll` run on the driver with SM3 (protocol v21), Wine's d3d8 / d3d9 suites run whole on both against the rig's baselines, the DXVK patches (`patches/dxvk/10`-`14`) landed, the Direct3D DLLs and the SysBus `-device d3dpt` are gone; Max Payne 2, Vice City, 3DMark2001 SE and Crimson Skies play on Win98; depth textures claimed since 2026-10-19 (M25's change to the core's format list), a game's hardware shadows run on XP; since 2026-10-24 without wrap or mips, as NVIDIA's (Wine's d3d9 device test crashed on Win98, whose runtime refuses a depth texture without DEPTHSTENCIL as the rig's does; doc 15) · left open: the OpenGL ICD (step 8; `OPENGL32.DLL` stays per game, user: not worth it for now), vertex texture fetch in a title (the path checked on XP and Win98 by D3DFEAT9's row F since 2026-10-21), a game's depth-texture shadows on Win98 (the path checked on XP and Win98 by D3DFEAT9's row G since 2026-10-22) |
| **M17** the driver's speed, on Max Payne 2 | `tracks/m17-driver-perf.md` | `tools/w98-mp2.sh`, `tools/ddi-rate.py`, `tools/guest-code-owner.py`; the executor's and the 9x HAL's hot paths (with M7 / M10's files) | Closed 2026-09-27 (user). The vCPU is the limit; ours is ~12 % of it. A texel mean computed on every texture bind for a trace line (7.3 % of the vCPU) is gone, and a flush hint lets the GPU start before the readback (+4.7 %, within one run) · left: host vertex buffers for the draws, one HAL walk instead of two, the readback |
| **M18** Windows 7, and Aero on it | `tracks/m18-win7.md` | the NT driver's Windows 7 paths (with M7's files), `guest-tools/src/d3dptvid/wddm/`, `guest-tools/build-wddm.cmd`, `guest-tools/src/setup.manifest`, `tools/win7-aero-test.sh`, the Windows 7 half of `tools/xp-driver-test.sh` | Steps 1 and 2 on `main`; each stretch is a `track/m18-*` branch, merged and deleted. The XP-model driver runs on 32-bit Windows 7, and the WDDM driver (kernel + Direct3D 9 user-mode driver; with `irq=on`, register sets v6 to v8 put the vertical blank and fences on the device's interrupt and keep the pointer on under DWM) gives Aero, with every test program exact under composition. SETUP installs it and turns Aero on (`CompositionPolicy=2`); the launcher has a Windows 7 family; a fresh install reaches Aero with SETUP and one restart (`win7-aero` check, `WIN7_ISO`). Built on the PC with the EWDK 10.0.19041 (`build-windows.sh wddm`, `--publish`); Linux and macOS fetch it (`scripts/windows-drivers.sh`); since 2026-10-20 Windows 7 also gets M25's `d3dpt11.dll` (Direct3D 10 / 11 programs at 10_1), its DWM staying on Direct3D 9's driver (dxdiag rates the 32-bit kernel driver WDDM 1.0) · step 6's loose ends (32-bit indices and full-screen flips landed with M20 in the shared user-mode driver, unchecked on Windows 7), timeout recovery, a WDDM 1.1 kernel driver for Windows 7 (M25) |
| **M19** the launcher on mitsuami | `tracks/m19-mitsuami-launcher.md` | `launcher-mitsuami/`, ADR-023 | The only launcher since 2026-10-02 (AppKit, WinUI 3, GTK 4, Kirigami with `kde`); every packager ships it as `2ksbox` and checks its window (`LAUNCHER_SHOT`); mitsuami 1.3.6 from crates.io (since 2026-10-20; crates.io since 2026-10-16, user; a git `rev` before); on macOS and GTK the machines are the window's sidebar (subtitle, menu, double-click, 250 wide; none in an empty library, whose window opens that much wider and gives the width back to the sidebar with the first machine), the toolbar Start and a group of the machine's Settings / Discs / Snapshots / Clone icons, New and the two libraries in File (GTK's primary menu, in the sidebar's header bar), on Windows (2026-10-11, user) no toolbar buttons but File, Machine and Help as a menu bar in the title bar before the title, as the player's, on KDE (2026-10-11, user) the same menus behind the drawer's button at the toolbar's start, New / Shelf / Shaders buttons at its end; the snapshots and shader profile tables edge to edge at their windows' top, and the details, those tables and the settings window's page scroll under the macOS 26 title bar; the disc shelf's drive and Library heading and About's header stay in the macOS title bar's glass, what's under them scrolling blurred to a hard edge (mitsuami's `header` and `header_edge`, 2026-10-11, user); the Flatpak's KDE build is the add-on `com._2ksbox.Launcher.KDE` · the KDE add-on in a real Plasma session, the macOS floor with the AppKit launcher |
| **M20** Windows 11 | `tracks/m20-win11.md` | patches 75–77 and `tpm/qemu/` (the libtpms TPM backend), libcrypto and libtpms in `build-deps.sh`, the x86_64 and aarch64 players (`--features qemu-x86_64` / `qemu-aarch64`), `scripts/build-edk2.sh` and `patches/edk2/`, `scripts/build-virtio-win.sh`, `Family::Win11` and `bundle::Arch` in `launcher-core` (with M6), `tools/tpm-qtest.py`, `tools/win11-spike.py` | Resumed 2026-10-10 (user; on hold the day before), its video driver on hold again that day and carried on by M25 (2026-10-16), all of it on `main` (ADR-024; step 5 merged from `track/m20-wddm`, then deleted). Stock Windows 11 installs with no bypass, its TPM 2.0 in QEMU's process (`-tpmdev libtpms`); x64 under KVM on Linux through `2ksbox-player-x86_64`, Windows 11 on Arm under HVF on the Air (`virt` board, our EDK2, virtio-win's ARM64 drivers disc, viogpudo once installed). A new machine starts setup from its disc by itself (2026-10-07): the disk and the drive carry `bootindex`, and the player answers the disc's "Press any key" from the firmware's serial console (`player-core/src/boot_prompt.rs`, `tools/win11-boot-prompt-test.sh`). The Linux tarball and Flatpak ship the x64 player, the macOS app Windows 11 on Arm (hypervisor entitlement); the Windows zip and MSIX carry the x64 player too (the packager boots it to its BIOS). On a Windows host (2026-10-09) the launcher makes the same x64 Windows 11 under WHPX: the libtpms TPM in the MSVC QEMU (patches 88, 89; OpenSSL and libtpms in `build-deps.sh`'s MSVC set), the clipboard's `qemu-vdagent` (spice-protocol there too), our OVMF with Secure Boot and no SMM, which WHPX lacks (`build-edk2.sh x86_64`, built natively with Visual Studio 2022; `patches/edk2/03`), and a guest's own restart working under WHPX (patch 87, `WHvResetPartition`); step 5: M18's WDDM driver on x64, test signed: it loads on build 26100 under WHPX on the PC and DWM composes through it (the player follows our adapter); the shared-target blits (protocol v23), the leaked host contexts and the device churn are fixed; since protocol v24 and driver 0.3.17 (2026-10-09) the taskbar, the tray, window captions and WinUI 3 apps (File Explorer, Notepad) draw as on Basic Display: the first blit into a fresh target kept, ColorFill and system-memory uploads into shared targets on the host, and feature level 9_3 (32-bit DRAW8 indices, the 9_2 caps and formats; `tools/wddm-probe.cpp`); driver 0.3.25 adds the 32-bit user-mode driver for WoW64 (D3DGAME9 32-bit: frame 300 pixel-identical to native DXVK) and flips that wait for the vertical blank (a full-screen game runs through DWM); register set v9's host-side VRAM fill and copy take the paging fills, transfers and present blits off the guest's CPU (uncached under WHPX): windowed D3DGAME9 from ~29 to ~54 frames a second; 2026-10-10 (M24's lessons): DWM had ignored our blank while Basic Display's screen was extended, so the guest agent makes d3dpt-vga's screen the only one, the device's blank follows the host screen's (`host-vblank`, DWM at 13.88 ms on the PC's 144 Hz screen) and each flip is pushed to the player (`flip-push`; the user: "smoother"); then the old artifacts (a hovered menu lost its other rows, window shadows darkened, Explorer flickered): the executor lost the guest's scissor whenever it bound target 0 again (`Ctx::scissor`), and the TDRs: a preemption left out a flip held for the vertical blank (driver 0.3.32); found with the executor's surface log (`D3DPT_SURF_LOG`, doc 15); WHPX resets no longer leave a dead VM (patch 92 sizes the xsave read; the player's Reset runs a paused machine); Explorer's XAML, which never composed in about two of three windows on Direct3D 9, draws since DWM composes through M25's Direct3D 11 driver (WDDM 1.2, 2026-10-17); a big desktop under WHPX (2026-10-23; the user: choppier with the window on a 4K screen) composes at 3840x2160 as at 1920x1080 (DwmFlush p95 18.3 ms, against 26 ms and a 108 ms stall): QEMU patch 99 gives WHPX a real dirty log (every refresh had redrawn the whole frame) and a pushed flip carries only its changed rows (doc 15) · an install through the launcher end to end on the PC, App Review of the hypervisor entitlement (TestFlight runs Windows 11 on Arm since 2026-10-18), `FRAMES` on the host's blank for 98 / XP (the desktop follows the window on our adapter since 2026-10-22: register set v12, the agent applying the driver's custom mode, doc 15) |
| **M21** QEMU 9.2 to 11.1 | `tracks/m21-qemu-upgrade.md` | the `qemu` submodule pin, `scripts/prepare-qemu.sh`, `scripts/configure-qemu.sh`, `patches/qemu/`, the qemu-3dfx port | Merged to `main` 2026-10-02: QEMU 11.1.2, qemu-3dfx's OpenGL half ported (`patches/qemu-3dfx/`), every TCG patch ported and bit-exact, game A/Bs at or above 9.2. Step 4: Windows (WHPX back for era machines, patch 84) and the Mac (patch 82: Windows 11 on Arm under HVF; App Store, community and Intel apps pass) done; old bundles keep `pc-i440fx-9.2`; the Linux tarball and the Flatpak pass on 11.1 (2026-10-04) · doc 22's numbers, the user's hand test |
| **M22** the player on mitsuami | `tracks/m22-mitsuami-player.md` | `player-core/` (with M2's and M13's files in it), `player-mitsuami/`, `tools/player-mitsuami-test.sh`, ADR-025 | On `main`. Everything but the window is `player-core`, which `player-mitsuami/` drives. The only player since 2026-10-24, when the winit one (`player/`) was removed with `test.sh` and the tools moved to it (user; step 6); every package's since 2026-10-07 (Windows since 2026-10-03, MSVC, Direct3D 12 present). Checked headless on sway and by hand on Windows. Windows 11 on Arm's desktop (2026-10-08, step 7): the player shows each guest flush (embed v12 `on_flush`), and QEMU patch 86's `sync-ctrl` copies each viogpudo present before the guest draws the next (the user: less choppy); `win11-frames` checks whole frames; each machine's window size is remembered (`--window-state`), which a Windows 11 guest's resolution follows; x64 Windows 11 has Arm's second screen (`virtio-gpu-pci`) and viogpudo on its drivers disc, so on Linux too once the driver is in; a Disc menu (Machine > Disc since 2026-10-20) over the machine's drive and the shared shelf, and View > Shader, which swaps the machine's shader profile live, Off included (2026-10-19, step 5; both follow file notifications and the tray's QMP events, no timers; `launcher_core::shelf` and `shader_library`, the bundle from the launcher's `--machine`) · step 2, the pointer, menus and close alert on a real desktop (and mitsuami's 5 px offset when sway tiles the window), then X11 / KDE, macOS, step 5's menus by hand off Windows (and the App Store sandbox's disc picks) |
| **M23** shared folders and the clipboard | `tracks/m23-integration.md` | `libsmb/`, `guest-agent/`, `player-core/src/share.rs`, `player-core/src/clipboard.rs`, patch 79 (`guestfwd=…-unix:`), doc 24, ADR-027 | Windows 11 on Arm (Mac) and x64 (Linux): a host folder through `libsmb` in the player (SMB 3.1.1 / 2.1, leases, `srvsvc`), the clipboard through `qemu-vdagent` (embed API v11) and our `guest-agent`; the drivers disc installs the guest agent at a new machine's first logon (`autounattend.xml`, 2026-10-09; `2ksbox\install.cmd` for one installed without the disc); the launcher's Clipboard and Shared folder settings; checks `smb`, `sharing`, `tools/smb-win11-test.sh`, `tools/clipboard-win11-test.sh` · Windows hosts (step 7) |
| **M24** our viogpudo | `tracks/m24-viogpudo.md` | `patches/viogpudo/`, `scripts/build-viogpudo.sh`, `guest-tools/viogpudo/` | Merged to `main` 2026-10-09 (opened the same day, on `track/m24-viogpudo`, then deleted). Windows 11's choppy desktop: viogpudo gives no vertical blank, so DWM composes at 64 Hz against a 60 Hz host screen. Patch 01 gives the driver a 60 Hz blank of its own from a timer on an absolute schedule; test signed in the guest (user decision). Built on the PC from NuGet's WDK with Visual Studio 2022; on x64 Windows 11 there DWM composes every 16.667 ms (`DwmFlush`, 16.3–17.1 ms) against 15.6 ms upstream, and the user finds it "much better", at 72 Hz on a 144 Hz screen "very smooth"; the driver's present path costs ~1 ms a full screen. The desktop follows the window: the drivers disc's `2ksbox\install.ps1` installs viogpudo's resolution service, with our `viogpuap` (patch 03: upstream's found no virtio-gpu behind the other card), and the guest agent makes the virtio-gpu's screen the only one. Step 4: the guest's blank follows the host screen, its rate (embed API v15, patch 02; 72 Hz on the PC's 144 Hz screen) and its phase (QEMU patch 90, embed API v16, patch 04: the host screen's blank raises the guest's; DWM at 13.886 ms, half the timer's jitter); on the Air (Windows 11 on Arm) the host's blank needed QEMU patch 91 (HVF lost GICv2m's MSI pulses), then DWM at 16.661 ms and the user's "smooth"; a new machine gets the guest agent at its first logon (the drivers disc's `autounattend.xml`, local account); Linux reads the window's screen's rate and blank through DRM (the screen from Wayland's presentation feedback or X11's RandR), and the user finds x64 Windows 11 under KVM "very smooth"; ours on the drivers disc, opt-in (`2ksbox\experimental-display`, warnings in its README and twice from the script; 2026-10-11; run from the disc on fresh x64 installs, Windows and Linux hosts, by 2026-10-20) · Smart App Control blocking test signing, Linux with two real screens, shipping (signing; M25's own adapter beside it since 2026-10-17) |
| **M25** Direct3D 10 and 11 | `tracks/m25-d3d11-umd.md` | `guest-tools/src/d3dptvid/wddm/um11/` and `wddm/umd_log.c`, `d3dpt/exec/d3dpt_exec_d11.cpp`, protocol v25's D11 record and v26's, register set v10's `CAP_D3D11`, the x64 kernel driver's WDDM 1.2 DDIs, `tools/d3d11-probe.cpp`, ADR-029 and its amendment | Opened 2026-10-16 on `track/m25-d3d11-umd` (user: a real Direct3D 10 / 11 driver, from scratch, feature level 10_0 first), merged to `main` 2026-10-20 and deleted; further stretches are `track/m25-*` branches. The WDDM driver's second user-mode driver, `d3dpt11.dll` (the Direct3D 11 DDI, Windows 8's 11.1 interface with its video functions, and Windows 7's), on x64 Windows 11: `D3D11CreateDevice` gives **10_1** on our adapter (since 2026-10-20; 10_0 from 2026-10-16, 9_3 before), Direct3D 10 programs on it (Windows 11 runs them through the 11.1 interface; 32-bit Windows 7 gets the driver too, with Direct3D 10.0's and 10.1's own interfaces, `d10_dev.c`; its DWM stays on Direct3D 9's driver while Windows 7 rates the kernel driver WDDM 1.0), run on DXVK's d3d11 on the host with shaders passed through (the host rebuilds the DXBC container); `d3d11-probe`'s scene matches WARP to the pixel offscreen and through a windowed swap chain of either DXGI model; Windows 11's shell (Explorer, Search, Start, WebView2) draws on it, and since 2026-10-17 (step 4) **DWM composes the desktop through it**, the x64 kernel driver claiming WDDM 1.2 (DWM's device asks for `VIDEO_SUPPORT`, which needs the 11.1 interface; DXGI primaries scanned out; views follow a flip chain's rotation; the user-mode drivers' fix-up reserve is 24 bytes a patch on x64), Explorer's XAML (M20's open bug) drawing, one screen or two; a flip-model swap chain no longer drops DWM's device (dxgkrnl at 1.2 refuses a write to an allocation opened read-only, so the user-mode drivers list another process's allocations as read); a host without Direct3D 11 leaves the guest on 9_3 (`CAP_D3D11`); the desktop at 1600x1200 composes at the screen's rate again (~31 frames/s before: the Direct3D 9 user-mode driver flushed at every NOOVERWRITE lock, a submission and a fence wait per DWM draw); the WDDM driver offers widescreen modes (1280x720 to 3840x2160, where six frames fit the segment); `d3dgame11`, the reference scene on Direct3D 10 / 11 features at 10_0 (instancing, texture arrays, stream output, DrawAuto, geometry shaders, GenerateMips, predication, FP16), matches WARP to 9 pixels (the executor does predication itself: DXVK's d3d11 stubs it); the Direct3D 9 drivers (WDDM, XP, 9x) claim depth textures (shadow maps, D16 / D24X8 / D24S8) since 2026-10-19, a game's hardware shadows run on Windows 11 and XP, and the x64 kernel driver's 1 GiB aperture gives Direct3D 9 a 1472 MiB texture budget (512 before): Unigine Heaven in Direct3D 9 runs on it, slowly (1-3 frames/s, open); on the Mac (2026-10-20) Windows 11 on Arm's desktop runs on our adapter (the driver turns on memory decoding Windows on Arm leaves off; QEMU patches 97, 98), Mica included, `d3dgame11` and `d3dgame8` at 60 frames/s up to 3840x2160; a TDR under a full-screen repaint is gone (WHPX's EOI exit ran the IOAPIC without the BQL; QEMU patch 94); since 2026-10-23 the user-mode drivers log only failures unless `D3D11Trace` / `D3D9Trace` names the process, and the kernel driver's routine lines need its service key's `Trace` = 1 (each line is a VM exit a character); since 2026-10-24 a screen Windows turns off is black, not the VGA core's stripes (register set v13's `BLANK`, doc 15) · multisampling (the runtime refuses some claim), games on Windows 11 on Arm (Heaven in Direct3D 11 crashes in KosmicKrisp's shader linker, a driver bug), 11_0, Windows 7's DWM on Direct3D 10.1 (a WDDM 1.1 kernel driver there) |
| **M26** zero-copy scanout | `tracks/m26-zero-copy.md` | the embed API's ring calls for Direct3D frames, the player's shared-texture import (`player-core/src/`), the executor's DXVK interop and slot copy (`d3dpt/exec/`, shared), the device's slot path in `d3dpt_vga.c` (with M7) | Opened 2026-10-23 on `track/m26-zero-copy`, merged to `main` 2026-10-24 and deleted (user: "probably good enough to merge") (user: a frame that changes everywhere, a full-screen video, the start menu, still goes from the GPU to VRAM, through three CPU copies and up again, 33 MB a frame at 3840x2160). Windows first: a ring of shared D3D12 textures the executor fills on the GPU at each flip and the player samples. Since 2026-10-23 Direct3D 11's flips (Windows 11's DWM) go through it (embed API v17, QEMU patch 101, `zero-copy` on d3dpt-vga; the user at 3840x2160: "much smoother"), and XP's and 98's Direct3D 9 flips (protocol v30's `SCANOUT`: the drivers no longer read back at EndScene; D3DGAME9 fullscreen at 1600x1200 on XP: 75 flips/s, 4 readbacks in 16 s). Since 2026-10-24: 98 on the ring in `player-mitsuami` (D3DGAME9 fullscreen at 800x600 under TCG, 60 flips/s, 3 readbacks in 25 s), a slot the player may still sample never written (embed API v18's `scanout_hold`, patch 101's `held`, `d3dpt_exec_ring_held`), and the guest cursor over a slot frame (a relative mouse, a grab) blended into a GPU copy of it. macOS on the ring the same day (step 6's half): one Metal heap per slot, the only handle KosmicKrisp's `VK_EXT_external_memory_metal` imports (DXVK patch 19), the player's texture placed on it described as KosmicKrisp's own (`metalring.rs`); D3DGAME9 fullscreen at 800x600 on 98 under TCG on the Air, 60 fps, 1539 slot frames, 4 readbacks ; Linux on the ring too (the player's Vulkan images exported as opaque fds, `vkring.rs`, DXVK patch 20 for `VK_KHR_external_memory_fd`; 98's D3DGAME9 at 60 flips/s and Windows 11's DWM under KVM, not yet through `test.sh all`). Windows 11 on Arm on the Mac (the user's machine): DWM's echoes fixed (a readback forgets the page's slots; a page's newest frame keeps its slot or goes back to VRAM), and five slots the default (`ring-slots`: with three, half of DWM's frames at 2560x1676 were read back); DWM makes about 40 frames/s in an animation at that size with the ring or without, GPU-bound (the track doc) · on Windows the commits since the Linux half (the echo fixes, five slots) neither built nor run on the PC; Linux not through it since the merge, nor the A/B at 4K |
| Everything else (M2's leftovers) | "Next steps" below | | as listed |

Rules: work on `main` or on a branch `track/<name>-<topic>` off it,
rebased on `main` before pushing and merged when green. Edit shared files
(`d3dpt/d3dpt_proto.h`, `d3dpt/exec/`, `scripts/test.sh`, `player-core/`,
`CLAUDE.md`, this doc) minimally and name the track in the commit
message. In this doc a track edits only its own row here, its "Where
things stand" row and its lines under "Next steps"; everything else goes
in its track doc. A merged track's branch and worktree are deleted.
Branches still on the remote: `track/m10-win98-driver`, `parallel-wt1`
and `track/m21-qemu-upgrade` (merged; M21 was squashed), `track/m9-hwmmu`
(the hardware-MMU gauge, parked), `m14-glide3` (abandoned, tagged
`m14-glide3-abandoned`) and `track/m14-voodoo2-sli` (an SLI pair and the
Voodoo 3's screen filter, unmerged). The Mac pulls `main`.

## Where things stand

| Area | State |
|---|---|
| QEMU | v11.1.2 + our port of qemu-3dfx's OpenGL half (`patches/qemu-3dfx/`, from `d00e858`) + our patches 000–101 (`patches/qemu/README.md`; track M21). Built without display, host-audio, extra network or network-block backends (the `no-optionals` check). On Linux it links a GLib of its own, static and hidden (`scripts/build-deps.sh`, `QEMU_DEPS` in `development.md`), so a toolkit on the process's GLib never meets QEMU's main loop; the Mac build needs no XQuartz (patch 70). Windows builds it against MSVC's runtime (`build-windows.sh qemu-msvc`, patch 83, ADR-026's third amendment, `build-windows.md` "QEMU under MSVC"), which the package ships with no runtime DLL (`package-windows.sh` checks) and `test.sh` runs; the mingw build and the winit player's Windows build were removed on 2026-10-11, the winit player everywhere on 2026-10-24. MSVC's ABI makes every enum an `int`, so its build fails on any enum it would miscompile (`QEMU_ENUM_UNSIGNED`). WHPX serves era machines again since patch 84. |
| Emulated CPU (TCG) | x87 shadows at PC=24/53/64 (doc 13), SSE and SIMD inline (doc 16), the M9 queue (REP, same-value SMC, soft immediates, inline TB lookup, TLB work). Default was 2.34x geomean over pristine 9.2.4 on the Air (measured before M21), all switches off 0.97x (doc 22). Every patch has an off switch in the machine form. Patch 85 (2026-10-07) fixes an upstream use-after-free that killed the player on any one-vCPU machine whose code buffer filled (Duke Nukem 3D in a Win98 DOS box, within minutes). Pinned guest registers (patch 21, doc 18) crashed XP, were never offered, and were not ported to 11.1 (user decision, 2026-10-01). The hardware-MMU design (1.1–1.2x) is parked (user decision, 2026-09-16). |
| Player | QEMU in-process (`libqemu-embed`, embed API v17), wgpu + librashader CRT chain, mode analysis (doc 03), the embed audiodev (f32, paced to the guest's clock, limiter; doc 11), guest cursor as the window cursor, Windows 98's and Windows 11's standard cursors as the host's own (recognised by their pixels, mitsuami's `Cursor::System`; 2026-10-12, user: the guest's was tiny on a scaled picture, and scaled up looked worse; Windows 11's since 2026-10-20, at every display scaling from 100 % to 200 % and the default pointer size, the busy ones' every frame; XP's pointer is GDI's own, never the hardware cursor), any other at the picture's zoom, gamepads, host modifier keys, the host's shortcuts to the guest on all three hosts (macOS since 2026-09-24: the window server's hot keys off while focused, the private call UTM ships on the App Store, doc 03 "Input path"), Alt+F4 and Cmd+Q ask. Ctrl+Alt+S shoots the guest's frame, Ctrl+Alt+Shift+S the window's (scaled and shaded), into the Pictures folder's `2ksbox` (found the platform's way) since 2026-10-07: they went to the working directory, `/` for an app from Finder, and the failed write aborted both players. View > Scale holds the picture at a whole scale in real pixels per scanline (1x is the guest's own resolution; 1x to 4x, 1x to 8x on Retina, so every scale the fit lands on has a name; user, 2026-10-09) or the largest that fits, cropping what overflows the window, and View > Fit Window to Picture sizes the window to it (`player-mitsuami`, 2026-10-07, doc 03 "Geometry"). Options: `docs/development.md`. Everything but the window is `player-core` (M22, 2026-10-01), so `player-mitsuami` (ADR-025) is the window: the player a checkout's launcher starts, and the one every package ships (macOS, Linux and the Flatpak since 2026-10-07); the winit player was removed on 2026-10-24 (user). |
| OpenGL pass-through | In the player on Linux (EGL), macOS (CGL) and Windows (WGL, doc 12 "The WGL rule"). Zero-copy: dma-buf ring on Linux (repairs a slot that stops being written through), IOSurface on macOS. The Glide pass-through was removed on 2026-09-23 (ADR-020): Glide is the Voodoo 2's. |
| Voodoo 2 (doc 21) | `-device voodoo2`, 86Box's chip, the machine's only Glide (ADR-020). 3dfx's own Win98 driver runs Quake II, UT and NFS Porsche (the user, by hand). FIFA 2000's and Carmageddon's FIFO hangs fixed (doc 21 §11, §13). 8 MB board by default (`texmem=2`), command FIFO in guest RAM (`ramfifo=on`, Quake II 41 → 147.5 fps; 147.9 on the Air, where the ARM64 JIT is 2.07× the interpreter). Open: a second game after one quits sometimes starts glitched. |
| Direct3D executor (doc 14) | Protocol v30 (v23 / v24 M20's, v25 to v29 M25's Direct3D 11 and GDI records, v30 M26's `SCANOUT`), one decoder, four D3D9s. DXVK (native and on Windows) is the default and the golden reference. Below the Vulkan 1.3 floor: Windows' own `d3d9.dll` (`D3DPT_D3D9`, ADR-007's second amendment) or Wine's on a Linux / macOS host (`exec=wine`, ADR-018, M15). `no-exec=on` models a host with no executor. On Windows the executor and DXVK are MSVC since 2026-10-04 (ADR-026's second amendment), DXVK with our patch 15 (`build-windows.md` "DXVK under MSVC"); the same day the tools (`launcherx`, `discx`, `synthx`, `wgl-probe`) moved too, leaving only QEMU, the winit test player (removed 2026-10-11) and the guest code on mingw. |
| Direct3D 10 / 11 (M25) | The WDDM driver's Direct3D 11 user-mode driver (`d3dpt11.dll`, ADR-029) on x64 Windows 11 at feature level 10_1 (and on 32-bit Windows 7, where Direct3D 10 programs get its 10.0 / 10.1 interfaces), DWM composing through it (11.1 interface, the x64 kernel driver claiming WDDM 1.2), protocol v25 / v26's D11 records (v27 to v29 the GDI side's) on DXVK's d3d11 (`d3dpt_exec_d11.cpp`), one host device with a context state per guest device. Checked against WARP in the guest (`tools/d3d11-probe.cpp`: offscreen and presented, 0 pixels off). Every package carries DXVK's d3d11 and dxgi since 2026-10-19 (without them `CAP_D3D11` stays off and Windows keeps d3d10level9); run on Windows, Linux (by hand under KVM, `d3dgame8`, `9` and `11`, 2026-10-20) and Mac hosts (DXVK's d3d11 and the executor build there since 2026-10-20, DXVK patch 18; Windows 11 on Arm, below). The driver also builds for Windows 11 on Arm (ARM64, its user-mode drivers ARM64X), and the aarch64 QEMU and the form offer the adapter there (experimental; on the Mac the machine boots Windows with its 512 MiB in a 64-bit BAR above 4 GiB since 2026-10-20, QEMU patch 97 giving an M1's 36-bit guest a high PCI window; since 2026-10-20 the desktop runs on it there, Mica included, DWM on DXVK's d3d11 on KosmicKrisp: the driver turns on the memory decoding Windows on Arm leaves off, and QEMU patch 98 fixed HVF's abort at the first present; `d3dgame11` and `d3dgame8` at 60 frames/s and every size up to 3840x2160 on the Air; games there open: Unigine Heaven in Direct3D 11 crashes in KosmicKrisp's shader linker, a driver bug). On Windows 11 the form names it experimental, asks before taking it and warns that it needs test mode. |
| XP display driver (doc 15) | `d3dpt-vga`, register set v13 (v6 to v8 add the interrupt, the fences and the cursor ownership the Windows 7 WDDM driver uses, M18; v9 the host's VRAM fill and copy the WDDM driver's paging and presents use, M20; v10 `CAP_D3D11` and v11 `HOST_MHZ`, the vertical blank's rate, M25; v12 `CAP_FOLLOW`, the WDDM desktop following the window, M20; v13 `BLANK`, a source the WDDM driver turns off shown black, M25). DirectDraw, a DirectX 9 DDI with shader model 3.0 to `d3d9.dll` (M16: the face, the tokens, the DX9 formats, mip generation, instancing, four render targets) and a DirectX 8 DDI with hardware T&L, shaders 1.x, palettes, colour keys, VRAM buffers, 16 streams, cube / volume textures, MSAA, gamma. Depth textures (D16 / D24X8 / D24S8) claimed since 2026-10-19 (M25), a game's hardware shadows run. Table fog on pre-transformed vertices follows the runtime's WINFO since 2026-10-20 (Gothic had no distance fog: DXVK patch 11, doc 15). FIFA 2000, Max Payne, Vice City, Moto Racer, Diablo and Gothic play. |
| Win98 display driver (doc 19) | The same core under a 9x layer; the default adapter for a new Win98 machine. The DirectX 3–8 checks pass as on XP, and a 2ksbox Win98 runs DirectX 9.0c. Crimson Skies, 3DMark 99 / 2001 SE, Carmageddon (Mode X), Blood in a DOS box, Age of Empires (§46). Blue screens and power-down show. |
| CD-ROM (docs 05, 17) | `libdisc` behind the `cdimage` driver: cue/bin, CCD, MDS, ISO and `isodir:` folders, L-EC, subchannel, CD-DA, a DVD profile past 80 minutes, the disc shelf from inside the guest (patch 52, `CDSHELF`; the listing names the disc in the drive from the medium itself, boot disc included; on XP it leaves the medium-change report to Windows and never dismounts, so AutoPlay and Explorer see each swap, 2026-10-04). SafeDisc 1.x's band read is the negative control; SafeDisc 2.x and ProtectCD never read theirs. |
| Music (doc 20) | OPL3 and MPU-401 (no IRQ line) on SoundFont GM or the user's MT-32 ROMs. The SB16 applies its mixer (patch 61). Open: Win98's own MIDI through our port loses instruments. |
| Gamepads (M13) | USB HID pad (patch 26), gameport (patch 27), key mapping. Done. |
| Guest machines (doc 06) | Six families: Windows 98, Windows XP, Windows 7, Windows 11, DOS, Other. Win98 / XP / Win7 start on `d3dpt-vga` (Win7 with `irq=on`), Windows 11 on `virtio-vga` alone on x64 (`q35`; since 2026-10-22, the standard VGA and a virtio-gpu before) and on Arm (`virt`; since 2026-10-23, ramfb and a virtio-gpu before), DOS / Other on `std`. No network card by default. Win98 is TCG with `hpet=off`; the BIOS date stamp makes it install ACPI. DOS paces with `-icount …,align=on`; every adapter has QEMU's `retrace=precise`, so a wait-for-retrace loop really waits (2026-09-24). |
| Guest tools (`guest-tools/README.md`) | One ISO. `SETUP.EXE` installs what this Windows can use; every program logs to `C:\2KSBOX` (`BOXLOG=` overrides). |
| User manual | `manual/`, a Starlight site published to GitHub Pages (<https://roboport-tecnologia.github.io/2ksbox/>) by `.github/workflows/manual.yml` on every push to `main` that touches it (2026-10-09; `development.md` "The user manual"). Thirteen pages, the launcher's own window and setting names; the privacy page shows `docs/privacy.md`. The repository's Pages source is GitHub Actions (set 2026-10-09). |
| Launcher (doc 07) | `launcher-mitsuami` over `launcher-core` (also `launcherx`, `launcher-capi`) is the only launcher since 2026-10-02 (user; ADR-023, M19): every package ships it, and the Qt one is deleted. Its library is a machine list beside the chosen machine's details, as UTM and VirtualBox (2026-10-01), and its disc shelf is one drive card over the library, Insert and Eject setting the boot disc and swapping a running machine's tray, the card reading the tray back (2026-10-01, doc 07 "One drive"); Escape in a label's field drops the edit instead of closing the window, and on Windows the field from the rename pencil or the menu's Rename works since mitsuami 1.3.6 (`da71060`; the user on the PC, 2026-10-20). The family picker offers Windows 98, Windows XP, Windows 7, Windows 11, DOS, Other (labels and order, user 2026-10-04). The machine form is a settings window, a page per section, every picker the style's own combo box over the model's rows; the Direct3D picker shows only what this host runs. A machine whose disk was deleted starts on a new empty one the size the old one was (`player::prepare`, 2026-10-11): the bundle's `disk_size`, which the wizard writes and the first start of an older bundle reads off the disk, else the family's default; unless the disk's folder is gone too (an unplugged drive). Extra QEMU arguments, clone (a copy of the disk, or the same disk with a one-at-a-time warning, 2026-09-26; on Windows its progress is `CopyFileExW`'s own count, 2026-10-19, doc 07), first-run preset download; the profile library has a default profile that every machine on "(default)" plays through, CRT Aperture from the first download (2026-09-24). The snapshot window is a tree (2026-09-23): a qcow2 records no parent, so the launcher writes each take and restore to `snapshots.toml` beside the bundle and reconciles it against the disk on every read; snapshots it has no record of sit at the top level; its header and rows share one set of column widths, so they line up at any window size (user report 2026-09-23). Window text is short and plain (user rule). An About window (2026-10-02) thanks the projects 2ksbox is built on (`launcher_core::about`, `launcherx --about`): in mitsuami from the toolbar's "i" (an info icon on every toolkit, user), and on macOS only from the application menu (user). A relative `disk`, `efi_vars` or `tpm_state` in `machine.toml` is the file beside it, and one in the machine's folder is written that way, so the folder moves whole (2026-10-11, doc 07 "The bundle"). Help > Open Machines Folder (macOS, GTK; 2026-10-11) opens the library's directory in the file manager. The System page has Processors (2026-10-11, doc 07): Windows 11 always, XP and 7 under hardware virtualization; emulated machines keep one. |
| Packages | All ship `launcher-mitsuami` as `2ksbox` since 2026-10-02 (ADR-023). Linux tarball (the host's GTK 4), Flatpak (`org.gnome.Platform` 49; add-ons: Wine, and since 2026-10-04 KDE, the Kirigami launcher for a Plasma session), macOS app in two builds (bundle IDs `com.2ksbox.2ksbox` and `com.2ksbox.2ksbox-community`, the registered App IDs, ADR-011; signed by David Rios Gomes Ltda, team `8JSATHUAL7`, since 2026-10-09 (`package-macos.sh` picks only that team's identities and profiles, `--team`); the App Store one as a sandboxed, signed `.pkg` for upload since 2026-10-06, `package-macos.sh --app-store`, not yet run through TestFlight, `build-macos.md` "The App Store package"; since 2026-10-06 it checks the icon's 512@2x and `ITSAppUsesNonExemptEncryption` = false (user) and its icon is the official 1024 master since 2026-10-07, on Apple's icon grid for the `.icns` (`packaging/icon/macos/`), and for macOS 26 (Light, Dark, Clear, Tinted) an `Assets.car` compiled from `packaging/icon/2ksbox.icon`, an Icon Composer document rebuilt from the user's artwork the same day; picks kept across launches as security-scoped bookmarks, `launcher_core::grants`; ADR-019: App Store 26+, community with the Wine pair down to macOS 12; nothing in either from Homebrew since 2026-09-23, `scripts/build-deps.sh` builds QEMU's libraries from source, `build-macos.md` "The libraries"; the community app for Intel Macs builds and packages on the Air under Rosetta since 2026-09-24, `scripts/build.sh --x86_64` + `package-macos.sh --x86_64`, untested on an Intel Mac, "The Intel build"; on Apple Silicon both carry Windows 11 on Arm since 2026-10-04: `2ksbox-player-aarch64` with the hypervisor entitlement, our EDK2 and the ARM64 drivers disc, the packager booting the staged machine's firmware under HVF), Windows zip (built, rolled and checked natively on the PC in MSYS2 since the cross build from Linux was retired on 2026-10-03, ADR-026; it needs the Windows App Runtime 2.4+) and, from 2026-09-23, the same tree as a Microsoft Store MSIX (`scripts/package-msix.sh`, packed on the PC; passes the certification kit; not yet uploaded: the submission's steps, text and privacy policy are written, `build-windows.md` "The Store package", `docs/privacy.md`; the listing and the submission steps moved to the store repo on 2026-10-04, user). Every packager opens the launcher's real window (`LAUNCHER_SHOT`). The Windows checks fail on anything they write into the staged tree (2026-10-02: a DXVK log and NVIDIA's `umdlogs` had shipped in the zip), and the launcher sends DXVK's log to the data directory, not the working one. |
| Tests | `scripts/test.sh host` (~30 s) / `all` (+ XP and DOS guests). Integration only, local only; `docs/testing.md`. On Windows it runs natively in MSYS2's MINGW64 shell on what `build-windows.sh` built (user, 2026-10-02; `docs/testing.md` "On Windows"). |
| Guest images | Outside the repo, read-only for a session: `~/vms/win98.qcow2`, `winxp.qcow2`, `winxp-m7*.qcow2`, `scratch.img` (E: in XP), and the launcher library's machines (`~/.local/share/2ksbox/machines/`: `base98-br`, `base98-us`, `claude98`, `win98-2`, …). Boot an overlay or a copy. |

## Build / run cheat sheet

Stages, player options and packagers: `docs/development.md`. Test tools:
`docs/testing.md`.

```sh
scripts/build.sh           # after every pull: everything, only what changed
                           # (-f re-runs every prepare, --test adds test.sh host)
scripts/build.sh --x86_64  # on the Air: the Intel build, under Rosetta, into build/x86_64
                           # (macOS: the deps stage builds QEMU's libraries from source,
                           # build/deps/<arch>)
                           # (then package-macos.sh --x86_64; build-macos.md "The Intel build")
scripts/test.sh            # host stage; `all` adds the guests (before any
                           # commit touching QEMU, embed, the D3D device, guest DLLs)

# the PC, from PowerShell: each verb opens MINGW64 itself (build-windows.md)
scripts\win.ps1 build      # build-windows.sh; also run, test, package, drivers, sideload, shell

# a machine the way the launcher runs it
target/release/launcherx --print-args <machine>/machine.toml   # the exact QEMU line
player-mitsuami/target/release/player-mitsuami --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc,hpet=off -cpu pentium3 -m 256 \
  -hda <overlay>.qcow2 -vga none -device d3dpt-vga -usb -device usb-tablet \
  -device sb16,audiodev=embed0 -qmp unix:/tmp/q.sock,server,nowait

# driving a guest
tools/qmpc.py /tmp/q.sock keys meta_l+r      # Run dialog; `type '…'`, `keys ret`
tools/qmpc.py /tmp/q.sock json '{"execute":"system_powerdown"}'   # clean stop
mcopy -i ~/vms/scratch.img@@1048576 ::/OUT/G9.BMP g9.bmp   # a file out of E:

# host-only D3D checks, and the XP driver loop
build/d3dpt-dp2-test x.bmp
tools/xp-driver-test.sh <overlay> d3d7        # NO_EXEC=1 / EXEC=wine for the fallbacks

# CD images
target/release/discx scan game.cue            # the bad-sector map of a dump
CDIMAGE_TRACE=1 build/qemu/qemu-system-i386 … -cdrom game.cue   # every ATAPI packet
# a disc with CD audio in the player: the explicit drive, on its own channel
#   -drive if=none,id=cd0,media=cdrom,file=game.cue \
#   -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0
```

A bare `qemu-system-i386` needs `-L qemu/pc-bios` by hand (its other
traps are under "Driving a guest headless"). The DOS batteries fetch the
FreeDOS floppy themselves; on a Mac they need `brew install nasm
mtools`.

## Open threads

Unfinished or unexplained things across tracks. A track's own open items
live in its track doc; fixed things leave this list.

- **The player's render thread** (`render.rs`, 2026-10-16) has run only
  on Windows so far: try it on the Air and on Linux.

- **GL on a Windows host.** The OpenGL pass-through runs there
  (`GLPROBE.EXE` in `base98-br` reads the host's renderer since the WGL
  fix, doc 12 "The WGL rule"), but no game has run on it. GLQuake is the
  user's next try.

- **Windows' own Direct3D 9 is unproved on the hosts it is for.** Both
  oracles match byte for byte on both backends, but only on an RTX 3090
  (ADR-007's second amendment). Expect gaps in pre-Broadwell Intel,
  Kepler and TeraScale drivers; next is a real title on such a host with
  `D3DPT_D3D9=system`.

- **Win98's ACPI standby does not come back** (doc 19 §41). Standby
  suspends the VM, input piles up in the embed queue (512 events, 151
  dropped, 20 s late on the user's run), and on wake nothing reprograms
  the adapter, so a blank text page idles back into standby.

- **Mortal Kombat 3 (DOS) dies on Start Game in a Win98 DOS box, and
  plays on pure DOS.** The user's report (2026-09-24): it looked
  timing-sensitive, and the 386DX combo got it a little further.
  Headless (`tools/win98-game-test.sh` on `base98-br`, the redump cue in
  the drive) the DOS box run is a DOS/4GW general protection fault right
  after "preloading fighter data", with EIP in the game's *data* object
  (a wild jump): the same unthrottled, with the precise retrace, and with
  every TCG fast path off (`x87-fast`, `sse-fast`, `simd-fast`,
  `rep-fast` and all nine `-accel tcg` switches), so not one of ours. The
  same install on the same image booted to DOS (`MSDOS.SYS`
  `BootGUI=0`, `OAKCDROM.SYS` + MSCDEX in CONFIG/AUTOEXEC, `MK3.EXE` from
  AUTOEXEC.BAT, unthrottled) plays: intro, fighter select, whole rounds,
  the round timer and the continue countdown at their own pace, and the
  Sound Blaster's own output (the CD's audio muted) recorded through
  `-audiodev wav`. A PCem report
  of the game going black on Start Game under Win9x, fixed by MS-DOS mode,
  is the same finding. Two things stood in the way and are fixed: the
  retrace (doc 06 "The display adapter") and the DOS drivers' "no disc"
  (patch 56). For the user: run it from a DOS family machine, or `SETUP
  /I 7` from the guest tools and open "2ksbox MS-DOS mode" on the desktop
  (doc 06 "DOS games in a Win98 DOS box"; the CD-ROM and `BLASTER` are
  there, and the game's SETSOUND then offers the SB16). Sound: the install has no
  `DIG.INI` (SETSOUND offers the Sound Blaster drivers only with a
  `BLASTER` variable, which the image does not set); a hand-written one
  (`SB16.DIG`, 220h/5/1/5) gets "Digital sound hardware not found" in the
  DOS box, where Blood and Duke find the card, and works on pure DOS.

- **Known failures on the Linux box** (M21's run on 11.1, 2026-10-02):
  `pit-guest` (the 15.6 ms-wait cases run the guest clock at 13 %, the
  same on a QEMU built without patch 56; on the PC the run as built is
  at 13 % too, since Windows' own waits are 15.6 ms). (`exec-no-device` failed until
  DXVK patch 16 on 2026-10-09: DXVK's exception crossed into the executor
  and aborted in the unwinder. `package`
  failed there too, on `build/` being a symlink, until `package-linux.sh`
  took its stage's real path on 2026-10-04; `companions-env` and
  `mode-sweep` on `NoCompositor`, until `mitsuami_check` stopped leaking its
  Broadway `XDG_RUNTIME_DIR` on 2026-10-09.) The Mac's host stage is green
  (with no Vulkan loader, DXVK segfaulted in the native harnesses until
  DXVK patch 17 on 2026-10-09; `dxvk-no-loader` checks it).

- **The zero-copy ring's frozen slot has no known cause.** `zc_probe()`
  repairs it; doc 12 §4 has what was ruled out and the suspects left.

- **3DMark 99 on the Windows PC: two threads** (M14/M11, `base98-br`,
  `scripts/win-voodoo-ab.sh`, log `build/win-voodoo-ab.log`; the FIFO
  hangs are fixed, doc 21 §9).
  - *A garbled loading screen*, seen once, usually the Fill Rate one. It
    is the 800x600 desktop on `d3dpt-vga` in stale bands, not a Voodoo
    frame. `vga:full-frames=on` does not change it, and no flips or
    executor batches run meanwhile, so the guest wrote those bytes. Next:
    which blit draws that background and where it reads from.
  - *The whole machine 3x slower after some guest restarts*, with no
    Voodoo (`no-voodoo`): 46.3, 46.7, 62.4, **15.0**, 37.8 fps across
    restarts in one player run, everything slower by the same factor. Not
    a context leak, the ring falling back to MMIO, or audio or input
    stalls. `d3dpt-vga` reports `N batches in 5.0 s, M ms of them in the
    executor`, and the script passes `-msg timestamp=on`. A flat host
    share with the rate halved points at the guest or the vCPU. Also
    check inside Windows (Performance tab: a file system not "32-bit"
    after hard resets) and the host's CPU use.

- **The NT side of the 3DMark2001 fixes is not re-run** (doc 19 §38):
  `xp-driver-test.sh install` installed no driver on a `winxp-m7`
  overlay (HEAD's and the previous driver alike), so BUMPTEST on XP is
  unmeasured. It matches the DRVINST Logo-dialog watcher bug fixed since;
  re-run with a current DRVINST.

- **A fault inside a DDI callback leaks the command-window lock** and
  freezes the session until the process dies (doc 19 §36). Accepted for
  v1 (user decision, 2026-09-16). The fix is an exception frame that
  releases the lock on unwind.

- **Win98 `SETUP /ALL` over an installed driver: `WININIT.INI [rename]`
  sometimes lacks the `SYSTEM\` entries** (`VOODOO=1
  tools/setup-guest-test.sh` on `~/vms/win98.qcow2`). All seven copies
  are staged and logged, but the INI read right after holds the four
  `INF\` renames and zero or one of the three `SYSTEM\` ones. Unknown
  whether the 9x profile cache had not flushed yet (`REBOOT=1` shows if
  the restart still applies them) or the renames are lost. That image's
  `SYSTEM\GLIDE*.DLL` and `FXMEMMAP.VXD` are read-only leftovers, so its
  three Voodoo marker checks fail regardless.

- **On the Cirrus, a VESA picture comes out in swapped blocks** (the user,
  in Duke Nukem 3D). The colours were the missing 4F09h, now fixed. Ruled
  out:
  - The chain-4 bug: in VBE modes both adapters map 0xA0000 as a RAM
    alias, so writes never reach `vga_mem_writeb`.
  - The window granularity: the Cirrus's 16 KiB is the hardware's, and
    `tools/vga-dirty-guest-test.py vesa cirrus` passes (`GRAN64=1`
    assumes 64 KiB, as the A/B).

  Untested hypothesis: Build's own Cirrus SVGA driver banks through
  GR9/GRB directly and may disagree with `cirrus_update_bank_ptr` about
  GR0B bit 5. Next: a test that drives those registers both ways round,
  or a log of the game's register writes.

- **Protected discs against the EDC-first cooked read: argued, not
  measured.** Re-run `discx scan` on the rig's protected dumps
  (`docs/tracks/m5-cdrom-backend.md` has the checks, doc 17 §2.5;
  `LIBDISC_NO_CORRECT=1` is the A/B).

- **The CD-ROM drive has no speed model.** It advertises 4x, `SET CD
  SPEED` does nothing, and a whole-disc read runs at hundreds of MB/s.
  `throttling.bps-read=` holds back a `.iso` but not a libdisc image.
  `tools/cd-rate-guest-test.py` shows the bytes do not depend on the read
  rate. Untested: whether a title that paces itself on CD reads minds a
  drive 100 times too fast. That needs a `speed=` on `ide-cd` both
  drivers honour (`docs/tracks/m5-cdrom-backend.md`).

- **Below the Vulkan floor: no real-user numbers.** Nobody has measured
  how many users are below DXVK's Vulkan 1.3 bar, or whether on such a
  host software Vulkan (lavapipe, "available, in software (slow)") beats
  the Wine executor (ADR-013/018, `launcherx --host-check`).

- **3D hand-off sync is `glFinish`** on both platforms. A fence would let
  the vCPU go on while the blit drains (doc 12).

## Next steps, in order

Each track's own order is in its track doc. This is the order across
tracks, plus the items no track owns.

0. **The review pass's Windows half** (`tracks/review-fixes-windows.md`,
   branch `track/review-fixes`): built and tested on the PC, the WDDM
   driver's four fixes made and checked on Windows 11 x64 (2026-10-25).
   Left: the driver on 32-bit Windows 7, `--publish`, the package, the
   player and launcher by hand, the executor's GDI cache and limits in a
   long Windows 11 session, and two open driver items (raw handles, host
   contexts left by a crashed process). Merge the branch after.

1. **M15, the Direct3D fallback on Wine** (ADR-018,
   `tracks/m15-wine-executor.md`). Steps 1–7 are done. Left: a game
   through the Flatpak's Wine add-on on a below-floor host, and the
   spike's two host tests on the rig's Linux Wine. A Windows host below
   the floor is not part of this; it runs its own `system32\d3d9.dll`.
   The Intel Mac app (ADR-019, built on the Air under Rosetta,
   `build-macos.md` "The Intel build") stays "untested" until an Intel
   Mac runs the reference scene; nobody here has one. The community
   build's floor, macOS 12 (`build-macos.md` "The floor"), was set
   before the AppKit launcher and holds until that launcher runs there.
2. **The measurements doc 22 still owes** (user decision, 2026-09-15).
   The Ryzen half of §6.2's games, including 3DMark2001 SE's high-detail
   Car Chase and Lobby as the benchmark for patch 47's inexact mode (+47 %
   and +15 % on the Air). Helper reach on x86-64, where it is the
   *always* case (`call [rip+pool]` from a PIE or a Windows EXE): a Linux
   / Win32 variant of patch 63 as the A/B through `tools/specbench`.
   Three QEMU builds from one tree behind a meson option (switches
   removed / hardwired on / switchable) to price the switches; later,
   "all off plus one switch". Not worth chasing: `bl` reach on the Mac
   (2 %).
3. **OpenGL pass-through (M3, doc 12).** Fence-based sync instead of
   `glFinish`, and a game on a Windows host (GLQuake). The Glide
   pass-through is gone (ADR-020, 2026-09-23); a Glide game on the DOS
   family runs on the Voodoo 2 and has not been tried.
4. **Display (M2, doc 03).** An answer for presets with no resolution
   override. XP's mode table fed from the player and a present
   signal in phase with its swapchain (M7). The player's own overlay
   controls: a snapshot (pause and the disc swap are its menus' since M22; doc 07).
5. **Windows host (M11's leftovers).** Moto Racer's speed on the PC
   (CPU-bound, not reproduced on Linux; M11 track doc). Live control
   over Winsock AF_UNIX on a real PC. The Store upload: the package
   installs through `scripts/win-sideload.ps1` and passes the
   certification kit, and the listing, privacy policy and steps are in
   the store repo; the Partner Center account is open (2026-10-06) and
   the workspace version is 1.0.0 (2026-10-06; the Store refuses a
   first number of 0) and the name `2ksbox` is reserved (2026-10-07);
   the MSIX is packed with that identity and certified (2026-10-17);
   left for the user, the submission and
   screenshots of the player's window with games in it (never the
   guest-frame shot scaled up). Zero-copy frames through a DXGI shared handle.
6. **M14, Voodoo 2** (its track doc, "Open, in order"): the glitched
   second Glide game, a client resuming on a dead ring, DxDiag's
   Direct3D 7 `GetDC` failure, the Windows build, Diablo II's
   numbers, patches 64 and 71 upstream.
7. **M10, Win98 driver** (its track doc, "Next steps"): the
   command-window lock (§36), the ACPI standby resume (§41).
8. **M12, music.** One dxdiag music run on Win98 with
   `LIBSYNTH_MIDI_LOG` and `LIBSYNTH_OPL_LOG` set (doc 20 §7.2). Then
   "MPU-401 Compatible" from Add New Hardware, the step Win98 needs
   before it plays MIDI to the port, and a host MIDI port (doc 20 §8).
9. **M6, launcher and packages.** `CDSHELF.EXE`'s Win98 (ASPI) run.
    The AppImage (6b′), the standalone Windows installer (6d) and a
    Flathub submission are dropped: the project publishes binaries only
    through app stores (ADR-028, 2026-10-06), and the MSIX is the
    Store's.
9a. **M19, the launcher on mitsuami.** The flip is done (2026-10-02);
    mitsuami is the crates.io release 1.3.6 (since 2026-10-20, from
    crates.io since 2026-10-16; a git
    `rev` before; ADR-023's amendment), and
    `packaging/flatpak/cargo-sources.json` is regenerated for it, crate
    archives only; the Linux packager ran with it on 2026-10-16 (every
    packager has); left: the macOS floor with an AppKit launcher and the
    KDE add-on in a real Plasma session
    (`tracks/m19-mitsuami-launcher.md`).
9b. **M22, the player on mitsuami.** Step 2: the pointer, the menus and
    the close alert on a real desktop, and mitsuami's tiled-window offset (`tracks/m22-mitsuami-player.md`).
    Windows 11 on Arm's desktop (step 7): the copies are down to what
    changed (2026-10-08) and the guest is not short of CPU (dwm 59 % of a
    core while a window moves); never a 1x guest screen (the user wants
    the window's pixels).
10. **M5, CD-ROM.** Triage FIFA 2002's no-match. Age of Mythology disc 1
    as a second SafeDisc 2 title. SecuROM (needs DPM in `mds.rs`).
    Multisession. CHD. Win98's CD Player by ear. M5g: an MSCDEX run and El Torito
    (the stale-file rule is checked on the host only; a guest check would be flaky).
10a. **M20 step 5, our WDDM driver on Windows 11** (`tracks/m20-win11.md`,
    "Resumed with M24's lessons", 2026-10-10; the video driver on hold,
    user 2026-10-10: "forget the video driver for now", and carried on
    by M25): Explorer's XAML, which never composed on the Direct3D 9
    driver, draws since DWM composes through M25's Direct3D 11 driver
    (WDDM 1.2, 2026-10-17). The host device lost on `d3d9=system` with
    every `Reset` failing was the guest's event queries, which the
    executor kept alive through the Reset (fixed 2026-10-22,
    `d3dpt-dp2-system` checks it; not yet seen recovering in a Windows 11
    session). The desktop follows the window (2026-10-22, register set
    v12: the device takes the window's size once the driver says the
    agent applies it, the driver makes it a custom mode in its and the
    monitor's lists, the agent sets it; doc 15 "The desktop following the
    window"; by hand on the PC's test machine, not yet on Arm or through a
    launcher machine's own agent). x64 Windows 11 on our adapter has it
    alone since the same day (EDK2 patch 04 drives it for the firmware;
    no standard VGA, no second screen; doc 06; on Linux too since
    2026-10-23, when `build.sh`'s `edk2` stage started building our OVMF
    there), and so does one on
    viogpudo: `virtio-vga` alone, whose VGA half upstream OVMF already
    drives. Windows 11 on Arm too since 2026-10-23 (the user: no second
    screen there either): QEMU patch 100 (`virtio-vga` on `virt`), EDK2
    patch 05 (QemuVideoDxe in ArmVirtQemu, frame buffer write-combined),
    no ramfb; and viogpudo moved off setup's drivers to the disc's
    `2ksbox\viogpudo`, added at the first logon, since in setup it took
    the one card and drew nothing. On the Air: the user's machine (a copy)
    on both adapters, firmware, Windows and its driver on the one card,
    viogpudo at the window's 2560x1676; setup from the Arm ISO with the
    drivers disc on screen (`win11-boot-prompt`). Not yet run: a whole
    install to the first logon's viogpudo, and x64 setup on the new disc.
    Left: `FRAMES` on the host's blank for
    98 / XP (the user saw 3D games judder).
11. **M24, our viogpudo** (`tracks/m24-viogpudo.md`): on x64 Windows 11
    (the PC) the guest's blank follows the host screen (72 Hz on 144),
    the desktop follows the window and the virtio-gpu's screen is the
    only one, and the host screen's blank raises the guest's (step 4 B).
    The user: "super smooth, desktop resizes correctly" (2026-10-09).
    On the Air (2026-10-09): macOS reads the screen's rate and blank
    (`screen.rs`, a `CVDisplayLink`); Windows 11 on Arm needs Smart App
    Control off for test signing, its desktop is choppy on the driver's
    timer (blanks bunch on a coarse tick under HVF), and with the host's
    blank it froze until QEMU patch 91 latched HVF's MSI pulses; with it
    DWM composes every 16.661 ms (16.45–16.93) and the user: "man, now
    this is smooth, it's awesome" (track doc, step 4 "On the Air").
    `viogpudo-install.ps1` now checks test mode (and names Smart App
    Control) before it touches the installed driver (the PC has published
    it for the new source hash). On Arm, ramfb's screen left extended was a machine without
    the guest agent (`2ksbox\install.cmd` never run: with it, the agent
    takes ramfb's screen off at logon); a new machine now gets it at its
    first logon (the drivers disc's `autounattend.xml`, which also asks for
    a local account), installed and checked on the Air. Its first pass
    names no product key with the key page shown, or Windows 10's setup
    stops at once ("cannot read the <ProductKey> setting"; 2026-10-15,
    both setups checked to their key page under KVM). The launcher has
    no way to delete a machine (a deleted disk comes back empty at its
    size since 2026-10-11). Linux reads the screen's rate and blank through DRM
    (`screen.rs`; 59.996 Hz on the user's DP-1), on the screen the
    window is on (Wayland's presentation feedback, X11's RandR); on a copy of the user's x64 `win11` under KVM
    with our viogpudo, the user: "it's very smooth". Until step 5 (shipping:
    attestation signing or upstream) the drivers disc carries ours opt-in
    in `2ksbox\experimental-display` (2026-10-11): a README of warnings,
    and the script warns twice before it changes anything; next, run it
    from the disc on a fresh overlay.
11a. **M25, Direct3D 10 and 11** (`tracks/m25-d3d11-umd.md`, "Open"):
    a blt-model (DISCARD) window's present goes through Windows'
    system-memory redirection (a STAGING copy the CPU reads, DWM's
    upload, one present per composition: the driver claims no GDI
    acceleration; CPU-visible texture bitmaps, `SupportSoftwareDeviceBitmaps`,
    were tried and change nothing for the present. GDI acceleration is
    in and **off by default since 2026-10-19** (the service key's
    `GdiAcceleration` = 1 turns it on): with it on GDI draws into a
    Direct3D 11 GDI-compatible texture itself, with the CPU, which a
    shared allocation in our memory segment cannot allow, and Notepad's
    text area stayed black; off, the frame rates are the same (~250 and
    ~470 frames/s unsynced, 58 with vsync) and every window draws
    (`gditest` 37 of 37; the track's "GDI-compatible textures"). With
    it on (protocol v27's
    `GDI` record): RenderKm's copies and fills (and since v29 every other
    operation, ROPs, StretchBlt, AlphaBlend, TransparentBlt, ClearType,
    done by the host on the CPU and proved by `d3dpt-gdi` against the
    PC's own GDI: Windows 11 sends next to none of them, the track's
    "GDI's other operations"), a present into a window's
    redirection bitmap and its paging done by the host, GDI's own CPU
    drawing brought over from the bitmaps' VRAM (they are CPU-visible;
    the host compares each with a shadow when DWM is about to sample it):
    the desktop draws. Open: with the bitmaps CPU-visible a DISCARD
    window's present goes into a STAGING copy the CPU moves into the
    bitmap, not into the bitmap (~240 frames/s unsynced since the
    display stopped waiting for flips' readbacks under the BQL and the
    staging surfaces are mapped cached, ~85 before; the flip model
    ~515; the track's "Where the STAGING frame went"); the x64 kernel
    driver's flips on the blank have been MMIO flips since 2026-10-18,
    `FlipOnVSyncMmIo`: a DMA flip held for the blank
    held every process's later fences, ~37 frames/s in runs of one and
    two blanks, and the no-wait DMA flip was bugcheck 0x113 at the
    adapter's stop; the track's "Flips that hold nothing"); the guest's
    vertical blank follows the host's again (the player answers a blank
    wait Windows refuses as occluded with DWM's composition, and logs it;
    QEMU patch 96 and register set v11's `HOST_MHZ` give the driver's modes
    the blank's real rate; "The host's vertical blank, again"), its ±2 ms
    open (not the batch: under a hardware accelerator the executor's
    batch runs without the BQL since 2026-10-18, flip model unsynced
    253 → 323 frames/s, DWM's pace unchanged; "Batches without the BQL";
    since protocol v28 a flip's readback is finished when the host's GPU
    is done or the display needs it, `d3dpt_exec_settle`, not waited for
    in the batch: flip model unsynced 323 → ~350-440 frames/s);
    which multisample claim the runtime refuses; games on Windows 11 on
    Arm (on the Mac since 2026-10-20 the desktop, Mica included, runs on
    the driver, DWM on DXVK's d3d11 on KosmicKrisp, QEMU patches 97 and
    98, `d3dgame8` / `11` at 60 frames/s; Unigine Heaven in Direct3D 11
    crashes in KosmicKrisp's shader linker, a driver bug; every package
    carries DXVK's d3d11 and dxgi since 2026-10-19, and a Linux host runs
    it under KVM, the user by hand 2026-10-20; the track's "The packages,
    and the driver for Windows 11 on Arm" and "Open"); Heaven in
    Direct3D 9 at 1-3 frames/s; then 11_0 (compute, unordered access
    views, tessellation) and Windows 7's DWM on Direct3D 10.1 (10_1 and
    Direct3D 10 programs are done, 2026-10-20). The kernel driver claims
    WDDM 1.2 since
    2026-10-17, so DWM composes on Direct3D 11 (the user-mode drivers list
    another process's allocations as read: a 1.2 kernel refused DWM's
    writes to a flip-model buffer it opened read-only, found with cdb on
    DWM; the track's "WDDM 1.2 claimed").
12. **The finished tracks' leftovers.** M4: a decoder thread for the executor. M7: a shader title, a
    split-stream title, 3DMark2001's Nature for cubes, StarCraft on 8 bpp,
    a driver stage in `scripts/test.sh`.
    M8: a Direct3D title with and without `*-fast=off`. M9: binary32 at PC=24
    on aarch64 (0.49 s against PC=53's 0.38 s on the Air, not profiled). M16: the OpenGL ICD
    (step 8, left open by user decision), vertex texture fetch in a title (D3DFEAT9's row F proves the path on XP and Win98, 2026-10-21), a game's depth-texture shadows on Win98 (D3DFEAT9's row G proves the path on XP and Win98, 2026-10-22).
    M17: host vertex buffers for the draws, one HAL walk instead of two,
    the readback's wait (`tracks/m17-driver-perf.md`, `tools/w98-mp2.sh`).
12. **Release 1.0.0** (`docs/release.md`, `scripts/release.sh`). The
    Linux gate passed on 2026-10-16 (`test.sh all`: 73 pass, the known
    `pit-guest`; `package-linux.sh`) and again on 2026-10-17 from
    `86057b78`'s icons, `package-flatpak.sh` too (the Wine and KDE
    add-ons; it had failed since 2026-10-07 on the 1024 icon, which
    flatpak's export refuses, so Linux packages stop at 512), and the whole gate
    again from `a5a18ece` (patch 94), the same results. The PC's
    passed on 2026-10-17 from `86057b78`, `54623171` and `a5a18ece`
    (QEMU patch 94) (`test.sh all`: 65 pass, 0 fail, `pit-guest` included each time
    (an earlier run that day failed it as on Linux), 18 skips
    that need Linux, macOS, a display, Wine, the winetests or a Windows
    7 disc; `package-windows.sh --msix`; the Store MSIX, 1.0.0.0 with
    Partner Center's identity `RoboportTecnologia.2ksbox`, sideloaded
    with `-Check` and through the certification kit:
    `OVERALL_RESULT=PASS`, the two optional FAILs of `build-windows.md`),
    and from `775833c6` (docs only since `a5a18ece`, so no `test.sh`):
    the build, the packages, the sideload and the certification kit,
    the same results. Windows' and Linux's icons lost the teal plate that day; only macOS
    keeps it. `README.md` and the manual's `install.mdx` link both
    listings since 2026-10-17 (user): the Microsoft Store's, Store ID
    `9NVNN81VKF05` (<https://apps.microsoft.com/detail/9NVNN81VKF05>),
    and the Mac App Store's, `id6819470979`
    (<https://apps.apple.com/app/2ksbox/id6819470979>). Left: the Microsoft submission, the Air's
    gate, the App Store package, then the tag, which the user makes on
    the commit both packages come from.
    The manual's release notes are the stores' *What's New*. Every
    package carries `licenses/` beside THIRD-PARTY-NOTICES.md
    (`tools/third-party-notices.py stage`, which fails on a missing text;
    run on Linux and the PC, not yet on the Air; glib's text is
    `LICENSES/LGPL-2.1-or-later.txt`, since Windows' unpack turns the
    `COPYING` symlink into a `.lnk`), and App Store re-uploads take
    `package-macos.sh --build N`.
    **1.1.0** (2026-10-18): v1.0.0 is tagged on `775833c6`; 1.1.0
    carries the TestFlight fixes (QEMU patch 95, the machines folder,
    mitsuami 1.3.3's New Folder, the empty library's width). The App Store package no longer carries the downloaded profile's quarantine (`build-macos.md`), which App Store Connect rejected for 1.0.0 and 1.1.0. Its launcher's sandbox has `network.server` too: with only `network.client`, every reply to the guest's UDP (slirp's unconnected sockets, DNS first) fails `recvfrom` with EPERM while `poll` keeps saying readable, so QEMU's main loop spun holding the BQL and a TestFlight Windows 11 lagged through its updates (a million sandbox denials in four seconds); `package-macos.sh` checks both. App Review asks why it needs `network.server` (2026-10-20): the reply is the guest's UDP. And since 2026-10-20 a denied reply no longer spins: libslirp drops a UDP socket whose `recvfrom` fails EPERM (`patches/deps/libslirp/02-udp-eperm-detach`), costing that one reply. Its gates
    run from the version bump's commit. The PC's ran from `40eb7822`
    (2026-10-18): the build, `test.sh all` without the XP guest tests
    (user; `WINXP_IMG` at no file skips them) 57 passed, 0 failed, the
    packages, the Store MSIX 1.1.0.0, the sideload `-Check` and the
    certification kit (`OVERALL_RESULT=PASS`, the two optional FAILs).
    THIRD-PARTY-NOTICES.md carries the written source offer (GPL-2
    §3(b); contato@roboport.com.br), About links this version's tag,
    and its Licences button shows every text the package carries
    (2026-10-17, user). UIDE's terms are "free with sources", so the
    guest tools disc carries its source, `DOSMODE\UIDE.ASM`, from FreeDOS
    1.4's package (2026-10-17, user; `third_party/uide/UPSTREAM`).
13. **The user manual** (`manual/`). Screenshots of the real windows,
    taken through `LAUNCHER_SHOT` and the player's headless dump so they
    never go stale; the store links once the listings are up.

## Gotchas

Cross-cutting traps, each as symptom → cause → rule. A trap that belongs
to one subsystem lives in its design doc; pointers are at the end.

### Building

- **The repository moved to `github.com/Roboport-Tecnologia/2ksbox`
  (2026-10-05).** GitHub redirects the old `davidrios/2ksbox` URL, but
  a checkout made before the move (the Mac, the PC) should run `git
  remote set-url origin git@github.com:Roboport-Tecnologia/2ksbox.git`.
- **A new flag in `configure-qemu.sh` reaches a build only through a
  configure.** `build.sh` now reconfigures when that script is newer
  than `build/qemu/build.ninja` (it used to watch only the meson files):
  a checkout built before libpng and libjpeg were disabled kept linking
  them, and `no-optionals` failed on a build that was right when made.
- **Two builds must never share the `qemu/` tree at once.**
  `build-windows.sh` re-applies the patch queue while
  `package-flatpak.sh` copies the tree, and the copy fails deep in the
  compile with a header neither build uses. The outputs (`build/qemu`,
  `build/win/qemu-msvc`) are separate; the sources are not.
- **A build belongs to one checkout.** Never point `QEMU_BIN` or any
  `*_BIN` at another checkout's artefacts, configure into its `build/`,
  or run its scripts. That tests someone else's patch queue, and meson's
  recorded source path makes the other build compile *your* sources from
  then on (check `build/qemu/meson-logs/`' "Source dir"). Moving a
  checkout invalidates `build/` too: `scripts/build.sh -f`.
- **A source edit with no effect: is its directory in the stamp?**
  `scripts/build.sh` skips `prepare-qemu.sh` when the hashed inputs of
  `stamp_stale qemu-prepare …` are unchanged, so an overlay missing from
  that list rebuilds the *old* file silently (`libsynth/qemu` was). `-f`
  bypasses every stamp.
- **Removing a patch to A/B it leaves its edits behind.**
  `prepare-qemu.sh` restores only files a *current* patch touches, so a
  dropped patch's changes to other files, and its new files, survive.
  Use the patch's off switch if it has one; otherwise `git checkout`
  those files in `qemu/` and re-prepare.
- **`configure`: "found no usable distlib".** pip 26 vendors
  `distlib.scripts` but not `distlib.version`, which QEMU's `mkvenv`
  imports. Install the real `distlib` for that interpreter. Python is
  uv's 3.12; 3.14 works only with the real `distlib` (MSYS2).
- **An ISO older than its sources means a stage died.**
  `build-wrappers.sh` is `set -e` and writes the ISO last. On a Mac,
  Homebrew's mingw is a symlink, so `build-driver.sh` finds the DDK
  headers through `-print-sysroot`.
- **Guest binaries are msvcrt and `-march=pentium3`.** Modern mingw links
  the UCRT, which 9x lacks and XP never loads, and qemu-3dfx compiles for
  `x86-64-v2`; the scripts force and check both. Define `PSAPI_VERSION
  1`, or `psapi.h` binds Windows 7's `K32*` exports and XP's loader stops
  the process in a hard-error box before `DllMain`.
- **Host toolchain.** QEMU needs `--disable-werror`, `-fPIC` and
  `b_staticpic` for the shared library. On macOS every stage targets
  the community build's floor (`scripts/macos-floor.sh`, 12.0); a hand-run
  cargo needs `MACOSX_DEPLOYMENT_TARGET` exported, and a `cargo clean`
  after the floor rises (`build.sh` does both). The macOS link needs
  `qemu_default_main` (defined in `embed/libqemu_embed.c`) and our ld64
  export list.
- **Windows: `Unable to create index.lock: File exists`, the lock gone
  when you look.** A scanner holds the lock of the git that just exited.
  `prepare-qemu.sh` runs every git through `qgit`, which retries on that
  and nothing else. A retrying wrapper must pass git's stdout through
  (`2>&1 >&3` inside `{ } 3>&1`) or `ls-files` returns nothing.

### Running on a Mac

- **A GNU-only command in `test.sh` can end the run early with a green
  summary.** macOS has no `nproc`; the `cpus` check's `$(( $(nproc) …
  ))` was an arithmetic error, and bash drops the whole top-level `case`
  for that, so from 04d6e9cc to 2026-10-17 `test.sh all` on the Air
  stopped after `machine-details`. The summary still printed "0 failed"
  and the script exited 0. Count the stages (`==> guest stage`, `win98`, `win7`) in the
  log, and use `getconf _NPROCESSORS_ONLN` for a core count.
- **`/opt/homebrew/lib` on `DYLD_LIBRARY_PATH` kills every image
  decode** (`SIGBUS` at `0xbad4007` in `IIO_Reader_GIF`). dyld searches
  it by leaf name first, and on a case-insensitive disk it answers
  ImageIO's `libGIF` / `libPng` / `libTIFF` / `libJPEG` with Homebrew's.
  Put only `/opt/homebrew/opt/vulkan-loader/lib` there; unsetting it at
  run time is too late. `DYLD_PRINT_LIBRARIES=1` shows it.
- **A benchmark a third slower than the last is a far launch.** TCG's
  code buffer 8 GiB from the helpers turns every helper call into
  `movz/movk ×4 + blr` (x87 / SSE helpers at 0.55–0.65x). Patch 63
  reserves it near the image at load time. Check the JIT addresses in a
  `sample` before believing a difference, and use
  `build/specbench/noaslr` for runs that must repeat (doc 22 §5.0).
- **A DXVK program's memory is its peak footprint** (`/usr/bin/time
  -l`), not RSS, because GPU memory is the same RAM. SIP strips `DYLD_*`
  at every system binary, so put `env DYLD_LIBRARY_PATH=…` last in a
  wrapper chain. A producer that never waits outruns DXVK's deferred
  frees (a 16 GB Mac swapped for minutes; doc 14).
- **Never call `gl*` / `CGL*` / `IOSurface*` by link in the embed
  backend.** The symbol can bind to a GLX library that silently no-ops;
  `dlsym` from the OpenGL.framework handle.
- **Only the window server's private hot key mode takes Cmd+Tab and
  Ctrl+Up from the host** (`CGSSetGlobalHotKeyOperatingMode`, what UTM
  and VirtualBox call). Carbon's public `PushSymbolicHotKeyMode` is a
  stub on macOS 26 (reads back as pushed, changes nothing), and an event
  tap with Accessibility never sees those chords. Doc 03 "Input path".

- **The App Store's installer leaves the app's files compressed**
  (`ls -lO` says `compressed`), and `lseek(SEEK_DATA)` answers `ENXIO`
  at every offset of such a file. QEMU took it for one hole, so pflash
  loaded nothing and Windows 11 on Arm from TestFlight sat at "Guest has
  not initialized the display" with an all-zero flash (`xp /4x 0` over
  QMP; 2026-10-18). Patch 95; a check against an installed app needs
  its own copies, `ditto --hfsCompression` makes one (`compressed-file`).

- **macOS's open panel never shows a folder inside the app's sandbox
  container**: handed one, it opens at its last folder. So every launcher
  asks for its machines folder at start, and the App Store build's
  other answer is "Later", never the container (`library::chosen_dir`,
  doc 07; `library-folder`; 2026-10-18).

### The player and the QEMU thread

- **Never `exit()` while the QEMU thread is alive.** QEMU's atexit
  handlers race `qemu_cleanup` (`mutex->initialized` on macOS). The
  player joins the thread; headless paths use `_exit`. A guest power-off
  ends the loop while the UI still holds the handle, hence the stop /
  release handshake before `qemu_embed_destroy`.
- **An occluded window gets no swapchain image.** Per-frame work that
  must not stall (importing zero-copy slots) runs on the wake event.
- **A whole player that lags (pointer, menus, Close) with a guest that
  is fine is the UI thread waiting.** It was the draw's wait for the
  swapchain, 16.5 ms a frame on Windows at frame latency 1 (2026-10-16):
  player-mitsuami now draws on a thread of its own (`render.rs`) and
  Windows' frame latency is 2 (doc 03 "Latency budget"). Anything the UI
  thread waits on (the session's lock, `Render::resize`) is kept short.
  Measure before guessing: time `draw` and a 10 ms UI timer's lateness.
- **A crackle report: ask for the lines first.** `[audio] device asks for
  N frames`, `qemu-embed: audio:` and `[audio] the guest's mix went past
  full scale`. A timing fault and a clipping fault sound alike and read
  differently. Pacing design: doc 11; `tools/audio-glitch-test.py`
  (`STALL=`, `CDAMP=`) reproduces both.

- **Host shortcuts stay the host's in the Flatpak on sway.** The
  sandbox's Wayland socket carries a security context and wlroots hides
  the shortcut-inhibit protocol from it; the player says so on stderr.
  A limitation by decision (2026-09-23), with the X11 override in
  `development.md` "Flatpak" and the README. Doc 03 "Input path".

### Driving a guest headless

- **Wait for the guest, never a clock.** `tools/guestwait.sh` waits for a
  line our device wrote, QMP block stats going quiet, or a knock on the
  Run dialog answered on COM1 (`docs/testing.md`, "Driving a guest").
  `BOOT_WAIT` and friends are caps on giving up. A screendump is never
  evidence of life: `vga_draw_text` draws over a dead machine.
- **A frozen first frame with a blinking caret is a guest with no timer
  interrupt**, not a hung emulator. `info registers` twice (EIP
  unchanged), `info pic` (an unmasked `irr` bit with `isr=00`), `info
  lapic` (`LVT0 masked`). Win98's restart was this (patch 22).
- **A "hung" Win98 desktop may be idle and unrepainted**: EIP moving with
  `HLT=1` across two `info registers`. `hang.txt` from
  `win98-game-test.sh` has it.
- **XP's lazy writer holds small FAT writes for minutes.** A harness asks
  COM1, not the scratch disk, whether a command finished.
- **Four shell traps that read as the test failing.** A `pgrep -f` /
  `pkill -f` pattern that appears in the calling command matches the
  wrapper (and `pkill` kills the session's shell); use `patter[n]`. Find
  QEMU with `ps -C qemu-system-i386 -o pid=`, never `pgrep -x` (`comm` is
  truncated at 15 characters). A deep `OUT=` makes the QMP socket fail
  with `AF_UNIX path too long` and the run does nothing. Editing a bash
  script under a running instance breaks that instance.
- **`grep -c` prints `0` and exits 1**, so `$(grep -c x f || echo 0)` is
  `0\n0`.
- **The user's images are read-only.** Boot a qcow2 overlay or a copy;
  while an overlay runs, the backing file is write-locked. Never run two
  TCG guests at once on one box: they starve each other and a slow run
  reads as a failure.
- **End a Win98 run with the ACPI power button** (`system_powerdown`).
  Keystrokes die in a modal dialog, and a machine that does not power off
  leaves the FAT dirty, so the next boot is safe mode: no driver, empty
  logs, exactly like the thing under test failing. A failed boot does the
  same; let the safe-mode boot finish before trusting the next run.
- **Win98 runs under TCG, not KVM** (the family's default). Under `-accel
  kvm` Explorer dies at start ("illegal operation", then *SHELL32.DLL is
  linked to missing export SHLWAPI.DLL:GetFileAttributesA*), so there is
  no Start menu to drive.
- **A bare `qemu-system-i386` has no 3D and opens no window.** QEMU is
  built with no display, host-audio or extra backends (`configure-qemu.sh`,
  the `no-optionals` check; `--disable-dsound` needed patch 23), so
  pass-through is refused for want of a context provider. With no
  `-display`, QEMU starts a VNC server on `localhost:5900`. Look with
  `-display vnc=:0`, and play into `-audiodev none`.
- **A game that "freezes" is often showing a message box you cannot
  see.** The player falls back to the VGA surface after 1 s without a
  presented 3D frame. Headless, `SHOTS=` in the game harnesses shows the
  box and `DRW_AFTER=` the stacks (XP).
- **A glitch shorter than a second shows only in the player's own
  frames** (`PLAYER=1 PLAYER_SHOT_EVERY=6`). QMP screendumps come once a
  second, a headless console refreshes only when asked, and a screendump
  shows the VGA surface, frozen while 3D presents.
- **An API's own answer is not evidence; the device's is.** `mcicda`
  answers `status mode` from the state it commanded (Win98 said
  "stopped" while the drive played on), and a check that greps for a
  *line* proves nothing about the value in it. Ask the device (a trace)
  and the output (the wav, the pixels), and assert the value.
- **Headless changes timing, not just output.** With no device window
  DXVK's `Present` returns at once, so a busy-poll of a query starved the
  thread that had to answer it. Put `Sleep(1)` between polls. A cold
  pipeline cache is the load that makes such a race show.
- **A game that runs far too fast is presenting, not timing.** Era titles
  pace by `Flip`, so a flip that never blocks is a missing frame limiter.
  `d3dpt-vga: N page flips in 5.0 s` is the guest's real frame rate; no
  line means it blits to the primary. `DDFLAGS=32768` turns the vertical
  blank off for the A/B.
- **KVM `-cpu host` breaks Max Payne's level loading** ("Corrupt JPEG
  data": a CPUID-dispatched decoder). `-cpu pentium3` under KVM works.
  Prefer an era CPU model for games.
- **An XP game "crashes at startup" with `0xc0000142`**: a DLL of ours
  returned FALSE from `DllMain`. Either qemu-3dfx's `OPENGL32.DLL` could
  not open `\\.\MAPMEM` (FXPTL.SYS and the MAPMEM service missing:
  install SETUP's device-mapper component as Administrator; OpenGL needs it
  too).
- **A benchmark inside a DOS `.COM` keeps its data off the code page**,
  or self-modifying-code invalidation dominates the number.

### Windows guests

- **Win98 must be an ACPI install**, or PCI hot-adds (USB tablet, AC'97,
  NIC) are never seen and Device Manager shows "Plug and Play BIOS".
  Setup compares F000:FFF5 with 12/01/99; `prepare-qemu.sh` stamps the
  firmware 12/31/99 (the `bios-date` check), so a plain `SETUP` installs
  ACPI. Repair an older image in Device Manager (`build-macos.md`), don't
  reinstall.
- **"Windows protection error" on `d3dpt-vga`, fine on the Cirrus.** A
  display driver built before 2026-09-12 wants the register set exactly
  and refuses a newer adapter, and `*DisplayFallback=0` leaves no VGA.
  Boot on the Cirrus, run the ISO's `SETUP /ALL`, switch back. Newer
  drivers accept any later register set (doc 15).
- **A fresh XP on the inbox VGA driver goes black (or to an empty text
  mode) when the resolution is changed, and at our driver's install**,
  which restarts the display the same way. Not the adapter: QEMU's own
  `-vga std` did it too, and KVM did not. Patch 44's retired TLB table
  was dropped in name only and came back at the next `mov cr3` (the VGA
  window's topology flush, then XP's int10 call), fixed 2026-09-24.
  `tools/xp-driver-test.sh <image> vesa` is the check; `-accel
  tcg,tlb-retire=off` the A/B. A black screen on a mode change is a TCG
  switch before it is a device.
- **`ExitWindowsEx` from a console program never returns on 9x** and
  holds the Win16Mutex; a worker thread makes it worse. Call it from a
  process with no console: `SETUP` re-execs itself detached as `SETUP
  /REBOOTNOW`. `rundll32 krnl386.exe,exitkernel` restarts with a dirty
  FAT. The proof of a restart is a second SeaBIOS banner.
- **Never overwrite a loaded 9x driver file in place.** KERNEL reloads
  discarded segments from the new file at the old addresses. Stage it as
  `NAME.EX_` and rename it through `WININIT.INI` on the restart
  (`guest-tools/README.md`).
- **An ISA device of ours must not sit on IRQ 9.** PIIX4 puts the ACPI
  SCI there, so a line only the guest can lower is re-entered on every
  `IRET` until #DF and a triple fault, which looks like a spontaneous
  reboot. DOS masks IRQ 9 and never shows it. Prefer no interrupt line
  where none is needed (doc 20 §5.1).
- **A guest that bugchecks under WHPX and runs under KVM**: look in
  `player.log` for "Unimplemented handler (<eip>) for <X>". WHPX runs
  every MMIO access through QEMU's own x86 emulator, which lacks
  instructions KVM's has and steps over them silently. PUSH was one
  (patch 93, 2026-10-12: Warcraft III's gamma ramp, our XP driver's
  `push [regs+0xa00]`, an unbalanced stack, a BSOD).
- **A level-triggered interrupt lost for good under WHPX** (a TDR with
  every fence done, the device's IRQ status pending): WHPX's EOI exit
  broadcast to the IOAPIC without the BQL, racing a device's raise on
  another vCPU. Patch 94 locks it (track M25, on main since 2026-10-17).

### Devices and corruption

- **What the guest writes is read once.** Shared memory (the command
  window, a header in VRAM) can change between a check and its use when
  another vCPU runs beside the device (any accelerator but TCG): copy it
  or read the field once, as `d3dpt_exec_submit` and `d3d_dma_append` do
  (doc 14 "Transport").
- **An interrupt a guest cannot acknowledge costs the line, not one
  interrupt.** On the edge-triggered i8259 every later assertion into the
  held line is lost silently (doc 20 §5.2). `info irq` counts rising
  edges, so a count that stops while the device still raises is this.
  Diagnose with `-d trace:pic_set_irq`, `trace-event-set-state
  memory_region_ops_write` over QMP for the ten seconds that matter, and
  the device's own status read from the monitor (`o` / `i`).
- **A crash that moves from victim to victim is memory corruption: A/B
  the TCG switches before the driver.** Win98 dying in `SETUP` was patch
  44's `uint16_t` TLB list wrapping, and one lucky control cost an
  evening. Use `-accel tcg,<switch>=off` per patch, repeat every control,
  and catch the reset with `-action reboot=shutdown,shutdown=pause -d
  cpu_reset`, reading the blue screen out of VRAM.
- **A QMP medium change must pass `force`.** Without it a guest that
  locked the tray (XP, for any open handle) gets an eject *request*, the
  command is refused, and the swap lands whenever the guest lets go.

### Where the subsystem traps live

- The WGL rule (a `wgl*ARB` call with no context faults): doc 12.
- The Win98 display driver: the text page behind the linear frame buffer
  (doc 19 §15), `pnpdrvr.drv` and the INF's `DelReg` (§16), `__loadds`
  and 32-bit register access (§14, §18), the DOS box's repaint (§29),
  Mode X for 320×200 (§30).
- The XP driver's dxg rules, the DirectX 6 flip chain, untracked GDI
  writes: doc 15.
- The CD drive: how each Windows stops a CD (doc 17 §5.4), and `libdisc`
  never asserting on a disc's values (doc 17).
- x87 precision modes and the batteries: doc 13. SSE: doc 16.
- A slang preset smearing its edges (`clamp_to_border`): doc 03.
- The macOS bundle: `build-macos.md` "The app".
- The Windows launcher (MSVC, WinUI 3, the App Runtime), the DLL
  closure, the Store package: `build-windows.md`.
