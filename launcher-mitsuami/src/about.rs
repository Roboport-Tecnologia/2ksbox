//! About 2ksbox, over `launcher_core::about`: the version, the licence
//! and the projects 2ksbox is built on, each a link with what it does
//! for us and its licence. Opened by the machine window's "?" and, on
//! macOS, by the application menu's About item (`app_menu`).

use launcher_core::about::{self, Credit};
use mitsuami::prelude::*;

const NAME_W: f32 = 170.0;
const LICENSE_W: f32 = 150.0;

#[derive(Clone, Copy)]
pub struct About {
    open: Signal<bool>,
}

impl Store for About {
    fn create() -> About {
        About { open: signal(false) }
    }
}

impl About {
    pub fn show(&self) {
        self.open.set(true);
    }
}

/// The app's menus. Only macOS has any: About there belongs in the
/// application menu, and the role puts it there. Elsewhere the toolbar's
/// "?" is the way in, and a menu bar would be a row for one item.
pub fn app_menu(about: About) {
    if cfg!(target_os = "macos") {
        set_menu(
            MenuBar::new().menu(
                Menu::new("Help")
                    .item(MenuItem::new("About 2ksbox").role(MenuRole::About).on_select(move || about.show())),
            ),
        );
    }
}

/// The app's icon as pixels, decoded once here: `Image` takes a file or
/// pixels, and an installed launcher has no file of its own to point at.
fn icon() -> Option<Pixels> {
    let png = include_bytes!("../../packaging/icon/2ksbox-128.png");
    let mut reader = png::Decoder::new(std::io::Cursor::new(png.as_slice())).read_info().ok()?;
    let mut rgba = vec![0; reader.output_buffer_size()?];
    let info = reader.next_frame(&mut rgba).ok()?;
    if info.color_type != png::ColorType::Rgba || info.bit_depth != png::BitDepth::Eight {
        return None;
    }
    rgba.truncate(info.buffer_size());
    Some(Pixels::new(info.width, info.height, rgba).scale(2.0))
}

fn open_url(url: &'static str) {
    spawn_local(async move {
        if let Err(e) = launch_url(url).await {
            eprintln!("[launcher] opening {url}: {e:?}");
        }
    });
}

#[component]
pub fn AboutWindow() -> impl View {
    let about = use_store::<About>();
    view! {
        <Window
            title="About 2ksbox"
            size=Size::new(700.0, 600.0)
            min_size=Size::new(500.0, 380.0)
            modal=Modality::Application
            open=move || about.open.get()
            @close_request=move || about.open.set(false)
        >
            // The credits scroll edge to edge, between separators, with
            // their own padding inside so the scroll bar never covers them.
            <Column grow=1.0 min_height=0>
                {crate::shot::arm(&["about"])}
                <Column padding=Spacing::Lg gap=Spacing::Md>
                    <Row gap=Spacing::Lg align=Align::Center>
                        {icon().map(|p| view! { <Image source=ImageSource::Pixels(p) width=64 height=64/> })}
                        <Column gap=Spacing::Xs grow=1.0>
                            <Text text_style=TextStyle::Title>{format!("{} {}", about::NAME, about::VERSION)}</Text>
                            <Text>{about::TAGLINE}</Text>
                            <Text text_style=TextStyle::Caption color=Color::SecondaryLabel>{about::LICENSE}</Text>
                            <Row>
                                <Button button_style=ButtonStyle::Borderless @click=|| open_url(about::URL)>
                                    {about::URL}
                                </Button>
                            </Row>
                        </Column>
                    </Row>
                    <Text weight=FontWeight::Semibold>{about::THANKS}</Text>
                </Column>
                <Separator/>
                <ScrollView grow=1.0 min_height=0>
                    <Column gap=Spacing::Lg padding_x=Spacing::Xl padding_y=Spacing::Md>
                        {credit_groups()}
                    </Column>
                </ScrollView>
                <Separator/>
                <Row padding=Spacing::Lg justify=Justify::End>
                    <Button role=ButtonRole::Default @click=move || about.open.set(false)>"Close"</Button>
                </Row>
            </Column>
        </Window>
    }
}

/// The credits, a heading per group (the window's content is rebuilt
/// from a closure, so this is called rather than captured).
fn credit_groups() -> Vec<impl View + use<>> {
    about::GROUPS
        .iter()
        .map(|group| {
            let rows = group.credits.iter().map(credit_row).collect::<Vec<_>>();
            view! {
                <Column gap=Spacing::Xs>
                    <Text text_style=TextStyle::Headline>{group.title}</Text>
                    {rows}
                </Column>
            }
        })
        .collect()
}

/// One project: its name as a link, what it does for us, its licence.
fn credit_row(c: &'static Credit) -> impl View + use<> {
    view! {
        <Row gap=Spacing::Md align=Align::Center>
            <Row width=NAME_W shrink=0.0>
                <Button button_style=ButtonStyle::Borderless tooltip=c.url @click=move || open_url(c.url)>
                    {c.name}
                </Button>
            </Row>
            <Text grow=1.0 max_lines=1u32>{c.what}</Text>
            <Text
                width=LICENSE_W
                shrink=0.0
                text_align=TextAlign::End
                text_style=TextStyle::Caption
                color=Color::SecondaryLabel
            >{c.license}</Text>
        </Row>
    }
}
