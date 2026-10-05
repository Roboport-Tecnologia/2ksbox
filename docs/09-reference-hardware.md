# 9. Reference hardware rig

A real period machine is the ground truth the emulated stack is judged
against: **Pentium 4 1.7, GeForce 6200, dual-boot Windows 98 / Windows
XP, a CRT monitor, a real optical drive.** Its results live in
`reference/`: the Direct3D goldens in `reference/d3d/rig-2026-09-03/`,
the benchmarks in `reference/benchmarks/rig-2026-09-04/`, and Wine's
d3d8 / d3d9 test suites' baselines in `reference/winetest/` (`rig-98.txt`,
`rig-xp.txt`; track M16). The CRT photo
set and the ATAPI traces are still to be taken. The rig stays stock; it
is an oracle, not a dev machine.

## The monitor

A **Samsung SyncMaster 753DFX**: 17" (≈16" viewable, ≈320×240 mm of
picture), DynaFlat flat glass, and a **delta dot-trio shadow mask at
≈0.20 mm horizontal pitch**, not an aperture grille. It has no damper
wires and no vertical stripes, so a Trinitron-style grille is the wrong
default for it (doc 03). `shaders/syncmaster-753dfx.slangp` approximates
it from that geometry. **Unverified.** The pitch and viewable width are
recalled from the model's class, not measured, and everything derived
scales with them. Measure the picture width and check the pitch in the
manual before treating them as fixtures; the photo set below turns the
approximation into a calibration.

## What it validates

- **Display pipeline (doc 03), the biggest win.** CRT photographs of
  known content to tune the presets against; the real aspect of 320×200
  on a 4:3 tube, 720×400 text and double-scanned low-res modes; 70 Hz DOS
  and 60/75/85 Hz SVGA as the reference for frame pacing.
- **CD-ROM (docs 05, 17).** Golden ATAPI traces as fixtures: the MMC
  command, response and sense sequences a protected title (SafeDisc,
  SecuROM) issues during its disc check, captured on the rig or the same
  drive in a modern box. Known-good dumps with subchannel and error data
  from discs that pass on the rig. A title that fails in the VM but passes
  on the rig is a backend bug.
- **3D (docs 04, 14).** Screenshots of the acceptance titles on the
  GeForce 6200 (native D3D8/9 and OpenGL) diffed against every emulated
  path; `D3DGAME9`'s golden frames are the executor's oracle (doc 14
  P0a). The 6200 is a 2004 DX9 card: right for the XP era and late-98
  Direct3D titles, no oracle for early Glide output (86Box and community
  references cover that).
- **Performance (docs 06, 22).** Super PI, 7-Zip and `SSEBENCH` have run
  on the rig (`reference/benchmarks/README.md`); 3DMark 99/2001 SE/03 and
  game timedemos are still to come. Expectations then read "X % of the
  reference P4", a testable claim.
- **OS behaviour.** Install quirks, control panels and autorun checked
  against real Win98/XP when a VM looks suspicious.

## The CRT photo set

The patterns are `guest-tools/src/crtcal.h`, one definition compiled into
both sides. `TESTS\CRTCAL.EXE` on the guest-tools ISO puts them on the
tube at the exact mode through an exclusive full-screen DirectDraw
primary, with no blit or stretch, so the photo doesn't measure a scaler. `build/crtcal-render` (`tools/crtcal-render.c`, the
`crtcal` check) writes the same pixels as BMPs, which `player --shader
<preset> --calib <dir>` runs through a preset. Put one photograph and one
shaded frame side by side, adjust the preset, repeat.

On the rig: `CRTCAL.EXE [w h [bpp]]`, then SPACE / 1–8 to step patterns,
`M` for the next mode, `L` to hide the legend, ESC to quit.

| # | Pattern | What it settles | The shot |
|---|---|---|---|
| 1 | `grid` | does the mode fill 4:3, what falls off each edge, is the geometry linear | whole screen, straight on, lens level with the tube's centre |
| 2 | `scanlines` | the beam's vertical profile, and **how many scanlines the tube draws** | macro on a band centre, plus one whole-screen frame |
| 3 | `mask` | mask kind, pitch in mm, stagger | macro, as close as the lens focuses, **ruler in frame** |
| 4 | `bloom` | how much the beam widens as it brightens | macro across the stack, one exposure for all rows |
| 5 | `sharp` | horizontal spot size, where the video bandwidth gives out | macro on the bar bands, at every mode |
| 6 | `halation` | how far light spreads into black | whole screen, dark room, exposure unchanged between shots |
| 7 | `gamma` | the tube's gamma against a dithered reference | whole screen, straight on; slightly defocused is right |
| 8 | `colour` | phosphor primaries and colour temperature | whole screen, fixed daylight white balance |

The two that matter most: **2 at 320×200**, the direct answer to
whether the tube draws 400 scanlines for a 200-line mode (doc 03 rule 3),
and **3 with a ruler**, which measures the ≈0.20 mm pitch. Doc 03's "Mode analysis" says which
shader parameters each pattern feeds; the preset file says which of its
values are derived and which wait on these photographs.

### 720×400, which needs DOS

Windows 98 cannot put its desktop at 720×400, the VGA *text* mode
(80×25 cells of 9×16, 400 lines, 70 Hz), so `TESTS\TEXTCAL.COM` is a DOS
`.COM`, run from FreeDOS or "Restart in MS-DOS mode". It draws with a
custom character generator (a periodic pattern tiles exactly from 256
glyphs of 9×16). Two patterns exist nowhere else:

- **Pattern 2, the 9th column.** A cell is 9 pixels wide and a glyph 8;
  for codes 0xC0–0xDF the 9th column repeats the 8th, otherwise it is
  background. A solid glyph below 0xC0 beside the block at 0xDB shows
  stripes on one half and continuous white on the other: doc 03 rule 2's
  "9-dot characters".
- **Pattern 6 against pattern 1.** Text mode's 400 lines are scanned
  once, mode 13h's 200 twice, so the same one-on-one-off pattern repeats
  every 2 lines on the tube in pattern 1 and every 4 in pattern 6. Two
  photographs at one camera setting settle doc 03 rule 3.

Under our emulator (FreeDOS, `TEXTCAL.COM` from `FDAUTO.BAT`) a QMP
screendump reports **720x400** and the player logs
`720x400 VGA text 80x25 (9-dot) — 4:3 picture, pixel aspect 0.741, 400
scanlines`.

### Getting the shot

- **Shutter ≥ 2 frame periods.** A CRT is lit only where the beam is; a
  fast shutter photographs a band. At 85 Hz use 1/30 s or slower, never a
  flash. This mistake ruins a whole set.
- **Manual everything.** Exposure, daylight white balance, ISO, focus.
  Halation and bloom are comparable only if exposure did not move, so
  write it down.
- **Tripod, straight on, dark room.** For `grid` the lens is level with
  the tube's centre; off-axis makes a linear picture look pincushioned.
- **A ruler in the macro frames**, taped flat to the glass and in focus
  with the phosphors. Without a scale a mask photo gives shape, not pitch.
- **RAW if available**, and record the OSD settings (brightness, contrast,
  colour-temperature preset). Turn **moiré reduction off**; it defocuses
  the beam on purpose.
- **Warm up twenty minutes.** A cold tube has not settled its geometry.
- One whole-screen frame per pattern per mode, plus the macros, into
  `reference/` with the capture settings in the file name or a sidecar.

## `reference/`

CRT photo sets (with capture settings), ATAPI trace fixtures, benchmark
results and real-hardware screenshots live there beside the tests that
use them. Disc dumps and disk images are never committed. Still to
capture: the CRT photo set (M2's calibration) and the ATAPI traces of
protected titles (doc 17's open items).
