//! "Clone…" on a machine row, over `launcher_core::clone_machine`: a new
//! machine that is a copy of this one, or that shares its disk. What is
//! copied, what is refused and every sentence are the core's; this is the
//! dialog, and the poll that watches the copy's thread.

use crate::machines::Library;
use launcher_core::clone_machine::CloneMachine;
use mitsuami::prelude::*;
use std::path::Path;
use std::time::Duration;

#[derive(Clone, Copy)]
pub struct Cloner {
    model: Signal<CloneMachine>,
}

impl Store for Cloner {
    fn create() -> Cloner {
        Cloner { model: signal(CloneMachine::default()) }
    }
}

impl Cloner {
    fn read<R>(&self, f: impl FnOnce(&CloneMachine) -> R) -> R {
        self.model.with(f)
    }

    fn edit(&self, f: impl FnOnce(&mut CloneMachine)) {
        self.model.update(f);
    }

    pub fn open_for(&self, bundle: &Path, running: bool) {
        self.edit(|c| {
            c.set_library_dir(launcher_core::library::default_dir());
            c.open_for_path(bundle, running);
        });
    }

    /// For the headless `clone:…:same` screen.
    pub fn set_same_disk(&self, on: bool) {
        self.edit(|c| c.same_disk = on);
    }

    pub fn busy(&self) -> bool {
        self.read(CloneMachine::busy)
    }

    fn close(&self) {
        // A copy under way runs to its end; the window stays until then.
        if !self.model.with_untracked(CloneMachine::busy) {
            self.edit(|c| c.open = false);
        }
    }

    /// Start the copy, and poll its thread until it ends: the progress
    /// line moves on each poll, and a finished clone lands in the list.
    pub fn submit(&self, library: Library) {
        self.edit(CloneMachine::submit);
        if !self.model.with_untracked(CloneMachine::busy) {
            return;
        }
        let cloner = *self;
        spawn_local(async move {
            loop {
                sleep(Duration::from_millis(300)).await;
                let mut ended = false;
                cloner.edit(|c| ended = c.poll());
                if ended {
                    break;
                }
            }
            library.refresh();
            if let Some(status) = cloner.model.with_untracked(|c| c.status().map(str::to_owned)) {
                library.status.set(status);
            }
        });
    }
}

#[component]
pub fn CloneWindow() -> impl View {
    let cloner = use_store::<Cloner>();
    let library = use_store::<Library>();
    let get = move |f: fn(&CloneMachine) -> String| move || cloner.read(f);
    view! {
        <Window
            title=get(|c| c.title())
            // As tall as what it shows, as the Qt dialog is: it grows for
            // the warning and the progress bar, and shrinks when they go.
            size=WindowSize::FollowHeight(560.0)
            modal=Modality::Application
            open=move || cloner.read(|c| c.open)
            @close_request=move || cloner.close()
        >
            <Column padding=Spacing::Lg gap=Spacing::Md>
                {crate::shot::arm(&["clone"])}
                <Show when=move || cloner.read(|c| !c.note().is_empty())>
                    <Text text_style=TextStyle::Caption>{get(|c| c.note())}</Text>
                </Show>
                <Row gap=Spacing::Sm align=Align::Center>
                    <Text>"Name"</Text>
                    <TextInput
                        grow=1.0
                        a11y_label="Name"
                        enabled=move || !cloner.busy()
                        value=get(|c| c.name.clone())
                        @input=move |s| cloner.edit(|c| c.name = s)
                        @submit=move || if cloner.read(CloneMachine::can_submit) { cloner.submit(library) }
                    />
                </Row>
                <Checkbox
                    enabled=move || !cloner.busy()
                    checked=move || cloner.read(|c| c.same_disk)
                    @change=move |on| cloner.edit(|c| c.same_disk = on)
                >{get(|c| c.same_disk_label().to_owned())}</Checkbox>
                <Show when=move || cloner.read(CloneMachine::tpm_applies)>
                    <Checkbox
                        enabled=move || !cloner.busy()
                        checked=move || cloner.read(|c| c.new_tpm)
                        @change=move |on| cloner.edit(|c| c.new_tpm = on)
                    >{get(|c| c.new_tpm_label().to_owned())}</Checkbox>
                </Show>
                <Show when=move || cloner.read(|c| c.warning().is_some())>
                    <Text color=Color::Warning>{get(|c| c.warning().unwrap_or_default().to_owned())}</Text>
                </Show>
                <Show when=move || cloner.busy()>
                    <Column gap=Spacing::Xs>
                        <Progress
                            label="Copying"
                            value=move || cloner.read(|c| c.progress().map_or(0.0, |(done, total)| {
                                if total == 0 { 1.0 } else { done as f64 / total as f64 }
                            }))
                        />
                        <Text text_style=TextStyle::Caption>{get(|c| c.progress_label())}</Text>
                    </Column>
                </Show>
                <Show when=move || cloner.read(|c| c.error().is_some())>
                    <Text color=Color::Error>{get(|c| c.error().unwrap_or_default().to_owned())}</Text>
                </Show>
                <Row gap=Spacing::Sm justify=Justify::End>
                    <Button role=ButtonRole::Cancel enabled=move || !cloner.busy() @click=move || cloner.close()>
                        "Cancel"
                    </Button>
                    <Button
                        role=ButtonRole::Default
                        enabled=move || cloner.read(CloneMachine::can_submit)
                        @click=move || cloner.submit(library)
                    >"Clone"</Button>
                </Row>
            </Column>
        </Window>
    }
}
