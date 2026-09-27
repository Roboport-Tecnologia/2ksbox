# The Microsoft Store listing

The text Partner Center asks for, kept here so a resubmission pastes
the same words (`docs/build-windows.md`, "The Store package", "The
submission"). Character limits are the Store's.

## Product identity (Partner Center → Product management → Product identity)

Copied from Partner Center into the pack, never the other way:

```sh
scripts/package-msix.sh build/win/package/2ksbox-<version>-windows-x86_64 \
  --identity <Package/Identity/Name> \
  --publisher '<Package/Identity/Publisher, CN=…>' \
  --publisher-display '<Package/Properties/PublisherDisplayName>'
```

## Properties

- **Category:** Utilities & tools, no subcategory. Not a game: the
  games that run inside a machine are the user's, not ours.
- **Privacy policy URL:** <https://github.com/davidrios/2ksbox/blob/main/docs/privacy.md>
- **Website:** <https://github.com/davidrios/2ksbox>
- **Support contact info:** <https://github.com/davidrios/2ksbox/issues>
- **System requirements, minimum:** Windows 10 version 2004 (build
  19041), 64-bit; 4 GB memory; a GPU with Vulkan 1.3 or Direct3D 9.
- **System requirements, recommended:** Windows 11, 64-bit; 8 GB memory;
  a GPU with Vulkan 1.3; Windows Hypervisor Platform turned on.

## Pricing and availability

Free, every market, visible in the Store.

## Age ratings (the IARC questionnaire)

The app is not a game and has none of the content the questionnaire
asks about: no violence, no user interaction with other users, no
purchases, no location sharing, no unrestricted internet browsing (the
shader download is a fixed file). Answer "no" throughout; the result is
"Everyone" / PEGI 3 / USK 0.

## Submission options: restricted capabilities

One sentence per capability, in the box Partner Center shows for it:

- **runFullTrust:** 2ksbox is a Win32 desktop application (a Qt launcher
  that starts a player process with an in-process CPU emulator, QEMU)
  which must run at medium integrity to open the user's disk images
  wherever they are, use the Windows Hypervisor Platform for hardware
  acceleration, and render through Vulkan and OpenGL.

## Store listing (en-US)

**Product name:** 2ksbox

**Short description** (up to 100 characters):

> Windows 98, XP and DOS as vintage boxes: 3D games, CD-ROM and music, on a CRT.

**Description** (up to 10 000 characters):

> 2ksbox runs Windows 98, Windows XP and DOS on your PC as "vintage
> boxes": whole machines of their era, with the 3D cards, CD-ROM drives
> and sound cards the games of the time were written for, shown through
> a CRT shader chain on your modern screen.
>
> What you get:
>
> • A launcher that sets up a machine in a few clicks: pick the era, the
>   memory, the display and the sound, point it at your install media,
>   and it boots.
> • Direct3D 8 and 9 games rendered by your own GPU through the box's
>   display adapter, at the resolution the game asks for.
> • An emulated 3dfx Voodoo 2 for Glide games, with 3dfx's own drivers.
> • OpenGL pass-through for GLQuake and its generation.
> • A CD-ROM drive that mounts disc images from a shelf, and swaps them
>   while the machine runs.
> • Music the way it sounded: an OPL3 for AdLib and Sound Blaster, a
>   General MIDI SoundFont synth, and MT-32 emulation with your own ROMs.
> • CRT shaders (the community's slang-shaders collection, downloaded on
>   first run) with per-machine profiles and a live preview.
> • Snapshots: save a running machine and come back to it.
> • Hardware acceleration through the Windows Hypervisor Platform when
>   it is on, and plain emulation when it is not.
>
> You bring the operating system and the games. 2ksbox ships no copy of
> Windows, DOS or any game, and no Roland ROMs.
>
> 2ksbox is free and open source (GPL-2.0), built on QEMU and on 86Box's
> Voodoo 2 emulation. Source, issues and documentation:
> https://github.com/davidrios/2ksbox

**What's new in this version** (release notes, up to 1 500 characters):

> First release in the Microsoft Store.

**Product features** (up to 20 lines of 200 characters):

- Windows 98, Windows XP and DOS machines, set up in a few clicks
- Direct3D 8/9 games on your own GPU
- Emulated 3dfx Voodoo 2 for Glide games
- OpenGL pass-through
- CD-ROM disc shelf with live swapping
- OPL3, SoundFont and MT-32 music
- CRT shaders with live preview and per-machine profiles
- Snapshots of running machines
- Hardware acceleration through Windows Hypervisor Platform

**Search terms** (up to 7, 30 characters each): Windows 98, Windows XP,
DOS, retro gaming, emulator, 3dfx, CRT shader

**Copyright and trademark info:** © 2026 David Rios. Windows is a
trademark of Microsoft Corporation. 3dfx and Voodoo are trademarks of
their owners. 2ksbox is not affiliated with or endorsed by any of them.

**Additional license terms:** the GPL-2.0, at
https://github.com/davidrios/2ksbox/blob/main/COPYING. The source code
of this package is at the same repository, tagged with the version in
the package.

## Screenshots

The Store wants PNGs of at least 1366 × 768 (up to 3840 × 2160), at
least one, up to ten, no added text or logos. They are shots of the
**player's window**, what the user sees: the guest's frame after the
geometry stage and the CRT shader chain, at the window's resolution.
Take them with the player full screen on a 1920×1080 or larger display,
with the player's Ctrl+Alt+Shift+S (the window's picture as a PNG, in
the working directory or `PLAYER_SHOT_DIR`) or Windows' own screenshot
(Win+PrtScn saves the whole screen to `Pictures\Screenshots`). **Not the
player's Ctrl+Alt+S shot**: that is the guest's frame at the guest's
resolution, before the shader, and scaling it up would show something
the box never draws (user, 2026-09-25). A good set:

1. The launcher with a few machines in the library.
2. A Windows 98 desktop.
3. A Direct3D game (Max Payne, Need for Speed: Porsche 2000).
4. A Glide game on the Voodoo 2.
5. A DOS game.
6. The shader editor's preview.

Captions, 200 characters each, are entered beside each upload.
