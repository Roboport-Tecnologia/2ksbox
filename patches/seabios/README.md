# SeaBIOS patch queue (the VGA BIOS)

Patches to the SeaBIOS tree QEMU pins (`qemu/roms/seabios`). The blobs
checked in were built from QEMU 9.2.4's pin, rel-1.16.3, and have not
been rebuilt since QEMU 11.1.2 (track M21) moved the pin to `b52ca86e`;
the next rebuild takes the new tree. `scripts/build-vgabios.sh` applies them with `patch -p1` to a
fresh copy (never the submodule) and builds the VGA BIOS for the two
variants a 2ksbox machine loads: `stdvga` (`-vga std`, and `d3dpt-vga`,
whose `romfile` it is) and `cirrus`. `scripts/prepare-qemu.sh` copies the
results, `firmware/vgabios-stdvga.bin` and `firmware/vgabios-cirrus.bin`,
over `qemu/pc-bios/`, so every package ships them.

**The blobs are checked in** and committed with any change here, as QEMU
does for its own `pc-bios/`. SeaBIOS needs gcc (`-m32`) and GNU ld, which
the Linux box has and the Mac, the Flatpak SDK and the Windows build do
not. A rebuild on another compiler is not byte-identical (QEMU's prebuilt
ROM and a pristine build here differ from byte 5), so a blob's evidence
is the `vbe-palette` guest check in `scripts/test.sh`, not a hash. The
version string is `<tag>-0-g<commit>-2ksbox` (today `rel-1.16.3-0-ga6ed6b70-2ksbox`), with no build time or
host name.

Patches are git-format diffs relative to the SeaBIOS tree (`git diff
--no-prefix --no-index a/<file> b/<file>` from two copies).

| Patch | What / why | Drop when |
|---|---|---|
| `01-vbe-set-palette` | VBE 4F09h, *Set/Get Palette Data*. BL 00h/80h set, 01h get, CX entries from DX, each blue/green/red/alignment in the DAC's current width, through the DAC ports; the secondary palette (02h/03h) and direct-colour modes are refused. SeaBIOS has no 09h case (`AX=0100`) yet reports every VBE mode as not VGA-compatible, which tells a program to use it. Without it DOS Quake quits at 640×480 and above with "Unable to load VESA palette", and Duke Nukem 3D's VESA modes draw in the default colours | SeaBIOS implements 4F09h and QEMU ships it |

## Known limits

- **VBE 4F08h** (set DAC width) still fails on the Cirrus, and on the
  standard VGA before a VBE mode is set. A program then stays at a 6-bit
  DAC, which is the width 4F09h's entries are read in.
