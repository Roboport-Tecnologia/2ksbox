//! The picture (doc 03): the guest's frame (uploaded, or a zero-copy
//! slot), the CRT chain, the geometry stage, and the blit into whatever
//! surface the front end has. Nothing here knows the window: the front end
//! makes the surface (`Gpu::new` takes any wgpu surface target, a
//! mitsuami `GpuSurface`'s handle), presents what `render`
//! drew, and applies the minimum size `take_min_size` asks for.

use crate::{mode, qemu_vm, shot_path};

#[cfg(target_os = "linux")]
use crate::dmabuf;
#[cfg(target_os = "macos")]
use crate::iosurface;

type Shown = (wgpu::Texture, wgpu::BindGroup, u32, u32);

/// The smallest window the picture wants (`Gpu::take_min_size`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum MinSize {
    /// The 1x picture at its displayed size, in the surface's own pixels.
    /// The front end caps it at the screen.
    Physical(u32, u32),
    /// A usable desktop, in points: the guest takes the window's size as
    /// its mode, whatever it is.
    Logical(u32, u32),
}

pub struct Gpu {
    surface: wgpu::Surface<'static>,
    pub(crate) device: wgpu::Device,
    pub(crate) queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    bgl: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    pipeline: wgpu::RenderPipeline,
    /// `pipeline` blending into a byte (non-sRGB) BGRA8 target: the guest
    /// cursor's sprite over a ring slot's frame, as `composite_cursor`
    /// blends it on the CPU
    cursor_pipeline: wgpu::RenderPipeline,
    /// a ring slot's frame with the cursor sprite blended in (and that
    /// texture's byte view to draw the sprite through), on show instead of
    /// the slot while `comp_on`; the sprite's texture
    comp: Option<(Shown, wgpu::TextureView)>,
    comp_on: bool,
    sprite: Option<Shown>,
    fb_tex: Option<Shown>,
    /// `fb_tex` does not hold the published frame (new): the next upload
    /// is whole, not what changed
    fb_whole: bool,
    /// where `fb_tex` holds the cursor sprite drawn over the frame, put
    /// back from the frame at the next take; and the sprite's pixels,
    /// blended there (a buffer kept, not one a frame)
    fb_sprite: Option<qemu_vm::Rect>,
    fb_sprite_px: Vec<u32>,
    /// the sprite last drawn into `fb_tex` (shape, hot spot): a take that
    /// changed nothing under it and moved nothing leaves it where it is
    fb_sprite_last: Option<(std::sync::Arc<qemu_vm::CursorImage>, i32, i32)>,
    /// zero-copy 3D frames: imported dma-buf ring slots and the one on show
    ext: Vec<Option<Shown>>,
    ext_current: Option<usize>,
    /// scanout ring slots taken off show whose last draw the GPU has
    /// finished since (bit n = slot n), for the device to have back (M26)
    slots_done: std::sync::Arc<std::sync::atomic::AtomicU32>,
    zero_copy: bool,
    adapter_info: wgpu::AdapterInfo,
    pub(crate) chain: Option<shader_chain::Chain>,
    chain_bg: Option<(wgpu::BindGroup, u32, u32)>,
    /// What the chain draws from may differ from its last run: the texture
    /// on show was written or swapped, or the preset, its parameters or
    /// the fit changed. An idle tick wakes the render thread without any
    /// of these, and an unanimated preset's last output is then drawn
    /// again instead of the chain run (`render`). Set whenever in doubt.
    chain_stale: bool,
    /// mode analysis of the guest surface on show (doc 03 rules 2 and 3)
    mode: mode::Mode,
    /// Where the picture goes in the host surface, in physical pixels
    /// (x, y, w, h). The geometry stage's answer, held rather than derived:
    /// see `guest_surface_changed`.
    geom: (f32, f32, f32, f32),
    /// the loaded preset has no parameter to carry a scanline count: said once
    warned_no_scanline_params: bool,
    /// the mode sweep renders to a fixed surface size instead of the window's,
    /// so what it checks does not depend on what the compositor handed us
    forced_surface: Option<(u32, u32)>,
    /// The adapter on show takes the window's size as its mode (virtio-gpu
    /// with Windows' viogpudo, M20), so the window is not held to the mode.
    follows_window: bool,
    /// The minimum size changed since the front end last took it.
    min_size_changed: bool,
    /// A whole scale the picture is held to, in physical pixels per
    /// scanline (the player's View > Scale), or `None` for the largest
    /// that fits. A surface too small for it crops the picture (`fit`).
    fixed_scale: Option<u32>,
}

impl Gpu {
    /// A device for `target`'s surface, configured at `size` (physical
    /// pixels).
    pub fn new(target: impl Into<wgpu::SurfaceTarget<'static>>, size: (u32, u32)) -> Self {
        Self::with_backends(target, size, None)
    }

    /// As [`Gpu::new`], on `backends` unless `WGPU_BACKEND` names others:
    /// a front end whose surface only one backend presents to says so
    /// (player-mitsuami on Windows, a child window Vulkan's frames never
    /// reach the screen through).
    pub fn with_backends(
        target: impl Into<wgpu::SurfaceTarget<'static>>,
        size: (u32, u32),
        backends: Option<wgpu::Backends>,
    ) -> Self {
        let mut instance_desc = wgpu::InstanceDescriptor::new_without_display_handle_from_env();
        if let (Some(backends), None) = (backends, std::env::var_os("WGPU_BACKEND")) {
            instance_desc.backends = backends;
        }
        let instance = wgpu::Instance::new(instance_desc);
        let surface = instance.create_surface(target).expect("create surface");
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            ..Default::default()
        }))
        .expect("no suitable GPU adapter");
        let desc = wgpu::DeviceDescriptor {
            label: Some("player"),
            // What the CRT chain needs of this adapter: clamp-to-border
            // sampling, without which librashader quietly samples
            // clamp-to-edge and a curved preset smears its outermost
            // pixels across everything outside the tube
            // (`shader_chain::required_features`).
            required_features: shader_chain::required_features(&adapter),
            ..Default::default()
        };
        // Linux: open the device with the dma-buf import extensions so 3D
        // frames can be sampled straight from the backend's buffers.
        #[cfg(target_os = "linux")]
        let opened = dmabuf::create_device(&adapter, &desc);
        #[cfg(not(target_os = "linux"))]
        let opened: Option<(wgpu::Device, wgpu::Queue, bool)> = None;
        let (device, queue, zero_copy) = match opened {
            Some(t) => t,
            None => {
                let (d, q) = pollster::block_on(adapter.request_device(&desc)).expect("request device");
                // macOS: IOSurface-backed Metal textures need no extensions
                (d, q, cfg!(target_os = "macos"))
            }
        };
        // `PLAYER_ZERO_COPY=0` refuses every slot the backend offers, so it
        // falls back to reading each frame back (doc 12 §4). The A/B that
        // separates a fault in the ring from one in what the guest drew.
        let zero_copy = zero_copy && std::env::var("PLAYER_ZERO_COPY").as_deref() != Ok("0");
        if zero_copy {
            eprintln!("[3d] zero-copy dma-buf import available");
        } else {
            eprintln!("[3d] zero-copy off: frames are read back");
        }
        // The CRT chain's border sampling, said out loud once. Without
        // it librashader samples clamp-to-edge (see
        // `shader_chain::required_features`) and a curved preset smears
        // its outermost pixels over everything outside the tube; whether
        // that is this adapter's limit or our own descriptor having lost
        // the feature is the difference worth printing, since the
        // picture looks the same either way.
        let border = wgpu::Features::ADDRESS_MODE_CLAMP_TO_BORDER;
        match (device.features().contains(border), adapter.features().contains(border)) {
            (true, _) => eprintln!("[shader] clamp-to-border sampling: on"),
            (false, true) => eprintln!(
                "[shader] clamp-to-border sampling: off although this adapter has it \
                 — curved presets will smear their edge pixels"
            ),
            (false, false) => eprintln!(
                "[shader] clamp-to-border sampling: off, this adapter has none \
                 — curved presets will smear their edge pixels"
            ),
        }

        let mut config = surface
            .get_default_config(&adapter, size.0.max(1), size.1.max(1))
            .expect("surface unsupported by adapter");
        let caps = surface.get_capabilities(&adapter);
        if caps.present_modes.contains(&wgpu::PresentMode::Mailbox) {
            config.present_mode = wgpu::PresentMode::Mailbox;
        }
        // Two on Windows: at one, Direct3D 12's `get_current_texture` waited
        // out a whole vblank whenever the guest drew at 60 Hz, 16.5 ms of
        // every frame (then on player-mitsuami's UI thread, so the pointer,
        // the menus and Close lagged; user, 2026-10-16). At two the draw is
        // 2.5 ms and publish→present p50 went from ~8 to ~1 ms.
        config.desired_maximum_frame_latency = if cfg!(windows) { 2 } else { 1 };
        surface.configure(&device, &config);

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("nearest"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("blit bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("blit"),
            source: wgpu::ShaderSource::Wgsl(include_str!("blit.wgsl").into()),
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("blit layout"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        // one shader, two pipelines: the picture, and an image blended
        // over it
        let make_pipeline = |label: &str, format: wgpu::TextureFormat, blend: Option<wgpu::BlendState>| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: Default::default(),
                depth_stencil: None,
                multisample: Default::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_main"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let pipeline = make_pipeline("blit", config.format, None);
        let cursor_pipeline = make_pipeline("cursor", wgpu::TextureFormat::Bgra8Unorm, Some(wgpu::BlendState::ALPHA_BLENDING));

        let adapter_info = adapter.get_info();
        Self {
            surface,
            device,
            queue,
            config,
            bgl,
            sampler,
            pipeline,
            cursor_pipeline,
            comp: None,
            comp_on: false,
            sprite: None,
            fb_tex: None,
            fb_whole: true,
            fb_sprite: None,
            fb_sprite_px: Vec::new(),
            fb_sprite_last: None,
            ext: Vec::new(),
            ext_current: None,
            slots_done: Default::default(),
            zero_copy,
            adapter_info,
            chain: None,
            chain_bg: None,
            chain_stale: true,
            mode: mode::Mode::analyse(0, 0),
            geom: (0.0, 0.0, 1.0, 1.0),
            warned_no_scanline_params: false,
            forced_surface: None,
            follows_window: false,
            min_size_changed: false,
            fixed_scale: None,
        }
    }

    pub fn load_shader(&mut self, path: &std::path::Path, params: &[(String, f32)]) {
        if let Err(e) = self.set_shader(Some((path, params))) {
            eprintln!("[shader] failed to load {}: {e}", path.display());
        }
    }

    /// The CRT chain swapped for `preset` with `params`, or taken away
    /// (`None`): the mitsuami player's Shader menu (track M22). A preset
    /// that fails to load leaves the chain as it was. The new chain gets
    /// the mode's parameters at once, as a first one does with the first
    /// frame.
    pub fn set_shader(&mut self, shader: Option<(&std::path::Path, &[(String, f32)])>) -> Result<(), String> {
        let chain = match shader {
            None => None,
            Some((path, params)) => {
                let c = shader_chain::Chain::load(path, &self.device, &self.queue, self.adapter_info.clone(), self.config.format)
                    .map_err(|e| e.to_string())?;
                c.set_parameters(params);
                eprintln!("[shader] loaded {}", path.display());
                Some(c)
            }
        };
        if chain.is_none() && self.chain.is_some() {
            eprintln!("[shader] none");
        }
        self.chain = chain;
        // the old chain's output, which a bind group of the same size
        // would otherwise go on drawing
        self.chain_bg = None;
        self.chain_stale = true;
        self.warned_no_scanline_params = false;
        self.mode = mode::Mode::analyse(0, 0);
        self.guest_surface_changed();
        Ok(())
    }

    /// Whether the device imports the backend's 3D frames (dma-buf or
    /// IOSurface) rather than having them read back.
    pub fn zero_copy(&self) -> bool {
        self.zero_copy
    }

    /// The host surface's size, as last configured (physical pixels).
    pub fn surface_px(&self) -> (u32, u32) {
        (self.config.width, self.config.height)
    }

    /// The headless sweep or calibration owns the surface's size.
    pub fn is_forced(&self) -> bool {
        self.forced_surface.is_some()
    }

    /// Whether the adapter on show takes the window's size, as the embed
    /// library says on each wake; the window's floor follows it.
    pub fn set_follows_window(&mut self, on: bool) {
        if self.follows_window != on {
            self.follows_window = on;
            self.geom = self.fit();
            self.chain_stale = true;
            eprintln!("[display] the guest {} the window's size", if on { "takes" } else { "no longer takes" });
            self.min_size_changed = true;
        }
    }

    /// Whether the guest takes the window's size (`set_follows_window`):
    /// then a scale or a window fitted to the picture means nothing, since
    /// the picture is the window.
    pub fn follows_window(&self) -> bool {
        self.follows_window
    }

    /// Hold the picture at a whole scale (physical pixels per scanline),
    /// or `None` to give it the largest that fits, as always.
    pub fn set_fixed_scale(&mut self, scale: Option<u32>) {
        let scale = scale.map(|s| s.max(1));
        if self.fixed_scale != scale {
            self.fixed_scale = scale;
            self.geom = self.fit();
            self.chain_stale = true;
        }
    }

    /// The picture's size with no bars around it, in physical pixels: at
    /// the scale it is held to, or at the one on show when it is not held.
    /// What "Fit Window to Picture" sizes the window to. `None` before the
    /// guest has a surface, and while the guest takes the window's size.
    pub fn picture_px(&self) -> Option<(u32, u32)> {
        let m = self.mode;
        if m.scanlines == 0 || self.follows_window || self.forced_surface.is_some() {
            return None;
        }
        let gh = m.scanlines as f32;
        let shown = (self.geom.3 / gh).floor().max(1.0) as u32;
        let scale = self.fixed_scale.unwrap_or(shown) as f32;
        Some(((gh * scale * m.display_aspect).round() as u32, (gh * scale) as u32))
    }

    /// The host surface changed: the picture's own size did not, so the
    /// mode analysis stands and only the fit is redone.
    pub fn resize(&mut self, w: u32, h: u32) {
        self.config.width = w.max(1);
        self.config.height = h.max(1);
        self.surface.configure(&self.device, &self.config);
        self.geom = self.fit();
        self.chain_stale = true;
    }

    /// (Re)create the guest framebuffer texture when its size changes.
    fn ensure_texture(&mut self, w: u32, h: u32) {
        if matches!(&self.fb_tex, Some((_, _, tw, th)) if *tw == w && *th == h) {
            return;
        }
        // XRGB8888 little-endian == BGRA8 byte order: upload as-is.
        // Guest pixels are sRGB-encoded: tag the texture sRGB when the swapchain
        // is sRGB (macOS default) so sampling decodes and presenting re-encodes;
        // on a linear swapchain (Linux default) pass values through unchanged.
        let format = if self.config.format.is_srgb() {
            wgpu::TextureFormat::Bgra8UnormSrgb
        } else {
            wgpu::TextureFormat::Bgra8Unorm
        };
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("guest framebuffer"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            // COPY_SRC so Ctrl+Alt+S can read the guest's own frame back
            usage: wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_DST
                | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let bg = self.make_bind_group(&tex);
        self.fb_tex = Some((tex, bg, w, h));
        self.fb_whole = true;
        self.fb_sprite = None;
        self.fb_sprite_last = None;
        self.chain_stale = true;
        if self.ext_current.is_none() {
            self.guest_surface_changed();
        }
    }

    fn make_bind_group(&self, tex: &wgpu::Texture) -> wgpu::BindGroup {
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("blit bg"),
            layout: &self.bgl,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
            ],
        })
    }

    /// Import a backend ring slot: a dma-buf (Linux, takes the fd) or an
    /// IOSurface (macOS).
    pub fn import_slot(&mut self, d: &qemu_vm::DmaBuf) {
        let srgb = self.config.format.is_srgb();
        let (slot, w, h) = (d.slot, d.w, d.h);
        // a scanout ring's image (M26) is ours already: nothing to import
        let r = match &d.tex {
            Some(tex) => Ok(tex.clone()),
            None => self.import_backend_slot(d, srgb),
        };
        match r {
            Ok(tex) => {
                let bg = self.make_bind_group(&tex);
                if self.ext.len() <= slot {
                    self.ext.resize_with(slot + 1, || None);
                }
                self.ext[slot] = Some((tex, bg, w, h));
                self.chain_stale = true;
                eprintln!("[3d] slot {slot}: imported {w}x{h}");
                if self.ext_current == Some(slot) {
                    self.guest_surface_changed();
                }
            }
            Err(e) => {
                eprintln!("[3d] slot {slot}: zero-copy import failed: {e}");
                if self.ext.len() > slot {
                    self.ext[slot] = None;
                }
            }
        }
    }

    /// The backend's buffer behind a ring slot, as a texture.
    fn import_backend_slot(&self, d: &qemu_vm::DmaBuf, srgb: bool) -> Result<wgpu::Texture, String> {
        let (w, h) = (d.w, d.h);
        #[cfg(target_os = "linux")]
        let r = dmabuf::import(&self.device, d.fd, w, h, d.stride, d.fourcc, d.modifier, srgb);
        #[cfg(target_os = "macos")]
        let r = iosurface::import(&self.device, d.iosurface as *mut std::ffi::c_void, w, h, srgb);
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        let r: Result<wgpu::Texture, String> = {
            let _ = (srgb, w, h);
            if d.fd >= 0 {
                unsafe { libc::close(d.fd) };
            }
            Err("no zero-copy import on this platform".into())
        };
        r
    }

    /// What makes a scanout ring's images on this device (track M26):
    /// Direct3D 12 on Windows, Metal on macOS, Vulkan with opaque fds on
    /// Linux, unless `PLAYER_ZERO_COPY=0` (the A/B). None elsewhere, and
    /// devices keep their surfaces.
    pub fn scanout_ring(&self) -> Option<qemu_vm::RingAlloc> {
        if std::env::var("PLAYER_ZERO_COPY").as_deref() == Ok("0") {
            return None;
        }
        #[cfg(windows)]
        {
            unsafe { self.device.as_hal::<wgpu::hal::api::Dx12>() }?;
            let device = self.device.clone();
            let srgb = self.config.format.is_srgb();
            Some(std::sync::Arc::new(move |w, h, n| crate::d3d12ring::alloc(&device, w, h, n, srgb)))
        }
        #[cfg(target_os = "macos")]
        {
            unsafe { self.device.as_hal::<wgpu::hal::api::Metal>() }?;
            let device = self.device.clone();
            let srgb = self.config.format.is_srgb();
            Some(std::sync::Arc::new(move |w, h, n| crate::metalring::alloc(&device, w, h, n, srgb)))
        }
        #[cfg(target_os = "linux")]
        {
            if !crate::vkring::available(&self.device) {
                return None;
            }
            let device = self.device.clone();
            let srgb = self.config.format.is_srgb();
            Some(std::sync::Arc::new(move |w, h, n| crate::vkring::alloc(&device, w, h, n, srgb)))
        }
        #[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
        None
    }

    /// Show an imported slot (Some) or the CPU-uploaded framebuffer (None).
    fn use_slot(&mut self, slot: Option<usize>) {
        let before = self.shown_size();
        let was = (self.ext_current, self.comp_on);
        self.comp_on = false;
        self.ext_current = match slot {
            Some(s) if self.ext.get(s).map(|e| e.is_some()).unwrap_or(false) => Some(s),
            _ => None,
        };
        // another texture on show, or the same slot with a new frame in it
        // (a ring slot index comes round again with new content)
        if (self.ext_current, self.comp_on) != was || self.ext_current.is_some() {
            self.chain_stale = true;
        }
        // a 3D frame and the VGA surface need not be the same size: the
        // picture just changed, even though neither texture did
        if self.shown_size() != before {
            self.guest_surface_changed();
        }
    }

    /// A slot just taken off show goes back to the device once the GPU
    /// has finished everything submitted so far, the last draws that
    /// sampled it among them (M26).
    fn release_after_gpu(&self, slot: usize) {
        let done = self.slots_done.clone();
        let bit = qemu_vm::slot_bit(slot);
        self.queue.on_submitted_work_done(move || {
            done.fetch_or(bit, std::sync::atomic::Ordering::AcqRel);
        });
    }

    /// The guest's cursor over the ring slot on show, where the host
    /// cursor cannot stand in for it (a relative mouse, a grab): the slot
    /// copied on the GPU into a texture of ours, the sprite's part inside
    /// the frame blended into that through its byte view (the CPU path's
    /// blend, `composite_cursor`), and the copy shown instead of the slot
    /// until the next frame. The slot itself is never written: the device
    /// owns what is in it. A frame in another format (an imported dma-buf
    /// that is not BGRA) shows without the sprite, as before.
    fn composite_slot_cursor(&mut self, c: &qemu_vm::CursorImage, x: i32, y: i32) {
        let Some(slot) = self.ext_current else { return };
        let Some((src, _, w, h)) = self.ext[slot].as_ref() else { return };
        let (src, w, h) = (src.clone(), *w, *h);
        let format = src.format();
        let bytes = format.remove_srgb_suffix();
        if bytes != wgpu::TextureFormat::Bgra8Unorm {
            return;
        }
        // the sprite's part inside the frame
        let (x0, y0) = (x - c.hot_x as i32, y - c.hot_y as i32);
        let (cx0, cy0) = (x0.max(0), y0.max(0));
        let cx1 = (x0 + c.width as i32).min(w as i32);
        let cy1 = (y0 + c.height as i32).min(h as i32);
        if cx0 >= cx1 || cy0 >= cy1 {
            return;
        }
        let (sw, sh) = ((cx1 - cx0) as u32, (cy1 - cy0) as u32);
        let mut px = Vec::with_capacity((sw * sh) as usize);
        for yy in cy0..cy1 {
            let row = (yy - y0) as usize * c.width as usize;
            px.extend_from_slice(&c.argb[row + (cx0 - x0) as usize..row + (cx1 - x0) as usize]);
        }
        let size = |w, h| wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 };
        if !matches!(&self.comp, Some(((t, _, cw, ch), _)) if *cw == w && *ch == h && t.format() == format) {
            let view_formats = [bytes];
            let tex = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("slot with cursor"),
                size: size(w, h),
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format,
                // COPY_SRC: Ctrl+Alt+S reads the frame on show back
                usage: wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_DST
                    | wgpu::TextureUsages::COPY_SRC
                    | wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: if bytes != format { &view_formats } else { &[] },
            });
            let bg = self.make_bind_group(&tex);
            let draw = tex.create_view(&wgpu::TextureViewDescriptor {
                format: Some(bytes),
                ..Default::default()
            });
            self.comp = Some(((tex, bg, w, h), draw));
        }
        if !matches!(&self.sprite, Some((_, _, pw, ph)) if *pw == sw && *ph == sh) {
            let tex = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("cursor sprite"),
                size: size(sw, sh),
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                // 0xAARRGGBB little-endian: B, G, R, A, straight alpha
                format: wgpu::TextureFormat::Bgra8Unorm,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let bg = self.make_bind_group(&tex);
            self.sprite = Some((tex, bg, sw, sh));
        }
        let ((comp, _, _, _), draw) = self.comp.as_ref().unwrap();
        let (sprite, sprite_bg, _, _) = self.sprite.as_ref().unwrap();
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: sprite,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(&px),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(sw * 4),
                rows_per_image: Some(sh),
            },
            size(sw, sh),
        );
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("slot with cursor"),
        });
        enc.copy_texture_to_texture(src.as_image_copy(), comp.as_image_copy(), size(w, h));
        {
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("cursor sprite"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: draw,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Store,
                    },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_viewport(cx0 as f32, cy0 as f32, sw as f32, sh as f32, 0.0, 1.0);
            pass.set_pipeline(&self.cursor_pipeline);
            pass.set_bind_group(0, sprite_bg, &[]);
            pass.draw(0..3, 0..1);
        }
        self.queue.submit(Some(enc.finish()));
        self.comp_on = true;
        self.chain_stale = true;
    }

    /// The size of the texture on show, if there is one.
    pub fn shown_size(&self) -> Option<(u32, u32)> {
        self.current().map(|(_, _, w, h)| (*w, *h))
    }

    /// A guest frame (or a test picture) has been put on show.
    pub fn has_frame(&self) -> bool {
        self.current().is_some()
    }

    /// The texture currently on show: an imported 3D slot or the upload.
    fn current(&self) -> Option<&Shown> {
        match self.ext_current {
            Some(_) if self.comp_on => self.comp.as_ref().map(|(c, _)| c),
            Some(s) => self.ext[s].as_ref(),
            None => self.fb_tex.as_ref(),
        }
    }

    /// Write the guest's own frame out as a PNG: the texture QEMU
    /// published, at the mode's own size, before the geometry stage
    /// stretched it and before the CRT chain drew on it. A shot of what
    /// the machine rendered, not of what the window shows. Ctrl+Alt+S.
    ///
    /// The imported 3D slot is shot the same way when one is on show, so
    /// this is the guest's frame on every path, zero-copy included.
    /// Returns where it went.
    pub fn screenshot(&self) -> Option<std::path::PathBuf> {
        let Some((tex, _, _, _)) = self.current() else {
            eprintln!("[shot] no guest frame yet");
            return None;
        };
        let (w, h, rgb) = shader_chain::read_texture(&self.device, &self.queue, tex);
        let path = shot_path()?;
        if let Err(e) = shader_chain::write_png(&path.to_string_lossy(), w, h, &rgb) {
            eprintln!("[shot] {}: {e}", path.display());
            return None;
        }
        eprintln!("[shot] {w}x{h} guest frame → {}", path.display());
        Some(path)
    }

    /// The window's picture into `view`: the chain's output (or the guest
    /// frame when no preset is loaded) in the geometry stage's viewport,
    /// black around it. `render` and `window_shot` both draw with this, so
    /// the shot is the presented picture pixel for pixel.
    fn draw_picture(&self, encoder: &mut wgpu::CommandEncoder, view: &wgpu::TextureView) {
        let bg: &wgpu::BindGroup = match (&self.chain, &self.chain_bg) {
            (Some(_), Some((bg, _, _))) => bg,
            _ => &self.current().unwrap().1,
        };
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("blit"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
                depth_slice: None,
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        let (x, y, w, h) = self.viewport();
        pass.set_viewport(x, y, w, h, 0.0, 1.0);
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, bg, &[]);
        pass.draw(0..3, 0..1);
    }

    /// Write what the window shows as a PNG: the picture after the geometry
    /// stage and the CRT chain, at the window's own size, black bars
    /// included. The chain's last output is drawn again rather than the
    /// chain run again, so an animated preset is not stepped by a shot.
    /// Ctrl+Alt+Shift+S; `screenshot` is the guest's own frame. Returns
    /// where it went.
    pub fn window_shot(&self) -> Option<std::path::PathBuf> {
        if self.current().is_none() {
            eprintln!("[shot] no guest frame yet");
            return None;
        }
        let (w, h) = self.surface_size();
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("window shot"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            // the swapchain's format, so the pipeline and its sRGB encoding
            // are the ones the window gets
            format: self.config.format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("window shot"),
            });
        self.draw_picture(&mut encoder, &view);
        self.queue.submit(Some(encoder.finish()));
        let (w, h, rgb) = shader_chain::read_texture(&self.device, &self.queue, &tex);
        let path = shot_path()?;
        if let Err(e) = shader_chain::write_png(&path.to_string_lossy(), w, h, &rgb) {
            eprintln!("[shot] {}: {e}", path.display());
            return None;
        }
        eprintln!("[shot] {w}x{h} window → {}", path.display());
        Some(path)
    }

    pub fn upload(&mut self, pixels: &[u32], w: u32, h: u32) {
        self.upload_rect(pixels, w, h, qemu_vm::Rect::whole(w as usize, h as usize));
    }

    /// Upload `r` of a `w`x`h` frame (M22): what changed since the last
    /// upload, read straight from the published pixels.
    pub fn upload_rect(&mut self, pixels: &[u32], w: u32, h: u32, r: qemu_vm::Rect) {
        self.ensure_texture(w, h);
        self.fb_whole = false;
        if r.x0 >= r.x1 || r.y0 >= r.y1 {
            return;
        }
        // the data from the rect's first pixel on, rows `w` apart
        let first = r.y0 * w as usize + r.x0;
        let last = (r.y1 - 1) * w as usize + r.x1;
        self.write_fb(&pixels[first..last], w, r);
    }

    /// The guest's cursor sprite over the frame just uploaded (a relative
    /// mouse, a grab): only its rect is blended and uploaded, and put back
    /// from the frame at the next take (it was the whole frame copied,
    /// blended and uploaded, twice, every frame).
    pub fn upload_sprite(&mut self, pixels: &[u32], w: usize, h: usize, c: &qemu_vm::CursorImage, x: i32, y: i32) {
        let x0 = (x - c.hot_x as i32).clamp(0, w as i32) as usize;
        let y0 = (y - c.hot_y as i32).clamp(0, h as i32) as usize;
        let x1 = (x - c.hot_x as i32 + c.width as i32).clamp(0, w as i32) as usize;
        let y1 = (y - c.hot_y as i32 + c.height as i32).clamp(0, h as i32) as usize;
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        let r = qemu_vm::Rect { x0, y0, x1, y1 };
        let (rw, rh) = (x1 - x0, y1 - y0);
        let mut px = std::mem::take(&mut self.fb_sprite_px);
        px.clear();
        for row in y0..y1 {
            px.extend_from_slice(&pixels[row * w + x0..row * w + x1]);
        }
        qemu_vm::composite_cursor(&mut px, rw, rh, c, x - x0 as i32, y - y0 as i32);
        self.write_fb(&px, rw as u32, r);
        self.fb_sprite_px = px;
        self.fb_sprite = Some(r);
    }

    /// Write `r` of `fb_tex` from `data`, rows `stride` pixels apart.
    fn write_fb(&mut self, data: &[u32], stride: u32, r: qemu_vm::Rect) {
        self.chain_stale = true;
        let (tex, _, _, _) = self.fb_tex.as_ref().unwrap();
        let w = stride;
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: tex,
                mip_level: 0,
                origin: wgpu::Origin3d { x: r.x0 as u32, y: r.y0 as u32, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(data),
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(w * 4),
                rows_per_image: Some((r.y1 - r.y0) as u32),
            },
            wgpu::Extent3d {
                width: (r.x1 - r.x0) as u32,
                height: (r.y1 - r.y0) as u32,
                depth_or_array_layers: 1,
            },
        );
    }

    /// Largest rect of the mode's own display aspect that fits the surface,
    /// centered (doc 03 geometry stage, rules 2 and 4). Pure, and run only
    /// when one of its two inputs changes (`guest_surface_changed` for the
    /// mode, `resize` for the host surface). Everything it needs about the
    /// guest is in `self.mode`, which is why the analysis is not repeated
    /// here.
    ///
    /// The height is an integer multiple of the guest's rows so scanlines
    /// stay even, and the width then follows the display aspect rather than
    /// the framebuffer's ratio. That is rule 2. A 320x200 mode is a 4:3
    /// picture, not a 1.6:1 one, and integer-scaling
    /// both axes would show it stretched. Square-pixel 4:3 modes (640x480,
    /// 800x600, …) come out exactly as they did before.
    ///
    /// A scale the user holds (`set_fixed_scale`) replaces the largest
    /// that fits, and is kept when the surface is smaller than the picture.
    fn fit(&self) -> (f32, f32, f32, f32) {
        let m = self.mode;
        if m.scanlines == 0 {
            return (0.0, 0.0, 1.0, 1.0); // no guest surface yet
        }
        let (sw, sh) = self.surface_size();
        let (sw, sh) = (sw as f32, sh as f32);
        let dar = m.display_aspect;
        // The vertical quantum is the scanline, not the guest row: on a
        // double-scanned mode they differ, and it is the scanline pitch that
        // has to come out even. 320x200 in a 2400-line surface is 6 pixels
        // per scanline this way and 5.5 if the rows are quantised instead.
        // Every whole scale of the scanlines is a whole scale of the rows
        // too, so this only ever refines the old rule.
        let gh = m.scanlines as f32;
        // bounded by both axes once the width is corrected; when even 1x does
        // not fit, fall back to a free fit so the picture is letterboxed
        // rather than clipped
        let most = (sh / gh).floor().min((sw / (gh * dar)).floor());
        // A scale the user chose stands even when the window is smaller
        // than the picture: the picture stays centred and what overflows
        // the window is cropped (the viewport may lie past the target;
        // only its size is bounded, by the largest texture, which is also
        // the most the chain's output can be).
        let limit = self.device.limits().max_texture_dimension_2d as f32;
        let held = self.fixed_scale.filter(|_| !self.follows_window).map(|n| (n as f32).min((limit / gh).floor()).min((limit / (gh * dar)).floor()));
        if let Some(n) = held.filter(|n| *n >= 1.0) {
            let (vw, vh) = ((gh * n * dar).round(), gh * n);
            return (((sw - vw) / 2.0).floor(), ((sh - vh) / 2.0).floor(), vw, vh);
        }
        let scale = most;
        let (vw, vh) = if scale >= 1.0 {
            (gh * scale * dar, gh * scale)
        } else if sw / sh > dar {
            (sh * dar, sh)
        } else {
            (sw, sw / dar)
        };
        // Whole pixels, always. The aspect correction makes the width
        // fractional (320x200 at 1x is 533.33 wide), and a fractional
        // viewport puts the picture on a sampling grid that moves with the
        // window's size: every odd pixel of width shifts the centred origin
        // by half a texel and the whole image crawls while the window is
        // dragged out. Rounding costs at most half a pixel of aspect, far
        // inside the 0.5 % the sweep allows, and buys a picture that stands
        // still. It also keeps the blit exactly the size of the chain's
        // output texture, which is integer anyway.
        let vw = vw.round().clamp(1.0, sw);
        let vh = vh.round().clamp(1.0, sh);
        (((sw - vw) / 2.0).floor(), ((sh - vh) / 2.0).floor(), vw, vh)
    }

    /// Where the picture goes: the held answer, never a fresh computation.
    /// A frame is drawn from this, so a mode change reaches the screen as
    /// one step: the analysis, the fit, the chain's output size and the
    /// preset's parameters all move together, before anything is drawn
    /// (doc 03 rule 5).
    pub fn viewport(&self) -> (f32, f32, f32, f32) {
        self.geom
    }

    /// Render into a fixed surface instead of the window's (the headless
    /// mode sweep and the calibration pass). A host-surface change like any
    /// other, so the fit follows it.
    pub(crate) fn force_surface(&mut self, size: (u32, u32)) {
        self.forced_surface = Some(size);
        self.geom = self.fit();
        self.chain_stale = true;
    }

    /// The surface the geometry stage fits the picture into.
    pub fn surface_size(&self) -> (u32, u32) {
        self.forced_surface
            .unwrap_or((self.config.width, self.config.height))
    }

    /// What the window may be shrunk to, when that changed since the last
    /// call: under the picture's own size the geometry stage has no whole
    /// scale left and falls back to a free fit, which is the one case where
    /// the guest's pixels are shrunk and the mode stops being
    /// pixel-accurate. The floor is the 1x picture at the *displayed*
    /// size, so an aspect-corrected mode counts its corrected width
    /// (320x200 -> 534x400), not its framebuffer's.
    ///
    /// Physical pixels: the surface is in physical pixels too, so this is
    /// the same quantity the scale is computed from on any HiDPI screen.
    /// The front end caps it at the screen, or a mode larger than the
    /// screen would ask for a window that cannot be placed, and grows a
    /// window already smaller than it.
    pub fn take_min_size(&mut self) -> Option<MinSize> {
        if !std::mem::take(&mut self.min_size_changed) || self.forced_surface.is_some() {
            return None; // headless sweep/calib: the surface is ours, not the window's
        }
        if self.follows_window {
            // the guest's mode is the window's size, whatever it is; the
            // floor is only a usable desktop
            return Some(MinSize::Logical(640, 480));
        }
        let m = self.mode;
        if m.scanlines == 0 {
            return None;
        }
        Some(MinSize::Physical((m.scanlines as f32 * m.display_aspect).ceil() as u32, m.scanlines))
    }

    /// The picture on show changed size. Everything the geometry stage
    /// decides is decided here: the mode analysis, what the window may be
    /// shrunk to, the scanline count the CRT preset is told (doc 03 rule 3)
    /// and the fit itself.
    ///
    /// This is the QEMU surface change, taken where it can be acted on. The
    /// switch itself arrives on the QEMU thread (`on_switch`), up to a
    /// refresh tick before the first frame of the new mode: re-fitting there
    /// would draw the *old* pixels into the new mode's box for that tick,
    /// which is the stretched leftover rule 5 forbids. The surface's own
    /// texture is therefore the trigger. It is (re)created by exactly the
    /// three things that can change what is on screen, and each of them
    /// calls this: the guest's framebuffer upload (`ensure_texture`), a 3D
    /// slot taken or dropped (`use_slot`), and a slot re-imported at another
    /// size (`import_slot`). Nothing derives geometry while a frame is
    /// drawn.
    fn guest_surface_changed(&mut self) {
        self.chain_stale = true;
        let Some((_, _, tw, th)) = self.current() else {
            return;
        };
        let (tw, th) = (*tw, *th);
        if self.mode.width == tw && self.mode.height == th {
            self.geom = self.fit();
            return;
        }
        self.mode = mode::Mode::analyse(tw, th);
        self.geom = self.fit();
        eprintln!("[display] mode {}", self.mode.describe());
        self.min_size_changed = true;
        let params = self.mode.shader_params();
        let Some(chain) = self.chain.as_ref() else {
            return;
        };
        // the A/B control: the preset left to its own resolution guess, which
        // is what every scanline preset did before mode analysis existed
        if std::env::var("PLAYER_MODE_PARAMS").as_deref() == Ok("0") {
            eprintln!("[shader] PLAYER_MODE_PARAMS=0: preset left to guess the scanline count");
            return;
        }
        if params.iter().all(|(n, _)| chain.has_parameter(n)) {
            chain.set_parameters(&params);
            let set: Vec<String> = params.iter().map(|(n, v)| format!("{n}={v}")).collect();
            eprintln!("[shader] mode parameters {}", set.join(" "));
        } else if !self.warned_no_scanline_params {
            self.warned_no_scanline_params = true;
            eprintln!(
                "[shader] this preset exposes no scanline-count parameter \
                 ({}): a double-scanned mode will be drawn with one scanline \
                 per guest row instead of the two per row the tube drew",
                params
                    .iter()
                    .map(|(n, _)| n.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
            );
        }
    }

    /// Next swapchain image. With FIFO this blocks until one is free; call
    /// it BEFORE sampling the guest frame so the newest frame is presented
    /// (a saturated queue otherwise ages every frame by a host vblank).
    pub fn acquire(&mut self) -> Option<wgpu::SurfaceTexture> {
        use wgpu::CurrentSurfaceTexture as Cst;
        match self.surface.get_current_texture() {
            Cst::Success(f) | Cst::Suboptimal(f) => Some(f),
            Cst::Timeout | Cst::Occluded => None,
            _ => {
                self.resize(self.config.width, self.config.height);
                None
            }
        }
    }

    /// Run the shader chain and, if `frame` is given, blit into it. The
    /// front end presents it (`present`), after whatever its window wants
    /// told first.
    pub fn render(&mut self, frame: Option<&wgpu::SurfaceTexture>) {
        if self.current().is_none() {
            return;
        }
        // CRT chain: guest texture → viewport-sized output texture (doc 03)
        let (_, _, vw, vh) = self.viewport();
        let (vw, vh) = (vw.max(1.0) as u32, vh.max(1.0) as u32);
        // Nothing the chain draws from changed since its last run, and the
        // preset draws the same picture from the same input: its last
        // output stands (`chain_stale`). `PLAYER_DUMP_OUT` counts runs,
        // so it gets every one, as before.
        static DUMP: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let dump = *DUMP.get_or_init(|| std::env::var_os("PLAYER_DUMP_OUT").is_some());
        let same = !self.chain_stale
            && !dump
            && matches!(&self.chain_bg, Some((_, w, h)) if *w == vw && *h == vh)
            && self.chain.as_ref().is_some_and(|c| !c.animated());
        self.chain_stale = false;
        let mut chain_enc = None;
        if let Some(chain) = self.chain.as_mut().filter(|_| !same) {
            let mut enc = self
                .device
                .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                    label: Some("shader"),
                });
            // field-level borrows: `chain` is borrowed mutably above
            let input: &wgpu::Texture = match (self.ext_current, &self.comp) {
                (Some(_), Some((c, _))) if self.comp_on => &c.0,
                (Some(s), _) => &self.ext[s].as_ref().unwrap().0,
                (None, _) => &self.fb_tex.as_ref().unwrap().0,
            };
            match chain.run(&self.device, &mut enc, input, vw, vh) {
                Ok((out, _)) => {
                    let out_tex = out.clone();
                    if !matches!(&self.chain_bg, Some((_, w, h)) if *w == vw && *h == vh) {
                        let view = out_tex.create_view(&wgpu::TextureViewDescriptor::default());
                        let bg = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
                            label: Some("chain bg"),
                            layout: &self.bgl,
                            entries: &[
                                wgpu::BindGroupEntry {
                                    binding: 0,
                                    resource: wgpu::BindingResource::TextureView(&view),
                                },
                                wgpu::BindGroupEntry {
                                    binding: 1,
                                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                                },
                            ],
                        });
                        self.chain_bg = Some((bg, vw, vh));
                    }
                    chain_enc = Some(enc);
                    if let Ok(path) = std::env::var("PLAYER_DUMP_OUT") {
                        let want: usize = std::env::var("PLAYER_DUMP_SEQ")
                            .ok()
                            .and_then(|v| v.parse().ok())
                            .unwrap_or(60);
                        if chain.frame_count() == want {
                            self.queue.submit(Some(chain_enc.take().unwrap().finish()));
                            shader_chain::dump_texture(&self.device, &self.queue, &out_tex, &path);
                            crate::hard_exit(0);
                        }
                    }
                }
                Err(e) => {
                    eprintln!("[shader] frame failed: {e}; disabling chain");
                    self.chain = None;
                    self.chain_bg = None;
                }
            }
        }
        if let Some(enc) = chain_enc {
            self.queue.submit(Some(enc.finish()));
        }
        let Some(frame) = frame else { return };
        let view = frame
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("frame"),
            });
        self.draw_picture(&mut encoder, &view);
        self.queue.submit(Some(encoder.finish()));
    }

    /// Show what `render` drew.
    pub fn present(&self, frame: wgpu::SurfaceTexture) {
        self.queue.present(frame);
    }

    /// Surface pixel → guest framebuffer coordinates (None outside the
    /// image): the point and the picture's own size.
    pub fn to_guest(&self, px: f64, py: f64) -> Option<(i32, i32, i32, i32)> {
        self.view().to_guest(px, py)
    }

    /// What a window asks of the picture, as it stands: for a front end
    /// that draws on a thread of its own and keeps this beside the window
    /// (player-mitsuami's render thread).
    pub fn view(&self) -> GpuView {
        GpuView {
            viewport: self.viewport(),
            shown: self.shown_size(),
            follows_window: self.follows_window,
            picture_px: self.picture_px(),
        }
    }
}

/// The picture's place in the surface, copied out of a `Gpu` (`Gpu::view`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GpuView {
    /// `Gpu::viewport`
    pub viewport: (f32, f32, f32, f32),
    /// `Gpu::shown_size`
    pub shown: Option<(u32, u32)>,
    /// `Gpu::follows_window`
    pub follows_window: bool,
    /// `Gpu::picture_px`
    pub picture_px: Option<(u32, u32)>,
}

impl GpuView {
    /// `Gpu::has_frame`
    pub fn has_frame(&self) -> bool {
        self.shown.is_some()
    }

    /// `Gpu::to_guest`
    pub fn to_guest(&self, px: f64, py: f64) -> Option<(i32, i32, i32, i32)> {
        let (tw, th) = self.shown?;
        let (x, y, w, h) = self.viewport;
        let gx = ((px as f32 - x) / w * tw as f32) as i32;
        let gy = ((py as f32 - y) / h * th as f32) as i32;
        if gx < 0 || gy < 0 || gx >= tw as i32 || gy >= th as i32 {
            return None;
        }
        Some((gx, gy, tw as i32, th as i32))
    }
}

/// Pull the newest guest frame into the GPU: an imported dma-buf slot is
/// selected, a CPU frame is uploaded. Returns its publish time.
pub(crate) fn present_guest_frame(
    gpu: &mut Gpu,
    display: &qemu_vm::Display,
    last_seq: &mut u64,
    composite_cursor: bool,
) -> Option<std::time::Instant> {
    for d in display.take_dmabufs() {
        gpu.import_slot(&d);
    }
    // scanout ring slots the GPU has finished drawing from go back to the
    // device, all but the one on show (M26)
    let _ = gpu.device.poll(wgpu::PollType::Poll);
    let done = gpu.slots_done.swap(0, std::sync::atomic::Ordering::AcqRel);
    if done != 0 {
        display.release_slots(done, gpu.ext_current.map_or(0, qemu_vm::slot_bit));
    }
    let shown_before = gpu.ext_current;
    // the guest's hardware cursor as a sprite in the frame, where the host
    // cursor cannot stand in for it (a relative mouse, a grab); asked
    // before the frame is lent, which holds the same lock
    let sprite = display.cursor_sprite(composite_cursor);
    display.take_newer(*last_seq, |f| {
        *last_seq = f.seq;
        static PUBLISH_LOG: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *PUBLISH_LOG.get_or_init(|| std::env::var_os("PLAYER_PUBLISH_LOG").is_some()) {
            match f.ext_slot {
                Some(s) => eprintln!("[present] seq {} {} slot {s}", f.seq, qemu_vm::trace_ms()),
                None => eprintln!("[present] seq {} {} surface {}x{} {:?}", f.seq, qemu_vm::trace_ms(), f.width, f.height, f.dirty),
            }
        }
        match f.ext_slot {
            Some(s) => {
                gpu.use_slot(Some(s));
                if let Some((c, x, y)) = &sprite {
                    gpu.composite_slot_cursor(c, *x, *y);
                }
            }
            None => {
                gpu.use_slot(None);
                let (w, h) = (f.width as u32, f.height as u32);
                // what changed since the last take, read straight from
                // the published frame (M22: no whole-frame copies)
                let whole = gpu.fb_whole || !matches!(gpu.fb_tex, Some((_, _, tw, th)) if tw == w && th == h);
                let region = if whole { Some(qemu_vm::Rect::whole(f.width, f.height)) } else { f.dirty };
                // a take that changed nothing, the sprite the same one in
                // the same place (a refresh tick, a guest idle under a
                // grabbed mouse): what is on show stands, and so does the
                // chain's output drawn from it
                let same_sprite = match (&sprite, &gpu.fb_sprite_last) {
                    (Some((c, x, y)), Some((lc, lx, ly))) => std::sync::Arc::ptr_eq(c, lc) && x == lx && y == ly,
                    (None, None) => true,
                    _ => false,
                };
                if region.is_some() || !same_sprite {
                    if let Some(r) = region {
                        gpu.upload_rect(f.pixels, w, h, r);
                    }
                    // the last sprite taken off (its rect from the frame)
                    // and this one drawn
                    if let Some(r) = gpu.fb_sprite.take().filter(|_| !whole) {
                        gpu.upload_rect(f.pixels, w, h, r);
                    }
                    if let Some((c, x, y)) = &sprite {
                        gpu.upload_sprite(f.pixels, f.width, f.height, c, *x, *y);
                    }
                    gpu.fb_sprite_last = sprite;
                }
            }
        }
        if let Some(p) = shown_before.filter(|p| gpu.ext_current != Some(*p)) {
            gpu.release_after_gpu(p);
        }
        f.published
    })
}
