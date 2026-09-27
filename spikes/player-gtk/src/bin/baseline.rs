//! The spike's baseline: the player's own display path (winit, a wgpu
//! surface in Mailbox with a frame latency of 1, render on the wake) on
//! the same 60 Hz pattern thread, measured the same way: publish→present
//! return, as the player's `PLAYER_LATENCY` reports it, and
//! publish→presented from the compositor's presentation-time feedback.
//!
//! `cargo run --release --bin baseline` (Wayland only).

#[path = "../../../../player/src/pattern.rs"]
#[allow(dead_code)]
mod pattern;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use winit::application::ApplicationHandler;
use winit::event::WindowEvent;
use winit::event_loop::{ActiveEventLoop, EventLoop};
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle};
use winit::window::{Window, WindowId};

type Slot = Arc<Mutex<Option<(Vec<u32>, Instant)>>>;

struct Gpu {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    pipeline: wgpu::RenderPipeline,
    tex: wgpu::Texture,
    bg: wgpu::BindGroup,
}

struct App {
    slot: Slot,
    gpu: Option<Gpu>,
    pres: Option<pres::Feedback>,
    published: Option<Instant>,
    returned: Vec<f32>,
    presented: Vec<f32>,
    discarded: u64,
    wakes: u64,
    redraws: u64,
    acquired: u64,
}

impl ApplicationHandler for App {
    fn resumed(&mut self, el: &ActiveEventLoop) {
        if self.gpu.is_some() {
            return;
        }
        let window = Arc::new(
            el.create_window(Window::default_attributes().with_title("2ksbox player (winit baseline)")).unwrap(),
        );
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            ..wgpu::InstanceDescriptor::new_without_display_handle_from_env()
        });
        let surface = instance.create_surface(window.clone()).unwrap();
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            ..Default::default()
        }))
        .unwrap();
        let (device, queue) = pollster::block_on(adapter.request_device(&Default::default())).unwrap();
        let size = window.inner_size();
        let mut config = surface.get_default_config(&adapter, size.width.max(1), size.height.max(1)).unwrap();
        if surface.get_capabilities(&adapter).present_modes.contains(&wgpu::PresentMode::Mailbox) {
            config.present_mode = wgpu::PresentMode::Mailbox;
        }
        config.desired_maximum_frame_latency = 1;
        surface.configure(&device, &config);
        eprintln!("[baseline] {:?}, {:?}", config.present_mode, config.format);

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: None,
            source: wgpu::ShaderSource::Wgsl(include_str!("../../../../player/src/blit.wgsl").into()),
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: None,
            layout: None,
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
                targets: &[Some(config.format.into())],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: None,
            size: wgpu::Extent3d { width: pattern::WIDTH as u32, height: pattern::HEIGHT as u32, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Bgra8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let sampler = device.create_sampler(&Default::default());
        let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipeline.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&tex.create_view(&Default::default())),
                },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(&sampler) },
            ],
        });
        if let (Ok(w), Ok(d)) = (window.window_handle(), window.display_handle())
            && let (RawWindowHandle::Wayland(w), RawDisplayHandle::Wayland(d)) = (w.as_raw(), d.as_raw())
        {
            match unsafe { pres::Feedback::new(d.display.as_ptr(), w.surface.as_ptr()) } {
                Ok(p) => self.pres = Some(p),
                Err(e) => eprintln!("[baseline] no presentation feedback: {e}"),
            }
        }
        self.gpu = Some(Gpu { window, surface, device, queue, config, pipeline, tex, bg });
    }

    fn user_event(&mut self, _el: &ActiveEventLoop, _: ()) {
        self.wakes += 1;
        if std::env::var_os("SPIKE_TRACE").is_some() && self.wakes % 60 == 0 {
            eprintln!("[trace] wakes {} redraws {} acquired {} presented {}", self.wakes, self.redraws, self.acquired, self.presented.len());
        }
        if let Some(p) = self.pres.as_mut() {
            for (published, at) in p.take() {
                match at {
                    Some(at) => self.presented.push(at.saturating_duration_since(published).as_secs_f32() * 1000.0),
                    None => self.discarded += 1,
                }
            }
        }
        if self.presented.len() >= 240 {
            let r = pct(&mut self.returned);
            let p = pct(&mut self.presented);
            eprintln!("[latency] publish→present-return {r} | publish→presented {p} ms (discarded {})", self.discarded);
            self.returned.clear();
            self.presented.clear();
            self.discarded = 0;
        }
        if let Some(g) = &self.gpu {
            g.window.request_redraw();
        }
    }

    fn window_event(&mut self, el: &ActiveEventLoop, _: WindowId, event: WindowEvent) {
        let Some(g) = self.gpu.as_mut() else { return };
        match event {
            WindowEvent::CloseRequested => el.exit(),
            WindowEvent::Resized(s) => {
                g.config.width = s.width.max(1);
                g.config.height = s.height.max(1);
                g.surface.configure(&g.device, &g.config);
            }
            WindowEvent::RedrawRequested => {
                self.redraws += 1;
                // the player's order: the swapchain image first, then the newest frame
                let frame = match g.surface.get_current_texture() {
                    wgpu::CurrentSurfaceTexture::Success(f) | wgpu::CurrentSurfaceTexture::Suboptimal(f) => f,
                    _ => return,
                };
                self.acquired += 1;
                if let Some((fb, t)) = self.slot.lock().unwrap().take() {
                    g.queue.write_texture(
                        wgpu::TexelCopyTextureInfo {
                            texture: &g.tex,
                            mip_level: 0,
                            origin: wgpu::Origin3d::ZERO,
                            aspect: wgpu::TextureAspect::All,
                        },
                        bytemuck::cast_slice(&fb),
                        wgpu::TexelCopyBufferLayout {
                            offset: 0,
                            bytes_per_row: Some(pattern::WIDTH as u32 * 4),
                            rows_per_image: None,
                        },
                        g.tex.size(),
                    );
                    self.published = Some(t);
                }
                let view = frame.texture.create_view(&Default::default());
                let mut enc = g.device.create_command_encoder(&Default::default());
                {
                    let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: None,
                        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                            view: &view,
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
                    let (w, h) = (g.config.width as f32, g.config.height as f32);
                    let (vw, vh) = if w * 3.0 > h * 4.0 { (h * 4.0 / 3.0, h) } else { (w, w * 3.0 / 4.0) };
                    pass.set_viewport(((w - vw) / 2.0).floor(), ((h - vh) / 2.0).floor(), vw, vh, 0.0, 1.0);
                    pass.set_pipeline(&g.pipeline);
                    pass.set_bind_group(0, &g.bg, &[]);
                    pass.draw(0..3, 0..1);
                }
                g.queue.submit(Some(enc.finish()));
                let published = self.published.take();
                if let (Some(p), Some(t)) = (self.pres.as_mut(), published) {
                    p.request(t);
                }
                g.window.pre_present_notify();
                g.queue.present(frame);
                if let Some(t) = published {
                    self.returned.push(t.elapsed().as_secs_f32() * 1000.0);
                }
            }
            _ => {}
        }
    }
}

fn pct(v: &mut [f32]) -> String {
    if v.is_empty() {
        return "-".into();
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    format!("p50 {:.1} p95 {:.1} max {:.1}", v[v.len() / 2], v[v.len() * 95 / 100], v[v.len() - 1])
}

fn main() {
    let el = EventLoop::new().unwrap();
    let proxy = el.create_proxy();
    let slot: Slot = Arc::default();
    let s = slot.clone();
    let hz: f64 = std::env::var("PATTERN_HZ").ok().and_then(|v| v.parse().ok()).unwrap_or(60.0);
    std::thread::spawn(move || {
        let mut p = pattern::Pattern::new();
        let period = Duration::from_secs_f64(1.0 / hz);
        let mut next = Instant::now();
        loop {
            p.render();
            *s.lock().unwrap() = Some((p.fb.clone(), Instant::now()));
            let _ = proxy.send_event(());
            next += period;
            std::thread::sleep(next.saturating_duration_since(Instant::now()));
        }
    });
    let mut app = App {
        slot,
        gpu: None,
        pres: None,
        published: None,
        returned: Vec::new(),
        presented: Vec::new(),
        discarded: 0,
        wakes: 0,
        redraws: 0,
        acquired: 0,
    };
    el.run_app(&mut app).unwrap();
}

#[path = "../pres.rs"]
mod pres;
