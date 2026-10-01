//! The player's picture in a mitsuami window on GTK 4 (a spike).
//!
//! The CRT chain renders into a ring of dma-buf images (`gpu.rs`); each one
//! goes to GTK as a `GdkDmabufTexture` on a `GtkPicture` inside a
//! `GtkGraphicsOffload`, which the compositor can show as a subsurface.
//! Beside it, mitsuami's own widgets: a menu and a status line.
//!
//! ```sh
//! player-gtk-spike [--shader <preset>]               # the player's test pattern, 60 Hz
//! player-gtk-spike [--shader <preset>] -- <qemu args> # a live guest
//! LAUNCHER_PLAYER_BIN=<this binary> 2ksbox           # the launcher's machines on it
//! ```
//!
//! It takes the player's own command line (`--shader`, `--shader-params`,
//! `--pad`, then `--` and QEMU's), so the launcher can start it in the
//! player's place. The pad is not read.
//!
//! `PLAYER_LATENCY=1` prints, every 240 shown frames, the same
//! publish→present-return the player prints, plus what GTK adds after it.

// the player's modules are edition 2021
#![allow(unsafe_op_in_unsafe_fn)]

#[path = "../../../player/src/audio.rs"]
#[allow(dead_code)]
mod audio;
#[path = "../../../player/src/dmabuf.rs"]
mod dmabuf;
mod gpu;
mod keys;
#[path = "../../../player/src/pattern.rs"]
#[allow(dead_code)]
mod pattern;
#[path = "../../../player/src/qemu_vm.rs"]
#[allow(dead_code)]
mod qemu_vm;
#[path = "../../../player/src/qmp.rs"]
#[allow(dead_code)]
mod qmp;
mod pres;
mod subsurface;

use gtk4::prelude::*;
use gtk4::{gdk, glib};
use mitsuami::gtk::NativeView;
use mitsuami::prelude::*;
use qemu_embed::Qemu;
use std::cell::RefCell;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// `qemu_vm.rs` calls the player's `PLAYER_DUMP` hook; the spike has none.
pub fn maybe_dump(_pixels: &[u32], _w: usize, _h: usize, _seq: u64) {}

enum Source {
    Pattern(Arc<Mutex<Option<(Vec<u32>, Instant)>>>),
    Qemu { vm: Qemu, display: qemu_vm::Display, last_seq: u64 },
}

/// One frame handed to GTK, waiting for its paint and then its presentation.
struct Shown {
    published: Instant,
    handoff: Instant,
    gpu_wait: Duration,
    /// the frame clock's counter of the paint that included it
    counter: Option<i64>,
    paint: Option<Instant>,
}

#[derive(Default)]
struct Stats {
    handoff: Vec<f32>,
    gpu_wait: Vec<f32>,
    paint: Vec<f32>,
    present: Vec<f32>,
    /// frames replaced on the picture before any paint included them
    superseded: u64,
    /// paints whose presentation time GTK never learned
    no_present_time: u64,
}

struct State {
    gpu: gpu::Gpu,
    source: Source,
    picture: gtk4::Picture,
    status: Signal<String>,
    /// handed off since the last paint; the last one is what the paint shows
    queued: Vec<Shown>,
    /// painted, waiting for the frame's presentation time
    painted: Vec<Shown>,
    stats: Stats,
    rendered_size: (u32, u32),
    /// Instant ↔ g_get_monotonic_time (both CLOCK_MONOTONIC on Linux)
    clock_base: (Instant, i64),
    qemu_thread: Option<std::thread::JoinHandle<i32>>,
    /// `SPIKE_MODE=subsurface`: the picture on a desync subsurface of our
    /// own instead of through GTK's offload
    sub_mode: bool,
    sub: Option<(subsurface::Sub, gpu::Surface, pres::Feedback)>,
    returned: Vec<f32>,
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

/// Wakes the GTK main loop from the QEMU (or pattern) thread; wakes
/// coalesce while one is pending.
fn waker() -> Arc<dyn Fn() + Send + Sync> {
    let pending = Arc::new(AtomicBool::new(false));
    Arc::new(move || {
        if pending.swap(true, Ordering::AcqRel) {
            return;
        }
        let pending = pending.clone();
        glib::MainContext::default().invoke(move || {
            pending.store(false, Ordering::Release);
            on_wake();
        });
    })
}

fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    STATE.with(|s| s.borrow_mut().as_mut().map(f))
}

/// A frame was published (or the picture's size changed): draw it and hand
/// it to GTK.
fn on_wake() {
    let stopped = with_state(|st| {
        let published = match &mut st.source {
            Source::Pattern(slot) => slot.lock().unwrap().take().map(|(fb, t)| {
                st.gpu.upload(&fb, pattern::WIDTH as u32, pattern::HEIGHT as u32);
                t
            }),
            Source::Qemu { display, last_seq, .. } => {
                if display.stopped() {
                    return true;
                }
                for d in display.take_dmabufs() {
                    st.gpu.import_slot(&d);
                }
                display.take_if_newer(*last_seq).map(|f| {
                    *last_seq = f.seq;
                    match f.ext_slot {
                        Some(s) => st.gpu.use_slot(s),
                        None => st.gpu.upload(&f.pixels, f.width as u32, f.height as u32),
                    }
                    f.published
                })
            }
        };
        let size = picture_px(&st.picture);
        if std::env::var_os("SPIKE_TRACE").is_some() {
            eprintln!("[trace] wake: published {} size {size:?} mapped {}", published.is_some(), st.picture.is_mapped());
        }
        if published.is_none() && size == st.rendered_size {
            return false;
        }
        if size.0 == 0 || size.1 == 0 {
            return false;
        }
        if st.sub_mode {
            present_on_sub(st, published, size);
            return false;
        }
        let Some((idx, gpu_wait)) = st.gpu.render(size.0, size.1) else { return false };
        st.rendered_size = size;
        let e = &st.gpu.ring[idx];
        let busy = e.busy.clone();
        let builder = gdk::DmabufTextureBuilder::new()
            .set_display(&st.picture.display())
            .set_width(e.w)
            .set_height(e.h)
            .set_fourcc(gpu::DRM_FORMAT_XRGB8888)
            .set_modifier(e.modifier)
            .set_n_planes(1)
            .set_offset(0, e.offset)
            .set_stride(0, e.stride);
        // The fd stays open in the ring until the release function says GTK
        // and the compositor are done with it.
        let built = unsafe {
            builder
                .set_fd(0, e.fd.as_raw_fd())
                .build_with_release_func(move || busy.store(false, Ordering::Release))
        };
        match built {
            Ok(tex) => st.picture.set_paintable(Some(&tex)),
            Err(err) => {
                eprintln!("[spike] GdkDmabufTexture: {err}");
                e.busy.store(false, Ordering::Release);
                return false;
            }
        }
        let now = Instant::now();
        if let Some(published) = published {
            st.queued.push(Shown { published, handoff: now, gpu_wait, counter: None, paint: None });
        }
        false
    })
    .unwrap_or(false);
    if stopped {
        quit();
    }
}

/// The subsurface mode's present: the player's own path, on our surface.
fn present_on_sub(st: &mut State, published: Option<Instant>, size: (u32, u32)) {
    if st.sub.is_none() {
        if !st.picture.is_mapped() {
            return;
        }
        match subsurface::Sub::new(&st.picture) {
            Ok(sub) => {
                let surface = st.gpu.surface_on(sub.display, sub.surface_ptr());
                let fb = unsafe { pres::Feedback::new(sub.display, sub.surface_ptr()) }.expect("presentation feedback");
                eprintln!("[spike] picture on a desync subsurface of our own");
                st.sub = Some((sub, surface, fb));
            }
            Err(e) => {
                eprintln!("[spike] no subsurface: {e}");
                st.sub_mode = false;
                return;
            }
        }
    }
    let (sub, surface, fb) = st.sub.as_mut().unwrap();
    sub.poll();
    sub.place(&st.picture);
    if let Some(t) = published {
        fb.request(t);
    }
    if st.gpu.render_to(surface, size.0, size.1) {
        st.rendered_size = size;
        if let Some(t) = published {
            st.returned.push(t.elapsed().as_secs_f32() * 1000.0);
        }
    }
    for (published, at) in fb.take() {
        match at {
            Some(at) => st.stats.present.push(at.saturating_duration_since(published).as_secs_f32() * 1000.0),
            None => st.stats.superseded += 1,
        }
    }
    if st.stats.present.len() >= 240 {
        let line = format!(
            "publish→present-return {} | publish→presented {} ms (discarded {})",
            pct(&mut st.returned),
            pct(&mut st.stats.present),
            st.stats.superseded
        );
        if std::env::var_os("PLAYER_LATENCY").is_some() {
            eprintln!("[latency] {line}");
        }
        st.status.set(format!("subsurface: {line}"));
        st.returned.clear();
        st.stats = Stats::default();
    }
}

/// The picture's size in device pixels.
fn picture_px(picture: &gtk4::Picture) -> (u32, u32) {
    let scale = picture.native().and_then(|n| n.surface()).map(|s| s.scale()).unwrap_or(1.0);
    ((picture.width() as f64 * scale).round() as u32, (picture.height() as f64 * scale).round() as u32)
}

/// After each paint: what it showed, and the presentation times GTK has
/// learned for earlier ones.
fn after_paint(clock: &gdk::FrameClock) {
    let resized = with_state(|st| {
        if let Some((sub, _, _)) = st.sub.as_mut() {
            sub.place(&st.picture);
        }
        let counter = clock.frame_counter();
        let now = Instant::now();
        if let Some(mut last) = st.queued.pop() {
            st.stats.superseded += st.queued.len() as u64;
            st.queued.clear();
            last.counter = Some(counter);
            last.paint = Some(now);
            st.painted.push(last);
        }
        let (base_i, base_us) = st.clock_base;
        let to_us = |t: Instant| match t.checked_duration_since(base_i) {
            Some(d) => base_us + d.as_micros() as i64,
            None => base_us - base_i.duration_since(t).as_micros() as i64,
        };
        let mut i = 0;
        while i < st.painted.len() {
            let s = &st.painted[i];
            let c = s.counter.unwrap();
            let complete = clock.timings(c).map(|t| (t.is_complete(), t.presentation_time()));
            match complete {
                Some((true, pt)) | Some((false, pt)) if pt != 0 => {
                    let ms = |t: Instant| t.duration_since(s.published).as_secs_f32() * 1000.0;
                    st.stats.handoff.push(ms(s.handoff));
                    st.stats.gpu_wait.push(s.gpu_wait.as_secs_f32() * 1000.0);
                    st.stats.paint.push(ms(s.paint.unwrap()));
                    st.stats.present.push((pt - to_us(s.published)) as f32 / 1000.0);
                    st.painted.swap_remove(i);
                }
                Some((true, _)) | None if counter - c > 8 => {
                    st.stats.no_present_time += 1;
                    st.painted.swap_remove(i);
                }
                _ => i += 1,
            }
        }
        if st.stats.present.len() >= 240 {
            report(st);
        }
        picture_px(&st.picture) != st.rendered_size
    })
    .unwrap_or(false);
    if resized {
        on_wake();
    }
}

fn pct(v: &mut [f32]) -> String {
    if v.is_empty() {
        return "-".into();
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    format!("p50 {:.1} p95 {:.1} max {:.1}", v[v.len() / 2], v[v.len() * 95 / 100], v[v.len() - 1])
}

fn report(st: &mut State) {
    let s = &mut st.stats;
    let line = format!(
        "publish→handoff {} | gpu wait {} | publish→paint {} | publish→presented {} ms (n={}, superseded {}, no time {}, starved {})",
        pct(&mut s.handoff),
        pct(&mut s.gpu_wait),
        pct(&mut s.paint),
        pct(&mut s.present),
        s.present.len(),
        s.superseded,
        s.no_present_time,
        st.gpu.starved,
    );
    if std::env::var_os("PLAYER_LATENCY").is_some() {
        eprintln!("[latency] {line}");
    }
    let mut present = s.present.clone();
    st.status.set(format!("publish→presented {} ms · ring {}", pct(&mut present), st.gpu.ring.len()));
    *s = Stats::default();
}

fn vm() -> Option<Qemu> {
    with_state(|st| match &st.source {
        Source::Qemu { vm, .. } => Some(*vm),
        Source::Pattern(_) => None,
    })
    .flatten()
}

fn send_key(sc: u32, down: bool) {
    if let Some(vm) = vm() {
        let q = qemu_embed::atset1_to_qcode(sc);
        if q != 0 {
            vm.key(q, down);
            vm.input_flush();
        }
    }
}

fn ctrl_alt_del() {
    for (sc, down) in [(0x1D, true), (0x38, true), (0xE053, true), (0xE053, false), (0x38, false), (0x1D, false)] {
        send_key(sc, down);
    }
}

/// Guest coordinates of a point on the picture (logical px), if it is on
/// the guest's image.
fn to_guest(x: f64, y: f64) -> Option<(i32, i32, i32, i32)> {
    with_state(|st| {
        let (gw, gh) = st.gpu.shown_size()?;
        let scale = st.picture.native().and_then(|n| n.surface()).map(|s| s.scale()).unwrap_or(1.0);
        let (pw, ph) = picture_px(&st.picture);
        let (vx, vy, vw, vh) = gpu::fit_4_3(pw, ph);
        let gx = ((x * scale) as f32 - vx) / vw * gw as f32;
        let gy = ((y * scale) as f32 - vy) / vh * gh as f32;
        (gx >= 0.0 && gy >= 0.0 && gx < gw as f32 && gy < gh as f32).then_some((gx as i32, gy as i32, gw as i32, gh as i32))
    })
    .flatten()
}

fn picture_widget(
    status: Signal<String>,
    source: Source,
    shader: Option<(std::path::PathBuf, Vec<(String, f32)>)>,
) -> gtk4::GraphicsOffload {
    let picture = gtk4::Picture::new();
    picture.set_can_shrink(true);
    picture.set_content_fit(gtk4::ContentFit::Fill);
    let offload = gtk4::GraphicsOffload::new(Some(&picture));
    offload.set_black_background(true);
    offload.set_focusable(true);
    offload.set_hexpand(true);
    offload.set_vexpand(true);

    let keys = gtk4::EventControllerKey::new();
    keys.connect_key_pressed(|_, _, code, _| {
        if let Some(sc) = keys::atset1(code) {
            send_key(sc, true);
        }
        glib::Propagation::Stop
    });
    keys.connect_key_released(|_, _, code, _| {
        if let Some(sc) = keys::atset1(code) {
            send_key(sc, false);
        }
    });
    offload.add_controller(keys);
    let motion = gtk4::EventControllerMotion::new();
    motion.connect_motion(|_, x, y| {
        if let (Some(vm), Some((gx, gy, gw, gh))) = (vm(), to_guest(x, y))
            && vm.mouse_is_absolute()
        {
            vm.mouse_abs(gx, gy, gw, gh);
            vm.input_flush();
        }
    });
    offload.add_controller(motion);
    let click = gtk4::GestureClick::new();
    click.set_button(0);
    let press = |g: &gtk4::GestureClick, down: bool| {
        let b = match g.current_button() {
            1 => 0,
            2 => 1,
            3 => 2,
            _ => return,
        };
        if let Some(vm) = vm() {
            vm.mouse_btn(b, down);
            vm.input_flush();
        }
    };
    click.connect_pressed(move |g, _, _, _| {
        if let Some(w) = g.widget() {
            w.grab_focus();
        }
        press(g, true)
    });
    click.connect_released(move |g, _, _, _| press(g, false));
    offload.add_controller(click);

    picture.connect_realize(|p| {
        if let Some(clock) = p.frame_clock() {
            clock.connect_after_paint(after_paint);
        }
        if std::env::var_os("SPIKE_INHIBIT").is_some()
            && let Some(top) = p.native().and_then(|n| n.surface()).and_then(|s| s.downcast::<gdk::Toplevel>().ok())
        {
            top.inhibit_system_shortcuts(None::<&gdk::Event>);
        }
    });
    picture.connect_map(|p| {
        p.parent().map(|o| o.grab_focus());
    });

    let mut gpu = gpu::Gpu::new();
    if let Some((path, params)) = shader {
        gpu.load_shader(&path, &params);
    }
    STATE.with(|s| {
        *s.borrow_mut() = Some(State {
            gpu,
            source,
            picture: picture.clone(),
            status,
            queued: Vec::new(),
            painted: Vec::new(),
            stats: Stats::default(),
            rendered_size: (0, 0),
            clock_base: (Instant::now(), glib::monotonic_time()),
            qemu_thread: None,
            sub_mode: std::env::var("SPIKE_MODE").as_deref() != Ok("offload"),
            sub: None,
            returned: Vec::new(),
        })
    });
    offload
}

/// Stop the machine and leave. QEMU's thread must be gone before the
/// process exits (CLAUDE.md), so this waits for it and then `_exit`s.
fn quit() {
    let (vm, join) = with_state(|st| {
        let vm = match &st.source {
            Source::Qemu { vm, display, .. } => {
                display.release();
                Some(*vm)
            }
            Source::Pattern(_) => None,
        };
        (vm, st.qemu_thread.take())
    })
    .unwrap_or((None, None));
    if let Some(vm) = vm {
        vm.vm_shutdown();
    }
    let mut status = 0;
    if let Some(join) = join {
        let deadline = Instant::now() + Duration::from_secs(15);
        while !join.is_finished() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(10));
        }
        status = if join.is_finished() { join.join().unwrap_or(1) } else { 1 };
    }
    unsafe { libc::_exit(status) };
}

/// The player's `--shader-params name=value,…`.
fn parse_shader_params(s: &str) -> Vec<(String, f32)> {
    s.split(',')
        .filter_map(|entry| {
            let (name, value) = entry.split_once('=')?;
            Some((name.trim().to_string(), value.trim().parse().ok()?))
        })
        .collect()
}

/// What the player finds for itself in a checkout (its `companions.rs`,
/// and QEMU's searches beside the player's binary, which this one is not
/// next to): the SoundFont and the Direct3D executor, from this checkout.
fn companions() {
    let root = std::path::Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../.."));
    for (var, rel) in [
        ("LIBSYNTH_SF2", "soundfonts/TimGM6mb.sf2"),
        ("D3DPT_EXEC_LIB", "build/d3dpt/libd3dpt_exec.so"),
        ("D3DPT_DXVK_LIB", "build/dxvk/src/d3d9/libdxvk_d3d9.so.0"),
    ] {
        let path = root.join(rel);
        if std::env::var_os(var).is_none() && path.exists() {
            // SAFETY: before any thread exists.
            unsafe { std::env::set_var(var, path) };
        }
    }
}

fn main() {
    companions();
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut shader = std::env::var("PLAYER_SHADER").ok().map(std::path::PathBuf::from);
    let mut params = Vec::new();
    while let Some(flag) = args.first().cloned() {
        match flag.as_str() {
            "--" => {
                args.remove(0);
                break;
            }
            "--shader" | "--shader-params" | "--pad" if args.len() >= 2 => {
                match flag.as_str() {
                    "--shader" => shader = Some(args[1].clone().into()),
                    "--shader-params" => params = parse_shader_params(&args[1]),
                    _ => eprintln!("[spike] {flag} {}: not read", args[1]),
                }
                args.drain(0..2);
            }
            _ => break,
        }
    }
    let shader = shader.map(|path| (path, params));
    let qemu_args = args;

    mitsuami::App::new()
        .window("2ksbox player (GTK spike)", Size::new(960.0, 760.0), move || {
            let status = signal(String::from("waiting for frames"));
            let wake = waker();
            let (source, qemu) = if qemu_args.is_empty() {
                let slot: Arc<Mutex<Option<(Vec<u32>, Instant)>>> = Arc::default();
                let (s, w) = (slot.clone(), wake.clone());
                let hz: f64 = std::env::var("PATTERN_HZ").ok().and_then(|v| v.parse().ok()).unwrap_or(60.0);
                std::thread::spawn(move || {
                    let mut p = pattern::Pattern::new();
                    let period = Duration::from_secs_f64(1.0 / hz);
                    let mut next = Instant::now();
                    loop {
                        p.render();
                        *s.lock().unwrap() = Some((p.fb.clone(), Instant::now()));
                        w();
                        next += period;
                        std::thread::sleep(next.saturating_duration_since(Instant::now()));
                    }
                });
                (Source::Pattern(slot), None)
            } else {
                // placeholder until the device exists (zero-copy needs it first)
                (Source::Pattern(Arc::default()), Some(qemu_args.clone()))
            };
            set_menu(
                MenuBar::new().menu(
                    Menu::new("Machine")
                        .item(MenuItem::new("Send Ctrl+Alt+Del").on_select(ctrl_alt_del))
                        .item(MenuItem::new("Reset").on_select(|| {
                            if let Some(vm) = vm() {
                                vm.vm_reset();
                            }
                        }))
                        .item(MenuItem::new("Power Button").on_select(|| {
                            if let Some(vm) = vm() {
                                vm.vm_powerdown();
                            }
                        }))
                        .item(MenuItem::new("Close Player").on_select(quit)),
                ),
            );
            let shader = shader.clone();
            let picture = NativeView::gtk(move |_cx| {
                let offload = picture_widget(status, source, shader);
                if let Some(args) = qemu {
                    let zero_copy = with_state(|st| st.gpu.zero_copy).unwrap_or(false);
                    let mut args = args;
                    let audio_cfg = if std::env::var_os("SPIKE_MUTE").is_some() {
                        // the machine's devices name embed0: a silent one
                        args.extend(["-audiodev".into(), "none,id=embed0".into()]);
                        None
                    } else {
                        let ring = audio::Ring::new();
                        let out = audio::start(ring.clone());
                        let cfg = out.as_ref().map(|o| (ring, o.sample_rate));
                        // the stream must outlive the machine
                        std::mem::forget(out);
                        cfg
                    };
                    let (vm, display, join, _qmp) = qemu_vm::start(args, audio_cfg, Some(wake), zero_copy);
                    with_state(|st| {
                        st.source = Source::Qemu { vm, display, last_seq: 0 };
                        st.qemu_thread = Some(join);
                    });
                }
                offload
            })
            .measure(|_, _| Size::new(0.0, 0.0))
            .grow(1.0)
            .basis(Length::Px(0.0));
            Column::new().height(Length::Percent(100.0)).children((
                picture,
                Text::new(move || status.get()).text_style(TextStyle::Caption).padding(6.0),
            ))
        })
        .run();
    quit();
}
