//! M0 stand-in for the guest framebuffer: 640×480 XRGB8888 test pattern.
//! Replaced by QEMU's DisplaySurface in M1; kept afterwards as a diagnostic
//! source (shader/geometry checks without booting a guest).

pub const WIDTH: usize = 640;
pub const HEIGHT: usize = 480;

// SMPTE-ish bars so channel order / gamma mistakes are obvious at a glance.
const BARS: [u32; 7] = [
    0x00c0c0c0, 0x00c0c000, 0x0000c0c0, 0x0000c000, 0x00c000c0, 0x00c00000, 0x000000c0,
];

pub struct Pattern {
    pub fb: Vec<u32>,
    frame: u64,
}

impl Pattern {
    pub fn new() -> Self {
        Self {
            fb: vec![0; WIDTH * HEIGHT],
            frame: 0,
        }
    }

    /// Render the next frame into `fb` (XRGB8888, row-major, pitch = WIDTH).
    pub fn render(&mut self) {
        let sweep = (self.frame as usize) % HEIGHT;
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                let mut px = BARS[x * BARS.len() / WIDTH];
                // 1px white border: pixel-exact edges are visible under shaders
                if x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 {
                    px = 0x00ffffff;
                }
                // moving scanline proves cadence (one sweep per 8 s at 60 Hz)
                if y == sweep {
                    px = 0x00ffffff;
                }
                self.fb[y * WIDTH + x] = px;
            }
        }
        self.frame = self.frame.wrapping_add(1);
    }
}

/// Geometry test image for the mode sweep (doc 03 rules 2 and 3): SMPTE
/// bars, a 1px border, a block of alternating single-pixel lines so
/// scanline handling is visible, and a circle drawn in *display* space.
/// The circle is an ellipse in the framebuffer, round on screen only when
/// the mode's pixel aspect has been applied.
pub fn geometry(w: usize, h: usize, display_aspect: f32) -> Vec<u32> {
    let mut fb = vec![0u32; w * h];
    let (cx, cy) = (display_aspect as f64 / 2.0, 0.5);
    // radius 0.45 of the picture height; the stroke is one guest row wide
    let (r, stroke) = (0.45f64, 1.0 / h as f64);
    for y in 0..h {
        for x in 0..w {
            let mut px = BARS[x * BARS.len() / w];
            // top-left quarter: one-pixel lines, the scanline reference
            if x < w / 4 && y < h / 4 && y % 2 == 0 {
                px = 0x00000000;
            }
            // the circle, in display space: x is stretched by the aspect
            let dx = (x as f64 + 0.5) / w as f64 * display_aspect as f64 - cx;
            let dy = (y as f64 + 0.5) / h as f64 - cy;
            let d = (dx * dx + dy * dy).sqrt();
            if (d - r).abs() < stroke {
                px = 0x00ffffff;
            }
            if x == 0 || y == 0 || x == w - 1 || y == h - 1 {
                px = 0x00ffffff;
            }
            fb[y * w + x] = px;
        }
    }
    fb
}
