//! A path field: a caption, a text input for a path, and "Browse…" onto
//! the platform's file dialog, as `launcher-qt`'s `PathField.qml` is.
//! Every window with a path in it uses this one.
//!
//! Typing is still allowed (a path the user already knows, or one on a
//! mount the dialog can't reach). What the dialog offers and where a pick
//! lands are `launcher_core::browse`'s (`extensions`, `picked`,
//! `remember`), as in every front end.

use launcher_core::browse::{self, Filter};
use mitsuami::prelude::*;

/// `value` is what the field shows, and the owner's model decides it: an
/// edit (typed or picked) goes out through `@edit`, and the value comes
/// back through the binding. `@pick` is the same path when it came from
/// the dialog, for a field where choosing a file is the whole answer (the
/// disc shelf's adder acts on it at once); `@edit` has it first.
#[component]
pub fn PathField(
    #[prop(into)] label: String,
    /// The caption's width, so a window's captions line up in a column.
    #[prop(default, into)]
    label_width: Length,
    value: Value<String>,
    #[prop(default, into)] placeholder: String,
    /// The extensions the dialog offers; `None` offers every file.
    filter: Option<Filter<'static>>,
    /// The dialog picks a folder instead of a file.
    #[prop(default)]
    folder: bool,
    on_edit: Callback<String>,
    on_pick: Callback<String>,
    on_submit: Callback<()>,
) -> impl View {
    let (title, edited, picked) = (label.clone(), on_edit.clone(), on_pick);
    let browse = move || {
        let mut request = OpenFile::new().title(title.clone());
        if folder {
            request = request.directories();
        }
        if let Some(filter) = filter {
            request = request.filter(FileFilter::new(filter.0, browse::extensions(filter)));
        }
        let (edited, picked) = (edited.clone(), picked.clone());
        spawn_local(async move {
            if let Some(path) = open_file(request).await.and_then(|p| p.into_iter().next()) {
                // A sandboxed dialog hands back the portal's copy; the
                // bundle wants the file's own path.
                let path = browse::picked(&path);
                browse::remember(&path);
                let path = path.display().to_string();
                edited.call(path.clone());
                picked.call(path);
            }
        });
    };
    view! {
        <Row gap=Spacing::Sm align=Align::Center shrink=0.0>
            <Text width=label_width shrink=0.0>{label.clone()}</Text>
            <TextInput
                grow=1.0
                a11y_label=label
                value=value
                placeholder=placeholder
                @input=move |s| on_edit.call(s)
                @submit=move || on_submit.call(())
            />
            <Button @click=browse>"Browse…"</Button>
        </Row>
    }
}
