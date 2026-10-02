//! A path field: a caption, a text input for a path, and "Browse…" onto
//! the platform's file dialog, as `launcher-qt`'s `PathField.qml` is.
//! Every window with a path in it uses this one.
//!
//! Typing is still allowed (a path the user already knows, or one on a
//! mount the dialog can't reach). What the dialog offers, where it opens
//! and where a pick lands are `launcher_core::browse`'s (`extensions`,
//! `browse_start`, `picked`, `remember`), as in every front end.
//!
//! `LAUNCHER_PICK=<label>=<path>` is the probe's way in, as `launcher-qt`'s
//! `pickdisc` is: the field with that caption prints the dialog it would
//! open (`pick <label>: start …, filters …`), then takes `<path>` as the
//! dialog's answer, down the same line a real pick goes. The dialog itself
//! is modal and needs a human.

use launcher_core::browse::{self, Filter};
use mitsuami::prelude::*;
use std::path::{Path, PathBuf};
use std::time::Duration;

/// `value` is what the field shows, and the owner's model decides it: an
/// edit (typed or picked) goes out through `@edit`, and the value comes
/// back through the binding.
#[component]
pub fn PathField(
    #[prop(into)] label: String,
    /// The caption's width, so a window's captions line up in a column.
    #[prop(default, into)]
    label_width: Length,
    value: Value<String>,
    #[prop(default, into)] placeholder: String,
    /// The extensions the dialog offers first; "All files" always follows,
    /// since a filter that hides the file someone is looking for is worse
    /// than none. `None` offers every file.
    filter: Option<Filter<'static>>,
    /// The dialog picks a folder instead of a file.
    #[prop(default)]
    folder: bool,
    /// Where the dialog opens while the field is empty, before the last
    /// folder browsed (`browse::browse_start`). Only the shader editor's
    /// preset field has one: the preset collection.
    #[prop(default = Value::Static(None))]
    empty_dir: Value<Option<PathBuf>>,
    on_edit: Callback<String>,
) -> impl View {
    // Decided when it opens: the value, and the last folder any dialog was
    // browsing, change between one click and the next.
    let request = {
        let (title, value) = (label.clone(), value.clone());
        move || {
            let mut request = OpenFile::new().title(title.clone());
            if folder {
                request = request.directories();
            } else if let Some(filter) = filter {
                request = request
                    .filter(FileFilter::new(filter.0, browse::extensions(filter)))
                    .filter(FileFilter::all("All files"));
            }
            match browse::browse_start(&value.get(), empty_dir.get().as_deref()) {
                Some(start) => request.start_folder(start),
                None => request,
            }
        }
    };
    let accept = {
        let edited = on_edit.clone();
        move |path: &Path| {
            // A sandboxed dialog hands back the portal's copy; the bundle
            // wants the file's own path.
            let path = browse::picked(path);
            browse::remember(&path);
            edited.call(path.display().to_string());
        }
    };
    if let Some(path) = scripted_pick(&label) {
        let (label, request, accept) = (label.clone(), request.clone(), accept.clone());
        spawn_local(async move {
            // Once the window is up, as a click would be.
            sleep(Duration::from_millis(300)).await;
            let request = request();
            let globs = |f: &FileFilter| if f.is_all() { "*".to_owned() } else { f.extensions.join(" ") };
            let filters: Vec<String> = request.filters.iter().map(|f| format!("{} ({})", f.name, globs(f))).collect();
            eprintln!(
                "[launcher] pick {label}: start {}, filters [{}]",
                request.start_folder.as_deref().map_or("(platform's)".into(), |p| p.display().to_string()),
                filters.join(" | "),
            );
            accept(&path);
        });
    }
    let browse = move || {
        let (request, accept) = (request(), accept.clone());
        spawn_local(async move {
            if let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) {
                accept(&path);
            }
        });
    };
    view! {
        <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
            <Text width=label_width shrink=0.0>{label.clone()}</Text>
            // `min_width=0`: a flex item is at least as wide as its content
            // (as in CSS), and WinUI's text box asks for its whole text, so
            // a long path pushed Browse… out of the window.
            <TextInput
                grow=1.0
                min_width=0
                a11y_label=label
                value=value
                placeholder=placeholder
                @input=move |s| on_edit.call(s)
            />
            <Button @click=browse>"Browse…"</Button>
        </Row>
    }
}

/// `LAUNCHER_PICK=<label>=<path>`'s path, for the field with that caption.
fn scripted_pick(label: &str) -> Option<PathBuf> {
    let pick = std::env::var("LAUNCHER_PICK").ok()?;
    let (field, path) = pick.split_once('=')?;
    (field == label).then(|| path.into())
}
