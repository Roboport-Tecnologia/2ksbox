# Building and testing on macOS (Apple Silicon)

Everything here runs natively on arm64; the test machine is an M1
MacBook Air. This doc covers setting a Mac up, building, running a
guest, the app and its two builds (ADR-019), and the macOS floor they
target. The stages themselves are in `docs/development.md`; Mac traps
that cut across subsystems are in `docs/00-status.md` ("Running on a
Mac").

## One-time setup

```sh
xcode-select --install                       # Apple clang + git
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config gnu-sed uv   # build tools only ("The libraries" below)
brew install mingw-w64 xorriso nasm mtools   # guest-tools ISO, the Wine pair, the DOS batteries
brew install autoconf automake libtool       # libtpms (the Windows 11 TPM) builds from its git tarball
brew install llvm lld                        # Windows 11 on Arm's firmware (scripts/build-edk2.sh)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

- **No library the app carries comes from Homebrew.** `build.sh`'s
  `deps` stage (`scripts/build-deps.sh`) builds QEMU's libraries (glib,
  pixman, libslirp, zstd, and libtpms with OpenSSL's libcrypto; static;
  and spice-protocol's headers)
  from pinned upstream tarballs into `build/deps/<arch>`, and
  `configure-qemu.sh` and the packager use nothing else. meson, ninja
  and pkg-config are needed to build them and ship nothing.
- **The launcher needs nothing extra.** `launcher-mitsuami` (ADR-023)
  is on AppKit, which every Mac has; `build.sh`'s `mitsuami` stage is a
  plain `cargo build --release` in `launcher-mitsuami/`. Until
  2026-10-02 the launcher was Qt, and the `deps` stage built Qt from
  source too.
- **llvm and lld** build EDK2 for the aarch64 `virt` board (`build.sh`'s
  `edk2` stage, `scripts/build-edk2.sh`, track M20 step 4): Apple's
  clang cannot link the ELF images EDK2 turns into PE. The firmware is
  ours because QEMU's prebuilt one has neither Secure Boot nor an AHCI
  driver. Build tools only, like the rest of this list.
- **gnu-sed**: qemu-3dfx's `sign_commit` uses GNU `sed -i`, so
  `prepare-qemu.sh` puts gnu-sed's `gnubin` first on `PATH`.
- **No XQuartz and no SDL2.** QEMU has no display of its own
  (`--disable-sdl --disable-cocoa …`; our qemu-3dfx port needs no SDL2),
  and patch 70 replaces
  qemu-3dfx's GLX backend on Darwin with one that only refuses; 3D is
  the player's CGL backend.
- `GL/glcorearb.h` is vendored in `third_party/khronos`, so the build
  never depends on the Mac's headers.

### Vulkan for the Direct3D executor

DXVK's d3d9 (ADR-007) runs over Mesa's **KosmicKrisp**, which needs
macOS 26 and is an optional component of the LunarG SDK. DXVK refuses
MoltenVK (`docs/spikes/spike-c-dxvk-native-macos.md`).

```sh
brew install vulkan-headers vulkan-loader vulkan-tools glslang
curl -sSL -o vulkan-sdk.zip https://sdk.lunarg.com/sdk/download/latest/mac/vulkan-sdk.zip
unzip -q vulkan-sdk.zip                      # vulkansdk-macOS-<ver>.app
V=1.4.357.1                                  # the version the zip carried
vulkansdk-macOS-$V.app/Contents/MacOS/vulkansdk-macOS-$V --root ~/VulkanSDK/$V \
  --accept-licenses --default-answer --confirm-command install
~/VulkanSDK/$V/MaintenanceTool.app/Contents/MacOS/MaintenanceTool \
  --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan.kosmic
export VK_ICD_FILENAMES=~/VulkanSDK/$V/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json
vulkaninfo --summary | grep -E "driverName|apiVersion"   # KosmicKrisp, 1.4.x
```

The installer will not add a component to an existing root, hence the
maintenance tool. SIP strips a `DYLD_*` variable exported to a
`#!/usr/bin/env bash` script (symptom: `Direct3DCreate9 failed`), so
`scripts/test.sh` sets the Vulkan environment itself on Darwin:
Homebrew's loader on `DYLD_LIBRARY_PATH`, and the SDK's KosmicKrisp ICD
unless `VK_ICD_FILENAMES` is set. Put **only**
`/opt/homebrew/opt/vulkan-loader/lib` on `DYLD_LIBRARY_PATH`, never
`/opt/homebrew/lib` (every image decode dies with `SIGBUS`; 00-status),
including in the shell a launcher starts from.

A launcher or player started from a checkout with none of that set
falls back to the SDK itself (2026-10-01, user): the newest
`~/VulkanSDK/<version>/macOS` with KosmicKrisp in it, the one
`package-macos.sh` ships. The launcher's probe opens its loader by full
path when no `libvulkan` loads by name (`host_gpu::sdk_loader`, reported
as "the Vulkan SDK's") and names its KosmicKrisp manifest
(`announce_driver`); the player opens the same loader by full path
before QEMU starts, which makes the executor's and DXVK's leaf-name
`dlopen`s return it (dyld matches the loaded image), and sets
`VK_DRIVER_FILES` (`companions::checkout_vulkan`). Before, both found
no loader there, and Automatic meant Wine. A `DYLD_LIBRARY_PATH` or
`VK_ICD_FILENAMES` of your own still wins.

### Open Watcom, for the Win98 display driver

The 16-bit `.drv` and the ring-0 `.vxd` (doc 19) build with Open
Watcom. Its snapshot has an arm64 macOS set (`armo64`) beside
Linux's `binl64`; `build-driver9x.sh` picks by `uname`.

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

Set `WATCOM=` if it lives elsewhere. The Mac's `d3dpt9v.vxd` is
byte-identical to Linux's; `d3dpt9x.drv` differs by one instruction
selection with the same semantics. `wdis` segfaults on our 16-bit
objects on both hosts.

## Building

```sh
git clone --recurse-submodules --shallow-submodules git@github.com:Roboport-Tecnologia/2ksbox.git
cd 2ksbox
scripts/build.sh            # QEMU ~10–15 min on the Air, the Rust side ~1 min
target/release/player       # the test pattern, through wgpu on Metal
```

The pattern is colour bars, a 1-px white border and a white line
sweeping down (≈ 8 s a pass): check sharp edges, integer-scaled 4:3 and
no tearing, also after a resize.

Mac specifics of the stages:

- **Everything targets the floor** (below). `build.sh` exports
  `MACOSX_DEPLOYMENT_TARGET=$(scripts/macos-floor.sh)`; a hand-run cargo
  must do the same, or rustc links for 11.0. The value is not in cargo's
  fingerprint, so `build.sh` runs `cargo clean --release` on a workspace
  whose binary was linked for a *newer* macOS when it builds that stage.
  `scripts/test.sh` exports the floor too, or the next `build.sh` would
  clean its player away.
- ld's `building for macOS-12.0, but linking with dylib '…' which was
  built for newer version` names a library that is not ours: nothing
  under `build/deps` is built for a newer macOS than the floor, and
  the package fails on any file that is.
- `configure-qemu.sh` passes the target as `-mmacosx-version-min` with
  `-Werror=unguarded-availability-new`, so an API newer than the floor
  without an `@available` check fails the build instead of dying on the
  floor's macOS (`strchrnul`, declared from 15.4 and found by meson
  anyway, was patch 46 until QEMU 10.1 fixed it upstream).
- `configure-qemu.sh` uses uv's Python only; a Python complaint means uv
  is not on `PATH`. Under Rosetta (`--x86_64`) it takes uv's x86_64
  build of the same version, because meson takes the machine its
  interpreter runs on for the build machine.
- Homebrew's mingw is symlinked into `/opt/homebrew/bin`, so
  `build-driver.sh` asks the compiler for its sysroot to find the DDK
  headers. `build-wrappers.sh` is `set -e` and writes the ISO last, so
  an ISO older than its sources means a stage died.

## Running a guest

**A bare `qemu-system-i386` has no display, audio backend or 3D**; the
embed library brings the 3D provider (patch 30) and the audiodev (patch
20). With no `-display` QEMU serves VNC on `localhost:5900`, so pass
`-display vnc=:0` and `-audiodev none,id=snd`. A guest asking it for 3D
is refused and keeps running.

```sh
build/qemu/qemu-system-i386 --version        # 11.1.2
printf 'info mtree\nquit\n' | build/qemu/qemu-system-i386 -machine pc -display none \
    -monitor stdio -net none 2>/dev/null | grep -E 'mesapt|glidept'
# expect the Mesa pass-through region and no glidept one (the qemu-3dfx port builds no hw/3dfx)
```

The player, as a machine runs it (`launcherx --print-args` prints the
launcher's exact line):

```sh
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc,hpet=off -cpu pentium3 -m 256 -hda <overlay>.qcow2 \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -device sb16,audiodev=embed0
# XP: -m 512 -device AC97,audiodev=embed0
```

On the Air, Win98 with crt-lottes measures p50 6–10 ms, p95 15–17 ms
publish→present (the vsync-phase floor). `-cpu pentium3` is the guest
tools' floor (SSE1, msvcrt); qemu-3dfx's `-cpu max` advice is for its
x86-64-v2 wrappers. Win9x wants at most 512 MB (VCache).

### 3D in the player

The macOS embed backend (`embed/mglcntx_embed.c`) is a drawable-less
CGL context handing frames to the player through an IOSurface ring
(`player-core/src/iosurface.rs`); doc 12 has the design. On a GL guest
(wglgears) expect:

```
glcntx: CGL (window-less)          renderbuffers 1/2 800x600
GL 2.1 Metal … / Apple M1          default FBO 1 (bound 1) … complete
glcntx: zero-copy: IOSurface ring  [3d] slot 0: imported 800x600
[3d] pass-through on               … and [3d] pass-through off on exit
```

An incomplete FBO publishes no frames (the desktop freezes); the
`glcntx:` lines name the failing call. `zero-copy off: <reason>` or
`[3d] slot N: zero-copy import failed` means readback carries the
frames.

### A Win98 guest by hand

The install must come out **ACPI** (doc 06), which a plain `SETUP`
does. Install on the Cirrus (Windows' in-box driver), then run
`D:\SETUP.EXE /ALL` from the guest-tools ISO for the rest, our display
driver included:

```sh
build/qemu/qemu-img create -f qcow2 ~/vms/win98.qcow2 4G
build/qemu/qemu-system-i386 -machine pc,hpet=off -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom ~/isos/Win98SE.iso -boot d \
  -vga cirrus -display vnc=:0 -net none -audiodev none,id=snd -device sb16,audiodev=snd
```

**Repairing a PnP-BIOS image** (installed before the BIOS stamp:
`info usb` shows the tablet, Windows shows nothing, Device Manager has
"Plug and Play BIOS" with a yellow !). Copy the CD's `WIN98` folder to
`C:\` first (the CD driver goes away mid-way), then Device Manager →
System devices → "Plug and Play BIOS" → Update Driver → "Display a
list…" → Show all hardware → "PCI Bus". Windows re-detects every device.

**The OpenGL check.** `SETUP /ALL` copies `TESTS\` into `C:\2KSBOX`,
and `SETUP /GAME 3 C:\2KSBOX` puts qemu-3dfx's `OPENGL32.DLL` beside
them. In the player `WGLGEARS.EXE` passes with smooth gears, a host
renderer string (not "GDI Generic") and `mesapt: DLL loaded` on stderr;
`TESTS\GLPROBE.EXE` answers the same in a log.

`tools/win98-driver-test.sh` runs here too. It needs only `mtools` from
outside the tree (ImageMagick's `identify` is optional; without it the
colour count reads `?`) and an ACPI image: on a PnP-BIOS one nothing
matches the INF and the run ends on the in-box VGA.

## The app

`scripts/package-macos.sh` makes the signed, notarized `.app` with the
JIT entitlement (doc 07) for a Mac with nothing installed:

```sh
scripts/package-macos.sh                       # build, stage, check, sign, notarize, dmg
scripts/package-macos.sh --no-sign --no-dmg    # the staging and its checks alone, ~20 s
scripts/package-macos.sh --community           # ADR-019's community build (below)
scripts/package-macos.sh --x86_64 --no-notarize # the Intel app, from scripts/build.sh --x86_64 (below)
```

`--no-build`, `--no-notarize`, `--identity`, `--keychain-profile` and
`--out` are in the script's header. On Apple Silicon the app also
carries Windows 11 on Arm (track M20): `MacOS/2ksbox-player-aarch64` on
`lib/2ksbox/libqemu-embed-aarch64.dylib`, our EDK2 pair in `pc-bios/`
and `share/2ksbox/drivers/2ksbox-drivers-arm64.iso`, from `build.sh`'s
`qemu`, `edk2`, `virtio` and `rust` stages; a missing one fails the
package. The Intel app has no Windows 11. Notarization credentials, once:

```sh
xcrun notarytool store-credentials 2ksbox-notary \
    --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
```

**The bundle is the prefix.** `Contents` has doc 07's `lib` /
`libexec` / `share` layout, found by the same `share/2ksbox` marker.
`MacOS/` does `bin/`'s job (Launch Services starts programs only from
there); `paths::bin_dir()` derives it from the running executable, so a
plain tarball on a Mac is still a Unix prefix.

**Nothing may come from outside the bundle.** Our own libraries go into
`Contents/lib/2ksbox`, every install name becomes `@rpath`, and **every
`LC_RPATH` pointing out of the app is deleted**. QEMU's libraries are
static in `libqemu-embed` and `qemu-img` ("The libraries" below), so
the non-system dylib closure the staging walks is empty on purpose; the
walk stays, because a library found under `/opt/homebrew` or
`/usr/local` is a configure that went wrong, and the `no-optionals`
check fails on the same thing earlier.

**The launcher brings no toolkit.** `MacOS/2ksbox` is
`launcher-mitsuami` (ADR-023) on AppKit, which every Mac has, so the
app has no `Frameworks`, `PlugIns` or deployment step of a toolkit; the
dylib closure above is the whole story. (Until 2026-10-02 the launcher
was Qt and `macdeployqt` filled those directories.)

**The ad-hoc re-sign finds every Mach-O by file type, not mode.** An
arm64 binary whose load commands changed under its signature is killed
silently (`SIGKILL` and nothing else), so the staging re-signs every
Mach-O ad hoc after rewriting install names; the Developer ID signature
replaces it further down.

The `Info.plist` is written once, at the end of the staging, with
`LSMinimumSystemVersion` measured from the bundle ("The floor" below).
Its `CFBundleIdentifier` is `com.2ksbox.2ksbox` for the App Store
build and `com.2ksbox.2ksbox-community` for the community one (the
registered App IDs; ADR-011: Apple forbids the underscore of
`com._2ksbox.Launcher`), so the two install side by side. Neither may
change once shipped.

**The Vulkan driver** is the one companion no load command names. The
app carries the LunarG loader and KosmicKrisp with its own ICD manifest,
found through DXVK patch 06 (`@loader_path` ahead of bare leaf names).
An installed player sets `D3DPT_EXEC_LIB`, `D3DPT_DXVK_LIB` and
`VK_DRIVER_FILES` when unset
(`player-core/src/companions.rs`); each `dlopen` search otherwise starts in a
`build/` directory. The launcher's probe (`--host-check`) opens the
app's `lib/2ksbox/libvulkan.1.dylib` by full path
(`host_gpu::shipped_loader`) and names the app's ICD to it at `main`
(`host_gpu::announce_driver`), because a leaf-name `dlopen` finds
nothing in the app and the loader reads drivers only from the
environment and system directories. The packager requires that
`--host-check` to say "the app's own".

**The checks are the point of the script:**

- no Mach-O may name an `@rpath` dependency nothing in the bundle
  resolves, expanded as dyld does (the loading executables' rpaths
  included);
- the staged launcher's `--paths`, run with `env -i` from `/`, resolves
  every companion inside the app; a machine created with the packaged
  `qemu-img` gets `-L` pointed at the packaged firmware;
- the packaged player runs under `DYLD_PRINT_LIBRARIES=1` and **every
  image the loader touches** must be inside the app, `/usr/lib` or
  `/System`;
- the staged launcher **opens a real window** (`LAUNCHER_SHOT=<png>`,
  `launcher-mitsuami/src/shot.rs`, under the same loader watch) and
  must write the PNG and load nothing from outside the app. AppKit has
  no offscreen mode, so the window shows for a moment on the
  packager's screen;
- on Apple Silicon, **Windows 11 on Arm end to end** (track M20): the
  staged launcher makes a Windows 11 machine (`--wizard-new win11`,
  `--prepare`), its arguments must name the app's EDK2
  (`pc-bios/2ksbox-aarch64-code.fd`) and drivers disc and ask for HVF,
  and `MacOS/2ksbox-player-aarch64` runs them until the firmware draws,
  then quits through QMP, loading nothing from outside the app; "failed
  to initialize hvf" fails it (the hypervisor entitlement lost in the
  re-sign). `--paths` must name that player and the disc inside the app.

The script's first runs with the AppKit launcher (2026-10-04, on QEMU
11.1, track M21 step 4) passed every check above for the App Store, the
community and the Intel app, unsigned (`--no-sign --no-dmg`).

Signing is inside-out, every nested Mach-O before the bundle that seals
it, with `--options runtime` and `packaging/macos/2ksbox.entitlements`
(`com.apple.security.cs.allow-jit`; without it TCG dies on its first
translated block). Windows 11 on Arm's player is signed with
`packaging/macos/hypervisor.entitlements` instead, in the ad-hoc re-sign
too: `com.apple.security.hypervisor` (HVF answers `HV_DENIED` without
it, signed or not) plus the JIT one. The packager checks the signed
player still has it. Notarization requires `--timestamp`, and Apple's
timestamp service drops out for seconds at a time, so that call alone
retries.

### Two builds (ADR-019)

| | App Store | Community |
|---|---|---|
| macOS | 26+, Apple Silicon | the floor (12.0, "The floor" below); Intel permitted, untested ("The Intel build" below) |
| Direct3D | DXVK on KosmicKrisp | the same, plus the executor on Wine below Vulkan 1.3 |
| Distribution | App Store | Developer ID DMG (`--community`) |

Both send the host's shortcuts to the guest the same way, through the
window server's private hot key mode (doc 03 "Input path"): UTM ships
that symbol on the Mac App Store and it works inside the sandbox, so
the App Store build is not held back to `PLAYER_KEYBOARD_MAC=presentation`
(Cmd+Tab and Cmd+H only) unless review ever objects.

Both come from the same script. `--community` adds
`libd3dpt_exec_remote.dylib`, `wine/d3dpt_exec.dll` and
`d3dpt-exec-host.exe`, built by `scripts/build-d3dpt-exec.sh --wine`
with mingw-w64 (ADR-018, doc 14). The App Store build carries nothing of
Wine and never gets a pre-26 version.

Below macOS 26 KosmicKrisp loads but reports no GPU
(`vkEnumeratePhysicalDevices` fails), so the community build runs the
same executor on the user's Wine (`2ksbox --host-check` says "runs through Wine on this
host", exit 0). A Mac with no Wine has no Direct3D pass-through. **No
package ships a Wine**;
the launcher's note says which to install:

- Homebrew's Wine casks are disabled (not notarized), so the options are
  WineHQ's tarball from Gcenx's releases (x86_64, under Rosetta on Apple
  Silicon) or CrossOver. A packaged app finds it in
  `/Applications/Wine {Stable,Staging,Devel}.app`, on `PATH` or through
  `D3DPT_WINE`; a checkout also looks in `build/wine/`.
- Native arm64 Wine has no OpenGL in `winemac.drv` (macOS gives the GL
  compatibility renderer only to Rosetta processes).
- **A macOS VM cannot test this path** (no accelerated OpenGL, which
  Wine's Mac driver requires). A pre-26 macOS on a second APFS volume
  can (`tools/macos-wine-spike-local.sh`; loop in M15's track doc).
- The `exec-wine` check skips over ssh, because Wine's Mac driver needs
  the window server.

The community build permits Intel, untested, because its Wine is
x86_64 on both architectures (ADR-019 has the reasons). No doc claims
Intel until an Intel Mac has run the reference scene.

### The Intel build

**Status (2026-09-24): builds and packages end to end on the Air.** The
first version of this recipe needed an Intel Homebrew, and Homebrew's
installer refuses one ("Homebrew on macOS is only supported on Apple
Silicon processors!", Homebrew 7.0.6's `install.sh`); that is what
ended the app's dependence on Homebrew ("The libraries" above). The
Intel build needs no second package manager: its libraries come
from `build-deps.sh --arch x86_64` (its meson builds get a cross file
naming x86_64, with `subsystem`, `kernel` and the Objective-C compiler
glib asks for), its Python from uv, its Rust from the same rustup. The
staged app passes every packager check under Rosetta (every Mach-O
x86_64, minimum macOS 12.0, the loader's images all inside the app, the
window), and `scripts/test.sh` runs that as `package-x86_64`. That was
measured with the Qt launcher, and again with the AppKit one on QEMU
11.1 (2026-10-04).
What is left is an Intel Mac for the reference scene; the DMG stays
"untested" until then.

The Intel app is the community build and nothing else: macOS 12 (the
floor), no App Store version, and **no Vulkan at all**, since KosmicKrisp
exists only as arm64 and MoltenVK is refused (ADR-007). So it carries no
loader, no ICD, no DXVK and no in-process executor; its Direct3D is the
executor on Wine (native x86_64 Wine there, no Rosetta), and a Mac with
no Wine has none. It is made on the Apple Silicon Mac, under Rosetta:

```sh
scripts/build.sh --x86_64                          # deps, qemu, rust, mitsuami, exec; dxvk is skipped
scripts/package-macos.sh --x86_64 --no-notarize    # build/macos-x86_64/2ksbox-<version>-macos-x86_64.dmg
scripts/package-macos.sh --x86_64 --no-sign --no-dmg   # the staging and its checks alone
                                                   # (scripts/test.sh's package-x86_64 check)
```

How it works, so it stays one build and not a second tree of scripts:

- `--x86_64` re-runs the script as an x86_64 process (`arch -x86_64`),
  and that is all. Under Rosetta `uname -m` and Apple's compiler answer
  x86_64 without being told, and `arch -x86_64 scripts/build.sh` is the
  same build. Build tools stay the native ones: an arm64 program runs
  from a Rosetta shell, and meson, ninja, mingw and xorriso are
  arm64 programs whose output is what their flags say.
- Every script that has a build directory recognises the translated
  process (`sysctl.proc_translated`) and keeps to `build/x86_64/`,
  `build/deps/x86_64/` and `target/x86_64-apple-darwin/` beside the
  native build's (`configure-qemu.sh`, `build-d3dpt-exec.sh`,
  `build-deps.sh`, `qemu-embed/build.rs`, `build.sh`,
  `package-macos.sh`), the way the Windows build keeps
  `build/win/`. The `qemu/` and `third_party/dxvk` trees and their
  prepare stamps are shared, so the two builds run one after the other,
  never at once.
- meson takes the machine its interpreter runs on for the build machine,
  so `build-deps.sh` gives its meson builds a cross file naming x86_64
  (pixman picks its SIMD paths by `host_machine.cpu_family`), and
  `configure-qemu.sh` runs QEMU's meson on uv's x86_64 Python
  (`uv python install cpython-<version>-macos-x86_64-none`, done for
  you).
- cargo is never translated (rustup's toolchain is arm64) and simply
  cross-compiles with `--target x86_64-apple-darwin`; `cc` adds
  `-arch x86_64` for that target. The launcher is built the same way,
  into `launcher-mitsuami/target/x86_64-apple-darwin/`.
- The packager checks the architecture of every Mach-O in the app
  beside the floor: a file that is not `x86_64` (an arm64 KosmicKrisp,
  say) fails the package. On an Intel Mac itself nothing is translated,
  and the same scripts make its native app with the same checks.

What the Air can and cannot prove: the staged app's own checks run under
Rosetta (the loader's image list, the window, `--host-check`,
the wizard), and the app can be opened under Rosetta for a look. TCG's
x86-64 backend and the Voodoo 2's SSE2 rasteriser are the Linux rig's
every day. But Rosetta translates the JIT's output and says nothing about
speed, and no Intel Mac is among the test machines, so the DMG goes on
the release page labelled untested until one has run the reference scene
(ADR-019).

### The libraries

User decision (2026-09-23): **depending on Homebrew for what the app
carries was a mistake.** Homebrew builds every library for the macOS it
runs on and publishes bottles for three releases back, so the app's
floor was Homebrew's floor; its installer refuses Intel Macs since
Homebrew 7, which ended the Intel build before it started; and a `brew
upgrade` changed what shipped. So the libraries are built here, from
upstream, and Homebrew is a source of build tools and of recipes to crib
flags and patches from, nothing more.

`scripts/build-deps.sh` (the `deps` stage of `build.sh`, macOS only)
builds everything from tarballs pinned by sha256 in the script, for the
architecture named (`--arch x86_64` for the Intel build) and
`MACOSX_DEPLOYMENT_TARGET`, into `build/deps/<arch>`; each package is
stamped by name, version and floor, so a floor change rebuilds all and
a recipe change for the same version wants `--clean`.

- **QEMU's side**: glib 2.90 with pcre2 10.48 and the libffi and stub
  libintl that glib's own tarball carries as subprojects, pixman 0.46,
  libslirp 4.9 and zstd 1.5, as **static archives** whose `.pc` files
  carry their private link lines publicly. `configure-qemu.sh` sets
  `PKG_CONFIG_LIBDIR` to that prefix and the SDK's own `.pc` files, so
  `libqemu-embed-i386.dylib`, `qemu-system-i386` and `qemu-img` carry
  the libraries and link nothing outside the system; a library QEMU
  would auto-detect from Homebrew (libpng, jpeg-turbo) is simply not
  found, and `--disable-png --disable-vnc-jpeg` say so on purpose. Not
  meson's `prefer_static`: QEMU turns it into `-static`, fatal on macOS.
- **libtpms 0.10.2 with OpenSSL 3.5's libcrypto**, static, for the TPM
  2.0 behind `-tpmdev libtpms` (track M20, patch 75).
- **spice-protocol 0.14.5's headers**, nothing linked: QEMU builds its
  `qemu-vdagent` chardev with them, the clipboard's channel to a guest
  agent (track M23). Its `.pc` is moved from `share/pkgconfig` into the
  prefix's `lib/pkgconfig`.
- **No toolkit.** Until 2026-10-02 the script also built Qt 6.9.3 as
  frameworks for the Qt launcher (and carried two qtdeclarative patches
  for the macOS style's button margins); the AppKit launcher needs
  none of it, and that part and its patches are gone.
- **Our patches on a package** live in `patches/deps/<name>/` and are
  applied to the unpacked tarball (`patches/deps/README.md` lists
  them). Each package's build stamp carries a hash of its patch
  set: editing one rebuilds that package on the next `build.sh`,
  nothing else.

On Linux, the Flatpak included, the script builds only QEMU's GLib
(with pcre2 and libslirp), libtpms with libcrypto and spice-protocol's
headers (`docs/development.md`, "The build, stage by stage"); `QEMU_DEPS=system`
takes the distribution's instead. What the Mac app's closure once was,
for the record: 43 Qt frameworks and about 30 dylibs (ICU, dbus,
OpenSSL, tiff, webp, jasper, lcms2, brotli and the rest) from
Homebrew's bottles, swapped for the floor's builds by a script that is
gone with them.

### The floor

The app runs down to **macOS 12 (Monterey)**, the number in
`scripts/macos-floor.sh`. It was set by Qt 6.9, the newest Qt line that
still ran on 12, while the launcher was Qt. The launcher is now
`launcher-mitsuami` on AppKit (ADR-023, 2026-10-02), and 12 stands until
a Mac that old has run it: mitsuami's own AppKit floor is not measured
yet. QEMU, glib, pixman, libslirp, zstd, the LunarG loader, KosmicKrisp
and Rust all go lower. Until 2026-09-23 the floor was Homebrew's (15.0),
because the app carried Homebrew's bottles; it moved the day the
libraries became ours ("The libraries" above). Changing the number and
running `scripts/build.sh` retargets everything.

Three pieces make the claim true:

- **Everything of ours is built for the floor**, through the deployment
  target and the availability error flag above (a new floor recompiles
  QEMU and DXVK).
- **The libraries are built for it too** (`scripts/build-deps.sh`
  passes the target to meson, configure and make alike), so no file in the
  app is a build for a newer macOS.
- **The package fails above it.** `LSMinimumSystemVersion` is the
  highest `LC_BUILD_VERSION` `minos` in the bundle, and any Mach-O above
  the floor fails `package-macos.sh` by name. The "still links" check
  skips a file's own install name, the first line `otool -L` prints,
  because a library built elsewhere keeps its absolute one. The LunarG
  loader and KosmicKrisp are 11.0 builds and never set the minimum.
