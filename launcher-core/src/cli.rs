//! Every debug verb that needs no toolkit, in one place, so the
//! launcher and `launcherx` answer the same ones with the same code
//! (doc 07; the verb table is in `docs/development.md`). They are how
//! the launcher is tested: the policy is integration and end-to-end
//! only, and a verb here drives the real model a button drives, without
//! a GUI click.
//!
//! `run` returns `Some(exit code)` when it recognised the verb, `None`
//! when the caller should keep looking or open a window. Both binaries
//! call it first thing, before a GUI exists.
//!
//! The headless screenshots are not here, because they need the
//! toolkit: `launcher-mitsuami` photographs its real windows with
//! `LAUNCHER_SHOT` and `LAUNCHER_SCREEN` (its `src/shot.rs`).

use crate::bundle::{self, Family, Machine, Music, Optimization, Sound};
use crate::{browse, clone_machine, control, disc_library, firstrun, grants, library, machines, player, preview,
    shader_library, shader_profile, shader_source, shelf, snaps, wizard};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// `PREVIEW_AREA=<w>x<h>`: the area the real editor's preview pane would
/// have reserved, since headlessly there is no window to measure one
/// from. 800x600 is a plausible non-fullscreen editor.
pub fn preview_area_env() -> (u32, u32) {
    std::env::var("PREVIEW_AREA")
        .ok()
        .and_then(|s| {
            let (w, h) = s.split_once('x')?;
            Some((w.parse().ok()?, h.parse().ok()?))
        })
        .unwrap_or((800, 600))
}

/// `PREVIEW_FRAME=<n>`: which frame of the preset to render, for a
/// preset whose picture depends on the frame number (an interlaced or
/// flickering CRT, a phosphor afterglow). The real editor takes this
/// from a clock so the picture moves; headlessly one frame has to be
/// named so it renders the same twice. Default 0.
pub fn preview_frame_env() -> usize {
    std::env::var("PREVIEW_FRAME").ok().and_then(|s| s.parse().ok()).unwrap_or(0)
}

pub fn parse_family(arg: Option<&str>, usage: &str) -> Family {
    match arg {
        Some("win98") => Family::Win98,
        Some("xp") => Family::Xp,
        Some("win7") => Family::Win7,
        Some("dos") => Family::Dos,
        Some("other") => Family::Other,
        Some("win11") => Family::Win11,
        _ => panic!("{usage}"),
    }
}

/// Handle one verb. `args` is whatever followed it.
pub fn run(verb: &str, args: &mut impl Iterator<Item = String>) -> Option<i32> {
    match verb {
        "--print-args" => {
            let path = args.next().expect("usage: --print-args <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            println!("{}", machine.qemu_args(&player::pc_bios_dir(), None).join(" "));
        }
        "--machine-details" => {
            // What the machine window shows of a selected machine: the
            // line under its name, then each group and its rows, as
            // `<group>\t<label>\t<value>`. The machine is found in the
            // library (`LAUNCHER_LIBRARY_DIR`), as the window finds it.
            let path = args.next().expect("usage: --machine-details <machine.toml>");
            let dir = Path::new(&path).parent().expect("a bundle in a directory").to_path_buf();
            let library = machines::Machines::load();
            let row = library.entries().iter().position(|e| e.dir == dir).expect("a machine in the library");
            println!("{}", library.subtitle(row));
            for group in library.details(row) {
                for row in group.rows {
                    println!("{}\t{}\t{}", group.title, row.label, row.value);
                }
            }
        }
        "--print-shader-args" => {
            let path = args.next().expect("usage: --print-shader-args <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            println!("{}", player::shader_args(&machine).join(" "));
        }
        "--print-player-args" => {
            // Everything the launcher puts on the player's own command
            // line, before the `--` that hands the rest to QEMU.
            // `--print-args` shows the QEMU half.
            let path = args.next().expect("usage: --print-player-args <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            let mut argv = player::shader_args(&machine);
            argv.extend(player::pad_args(&machine));
            argv.extend(player::share_args(&machine));
            println!("{}", argv.join(" "));
        }
        "--prepare" => {
            // What the player's start makes before QEMU runs (a Windows
            // 11 machine's firmware variables), for a script that runs
            // QEMU itself from `--print-args`.
            let path = args.next().expect("usage: --prepare <machine.toml>");
            let machine = Machine::load(Path::new(&path)).expect("load bundle");
            if let Err(e) = player::prepare(&machine) {
                eprintln!("{e}");
                return Some(1);
            }
        }
        "--play" => {
            let path = PathBuf::from(args.next().expect("usage: --play <machine.toml>"));
            let machine = Machine::load(&path).expect("load bundle");
            // The same monitor socket the grid's "Play" opens, so the
            // live half of `--discs`/`--snapshots` can be scripted
            // against a player started this way.
            let dir = path.parent().unwrap_or(Path::new("."));
            let socket = control::socket_path(dir);
            let shelf_path = control::shelf_path(dir);
            machines::publish_shelf(&disc_library::default_path(), &shelf_path);
            let child = player::spawn(&machine, Some(&socket), Some(&shelf_path)).expect("spawn player");
            println!("pid {} qmp {} shelf {}", child.id(), socket.display(), shelf_path.display());
        }
        "--qmp-socket" => {
            // Where a bundle's live-control monitor socket lives, so a
            // script (or a hand-run QEMU standing in for the player) can
            // put one there.
            let path = PathBuf::from(args.next().expect("usage: --qmp-socket <machine.toml>"));
            println!("{}", control::socket_path(path.parent().unwrap_or(Path::new("."))).display());
        }
        "--kvm" => {
            // What the wizard's acceleration hint reads, on its own:
            // this host's answer, not the bundle's setting.
            println!("{}", if player::hw_accel_available() { "available" } else { "not available" });
            // And the acceleration picker's entries, on a new XP machine
            // or the one given (`--kvm [machine.toml]`): hardware
            // acceleration only where it is here, or where the machine
            // already asks for it.
            let mut form = wizard::Form::default();
            match args.next() {
                Some(bundle) => form.open_edit_path(PathBuf::from(bundle)),
                None => form.open_new(Family::Xp),
            }
            let choices: Vec<_> = form.accel_choices().iter().map(|a| a.label()).collect();
            println!("choices: {}", choices.join(", "));
        }
        "--host-check" => {
            // The other half of `--kvm`: what this host can do for the
            // guest's 3D, asked before a machine is created (ADR-013).
            // The bar is DXVK's own, a Vulkan 1.3 device, and a host
            // below it still runs every guest. Exits non-zero only when
            // the pass-through is unavailable on every back end
            // (`HostGpu::backend`). A software driver is slow, not
            // absent, so a script asking "can this host do 3D" hears yes,
            // and so does a Linux or macOS host below the bar that has
            // Wine and the executor's Windows build (ADR-018).
            let probe = crate::host_gpu::probe();
            print!("{}", crate::host_gpu::report_text(&probe));
            return Some(if probe.gpu.pass_through_available() { 0 } else { 1 });
        }
        "--about" => {
            // The About window's words and credits (`about.rs`).
            print!("{}", crate::about::text());
        }
        "--paths" => {
            // Every companion a launcher would reach for, and where it
            // found it (`paths.rs`). `scripts/package-linux.sh` checks a
            // staged package with it (a launcher that still answers with
            // the checkout it was built from is not packaged), and it is
            // the first thing to ask of an installed build that says a
            // file is missing.
            print!("{}", paths_text());
        }
        "--diagnose" => {
            // The same answers, plus the host's 3D, written **into the
            // launcher's log** as well as printed, because on Windows the
            // launcher is a windowed program with no stdout when started
            // by double-click. A user can send one file
            // (`fatal::log_path()`), which also holds the start-up
            // milestones and any panic. `2ksbox-debug.bat` in the Windows
            // package runs this on a double-click.
            let probe = crate::host_gpu::probe();
            let text = format!("{}\n{}", paths_text(), crate::host_gpu::report_text(&probe));
            print!("{text}");
            crate::fatal::record("--diagnose", &text);
        }
        "--new" => {
            let usage = "usage: --new <win98|xp|win7|dos|other|win11> <name> <disk.qcow2>";
            let family = parse_family(args.next().as_deref(), usage);
            let name = args.next().expect(usage);
            let disk = args.next().expect(usage).into();
            let path = library::create(&library::default_dir(), family, name, disk).expect("create bundle");
            println!("{}", path.display());
        }
        "--wizard-new" => {
            // Headless equivalent of the "New machine" window: the real
            // form's `submit`, disk creation via qemu-img included.
            let usage = "usage: --wizard-new <win98|xp|win7|dos|other|win11> <name> <disk-size-gb>";
            let family = parse_family(args.next().as_deref(), usage);
            let name = args.next().expect(usage);
            let size_gb: u32 = args.next().expect(usage).parse().expect("disk size must be a number");
            let mut form = wizard::Form::with_new_disk(family, name, size_gb);
            match form.submit(&library::default_dir()) {
                Some(path) => println!("{}", path.display()),
                None => panic!("create bundle: {}", form.error.unwrap_or_default()),
            }
        }
        "--wizard-edit" => {
            // Headless equivalent of clicking "Edit…" then "Save": load
            // a bundle, change the fields given, save it back in place.
            // `-` keeps a field as it is.
            let usage =
                "usage: --wizard-edit <machine.toml> <new-name|-> [ram-mb|-] [auto|kvm|tcg|-] [net|nonet] [cpu-speed|-] [boot|-] [seamless|noseamless] [d3dpt|std|cirrus|-] [none|usb|keys|-] [voodoo|voodoo-undither|novoodoo|-] [extra-qemu-args|-] [auto|dxvk|system|-] [clipboard|noclipboard|-] [shared-folder|none|-]";
            let path: PathBuf = args.next().expect(usage).into();
            let new_name = args.next().expect(usage);
            let mut form = wizard::Form::default();
            form.open_edit_path(path);
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            if new_name != "-" {
                form.name = new_name;
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(ram) => form.choose_ram_mb(ram.parse().expect("ram must be a number")),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("auto") => form.choose_accel(bundle::Accel::Auto),
                Some("kvm") => form.choose_accel(bundle::Accel::Kvm),
                Some("tcg") => form.choose_accel(bundle::Accel::Tcg),
                Some(other) => panic!("unknown accelerator {other:?}; {usage}"),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("net") => form.choose_network(true),
                Some("nonet") => form.choose_network(false),
                Some(other) => panic!("networking is net or nonet, not {other:?}; {usage}"),
            }
            // The processor and the boot order, by label.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(label) => form.choose_cpu_speed(
                    bundle::CpuSpeed::ALL
                        .into_iter()
                        .find(|s| s.label().eq_ignore_ascii_case(label))
                        .unwrap_or_else(|| panic!("unknown processor {label:?}; {usage}")),
                ),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(label) => {
                    form.boot = bundle::Boot::ALL
                        .into_iter()
                        .find(|b| b.label().eq_ignore_ascii_case(label))
                        .unwrap_or_else(|| panic!("unknown boot order {label:?}; {usage}"))
                }
            }
            // The pointer, the same way: with the tablet there is no
            // grab at all, without it the player takes the pointer on a
            // click. Its effect is a device on the command line, so
            // `--print-args` is the check.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("seamless") => form.choose_seamless_mouse(true),
                Some("noseamless") => form.choose_seamless_mouse(false),
                Some(other) => panic!("the pointer is seamless or noseamless, not {other:?}; {usage}"),
            }
            // The display adapter. Which names a family accepts is the
            // form's business (`video_choices`). One it does not offer is
            // a no-op here rather than an error, so a script can set the
            // same field on every machine it walks.
            match args.next().as_deref() {
                None | Some("-") => {}
                // `choose_video` refuses one this family does not offer,
                // so `std` is a no-op on an XP machine rather than a
                // machine with no driver.
                Some("d3dpt") => form.choose_video(bundle::Video::D3dpt),
                Some("std") => form.choose_video(bundle::Video::Std),
                Some("cirrus") => form.choose_video(bundle::Video::Cirrus),
                Some(other) => panic!("the adapter is d3dpt, std or cirrus, not {other:?}; {usage}"),
            }
            // What a host gamepad does (M13). By the same rule as the
            // adapter: a setting this family does not offer is refused
            // by `choose_pad` rather than written, so a script can set
            // it on every machine it walks.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(name) => form.choose_pad(
                    bundle::Pad::from_name(name)
                        .unwrap_or_else(|| panic!("unknown gamepad setting {name:?}; {usage}")),
                ),
            }
            // The Voodoo 2 (doc 21), the other field whose effect is a
            // device on the command line.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("voodoo") => {
                    form.choose_voodoo2(true);
                    form.choose_voodoo2_undither(false);
                }
                // the card with its dither undone (doc 21 §12): one
                // token rather than a positional of its own, so every
                // caller of this verb keeps the arguments it passes
                Some("voodoo-undither") => {
                    form.choose_voodoo2(true);
                    form.choose_voodoo2_undither(true);
                }
                Some("novoodoo") => form.choose_voodoo2(false),
                Some(other) => panic!(
                    "the Voodoo 2 is voodoo, voodoo-undither or novoodoo, not {other:?}; {usage}"
                ),
            }
            // The extra QEMU arguments, as one line the way the field
            // takes them; an empty one clears them. A quote left open is
            // the form's refusal at `submit`, like any other.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(line) => form.extra_qemu_args = line.to_string(),
            }
            // Which Direct3D 9 the host runs the executor on (ADR-007's
            // second amendment). Last, so every existing caller of this
            // verb keeps the arguments it passes.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("auto") => form.choose_d3d9(bundle::D3d9::Auto),
                Some("dxvk") => form.choose_d3d9(bundle::D3d9::Dxvk),
                Some("system") => form.choose_d3d9(bundle::D3d9::System),
                Some(other) => {
                    panic!("the Direct3D 9 is auto, dxvk or system, not {other:?}; {usage}")
                }
            }
            // Windows 11's sharing (M23), after everything older, so
            // every existing caller keeps the arguments it passes.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("clipboard") => form.clipboard = true,
                Some("noclipboard") => form.clipboard = false,
                Some(other) => panic!("the clipboard is clipboard or noclipboard, not {other:?}; {usage}"),
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some("none") => form.shared_folder.clear(),
                Some(dir) => form.shared_folder = dir.to_string(),
            }
            match form.submit(&library::default_dir()) {
                Some(saved) => println!("{}", saved.display()),
                None => panic!("save bundle: {}", form.error.unwrap_or_default()),
            }
        }
        "--clone" => {
            // Headless equivalent of a row's "Clone…" then "Clone": the
            // same model, so the same default name, the same refusals (a
            // running machine, a name already in the library) and the
            // same copy. With no name it takes the one the window offers.
            // `--same-disk` is the window's checkbox: no copy of the disk.
            // `--new-tpm` the other one (a Windows 11 machine's).
            let usage = "usage: --clone <machine.toml> [--same-disk] [--new-tpm] [new name]";
            let path: PathBuf = args.next().expect(usage).into();
            let mut window = clone_machine::CloneMachine::default();
            window.open_for_path(&path, false);
            let mut name = args.next();
            loop {
                match name.as_deref() {
                    Some("--same-disk") => window.same_disk = true,
                    Some("--new-tpm") => window.new_tpm = true,
                    _ => break,
                }
                name = args.next();
            }
            if let Some(name) = name {
                window.name = name;
            }
            if window.error().is_none() {
                window.submit();
                window.wait();
            }
            match (window.error(), window.saved_path()) {
                (None, Some(saved)) => println!("{}", saved.display()),
                (error, _) => {
                    eprintln!("[clone] {}", error.unwrap_or("nothing was cloned"));
                    return Some(1);
                }
            }
        }
        "--optimizations" => {
            // Headless equivalent of the wizard's "Emulation
            // optimizations" section: the real form's checkboxes and its
            // "All defaults" button, then a save. With no changes it
            // only reports, which is also how a bundle is read back
            // after one: the state, and whether it is the shipped one.
            let usage = "usage: --optimizations <machine.toml> [<name> on|off | defaults]...";
            let path: PathBuf = args.next().expect(usage).into();
            let mut form = wizard::Form::default();
            form.open_edit_path(path.clone());
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            let mut changed = false;
            while let Some(arg) = args.next() {
                changed = true;
                if arg == "defaults" {
                    form.reset_optimizations();
                    continue;
                }
                let opt = Optimization::ALL
                    .into_iter()
                    .find(|o| o.key() == arg)
                    .unwrap_or_else(|| panic!("unknown optimization {arg:?}; {usage}"));
                let on = match args.next().as_deref() {
                    Some("on") => true,
                    Some("off") => false,
                    other => panic!("{} takes on or off, not {other:?}; {usage}", opt.key()),
                };
                form.choose_optimization(opt, on);
            }
            if changed && form.submit(&library::default_dir()).is_none() {
                eprintln!("[optimizations] {}", form.error.unwrap_or_default());
                return Some(1);
            }
            // What the section says above the switches. On a machine
            // headed for KVM it says they do nothing, which a script
            // should see too.
            println!("[optimizations] {}", form.optimizations_note());
            for opt in Optimization::ALL {
                println!(
                    "{}\t{}\t{}",
                    opt.key(),
                    if form.optimization_enabled(opt) { "on" } else { "off" },
                    if form.optimizations().is_default(opt) { "default" } else { "changed" }
                );
            }
        }
        "--music" => {
            // Headless equivalent of the machine form's two audio rows
            // (doc 20 §6): the sound card, what is on the MIDI port, and
            // the two files only the user can supply. With no arguments
            // it reports, which is also how a bundle is read back after
            // a change: the state, and whether it is the family's.
            let usage = "usage: --music <machine.toml> [card|-] [gm|mt32|none|-] [soundfont|-] [romdir|-]";
            let path: PathBuf = args.next().expect(usage).into();
            let mut form = wizard::Form::default();
            form.open_edit_path(path);
            if let Some(e) = &form.error {
                panic!("{e}");
            }
            let mut changed = false;
            // A card this family does not offer is a no-op rather than
            // an error, like the adapter in --wizard-edit: a script can
            // then set the same field on every machine it walks.
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(key) => {
                    changed = true;
                    form.choose_sound(
                        Sound::ALL
                            .into_iter()
                            .find(|c| c.key() == key)
                            .unwrap_or_else(|| panic!("unknown sound card {key:?}; {usage}")),
                    );
                }
            }
            match args.next().as_deref() {
                None | Some("-") => {}
                Some(key) => {
                    changed = true;
                    form.choose_music(
                        Music::ALL
                            .into_iter()
                            .find(|m| m.key() == key)
                            .unwrap_or_else(|| panic!("unknown MIDI port {key:?}; {usage}")),
                    );
                }
            }
            for (field, value) in [(0, args.next()), (1, args.next())] {
                let Some(value) = value else { continue };
                if value == "-" {
                    continue;
                }
                changed = true;
                let value = if value == "none" { String::new() } else { value };
                if field == 0 {
                    form.soundfont = value;
                } else {
                    form.mt32_roms = value;
                }
            }
            if changed && form.submit(&library::default_dir()).is_none() {
                eprintln!("[music] {}", form.error.unwrap_or_default());
                return Some(1);
            }
            println!(
                "card\t{}\t{}",
                form.sound().key(),
                if form.sound_is_default() { "default" } else { "changed" }
            );
            println!(
                "music\t{}\t{}",
                form.music().key(),
                if form.music_is_default() { "default" } else { "changed" }
            );
            println!("soundfont\t{}", if form.soundfont.is_empty() { "(the one we ship)" } else { &form.soundfont });
            println!("romdir\t{}", if form.mt32_roms.is_empty() { "(none)" } else { &form.mt32_roms });
        }
        "--boot-disc" => {
            // Headless equivalent of a row's "Boot" button: which disc is
            // in the machine's drive when it starts.
            let usage = "usage: --boot-disc <machine.toml> <disc|none>";
            let path: PathBuf = args.next().expect(usage).into();
            let mut shelf = shelf::Shelf::default();
            shelf.open_for_path(path, &disc_library::default_path());
            let disc = args.next().expect(usage);
            shelf.set_boot(if disc == "none" { None } else { Some(disc.into()) });
            match shelf.last_result() {
                Ok(status) => println!("{}", status.unwrap_or("(nothing happened)")),
                Err(e) => {
                    eprintln!("[boot-disc] {e}");
                    return Some(1);
                }
            }
        }
        "--discs" => {
            // Headless equivalent of the shelf window: with no arguments
            // it prints the shared shelf, otherwise it runs the same
            // `add`/`remove` the buttons do. `+tools` stands for the
            // "Add guest-tools ISO" button, and `publish <bundle dir>`
            // for what the GUI does on its own when a machine is running.
            // A directory can be added like any other entry: it goes in
            // the drive as a generated disc (`disc_library::qemu_medium`).
            let library_path = disc_library::default_path();
            machines::import_legacy_discs(&library::scan(&library::default_dir()), &library_path);
            let mut shelf = shelf::Shelf::default();
            shelf.open_library(&library_path);
            let mut removing = false;
            let mut publish_to: Option<PathBuf> = None;
            let mut expecting_dir = false;
            for arg in args.by_ref() {
                match arg.as_str() {
                    _ if expecting_dir => {
                        publish_to = Some(arg.into());
                        expecting_dir = false;
                    }
                    "add" => removing = false,
                    "remove" => removing = true,
                    "+tools" => shelf.add_guest_tools(),
                    // What a running machine's drive is given (patch 52):
                    // the flat `<label>\t<path>` file the in-guest CDSHELF
                    // program lists. The GUI does it whenever the shelf
                    // changes under a machine that is up (`republish_shelf`);
                    // headlessly there is no such moment, so name the
                    // bundle directory to write it now.
                    "publish" => expecting_dir = true,
                    other if removing => shelf.remove(Path::new(other)),
                    other => shelf.add(other.into()),
                }
            }
            // A window saves when a field reports a finished edit;
            // headlessly there is no window, so flush here.
            shelf.flush().expect("save the shelf");
            if let Err(e) = shelf.last_result() {
                eprintln!("[discs] {e}");
            }
            if let Some(dir) = publish_to {
                let path = control::shelf_path(&dir);
                machines::publish_shelf(&library_path, &path);
                println!("shelf published to {}", path.display());
            }
            for disc in shelf.discs() {
                println!("{}\t{}", disc.label, disc.path.display());
            }
        }
        "--insert-disc" => {
            // Headless equivalent of the shelf's live "Insert"/"Eject"
            // buttons, against the machine's running monitor.
            let usage = "usage: --insert-disc <machine.toml> <disc|eject>";
            let path: PathBuf = args.next().expect(usage).into();
            let what = args.next().expect(usage);
            let mut shelf = shelf::Shelf::default();
            shelf.open_for_path(path, &disc_library::default_path());
            if what == "eject" {
                shelf.eject_live();
            } else {
                shelf.insert_live(Path::new(&what));
            }
            match shelf.last_result() {
                Ok(status) => println!("{}", status.unwrap_or("(nothing happened)")),
                Err(e) => {
                    eprintln!("[disc-shelf] {e}");
                    return Some(1);
                }
            }
        }
        "--drive" => {
            // Headless equivalent of the shelf window opened for a
            // machine: its drive card, then each disc with its kind and
            // whether it is the one in the drive. `insert`/`eject` are the
            // card's and the rows' buttons, which set the boot disc and,
            // on a running machine, swap the disc now as well.
            let usage = "usage: --drive <machine.toml> [insert <disc>|eject]";
            let path: PathBuf = args.next().expect(usage).into();
            let mut shelf = shelf::Shelf::default();
            shelf.open_for_path(path, &disc_library::default_path());
            match (args.next().as_deref(), args.next()) {
                (Some("insert"), Some(disc)) => shelf.insert(Path::new(&disc)),
                (Some("eject"), None) => shelf.eject(),
                (None, _) => {}
                _ => panic!("{usage}"),
            }
            if let Err(e) = shelf.last_result() {
                eprintln!("[drive] {e}");
                return Some(1);
            }
            println!("running\t{}", shelf.running());
            if let Some(card) = shelf.drive_card() {
                let kind = card.kind.map_or("empty", |k| k.key());
                println!("drive\t{kind}\t{}\t{}", card.title, card.detail);
            }
            for (row, disc) in shelf.discs().iter().enumerate() {
                let kind = shelf.row_kind(row).map_or("", |k| k.key());
                let mark = if shelf.row_in_drive(row) { "in-drive" } else { "-" };
                println!("{mark}\t{kind}\t{}\t{}", disc.label, shelf.row_detail(row));
            }
        }
        "--snapshots" => {
            // Headless equivalent of the "Snapshots…" window: the same
            // operations its buttons run, over a bundle's disk.
            let usage = "usage: --snapshots [--live] <machine.toml> [take|delete|restore <name>]";
            let mut next = args.next();
            // `--live` drives a running machine's monitor instead of
            // qemu-img, the way the window does when its player is up.
            let live = next.as_deref() == Some("--live");
            if live {
                next = args.next();
            }
            let path: PathBuf = next.expect(usage).into();
            let mut window = snaps::Snapshots::default();
            window.open_for_path(&path, live);
            match (args.next().as_deref(), args.next()) {
                (Some("take"), Some(name)) => window.take(&name),
                (Some("delete"), Some(name)) => window.drop_snapshot(&name),
                (Some("restore"), Some(name)) => window.revert(&name),
                (None, _) => {}
                _ => panic!("{usage}"),
            }
            // A live operation is a QMP job: it returns as soon as the
            // job exists and finishes later, so wait for it here the way
            // the window's timer does.
            window.wait_for_job(Duration::from_secs(120));
            print_snapshots(&window);
        }
        "--shaders" => {
            // What the profile manager's preset row reads: where this
            // host's `.slangp` collection is, or nothing, which puts the
            // "Download presets" button on screen. "Browse…" opens here
            // when the field is empty.
            match shader_source::presets_dir() {
                Some(dir) => println!("{}", dir.display()),
                None => println!("(none; would install into {})", shader_source::install_dir().display()),
            }
        }
        "--download-shaders" => {
            // The button's own work, without a window: fetch and unpack
            // the collection, printing what arrived.
            let dest = args.next().map(PathBuf::from).unwrap_or_else(shader_source::install_dir);
            let bytes = std::sync::atomic::AtomicU64::new(0);
            match shader_source::fetch(&dest, &bytes) {
                Ok(()) => println!(
                    "{:.1} MB -> {} ({})",
                    bytes.load(std::sync::atomic::Ordering::Relaxed) as f64 / 1_000_000.0,
                    dest.display(),
                    if shader_source::has_presets(&dest) { "presets found" } else { "NO PRESETS" }
                ),
                Err(e) => {
                    eprintln!("[shaders] {e}");
                    return Some(1);
                }
            }
        }
        "--first-run" => {
            // The offer a brand-new launcher makes on the way up
            // (`firstrun.rs`), without a window: `status` prints what the
            // dialog would show, `decline` answers it the way "Not now"
            // does, and `accept` runs the real download and the starter
            // profiles after it, waiting here for the thread a front end
            // would poll from a timer.
            let usage = "usage: --first-run [status|accept|decline]";
            let mut model = firstrun::FirstRun::check(shader_library::default_dir());
            match args.next().as_deref() {
                None | Some("status") => {}
                Some("decline") => model.decline(),
                Some("accept") => {
                    model.accept();
                    // The poll a front end's timer does, at a pace a
                    // terminal can read. `state` also turns a finished
                    // download into the profiles, so the loop ends on the
                    // step after that and the line below is the outcome,
                    // printed once.
                    while model.busy() {
                        std::thread::sleep(Duration::from_millis(500));
                        let message = model.state();
                        if message.step == firstrun::Step::Downloading {
                            println!("running: {}", message.detail);
                        }
                    }
                }
                Some(other) => panic!("unknown first-run action {other:?}; {usage}"),
            }
            if print_first_run(&mut model) == firstrun::Step::Failed {
                return Some(1);
            }
        }
        "--default-profiles" => {
            // The other half of a "yes", on a collection that is already
            // there: the starter profiles, so the flow can be checked
            // without 50 MB of network in a test suite (the
            // `shader-defaults` check).
            let presets = match args.next() {
                Some(dir) => PathBuf::from(dir),
                None => match shader_source::presets_dir() {
                    Some(dir) => dir,
                    None => {
                        eprintln!("[first-run] no preset collection; pass one, or --download-shaders first");
                        return Some(1);
                    }
                },
            };
            let dir = shader_library::default_dir();
            let added = shader_library::create_defaults(&dir, &presets);
            if added.is_empty() {
                println!("(nothing to add; {} already has them)", dir.display());
            }
            for name in added {
                println!("{name}");
            }
        }
        "--browse-start" => {
            // Where a path field's "Browse…" would open: the value's own
            // directory, or the preset collection for an empty preset
            // field, or where the last dialog was browsing. A second
            // argument `file` asks for any other field, which has no
            // suggestion of its own. The dialog itself is modal and needs
            // a human, so this checks the decision, not the dialog.
            let value = args.next().unwrap_or_default();
            let presets = match args.next().as_deref() {
                Some("file") => None,
                _ => shader_source::presets_dir(),
            };
            match browse::browse_start(&value, presets.as_deref()) {
                Some(dir) => println!("{}", dir.display()),
                None => println!("(OS default)"),
            }
        }
        "--picked" => {
            // What a path a dialog handed back is kept as: a document
            // portal path becomes the file's own (`browse::picked`), any
            // other is printed as given. `package-flatpak.sh` asks this
            // inside the sandbox with a file it exported through the
            // portal first.
            let path = args.next().expect("usage: --picked <path>");
            println!("{}", browse::picked(Path::new(&path)).display());
        }
        "--grants" => {
            // Every kept bookmark (`grants`), resolved as a launcher run
            // resolves them: the path and whether access started. In the
            // sandbox, a path whose access did not start cannot be opened.
            let grants = grants::restore();
            for g in &grants {
                match &g.path {
                    Ok(p) => println!("{}  {}", if g.open { "open  " } else { "closed" }, p.display()),
                    Err(e) => println!("broken  {} ({e})", g.bookmark.display()),
                }
            }
            println!(
                "{} kept in {}{}",
                grants.len(),
                grants::dir().map_or("(no data directory)".into(), |d| d.display().to_string()),
                if grants::sandboxed() { ", sandboxed" } else { "" }
            );
        }
        "--keep-grant" => {
            // Keep a bookmark for a path as a dialog's pick would, in or
            // out of the sandbox: outside it, to check the bookmarks
            // themselves with `--grants`.
            let path = args.next().expect("usage: --keep-grant <path>");
            match grants::bookmark_now(Path::new(&path)) {
                Ok(()) => println!("kept {path}"),
                Err(e) => {
                    eprintln!("--keep-grant: {e}");
                    return Some(1);
                }
            }
        }
        "--default-shader-profile" => {
            // The library's default profile (`shader_library::DEFAULT_FILE`),
            // what a machine on "(default)" plays with: prints the id, or
            // "(none)"; with an argument, sets it ("(none)" clears it).
            let dir = shader_library::default_dir();
            if let Some(id) = args.next() {
                let id = (id != "(none)").then_some(id);
                if let Some(id) = &id {
                    if shader_library::find(&dir, id).is_none() {
                        panic!("no profile {id} in {}", dir.display());
                    }
                }
                shader_library::set_default(&dir, id.as_deref()).expect("set the default profile");
            }
            println!("{}", shader_library::default_id(&dir).unwrap_or_else(|| "(none)".to_string()));
        }
        "--new-shader-profile" => {
            let usage = "usage: --new-shader-profile <name> <preset.slangp>";
            let name = args.next().expect(usage);
            let preset = args.next().expect(usage).into();
            let path = shader_library::create(&shader_library::default_dir(), name, preset).expect("create profile");
            println!("{}", path.display());
        }
        "--set-shader-param" => {
            let usage = "usage: --set-shader-param <profile.toml> <param> <value>";
            let path: PathBuf = args.next().expect(usage).into();
            let param = args.next().expect(usage);
            let value: f32 = args.next().expect(usage).parse().expect("value must be a number");
            let mut profile = shader_profile::ShaderProfile::load(&path).expect("load profile");
            profile.params.insert(param, value);
            profile.save(&path).expect("save profile");
            println!("{}", profile.params_arg().unwrap_or_default());
        }
        "--list-shader-params" => {
            let preset = args.next().expect("usage: --list-shader-params <preset.slangp>");
            let params = shader_profile::parameter_meta(Path::new(&preset)).expect("parse preset");
            for p in params {
                println!("{} [{}..{}] step {} = {} — {}", p.id, p.minimum, p.maximum, p.step, p.default, p.description);
            }
        }
        "--assign-shader" => {
            let usage = "usage: --assign-shader <machine.toml> <profile-id-or-(none)>";
            let path: PathBuf = args.next().expect(usage).into();
            let id = args.next().expect(usage);
            let mut machine = Machine::load(&path).expect("load bundle");
            machine.shader_profile = if id == "(none)" { None } else { Some(id) };
            machine.save(&path).expect("save bundle");
        }
        "--preview-shader" => {
            // Headless equivalent of the profile editor's live preview
            // pane: the real `preview::Preview`, on a real (if
            // windowless) adapter and device, proving the image-decode
            // and render path without a GUI click. Both binaries answer
            // it with the same code, so their PNGs are byte-identical
            // (doc 07).
            let usage = "usage: --preview-shader <preset.slangp> <image> <out.png> [name=value,...]";
            let preset: PathBuf = args.next().expect(usage).into();
            let image: PathBuf = args.next().expect(usage).into();
            let out = args.next().expect(usage);
            let params = shader_profile::parse_params(&args.next().unwrap_or_default());
            let (w, h) = preview_area_env();
            let mut preview = preview::Preview::headless().expect("a headless wgpu device");
            // The editor's preview follows a clock, so a preset whose
            // picture depends on the frame number moves in it. Here one
            // frame is named instead, so the same command twice gives
            // the same PNG.
            preview.pin_frame(preview_frame_env());
            preview.update(&preset, &params, &image, w, h);
            if let Some(err) = preview.error() {
                eprintln!("[preview] {err}");
            }
            // Whether the editor would keep redrawing this preset. This
            // is the only way a test can see that decision.
            println!("{}", if preview.frame_interval().is_some() { "animated" } else { "still" });
            preview.dump_png(&out).expect("no frame rendered");
        }
        _ => return None,
    }
    Some(0)
}

/// The snapshot window's own output, for the verbs that drive it.
pub fn print_snapshots(window: &snaps::Snapshots) {
    if let Some(status) = window.status() {
        println!("[snapshots] {status}");
    }
    if let Some(err) = window.error() {
        eprintln!("[snapshots] {err}");
    }
    // The tree as the window draws it: the name indented by its depth,
    // and a last column marking the snapshot the disk's present state
    // descends from (`current`) or one with no record (`no record`).
    for snap in window.snapshots() {
        let mark = if snap.current {
            "current"
        } else if snap.recorded {
            ""
        } else {
            "no record"
        };
        println!(
            "{}\t{}{}\t{}\t{}\t{mark}",
            snap.id,
            "  ".repeat(snap.depth),
            snap.name,
            snap.date_label(),
            snap.size_label()
        );
    }
}

/// One line for whatever the first-run dialog would be showing: the
/// step, then the words the dialog itself would put on screen, newlines
/// flattened so a check can grep one line. Both come from a single
/// `state()`, so what the suite reads is what a window draws.
fn print_first_run(model: &mut firstrun::FirstRun) -> firstrun::Step {
    let message = model.state();
    let name = match message.step {
        firstrun::Step::Idle => "idle",
        firstrun::Step::Asking => "asking",
        firstrun::Step::Downloading => "running",
        firstrun::Step::Failed => "failed",
        firstrun::Step::Done => "done",
    };
    if message.step == firstrun::Step::Idle {
        println!("idle");
    } else {
        println!("{name}: {} {}", message.headline, message.detail.replace('\n', " "));
    }
    message.step
}

/// Where every companion resolved, as text, one line each, in the format
/// `scripts/package-linux.sh` and `scripts/package-windows.sh` read.
/// Shared by `--paths`, which prints it, and `--diagnose`, which also
/// files it where a windowed program's user can find it.
fn paths_text() -> String {
    use std::fmt::Write;
    let mut s = String::new();
    match crate::paths::install_prefix() {
        Some(prefix) => writeln!(s, "prefix       {}", prefix.display()),
        None => writeln!(s, "prefix       (not installed; a checkout build)"),
    }
    .ok();
    writeln!(s, "checkout     {}", crate::paths::checkout(".").display()).ok();
    // The library: where machines, discs and profiles go. A packaged
    // (MSIX) launcher keeps it outside the virtualised AppData
    // (`paths::data_dir`), which is the one thing this line exists to show.
    match crate::paths::data_dir() {
        Some(dir) if crate::paths::packaged() => writeln!(s, "library      {} (packaged)", dir.display()),
        Some(dir) => writeln!(s, "library      {}", dir.display()),
        None => writeln!(s, "library      (no home directory)"),
    }
    .ok();
    writeln!(s, "player       {}", player::player_binary().display()).ok();
    writeln!(s, "player-x86_64 {}", player::target_player_binary("x86_64").display()).ok();
    // Windows 11 on Arm's player, which only an Arm host builds (M20)
    if cfg!(target_arch = "aarch64") {
        writeln!(s, "player-aarch64 {}", player::target_player_binary("aarch64").display()).ok();
    }
    writeln!(s, "qemu-img     {}", player::qemu_img_binary().display()).ok();
    writeln!(s, "pc-bios      {}", player::pc_bios_dir().display()).ok();
    match disc_library::guest_tools_iso() {
        Some(iso) => writeln!(s, "guest-tools  {}", iso.display()),
        None => writeln!(s, "guest-tools  (none built or shipped)"),
    }
    .ok();
    // Windows 11's drivers disc for this host's processor, the one its
    // machines get (M23)
    match disc_library::drivers_iso(crate::bundle::Arch::native()) {
        Some(iso) => writeln!(s, "drivers      {}", iso.display()),
        None => writeln!(s, "drivers      (none built or shipped)"),
    }
    .ok();
    match shader_source::presets_dir() {
        Some(dir) => writeln!(s, "shaders      {}", dir.display()),
        None => writeln!(
            s,
            "shaders      (none; downloadable into {})",
            shader_source::install_dir().display()
        ),
    }
    .ok();
    // The executor's other process (ADR-018, M15): the Wine it would run
    // on and the Windows build it would run there. A below-floor host
    // needs both, and each can be missing for a different reason.
    if !cfg!(windows) {
        match crate::host_gpu::wine() {
            Some(w) => writeln!(s, "wine         {} ({})", w.path.display(), w.version),
            None => writeln!(s, "wine         (none found)"),
        }
        .ok();
        match crate::host_gpu::exec_host() {
            Some(exe) => writeln!(s, "wine-host    {}", exe.display()),
            None => writeln!(s, "wine-host    (not built or shipped)"),
        }
        .ok();
    }
    // The Vulkan the probe and the executor run on: the package's own
    // loader and driver where the package carries them (the macOS app),
    // the system's everywhere else.
    match crate::host_gpu::shipped_loader() {
        Some(p) => writeln!(s, "vulkan       {}", p.display()),
        None => writeln!(s, "vulkan       (the system's loader)"),
    }
    .ok();
    match crate::host_gpu::shipped_icd() {
        Some(p) => writeln!(s, "vulkan-icd   {}", p.display()),
        None => writeln!(s, "vulkan-icd   (the system's drivers)"),
    }
    .ok();
    writeln!(s, "machines     {}", library::default_dir().display()).ok();
    writeln!(s, "discs        {}", disc_library::default_path().display()).ok();
    writeln!(s, "profiles     {}", shader_library::default_dir().display()).ok();
    s
}
