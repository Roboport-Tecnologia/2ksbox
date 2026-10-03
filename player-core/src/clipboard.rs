//! The host's clipboard and the guest's, text only (track M23, doc 24 §3).
//!
//! QEMU's `qemu-vdagent` chardev carries the guest's side; the embed
//! library joins QEMU's clipboard for us (embed API v11). Here a thread
//! polls the host's clipboard twice a second and offers new text to the
//! guest, and the guest's text is written to the host's clipboard as it
//! arrives. Each side remembers what it last took from the other, so a
//! text never bounces back. Only for a machine whose command line has a
//! `qemu-vdagent`; any front end gets it, with no toolkit involved.

use qemu_embed::Qemu;
use std::sync::mpsc::{channel, Sender};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

/// The VM handle while it may be called; cleared by [`stop`] before the
/// VM is destroyed.
static VM: Mutex<Option<Qemu>> = Mutex::new(None);
/// Text the guest set, on its way to the host's clipboard.
static FROM_GUEST: OnceLock<Mutex<Sender<String>>> = OnceLock::new();

/// Whether a command line carries the guest agent's chardev.
pub fn wanted(qemu_args: &[String]) -> bool {
    qemu_args.iter().any(|a| a.contains("qemu-vdagent"))
}

/// After `Qemu::new`, on the QEMU thread.
pub fn start(q: Qemu) {
    *VM.lock().unwrap() = Some(q);
    let (tx, rx) = channel::<String>();
    let _ = FROM_GUEST.set(Mutex::new(tx));
    q.set_clipboard_handler(Box::new(|text| {
        // QEMU's thread with the BQL held: hand off, never block
        if let Some(tx) = FROM_GUEST.get() {
            let _ = tx.lock().unwrap().send(text);
        }
    }));
    std::thread::Builder::new()
        .name("clipboard".into())
        .spawn(move || {
            let mut board = match arboard::Clipboard::new() {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("[clipboard] no host clipboard: {e}");
                    return;
                }
            };
            // what the host's clipboard held when we last looked or wrote
            let mut seen = board.get_text().ok();
            let mut first = true;
            loop {
                match rx.recv_timeout(Duration::from_millis(500)) {
                    Ok(text) => {
                        eprintln!("[clipboard] guest -> host, {} bytes", text.len());
                        if seen.as_deref() != Some(text.as_str()) {
                            if let Err(e) = board.set_text(text.clone()) {
                                eprintln!("[clipboard] host clipboard: {e}");
                            }
                            seen = Some(text);
                        }
                    }
                    Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
                    Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
                }
                let now = board.get_text().ok();
                // the text already there at start goes to the guest too,
                // once its agent is up to take it (vdagent offers it then)
                if (now != seen || first) && now.as_deref().is_some_and(|t| !t.is_empty()) {
                    let vm = VM.lock().unwrap();
                    let Some(q) = vm.as_ref() else { return };
                    q.set_clipboard_text(now.as_deref().unwrap());
                    eprintln!("[clipboard] host -> guest, {} bytes", now.as_deref().unwrap().len());
                    first = false;
                }
                seen = now;
                if VM.lock().unwrap().is_none() {
                    return;
                }
            }
        })
        .expect("spawn clipboard thread");
}

/// Before the VM is destroyed: no call reaches it after this returns.
pub fn stop() {
    *VM.lock().unwrap() = None;
}
