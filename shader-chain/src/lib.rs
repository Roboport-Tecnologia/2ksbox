//! librashader (RetroArch slang) filter chain on wgpu. Shared by the
//! player (doc 03's CRT pass: guest framebuffer -> letterboxed viewport)
//! and the launcher's shader profile preview (doc 07: a still image ->
//! a frame the front end shows, re-run as parameter sliders move). Both
//! load a preset, set its parameters and run a frame, so one crate keeps
//! them from driving librashader differently.

use librashader::preprocess::ShaderSource;
use librashader::presets::{ShaderFeatures, ShaderPreset};
use librashader::runtime::wgpu::{FilterChain, FilterChainOptions, WgpuOutputView};
use librashader::runtime::{FilterChainParameters, Size, Viewport};
use std::path::Path;

/// The device features a filter chain needs, of those this adapter has.
/// **Request these when opening the device the chain will run on.**
///
/// There is one, and it is needed for correctness. A slang preset's
/// default wrap mode is `clamp_to_border` (RetroArch's own default, and
/// librashader's `WrapMode::default()`), with a transparent black border.
/// librashader's wgpu runtime silently downgrades every such sampler to
/// `clamp_to_edge` on a device opened without
/// `ADDRESS_MODE_CLAMP_TO_BORDER` (`samplers.rs`: "if the device doesn't
/// have clamp to border support, approximate it with clamp to edge").
/// A preset that curves the picture samples outside it at the corners
/// and along the edges, and then gets the outermost row of pixels
/// smeared outwards instead of black. The user saw that as "outside the
/// curve is glitched, it repeats the last colour".
///
/// Masked by what the adapter has, so this is always safe to pass. A
/// device that cannot do it is opened without it, and presets fall back
/// to the smeared edges rather than failing to open.
pub fn required_features(adapter: &wgpu::Adapter) -> wgpu::Features {
    adapter.features() & wgpu::Features::ADDRESS_MODE_CLAMP_TO_BORDER
}

pub struct Chain {
    chain: FilterChain,
    frame_count: usize,
    animated: bool,
    out: Option<(wgpu::Texture, wgpu::TextureView, u32, u32)>,
    format: wgpu::TextureFormat,
}

impl Chain {
    pub fn load(
        path: &Path,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        adapter_info: wgpu::AdapterInfo,
        format: wgpu::TextureFormat,
    ) -> Result<Chain, String> {
        let opts = FilterChainOptions {
            force_no_mipmaps: false,
            enable_cache: false, // needs Features::PIPELINE_CACHE; not requested
            adapter_info: Some(adapter_info),
        };
        let chain =
            FilterChain::load_from_path(path, ShaderFeatures::NONE, device, queue, Some(&opts))
                .map_err(|e| format!("{e}"))?;
        Ok(Chain {
            chain,
            frame_count: 0,
            animated: preset_is_animated(path),
            out: None,
            format,
        })
    }

    /// The preset's current value for a parameter, or `None` when it does
    /// not declare one by that name. Mode analysis (doc 03 rule 3) can
    /// only tell a preset the scanline count through parameters the preset
    /// has, and most presets have none, so `None` is a normal outcome.
    pub fn parameter(&self, name: &str) -> Option<f32> {
        self.chain.parameters().parameter_value(name)
    }

    pub fn has_parameter(&self, name: &str) -> bool {
        self.parameter(name).is_some()
    }

    /// Override parameter values by name (the launcher's shader profiles,
    /// doc 07) without reloading the chain. Cheap enough to call on every
    /// slider tick in the profile preview, unlike `load`, which recompiles
    /// shaders. A name the preset doesn't declare is skipped with a stderr
    /// line, so a profile saved against an older preset version, or a
    /// stale slider from a preset just swapped out, doesn't fail the load.
    pub fn set_parameters(&self, params: &[(String, f32)]) {
        if params.is_empty() {
            return;
        }
        self.chain.parameters().update_parameters(|map| {
            for (name, value) in params {
                if let Some(slot) = map.get_mut::<str>(name.as_ref()) {
                    *slot = *value;
                } else {
                    eprintln!("[shader] preset has no parameter named {name:?}, ignoring override");
                }
            }
        });
    }

    fn ensure_out(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        if matches!(&self.out, Some((_, _, ow, oh)) if *ow == w && *oh == h) {
            return;
        }
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("shader output"),
            size: wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: self.format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
        self.out = Some((tex, view, w, h));
    }

    /// Run the chain: `input` texture -> output texture of size (w, h),
    /// as the next frame in sequence. Returns the output texture and its
    /// view (valid until the next size change).
    pub fn run(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        input: &wgpu::Texture,
        w: u32,
        h: u32,
    ) -> Result<(&wgpu::Texture, &wgpu::TextureView), String> {
        self.encode(device, encoder, input, w, h, self.frame_count)?;
        self.frame_count += 1;
        let (tex, view, _, _) = self.out.as_ref().unwrap();
        Ok((tex, view))
    }

    /// The same, with the frame number said outright rather than counted.
    ///
    /// A preset animates against the frame number (`FrameCount`, and the
    /// pass's own `frame_count_mod`), not the number of renders. The two
    /// match only for a consumer that renders every frame, as the player
    /// does. The launcher's preview renders a still image on a timer that
    /// a busy UI or a slow readback can miss ticks of, so it drives this
    /// from the clock, and a preset flickers at the rate it would in the
    /// player however many frames the launcher drew.
    pub fn run_at(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        input: &wgpu::Texture,
        w: u32,
        h: u32,
        frame_count: usize,
    ) -> Result<(&wgpu::Texture, &wgpu::TextureView), String> {
        self.encode(device, encoder, input, w, h, frame_count)?;
        let (tex, view, _, _) = self.out.as_ref().unwrap();
        Ok((tex, view))
    }

    /// The render itself, borrowing nothing out, so both callers above
    /// can still touch `self` after it.
    fn encode(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        input: &wgpu::Texture,
        w: u32,
        h: u32,
        frame_count: usize,
    ) -> Result<(), String> {
        self.ensure_out(device, w, h);
        let (_, view, _, _) = self.out.as_ref().unwrap();
        let size = Size::new(w, h);
        let viewport = Viewport {
            x: 0.0,
            y: 0.0,
            mvp: None,
            output: WgpuOutputView::new_from_raw(view, size, self.format),
            size,
        };
        self.chain
            .frame(input, &viewport, encoder, frame_count, None)
            .map_err(|e| format!("{e}"))
    }

    pub fn frame_count(&self) -> usize {
        self.frame_count
    }

    /// Whether this preset's picture can change from one frame to the
    /// next even though nothing else does: an interlaced or flickering
    /// CRT, a phosphor afterglow, a shimmering NTSC signal (see
    /// `preset_is_animated`). A consumer that renders on demand (the
    /// launcher's preview) has to keep rendering while this is true, or
    /// it shows one frozen frame of an effect that should move.
    pub fn animated(&self) -> bool {
        self.animated
    }

    /// The texture the last `run` wrote, if `run` has ever succeeded, for
    /// a consumer that needs to read it back itself (the player's
    /// `PLAYER_DUMP_OUT` and its mode sweep, the launcher's shader-preview
    /// debug verb) via `dump_texture` / `read_texture`.
    pub fn output_texture(&self) -> Option<&wgpu::Texture> {
        self.out.as_ref().map(|(tex, _, _, _)| tex)
    }

    /// The view onto it. `run` returns this too, but only borrowed for
    /// the length of that call. A consumer that hands the frame to a
    /// toolkit after rendering needs to ask for it separately.
    pub fn output_view(&self) -> Option<&wgpu::TextureView> {
        self.out.as_ref().map(|(_, view, _, _)| view)
    }
}

/// The uniforms whose value changes from frame to frame while the input
/// stands still. Presets mostly animate against `FrameCount`
/// (`mod(FrameCount, 2.0)` draws alternating fields, a flickering
/// phosphor, a rolling NTSC phase). librashader passes the rest through
/// too, and a preset reading one of them is animated as well.
const FRAME_VARYING_UNIFORMS: [&str; 4] = [
    "FrameCount",
    "FrameDirection",
    "FrameTimeDelta",
    "CurrentSubFrame",
];

/// A texture bound to an earlier frame: the previous frames of the input
/// (`OriginalHistory1`…) or a pass's own last output, which is
/// `PassFeedback2` by number and `<alias>Feedback` when the preset named
/// the pass, so the bare suffix catches both. A pass sampling one of
/// these settles over several frames (an afterglow, a motion blur, a
/// running average), so it is animated too.
const FEEDBACK_TEXTURES: [&str; 2] = ["OriginalHistory", "Feedback"];

/// Whether a preset's picture can change from one frame to the next on a
/// still input.
///
/// This reads the preset's shader sources rather than reflecting the
/// compiled SPIR-V. Reflection would answer exactly, but only by
/// compiling every pass a second time, and the point of asking is to
/// avoid work. The reading is conservative: when in doubt it says
/// animated. Wrong that way, a preview redraws a picture that never
/// changes; wrong the other way, a flicker effect freezes on one frame.
///
/// Nearly every CRT shader declares `FrameCount` in its uniform block
/// and most never read it (1131 of the slang-shaders tree declare it,
/// 271 use it), so the declaration alone means nothing. This looks for a
/// use: the member access `params.FrameCount` / `global.FrameCount` that
/// GLSL requires to read a named uniform block's member. The sources are
/// the preprocessed ones, so an `#include`d `#define FrameCount
/// params.FrameCount` (how the shaders that seem to use it bare do it)
/// is already expanded here.
pub fn preset_is_animated(path: &Path) -> bool {
    let Ok(preset) = ShaderPreset::try_parse(path, ShaderFeatures::NONE) else {
        return true; // unreadable: `Chain::load` will have its own say
    };
    preset.passes.iter().any(|pass| {
        // A pass that asks for the frame count modulo N is reading the
        // frame count, whatever its source looks like.
        pass.meta.frame_count_mod > 0
            || match ShaderSource::load(&pass.path, preset.features) {
                Ok(src) => source_is_animated(&src.vertex) || source_is_animated(&src.fragment),
                Err(_) => true,
            }
    })
}

fn source_is_animated(src: &str) -> bool {
    FEEDBACK_TEXTURES.iter().any(|name| src.contains(name))
        || FRAME_VARYING_UNIFORMS
            .iter()
            .any(|name| reads_member(src, name))
}

/// Whether `src` reads `<block>.<name>` anywhere: a use of the uniform,
/// as opposed to the `uint FrameCount;` that declares it.
fn reads_member(src: &str, name: &str) -> bool {
    let mut rest = src;
    while let Some(at) = rest.find(name) {
        let before = rest[..at].trim_end();
        let after = &rest[at + name.len()..];
        // `.FrameCount`, and not `.FrameCountSomething`.
        if before.ends_with('.')
            && !after
                .chars()
                .next()
                .is_some_and(|c| c.is_alphanumeric() || c == '_')
        {
            return true;
        }
        rest = after;
    }
    false
}

/// Debug/tooling readback of a texture to PNG (the player's
/// `PLAYER_DUMP_OUT`). Blocks on the GPU.
pub fn dump_texture(device: &wgpu::Device, queue: &wgpu::Queue, tex: &wgpu::Texture, path: &str) {
    let (w, h, rgb) = read_texture(device, queue, tex);
    match write_png(path, w, h, &rgb) {
        Ok(()) => eprintln!("dumped shader output {w}x{h} to {path}"),
        Err(e) => eprintln!("dump {path}: {e}"),
    }
}

/// Read a texture back as RGB8, row-major. Blocks on the GPU; diagnostics
/// only (the mode sweep measures the frame it just rendered).
pub fn read_texture(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    tex: &wgpu::Texture,
) -> (u32, u32, Vec<u8>) {
    let (w, h) = (tex.width(), tex.height());
    let bpr = (w * 4).div_ceil(256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: (bpr * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("readback"),
    });
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: tex,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &buf,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(bpr),
                rows_per_image: Some(h),
            },
        },
        wgpu::Extent3d {
            width: w,
            height: h,
            depth_or_array_layers: 1,
        },
    );
    queue.submit(Some(enc.finish()));
    let slice = buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    let _ = device.poll(wgpu::PollType::wait_indefinitely());
    rx.recv().unwrap().expect("map readback");
    let data = slice.get_mapped_range().expect("mapped range");
    let bgra = tex.format() == wgpu::TextureFormat::Bgra8Unorm
        || tex.format() == wgpu::TextureFormat::Bgra8UnormSrgb;
    let mut rgb = Vec::with_capacity((w * h * 3) as usize);
    for y in 0..h {
        let row = &data[(y * bpr) as usize..(y * bpr + w * 4) as usize];
        for px in row.chunks(4) {
            if bgra {
                rgb.extend_from_slice(&[px[2], px[1], px[0]]);
            } else {
                rgb.extend_from_slice(&[px[0], px[1], px[2]]);
            }
        }
    }
    drop(data);
    buf.unmap();
    (w, h, rgb)
}

/// A PNG of `rgb`. An error, never a panic: a player saves its shots
/// with this from a key handler, where a panic aborts the process, and
/// the directory may not be writable.
pub fn write_png(path: &str, w: u32, h: u32, rgb: &[u8]) -> std::io::Result<()> {
    let file = std::fs::File::create(path)?;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header()?.write_image_data(rgb)?;
    Ok(())
}
