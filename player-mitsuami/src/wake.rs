//! The QEMU thread's wake, as a future the UI thread awaits. QEMU calls
//! the player's waker for every published frame and every cursor change,
//! from its own thread; mitsuami's executor turns the UI thread's run loop
//! when a task's waker fires from any thread, so the picture is drawn on
//! the UI thread right after the publish, as the winit player draws on its
//! event loop's wake. Wakes coalesce while one is pending.

use std::future::poll_fn;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Poll, Waker};

#[derive(Default)]
pub struct Wake {
    pending: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

impl Wake {
    /// The function QEMU's thread calls (`Session::start`'s waker).
    pub fn notifier(self: &Arc<Self>) -> Arc<dyn Fn() + Send + Sync> {
        let wake = self.clone();
        Arc::new(move || {
            wake.pending.store(true, Ordering::Release);
            if let Some(w) = wake.waker.lock().unwrap().take() {
                w.wake();
            }
        })
    }

    /// Resolves once a wake has come since the last one was taken.
    pub async fn next(&self) {
        poll_fn(|cx| {
            if self.pending.swap(false, Ordering::AcqRel) {
                return Poll::Ready(());
            }
            *self.waker.lock().unwrap() = Some(cx.waker().clone());
            // a wake between the swap and the store would be lost
            if self.pending.swap(false, Ordering::AcqRel) {
                return Poll::Ready(());
            }
            Poll::Pending
        })
        .await
    }
}
