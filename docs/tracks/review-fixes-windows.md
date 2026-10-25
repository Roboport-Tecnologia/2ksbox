# The review pass's Windows half (2026-10-25)

Branch `track/review-fixes`: three review passes over the host code and
the drivers (bugs, security, performance), then the fixes that could be
made and checked on Linux. `scripts/test.sh all` passed there (74, with
`pit-guest` the baseline failure). What is left needs the PC: code only
the EWDK builds, and code that runs only on a Windows host or in a
Windows 11 guest on our adapter. This doc is the list, in order. Delete
it when every item is done or moved to its track doc.

## 1. Build and run what Linux could not

Nothing below has been compiled on Windows. In MSYS2's MINGW64 shell,
on the branch:

```sh
scripts/build-windows.sh                 # QEMU (MSVC), player, launcher, executor, DXVK
scripts/build-windows.sh wddm            # the WDDM driver, EWDK 10.0.19041
scripts/test.sh all                      # docs/testing.md "On Windows"
scripts/package-windows.sh               # rolls and checks the package
```

What each part has to show:

- **The WDDM driver** (`wddm/km/d3dptkmd.c`, commit d80cd9e5): it
  compiles and loads, and the desktop and the probes behave as before.
  The four changes: `run_d3d`'s RETURN slot check is 64-bit (on x86 it
  wrapped, a write below the VRAM mapping); `run_blt`'s CPU path checks
  the source row for the bytes it copies (`w * dst.bpp`, it checked
  `w * src.bpp`); `run_d3d` and `d3d_put` check each record's size again
  where they read and copy it; `d3dpt_remove_device` closes
  `follow_event_h`. Check on x64 Windows 11 and on 32-bit Windows 7:
  `D3DGAME11 -compare 300` (doc 15's probes, `g11.ps1`), `D3DGAME9
  -frames 600` windowed and with `-fs` (it has no `-compare`), DWM
  composing, the taskbar's text, a window dragged, a full-screen flip
  chain. Then `build-windows.sh wddm --publish` so Linux and the Mac
  fetch it.
  - **Windows 11 x64, 2026-10-25, with section 2's fixes too**: it
    compiles for x86, x64 and ARM64 with no warnings and loads;
    `D3DGAME11.EXE` and `D3DGAME64.EXE -compare 300` PASS; `D3DGAME9
    -frames 600` 59 fps windowed, 60 fps full screen (the flip waits for
    the blank); `GDITEST` 37 of 37; DWM composes, the taskbar's text and
    clock draw; no refusal, reset or `CollectDbgInfo` in the log. Not yet
    checked: 32-bit Windows 7, a window dragged by hand, `--publish`.
- **The executor** (`d3dpt/exec/`, MSVC): `d3dpt-dp2` and
  `d3dpt-dp2-system` in the suite. The `system` run matters: Windows'
  own d3d9 is the one D3D9 that the saturated primitive count
  (`prim_verts`) and the bounded `QUERY_GET_DATA` were not run against.
- **The player** (`player-core`, `player-mitsuami`, MSVC):
  `player-mitsuami` in the suite, then by hand a 98 or XP game with a
  relative mouse (the cursor sprite is now blended into its own rect
  and put back at the next frame: no trail, no flicker, gone when the
  pointer leaves), and closing a Windows 11 machine (the host-vblank
  thread now stops before the VM is freed).
- **The launcher** (`launcher-core`, MSVC): bundles are now written to a
  temporary file and renamed over (`paths::write_atomic`). On Windows
  the rename is refused while another process holds the file open, and
  the write then falls back to in place: edit a running machine's
  settings and the disc shelf while it plays. Starting a machine that
  is already up must say so instead of starting a second player.
- **The suite on the PC, 2026-10-25**: everything built (`build-windows.sh`,
  505 s). `test.sh all` found two regressions of pass 3's form check
  ("That disk image doesn't exist."): `capi` and `mitsuami` create a
  machine on the null device, and Windows' `NUL` has no metadata, so
  `Path::exists` is false there. The check now also takes a path that
  opens (`wizard.rs`), and the launcher's `create` screen names `NUL` on
  Windows rather than `/dev/null`. `whpx-i386` failed once (the MMIO push
  never reported) and passed on its own run after; watch it. `test.sh`
  gained single-check mode for these reruns (`test.sh capi mitsuami`,
  docs/testing.md).

## 2. The WDDM driver: the fixes made on the PC

Made in `wddm/km/d3dptkmd.c` on the PC and checked with item 1's
driver on Windows 11 x64 (its results above); Windows 7 is still to do.
A refusal shows in the driver's log as one of the lines named below.

1. **`CreateAllocation` checks the descriptor** (`alloc_desc_check`):
   a kind we know, sizes in 64 bits and refused past the segment (on x86
   a size near 4 GiB wrapped to one page), a D3D allocation's first level
   (`pitch` x rows, DXT rows in blocks) and every `lv[]` offset inside
   `size`; dxgkrnl's kinds get `bpp` and `pitch` remade from `format` and
   `w`, as GetStandardAllocationDriverData makes them. The check runs on
   the driver's copy. A failed pool allocation returns
   `STATUS_NO_MEMORY`. Log: `CreateAllocation refused, kind`.
2. **A patch lands inside one record's body** (Render): not on a header,
   and not on the context word of a record that names one; the compare
   is `PatchOffset <= CommandLength - 4`, which cannot wrap. Log: `a bad
   patch location`.
3. **A process names only its own host context.** No protocol change:
   `CreateContext` (which runs in the calling process) refuses an id that
   is not built from the caller's process id as the user-mode drivers
   build it (`pid << 8 | n`; `1 << 31 | pid << 7 | n`; `host_ctx_mine`),
   and Render, which runs in the submitting process, passes only the ops
   the user-mode drivers send (NOP, RELEASE, the queries, CTX_CREATE,
   CTX_DESTROY, DP2, READBACK, VRAM_DIRTY, D11, the RETURN record), with
   the context word of CTX_CREATE, CTX_DESTROY, DP2 and D11 built from
   that process's id the same way. Render cannot compare with its
   context's id: the driver is not MultiEngineAware, so dxgkrnl never
   calls CreateContext and Render's handle is the *device's* (the first
   build compared with it, read a pointer, and refused every Direct3D
   record; the desktop froze). Before, a user record could also register VRAM
   (`VRAM_SURFACE`), release another's surface, or run a GDI operation
   over any VRAM. Logs: `CreateContext: host context ... is not process`,
   `Render: record refused, op`.
4. **The deferred flip's address is queued under the interrupt's
   synchronization**: `run` keeps it in `defer_addr`, and
   `finish_submission` (already a `DxgkCbSynchronizeExecution` callback)
   writes `flip_addr`, `flip_pa` and `flip_vram`.

5. **A process names only its own submission's surfaces** (protocol
   v31, `SCOPE`). Resources and surfaces are global host handles, and a
   user-mode driver could write one into a record raw instead of through
   a patch. The kernel driver now opens a scope of the submission's
   allocation-list handles around its records (`d3d_scope`), and the
   executor refuses a surface or resource a record or token names from
   outside it (`surf_named`, `buf_ref`, `res_ref`, `RES_CREATE` and the
   other naming sites), while a handle a context bound earlier stays
   usable. Queries carry the process's id too, now `(pid >> 2) << 12 |
   n` with a process-wide count (it was 12 bits of the pid, so two
   processes could already collide), checked like contexts. Doc 14
   "One process cannot name another's objects"; `d3dpt-dp2`'s scope
   cases.
6. **A crashed process's host contexts are destroyed.** CreateContext
   and DestroyContext never run here, so the device remembers the host
   contexts its submissions name (`device_note_ctx`, from Render's
   CTX_CREATE, DP2 and D11 records: a Direct3D 11 context is made by its
   first D11 record, there is no CTX_CREATE) and DestroyDevice queues
   their destroy, the call dxgkrnl makes for a process that ended
   without its own. Checked on Windows 11 x64: D3DGAME11 killed after
   10 s, `DestroyDevice: host contexts to destroy 1 first 0x80038001`
   (its pid 1792, 0x700, in the Direct3D 11 encoding).
   - The test machine had `dwm.exe` in `HKLM\SOFTWARE\2ksbox`'s
     `D3D11Trace` (every call logged, a VM exit a character): it filled
     the DEBUG register's 1000 lines a second and slowed the desktop
     (D3DGAME11 -compare 9.3 s, 6.3 s without). Taken off 2026-10-25;
     `d3d11-probe.exe` stays.

## 3. Changed on Linux, to be checked in a Windows 11 guest

These run in the executor or the player on any host, but only a Windows
11 guest on our adapter exercises them, and the reference for them is the
PC (`D3DGAME11 -compare`, WARP as the oracle).

- **GDI staging cache** (`d3dpt_exec_d11.cpp`, `gdi_tex`, `gdi_read` /
  `gdi_read_end`): GDI operations read through four cached textures that
  only grow (destination, source, ClearType coverage, a scroll's temp),
  and all of an operation's reads are queued before the first `Map`, so
  the GPU drains once an operation instead of up to three times, with no
  texture made per operation. `gdi_copy` reads the bounding box of its
  rectangles once. One behaviour change: a staging texture or `Map`
  that fails now skips the whole operation, where it skipped one
  rectangle. Check Windows 11's desktop for text (ClearType), window
  scrolling, menus and a dragged window; compare against Basic Display
  by eye, and run `D3DGAME11 -compare 300`. Worth a before / after of
  the guest's frame rate while scrolling a long Explorer list.
- **`rop3` fast paths**: the common ROPs (0x00, 0x33, 0x55, 0x66, 0x88,
  0xCC, 0xEE, 0xF0, 0xFF) skip the per-bit loop. Checked identical to
  the loop for all 512 values on Linux; nothing to do but watch for odd
  colours in GDI drawing.
- **Object and resource limits** (`d3dpt_exec_d11.cpp` 202-211,
  `res_size`, `put_obj`; `d3dpt_exec_ddi.cpp` 264-274): 4096 Direct3D 11
  contexts, 262144 resources, 1048576 objects across contexts and an
  8 GiB budget for resources (each within Direct3D 11's own limits:
  16384 a side, 2048 for 3D and arrays, 2 GiB a resource); for Direct3D
  9, 262144 surface registrations, 4096 contexts, 65536 shaders,
  16384 state blocks and 4096 palettes; 65536 queries. A refusal logs
  once and fails as that call fails. Far above what DWM and Heaven use,
  but confirm nothing hits them: a long session on Windows 11 (Explorer,
  Edge, a Direct3D 11 game) with the executor's log checked for
  "limit" / "budget" lines.
- **Live polls time out after 1 s** (`control.rs` `connect_poll`,
  `TIMED_OUT`, `is_timeout`): the disc shelf's and snapshot window's
  timers no longer freeze the window for 10 s on a wedged QEMU. On
  Windows the monitor is AF_UNIX through `control.rs`'s `imp` and a
  timeout reads as `TimedOut`, not `WouldBlock`; `is_timeout` accepts
  both, but no Windows run has seen it. Pause a machine and leave its
  Discs window open: the drive must not flicker empty.
- **The idle-tick skip of the CRT chain** (`gpu.rs` `chain_stale`):
  identical on every host, but the PC's 144 Hz screen is where a stale
  frame would show first. A preset that is not animated, a still
  desktop, then a window dragged: no frame may lag.
