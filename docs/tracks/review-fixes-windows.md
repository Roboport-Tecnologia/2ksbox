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
  `D3DGAME11 -compare 300` and `D3DGAME9 -compare 300` (doc 15's probes,
  `g11.ps1`), DWM composing, the taskbar's text, a window dragged, a
  full-screen flip chain. Then `build-windows.sh wddm --publish` so Linux
  and the Mac fetch it.
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

## 2. The WDDM driver: fixes left

Each needs the EWDK to build and a Windows 7 / 11 guest to check, so
none was attempted on Linux. Line numbers are as of d80cd9e5.

1. **`CreateAllocation` trusts the user-mode driver's descriptor**
   (`d3dptkmd.c` ~1795-1890). Any process can call D3DKMTCreateAllocation
   with its own private data.
   - On x86, `ROUND_TO_PAGES((SIZE_T)d->size)` wraps to 0 for a size
     over 0xFFFFF000, and `(SIZE_T)d->pitch * d->h` wraps the same way.
     `info->Size` is then tiny while `w`, `h`, `pitch` and `lv[]` still
     describe a large surface, which `d3d_register` hands to the host:
     a one-page allocation registered as a surface over other processes'
     VRAM (the host bounds it by all of VRAM, not by the allocation).
   - Nothing checks `kind` is one we know, `pitch >= w * bpp`, `bpp`
     against `format`, or that the `lv[]` offsets and sizes fit inside
     `d->size`.
   - A failed `ExAllocatePoolWithTag` returns `STATUS_INVALID_PARAMETER`
     through `fail:` (~1890); it should be `STATUS_NO_MEMORY`.
   - Fix: compute sizes in `ULONGLONG` and refuse past a cap (the
     segment's size); refuse an unknown `kind`; for the non-D3D kinds
     recompute `bpp` and `pitch` from `format` and `w` rather than take
     them; check every `lv[i]` inside `d->size`; keep the status in a
     variable that `fail:` returns.
2. **Patch locations may land on a record's header** (Render, ~3890-3925).
   The records are checked first, then the patches are applied, and a
   patch is only required to be in range and 4-aligned. A HANDLE or
   OFFSET patch on a record's `size` rewrites it after the check. The
   readers now refuse a record whose size no longer fits (d80cd9e5), so
   this no longer reaches a 4 GiB copy, but the batch is cut short.
   Fix at the source: while checking the records in Render, keep their
   start offsets, and refuse a patch whose `[PatchOffset, +4)` is not
   wholly inside one record's body (at or past `start +
   sizeof(d3dpt_cmd)`). Compare with `PatchOffset > CommandLength - 4`,
   which cannot wrap.
3. **One process can name another's host context.** The user-mode
   drivers make the id (`um/d3dptumd.c:2371`, `(pid << 8) | n`;
   `um11/d11_dev.c:371`), `CreateContext` takes it from the private data
   (~1646), `DestroyContext` queues `CTX_DESTROY` for it (~1670), and the
   records name it too. A process that passes DWM's id destroys DWM's
   host context when it exits, or draws into it. Making the kernel driver
   pick the id is not enough on its own, because the records carry it:
   the kernel driver would have to give each `D3DPT_CONTEXT` its own id
   (from a counter, not the pid) and either rewrite or check the context
   handle in every record of a submission against the submitting
   context's (`Render` already walks every record). A protocol change on
   both sides; bump `D3DPT_PROTO_VERSION` if the records change.
4. **Flip state written outside the interrupt lock** (`run`, ~2929-2931;
   the rule is stated at ~154). `flip_addr`, `flip_pa` (64-bit, tears on
   x86) and `flip_vram` are written there while the ISR and
   `release_flip_sync` read them. `MaxQueuedFlipOnVSync = 1` keeps it
   from happening today. Move the three writes into the
   `DxgkCbSynchronizeExecution` callback `finish_submission` already
   runs.

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
