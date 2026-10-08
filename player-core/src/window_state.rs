//! The player window's remembered size, one file per machine (M22).
//!
//! The launcher passes `--window-state <bundle>/window.toml`; the player
//! opens its window at the size in it and writes it when the user resizes
//! the window. Sizes are in points (the toolkit's units), so a Retina
//! screen gives the guest twice as many pixels. A Windows 11 guest takes
//! its screen from the window's size when it boots, so this is also the
//! resolution it gets next time.

use std::path::Path;

/// The remembered content size in points, if the file has one.
pub fn load(path: &Path) -> Option<(f32, f32)> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut width = None;
    let mut height = None;
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        let value: f32 = match value.trim().parse() {
            Ok(v) if v >= 1.0 => v,
            _ => continue,
        };
        match key.trim() {
            "width" => width = Some(value),
            "height" => height = Some(value),
            _ => {}
        }
    }
    Some((width?, height?))
}

/// Remember a content size in points. Written beside and renamed over,
/// so a player killed mid-write leaves the old size, not half a file.
pub fn save(path: &Path, width: f32, height: f32) {
    let text = format!(
        "# The player window's size in points (M22), written when it is resized.\n\
         width = {}\nheight = {}\n",
        width.round(),
        height.round()
    );
    let tmp = path.with_extension("toml.tmp");
    let result = std::fs::write(&tmp, text).and_then(|()| std::fs::rename(&tmp, path));
    if let Err(e) = result {
        eprintln!("[window] size not remembered in {}: {e}", path.display());
    }
}
