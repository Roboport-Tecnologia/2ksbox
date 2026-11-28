# Track M25: Direct3D 10 and 11, a user-mode driver of our own

Opened 2026-10-16 (user: "the correct would be to implement a real
direct3d 10 or 11 driver, right?", then "let's do it from scratch
ourselves ... first milestone feature level 10_0"). ADR-029. Branch
`track/m25-d3d11-umd`, merged to `main` 2026-10-20 (user: "good enough
to be merged") and deleted; each later stretch is a `track/m25-*` branch.

**The goal** (user, 2026-11-26): The Witcher 3 on its Direct3D 11
renderer in Windows 11 on Arm under HVF on the Mac. That is an x64
program (Prism emulates it in the guest) at feature level **11_0**:
compute, unordered access views and tessellation, which is the half of
step 5 not started.

This doc: the state, the next steps in order, the design, what the
track owns, the test loop, the rules the work found (each with the log
entry that found it), the open items, and the dated log.

## State (2026-11-27)

- **Guest: `d3dpt11.dll`** at feature level **10_1** (shader model 4.1)
  with 10_0 to 9_1 below it. Windows 8's 11.1 interface (the one
  Windows 11 uses, with its video functions), Windows 7's 11.0 one, and
  Direct3D 10.0 / 10.1's own interfaces for Windows 7's runtime. Built
  x64 with a 32-bit WoW64 copy, 32-bit for Windows 7, and ARM64X plus
  x86 for Windows 11 on Arm.
- **The x64 kernel driver claims WDDM 1.2.** On Windows 11 DWM composes
  the desktop through `d3dpt11.dll` from logon (x64 under WHPX and KVM,
  Arm under HVF). The 32-bit Windows 7 kernel driver rates WDDM 1.0, so
  Windows 7's DWM stays on the Direct3D 9 driver.
- **Host: `d3dpt_exec_d11.cpp`** on DXVK's d3d11 (an 11_0 device on the
  PC). One host device, a context state per guest device; shaders
  passed through, with the DXBC container rebuilt on the host. Wire:
  protocol v25 / v26 (the D11 record), v27 to v29 (GDI), v28's deferred
  scanout readback.
- **Checks.** `tools/d3d11-probe.cpp` against WARP in the guest, 0 pixels
  off: offscreen, both swap chain models, Direct3D 10's API, also on
  Windows 7. `guest-tools/src/d3dgame11.c -compare N`, the reference
  scene on 10_0's features, within 9 pixels. `d3dpt-gdi` in
  `scripts/test.sh` (the host's GDI operations, 90 checks).
- **Speed on the PC** (d3dgame11 windowed at 640x480): unsynced, about
  470-515 frames/s with the flip model and about 250 with DISCARD;
  58-59 with vsync. The desktop at 1920x1200 composes at the screen's
  rate. **Unigine Heaven in Direct3D 11** (32-bit, 1280x720, Low, no
  tessellation): its benchmark averages 200.6 frames/s full screen at
  1280x720 Low (score 5052) and 184.2 at 1920x1080 High (score 4639);
  ~70 windowed, from ~46 windowed at the start of 2026-11-27; the
  game's own render thread is the limit now (the guest's GPU scheduler
  at ~45% of a vCPU, the host's executor ~30%). "Heaven in Direct3D 11
  on the PC".
- **The submission path** (2026-11-27): 1 MiB submissions of up to 512
  allocations built in cached memory, the registration table in the DMA
  buffer's private data, GetData that never waits, register set v14's
  one-exit append and v15's asynchronous batches and immediate flips
  (their fences by `IRQ_DMA`; `-global d3dpt-vga.async=off` or the
  service key's `Async` = 0 for the A/B).
- **GDI acceleration** is built and **off by default**
  (`GdiAcceleration` = 1 turns it on; "GDI-compatible textures").
- **Every package** carries DXVK's d3d11 and dxgi; without them
  `CAP_D3D11` stays off and the guest keeps 9_3.
- **On the Mac** (Windows 11 on Arm, HVF, DXVK on KosmicKrisp): the
  desktop with Mica, and `d3dgame8` / `d3dgame11` at 60 frames/s at
  every size up to 3840x2160. **Unigine Heaven in Direct3D 11 crashes
  inside KosmicKrisp** (Mesa's `nir_opt_varyings`, while building a
  pipeline). On the PC Heaven runs in Direct3D 9 at 1-3 frames/s.
- **KosmicKrisp's features** (`vulkaninfo` on the Air, LunarG SDK
  1.4.357.1, 2026-11-26): `tessellationShader` true, **`geometryShader`
  false** (DXVK patch 02 makes it optional, and its note says it must be
  required again for d3d10/11), `maxComputeSharedMemorySize` 32768 (the
  32 KB compute shader 5.0 asks for), `vertexPipelineStoresAndAtomics`
  true. DXVK's d3d11 there, geometry shaders or not, reports **11_1 at
  most and makes 11_0 devices** (`player-mitsuami_d3d11.log` in the
  launcher's data dir, the Air, 2026-10-26). Unchecked: whether
  d3dgame11's geometry-shader billboards draw on the Mac.

## Next steps, in order

0. ~~Red and blue swapped in a windowed RGBA present~~: done 2026-11-27
   (protocol v32, "Heaven in Direct3D 11 on the PC").
1. **Heaven's KosmicKrisp hang** (on the Air). The pipeline is known and
   its shaders kept ("Heaven on the Air, again"), but a native build of
   it does not hang: find what DXVK's device does that the repro does
   not, or run Mesa main's KosmicKrisp (DXVK refused to create an
   instance on a first build), then report it to Mesa. A heavy Direct3D
   11 game would hit the same class of bug.
2. **Feature level 11_0**, the rest of step 5. In order, each checked
   against WARP by `d3d11-probe` or a `d3dgame11` part, each protocol
   change a `D3DPT_PROTO_VERSION` bump:
   1. ~~Compute shaders and UAVs~~ (done 2026-11-27, protocol v34:
      "Feature level 11_0, step 1").
   2. ~~Tessellation~~ (done 2026-11-27, protocol v35: "Feature level
      11_0, step 2").
   3. ~~Claim 11_0 for every process~~ (done 2026-11-27: "Feature level
      11_0, step 3"; `D3D11FL10` keeps a process at 10_1). The pipeline
      statistics query answers in its 11 layout (hull and domain
      invocations in the probe).
3. **Geometry shaders on the Mac.** Find out what d3dgame11's billboards
   and stream output do on KosmicKrisp, where DXVK offers 11_0 with no
   `geometryShader` (a draw with one must fail or be dropped). If the Witcher 3 needs them, emulate them,
   or wait for KosmicKrisp.
4. **A stepping stone, then the game.** A Direct3D 11 workload with a
   native Mac build runs first on Windows 11 on Arm, for a baseline: the
   Diligent Engine samples (Vulkan on KosmicKrisp natively, D3D11 in the
   guest), or Heaven. Then the Witcher 3, measured against the Mac's
   only other way to run it (CrossOver's D3DMetal).
5. The open items not on that path, under "Open": DISCARD's present,
   multisampling, the blank's ±2 ms, Windows 7's DWM on Direct3D 10.1,
   Heaven in Direct3D 9.
6. **Speed on the PC, further** (after 2026-11-27): the game's thread is
   the limit: Direct3D 11's runtime and the user-mode driver at ~1.5
   million DDI calls a second (a CPU profile in the guest would say how
   much is ours: WPR is in Windows, its analysis tools are not on the
   PC); a windowed game still waits for its present's readback.

## Why

M20 step 5 put M18's WDDM driver on Windows 11: a WDDM 1.1 kernel driver
and a Direct3D 9 user-mode driver. Windows 11's `d3d11.dll` reaches it
through `d3d10level9`, which tops out at feature level 9_3 (M20 got it
there from 9_1). Edge draws in software (`edge://gpu`: "Software only";
Chromium wants 10_0), no Direct3D 10 or 11 program gets a device, and
DWM, XAML and Direct2D all run through the 10level9 layer, where M20's
last open bug (Explorer's XAML that never composes) also lives.

## The shape

- **Guest: `d3dpt11.dll`** (`guest-tools/src/d3dptvid/wddm/um11/`), the
  Direct3D 11 DDI: `OpenAdapter10_2`, Windows 8's 11.1 interface
  (`D3D11_1_DDI`, with its video functions, `d11_video.c`; step 4) and
  the Windows 7 one (`D3D11_0_7_DDI`), the 3D pipeline level from
  `D3D11DDICAPS_3DPIPELINESUPPORT`. Direct3D 10.0 / 10.1's interfaces
  for Windows 7's runtime (`d10_dev.c`) convert the dozen entries whose
  arguments are older and call the 11 ones. Written in C like the
  Direct3D 9 driver, with the same command buffer, allocation list and
  patch locations (`../d3dpt_wddm.h`). The INF names it after the
  Direct3D 9 driver in `UserModeDriverName` (Direct3D 9 reads the first
  name, 10 / 10.1 / 11 the next ones); a 32-bit copy for WoW64.
- **Wire: protocol v25**, one new record, `D3DPT_OP_D11`: a context's
  run of Direct3D 11 tokens, as `D3DPT_OP_DP2` carries a DP2 stream.
  Objects (shaders, states, views, layouts, queries) are the context's,
  by ids the guest picks; resources are allocations, by the host handle
  the kernel driver patches in, so another process (DWM) can name the
  same one. v26 added the 11.1 interface's tokens, v27 to v29 the GDI
  record, v28 the deferred scanout readback (`RES_SCANOUT`, GDI `SYNC`).
- **Resources.** Every resource is one allocation. One the CPU may map
  (dynamic, staging) or that leaves the device (shared, a primary) has
  its subresources laid out in it; a default one only names itself and
  lives on the host. The kernel driver registers each with the host as
  before (`VRAM_SURFACE`, a new caps bit `D3DPT_VS_D3D11` for one with
  no Direct3D 9 object behind it).
- **Host: `d3dpt/exec/d3dpt_exec_d11.cpp`**, DXVK's d3d11 through
  `D3D11CoreCreateDevice` with DXVK's own DXGI adapter (on Windows both
  loaded by file name, `dxvk_d3d11.dll` / `dxvk_dxgi.dll`, beside
  `dxvk_d3d9.dll`; the process already holds Windows' own `dxgi.dll`).
  One device for the guest; each guest device an
  `ID3DDeviceContextState`, swapped in when its records run.
- **Shaders** pass through: the DDI gives the program tokens
  (SHDR/SHEX) and the I/O signatures by register; the host writes a
  DXBC container (ISGN/OSGN from the signatures, the code chunk, DXBC's
  own MD5 hash, which DXVK checks) and creates the shader from it.
  Input layouts get the same synthesized semantics.

## Steps

1. **The skeleton.** The DLL loads, `d3d11.dll` opens it, and
   `D3D11CreateDevice` at feature level 10_0 on our adapter succeeds.
   Done 2026-10-16.
2. **Protocol v25 and the host's device**, checked by an in-guest
   program against WARP. Done 2026-10-16.
3. **Presenting**: a windowed DXGI swap chain through DWM. Done
   2026-10-16.
4. **DWM on it**: the desktop composed through our device instead of
   10level9. Done 2026-10-17.
5. **10_1, then 11_0**: compute, unordered access views, tessellation.
   10_1 done 2026-10-20 (with Direct3D 10's own interfaces for Windows
   7); 11_0 is "Next steps" 2.
6. **Games on Windows 11 on Arm** ("Next steps" 1, 3 and 4), toward the
   Witcher 3.

## Owns

`guest-tools/src/d3dptvid/wddm/um11/` and `wddm/umd_log.c`,
`d3dpt/exec/d3dpt_exec_d11.cpp`, protocol v25's D11 record and the ones
after it, register set v10's `CAP_D3D11`, the x64 kernel driver's WDDM
1.2 DDIs, `tools/d3d11-probe.cpp`, `guest-tools/src/d3dgame11.c`,
`tools/d3dpt-gdi-test.cpp`, ADR-029 and its amendment, and this doc. The
shared files (`d3dpt/d3dpt_proto.h`, the rest of `d3dpt/exec/`, the WDDM
kernel driver with M18 / M20) are edited minimally, the track named in
the commit.

## Test loop

**On the PC** (x64 Windows 11 under WHPX): the Windows 11 test machine
(`D:\vms\win11-test`, M20's tools, outside the repo): `run-m25.sh`
(M20's `run-m20.sh` on the overlay pair `m25`, its own logs;
`vgamem_mb=512` since 2026-10-19, so Windows lists every size up to
3840x2160, `VGAMEM=128` the device's default; the launcher's Windows 11
machines on our adapter get 512 too, the 32-bit families keep the
default), `m25-test.sh` (installs the set staged in `M25\WDDM64` with a
build number of the minute in the INF, restarts, runs
`tools/d3d11-probe.cpp` as the logged-on user, prints its output and its
process's driver lines; `INSTALL=0`, `PROBE_ARGS=present`, `"present
12000"` to hold each window 12 s, `flags`). Step 4 added `stage25.sh`
(this checkout's `build/wddm/x64` into `M25\WDDM64`), `inst25.sh`
(install, wait for the restart, detach Basic Display's screen as the
guest agent would, `M25\oneshot.ps1`), `dwm25.sh <tag>` (DWM off
`D3D11Exclude`, restarted, its driver lines into `m25-dwm.log`, a
screendump; `EXCLUDE=1` puts it back on Direct3D 9's driver, `TRACE=1`
traces it) and `shot25.sh <tag>` (a screendump of d3dpt0). The driver
is built with the EWDK (`build-windows.sh wddm`, or `guest-tools/
build-wddm.cmd` alone) and installed by `wddm-install.ps1`, which
stamps `DriverVer` with the install's date and minute. The test
machine's helper folders (`G11\`, `LOAD\`, `SETRES\`, `DBG\`) are named
in the log entries that made them.

Speed (2026-11-27, the test machine's folder): `hcycle.sh <tag>
[build]` (from the checkout; with `build`: the driver built, staged and
installed) runs Heaven for 90 s (`HSCRIPT=h11f`: full screen; `h11`:
windowed), takes a screendump and prints the device's rates and the
executor's present count (the player run with `D3DPT_D11_PROF=1`);
`HRUN\cpu.ps1` gives 5 s of CPU per process and per Heaven thread;
`HRUN\prof.ps1` a WPR sample profile by module with `d3dpt11_32.dll`'s
hot offsets (resolve them with the x86 cdb on `build/wddm/x86/d3dpt11.dll`
and its PDB); `HRUN\waits.ps1` where Heaven's busiest thread blocks
(the stacks WPR takes at its context switches, the 32-bit frames).
Heaven's own F12 screenshot (`C:\Users\test\Heaven\screenshots`) shows
a full-screen frame where QMP's screendump cannot.

**On the Air** (Windows 11 on Arm under HVF): a launcher machine with
"2ksbox adapter (experimental)" (test mode, Secure Boot off), the Arm
set installed from the guest-tools disc's `WDDMARM\` with
`wddm-install.ps1`. The Arm kernel driver is built as our viogpudo is
(`build-windows.sh`, `scripts/nuget-wdk.sh`; `docs/build-windows.md`
"The WDDM driver"), the ARM64X user-mode drivers with VS 2022 on the PC.

**Logs and switches:**

| Where | What |
|---|---|
| `HKLM\SOFTWARE\2ksbox` `D3D11Trace` / `D3D9Trace` (REG_MULTI_SZ, executable names) | every DDI call of those processes, with each format and multisample answer once (`D11_TRACE_FEW`); their log cap goes from 400 lines to 400,000. Without it a user-mode driver logs only failures, refusals, `todo`s and the process's OpenAdapter line |
| `HKLM\SOFTWARE\2ksbox` `D3D11Prof` (REG_MULTI_SZ, executable names) | every 5 s, at a present, two lines of where `d3dpt11.dll`'s time goes in those processes: DDI calls, then calls / microseconds / KiB of RenderCb, uploads, maps, unmaps, GetData and PresentCb (TSC cycles; nested: a GetData's flushes are in its time and in RenderCb's) |
| `D3DPT_DDI_VBCACHE=0` | turns off the buffer cache (on by default since 2026-11-27): the Direct3D 9 decoder draws from host copies of VRAM vertex / index buffers drawn twice unwritten ("A host copy of VRAM buffers"); a line every 5,000 draws counts hits, misses and copies made |
| `D3DPT_DP2_PROF=1` | the Direct3D 9 decoder's time every 5 s as `key:count:us`: keys 0-255 DP2 commands (200 is `D3DPT_DP2_DRAW8`), 256 + op the records (a DP2 record's time includes its commands) |
| `HKLM\SOFTWARE\2ksbox` `D3D11Exclude` | processes refused by `d3dpt11.dll` (they fall back to d3d10level9), set by hand; `wddm-install.ps1` removes an earlier install's |
| `HKLM\SOFTWARE\2ksbox` `D3D11Win7` | processes offered only the Windows 7 interface: an A/B for the 11.1 one |
| `HKLM\SOFTWARE\2ksbox` `D3D11FL10` | processes (or `*`) kept at feature level 10_1; every other is offered 11_0 since 2026-11-27 |
| `HKLM\SOFTWARE\2ksbox` `D3D11NoMSAA` | processes (or `*`) offered no multisampling; every other gets 2x, 4x and 8x since 2026-11-27 |
| `HKLM\SYSTEM\CurrentControlSet\Services\d3dptkmd` `Trace` = 1 | the kernel driver's routine lines (devices, allocations, submissions, presents, VidPn, the 5 s timings and RenderKm census; since 2026-11-27 also "Render in 5 s": Render's calls, microseconds, allocations, patches, KiB and its phases copy / check / reg / patch, in hex). Read at DriverEntry: restart, or disable and enable the adapter. Start and stop, failures, refusals, TDR dumps and the relayed user-mode lines log without it |
| the same key's `GdiAcceleration`, `GdiCpuVisible`, `Cached` | GDI acceleration on (default off), its bitmaps not CPU-visible (default visible), staging allocations uncached (default cached) |
| the same key's `StartStage`, `StartRegs`, `BarDiag`, `InitStatus` | written by every start: where it stopped, the command register and MAGIC / VERSION as read, the BARs and memory resources |
| `player-mitsuami_d3d11.log` / `_dxgi.log` in the player's working directory | what DXVK refused, by name |
| `D3DPT_D11_PROF=1` | the executor's time per token every 5 s (64 + op for GDI records, 72 / 73 a readback's two halves) |
| `D3DPT_DXVK_D3D11_LIB` pointing nowhere | a host with no Direct3D 11: `CAP_D3D11` off, the guest on 9_3 |
| `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\dwm.exe` (`DumpFolder` `C:\2KSBOX\dumps`, `DumpType` 1) | a minidump of each DWM crash; out zipped and base64 through `guest-run.sh` (~250 KB of text), read on the PC with the EWDK's `cdb` (`E:\Program Files\Windows Kits\10\Debuggers\x64`, `_NT_SYMBOL_PATH=srv*<cache>*https://msdl.microsoft.com/download/symbols`): `!analyze -v`, then `.frame /r` |

Traps of the loop:

- **Reinstall with a new DriverVer.** Windows keeps the installed
  package's files when the version is the same, so a rebuilt DLL with
  the old version never lands (the first two cycles ran the old DLLs).
- The user-mode log names the process (`[pid]`) and the kernel driver
  writes a line at a time: Windows 11's shell opens the adapter from a
  dozen processes, and their lines interleaved by the character.
- Every debug line is one DEBUG register write a character, a VM exit
  each under HVF: untraced logging is what keeps the Mac's desktop
  fast.
- `New-Item -Force` on a registry key that exists empties it (it took
  the D3D11Exclude value with it once).
- A 32-bit program reads the `2ksbox` key through the 64-bit view
  (`RRF_SUBKEY_WOW6464KEY`, defined by hand: the EWDK's headers predate
  it); `WOW6432Node` would hide it.
- A console window over a program's own reads as the window missing in
  a screendump: run it under `conhost --headless` (`G11\g11.ps1`).
- The probe and the reference scenes are DPI-aware: Windows scaled an
  unaware window 175% at 1920x1200, and the screen was read where the
  window was not.
- `ChangeDisplaySettingsEx` with `CDS_UPDATEREGISTRY` from a scheduled
  task fails for a monitor Windows has stored nothing for:
  `SETRES\setres.ps1` changes the mode without the registry, then saves
  it with `SetDisplayConfig(SDC_SAVE_TO_DATABASE)`.
- The test machine's display and sleep timeouts are off (`powercfg`): a
  screen Windows turns off used to look like a frozen guest.
- Never pipe `g11run.sh` into `head`: the closed pipe's SIGPIPE makes
  the runner exit non-zero, which the watchdog reports as a frozen
  guest (three false alarms on 2026-11-27). Write to a file first.
- In a PowerShell script for the guest, a helper named `h` (or any
  built-in alias: `h` is Get-History) never runs; aliases win.
- `scripts/test.sh all` runs from the PowerShell tool on the PC:
  `win.ps1` started from Git Bash gets `pwd` as `C:/...`, whose colon
  breaks `PATH`.

## Rules the work found

Each was a day; the log entry named has the evidence.

**The runtime and DWM**

- Claim **no multisampling**: the runtime walks
  CheckMultisampleQualityLevels and refuses the device at some claim
  (which one is open). Step 1.
- DWM's composition device asks for `VIDEO_SUPPORT`, which the runtime
  grants only to a driver with the **11.1 interface's video functions**
  on a kernel driver claiming **WDDM 1.2**. Without either, DWM goes to
  d3d10level9. "Step 4", "Why DWM is on d3d10level9".
- A WDDM 1.2 kernel **refuses a write to an allocation opened
  read-only** (`DXGI_ERROR_ACCESS_DENIED` in `RenderCB`, the device
  removed), as DWM opens a flip-model swap chain's buffers. Another
  process's allocation is listed as read, a destination as written.
  "WDDM 1.2 claimed".
- A kernel driver fix-up entry is **24 bytes on x64**, WoW64 included:
  reserve that per patch location. "Step 4".
- DXGI's primaries are **scanned out**, not `NO_SCANOUT`, or DXGI never
  sets the mode on DWM's primary. "Step 4".
- A flip-model swap chain rotates its buffers' identities at each
  present: **the host's views are made again** after a rotation. Its
  buffers are described to DWM as render-target textures. "Step 3",
  "Step 4".
- **Windows 7's runtime** opens a driver's own Direct3D 10.0 / 10.1
  interfaces and refuses 11_1 in the list of levels asked for. Windows 8
  and later run Direct3D 10 programs on the 11.1 interface. "Feature
  level 10_1".
- A runtime at 10_x reads `PIPELINE_STATISTICS` as Direct3D 10's 64
  bytes, where DXVK takes only Direct3D 11's 88: the executor asks the
  host at the type's size. "Windows 11 on Arm: the driver starts".
- DXVK's d3d11 stubs `SetPredication`: **the executor predicates**
  itself. "A game-like scene".

**Memory, coherence and the wire**

- The host keeps a Direct3D 11 object and a Direct3D 9 one per shared
  allocation; whichever drew last is marked newer and synced through
  VRAM before the other reads it (`exec_d11_sync`). "Windows 11's shell
  on it".
- A staging resource is read back only when the host wrote it since:
  a map polled with DONOTWAIT must not queue work. Same entry.
- A driver reinstall restarts the allocation handles at 1 with no
  machine reset: `RES_CREATE` replaces whatever had the handle. Same.
- **A NOOVERWRITE lock never flushes.** "The desktop at high
  resolutions".
- **A flip's readback is deferred** (`RES_SCANOUT`), paid when the GPU is
  done or the display needs it (`d3dpt_exec_settle`), and **the display
  never blocks on it under the BQL** (`fb_settled`: a trylock and a
  `DO_NOT_WAIT` map, then a 1 ms retry). Settling only at the refresh
  showed old frames ("reverb"). "Where the present really goes", "Where
  the STAGING frame went".
- Both segments are `CacheCoherent` and staging allocations `Cached`: a
  write-combined lock made every CPU read uncached. "Where the STAGING
  frame went".
- Under a hardware accelerator the batch runs **without the BQL**; the
  doorbell lowers QEMU's re-entrancy guard while the BQL is away. "Batches
  without the BQL".
- The x64 aperture is 1 GiB: Direct3D 9 counts textures against
  dedicated memory plus the aperture and refuses past that itself.
  "Unigine Heaven in Direct3D 9".

- After a stop and a start (a driver update) the device still holds the
  last instance's fence: a WDDM driver resets it before enabling the
  interrupt and **never reports a fence it has not submitted** (bugcheck
  0x119, subtype 1). "Bugcheck 0x119 after a driver update".

**Flips and the blank**

- A fence held for the vertical blank holds **every process's** later
  fences. A no-wait DMA flip is bugcheck 0x113 at the adapter's stop.
  The x64 driver's flips on the blank are **MMIO flips**
  (`FlipOnVSyncMmIo`), the blank reporting the very `PrimaryAddress` it
  was given. "Flips that hold nothing".
- The guest's blank follows the host's; an occluded host wait is
  answered by DWM's composition. "The host's vertical blank, again".

**GDI**

- Windows 11 draws GDI **with the CPU** into CPU-visible redirection
  bitmaps and sends RenderKm next to nothing. Bitmaps that are not
  CPU-visible lose that drawing. "Bitmaps the CPU cannot see, again".
- With GDI acceleration on, GDI draws into a Direct3D 11
  `GDI_COMPATIBLE` texture itself, which a shared allocation in our
  segment cannot allow: **off by default**. "GDI-compatible textures".

**Windows on Arm**

- Windows on Arm hands the driver its device with **memory decoding
  off**: the driver turns it on. "Windows 11 on Arm: the driver starts".
- The 512 MiB frame buffer is a **64-bit BAR above 4 GiB** (`bar64=on`,
  no option ROM, QEMU patch 97 for an M1's 36-bit IPA). "The frame
  buffer above 4 GiB".
- HVF's dirty-log ranges are whole 16 KiB host pages (QEMU patch 98).
  "Windows 11 on Arm: the driver starts".
- The user-mode drivers are **ARM64X** (VS 2022's ARM64EC tools) and
  every Arm build compiles with `/volatile:ms`. "The driver builds for
  Windows 11 on Arm".

## Open

- **Feature level 11_0** ("Next steps" 2).
- **Unigine Heaven in Direct3D 11 on the Mac** hangs in KosmicKrisp
  ("Heaven on the Air, again"; "Next steps" 1).
- **A windowed (blt-model) game's present waits for the host's GPU**:
  the host reads the frame back into the window's STAGING surface inside
  the guest's submission (~2 ms of Heaven's frame at 1280x720 since each
  record is flushed, ~79-88 frames/s windowed against ~218 full screen).
  "Heaven in Direct3D 11 on the PC"; "Full screen, 2026-11-27".
- **Heaven's full-screen GUI laid out for the desktop** (user, 2026-11-27:
  "the image looks zoomed in and cropped"): on some launches in Direct3D
  11 Heaven sets a 1920x48 viewport for its menu bar and lays its
  dialogs out for 1920x1080 inside its 1280x720 buffers, so the GUI comes
  out 1.5x and cropped; the 3D scene is 1280x720 and right. Seen on a
  relaunch after killing a windowed or full-screen Heaven, never on the
  first launch after a boot, never in Direct3D 9. With it, once:
  `CreateSwapChain` failing with `DXGI_ERROR_INVALID_CALL` ("can't set
  1280x720 fullscreen video mode"). Narrowed: it follows Heaven losing
  focus (the test scripts' console takes it; Alt+Tab does it too). DXGI
  then gives the window back its windowed size (1920x1080, style
  0x94000000) but Windows never commits the desktop's mode again (the
  kernel driver's last `CommitVidPn` stays 1280x720, and Heaven stays the
  foreground window), so Heaven, back, lays its GUI out for a 1920x1080
  window in a 1280x720 mode. Whether Windows skips the restore for
  something we report or for an Alt+Tab with nothing to switch to is
  open. `HWIN\hwin.exe` (the test machine's folder: a GUI-subsystem
  program that takes no focus) prints Heaven's windows, the screen and
  the mode; `hz.sh <tag>` starts Heaven and runs it. "Full screen,
  2026-11-27".
- **Geometry shaders on KosmicKrisp** ("State"; "Next steps" 3).
- **A blt-model (DISCARD) window's present** goes through a STAGING
  copy the CPU moves into a CPU-visible redirection bitmap: ~250
  frames/s unsynced against the flip model's ~500 ("Where the STAGING
  frame went"). With vsync it still spreads (median ~19-20 ms, ~50 of
  380 frames doubled; the flip model holds 17).
- **GDI acceleration on by default** waits for a shared allocation GDI
  can reach with the CPU ("GDI-compatible textures").
- ~~Multisampling~~: 2x, 4x and 8x since 2026-11-27 ("Multisampling").
- **The vertical blank's ±2 ms** between the player and the device: not
  the executor's batch ("Batches without the BQL").
- **Unigine Heaven in Direct3D 9**: the buffer cache and the late
  interleave took its benchmark from 27.3 to 70.5 frames/s (2026-11-27,
  "A host copy of VRAM buffers"); the executor is still ~43-56% busy,
  Direct3D 11 on the same scene ~200. Each texture also takes its size of the
  process's address space (CPU-visible allocations), so a 32-bit game
  has ~1.9 GiB for textures and its own memory together; the XP and 9x
  drivers' memory has no paging at all.
- **Windows 7's DWM on `d3dpt11.dll`** waits for a WDDM 1.1 32-bit kernel
  driver, likely the missing GDI acceleration ("Windows 7's DWM stays
  on Direct3D 9's driver").
- Logged and not done: a DXGI Blt between other formats than the
  8-bit four-channel ones, or multisampled, a Blt1 of part of its source,
  a Blt into a surface only Direct3D 9 knows, the video processor's
  Blt, ClearView of a view that is not a render target's, a blend
  state's logic op, a rasterizer state's forced sample count (DWM's make
  one, ignored).
- The vendors' raw-depth FOURCCs (INTZ, DF24, DF16) on the Direct3D 9
  drivers, and a probe of our own ("Depth textures").

## Log

Dated, oldest first: what was tried, measured and found. The sections
above are the summary. Items that left "Open" are here too, under their
date.

### Step 1 (2026-10-16)

`d3d11.dll` loads `d3dpt11.dll` (the names after the first in
`UserModeDriverName`) and calls `OpenAdapter10_2`; `D3D11CreateDevice`
with every level asked for gives **10_0** on our adapter (9_3 before,
through d3d10level9), the host opens DXVK's d3d11 (feature level 11_0).
A process the driver refuses (D3D11Exclude, or no `CAP_D3D11`) falls
back to d3d10level9 and the Direct3D 9 driver, as before: DWM stays
there (`wddm-install.ps1` writes `dwm.exe`).

- Every device the runtime made failed at the end with
  `DXGI_ERROR_DRIVER_INTERNAL_ERROR`, after the driver had answered
  everything: the runtime walks CheckMultisampleQualityLevels per format
  and count, and stopped at the first count claimed (2x, one level, for
  R32G32B32A32_FLOAT). With no multisampling claimed devices are made.
  Which claim it refuses is open.

### Step 2 (2026-10-16)

The probe's scene on our adapter at 10_0 against WARP in the same guest:
**0 of 65,536 pixels differ by more than 8** (the largest difference 1):
resources, an immutable and an updated texture, a dynamic constant
buffer, the input layout, shaders through the rebuilt DXBC container
(DXVK checked its hash and took it), sampler, blend, depth, an indexed
draw, a copy into staging read back through VRAM.

### Step 3, presenting (2026-10-16)

The scene through a windowed swap chain read off the screen after DWM:
**0 pixels differ** in both models. The blt model (DISCARD) goes through
the kernel driver's Present after a readback into the buffer's VRAM. The
flip model (FLIP_SEQUENTIAL) has DWM open the app's buffers through the
Direct3D 9 driver and sample them: black until those buffers were
described to it as render-target *textures*.

### Windows 11's shell on it (2026-10-16)

With DWM excluded, Explorer, Search, the Start menu, WebView2 and the
rest get 10_0 devices from this driver, and the taskbar draws whole
(icons, the search box, the tray, the clock). Two things it took:

- **Coherence between the two drivers' copies of a shared surface.** The
  host keeps a Direct3D 11 object and a Direct3D 9 one per allocation;
  this side marks a resource in VRAM newer whenever it draws, clears or
  copies into it, and the Direct3D 9 side's surface lookup pulls it back
  through VRAM first (`exec_d11_sync`). Without it every icon was a
  black square.
- **A staging map polled with DONOTWAIT** queued a readback and a flush
  each time, so the allocation was always busy and the runtime's answer
  always "still drawing": Explorer's icons (uploaded through 13x13 to
  46x46 staging textures) never arrived. A staging resource is read back
  only when the host wrote it since (a copy, an update, initial data).

Also: **a driver reinstall restarts the kernel driver's allocation
handles at 1 with no machine reset**, so the host held old objects under
reused handles (DXVK: "Incompatible view dimension for Buffer") and the
player died; a creator's RES_CREATE now replaces whatever had the handle.
**`CAP_D3D11`** (register set v10): the device asks the executor
(`d3dpt_exec_features`) whether a Direct3D 11 device opens, and the
driver refuses to open without it, so a host with no d3d11 (an older
executor, a package without DXVK's d3d11, the Wine executor) leaves the
guest on 9_3: checked with `D3DPT_DXVK_D3D11_LIB` pointing nowhere.

`scripts/test.sh all` on the PC with these changes (2026-10-16): 64
passed, 16 skipped, 3 failed (`cpus`, `clone`, `win11-snapshots`: the
launcher's machine files, with this checkout's `launcherx` older than
`main`; nothing this track touches).

### Step 4, DWM on it (2026-10-17)

Windows 11 composes through this driver: DWM's devices are 11.1 ones on
our adapter from logon, nothing of it goes to the Direct3D 9 driver, and
the desktop, the Start menu (its acrylic too), the taskbar and File
Explorer (its XAML header and Home page as well, M20's open bug) draw
whole, with one screen or with Basic Display's extended beside it, and
through live switches between the two. `d3d11-probe` passes offscreen
and through both swap chain models (0 pixels off). What it took:

- **The 11.1 interface.** With D3D11Exclude empty DWM made one device
  on us at 10_0 and composed through a second one on d3d10level9 anyway.
  `d3d11-probe flags` found why: every `D3D11CreateDevice` flag gets 10_0
  on our adapter except `VIDEO_SUPPORT`, which got 9_3, and DWM's
  composition device asks for it; the runtime gives such a device only
  to a driver with the 11.1 interface's video functions. The runtime
  takes the 11.1 interface over our WDDM 1.1 kernel driver (tested with
  the version offered to the probe alone first). `d11_video.c`: no
  decoder profiles, no content protection, a video processor for the RGB
  formats (its Blt logged, not done). The 11.1 table is the 11.0 one
  entry for entry plus five: the entries whose arguments changed (copy
  flags, `SetConstantBuffers1`, 11.1's blend and rasterizer descriptions,
  shaders with 11.1's signatures, which carry the component types, a
  flush that answers) and Discard, ClearView, CheckDirectFlipSupport (no),
  AssignDebugBinary, the constant-buffer map; DXGI 1.2's Blt1 and
  offer/reclaim. **Protocol v26**: a signature entry's component type,
  `SET_CB1`, `CLEAR_VIEW`.
- **Primaries the display scans out.** The driver marked DXGI's
  primaries `NO_SCANOUT` (step 1's choice); then DXGI never sets the
  display mode on DWM's primary, and its frames went where the screen
  never looked (a black desktop). They are scanned out now, as the
  Direct3D 9 driver's are.
- **Views follow a flip-model swap chain's rotation.** DWM renders
  through render target views it makes once per buffer; DXGI rotates the
  buffers' identities at each present, which the driver did to its
  resources but not to the host's views, so DWM drew into one host
  buffer while the screen flipped through the others (only the regions
  redrawn showed). Each view keeps its creation; after a rotation the
  views of the rotated buffers are made again under their ids and bound
  again where they were.
- **The fix-up reserve.** A kernel driver fix-up entry is three ULONGs
  and a pointer, 24 bytes on x64 (also for a 32-bit process under
  WoW64), and both user-mode drivers reserved 16 per patch location.
  DWM filled a command buffer to 251,616 bytes with 392 patches and the
  kernel driver refused it ("Render does not fit one DMA buffer"):
  every object it created was lost (`resource N unknown` on the host,
  the screen black after a topology change). Both reserve 24 now.

`scripts/test.sh all` on the PC with step 4 (2026-10-17): 67 passed, 16
skipped, none failed.

### WDDM 1.2's DDIs (2026-10-17)

(user: "also implement wddm 1.2"). The x64 kernel driver has Windows 8's
interface and WDDM 1.2's DDIs: the per-engine TDR
(QueryDependentEngineGroup, QueryEngineStatus, ResetEngine), handing the
screen to Basic Display (StopDeviceAndReleasePostDisplayOwnership) and
the bugcheck screen (SystemDisplayEnable / Write); with them reported
Windows lists the adapter as **WDDM 1.2, DDI 11.1** (dxdiag). It does
not report 1.2 yet (`D3DPT_CLAIM_WDDM12`): with it, every program that
presents through a window's swap chain made DWM fail fast, again and
again while the window lived. Bisected on the PC: the 11.1 user-mode
driver on a WDDM 1.1 kernel driver passes; Windows 8's interface with
the new DDIs and the 1.0 caps passes; `WDDMVersion = DXGKDDI_WDDMv1_2`
alone (no preemption caps, no per-engine TDR) fails, whether the program
is on the 11.1 or the Windows 7 interface and whether DWM is on this
driver or on d3d10level9. In DWM's dumps the error is
`DXGI_ERROR_DEVICE_REMOVED`, met in `CD3DDevice::CopyTexture2D` (from
`CSecondarySysmemBitmap::Update`, DWM's copy of a swap chain's buffer
through system memory, with a WARP lock) or `CreateTexture`; neither
driver logs an error, the kernel driver refuses no present, and dxgkrnl
logs no event. Left for later (user, 2026-10-17); found and claimed the
same day ("WDDM 1.2 claimed", below).

### The desktop at high resolutions (2026-10-17)

(user: "laggy on win 11 ... works well on low resolutions, but higher
resolutions are slow"). A maximized window repainted every 10 ms
(`LOAD\anim.ps1` on the test machine, run as the logged-on user): DWM
composed **~31 frames/s at 1600x1200 against ~58 at 1024x768**, with the
guest mostly idle (17% of four CPUs: System, where the executor runs
inside the submission, 34% of one, DWM 13%) and the host executor 22%
busy. A latency problem, then: at 1600x1200 DWM's frame is ~17 Direct3D
9 submissions (the window into a full-screen intermediate, the taskbar
acrylic's blur chain, the primary) against ~4 at 1024x768, and the
Direct3D 9 user-mode driver flushed its command buffer at every Lock, so
each draw after a write to DWM's dynamic vertex ring was a submission of
its own and the lock then waited for that submission's fence (dxgkrnl's
scheduler, the device's interrupt, the DPC). DWM missed every other
vertical blank. A NOOVERWRITE lock no longer flushes (the caller writes
only bytes no queued command reads, and dxgkrnl, which knows of no work
on the allocation, locks it at once): the same load at 1600x1200 now
composes at **54-58 frames/s**, ~900 batches in 5 s instead of ~6,300.
The Direct3D 11 driver had no such cost (a dynamic resource maps a CPU
copy and its unmap sends the bytes that changed). Not it, measured on
the way: the segment (`vgamem_mb=512` gave the same 31) and the
readbacks' size (~3.8 ms at 1600x1200, ~3.4 ms at 1024x768, most of it
the wait for the GPU).

Seen on the way: under the full-window load the guest twice hit a TDR
within ~30 s and restarted (below, fixed); and DWM composed on
d3d10level9, the Direct3D 9 driver, not as step 4 found it.

### A TDR under a full-screen repaint (2026-10-17, fixed by QEMU patch 94)

Seen under `LOAD\anim.ps1` maximized at 1600x1200, before the
NOOVERWRITE change (not in one run after it): `CollectDbgInfo` with every fence done on
the host and the last one never reported (`fences submitted 0xbb37 done
0xbb37 notified 0xbb36`), the device's `IRQ_STATUS` 3 (both events
pending, enabled) while no interrupt service ran, then
`ResetFromTimeout` and a guest restart. **The cause is QEMU's**: WHPX's
APIC EOI exit called `ioapic_eoi_broadcast()` without the BQL from the
vCPU loop while the device raised its line under it from another vCPU,
and the two interleave so that neither delivers (patch 94,
`094-whpx-eoi-bql`, which takes the BQL; every guest's level-triggered
INTx under WHPX could lose its line so, an XP's sound or USB card on an
APIC HAL included; KVM's IOAPIC is in the kernel and TCG's EOI holds the
BQL). Checked 2026-10-17: the old Direct3D 9 driver (a submission per
draw, the most interrupts), which hit the TDR within ~30-40 s three runs
of three, ran the same load on the patched QEMU three times 180 s with
no TDR and no restart. Fixed.

### Why DWM is on d3d10level9: the WDDM version (2026-10-17)

(user: "could it be because of the wddm 1.1 / 1.2 difference?"). It is.
A `D3D11CreateDevice` is two opens of the adapter: a probe (`GetCaps`
130, the pipeline levels; `GetSupportedVersions`) and the device's (the
versions again, the other caps, `CreateDevice`). DWM's first device
(flags 0) is made on the 11.1 interface (`0xb000f` is 11.15,
`D3D11_1_DDI_INTERFACE_VERSION`, not Windows 7's `0xb000b`); for its
composition device (flags `0x11` at the DDI) the runtime makes the
second open, asks only the supported versions and closes it, before any
of the user-mode driver's caps, so the refusal is on the kernel driver's
side; after a third probe DWM opens the Direct3D 9 driver. With
`D3DPT_CLAIM_WDDM12` 1 every DWM, the one at logon included, gets the
composition device here (dxdiag: WDDM 1.2, DDI 11.1, levels 10_0 to
9_1), and the other processes' 0x11 devices land on the 11.1 interface
at 1.1 too. So step 4's DWM ran on the session's 1.2-claiming builds;
the commit that turned the claim off (DWM's device removed whenever a
program presents through a window's swap chain, above) sent DWM back to
d3d10level9, and its "either way" was wrong. Rechecked with QEMU patch
94: at 1.2, `d3d11-probe present 12000` still makes DWM crash (Windows
Error Reporting, a new DWM) and both presented scenes read back black.
Not it: DXGI's adapter cache (`HKLM\SOFTWARE\Microsoft\DirectX\{...}`,
which holds 9_1 and driver version 0 for this adapter; set to 10_0, DWM
still fell back).

### Widescreen modes (2026-10-17)

(user: "add more resolution options to our driver on windows 10/11").
The host's mode table is the XP-era one (640x480 to 1600x1200, 4:3 and
5:4, shared with the XP miniport); the WDDM kernel driver now adds
1280x720, 1280x800, 1366x768, 1440x900, 1600x900, 1680x1050, 1920x1080,
1920x1200, 2560x1440, 2560x1600 and 3840x2160 at 60 Hz (`wide_sizes`),
each where six of its frames fit the segment (DWM's primaries, its
intermediate and the windows' surfaces): the default 63 MiB stops at
1920x1200, `vgamem_mb=256` adds 2560x1600, `vgamem_mb=512` 3840x2160.
The device takes any linear mode. XP and 9x keep their lists. The same
full-window load with the NOOVERWRITE change, each from a fresh boot:
**1920x1080 57-59 frames/s** on the default segment (a run right after
switching modes in the same session gave 30, not seen again from a
boot); **2560x1440 ~32** (`vgamem_mb=256`), with the guest mostly idle
and the executor 28% busy: per frame the primary's readback (~6.5 ms,
~3.8 of it waiting for the GPU and the copy across) and the
untracked-pixel passes over the same 14.7 MB (the frame's first draw
compares the target with its shadow, ~1.5 ms; the readback compares
again and copies twice) run inside DWM's present, on the vCPU, and the
flip misses its blank. What would take it further: the readback only of
the rectangles drawn since the last one (a desktop that changes a little
changes a little), and the readback's copy done at the flip instead of
inside the present.

### A game-like scene on Direct3D 10 / 11 (2026-10-17)

(user: "create a mini game-like scene using directx 10/11 features, same
way you did for directx 8/9"). `guest-tools/src/d3dgame11.c`, on
d3dgame.h's scene: the cubes in one instanced draw with a texture array,
the wave grid made on the GPU and captured by stream output, DrawAuto,
geometry-shader billboards, GenerateMips, occlusion predicates and an
FP16 target tonemapped by a fullscreen triangle with no vertex buffer,
all at 10_0; `-compare N` checks frame N against WARP (docs/testing.md).
On this PC's RTX 3090 natively: 9 pixels of 307,200 off. In the guest,
first run: 351 off, 342 in one band: **the red marker drew**. DXVK's
d3d11 stubs `SetPredication` (it keeps the predicate and draws), so the
executor now waits for a predicate's answer when it is set and skips the
predicated operations itself (doc 14). Then: 9 off, x64 and the WoW64
32-bit build alike, the occlusion counts equal to WARP's (182 and 0); on
the desktop through either swap chain model (screendumps), 34-37
frames/s windowed at 640x480 with DISCARD, ~58 with FLIP_SEQUENTIAL. A
console window over a program's own (a scheduled task's `cmd`) reads as
the window missing in a screendump: `G11\g11.ps1` runs it under `conhost
--headless`.

### WDDM 1.2 claimed (2026-10-17)

(user, after a session on the desktop: "everything is laggy and glitchy
... let's take care of the speed first"). With DWM on d3d10level9 every
Direct3D 11 window's frame crossed to the Direct3D 9 driver through VRAM
(the host's d3d11 object read back, the d3d9 one uploaded again), and
DWM's primary went to VRAM for the display after that: two waits for the
GPU a frame, on the one executor thread, which was 55-60% busy with
d3dgame11 windowed. The x64 kernel driver now claims WDDM 1.2 (and
`SupportDirectFlip`, mandatory at 1.2, whose user-mode check already
answers no), DWM's composition device is ours from logon, and the same
run keeps the executor 19-24% busy.

The window swap chain bug, narrowed down on the guest: the blt model
(DISCARD) never broke DWM, the **flip model** always did (d3dgame11
`-flip`, the probe's second window; the probe's "every program" was its
flip half). DWM's own driver log was spent long before (4000 lines a
process), so `D3D11Trace` now lifts a traced process's cap to 400,000;
the trace showed DWM opening the program's two buffers, a flush, and its
device gone. Windows' own ETW (DxgKrnl, DXGI, Direct3D 11 through
`logman`, `G11\etw.ps1`) has no event for it. **cdb attached to DWM**
(`DBG\dbg.ps1`: the EWDK's x64 debugger and the PC's downloaded symbols
on the command CD, the guest having no network; breakpoints on
`d3d11!CDevice::RemoveDevice` and its callers) caught it:
`NDXGI::CDevice::RenderCB` removing the device with
**`DXGI_ERROR_ACCESS_DENIED`**, under our `cmd_flush` in DWM's present.
dxgkrnl refused the submission: all three user-mode drivers listed every
allocation with `WriteOperation`, and a 1.2 kernel refuses a write to an
allocation the device opened read-only, as DWM opens a flip-model swap
chain's buffers (1.1 never checked). Now Direct3D 11's `patch_res` lists
another process's allocation as read and `patch_dst` (a copy's, an
update's, a blt's destination; a readback) as written; the Direct3D 9
driver lists any opened allocation as read (the host runs a submission
to its end before its fence, so the flag orders no wait of ours).

Checked at 1920x1200 (the user's mode): d3dgame11 through either model
with no DWM crash, `-compare 60` PASS (4 of 307,200 off, the occlusion
counts WARP's), `d3d11-probe present` 0 pixels off in both models (it is
DPI-aware now: Windows scaled its unaware window 175% and the screen was
read where the window was not), File Explorer whole, d3dgame9 ~35
frames/s, the maximized repaint 54-56 frames/s. d3dgame11 windowed: ~57
frames/s with FLIP_SEQUENTIAL, still ~37 with DISCARD (both as before;
below). The reference scenes are DPI-aware too (`d3dgame.h`): their
640x480 was shown at 1120x840, every pixel resampled. The user, on the
desktop by hand with this driver: "the desktop is actually feeling much
snappier, no visual glitches anymore and the demo games are slow but
don't have terrible frame pacing. the desktop is already feeling better
than the viogpudo one". `scripts/test.sh all` on the PC with these
changes: 67 passed, 16 skipped, none failed.

### Flips that hold nothing (2026-10-18)

(the user, watching d3dgame11 windowed with DISCARD: "it oscilates
between fast and slow. like, several frames fast, then several slow").
Not the VRAM crossings: a per-frame trace (`d3dgame11 -trace N`) had
runs of ~33 ms frames and runs of ~17 ms, nearly all of each inside
`Present`, the executor and DWM mostly idle (`D3DPT_D11_PROF=1`). The
kernel driver held a flip's fence until the vertical blank (the 600
frames/s fix of M20), and fences complete in order, so **every process's
later submissions** waited behind DWM's flip: a present just before
DWM's took one blank, one just after it two, and the phase between the
two clocks drifted.

- `FlipOnVSyncWithNoWait` alone (the flip's fence done at once, the
  scanout and a CRTC_VSYNC at the blank) was bugcheck 0x113, subtype
  0x35, at the adapter's next stop (the restart ~9-14 s into each boot,
  or a reinstall): `VIDMM_FLIP_QUEUE_REFERENCES::ReleaseReference` on a
  destroyed allocation with 0x5a / 0x65 / 0xb3 flips still counted on
  source 0. Read in dxgmms1 with cdb: a DMA flip's queue entry is
  retired only when a vertical blank's cookie matches the address the
  entry holds, which none of ours ever did. Reporting the shown flip
  when the blank's interrupt went off did not help. The guest sat in
  recovery (QMP `send-key ret` twice gets past it).
- **`FlipOnVSyncMmIo`** (with `FlipOnVSyncWithNoWait`, x64 only): a flip
  on the blank is no DMA buffer (`Present` gets a NULL one) but
  `SetVidPnSourceAddress` with `FlipOnNextVSync`, called once the
  frame's rendering is done; the driver latches it and the blank shows it
  and reports **the very 64-bit `PrimaryAddress`** it was given
  (`shown_pa`), which is what the cookie is compared with. No fence
  waits for a blank any more; dxgkrnl paces the flips itself.

d3dgame11 windowed at 640x480, `-novsync`: DISCARD 70-115 frames/s
(30-45 before), FLIP_SEQUENTIAL ~400 (~57); with vsync, ~50-57, the
frame intervals one spread around 17-20 ms instead of two runs;
full-screen d3dgame9 still exactly 60. The driver reinstalled and the
boot-time restart stopped the adapter with no bugcheck after ~19,700
blanks of DWM's MMIO flips. A redirection bitmap preferring the aperture
(the CPU reads it, and VRAM is an uncached BAR under WHPX) measured no
gain (84-89 against 79-93) and was left out. `scripts/test.sh all` on
the PC: 67 passed, 16 skipped, none failed (run from the PowerShell
tool: `win.ps1` started from Git Bash gets `pwd` as `C:/...`, whose
colon breaks `PATH`, and the player's two checks fail with no output).

Also: `D3D11Trace` / `D3D11Exclude` never reached a 32-bit program (its
`RegGetValueA` read `WOW6432Node`); it asks for the 64-bit view now
(`RRF_SUBKEY_WOW6464KEY`, defined by hand: the EWDK's headers predate it).
Windows turning the screen off (`SetVidPnSourceVisibility 0`, the adapter
to D3) leaves the device's last frame up, which reads as a frozen guest;
the test machine's display and sleep timeouts are off (`powercfg`).

### The host's vertical blank, again (2026-10-18)

(the user: "first fix vblank syncing with host"; M24's design,
`screen::HostVBlank`, QEMU patch 90, `d3dpt-vga`'s `host-vblank`). Two
faults:

- **The host's blanks had stopped coming.** The device's last summary
  read `16284 the host's, 20413 the timer's`, and not one came after: the
  frame traces above ran on the device's own 60 Hz timer. On the PC's new
  screen (an LG 4K at 60 Hz, the Meta Quest runtime installed)
  `D3DKMTWaitForVerticalBlankEvent` failed at once with
  `STATUS_GRAPHICS_PRESENT_OCCLUDED` (0xC01E0006), as did
  `DCompositionWaitForCompositorClock`, while `DwmFlush` kept DWM's
  16.7 ms. The player's thread took every failure for a monitor gone,
  reopened it, and said nothing. Now an occluded wait is answered by DWM's
  next composition (never sooner than three quarters of the screen's
  period after the last tick, as an idle DWM returns at once), and the
  player says which: `[display] the host screen's vertical blank is
  refused (occluded): DWM's composition stands in for it`, `... is back`,
  `... failed (0x...)`. The refusal comes and goes with the host's display
  (the next boot waited on the real blank again).
- **The modes said 60 Hz whatever the blank's rate.** QEMU patch 96 keeps
  the rate the player gives the guest's blank (`qemu_embed_set_refresh_rate`,
  72 Hz on a 144 Hz screen), `d3dpt-vga` reads it out of `HOST_MHZ`
  (register set v11), and the WDDM driver advertises every mode at it, a
  rational (59,997/1000 here; DWM's `rateRefresh` now 16.665 ms, 16.819
  before). Not through the console's UI info, as virtio-gpu has it: a
  device that takes UI info is one whose guest follows the window's size.
  The made-up EDID's range limits are 24-240 Hz, 15-255 kHz and 2550 MHz
  (50-75 Hz, 80 kHz and 170 MHz before: 72 Hz on a 144 Hz screen is
  86 kHz at 1920x1200 and 155 kHz at 3840x2160 with no blanking). A
  changed EDID is a new monitor to an installed guest, which starts it at
  1024x768 once (the user: no problem, for now).

On the way: `ChangeDisplaySettingsEx` with `CDS_UPDATEREGISTRY` from a
scheduled task fails (-1) for a monitor Windows has stored nothing for,
while the same change without the registry succeeds: the test's tool, not
the driver. `SETRES\setres.ps1` on the test machine (`size.txt` beside it,
"W H") now makes the change without the registry, then saves the active
configuration as the Settings app does (`SetDisplayConfig` with
`SDC_SAVE_TO_DATABASE`); the new monitor came back at 1920x1200 after a
restart.

Measured with the host's blank: DWM composes 223 of 300 frames in its
16-17 ms bucket (`guest-tools/viogpudo/dwm-pace.ps1`, 5th to 95th
percentile 14.6-18.0 ms, none doubled); d3dgame11 windowed with vsync,
flip model, 149 of 201 frames at 16.5-17.5 ms, 12 doubled. Timestamps at
both ends (removed) put the ±2 ms between the player and the device: the
host's blanks reached QEMU 13.8-19.5 ms apart (87 of 600 off by over
1 ms), the guest's interrupt 13.6-19.4. A blank waits for the main loop,
and the main loop for the BQL, which a vCPU holds through a whole batch:
the DOORBELL write runs the executor inside it, 1-3 ms for a DWM frame.
Releasing the BQL around `submit` alone (a prototype) bugchecked the guest
at boot twice: the batch needs the device's state guarded properly first.
`scripts/test.sh all` on the PC with these changes: 67 passed, 16 skipped,
none failed.

### Batches without the BQL (2026-10-18)

(user: "continue with driver optimization"). Under a hardware
accelerator `d3dpt-vga` now drops the BQL around the executor's `submit`
(its own `exec_lock` serialises the executor's calls; TCG keeps the
BQL). The executor reads only RAM (the window, VRAM) and calls back only
the log and `memory_region_set_dirty`; a reset pauses the vCPUs and
waits for this one to hold the BQL again. The earlier prototype's
bugcheck, and this one's first boots, were **0x119 subtype 1, fence 0
reported** (minidumps read with cdb in the guest; the driver's fence
reports logged): QEMU's re-entrancy guard is the device's, not the
thread's, so while one vCPU sat in the DOORBELL write another vCPU's
register read was refused (`Blocked re-entrant IO on MemoryRegion:
d3dpt-vga.regs at addr: 0xBC`, FENCE_DONE), read 0, and the interrupt
service reported fence 0. The doorbell lowers the guard while the BQL is
away and raises it again under it (the executor makes no DMA). One run
each on the same build, the BQL held / released: d3dgame11 windowed,
flip model, unsynced **253 / 323 frames/s**, DISCARD 83 / 93; with
vsync, flip 296 / 311 of 380 frames at 15.5-17.8 ms (13 / 5 doubled),
DISCARD 73 / 57 (47 / 56 doubled, within one run's noise). **DWM's pace
did not change** (`dwm-pace.ps1` p5-p95 14.4-18.1 / 14.4-18.2 ms): the
±2 ms on the guest's blank is not the batch under the BQL, as guessed
above.

### Where a DISCARD window's frame goes (2026-10-18)

The kernel driver's Present of a blt-model window copies the buffer into
a **STAGING** standard allocation (no host handle), not into a surface
DWM samples: the driver claims no GDI hardware acceleration, so every
window's redirection surface is in system memory, and Windows reads the
staging copy with the CPU and DWM uploads it again. Each present waits
for a DWM composition (unsynced it measures 16-17 ms in `Present`,
~60-90 frames/s). A copy on the host GPU has no destination object to
copy into, so `PRESENT_BLT` (a host-side present copy, drafted) was
dropped. What would change it: redirection surfaces in video memory,
`SupportSoftwareDeviceBitmaps` (CPU-visible texture redirection bitmaps)
or full GDI acceleration, both of which change every window and not only
Direct3D ones.

### `SupportSoftwareDeviceBitmaps`, tried (2026-10-18)

(user: "do the experiment"). Claimed, Windows 11 asks
`GetStandardAllocationDriverData` for `GDISURFACE` type 6 only
(`TEXTURE_CPUVISIBLE`, A8R8G8B8: 1x1, the taskbar's 1920x60, 1920x1200
surfaces, each window with its frame); refused, it falls back to the
system-memory bitmaps with the desktop whole. Taken (an allocation in
VRAM with a host handle, a render-target texture), the desktop stayed
whole and DWM never opened one through the Direct3D 11 driver, and **a
DISCARD present still copied into a 640x480 STAGING surface** while its
window had a 646x515 bitmap of this kind: Windows moves the frame into
the bitmap on the CPU either way. So CPU-visible bitmaps change where
GDI draws (into VRAM, an uncached BAR under WHPX, with no way for the
host to see the writes) and nothing about the blt-model present;
reverted. What is left is GDI acceleration proper
(`SupportKernelModeCommandBuffer`, `DxgkDdiRenderKm`, type 1 `TEXTURE`
bitmaps the CPU never touches).

### GDI acceleration, logged only (2026-10-18)

(user: "yes, go on"). `SupportKernelModeCommandBuffer` claimed,
`DxgkDdiRenderKm` logging its commands and emitting an empty DMA buffer.
Windows 11 asks for `STAGING_CPUVISIBLE` (type 2, 1920x1200 and the
lookup table's 512x16), `STAGING` (3, 1920x64) and `LOOKUPTABLE` (4,
512x16 A8: ClearType's gamma); while the lookup table is refused it
retries and stays on software GDI (no RenderKm at all). With all three
taken it asks for **`TEXTURE` bitmaps** (type 1: 1920x1200, 1920x60, a
window's 646x515, 1x1), and RenderKm brings ColorFills into them and
SRCCOPY BitBlts from a CPU-visible staging surface. Then **a DISCARD
present copies straight from the swap chain's buffer into its window's
`TEXTURE` bitmap** (no staging, no CPU copy), and d3dgame11 with vsync
held 60.0 frames/s, 16.61 ms in each present, over 1800 frames (median
~20 ms with doubled frames before). DWM opens the bitmaps through the
Direct3D 11 driver (`OpenResource` 646x515, 1920x1200, 1920x60), so what
it samples is a host object: a present's copy can be a copy on the host
GPU. With nothing drawn the wallpaper and the window's inside were
black; DWM's own title bars and taskbar drew. Reverted. Doing it for
real: each RenderKm operation run on the host into the bitmap's object
(BitBlt with its ROPs, ColorFill, AlphaBlend, StretchBlt,
TransparentBlt, ClearType blend against the lookup table), the staging
surfaces uploaded from VRAM where they are sources and read back where
they are destinations, and the present's Blt into a bitmap as a host
copy.

### GDI acceleration, copies, fills and the present (2026-10-18)

(user: "start with copies, fills and the present"; then "this branch is
experimental, you can leave it on"). On by default on this branch: the
x64 kernel driver claims `SupportKernelModeCommandBuffer` when the
executor is present, unless the service key's `GdiAcceleration` is 0
(`HKLM\SYSTEM\CurrentControlSet\Services\d3dptkmd`, read at DriverEntry;
a restart applies it). Then:

- **GDI surfaces** (`GetStandardAllocationDriverData`'s `GDISURFACE`):
  `TEXTURE` bitmaps (a host handle, a render-target texture there, not
  CPU-visible), the two staging types and ClearType's lookup table, all
  in VRAM (kind `D3DPT_ALLOC_GDI`, the type in `source`).
- **RenderKm** (`d3dpt_render_km`): a `PKT_REG` of the allocation list,
  as Render's, then a `PKT_GDI` per SRCCOPY BitBlt and PATCOPY ColorFill
  (their sub-rectangles; `MultipassOffset` the next command's offset
  when the DMA buffer is full); the submission hands each to the host as
  protocol v27's `GDI` record. The other operations are counted and
  logged every 5 s (`RenderKm in 5 s, not done yet`).
- **The host** (`exec_d11_gdi`): each side is the bitmap's Direct3D 11
  object (made at its first use; DWM's `RES_CREATE` then finds it) or
  VRAM (the staging surfaces; the host's pixels synced there first):
  `CopySubresourceRegion` (through a temporary within one bitmap when the
  rectangles overlap), `UpdateSubresource` from VRAM, a staged readback
  into VRAM, `memmove`; fills by `ClearView` with alpha 255 (GDI leaves a
  fill's alpha 0, and DWM composes the bitmap with it: a white fill came
  out black).
- **The present** into a `TEXTURE` bitmap is the same record, a copy on
  the host GPU (`gdi_present`), where a blt-model window's frame went
  through a staging surface the CPU read.
- **Paging**: a bitmap transferred out of VRAM is written back from its
  object first, and re-read after a transfer brings it in (`gdi_paged`,
  `D3DPT_GDI_VRAM`): GDI drawing on the CPU pages it through system
  memory (the logon's 1920x1200 bitmap went out and in once).

d3dgame11 windowed, DISCARD, with it on: with vsync 304 of 380 frames at
15.5-17.8 ms and 1 doubled (median 16.7 ms; 57 and 56 before), unsynced
**340 frames/s** (93 before; the flip model's 323), 300 host copies in
each 5 s, the scene right on the screen. **GDI's own content is not
there yet**: the wallpaper and a classic dialog's inside (`winver`) stay
black, Notepad's text area blank. For `winver` the host got one fill of
a 403x20 band (and, another boot, a white fill of the whole window) and
nothing else: no copy, no paging, and its bitmap's VRAM was black when
re-read (a destructive test, removed), so its drawing reaches the window
some way not found yet; ColorFill's PATINVERT (the caret) and the other
operations are not done. Off (`GdiAcceleration` = 0) nothing changes:
Windows asks for no GDI surface.

### GDI's own drawing (2026-10-18)

GDI draws most of a window (text, the wallpaper, a dialog's logo and
background) with the CPU straight into the redirection bitmap, and only
into one that is CPU-visible: with the bitmaps not CPU-visible that
drawing was simply lost (dxgkrnl does not page them for it), which was
the black. Now the bitmaps are CPU-visible, and the host keeps a
**shadow** of each bitmap's VRAM: a Direct3D submission naming a bitmap
(DWM about to sample it) sends protocol v27's `GDI` REFRESH first, and
the host uploads the spans of VRAM that differ from the shadow (row by
row, rows of one span together). A bitmap's first touch by the host (a
GDI operation, a present) uploads its VRAM and starts the shadow first;
the host's fills and copies from VRAM go into the bitmap's VRAM and
shadow too; a copy between objects (a present) stays in the object until
the CPU draws over it. The refresh costs under 1 ms of the executor
every 5 s (`D3DPT_D11_PROF`, then token 63). The desktop, `winver`
(logo, text, the OK button) and a DISCARD window's scene all draw.

### The price: a present into a CPU-visible bitmap is held (2026-10-18)

D3dgame11 windowed, DISCARD, unsynced: ~85 frames/s in runs of ~3 ms and
~15-20 ms presents (the user saw the fast/slow oscillation again),
against ~430 frames/s with the bitmaps not CPU-visible (with or without
the refresh: it is the flag). ETW in the guest (`G11\etw2.ps1` on the
test machine, DxgKrnl's ids read from the manifest): the present is a
plain Blt (`bRedirectedPresent` false), and the game thread's per-frame
Lock (event 41) waits 13-17 ms (event 103) behind it, about one
composition; the title is set once a second, so it is not GDI drawing
the caption. What holds a GPU write into a CPU-visible bitmap is not
found yet.

### Where the present really goes, and readbacks that wait (2026-10-18)

(user: "continue with driver optimization"). ETW again (`G11\etw3.ps1`
on the test machine: every process's queue and DMA packets, the game's
Lock and DXGI's Present, merged over 120 ms of the steady state), and
the kernel driver logging a present's destination: with the bitmaps
CPU-visible **the present is not into the redirection bitmap at all**.
The game's window has its 646x515 `TEXTURE` bitmap, but its frame goes
into a 640x480 STAGING standard allocation, and the game thread waits
for that packet and locks the staging surface (read-only) for Windows'
CPU copy into the bitmap, as with `SupportSoftwareDeviceBitmaps`
(above): a CPU-visible bitmap is filled by the CPU whatever its type. So
the ~85 frames/s are the old system-memory path, and the fast host
copies of "GDI acceleration, copies, fills and the present" need bitmaps
that are not CPU-visible; how GDI's CPU drawing reaches those is what is
open.

The profile (`D3DPT_D11_PROF`, now with GDI records as 64 + their op and
a readback's two halves as 72 and 73) found the other cost: **a
readback waits for the host GPU**, 2-4 ms in `Map` (the GPU finishing the
frame) against ~1 ms copying into VRAM, and the user-mode driver read
the buffer back at every present: ~1 s of the executor's in each 5 s,
mostly DWM's 1920x1200 flips, every guest submission queued behind each.
Protocol v28:

- A **blt-model present** reads nothing back in the user-mode driver.
  The kernel driver's present into a redirection bitmap is a host copy
  (`gdi_present`); one through VRAM (a STAGING destination, as now)
  first sends v28's GDI `SYNC` for its source (the host's pixels into
  VRAM, nothing if VRAM has them: the executor's `newer`), and so does a
  paging transfer out of VRAM of a Direct3D 11 allocation.
- A **flip's readback** is `RES_SCANOUT`: the host copies the buffer
  into its staging twin and leaves the copy into VRAM owed. It is paid
  at the next batch if the GPU is done (`DO_NOT_WAIT`), before anything
  else reads that allocation's VRAM (`exec_d11_sync`, a later readback,
  `SYNC`), and by the display: `d3dpt-vga`'s refresh and its flip-push
  (the page copied at the flip's register write) call the new optional
  `d3dpt_exec_settle` under `exec_lock`, waiting for a running batch.
  The first version settled only at the refresh, with a try-lock: the
  flip-push copied the page before it was paid, and everything that moved
  on the desktop flickered with its own past (the user: "like reverb"),
  the frame of two or three flips before. An executor
  never asked to settle (the remote one under Wine) defers nothing. A
  guest CPU write to that VRAM (`RES_DIRTY`) drops what is owed. The
  first build recursed: the VRAM lookup syncs the allocation, which
  settled the same entry again (a stack overflow in the vCPU thread);
  an entry leaves the list before it is paid.

d3dgame11 windowed, one run each, the profile on: flip model unsynced
560 frames/s with the flicker (the try-locked settle), **~350-440 with
it fixed** (900 frames in 2.0-2.6 s, three runs; 323 before), with vsync
60.0 frames/s at 16.4 ms in each present (with the flicker); the user
then saw the desktop right ("it looks good now"); a readback's wait for the GPU 0.03-0.5 ms where it was 2-4;
the desktop draws, the clock current. DISCARD unchanged, as above (~80
unsynced, 47 with vsync): its present still goes through STAGING, now
with a `SYNC` (~1.1 ms, the GPU's frame) where the readback was.

### Bitmaps the CPU cannot see, again (2026-10-18)

(user: "go on"). The kernel driver gained the service key's
`GdiCpuVisible` (REG_DWORD, default 1; 0 makes the `TEXTURE` bitmaps not
CPU-visible, read at DriverEntry) and a census of RenderKm: every 5 s a
`RenderKm in 5 s:` line per operation, ROP and surface kinds (kind * 16
+ GDI type: 0x51 a redirection bitmap, 0x52 / 0x53 the staging ones,
0x54 the lookup table, 0x40 a Direct3D allocation), with the first one's
sizes and host handles; and a line per GDI surface paged (the first 64).
(Both need the service key's `Trace` = 1 since 2026-10-23.)

- **CPU-visible** (the default): RenderKm carries almost nothing (two
  copies in 5 s, into ClearType's lookup table). GDI draws with the CPU,
  in effect software GDI on bitmaps in VRAM.
- **Not CPU-visible**: the wallpaper and `winver`'s inside black as
  before, and now Notepad's text area too. `winver` gets its 591x529
  `TEXTURE` bitmap and then **no RenderKm command and no paging at all**:
  its drawing goes nowhere the driver sees. Notepad's is a BitBlt from
  the bitmap into a 944x508 Direct3D allocation and back (GDI drawing on
  a Direct3D surface with the CPU), which the host copied from its object
  and not from the VRAM the CPU drew into. Windows 11 draws GDI with the
  CPU into a CPU-visible redirection surface and has no other route for
  it, so the bitmaps stay CPU-visible, and what is left is making the
  STAGING path fast.

So the blt-model present into a STAGING surface is now a host copy as
well (`gdi_present`, a Direct3D 11 buffer of 32 bits a pixel): the host
reads the frame straight into the staging surface's VRAM, where `SYNC`
put it into the buffer's own VRAM and this CPU copied it on through the
BAR. The scene is right (a screendump of the window); the rate is not
moved, ~80 frames/s unsynced and ~45 with vsync (one run each), so the
guest's copy was not what held the frame.

### Where the STAGING frame went (2026-10-18)

(user: "proceed"). An ETW timeline (`etw3.ps1`, d3dgame11 DISCARD
unsynced, median present interval 11.8 ms) and the executor's profile
showed two costs:

- **The display waited for the flip's readback under the BQL.** At every
  one of DWM's flips (~57/s) the flip-push and the refresh settled the
  owed readback with a blocking Map: 2.8 ms of GPU wait (slot 72) plus
  1 ms of copy on a vCPU or the main loop, holding the BQL, ~20% of the
  time with the whole guest stopped. `d3dpt_exec_settle` now takes
  `wait` and returns how many readbacks are still owed; the device tries
  it (`fb_settled`: a trylock of the executor's lock and a Map with
  `DO_NOT_WAIT`), and a flip-push that finds its page owed comes back
  from a 1 ms realtime timer (the newest page then; after 20 tries it
  waits), a refresh skips (after 3 it waits). Nothing stale is shown, so
  no flicker. The executor flushes once an owed copy is queued, so the
  GPU has it. Slot 72 went from 796 ms to 9 ms per 5 s; the rate from
  ~85 to ~97-119 frames/s (median interval 8.9 ms).
- **The game's CPU copy out of the STAGING surface read write-combined
  memory.** Between the game's Lock of the staging surface and its
  Present, 1.6-13 ms: segment 0 was not `CacheCoherent`, so Windows
  mapped the Lock write-combined and every read was uncached. Both
  segments are now `CacheCoherent` and a staging allocation `Cached`
  (the service key's `Cached` = 0 goes back; both are the host's RAM or
  the guest's, which the host reads and writes coherently, and WHPX does
  not trap VRAM: its `log_sync` marks it all dirty). The copy is
  0.1-0.4 ms.

DISCARD unsynced is now **~210-240 frames/s** (600 frames in 2.9 s,
1200 in 5.0 s; ~85 before), with vsync 58.3 (~45 before); the flip
model unsynced 515 (~350-440), with vsync 58.8. The scene and the
desktop are right (screendumps mid-run).

### GDI's other operations (2026-10-19)

(user: "proceed until you implement all the remaining GDI operations").
RenderKm's every operation now reaches the host (protocol v29,
`d3dpt_gdi2`): a BitBlt or a ColorFill with any ROP (the DDI's named
ones mapped to their ROP3, P the fill's colour), StretchBlt
(COLORONCOLOR, MirrorX / MirrorY, and the mono modes, whose shrink ANDs
or ORs the pixels it drops), AlphaBlend (constant alpha, premultiplied
per-pixel alpha, both, stretched), TransparentBlt (the key with or
without its alpha) and ClearTypeBlend (the DDI's own formula, with the
lookup table's gamma row or without). The kernel driver turns each into
a `PKT_GDI2` (`gdi_render2`, its allocations' places filled at submit
time as the copies' are) and claims `SupportAllBltRops`,
`SupportMirrorStretchBlt` and `SupportMonoStretchBltModes`; the host
does them on the CPU (`gdi_ex`): the destination's sub-rectangles and
the source rectangle read (an object's through a staging copy, VRAM's in
place), computed, and written back as a copy's are (the object, its VRAM
and shadow). A ROP into a redirection bitmap comes out opaque, as a fill
does.

Windows 11 does not send them. `gditest.exe` (new: each operation on a
window against the same on a DIB section) is 34 of 34 exact on the test
machine, and the census shows why: GDI drew all of it with the CPU, and
RenderKm carried one copy into ClearType's lookup table. With
`GdiCpuVisible=0` every case fails as `winver` did, nothing coming through
RenderKm. So the host's half is proved without a guest:
`tools/d3dpt-gdi-test.cpp` (`d3dpt-gdi` in `test.sh`) feeds the records
to the executor, into plain VRAM and into a redirection bitmap's object,
against this PC's GDI on DIB sections and, where GDI has no call (a
mirrored stretch, the mono modes, ClearType's blend), the DDI's formula:
90 checks, all exact but AlphaBlend's rounding (within 1). It found a
vertical mirror applied to AlphaBlend whenever its constant alpha had
bit 1 set (the flags share a word). Windows 7's GDI does use RenderKm,
but its WDDM 1.1 build has no GDI acceleration (track M18's).

### GDI-compatible textures, and GDI acceleration off again (2026-10-19)

(user: "just continue"). Opening common windows one by one on the test
machine (Notepad, `winver`, Character Map, a console in Terminal): all
drew but **Notepad's text area, black with the default settings**, not
only with `GdiCpuVisible=0`. Its rich-edit control draws with GDI into a
Direct3D 11 texture made `GDI_COMPATIBLE` (`IDXGISurface1::GetDC`; the
runtime keeps the flag from the driver, `d3d10umddi.h` reserves it, and
makes the texture shared: Notepad's 944x508 `misc 0x2`). `gditest`
gained three cases that do the same (a clear, GDI's fill and opaque
text, GDI reading the texture back, a Direct3D copy, a GDI invert across
it) and failed them: the texture kept Direct3D's clear, and GDI's own
read gave nothing (the DIB's zeros). The only RenderKm the census saw
was the invert (a ColorFill DSTINVERT on the Direct3D allocation);
`GetDC` reaches the user-mode driver only as DXGI's
`ResolveSharedResource`, and nothing marks `ReleaseDC`. With GDI
acceleration on, GDI opens the texture itself and draws on it with the
CPU where RenderKm does not, and a shared Direct3D allocation is not
CPU-visible (Windows 7's dxgmms1 takes a shared CPU-visible allocation
only in an aperture segment, M18 step 7). Made CPU-visible on the x64
build, Windows 11 refused it as well: every shared render target's
`pfnAllocateCb` failed `E_INVALIDARG` and the taskbar went (reverted).
An aperture segment would need the host to reach the guest's aperture
pages, which it cannot.

With **GDI acceleration off** Notepad draws, and `gditest` is 37 of 37
exact: Windows then moves a GDI-compatible texture's pixels itself. The
frame rates do not depend on it any more (the readback and cached-staging
work above helps either way): d3dgame11 windowed, unsynced, DISCARD 1200
frames in 4.8 s (~250 frames/s; ~210-240 with it on) and the flip model
1200 in 2.5 s (~470; 515), with vsync 58.2 and 58.8 (58.3, 58.8). So the
kernel driver's default is **off** (`GdiAcceleration` absent or 0; 1 turns
it on, a restart applies it), as "Open" asked before the track merges;
the host's GDI operations and `d3dpt-gdi` stay.

### Depth textures on the Direct3D 9 driver (2026-10-19)

The user: a Direct3D 9 game on Windows 11 said "hardware shadows are not
supported". The WDDM Direct3D 9 driver claimed D16 / D24X8 / D24S8 as
depth buffers only, so `CheckDeviceFormat(D3DUSAGE_DEPTHSTENCIL,
D3DRTYPE_TEXTURE)` failed. It now claims them as textures too (the
core's format list, `core_caps.c`, so the XP and 9x drivers as well:
their surface with `DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER` reaches the host
with both caps), and the executor makes a resource that is both texture
and depth buffer a `D3DUSAGE_DEPTHSTENCIL` texture, level 0 the depth
buffer, sampled with DXVK's depth compare (NVIDIA's shadow maps), never
uploaded from VRAM; a host that refuses one gets the plain depth
surface. No new wire: the host reads the caps it already had. The user
ran the game's hardware shadows on Windows 11 and on XP (2026-10-20).
Since 2026-10-24 they carry `D3DFORMAT_OP_NOTEXCOORDWRAPNORMIP` as
NVIDIA's do (no wrap, no mip chain): Win98's runtime refuses a depth
texture without `D3DUSAGE_DEPTHSTENCIL`, as on the rig, and Wine's d3d9
device test, told `QUERY_WRAPANDMIP` passed, made one with usage 0 and
crashed (`win98-winetest`; doc 15 "Depth textures"). Open: the vendors'
raw-depth FOURCCs (INTZ, DF24, DF16; not Direct3D 9's own formats), a
probe of our own.

### Unigine Heaven in Direct3D 9 (2026-10-19)

On an overlay of the user's machine, High, 1920x1200 full screen. With
the depth textures above it loaded and crashed before its first frame
(an access violation in `Unigine_x86.dll`); its log said every texture
after a point failed "out of video memory", and none of those calls
reached the user-mode driver. `D3D9MEM.EXE` showed why: Direct3D 9
counts its textures against the dedicated memory plus the shared system
memory the adapter reports, and refuses past that itself; at a 64 MiB
aperture that was 512 MiB (127 default-pool textures of 4 MiB). The x64
kernel driver's aperture is 1 GiB now: the budget is 1472 MiB, 367 such
textures are made (three times the VRAM segment; the memory manager
evicts them to system memory), and Heaven runs with its shadows, at 1 to
3 frames a second (the executor busy 28% of the time; open). Also open:
each texture still takes its size of the process's address space
(CPU-visible allocations), so a 32-bit game has ~1.9 GiB for textures
and its own memory together; and the XP and 9x drivers' memory has no
paging at all.

### Explorer's text and icons (2026-10-19)

The missing text, icons and clipped shadows the user saw in Explorer by
hand (M20's) were gone on the user's own look at the desktop on the 10_0
device, most likely Direct3D 9 on WDDM 1.0 /
1.1, which DWM composed on before.

### Feature level 10_1, and Direct3D 10 programs (2026-10-20)

(2026-10-20, user: "Do d3d10 and feature level 10_1"). `GetCaps`' 3D
pipeline level claims 10_1 beside 10_0: shader model 4.1, cube arrays
and a blend state per target, which the driver already passed through
(the host's DXVK device is 11_0; no wire change). On Windows 11,
`D3D11CreateDevice` with every level now gives **10_1** on our adapter,
and DWM's device is 10_1 with the desktop composing as before.

Direct3D 10 programs on Windows 8 and later need nothing else:
`d3d10.dll` and `d3d10_1.dll` run on Direct3D 11's runtime there, and
every device the probe made through them reached the driver through the
11.1 interface (`0xb000f`). (The "Open" note that `d3d10.dll` wanted the
10 / 10.1 DDI versions was wrong for Windows 11.) Windows 7's runtime is
another: it opens a driver's own Direct3D 10.0 and 10.1 interfaces. So
`GetSupportedVersions` lists them (Windows 7's and those of Vista's DXGI
1.1 update), and `d10_dev.c` gives each its device table: the 11
entries, but for the dozen whose arguments are older (a resource without
ByteStride, 10.0's cube view and blend state, a depth view without
flags, stream output with one stride and no streams, render targets with
no UAVs), which convert to 11's and call the 11 entry. 32-bit Windows 7
gets `d3dpt11.dll` now too (the INF's x86 section, the guest disc's
`WDDM\`).

`d3d11-probe` grew the checks: the scene at 10_1 with shader model 4.1
(a Gather) on ours and WARP, and through Direct3D 10's API (`d3d10.dll`,
and `d3d10_1.dll` at 10_1 and 10_0, with 10.0's blend state and
R8G8B8A8, since `d3d10.dll` has no B8G8R8A8 target) against WARP's 10_0
frame. On the PC's Windows 11 all parts **PASS, 0 pixels off**, `present`
too. On 32-bit Windows 7 (the M18 machine, the probe built x86 with the
SDK's `d3dcompiler_47.dll` beside it, on a stick hot-plugged by QMP):
**PASS, 0 pixels off** in every part, through Windows 7's 11.0 interface
(`0xb000b`), 10.0 (`0xa0009`: `d3d10.dll`, and `d3d10_1.dll` at 10_0)
and 10.1 (`0xa000a`), each converter called. Its "every level" line is
`E_INVALIDARG` there: Windows 7's runtime refuses 11_1 in the list asked
for.

### Windows 7's DWM stays on Direct3D 9's driver (2026-10-20)

(through `d3d10level9`): it never opens `d3dpt11.dll`, while `dxdiag`
does (DDI 10.1, its device made through the 10.1 interface). `dxdiag`
rates the 32-bit kernel driver **WDDM 1.0**, and Windows 7's DWM puts a
1.0 driver on 10level9, as Windows 11's did at 1.1 (step 4). The
likeliest reason for 1.0 is the missing GDI acceleration, which WDDM 1.1
requires (the x64 driver's RenderKm, off by default and not in the
32-bit build); not checked. `win7-aero-test.sh`'s `dwm` verdict accepts
either DLL and says which.

### The packages, and the driver for Windows 11 on Arm (2026-10-19)

Beside its d3d9 (2026-10-19): the Windows zip and MSIX as
`dxvk_d3d11.dll` and `dxvk_dxgi.dll`, the Linux tarball and the Flatpak
(which is the tarball's tree) as `lib/2ksbox/libdxvk_d3d11.so.0` and
`libdxvk_dxgi.so.0`, the Mac app as `lib/2ksbox/libdxvk_d3d11.0.dylib`
and `libdxvk_dxgi.0.dylib` (App Store and community builds; not the
Intel one, which has no DXVK). The packaged player names them to the
executor as it names the d3d9 (`player-core/src/companions.rs`:
`D3DPT_DXVK_D3D11_LIB`, `D3DPT_DXVK_DXGI_LIB`, printed by `--companions`
as `dxvk-d3d11` and `dxvk-dxgi`), and each packager checks that the
staged player found the staged copies, as it does for the d3d9. Where
the environment names only the d3d9 (a harness), the executor looks
beside it, or in DXVK's build tree's `src/d3d11/` and `src/dxgi/` when
the d3d9 is in its `src/d3d9/`. `build.sh` configures a Linux or Mac
`build/dxvk` from before d3d11 was enabled afresh. The executor
(`d3dpt_exec_d11.cpp` with the rest) compiles for Linux with g++ against
DXVK's native headers, warning-free and with no unresolved symbol
(checked in WSL's Ubuntu on the PC), and its Wine build with mingw. **On
a Linux host Windows 11 on the driver works as on the Windows one** (the
user, by hand, 2026-10-20: under KVM, `d3dgame8`, `d3dgame9` and
`d3dgame11` run as on the Windows host). On macOS DXVK's d3d11 and dxgi
build since 2026-10-20 (DXVK patch 18: ld64 export lists, a `tzcnt`
overload clang called ambiguous), and so does the executor (`build.sh
dxvk exec`); the same day they ran there, Windows 11 on Arm's desktop on
DXVK's d3d11 on KosmicKrisp (Open, "Windows 11 on Arm").

### The driver builds for Windows 11 on Arm (2026-10-19)

Into `build/wddm/arm64` (`build-wddm.cmd`, the `wddm` stage), and the
guest-tools ISO carries it as `WDDMARM\` with the same INF (now with an
`NTarm64` decoration) and installer:

- `d3dptkmd.sys` ARM64, from the EWDK as the others. The only x86-only
  line in it was the debug console's port write before the register BAR
  is mapped; on Arm those lines are dropped.
- `d3dptumd.dll` and `d3dpt11.dll` **ARM64X**: one DLL with an ARM64 half
  for Arm processes (DWM) and an ARM64EC half for the x64 programs Windows
  emulates, under the one `UserModeDriverName`. VS 2019, the EWDK's, has
  no ARM64EC, so they are VS 2022's (17.8 or later, its ARM64EC build
  tools, `BuildAsX`) on its own Windows SDK, with the EWDK's display
  headers copied beside them (`build/wddm/arm64/wdkinc`: the SDK has no
  `d3d10umddi.h`, and the EWDK's include path predates ARM64EC). Without
  VS 2022 the Arm set is skipped with a note.
- `d3dptumd32.dll` and `d3dpt11_32.dll`, the x86 builds, for the x86
  programs Windows on Arm emulates under WoW64 (`UserModeDriverNameWow`,
  SysWOW64), as on x64.
- All three Arm builds compile with `/volatile:ms`: MSVC's Arm default
  orders no volatile access, and the drivers start the device with a
  register write after their writes to memory.

`windows-drivers.sh` publishes and fetches the Arm set with the rest.

### The launcher offers it, experimental (2026-10-19)

(2026-10-19, user: "a warning-gated experimental driver option that
needs test mode", as the drivers disc's experimental viogpudo is). The
aarch64 QEMU builds `d3dpt-vga` and its executor loader too
(`d3dpt/hw/meson.build`, `arm_system_ss`; the device has nothing x86 in
it, and the virt board's PCI selects VGA_PCI, so the VGA core). A
Windows 11 machine on Arm with `video = "d3dpt"` gets `ramfb` (the
firmware's screen) then
`d3dpt-vga,irq=on,vgamem_mb=512,bar64=on,romfile=` in place of
virtio-gpu, on `virt,gic-version=3,highmem-mmio-size=16G` (`arm_args`,
checked with `launcherx --print-args` on a scratch bundle).

### The frame buffer above 4 GiB (2026-10-20)

(2026-10-20, the Mac's first boot on it). As a 32-bit BAR, 512 MiB fits
nowhere in the `virt` board's 32-bit PCI window (0x10000000-0x3eff0000)
aligned to its size: EDK2 failed the bus's resources, lost the AHCI disk
and dropped to its shell ("map: no mapping found", Boot0004 "Not Found";
256 MiB fits, and was the stopgap for a few hours). So on Arm the
device's `bar64=on` makes VRAM a 64-bit prefetchable BAR 0 (slots 0 and
1) and the registers BAR 2, and the kernel driver's `find_bars` reads
either layout and takes an address above 4 GiB (x64 and the 32-bit
families keep the old layout). Two more things stood in the way on the
Air: an M1 gives an HVF guest a 36-bit IPA, and QEMU never put the high
regions below 256 GiB, nor its PCIe window below 512 GiB, so the machine
had no high window at all; QEMU patch 97 lets them follow the memory
there and the window shrink, and the machine asks for 16 GiB
(0x4_0000_0000 to 0x7_ffff_ffff with 4 GiB of RAM). And EDK2 moves a
64-bit BAR below 4 GiB on a device with an option ROM
(`PcdPciDegradeResourceForOptionRom`), so the Arm device has none
(`romfile=`; the x86 VGA BIOS is no use there). Checked: EDK2 maps the
bus with the BAR on it (AHCI at 0x10006000) and the user's machine boots
Windows on the layout; Windows puts BAR 0 at 0x7_e000_0000 (Open,
"Windows 11 on Arm"). In the form our adapter on Windows 11, either
architecture, is "2ksbox adapter (experimental)"; picking it asks first
in a warning alert (`Form::video_question`: early build, test mode,
Secure Boot off, the watermark, anti-cheat; Cancel the default), and
while it is the pick a warning under the picker says test mode
(`video_warning`); the note names `WDDMARM\` or `WDDM64\`. The aarch64
QEMU is built only on an Arm host; on the Mac (2026-10-20) it builds
with the device and the desktop runs on it, the executor on DXVK on
KosmicKrisp (Open, "Windows 11 on Arm").

### Windows 11 on Arm: the driver starts (2026-10-20)

The driver builds, the aarch64 QEMU has the device and the launcher
offers it (the entries above); on the Mac the machine boots Windows with the
512 MiB frame buffer in a 64-bit BAR (2026-10-20, QEMU patch 97; Windows
puts BAR 0 at 0x7_e000_0000 and the registers' BAR 2 at 0x3efe_b000).
The Code 48 first seen there (`0xC000036B`,
`STATUS_DRIVER_BLOCKED_CRITICAL`) was the x64 driver installed on Arm by
mistake. The Arm build loads and **fails its start**: DxgKrnl-Admin
event 549, `StartAdapter_DdiStartDeviceFailed`,
`STATUS_DEVICE_CONFIGURATION_ERROR`, with no line from after the
registers are mapped, so `find_bars` matched no BAR. It now decodes
`CmResourceTypeMemoryLarge` too (`RtlCmDecodeMemIoResource`; a large BAR
may come that way), and a failed match leaves `BarDiag` in the service
key (REG_BINARY, 64-bit words: BAR 0 | BAR 1 << 32, BAR 2, then each
memory resource's type << 16 | flags, start, length). `InitStatus`
there: 0xffffffff when `DriverEntry` runs, then `DxgkInitialize`'s
status. With that build `InitStatus` was 0 and no `BarDiag` came, yet
the start still failed with the same status and no debug line: so
`BarDiag` is now written every time, and `StartStage` (1 entered, 2
after `DxgkCbGetDeviceInformation`, 3 after `find_bars`, 4 after the
register BAR's length check, 5 mapped) says where the start stops. It
stopped at 5 with the resources right (BAR 0 a plain memory resource,
512 MiB at 0x7_e000_0000; BAR 2, 4 KiB at 0x3efe_b000) and still no
debug line: the register set read back as none of ours, as if memory
decoding were off during the start (the command register read 0x0004
every time it was looked at from the host). `StartRegs` (32-bit words:
the command register, MAGIC and VERSION as read, then the same after the
driver turns memory decoding on if it was off) checks it, and the
turning on is the fix: `StartRegs` read 0x0004 (bus master only) with
MAGIC and VERSION 0xffffffff, then 0x0006, 0x42463344 and 11. Windows on
Arm hands the driver its device with memory decoding off (the x64
machines never did), so the driver turns it on itself when it is off,
and keeps `StartRegs` / `StartStage` / `BarDiag` / `InitStatus` as the
record of a start there. **With it the driver starts on Arm**
(2026-10-20): `StartDevice` reads register set v11, the Direct3D window
at 448 MiB, the interrupt for the vertical blank and fences, Windows
commits 1024x768 on our adapter, and TextInputHost, Explorer and DWM
open Direct3D 11 devices on DXVK's d3d11 on KosmicKrisp and render. Then
DWM's first present moved the scanout to VRAM offset 0x565000 and the
player aborted in HVF (`do_hv_vm_protect`: a 4 KiB-aligned range where
Apple Silicon's host page is 16 KiB); QEMU patch 98 rounds HVF's
dirty-log ranges to whole host pages. With it the desktop runs on our
adapter, Mica's translucency included, `d3dgame11` and `d3dgame8` run at
60 frames/s, and every size up to 3840x2160 works (the 512 MiB segment
above 4 GiB; the user on the Air, 2026-10-20). The Mac app carries the
driver in its guest-tools disc (`WDDMARM\`). Left there: games. The
drivers' debug lines are one DEBUG register write a character, a VM exit
each under HVF, and the desktop's steady stream of them (allocations,
VidPn negotiation, adapter queries, the user-mode drivers' 4000 lines a
process) slowed it (user): the kernel driver's per-event lines now log
their first 8 to 32 (`log_few`), and the user-mode drivers 400 lines a
process (`D3D11Trace` still lifts it). Still hundreds of lines in
seconds (user, 2026-10-23: each entry's first two calls, each resource
and present, in a dozen processes), so the routine lines of both
user-mode drivers now need `D3D11Trace` / `D3D9Trace`; without them only
failures and refusals are logged. The kernel driver's per-event lines
went the same way (user, the same day): `log_few` and every routine line
(devices, contexts, allocations, submissions, presents, VidPn
negotiation, interrupt control, the 5 s packet timings and RenderKm
census, whose clock reads and bookkeeping stop too) need the service
key's `Trace` = 1 (`HKLM\SYSTEM\CurrentControlSet\Services\d3dptkmd`,
REG_DWORD, read at DriverEntry, so a restart or a disable / enable of
the adapter); the device's start and stop, failures, refusals, TDR dumps
and the user-mode drivers' relayed lines log without it. Unigine Heaven
runs there in Direct3D 9; in Direct3D 11 it stops on its loading screen
(user, 2026-10-20), every GetData of one of its queries answered
`E_INVALIDARG` by the host (token 57). DXVK's immediate context refuses
a size other than the query type's own, and the query exists (no
creation failure was logged); the executor now names the query's type
and both sizes when GetData fails. It was `PIPELINE_STATISTICS` asked
for 64 bytes, Direct3D 10's eight counters, where DXVK takes only
Direct3D 11's 88 (the same eight, then hull, domain and compute): a
runtime at feature level 10_x reads the 10 layout. The executor now asks
the host at the query's own size and gives the guest the bytes it named.
Past that, Heaven loads for a long while (x86 under emulation on Arm)
and then the player crashed in KosmicKrisp itself (LunarG SDK
1.4.357.1): a null dereference in Mesa's `nir_opt_varyings` /
`nir_opt_dce`, from `vkCreateGraphicsPipelines` as DXVK built a draw's
pipeline on its CS thread (macOS crash report, 2026-10-20 13:16). A
driver bug, open: the shaders want capturing (DXVK_SHADER_DUMP_PATH) for
a newer KosmicKrisp or a Mesa report. The Arm kernel driver is built as
our viogpudo is (`build-windows.sh`, `scripts/nuget-wdk.sh`;
docs/build-windows.md "The WDDM driver"), and `wddm-install.ps1` stamps
`DriverVer` with the install's date and minute so a new build always
replaces the installed one.

### The screen Windows turns off (2026-10-24, register set v13)

Seen on Linux under KVM, the lock screen idle a minute: the screen
showed garbage. The kernel driver's `SetVidPnSourceVisibility(FALSE)` wrote `ENABLE` = 0,
which the device takes for a return to VGA, so after the grace it showed
the VGA core, whose VBE registers still held the firmware's mode
(1280x800), over VRAM laid out for the driver's (2048 wide, pitch 8192):
stripes until a mouse move. Now a device at v13 gets `BLANK` = 1
instead, `ENABLE` untouched; the device holds the last frame for the
grace, then shows black at the mode's size, and `BLANK` = 0 puts the
desktop back (an older device still gets `ENABLE` = 0). Bugcheck output
(`SystemDisplayEnable`) and `ResetDevice` clear it. Checked on the PC
under WHPX at 3840x2160 (monitor timeout 1 minute): "source off
(black)", a black screendump, a click and the desktop back.

### Heaven on the Air, again (2026-11-26)

On a fresh overlay of the user's Windows 11 on Arm machine (`build/m25h/`
on the Air, outside the repo: `run.sh` boots it with QMP and a FAT
`REPORT` disk on USB, `drive.py` logs in over QMP and runs
`heaven.ps1`, which starts Heaven windowed at 1280x720 on the API
asked), Heaven on Direct3D 11 reaches its loading screen and then the
whole guest freezes, every run at the same place: the 99th optimized
pipeline DXVK builds (a VS `a5b34593...` and a PS `096de8fd...` that
writes two targets, into one colour target, depth clamp on). DXVK's CS
thread is in `vkCreateGraphicsPipelines`: KosmicKrisp's
`kk_compile_shaders` -> `nir_opt_varyings_bulk` -> `nir_opts` ->
`nir_opt_dce` -> `dce_cf_list` faults (`EXC_BAD_ACCESS`, code 2) on a
bitset read through a garbage pointer (`x21` = 0x00000b0300000000) and
spins at 100% of a core, so the vCPU in the executor's batch waits on
DXVK forever and QEMU's main loop on the executor's lock (October's
crash at the same place). The DXBC, SPIR-V and pipeline state of all
99 pipelines are kept on the Air (`build/m25-kk-crash/`, with two
native harnesses, `kkpipe` and `kkseq`), but built natively on the
same KosmicKrisp (Mesa `git-92ec601a34`, SDK 1.4.357.1), one or all 99
in order, with DXVK's robustness features, every pipeline compiles. A
GFXReconstruct capture is no use here (its page guard needs SIGSEGV,
which QEMU blocks; KosmicKrisp has no buffer device address capture
replay). Mesa main (`fb34d01`, debug, built in `build/mesa-kk`) failed
before the question: DXVK could not create an instance on it, so the
executor stayed off. Unrelated but close: mesa!45358 (Mesa 26.2.6)
fixed a use-list corruption in `nir_opt_varyings`' dead-varying removal
for tessellation inputs.

**Red and blue swapped.** Heaven's swap chain is R8G8B8A8 and its
window is blt-model: the kernel driver's `gdi_present` hands the host
only A8R8G8B8 / X8R8G8B8 sources, so the frame goes through `run_blt`,
which copies the bytes into Windows' BGRA staging surface as they are
(a shot of the loading screen: the dragon blue, the sky orange).
Direct3D 9 is right. The fix planned: the host's GDI sides also take
A8B8G8R8 / X8B8G8R8 (D3DDDIFMT 32 / 33) with an R8G8B8A8 object,
`gdi_read` reads such an object through an RGBA staging texture and
swaps the channels so every operation sees BGRA, and a copy that would
need a GPU conversion (object to object, into an RGBA side) is refused;
the kernel driver's STAGING branch of `gdi_present` sends those formats
too (protocol v32: the proto version is checked only between QEMU's
device and the executor, never with the guest driver). The kernel half
builds only on the PC.

Also found on the way: Xcode's macOS 27 SDK broke the Mac build twice
(QEMU's `apple-gfx`, DXVK's `util_math.h`; both fixed on this branch),
and DWM's submissions on the Air are sometimes refused with
`0xc0262111` (`STATUS_GRAPHICS_CANT_RENDER_LOCKED_ALLOCATION`), not
followed up.


### Heaven in Direct3D 11 on the PC (2026-11-27)

The user: before the Mac, make it run fast here to prove it. Unigine
Heaven 4.0 (32-bit, WoW64's `d3dpt11_32.dll`), windowed 1280x720, Low,
no tessellation, on the test machine's overlay at 1920x1080
(`HRUN\heaven-api.ps1 -Api direct3d11`; the loop is `hcycle.sh <tag>
[build]` on the test machine: build, stage, install, 90 s of Heaven, a
screendump, the device's rates and the executor's present count with
`D3DPT_D11_PROF=1`).

**A crash every other run: Map's pointer was 8-byte aligned.** Heaven
died ~12 s in, after loading the world, with an access violation in
`Unigine_x86.dll` at `movaps [edi-10h], xmm3`. cdb attached in the guest
(the EWDK's x86 debugger on the command CD, `sxd -c2` for the second
chance; a debugger that launches the process gets the debug heap and
moves the allocations) showed the page committed read-write: `movaps`
faults on an address not 16-byte aligned, and the address was a dynamic
buffer's CPU copy, a `HeapAlloc` block, which a 32-bit process's heap
aligns to 8. A GPU mapping is page-aligned and Heaven fills its buffers
with aligned SSE stores; whether the block happened to be 16-aligned
decided the run. The copies are 64-byte aligned now (`d11_res.c`).

**Where a frame goes.** With the crash gone Heaven ran at ~46-50
frames/s, colours swapped (below). The executor's profile (bytes per
token now in `D3DPT_D11_PROF`'s lines) gives the frame: ~1,850 draws and
~3,000 constant-buffer updates of ~1.5 KB, 4.4 MB of records, the host's
executor busy ~4.4 ms (22-28% of the time). In the guest, Heaven's
render thread used ~60% of a vCPU and the System process ~85%: the
kernel driver's submissions (dxgkrnl's GPU thread) were the limit, not
the host. The kernel driver's packet times (`Trace` = 1) per 5 s: the
present's copy (BLT) 547 ms, paging the window's 1280x720 surface back
and forth (TRANSFER) 222 ms, the records' appends 472 ms, the patch
packet 469 ms, the registrations 132 ms, the doorbell (the host running
the batch) 869 ms. Changes:

- **Submissions of 1 MiB and 512 allocations** (were 256 KiB and 64,
  `D3DPT_WDDM_MAX_LIST`, `D3DPT_DMA_SIZE`, the patch list 16384): a
  frame of draws that each name their own buffers split into ~45
  submissions at 64 allocations; ~15 now. The user-mode driver finds an
  allocation's slot by a hash of its handle (`al_find` / `al_slot`), not
  a walk of up to 512.
- **GetData no longer waits.** Heaven reads two pipeline-statistics
  queries ~3 times a frame; each read flushed and locked the result page,
  so it waited for everything queued ahead, the game and the GPU taking
  turns. Each query has its own result page now; the first GetData after
  an End sends the fetch, later ones look with a lock that does not wait,
  and the answer is kept until the next End.
- **The patch packet carries only the offset patches**: the handle
  patches are written in Render and were walked again at submit time.
- **An RGBA present is the host's (protocol v32).** Heaven's swap chain
  is R8G8B8A8, and the kernel driver handed the host only BGRA sources:
  the frame was read back into VRAM and copied into the window's STAGING
  surface by the guest's CPU through the BAR, ~2 ms of each frame plus
  the readback, with red and blue swapped ("Heaven on the Air, again").
  The host's GDI copy now takes an A8B8G8R8 / X8B8G8R8 source (read
  through an RGBA staging texture, swapped on the CPU) into VRAM; into
  an object it refuses (that would be a conversion on its GPU), and the
  kernel driver sends RGBA only on its STAGING branch. The colours are
  right (red roofs, a brown dragon), and the present's guest side went
  from 547 ms per 5 s to nothing.
- Smaller: a token's payload is not zeroed before the update is copied
  over it (`tok_z`), and a dynamic buffer's Unmap finds the changed
  range a word at a time.

**Result.** Windowed, ~54 frames/s (from ~46): what is left is the
present's readback, which makes the host wait for its GPU to finish the
frame (~4 ms, inside the guest's submission: a blt-model window
presents into a CPU-visible surface), and the appends. **Full screen
(`-video_fullscreen 1`, 1280x720) Heaven runs at ~128-137 frames/s**:
the executor counts ~640 scanouts per 5 s, and Heaven's own screenshot
(F12, `C:\Users\test\Heaven\screenshots`) shows "FPS: 137" over a frame
with the right colours. A full-screen flip's readback is deferred
(protocol v28), so the host's GPU and the guest overlap. The QMP
screendump of a full-screen game shows a stale frame (the scanout ring
keeps the frames on the host's GPU); Heaven's F12 is the check.

Tools found on the way: a sampler of the vCPUs' `RIP` through QMP's
`info registers -a` is no profiler under WHPX (the registers are the
last exit's: every sample landed in our device's register writes), and
g11run's freeze watchdog fired three times on runs piped into `head`
(SIGPIPE reads as a non-zero exit, not a frozen guest).

**Then Heaven's own thread.** Full screen, with the appends' copy out
of the BQL (`d3d_dma_append` drops it as the doorbell does), Heaven's
render thread was at 87% of a vCPU and the System process at 65%: the
game's side was the limit. `D3D11Prof` (a profile of `d3dpt11.dll` in
TSC cycles, "Logs and switches") put 45% of that thread in RenderCb,
~1.2 ms a submission, and the kernel driver's own Render ("Render in 5
s", traced) at ~0.7 ms of it: per 5 s the copy 56 ms, the records'
check 2 ms, but the registrations 272 ms and the patches 1,006 ms, each
store into the DMA buffer 250-500 ns. The DMA buffer is write-combined:
a bulk copy runs at ~15 GB/s, scattered stores do not. Render now
builds the submission in the device's own (cached) scratch memory and
copies it into the DMA buffer in one pass: Render 1.33 s -> 0.17 s per
5 s (patches 44 ms, registrations 2 ms), RenderCb 2.1 s -> 1.4 s, and
full screen ~140-150 frames/s in the scenes measured. What is left in
RenderCb is dxgkrnl's (~600 µs a submission: its allocation and patch
lists; ~290 allocations and ~2,100 patches each), then GetData's two
flushes per fetch (~0.8 s per 5 s, half the submissions) and
PresentCb. `d3d11-probe` passes and `d3dgame11 -compare 60` matches WARP
(4 of 307,200 pixels off by more than 8) on the result.

**Then the submissions themselves** (the System process back at ~93%
once Heaven's thread waited on its presents):

- **GetData's fetch rides in the open buffer** (`cmd_reserve` keeps the
  RETURN and its GET together) and is sent by the frame's own flush, or
  by the caller's next poll: GetData 0.8 s -> 0.04 s per 5 s, RenderCb
  1.4 s -> 0.4 s (half the submissions had been fetches).
- **The registration table lives in the DMA buffer's private data**
  (cached kernel memory; `reg_priv`): Patch updates it there and the
  submission reads it there, where every entry read from the
  write-combined DMA buffer was uncached. REG 324 ms -> 5 ms per 5 s. A
  buffer whose private data carries no table (presents, paging) keeps
  the copy in the DMA buffer.
- **Register set v14's `DMA_APPEND1`** (protocol v33: the address, size
  and answer in the window's header): one exit an append where there
  were six, served by the device in a 4-byte region of its own without
  the BQL, copying only from guest RAM (`d3dpt_dma1_write`). Inside the
  device an append now takes ~20 us; seen from the guest its exit still
  takes ~350 us against ~14 us for a register read or a refused append,
  most likely interrupt work delivered as the vCPU resumes
  (`WHPX_DIRTY_TRACK=0` brought it to ~160 us: the dirty log's clearing
  flushes every vCPU's TLB). Not followed further.
- **QEMU patch 99 leaves the command window untracked**
  (`MemoryRegion`'s `dirty_track_skip_offset` / `_size`, set by
  d3dpt-vga): no display scans it, and the host writes megabytes a frame
  into it.

Heaven full screen now ~155-175 frames/s (770-880 scanouts per 5 s at
the same point of the flythrough; ~130 before these, ~46 windowed at the
start of the day). `d3d11-probe` and `d3dgame11 -compare 60` pass. What
is left on the System side per 5 s: the doorbell (the host running the
batches) ~1.7 s, the appends' exits ~0.9 s, paging ~0.05 s, and
dxgkrnl's own work; on Heaven's side, PresentCb (waiting for the queue)
~1.6 s, RenderCb ~0.3 s.

**Asynchronous batches (register set v15).** A submission ran inside
its doorbell: the guest's GPU scheduler waited for the host on every
one. `ASYNC_KICK` hands the window's batch and the submission's fence to
a thread of the device's; the thread runs the batches in order under
the executor's lock, makes `FENCE_DONE` the fence and raises `IRQ_DMA`
(a bottom half; the status is latched while the driver has the
interrupt off, else the first fences at boot were lost and the desktop
never came: found on the first boot). The kernel driver kicks only a
submission of its records alone and drains (`ASYNC_DRAIN`) before every
other one and at a preemption; QueryCurrentFence reads `FENCE_DONE`. In
async mode an append only records where its records are, and the
thread copies them out of guest memory (the DMA buffer is the
submission's until its fence is done). Windowed Heaven ~54 -> ~70
frames/s (its present's readback now overlaps the guest); full screen
unchanged at ~145-165, each flip draining. Seen from the guest an
append's exit still takes ~280 us with nothing in the handler: it is
the interrupts and DPCs (the fences') a vCPU takes as it resumes, real
work landing there. Five minutes of Heaven full screen: no TDR;
`d3d11-probe` and `d3dgame11 -compare 60` (DISCARD and `-flip`) pass.
Next for full screen: flips through the queue instead of a drain.

**Immediate flips through the queue too** (`ASYNC_FLIP`, register set
v15): the kernel driver no longer drains for a flip that does not wait
for the vertical blank; the device sets `OFFSET` after the flip's batch
(in the bottom half, under the BQL) and then its fence. Full screen the
flips cost nothing on the System side (873 in 0.15 ms per 5 s); the
frame rate stays ~150, and the System process is down to ~45% of a vCPU
(from ~93% this morning) while Heaven's own render thread is at ~81%:
the game, Direct3D 11's runtime (~1.5 million DDI calls a second) and
the user-mode driver are the limit now. Five more minutes full screen
without a TDR, the desktop back afterwards; the WARP comparisons pass.

**A CPU profile of the guest** (`HRUN\prof.ps1` on the test machine:
Windows' own WPR samples for a few seconds, `tracerpt` turns the trace
into CSV, and the script, in the guest, puts each sample in its module
from the trace's image loads; offsets in `d3dpt11_32.dll` resolve with
cdb against `build/wddm/x86/d3dpt11.pdb`). Heaven full screen, its
threads' samples: `Unigine_x86.dll` 68%, `d3dpt11_32.dll` 12%,
ntoskrnl ~5%, `d3d11.dll` ~4.5%, `d3dptkmd.sys` ~4%, `dxgmms1.sys` ~3%.
The game is CPU-bound in its own engine; our whole stack is about a
quarter of its thread. In the user-mode driver a third of the samples
were Unmap's diff of dynamic buffers (~25 maps of 128 KB a frame, word
by word): now 64 bytes a step with SSE2 (MSVC's inlined `memcmp` was
byte-wise and slower; Unmap 165-209 ms -> 82 ms per 5 s). Another 5% was
the view list walked at every flip-model rotation: only views of VRAM
resources (the ones a rotation can name) are listed now. What is left
there: records, uploads and patches (`rec_close`, `upload`,
`patch_res`). No kicked batch waited for one of the device's three
slots (its rate line counts them), yet Heaven spends about a third of
its thread in PresentCb: what DXGI waits for there is the next question.

**Where Heaven's thread blocks** (`HRUN\waits.ps1`, full screen, 3 s):
of ~6,000 switches out of its main thread, ~3,500 are the engine's own
waits (one call site in `Unigine_x86.dll` into KernelBase, its job
system), ~260 are DXGI's throttling inside our `dxgi_present`'s
PresentCb, ~90 RenderCb in `cmd_flush`. With the host's RTX 3090 at
~10% and none of the device's batch slots ever full, Heaven at ~150-165
frames/s full screen is limited by its own engine on this PC, our stack
being about a quarter of its main thread.

**Bugcheck 0x119 after a driver update (fixed).** Installing a new
driver build with async on bugchecked the guest now and then (five
minidumps over the night, all `0x119` subtype 1, "the driver has
reported an invalid fence ID", in `d3dpt_interrupt`); once Windows went
into automatic repair, and a reset brought it back. An update stops and
starts the adapter without resetting the device: the device kept the
last instance's `FENCE_DONE` and, with the bottom half's latch, a
pending `IRQ_DMA`, and the new instance's first interrupt reported a
fence ahead of everything it had submitted. StartDevice now drains the
device, turns its interrupts off and writes `FENCE` = 0 before enabling
them, starts its own count at 0, and the ISR never reports a fence of 0
or one past the last it submitted. Checked: a driver update and three
times disabling and enabling the adapter, no new minidump.

**A window's present cannot be kicked.** Kicked with its batch (the
host's copy into the STAGING surface done on the device's thread before
the fence), Heaven's window came out dimmed and slower (~54 frames/s
against ~70): Windows moves the STAGING surface on into the window
before that present's fence. It stays a synchronous submission.

**Heaven's own benchmark** (F9 through QMP, the results read from
Heaven's F12 screenshot; 4 vCPUs under WHPX on the Ryzen 9 5900X, the
host's RTX 3090; Direct3D 11, full screen, tessellation off, as 10_1
has none):

| Mode | Quality | Average | Score | Min / max |
|---|---|---|---|---|
| 1280x720 | Low | 200.6 frames/s | 5052 | 51.7 / 527.0 |
| 1920x1080 | High | 184.2 frames/s | 4639 | 10.8 / 435.7 |

Barely apart: the game's thread is the limit, not the path to the GPU.

**The other WDDM guests on the changed driver and device** (2026-11-27):
`scripts/test.sh` host stage 46 passed, 0 failed (8 platform skips); the
XP guest checks `guest-ddvm`, `guest-G9` and `guest-G8` (native and rig)
pass on the changed device, and the whole guest stage after the evening's last changes (23 passed, 0 failed, 8 skipped); `win7-aero` passes on the 32-bit kernel
driver with today's changes (a fresh unattended install from the disc,
SETUP, DWM on our driver, D3DGAME9 within budget).

**Heaven in Direct3D 9, why it is slow** (`D3DPT_DP2_PROF`, windowed,
1280x720 Low): the executor is busy ~72% of the time, ~3 s of every 5 in
`D3DPT_DP2_DRAW8`, ~30,000 draws a second at ~20 us each. Every DRAW8
becomes a `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` on the host: the
draw's whole vertex range is read out of guest VRAM and uploaded again
through DXVK's UP path, and its indices are rebased one by one, draw
after draw, for meshes that never change. The fix is host vertex and
index buffers kept for VRAM ranges and dropped when the guest writes
them (a DRAW8 with `VRAM_VB` / `VRAM_IB` would draw from them with a
base vertex); its invalidation is the hard part (the Direct3D 9 drivers
of XP, 9x and WDDM all reach this decoder). Readbacks (`READBACK`
records) are another ~0.5 s per 5. Not started.

**A host copy of VRAM buffers (`D3DPT_DDI_VBCACHE=1`, opt-in, 2026-11-27).**
A VRAM vertex or index buffer drawn from twice with no write in between
gets a host buffer (D3DPOOL_DEFAULT, filled from VRAM); a DRAW8 whose
streams and indices are all such buffers draws from them with
`DrawIndexedPrimitive`: every stream at the vertex `min_index` names
(an instance stream at its first element), the shader's own
multi-stream declaration, the stream frequencies of an instanced draw
set and reset. A buffer's `dirty` is the write signal (every
`VRAM_DIRTY` / `VRAM_DIRTY_RANGE` the drivers send at Unlock and
BUFFERBLT sets it, nothing else clears it for a buffer): a written
buffer's copy is dropped and it goes back to the UP path until it is
drawn clean twice again. Heaven in Direct3D 9 (windowed, 1280x720
Low): ~98% of draws from host copies, the executor ~58,000 draws a
second at ~7 us (was ~30,000 at ~20), 55% busy (was 72%), 38 frames/s
(~19 without), the picture right. With it on, the XP checks `guest-ddvm`,
`guest-G9`, `guest-G8` and `guest-F9` pass, D3DGAME9 / 8 and D3DFEAT9
pixel-identical to their native frames, and the cache does serve them
(their QEMU logs: "the first draw from host copies", draws with VRAM
vertex and index buffers). Off by default until Win98 games and more
Direct3D 9 titles have run with it; then a candidate for the default.

The write signals alone were not enough: with the cache on,
`win98-winetest` came out 3 keys worse than its baseline (Wine's d3d9
`visual.c:1138` and `:713`, d3d8 `visual.c:666`, all an application's
`DrawIndexedPrimitiveUP`, which Windows 98's runtime puts through a
VRAM buffer of its own and rewrites with no dirty record the host
sees). Each copy now keeps a CPU shadow, and every cached draw compares
the bytes it reads (each stream's range, its indices) with VRAM,
writing what differs into the copy (`cache_fresh`); a buffer that moved
or changed size in VRAM is dropped as written. `win98-winetest` and
`win98-dx9` then pass with it on (0 keys worse; d3d9 visual 1,298
failures, as without the cache), and Heaven in Direct3D 9 needed no
refresh at all: ~45,000 draws a second, the executor 62% busy, ~34-38
frames/s. On that build, with it on, the XP checks (`guest-ddvm`, `G9`,
`G8`, `F9`) and the host's `d3dpt-dp2` / `d3dpt-dp2-system` pass too.
It stays opt-in until a Win98 game and a few more Direct3D 9 titles have
run with it; the evidence so far argues for making it the default then.

Heaven's own benchmark in Direct3D 9 (1280x720 full screen, Low,
tessellation off, the same build): **44.0 FPS, score 1109** with
`D3DPT_DDI_VBCACHE=1` (min 10.3, max 99.0), **27.3 FPS, score 687**
without (min 13.5, max 58.6): +61%. A trap on the way: the AutoPlay
toast for the run script's CD (E:) takes focus and minimises a
full-screen Direct3D 9 window, so the benchmark sits at the desktop with
the executor idle; the test guest now has AutoPlay off
(`HKCU\...\Explorer\AutoplayHandlers` `DisableAutoplay=1`).

On a Windows 98 game, 3DMark2001 SE's whole Benchmark on `base98-br`
(TCG, `tools/w98-3dmark2001.sh` with `TDM_EXE=3DMARK~1.EXE`): **4749**
3D marks with the cache, **4708** without, mean 45.9 against 45.2
frames/s over 80 periods: noise, as it should be on a guest whose CPU,
not the executor, is the limit. Every test ran to the score dialog with
the picture right; 67% of eligible draws came from host copies, and the
VRAM check refreshed 46 ranges the write signals had missed. So the
cache costs a TCG guest nothing and pays where the guest is fast enough
(WHPX, HVF) to make the executor the bottleneck.

**On by default from here (2026-11-27)**, as M25's experimental branch
allows: `D3DPT_DDI_VBCACHE=0` turns it off, the first A/B for a
Direct3D 9 picture that differs. Its evidence is the runs above, all
with it on: the XP checks, `win98-dx9`, `win98-winetest` (0 keys
worse), `d3dpt-dp2` / `-system` / `-gdi`, Heaven, 3DMark2001. Before
merging to main, `test.sh all` runs with the default as it stands.

**The interleave only on a miss (2026-11-27).** A draw through a vertex
shader that reads more than one stream was interleaved into one CPU
vertex (`ilv_fill`, every vertex of every stream) before the cache was
asked, and a cache hit then drew from the host copies and threw the
copy away. Heaven's draws are all such draws. The interleave now waits
for the miss: DRAW8 ~3.3 us a draw (was ~7), 65,000-80,000 draws a
second, the executor 43-56% busy, Heaven windowed ~53 frames/s (was
34-38) and **its benchmark 70.5 FPS, score 1777** (min 33.4, max 134.7;
44.0 / 1109 with the cache alone, 27.3 / 687 with neither). The XP
checks, `win98-dx9`, `win98-winetest` (0 keys worse) and the
`d3dpt-dp2` / `-system` / `-gdi` oracles pass on that build. Tried
after it and dropped: setting the interleaved declaration only on a
miss, so a hit no longer switches the declaration twice a draw (3.6 us
a draw against 3.3: DXVK's own redundancy checks already make the
switch cheap).

### Full screen, 2026-11-27

**Each record flushed.** The executor left a record's commands with
DXVK until something waited, so a windowed present's readback (the
host's copy into the window's STAGING surface, inside the guest's
submission) waited for the whole frame to render from its Map on. Each
Direct3D 11 record now ends with a `Flush` of the immediate context
(`flush_each`, `D3DPT_D11_FLUSH=0` the A/B): the GPU renders while the
guest builds the rest of the frame. Heaven windowed, the present's host
time ~3.4 -> ~2.0 ms (`D3DPT_D11_PROF` token 65), the game's present
call ~8.8 -> ~7.2 ms, ~76 -> ~79-88 frames/s; the flushes cost ~5 ms
per 5 s (token 0). Full screen the benchmark held (208.2).

**Red and blue swapped full screen** (user: "did you notice that in
full screen it has blue and red channels swapped?"). Heaven's swap chain
is R8G8B8A8 and the mode is B8G8R8A8, so DXGI presents full screen with
a Blt from the back buffer into a primary of its own in the mode's
format, and the host did that Blt as a `CopySubresourceRegion`, which
DXVK carries out as a byte copy between the two formats. The primary's
flips then reached the scanout ring BGRA with the bytes RGBA. The Blt
now draws when the channels' order or the size differs (`blt_draw`):
the source copied into a texture it can sample, a full-screen triangle
(`d3dpt/exec/d3dpt_blt.hlsl`, compiled by fxc into the checked-in
`d3dpt_blt.h`, DXBC that DXVK reads on every host) into one of the
destination's format at the rectangle's size, that copied into place,
in a context state of its own so the guest context's bindings are not
touched. It does the scaled Blt too (linear filtering), which was
logged and skipped. The 8-bit four-channel families only; anything else
is logged once per pair. Heaven full screen with the right colours in
the player, and **its benchmark 218.6 FPS, score 5506** (1280x720 Low,
min 55.7, max 539.9). `d3dgame11 -compare 60` passes in both models.

Seen on the way: QEMU's screendump reads the display's VRAM, which a
flip on the scanout ring does not update every frame, and showed the
colours right while the player showed them swapped; the player's own
window (PrintWindow on its HWND, `pwin.ps1` in the session scratchpad)
is the check for a ring frame. A first try, swapping the channels in
the readback of a flipped R8G8B8A8 resource, never ran (no such flip
happened) and marking the resource `newer` there made every reference to
it read the frame back again: 81 frames/s. Dropped.

**The GUI laid out for the desktop** (user: "the image looks zoomed in
and cropped to screen size"): see "Open". The executor's viewports show
it: the scene at 1280x720 and its post-processing chain at fractions of
it, the menu bar at 1920x48. A relaunch while Heaven is minimised by the
test scripts' console (it loses focus at its start and stays minimised
until its taskbar button is clicked: `qmp.py click 1206 1054 1920
1080`) is when it happens; the first launch after a boot lays out right.

### Feature level 11_0, step 1: compute and UAVs (2026-11-27)

(user: "start on feature level 11"). Protocol v34 adds the compute
pipeline and unordered access: `UAV_CREATE` (the API's UAV dimension and
union; a buffer's flags are the DDI's, which equal the API's raw /
append / counter), `SET_UAV` (the compute stage's, with initial counts),
`SET_TARGETS_UAV` (the output merger's targets with UAVs; the driver
keeps a shadow of the 64 slots, because the DDI names the slots that
change and `OMSetRenderTargetsAndUnorderedAccessViews` takes every bound
one, unbinding the rest), `DISPATCH`, `DISPATCH_INDIRECT`,
`DRAW_INDIRECT` (instanced and indexed: the two DDI entries had shared
one stub), `CLEAR_UAV` (uint and float) and `COPY_STRUCTURE_COUNT`; a
compute shader goes through `SHADER_CREATE` with empty signatures. The
host marks what a dispatch's UAVs (and a draw's output-merger UAVs, a
UAV clear, a structure count) can write `newer`, as a draw's targets,
so a CPU read takes it back.

Claiming 11_0 needed two more answers, each found by its absence:
- `CheckFormatSupport`'s `D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES` for the
  formats 11_0 requires typed UAV stores of (the float, UNORM, SNORM and
  integer colour formats from 128 bits to 8, no sRGB, no BGRA;
  `fmt_uav`).
- `D3D11DDICAPS_SHADER`'s
  `COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X`. Without it
  every device on the adapter, 10_0 included, failed with
  `DXGI_ERROR_DRIVER_INTERNAL_ERROR` right after `GetSupportedVersions`,
  before `CalcPrivateDeviceSize`. The runtime's reason was in its own ETW
  journal: "Driver doesn't support compute on FL11"
  (`Microsoft-Windows-Direct3D11`'s `JournalEntry` events;
  `PROBE\etw.ps1` on the test machine traces the probe under it). That
  journal is the first place to look the next time the runtime refuses
  a device with nothing in the driver's log.

The claim is behind `D3D11FL11` (`HKLM\SOFTWARE\2ksbox`, processes or
`*`) until tessellation is done: a game at 11_0 would find hull and
domain shaders that draw nothing. `d3d11-probe` gained an 11_0 part,
the same work on ours and on WARP: a dispatch with a structured SRV and
UAV and a constant buffer, raw-buffer atomics, an append buffer and
`CopyStructureCount`, `DispatchIndirect`, a typed texture UAV after both
UAV clears, and a pixel shader's UAV at the output merger through
`Draw` and `DrawInstancedIndirect`. All identical to WARP's on the
first run (structured output 0 of 256 off, counter 512 / 512, appended
128 sum 16128 / same, pixel count 12288 / 12288, the image 0 of 4096
off by more than 1); the 10_0, 10_1 and Direct3D 10 scenes pass beside
it on the same driver, `d3dgame11 -compare 60` passes, and so do the
host's `d3dpt-dp2` / `-system` / `-gdi` on the v34 QEMU and executor.

### Feature level 11_0, step 2: tessellation (2026-11-27)

Protocol v35: `SHADER_CREATE` takes hull and domain shaders. Their
patch-constant signature rides where a geometry shader's stream output
would (`nso` counts its `d3dpt_d11_sig` entries after the output
signature's), so the token keeps its layout for older guest drivers. The
host builds the container with a `PCSG` chunk beside ISGN / OSGN (a hull
shader's patch constants as outputs, a domain shader's as inputs). The
DDI names tessellation factors as the bytecode does, one name per edge
(`D3D11_SB_NAME` 11-22); the container wants them by kind and index
(`D3D_NAME` 11-16: quad edge 0-3, quad inside 0-1, triangle edge 0-2,
triangle inside, line detail and density), which `sig_entries` now maps.
The stage setters already took both stages, and the patch topologies
pass through `SET_TOPOLOGY` as they are.

`d3d11-probe`'s 11_0 part draws one quad patch with uneven factors
(edges 4, 6, 8, 5, inside 7 and 3, integer partitioning), displaced and
coloured by its domain location, a patch constant (`TINT`) crossing
from hull to domain: identical to WARP's frame (0 of 65,536 pixels off,
41,945 drawn). The pipeline statistics answer: hull invocations 1 / 1,
domain 51 / 135 (vertex reuse is the implementation's). A first version
drew nothing on either (the domain's v up the screen turns the
tessellator's clockwise triangles around, and the default state culled
them all) and "passed"; the check now also wants a quarter of the frame
drawn.

### Feature level 11_0, step 3: claimed for every process (2026-11-27)

With compute and tessellation equal to WARP's, the driver offers 11_0 to
every process; `D3D11FL10` (`HKLM\SOFTWARE\2ksbox`, processes or `*`)
keeps one at 10_1, the A/B and the way out for a game that does worse at
11_0. DWM stays on Direct3D 9's driver (`D3D11Exclude`) as before.
Checked with nothing listed: `d3d11-probe` PASS (every level gives 11_0
on ours; the 10_0 / 10_1 / Direct3D 10 scenes, compute and tessellation
all equal to WARP's), `d3dgame11 -compare 60` PASS at 11_0, Edge's GPU
process at 11_0 draws its pages.

**Unigine Heaven with tessellation** (user: "where's tesselation?"): its
Tessellation button, greyed at 10_1, is live at 11_0, and the wireframe
(F2) shows the ground, the dragon and the walls tessellated. Its
benchmark in Direct3D 11, 1280x720 full screen, Low, **tessellation
Normal: 175.4 FPS, score 4419** (min 9.7, max 433.6); without
tessellation the same build gave 218.6.

### Multisampling (2026-11-27)

(user: "what about multisampling?"). The host already made multisampled
resources and resolved them on readback and copy; the driver claimed
none, because every claim tried in step 1 made the runtime refuse every
device. The runtime's ETW journal (`PROBE\etw.ps1`) named each refusal
in turn:
- "MSAA quality for parent != child": a typeless format (no format
  support of its own) answered 0 while its typed ones answered 1.
- "MSAA quality reported to be 0": a count feature level 11_0 wants was
  0 somewhere (8x was claimed only up to 64-bit formats).

Guessing cost a ten-minute cycle each, so a host-side tool (`mscaps.cpp`
in the session's scratchpad: every format's `CheckFormatSupport`
multisample bits and `CheckMultisampleQualityLevels` at 2x-32x, on WARP
and on the PC's NVIDIA driver at 11_0) gave the pattern to copy: 2x, 4x
and 8x, one quality level, for every format that can be a multisampled
target, 128-bit ones included; a typeless format as its family; and 0
for everything else, a depth format's sampled and stencil views (21,
22, 46, 47) included though they load multisampled. The driver now
answers exactly that (`d11_check_ms`, `ms_family`), and `fmt_support`
reports `MULTISAMPLE_RENDERTARGET` / `MULTISAMPLE_LOAD` (the stencil views
22 and 47 get the load bit, which they lacked). `D3D11NoMSAA` lists a
process offered none.

DWM crashed in `dwmcore.dll` while the claims were wrong, because its
device was refused like every other; it composes with them right (no
crash after a boot, the desktop as before). Also seen on the way: the
first run of a new probe right after `inst25.sh`'s install-and-restart
sat at load (one thread, the image's sections only, Defender active) for
minutes; after a plain reboot it ran at once.

`d3d11-probe` draws two overlapping triangles into a 4x target with a 4x
depth buffer, resolves and compares with WARP: 0 of 65,536 pixels off
(largest 2), 781 edge pixels blended in 13 colours (the three flat ones
and the coverage steps between them), and the 10_0 / 10_1 / Direct3D 10 /
compute / tessellation parts pass beside it. **Heaven, Direct3D 11, 4x
anti-aliasing, tessellation Normal, 1280x720 full screen, Low: 174.3
FPS, score 4392** (its settings say "1280x720 4xAA"; the edges in its
screenshot are blended), against 175.4 without anti-aliasing.

**The counts from the host** (user: "can't you claim the antialias
support based on the host graphics support?"). A fixed 2x / 4x / 8x
would oversell a host without 8x (the Mac's, unasked) and undersell one
with 16x. The executor now asks its own Direct3D 11 device
(`exec_d11_msaa`): for each of 2x, 4x, 8x and 16x, whether every format
the guest calls a multisampled target has a quality level there, in two
size classes (up to 64 bits a pixel, 128-bit), logging a format that
holds a count back. The counts go `d3dpt_exec_features` ->
`D3DPT_EXEC_FEAT_MSAA` -> the device's `CAPS` bits 16-23 (register set
v16; QEMU's `msaa=` property masks them) -> the kernel driver's
`fb_caps` -> the Direct3D 11 driver, which claims exactly those
(`fmt_msaa_counts`); an older host sets none, and none is claimed.

The first run found a wrong guest claim: B4G4R4A4 (115) has no
multisampling on the host ("held back by 115@2x 115@4x 115@8x", and the
NVIDIA driver offers none either), and the strict rule took every count
from the up-to-64-bit class for it; 115 is out of the list on both
sides. The PC's RTX 3090 through DXVK offers 2x / 4x / 8x for both
classes (`0x77`, no 16x), and the probe passes on it. The runtime takes
a partial claim (with only the 128-bit class multisampled, devices were
made), and with `msaa=0x33` (2x and 4x only, a weaker host) the probe
passes too, its 4x frame equal to WARP's.

### The Witcher 3 (2026-11-27, register set v17)

(user, on their own machine: "it only runs in borderless mode, if I try
to put it to full screen the window hides and don't show again ... there's
no resolution options showing"; and "the far away terrain in the mountain
pass viewing from the balcony is missing the texture".)

**No resolutions.** `tools/dxgi-modes.cpp` (docs/testing.md) printed
DXGI's mode list per format: B8G8R8A8 had all 19 modes, **R8G8B8A8 none**,
and `FindClosestMatchingMode` in R8G8B8A8 failed with
`DXGI_ERROR_NOT_FOUND`. DXGI lists a format's modes only from source
modes of that format, and the kernel driver offered every mode as
A8R8G8B8 alone; a game that asks for R8G8B8A8 (The Witcher 3) had an
empty list and no mode to go full screen in. The kernel driver now offers
each size as A8B8G8R8 too (`fill_source_modes`); the desktop still
commits A8R8G8B8.

**Showing such a mode.** A full-screen R8G8B8A8 swap chain then flips its
own buffers in an A8B8G8R8 mode, red in the low byte. Register set v17's
`SCAN_RGB` (`CAP_SCAN_RGB`) tells the device the mode's order; the kernel
driver offers A8B8G8R8 only on a device with the cap and writes the
register at `CommitVidPn`. The device shows such a page through its
shadow, pixman converting from x8b8g8r8 (`fb_make_src`). The scanout ring
takes an RGBA frame too: `ring_take` draws it into the slot in the slot's
order with the converting Blt's kit (`blt_draw_res`, `blt_draw`'s work on
bare resources), where it read the frame back before. `dxgi-modes ...
fullscreen` (an R8G8B8A8 swap chain full screen, red) shows pure red in
the player's window (the ring) and in a screendump of `d3dpt0` (VRAM).
In the game, Options > Video > Graphics lists the resolutions and Full
Screen switches into an A8B8G8R8 mode with the right colours.

**Open:** on the overlay the game left full screen again when it
re-created its device for a save's load, and started windowed with Full
Screen saved (the runs typed through QMP). On the user's own machine with
the new driver (user, 2026-11-27) full screen works and Heaven's colours
are right, but the Graphics page shows **no resolution slider**, where
the overlay's showed one; not looked at yet.

**The distant terrain.** The executor logged `an object of kind 9:
0x80070057` once a session: an input layout refused. Logged in full
(`d11: input layout N refused`, the first 16 with their elements), it had
**no elements**: a vertex shader that makes its vertices from
`SV_VertexID`. Direct3D 11 takes such a layout; the executor passed an
empty vector's `data()`, null, which DXVK refuses, and every draw with it
was lost. A zero-element layout now gets a valid pointer; the game's
sessions log no refused layout since.

**The driver install on a machine whose clock went back.**
`wddm-install.ps1` stamps DriverVer from the guest's clock (the day and
the minute); the user's machine, booted from an overlay whose clock read
hours earlier, kept the package installed that day (0.4.330.1159 over
the new 0.4.330.1055) and the install silently did nothing. The stamp is
now never older than the driver a d3dpt-vga device runs (date and
version, past it by one).
