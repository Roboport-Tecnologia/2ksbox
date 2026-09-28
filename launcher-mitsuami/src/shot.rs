//! The headless screenshot: `LAUNCHER_SHOT=<file.png>` photographs the
//! window once it has settled and exits. It is how a packager proves the
//! binary opens a real window with its toolkit found (as `launcher-qt`'s
//! `LAUNCHER_QT_SHOT` does), and how a check looks at a window without a
//! desktop. `LAUNCHER_SHOT_DELAY_MS` (default 800) is how long it waits.
//!
//! On GTK a window with no desktop wants a display of its own: run it
//! under `gtk4-broadwayd` (`GDK_BACKEND=broadway`), as mitsuami's own
//! native tests do (docs/testing.md).

use mitsuami::core::{CurrentWindow, Ui};
use mitsuami::prelude::*;
use std::time::Duration;

/// `LAUNCHER_SCREEN=<window>[:<arg>]` names the window to photograph
/// (none: the machine window) and what to open it on; this is `<arg>` when
/// it names `window`.
pub fn screen(window: &str) -> Option<String> {
    let screen = std::env::var("LAUNCHER_SCREEN").ok()?;
    let (name, arg) = screen.split_once(':').unwrap_or((&screen, ""));
    (name == window).then(|| arg.to_owned())
}

/// Call from inside a window's content, which is where the window and the
/// `Ui` are provided. Only a window whose `screens` hold the one
/// `LAUNCHER_SCREEN` names (`""` for none) is photographed.
pub fn arm(screens: &[&str]) {
    let named = std::env::var("LAUNCHER_SCREEN").unwrap_or_default();
    if !screens.contains(&named.split(':').next().unwrap_or("")) {
        return;
    }
    let Some(path) = std::env::var_os("LAUNCHER_SHOT").filter(|p| !p.is_empty()).map(std::path::PathBuf::from)
    else {
        return;
    };
    let delay = std::env::var("LAUNCHER_SHOT_DELAY_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(800);
    let (Some(ui), Some(CurrentWindow(window))) = (inject::<Ui>(), inject::<CurrentWindow>()) else {
        eprintln!("[launcher] LAUNCHER_SHOT: no window to photograph");
        std::process::exit(1);
    };
    spawn_local(async move {
        sleep(Duration::from_millis(delay)).await;
        if std::env::var_os("LAUNCHER_SHOT_TREE").is_some() {
            if let Some(tree) = ui.inspect(window) {
                print_tree(&tree, 0);
            }
        }
        let code = match ui.capture(window).await {
            Ok(image) => match write_png(&path, &image) {
                Ok(()) => {
                    eprintln!("[launcher] shot {}x{} -> {}", image.width, image.height, path.display());
                    0
                }
                Err(e) => {
                    eprintln!("[launcher] {}: {e}", path.display());
                    1
                }
            },
            Err(e) => {
                eprintln!("[launcher] capture failed: {e:?}");
                1
            }
        };
        std::process::exit(code);
    });
}

fn write_png(path: &std::path::Path, image: &mitsuami::core::backend::Image) -> Result<(), Box<dyn std::error::Error>> {
    let file = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut encoder = png::Encoder::new(file, image.width, image.height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(&image.rgba)?;
    Ok(())
}

/// `LAUNCHER_SHOT_TREE=1`: every node's kind and frame, for a layout
/// that comes out wrong.
fn print_tree(node: &mitsuami::core::NodeInfo, depth: usize) {
    let f = node.frame;
    eprintln!("{:indent$}{:?} {:.0},{:.0} {:.0}x{:.0}", "", node.kind, f.origin.x, f.origin.y, f.size.width, f.size.height, indent = depth * 2);
    for child in &node.children {
        print_tree(child, depth + 1);
    }
}
