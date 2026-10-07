//! The two headless passes over the display path, no guest: doc 03's mode
//! sweep (`--mode-sweep <dir>`) and doc 09's calibration shading
//! (`--calib <bmp|dir>`). Both render into a fixed surface
//! (`Gpu::force_surface`) and read the chain's output back, so neither
//! depends on what the compositor handed the window.

use crate::gpu::Gpu;
use crate::{mode, pattern};

/// Shade the calibration patterns (doc 09): each BMP that
/// `tools/crtcal-render` wrote goes through the loaded preset at the size it
/// was drawn at, and the shaded frame lands beside it as a PNG. That is the
/// other half of the comparison: one photograph of the tube showing the
/// pattern, one shaded frame of the same pattern, held side by side.
pub struct Calib {
    pub(crate) files: Vec<std::path::PathBuf>,
    pub(crate) i: usize,
    pub(crate) last_surface: (u32, u32),
}

/// The 24-bit bottom-up BMPs `crtcal-render` writes. Deliberately not a
/// general decoder: anything else is a mistake worth reporting, not
/// something to guess at.
pub(crate) fn read_bmp(path: &std::path::Path) -> Result<(u32, u32, Vec<u32>), String> {
    let d = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if d.len() < 54 || d[0] != b'B' || d[1] != b'M' {
        return Err(format!("{}: not a BMP", path.display()));
    }
    let u32le = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]);
    let (off, w, h) = (u32le(10) as usize, u32le(18), u32le(22));
    let bpp = u16::from_le_bytes([d[28], d[29]]);
    if bpp != 24 {
        return Err(format!("{}: {bpp}-bit BMP, expected 24", path.display()));
    }
    let stride = (w as usize * 3 + 3) & !3;
    if off + stride * h as usize > d.len() {
        return Err(format!("{}: truncated", path.display()));
    }
    let mut px = vec![0u32; (w * h) as usize];
    for y in 0..h as usize {
        let src = off + (h as usize - 1 - y) * stride; // BMP rows run bottom-up
        for x in 0..w as usize {
            let p = &d[src + x * 3..src + x * 3 + 3];
            px[y * w as usize + x] =
                (p[2] as u32) << 16 | (p[1] as u32) << 8 | p[0] as u32;
        }
    }
    Ok((w, h, px))
}

/// The surface the sweep fits its pictures into: 1600x1200, the tallest mode
/// in the table, needs 2400 lines for its 1200 scanlines to be countable.
pub(crate) const SWEEP_SURFACE: (u32, u32) = (3200, 2400);

/// The mode sweep (doc 03's "The mode sweep", M2): step through every mode
/// the table knows, upload a geometry pattern at that size and run the real
/// display path (mode analysis, the geometry stage, the loaded preset),
/// then check what each did with it. No guest and no QEMU. The boundary
/// under test is the player's own display path.
pub struct Sweep {
    out: std::path::PathBuf,
    pub(crate) sizes: Vec<(u32, u32)>,
    pub(crate) i: usize,
    pub(crate) fails: Vec<String>,
    /// a preset was asked for on the command line
    want_chain: bool,
    /// surface size at the last redraw: the compositor settles on one a
    /// frame or two in, and a mode measured against a size that is about to
    /// change is measured against nothing
    pub(crate) last_surface: (u32, u32),
}

impl Sweep {
    pub(crate) fn new(out: std::path::PathBuf, want_chain: bool) -> Sweep {
        Sweep {
            out,
            sizes: mode::sweep_sizes(),
            i: 0,
            fails: Vec::new(),
            want_chain,
            last_surface: (0, 0),
        }
    }
}

/// Upload the geometry pattern for the mode about to be checked.
pub(crate) fn sweep_upload(gpu: &mut Gpu, s: &Sweep) {
    let (w, h) = s.sizes[s.i];
    let m = mode::Mode::analyse(w, h);
    let fb = pattern::geometry(w as usize, h as usize, m.display_aspect);
    gpu.upload(&fb, w, h);
}

/// The number of scanlines a shaded frame actually has: count the bright
/// bands down one column of flat picture (left edge, below the line-pair
/// block and clear of the circle) and scale to the full height.
///
/// Counted rather than measured as a repeat period, because the pitch need
/// not be a whole number of output pixels. 400 scanlines in a 2200-pixel
/// viewport alternate 5 and 6 pixels, and their *period* is then two
/// scanlines, which would read as half the count.
fn measure_scanlines(w: u32, h: u32, rgb: &[u8]) -> Option<u32> {
    let x = (w as f32 * 0.08) as u32;
    let (y0, y1) = (h * 2 / 5, h * 7 / 10);
    let lum: Vec<f32> = (y0..y1)
        .map(|y| {
            let i = ((y * w + x) * 3) as usize;
            0.299 * rgb[i] as f32 + 0.587 * rgb[i + 1] as f32 + 0.114 * rgb[i + 2] as f32
        })
        .collect();
    let (lo, hi) = lum.iter().fold((f32::MAX, f32::MIN), |(a, b), v| (a.min(*v), b.max(*v)));
    if hi - lo < 4.0 {
        return None; // no scanline structure to count
    }
    // upward crossings of the midpoint, with hysteresis so the shader's own
    // dithering cannot add a band
    let mid = (lo + hi) / 2.0;
    let (up, down) = (mid + (hi - lo) * 0.15, mid - (hi - lo) * 0.15);
    let mut crossings: Vec<f32> = Vec::new();
    let mut armed = false;
    for (i, v) in lum.iter().enumerate() {
        if *v < down {
            armed = true;
        } else if *v > up && armed {
            // where the rise crossed the midpoint, to sub-pixel precision
            let (prev, here) = (lum[i.saturating_sub(1)], *v);
            let frac = if here > prev {
                ((mid - prev) / (here - prev)).clamp(0.0, 1.0)
            } else {
                0.0
            };
            crossings.push(i as f32 - 1.0 + frac);
            armed = false;
        }
    }
    if crossings.len() < 4 {
        return None;
    }
    // the pitch from the span between the first and last band, not from the
    // count over the sampled strip: the strip's own length would round in
    let pitch = (crossings[crossings.len() - 1] - crossings[0]) / (crossings.len() - 1) as f32;
    Some((h as f32 / pitch).round() as u32)
}

/// Check the frame just rendered, dump it, and step to the next mode.
/// Returns true when the sweep is finished.
pub(crate) fn sweep_step(gpu: &mut Gpu, s: &mut Sweep) -> bool {
    let (w, h) = s.sizes[s.i];
    let m = mode::Mode::analyse(w, h);
    if s.last_surface != gpu.surface_size() {
        s.last_surface = gpu.surface_size();
        return false; // measure this mode once the surface has settled
    }
    let (vx, vy, vw, vh) = gpu.viewport();
    let (sw, sh) = gpu.surface_size();
    let (sw, sh) = (sw as f32, sh as f32);
    let mut bad: Vec<String> = Vec::new();

    // rule 2: the picture on screen has the mode's display aspect, whatever
    // the framebuffer's own ratio is
    let got = vw / vh;
    if (got - m.display_aspect).abs() > m.display_aspect * 0.005 {
        bad.push(format!(
            "on-screen aspect {got:.4}, want {:.4}",
            m.display_aspect
        ));
    }
    // it is inside the window, centered
    if vw > sw + 0.5 || vh > sh + 0.5 || vx < -0.5 || vy < -0.5 {
        bad.push(format!("viewport {vw}x{vh}+{vx}+{vy} does not fit {sw}x{sh}"));
    }
    // rule 4: where a whole multiple of the scanlines fits, the height is one
    let scale = vh / m.scanlines as f32;
    if scale >= 1.0 && (scale - scale.round()).abs() > 0.001 {
        bad.push(format!("scanline pitch {scale:.4} px is not a whole number"));
    }
    // rule 3: the preset was told this mode's scanline count (skipped under
    // the PLAYER_MODE_PARAMS=0 control, where by definition it was not)
    let params = m.shader_params();
    let control = std::env::var("PLAYER_MODE_PARAMS").as_deref() == Ok("0");
    match gpu.chain.as_ref() {
        _ if control => {}
        Some(chain) if params.iter().all(|(n, _)| chain.has_parameter(n)) => {
            for (name, want) in &params {
                let got = chain.parameter(name).unwrap_or(f32::NAN);
                if (got - want).abs() > 0.001 {
                    bad.push(format!("preset parameter {name} is {got}, want {want}"));
                }
            }
        }
        Some(_) => {} // preset has no scanline control; guest_surface_changed said so
        None if s.want_chain => bad.push("the preset did not load".to_string()),
        None => {}
    }

    // rule 3, end to end: count the scanlines in the frame the preset just
    // drew and hold them against the ones the tube scanned. The dump is for
    // the eye. The circle is round when the geometry is right.
    let mut drawn = String::new();
    if let Some(tex) = gpu.chain.as_ref().and_then(|c| c.output_texture()) {
        let (ow, oh, rgb) = shader_chain::read_texture(&gpu.device, &gpu.queue, tex);
        // Below three output pixels per scanline there is nothing to count:
        // at two the preset has no room for a gap and draws a flat field
        // (measured: one LSB of modulation at 1152x864 and above).
        let countable = oh >= m.scanlines * 3;
        let measured = if countable {
            measure_scanlines(ow, oh, &rgb)
        } else {
            None
        };
        if let Some(got) = measured {
            drawn = format!(", {got} drawn");
        }
        if countable && !control {
            match measured {
                Some(got) if got == m.scanlines => {}
                Some(got) => bad.push(format!(
                    "the frame has {got} scanlines, the tube scanned {}",
                    m.scanlines
                )),
                None => bad.push("the frame has no scanline structure to count".to_string()),
            }
        }
        let path = s.out.join(format!("{w}x{h}.png"));
        if let Err(e) = shader_chain::write_png(&path.to_string_lossy(), ow, oh, &rgb) {
            bad.push(format!("{}: {e}", path.display()));
        }
    }

    if bad.is_empty() {
        println!("  ok   {} → viewport {vw:.0}x{vh:.0}{drawn}", m.describe());
    } else {
        println!("  FAIL {} → viewport {vw:.0}x{vh:.0}{drawn}", m.describe());
        for b in &bad {
            println!("         {b}");
        }
        s.fails.push(format!("{w}x{h}: {}", bad.join("; ")));
    }

    s.i += 1;
    s.i >= s.sizes.len()
}
