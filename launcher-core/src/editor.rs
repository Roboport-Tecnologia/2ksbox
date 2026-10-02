//! The shader profile editor's model, and the preset collection behind
//! it: pick a `.slangp`, list its parameters
//! (`shader_profile::parameter_meta`), override any of them and leave
//! the rest at the preset's own default, and save the result as a
//! profile.
//!
//! A parameter the user didn't touch is left out rather than written at
//! its current value. A profile that names only what the user moved
//! still means the right thing after the preset gains parameters or
//! changes a default.
//!
//! Two rules that two front ends each once got one of wrong:
//!
//! * **Only an overridden row can move.** A greyed-out slider still
//!   reports a value, and several presets have defaults off their own
//!   step grid (crt-lottes: `warpX` 0.031, step 0.01). Without the guard
//!   in `set_value`, opening such a preset overrode those parameters with
//!   the snapped value.
//! * **Saving a new profile keeps the overrides.** `shader_library::create`
//!   reserves the `<slug>.toml` and writes a bare profile, so the
//!   overrides the editor collected have to go into the same file
//!   straight after. The retired egui build dropped them
//!   (`create(…).map(|_| ())`).

use crate::browse::Filter;
use crate::shader_library;
use crate::shader_profile::{self, ParamMeta, ShaderProfile};
use crate::shader_source::{self, Download, Status};
use std::path::{Path, PathBuf};

pub const PRESET_FILTER: Filter<'static> = ("Shader presets", &["slangp"]);
pub const IMAGE_FILTER: Filter<'static> = ("Images", &["png", "jpg", "jpeg", "bmp"]);

/// What the preset-collection row says, on both screens. The profile
/// list is where someone discovers they have no shaders at all, and the
/// editor is where an empty preset field stops them mid-profile.
pub enum PresetState {
    /// A collection is on disk: nothing to say, the picker just works.
    Ready(PathBuf),
    /// None yet, and here is where the button would put one.
    Missing { install_dir: PathBuf, size: &'static str },
    /// A download is running; the megabytes so far.
    Downloading(f64),
    Failed(String),
}

/// The preset collection this launcher can offer, and a download of it
/// if one is running. The directory is cached, because finding it walks
/// the collection's top two levels (`shader_source::has_presets`), which
/// is cheap once but not sixty times a second.
#[derive(Default)]
pub struct Presets {
    dir: Option<PathBuf>,
    looked: bool,
    download: Option<Download>,
}

impl Presets {
    /// Where the collection is, or `None`. An empty preset field's
    /// "Browse…" also opens here, since nobody navigates to a `.slangp`
    /// by hand.
    pub fn dir(&mut self) -> Option<PathBuf> {
        if !self.looked {
            self.dir = shader_source::presets_dir();
            self.looked = true;
        }
        self.dir.clone()
    }

    pub fn start_download(&mut self) {
        self.download = Some(Download::start(shader_source::install_dir()));
    }

    /// Drop the cached answer, so the next `dir`/`state` looks again.
    ///
    /// The cache is otherwise kept for the life of the process, and the
    /// first-run offer (`firstrun.rs`) can also put a collection on disk
    /// while this model is alive. Without this, accepting the offer left
    /// the profile manager showing "No shader presets on this machine"
    /// over a collection that had just been downloaded.
    pub fn forget(&mut self) {
        self.dir = None;
        self.looked = false;
    }

    /// The row's current state, advancing a finished download into the
    /// cached directory on the way past. Safe to call as often as a
    /// front end likes, once per frame or from a timer.
    pub fn state(&mut self) -> PresetState {
        if let Some(download) = &self.download {
            match download.status() {
                Status::Running(bytes) => return PresetState::Downloading(bytes as f64 / 1_000_000.0),
                Status::Done(dir) => {
                    self.download = None;
                    self.dir = Some(dir.clone());
                    self.looked = true;
                    return PresetState::Ready(dir);
                }
                Status::Failed(err) => {
                    // Kept, not cleared: the message stays on screen with
                    // a "Try again" beside it until the user acts.
                    return PresetState::Failed(err);
                }
            }
        }
        match self.dir() {
            Some(dir) => PresetState::Ready(dir),
            None => PresetState::Missing {
                install_dir: shader_source::install_dir(),
                size: shader_source::DOWNLOAD_SIZE,
            },
        }
    }

    /// Whether a download is running or has failed without being
    /// retried. While it is, a front end keeps its timer going.
    pub fn download_active(&self) -> bool {
        self.download.is_some()
    }
}

#[derive(Default)]
pub struct Editor {
    /// Whether the editor is up.
    pub open: bool,
    pub name: String,
    pub preset_path: String,
    /// A screenshot to preview the shader against.
    pub preview_image_path: String,
    /// What the last `save` refused, for the form to show.
    pub error: Option<String>,

    /// `None` for a new profile; `Some(path)` to save back in place.
    path: Option<PathBuf>,
    /// The last preset path successfully parsed, so parameters are
    /// re-read (and slider state re-derived) only when it changes, not on
    /// every frame the field is drawn.
    parsed_preset: Option<PathBuf>,
    params: Vec<ParamMeta>,
    /// One entry per `params`, in the same order: `Some(value)` when
    /// overridden. Indexed in lockstep rather than keyed by name so a
    /// slider drag doesn't need a map lookup per frame.
    overrides: Vec<Option<f32>>,
    parse_error: Option<String>,
}

impl Editor {
    /// Open on a new, empty profile.
    pub fn new_profile(&mut self) {
        *self = Editor { open: true, ..Default::default() };
    }

    /// Open on an existing one.
    pub fn edit(&mut self, path: PathBuf, profile: &ShaderProfile) {
        *self = Editor {
            open: true,
            name: profile.name.clone(),
            preset_path: profile.preset.display().to_string(),
            path: Some(path),
            ..Default::default()
        };
        self.reparse();
        // Line the overrides up with the freshly parsed parameters after
        // `reparse`, so a preset that dropped one since the profile was
        // saved doesn't leave a dangling override.
        for (meta, over) in self.params.iter().zip(self.overrides.iter_mut()) {
            if let Some(&v) = profile.params.get(&meta.id) {
                *over = Some(v);
            }
        }
    }

    /// The same, from a path alone, for a front end that addresses its
    /// windows by path rather than by a profile it already holds.
    pub fn edit_path(&mut self, path: PathBuf) {
        match ShaderProfile::load(&path) {
            Ok(profile) => self.edit(path, &profile),
            Err(e) => {
                *self = Editor { open: true, ..Default::default() };
                self.error = Some(format!("{}: {e}", path.display()));
            }
        }
    }

    /// Open the editor pre-filled with a preset and preview image, with
    /// no saved profile behind it. The front end's debug hook uses it to
    /// screenshot the real editor with no GUI click.
    pub fn open_with(&mut self, preset_path: String, preview_image_path: String) {
        *self = Editor { open: true, preset_path, preview_image_path, ..Default::default() };
        self.reparse();
    }

    /// Re-read the preset's parameters if the path changed. Cheap enough
    /// to call from a field's commit handler or on
    /// every frame.
    pub fn reparse(&mut self) {
        let trimmed = self.preset_path.trim();
        if trimmed.is_empty() {
            self.params.clear();
            self.overrides.clear();
            self.parsed_preset = None;
            self.parse_error = None;
            return;
        }
        let path = Path::new(trimmed);
        if self.parsed_preset.as_deref() == Some(path) {
            return;
        }
        self.parsed_preset = Some(path.to_path_buf());
        match shader_profile::parameter_meta(path) {
            Ok(params) => {
                self.overrides = vec![None; params.len()];
                self.params = params;
                self.parse_error = None;
            }
            Err(e) => {
                self.params.clear();
                self.overrides.clear();
                self.parse_error = Some(e);
            }
        }
    }

    pub fn params(&self) -> &[ParamMeta] {
        &self.params
    }

    /// One row: its metadata and its override, if any.
    pub fn param(&self, row: usize) -> Option<(&ParamMeta, Option<f32>)> {
        Some((self.params.get(row)?, *self.overrides.get(row)?))
    }

    /// The value a row's slider shows: the override where there is one,
    /// the preset's own default otherwise.
    pub fn value(&self, row: usize) -> Option<f32> {
        let (meta, over) = self.param(row)?;
        Some(over.unwrap_or(meta.default))
    }

    pub fn is_overridden(&self, row: usize) -> bool {
        self.overrides.get(row).map(Option::is_some).unwrap_or(false)
    }

    /// The description worth showing under a row: none when it merely
    /// repeats the parameter's own id.
    pub fn description(&self, row: usize) -> Option<&str> {
        let meta = self.params.get(row)?;
        (!meta.description.is_empty() && meta.description != meta.id).then_some(meta.description.as_str())
    }

    /// Override (or stop overriding) one parameter. Starting to override
    /// seeds the preset's own default, so the slider doesn't jump.
    pub fn set_override(&mut self, row: usize, enabled: bool) {
        let (Some(meta), Some(over)) = (self.params.get(row), self.overrides.get_mut(row)) else {
            return;
        };
        *over = if enabled { Some(meta.default) } else { None };
    }

    /// Move an overridden parameter. Ignored for a row that isn't
    /// overridden; this module's header says why.
    pub fn set_value(&mut self, row: usize, value: f32) {
        if let Some(over @ Some(_)) = self.overrides.get_mut(row) {
            *over = Some(value);
        }
    }

    pub fn parse_error(&self) -> Option<&str> {
        self.parse_error.as_deref()
    }

    /// Whether there is anything the preview could render yet.
    pub fn renderable(&self) -> bool {
        self.parse_error.is_none()
            && !self.params.is_empty()
            && !self.preset_path.trim().is_empty()
            && !self.preview_image_path.trim().is_empty()
    }

    /// The values the preview actually renders with: the override where
    /// there is one, the preset's own default everywhere else.
    pub fn effective(&self) -> Vec<(String, f32)> {
        self.params
            .iter()
            .zip(&self.overrides)
            .map(|(meta, over)| (meta.id.clone(), over.unwrap_or(meta.default)))
            .collect()
    }

    /// The profile the current fields describe: only the overridden
    /// parameters, by design.
    pub fn build(&self) -> ShaderProfile {
        let mut profile = ShaderProfile::new(self.name.clone(), self.preset_path.trim().into());
        for (meta, over) in self.params.iter().zip(self.overrides.iter()) {
            if let Some(v) = over {
                profile.params.insert(meta.id.clone(), *v);
            }
        }
        profile
    }

    /// Write the profile, closing the editor on success. `false` leaves
    /// it open with `error` saying why.
    pub fn save(&mut self, profiles_dir: &Path) -> bool {
        if self.name.trim().is_empty() {
            self.error = Some("A name is required.".into());
            return false;
        }
        if self.preset_path.trim().is_empty() {
            self.error = Some("A preset is required.".into());
            return false;
        }
        let profile = self.build();
        let result = match &self.path {
            Some(path) => profile.save(path),
            // `create` reserves the `<slug>.toml` and writes a bare
            // profile; the overrides go into the same file straight
            // after, or a new profile would save none of them.
            None => shader_library::create(profiles_dir, profile.name.clone(), profile.preset.clone())
                .and_then(|path| profile.save(&path)),
        };
        match result {
            Ok(()) => {
                self.error = None;
                self.open = false;
                true
            }
            Err(e) => {
                self.error = Some(e.to_string());
                false
            }
        }
    }
}
