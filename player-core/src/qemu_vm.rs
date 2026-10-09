//! In-process QEMU: spawns the QEMU thread, receives display callbacks
//! (QEMU thread, BQL held, so copy and return), publishes frames to the
//! render thread.

use qemu_embed::{Qemu, RawDisplayCb};
use std::ffi::{c_int, c_void};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// A backend ring slot offered for zero-copy import: a dma-buf (Linux,
/// `fd` >= 0) or an IOSurface (macOS, `iosurface` non-null).
pub struct DmaBuf {
    pub slot: usize,
    pub fd: i32,
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    pub fourcc: u32,
    pub modifier: u64,
    #[cfg_attr(not(target_os = "macos"), allow(dead_code))]
    pub iosurface: usize,
}

/// A rectangle of guest pixels, `x0..x1` by `y0..y1`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x0: usize,
    pub y0: usize,
    pub x1: usize,
    pub y1: usize,
}

impl Rect {
    pub fn whole(w: usize, h: usize) -> Rect {
        Rect { x0: 0, y0: 0, x1: w, y1: h }
    }
    fn union(a: Option<Rect>, b: Rect) -> Rect {
        match a {
            None => b,
            Some(a) => Rect { x0: a.x0.min(b.x0), y0: a.y0.min(b.y0), x1: a.x1.max(b.x1), y1: a.y1.max(b.y1) },
        }
    }
}

/// The frame the QEMU thread published, kept whole in `pixels`.
struct Frame {
    /// Some(slot): the frame lives in an imported dma-buf
    ext_slot: Option<usize>,
    width: usize,
    height: usize,
    pixels: Vec<u32>,
    /// what changed since the render thread last took it (M22): only that
    /// is uploaded
    pending: Option<Rect>,
    seq: u64,
    /// when the QEMU refresh tick published this frame
    published: std::time::Instant,
}

/// The newest frame, lent to the render thread under the lock
/// (`Display::take_newer`): `dirty` is what changed since its last take.
pub struct FrameRef<'a> {
    pub ext_slot: Option<usize>,
    pub width: usize,
    pub height: usize,
    pub pixels: &'a [u32],
    pub dirty: Option<Rect>,
    pub seq: u64,
    pub published: std::time::Instant,
}

/// Debug: `PLAYER_KEYS="90:enter,150:1"` presses named keys at guest frame
/// numbers (headless input verification; names: a-z, 0-9, enter, esc, space,
/// tab, up, down, left, right, f1-f12).
struct KeyScript {
    steps: Vec<(u64, Vec<u32>, bool)>, // (frame seq, AT set-1 scancodes pressed together, down)
    next: usize,
}

fn key_name_to_atset1(name: &str) -> Option<u32> {
    let n = name.to_ascii_lowercase();
    let b = n.as_bytes();
    if b.len() == 1 {
        return match b[0] {
            b'a'..=b'z' => {
                const ROW: &[(u8, u32)] = &[
                    (b'q', 0x10),
                    (b'w', 0x11),
                    (b'e', 0x12),
                    (b'r', 0x13),
                    (b't', 0x14),
                    (b'y', 0x15),
                    (b'u', 0x16),
                    (b'i', 0x17),
                    (b'o', 0x18),
                    (b'p', 0x19),
                    (b'a', 0x1E),
                    (b's', 0x1F),
                    (b'd', 0x20),
                    (b'f', 0x21),
                    (b'g', 0x22),
                    (b'h', 0x23),
                    (b'j', 0x24),
                    (b'k', 0x25),
                    (b'l', 0x26),
                    (b'z', 0x2C),
                    (b'x', 0x2D),
                    (b'c', 0x2E),
                    (b'v', 0x2F),
                    (b'b', 0x30),
                    (b'n', 0x31),
                    (b'm', 0x32),
                ];
                ROW.iter().find(|(c, _)| *c == b[0]).map(|(_, s)| *s)
            }
            b'1'..=b'9' => Some(0x02 + (b[0] - b'1') as u32),
            b'0' => Some(0x0B),
            _ => None,
        };
    }
    Some(match n.as_str() {
        "ctrl" => 0x1D,
        "alt" => 0x38,
        "shift" => 0x2A,
        "enter" => 0x1C,
        "esc" => 0x01,
        "space" => 0x39,
        "tab" => 0x0F,
        "up" => 0xE048,
        "down" => 0xE050,
        "left" => 0xE04B,
        "right" => 0xE04D,
        _ => {
            let f: u32 = n.strip_prefix('f')?.parse().ok()?;
            match f {
                1..=10 => 0x3B + f - 1,
                11 => 0x57,
                12 => 0x58,
                _ => return None,
            }
        }
    })
}

fn key_script_from_env() -> Option<KeyScript> {
    let spec = std::env::var("PLAYER_KEYS").ok()?;
    let mut steps = Vec::new();
    for item in spec.split(',') {
        let (at, names) = item.split_once(':')?;
        let chord: Option<Vec<u32>> = names
            .split('+')
            .map(|n| key_name_to_atset1(n.trim()))
            .collect();
        // a real press: down at the frame, up PLAYER_KEYS_HOLD frames later
        // (default 6 ≈ 100 ms at the 16 ms refresh); a down+up in one flush is
        // a zero-length press that a game polling the keyboard never sees
        let hold: u64 = std::env::var("PLAYER_KEYS_HOLD")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(6);
        let at: u64 = at.trim().parse().ok()?;
        let chord = chord?;
        steps.push((at, chord.clone(), true));
        steps.push((at + hold, chord, false));
    }
    steps.sort();
    Some(KeyScript { steps, next: 0 })
}

struct Shared {
    vm: Option<Qemu>,
    script: Option<KeyScript>,
    // current surface (valid between on_switch calls, QEMU thread only)
    surface: *const u8,
    stride: usize,
    width: usize,
    height: usize,
    back: Vec<u32>,
    // what changed in `back` since the last publish: only that is copied
    // into `front` (M22)
    back_dirty: Option<Rect>,
    // `front` does not hold `back`'s picture (a 3D readback, a frame shown
    // black): the next VGA publish copies all of it
    front_stale: bool,
    // a real mode change happened at this instant; while it is recent, a
    // uniform single-colour VGA frame is the guest's transitional fill (XP
    // paints white around a switch) and is published as black instead
    switched_at: Option<std::time::Instant>,
    blanked: u32,
    // published to the render thread
    front: Frame,
    // wakes the render thread after each publish (event-loop proxy)
    waker: Option<Arc<dyn Fn() + Send + Sync>>,
    // 3D pass-through active (GL swaps or the Direct3D device's presents)
    three_d: bool,
    // the last 3D frame's instant, and whether the VGA surface changed since
    // the last publish: while 3D is active the VGA surface is shown only when
    // no 3D frame arrived for VGA_FALLBACK and the guest drew on it (a game
    // waiting at a DirectShow movie or a dialog, a process that died without
    // releasing the device)
    last_3d: Option<std::time::Instant>,
    vga_dirty: bool,
    showing_vga: bool,
    // zero-copy: the GPU side can import dma-bufs; offered slots wait here
    zero_copy: bool,
    dmabufs: Vec<DmaBuf>,
    // QEMU's main loop has returned; the handle must not be used any more
    stopped: bool,
    // the UI thread promises no further calls; qemu_embed_destroy may run
    released: bool,
    // the guest's hardware cursor (the d3dpt-vga driver, doc 15): its shape
    // as the guest defined it, a sequence number per define / clear, and
    // whether the guest shows it. None until the guest ever positioned it
    // (a guest with a software pointer, cirrus / vga.sys, never does)
    cursor: Option<Arc<CursorImage>>,
    cursor_seq: u64,
    cursor_visible: Option<bool>,
    // where the guest put it (the hot spot, guest pixels): the sprite is
    // composited there when the host cursor cannot stand in for it (a
    // relative mouse, a grab)
    cursor_x: i32,
    cursor_y: i32,
}
unsafe impl Send for Shared {}

/// A guest hardware-cursor shape: straight alpha, 0xAARRGGBB per pixel.
pub struct CursorImage {
    pub argb: Vec<u32>,
    pub width: u32,
    pub height: u32,
    pub hot_x: u32,
    pub hot_y: u32,
}

pub struct Display(Arc<Mutex<Shared>>);

impl Display {
    /// True once QEMU's main loop has returned (guest power-off, `quit`).
    pub fn stopped(&self) -> bool {
        self.0.lock().unwrap().stopped
    }
    /// Promise that no thread but the QEMU thread will touch the VM handle
    /// from now on; lets the QEMU thread proceed to `qemu_embed_destroy`.
    pub fn release(&self) {
        self.0.lock().unwrap().released = true;
    }
    /// Copy out the latest frame if it changed since `last_seq`.
    /// Lend the newest frame to `f` if it is newer than `last_seq`, under
    /// the lock: `f` uploads what changed straight from the published
    /// pixels, so no whole frame is copied to take it (M22). What changed
    /// is then the render thread's.
    pub fn take_newer<R>(&self, last_seq: u64, f: impl FnOnce(FrameRef) -> R) -> Option<R> {
        let mut s = self.0.lock().unwrap();
        if s.front.seq == last_seq || s.front.width == 0 {
            return None;
        }
        let dirty = s.front.pending.take();
        let fr = &s.front;
        Some(f(FrameRef {
            ext_slot: fr.ext_slot,
            width: fr.width,
            height: fr.height,
            pixels: &fr.pixels,
            dirty,
            seq: fr.seq,
            published: fr.published,
        }))
    }

    /// dma-buf slots offered since the last call (import them on the GPU thread).
    /// How many frames QEMU has *published*, whether or not any of them
    /// reached the screen.
    ///
    /// Not the same number as the `last_seq` the render path keeps: that
    /// one only moves when a frame is actually presented, and an occluded
    /// window presents nothing. `PLAYER_PAD_SCRIPT` is keyed on this one
    /// so a headless run (every scripted run is one) advances the
    /// script at the guest's own rate rather than stopping dead behind a
    /// terminal window.
    pub fn published_seq(&self) -> u64 {
        self.0.lock().unwrap().front.seq
    }

    pub fn take_dmabufs(&self) -> Vec<DmaBuf> {
        std::mem::take(&mut self.0.lock().unwrap().dmabufs)
    }

    /// The guest's cursor shape if it changed since `last_seq`: the new
    /// sequence number and the shape (None = the guest cleared it).
    pub fn cursor_if_newer(&self, last_seq: u64) -> Option<(u64, Option<Arc<CursorImage>>)> {
        let s = self.0.lock().unwrap();
        if s.cursor_seq == last_seq {
            return None;
        }
        Some((s.cursor_seq, s.cursor.clone()))
    }

    /// Whether the guest shows its hardware cursor; None while it never had one.
    pub fn cursor_visible(&self) -> Option<bool> {
        self.0.lock().unwrap().cursor_visible
    }

    /// The sprite to composite: the shape and the hot spot's guest position,
    /// while the guest shows a hardware cursor.
    pub fn cursor_sprite(&self) -> Option<(Arc<CursorImage>, i32, i32)> {
        let s = self.0.lock().unwrap();
        if s.cursor_visible != Some(true) {
            return None;
        }
        s.cursor.clone().map(|c| (c, s.cursor_x, s.cursor_y))
    }
}

/// Blend a cursor sprite into an XRGB frame at its hot-spot position.
pub fn composite_cursor(pixels: &mut [u32], width: usize, height: usize, c: &CursorImage, x: i32, y: i32) {
    let x0 = x - c.hot_x as i32;
    let y0 = y - c.hot_y as i32;
    for cy in 0..c.height as i32 {
        let fy = y0 + cy;
        if fy < 0 || fy >= height as i32 {
            continue;
        }
        for cx in 0..c.width as i32 {
            let fx = x0 + cx;
            if fx < 0 || fx >= width as i32 {
                continue;
            }
            let src = c.argb[(cy as u32 * c.width + cx as u32) as usize];
            let a = src >> 24;
            if a == 0 {
                continue;
            }
            let dst = &mut pixels[fy as usize * width + fx as usize];
            if a == 255 {
                *dst = src & 0x00ff_ffff;
                continue;
            }
            let mut out = 0u32;
            for shift in [0, 8, 16] {
                let s = (src >> shift) & 0xff;
                let d = (*dst >> shift) & 0xff;
                out |= ((s * a + d * (255 - a)) / 255) << shift;
            }
            *dst = out;
        }
    }
}

unsafe extern "C" fn on_cursor(
    ud: *mut c_void,
    argb: *const u32,
    w: c_int,
    h: c_int,
    hot_x: c_int,
    hot_y: c_int,
) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    s.cursor = if argb.is_null() || w <= 0 || h <= 0 {
        None
    } else {
        let n = w as usize * h as usize;
        Some(Arc::new(CursorImage {
            argb: std::slice::from_raw_parts(argb, n).to_vec(),
            width: w as u32,
            height: h as u32,
            hot_x: hot_x.max(0) as u32,
            hot_y: hot_y.max(0) as u32,
        }))
    };
    s.cursor_seq += 1;
    if s.cursor_seq <= 4 {
        let opaque = s.cursor.as_ref().map(|c| c.argb.iter().filter(|p| **p >> 24 != 0).count()).unwrap_or(0);
        eprintln!("[cursor] guest shape {}", if s.cursor.is_some() { format!("{w}x{h} hot {hot_x},{hot_y}, {opaque} opaque pixels") } else { "cleared".into() });
    }
    let waker = s.waker.clone();
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

unsafe extern "C" fn on_mouse_set(ud: *mut c_void, x: c_int, y: c_int, on: bool) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    let changed = s.cursor_visible != Some(on) || s.cursor_x != x || s.cursor_y != y;
    if s.cursor_visible != Some(on) && s.cursor_seq <= 4 {
        eprintln!("[cursor] guest {} it at {x},{y}", if on { "shows" } else { "hides" });
    }
    s.cursor_visible = Some(on);
    s.cursor_x = x;
    s.cursor_y = y;
    // a move alone republishes the frame: the UI composites the sprite at
    // the new place when it has to (no cost when the host cursor is used,
    // since the copy happens only if the UI takes the frame)
    if changed && s.cursor.is_some() && s.front.width != 0 && s.front.ext_slot.is_none() {
        publish_trace(&format_args!("cursor"));
        s.front.seq += 1;
    }
    let waker = if changed { s.waker.clone() } else { None };
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

unsafe extern "C" fn on_switch(
    ud: *mut c_void,
    px: *const u8,
    w: c_int,
    h: c_int,
    stride: c_int,
    _fmt: u32,
) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    // a DirectDraw page flip is a switch to another VRAM offset at the same
    // size, once per frame: log only real mode changes
    if s.width != w as usize || s.height != h as usize || s.stride != stride as usize {
        eprintln!("[display] switch {w}x{h} stride {stride}");
        s.switched_at = Some(std::time::Instant::now());
    }
    s.surface = px;
    s.stride = stride as usize;
    s.width = w as usize;
    s.height = h as usize;
    let n = s.width * s.height;
    s.back.clear();
    s.back.resize(n, 0);
    s.vga_dirty = true;
    copy_rect(&mut s, 0, 0, w as usize, h as usize);
}

unsafe extern "C" fn on_update(ud: *mut c_void, x: c_int, y: c_int, w: c_int, h: c_int) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    s.vga_dirty = true;
    copy_rect(&mut s, x as usize, y as usize, w as usize, h as usize);
}

unsafe extern "C" fn on_3d_active(ud: *mut c_void, active: bool) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    s.three_d = active;
    eprintln!("[3d] pass-through {}", if active { "on" } else { "off" });
}

/// While 3D is active, the VGA surface is shown once no 3D frame has
/// arrived for this long and the guest has drawn on the surface since.
const VGA_FALLBACK: std::time::Duration = std::time::Duration::from_millis(1000);

fn note_3d_frame(s: &mut Shared) {
    s.last_3d = Some(std::time::Instant::now());
    if s.showing_vga {
        s.showing_vga = false;
        eprintln!("[display] 3D frames again, VGA surface hidden");
    }
}

/// A presented 3D frame (vCPU thread): publish it directly, this is the
/// guest's vsync point, not the refresh tick.
unsafe extern "C" fn on_3d_frame(ud: *mut c_void, px: *const u8, w: c_int, h: c_int, stride: c_int) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    note_3d_frame(&mut s);
    let (w, h, stride) = (w as usize, h as usize, stride as usize);
    if s.front.width != w || s.front.height != h {
        s.front.width = w;
        s.front.height = h;
        s.front.pixels = vec![0; w * h];
        s.front.pending = None;
    }
    for y in 0..h {
        let row = std::slice::from_raw_parts(px.add(y * stride) as *const u32, w);
        s.front.pixels[y * w..(y + 1) * w].copy_from_slice(row);
    }
    s.front.pending = Some(Rect::whole(w, h));
    s.front_stale = true;
    publish_trace(&format_args!("readback {w}x{h}"));
    s.front.ext_slot = None;
    s.front.seq += 1;
    s.front.published = std::time::Instant::now();
    let waker = s.waker.clone();
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

/// The backend offers a ring slot's dma-buf (vCPU thread). Accept only when
/// the GPU side can import; the fd is ours from here on.
unsafe extern "C" fn on_3d_dmabuf(
    ud: *mut c_void,
    slot: c_int,
    fd: c_int,
    w: c_int,
    h: c_int,
    stride: c_int,
    fourcc: u32,
    modifier: u64,
) -> c_int {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    if !s.zero_copy || slot < 0 {
        return 0;
    }
    // `PLAYER_ZC_IMPORT=0`: take the offer and import nothing. Declining one
    // turns the whole ring off (the backend falls back to reading frames
    // back), so this is the only way to have the ring running with no Vulkan
    // behind it. That tells whether importing a buffer is what
    // stops it being written through (doc 12 §4). The picture is wrong while
    // it is set: nothing has the slots, so every 3D frame falls back to the
    // VGA surface. It is for reading the backend's own `EMBED_ZC_CHECK`
    // lines, not for looking at.
    if std::env::var("PLAYER_ZC_IMPORT").as_deref() == Ok("0") {
        libc::close(fd);
        return 1;
    }
    s.dmabufs.push(DmaBuf {
        slot: slot as usize,
        fd,
        w: w as u32,
        h: h as u32,
        stride: stride as u32,
        fourcc,
        modifier,
        iosurface: 0,
    });
    1
}

/// macOS: the backend offers an IOSurface-backed ring slot.
unsafe extern "C" fn on_3d_iosurface(
    ud: *mut c_void,
    slot: c_int,
    iosurface: *mut c_void,
    w: c_int,
    h: c_int,
) -> c_int {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    if !s.zero_copy || slot < 0 || iosurface.is_null() {
        return 0;
    }
    s.dmabufs.push(DmaBuf {
        slot: slot as usize,
        fd: -1,
        w: w as u32,
        h: h as u32,
        stride: 0,
        fourcc: 0,
        modifier: 0,
        iosurface: iosurface as usize,
    });
    1
}

/// `PLAYER_PUBLISH_LOG=1`: one line per published frame naming the source
/// that made it: a ring slot, the readback path, the VGA surface or the
/// cursor's republish. Which of them a frame came from is invisible in the
/// picture, and a flicker is two of them taking turns.
fn publish_trace(tag: &std::fmt::Arguments) {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static N: AtomicUsize = AtomicUsize::new(0);
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if !*ON.get_or_init(|| std::env::var_os("PLAYER_PUBLISH_LOG").is_some()) {
        return;
    }
    let n = N.fetch_add(1, Ordering::Relaxed);
    if n < 5000 {
        eprintln!("[pub] {n} {tag}");
    }
}

/// A ring slot holds a complete frame: publish by reference.
unsafe extern "C" fn on_3d_frame_ready(ud: *mut c_void, slot: c_int) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    note_3d_frame(&mut s);
    publish_trace(&format_args!("slot {slot}"));
    s.front.ext_slot = Some(slot as usize);
    s.front.seq += 1;
    s.front.published = std::time::Instant::now();
    let waker = s.waker.clone();
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

/// After a real mode switch, a uniform single-colour VGA frame within this
/// window is the guest's transitional fill and is shown as black.
const SWITCH_GRACE: std::time::Duration = std::time::Duration::from_millis(1500);

/// `PLAYER_REFRESH_LOG=1`: a frame counter every hundred guest frames.
///
/// Off by default. On a machine left running it is a line or two a
/// second for as long as the guest is up, and it buries everything the
/// player prints when something is actually wrong. It still answers the
/// one question nothing else does, whether the guest is drawing at all,
/// so it stays behind a knob like `PLAYER_CURSOR_LOG` and
/// `PLAYER_LATENCY`. Read once, because this sits in the publish path.
fn refresh_log() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("PLAYER_REFRESH_LOG").is_some())
}

/// Every pixel the same colour (sampled every 61st plus the last one).
fn is_uniform(px: &[u32]) -> bool {
    match px.first() {
        Some(&v) => px[px.len() - 1] == v && px.iter().step_by(61).all(|&p| p == v),
        None => true,
    }
}

unsafe extern "C" fn on_refresh_done(ud: *mut c_void) {
    let shared = &*(ud as *const Mutex<Shared>);
    let mut s = shared.lock().unwrap();
    if s.width == 0 {
        return;
    }
    if s.three_d {
        // 3D active: recent 3D frames win; the VGA surface is shown only
        // once they stop and the guest drew on it (GL pass-through never
        // updates the surface, so nothing changes there)
        let recent = s.last_3d.map_or(false, |t| t.elapsed() < VGA_FALLBACK);
        if recent || !s.vga_dirty {
            return;
        }
        if !s.showing_vga {
            s.showing_vga = true;
            eprintln!("[display] no 3D frame for {} ms and the guest drew on the VGA surface: showing it", VGA_FALLBACK.as_millis());
        }
    }
    s.vga_dirty = false;
    let (w, h) = (s.width, s.height);
    // only what changed since the last publish is copied (M22), and a tick
    // with no change copies nothing (it published the whole frame on every
    // tick before, ~45 times a second on an idle desktop); all of it after
    // a resize or a 3D readback. The frame still counts: scripted keys and
    // pads step by published frames.
    let mut region = s.back_dirty.take();
    if s.front.width != w || s.front.height != h || s.front_stale {
        s.front.width = w;
        s.front.height = h;
        s.front.pixels = vec![0; w * h];
        s.front_stale = false;
        region = Some(Rect::whole(w, h));
        // what changed in a frame of another size is no region of this
        // one: kept, a bigger frame's rect outran this one's pixels when
        // the render thread had not taken it before the switch back
        // (800x600 -> 2560x1676 -> 800x600 in Windows setup)
        s.front.pending = None;
    }
    let Shared {
        back,
        front,
        vm,
        script,
        waker,
        switched_at,
        blanked,
        front_stale,
        ..
    } = &mut *s;
    // XP paints the whole screen white around a mode switch (seen on the
    // VGA surface of a bare qemu-system-i386 too): a uniform frame inside
    // the grace window is that fill, show black until real content arrives
    let transitional = match *switched_at {
        Some(t) if t.elapsed() <= SWITCH_GRACE && is_uniform(back) => true,
        Some(_) => {
            *switched_at = None;
            if *blanked > 0 {
                eprintln!("[display] {} transitional frame(s) after the switch shown black", blanked);
                *blanked = 0;
            }
            false
        }
        None => false,
    };
    if transitional {
        *blanked += 1;
        front.pixels.fill(0);
        region = Some(Rect::whole(w, h));
        *front_stale = true;
    } else if let Some(r) = region {
        for y in r.y0..r.y1 {
            let row = y * w;
            front.pixels[row + r.x0..row + r.x1].copy_from_slice(&back[row + r.x0..row + r.x1]);
        }
    }
    if let Some(r) = region {
        front.pending = Some(Rect::union(front.pending, r));
    }
    publish_trace(&format_args!("vga {}x{}{}", front.width, front.height,
                                if transitional { " (transitional)" } else { "" }));
    front.ext_slot = None;
    front.seq += 1;
    front.published = std::time::Instant::now();
    let waker = waker.clone();
    if front.seq % 100 == 0 && refresh_log() {
        eprintln!("[display] refresh #{}", front.seq);
    }
    if let (Some(vm), Some(sc)) = (vm.as_ref(), script.as_mut()) {
        while sc.next < sc.steps.len() && sc.steps[sc.next].0 <= front.seq {
            let (_, chord, down) = &sc.steps[sc.next];
            let qcodes: Vec<u32> = chord
                .iter()
                .map(|&c| qemu_embed::atset1_to_qcode(c))
                .collect();
            eprintln!(
                "[script] frame {} chord {:x?} -> qcodes {:?} {}",
                front.seq,
                chord,
                qcodes,
                if *down { "down" } else { "up" }
            );
            if *down {
                for &q in &qcodes {
                    vm.key(q, true);
                }
            } else {
                for &q in qcodes.iter().rev() {
                    vm.key(q, false);
                }
            }
            vm.input_flush();
            sc.next += 1;
        }
    }
    crate::maybe_dump(&front.pixels, front.width, front.height, front.seq);
    drop(s);
    if let Some(w) = waker {
        w();
    }
}

/// v12: the device pushed a whole frame outside the tick (virtio-gpu's
/// RESOURCE_FLUSH, the Voodoo 2's end of frame): publish it now instead of
/// at the next tick, which then finds nothing dirty. The tick alone held a
/// Windows 11 desktop to ~44 frames a second whatever the guest drew (QEMU
/// re-arms it after the refresh's own copy) and showed frames out of step
/// with the guest's presents. `PLAYER_FLUSH=0` leaves it to the tick, the
/// A/B.
unsafe extern "C" fn on_flush(ud: *mut c_void) {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if *ON.get_or_init(|| std::env::var("PLAYER_FLUSH").as_deref() != Ok("0")) {
        on_refresh_done(ud);
    }
}

fn copy_rect(s: &mut Shared, x: usize, y: usize, w: usize, h: usize) {
    if s.surface.is_null() || w == 0 || h == 0 {
        return;
    }
    let (sw, sh) = (s.width, s.height);
    let x1 = (x + w).min(sw);
    let y1 = (y + h).min(sh);
    if x >= x1 || y >= sh.min(y1) {
        return;
    }
    s.back_dirty = Some(Rect::union(s.back_dirty, Rect { x0: x, y0: y.min(sh), x1, y1 }));
    for row in y.min(sh)..y1 {
        // SAFETY: surface valid until the next on_switch (header contract);
        // rows are `stride` bytes apart and the rect is pre-clamped by QEMU.
        let src = unsafe { s.surface.add(row * s.stride + x * 4) as *const u32 };
        let dst = &mut s.back[row * sw + x..row * sw + x1];
        unsafe { std::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), x1 - x) };
    }
}

/// Start QEMU on its own thread. Returns once the VM is created and started.
/// `audio`: (ring, sample rate). The ring is installed before qemu_init and
/// an `-audiodev embed,id=embed0` matching the host rate is appended; attach
/// devices with `audiodev=embed0`.
pub fn start(
    mut args: Vec<String>,
    audio: Option<(Arc<crate::audio::Ring>, u32)>,
    waker: Option<Arc<dyn Fn() + Send + Sync>>,
    zero_copy: bool,
) -> (Qemu, Display, JoinHandle<i32>, Option<Arc<crate::qmp::Qmp>>) {
    // QMP monitor on a socketpair: our end stays here, QEMU gets the fd.
    let qmp = match crate::qmp::Qmp::pair() {
        Ok((q, fd)) => {
            args.extend(crate::qmp::Qmp::qemu_args(fd));
            Some(q)
        }
        Err(e) => {
            eprintln!("[qmp] socketpair failed: {e}; no monitor");
            None
        }
    };
    crate::boot_prompt::attach(&mut args);
    let ring_ptrs = audio.map(|(ring, rate)| {
        args.push("-audiodev".into());
        // the cushion QEMU keeps in the ring under the host device's own
        // pull; the consumer waits for the same amount before it plays. A
        // 5 ms mixer tick: a main loop held up by a 3D swap is paid back a
        // tick at a time, and finer ticks fit between the stalls
        // (embed/embedaudio.c). f32, because QEMU's s16 output saturates
        // a mix of voices that sums past full scale and its float output
        // does not: the player's limiter turns it down instead
        // (player-core/src/audio.rs)
        let cushion = crate::audio::cushion_ms() * 1000;
        args.push(format!(
            "embed,id=embed0,timer-period=5000,out.frequency={rate},out.channels=2,\
             out.format=f32,out.buffer-length={cushion}"
        ));
        // ring lives for the process; leak a strong ref for the C side
        let r = Arc::into_raw(ring);
        (r as usize, rate)
    });
    let shared = Arc::new(Mutex::new(Shared {
        vm: None,
        script: key_script_from_env(),
        surface: std::ptr::null(),
        stride: 0,
        width: 0,
        height: 0,
        back: Vec::new(),
        back_dirty: None,
        front_stale: false,
        switched_at: None,
        blanked: 0,
        front: Frame {
            ext_slot: None,
            width: 0,
            height: 0,
            pixels: Vec::new(),
            pending: None,
            seq: 0,
            published: std::time::Instant::now(),
        },
        waker,
        three_d: false,
        last_3d: None,
        vga_dirty: false,
        showing_vga: false,
        zero_copy,
        dmabufs: Vec::new(),
        stopped: false,
        released: false,
        cursor: None,
        cursor_seq: 0,
        cursor_visible: None,
        cursor_x: 0,
        cursor_y: 0,
    }));
    // Leak one strong ref for the C side; the process holds one VM for life.
    let ud = Arc::into_raw(shared.clone()) as usize;
    let shared_q = shared.clone();
    let (tx, rx) = mpsc::channel();
    let handle = std::thread::Builder::new()
        .name("qemu".into())
        .spawn(move || {
            let cb = RawDisplayCb {
                on_switch: Some(on_switch),
                on_update: Some(on_update),
                on_refresh_done: Some(on_refresh_done),
                on_cursor: Some(on_cursor),
                on_mouse_set: Some(on_mouse_set),
                on_3d_active: Some(on_3d_active),
                on_3d_frame: Some(on_3d_frame),
                on_3d_dmabuf: Some(on_3d_dmabuf),
                on_3d_frame_ready: Some(on_3d_frame_ready),
                on_3d_iosurface: Some(on_3d_iosurface),
                on_flush: Some(on_flush),
            };
            if let Some((r, _)) = ring_ptrs {
                let ring = unsafe { &*(r as *const crate::audio::Ring) };
                unsafe {
                    qemu_embed::set_audio_ring(
                        ring.buf.as_ptr() as *mut u8,
                        ring.buf.len(),
                        ring.wr.as_ptr(),
                        ring.rd.as_ptr(),
                    )
                };
            }
            let (q, owner) =
                unsafe { Qemu::new(&args, &cb, ud as *mut c_void) }.expect("qemu_embed_new failed");
            if crate::clipboard::wanted(&args) {
                crate::clipboard::start(q); // M23: the guest agent's clipboard
            }
            q.vm_start();
            tx.send(q).unwrap();
            let status = owner.run();
            // The UI thread holds a copy of the handle (input path). Tell it
            // the loop is over and wait until it has stopped calling in;
            // destroy frees the input mutex and the object itself.
            let waker = {
                let mut s = shared_q.lock().unwrap();
                s.stopped = true;
                s.vm = None;
                s.waker.clone()
            };
            if let Some(w) = waker {
                w();
            }
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
            while !shared_q.lock().unwrap().released {
                if std::time::Instant::now() > deadline {
                    eprintln!("[qemu] UI thread did not release the VM handle; cleaning up anyway");
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            crate::clipboard::stop();
            crate::share::stop();
            owner.destroy(status);
            status
        })
        .expect("spawn qemu thread");
    let q = rx.recv().expect("qemu thread died during init");
    // pull guest frames at ~60 Hz instead of QEMU's 30 ms default (doc 03)
    let refresh_ms: u32 = std::env::var("PLAYER_REFRESH_MS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(16);
    q.set_refresh_ms(refresh_ms);
    shared.lock().unwrap().vm = Some(q);
    if let Some(qmp) = &qmp {
        // capabilities are negotiated by the reader thread on the greeting
        if !qmp.wait_ready(std::time::Duration::from_secs(5)) {
            eprintln!("[qmp] no greeting from the monitor");
        }
        match qmp.execute("query-version", serde_json::Value::Null) {
            Ok(v) => {
                let q = &v["qemu"];
                eprintln!("[qmp] connected: QEMU {}.{}.{}", q["major"], q["minor"], q["micro"]);
            }
            Err(e) => eprintln!("[qmp] query-version: {e}"),
        }
    }
    (q, Display(shared), handle, qmp)
}
