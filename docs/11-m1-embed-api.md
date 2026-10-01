# 11. M1 design: `libqemu_embed`, QEMU in-process

The library that puts QEMU inside the player: its shape, the QEMU entry
points it uses, the patches it needs, the audio driver and the hazards.
The API is **v9** (`QEMU_EMBED_API_VERSION` in `embed/libqemu_embed.h`
and `API_VERSION` in the `qemu-embed` crate move together; rebuild the
libraries before the players link). The 3D context provider is doc 12, the
player's display pipeline doc 03. QEMU file:line references are to
`qemu/` as prepared from v9.2.4.

## Shape

One shared library per target, `libqemu-embed-<target>.{so,dylib,dll}`
(`i386` for the era's machines, `x86_64` for Windows 11), built by
QEMU's own meson from the per-target static library (which already
excludes `system/main.c`, so there is no `main()`) plus our shim
`embed/libqemu_embed.c`. `prepare-qemu.sh` rsyncs `embed/` into
`qemu/embed/`, like the 3dfx overlay. A stale copy links the player
against an old library (`undefined symbol _qemu_embed_…`;
`qemu-embed/build.rs` warns). The `qemu-embed` crate's bindings are
hand-written: the API is small, `qemu_embed_api_version()` catches
drift, and no libclang is needed.

**One player binary per target, each linking its QEMU.** The era's
`2ksbox-player` links i386; Windows 11's `2ksbox-player-x86_64` is the
same player built with `--features qemu-x86_64` (track M20), and the
launcher starts the one a machine needs (`player::player_binary_for`).
Not one player opening its QEMU at run time: patch 63 reserves TCG's
code buffer next to the helpers from a constructor that has to run when
the image loads, before `main()` and anything else fragments the address
space, and a library opened later puts it 8 GiB away on Apple Silicon in
a third of launches, helper-heavy code then 35–45 % slower (doc 22 §5.0).
Tried and reverted in M20 step 3 for that reason.

**Thread contract.** Call `qemu_embed_new`, `_run` and `_destroy` on one
thread. Display callbacks fire on that thread with the BQL held and must
not block. Everything else may come from any thread.

## The API by version

| v | Adds |
|---|---|
| 1 | lifecycle (`new` from a plain `qemu-system` argv, `run`, `destroy`); VM start / pause / reset / powerdown / shutdown / running; 2D display callbacks (`on_switch`, `on_update`, `on_refresh_done`, `on_cursor`, `on_mouse_set`); keyboard qcodes, `atset1_to_qcode`, relative / absolute pointer and buttons, `mouse_is_absolute`, `input_flush` |
| 2 | `set_audio_ring` (the `embed` audiodev) |
| 3 | `set_refresh_ms`, the display refresh pull interval (QEMU's default 30 ms; the player asks ~16) |
| 4 | `on_3d_active`, `on_3d_frame`: qemu-3dfx's frames, copied (doc 12) |
| 5 | Linux zero-copy: `on_3d_dmabuf` offers each ring slot's dma-buf once, `on_3d_frame_ready` names the slot per frame |
| 6 | macOS zero-copy: `on_3d_iosurface` |
| 7 | `socket_to_fd` for the QMP socket on Windows (below) |
| 8 | `pad_state`, `pad_present`: the gamepad (M13); the same bytes feed the gameport |

Windows has no zero-copy slot; its 3D frames arrive through
`on_3d_frame` (a DXGI shared handle is open, M11).

## What needs no QEMU changes

- **Lifecycle.** `qemu_init(argc, argv)` → `qemu_main_loop()` →
  `qemu_cleanup()` (`include/sysemu/sysemu.h:98-100`) on one
  caller-created thread. `qemu_init` takes the BQL on the calling thread
  (`system/runstate.c:864`, thread-local ownership `system/cpus.c:515`),
  so init and the main loop **must share a thread**. The library always
  appends `-S` and `-display none`, so the guest is paused when init
  returns (otherwise `qmp_cont` runs before displays exist, `vl.c:2751`);
  the player hooks up, then starts the VM.
- **Display.** After init, with the BQL held, the library registers a
  `DisplayChangeListener` on `qemu_console_lookup_default()`
  (`ui/console.c:694`). `dpy_gfx_switch` hands over a new surface, freed
  on return, so never retain it (`ui/console.c:853`). `dpy_gfx_update`
  gives a clamped dirty rect. `dpy_refresh` calls `graphic_hw_update()`,
  the pull that makes the VGA device render; the GUI timer exists only if
  some listener has `dpy_refresh` (`ui/console.c:108-127`). On a machine
  with two adapters (Windows 11 on Arm's `ramfb` and `virtio-gpu-pci`,
  M20) the listener moves, from a bottom half, to the last graphic
  console whose surface is not a placeholder, and back to the default
  when there is none (`embed_live_console`); the move is one more
  `dpy_gfx_switch`, and input follows it.
- **The window's size** (v9). `qemu_embed_set_window_size(w, h, dpi)`
  passes the player window's drawable size in physical pixels to the
  console on show as QEMU's `QemuUIInfo` (`dpy_set_ui_info`, with a
  width and height in millimetres for that DPI): the first size at once,
  later ones after QEMU's one-second settle, and the current one again on
  a console switch. Only an adapter with a `ui_info` hook hears it
  (virtio-gpu); `qemu_embed_display_follows_window` says whether the one
  on show does, and the player then lets the window go below the guest's
  mode. What the guest makes of it is its driver's: Windows 11 on Arm's
  viogpudo takes it when it starts, not live (track M20).
  `dpy_gfx_check_format` accepts only `x8r8g8b8`, so QEMU shadows
  8/15/16/24 bpp into 32 bpp; 32 bpp modes are zero-copy (the surface
  points into VRAM, `hw/display/vga.c:1637`). All callbacks fire on the
  main-loop thread under the BQL, and registration fires switch + update
  + cursor synchronously.
- **Input.** `qemu_input_event_send_key_qcode`,
  `qemu_input_queue_rel/abs/btn` + `qemu_input_event_sync`
  (`include/ui/input.h`) must run under the BQL and are dropped while
  paused. The library enqueues from any thread and drains in a
  bottom-half (`aio_bh_schedule_oneshot`), one sync per batch. It prints
  `qemu-embed: input:` lines on stderr (drain latency, zero-length
  presses, drops) only when something is off.
  `qemu_input_is_absolute()` plus the mouse-mode notifier say whether the
  guest wants tablet or PS/2 semantics.
- **VM control.** `qemu_system_{vmstop,reset,powerdown,shutdown}_request()`
  are async and thread-safe; `vm_start()` needs the BQL, so it goes
  through a bottom-half.

### QMP

The player (`player/src/qmp.rs`) makes a `socketpair(AF_UNIX)` and passes
one end as `-chardev socket,id=qmp0,fd=N -mon chardev=qmp0,mode=control`:
full QMP with events, the monitor on its own iothread, no filesystem path
and no network. The player logs notable events and runs
`PLAYER_QMP_EXEC` once the guest has drawn. Live control from the
launcher is a second monitor the launcher adds (doc 07).

Windows has no `socketpair()`. The player makes a connected pair on
loopback and keeps it only when the accepted peer's address is exactly
the one its own connecting socket was given, which no other process can
hold at the same time. `N` is **not** a `SOCKET`: every socket call in
QEMU's Windows build is an `os-win32.h` wrapper that starts with
`_get_osfhandle(fd)`, so `fd=` indexes the C-runtime descriptor table of
whichever CRT the module links, which the player cannot know. The handle
crosses as a handle and **`qemu_embed_socket_to_fd()` converts it inside
the library**. A raw `SOCKET` is refused at startup as `File descriptor
'N' is not a socket`.

## What needs patches

1. **`10-embed-api.patch`**, meson: `shared_library('qemu-embed-<target>',
   files('embed/libqemu_embed.c'), objects: lib.extract_all_objects(…),
   …)` beside the executable. Linking the objects directly, not through
   the archive, keeps the `type_init` constructors; it needs `-fPIC`
   (`configure-qemu.sh` adds it).
2. **`20-embed-audio.patch`**, the `embed` audiodev. The driver is
   `embed/embedaudio.c`, compiled into the shared library. QEMU needs a
   `qapi/audio.json` enum + union entry, a per-direction case in
   `audio_template.h`, **and a case in `audio/audio.c:
   audio_create_pdos()`**; without that the pdo is NULL and
   `audio_validate_per_direction_opts` segfaults.
3. **Patch 30** puts qemu-3dfx's display entry points behind a provider
   vtable, and the library registers a window-less provider (doc 12).
   Patch 31 makes the native Mesa backend weak so ours overrides it.

## The audio driver and its pacing

The player appends `-audiodev embed,id=embed0,timer-period=5000,
out.frequency=<host rate>,out.channels=2,out.format=f32`; devices attach
with `audiodev=embed0`. QEMU's mixer writes interleaved PCM straight into
the caller's ring (`get_buffer_out` / `put_buffer_out`), and cpal drains
it (doc 07). The header of `embed/embedaudio.c` has the full argument,
including the two earlier designs that failed. The rules:

- **The guest is drained at its own clock's pace, never in a burst.**
  QEMU's sb16 and AC97 move the guest's DMA exactly as far as the mixer
  drains it, so a burst moves the play cursor past what the guest's
  driver has written. That was the crackle (`tools/audio-glitch-test.py`).
- **A stalled main loop is paid back gradually.** A 3D swap under the
  big lock is repaid over the following ticks, at most three ticks'
  worth between two (15 ms at `timer-period=5000`), with up to 100 ms
  owed.
- **A ±25/10 % rate correction holds the ring's minimum** over a quarter
  second at `out.buffer-length` (default 40 ms, the player's
  `PLAYER_AUDIO_MS`), the cushion under whatever period the host device
  drains in. The player starts a stream once the ring holds a period
  plus the cushion.
- **The ring is f32.** QEMU's mixer sums every voice at full scale; its
  s16 conversion saturates the sum and its float one does not, so the
  player limits instead of clipping.

## Hazards

- `qemu_init` errors are `error_fatal` → `exit(1)`, so validate the
  configuration before calling.
- `os_setup_signal_handling()` installs SIGINT/SIGHUP/SIGTERM handlers
  process-wide (`os-posix.c:57`); the player's signals are QEMU's for the
  life of the process.
- `qemu_cleanup` is incomplete (`runstate.c:929` TODO), so it is **one VM
  per process lifetime**, which matches the launcher/player split (doc
  02). Never exit the process while the QEMU thread is alive: QEMU's
  atexit handlers race `qemu_cleanup`. The player joins the thread and
  returns from `main` (headless paths use `_exit`). A guest power-off
  while the UI still holds the handle goes through a stop/release
  handshake.
- qemu-3dfx's `graphic_hw_passthrough()` makes `graphic_hw_update` skip
  the device while 3D is active (`ui/console.c:147-152`). Upstream then
  renders into QEMU's SDL2 window and refuses to activate without one;
  patch 30's provider replaces it, and QEMU is built `--disable-sdl`.

## Player side

`player/src/qemu_vm.rs` spawns the QEMU thread, copies dirty rects into
a shared staging frame under the callback and publishes it on
`on_refresh_done`. While 3D is active, the VGA surface is shown only
once 3D frames stop and the guest has drawn on it. 3D frames arrive as a
copy or a ring slot index (`dmabuf.rs`, `iosurface.rs`). Keyboard and
mouse go from winit through `qemu_embed_key` / `mouse_*` and one
`input_flush` per batch; the pad goes through `pad_state` once per
published frame. The rest of the player is doc 03 and doc 07.
