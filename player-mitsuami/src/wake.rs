//! The QEMU thread's wake, as a future the UI thread awaits. QEMU calls
//! the player's waker for every published frame and every cursor change,
//! from its own thread; mitsuami's executor turns the UI thread's run loop
//! when a task's waker fires from any thread, so the picture is drawn on
//! the UI thread right after the publish, as the winit player draws on its
//! event loop's wake. Wakes coalesce while one is pending.
//!
//! Every wait goes through the executor, even when a wake came while the
//! UI thread was busy: a draw waits about 16 ms for its drawable on macOS,
//! QEMU publishes meanwhile, and a wait that returned at once drew again
//! straight away, so the platform's input (a locked mouse) got in only
//! every 300 ms. Woken from QEMU's thread, the task runs on the run loop's
//! next turn, after that input (mitsuami's executor).

use std::future::poll_fn;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Poll, Waker};

#[derive(Default)]
pub struct Wake {
    pending: AtomicBool,
    /// The waiting task's waker, kept between waits so a wake that comes
    /// while the task runs still goes through the executor.
    waker: Mutex<Option<Waker>>,
}

impl Wake {
    /// The function QEMU's thread calls (`Session::start`'s waker).
    pub fn notifier(self: &Arc<Self>) -> Arc<dyn Fn() + Send + Sync> {
        let wake = self.clone();
        Arc::new(move || {
            if wake.pending.swap(true, Ordering::AcqRel) {
                return;
            }
            if let Some(w) = wake.waker.lock().unwrap().clone() {
                w.wake();
            }
        })
    }

    /// Resolves once a wake has come since the last one was taken, never
    /// on its first poll.
    pub async fn next(&self) {
        let mut first = true;
        poll_fn(|cx| {
            if !first && self.pending.swap(false, Ordering::AcqRel) {
                return Poll::Ready(());
            }
            let had = self.waker.lock().unwrap().replace(cx.waker().clone()).is_some();
            if first {
                first = false;
                // a wake before any waker was kept woke nothing
                if !had && self.pending.load(Ordering::Acquire) {
                    cx.waker().wake_by_ref();
                }
            }
            Poll::Pending
        })
        .await
    }
}
