# 2ksbox

Run Windows 98, Windows XP and DOS the way the machines of the era did.
Period 3D games run accelerated on your GPU, the picture goes through a
CRT shader instead of a blurry stretched rectangle, and the CD-ROM drive
runs raw dumps of the discs you own, copy protection included.

Built on a patched QEMU. Runs on Linux, Windows and macOS (Apple Silicon).
Free software, GPL-2.0.

## What it does

- **A machine library, not a command line.** The launcher creates a
  Windows 98, Windows XP, DOS or "Other" (BeOS, a period Linux, OS/2)
  machine from a short wizard with defaults for each family. Each machine
  opens in its own player window.
- **Real 3D in the guest.** DirectX 1 up to 9 through our own
  paravirtual display adapter and driver, OpenGL passed through to the
  host, and an emulated 3dfx Voodoo 2 running 3dfx's own driver for
  Glide games. Quake II, Unreal Tournament, GTA
  Vice City, Max Payne and Need for Speed: Porsche Unleashed all run.
- **A CRT on your monitor.** The guest's own framebuffer, at its native
  resolution and aspect (320×200 included), through a libretro slang
  shader chain, with shader profiles and a live preview in the launcher.
- **Your discs.** cue/bin, CloneCD, Alcohol, ISO, and any folder on your
  disk served as a CD. Every machine shares one disc shelf; swap discs
  while a machine runs, from the launcher or from inside the guest. CD
  audio plays.
- **Music.** A Sound Blaster 16 with a real OPL3, and an MPU-401 with
  General MIDI (a bank is included) or a Roland MT-32 (bring your own
  ROMs).
- **Gamepads.** Your PlayStation, Xbox or Switch controller reaches the
  guest as a generic USB controller or a DOS gameport joystick.
- **Snapshots and clones.** Named snapshots ("fresh install", "before
  game X") and one-click copies of a whole machine.
- **DOS at period speed.** A DOS machine's processor is throttled to a
  chosen rate for speed-sensitive games (not yet widely tested).

Not the goal: cycle-accurate emulation of specific chipsets (that is
86Box and PCem), modern guests, or piracy. Use only your own install
media, licences and disc dumps.

## What you need

| Host | Requirements |
|---|---|
| Linux | An x86-64 machine. KVM for near-native XP (optional; Windows 98 is emulated on purpose). A GPU with Vulkan 1.3 for the fast Direct3D path. Without it, Direct3D runs through Wine on the host if Wine is installed; with neither, the guest has no Direct3D (OpenGL and the Voodoo 2 still work). |
| macOS | Apple Silicon, macOS 15 or newer. Guests are emulated (no x86 virtualization on these Macs) and still run faster than a period PC. The fast Direct3D path needs macOS 26; on older releases Direct3D runs through Wine if it is installed, and with no Wine the guest has no Direct3D (OpenGL and the Voodoo 2 still work). |
| Windows | 64-bit Windows 10 or 11. WHPX (the Windows Hypervisor Platform) accelerates XP when it is enabled. Without Vulkan 1.3, Direct3D runs on Windows' own Direct3D 9. |

You also need install media for the guest operating system (your own
Windows 98 / XP CD image, a DOS floppy or CD) and, for games, dumps of
your own discs.

## Getting 2ksbox

There are no downloadable packages yet, so build 2ksbox from source.
Once the tools are installed the build is one command, and it produces
the same launcher, player and guest-tools disc as a packaged release.

## Building from source

### 1. Install the tools

**Linux.** Developed on Arch; the Debian/Ubuntu column was checked on
Debian 12 and 13 and Ubuntu 24.04 and 26.04.

| Purpose | Arch | Debian / Ubuntu |
|---|---|---|
| Compilers and build tools | `base-devel git ninja meson pkgconf` | `build-essential git ninja-build meson pkg-config` |
| QEMU's libraries | `pixman zlib libffi mesa libx11` | `libpixman-1-dev zlib1g-dev libffi-dev libgl-dev libx11-dev` |
| The launcher (Qt 6) | `qt6-base qt6-declarative` | `qt6-base-dev qt6-declarative-dev qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-dialogs` |
| Direct3D executor (optional) | `vulkan-headers vulkan-icd-loader glslang` | `libvulkan-dev glslang-tools` |
| Direct3D through Wine, for a GPU without Vulkan 1.3 (optional) | `mingw-w64-gcc wine` | `g++-mingw-w64-x86-64 wine` |
| Guest tools disc (optional) | `mingw-w64-gcc nasm xorriso` | `gcc-mingw-w64-i686 nasm xorriso` |

QEMU gets a GLib of its own: the build downloads GLib, PCRE2 and libslirp
and builds them for it, which needs meson 1.4 or newer. Debian 12 and
Ubuntu 24.04 package older ones; there, `uv tool install meson` (uv is
below) gives a newer one.

The guest tools disc also needs **Open Watcom v2** for the Windows 98
display driver. Unpack the `ow-snapshot.tar.xz`
of its [latest CI release](https://github.com/open-watcom/open-watcom-v2)
into `~/.local/opt/open-watcom`, or point `WATCOM` at it. Without it the
disc is built minus those two, and a Windows 98 machine then has no
display driver.

Then install Rust and uv from their own installers. uv provides the
Python QEMU's build wants; a system Python 3.8 to 3.13 works too with
`QEMU_PYTHON` set to it. Ubuntu 26.04's system Python is 3.14, which
QEMU's build refuses, so there uv is required.

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
curl -LsSf https://astral.sh/uv/install.sh | sh
```

**macOS** (Apple Silicon):

```sh
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson cmake pkg-config gnu-sed uv   # build tools; the app's libraries and Qt are built from source by the build
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Optional on macOS: `brew install mingw-w64 nasm xorriso` and Open Watcom
(as above) for the guest tools disc, and the Vulkan SDK with KosmicKrisp
for the Direct3D executor on macOS 26 (the recipe is in
[docs/build-macos.md](docs/build-macos.md)).

**Windows.** The Windows build is made *on a Linux machine* with podman
or docker and copied over as a zip (step 5).

### 2. Get the source

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/davidrios/2ksbox
cd 2ksbox
```

If you cloned without submodules, run
`git submodule update --init --depth 1`.

### 3. Build

```sh
scripts/build.sh
```

That builds everything this machine has the tools for: QEMU, the Rust
programs, the launcher, the Direct3D executor and the guest-tools
disc. The first build takes about fifteen minutes and several
gigabytes; later ones redo only what changed. The closing summary lists
every stage as built or skipped, with the missing tool for a skipped
one. A skipped optional stage means a missing feature, not a broken
build: install the tool and run the command again.

Check two lines in the summary:

- `qt` must be built, or there is no launcher.
- `guest` should be built. Without the guest-tools disc there are no
  guest drivers, and a machine has plain VGA and no 3D.

After every `git pull`, run `scripts/build.sh` again.

### 4. Run it

```sh
launcher-qt/target/release/launcher-qt
```

Nothing has to be installed: the launcher finds the player, QEMU, the
firmware and the guest-tools disc in its checkout. Machines, discs and
profiles go under your user data directory
(`~/.local/share/2ksbox` on Linux, `~/Library/Application Support/2ksbox`
on macOS).

### 5. Windows

The Windows build is a cross build on Linux, in a podman (or docker)
container the first command makes:

```sh
scripts/win-cross.sh --build      # once: the cross container (~5 min, ~3 GB)
scripts/build-windows.sh
scripts/package-windows.sh
```

The result is `build/win/package/2ksbox-<version>-windows-x86_64.zip`.
Unzip it anywhere on the Windows machine and run `2ksbox.exe`. The
details, and a native build in MSYS2 for debugging, are in
[docs/build-windows.md](docs/build-windows.md).

## First run

1. **Shader presets.** The first time the launcher starts with no
   shader collection, it offers to download libretro's. Say yes: the
   starter profiles are made from it. A source checkout already has it.
2. **Create a machine.** *New machine* walks through family (Windows 98,
   Windows XP, DOS, Other), name, memory, processor, acceleration,
   networking, pointer, disk size and install media. The defaults suit
   the family; change anything later in the machine's settings.
3. **Install the operating system.** Point the install media at your
   Windows CD image and start the machine.
4. **Install the guest tools.** Open the disc shelf, press *Add
   guest-tools ISO*, and put it in the machine's CD drive. Inside the
   guest, run `SETUP.EXE` from that drive (`D:\SETUP.EXE /ALL` from the
   Run box installs everything this Windows can use: the display driver,
   the device mapper, the OpenGL pass-through, the disc-shelf program),
   then restart. `SETUP /LIST` shows what is on the disc; its
   `README.TXT` explains every folder.
5. **Take a snapshot.** *Snapshots…* on the machine. "Fresh install" is
   the one you will keep coming back to, especially on Windows 98.

## Playing games

- **Install from your dumps.** Add the disc image (or a folder) to the
  disc shelf, put it in the drive, install in the guest. For multi-disc
  installs, swap discs from the launcher or from inside the guest with
  `CDSHELF.EXE` (a DOS box has `CDSHELF.COM`), which the guest tools
  install.
- **DirectX 1 up to 9 games** run through the display driver, nothing to
  copy per game. On Windows 98, install DirectX 9.0c first; 98 SE's own
  DirectX 6.1 gets DirectDraw but no Direct3D.
- **When the host has no Direct3D for the guest** (no Vulkan 1.3 and no
  Wine), the guest has none either: the 2ksbox adapter keeps its 2D and
  DirectDraw, and OpenGL games and the Voodoo 2 still run. Installing
  Wine on the host brings Direct3D back.
- **Glide games** (3dfx) run on the emulated Voodoo 2: turn it on in the
  machine's settings and install 3dfx's own Voodoo 2 driver in the guest.
  The game then uses its own Glide against a real card.
- **OpenGL games** (Quake II and friends) get `OPENGL32.DLL` next to the
  game's EXE: `SETUP /GAME 3 <game folder>`, or copy the disc's
  `OPENGL\` folder.
- **Speed.** On an M1 Mac or a Ryzen 5700X the emulated machine is
  roughly a 1.7 GHz Pentium 4 (circa 2001), and both the M1 and an
  RX 9060 XT with the Ryzen ran era games at comfortable frame rates.
  The *Emulation optimizations* switches ship at the settings that
  measured best; they are for troubleshooting, not tuning.
- **Too fast, too slow, or wrong colours** usually means the game wants
  another setting: a slower processor on a DOS machine, a different
  display adapter, a sound card it knows. The wizard explains each
  choice.

## Day to day

Keys in the player window:

| Keys | What |
|---|---|
| Ctrl+Alt+G | release the mouse grab (a click grabs it again) |
| Ctrl+Alt+K | hand the host's own shortcuts (the Windows key, Alt+Tab) back to the host, or to the guest again |
| Ctrl+Alt+Shift+D | Ctrl+Alt+Del in the guest |
| Ctrl+Alt+Shift+F | windowed full screen on and off |
| Ctrl+Alt+S | save the guest's own frame as a PNG |
| Ctrl+Alt+Shift+S | save what the window shows (scaled, through the CRT shader) as a PNG |
| Alt+F4 | asks before stopping the machine; the window's close button does not |

Windows machines use a "seamless" mouse by default: the host pointer is
the guest's cursor and the window never grabs. Turn it off in the
machine's settings for games that want a real PS/2 mouse.

- **Shader profiles…** names a preset plus your parameter overrides, with
  a live preview against a screenshot. A machine picks a profile by name,
  or stays on "(default)", which is whichever profile you mark **Use as
  default** (CRT Aperture after the first download; "No default" means
  no shader).
- **Clone…** copies a machine whole, disk and snapshots, under a new
  name.
- **Gamepads.** A machine's settings choose whether a pad appears in the
  guest as a USB controller (Windows 98 SE, Me and XP see it with no
  driver) or a DOS gamepad.
- **Music.** A machine's settings choose its sound card and its MIDI
  port. General MIDI plays through the included bank or one of your own.
  The MT-32 needs your own ROMs, which you point the launcher at.

## When something goes wrong

- **"Something is missing."** `2ksbox --paths` (`launcher-qt --paths` in
  a checkout) prints where this build looks for each companion program
  and file. `2ksbox --diagnose` prints the same plus what it found of
  this host's 3D, and writes it to `launcher.log` beside the machine
  library. Attach that log to a bug report. On Windows,
  `2ksbox-debug.bat` in the package does both from a console window.
- **The Direct3D note in the machine's settings** says what this host
  gives the guest's Direct3D. Without Vulkan 1.3 it is Wine on the host
  (slower; the note names the Wine package if none is installed), and
  with no Wine there is no Direct3D. Neither is an error. Keep the
  2ksbox adapter anyway: only its Direct3D
  needs the host, and everything else it does still beats the Cirrus.
  "In software (slow)" means a software Vulkan driver was found; a game
  may be faster the other way, so try both.
- **The Windows key and Alt+Tab still go to the host, in the Flatpak on
  sway** (and other wlroots compositors). The sandbox never gets the
  protocol that hands shortcuts to a window, so the title says they are
  the host's. Run the app under X11 instead, once:
  `flatpak override --user --nosocket=wayland --socket=x11
  com._2ksbox.Launcher`. GNOME and KDE may not need this.
- **A black desktop or a stop after a display-adapter change.** Windows
  wants a driver for the new adapter. If the guest tools were installed
  before the change, Windows finds it on the next boot; otherwise switch
  back, run `SETUP /ALL` in the guest, and switch again.

## Documentation

- [docs/06-guest-machines.md](docs/06-guest-machines.md): what each
  machine family is, its defaults, and what to expect from it.
- [docs/07-frontend.md](docs/07-frontend.md): the launcher and the
  player in detail (the library, the wizard, snapshots, the disc shelf).
- [docs/development.md](docs/development.md): the developer guide, with
  the design documents, the build stage by stage, every player option
  and environment variable, packaging and diagnostics.
- [docs/build-macos.md](docs/build-macos.md) and
  [docs/build-windows.md](docs/build-windows.md): the platform
  specifics.

## License

GPL-2.0. The player links QEMU in-process and is GPL-2.0-only. The
launcher is GPL-2.0-or-later. The licence text is in [COPYING](COPYING)
and every third-party component is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

2ksbox collects nothing and connects to the internet only for the
shader download you accept and for a machine whose network you turn on:
[docs/privacy.md](docs/privacy.md).

If you package or redistribute the player, read the licensing section of
[docs/development.md](docs/development.md) first. Its dependency tree
contains Apache-2.0 crates that GPLv2 cannot formally combine with.
