//! The shader profile library: a directory of `<slug>.toml` files, one per
//! profile. It is flat, unlike the machine library's per-bundle
//! subdirectories (`library.rs`), since a profile has no disk image
//! beside it to keep together. Like the machine library it is a plain,
//! documented directory with no database (doc 07).

use crate::shader_profile::ShaderProfile;
use std::path::{Path, PathBuf};

/// The default profile directory: the platform data dir plus
/// `shader-profiles`, alongside `library::default_dir`'s `machines`.
/// `LAUNCHER_SHADER_PROFILES_DIR` overrides it.
pub fn default_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_SHADER_PROFILES_DIR") {
        return dir.into();
    }
    crate::paths::data_dir().map(|d| d.join("shader-profiles")).unwrap_or_else(|| PathBuf::from("shader-profiles"))
}

pub struct ProfileEntry {
    pub path: PathBuf,
    pub profile: ShaderProfile,
    /// The library's default (`default_id`): what a machine on the app
    /// default plays with.
    pub is_default: bool,
}

/// The file beside the profiles naming the library's default: one line,
/// a profile id. A machine whose `shader_profile` is `None` ("(default)"
/// in every picker) plays with this profile; with no file, or a file
/// naming a profile that is gone, it plays unshaded, as before there was
/// a default. One file rather than a flag in each profile, so an editor
/// that rewrites a profile cannot drop the mark and there is never a
/// second default.
pub const DEFAULT_FILE: &str = "default-profile.txt";

/// The default profile's id, when the file names a profile that exists.
pub fn default_id(dir: &Path) -> Option<String> {
    let id = std::fs::read_to_string(dir.join(DEFAULT_FILE)).ok()?;
    let id = id.trim();
    (!id.is_empty() && dir.join(format!("{id}.toml")).is_file()).then(|| id.to_string())
}

/// Mark `id` as the library's default, or `None` for no default.
pub fn set_default(dir: &Path, id: Option<&str>) -> std::io::Result<()> {
    let file = dir.join(DEFAULT_FILE);
    match id {
        Some(id) => {
            std::fs::create_dir_all(dir)?;
            std::fs::write(file, format!("{id}\n"))
        }
        None => match std::fs::remove_file(file) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            r => r,
        },
    }
}

/// The default profile itself, for a machine on the app default.
pub fn find_default(dir: &Path) -> Option<ShaderProfile> {
    find(dir, &default_id(dir)?)
}

/// What every picker's first row and the grid's "Shader" column call
/// the app default: the bare word with no default marked, the word and
/// the profile's name with one.
pub fn default_label(profiles: &[ProfileEntry]) -> String {
    match profiles.iter().find(|e| e.is_default) {
        Some(e) => format!("{} {}", crate::wizard::SHADER_DEFAULT_LABEL, e.profile.name),
        None => crate::wizard::SHADER_DEFAULT_LABEL.to_string(),
    }
}

/// The same for a machine of `family`: a modern one's default is no
/// shader at all, whatever the library marks (`player::resolve_shader`),
/// and the row says so rather than naming a profile it will not run.
pub fn default_label_for(family: crate::bundle::Family, profiles: &[ProfileEntry]) -> String {
    if family.is_modern() {
        format!("{} {}", crate::wizard::SHADER_DEFAULT_LABEL, NO_SHADER_LABEL)
    } else {
        default_label(profiles)
    }
}

/// What a modern machine's default runs: nothing.
pub const NO_SHADER_LABEL: &str = "None";

/// The profile id a machine's `shader_profile` field stores: a `.toml`
/// file's bare stem (e.g. `trinitron-warm`), matching `path`'s own name so
/// looking one up by id (`find`, below) is a plain filename join.
pub fn id_of(path: &Path) -> String {
    path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

/// An id from a profile name: lowercase, non-alphanumerics collapsed to
/// `-`, deduplicated against what's already in `dir`. Same scheme as
/// `library::slug`, checked against `<candidate>.toml` files instead of
/// bundle subdirectories.
fn slug(dir: &Path, name: &str) -> String {
    let mut base: String = name
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while base.contains("--") {
        base = base.replace("--", "-");
    }
    let base = base.trim_matches('-');
    let base = if base.is_empty() { "profile" } else { base };
    let mut candidate = base.to_string();
    let mut n = 2;
    while dir.join(format!("{candidate}.toml")).exists() {
        candidate = format!("{base}-{n}");
        n += 1;
    }
    candidate
}

/// Create a new profile under `dir`, returning its file path.
pub fn create(dir: &Path, name: String, preset: PathBuf) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    let path = dir.join(format!("{}.toml", slug(dir, &name)));
    ShaderProfile::new(name, preset).save(&path)?;
    Ok(path)
}

/// The starter profiles (`shader_source::DEFAULT_PROFILES`) against the
/// collection at `presets_dir`, returning the names actually written.
///
/// It never writes a **second** profile under a name the library already
/// has. `create`'s slug deduplication would make `crt-aperture-2`, so
/// re-running this (a second download, a `--default-profiles` by hand)
/// has to be a no-op. It also skips a preset the collection doesn't
/// contain, since a profile naming a missing `.slangp` is a parse error
/// waiting for whoever opens it.
///
/// A library with no default yet gets the first starter (CRT Aperture,
/// the Windows-era tube) as its default (user decision 2026-09-24), so
/// every machine on "(default)" plays through it from the first
/// download on. A default the user set stays theirs.
pub fn create_defaults(dir: &Path, presets_dir: &Path) -> Vec<String> {
    let existing: Vec<String> = scan(dir).into_iter().map(|e| e.profile.name).collect();
    let mut added = Vec::new();
    for (name, rel) in crate::shader_source::DEFAULT_PROFILES {
        if existing.iter().any(|have| have == name) {
            continue;
        }
        let preset = presets_dir.join(rel);
        if !preset.is_file() {
            eprintln!("[shader-library] no {} in the collection; skipping the {name} profile", preset.display());
            continue;
        }
        // Absolute, whatever `presets_dir` was. The player reads the
        // profile, starting from whatever directory the launcher was
        // in, and a relative preset would resolve against that instead
        // of against the collection.
        let preset = std::path::absolute(&preset).unwrap_or(preset);
        match create(dir, (*name).to_string(), preset) {
            Ok(_) => added.push((*name).to_string()),
            Err(e) => eprintln!("[shader-library] creating the {name} profile: {e}"),
        }
    }
    if default_id(dir).is_none() {
        if let Some((first, _)) = crate::shader_source::DEFAULT_PROFILES.first() {
            let starter = scan(dir).into_iter().find(|e| e.profile.name == *first);
            if let Some(entry) = starter {
                if let Err(e) = set_default(dir, Some(&id_of(&entry.path))) {
                    eprintln!("[shader-library] marking the {first} profile as the default: {e}");
                }
            }
        }
    }
    added
}

/// Every `*.toml` directly under `dir`. A file that fails to parse is
/// skipped with a stderr line, as in `library::scan`.
pub fn scan(dir: &Path) -> Vec<ProfileEntry> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let default = default_id(dir);
    let mut entries: Vec<ProfileEntry> = read
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "toml"))
        .filter_map(|path| match ShaderProfile::load(&path) {
            Ok(profile) => {
                let is_default = default.as_deref() == Some(id_of(&path).as_str());
                Some(ProfileEntry { path, profile, is_default })
            }
            Err(err) => {
                eprintln!("[shader-library] skipping {}: {err}", path.display());
                None
            }
        })
        .collect();
    entries.sort_by(|a, b| a.profile.name.cmp(&b.profile.name));
    entries
}

/// Look up a profile by id (a machine's `shader_profile` value) under
/// `dir`. `None` covers both "no such profile" and "unreadable". A
/// dangling reference (the profile was deleted after a machine picked it)
/// falls back to no shader override rather than failing the machine.
pub fn find(dir: &Path, id: &str) -> Option<ShaderProfile> {
    ShaderProfile::load(&dir.join(format!("{id}.toml"))).ok()
}

/// What a front end asks before `delete`: a headline naming the profile,
/// and the line under it.
pub fn delete_question(name: &str) -> (String, &'static str) {
    (format!("Delete the profile “{name}”?"), "This can't be undone.")
}

/// Delete a profile; if it was the library's default, the library has
/// no default afterwards (`default_id` would say so anyway, but the
/// file should not name a ghost).
pub fn delete(path: &Path) -> std::io::Result<()> {
    std::fs::remove_file(path)?;
    if let Some(dir) = path.parent() {
        if std::fs::read_to_string(dir.join(DEFAULT_FILE)).is_ok_and(|s| s.trim() == id_of(path)) {
            set_default(dir, None)?;
        }
    }
    Ok(())
}
