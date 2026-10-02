//! The shader profile editor's live preview: a still image (a game
//! screenshot, typically) run through the same `shader-chain` crate the
//! player uses, re-rendered whenever the preset, its parameter values or
//! the image change.
//!
//! **It opens its own GPU.** The front end draws with the platform's own
//! widgets, which hand out no GPU device to render into, so
//! `headless()` opens a windowless device
//! (~40 MB of VRAM and one more driver context) and the front end reads
//! the frame back to the CPU (`read_frame`). Everything in between
//! (decoding the image, the source-size cap, loading the chain, the
//! integer-scale viewport math) is in this file, and the
//! `--preview-shader` verb dumps the same frame as a PNG (doc 07).

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// A source bigger than this is downsized on the CPU before the shader
/// sees it. Any real game resolution passes through untouched, so the
/// preview looks like the player: the native resolution as-is, then
/// integer-scaled the way `player::Gpu::viewport` does. The cap only
/// stops a 12-megapixel phone photo from rendering at full size every
/// frame.
const MAX_SOURCE_W: u32 = 1600;
const MAX_SOURCE_H: u32 = 1200;

/// The rate the preview's clock runs at, and so the rate an animated
/// preset flickers, rolls or fades at in it: the frame rate the player
/// presents the same preset at on a machine of the era. The preview
/// counts frames off this clock rather than counting its own renders
/// (see `shader_chain::Chain::run_at`). A front end that cannot redraw
/// this often (every frame is read back to the CPU and handed to the
/// window as pixels) then shows an effect at its real speed with frames
/// missing, not in slow motion.
pub const FRAME_RATE: f64 = 60.0;

/// How long a front end may wait before asking for the next frame of an
/// animated preset. Just under 1/60 s, so a timer on this interval and a
/// clock at `FRAME_RATE` don't beat into a visibly uneven flicker.
pub const FRAME_INTERVAL: Duration = Duration::from_millis(16);

pub struct Preview {
    device: wgpu::Device,
    queue: wgpu::Queue,
    adapter_info: wgpu::AdapterInfo,
    image_path: Option<PathBuf>,
    input: Option<(wgpu::Texture, u32, u32)>,
    preset_path: Option<PathBuf>,
    chain: Option<shader_chain::Chain>,
    viewport: (u32, u32),
    /// When the loaded preset started animating (its frame 0).
    clock: Instant,
    /// A frame number to render instead of the clock's, for the headless
    /// verbs, so "the same preset at frame 12" can be dumped twice and
    /// compared.
    pinned_frame: Option<usize>,
    error: Option<String>,
}

impl Preview {
    fn new(device: wgpu::Device, queue: wgpu::Queue, adapter_info: wgpu::AdapterInfo) -> Preview {
        Preview {
            device,
            queue,
            adapter_info,
            image_path: None,
            input: None,
            preset_path: None,
            chain: None,
            viewport: (0, 0),
            clock: Instant::now(),
            pinned_frame: None,
            error: None,
        }
    }

    /// Open a windowless adapter and device of this preview's own. The
    /// toolkit will not lend one, and the headless verbs have none.
    /// Fails only when there is no usable GPU at all, which would stop
    /// the player too.
    pub fn headless() -> Result<Preview, String> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            ..Default::default()
        }))
        .map_err(|e| format!("no suitable GPU adapter: {e}"))?;
        let adapter_info = adapter.get_info();
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("shader preview"),
            // The preview opens its device with the player's feature
            // set. Without it a curved preset previews with its edge
            // pixels smeared outwards (`shader_chain::required_features`).
            required_features: shader_chain::required_features(&adapter),
            ..Default::default()
        }))
        .map_err(|e| format!("requesting a device: {e}"))?;
        Ok(Preview::new(device, queue, adapter_info))
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// How soon the caller must call `update` again, or `None` when it
    /// need not: the loaded preset draws the same picture forever and
    /// only a slider, a new image or a resize can change it.
    ///
    /// Plenty of presets do not stand still: an interlaced CRT draws
    /// alternate fields, a phosphor afterglow decays over several
    /// frames, an NTSC signal shimmers. A preview that renders only when
    /// something is clicked shows one frozen frame of that. Whether a
    /// preset animates is `shader_chain::preset_is_animated`'s answer,
    /// not a front end's, and so is the interval the front end's timer
    /// runs at.
    pub fn frame_interval(&self) -> Option<Duration> {
        self.chain.as_ref()?.animated().then_some(FRAME_INTERVAL)
    }

    /// Render one fixed frame number from now on instead of following
    /// the clock, for the headless verbs and the tests behind them, where
    /// "frame 0 and frame 7 of this preset differ" has to be repeatable.
    /// The real editor never uses it.
    pub fn pin_frame(&mut self, frame: usize) {
        self.pinned_frame = Some(frame);
    }

    /// The frame number the next render will be: elapsed time at
    /// `FRAME_RATE` since the preset was loaded, unless pinned.
    fn frame(&self) -> usize {
        self.pinned_frame
            .unwrap_or_else(|| (self.clock.elapsed().as_secs_f64() * FRAME_RATE) as usize)
    }

    /// The exact size the last rendered frame came out at: the input
    /// image's own size times the largest integer factor that fits the
    /// area asked for. Never a fraction, so a second, non-integer
    /// resample never blurs the shader's output. It is smaller than the
    /// area on one axis (letterboxed) unless the aspect ratios match. The
    /// caller centres it and fills the rest with black, as the player
    /// does, rather than stretching it to fit.
    pub fn viewport(&self) -> (u32, u32) {
        self.viewport
    }

    /// The last rendered frame, for `read_frame` and `dump_png`.
    fn output_texture(&self) -> Option<&wgpu::Texture> {
        self.chain.as_ref().and_then(shader_chain::Chain::output_texture)
    }

    /// Reflect the editor's current preset path, effective parameter
    /// values (defaults already merged with overrides), image path, and
    /// the area available to render into, reloading only what changed
    /// and re-rendering a frame.
    pub fn update(&mut self, preset: &Path, params: &[(String, f32)], image: &Path, area_w: u32, area_h: u32) {
        if self.image_path.as_deref() != Some(image) {
            self.load_image(image);
        }
        if self.preset_path.as_deref() != Some(preset) {
            self.load_preset(preset);
        }
        if self.input.is_none() || self.chain.is_none() {
            return;
        }
        self.error = None;
        if let Some(chain) = &self.chain {
            chain.set_parameters(params);
        }
        self.render(area_w, area_h);
    }

    /// Read the last frame back as `(width, height, RGB8)`, the CPU path,
    /// since the front end cannot take a texture.
    pub fn read_frame(&self) -> Option<(u32, u32, Vec<u8>)> {
        let tex = self.output_texture()?;
        Some(shader_chain::read_texture(&self.device, &self.queue, tex))
    }

    /// Dump the last frame as a PNG, which is what `--preview-shader`
    /// writes.
    pub fn dump_png(&self, out: &str) -> Result<(), String> {
        let tex = self.output_texture().ok_or("no frame rendered")?;
        shader_chain::dump_texture(&self.device, &self.queue, tex, out);
        Ok(())
    }

    fn load_image(&mut self, path: &Path) {
        self.image_path = Some(path.to_path_buf());
        self.input = None;
        let img = match image::open(path) {
            Ok(img) => img,
            Err(e) => {
                self.error = Some(format!("loading {}: {e}", path.display()));
                return;
            }
        };
        // A CRT preset is written to upscale a small native-resolution
        // source, never to shrink one. Several (crt-aperture.slang, e.g.)
        // compute `scale = floor(OutputSize / SourceSize)` and divide by
        // it, which is 0 when the source is bigger than the render target:
        // NaN, a solid black frame (a 1025x791 photo through crt-aperture
        // rendered black). `render`'s `.max(1.0)` on the scale prevents
        // that regardless of this cap, which only stops a huge photo from
        // rendering at full size every frame.
        let img = if img.width() > MAX_SOURCE_W || img.height() > MAX_SOURCE_H {
            img.resize(MAX_SOURCE_W, MAX_SOURCE_H, image::imageops::FilterType::Triangle)
        } else {
            img
        };
        let img = img.to_rgba8();
        let (w, h) = img.dimensions();
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("shader preview input"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            // Not sRGB: the shader's own GAMMA_INPUT parameter (present
            // on most CRT presets) does that math, as when the player
            // feeds it raw (non-sRGB-tagged) guest pixels.
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        self.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            img.as_raw(),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(4 * w), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.input = Some((tex, w, h));
    }

    fn load_preset(&mut self, path: &Path) {
        self.preset_path = Some(path.to_path_buf());
        self.chain = None;
        // A preset that animates starts at its own frame 0, not at
        // however long this window has been open.
        self.clock = Instant::now();
        // The readback path is indifferent to the format; RGBA8 is what
        // `shader_chain::read_texture` and `dump_texture` expect.
        match shader_chain::Chain::load(
            path,
            &self.device,
            &self.queue,
            self.adapter_info.clone(),
            wgpu::TextureFormat::Rgba8Unorm,
        ) {
            Ok(c) => self.chain = Some(c),
            Err(e) => self.error = Some(format!("loading shader: {e}")),
        }
    }

    fn render(&mut self, area_w: u32, area_h: u32) {
        let Some((_, iw, ih)) = &self.input else { return };
        let (iw, ih) = (*iw, *ih);
        if self.chain.is_none() {
            return;
        }
        // Exactly `player::Gpu::viewport`'s math: the largest integer
        // scale that fits the area, floored (a fraction would blur the
        // thing being previewed) and never below 1. The floor of 1 also
        // keeps some presets' own `1.0 / floor(OutputSize/SourceSize)`
        // away from a division by zero. When the area is smaller than the
        // image, the frame is bigger than the area and the caller crops
        // it around the centre, as the player does when its window is
        // smaller than the guest's native resolution.
        let (aw, ah) = (area_w.max(1) as f32, area_h.max(1) as f32);
        let scale = (aw / iw as f32).min(ah / ih as f32).floor().max(1.0);
        let (rw, rh) = ((iw as f32 * scale) as u32, (ih as f32 * scale) as u32);
        self.viewport = (rw, rh);

        let frame = self.frame();
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("shader preview") });
        let result = {
            let input_tex = &self.input.as_ref().unwrap().0;
            let chain = self.chain.as_mut().unwrap();
            chain
                .run_at(&self.device, &mut encoder, input_tex, rw, rh, frame)
                .map(|_| ())
        };
        match result {
            Ok(()) => {
                self.queue.submit(Some(encoder.finish()));
            }
            Err(e) => self.error = Some(e),
        }
    }
}
