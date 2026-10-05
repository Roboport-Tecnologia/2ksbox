# Track M15: the Direct3D executor on Wine, on the host

The Direct3D fallback for a Linux or macOS host below DXVK's Vulkan 1.3
floor. The same paravirtual device and executor run on **Wine's d3d9
(WineD3D over OpenGL) in a Wine process on the host**, instead of a 2015
Wine copied into the guest beside every game (user, 2026-09-22: "running
an old unsupported Wine version in the guest is bad UX and doesn't make
sense"). ADR-018 is the decision and its rejected alternatives; doc 14
"The executor on Wine, in another process" is the design (the child, the
wire, the shared file). A Windows host uses its own d3d9 in process
instead (ADR-007's second amendment).

## Scope and files

- The child: `d3dpt/exec/d3dpt_exec_host.c` → `d3dpt-exec-host.exe`, a
  PE program that loads the Windows build of `d3dpt_exec.dll`.
- QEMU's side: `d3dpt/exec/d3dpt_exec_remote.c` →
  `libd3dpt_exec_remote`, the wire `d3dpt/exec/d3dpt_remote.h`, and the
  back-end choice in `d3dpt/hw/d3dpt_exec_load.[ch]`.
- The shared file behind device memory: `d3dpt/hw/d3dpt_vga.c` (VRAM,
  QEMU patch 73). The SysBus window's `d3dpt_mm.c` went with M16 step 7.
- Build and packages: `scripts/build-d3dpt-exec.sh --wine` (the PE pair
  into `build/d3dpt/wine/`), `scripts/build.sh`, `package-linux.sh`,
  `package-macos.sh --community`, the Flatpak manifest.
- Launcher: `launcher-core/src/host_gpu.rs` (`D3dBackend::Wine`, the
  third verdict), `wizard::Form::d3d9_note()`, and
  `player-core/src/companions.rs` (`wine`, `wine-host`, `d3dpt-remote`).
- Tests: the `exec-wine` check, the `EXEC=` knob of
  `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` and
  `tools/tcg-profile.sh`, and `tools/macvm-wine-spike.sh` /
  `tools/macos-wine-spike-local.sh`.
- Shared, not edited: `d3dpt/exec/d3dpt_exec.cpp` needs nothing for Wine
  (the `system` branch's hidden window and executor-owned scene are what
  wined3d wants); `d3dpt/d3dpt_proto.h`.
- **Removed in step 6** (2026-09-23): all of WineD3D-in-guest (the
  wine9x build in `guest-tools/build-wrappers.sh`, `patches/wine9x/`,
  the ISO's `WINED3D\` folders, `SETUP /GAME 4`/`5` and `/I 7`,
  `guest-tools/src/d3dpre.c`, `tools/xp-wined3d-test.sh`,
  `tools/wined3d-sys-test.sh`). A below-floor host with no Wine has no
  Direct3D pass-through.

## State

Built and working end to end on the M1 Air, and accepted (step 5) on a
real macOS 15: the user ran the packaged community app there on
2026-09-23 and reported that it "works wonderfully". No frame rate was
written down for that run. Step 6, the removal of WineD3D-in-guest,
landed the same day. Measured numbers are in doc 14 under the Wine
section.

- **Host tests.** `d3dpt-dp2-test` and `d3dpt-exec-test` through the
  child are byte-identical to in-process DXVK, under Rosetta (WineHQ
  11.17) and on a real macOS 15.8 on the M1's GL (328 fps,
  user-confirmed). The `exec-wine` check holds
  `d3dpt-dp2-test`'s half (`d3dpt-exec-test` left with the guest DLLs'
  records, M16 step 7).
- **XP guest.** D3DGAME8 within the rig budget (600 frames in 3.4 s
  against 2.1 s in process); the ten DX8 DDI probes give the in-process
  verdicts; FIFA 2000 plays a match at 22.6 frames/s against 19.0.
- **Win98 guest.** D3D7TEST through the 9x HAL, 300 frames at 57 fps,
  byte-identical to `dp2-test.bmp`.
- **Launcher and packages.** The third verdict works, `--host-check`
  exits 0 through Wine, and `--paths` / `--companions` list the Wine and
  the pair. The Linux package and the macOS community build carry
  `libd3dpt_exec_remote` + the PE pair (all or none). No package ships a
  Wine; the note says which to install.
- **The community app on a real macOS 15** boots the XP machine to its
  desktop on the Wine executor through the packaged pair (`linear mode
  on (800x600x16)` at 25 s, `Direct3D executor in another process
  (Wine), ready`), after two fixes: the realize crash (Traps) and the
  launcher probing no Vulkan (`docs/build-macos.md` "The app").
  `build/macos-community/` is rebuilt with both.

## Rejected alternatives

The first three are ADR-018's; listed so nobody re-proposes them.

- **wined3d in process (winelib).** `d3d9.dll` and `wined3d.dll` are PE
  modules needing Wine's loader and a `wineserver`; a winelib binary *is*
  a Wine process.
- **An executor of our own over GL, Metal or wgpu.** A second D3D9
  implementation, which WineD3D already is.
- **Native arm64 Wine on macOS.** `winemac.drv` gets no OpenGL there.
  Revisit when Wine's Mac driver has Metal-backed GL.
- **Copying VRAM over the pipe.** The DDI path reads vertex buffers out
  of VRAM per draw and writes a readback per frame, so a copy is a round
  trip per draw.
- **A socket instead of stdio.** The child's two pipes are enough, and
  Wine's AF_UNIX support is recent.

## Test loop

The knobs (`D3DPT_EXEC`, the adapter's `exec=`, `D3DPT_WINE`,
`D3DPT_WINEPREFIX`, `D3DPT_EXEC_REMOTE_LIB`, `D3DPT_EXEC_HOST`,
`D3DPT_WINE_RENDERER`, `D3DPT_REMOTE_DIR`) are in `docs/development.md`,
the tools in `docs/testing.md`. On a host with Vulkan, `exec=wine` is the
A/B.

```sh
scripts/build.sh                  # the native stack, libd3dpt_exec_remote, and with mingw the PE pair
scripts/test.sh host              # exec-wine (SKIPs without a Wine or the pair)
build/qemu/qemu-img create -f qcow2 -b ~/vms/winxp-m7.qcow2 -F qcow2 build/xp.qcow2   # never the image
tools/xp-driver-test.sh build/xp.qcow2 install                 # once: the image's driver predates 2026-09-12
EXEC=wine tools/xp-driver-test.sh build/xp.qcow2 d3dgame8      # the reference scene on Wine
EXEC=dxvk tools/xp-driver-test.sh build/xp.qcow2 d3dgame8      # the control
EXEC=wine tools/xp-driver-test.sh build/xp.qcow2 probes        # the ten DX8 DDI probes
EXEC=wine tools/xp-fifa-match.sh tcg build/xp.qcow2            # a real game
# the Win98 HAL: base98-us has a Voodoo 2, in the launcher's slot (addr=0x05)
STAGE=guest-tools/out/driver9x/d3d7test.exe PULL='2KSBOX\D3D7TEST.LOG 2KSBOX\D3D7TEST.BMP' \
  GUEST_CMD=$'start /w C:\\D3D7TEST.EXE 640 480 32 300' \
  EXTRA='-device voodoo2,addr=0x05 -global voodoo2.texmem=2 -global d3dpt-vga.exec=wine' \
  tools/win98-game-test.sh "<machines dir>/base98-us/disk.qcow2" d3d7wine
python3 tools/bmpdiff.py build/test/dp2-test.bmp build/w98game/d3d7wine/D3D7TEST.BMP --tolerance 8
```

**A pre-26 macOS** (the community build's user, ADR-019) cannot be
tested in a VM: Wine has no OpenGL there (`docs/build-macos.md` "The
app"; `tools/macvm-wine-spike.sh` shows it). The real one is a second
APFS volume on the Air with macOS 15. Boot into it and run
`tools/macos-wine-spike-local.sh` from the other volume's checkout in a
Terminal. A bare macOS 15 needs Rosetta (`softwareupdate
--install-rosetta --agree-to-license`); its Python is the Command Line
Tools stub, so the script falls back to uv's.

**Which Wine.** Linux: the distro's (`wine`/`wine64` on `PATH`). Mac:
WineHQ's tarball from Gcenx's releases, unpacked into `build/wine/` or
installed in `/Applications` (`docs/build-macos.md` "The app" has the
search order).

## Traps

- **Run `install` on an XP overlay first.** `winxp-m7.qcow2`'s driver
  (2026-09-08) predates the any-newer-version rule and refuses the
  adapter. XP falls back to VGA, D3DGAME8 dies in a modal box, and the
  killed guest loses its unflushed logs. The mode-set line after the
  restart proves the install.
- **XP with its display driver refused showed a black screen** on the
  adapter (the VGA core in a chained 256-colour 800×600 mode, `sr4=0a
  gr5=50`, drawing nothing). That was patch 44's retired TLB table
  revived across the VGA window's topology flush, fixed 2026-09-24; the
  inbox driver's VESA desktop renders now. COM1 is still the record of
  an install that stalls.
- **wined3d's first batch stalls the vCPU** while it compiles shaders,
  so the child makes its device at probe time (doc 14). A 3.5 s stall
  inside an MMIO read reset the Win98 machine, and let patch 65 raise a
  quarter second of owed PIT ticks back to back into a VMM fault; the
  patch now paces the catch-up (`patches/qemu/README.md`, 65).
- **Carry `base98-us`'s Voodoo 2 in the launcher's slot** in a harness
  run. Without it 3dfx's login helper opens an "Error" box that takes
  exclusive mode (`DDERR_NOEXCLUSIVEMODE` at the first `BeginScene`); in
  another slot Windows finds new hardware and wants a restart.
- **Moto Racer is no Direct3D test on these images.** `winxp-m7`'s copy
  runs its software renderer, and on `base98-us` it takes the Voodoo 2.
- **QEMU must survive DXVK's probe on a host with no Vulkan** (doc 14
  "DXVK": ask for `libvulkan` first, never close a probed library) **and
  on a loader with no working device.** On macOS 15 KosmicKrisp reports
  no GPU, DXVK's constructor throws, and a second `Direct3DCreate9` on
  the same library faulted. DXVK patch 09 (`patches/dxvk/README.md`) and
  the executor's never-ask-twice rule fix it; `exec-no-device` checks
  the artefacts.
- **The shared-file path is POSIX-only** (doc 14, "The shared file").
  A future Windows out-of-process executor needs another mapping call.
- An overlay's absolute backing path does not exist on the other macOS
  volume. Flatten the image (`qemu-img convert`) for a run there.
- Homebrew's mingw links `libwinpthread-1.dll` dynamically, so the pair
  needs it beside the `.exe`.

## Steps

Numbered as ADR-018, doc 07 and CLAUDE.md cite them.

1. **The spike.** Done. Linux Wine on a real GL (the rig) is one
   `scripts/test.sh host` run away (`exec-wine`).
2. **The transport.** Done (doc 14). If a guest profile ever shows the
   pipe round trip, the answer is a bounded spin on a sequence word in
   the shared file before blocking.
3. **The guest.** Done on XP and Win98. Open: who programs the XP
   black-screen VGA mode (Traps) and why it draws nothing.
4. **The launcher and the packages.** Done (doc 07 has the verdicts).
5. **A real game on a below-floor host.** The acceptance, and what is
   left. The community app is built on the 26 side (`scripts/package-macos.sh
   --community --no-sign --no-dmg --out build/macos-community`;
   `docs/build-macos.md` has what it needs) with
   `build/xp-mac15.qcow2` (the XP overlay, flattened). Booted into
   macOS 15, in a Terminal:

   ```sh
   cd "/Volumes/Macintosh HD - Data/Users/david/work/win-98-xp-virt"
   cp -R "build/wine/Wine Staging.app" /Applications/   # once
   build/macos-community/2ksbox.app/Contents/MacOS/2ksbox --host-check
   #   "runs through Wine on this host (Wine 11.17, OpenGL)", exit 0: KosmicKrisp
   #   loads below 26 but reports no GPU, so DXVK finds no device and the third verdict is the one
   open build/macos-community/2ksbox.app
   #   an XP machine on a copy of build/xp-mac15.qcow2, Direct3D "auto";
   #   FIFA 2000 into a match
   ```

   It passes when the QEMU log says `exec: Direct3D executor in another
   process (Wine), ready`, the match draws on the M1's GL through the
   packaged pair, and its frame rate is written down against the Air's
   22.6. The reboot is the user's. **Passed on 2026-09-23**: the user
   ran the packaged app on the 15 volume and reported it "works
   wonderfully". The frame rate of that run was not written down; if it
   is ever wanted, it is the harness's count (`tools/tcg-fps.py`) on
   that volume against the Air's 22.6.
6. **Retire WineD3D-in-guest, in one commit.** Done on 2026-09-23: the
   ISO's `WINED3D\` folders and README, `SETUP /GAME 4`/`5` (the Glide
   sets took those numbers, then left with ADR-020), `/I 7` with `D3DPRE.EXE` and the
   `DDRAWME`/`DDSYS` switcher, the wine9x build and `patches/wine9x/`,
   `tools/xp-wined3d-test.sh` and `wined3d-sys-test.sh`, the launcher's
   WineD3D advice (a host with none is told to install Wine), doc 04's
   row, doc 19 §42–44 and CLAUDE.md's sentence. The 9x driver's
   `D3DPT_ESC_HOSTINFO` escape, which only the helper asked, stays as a
   diagnostic.
7. **The Flatpak's Wine add-on** (user decision, 2026-09-23: "go with
   the extension"). Done the same day, on `track/m15-flatpak-wine`.
   Flathub refuses a second listing of the same app, so there is no DXVK
   app beside a Wine app; and inside the sandbox the app cannot run the
   host's Wine (`flatpak-spawn --host` is a sandbox escape reviewers
   refuse), so the manifest's "the host's own Wine" never held there.
   The Wine is an **extension** of the app, which Flathub lists on the
   app's page as an add-on:
   - `com._2ksbox.Launcher.Wine`, declared in the app manifest under
     `add-extensions` at `lib/2ksbox/wine` (`no-autodownload`,
     `autodelete`; `version` unset, so it follows the app's branch), the
     directory the tarball uses for the PE pair. The app ships the
     directory empty, and stages the remote library itself, since the
     tarball's stager only stages it together with the pair.
   - `packaging/flatpak/com._2ksbox.Launcher.Wine.yml`: `build-extension`
     with the app as its runtime (the Flathub add-on shape), on
     `org.kde.Sdk` 6.10 then (`org.gnome.Sdk` 49 since M19 moved the app
     to GNOME's runtime) plus `org.freedesktop.Sdk.Extension.mingw-w64`
     25.08. It builds Wine 11.0 from source, 64-bit only, modelled on
     Flathub's `org.winehq.Wine` `stable-25.08` and trimmed (no Gecko or
     Mono, no sound, printing, scanners, cameras, USB, GStreamer or
     network APIs), and the pair with `scripts/build-d3dpt-exec.sh
     --wine` from `d3dpt/` and the script alone. 175 MB installed; the
     app and the add-on build in about half an hour together on the
     Ryzen. Its AppStream file is an `addon` component that `extends`
     the app. No binary is checked in.
   - Both manifests take their branch from the builder:
     `package-flatpak.sh` passes `--default-branch=stable`, Flathub's, so
     the add-on's `runtime-version: stable` resolves locally too, and
     every ref in the script names the branch. `--no-wine` skips the
     add-on, `--wine-only` rebuilds it onto the installed app.
   - Finding it: `host_gpu.rs`'s `find_wine` tries
     `paths::shipped("lib/2ksbox/wine/bin/wine")` right after
     `D3DPT_WINE`; the player's `companions.rs` exports that path as
     `D3DPT_WINE` (a new `wine` line in `--companions`); the C loader's
     `find_wine` looks for `wine/bin/wine` beside the library through the
     same `dladdr` helper `find_host_exe` uses. Inside a sandbox
     (`/.flatpak-info`) `wine_install_hint()` names the add-on.
   - The check (`package-flatpak.sh`): with the add-on installed, `wine`
     and `wine-host` in `--paths` must be under the mount point,
     `d3dpt-remote` must be shipped, and the pair must start under the
     add-on's Wine in the sandbox (`d3dpt-exec-host.exe` with a shared
     file that does not exist prints `executor …, protocol 13` and exits
     4). Passed 2026-09-23 on this box.
   - Found on the way: `build-d3dpt-exec.sh` exited 1 on a host without
     mingw even when the pair was optional (a bare `[ … ] && exit 1` as
     its last line), which stopped the Flatpak's build. The worktree
     needed QEMU's four meson subprojects downloaded by hand (`meson
     subprojects download` in `qemu/`), which a normal `build.sh` does.
   - Still open: a game through the add-on on a below-floor host (this
     box has Vulkan 1.3, so the sandbox check cannot reach the child's
     device), and the Flathub submission itself, where the add-on is its
     own repo submitted after the app.

## Rules

- One decoder for every back end (DXVK native, DXVK on Windows, the
  Windows system d3d9, Wine). Nothing Wine-specific goes into
  `d3dpt_exec.cpp`; a WineD3D quirk is answered in the child or written
  down.
- Every claim about a frame comes from a BMP in `build/` diffed with
  `tools/bmpdiff.py`.
- One TCG guest at a time; end scripted Win98 runs with the ACPI power
  button (CLAUDE.md).
