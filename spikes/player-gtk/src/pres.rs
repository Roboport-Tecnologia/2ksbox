//! wp_presentation feedback on a surface someone else made (winit's, or
//! the spike's own subsurface), on a queue of our own over the toolkit's
//! connection (the way the player's `kbcapture.rs` inhibits shortcuts).

use std::ffi::c_void;
use std::time::{Duration, Instant};
use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{wl_registry, wl_surface};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols::wp::presentation_time::client::{
    wp_presentation::{self, WpPresentation},
    wp_presentation_feedback::{self, WpPresentationFeedback},
};

#[derive(Default)]
pub struct State {
    clock: Option<u32>,
    done: Vec<(Instant, Option<Instant>)>,
}

pub struct Feedback {
    conn: Connection,
    queue: EventQueue<State>,
    state: State,
    presentation: WpPresentation,
    surface: wl_surface::WlSurface,
}

impl Feedback {
    /// # Safety
    /// `display` and `surface` are winit's live `wl_display` and `wl_surface`.
    pub unsafe fn new(display: *mut c_void, surface: *mut c_void) -> Result<Feedback, String> {
        let conn = unsafe { Connection::from_backend(Backend::from_foreign_display(display.cast())) };
        let (globals, mut queue) = registry_queue_init::<State>(&conn).map_err(|e| e.to_string())?;
        let qh = queue.handle();
        let presentation: WpPresentation =
            globals.bind(&qh, 1..=1, ()).map_err(|_| "no wp_presentation".to_string())?;
        let id = unsafe { ObjectId::from_ptr(wl_surface::WlSurface::interface(), surface.cast()) }
            .map_err(|e| e.to_string())?;
        let surface = wl_surface::WlSurface::from_id(&conn, id).map_err(|e| e.to_string())?;
        let mut state = State::default();
        queue.roundtrip(&mut state).map_err(|e| e.to_string())?;
        Ok(Feedback { conn, queue, state, presentation, surface })
    }

    /// Ask when the surface's next commit (the present) reaches the screen.
    pub fn request(&mut self, published: Instant) {
        self.presentation.feedback(&self.surface, &self.queue.handle(), published);
        let _ = self.conn.flush();
    }

    /// Frames answered since the last call: when each was published and
    /// when it was shown (`None`: discarded).
    pub fn take(&mut self) -> Vec<(Instant, Option<Instant>)> {
        let _ = self.queue.dispatch_pending(&mut self.state);
        std::mem::take(&mut self.state.done)
    }
}

/// A CLOCK_MONOTONIC time as an Instant (Instant is CLOCK_MONOTONIC on Linux).
fn to_instant(sec: u64, nsec: u32) -> Instant {
    let mut now = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut now) };
    let at = Duration::new(sec, nsec);
    let then = Duration::new(now.tv_sec as u64, now.tv_nsec as u32);
    let i = Instant::now();
    match then.checked_sub(at) {
        Some(ago) => i - ago,
        None => i + (at - then),
    }
}

impl Dispatch<WpPresentation, ()> for State {
    fn event(s: &mut Self, _: &WpPresentation, e: wp_presentation::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        if let wp_presentation::Event::ClockId { clk_id } = e {
            s.clock = Some(clk_id);
            if clk_id != libc::CLOCK_MONOTONIC as u32 {
                eprintln!("[baseline] presentation clock {clk_id} is not CLOCK_MONOTONIC: times are off");
            }
        }
    }
}

impl Dispatch<WpPresentationFeedback, Instant> for State {
    fn event(
        s: &mut Self,
        _: &WpPresentationFeedback,
        e: wp_presentation_feedback::Event,
        published: &Instant,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match e {
            wp_presentation_feedback::Event::Presented { tv_sec_hi, tv_sec_lo, tv_nsec, .. } => {
                let sec = ((tv_sec_hi as u64) << 32) | tv_sec_lo as u64;
                s.done.push((*published, Some(to_instant(sec, tv_nsec))));
            }
            wp_presentation_feedback::Event::Discarded => s.done.push((*published, None)),
            _ => {}
        }
    }
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
