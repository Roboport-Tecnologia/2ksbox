//! A window's remembered frame: the player's, one file per machine (M22),
//! and the launcher's machines window, one file in the data directory
//! (`launcher_file`).
//!
//! The launcher passes `--window-state <bundle>/window.toml`; the player
//! opens its window as it was left (where it was, its size, maximized or
//! not) and writes it when the user moves, resizes or maximizes the
//! window, and the launcher does the same with its own. The toolkit keeps
//! the frame (mitsuami's `Window::frame`): its
//! size is the restored one, never a maximized or full-screen window's,
//! and its position is in the platform's own desktop coordinates, absent
//! where a window can't know where it is (GTK, Wayland). Sizes are in
//! points (the toolkit's units), so a Retina screen gives the guest twice
//! as many pixels. A Windows 11 guest takes its screen from the window's
//! size when it boots, so this is also the resolution it gets next time.

use std::path::{Path, PathBuf};

/// The launcher's machines window, `launcher-window.toml` in the data
/// directory.
pub fn launcher_file() -> Option<PathBuf> {
    crate::paths::data_dir().map(|d| d.join("launcher-window.toml"))
}

/// A window as it was left.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WindowState {
    /// Its top-left corner, in the platform's coordinates: a value to hand
    /// back, not to compute with.
    pub position: Option<(f32, f32)>,
    /// Its restored content size in points.
    pub size: (f32, f32),
    pub maximized: bool,
}

/// The remembered window, if the file has a size. A file from before
/// positions were kept has only `width` and `height`.
pub fn load(path: &Path) -> Option<WindowState> {
    let text = std::fs::read_to_string(path).ok()?;
    let (mut width, mut height, mut x, mut y, mut maximized) = (None, None, None, None, false);
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        let value = value.trim();
        match key.trim() {
            "maximized" => maximized = value == "true",
            key => {
                let Ok(v) = value.parse::<f32>() else { continue };
                if !v.is_finite() {
                    continue;
                }
                match key {
                    "width" if v >= 1.0 => width = Some(v),
                    "height" if v >= 1.0 => height = Some(v),
                    "x" => x = Some(v),
                    "y" => y = Some(v),
                    _ => {}
                }
            }
        }
    }
    Some(WindowState { position: x.zip(y), size: (width?, height?), maximized })
}

/// Remember a window. Written beside and renamed over, so a process killed
/// mid-write leaves the old frame, not half a file.
pub fn save(path: &Path, state: &WindowState) {
    let mut text = String::from("# A window as it was left: its restored size in points,\n");
    text.push_str("# where it was in the platform's coordinates, and whether it was maximized.\n");
    text.push_str(&format!("width = {}\nheight = {}\n", state.size.0.round(), state.size.1.round()));
    if let Some((x, y)) = state.position {
        text.push_str(&format!("x = {}\ny = {}\n", x.round(), y.round()));
    }
    text.push_str(&format!("maximized = {}\n", state.maximized));
    let tmp = path.with_extension("toml.tmp");
    let result = std::fs::write(&tmp, text).and_then(|()| std::fs::rename(&tmp, path));
    if let Err(e) = result {
        eprintln!("[window] frame not remembered in {}: {e}", path.display());
    }
}

/// A window's frame on its way to its file: written when it is the
/// user's, not a frame of the first two seconds (the platform's opening:
/// the frame asked for, then, on macOS, that frame shrunk to fit a screen
/// too small for it) and not the one already written. mitsuami keeps the
/// restored frame, so a maximized or full-screen window changes only
/// `maximized`. A drag reports every step, so a front end writes once the
/// frame has rested for `SETTLE`: `changed` says when to start that wait,
/// and `settled` ends it.
pub struct Keeper {
    path: PathBuf,
    /// The window as it opened or was last remembered: a change from it
    /// is the user's.
    remembered: Option<WindowState>,
    /// When the frame was first followed.
    opened_at: Option<std::time::Instant>,
    /// A write is waiting out a move or a resize.
    saving: bool,
}

const OPENING: std::time::Duration = std::time::Duration::from_secs(2);

impl Keeper {
    /// How long a frame rests before it is written.
    pub const SETTLE: std::time::Duration = std::time::Duration::from_millis(500);

    pub fn new(path: PathBuf) -> Keeper {
        Keeper { path, remembered: None, opened_at: None, saving: false }
    }

    /// The window as it was left, to open it so.
    pub fn load(&self) -> Option<WindowState> {
        load(&self.path)
    }

    /// The toolkit's frame, as it changes. True when the front end should
    /// call `settled` after `SETTLE`.
    pub fn changed(&mut self, state: WindowState) -> bool {
        if !(state.size.0 >= 1.0 && state.size.1 >= 1.0) {
            return false;
        }
        let opening = self.opened_at.get_or_insert_with(std::time::Instant::now).elapsed() < OPENING;
        if self.remembered == Some(state) {
            return false;
        }
        self.remembered = Some(state);
        if opening || self.saving {
            return false;
        }
        self.saving = true;
        true
    }

    /// The wait is over: write the frame as it is now.
    pub fn settled(&mut self) {
        self.saving = false;
        if let Some(state) = self.remembered {
            save(&self.path, &state);
        }
    }
}
