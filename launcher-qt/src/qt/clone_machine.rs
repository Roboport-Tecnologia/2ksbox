//! "Clone…" (doc 07), as a QObject over
//! `launcher_core::clone_machine::CloneMachine`.
//!
//! The model decides everything: the name offered, what is copied and
//! where, the refusal while the machine runs, the sentences. This file
//! is the projection onto properties and the four things a view does:
//! type a name, tick "same hard disk", press Clone, and poll a running copy from a `Timer`
//! (`Main.qml`, which also rescans the grid when it lands).

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(bool, open)]
        #[qproperty(QString, title)]
        #[qproperty(QString, name)]
        #[qproperty(QString, note)]
        #[qproperty(bool, same_disk)]
        #[qproperty(QString, same_disk_label)]
        /// The shared disk's rule, or the machine is running: drawn as a warning, and Clone is off.
        #[qproperty(QString, warning)]
        #[qproperty(QString, error)]
        /// The line for the grid once a clone has landed.
        #[qproperty(QString, status)]
        /// A copy is running: the poll timer runs and the window's fields
        /// are off.
        #[qproperty(bool, busy)]
        #[qproperty(bool, can_submit)]
        /// 0..1 while a copy runs.
        #[qproperty(f64, progress)]
        #[qproperty(QString, progress_label)]
        type CloneModel = super::CloneModelRust;

        #[qinvokable]
        fn open_for(self: Pin<&mut CloneModel>, bundle_path: &QString, running: bool);

        /// The name field changed. Not the property's own setter, which
        /// would change the property and leave the model behind it.
        #[qinvokable]
        fn rename(self: Pin<&mut CloneModel>, name: &QString);

        /// The "same hard disk" checkbox changed.
        #[qinvokable]
        fn share_disk(self: Pin<&mut CloneModel>, same: bool);

        #[qinvokable]
        fn submit(self: Pin<&mut CloneModel>);

        /// Ask a running copy whether it has finished; true on the call
        /// that saw it end, which is the grid's cue to rescan.
        #[qinvokable]
        fn poll(self: Pin<&mut CloneModel>) -> bool;

        /// Cancel, or the window closed: put the window away. A copy
        /// already running carries on and lands by itself.
        #[qinvokable]
        fn dismiss(self: Pin<&mut CloneModel>);

        /// The `machine.toml` the last clone wrote.
        #[qinvokable]
        fn saved_path(self: &CloneModel) -> QString;
    }
}

use crate::{qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use launcher_core::clone_machine::CloneMachine;
use std::path::PathBuf;
use std::pin::Pin;

#[derive(Default)]
pub struct CloneModelRust {
    open: bool,
    title: QString,
    name: QString,
    note: QString,
    same_disk: bool,
    same_disk_label: QString,
    warning: QString,
    error: QString,
    status: QString,
    busy: bool,
    can_submit: bool,
    progress: f64,
    progress_label: QString,

    /// The window's state machine. Everything above is a projection.
    model: CloneMachine,
}

impl ffi::CloneModel {
    fn open_for(mut self: Pin<&mut Self>, bundle_path: &QString, running: bool) {
        let path = PathBuf::from(bundle_path.to_string());
        self.as_mut().rust_mut().model.open_for_path(&path, running);
        self.publish();
    }

    fn rename(mut self: Pin<&mut Self>, name: &QString) {
        self.as_mut().rust_mut().model.name = name.to_string();
        self.publish();
    }

    fn share_disk(mut self: Pin<&mut Self>, same: bool) {
        self.as_mut().rust_mut().model.same_disk = same;
        self.publish();
    }

    fn submit(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.submit();
        self.publish();
    }

    fn poll(mut self: Pin<&mut Self>) -> bool {
        let ended = self.as_mut().rust_mut().model.poll();
        self.publish();
        ended
    }

    fn dismiss(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().model.open = false;
        self.publish();
    }

    fn saved_path(&self) -> QString {
        self.rust().model.saved_path().map(|p| qs(p.display())).unwrap_or_default()
    }

    /// The model, onto the properties, each through its own setter (see
    /// the header of `main.rs`).
    fn publish(mut self: Pin<&mut Self>) {
        let (open, title, name, note, same_disk, same_disk_label, warning, error, status, busy, can_submit, progress, progress_label);
        {
            let m = &self.rust().model;
            open = m.open;
            title = qs(m.title());
            name = qs(&m.name);
            note = qs(m.note());
            same_disk = m.same_disk;
            same_disk_label = qs(m.same_disk_label());
            warning = qs_opt(m.warning());
            error = qs_opt(m.error());
            status = qs_opt(m.status());
            busy = m.busy();
            can_submit = m.can_submit();
            progress = m.progress().map(|(done, total)| if total == 0 { 0.0 } else { done as f64 / total as f64 });
            progress_label = qs(m.progress_label());
        }
        self.as_mut().set_title(title);
        self.as_mut().set_name(name);
        self.as_mut().set_note(note);
        self.as_mut().set_same_disk(same_disk);
        self.as_mut().set_same_disk_label(same_disk_label);
        self.as_mut().set_warning(warning);
        self.as_mut().set_error(error);
        self.as_mut().set_status(status);
        self.as_mut().set_busy(busy);
        self.as_mut().set_can_submit(can_submit);
        self.as_mut().set_progress(progress.unwrap_or(0.0));
        self.as_mut().set_progress_label(progress_label);
        // Last: `open` is what `Main.qml` shows the window on, so
        // everything its first frame draws is current by then.
        self.as_mut().set_open(open);
    }
}
