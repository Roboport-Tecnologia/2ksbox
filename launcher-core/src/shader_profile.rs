//! Shader profiles (doc 07 settings taxonomy): a named, reusable shader
//! preset selection plus the parameter overrides tweaked on top of it,
//! independent of any one machine. A machine references a profile by its
//! id, the file stem (`bundle::Machine::shader_profile`), and `player.rs`
//! resolves that into the `--shader`/`--shader-params` the player binary
//! takes.
//!
//! Only overridden parameters are stored. Everything else stays at the
//! preset's own default, so a profile stays valid when the preset gains
//! new parameters.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderProfile {
    pub name: String,
    /// A libretro slang preset (`.slangp`), ours under `shaders/` or
    /// upstream under `third_party/slang-shaders/`.
    pub preset: PathBuf,
    /// Parameter overrides, keyed by the preset's own parameter id
    /// (`#pragma parameter <id> ...`). `BTreeMap` so the saved TOML and
    /// the `--shader-params` command line are both in a stable order.
    #[serde(default)]
    pub params: BTreeMap<String, f32>,
}

impl ShaderProfile {
    pub fn new(name: String, preset: PathBuf) -> Self {
        ShaderProfile { name, preset, params: BTreeMap::new() }
    }

    pub fn load(path: &Path) -> std::io::Result<ShaderProfile> {
        let text = std::fs::read_to_string(path)?;
        toml::from_str(&text).map_err(std::io::Error::other)
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let text = toml::to_string_pretty(self).map_err(std::io::Error::other)?;
        crate::paths::write_atomic(path, text)
    }

    /// The player's `--shader-params` value (`name=value,name=value`,
    /// sorted by name), or `None` if nothing is overridden.
    pub fn params_arg(&self) -> Option<String> {
        if self.params.is_empty() {
            return None;
        }
        Some(
            self.params
                .iter()
                .map(|(k, v)| format!("{k}={v}"))
                .collect::<Vec<_>>()
                .join(","),
        )
    }
}

/// A preset's parameters, as declared by the shader source (`#pragma
/// parameter`) with any value the preset file itself already overrides.
/// This is what the profile manager needs to draw a slider per parameter
/// (id, description, default, range, step). Introspection only; the
/// filter chain itself is built elsewhere.
pub struct ParamMeta {
    pub id: String,
    pub description: String,
    pub default: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub step: f32,
}

/// Parse `preset` and list its parameters, sorted by id. `Err` covers a
/// bad path or a preset librashader can't parse (e.g. one of upstream's
/// `BROKEN_SHADERS.md` entries). The manager shows the error inline
/// rather than letting a bad preset crash the picker.
pub fn parameter_meta(preset: &Path) -> Result<Vec<ParamMeta>, String> {
    let parsed = librashader::presets::ShaderPreset::try_parse(
        preset,
        librashader::presets::ShaderFeatures::NONE,
    )
    .map_err(|e| format!("{e}"))?;
    let mut params: Vec<ParamMeta> = librashader::presets::get_parameter_meta(&parsed)
        .map_err(|e| format!("{e}"))?
        .map(|p| ParamMeta {
            id: p.id.to_string(),
            description: p.description,
            default: p.initial,
            minimum: p.minimum,
            maximum: p.maximum,
            step: p.step,
        })
        .collect();
    params.sort_by(|a, b| a.id.cmp(&b.id));
    Ok(params)
}

/// Parse a `--preview-shader`-style `name=value,name=value` list, the
/// format `ShaderProfile::params_arg` produces and the player's
/// `--shader-params` consumes. A malformed entry is skipped with a
/// stderr line, as in the player's own `parse_shader_params`.
pub fn parse_params(s: &str) -> Vec<(String, f32)> {
    s.split(',')
        .filter(|entry| !entry.trim().is_empty())
        .filter_map(|entry| {
            let (name, value) = entry.split_once('=')?;
            match value.trim().parse::<f32>() {
                Ok(v) => Some((name.trim().to_string(), v)),
                Err(e) => {
                    eprintln!("[shader-profile] bad param entry {entry:?}: {e}");
                    None
                }
            }
        })
        .collect()
}
