//! The first-run shader offer: the one question a brand-new launcher
//! asks before it shows anything else.
//!
//! Without a preset collection every machine looks like a flat window,
//! and the CRT chain (doc 03) is the point of the player. `shader_source`
//! can fetch libretro's collection, but its download button is two
//! windows deep, on the profile manager's preset row, where someone who
//! has never opened the profile manager will not find it.
//!
//! So this is the same download, asked as a question on the way up and
//! answered once. Two rules make "once" mean once:
//!
//! * The question is asked only when there is **no collection at all**
//!   (`shader_source::presets_dir`). A source checkout has one (the
//!   submodule), and so does any package that ships one.
//! * Answering it either way writes a marker (`first-run.txt` in the
//!   profile directory), so a "no" is not asked again on every start.
//!   The marker sits beside the profiles rather than the presets because
//!   a successful download replaces the preset directory by a rename
//!   (`shader_source::fetch`) and would take the marker with it.
//!
//! When a "yes" finishes, the starter profiles
//! (`shader_source::DEFAULT_PROFILES`) are written against the new
//! collection, so the first machine someone creates has a CRT to pick
//! rather than four hundred `.slangp` files to guess from.

use crate::shader_library;
use crate::shader_source::{self, Download, Status};
use std::path::{Path, PathBuf};

/// The dialog's title. It lives here for the same reason the sentences
/// do (see [`Message`]).
pub const TITLE: &str = "Download shader presets?";

/// Which step the offer is on, which decides the answers it can take.
/// `Idle` means nothing on screen, the state for every start after the
/// first and for every launcher that already has a collection.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Step {
    Idle,
    /// A question, with a confirm and a cancel.
    Asking,
    /// A download in flight. Not a question: there is nothing to answer
    /// and nothing to press.
    Downloading,
    /// It failed, and can be retried or given up on.
    Failed,
    /// It worked; there is one thing to say and an acknowledgement.
    Done,
}

/// The step and the words that go with it, from one poll.
///
/// The words live here rather than in a front end because two front
/// ends once formatted the same sentence separately ("Downloading shader
/// presets… 12.3 MB" in Rust and in QML), which is the drift
/// `launcher-core` exists to prevent. A front end lays `headline` and
/// `detail` out the way its toolkit does (an alert's message and its
/// informative text) and writes neither.
pub struct Message {
    pub step: Step,
    /// The situation, in one line.
    pub headline: String,
    /// What follows from it: the question itself, the megabytes so far,
    /// the error, or what was installed. May be several paragraphs.
    pub detail: String,
}

/// The offer's whole state machine. Built by `check`, polled by `state`
/// from a timer or a polling task, the same way `editor::Presets` is.
#[derive(Default)]
pub struct FirstRun {
    profiles_dir: PathBuf,
    asking: bool,
    download: Option<Download>,
    /// The finished download's sentence, or its failure, kept until the
    /// user dismisses it so neither flashes past.
    outcome: Option<Result<String, String>>,
}

impl FirstRun {
    /// Decide whether to ask, given where profiles live
    /// (`shader_library::default_dir()` for the front end). Cheap: a
    /// `stat` for the marker and, only when it is absent, `presets_dir`'s
    /// two-level walk.
    pub fn check(profiles_dir: PathBuf) -> FirstRun {
        let asking = !marker(&profiles_dir).exists() && shader_source::presets_dir().is_none();
        FirstRun { profiles_dir, asking, ..FirstRun::default() }
    }

    /// A model that never asks, for a run where the question has no
    /// place (a diagnostic run, a headless frame grab).
    pub fn silent() -> FirstRun {
        FirstRun::default()
    }

    /// Whether anything is on screen. A front end shows its dialog on
    /// this and reads `state` for what goes in it.
    pub fn open(&self) -> bool {
        self.asking || self.download.is_some() || self.outcome.is_some()
    }

    /// Whether a download is running, which keeps a front end's timer
    /// going, like `editor::Presets::download_active`.
    pub fn busy(&self) -> bool {
        self.download.is_some()
    }

    /// Yes: remember the question was answered and start the download.
    pub fn accept(&mut self) {
        self.asking = false;
        self.answered("accepted");
        self.start();
    }

    /// No: remember it and say nothing more, this start or any other.
    pub fn decline(&mut self) {
        self.asking = false;
        self.outcome = None;
        self.answered("declined");
    }

    /// The "Try again" on a failed download.
    pub fn retry(&mut self) {
        self.outcome = None;
        self.start();
    }

    /// Close whatever is on screen: the OK on the finished line, and the
    /// way out of a failure that is not being retried.
    pub fn dismiss(&mut self) {
        self.asking = false;
        self.download = None;
        self.outcome = None;
    }

    /// What to say and what can be answered, turning a finished download
    /// into the starter profiles on the way past. Safe to call as often
    /// as a front end likes, from a repaint or a timer.
    pub fn state(&mut self) -> Message {
        if let Some(download) = &self.download {
            match download.status() {
                Status::Running(bytes) => {
                    return Message {
                        step: Step::Downloading,
                        headline: "Downloading shader presets…".into(),
                        detail: format!("{:.1} MB so far.", bytes as f64 / 1_000_000.0),
                    }
                }
                Status::Done(dir) => {
                    self.download = None;
                    self.outcome = Some(Ok(self.install_defaults(&dir)));
                }
                Status::Failed(err) => {
                    self.download = None;
                    self.outcome = Some(Err(err));
                }
            }
        }
        match &self.outcome {
            Some(Ok(detail)) => Message {
                step: Step::Done,
                headline: "Shader presets installed.".into(),
                detail: detail.clone(),
            },
            Some(Err(err)) => Message {
                step: Step::Failed,
                headline: "Couldn't download the shader presets.".into(),
                detail: err.clone(),
            },
            // The question names the size and the destination, like the
            // profile manager's row: 50 MB on a phone tether is a
            // decision, and nobody can undo a download that lands
            // somewhere unnamed.
            None if self.asking => Message {
                step: Step::Asking,
                headline: "No CRT shader presets are installed yet.".into(),
                detail: format!(
                    "They make a machine look like the monitor it was played on.\n\n\
                     Download libretro's slang-shaders ({size}) into {dir}?\n\n\
                     This also adds {count} ready-made profiles: {names}.",
                    size = shader_source::DOWNLOAD_SIZE,
                    dir = shader_source::install_dir().display(),
                    count = shader_source::DEFAULT_PROFILES.len(),
                    names = shader_source::default_profile_names(),
                ),
            },
            None => Message { step: Step::Idle, headline: String::new(), detail: String::new() },
        }
    }

    fn start(&mut self) {
        self.download = Some(Download::start(shader_source::install_dir()));
    }

    /// The starter profiles against the collection that just landed, and
    /// the sentence naming them. A profile whose name is already taken is
    /// left alone (`shader_library::create_defaults`), so this never
    /// duplicates a profile in somebody's library.
    fn install_defaults(&self, presets: &Path) -> String {
        let added = shader_library::create_defaults(&self.profiles_dir, presets);
        let mut line = format!("Installed into {}.", presets.display());
        if !added.is_empty() {
            line.push_str(&format!(" Added {} ready-made profiles: {}.", added.len(), added.join(", ")));
        }
        let default = shader_library::scan(&self.profiles_dir).into_iter().find(|e| e.is_default);
        if let Some(entry) = default {
            line.push_str(&format!(" Machines on \"(default)\" use {}.", entry.profile.name));
        }
        line
    }

    /// Write the marker. A failure here is only a warning: the worst it
    /// costs is being asked again next start.
    fn answered(&self, how: &str) {
        if self.profiles_dir.as_os_str().is_empty() {
            return;
        }
        let path = marker(&self.profiles_dir);
        let body = format!(
            "The first-run shader-preset offer was {how}.\n\
             Delete this file to be asked again the next time {name} starts.\n",
            name = crate::paths::NAME,
        );
        let wrote = std::fs::create_dir_all(&self.profiles_dir).and_then(|()| std::fs::write(&path, body));
        if let Err(e) = wrote {
            eprintln!("[first-run] {}: {e}", path.display());
        }
    }
}

/// The marker file: plain text in the profile directory, so somebody who
/// finds it can read what it is and delete it (doc 07's "Shader
/// profiles, presets and the preview").
pub fn marker(profiles_dir: &Path) -> PathBuf {
    profiles_dir.join("first-run.txt")
}
