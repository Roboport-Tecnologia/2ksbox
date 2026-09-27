//! The picture's GPU half: a Vulkan wgpu device, the guest frame (uploaded
//! or a zero-copy slot), the CRT chain, and a ring of exportable dma-buf
//! images for GTK to show. The player renders into a swapchain image; here
//! the last pass renders into one of these instead, and GTK gets the fd.

use ash::vk;
use std::os::fd::{FromRawFd, OwnedFd};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use wgpu::hal::api::Vulkan;

use crate::dmabuf;
use crate::qemu_vm;

/// DRM_FORMAT_XRGB8888 ('XR24'): BGRA bytes, alpha ignored. Guest pixels
/// carry no alpha, and an opaque format is what lets the compositor put the
/// buffer on a plane.
pub const DRM_FORMAT_XRGB8888: u32 =
    (b'X' as u32) | ((b'R' as u32) << 8) | ((b'2' as u32) << 16) | ((b'4' as u32) << 24);
const DRM_FORMAT_MOD_LINEAR: u64 = 0;
/// Images in the ring: one on screen, one queued in GTK, one being drawn,
/// one spare for the compositor's hold.
const RING: usize = 4;

/// One exportable image: a wgpu render target whose memory is a dma-buf.
pub struct Export {
    _tex: wgpu::Texture,
    view: wgpu::TextureView,
    pub fd: OwnedFd,
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    pub offset: u32,
    pub modifier: u64,
    /// Set while GTK (or the compositor) holds the buffer; cleared by the
    /// texture's release function.
    pub busy: Arc<AtomicBool>,
}

type Shown = (wgpu::Texture, wgpu::BindGroup, u32, u32);

pub struct Gpu {
    instance: wgpu::Instance,
    device: wgpu::Device,
    queue: wgpu::Queue,
    adapter_info: wgpu::AdapterInfo,
    bgl: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    pipeline: wgpu::RenderPipeline,
    pub zero_copy: bool,
    chain: Option<shader_chain::Chain>,
    chain_bg: Option<(wgpu::BindGroup, u32, u32)>,
    fb_tex: Option<Shown>,
    ext: Vec<Option<Shown>>,
    ext_current: Option<usize>,
    pub ring: Vec<Export>,
    /// Renders refused because every image was still held
    pub starved: u64,
}

impl Gpu {
    pub fn new() -> Gpu {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..wgpu::InstanceDescriptor::new_without_display_handle_from_env()
        });
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            ..Default::default()
        }))
        .expect("no Vulkan adapter");
        let desc = wgpu::DeviceDescriptor {
            label: Some("player-gtk"),
            required_features: shader_chain::required_features(&adapter),
            ..Default::default()
        };
        // The player's own device setup: the dma-buf extensions import the
        // backend's 3D slots and export our images alike.
        let (device, queue, exts) = dmabuf::create_device(&adapter, &desc).expect("a Vulkan device");
        assert!(exts, "the spike needs the dma-buf extensions to export its images");
        let zero_copy = std::env::var("PLAYER_ZERO_COPY").as_deref() != Ok("0");
        let adapter_info = adapter.get_info();
        eprintln!("[spike] {} ({:?})", adapter_info.name, adapter_info.driver);

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("nearest"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let tex_entry = |binding, ty| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty,
            count: None,
        };
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("blit bgl"),
            entries: &[
                tex_entry(
                    0,
                    wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                ),
                tex_entry(1, wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering)),
            ],
        });
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("blit"),
            source: wgpu::ShaderSource::Wgsl(include_str!("../../../player/src/blit.wgsl").into()),
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("blit layout"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("blit"),
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
                    format: wgpu::TextureFormat::Bgra8Unorm,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
        Gpu {
            instance,
            device,
            queue,
            adapter_info,
            bgl,
            sampler,
            pipeline,
            zero_copy,
            chain: None,
            chain_bg: None,
            fb_tex: None,
            ext: Vec::new(),
            ext_current: None,
            ring: Vec::new(),
            starved: 0,
        }
    }

    pub fn load_shader(&mut self, path: &std::path::Path, params: &[(String, f32)]) {
        match shader_chain::Chain::load(
            path,
            &self.device,
            &self.queue,
            self.adapter_info.clone(),
            wgpu::TextureFormat::Bgra8Unorm,
        ) {
            Ok(c) => {
                c.set_parameters(params);
                eprintln!("[shader] loaded {}", path.display());
                self.chain = Some(c);
            }
            Err(e) => eprintln!("[shader] failed to load {}: {e}", path.display()),
        }
    }

    fn bind_group(&self, tex: &wgpu::Texture) -> wgpu::BindGroup {
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("blit bg"),
            layout: &self.bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&self.sampler) },
            ],
        })
    }

    /// A CPU frame (XRGB8888, BGRA bytes) as the picture's source.
    pub fn upload(&mut self, pixels: &[u32], w: u32, h: u32) {
        self.ext_current = None;
        if !matches!(&self.fb_tex, Some((_, _, tw, th)) if *tw == w && *th == h) {
            let tex = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("guest framebuffer"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Bgra8Unorm,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let bg = self.bind_group(&tex);
            self.fb_tex = Some((tex, bg, w, h));
        }
        let tex = &self.fb_tex.as_ref().unwrap().0;
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            bytemuck::cast_slice(pixels),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
    }

    /// A backend ring slot (the guest's 3D frames), imported once.
    pub fn import_slot(&mut self, d: &qemu_vm::DmaBuf) {
        match dmabuf::import(&self.device, d.fd, d.w, d.h, d.stride, d.fourcc, d.modifier, false) {
            Ok(tex) => {
                let bg = self.bind_group(&tex);
                if self.ext.len() <= d.slot {
                    self.ext.resize_with(d.slot + 1, || None);
                }
                self.ext[d.slot] = Some((tex, bg, d.w, d.h));
                eprintln!("[3d] slot {}: imported {}x{}", d.slot, d.w, d.h);
            }
            Err(e) => eprintln!("[3d] slot {}: import failed: {e}", d.slot),
        }
    }

    pub fn use_slot(&mut self, slot: usize) {
        self.ext_current = self.ext.get(slot).and_then(|e| e.as_ref()).map(|_| slot);
    }

    /// The guest picture's own size.
    pub fn shown_size(&self) -> Option<(u32, u32)> {
        self.current().map(|(_, _, w, h)| (*w, *h))
    }

    fn current(&self) -> Option<&Shown> {
        match self.ext_current {
            Some(s) => self.ext[s].as_ref(),
            None => self.fb_tex.as_ref(),
        }
    }

    /// A free image of `w`×`h` from the ring, allocating while the ring is
    /// short. Images of another size are dropped once GTK lets go of them.
    fn take_image(&mut self, w: u32, h: u32) -> Option<usize> {
        self.ring.retain(|e| e.busy.load(Ordering::Acquire) || (e.w == w && e.h == h));
        if let Some(i) = self.ring.iter().position(|e| !e.busy.load(Ordering::Acquire)) {
            return Some(i);
        }
        if self.ring.len() >= RING {
            return None;
        }
        match export_image(&self.device, w, h) {
            Ok(e) => {
                if self.ring.is_empty() {
                    eprintln!(
                        "[spike] export {}x{}: modifier 0x{:x}, stride {}, offset {}",
                        e.w, e.h, e.modifier, e.stride, e.offset
                    );
                }
                self.ring.push(e);
                Some(self.ring.len() - 1)
            }
            Err(e) => {
                eprintln!("[spike] export image: {e}");
                None
            }
        }
    }

    /// Run the chain and draw the picture, letterboxed 4:3, into a free ring
    /// image of `w`×`h`. Waits for the GPU before returning (the image goes
    /// to another process with no fence yet); returns the image's index and
    /// that wait.
    pub fn render(&mut self, w: u32, h: u32) -> Option<(usize, Duration)> {
        self.current()?;
        let Some(idx) = self.take_image(w, h) else {
            self.starved += 1;
            return None;
        };
        let view = self.ring[idx].view.clone();
        let sub = self.draw(&view, w, h)?;
        let t = Instant::now();
        let _ = self.device.poll(wgpu::PollType::Wait { submission_index: Some(sub), timeout: None });
        self.ring[idx].busy.store(true, Ordering::Release);
        Some((idx, t.elapsed()))
    }

    /// A wgpu surface on a Wayland `wl_surface` of our own (the spike's
    /// subsurface), set up as the player sets up its window's.
    pub fn surface_on(&self, display: *mut std::ffi::c_void, surface: *mut std::ffi::c_void) -> Surface {
        use wgpu::rwh::{RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle, WaylandWindowHandle};
        let target = wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_display_handle: Some(RawDisplayHandle::Wayland(WaylandDisplayHandle::new(
                std::ptr::NonNull::new(display).unwrap(),
            ))),
            raw_window_handle: RawWindowHandle::Wayland(WaylandWindowHandle::new(
                std::ptr::NonNull::new(surface).unwrap(),
            )),
        };
        let surface = unsafe { self.instance.create_surface_unsafe(target) }.expect("a wgpu surface on the subsurface");
        Surface { surface, config: None }
    }

    /// Draw into the surface's next image and present it; false when there
    /// was nothing to draw or no image.
    pub fn render_to(&mut self, s: &mut Surface, w: u32, h: u32) -> bool {
        if self.current().is_none() {
            return false;
        }
        if !matches!(&s.config, Some(c) if c.width == w && c.height == h) {
            let config = wgpu::SurfaceConfiguration {
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                format: wgpu::TextureFormat::Bgra8Unorm,
                width: w,
                height: h,
                present_mode: wgpu::PresentMode::Mailbox,
                desired_maximum_frame_latency: 1,
                alpha_mode: wgpu::CompositeAlphaMode::Opaque,
                view_formats: vec![],
                color_space: Default::default(),
            };
            s.surface.configure(&self.device, &config);
            s.config = Some(config);
        }
        let frame = match s.surface.get_current_texture() {
            wgpu::CurrentSurfaceTexture::Success(f) | wgpu::CurrentSurfaceTexture::Suboptimal(f) => f,
            _ => {
                s.config = None;
                return false;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        if self.draw(&view, w, h).is_none() {
            return false;
        }
        self.queue.present(frame);
        true
    }

    /// The chain and the letterboxed blit into `target` (`w`×`h`).
    fn draw(&mut self, target: &wgpu::TextureView, w: u32, h: u32) -> Option<wgpu::SubmissionIndex> {
        self.current()?;
        let (vx, vy, vw, vh) = fit_4_3(w, h);
        let mut enc = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("frame") });
        let mut use_chain = false;
        if let Some(chain) = self.chain.as_mut() {
            let input = match self.ext_current {
                Some(s) => &self.ext[s].as_ref().unwrap().0,
                None => &self.fb_tex.as_ref().unwrap().0,
            };
            match chain.run(&self.device, &mut enc, input, vw as u32, vh as u32) {
                Ok((out, _)) => {
                    let out = out.clone();
                    if !matches!(&self.chain_bg, Some((_, cw, ch)) if *cw == vw as u32 && *ch == vh as u32) {
                        let bg = self.bind_group(&out);
                        self.chain_bg = Some((bg, vw as u32, vh as u32));
                    }
                    use_chain = true;
                }
                Err(e) => {
                    eprintln!("[shader] frame failed: {e}; disabling chain");
                    self.chain = None;
                    self.chain_bg = None;
                }
            }
        }
        {
            let bg = match (use_chain, &self.chain_bg) {
                (true, Some((bg, _, _))) => bg,
                _ => &self.current().unwrap().1,
            };
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("blit"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_viewport(vx, vy, vw, vh, 0.0, 1.0);
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, bg, &[]);
            pass.draw(0..3, 0..1);
        }
        Some(self.queue.submit(Some(enc.finish())))
    }
}

/// A wgpu surface and its configuration, made by [`Gpu::surface_on`].
pub struct Surface {
    surface: wgpu::Surface<'static>,
    config: Option<wgpu::SurfaceConfiguration>,
}

/// The largest 4:3 rectangle centred in `w`×`h`.
pub fn fit_4_3(w: u32, h: u32) -> (f32, f32, f32, f32) {
    let (w, h) = (w as f32, h as f32);
    let (vw, vh) = if w * 3.0 > h * 4.0 { (h * 4.0 / 3.0, h) } else { (w, w * 3.0 / 4.0) };
    (((w - vw) / 2.0).floor(), ((h - vh) / 2.0).floor(), vw.floor().max(1.0), vh.floor().max(1.0))
}

/// A BGRA8 render target whose memory is exported as a dma-buf, linear.
fn export_image(device: &wgpu::Device, w: u32, h: u32) -> Result<Export, String> {
    let hal = unsafe { device.as_hal::<Vulkan>() }.ok_or("not a Vulkan device")?;
    let raw = hal.raw_device().clone();
    let instance = hal.shared_instance().raw_instance().clone();
    let phd = hal.raw_physical_device();

    let modifiers = [DRM_FORMAT_MOD_LINEAR];
    let mut ext_mem =
        vk::ExternalMemoryImageCreateInfo::default().handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let mut mod_list = vk::ImageDrmFormatModifierListCreateInfoEXT::default().drm_format_modifiers(&modifiers);
    let info = vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(vk::Format::B8G8R8A8_UNORM)
        .extent(vk::Extent3D { width: w, height: h, depth: 1 })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .usage(
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::TRANSFER_DST,
        )
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
        .push_next(&mut ext_mem)
        .push_next(&mut mod_list);
    let image = unsafe { raw.create_image(&info, None) }.map_err(|e| format!("vkCreateImage: {e}"))?;

    let req = unsafe { raw.get_image_memory_requirements(image) };
    let mem_props = unsafe { instance.get_physical_device_memory_properties(phd) };
    let types = || (0..mem_props.memory_type_count).filter(|i| req.memory_type_bits & (1 << i) != 0);
    let local = |i: &u32| {
        mem_props.memory_types[*i as usize].property_flags.contains(vk::MemoryPropertyFlags::DEVICE_LOCAL)
    };
    let Some(mem_type) = types().find(local).or_else(|| types().next()) else {
        unsafe { raw.destroy_image(image, None) };
        return Err("no memory type for the image".into());
    };
    let mut export =
        vk::ExportMemoryAllocateInfo::default().handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
    let alloc = vk::MemoryAllocateInfo::default()
        .allocation_size(req.size)
        .memory_type_index(mem_type)
        .push_next(&mut export)
        .push_next(&mut dedicated);
    let memory = match unsafe { raw.allocate_memory(&alloc, None) } {
        Ok(m) => m,
        Err(e) => {
            unsafe { raw.destroy_image(image, None) };
            return Err(format!("vkAllocateMemory(export): {e}"));
        }
    };
    let cleanup = move |raw: &ash::Device| unsafe {
        raw.destroy_image(image, None);
        raw.free_memory(memory, None);
    };
    if let Err(e) = unsafe { raw.bind_image_memory(image, memory, 0) } {
        cleanup(&raw);
        return Err(format!("vkBindImageMemory: {e}"));
    }
    let fd_loader = ash::khr::external_memory_fd::Device::new(&instance, &raw);
    let fd = match unsafe {
        fd_loader.get_memory_fd(
            &vk::MemoryGetFdInfoKHR::default()
                .memory(memory)
                .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT),
        )
    } {
        Ok(fd) => unsafe { OwnedFd::from_raw_fd(fd) },
        Err(e) => {
            cleanup(&raw);
            return Err(format!("vkGetMemoryFd: {e}"));
        }
    };
    let mod_loader = ash::ext::image_drm_format_modifier::Device::new(&instance, &raw);
    let mut mod_props = vk::ImageDrmFormatModifierPropertiesEXT::default();
    if let Err(e) = unsafe { mod_loader.get_image_drm_format_modifier_properties(image, &mut mod_props) } {
        cleanup(&raw);
        return Err(format!("vkGetImageDrmFormatModifierProperties: {e}"));
    }
    let layout = unsafe {
        raw.get_image_subresource_layout(
            image,
            vk::ImageSubresource { aspect_mask: vk::ImageAspectFlags::MEMORY_PLANE_0_EXT, mip_level: 0, array_layer: 0 },
        )
    };

    let size = wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 };
    let hal_desc = wgpu::hal::TextureDescriptor {
        label: Some("export"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Bgra8Unorm,
        usage: wgpu::TextureUses::COLOR_TARGET | wgpu::TextureUses::RESOURCE | wgpu::TextureUses::COPY_SRC,
        memory_flags: wgpu::hal::MemoryFlags::empty(),
        view_formats: vec![],
    };
    let raw2 = raw.clone();
    let hal_tex = unsafe {
        hal.texture_from_raw(
            image,
            &hal_desc,
            Some(Box::new(move || cleanup(&raw2))),
            wgpu::hal::vulkan::TextureMemory::External,
        )
    };
    drop(hal);
    let tex = unsafe {
        device.create_texture_from_hal::<Vulkan>(
            hal_tex,
            &wgpu::TextureDescriptor {
                label: Some("export"),
                size,
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Bgra8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            },
            wgpu::TextureUses::UNINITIALIZED,
        )
    };
    let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
    Ok(Export {
        _tex: tex,
        view,
        fd,
        w,
        h,
        stride: layout.row_pitch as u32,
        offset: layout.offset as u32,
        modifier: mod_props.drm_format_modifier,
        busy: Arc::new(AtomicBool::new(false)),
    })
}
