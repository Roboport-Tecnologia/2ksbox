//! Where the shader presets come from: the collection itself, not the
//! profiles built on top of it (`shader_library.rs`).
//!
//! In a source checkout they are the `third_party/slang-shaders`
//! submodule. Someone who cloned without `--recurse-submodules`, or who
//! installs a package without them, has no such directory, and a preset
//! picker that opens on nothing is a dead end. So the collection can also
//! be **downloaded**: upstream's tarball, unpacked into the platform data
//! directory beside `machines/` and `shader-profiles/`. Never into
//! `third_party/`, which belongs to git.

use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

/// libretro's own collection, the same repository the `third_party`
/// submodule points at, as a tarball of the current `master` from
/// codeload, where a `git clone` fetches from anyway. Not pinned to the
/// submodule's commit: the binary that downloads this may be a packaged
/// launcher with no repository to read a pin from, and presets are
/// additive (a profile stores parameter overrides by name and tolerates
/// the preset gaining new ones; see `shader_profile`).
const TARBALL_URL: &str = "https://codeload.github.com/libretro/slang-shaders/tar.gz/refs/heads/master";

/// Roughly what that tarball weighs, so the button can say so before
/// someone starts it on a phone tether. Approximate on purpose.
pub const DOWNLOAD_SIZE: &str = "~50 MB";

/// The starter profiles written when a collection first lands
/// (`firstrun.rs`): a display name and the preset's path within the
/// collection, each at the preset's own defaults. A profile stores only
/// what someone overrode (`shader_profile`), so "default settings" is an
/// empty override table, not a snapshot of today's defaults.
///
/// Two CRTs and a monochrome computer monitor, the range the machines
/// cover: an aperture-grille tube for the Windows era, the heavier
/// `crt-royale` for a host that can afford it, and the Apple II monitor
/// for the green-screen end. Anything else is one preset pick away.
pub const DEFAULT_PROFILES: &[(&str, &str)] = &[
    ("CRT Aperture", "crt/crt-aperture.slangp"),
    ("CRT Royale", "crt/crt-royale.slangp"),
    ("Apple II", "presets/apple-monitor-II.slangp"),
];

/// The three names in one phrase, for the sentence that offers them.
pub fn default_profile_names() -> String {
    let names: Vec<&str> = DEFAULT_PROFILES.iter().map(|(name, _)| *name).collect();
    match names.split_last() {
        Some((last, [])) => (*last).to_string(),
        Some((last, rest)) => format!("{} and {last}", rest.join(", ")),
        None => String::new(),
    }
}

/// The collection that came with this build: the checkout's
/// `third_party/slang-shaders` submodule, or, for an installed launcher,
/// whatever the package shipped in `share/2ksbox/shaders`.
/// `scripts/package-linux.sh` ships none by default (80 MB, and the
/// manager can fetch them), but `--with-shaders`, a Flatpak or a distro
/// package would, and then nobody should be asked to download them.
pub fn repo_dir() -> PathBuf {
    crate::paths::resource("share/2ksbox/shaders", "third_party/slang-shaders")
}

/// Where a download lands, and the first place looked at when
/// `LAUNCHER_SHADERS_DIR` names it: the platform data dir plus
/// `shaders`, alongside `machines/` and `shader-profiles/`.
pub fn install_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_SHADERS_DIR") {
        return dir.into();
    }
    crate::paths::data_dir().map(|d| d.join("shaders")).unwrap_or_else(|| PathBuf::from("shaders"))
}

/// The preset collection on this machine, or `None` if there isn't one,
/// which puts the "Download presets" button on screen.
///
/// When `LAUNCHER_SHADERS_DIR` is set nothing else is consulted.
/// Otherwise the collection this build came with (`repo_dir`) wins over a
/// downloaded copy. In a checkout it is the submodule that a developer's
/// `--shader third_party/slang-shaders/…` paths and this repo's docs
/// refer to, and in a package it is the one the package guarantees.
pub fn presets_dir() -> Option<PathBuf> {
    if std::env::var_os("LAUNCHER_SHADERS_DIR").is_some() {
        let dir = install_dir();
        return has_presets(&dir).then_some(dir);
    }
    let repo = repo_dir();
    if has_presets(&repo) {
        return Some(repo);
    }
    let installed = install_dir();
    has_presets(&installed).then_some(installed)
}

/// Whether `dir` looks like a preset collection: at least one `.slangp`
/// within two levels (upstream keeps them one directory down, as in
/// `crt/crt-lottes.slangp`, with a few at the top). Cheap enough for the
/// ~50 directories that tree has, and the caller caches the answer rather
/// than asking per frame. An empty or half-unpacked directory reads as
/// "no presets".
pub fn has_presets(dir: &Path) -> bool {
    any_preset(dir, 2)
}

fn any_preset(dir: &Path, depth: u32) -> bool {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return false;
    };
    let mut subdirs = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("slangp")) {
            return true;
        }
        if depth > 0 && entry.file_type().is_ok_and(|t| t.is_dir()) {
            subdirs.push(path);
        }
    }
    subdirs.iter().any(|d| any_preset(d, depth - 1))
}

/// What the UI shows about a download in flight.
pub enum Status {
    /// Bytes of the tarball read so far. There is no total: codeload
    /// streams the archive and sends no `Content-Length`, so there is no
    /// percentage to show.
    Running(u64),
    Done(PathBuf),
    Failed(String),
}

/// A download running on its own thread. Dropping this doesn't cancel
/// it: the thread finishes writing and exits. Nothing it writes outside
/// its staging directory is visible until the final rename.
pub struct Download {
    bytes: Arc<AtomicU64>,
    result: Arc<Mutex<Option<Result<PathBuf, String>>>>,
}

impl Download {
    /// Fetch and unpack the collection into `dest`, replacing whatever
    /// is there. Returns immediately.
    pub fn start(dest: PathBuf) -> Download {
        let bytes = Arc::new(AtomicU64::new(0));
        let result = Arc::new(Mutex::new(None));
        let (b, r) = (bytes.clone(), result.clone());
        std::thread::spawn(move || {
            // A panic in `fetch` still ends the download, as a failure,
            // or the window would say "Downloading" forever.
            let fetched = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| fetch(&dest, &b)));
            let outcome = match fetched {
                Ok(r) => r.map(|()| dest).map_err(|e| e.to_string()),
                Err(_) => Err("the download stopped on an internal error".to_string()),
            };
            *r.lock().unwrap_or_else(|e| e.into_inner()) = Some(outcome);
        });
        Download { bytes, result }
    }

    pub fn status(&self) -> Status {
        match self.result.lock().unwrap().clone() {
            None => Status::Running(self.bytes.load(Ordering::Relaxed)),
            Some(Ok(dir)) => Status::Done(dir),
            Some(Err(e)) => Status::Failed(e),
        }
    }
}

/// The whole job, synchronously: used by the thread above and directly
/// by `cli.rs`'s `--download-shaders` verb, so the real fetch and unpack
/// can be exercised without a window.
pub fn fetch(dest: &Path, bytes: &AtomicU64) -> std::io::Result<()> {
    // Unpack beside the destination and rename only once the whole
    // archive is out, so an interrupted download never leaves a
    // half-collection that `has_presets` would call installed.
    let staging = staging_dir(dest);
    if staging.exists() {
        std::fs::remove_dir_all(&staging)?;
    }
    if let Some(parent) = staging.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let counted = Counting { inner: get(TARBALL_URL)?, bytes };
    unpack(counted, &staging)?;
    if !has_presets(&staging) {
        std::fs::remove_dir_all(&staging).ok();
        return Err(std::io::Error::other("the downloaded archive contained no .slangp presets"));
    }
    // Replace the old collection only now, and only after the new one is
    // known good. The old directory goes out of the way first because a
    // rename onto a non-empty directory fails.
    let previous = staging.with_extension("previous");
    std::fs::remove_dir_all(&previous).ok();
    let had = dest.exists();
    if had {
        std::fs::rename(dest, &previous)?;
    }
    if let Err(e) = std::fs::rename(&staging, dest) {
        // the old collection back where it was, not stranded for the
        // next download to delete
        if had {
            std::fs::rename(&previous, dest).ok();
        }
        return Err(e);
    }
    std::fs::remove_dir_all(&previous).ok();
    Ok(())
}

/// The body of a GET of `url`, read as it arrives.
#[cfg(feature = "download")]
fn get(url: &str) -> std::io::Result<impl Read> {
    let response = ureq::get(url).call().map_err(|e| std::io::Error::other(format!("{url}: {e}")))?;
    Ok(response.into_body().into_reader())
}

/// A build without the `download` feature (the player's) fetches nothing.
#[cfg(not(feature = "download"))]
fn get(url: &str) -> std::io::Result<std::io::Empty> {
    Err(std::io::Error::other(format!("{url}: built without downloads")))
}

fn staging_dir(dest: &Path) -> PathBuf {
    let name = dest.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "shaders".into());
    dest.with_file_name(format!("{name}.part"))
}

/// Unpack the gzipped tar into `into`, dropping the archive's single
/// top-level directory (`slang-shaders-master/`) so the presets land at
/// `into/crt/…` rather than one level down.
fn unpack(reader: impl Read, into: &Path) -> std::io::Result<()> {
    let mut archive = tar::Archive::new(flate2::read::GzDecoder::new(reader));
    for entry in archive.entries()? {
        let mut entry = entry?;
        // Only real files and directories. A tar can name a symlink
        // pointing anywhere on the host, and nothing in a shader
        // collection needs one, so any other entry is skipped.
        let kind = entry.header().entry_type();
        if !kind.is_file() && !kind.is_dir() {
            continue;
        }
        let path = entry.path()?.into_owned();
        let mut components = path.components();
        components.next(); // the archive's own top-level directory
        let rel = components.as_path();
        if rel.as_os_str().is_empty() {
            continue;
        }
        // `..` or an absolute path in an archive is how a tarball writes
        // outside the directory it is being unpacked into.
        if rel.components().any(|c| !matches!(c, Component::Normal(_))) {
            continue;
        }
        let out = into.join(rel);
        if kind.is_dir() {
            std::fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(parent) = out.parent() {
            std::fs::create_dir_all(parent)?;
        }
        entry.unpack(&out)?;
    }
    Ok(())
}

/// Counts what passes through, so the window can say how far along the
/// download is while it streams into the unpacker.
struct Counting<'a, R> {
    inner: R,
    bytes: &'a AtomicU64,
}

impl<R: Read> Read for Counting<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.bytes.fetch_add(n as u64, Ordering::Relaxed);
        Ok(n)
    }
}
