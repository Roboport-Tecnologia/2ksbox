//! The first-run shader offer (`launcher_core::firstrun`): on the first
//! start of a launcher with no preset collection, a question; after a
//! yes, the download's progress in the machine window's toolbar; then
//! what came of it. The steps and every sentence are the core's. The
//! question and the outcome are the platform's own alert, as the Qt build
//! uses Qt's `MessageDialog`.
//!
//! `LAUNCHER_SCREEN=firstrun[:<answers>]` scripts the alerts instead:
//! each one prints what it would have shown and takes the next answer
//! (`yes`, `no`, `retry`, `cancel`, `ok`), since a native alert can't be
//! pressed headless. With no answer left it leaves the question up.

use crate::machines::Library;
use crate::shaders::Shaders;
use launcher_core::firstrun::{FirstRun, Message, Step};
use launcher_core::shader_library;
use mitsuami::prelude::*;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::Duration;

#[derive(Clone, Copy)]
pub struct Offer {
    model: Signal<FirstRun>,
    /// The download's line for the toolbar, while one runs.
    pub progress: Signal<Option<String>>,
}

impl Store for Offer {
    fn create() -> Offer {
        Offer { model: signal(FirstRun::silent()), progress: signal(None) }
    }
}

/// Where the answers come from: the platform's alert, or a script.
#[derive(Clone)]
enum Answers {
    Alert,
    Script(Rc<RefCell<VecDeque<String>>>),
}

impl Answers {
    /// Show `message` with `buttons` and return the index answered, or
    /// `None` when a script has run out (the question stays up).
    async fn ask(&self, message: &Message, buttons: &[&str], style: AlertStyle) -> Option<usize> {
        match self {
            Answers::Alert => {
                let mut request = Alert::new(message.headline.clone()).message(message.detail.clone()).style(style);
                for button in buttons {
                    request = request.button(*button);
                }
                Some(alert(request).await)
            }
            Answers::Script(script) => {
                let detail = message.detail.replace('\n', " ");
                eprintln!("[launcher] firstrun {:?}: {} | {detail} [{}]", message.step, message.headline, buttons.join("/"));
                let answer = script.borrow_mut().pop_front()?;
                let index = buttons.iter().position(|b| b.eq_ignore_ascii_case(&answer));
                eprintln!("[launcher] firstrun answered {answer}");
                Some(index.unwrap_or(buttons.len() - 1))
            }
        }
    }
}

impl Offer {
    fn state(&self) -> Message {
        let mut message = None;
        self.model.update(|m| message = Some(m.state()));
        message.expect("set by the update")
    }

    /// Ask, on a launcher that has never been asked and has no presets.
    /// `script` is the headless screen's answers, `None` for the alerts.
    pub fn start(&self, library: Library, shaders: Shaders, script: Option<&str>) {
        self.model.set(FirstRun::check(shader_library::default_dir()));
        let answers = match script {
            Some(script) => {
                eprintln!("[launcher] firstrun: open={}", self.model.with_untracked(FirstRun::open));
                let words = script.split(',').filter(|w| !w.is_empty()).map(str::to_owned).collect();
                Answers::Script(Rc::new(RefCell::new(words)))
            }
            None => Answers::Alert,
        };
        if !self.model.with_untracked(FirstRun::open) {
            return;
        }
        let offer = *self;
        spawn_local(async move { offer.run(answers, library, shaders).await });
    }

    async fn run(self, answers: Answers, library: Library, shaders: Shaders) {
        loop {
            let message = self.state();
            match message.step {
                Step::Idle => break,
                Step::Asking => match answers.ask(&message, &["Yes", "No"], AlertStyle::Info).await {
                    Some(0) => self.model.update(FirstRun::accept),
                    Some(_) => self.model.update(FirstRun::decline),
                    None => return,
                },
                Step::Downloading => {
                    self.progress.set(Some(format!("{} {}", message.headline, message.detail)));
                    sleep(Duration::from_millis(300)).await;
                }
                Step::Failed => {
                    self.progress.set(None);
                    match answers.ask(&message, &["Retry", "Cancel"], AlertStyle::Warning).await {
                        Some(0) => self.model.update(FirstRun::retry),
                        Some(_) => self.model.update(FirstRun::dismiss),
                        None => return,
                    }
                }
                Step::Done => {
                    self.progress.set(None);
                    // A collection and the starter profiles just landed:
                    // the profile manager's cached answer and the list's
                    // Shader column are stale.
                    shaders.presets_changed();
                    library.refresh();
                    if answers.ask(&message, &["OK"], AlertStyle::Info).await.is_none() {
                        return;
                    }
                    self.model.update(FirstRun::dismiss);
                }
            }
        }
        if let Answers::Script(_) = answers {
            eprintln!("[launcher] firstrun settled: open={}", self.model.with_untracked(FirstRun::open));
        }
    }
}
