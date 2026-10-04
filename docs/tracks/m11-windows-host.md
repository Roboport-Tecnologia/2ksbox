# Track M11: the Windows host (build, package, run)

Windows as a *host*: the stack built natively on a Windows PC into a
portable zip and a Store MSIX (it began as a cross build from Linux,
retired on 2026-10-03, ADR-026), and the Windows branches of shared
code. The track is done: the package runs on
the user's PC (Ryzen 9 5900X, RTX 3090, the `base98-br` image), 3D guests
included on both Direct3D backends and the OpenGL pass-through. This file
keeps scope, test loop, traps and open items. The design:

- `docs/build-windows.md`: the container, the stages, the DLL closure,
  the WinUI launcher (MSVC, built on the PC), `2ksbox-debug.bat`, the
  Direct3D 9 backends, WGL, WHPX, the native MSYS2 build.
- Patch 68 (clang-built QEMU) in `patches/qemu/README.md`.
- Doc 12 "The WGL rule"; ADR-007 and its second amendment for which
  Direct3D 9 the executor runs on; doc 03 "Input path" and
  `player/src/kbcapture.rs` for the keyboard capture.

## Scope and files

- Build: `scripts/build-windows.sh` (natively under MSYS2 MINGW64; the
  cross build from Linux, `win-cross.sh` and its container, was retired
  on 2026-10-03, ADR-026), the `--windows` mode of `scripts/configure-qemu.sh`,
  `scripts/configure-dxvk.sh` and `scripts/build-d3dpt-exec.sh` (DXVK and
  the executor MSVC since 2026-10-04, in `scripts/msvc-env.sh`'s
  environment), `scripts/cargo-msvc.sh` (the MSVC Rust tools),
  `scripts/win-run.sh`,
  `guest-tools/msys2-i686.sh`.
- Package: `scripts/package-windows.sh`; the Store's MSIX: `scripts/package-msix.sh`,
  `packaging/windows/AppxManifest.xml.in`, `packaging/windows/Assets/`,
  `scripts/win-sideload.ps1`; the submission: `packaging/windows/store-listing.md`,
  `docs/privacy.md`; the executables'
  manifest `packaging/windows/app.manifest` (with the icon, `win-icon.rs`).
- Windows branches of shared code: `embed/mglcntx_embed.c` (WGL) and
  `tools/wgl-probe.c`; `launcher-core/src/console.rs`, `fatal.rs`,
  `paths.rs`, `player.rs`, `wizard.rs`, `bundle.rs` (one-folder layout,
  WHPX naming), `control.rs` (live control over Winsock AF_UNIX);
  `player-core/src/qmp.rs`, `player/src/kbcapture.rs`;
  `d3dpt/hw/d3dpt_exec_load.c`,
  `d3dpt/exec/*.cpp`; `libdisc/src/bin/discx.rs`.
- Patches added: 68 (clang), 69 (mkvenv's `file://C:/…` wheels URL under
  Python 3.14); Windows hunks in 10, 13, 50.

## Test loop

On the PC, in MSYS2's MINGW64 shell (setup in `docs/build-windows.md`
"Building on Windows"):

```sh
scripts/build-windows.sh              # every stage
scripts/build-windows.sh rust         # one stage (stages are positional)
scripts/package-windows.sh            # the zip, checked by Windows itself
scripts/package-windows.sh --msix     # ... and the Store MSIX
scripts/win-run.sh launcher           # the launcher out of the checkout
GDB=1 scripts/win-run.sh player ...   # the [player] line from launcher.log
build/win/d3dpt-dp2-test.exe                  # with D3DPT_D3D9=dxvk and =system
```

When a package misbehaves, ask the user for the `2ksbox-debug.log` that
`2ksbox-debug.bat` writes; `docs/build-windows.md` "The package" says how
to read it. `PLAYER_KEYBOARD_LOG=1` puts the keyboard capture's decisions
in `player.log`. Both logs are in `%APPDATA%\2ksbox\data`, not
`%APPDATA%\2ksbox`.

## Traps

Detailed in `docs/build-windows.md`:

- An import-table closure misses DLLs loaded with `LoadLibrary`
  (sdl2-compat's SDL3, libepoxy's `libEGL`), so the package runs a strings
  pass ("The package").
- Fedora has no `mingw64-libslirp`; without it every machine dies with
  "network backend 'user' is not compiled into this binary" ("Why a
  container").
- `windows_subsystem = "windows"` loses the console for debug verbs and
  the player's output, and cmd does not wait for a windowed program ("The
  package").
- MSYS2's coreutils `link` shadows MSVC's `link.exe`, and an MSVC exe
  imports `vcruntime140.dll` unless linked `+crt-static` ("The
  launcher"). The Qt launcher's emutls and import-library traps went
  with it (2026-10-02).
- A COFF weak external is not an ELF weak definition, so
  `mglcntx_mingw.c` is split ("OpenGL for a Win98 guest").
- QEMU is built with clang because mingw GCC's emulated TLS made a VGA
  register read 2.3x Linux's (patch 68; `WIN_QEMU_CC=gcc` is the old
  build).
- DXVK and the executor change compiler together or not at all (DXVK's
  exceptions), and DXVK under MSVC reused wrong pipelines until patch 15:
  an `eq()` that only MSVC's `unordered_map` calls without a hash match
  ("DXVK under MSVC").

Kept here:

- **The emutls fix's limits.** A local `__once_proxy` fixes it;
  `-static-libstdc++`, `-C link-self-contained=no` and `-shared-libgcc`
  do not. MSYS2's GCC 16 exports no `std::__once_call`, so the proxy
  compiles only where `_GLIBCXX_NO_EXTERN_THREAD_LOCAL` is absent.
- **QMP `fd=` on Windows is a C-runtime descriptor**, not a `SOCKET`;
  `qemu_embed_socket_to_fd()` converts it (embed API v7, doc 11).
- **A low-level keyboard hook in the player is never called while the
  player is in front**, cause unknown. The player takes the Windows keys
  with raw input and `RIDEV_NOHOTKEYS` (doc 03 "Input path"). System
  hotkeys (Alt+Tab, Alt+F4, Ctrl+Alt+Del, Win+L) reach no program.

## What stayed open

1. **Moto Racer is slow on the 5900X with the CPU at 5 %** (one of 24
   threads, so CPU-bound), in the menus and the software-renderer race.
   Not reproduced on Linux (60 flips/s). The user's log has no
   software-race window, and its last session switches to 640x480 at
   **8 bits** twice with no page flips, where Linux ran 16 bits and
   flipped. That, and timing the clang-built QEMU there, are the next
   questions for the PC.
2. **Live control on the PC.** Winsock AF_UNIX in
   `launcher-core/src/control.rs` is written but has not run, since wine
   has no AF_UNIX (`socket()` answers 10047). Start a machine, take a
   snapshot, swap a disc; `live control off: …` in `launcher.log` means
   the trial bind failed.
3. **The Windows-built guest-tools ISO in a guest.** Half done
   (2026-10-03): `test.sh all` on the PC installs the XP display driver
   from an ISO built natively there and runs Direct3D 9 and 8 scenes
   through it (`guest-G9`, `-G8`, `-F9`, `guest-ddvm` pass). Left: its
   `SETUP.EXE`, and the Win98 half (the checks need the winetests and a
   FreeDOS floppy that PC's checkout lacked).
4. **An installer** beside the zip (doc 07). QEMU's own NSIS recipe
   (`mingw-w64-x86_64-nsis` in MSYS2) is one way.
5. **Zero-copy frames** through a DXGI shared handle, the counterpart of
   the dma-buf ring and IOSurface. Frames take the readback path today.
6. **A Windows check that boots a guest**, shaped like
   `tools/xp-driver-test.sh`: drive it over QMP, pull the artefacts, diff
   a frame.
