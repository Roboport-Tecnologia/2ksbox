# Building and packaging for Windows (from Linux)

The Windows package is a **cross build from Linux**: QEMU with a mingw
toolchain, Rust for `x86_64-pc-windows-gnu`, the Qt launcher and the
Direct3D executor, rolled into a portable zip. The same stages also
build **natively under MSYS2** for debugging with gdb on a Windows PC
("Building on Windows" below); the package still comes from Linux.

The package runs on the user's PC (Ryzen 9 5900X, RTX 3090), 3D guests
included; what has run there is in `docs/tracks/m11-windows-host.md`.
Names and the install layout are in doc 07.

## The short version

```sh
scripts/win-cross.sh --build      # once: the cross container (~5 min, ~3 GB)
scripts/build-windows.sh          # qemu, rust, qt, exec, guest-tools
scripts/package-windows.sh        # the zip, checked under wine
scripts/package-windows.sh --msix # ... and the Store's MSIX layout ("The Store package")
```

The artefact is `build/win/package/2ksbox-<version>-windows-x86_64.zip`.
Windows output goes to `build/win/` and `target/x86_64-pc-windows-gnu/`,
never `build/qemu` or `target/release`, so a checkout holds both builds.
The *sources* are shared: never run `build-windows.sh` (which
re-applies the patch queue) while another build reads `qemu/`
(00-status, "Building").

## Why a container

QEMU needs glib, pixman, zlib and libepoxy **for the mingw target**.
Arch packages only `mingw-w64-gcc`; Fedora packages them all and is
QEMU's own Windows CI base
(`qemu/tests/docker/dockerfiles/fedora-win64-cross.docker`).
`packaging/windows/Dockerfile` adds:

- **rustup with `x86_64-pc-windows-gnu`.** The player links the embed
  DLL and `libdisc` links into QEMU, so Rust must share the mingw ABI.
- **Python 3.13.** Fedora's default 3.14 is past QEMU 11.1's `mkvenv`
  (3.9–3.13). The real `distlib` goes in at image build, the last moment
  with a network, because pip ≥ 26's copy is incomplete ("found no
  usable distlib").
- **libslirp from source.** There is no `mingw64-libslirp`, and every
  launcher machine asks for `-netdev user`. Without it a machine dies
  with "network backend 'user' is not compiled into this binary" while
  configure only said `slirp support: NO`; `package-windows.sh` checks
  the embed DLL's import table for it.
- **clang and lld**, through `packaging/windows/clang-mingw-cc` / `-cxx`.
  mingw GCC has only emulated TLS, which QEMU touches on every device
  access (a VGA register read cost 2.3x Linux's; patch 68).
  `WIN_QEMU_CC=gcc` builds the old way.

`scripts/win-cross.sh` runs a command in the container with the checkout
bind-mounted **at the same absolute path** (meson records absolute
paths), under rootless podman's `--userns=keep-id` so files come out
owned by you. `CONTAINER=docker` switches engines;
`WIN_CROSS_FEDORA=` picks another base.

Three things that look like the build ignoring you:

- `win-cross.sh` builds the image only when it is *missing*; after a
  Dockerfile change run `scripts/win-cross.sh --build`.
- It forwards a **whitelist** of environment variables (`JOBS`,
  `QEMU_PYTHON`, `WIN_QEMU_CC`, …); an ignored knob is probably not on
  it. Stages are positional: `scripts/build-windows.sh qemu`.
- `build-windows.sh` configures QEMU only when `build/win/qemu/build.ninja`
  is missing or the compiler changed. After a configure flag change, run
  `scripts/win-cross.sh scripts/configure-qemu.sh --windows` by hand.

## What each stage produces

| Stage | Output | Notes |
|---|---|---|
| `qemu` | `build/win/qemu/{qemu-system-i386,qemu-img,qemu-io}.exe`, `libqemu-embed-i386.dll` | `configure-qemu.sh --windows`; clang; a directory from another QEMU release configures afresh; no WHPX in i386 since 11.1 (Acceleration) |
| `rust` | `target/x86_64-pc-windows-gnu/release/{player,launcherx,discx}.exe` | `qemu-embed/build.rs` finds the DLL in `build/win/qemu` |
| `qt` | `launcher-qt/target/x86_64-pc-windows-gnu/release/launcher-qt.exe` | the package's `2ksbox.exe` (ADR-015); its own workspace |
| `exec` | `build/win/dxvk/src/d3d9/d3d9.dll`, `build/win/d3dpt/d3dpt_exec.dll`, `build/win/d3dpt-dp2-test.exe`, `build/win/wgl-probe.exe` | DXVK (patch 08's headless WSI), the executor, its host test, the offscreen-GL probe |
| `guest` | `guest-tools/out/guest-tools-*.iso` | host-independent, built only if absent |

## The package

A Windows package is **one folder**, not a Unix prefix: executables at
the top, every DLL beside them (where the loader looks), data
directories under it.

```
2ksbox.exe  2ksbox-player.exe  qemu-img.exe
libqemu-embed-i386.dll  d3dpt_exec.dll  dxvk_d3d9.dll  <the mingw runtime, Qt>
pc-bios\  guest-tools\  shaders\  tools\  doc\  plugins\  qml\  qt.conf
2ksbox-debug.bat
```

`launcher-core/src/paths.rs` takes the executable's directory as the
prefix, with `pc-bios\` as the marker. User data lives in
`%APPDATA%\2ksbox\data` (from an installed MSIX, `%USERPROFILE%\2ksbox`;
"The Store package").

**Both programs are windowed** (`windows_subsystem = "windows"`), or a
double-click opens a black terminal. So the package provides:

- a console for the debug verbs: `launcher-core/src/console.rs`
  attaches the launching one and starts every child with
  `CREATE_NO_WINDOW`;
- a home for the player's output, `%APPDATA%\2ksbox\data\player.log`;
- somewhere for a failure to go. `launcher-core/src/fatal.rs`, the
  launcher's first call, writes `launcher.log` beside `player.log` with a
  milestone per start-up step (the last line names the step that died),
  and a panic hook files message, location and backtrace and shows a
  message box. `--diagnose` writes `--paths` and `--host-check` there.

Every Play writes the player command line, quoted for a shell, into
`launcher.log` as `[player] …`, into the head of `player.log`, and to a
terminal if there is one.

**`2ksbox-debug.bat`** runs the launcher through `start "" /b /wait`
(cmd does not wait for a windowed program, and that form does not pass
redirection on, so output comes from the program's own files) and
writes `2ksbox-debug.log` with the exit codes and a copy of
`launcher.log`. **A missing `launcher.log` is itself the answer**:
nothing of ours ran, and the exit code says why (`0xC0000135` a missing
DLL, `0xC0000142` an initialiser, `0xC0000005` a fault).

**A verb ends the process with `TerminateProcess`, not `exit`**
(`launcher_core::console::exit_after_verb`). The QML module compiled
into `2ksbox.exe` keeps a `QGlobalStatic` whose destructor calls into
`Qt6Qml.dll`; with msvcrt as the C runtime the executable's destructors
run inside msvcrt's `DLL_PROCESS_DETACH`, after the Qt DLLs (loaded
later, detached earlier) are gone, so every `--paths` and `--diagnose`
printed its whole answer and then died with `0xC0000005` (found
2026-09-23 running the staged package natively). A GUI run tears Qt
down in order and never saw it. The verb has written everything by
then; Rust's buffers are flushed and the process is ended.

**The DLLs are a closure, not a list.** `objdump` walks the staged
binaries' import tables and ships what is in the mingw sysroot, never
Windows' own (`kernel32`, `opengl32`, `d3d9`, the `api-ms-win-*` sets);
a system DLL copied in makes an app run only where it was built. Import
tables miss what is loaded at run time (`libepoxy-0.dll` names `libEGL`
/ `libGLESv2` as strings; Fedora's SDL2 `LoadLibrary`ed SDL3 before SDL
was dropped), so a second pass searches every staged binary for the
name of any sysroot DLL not yet staged.

**`package-windows.sh` runs the staged package under wine**, from
outside the checkout with an empty environment:

- the launcher's `--paths` must answer inside the package;
- the player's `--companions` must name the staged executor and
  `dxvk_d3d9.dll` (loaded by name, in no import table);
- the display driver's host test must draw through that pair and read
  the right pixels (through winevulkan; skipped without a Vulkan device);
- the packaged `qemu-img.exe` must write a qcow2, which also proves the
  DLL closure.

Wine is not the target, so a failure there is investigated, not
believed; but a package that fails these is broken on every Windows.

## Which Direct3D 9 the executor runs on

DXVK, as everywhere (ADR-007). On Windows only, the **system's own
Direct3D 9** is the fallback for a host below DXVK's Vulkan 1.3 floor
(pre-Broadwell Intel, Kepler and older, TeraScale; ADR-007's second
amendment has the reasons). DXVK stays the default and the only
rasteriser a frame is compared against.

```sh
D3DPT_D3D9=auto      # DXVK, then the system library if DXVK opens no adapter
D3DPT_D3D9=dxvk      # DXVK or nothing
D3DPT_D3D9=system    # this PC's own d3d9.dll (%SystemRoot%\system32, by full path)
```

A machine says it as `-device d3dpt-vga,d3d9=<which>`, written by the
form's **Direct3D** row; the launcher resolves `auto` from its own
Vulkan probe, the only side that can tell software Vulkan from a real
device. The environment variable wins over the property, which is how
the two are compared on one host. DXVK is loaded only as
`dxvk_d3d9.dll`, never by the system's name.

**Both host tests run on both backends, and that is the check** (the
backend's first version had no oracle and drew black on a user's PC):

```sh
export D3DPT_EXEC_LIB=build/win/d3dpt/d3dpt_exec.dll
export D3DPT_DXVK_LIB="$PWD/build/win/d3dpt/dxvk_d3d9.dll"
for b in dxvk system; do
  D3DPT_D3D9=$b build/win/d3dpt-dp2-test.exe out-dp2-$b.bmp    # the display driver's checks
done
```

Both must PASS, and the two BMPs should be byte-identical (they were on
the RTX 3090).

## OpenGL for a Win98 guest

A Win98 guest's 3D is qemu-3dfx's Mesa pass-through, which needs a GL
context **inside the embed library**, where there is no window. On
Windows `embed/mglcntx_embed.c` uses WGL with a `WGL_ARB_pbuffer`
standing in for the window (doc 12). `TESTS\GLPROBE.EXE` in a Win98
guest on the user's PC reads `NVIDIA GeForce RTX 3090/PCIe/SSE2`.

- **Every ARB call borrows a context.** An ARB entry point called
  through libepoxy with no context current *faults* (GLQuake took the
  process down; doc 12, "The WGL rule").
- **One window remains**, a 1×1 popup never shown: WGL reaches pixel
  formats and extension entry points only through a window's DC, and
  `wglCreatePbufferARB` takes one to name the device.
- **`mglcntx_mingw.c` is split, not weakened.** qemu-3dfx's own WGL
  backend stays for `qemu-system-i386.exe`, but a COFF weak external is
  not an ELF weak definition, so the backend sits behind
  `MESAGL_WGL_BACKEND` and patch 10 compiles it into the emulators alone
  (patch 31 has the Linux/macOS side). `strings` is the check: the
  emulator has the WGL backend's messages, the DLL the window-less
  one's, neither the other's.
- **`tools\wgl-probe.exe`**, shipped in the package, runs the pbuffer
  sequence with no QEMU and prints where it stops. Run it first when a
  Win98 guest gets no 3D. It answers "will this driver give me an
  offscreen pbuffer", not "does our dispatch survive" (it resolves its
  pointers with a context current).

## Qt, which the package carries

`2ksbox.exe` is `launcher-qt` (ADR-015), so the zip carries Qt's DLLs,
the platform plugin and the QtQuick QML trees. Fedora ships
`mingw64-qt6-*` to link against, a native Qt of the same version for the
build-time tools, and `x86_64-w64-mingw32-qmake-qt6`, which answers
`QT_INSTALL_*` with the target's paths and `QT_HOST_*` with the host's,
as cxx-qt's cargo-only build needs.

- `CXX_QT_AUTORCC_OPTIONS=--no-zstd` (set in the image): the host `rcc`
  has zstd and the mingw `Qt6Core` does not.
- **There is no cross `windeployqt`**, so `package-windows.sh` copies
  the plugin directories (**no window without
  `plugins\platforms\qwindows.dll`**, in no import table), the QML
  module trees the views import, and a `qt.conf` pointing at both. The
  DLL closure walks plugins and QML modules too.
- A PE import library only satisfies symbols already undefined when the
  linker reaches it, so `launcher-qt/build.rs` names
  `-lQt6QuickControls2` again after the archive holding
  `appearance.cpp`.
- **Two emutls registries.** rustc links libgcc statically while
  `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s, so cxx-qt's
  `std::call_once` reached a `__once_proxy` reading `NULL` and the
  launcher died before `main` (`0xC0000005`).
  `launcher-qt/src/once_proxy.cpp` supplies a local proxy
  (`tools/qtmin/` has the diagnosis).

The package checks require `--paths` to answer and the staged launcher
to open a window offscreen. Under wine the window grab is a report, not
a verdict (wine's Qt 6 is not the target's); the last word is
`2ksbox-debug.bat` on a real PC.

## The Store package

The Microsoft Store takes a Win32 app as an **MSIX**, which is the
staged folder above plus a manifest and four logos, so the Store package
is the zip's contents and nothing more. `scripts/package-msix.sh
<staged>` writes that layout under `build/win/package/msix/` and packs
it; `package-windows.sh --msix` runs it after the zip's checks.

- `packaging/windows/AppxManifest.xml.in` is the manifest: a full-trust
  desktop app (`runFullTrust`, because the launcher spawns the player and
  QEMU runs in-process) with `internetClient` for the first-run preset
  download, on Windows 10 2004 and later, x64.
- `packaging/windows/Assets/` holds the logos at the sizes the Store
  checks (44, 50, 150, 310×150), derived from the master by
  `scripts/gen-icons.sh` like every other icon.
- `2ksbox-debug.bat` is left out: an installed package's folder is
  read-only. The launcher's log is where it always is.

Packing needs `makeappx.exe`, which is the Windows SDK's, so a Linux host
stops at the layout and the pack happens on the PC (Git Bash or MSYS2,
the SDK installed; the script finds the newest one under `Windows
Kits`). `makeappx` validates the manifest and every path it names, so a
pack that succeeds is structurally what the Store's upload check
accepts.

```sh
scripts/package-msix.sh build/win/package/2ksbox-<version>-windows-x86_64
```

**Identity.** An MSIX carries the publisher's identity, and the Store's
comes from Partner Center: reserve the name `2ksbox` there, and its
*Product identity* page gives the package name, the publisher
(`CN=<GUID>`) and the publisher display name. Pass them as `--identity`,
`--publisher`, `--publisher-display` (or `MSIX_IDENTITY`,
`MSIX_PUBLISHER`, `MSIX_PUBLISHER_DISPLAY`); an upload whose values
differ is refused. Without them the script fills in a development
identity (`CN=2ksbox-dev`) that installs only sideloaded. The version is
four numbers, `Cargo.toml`'s plus a fourth. For a Store identity it is
`.0`: the Store keeps that number for itself and each upload must be
higher than the last accepted one. For the development identity it
counts up on every pack (`build/win/package/msix-revision`), because
Windows refuses to install a package whose version it already has, and
a development package is installed over and over; only the newest
package is kept in the folder.

**A sideload**, to run the package as the Store would install it. The
Store signs its own uploads, so an upload stays unsigned; a sideload is
signed with a certificate whose subject equals the manifest's publisher
and which the PC trusts. `scripts/win-sideload.ps1` does all of it:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/win-sideload.ps1            # newest build/win/package/*.msix
powershell -ExecutionPolicy Bypass -File scripts/win-sideload.ps1 -Check     # ... and read back where it keeps its library
powershell -ExecutionPolicy Bypass -File scripts/win-sideload.ps1 -NoInstall # sign only, no administrator
powershell -ExecutionPolicy Bypass -File scripts/win-sideload.ps1 -Remove    # uninstall; the library stays
```

It reads the identity out of the package, makes (once) a certificate in
the user's store with that publisher as its subject, trusts it in
`LocalMachine\TrustedPeople` through one UAC prompt (the only step that
needs an administrator), signs a copy (`*-sideload.msix`) and installs
it; the app is then in the Start menu. With `-Check` it also runs the
installed launcher's `--diagnose` **with package identity**
(`Invoke-CommandInDesktopPackage`, given the executable's full path: a
bare name resolves against the caller's directory) and reads the
`library` line back out of the `launcher.log` it wrote: the line must
end in `(packaged)` and the log must be under `%USERPROFILE%\2ksbox`,
or the script fails. The check is off by default because it starts the
app, which an install does not need (user). `package-msix.sh --pfx` is
the same signing for a PFX of your own. The Windows App Certification Kit runs against the installed
package (`appcert.exe test -appxpackagepath <msix> -reportoutputpath
report.xml`), and certification runs the same checks, so run it before
an upload.

**What differs from the zip: the library is `%USERPROFILE%\2ksbox`.**
A packaged app's writes to `%APPDATA%` are **virtualised**: they land
in `%LOCALAPPDATA%\Packages\<family>\LocalCache\Roaming`, the package's
own copy, and **an uninstall deletes that copy**, which for the zip's
layout would be the user's machines and their disks. The
`unvirtualizedResources` capability would turn that off, but Microsoft
reserves it for its partners' games (it "could compromise the system's
ability to uninstall cleanly"), so it is not declared. Instead the
launcher asks Windows whether it runs with package identity
(`GetCurrentPackageFullName`, `paths::packaged()`) and, when it does,
keeps the library at the profile root, `%USERPROFILE%\2ksbox`, as
VirtualBox keeps `VirtualBox VMs` there: not virtualised, not synced by
OneDrive as `Documents` is, and untouched by an uninstall. `2ksbox.exe
--paths` (and `--diagnose`'s copy in `launcher.log`) prints it as
`library … (packaged)`; `LAUNCHER_PACKAGED=1` makes a plain build answer
the same, for a check without an install, and `scripts/win-sideload.ps1`
checks the real thing from an installed package. `launcher.log` and
`player.log` move with it. A library the zip build made in
`%APPDATA%\2ksbox\data` is not adopted (a rename out of a virtualised
directory is not a rename): copy its `machines`, `discs.toml` and
`shader-profiles` into `%USERPROFILE%\2ksbox` by hand.

**Certification, before the upload.** The Windows App Certification Kit
(`appcert.exe`, in the Windows SDK) runs the checks certification runs,
against a *signed* package, so the sideload's copy is the one to test:

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\App Certification Kit\appcert.exe' test `
  -appxpackagepath build\win\package\2ksbox-<version>-windows-x64-sideload.msix `
  -reportoutputpath build\win\package\wack-report.xml     # elevates: one UAC prompt, ~5 min
```

The 2026-09-23 run of the development package: `OVERALL_RESULT=PASS`.
Two *optional* tests fail by the package's nature and stay so (an
optional FAIL does not change the overall result): "Archive files
usage" (PE files inside an archive: the guest tools' EXEs and DLLs
inside the ISO) and "Blocked executables" (references to process-launch
APIs: the launcher starts the player). The first run was a WARNING for
"DPI awareness", because neither executable's manifest declared it;
`packaging/windows/app.manifest` (per-monitor v2, embedded by
`packaging/windows/win-icon.rs` beside the icon, in place of mingw's
default manifest) answers it, and the second run passed.

**The submission**, step by step. Everything Partner Center asks for is
written down in `packaging/windows/store-listing.md`, so a resubmission
pastes the same words.

1. A developer account at <https://partner.microsoft.com/dashboard>
   (individual, a one-time fee), then *Apps and games → New product →
   MSIX or PWA app* and reserve the name `2ksbox`.
2. *Product management → Product identity* shows the three values;
   pack with them (`--identity`, `--publisher`, `--publisher-display`).
   **The version's first number cannot be 0** (the Store's rule, and
   `package-msix.sh` refuses it for a Store identity), so the first
   upload needs `Cargo.toml` at 1.0.0 or later, or `--version`; the
   fourth number is 0.
3. Sideload that exact package once (`win-sideload.ps1 -Check`) and run
   the certification kit on its signed copy (above).
4. In the submission: *Packages* takes the unsigned `.msix` (the Store
   signs it); *Properties* takes the category, the privacy-policy URL
   (mandatory because of `internetClient`; it is
   `docs/privacy.md` on GitHub) and the system requirements;
   *Age ratings* is the IARC questionnaire (all "no"); *Pricing* is
   free; *Store listing* takes the text and at least one screenshot of
   1366×768 or more, a shot of the player's window with the shader
   chain on it (the player full screen, Ctrl+Alt+Shift+S or Win+PrtScn), never
   the player's Ctrl+Alt+S guest frame scaled up (the listing file says
   why); *Submission options* takes
   one sentence per restricted capability (`runFullTrust`, in the
   listing file).
5. Submit; certification takes up to a few days, and its report names
   any failing test by the kit's name.

An update is the same pack with a higher version, uploaded to a new
submission; the Microsoft Store Developer CLI (`winget install
"Microsoft Store Developer CLI"`, `msstore publish --inputFile
<msix> --appId <productId>`) does that from a shell, but it signs in
with an Entra ID tenant, not the Microsoft account, so it is for after
the first submission has gone through the web form.

Microsoft lets the listing carry the app's own licence terms, and its
policy permits open-source apps; whether GPL-2 QEMU and 86Box go up
under Store terms is the same question ADR-019 leaves to the user.

## Acceleration

Windows' hardware acceleration is **WHPX** (Windows Hypervisor
Platform). **Since QEMU 11.1 it is not in `qemu-system-i386`**, the
target the package ships: upstream builds it for x86_64 only, and put
back for i386 it dies in SeaBIOS (track M21, step 4). So every era
machine runs TCG on Windows for now; what follows is the launcher's
side, unchanged. `Accel::Auto` is `whpx:tcg`, "hardware
acceleration required" is `whpx`, and the wizard's hint asks
`WHvGetCapability`, because the feature can be installed and still off
(Hyper-V or WSL2 may hold the root partition). Turn it on with:

```
dism /online /enable-feature /featurename:HypervisorPlatform /all
```

A machine with a fixed processor speed (doc 06's DOS family) is
emulated regardless.

## What is not there yet

- **Zero-copy 3D frames.** Frames take the readback path (a copy per
  frame); the counterpart of Linux's dma-buf and macOS's IOSurface would
  be a DXGI shared handle.
- **Live control on a real PC.** The QMP socket for snapshots and the
  disc shelf is `unix:` on Windows too, reached through Winsock AF_UNIX
  (`launcher-core/src/control.rs`). Wine has no AF_UNIX (`socket()`
  answers 10047), so it has never run. `live control off: …` in
  `launcher.log` means the trial bind failed and the machine ran
  without it.
- **No installer** beside the zip for users outside the Store (doc 07
  wants one; QEMU's `mingw32-nsis` recipe is within the image's reach).
  The MSIX is one, but only through the Store or a trusted certificate.
- **The Store package has not been uploaded.** It installs and runs on
  the PC through `scripts/win-sideload.ps1` (2026-09-23) and passes the
  certification kit ("The Store package"); its
  packaged library location has been checked with `LAUNCHER_PACKAGED=1`,
  not yet read back from the installed package (`win-sideload.ps1
  -Check`). What is left needs the user: the Partner Center account and
  the name reservation (the identity triple), a version of 1.0.0 or
  later, and screenshots of the player's window with games in it.
- **No Windows check that boots a guest**, in the shape of
  `tools/xp-driver-test.sh`.

## Building on Windows

For a fault that shows only on real Windows, `scripts/build-windows.sh`
runs all its stages in **MSYS2's MINGW64 shell** with no container, and
`scripts/win-run.sh` runs the result out of the checkout. Every stage
builds on the user's PC and the launcher runs there; the ISO this build
makes has not yet been booted in a guest. The package stays on Linux.

**MINGW64, not UCRT64 or CLANG64**: it is the cross image's ABI (msvcrt,
GCC's runtime and libstdc++, Rust's `x86_64-pc-windows-gnu`), so a fault
reproduced here is almost always one in the shipped build. The scripts
refuse the other two shells.

Once, on the PC:

```sh
# 1. MSYS2 from https://www.msys2.org, then the "MSYS2 MINGW64" shell:
pacman -Syu                                   # again if it asks to restart
pacman -S git
git config --global core.autocrlf false       # CRLF breaks every patch of the queue
cd /c && git clone --recurse-submodules --shallow-submodules https://github.com/davidrios/2ksbox
cd 2ksbox && scripts/build-windows.sh --msys2-deps

# 2. Rust from https://rustup.rs with the GNU host (MSVC needs Microsoft's linker):
./rustup-init.exe -y --default-host x86_64-pc-windows-gnu
echo 'export PATH="$(cygpath "$USERPROFILE")/.cargo/bin:$PATH"' >> ~/.bashrc && . ~/.bashrc

# 3. Open Watcom: the same ow-snapshot.tar.xz as on Linux (binaries in binnt64):
mkdir -p /c/WATCOM && tar -C /c/WATCOM -xf ow-snapshot.tar.xz
echo 'export WATCOM=/c/WATCOM' >> ~/.bashrc && . ~/.bashrc
```

Then, as often as needed:

```sh
scripts/build-windows.sh                  # qemu rust qt exec, and the ISO if there is none
scripts/build-windows.sh rust             # one stage
scripts/build-windows.sh guest            # the ISO again, after a driver change
scripts/win-run.sh launcher               # the Qt launcher, out of the checkout
GDB=1 scripts/win-run.sh player ...       # the player under gdb
```

`scripts/win-run.sh` stands in for the one-folder package: it puts
`build/win/qemu` on `PATH` for the embed DLL, names the player to the
launcher, and names the executor and DXVK (copied to `dxvk_d3d9.dll`) to
QEMU. The launcher writes nothing to the terminal, so paste the
`[player] …` line from `launcher.log` after `GDB=1 scripts/win-run.sh
player`.

What differs from the cross build, and why:

- **Python is MSYS2's own 3.14**, with `python-distlib`. MSYS2 has no
  older one, and a python.org venv has `Scripts\` where configure looks
  for `bin/`. QEMU 11.1 builds on 3.14 with the real `distlib`, and
  `configure-qemu.sh` accepts 3.14 only then. (9.2's mkvenv gave pip a
  `file://C:/…` wheels URL, a host named `C:`; patch 69 fixed that
  until 11.0 passed a plain path.)
- **Optional libraries are pinned off.** QEMU links what it detects, and
  MSYS2 with Qt has zstd, gnutls and others the cross image lacks, so
  native `configure-qemu.sh` disables each one the cross build lacks.
- **lld is named through meson's `CC_LD`**, because a native meson
  cannot run the `clang-mingw-cc` shell script as a compiler. Same
  clang, linker and flags.
- **Paths in meson's files are `C:/…`** (`cygpath -m`).
  `qemu-embed/build.rs` strips `canonicalize`'s `\\?\` prefix, because
  the linker appends `/libqemu-embed-…` and `/` is not a separator in a
  verbatim path.
- **Qt's tools run from `build/win/qt-host`.** qt-build-utils starts
  moc, rcc, qmltyperegistrar and qmlcachegen with an *empty*
  environment, and MSYS2 installs them in `share/qt6/bin`, away from
  their DLLs, so none starts ("moc unexpectedly exited"). The `qt` stage
  copies each tool beside its DLLs (found with `ldd`) and sets `QMAKE` to
  `packaging/windows/qmake-host.c`, a static wrapper that answers the
  tool-directory queries with that folder and passes the rest to
  `qmake6`. It then test-runs each tool with an empty environment and
  prints what failed (qt-build-utils only says "could not find Qt").
- **GCC 16 has no `std::__once_call` to borrow.** Its libstdc++ is built
  with `_GLIBCXX_NO_EXTERN_THREAD_LOCAL`, so the pre-`main` fault cannot
  happen and `once_proxy.cpp` compiles only where that macro is absent.
  The package still carries it.
- **Package versions follow MSYS2** (Qt 6.11 against Fedora's 6.10, GCC
  16 against 15), so the zip remains the verdict.
- `prepare-qemu.sh` runs every git through `qgit`, which retries on
  `Unable to create index.lock: File exists`, a scanner holding the lock
  of the git that just exited (00-status, "Building").

**The guest-tools ISO** comes from the same scripts as on Linux
(`build-wrappers.sh`, `build-driver.sh`, `build-driver9x.sh`), each of
which first sources `guest-tools/msys2-i686.sh`, the whole port:

- qemu-3dfx's `conf_wrapper` builds natively only with
  `MSYSTEM=MINGW32` and an i686 `gcc`, so the file sets that, puts
  `/mingw32/bin` first on `PATH` (the x86_64 python3 and gendef behind
  it), and shims the target-prefixed binutils our scripts call
  (`i686-w64-mingw32-objdump`, `-nm`, `-ar`, `-windres`). conf_wrapper
  writes a plain `gcc` into its Makefiles, so `build-wrappers.sh` gives
  plain `gcc` the same msvcrt and `-march=pentium3` flags.
- **It links Linux's i686 runtime, not MSYS2's.** MSYS2's 32-bit
  runtime targets the Pentium 4 (SSE2 in printf/dtoa, libgcc's
  double-to-unsigned conversion, msvcrt's stat and time helpers,
  libwinpthread), so nearly every guest program linked with it fails
  the ISO's Pentium III check. The first run downloads
  Arch's `mingw-w64-crt` and `-winpthreads` 14.0.0-1 and the i686 libgcc
  of `mingw-w64-gcc` 16.2.0-2, pinned by sha256, into
  `guest-tools/tools/i686-runtime/`; the i686 `gcc` shims link them with
  `-B`/`-L`, and headers stay MSYS2's. MSYS2's i686 gcc must be 16.2.0,
  or the script says to re-pin.
- Open Watcom runs from `binnt64`.
- Paths need no conversion (MSYS2 rewrites `/c/…` arguments and
  environment lists like Watcom's `INCLUDE` for a native program); only
  Watcom's `@file` gets `cygpath -m`.
- QEMU's seven symbolic links are in Linux-only subprojects that are
  never built, so a checkout without symlink support is fine.

## Running it there

Unzip anywhere and run `2ksbox.exe`; machines, `discs.toml`, shader
profiles and downloaded presets live in `%APPDATA%\2ksbox\data` (a Store
install keeps them in `%USERPROFILE%\2ksbox` instead). When
something misbehaves, run `2ksbox-debug.bat` and send
`2ksbox-debug.log`. `PLAYER_KEYBOARD_LOG=1`
adds the keyboard capture's decisions to `player.log`.
