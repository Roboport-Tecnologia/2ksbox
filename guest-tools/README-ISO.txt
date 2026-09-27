2ksbox guest tools, built from qemu-3dfx commit @REV@
(it must match the sign_commit stamp of the host's QEMU build)

Run SETUP.EXE. It knows which of these files your Windows wants, copies
them, registers what needs registering, and writes SETUP.LOG. From a
command prompt or the Run box:  D:\SETUP.EXE
  SETUP /ALL             install everything this Windows can use
  SETUP /LIST            print the lists below and exit
  SETUP /GAME <n> <dir>  copy one file set next to a game's EXE

Every program on this disc writes its log, and any BMP it dumps, to
C:\2KSBOX, whatever folder you started it from. The disc is read-only,
and the Run box's current folder could be anything. Set BOXLOG to put
them somewhere else:  set BOXLOG=E:\

What is on the disc, if you would rather do it by hand:

MAPPER\   the device mapper. OPENGL32.DLL reaches the host through
          it; without it, it refuses to load (0xc0000142 / "failed to
          initialize" on 2000/XP).
          Windows 98/Me: FXMEMMAP.VXD -> C:\WINDOWS\SYSTEM
          2000/XP:       FXPTL.SYS -> system32\drivers, then run
                         INSTDRV.EXE as Administrator.
          A machine with the emulated Voodoo 2 has 3dfx's own copy of
          FXMEMMAP.VXD from 3dfx's driver; SETUP leaves it alone. Glide
          games run on that card with 3dfx's driver; there is no Glide
          on this disc.

DRIVER\   the 2000/XP display driver for the paravirtual adapter. Boot
          the machine with -vga none -device d3dpt-vga, then run
          DRVINST.EXE -reboot. DRIVER\README.TXT lists its own test
          programs (DDTEST, D3D7TEST, SHTEST, ...). Windows 9x wants
          DRIVER9X\ instead, and SETUP picks for you. With the driver
          installed, games use Windows' own Direct3D (DirectX 9.0c on
          98, the one XP has): nothing to copy next to a game.

DRIVER9X\ the Windows 98/Me display driver for the same adapter, with the
          same machine options: -vga none -device d3dpt-vga. There is
          nothing to run. Copy D3DPT9X.INF, D3DPT9X.DRV and D3DPT9V.VXD
          into C:\WINDOWS\INF (SETUP does) and restart. Plug and Play
          matches the adapter, installs the driver with no clicks, then
          asks to restart once more. The .DRV draws. The .VXD claims the
          adapter's PCI resources, and without it Windows takes them
          away again.

DINPUT\   DINPUT.DLL, per game. It fixes "the keyboard does nothing in
          the game" when the game polls a non-exclusive DirectInput
          keyboard from a loop that never pumps messages (FIFA 2000's
          match). It merges what Windows reports pressed into the state.
          Copy it next to the game's EXE (SETUP /GAME 2). Set
          D3DPT_DINPUT_LOG=1 for a log, dinput_log.txt next to the EXE.

OPENGL\   OPENGL32.DLL, the OpenGL pass-through wrapper. Put it next to
          an OpenGL game's EXE (Quake 2 and the like). TESTS\WGLGEARS.EXE
          is the two-second check that it works.
          WRAPGL32.EXT goes with it and is the one file here you edit.
          It caps the OpenGL extension list the game sees. A modern host
          reports several thousand characters of extension names, and a
          1990s game reads that into a fixed buffer. GLQuake's is 4096
          bytes, and it crashes on the full list. The shipped cap is
          what such a game can hold. Raise the year for a later game, or
          delete the file to pass everything through.
          SETUP /GAME 3 copies both and never overwrites a WRAPGL32.EXT
          you have changed.

TESTS\    every test, benchmark and calibration program on the disc, one
          copy each. SETUP puts them in C:\2KSBOX. The Direct3D ones run
          on the display driver; WGLGEARS needs OPENGL32.DLL beside it:
          copy the EXE into a folder of its own and use SETUP /GAME 3
          there.
            D3DGAME9 D3DGAME8   the reference scene. -frames N runs a
                                fixed sequence, -dump N x.bmp writes a
                                frame. Run these on real hardware
                                first: those BMPs are what the emulated
                                paths are compared against.
            D3DFEAT9            shaders, queries, cube maps, state blocks
            D3D9TEST            adapter, caps, x87 control word, a
                                spinning triangle with fps
            DDVMTEST            what a video-memory check sees
            MODETEST            the display modes the driver offers. Run
                                it when a fullscreen game dies at
                                startup.
            WGLGEARS            OpenGL, next to OPENGL32.DLL
            WAITFILE            waits for a file to appear, then starts
                                something. Unlike a batch file's CHOICE,
                                it leaves the guest idle while it waits.
            SSEBENCH            SSE and x87 throughput in ns per op
            CDTEST              CD audio through MCI
            CRTCAL              the CRT calibration patterns, exclusive
                                full-screen at the exact mode
            TEXTCAL.COM         the 720x400 text-mode patterns (DOS only)

CDSHELF\  the host's disc shelf, from inside the machine. CDSHELF.EXE on
          98/2000/XP, CDSHELF.COM in a DOS box. Nothing to install (SETUP
          only puts the EXE where the Run box finds it). With no
          arguments the EXE opens a window, and the COM lists the shelf
          and waits for a key. Both also take a command:
            CDSHELF LIST   print the shelf and exit
            CDSHELF 3      put slot 3 in the drive
            CDSHELF E      empty the drive
          An insert always empties the drive first and waits for it.
          Otherwise Windows and MSCDEX keep showing the old disc.

VOODOO2\  V2START.EXE, for Windows 98/Me with the emulated Voodoo 2 and
          3dfx's own driver. That driver initialises the card from a
          start-up program for a few seconds after every login, and a
          Glide game started meanwhile hangs. SETUP moves the driver's
          "Voodoo2" entry out of HKLM\...\CurrentVersion\Run into
          HKLM\SOFTWARE\2ksbox\Voodoo2 and puts V2START.EXE (in
          C:\WINDOWS) in its place. It runs the same command and shows
          "Voodoo 2 driver is loading, please wait before running 3dfx
          games" until it has finished. C:\WINDOWS\V2START.LOG says what
          happened at the last login.

DOSMODE\  UIDE.SYS, FreeDOS's CD/DVD driver (public domain), for Windows
          98/Me's MS-DOS mode, which has no CD-ROM drive and no BLASTER
          variable of its own. SETUP's MS-DOS mode component (off by
          default; pick it from the menu or SETUP /I 7) copies the driver
          to C:\2KSBOX and writes "2ksbox MS-DOS mode.pif" on the desktop,
          a program information file with its own CONFIG.SYS (this
          machine's lines, HIMEM, the driver CD-only with no cache) and
          AUTOEXEC.BAT (this machine's lines, PATH, MSCDEX, SET
          BLASTER=A220 I5 D1 H5 P330 T6), the way Windows 98's own "MS-DOS
          mode for games" samples are built. Open it: Windows restarts in
          MS-DOS mode with the CD-ROM as the drive after the hard disk,
          and EXIT boots Windows again. Nothing changes how Windows itself
          starts. For a DOS game that dies in a DOS box (Mortal Kombat 3).
          A game's own sound setup then wants Sound Blaster 16, 220h,
          IRQ 5, DMA 1.
