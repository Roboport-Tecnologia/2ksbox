//! One run of the player: what it shows (a guest, the test pattern, the
//! mode sweep or the calibration pass), the QEMU thread and the host audio
//! behind a guest, and what happens on each of the three things a front
//! end hears about: a wake from the QEMU thread (`wake`), a frame to draw
//! (`draw`, then `presented` once it is shown), and a turn of its event
//! loop (`headless_tick`).

use crate::gpu::{present_guest_frame, Gpu};
use crate::pattern::{self, Pattern};
use crate::sweep::{self, Calib, Sweep};
use crate::{audio, hard_exit, qemu_vm, qmp, shot_every, Args};
use qemu_embed::Qemu;
use std::sync::Arc;
use std::time::Instant;

pub enum Source {
    Pattern(Pattern),
    Sweep(Sweep),
    Calib(Calib),
    Qemu {
        vm: Qemu,
        display: qemu_vm::Display,
        last_seq: u64,
        qmp: Option<Arc<qmp::Qmp>>,
        /// PLAYER_QMP_EXEC ran (once, after the first guest frame)
        qmp_exec_done: bool,
    },
}

pub struct Session {
    source: Source,
    qemu_thread: Option<std::thread::JoinHandle<i32>>,
    /// The host's audio stream; it must outlive the machine.
    _audio: Option<audio::Output>,
    latency: Vec<f32>, // ms, publish→present per presented guest frame
    /// `PLAYER_SHOT_EVERY`: the guest-frame bucket the last periodic shot
    /// was taken in, so a run shoots once per bucket however often it
    /// redraws.
    shot_bucket: u64,
    /// Set once the machine is being shut down: no further calls into the
    /// VM handle, which the QEMU thread is about to destroy.
    closing: bool,
}

impl Session {
    /// Load the preset into `gpu` and start what `args` asks for. A guest
    /// starts here, its QEMU thread calling `waker` for every published
    /// frame (and every cursor change): render on publish, not on a
    /// free-running redraw, or a frame waits up to one host frame before it
    /// even reaches the swapchain (doc 03).
    pub fn start(args: &Args, gpu: &mut Gpu, waker: Option<Arc<dyn Fn() + Send + Sync>>) -> Session {
        if let Some(p) = &args.shader {
            gpu.load_shader(p, &args.shader_params);
        }
        let mut session = Session {
            source: Source::Pattern(Pattern::new()),
            qemu_thread: None,
            _audio: None,
            latency: Vec::new(),
            shot_bucket: 0,
            closing: false,
        };
        if let Some(target) = args.calib.clone() {
            let mut files: Vec<std::path::PathBuf> = if target.is_dir() {
                std::fs::read_dir(&target)
                    .map(|rd| {
                        rd.filter_map(|e| e.ok().map(|e| e.path()))
                            .filter(|p| p.extension().and_then(|e| e.to_str()) == Some("bmp"))
                            .collect()
                    })
                    .unwrap_or_default()
            } else {
                vec![target.clone()]
            };
            files.sort();
            if files.is_empty() {
                eprintln!("calib: no .bmp in {}", target.display());
                hard_exit(1);
            }
            if args.shader.is_none() {
                eprintln!("calib: --shader is the whole point; nothing to shade with");
                hard_exit(1);
            }
            gpu.force_surface(sweep::SWEEP_SURFACE);
            println!("shading {} calibration pattern(s)", files.len());
            session.source = Source::Calib(Calib {
                files,
                i: 0,
                last_surface: (0, 0),
            });
            return session;
        }
        if let Some(dir) = args.sweep.clone() {
            if let Err(e) = std::fs::create_dir_all(&dir) {
                eprintln!("mode sweep: {}: {e}", dir.display());
                hard_exit(1);
            }
            // big enough that every mode in the table gets at least two
            // output pixels per scanline, so the count is measurable for all
            // of them rather than only the low-resolution ones
            gpu.force_surface(sweep::SWEEP_SURFACE);
            println!(
                "mode sweep into {} at {}x{}",
                dir.display(),
                sweep::SWEEP_SURFACE.0,
                sweep::SWEEP_SURFACE.1
            );
            session.source = Source::Sweep(Sweep::new(dir, args.shader.is_some()));
            return session;
        }
        if args.qemu.is_empty() {
            return session;
        }
        // host audio first: QEMU's audiodev must match the device rate
        let ring = audio::Ring::new();
        let audio_out = audio::start(ring.clone());
        if audio_out.is_none() {
            eprintln!("[audio] no output device; guest audio disabled");
        }
        let audio_cfg = audio_out.as_ref().map(|o| (ring, o.sample_rate));
        session._audio = audio_out;
        let (vm, display, join, qmp) = qemu_vm::start(args.qemu.clone(), audio_cfg, waker, gpu.zero_copy());
        session.qemu_thread = Some(join);
        session.source = Source::Qemu {
            vm,
            display,
            last_seq: 0,
            qmp,
            qmp_exec_done: false,
        };
        session
    }

    /// The machine's handle, while it may be called.
    pub fn vm(&self) -> Option<Qemu> {
        match &self.source {
            Source::Qemu { vm, .. } if !self.closing => Some(*vm),
            _ => None,
        }
    }

    /// The guest's display, while there is a guest.
    pub fn display(&self) -> Option<&qemu_vm::Display> {
        match &self.source {
            Source::Qemu { display, .. } => Some(display),
            _ => None,
        }
    }

    /// The QMP connection the player keeps to its own QEMU.
    pub fn qmp(&self) -> Option<Arc<qmp::Qmp>> {
        match &self.source {
            Source::Qemu { qmp, .. } => qmp.clone(),
            _ => None,
        }
    }

    /// The sweep and the calibration pass render to their own surface and
    /// never present: they must not acquire either. With FIFO the second
    /// acquire blocks until the first image has been scanned out, which an
    /// occluded window (a test run behind a terminal, the usual case) never
    /// does.
    pub fn offscreen(&self) -> bool {
        matches!(self.source, Source::Sweep(_) | Source::Calib(_))
    }

    /// Whether the next frame is wanted at once: everything but a guest,
    /// which wakes the front end when it publishes.
    pub fn animates(&self) -> bool {
        !matches!(self.source, Source::Qemu { .. })
    }

    /// The newest picture into `gpu`, the chain run, and the picture drawn
    /// into `frame` (none for an offscreen run). Returns the publish time of
    /// the guest frame drawn, if a new one was. `composite_cursor`: the
    /// guest's cursor goes into the frame, because the host cursor cannot
    /// stand in for it.
    pub fn draw(
        &mut self,
        gpu: &mut Gpu,
        frame: Option<&wgpu::SurfaceTexture>,
        composite_cursor: bool,
    ) -> Option<Instant> {
        let mut published = None;
        match &mut self.source {
            Source::Pattern(p) => {
                p.render();
                gpu.upload(&p.fb, pattern::WIDTH as u32, pattern::HEIGHT as u32);
            }
            Source::Sweep(s) => sweep::sweep_upload(gpu, s),
            Source::Calib(c) => match sweep::read_bmp(&c.files[c.i]) {
                Ok((w, h, px)) => gpu.upload(&px, w, h),
                Err(e) => {
                    eprintln!("calib: {e}");
                    hard_exit(1);
                }
            },
            Source::Qemu {
                display, last_seq, ..
            } => {
                published = present_guest_frame(gpu, display, last_seq, composite_cursor);
            }
        }
        gpu.render(frame);
        published
    }

    /// After the frame `draw` drew was presented (or would have been):
    /// the periodic shot, the latency figure, and the sweep and
    /// calibration steps, which exit the process when they are done.
    pub fn presented(&mut self, gpu: &mut Gpu, published: Option<Instant>) {
        // PLAYER_SHOT_EVERY=<n>: the Ctrl+Alt+S shot on its own, once
        // every n guest frames. How a scripted run sees a 3D frame at
        // all: a QMP screendump shows only the VGA surface, which is
        // frozen while the guest presents through the 3D device
        // (CLAUDE.md), and this shot is of whatever the window shows,
        // the imported 3D slot included.
        if published.is_some() {
            if let (Some(every), Source::Qemu { last_seq, .. }) = (shot_every(), &self.source) {
                let bucket = *last_seq / every;
                if bucket != self.shot_bucket {
                    self.shot_bucket = bucket;
                    gpu.screenshot();
                }
            }
        }
        if let Some(t) = published {
            // publish→present (measured after the present call), doc 03's latency gate
            self.latency.push(t.elapsed().as_secs_f32() * 1000.0);
            if self.latency.len() >= 240 {
                if std::env::var("PLAYER_LATENCY").is_ok() {
                    let mut v = self.latency.clone();
                    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
                    eprintln!(
                        "[latency] publish→present p50 {:.1} ms  p95 {:.1} ms  max {:.1} ms (n={})",
                        v[v.len() / 2],
                        v[v.len() * 95 / 100],
                        v[v.len() - 1],
                        v.len()
                    );
                }
                self.latency.clear();
            }
        }
        if let Source::Calib(c) = &mut self.source {
            if c.last_surface != gpu.surface_size() {
                c.last_surface = gpu.surface_size();
            } else {
                let src = c.files[c.i].clone();
                match gpu.chain.as_ref().and_then(|ch| ch.output_texture()) {
                    Some(tex) => {
                        let (ow, oh, rgb) = shader_chain::read_texture(&gpu.device, &gpu.queue, tex);
                        let out = src.with_extension("shaded.png");
                        shader_chain::write_png(&out.to_string_lossy(), ow, oh, &rgb);
                        println!(
                            "  {} → {} ({ow}x{oh})",
                            src.file_name().unwrap_or_default().to_string_lossy(),
                            out.display()
                        );
                    }
                    None => {
                        eprintln!("calib: the preset did not load");
                        hard_exit(1);
                    }
                }
                c.i += 1;
                if c.i >= c.files.len() {
                    hard_exit(0);
                }
            }
        }
        if let Source::Sweep(s) = &mut self.source {
            if sweep::sweep_step(gpu, s) {
                if s.fails.is_empty() {
                    println!("mode sweep: {} modes OK", s.sizes.len());
                    hard_exit(0);
                }
                println!("mode sweep: {} of {} modes failed", s.fails.len(), s.sizes.len());
                hard_exit(1);
            }
        }
    }

    /// The QEMU thread woke the front end: a frame was published, or the
    /// guest's cursor changed. Imports the 3D slots it announced (here, not
    /// on the redraw: an occluded window gets no usable swapchain image but
    /// must still keep up), follows the adapter's hold on the window's size,
    /// and reads QMP. Returns true once QEMU's main loop has returned
    /// (guest power-off, `quit`): the front end then leaves, and joins.
    pub fn wake(&mut self, gpu: &mut Gpu) -> bool {
        let follows = self.vm().is_some_and(|vm| vm.display_follows_window());
        gpu.set_follows_window(follows);
        if let Source::Qemu { display, .. } = &self.source {
            for d in display.take_dmabufs() {
                gpu.import_slot(&d);
            }
            if display.stopped() && !self.closing {
                self.closing = true;
                return true;
            }
        }
        if let Source::Qemu {
            qmp: Some(qmp),
            qmp_exec_done,
            last_seq,
            ..
        } = &mut self.source
        {
            // QMP events: all of them under PLAYER_QMP=1, the notable ones always
            let verbose = std::env::var("PLAYER_QMP").is_ok();
            for ev in qmp.take_events() {
                let name = ev["event"].as_str().unwrap_or("?");
                if verbose || qmp::is_notable(name) {
                    eprintln!("[qmp] event {name} {}", ev.get("data").unwrap_or(&serde_json::Value::Null));
                }
            }
            // PLAYER_QMP_EXEC: one request object or an array of them, run once
            // the guest has drawn. A shell-level way to try commands
            // (eject, blockdev-change-medium, snapshot-save, ...).
            if !*qmp_exec_done && *last_seq > 0 {
                *qmp_exec_done = true;
                if let Ok(spec) = std::env::var("PLAYER_QMP_EXEC") {
                    let reqs = match serde_json::from_str::<serde_json::Value>(&spec) {
                        Ok(serde_json::Value::Array(reqs)) => reqs,
                        Ok(r) => vec![r],
                        Err(e) => {
                            eprintln!("[qmp] PLAYER_QMP_EXEC is not JSON: {e}");
                            Vec::new()
                        }
                    };
                    // On a thread of their own, not this one: a `quit`'s
                    // reply comes only once QEMU has torn down, and QEMU's
                    // thread waits for this one to release the VM before
                    // it does. Blocked here, this thread let it tear down
                    // after 5 s anyway and then read a freed surface: a
                    // segfault at exit in about one run in twelve.
                    let qmp = qmp.clone();
                    std::thread::spawn(move || {
                        for r in &reqs {
                            eprintln!("[qmp] {r} -> {:?}", qmp.execute_raw(r));
                        }
                    });
                }
            }
        }
        false
    }

    /// Headless verification (PLAYER_DUMP_OUT, PLAYER_SHOT_EVERY), once per
    /// turn of the front end's event loop. An occluded window may never be
    /// redrawn, and a scripted run's window is behind a terminal or on
    /// another workspace as a rule. The shader chain renders into our own
    /// texture, so the frame is driven from here in those modes. With the
    /// periodic shot on the redraw path, a PLAYER=1 run of
    /// tools/win98-game-test.sh took no shot at all.
    pub fn headless_tick(&mut self, gpu: &mut Gpu) {
        let every = shot_every();
        if std::env::var("PLAYER_DUMP_OUT").is_err() && every.is_none() {
            return;
        }
        if let Source::Qemu {
            display, last_seq, ..
        } = &mut self.source
        {
            if present_guest_frame(gpu, display, last_seq, true).is_some() {
                gpu.render(None);
                if let Some(every) = every {
                    let bucket = *last_seq / every;
                    if bucket != self.shot_bucket {
                        self.shot_bucket = bucket;
                        gpu.screenshot();
                    }
                }
            }
        }
    }

    /// The window's drawable size and DPI to the guest's adapter, which
    /// takes it as its mode when it can (`Qemu::set_window_size`). Physical
    /// pixels, so a HiDPI screen gets a sharp 1:1 picture (user decision;
    /// Windows' scaling is the user's to set). Sent on every resize: Windows
    /// 11 on Arm's viogpudo takes the size only when it starts, so a
    /// resize shows after Windows restarts, and QEMU keeps the last size
    /// for it.
    pub fn tell_window_size(&self, gpu: &Gpu, size: (u32, u32), scale: f64) {
        let Some(vm) = self.vm() else { return };
        if gpu.is_forced() {
            return;
        }
        let dpi = (96.0 * scale).round() as u32;
        vm.set_window_size(size.0, size.1, dpi);
    }

    /// Pull the plug: QEMU stops at once, and nothing calls the handle
    /// again. The front end then leaves its event loop and calls `join`.
    pub fn shut_down(&mut self) {
        if let Some(vm) = self.vm() {
            vm.vm_shutdown();
        }
        self.closing = true;
    }

    /// Orderly exit: QEMU must finish `qemu_cleanup` before the process
    /// exits, or QEMU's own atexit handlers (audio_cleanup, exit notifiers)
    /// run on this thread concurrently with the main loop. On macOS that shows as
    /// `assertion failed: mutex->initialized` in qemu_mutex_lock_impl.
    /// Returns QEMU's exit status.
    pub fn join(&mut self) -> i32 {
        self.closing = true;
        if let Source::Qemu { display, .. } = &self.source {
            display.release(); // no more calls from this thread: cleanup may free
        }
        let Some(handle) = self.qemu_thread.take() else {
            return 0;
        };
        let deadline = Instant::now() + std::time::Duration::from_secs(15);
        while !handle.is_finished() {
            if Instant::now() > deadline {
                eprintln!("[player] QEMU did not shut down in 15 s; exiting without cleanup");
                hard_exit(1);
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        handle.join().unwrap_or(1)
    }
}
