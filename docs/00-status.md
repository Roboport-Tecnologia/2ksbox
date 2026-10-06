# 0. Status and how to resume (updated 2026-10-05)

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
| **M6** launcher and packaging | `tracks/m6-launcher.md` | `launcher-core/`, `launcher-capi/`, `shader-chain/`, `scripts/package-*.sh`, doc 07 | Shipped (the front end is M19's since 2026-10-02, ADR-023), continues on `main` · AppImage, Windows installer, the player's own overlay controls |
| **M7** XP display driver | `tracks/m7-display-driver.md` | `d3dpt/hw/d3dpt_vga.c`, `d3dpt/hw/d3dpt_exec_load.[ch]`, `d3dpt/d3dpt_fb.h`, `d3dpt/exec/d3dpt_exec_ddi.cpp`, `guest-tools/src/d3dptvid/nt/`, `tools/xp-*.sh`, `tools/d3dpt-dp2-test.cpp`, doc 15 | Done through protocol v13 · a title for each probe-only DX8 feature, more 8 bpp titles, a driver stage in `scripts/test.sh` |
| **M8** x87 / SSE fast paths | `tracks/m8-tcg-fastpaths.md` | patches 05, 06, 11, 12, `tools/x87-*`, `tools/sse-guest-test.py`, docs 13, 16 | Done · a real Direct3D workload with and without `*-fast=off` |
| **M9** TCG on Apple Silicon | `tracks/m9-tcg-aarch64.md` | `tools/tcg-profile.*`, `tools/tcg-hot.py`, the TCG patches from 13 on | Done; optimization closed by user decision (2026-09-12) · binary32 at PC=24 slower than PC=53 on aarch64, the game tests uncapped on the Air |
| **M10** Win98 display driver | `tracks/m10-win98-driver.md` | `guest-tools/src/d3dptvid/core/` and `w9x/`, `guest-tools/build-driver*.sh`, `setup.c`'s 9x role, `tools/win98-*.sh`, doc 19, ADR-012 | Active; steps 0–4 done, step 5 (real titles) under way · the ACPI standby resume |
| **M11** Windows host | `tracks/m11-windows-host.md` | `packaging/windows/`, `build-windows.sh`, `package-windows.sh`, `package-msix.sh`, `win-run.sh`, `embed/mglcntx_embed.c`'s WGL half, `build-windows.md` | Done; Windows builds natively in MSYS2 (ADR-026), the package is all MSVC, runs guests on the user's PC and packs as a Store MSIX · Moto Racer's speed there, live control over AF_UNIX, the Store upload, an installer |
| **M12** music | `tracks/m12-music.md` | `libsynth/`, patches 60–61, `soundfonts/`, `bundle::Sound` / `Music`, `tools/midi-guest-test.py`, doc 20 | All stages landed · capture Win98's failing MIDI run, a host MIDI port |
| **M13** gamepads | `tracks/m13-gamepads.md` | `player-core/src/pad.rs`, `gamepad/`, patches 26–27, `bundle::Pad`, `tools/pad-guest-test.py` | Done · a real controller on the key mapping, the USB pad on Win98 FE / Me |
| **M14** Voodoo 2 device | `tracks/m14-voodoo2.md` | `voodoo/`, patch 62 and the Voodoo patches after it, `tools/voodoo-guest-test.py`, `scripts/sync-86box-voodoo.sh`, doc 21 | Active on `main` · a second Glide game after one has quit, a client left on a dead ring, the Windows build; the Air done 2026-10-04 (Quake II 147.9 fps, UT 39.9, as on Linux) |
| **M15** Direct3D executor on Wine | `tracks/m15-wine-executor.md` | `d3dpt/exec/d3dpt_exec_host.c`, `d3dpt_exec_remote.c`, `d3dpt_remote.h`, the loader's library choice, `build-d3dpt-exec.sh --wine`, `player-core/src/companions.rs`, `launcher-core/src/host_gpu.rs` | Steps 1–7 done: the community app passed on a real macOS 15, WineD3D-in-guest was removed, and the Flatpak's Wine add-on `com._2ksbox.Launcher.Wine` was built and checked in the sandbox (2026-09-23) · a game through the add-on on a below-floor host; the spike's host tests on the rig's Linux Wine |
| **M16** DirectX 9 driver, no custom DLLs | `tracks/m16-dx9-ddi.md` | the DX9 DDI in `guest-tools/src/d3dptvid/` (with M10 / M7's files), its decoder cases in `d3dpt/exec/d3dpt_exec_ddi.cpp`, the Wine test suites' build, runner and baselines, doc 15's DX9 section | Closed 2026-09-27 (user), steps 0–7: XP's and Win98's own `d3d9.dll` / `d3d8.dll` run on the driver with SM3 (protocol v21), Wine's d3d8 / d3d9 suites run whole on both against the rig's baselines, the DXVK patches (`patches/dxvk/10`-`14`) landed, the Direct3D DLLs and the SysBus `-device d3dpt` are gone; Max Payne 2, Vice City, 3DMark2001 SE and Crimson Skies play on Win98 · left open: the OpenGL ICD (step 8; `OPENGL32.DLL` stays per game, user: not worth it for now), vertex texture fetch in a title |
| **M17** the driver's speed, on Max Payne 2 | `tracks/m17-driver-perf.md` | `tools/w98-mp2.sh`, `tools/ddi-rate.py`, `tools/guest-code-owner.py`; the executor's and the 9x HAL's hot paths (with M7 / M10's files) | Closed 2026-09-27 (user). The vCPU is the limit; ours is ~12 % of it. A texel mean computed on every texture bind for a trace line (7.3 % of the vCPU) is gone, and a flush hint lets the GPU start before the readback (+4.7 %, within one run) · left: host vertex buffers for the draws, one HAL walk instead of two, the readback |
| **M18** Windows 7, and Aero on it | `tracks/m18-win7.md` | the NT driver's Windows 7 paths (with M7's files), `guest-tools/src/d3dptvid/wddm/`, `guest-tools/build-wddm.cmd`, `guest-tools/src/setup.manifest`, `tools/win7-aero-test.sh`, the Windows 7 half of `tools/xp-driver-test.sh` | Steps 1 and 2 on `main`; each stretch is a `track/m18-*` branch, merged and deleted. The XP-model driver runs on 32-bit Windows 7, and the WDDM driver (kernel + Direct3D 9 user-mode driver; with `irq=on`, register sets v6 to v8 put the vertical blank and fences on the device's interrupt and keep the pointer on under DWM) gives Aero, with every test program exact under composition. SETUP installs it and turns Aero on (`CompositionPolicy=2`); the launcher has a Windows 7 family; a fresh install reaches Aero with SETUP and one restart (`win7-aero` check, `WIN7_ISO`). Built on the PC with the EWDK 10.0.19041 (`build-windows.sh wddm`, `--publish`); Linux and macOS fetch it (`scripts/wddm-prebuilt.sh`) · step 6's loose ends, timeout recovery |
| **M19** the launcher on mitsuami | `tracks/m19-mitsuami-launcher.md` | `launcher-mitsuami/`, ADR-023 | The only launcher since 2026-10-02 (AppKit, WinUI 3, GTK 4, Kirigami with `kde`); every packager ships it as `2ksbox` and checks its window (`LAUNCHER_SHOT`); mitsuami pinned at 1.0.0 (`0e21f20`); the Flatpak's KDE build is the add-on `com._2ksbox.Launcher.KDE` · the Linux packager's first run with it, the KDE add-on in a real Plasma session, the macOS floor with the AppKit launcher |
| **M20** Windows 11 | `tracks/m20-win11.md` | patches 75–77 and `tpm/qemu/` (the libtpms TPM backend), libcrypto and libtpms in `build-deps.sh`, the x86_64 and aarch64 players (`--features qemu-x86_64` / `qemu-aarch64`), `scripts/build-edk2.sh` and `patches/edk2/`, `scripts/build-virtio-win.sh`, `Family::Win11` and `bundle::Arch` in `launcher-core` (with M6), `tools/tpm-qtest.py`, `tools/win11-spike.py` | On `main` (ADR-024). Stock Windows 11 installs with no bypass, its TPM 2.0 in QEMU's process (`-tpmdev libtpms`); x64 under KVM on Linux through `2ksbox-player-x86_64`, Windows 11 on Arm under HVF on the Air (`virt` board, our EDK2, virtio-win's ARM64 drivers disc, viogpudo once installed). The Linux tarball and Flatpak ship the x64 player, the macOS app Windows 11 on Arm (hypervisor entitlement); the Windows zip and MSIX carry the x64 player too (the packager boots it to its BIOS) · Windows hosts (QEMU 11.1 builds no TPM there; the launcher says so), the App Store review of that entitlement |
| **M21** QEMU 9.2 to 11.1 | `tracks/m21-qemu-upgrade.md` | the `qemu` submodule pin, `scripts/prepare-qemu.sh`, `scripts/configure-qemu.sh`, `patches/qemu/`, the qemu-3dfx port | Merged to `main` 2026-10-02: QEMU 11.1.2, qemu-3dfx's OpenGL half ported (`patches/qemu-3dfx/`), every TCG patch ported and bit-exact, game A/Bs at or above 9.2. Step 4: Windows (WHPX back for era machines, patch 84) and the Mac (patch 82: Windows 11 on Arm under HVF; App Store, community and Intel apps pass) done; old bundles keep `pc-i440fx-9.2` · step 4's Linux package (the Flatpak passed 2026-10-04, M19), doc 22's numbers, the user's hand test |
| **M22** the player on mitsuami | `tracks/m22-mitsuami-player.md` | `player-core/` (with M2's and M13's files in it), `player-mitsuami/`, `tools/player-mitsuami-test.sh`, ADR-025 | On `main`. Everything but the window is `player-core`; `player/` (winit) and `player-mitsuami/` drive it. The default player in a checkout (`launcher_core::player`); the Windows zip and MSIX ship it as `2ksbox-player.exe` (MSVC, Direct3D 12 present), the Linux and macOS packages still ship winit. Checked headless on sway and by hand on Windows · step 2, the pointer, menus and close alert on a real desktop (and mitsuami's 5 px offset when sway tiles the window), then X11 / KDE, macOS, the flip |
| **M23** shared folders and the clipboard | `tracks/m23-integration.md` | `libsmb/`, `guest-agent/`, `player-core/src/share.rs`, `player-core/src/clipboard.rs`, patch 79 (`guestfwd=…-unix:`), doc 24, ADR-027 | Windows 11 on Arm (Mac) and x64 (Linux): a host folder through `libsmb` in the player (SMB 3.1.1 / 2.1, leases, `srvsvc`), the clipboard through `qemu-vdagent` (embed API v11) and our `guest-agent`; the drivers disc's `2ksbox\install.cmd` sets up the guest; the launcher's Clipboard and Shared folder settings; checks `smb`, `sharing`, `tools/smb-win11-test.sh`, `tools/clipboard-win11-test.sh` · Windows hosts (step 7) |
| Everything else (M2's leftovers) | "Next steps" below | | as listed |

Rules: work on `main` or on a branch `track/<name>-<topic>` off it,
rebased on `main` before pushing and merged when green. Edit shared files
(`d3dpt/d3dpt_proto.h`, `d3dpt/exec/`, `scripts/test.sh`, `player/`,
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
| QEMU | v11.1.2 + our port of qemu-3dfx's OpenGL half (`patches/qemu-3dfx/`, from `d00e858`) + our patches 00–84 (`patches/qemu/README.md`; track M21). Built without display, host-audio, extra network or network-block backends (the `no-optionals` check). On Linux it links a GLib of its own, static and hidden (`scripts/build-deps.sh`, `QEMU_DEPS` in `development.md`), so a toolkit on the process's GLib never meets QEMU's main loop; the Mac build needs no XQuartz (patch 70). Windows has two builds: mingw clang (`build/win/qemu`, for `test.sh` and the winit player) and MSVC (`build-windows.sh qemu-msvc`, patch 83, ADR-026's third amendment, `build-windows.md` "QEMU under MSVC"), which the package ships with no runtime DLL (`package-windows.sh` checks). MSVC's ABI makes every enum an `int`, so its build fails on any enum it would miscompile (`QEMU_ENUM_UNSIGNED`). WHPX serves era machines again since patch 84. |
| Emulated CPU (TCG) | x87 shadows at PC=24/53/64 (doc 13), SSE and SIMD inline (doc 16), the M9 queue (REP, same-value SMC, soft immediates, inline TB lookup, TLB work). Default was 2.34x geomean over pristine 9.2.4 on the Air (measured before M21), all switches off 0.97x (doc 22). Every patch has an off switch in the machine form. Pinned guest registers (patch 21, doc 18) crashed XP, were never offered, and were not ported to 11.1 (user decision, 2026-10-01). The hardware-MMU design (1.1–1.2x) is parked (user decision, 2026-09-16). |
| Player | QEMU in-process (`libqemu-embed`, embed API v11), wgpu + librashader CRT chain, mode analysis (doc 03), the embed audiodev (f32, paced to the guest's clock, limiter; doc 11), guest cursor as the window cursor, gamepads, host modifier keys, the host's shortcuts to the guest on all three hosts (macOS since 2026-09-24: the window server's hot keys off while focused, the private call UTM ships on the App Store, doc 03 "Input path"), Alt+F4 and Cmd+Q ask. Ctrl+Alt+S shoots the guest's frame, Ctrl+Alt+Shift+S the window's (scaled and shaded). Options: `docs/development.md`. Everything but the window is `player-core` (M22, 2026-10-01), so a second front end, `player-mitsuami` (ADR-025), runs the same machine: the default player in a checkout (the launcher starts it when built), and the one the Windows package ships; Linux and macOS packages still ship the winit player. |
| OpenGL pass-through | In the player on Linux (EGL), macOS (CGL) and Windows (WGL, doc 12 "The WGL rule"). Zero-copy: dma-buf ring on Linux (repairs a slot that stops being written through), IOSurface on macOS. The Glide pass-through was removed on 2026-09-23 (ADR-020): Glide is the Voodoo 2's. |
| Voodoo 2 (doc 21) | `-device voodoo2`, 86Box's chip, the machine's only Glide (ADR-020). 3dfx's own Win98 driver runs Quake II, UT and NFS Porsche (the user, by hand). FIFA 2000's and Carmageddon's FIFO hangs fixed (doc 21 §11, §13). 8 MB board by default (`texmem=2`), command FIFO in guest RAM (`ramfifo=on`, Quake II 41 → 147.5 fps; 147.9 on the Air, where the ARM64 JIT is 2.07× the interpreter). Open: a second game after one quits sometimes starts glitched. |
| Direct3D executor (doc 14) | Protocol v22, one decoder, four D3D9s. DXVK (native and on Windows) is the default and the golden reference. Below the Vulkan 1.3 floor: Windows' own `d3d9.dll` (`D3DPT_D3D9`, ADR-007's second amendment) or Wine's on a Linux / macOS host (`exec=wine`, ADR-018, M15). `no-exec=on` models a host with no executor. On Windows the executor and DXVK are MSVC since 2026-10-04 (ADR-026's second amendment), DXVK with our patch 15 (`build-windows.md` "DXVK under MSVC"); the same day the tools (`launcherx`, `discx`, `synthx`, `wgl-probe`) moved too, leaving only QEMU, the winit test player and the guest code on mingw. |
| XP display driver (doc 15) | `d3dpt-vga`, register set v8 (v6 to v8 add the interrupt, the fences and the cursor ownership the Windows 7 WDDM driver uses, M18). DirectDraw, a DirectX 9 DDI with shader model 3.0 to `d3d9.dll` (M16: the face, the tokens, the DX9 formats, mip generation, instancing, four render targets) and a DirectX 8 DDI with hardware T&L, shaders 1.x, palettes, colour keys, VRAM buffers, 16 streams, cube / volume textures, MSAA, gamma. FIFA 2000, Max Payne, Vice City, Moto Racer and Diablo play. |
| Win98 display driver (doc 19) | The same core under a 9x layer; the default adapter for a new Win98 machine. The DirectX 3–8 checks pass as on XP, and a 2ksbox Win98 runs DirectX 9.0c. Crimson Skies, 3DMark 99 / 2001 SE, Carmageddon (Mode X), Blood in a DOS box. Blue screens and power-down show. |
| CD-ROM (docs 05, 17) | `libdisc` behind the `cdimage` driver: cue/bin, CCD, MDS, ISO and `isodir:` folders, L-EC, subchannel, CD-DA, a DVD profile past 80 minutes, the disc shelf from inside the guest (patch 52, `CDSHELF`; the listing names the disc in the drive from the medium itself, boot disc included; on XP it leaves the medium-change report to Windows and never dismounts, so AutoPlay and Explorer see each swap, 2026-10-04). SafeDisc 1.x's band read is the negative control; SafeDisc 2.x and ProtectCD never read theirs. |
| Music (doc 20) | OPL3 and MPU-401 (no IRQ line) on SoundFont GM or the user's MT-32 ROMs. The SB16 applies its mixer (patch 61). Open: Win98's own MIDI through our port loses instruments. |
| Gamepads (M13) | USB HID pad (patch 26), gameport (patch 27), key mapping. Done. |
| Guest machines (doc 06) | Six families: Windows 98, Windows XP, Windows 7, Windows 11, DOS, Other. Win98 / XP / Win7 start on `d3dpt-vga` (Win7 with `irq=on`), Windows 11 on `std` (`q35` on x64, `virt` with ramfb / viogpudo on Arm), DOS / Other on `std`. No network card by default. Win98 is TCG with `hpet=off`; the BIOS date stamp makes it install ACPI. DOS paces with `-icount …,align=on`; every adapter has QEMU's `retrace=precise`, so a wait-for-retrace loop really waits (2026-09-24). |
| Guest tools (`guest-tools/README.md`) | One ISO. `SETUP.EXE` installs what this Windows can use; every program logs to `C:\2KSBOX` (`BOXLOG=` overrides). |
| Launcher (doc 07) | `launcher-mitsuami` over `launcher-core` (also `launcherx`, `launcher-capi`) is the only launcher since 2026-10-02 (user; ADR-023, M19): every package ships it, and the Qt one is deleted. Its library is a machine list beside the chosen machine's details, as UTM and VirtualBox (2026-10-01), and its disc shelf is one drive card over the library, Insert and Eject setting the boot disc and swapping a running machine's tray, the card reading the tray back (2026-10-01, doc 07 "One drive"). The family picker offers Windows 98, Windows XP, Windows 7, Windows 11, DOS, Other (labels and order, user 2026-10-04). The machine form is a settings window, a page per section, every picker the style's own combo box over the model's rows; the Direct3D picker shows only what this host runs. Extra QEMU arguments, clone (a copy of the disk, or the same disk with a one-at-a-time warning, 2026-09-26), first-run preset download; the profile library has a default profile that every machine on "(default)" plays through, CRT Aperture from the first download (2026-09-24). The snapshot window is a tree (2026-09-23): a qcow2 records no parent, so the launcher writes each take and restore to `snapshots.toml` beside the bundle and reconciles it against the disk on every read; snapshots it has no record of sit at the top level; its header and rows share one set of column widths, so they line up at any window size (user report 2026-09-23). Window text is short and plain (user rule). An About window (2026-10-02) thanks the projects 2ksbox is built on (`launcher_core::about`, `launcherx --about`): in mitsuami from the toolbar's "i" (an info icon on every toolkit, user), and on macOS only from the application menu (user). |
| Packages | All ship `launcher-mitsuami` as `2ksbox` since 2026-10-02 (ADR-023). Linux tarball (the host's GTK 4), Flatpak (`org.gnome.Platform` 49; add-ons: Wine, and since 2026-10-04 KDE, the Kirigami launcher for a Plasma session), macOS app in two builds (bundle IDs `com.2ksbox.2ksbox` and `com.2ksbox.2ksbox-community`, the registered App IDs, ADR-011; the App Store one as a sandboxed, signed `.pkg` for upload since 2026-10-06, `package-macos.sh --app-store`, not yet run through TestFlight, `build-macos.md` "The App Store package"; picks kept across launches as security-scoped bookmarks, `launcher_core::grants`; ADR-019: App Store 26+, community with the Wine pair down to macOS 12; nothing in either from Homebrew since 2026-09-23, `scripts/build-deps.sh` builds QEMU's libraries from source, `build-macos.md` "The libraries"; the community app for Intel Macs builds and packages on the Air under Rosetta since 2026-09-24, `scripts/build.sh --x86_64` + `package-macos.sh --x86_64`, untested on an Intel Mac, "The Intel build"; on Apple Silicon both carry Windows 11 on Arm since 2026-10-04: `2ksbox-player-aarch64` with the hypervisor entitlement, our EDK2 and the ARM64 drivers disc, the packager booting the staged machine's firmware under HVF), Windows zip (built, rolled and checked natively on the PC in MSYS2 since the cross build from Linux was retired on 2026-10-03, ADR-026; it needs the Windows App Runtime 2.4+) and, from 2026-09-23, the same tree as a Microsoft Store MSIX (`scripts/package-msix.sh`, packed on the PC; passes the certification kit; not yet uploaded: the submission's steps, text and privacy policy are written, `build-windows.md` "The Store package", `docs/privacy.md`; the listing and the submission steps moved to the store repo on 2026-10-04, user). Every packager opens the launcher's real window (`LAUNCHER_SHOT`). The Windows checks fail on anything they write into the staged tree (2026-10-02: a DXVK log and NVIDIA's `umdlogs` had shipped in the zip), and the launcher sends DXVK's log to the data directory, not the working one. |
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

# a machine the way the launcher runs it
target/release/launcherx --print-args <machine>/machine.toml   # the exact QEMU line
target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
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
  same on a QEMU built without patch 56) and `exec-no-device`. (`package`
  failed there too, on `build/` being a symlink, until `package-linux.sh`
  took its stage's real path on 2026-10-04.) The Mac's host stage is green.

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
   controls (pause, snapshot, disc swap; doc 07).
5. **Windows host (M11's leftovers).** Moto Racer's speed on the PC
   (CPU-bound, not reproduced on Linux; M11 track doc). Live control
   over Winsock AF_UNIX on a real PC. The Store upload: the package
   installs through `scripts/win-sideload.ps1` and passes the
   certification kit, and the listing, privacy policy and steps are in
   the store repo; the user's part is the Partner Center account, the
   name reservation, a version of 1.0.0 or later (the Store refuses a
   first number of 0) and screenshots of the player's window with games
   in it (never the guest-frame shot scaled up). An installer for users
   outside the Store. Zero-copy frames through a DXGI shared handle.
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
9. **M6, launcher and packages.** An AppImage (6b′), the Windows
    installer (6d; the MSIX covers the Store, above), screenshots for a
    Flathub submission, `CDSHELF.EXE`'s Win98 (ASPI) run.
9a. **M19, the launcher on mitsuami.** The flip is done (2026-10-02);
    mitsuami pinned at 1.0.0 (`0e21f20`) since 2026-10-04; left: run
    the Linux packager with it (the Windows, macOS and Flatpak ones
    have), and the macOS floor with an AppKit launcher
    (`tracks/m19-mitsuami-launcher.md`).
9b. **M22, the player on mitsuami.** Step 2: the pointer, the menus and
    the close alert on a real desktop, latency against the winit player,
    and mitsuami's tiled-window offset (`tracks/m22-mitsuami-player.md`).
10. **M5, CD-ROM.** Triage FIFA 2002's no-match. Age of Mythology disc 1
    as a second SafeDisc 2 title. SecuROM (needs DPM in `mds.rs`).
    Multisession. CHD. Win98's CD Player by ear. M5g: a guest-side check
    of the stale-file rule.
11. **The finished tracks' leftovers.** M4: a decoder thread for the executor. M7: a shader title, a
    split-stream title, 3DMark2001's Nature for cubes, StarCraft / Age of
    Empires on 8 bpp, a driver stage in `scripts/test.sh`.
    M8: a Direct3D title with and without `*-fast=off`. M9: the Air's game
    tests uncapped (`DDFLAGS=32768`), binary32 at PC=24 on aarch64 (0.49 s
    against PC=53's 0.38 s on the Air, not profiled). M16: the OpenGL ICD
    (step 8, left open by user decision), vertex texture fetch in a title.
    M17: host vertex buffers for the draws, one HAL walk instead of two,
    the readback's wait (`tracks/m17-driver-perf.md`, `tools/w98-mp2.sh`).

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
  `build/win/qemu`) are separate; the sources are not.
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

### The player and the QEMU thread

- **Never `exit()` while the QEMU thread is alive.** QEMU's atexit
  handlers race `qemu_cleanup` (`mutex->initialized` on macOS). The
  player joins the thread; headless paths use `_exit`. A guest power-off
  ends the loop while the UI still holds the handle, hence the stop /
  release handshake before `qemu_embed_destroy`.
- **An occluded window gets no swapchain image.** Per-frame work that
  must not stall (importing zero-copy slots) runs on the wake event.
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

### Devices and corruption

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
