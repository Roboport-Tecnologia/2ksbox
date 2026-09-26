# Track M16: a DirectX 9 driver, and no custom DLLs (ADR-021)

The display driver becomes a DirectX 9 driver: Microsoft's own
`d3d9.dll` reaches the host through the driver's DDI, on XP and on
Win98, with shader model 3.0. Then every per-game graphics DLL on the
guest-tools ISO goes: `D3D8.DLL`, `D3D9.DLL` and `DDRAW.DLL` from
`D3DPT\`, and finally `OPENGL32.DLL` from `OPENGL\`, replaced by an
OpenGL ICD that the display driver installs (user, 2026-09-25: "our
native directx 9 driver. no more custom dlls anywhere"). `DINPUT.DLL`,
the FIFA 2000 keyboard shim, is not graphics and stays per game (user,
same day: "leave dinput for now").

A driver component that Windows itself loads is not a custom DLL in
this sense: the display DLL, the 9x HAL (`D3DPT9HL.DLL`) and the ICD
are installed once, by the driver's INF, and no game folder or system
file changes. The rule the track ends at is **nothing beside a game,
and no Windows file replaced**.

ADR-021 is the decision. Doc 15 "The DirectX 8 DDI" is where the driver
stands today; doc 19 §25 is the same DDI on 9x; doc 14 is the protocol
and executor. Read `docs/00-status.md` first for the track rules.

## State

Opened 2026-09-25. Step 0 is built and run on today's driver; the rig's
two baselines are the part left (they are the user's runs). Step 1 is
done: XP's `d3d9.dll` takes the driver as a DirectX 9 device with vs /
ps 3.0, 256 vertex constants, 16 streams and hardware vertex processing
(`DX9CAPS.EXE`), and `d3d8.dll` still sees the DX8 driver (the ten DX8
probes and SHTEST pass). Doc 15 "The DirectX 9 DDI" has the queries, the
runtime's caps check (the hard part: a failed check gives DX7-level caps,
not an error) and the tokens. Step 2 is done (2026-09-26): declarations,
shader code, integer / boolean constants, scissor, SETSTREAMSOURCE2 and
the target tokens reach the host (protocol v14); the blits and colour fill
run in the driver and the event and occlusion queries answer (no wire
change); D3DFEAT9's frame **and every getter line** match the native run.
Step 3 has its formats: the DX9 formats (float, 10-bit, 16-bit), sRGB,
the ARGB group's conversions and the DX9 samplers (doc 15 "The DX9
formats", "Samplers"), and mip generation (protocol v15, doc 15 "Mip
generation") with a managed texture's SetLOD, instancing (v16, doc 15
"Instancing") and four render targets (v17). What step 3 has left: a
vertex texture
fetch checked by a title, the 2.0-cap flag.

- **D3DGAME9 through XP's own `d3d9.dll`** (2026-09-25,
  `xp-driver-test.sh d3dgame9`): 600 frames on a hardware-vertex-processing
  device, frame 300 dumped, **0 pixels differ** from the native DXVK
  frame since finding 8's fix (17 % before it, all texture minification).
  D3DGAME8 through `d3d8.dll` on the same build: 0 pixels differ.
- **D3DFEAT9 through `d3d9.dll`** (`xp-driver-test.sh d3dfeat9`): the
  frame is **byte-identical** to the native run, the occlusion query
  counts the same 21316 pixels, and since 2026-09-26 the A16B16G16R16F
  render target's readback matches too (it was `D3DERR_INVALIDCALL`
  until the float formats were listed; listing them first broke the
  whole frame, findings 13 and 14). D3DFEAT9 itself changed: its ColorFill target is
  a render-target texture now, because Microsoft's runtime refuses
  ColorFill on a default-pool texture without `D3DUSAGE_RENDERTARGET`
  (Wine's `colorfill_test` says so; DXVK and our `D3D9.DLL` let it by).
- **Wine's suite on the DX9 face** (2026-09-26): d3d9 stateblock 14738
  checks, **0** failures. d3d9 visual **runs to its end**: 201792 checks,
  622 failures (803 before mip generation, findings 15, 17, 18, 20 to 23, 25, 26,
  instancing and DXT volumes; `multiple_rendertargets_test` runs since v17 and passes) (the crash at `visual.c:25891` was the test's own, a
  device with no window, which XP refuses: patch 03 of
  `patches/winetest/`). **Every file runs to its end** since findings 5
  and 2 were fixed (2026-09-26): d3d9 device 159591 checks, 21 failures;
  d3d8 visual 142764 / 158; d3d8 device 55641 / 2; d3d8 stateblock 9283
  / 0. `reference/winetest/xp-driver.txt` was saved again from this run
  (74 keys); the checks it gained are all in the three files that used
  to crash. After findings 29 to 31: d3d9 visual 618, d3d9 device 21,
  d3d8 visual 155, and **against the rig only finding 24 is left that
  DXVK's own run does not share** (`depth_clamp_test`, `z_range_test`,
  both runtimes).
- **DXVK's own failures** (`tools/winetest-dxvk.sh`, new): the same test
  EXEs under the host's Wine on DXVK's 32-bit `d3d9.dll` / `d3d8.dll`,
  in a headless sway. d3d9 visual 209672 checks, 1291 failures, no crash;
  d3d9 device 154166 / 830. Saved as `reference/winetest/dxvk-wine.txt`.
  A guest failure DXVK shares is DXVK's (most of `fog_with_shader_test`'s
  174: a vs 1.x that writes no `oFog` is not fogged, also on native
  DXVK); `winetest-summary.py --baseline dxvk-wine.txt --by-function
  build/winetest/wine-11.0` counts the rest per test function. A check
  Wine marks `todo_wine` fails on its host as "Test marked todo", which
  the summary counts as a failure since 2026-09-26 (it never appears on
  Windows); before that, `test_fog`, `test_shademode` and
  `pretransformed_varying_test` looked like ours and were DXVK's.
  Against it the guest's d3d9 visual fails 18 checks beyond DXVK:
  `test_fog` 6, `depth_clamp_test` 5 and `z_range_test` 2 (finding 24),
  `test_updatetexture` 2 (the volume cases),
  `conditional_np2_repeat_test` 2, and one in `test_desktop_window`.
- **Where DXVK itself differs.** A guest failure DXVK shares is DXVK's
  behaviour, and a patch in `patches/dxvk/` would be the fix. The clear
  case: fog under a vertex shader that writes no `oFog` (most of
  `fog_with_shader_test`'s 174), where DXVK leaves the pixel unfogged
  and Wine's reference card (a GeForce 7600) fogs it fully; a native
  DXVK probe answers as the guest does. DXVK also makes no autogen
  mipmap levels after an upload, which the host works around. Rule
  (user question, 2026-09-26): patch DXVK only where the rig's run and
  DXVK disagree and a title could meet it, so the rig's baselines come
  first. Part of DXVK's 1291 under Wine is Wine's (windows, GDI), not
  DXVK's.
- **Against the rig's XP run** (2026-09-26, `--baseline rig-xp.txt
  --split dxvk-wine.txt`): the rig fails 1448 of d3d9 visual's checks
  (a 2005 card), and `test_fog`'s 6 and `test_desktop_window`'s 1 are
  among them. The guest failed 53 keys the rig passes, 41 since
  findings 5, 2, 27 and 28 (the device files run whole, and pass
  wherever the rig does). DXVK's own run
  fails the same checks in `fog_with_shader_test` (174, and d3d8's
  119), `test_pointsize` 27, `test_shademode` 10, `fog_special_test` 8,
  `test_table_fog_zw` 8, `pretransformed_varying_test` 5, `test_ffp_w`
  4, `test_format_conversion` 2, `fp_special_test` 1 and
  `test_negative_fixedfunction_fog` 1 (d3d8 visual adds
  `test_shademode` 6): under the rule above these are the candidates for
  a patch in `patches/dxvk/`, each one a user decision, and they wait
  until the driver works (user, 2026-09-26: "let's do the dxvk patches
  after the driver is working"). The rest is ours: `depth_clamp_test` 5
  and `z_range_test` 2 (finding 24, a DXVK patch too, in d3d8 as well),
  `test_updatetexture` 2 in each runtime (volumes, finding 29),
  `conditional_np2_repeat_test` 2 (finding 30), and d3d8's
  `test_scalar_instructions` 1 (finding 31); since those, finding 24
  alone.
- **Step 5 begun (2026-09-26): Win98 on the DX9 face.** The 9x HAL
  offers what the NT layer does: `core.dx9`, the 15 FourCCs (shared block
  v7), `DDCAPS_BLT` in `dwSVBCaps`, lightweight mipmaps sized in
  `CreateSurface32`, DX9 query responses in `dwErrorOffset`,
  `GetDriverState` failing, the cube-face mip walk.
  `tools/win98-winetest.sh` runs the suite on a raw copy of `base98-br`
  (DirectX 9.0c) in about a minute. First results: d3d9 stateblock 14738 /
  0 (182 failures on the DX8 face before), d3d8 stateblock 9283 / 0, d3d9
  visual runs to its end with 1542 failures (XP 618, not yet triaged).
  Three things found on the way:
  - **The desktop must be 32 bpp.** On 16 bpp d3d8 refuses the tests'
    windowed A8R8G8B8 device and every d3d8 file skips; the rig's first
    Win98 run likely failed this way. The harness runs `SETBPP -save 32`
    (saved, because a full-screen device restores the registry's mode).
  - **d3d8 visual and device crashed in `3DFX32V2.DLL+0x185fb`**, 3dfx's
    own Voodoo 2 DirectDraw driver, which `base98-br` has installed: the
    run booted without the card and d3d8 loaded its driver anyway. Not
    ours (finding 32).
  - **Full-screen devices fail** `D3DERR_DRIVERINTERNALERROR` (`0x88760827`) inside the tests
    (A8R8G8B8 back buffer, D24S8, 640x480 / 800x600, `device.c:234` and
    `305`), while `D9CTEST.EXE`'s full-screen devices of the same formats
    are made. Found (2026-09-26, `D9CTEST -modechange`, which replays the
    first `test_wndproc` case with the test's message pumping, and runs on
    XP too): with the device window apart from the focus window, a mode
    change and focus handed to the desktop, **Win98's foreground lock**
    refuses the test's `SetForegroundWindow` back to the focus window (XP
    allows it). With the lock lifted (`-nolock`,
    `SPI_SETFOREGROUNDLOCKTIMEOUT` 0) the window is foreground again, but
    Win98's runtime still reports `DEVICELOST` where XP's says
    `DEVICENOTRESET` and resets. Every mode switch reaches the driver as
    asked, so the lost device is above it, in Win98's DirectDraw and
    USER (the tests never see `WM_ACTIVATEAPP` there either). That
    process then keeps DirectDraw's "focus window set" bit, and every
    later full-screen create fails `DDERR_HWNDALREADYSET`: ddraw's
    `SetCooperativeLevel` with `DDSCL_SETFOCUSWINDOW` refuses whenever
    the bit is on (`0xbaad2a6d`), whatever the window. The rig's run
    should show the same. `test_swapchain_parameters`' 12
    in each runtime are its full-screen cases (tests 4 to 6, 13, 14) and
    the same state. `test_getdc`'s 61 (d3d9) are GDI's answers on Win98
    (a top-down DIB's height of -64, the resolution fields, no DC for the
    alpha formats), not the driver's. d3d9 visual's `visual.c:21788` and
    d3d8's `7402` (`test_updatetexture`, a volume update between A8R8G8B8
    and X8R8G8B8) look like Win98's runtime too: no run's log shows a
    `VOLUMEBLT` (the per-process log caps leave that short of proof), so
    it seems to copy volumes itself, where XP's goes through the driver,
    which refuses the format pair. Unconfirmed; the rig will say.
    `device.c:11324` ff. "Expected no format" is `GetPixelFormat` on a
    Win98 window, likely Win98's (the rig will say).
- **Step 5, second pass (2026-09-26).** The regression side first, all on
  Win98 through Microsoft's runtimes: D3DGAME9 and D3DGAME8 frames **0
  pixels** from the native one, D3DFEAT9's **byte-identical** (its
  A16B16G16R16F readback line fails, finding 35), the ten DX8 probes,
  SHTEST, CKTEST and EBTEST pass (PATCHTST not offered, as on XP), and
  DDTEST's four sysmem blits succeed (9x's HEL keeps them with
  `DDCAPS_BLT` in `dwSVBCaps`). Then findings 32 to 35: **every Win98
  test file runs to its end** (d3d9 visual 201827 checks / 1544
  failures, device 57209 / 152, stateblock 14738 / 0; d3d8 visual
  142764 / 156, device 55352 / 60, stateblock 9283 / 0). Most of d3d9
  visual's excess over XP is finding 35 (`test_fog` 280,
  `test_texture_transform_flags` 644, both reading float targets back).
  Saved as the Win98 driver baseline, `reference/winetest/w98-driver.txt`
  (165 keys, only ever shrinks; `WT_BASELINE=w98-driver` checks a run
  against it). `scripts/test.sh guest` / `all` run both on Win98 now
  (`win98-dx9`, `win98-winetest`, after the XP stage, ~5 min under TCG):
  the scenes, probes and DDTEST in one boot (`tools/win98-dx9-test.sh`),
  then the suites against that baseline. `D9CTEST.EXE` (new) creates the resources the tests found failing,
  one line each; `-readback` runs the float readbacks alone,
  `-modechange` the focus-loss case (`-modechange -nolock` with Win98's
  foreground lock off).
- **A modern card, for contrast** (`reference/winetest/win11-rtx3090.txt`,
  the user's Windows 11 PC, RTX 3090, 2026-09-26): d3d9 visual 210814
  checks, 69 failures; device 160756 / 0; d3d8 visual 2; d3d8 device
  crashes at `device.c:9979`, as it does on the guest. Not the oracle
  (Windows 11's runtime is not XP's), but it is the card Wine's tests are
  written against. Against it the guest also fails `test_fog` 6,
  `test_refcount` 16 and `test_desktop_window` 1, which the rig fails
  too, and three more DXVK-shared groups the rig fails:
  `test_texture_transform_flags` 131, `fog_test` 2,
  `test_mvp_software_vertex_shaders` 1.
- **Findings from the DX9 face:**
  6. *Fixed.* `d3d9.dll` registers its textures' system-memory copies
     with no pixel format (a DXT1 one as its block rows' bytes by block
     rows), so every `TEXBLT` from a 16-bit or DXT texture was refused as
     a format mismatch. Such a surface is marked (`SURF.nopf`) and a
     `TEXBLT` from it takes the target's format and size.
  7. *Fixed.* `GetRenderTargetData` failed before reaching the driver:
     a DX9 runtime makes a system-memory offscreen plain surface only in a
     format carrying `D3DFORMAT_OP_OFFSCREENPLAIN`. The RGB formats carry
     it, for the DX9 runtime only (the last `DXVERSION` said 0x9xx).
  8. *Fixed.* The video-memory copy of a mipmapped texture came as one
     surface (`caps 0x10005000`, no MIPMAP) and the host sampled level 0
     only. It is a **lightweight mipmap** (`DDSCAPS3_LIGHTWEIGHTMIPMAP`,
     0x400 in `dwCaps3`): for a texture with a full chain in the default
     pool, or a managed one's video-memory copy, `d3d9.dll` creates one
     surface and the driver keeps every level inside it (the runtime's
     check at 0x4fd20820: all levels, not scratch / system memory, no
     render target / depth / dynamic usage, a HAL device; for the managed
     pool only if the driver manages resources). The runtime never locks
     the sublevels, and its `TEXBLT` brings all of them. The NT
     `DdCreateSurface` sizes the surface for the whole chain
     (`surf_lw_layout`: each level after the last, dword rows, block rows
     for DXT) and `d3d_register_at` hands the host those offsets. The 9x
     HAL does not size one yet (step 5).
  9. *Fixed.* The DX9 blits: `BLT` (StretchRect, `GetRenderTargetData`),
     `SURFACEBLT` (UpdateSurface) and `COLORFILL` run in the driver on
     the surfaces' memory, as `TEXBLT` does: the record ends before one
     that follows other tokens, a video-memory surface the host drew into
     is read back first, and the one written gets `VRAM_DIRTY`. Same-size
     rectangles copy (DXT in whole blocks), StretchRect's scaled ones take
     the nearest texel; formats of one texel size copy as they are.
  10. *Fixed.* Queries. The driver lists `D3DQUERYTYPE_EVENT` and
     `OCCLUSION` (`GETD3DQUERYCOUNT` / `GETD3DQUERY`). The runtime reads a
     result from the **command buffer's start**: after a successful call
     with a nonzero `dwErrorOffset` (DDI version 8 and later), that many
     bytes of `RESPONSEQUERY` blocks (the DP2 command, the block's bytes,
     then {id, size, data} per query; `RESPONSECONTINUE` asks it to call
     again), `d3d9.dll`'s parser at 0x4fd75950. The mingw headers have no
     response structs. An occlusion query is the host's
     (`D3DPT_OP_CREATE_QUERY` / `QUERY_ISSUE` / `QUERY_GET_DATA`, the DLL
     path's); an `ISSUEQUERY` starts a record, and its end waits for the
     count, which the host has once the draws before it ran. The
     responses are written after the whole call is walked, since they
     overwrite the commands.
  11. *Fixed.* The executor refused every vs 1.1 function `d3d9.dll` sent
     (`vertex shader function 0x1 is not valid vs 1.x`): DX9's vs 1.1
     carries `dcl` instructions, which the SM1 validator took from
     `d3d8.dll`'s shaders never had. The draw fell back to the fixed
     function, which got D3DFEAT9's texture and colours right by luck and
     its cube lookup (the normal as `oT1`) black. `sm1_valid` takes `dcl`
     for vs 1.x (a usage token, then an input register);
     `d3dpt-dp2-test` has the case and a hostile one.
  12. *Fixed.* The DX9 formats were not listed, so no float target or
     texture existed (doc 15 "The DX9 formats"). Listed for `d3d9.dll`
     only, after `d3d8.dll`'s list; the sRGB ops and the ARGB group too.
     Wine's `srgbwrite_format_test`, `np2_stretch_rect_test`,
     `color_fill_test` (float, G16R16) and `test_format_conversion`
     (but YUY2) pass since.
  13. *Fixed.* With a float target made, released and replaced, the next
     DP2 call failed as a batch (`BAD_HANDLE`) and every Present after
     it with `D3DERR_DEVICELOST`: the host bound the context's target
     before walking the stream, and the target was gone, its handle
     already reused. `d3d9.dll` sends a SetRenderTarget lazily (with the
     next draw), so a context now forgets a released target and the
     stream's SETRENDERTARGET binds the new one.
  14. *Fixed.* Then every draw of D3DFEAT9's frame came out empty: a
     CreateStateBlock between the float target and the lazy switch back
     captured the 4x4 viewport on the host, and each EXECUTE put it back
     (the occlusion query counted 1 pixel). A DX9 context's viewport now
     stays the stream's across EXECUTE.
  15. *Fixed.* The host device's state is shared by every context: d3d8
     visual's `lighting_test` (`visual.c:640`) failed only after d3d9
     visual had run in the same boot, a light the previous process left
     enabled. The runtime sends a new context its render and stage
     states, not every light, so a new context now gets the device's
     first state with every light off (doc 15). Two of d3d9 visual's own
     failures went with it. Two contexts alive at once still share the
     device's lights.
  16. *Mostly DXVK's.* `test_fog` / `test_texture_transform_flags` on an
     A32B32G32R32F target: with Wine's todo marks counted, 6 of
     `test_fog`'s checks fail beyond DXVK (VS_MODE_FFP, finding 24's
     kind), and the texture transform ones are DXVK's.
  17. *Fixed.* A managed texture's SetLOD sampled its level 0 whatever
     the LOD. `d3d9.dll` sends no SETTEXLOD: it makes the video-memory
     copy again with fewer levels and TEXBLTs the full system-memory chain
     into it, and the driver copied level 0 onto level 0. A TEXBLT now
     matches the levels by size (doc 15 "SetLOD and UpdateTexture");
     Wine's `maxmip_test` passes and `test_updatetexture` went from 18
     failures to 15.
  18. *Fixed.* `test_generate_mipmap` samples the sublevels of a
     render-target texture the application never filled (black on real
     hardware): the host made every render-target texture with one level,
     so level 0 showed through. It makes them with the guest's levels now.
     And a level as a render target (`clear_test`) was a surface of its
     own on the host: protocol v19 links each level's handle to its
     texture (doc 15 "A render-target texture's levels").
  19. *Fixed.* DXT volume textures from `d3d9.dll` came out black: the
     driver's VOLUMEBLT refused DXT, and a source with no pixel format.
     Wine's `volume_dxtn_test` and `volume_srgb_test` pass since.
  20. *Fixed.* A declaration that reads a stream nothing is bound to
     skipped the draw on the host; Direct3D 9 draws it with zeros from
     that stream. Wine's `test_default_diffuse`, `test_color_vertex`,
     `test_pointsize` (an unbound PSIZE stream) and six of
     `test_updatetexture`'s checks pass since: d3d9 visual 657 (707).
  21. *Fixed.* UpdateSurface from a system-memory DXT texture was
     refused: such a surface has no pixel format, its width is bytes per
     block row and its height block rows, and the blit checked a texel
     rectangle against those. Five of `update_surface_test`'s seven pass;
     the other two since finding 25.
  22. *Fixed.* StretchRect between two depth buffers copied VRAM that the
     host never writes depth into. The walker sends such a BLT to the host
     (protocol v18), which runs DXVK's StretchRect; `depth_blit_test`
     passes.
  23. *Fixed.* UpdateTexture into a cube: `d3d9.dll` registers a
     system-memory cube's faces as plain surfaces in the mode's X8R8G8B8
     and updates face by face, the +X face under the cube's root handle.
     The driver refused both (format, and a cube root from a plain
     source). A system-memory source of the target's texel size is the
     target's format now, and a plain source into a cube root is its +X
     face. `test_updatetexture` 2 failures (9).
  24. *Open, a DXVK patch.* Pre-transformed vertices past z = 1:
     without `D3DPMISCCAPS_CLIPTLVERTS` (never claimed, doc 15) the runtime
     clips such geometry to the screen itself and hands the rest on, and
     the cards of the era draw it unclipped in depth (Wine's `z_range_test`
     expects that exactly when the cap is absent; `depth_clamp_test` too).
     DXVK claims the cap itself and always clips depth
     (`BindRasterizerState`: `setDepthClip(true)`). The fix is a patch
     making depth clip follow `D3DRS_CLIPPING`, and the executor turning
     clipping off for an XYZRHW draw. The rig passes all seven, so it
     meets the DXVK rule above; it waits for a user decision. `test_fog`'s
     6 fail on the rig too and are not ours to fix.
  25. *Fixed.* The mip walk stopped early or wandered. A system-memory
     DXT chain with no pixel format is sized in block-row bytes by block
     rows, so its 4x4, 2x2 and 1x1 levels are all one block and the walk,
     which wanted each level smaller, stopped at five; and a cube root
     lists its sibling faces as attached MIPMAP surfaces of the same size.
     `d3dpt_os_next_mip` takes a level no larger than its parent on the
     same cube face; `update_surface_test` passes.
  26. *Fixed.* An indexed draw whose declared vertex range runs one past
     the buffer (`test_sysmem_draw`) was refused; the cards of the era
     draw it. The walker trims the range to what the buffer holds, and the
     host still checks every index against it.
  27. *Fixed.* `CheckDeviceFormat(D3DUSAGE_AUTOGENMIPMAP)` said `D3D_OK`
     for A1R5G5B5 and A4R4G4B4, which the driver cannot render to; real
     drivers say `D3DOK_NOAUTOGEN` (d3d9 `test_mipmap_gen`). The
     autogen op now goes only on formats that are render targets.
  28. *Fixed.* d3d8's `GetInfo` answered `S_OK` for every id:
     `DdGetDriverState` claimed success with nothing written. It fails
     now, which the runtime returns as `S_FALSE` (d3d8 `test_get_info`,
     251 checks).
  29. *Fixed.* A volume `UpdateTexture` from a larger chain (8x8x8 of 4
     levels into 2x2x2 of 2) copied the source's top level into the
     target's: `VOLUMEBLT` now leaves out the source's top levels until
     the sizes match, the box scaled down with them, as `TEXBLT` does
     (`test_updatetexture`'s volume cases 5 and 10, both runtimes).
  30. *Fixed.* The DX9 caps claimed `POW2 | NONPOW2CONDITIONAL`, which
     promises that a non-power-of-two texture is clamped; DXVK repeats it
     (`conditional_np2_repeat_test`). They now claim any size for 2D,
     cube and volume textures, as the rig's GeForce 6 does (the rig skips
     that test), and `test_npot_textures` wants the three alike. The DX7 /
     DX8 faces keep the conditional claim (doc 15 "Texture sizes").
  31. *Fixed, in the executor.* A DX8 vertex shader's scalar instruction
     (`rcp`, `rsq`, `exp`, `log`, `expp`, `logp`) with no source swizzle
     reads `.w`; d3d9, which the host runs it on, reads `.x` unless the
     swizzle replicates. The decoder gives such a source `.wwww`, as
     DXVK's own d3d8 does (d3d8 `test_scalar_instructions`' `logp`).
  32. *Worked around in the harness (Win98).* 3dfx's `3DFX32V2.DLL`,
     installed in `base98-br`, was loaded by d3d8.dll with no card on the
     machine and crashed d3d8 visual and device. `tools/win98-winetest.sh`
     renames it `.OFF` in the raw copy unless `EXTRA` has a voodoo2.
     Whether a player meets it (3dfx's driver installed, the Voodoo
     unticked): the runtimes list one adapter only (`DX8CAPS` / `DX9CAPS`
     print them all now), Max Payne (d3d8, full screen) runs with the DLL
     present, and `D9CTEST -modechange` does too. Only d3d8 visual and
     device crashed in it, so far. Both
     device files then crashed on Wine's 2x4x8 system-memory volume
     (finding 33): `patches/winetest/07` guards that dereference, and
     they run whole.
  33. *Fixed (Win98).* Mipmapped textures of a non-power-of-two size
     (a 10x10 chain in every pool, a 257 managed chain), cube maps of edge
     3 and Wine's 2x4x8 volume of 4 levels failed `D3DERR_DRIVERINTERNALERROR`
     with no driver call. **Win98's DirectDraw refuses them itself** (doc
     19 §45): power-of-two width and height for a `MIPMAP` surface, a
     power-of-two square cube, and a mip count up to log2 of the larger
     of width and height plus one. Found with a gdbstub breakpoint on
     ddraw's error mapper. The DX9 caps claim `POW2 | NONPOW2CONDITIONAL`
     and the cube / volume POW2 flags on 9x (`core.pow2_mips`), so the
     runtime refuses such a texture up front with `D3DERR_INVALIDCALL`
     and titles take their power-of-two path. `test_npot_textures` passes; the price is
     `conditional_np2_repeat_test`'s 2 (finding 30's clamp). The volume's
     mip-count rule has no cap. A real card meets the same rule, so the
     rig's Win98 run should agree.
  34. *Fixed (Win98).* 9x DirectDraw refuses a
     `DDSCAPS_EXECUTEBUFFER` surface in video memory unless the
     HALINFO's `ddsCaps` claims the bit, so the runtime had kept every
     vertex and index buffer in system memory (protocol v9's VRAM buffers
     were NT's only). The 9x HAL claims it, and `CreateExecuteBuffer32`
     takes the size from `dwWidth` when `dwLinearSize` is 0: 9x's runtime
     sends it there (doc 19 §45). Claimed without that, `CreateDevice`
     failed out of video memory. D3DGAME8/9 (0 pixels) and D3DFEAT9
     (byte-identical) draw from VRAM buffers on Win98 now (the device's
     `buffer writes`), the probes, SHTEST, CKTEST, EBTEST and DDTEST pass,
     and the Wine suites match `w98-driver.txt` key for key. Max Payne
     (DX8, hardware T&L) draws its main menu on `base98-br` from VRAM
     buffers at 60 frames/s (about 300 buffer writes and 4800 draws per
     5 s): `CDS=<its ISO> GUEST_CMD=...MAXPAYNE.EXE CLICKS="130:607,160"`
     (Play in v1.0's launcher, which `-skipstartup` does not skip)
     `RUN_SECS=330 tools/win98-game-test.sh`.
  35. *Win98's own.* A system-memory float surface
     (`CreateOffscreenPlainSurface`, A16B16G16R16F / A32B32G32R32F) fails
     `D3DERR_DRIVERINTERNALERROR` with no driver call, so a float render target
     cannot be read back: D3DFEAT9's readback line and most of `test_fog`
     / `test_texture_transform_flags` on Win98. 9x DirectDraw's HEL makes
     a system-memory offscreen plain surface only in the primary's format
     or one of its own 20 (doc 19 §45); the driver has no say, so the
     rig's Win98 should fail the same checks.

- **The suites in a guest** (2026-09-25). Wine 11.0 is the pin: its
  test EXEs import only functions that XP's and Win98's own
  `kernel32`, `user32`, `gdi32` and `msvcrt` export (checked against the
  export tables of `~/vms/winxp.qcow2` and `~/vms/win98.qcow2`), so no
  older tag was needed. `guest-tools/build-winetests.sh` builds
  `d3d8_test.exe`, `d3d9_test.exe`, the runner `wtrun.exe` and the
  rig's `RUNALL.BAT` into `build/winetest/out/`; two patches
  (`patches/winetest/`) fence off Windows 10 and WoW64 code mingw cannot
  compile. `tools/xp-driver-test.sh <image> winetest` runs them;
  `tools/winetest-summary.py` compares.
- **The driver's baseline** (`reference/winetest/xp-driver.txt`, only
  ever shrinks; the DX8 driver on `winxp-m7`, KVM). After the first
  fixes below (2026-09-25):

  | test file | executed | failures | state |
  |---|---|---|---|
  | d3d9 stateblock | 14738 | 182 | ran |
  | d3d9 visual | | 149 | crash in `test_updatetexture` |
  | d3d9 device | | 4 | crash in `test_update_volumetexture` |
  | d3d9 d3d9ex | | | skipped (no `Direct3DCreate9Ex` on XP) |
  | d3d8 stateblock | 9283 | 0 | ran (skipped whole before fix 1) |
  | d3d8 visual | | 146 | crash in `test_updatetexture` |
  | d3d8 device | | 4 | crash after the lost-device test (finding 5) |

  A crash ends a file, so the counts are floors. `DX8CAPS.EXE` /
  `DX9CAPS.EXE` (`xp-driver-test.sh caps`) print what each runtime makes
  of the driver, one line per question, and the caps it reports.
- **Findings, in the order the suite showed them:**
  1. *Fixed.* d3d8 made no windowed device with an A8R8G8B8 back
     buffer on the X8R8G8B8 desktop: A8R8G8B8 lacked
     `D3DFORMAT_OP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET`. Its value is
     **0x100** (`ddk/ddrawint.h`); 0x20 is no op, and with it d3d8.dll
     dropped the HAL entirely (every call `D3DERR_NOTAVAILABLE`, SHTEST
     too). Take op values from the DDK header, never from memory.
  2. *Fixed* (2026-09-26, d3d8; d3d9's went with step 1). d3d8.dll hands
     an `UpdateTexture` from system memory to the driver (a DP2 `TEXBLT`
     / `VOLUMEBLT`) only when `dwSVBCaps` has `DDCAPS_BLT` (its check at
     0x6df5b900 in 5.3.2600.5512; from video memory it wants `DDCAPS_BLT`
     in `dwCaps`). Without it the runtime locks and copies each level
     itself, and that copy indexes past the source's levels when the
     source has fewer than the target (`test_update_volumetexture` case
     5, a NULL read at 0x6df58c12). The driver now claims `DDCAPS_BLT` in
     `dwSVBCaps` alone (doc 15 "Blit caps and the HEL"). What was seen
     before:
     `UpdateTexture` faults inside the runtime, in d3d8.dll and
     d3d9.dll alike, before any `TEXBLT` reaches the driver: the first
     instruction of a runtime routine reads a NULL object. Several
     updates succeed first, so it is one case of Wine's table (formats,
     levels or pools). Recheck after step 1, which changes what the
     runtime is told.
  3. *Fixed.* Cube and volume textures of a non-power-of-two size were
     created where real drivers refuse them: the caps now carry
     `CUBEMAP_POW2` / `VOLUMEMAP_POW2` wherever 2D textures are POW2.
     The DX8 probes still pass.
  4. *The DX9 work itself.* d3d9 stateblock's 182: pixel shader integer
     and boolean constants refused by a runtime that sees ps 1.4.
  5. *Fixed* (2026-09-26). `d3d_core`, the one pointer to the PDEV
     whose Direct3D is on, was cleared by the 640x480 PDEV's
     `DrvDisableSurface`, and the desktop's kept PDEV came back through
     `DrvAssertMode(TRUE)` alone, which never set it again: every context
     call found no PDEV. `DrvAssertMode(TRUE)` now points it back at a
     PDEV with Direct3D on. d3d9 `test_reset` passes, and both device
     files run to their end. What was seen before the fix:
     The lost-device test: a fullscreen device minimized and
     restored fails `Reset` with `D3DERR_INVALIDCALL` (d3d8 device.c:3244),
     and once the desktop is back in its own mode, dxg never calls
     `DrvEnableDirectDraw` on it again (QEMU log: `dd disabled`, then
     GetDriverInfo queries but no `dd enabled`), so later devices fail
     in every process until a reboot. A game's Alt+Tab takes this path.
     The harness runs the `device` files last because of it. 2026-09-26,
     d3d9 `device.c` `test_reset` shows the same: a fullscreen device at
     640x480 Reset to 800x600, the desktop's own mode. Windows gives the
     desktop's kept PDEV back through `DrvAssertMode(TRUE)` (no
     `DrvEnableSurface`, unlike a new PDEV), the 640x480 PDEV's
     DirectDraw is disabled, and from then on the runtime only toggles
     exclusive mode: no `DrvGetDirectDrawInfo`, no `DrvEnableDirectDraw`,
     no surface creation reaches the driver, and Reset returns
     `D3DERR_INVALIDCALL` (the test then crashes on a NULL swapchain).
     The desktop PDEV's DirectDraw is never disabled on the way out
     (`DrvAssertMode(FALSE)` only), and after it is back dxg does call the
     driver (`DdSetExclusiveMode` on and off, three times), but the flip
     chain's creation never reaches `DdCreateSurface` and windowed devices
     in later processes fail too. Other kept PDEVs brought back the same
     way (640x480, 1024x768) keep working. Next: dxg's or the runtime's
     reason, under the QEMU gdbstub (`d3d9.dll`'s Reset path).

## Why SM3 in one step

The shader bytecode passes through the driver untouched and DXVK
compiles every version up to 3.0, so the shader model the driver claims
is a set of caps fields. The work is the DX9 DDI, which 2.0 and 3.0
share. SM3 adds vertex texture fetch, instancing
(`SETSTREAMSOURCEFREQ`) and a few caps on top, and none of it changes
code written for 2.0. So the driver claims 3.0 from the first DX9
build, and a flag caps the claim at 2.0 as the A/B: a title that works
at 2.0 and breaks at 3.0 has its bug in the SM3 part. The reference
rig's GeForce 6200 (doc 09) is an SM3 card, so it answers for 3.0.

## Scope and files

- Guest driver: `guest-tools/src/d3dptvid/core/` (caps, the GDI2
  queries, the DP2 walker), `nt/` and `w9x/` where the two OSes differ,
  the DDI header `core/d3dpt_ddi.h`. `core/` and `w9x/` belong to M10
  and `nt/` to M7 (done); this track edits them for the DX9 DDI and
  names M16 in the commit message.
- Executor: `d3dpt/exec/d3dpt_exec_ddi.cpp` (the driver's half of the
  decoder), `d3dpt/d3dpt_proto.h` (one bump per wire change),
  `tools/d3dpt-dp2-test.cpp` (a section per new record, hostile ones
  included).
- The Wine conformance suites: a build script in `guest-tools/`, a
  runner on the ISO's `TESTS\`, a host-side summary in `tools/`, the
  baselines under `reference/winetest/`, a stage in `scripts/test.sh`.
- Removals (steps 7 and 8): `guest-tools/src/d3dpt/` except `dinput.c`,
  SETUP's file sets 1 and 3, the SysBus `-device d3dpt`
  (`d3dpt/hw/d3dpt_mm.c`, QEMU patch 40) once nothing uses it, the
  device mapper (`MAPPER\`, SETUP component 2) once the ICD no longer
  needs it. `FXMEMMAP.VXD` that 3dfx's own Voodoo 2 driver installs is
  3dfx's and stays.
- Docs: doc 15 (a "DirectX 9 DDI" section), doc 19 (its 9x half),
  doc 14 (the DLL sections shrink to history), `guest-tools/README.md`,
  `docs/testing.md`, this doc.

## The test loop

**Wine's Direct3D test suites are the conformance check** (doc 14
"Reference workloads and conformance"): `dlls/d3d9/tests/` (`visual`,
`device`, `stateblock`, `d3d9ex`) and `dlls/d3d8/tests/` (`visual`,
`device`, `stateblock`). `visual.c` draws and reads back pixels for
fixed function, shaders 1.x to 3.0, fog, sRGB, float targets, multiple
render targets and depth bias; `device.c` checks the API (textures,
surfaces, volumes, queries, declarations, lost devices), `stateblock.c`
the state blocks. Wine writes them to pass on real Windows drivers and marks
where real drivers differ with `broken()`. A failure on our driver is
therefore most likely a driver bug. Run in the guest, they go through
Microsoft's runtime into our DDI, so they test exactly the path this
track builds.

- **The oracle is the rig**: the same binaries on the GeForce 6200 under
  XP and under Win98 give the pass list, per test file.
- **The check** is "nothing fails here that passes on the rig": a
  baseline in `reference/winetest/` is one line per failing check, keyed
  by its source line (stable for one pinned tag), and
  `tools/winetest-summary.py --baseline` exits 1 on a key the baseline
  lacks or one failing more often.
- **The rig's run**: copy `build/winetest/out/` to the rig, run
  `RUNALL.BAT` from inside it under XP and under Win98 (DirectX 9.0c),
  bring back `C:\2KSBOX\WINETEST`, and save each with
  `tools/winetest-summary.py <dir> --save reference/winetest/rig-xp.txt`
  (`rig-98.txt`). The run's own files go in
  `reference/winetest/logs/<name>/` beside it, so the failure text can be
  read and the baseline saved again without the rig. Both saved 2026-09-26. XP's is complete: every file ran
  to its end (d3d9 visual 210849 checks, 1448 failures; device 161316 /
  21; d3d8 visual 142874 / 44; device 57409 / 10). **Win98's is
  partial**: the test EXEs call `EnumDisplaySettingsW` and
  `GetMonitorInfoW`, which Win98 does not implement, so d3d8's files and
  d3d9 device fail device creation and skip nearly everything, and d3d9
  visual crashes at `visual.c:12859` (a failed CreateOffscreenPlainSurface,
  then the test's own NULL dereference). Only d3d9 stateblock (14738, 0)
  is whole. For a useful Win98 oracle the test build now carries
  `guest-tools/src/winetest/w98compat.h` (the Unicode display calls fall
  back to the ANSI ones where Win98 has only a stub), patch 04 (a failed
  `CreateDevice` is traced, to see why d3d8 made no device) and patch 05
  (the guard at 12859), all without moving a source line; XP's run is
  key for key the same. `build/winetest/rig-winetest-98.zip` is the
  build for the rig's next Win98 run (remade 2026-09-26 afternoon with
  patches 06 and 07, waiting on the user; there is no `zip` here, so the
  archive is made with Python's `zipfile` from `build/winetest/out/`).
  The rig's desktop must be at 32 bpp first (d3d8 skips every file on
  16 bpp). That run settles findings 33 and 35, `test_getdc` and the
  full-screen cases on Win98: the guest's differences from XP there look
  like Win98's own.
- **DXVK's own run** says which failures are DXVK's: `tools/winetest-dxvk.sh`
  runs the same EXEs on the host's Wine with DXVK's `d3d9.dll`, and
  `reference/winetest/dxvk-wine.txt` is its baseline. A guest failure
  DXVK shares is not the driver's to fix (the executor's goldens are
  DXVK's too); the rest is, `--by-function` says where.
- **The other oracles stay**: D3DGAME9 / D3DGAME8 / D3DFEAT9 against the
  rig golden and the native DXVK frame (doc 14 P0a), now through
  Microsoft's runtime instead of our DLLs, and the DX8 probes of doc 15
  for the regression side.

## Steps

0. **The suite in a guest.** Done on 2026-09-25 except the rig's two
   runs (State, above). Wine 11.0 loads on XP and 98; no test imports
   D3DX or `d3dcompiler`. The first fixes, before step 1: the three
   driver bugs the first run found, since the crashes hide everything
   after them in a file.
1. *Done 2026-09-25.* **The DX9 face.** `GetDriverInfo2` answers `DXVERSION` 0x900, the DDI
   version query, `GETD3DCAPS9` (a `D3DCAPS9` with vs/ps 3.0, the
   `VS20Caps` / `PS20Caps` and instruction-slot fields), the format list
   with the DX9 `D3DFORMAT_OP_*` bits, the query types and the
   multisample quality levels. The DX8 path stays as it is behind a
   flag, as `DDF_NO_DX8` keeps the DX7 face. Two new flags are needed
   (the DX8 face, and 2.0 as the cap), and `ddflags` has two bits left
   (0x40000000, 0x80000000); decide here whether they take the last two
   or a second property starts. Expect `d3d9.dll` to check the answers
   the way `d3d8.dll` did (doc 15: `dwActualSize`, the HAL-info flag),
   and read its disassembly before guessing. Done when D3DGAME9 fixed
   function runs through Microsoft's `d3d9.dll` on the driver and
   matches the native frame.
2. **The DX9 tokens.** What the DP2 stream carries for a DX9 driver:
   vertex declarations (`D3DVERTEXELEMENT9`) and their create / set /
   delete, shader functions for vs/ps 2.0 and 3.0, integer and boolean
   constants, the scissor rect, `SETSTREAMSOURCE2` with its stride,
   render target and depth-stencil binds (`SETRENDERTARGET2`, several
   targets), queries (create / issue / response), the blits and colour
   fill, `GENERATEMIPSUBLEVELS`, the DX9 sampler states (sRGB among
   them). Each is a record in `d3dpt_proto.h` (bump per change), a
   decoder case in `d3dpt_exec_ddi.cpp` that validates it, and a
   `d3dpt-dp2-test` section. Done when D3DFEAT9 through Microsoft's
   runtime is byte-identical to the native DXVK run.
3. **SM3 and the DX9 formats.** Vertex texture fetch (the four vertex
   samplers, the vertex-texture format op), instancing
   (`SETSTREAMSOURCEFREQ`), float textures and targets (R16F to
   A32B32G32R32F) with the blending and filtering ops DXVK really has,
   sRGB read and write, multiple render targets, the depth-bias and
   stretch-filter caps. The 2.0-cap flag lands here.
4. **XP against the rig.** The Wine suites on XP down to the rig's
   failures, file by file. A `dx9` stage in `scripts/test.sh` runs them
   from an XP snapshot (it may be the `driver` stage M7 still owes).
   The DX8 probes and D3DGAME8 through `d3d8.dll` must not move.
5. **Win98.** The same `core/` answers the same queries through the 9x
   HAL (doc 19 §4: `GetDriverInfo2` reaches a 9x driver). DirectX 9.0c
   is Win98's last runtime and the one to test. The suites on Win98
   against the rig's Win98 run.
6. **Titles.** DX9 titles of the era, SM2 and SM3, on XP and Win98, with
   the user by hand. Each gets a row in the table below as it is tried.
7. **Retire the Direct3D DLLs.** `D3D8.DLL`, `D3D9.DLL` and `DDRAW.DLL`
   leave the ISO with their source, generators and SETUP's file set 1
   (the set numbers after it do not move). The guest checks in
   `scripts/test.sh` that ran D3DGAME9 / D3DGAME8 / D3DFEAT9 through
   the DLLs run them on `d3dpt-vga` through Microsoft's runtime. Vice
   City's 256 MB video-memory question, which `DDRAW.DLL` answered,
   gets the driver's own answer. The SysBus `-device d3dpt` (patch 40)
   has no user left and goes. `D3DPT\` keeps `DINPUT.DLL` alone; the
   folder's name is settled then.
8. **The OpenGL ICD.** qemu-3dfx's GL wrapper, today `OPENGL32.DLL`
   copied beside a game, becomes an installable client driver
   registered under `OpenGLDrivers` by both INFs, so a game loads
   Windows' own `opengl32.dll`, which loads the ICD. The ICD reaches
   `hw/mesa` through the display driver (an escape that maps the
   window) rather than the mapper. Open until then: whether it is a
   patch on qemu-3dfx's wrapper or our own ICD around its code, and
   `WRAPGL32.EXT`'s per-title extension cap in a world with no file
   beside the game. The check is `GLPROBE`, `wglgears`, Wine's
   `opengl32` tests (WGL and pixel formats), GLQuake and a Quake III
   timedemo. Then `OPENGL\`, SETUP's set 3, the mapper component and
   `FXPTL.SYS` go.

## Traps known before starting

- `ddflags` is full after step 1 (above).
- Every protocol bump makes the executor and the ISO stale silently
  (`protocol mismatch`); `scripts/build.sh` rebuilds both.
- **Never claim `D3DPMISCCAPS_CLIPTLVERTS`** (doc 15): the DX9 caps
  inherit the DX8 rule.
- The DX8 runtime's filter numbering (doc 15 "Filter numbering") is
  per interface version; `d3d9.dll`'s `dwhContext` value has to be read
  from a real context create before the rewrite can be trusted.
- The executor's `system` and Wine backends are second rasterisers
  (doc 14): goldens stay DXVK's, and both oracles run on all three
  whenever the decoder changes.

## Titles, by feature

Filled in as step 6 runs. A title gets a row when it has been tried.

| Title | API / SM | OS | Result |
|---|---|---|---|
