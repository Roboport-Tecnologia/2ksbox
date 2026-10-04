# Building and packaging for Windows

Everything Windows is built **natively on a Windows PC** (ADR-026): in
MSYS2's MINGW64 shell, with Visual Studio's C++ tools beside it
("Building on Windows" below), and the zip and the MSIX are rolled and
checked there too, by Windows itself. The cross build from Linux
(`scripts/win-cross.sh` and its Fedora container) was retired on
2026-10-03. What builds each part is under "The toolchains".

The package runs on the user's PC (Ryzen 9 5900X, RTX 3090), 3D guests
included; what has run there is in `docs/tracks/m11-windows-host.md`.
Names and the install layout are in doc 07.

## The short version

```sh
# on the PC, in MSYS2's MINGW64 shell ("Building on Windows"):
scripts/build-windows.sh          # qemu, rust, mitsuami, exec, guest-tools
scripts/package-windows.sh        # the zip, checked on this PC
scripts/package-windows.sh --msix # ... and the Store's MSIX layout ("The Store package")
```

The artefact is `build/win/package/2ksbox-<version>-windows-x86_64.zip`.
Windows output goes to `build/win/`, `target/x86_64-pc-windows-gnu/` and
`target/x86_64-pc-windows-msvc/`,
never `build/qemu` or `target/release`, so a checkout holds both builds.
The *sources* are shared: never run `build-windows.sh` (which
re-applies the patch queue) while another build reads `qemu/`
(00-status, "Building").

## The toolchains

| Part | Built with | Why |
|---|---|---|
| QEMU, `libqemu-embed-i386.dll`, `qemu-img` | MSYS2's clang against its mingw runtime (msvcrt), lld | upstream QEMU builds on Windows only under mingw; clang because mingw GCC has only emulated TLS, which QEMU touches on every device access (a VGA register read cost 2.3x Linux's; patch 68). `WIN_QEMU_CC=gcc` builds the old way |
| `libdisc`, `libsynth` (inside QEMU), the winit `player.exe` | Rust `x86_64-pc-windows-gnu` | the same mingw ABI as QEMU; the winit player is not shipped, but `test.sh` runs it |
| `launcherx`, `discx`, `synthx`, `tools\wgl-probe.exe` | Rust `x86_64-pc-windows-msvc` (`scripts/cargo-msvc.sh`) and `cl`, static C runtime | everything that does not link into QEMU is MSVC (ADR-026's second amendment) |
| `2ksbox.exe` (`launcher-mitsuami`), `2ksbox-player.exe` (`player-mitsuami`) | Rust `x86_64-pc-windows-msvc`, static C runtime | WinUI 3 needs MSVC (ADR-023, ADR-025); the player links QEMU's mingw DLL across the two C runtimes (doc 11, "The C runtime boundary") |
| DXVK's `d3d9.dll`, `d3dpt_exec.dll` | Visual Studio's `cl`, static C runtime, in the environment `scripts/msvc-env.sh` sets up | DXVK throws C++ exceptions out of `Direct3DCreate9`, which only an executor built by the same compiler catches, so the two moved to MSVC together (ADR-026's amendments). QEMU loads the executor by name; only C crosses that edge. DXVK under MSVC needed patch 15 ("DXVK under MSVC" below) |
| the guest-tools ISO | MSYS2's i686 GCC with Linux's i686 runtime, Open Watcom | Windows 9x and XP guests; modern MSVC targets neither |
| the WDDM driver | the Enterprise WDK 10.0.19041 | Windows 7 and 32-bit kernel drivers ("The WDDM driver") |

QEMU's libraries are MSYS2's (`--msys2-deps`), libslirp among them: every
launcher machine asks for `-netdev user`, and a QEMU built without it
dies with "network backend 'user' is not compiled into this binary"
while configure only said `slirp support: NO`; `package-windows.sh`
checks the embed DLL's import table for it.

`build-windows.sh` configures QEMU only when `build/win/qemu/build.ninja`
is missing, the compiler or the QEMU release changed, or a meson file or
`configure-qemu.sh` is newer than the last configure.

## What each stage produces

| Stage | Output | Notes |
|---|---|---|
| `qemu` | `build/win/qemu/{qemu-system-i386,qemu-img,qemu-io}.exe`, `libqemu-embed-i386.dll` | `configure-qemu.sh --windows`; clang; a directory from another QEMU release configures afresh; no WHPX in i386 since 11.1 (Acceleration) |
| `rust` | `target/x86_64-pc-windows-gnu/release/player.exe`, `target/x86_64-pc-windows-msvc/release/{launcherx,discx,synthx}.exe` | `qemu-embed/build.rs` finds the DLL in `build/win/qemu`; the winit player is for `test.sh`; the tools are MSVC (`scripts/cargo-msvc.sh`, rustup's `stable-x86_64-pc-windows-msvc`), skipped without it |
| `mitsuami` | `launcher-mitsuami/target/release/launcher-mitsuami.exe`, `player-mitsuami/target/release/player-mitsuami.exe` | the package's `2ksbox.exe` and `2ksbox-player.exe` (ADR-023, track M22); their own workspaces; MSVC ("The launcher") |
| `exec` | `build/win/dxvk/src/d3d9/d3d9.dll`, `build/win/d3dpt/d3dpt_exec.dll`, `build/win/d3dpt-dp2-test.exe`, `build/win/wgl-probe.exe` | DXVK (patch 08's headless WSI), the executor and the offscreen-GL probe, MSVC; the executor's host test (mingw, so it loads the executor as QEMU does). Skipped without Visual Studio's C++ tools |
| `guest` | `guest-tools/out/guest-tools-*.iso` | host-independent, rebuilt when its sources move (`build.sh`'s stamp) |

## The package

A Windows package is **one folder**, not a Unix prefix: executables at
the top, every DLL beside them (where the loader looks), data
directories under it.

```
2ksbox.exe  2ksbox-player.exe  qemu-img.exe
libqemu-embed-i386.dll  d3dpt_exec.dll  dxvk_d3d9.dll  <the mingw runtime>
pc-bios\  guest-tools\  shaders\  tools\  doc\
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
terminal if there is one. DXVK's own log, `2ksbox-player_d3d9.log`, goes
beside them too: the launcher sets `DXVK_LOG_PATH` to the data directory
unless the caller set one, because DXVK otherwise writes into the working
directory, which for a double-clicked launcher is the package's folder.

**`2ksbox-debug.bat`** runs the launcher through `start "" /b /wait`
(cmd does not wait for a windowed program, and that form does not pass
redirection on, so output comes from the program's own files) and
writes `2ksbox-debug.log` with the exit codes and a copy of
`launcher.log`. **A missing `launcher.log` is itself the answer**:
nothing of ours ran, and the exit code says why (`0xC0000135` a missing
DLL, `0xC0000142` an initialiser, `0xC0000005` a fault).

**A verb ends the process with `TerminateProcess`, not `exit`**
(`launcher_core::console::exit_after_verb`): it has written everything
by then, and no global destructor runs after its DLLs are gone. The Qt
launcher (retired 2026-10-02) printed every `--paths` and `--diagnose`
answer and then died with `0xC0000005` in a Qt destructor that way.

**The DLLs are a closure, not a list.** `objdump` walks the staged
binaries' import tables and ships what is in the mingw sysroot, never
Windows' own (`kernel32`, `opengl32`, `d3d9`, the `api-ms-win-*` sets);
a system DLL copied in makes an app run only where it was built. Import
tables miss what is loaded at run time (`libepoxy-0.dll` names `libEGL`
/ `libGLESv2` as strings; Fedora's SDL2 `LoadLibrary`ed SDL3 before SDL
was dropped), so a second pass searches every staged binary for the
name of any sysroot DLL not yet staged.

### The checks

**`package-windows.sh` runs the staged package** on Windows itself
("Packaging on Windows"), from outside the checkout with an empty
environment:

- the launcher's `--paths` must answer inside the package;
- the player's `--companions` must name the staged executor and
  `dxvk_d3d9.dll` (loaded by name, in no import table);
- the display driver's host test must draw through that pair and read
  the right pixels (skipped without a Vulkan device), and on the PC's
  own `system32\d3d9.dll`;
- the packaged `qemu-img.exe` must write a qcow2, which also proves the
  DLL closure;
- `2ksbox.exe` and `2ksbox-player.exe` must not import `vcruntime*` /
  `msvcp*` (they link their C runtime statically; "The launcher");
- the launcher must draw its window, and the player must run a machine
  with a General MIDI port to its BIOS and quit, from outside the
  package folder.

### Packaging on Windows

The mingw runtime and the binutils are MSYS2's own (`/mingw64/bin`,
`objdump`, `strip`), the ones the native build linked against. The DLL closure is the same
walk; Windows' Vulkan loader is never staged although `/mingw64/bin`
carries one (`vulkan-1.dll` must be the GPU driver's).

The checks run the package itself, from its folder, with `PATH` holding
only `%SystemRoot%`'s directories, so a DLL missing from the package
fails here instead of being found in MSYS2. `LAUNCHER_DATA_DIR` points
the launcher's data (library, `launcher.log`, the check's machine) at a
scratch directory, because Windows' known folders, not the environment,
place `%APPDATA%`. The `LAUNCHER_PACKAGED=1` check has to see the real
`%USERPROFILE%\2ksbox`; a run leaves it as it found it. Since the
checks run from the package's folder, anything written into the working
directory would ship: `DXVK_LOG_PATH`, `%ProgramData%` (without which
NVIDIA's driver makes `NVIDIA Corporation\umdlogs` there) and
`%LOCALAPPDATA%` point at the scratch directory, and the staged tree is listed before and after; a new
entry is removed and fails the package.

The launcher's window is `LAUNCHER_SHOT`, which starts WinUI 3 and the
Windows App Runtime (the window shows for a moment), and `wgl-probe`
answers for the PC's real GL.

The first run with the mitsuami launcher (2026-10-02) passed every
check but one: 16 mingw DLLs where the Qt launcher needed 71, the window
drawn, the C runtime static. The one is the executor's open thread on
the system d3d9 (00-status).

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

### DXVK under MSVC

DXVK and the executor are built with Visual Studio's `cl` (2026-10-04,
ADR-026's second amendment), so a C++ exception DXVK throws (a host with
no Vulkan device, `test.sh`'s `exec-no-device`) is caught by the
executor rather than ending QEMU. `scripts/msvc-env.sh`, sourced, puts
`cl`, `link`, `rc` and the SDK on `PATH` with `INCLUDE`/`LIB` from
`vcvars64.bat` (found by `vswhere`); `configure-dxvk.sh --windows` and
`build-d3dpt-exec.sh --windows` source it, and `build-windows.sh` runs
DXVK's ninja inside it. A `build/win/dxvk` configured before the move
(no `.2ksbox-cc` saying `msvc`) is configured afresh. Both DLLs link the
C runtime statically and import only system DLLs; `package-windows.sh`
checks that. The host test `d3dpt-dp2-test.exe` stays mingw on purpose:
it opens the executor as QEMU does, across the two C runtimes.

The first MSVC DXVK failed 8 of the oracle's 127 checks (the fixed
function on a two-stream declaration, cube and volume textures) with
every CPU-side value identical to the mingw build's. The cause was an
upstream bug that only MSVC's STL reaches: `DxvkGraphicsPipelineVertexInputState::eq`
overwrote a `false` from the attribute comparison with the divisor
loop's result, so two vertex layouts with the same counts and divisors
compared equal. libstdc++'s `unordered_map` compares the stored hash
before calling `eq`, MSVC's does not, so only the MSVC build reused the
wrong pipeline (one `vkCreateGraphicsPipelines` fewer in a
`VK_LAYER_LUNARG_api_dump` diff of the two builds). DXVK patch 15 fixes
it. An MSVC-only rendering difference is most likely another such `eq`:
diff the two builds' API dumps before reading code.

### QEMU under MSVC

QEMU also builds against MSVC's runtime (2026-10-04, user: "see if you
can also make qemu compile on msvc"). It is opt-in, beside the mingw
build that still ships, in its own `build/win/qemu-msvc`:

```sh
scripts/build-deps.sh               # zlib, pcre2, glib, pixman, libslirp, libepoxy
WIN_QEMU_CC=msvc scripts/configure-qemu.sh
ninja -C build/win/qemu-msvc        # in any MINGW64 shell
WIN_QEMU_CC=msvc scripts/test.sh all
```

The two scripts find Visual Studio themselves (`scripts/msvc-env.sh`),
and configure writes what the build needs of it into the build
directory: its header and library directories as `-idirafter` and
`-Wl,-libpath:` flags (8.3 short names, since configure splits its
flags on spaces; `-idirafter` and its directory as two words, since
MSYS2 rewrites the `/PROGRA~1` in a joined `-idirafterC:/PROGRA~1`; not
`-L`, which meson's own link checks hand lld-link as an unknown `-L`;
clang finds no UCRT here on its own), and the libraries' `.pc`
directory as meson's `pkg_config_libdir`, so a regeneration under a
plain `ninja` does not take MSYS2's mingw glib. A build directory
configured before 2026-10-04's fix wants `msvc-env.sh` sourced first
(`fatal error: 'sys/types.h' file not found` otherwise), or configuring
again.

- **The compiler** is MSYS2's clang targeting `x86_64-pc-windows-msvc`
  (its GNU driver, since QEMU's flags are GCC's; Visual Studio ships no
  clang-cl here), with Visual Studio's headers, the UCRT and the static C
  runtime (`-Db_vscrt=mt`), linked by lld-link. Not `cl`: QEMU is GNU C
  throughout (statement expressions, `typeof`, `__attribute__`).
- **The libraries** are ours, static, from `scripts/build-deps.sh` on
  Windows into `build/deps/x86_64-msvc`, with clang-cl (glib's meson
  takes only an MSVC-syntax compiler for MSVC). MSYS2's are mingw's. The
  embed DLL then imports nothing but Windows' own DLLs, where the mingw
  one needs glib, pixman, libslirp, libepoxy, zlib, bzip2, libgcc and
  winpthread beside it. libslirp's and libepoxy's `.pc` files gain the
  define that stops their headers declaring `dllimport`
  (`LIBSLIRP_STATIC`, `EPOXY_PUBLIC=extern`); libslirp has a patch
  making iconv optional (`patches/deps/README.md`); libepoxy's EGL half
  builds against `third_party/khronos/EGL`.
- **QEMU's side** is patch 83 (`patches/qemu/README.md`): POSIX headers
  mingw has and the UCRT lacks (`qemu/include/msvc/`, functions in
  `util/oslib-msvc.c`), compiler-rt's 128-bit division, `setjmp` without
  unwinding, and the enums. **MSVC's ABI makes every enum an `int`**:
  an enum bit-field one bit short reads back negative (`TCGTemp.kind`
  turned `TEMP_CONST` into -4 and TCG aborted in `la_bb_end`; the Sound
  Blaster raised no interrupt), and an enumerator past `INT_MAX` is
  truncated. `QEMU_ENUM_UNSIGNED` types those enums, and the MSVC build
  makes the three warnings that find them errors, so a QEMU bump that
  adds one fails to build rather than to run.
- **WHPX** needs the Windows SDK 10.0.26100 (its VMX capability codes);
  `configure-qemu.sh` stops on an older SDK rather than build without it
  (user).
- Not built under MSVC: `qemu-nbd` (POSIX threads), the guest agent
  (lld-link refuses its two resource objects), bzip2 (dmg's bz2 chunks).
  None ships.
- `libqemu-embed-*.dll` keeps mingw's `lib` prefix, the name the players
  import (`raw-dylib`), so a player runs either build unchanged: the
  first one on `PATH` wins. `WIN_QEMU_CC=msvc scripts/win-run.sh
  launcher` (or `player`, `qemu`) puts `build/win/qemu-msvc` there in
  place of `build/win/qemu`; the launcher's players inherit it.

**Tested (2026-10-04):** QEMU's own unit tests, 99 of 99 (the RCU ones
with the arguments meson gives them, the subprocess ones with glib's
`gspawn-win64-helper.exe` beside them); `WIN_QEMU_CC=msvc scripts/test.sh
all`, which takes the MSVC build for the host checks and the guest
tools (`tools/guestwait.sh`, `tools/qemuhost.py`; XP's Direct3D frames
among them): 47 passed, 1 failed, 28 skipped, the same checks as the
mingw build's run the same day (`sharing` fails on both: `qemu-vdagent`
needs spice-protocol, which Windows builds without). The checks that
boot a machine through a player skipped there (no player built in that
checkout); by hand, `player-mitsuami` with `build/win/qemu-msvc` first on
`PATH` boots the XP machine (`launcherx --print-args`, through an
overlay) to its desktop, `PLAYER_DUMP` frame #3000 after 53 s, the mingw
DLL's run 52 s with the same frame. Not done yet: a real speed
comparison, and the package (`package-windows.sh` still rolls the mingw
QEMU).

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

## The launcher

`2ksbox.exe` is `launcher-mitsuami` (ADR-023): WinUI 3 through
mitsuami. The same stage then builds `player-mitsuami` (track M22),
which the package ships as `2ksbox-player.exe` (2026-10-03), MSVC as
well, linking QEMU's mingw DLL across the two C runtimes (doc 11, "The
C runtime boundary"); a launcher in the checkout starts it once built.
They are the package's two MSVC binaries, each with a static C runtime.
`package-windows.sh` checks both import no Visual C++ runtime DLL, and
runs the staged player from outside the package folder on a machine
with a General MIDI port to its BIOS and a clean exit, which is WinUI
3, Direct3D 12 on its surface, QEMU's DLL and the packaged SoundFont at
once. `build-windows.sh mitsuami` builds it in
MSYS2's MINGW64 shell with `cargo +stable-x86_64-pc-windows-msvc` (Visual
Studio's C++ tools must be installed; `rustup toolchain install
stable-x86_64-pc-windows-msvc` once), into
`launcher-mitsuami/target/release/`.

- **Coreutils' `link` shadows Microsoft's.** MSYS2's `/usr/bin` has a
  `link.exe` of its own, found before Visual Studio's ("link: extra
  operand"), so the stage runs cargo with `/usr/bin` and `/bin` off
  `PATH`. `/mingw64/bin` stays, for `windres`.
- **A static C runtime** (`-C target-feature=+crt-static`), so the exe
  imports no `vcruntime140.dll`, which no Windows comes with; the
  packager checks it stays that way. It talks to the player and QEMU
  only through a command line, so the two C runtimes never meet.
- **The Windows App Runtime 2.4 or later** is a framework package that
  Microsoft installs once per PC. mitsuami adds it to the process at
  start (a dynamic package dependency); without it the launcher's log
  says to install it. The zip cannot carry it; the MSIX declares it
  (`PackageDependency` on `Microsoft.WindowsAppRuntime.2`), and the
  Store installs it with the app.
- **The icon and manifest** come from `packaging/windows/win-icon.rs`,
  as for the player: on MSVC `windres` writes a `.res` that Microsoft's
  linker takes as it is, and the linker makes no manifest of its own
  (`/MANIFEST:NO`). WinUI starts with that manifest (checked
  2026-10-02).

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

Packing needs `makeappx.exe`, which is the Windows SDK's (the script
finds the newest one under `Windows Kits`). `makeappx` validates the manifest and every path it names, so a
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

**The submission** (the steps, and the text and answers Partner Center
asks for) is kept in the store repo since 2026-10-04 (user), so the listing copy is no ready-made kit for an
impostor listing. Everything that makes the package stays here.

Microsoft lets the listing carry the app's own licence terms, and its
policy permits open-source apps; whether GPL-2 QEMU and 86Box go up
under Store terms is the same question ADR-019 leaves to the user.

## Acceleration

Windows' hardware acceleration is **WHPX** (Windows Hypervisor
Platform), for **Windows 11 machines only**. Era machines are emulated
(user, 2026-10-02): QEMU 11.1 builds WHPX into x86_64 only, not the
`qemu-system-i386` the era runs on, and put back for i386 it dies in
SeaBIOS (track M21, step 4). So an era machine's `Accel::Auto` is
`tcg` and the picker offers no hardware entry for it, as on macOS. A
Windows 11 machine's `Accel::Auto` is `whpx:tcg`, "hardware
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

`scripts/build-windows.sh` runs all its stages in **MSYS2's MINGW64
shell**, and `scripts/win-run.sh` runs the result out
of the checkout. Every stage builds on the user's PC and the launcher
runs there; the ISO this build makes installs the XP display driver in
a guest and runs `test.sh`'s Direct3D checks (2026-10-03), its
`SETUP.EXE` and the Win98 half untried. The launcher builds only here, so `scripts/package-windows.sh`
rolls and checks the zip here too ("Packaging on Windows").

**MINGW64, not UCRT64 or CLANG64**: it is the package's ABI (msvcrt,
GCC's runtime and libstdc++, Rust's `x86_64-pc-windows-gnu`). The
scripts refuse the other two shells.

Once, on the PC:

```sh
# 1. MSYS2 from https://www.msys2.org, then the "MSYS2 MINGW64" shell:
pacman -Syu                                   # again if it asks to restart
pacman -S git
git config --global core.autocrlf false       # belt and braces: CRLF breaks every patch of the queue
cd /c && git clone --recurse-submodules --shallow-submodules https://github.com/davidrios/2ksbox
cd 2ksbox && scripts/build-windows.sh --msys2-deps

# 2. Rust from https://rustup.rs with the GNU host (MSVC needs Microsoft's linker):
./rustup-init.exe -y --default-host x86_64-pc-windows-gnu
echo 'export PATH="$(cygpath "$USERPROFILE")/.cargo/bin:$PATH"' >> ~/.bashrc && . ~/.bashrc

# 3. Open Watcom: the same ow-snapshot.tar.xz as on Linux (binaries in binnt64),
#    open-watcom-v2's Last-CI-build release, unpacked where build-driver9x.sh
#    looks by default (or anywhere, with WATCOM= naming it):
curl -LO https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar -C ~/.local/opt/open-watcom -xf ow-snapshot.tar.xz
```

`--msys2-deps` also installs what `scripts/test.sh` needs here
(`docs/testing.md` "On Windows"), and the suite runs in this same shell.

Line endings: Git for Windows (the `git` outside MSYS2, which Visual
Studio and most editors use) sets `core.autocrlf=true` system-wide, and a
clone or `git worktree add` made with it converts. `.gitattributes`
(`* -text`) keeps this repository's files as committed regardless. The
submodules have their own attributes, so `build-windows.sh` runs every
git with `core.autocrlf=false` and checks out again any submodule it
finds converted.

Then, as often as needed:

```sh
scripts/build-windows.sh                  # qemu rust mitsuami exec wddm, and the ISO when its sources moved
scripts/build-windows.sh rust             # one stage
scripts/build-windows.sh guest            # the ISO again, whatever the stamp says
scripts/win-run.sh launcher               # the launcher, out of the checkout
GDB=1 scripts/win-run.sh player ...       # the player under gdb
scripts/package-windows.sh                # the zip ("Packaging on Windows")
```

`scripts/win-run.sh` stands in for the one-folder package: it puts
`build/win/qemu` on `PATH` for the embed DLL, names the winit player to
the launcher only when `player-mitsuami` is not built (once built, the
launcher finds that one itself and it is the default; `LAUNCHER_PLAYER_BIN`
picks either), and names the executor and DXVK (copied to `dxvk_d3d9.dll`) to
QEMU. The launcher writes nothing to the terminal, so paste the
`[player] …` line from `launcher.log` after `GDB=1 scripts/win-run.sh
player`.

Notes on the build, and why:

- **Python is MSYS2's own 3.14**, with `python-distlib`. MSYS2 has no
  older one, and a python.org venv has `Scripts\` where configure looks
  for `bin/`. QEMU 11.1 builds on 3.14 with the real `distlib`, and
  `configure-qemu.sh` accepts 3.14 only then. (9.2's mkvenv gave pip a
  `file://C:/…` wheels URL, a host named `C:`; patch 69 fixed that
  until 11.0 passed a plain path.)
- **Optional libraries are pinned off.** QEMU links what it detects, and
  MSYS2 has zstd, gnutls and others the package does not ship, so
  `configure-qemu.sh` disables each one.
- **lld is named through meson's `CC_LD`.**
- **Paths in meson's files are `C:/…`** (`cygpath -m`).
  `qemu-embed/build.rs` strips `canonicalize`'s `\\?\` prefix, because
  the linker appends `/libqemu-embed-…` and `/` is not a separator in a
  verbatim path.
- **Package versions follow MSYS2.**
- **Prepare is skipped by `build.sh`'s stamp** (`build/.stamp-qemu-prepare`,
  the same inputs and file). It used to run every time, and since it
  rewrites every patched file, ninja rebuilt all of QEMU (~1400 steps,
  4 minutes) on each run. The stamp is removed before a prepare and
  written once it finishes, so an interrupted prepare is redone; `-f`
  prepares regardless.
- `prepare-qemu.sh` runs every git through `qgit`, which retries on
  `Unable to create index.lock: File exists`, a scanner holding the lock
  of the git that just exited (00-status, "Building").

**The guest-tools ISO** is rebuilt by a default run when its sources
moved, by `scripts/build.sh`'s stamp (`build/.stamp-guest-tools`: the
guest sources, the protocol and register headers, the build scripts and
qemu-3dfx's revision; one file for both scripts, since one checkout holds
one ISO). Until 2026-10-03 a default run only checked that an ISO
existed, and one from before M16 failed the XP Direct3D checks with
`CreateDevice failed 0x8876086c` (a vs 1.1 / ps 1.4 driver). It comes
from the same scripts as on Linux
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

## The WDDM driver

Windows 7's WDDM display driver (track M18 step 2, ADR-022) builds on
the PC with Microsoft's toolchain and the WDK's own headers, read from
the kit and never committed. It is `build-windows.sh`'s `wddm` stage
(since 2026-10-04), which runs `guest-tools/build-wddm.cmd` below when
an EWDK is mounted (or named by `EWDK` / `EWDK_ISO`) and is skipped with
a note otherwise; the `guest` stage after it puts the result on the
guest-tools ISO, in `WDDM\`.

**Linux and macOS ISOs get the PC's build** (2026-10-04, user):
`scripts/wddm-prebuilt.sh` names the driver by a hash of the sources it
is built from (`wddm/`, the shared `core/`, `d3dpt_fb.h`, `d3dpt_enc.h`,
`d3dpt_proto.h`, `build-wddm.cmd`; `wddm-prebuilt.sh key`). The `wddm`
stage writes that hash to `build/wddm/x86/.key`, and `build-windows.sh
wddm --publish` uploads the three files as `wddm-<hash>.tar.gz` to the
repository's `wddm-prebuilt` release (a prerelease that holds nothing
else; `gh`, logged in). It refuses a build whose `.key` is not the
checkout's and sources with uncommitted changes, so a name always means
committed sources. `scripts/build.sh`'s `guest` stage on Linux or a Mac
runs `wddm-prebuilt.sh fetch` first, which downloads the asset for its
own hash into `build/wddm/x86` (and removes one fetched for other
sources); the ISO's stamp then sees the driver. Nothing published for
the hash means an ISO with no `WDDM\` and a note, so **after a commit
that changes the driver's sources, publish from the PC** or the other
hosts' ISOs lose it. `WDDM_PREBUILT=0` never fetches. On the PC with no
EWDK mounted, the `wddm` stage fetches too.

**The kit: the Enterprise WDK for Windows 10, version 2004**
(10.0.19041, with VS 2019 Build Tools 16.7). It is the last WDK that
targets Windows 7 (Microsoft lists the 2004 kit as "supported for Windows
7/Windows 8/Windows 8.1 driver development only"), and the 22000 kits
also dropped 32-bit kernel drivers. Newer Visual Studio versions do not
replace it. The EWDK is one self-contained ISO, mounted rather than
installed, so it sits beside any other Visual Studio without touching
it: `EWDK_vb_release_svc_prod1_19041_201201-2105.iso`, 13.2 GB, from
Microsoft's "Supported and other WDK download versions" page (the EWDK
link under Windows 10 2004, accepting the license). On the user's PC it
lives in `D:\stuff\downloads\`.

```sh
scripts/build-windows.sh wddm           # the stage alone (MINGW64)
scripts/build-windows.sh wddm --publish # ... then publish it for Linux and macOS
cmd //c guest-tools\\build-wddm.cmd     # or the script itself, from MSYS2, Git Bash or cmd
```

The script finds a mounted EWDK on any drive (or `EWDK=E:`; or
`EWDK_ISO=<path>` mounts it first), sets up its x86 environment and runs
MSBuild on `guest-tools/src/d3dptvid/wddm/km/d3dptkmd.vcxproj`
(`WindowsKernelModeDriver10.0`, `TargetVersion=Windows7`, Win32, `/W4
/WX`, `displib.lib` for `DxgkInitialize`) and on
`wddm/um/d3dptumd.vcxproj`, Direct3D 9's user-mode driver (the kit's
`v142` user-mode toolset, Win32, `/W4 /WX`, `/arch:IA32` for guest
code's Pentium III floor, the C runtime linked in statically because
Windows 7 has none of VS 2019's (its own SSE2 routines are picked by a
CPU check at run time, so the ISO stages the DLL after its pentium3
check), subsystem 6.01; the
shared core's `core_caps.c` / `core_surf.c` compiled in), into
`build/wddm/x86/`: `d3dptkmd.sys`, `d3dptumd.dll` and `d3dptkmd.inf`
(which installs both). The drivers are unsigned; 32-bit Windows 7 loads
them after a prompt. Notes:

- The kernel-mode include path has no `<stdint.h>`, which
  `d3dpt/d3dpt_fb.h` includes, and MSVC's own pulls in the user-mode CRT:
  `wddm/km/kinc/stdint.h` is the typedefs alone.
- `dumpbin -headers -imports` (from the EWDK's MSVC `bin\Hostx86\x86`)
  is the check that a build still loads on Windows 7: machine `x86`,
  subsystem version 6.01, imports from `ntoskrnl.exe` only (the DLL:
  `KERNEL32.dll` and `GDI32.dll`'s two `D3DKMT*` calls, nothing
  `api-ms-win-*`).
- The shared core's `d3dpt_ddi.h` marks a nameless union with mingw's
  `__GNU_EXTENSION`, which the DLL's project defines empty, and the core's
  sources include `<windef.h>` first, which MSVC's SDK takes only after
  `<windows.h>` (forced in for those two files).
- MSBuild's "'pwsh.exe' is not recognized" after the link is a WDK
  post-build step looking for PowerShell 7; it changes nothing.

## Running it there

Unzip anywhere and run `2ksbox.exe`; machines, `discs.toml`, shader
profiles and downloaded presets live in `%APPDATA%\2ksbox\data` (a Store
install keeps them in `%USERPROFILE%\2ksbox` instead). When
something misbehaves, run `2ksbox-debug.bat` and send
`2ksbox-debug.log`. `PLAYER_KEYBOARD_LOG=1`
adds the keyboard capture's decisions to `player.log`.
