//! Mode analysis (doc 03, "Pixel accuracy rules" 2 and 3): what a guest
//! framebuffer size meant on a monitor of the era, and what the CRT presets
//! have to be told about it.
//!
//! Two facts the framebuffer itself does not carry:
//!
//! * **Pixel aspect.** 320x200 is a 4:3 picture drawn with pixels taller
//!   than they are wide, not a 1.6:1 one. The geometry stage takes the
//!   aspect from here, never from width/height (rule 2).
//! * **Scanline count.** A real VGA double-scans its short modes: the CRTC
//!   repeats every row so 320x200 lands on the standard 400-line raster.
//!   A scanline shader handed a 200-line surface draws 200 fat scanlines
//!   where the tube drew 400 (rule 3), so it has to be told.
//!
//! The table is the exception list. A mode not in it is taken to have square
//! pixels, which is right for every SVGA mode and for anything modern a guest
//! might set: forcing 4:3 on an unlisted size would distort a widescreen one,
//! and every 4:3 mode that *does* have square pixels comes out at 4:3 from
//! that rule anyway. What the table has to carry is the modes where the two
//! disagree (the VGA's 200-, 240-, 350- and 400-line modes, whose pixels
//! are not square), plus a name for each, and the place a correction goes
//! when one is measured against the reference CRT (doc 09).
//!
//! Two things are rules rather than entries, because a table of exact sizes
//! cannot state them. Double-scanning: the CRTC sets its bit below ~300
//! lines. And the VGA raster itself (`vga_raster`): a size at one of the
//! four VGA widths and at most 480 lines is a 4:3 picture whether or not its
//! line count is one of the round ones, because a guest can scan any number
//! of lines onto that one raster and QEMU's text path does not even report
//! the number it scanned (720x396 for a 400-line raster with a 12-line
//! character cell, which is XP's text-mode setup).

/// What a guest framebuffer size meant on the tube.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    /// Aspect of the picture the mode was meant to fill.
    pub display_aspect: f32,
    /// Lines the CRT actually scanned: twice `height` when double-scanned.
    pub scanlines: u32,
    /// The CRTC repeated every row onto a taller raster.
    pub doubled: bool,
    /// Name for the log; "" for a mode that is not in the table.
    pub label: &'static str,
}

const DAR_4_3: f32 = 4.0 / 3.0;
const DAR_5_4: f32 = 5.0 / 4.0;

/// Below this many lines the VGA CRTC sets its double-scan bit, putting a
/// 200- or 240-line mode on the 400/480-line raster. 350-, 400- and
/// 480-line modes have their own vertical timing and are scanned once.
const DOUBLE_SCAN_BELOW: u32 = 300;

/// The widths a VGA can display: the 8- and 9-dot character clocks and the
/// halved low-resolution ones. Every mode the CRTC scans at one of these is
/// a 4:3 picture on the tube whatever its line count (see `vga_raster`).
const VGA_WIDTHS: &[u32] = &[320, 360, 640, 720];

/// The tallest raster a VGA scans (480 lines). Above it a size is an SVGA
/// mode, whose pixels are square.
const VGA_LINES_MAX: u32 = 480;

/// Is this size a VGA raster, a 4:3 picture whose pixels are not square?
///
/// The table's exact sizes cannot answer this on their own, because the size
/// the player is handed is not always the raster the CRTC scans. QEMU's text
/// path reports `rows x cheight`, and the last partial row is dropped: a
/// 400-line raster with a 12-line character cell arrives as **720x396**,
/// which is what XP's text-mode setup programs, and an exact-match table
/// then calls it an unlisted 1.818:1 picture and draws the install
/// stretched. The raster did not change. A VGA has one raster and four
/// widths, so the width plus a line count in range is what identifies it,
/// and the odd line counts (350 with an 8-line cell -> 344, 400 with a
/// 14-line one -> 392) come out at 4:3 like the round ones.
fn vga_raster(width: u32, height: u32) -> bool {
    VGA_WIDTHS.contains(&width) && height > 0 && height <= VGA_LINES_MAX
}

/// The era modes, by name. What has to be here is the first group, whose
/// pixels are not square and whose display aspect the square-pixel rule
/// would therefore get wrong; the rest carry only their names.
const TABLE: &[(u32, u32, f32, &str)] = &[
    // the non-square-pixel modes: a 4:3 picture on a framebuffer that is not
    (320, 200, DAR_4_3, "VGA 320x200 (mode 13h)"),
    (320, 400, DAR_4_3, "VGA 320x400"),
    (320, 480, DAR_4_3, "VGA 320x480 (mode X)"),
    (360, 240, DAR_4_3, "VGA 360x240 (mode X)"),
    (360, 480, DAR_4_3, "VGA 360x480 (mode X)"),
    (640, 200, DAR_4_3, "CGA 640x200"),
    (640, 350, DAR_4_3, "EGA 640x350 (mode 10h)"),
    (640, 400, DAR_4_3, "VGA 640x400"),
    (720, 400, DAR_4_3, "VGA text 80x25 (9-dot)"),
    // square-pixel modes, here for their names and to be swept
    (320, 240, DAR_4_3, "VGA 320x240 (mode X)"),
    (640, 480, DAR_4_3, "VGA 640x480"),
    (800, 600, DAR_4_3, "SVGA 800x600"),
    (1024, 768, DAR_4_3, "XGA 1024x768"),
    (1152, 864, DAR_4_3, "SVGA 1152x864"),
    (1280, 960, DAR_4_3, "SVGA 1280x960"),
    (1280, 1024, DAR_5_4, "SXGA 1280x1024"),
    (1600, 1200, DAR_4_3, "UXGA 1600x1200"),
];

impl Mode {
    /// Analyse a guest framebuffer size.
    pub fn analyse(width: u32, height: u32) -> Mode {
        let square = if height == 0 {
            DAR_4_3
        } else {
            width as f32 / height as f32
        };
        let (display_aspect, label) = TABLE
            .iter()
            .find(|(w, h, _, _)| *w == width && *h == height)
            .map(|(_, _, dar, label)| (*dar, *label))
            .unwrap_or_else(|| {
                if vga_raster(width, height) {
                    (DAR_4_3, "VGA raster (4:3)")
                } else {
                    (square, "")
                }
            });
        let doubled = height > 0 && height < DOUBLE_SCAN_BELOW;
        Mode {
            width,
            height,
            display_aspect,
            scanlines: if doubled { height * 2 } else { height },
            doubled,
            label,
        }
    }

    /// Width:height of one pixel on the tube; 1.0 is square. Derived, never
    /// stored: a mode's pixel aspect *is* its display aspect over the
    /// framebuffer's own ratio (320x200 -> 0.833, i.e. doc 03's 1:1.2).
    pub fn pixel_aspect(&self) -> f32 {
        if self.width == 0 || self.height == 0 {
            return 1.0;
        }
        self.display_aspect / (self.width as f32 / self.height as f32)
    }

    /// Parameter overrides that tell a CRT preset this mode's scanline count
    /// (doc 03 rule 3). The names are crt-guest-advanced's; a preset that
    /// declares none of them has no way to be told and is left at its
    /// defaults (see `Chain::has_parameter` at the call site).
    ///
    /// `vga_mode` ("VGA Single/Double Scan mode") switches that preset from
    /// its console-oriented interlace guess to the two VGA cases, and
    /// `inter` then picks between them: the preset takes the double-scan
    /// branch when `inter` is above the source height and the single-scan
    /// branch when it is at or below. Its default of 375 is a guess about
    /// which side of the line a source falls on; mode analysis knows, so it
    /// states the answer categorically instead of leaving the guess to a
    /// threshold that 720x400 text sits right on top of.
    pub fn shader_params(&self) -> Vec<(String, f32)> {
        vec![
            ("vga_mode".to_string(), 1.0),
            ("inter".to_string(), if self.doubled { 800.0 } else { 0.0 }),
        ]
    }

    /// One line for the log when the guest changes mode.
    pub fn describe(&self) -> String {
        let name = if self.label.is_empty() {
            "unlisted mode".to_string()
        } else {
            self.label.to_string()
        };
        let dar = if (self.display_aspect - DAR_5_4).abs() < 0.001 {
            "5:4".to_string()
        } else if (self.display_aspect - DAR_4_3).abs() < 0.001 {
            "4:3".to_string()
        } else {
            format!("{:.3}:1", self.display_aspect)
        };
        format!(
            "{}x{} {} — {} picture, pixel aspect {:.3}, {} scanlines{}",
            self.width,
            self.height,
            name,
            dar,
            self.pixel_aspect(),
            self.scanlines,
            if self.doubled {
                " (double-scanned)"
            } else {
                ""
            },
        )
    }
}

/// Every mode in the table, plus a size that is not in it, so the sweep
/// covers the derived fallback as well as the named entries.
pub fn sweep_sizes() -> Vec<(u32, u32)> {
    let mut v: Vec<(u32, u32)> = TABLE.iter().map(|(w, h, _, _)| (*w, *h)).collect();
    v.push((720, 396)); // a raster the text path truncated: the `vga_raster` rule
    v.push((512, 384)); // in no group: the square-pixel fallback
    v
}
