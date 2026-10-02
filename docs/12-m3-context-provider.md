# 12. Window-less GL context provider for qemu-3dfx (M3)

qemu-3dfx renders guest OpenGL with host OpenGL, and upstream assumes
an SDL window to do it in. The embed library has no window: the player
owns the screen (doc 03) and QEMU is built `--disable-sdl`. This doc
covers the offscreen context that replaces the window and hands its
frames to the player's shader chain, the zero-copy ring (§4) and the
Windows WGL rule. The Direct3D path is doc 14; Glide is the emulated
Voodoo 2, whose frames go through the ordinary VGA surface (doc 21).
§5 records the Glide pass-through this provider once also served.

## State

| | Linux | macOS | Windows |
|---|---|---|---|
| GL context | EGL surfaceless, pbuffer as FBO 0 | CGL, an FBO stands in for FBO 0 | WGL pbuffer |
| Frames to the player | dma-buf ring (zero-copy) | IOSurface ring (zero-copy) | readback |

Verified: Win98 wglgears in the player runs at 575–600 fps on Linux
through the ring (420–450 by readback), the Air reports `GL 2.1 Metal /
Apple M1`, and `TESTS\GLPROBE.EXE` on a Windows host reads the host's
own renderer through the pass-through. GLQuake on a Windows host has not
been tried.

## Facts that shape the design

File:line refs are to `qemu/` as prepared by `scripts/prepare-qemu.sh`.
`ui/sdl2.c` is in the tree but no longer compiled; it shows the upstream
shape we replaced.

- **All host GL runs on the vCPU thread under the BQL.** The Mesa device
  is an MMIO handler (`mesapt_mm.c`); every GL call, context creation
  (`MGLCreateContext`) and present (`MGLSwapBuffers`) happens there, and
  the main-loop timers that touch GL (`sched_wndproc`,
  `deactivateOneshot`, `dispTimerProc`) hold the BQL too. There is no GL
  thread; the BQL is the mutex.
- **The guest frame lives in FBO 0.** `MesaBlitScale` upscales by
  `glCopyTexImage2D` from FBO 0 and `MesaRenderScaler` rewrites viewports
  only when `framebuffer_binding == 0`, so FBO 0 itself is made
  offscreen rather than an app FBO bound.
- **Compatibility profile is mandatory**, since guests are GL 1.1–2.1
  fixed-function. QEMU's `qemu_egl_init_ctx()` hardcodes core profile, so
  we create our own context. On macOS compat GL is 2.1 only.
- **The UI seam is 11 functions** (`include/ui/console.h`):
  `mesa_{prepare,release}_window`, `mesa_renderer_stat`,
  `mesa_gui_fullscreen`, `mesa_cursor_define`, `mesa_mouse_warp`,
  `glide_{prepare,release}_window`, `glide_window_stat`,
  `glide_gui_fullscreen`, `glide_renderer_stat` (the five `glide_*` are
  unreferenced since patch 74, §5). In the handshake the
  vCPU calls `mesa_prepare_window(msaa, alpha, 0, cwnd_fn)`, the
  provider must call `cwnd_fn(swnd, nwnd, opaque)`, which sets
  `wnd_ready`, and the guest spins on MMIO `0xFB8` until then.
  `mesa_gui_fullscreen(sizev)` runs on every swap and must be
  synchronous; returning target size == guest size makes the in-QEMU
  scaler inert (we scale in wgpu).
- **`MGLCreateContext` / `MGLMakeCurrent` / `MGLSwapBuffers` are the only
  window-system-specific parts** of `hw/mesa`; `MGLFuncHandler`, pbuffer
  emulation and `MGLUpdateGuestBufo` are reusable. The native backend
  (`mglcntx_linux.c`, GLX) is linked **weak** (patch 31) so
  `embed/mglcntx_embed.c` overrides it inside the embed library only. On
  macOS it only refuses a context (patch 70), so the Mac build needs no
  XQuartz.
- **3D-active signalling** is `graphic_hw_passthrough(con, on)`, which
  makes `graphic_hw_update` skip the VGA device and tells no display
  listener, so the embed API gets an explicit edge.

## Design

1. **Patch `30-3dfx-ui-vtable`.** The 11 entry points become a vtable
   registered by the display (`QemuFxUiOps`). With no provider Mesa
   refuses contexts cleanly (patch 04) and the VM keeps running, which is
   what a bare `qemu-system-i386` does.
2. **`embed/mglcntx_embed.c`**, the backend built into the embed library,
   a copy of the GLX backend with the window-system calls replaced:
   - **Linux.** EGL on a DRM render node, our own compatibility-profile
     context, an EGL pbuffer at guest resolution so FBO 0 stays valid.
   - **macOS.** CGL (no main-thread requirement) with no drawable. An FBO
     over shared renderbuffers plays the default framebuffer and
     `glBindFramebuffer(…, 0)` is redirected to it in the dispatch table
     (patch 32's `MesaGLSetFunc`); WGL pbuffers are emulated with FBOs in
     the same context. Legacy 2.1 by default, core 3.2/4.1 when the guest
     asks through `wglCreateContextAttribsARB`. Every GL/CGL/IOSurface
     symbol is `dlsym`'d from the OpenGL.framework handle, never linked:
     a symbol bound to XQuartz's Mesa libGL silently no-ops with no GLX
     context.
   - **Windows.** WGL through libepoxy, a pbuffer drawable ("The WGL
     rule" below).
   - `MGLSwapBuffers` = blit + publish + notify, never blocking the vCPU
     on the consumer.
3. **`embed/embedfx.c`**, the provider, with the 11 entry points for the
   embed library. `mesa_prepare_window` creates the drawable and calls
   `cwnd_fn`; `mesa_gui_fullscreen` returns target == guest size;
   `renderer_stat` keeps `graphic_hw_passthrough` and raises the embed
   callback.
4. **The embed API's 3D callbacks and the zero-copy ring** (below).
5. **Glide** through our own OpenGLide build: retired (§5).

### 4. Frames to the player

The callbacks (`embed/libqemu_embed.h`) are `on_3d_active(bool)` on the
edge, `on_3d_frame(pixels, w, h)` for the readback path, and for
zero-copy `on_3d_dmabuf` (Linux) / `on_3d_iosurface` (macOS), each slot
offered once, then `on_3d_frame_ready(slot)` per frame so only the index
travels. A front end that declines an offer keeps the readback path.

- **Linux.** A ring of three linear ARGB8888 GBM buffers imported into GL
  as EGLImage textures; each swap blits FBO 0 (Y-flipped) into the next.
  The player imports each dma-buf into wgpu through `wgpu-hal` Vulkan
  (`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`,
  `player-core/src/dmabuf.rs`). The frontend gets its own `gbm_bo_get_fd`,
  since a Vulkan import takes ownership of the fd, and a declined offer
  is closed.
- **macOS.** An IOSurface ring bound to rectangle textures with
  `CGLTexImageIOSurface2D`. The player wraps each surface in a Metal
  texture (`player-core/src/iosurface.rs`).
- **Sync** is `glFinish` before the hand-off; a fence is the open
  refinement.

**A ring buffer can stop being written through, and the ring remakes
it.** GLQuake on Win98 flickered, every third frame the same frozen
picture: GL reads back every frame blitted into the slot, while the
dma-buf's own memory, which the frontend samples, keeps its first frame.
`zc_probe()` now and then writes a known colour into the slot through
GL before the blit and reads the buffer's memory back with
`gbm_bo_map`; a slot that disagrees is freed and made again, and the new
one holds. The schedule is dense while the ring is young and one
present in 512 after, because a buffer goes bad early and never recovers
(GLQuake: one slot, thirteen presents in; one repair, then every frame
distinct at 72 fps; wglgears never trips it). The knobs (`EMBED_ZC_PROBE`, `EMBED_ZC_HEAL`, `EMBED_ZC_SLOTS`,
`EMBED_ZC_CHECK`, `EMBED_ZC_MARK`, `PLAYER_ZC_IMPORT`,
`PLAYER_ZERO_COPY=0`) are in `docs/development.md`. Don't trust
`EMBED_ZC_CHECK` for this: it compares the GL and CPU views of a
*frame*, and while the picture does not change, a slot not written
through looks like one that is.

The cause is open. Ruled out, each measured:

- **the frontend**: with `PLAYER_ZC_IMPORT=0` (take every offer, import
  nothing) the slot still goes bad, 94 of 95 samples, so the divergence
  is GL against `gbm_bo_map` inside QEMU's process;
- **the import**: `tools/zc-vulkan-test.c` imports the ring as
  `dmabuf.rs` does, with no guest and no wgpu, and stays clean in every
  mode;
- **the guest's GL calls**: the 27 of `FuncTrace,1` that only GLQuake
  makes are all in `zc-vulkan-test --draw=all`, clean;
- **the frame it breaks on**: `FuncTrace,2` puts the break in GLQuake's
  first 3D frame (a level load: 934 `glTexImage2D`, 330 `glDrawBuffer`
  toggles), and `--draw=load` does the same volumes, clean;
- allocation pressure (`--draw=alloc`, 256 MB), drawable size or stride,
  the render scaler, the ring's size, fd ownership, a settling delay.

What is left is mesapt's own host path: the decoder's texture uploads
through shared memory, the vertex-array cache, mapped buffers.

**Driving the backend without a guest.** The `MGL*` backend can be
driven right after `qemu_embed_new`, with the BQL held, in this order:
`InitMesaGL` → `MGLTmpContext` → Choose/SetPixelFormat →
`MGLCreateContext(MESAGL_MAGIC)` → `MGLMakeCurrent(MESAGL_MAGIC, 0)` →
draw → `MGLSwapBuffers`. `tools/embed-3d-test.c` does that, checks the
activation callbacks and orientation, and draws several frames per
slot, requiring each slot's own memory to follow them. One blit per
slot only proves the ring was wired: a bad slot's first blit does land.

### 5. Glide: the pass-through, retired

Until 2026-09-23 this provider also served qemu-3dfx's Glide device
(`hw/3dfx`, a dispatcher that `dlopen`s a host `libglide2x` and looks up
183 entry points; upstream ships that library to donors only). Ours was
**OpenGLide** (LGPL) with a window-less platform layer (`glidept/`), a
patch queue and `scripts/build-glide.sh`; patch 33 reversed upstream's
handshake and handed the wrapper *this* context through a `GlideHostOps`
table, so `grBufferSwap` published a frame as a GL swap does. It ran
Rayman 2 and Carmageddon's DOS build headless, and no user ever played a
title on it by hand.

**ADR-020 removed all of it** (user decision: the emulated Voodoo 2 is
"a much better experience"): the submodule, the patch queue, `glidept/`,
patch 33, the guest `GLIDE*.DLL` and `GLIDE2X.OVL`, the embed provider's
Glide half and the two Glide checks. Patch 74 keeps `hw/3dfx` out of the
QEMU build (its directory is still overlaid because `sign_commit` stamps
a file in it), so a machine has no `glidept` MMIO region and the UI
table's five `glide_*` entries are unreferenced defaults. A Glide game,
Windows or DOS, runs on the Voodoo 2 with 3dfx's own driver and the
game's own Glide (doc 21). The design and its tests are in the history
before that commit (`tools/glide-host-test.cpp`, `GLIDETEST.EXE`,
`tools/glide-guest-test.sh`).

## The two moments a frame is presented

`MGLSwapBuffers` is one. The other is a **flush with the front buffer
selected**: a program rendering into `GL_FRONT` never swaps, and on a
real window it does not need to. Wine's ddraw presents the DirectDraw
primary that way, so on our offscreen drawable those frames went
nowhere, a black screen with the guest running behind it (doc 19 §44).

The backend therefore interposes four entry points in the guest's
dispatch table (`MesaGLSetFunc`, patch 32): `glDrawBuffer` and
`glReadBuffer`, which map a `GL_FRONT`/`GL_BACK` selection on the
guest's framebuffer 0 onto whatever plays it here
(`GL_COLOR_ATTACHMENT0` for macOS's FBO, `GL_BACK` (or `GL_FRONT` if
single-buffered) for the EGL and WGL pbuffers), and `glFlush`/`glFinish`,
which publish while the front buffer is selected. Only the guest's calls
take the hooks; ours go straight to GL, so nothing recurses.

## The WGL rule

On Windows the backend's entry points come from **libepoxy**, which
resolves each one lazily at its first call, and WGL answers
`wglGetProcAddress` only while a context is current on the calling
thread. An ARB call with nothing current therefore does not fail: it
**faults** with ACCESS_VIOLATION, taking the whole process and leaving a
log that ends at the line before.

`plat_open()` ends with `wglMakeCurrent(NULL, NULL)` on purpose (a WGL
context is current on one thread at a time, and the thread that opens
the backend is not always the one that draws), so the next ARB call,
`wglChoosePixelFormatARB` in `plat_choose`, ran with nothing current.
GLQuake, the first GL guest on Windows, took the player down that way
with `glcntx: ChoosePixelFormat()` as the last line of `player.log`.

1. **Every ARB call in the Windows backend borrows a context** when the
   caller has none (`wgl_borrow_ctx` / `wgl_return_ctx` in
   `embed/mglcntx_embed.c`) and restores exactly what it found,
   including "nothing", which the rest of the file relies on. A new ARB
   call must do the same.
2. **`wgl-probe.exe` (`tools/wgl-probe.c`) cannot catch this.** It
   resolves the ARB entry points by hand with its own context current,
   so it passes where the backend faults. It answers "will this driver
   give me an offscreen pbuffer at all", not "does our dispatch
   survive".

With the borrow the chain works on the user's PC: `GLPROBE.EXE` in Win98
gets a pixel format and an 800x600 drawable, reads the host's strings
through the pass-through (`NVIDIA GeForce RTX 3090/PCIe/SSE2`, `4.6.0
NVIDIA 616.64`) and unloads cleanly.

## Order

vtable patch → embed provider on Linux with readback → dma-buf import →
macOS CGL/IOSurface → the Windows WGL backend. Open:

- fence-based sync instead of `glFinish`, on both zero-copy platforms;
- the frozen ring slot's cause (§4);
