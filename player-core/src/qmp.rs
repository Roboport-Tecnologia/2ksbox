//! QMP over a socketpair (doc 11 §QMP). The player keeps one end; the other
//! is handed to QEMU as `-chardev socket,id=qmp0,fd=N -mon
//! chardev=qmp0,mode=control`. No filesystem path: the monitor exists only
//! inside this process. Full QMP including events; the monitor runs on
//! QEMU's own iothread, so commands are safe from any thread.
//!
//! Windows has no `socketpair()`, so the pair is made on the loopback
//! interface and the connection is proved to be our own before it is used
//! (see `pair`). The handle is then turned into a descriptor by the embed
//! library itself (`qemu_embed::socket_to_fd`): `fd=` is a plain integer on
//! both platforms, but on Windows it is a C-runtime descriptor QEMU resolves
//! with `_get_osfhandle`, not a `SOCKET`. QEMU refuses a raw handle with
//! "File descriptor 'N' is not a socket".
//!
//! `execute` is synchronous (id-matched, 10 s timeout). Events accumulate in
//! a queue the UI thread drains with `take_events`.

use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;

#[cfg(unix)]
use std::os::fd::IntoRawFd;
#[cfg(unix)]
use std::os::unix::net::UnixStream as Stream;
#[cfg(windows)]
use std::net::TcpStream as Stream;
#[cfg(windows)]
use std::os::windows::io::IntoRawSocket;

pub struct Qmp {
    writer: Mutex<Stream>,
    pending: Mutex<HashMap<u64, mpsc::Sender<Value>>>,
    events: Mutex<VecDeque<Value>>,
    next_id: AtomicU64,
    /// set once the greeting arrived and capabilities were negotiated
    ready: Mutex<bool>,
}

impl Qmp {
    /// Create the pair. Returns the client and the raw fd to pass to QEMU
    /// (kept open for the process lifetime; QEMU owns it after init).
    pub fn pair() -> std::io::Result<(Arc<Qmp>, i32)> {
        let (ours, theirs) = socket_pair()?;
        let reader = ours.try_clone()?;
        let qmp = Arc::new(Qmp {
            writer: Mutex::new(ours),
            pending: Mutex::new(HashMap::new()),
            events: Mutex::new(VecDeque::new()),
            next_id: AtomicU64::new(1),
            ready: Mutex::new(false),
        });
        let q = qmp.clone();
        std::thread::Builder::new()
            .name("qmp".into())
            .spawn(move || q.reader_loop(reader))
            .expect("spawn qmp reader");
        Ok((qmp, into_raw(theirs)?))
    }

    /// QEMU command-line fragment that attaches the monitor to `fd`.
    pub fn qemu_args(fd: i32) -> Vec<String> {
        vec![
            "-chardev".into(),
            format!("socket,id=qmp0,fd={fd}"),
            "-mon".into(),
            "chardev=qmp0,mode=control".into(),
        ]
    }

    fn reader_loop(&self, stream: Stream) {
        let mut rd = BufReader::new(stream);
        let mut line = String::new();
        loop {
            line.clear();
            match rd.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
            let v: Value = match serde_json::from_str(line.trim()) {
                Ok(v) => v,
                Err(e) => {
                    eprintln!("[qmp] bad json: {e}: {}", line.trim());
                    continue;
                }
            };
            if v.get("QMP").is_some() {
                // greeting: negotiate capabilities without blocking a caller
                let _ = self.send(json!({"execute": "qmp_capabilities", "id": 0}));
                continue;
            }
            if v.get("event").is_some() {
                let mut ev = self.events.lock().unwrap();
                if ev.len() >= 256 {
                    ev.pop_front();
                }
                ev.push_back(v);
                continue;
            }
            let id = v.get("id").and_then(Value::as_u64);
            if id == Some(0) {
                *self.ready.lock().unwrap() = v.get("return").is_some();
                continue;
            }
            if let Some(tx) = id.and_then(|id| self.pending.lock().unwrap().remove(&id)) {
                let _ = tx.send(v);
            } else {
                eprintln!("[qmp] unmatched reply: {v}");
            }
        }
        // monitor gone: fail every waiter
        self.pending.lock().unwrap().clear();
    }

    fn send(&self, v: Value) -> std::io::Result<()> {
        let mut w = self.writer.lock().unwrap();
        w.write_all(v.to_string().as_bytes())?;
        w.write_all(b"\n")
    }

    /// Run one command. `args` may be `Value::Null` for none. Returns the
    /// `return` payload, or the QMP error description.
    pub fn execute(&self, cmd: &str, args: Value) -> Result<Value, String> {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel();
        self.pending.lock().unwrap().insert(id, tx);
        let mut msg = json!({"execute": cmd, "id": id});
        if !args.is_null() {
            msg["arguments"] = args;
        }
        if let Err(e) = self.send(msg) {
            self.pending.lock().unwrap().remove(&id);
            return Err(format!("write: {e}"));
        }
        let reply = rx
            .recv_timeout(Duration::from_secs(10))
            .map_err(|_| {
                self.pending.lock().unwrap().remove(&id);
                "timeout".to_string()
            })?;
        if let Some(r) = reply.get("return") {
            Ok(r.clone())
        } else if let Some(e) = reply.get("error") {
            Err(format!(
                "{}: {}",
                e.get("class").and_then(Value::as_str).unwrap_or("Error"),
                e.get("desc").and_then(Value::as_str).unwrap_or("?")
            ))
        } else {
            Err(format!("malformed reply: {reply}"))
        }
    }

    /// Run a raw QMP request object (`{"execute": ..., "arguments": ...}`).
    pub fn execute_raw(&self, req: &Value) -> Result<Value, String> {
        let cmd = req
            .get("execute")
            .and_then(Value::as_str)
            .ok_or_else(|| "missing \"execute\"".to_string())?;
        self.execute(cmd, req.get("arguments").cloned().unwrap_or(Value::Null))
    }

    /// Wait until the greeting has been answered with qmp_capabilities
    /// (commands sent before that are rejected by QEMU).
    pub fn wait_ready(&self, timeout: Duration) -> bool {
        let deadline = std::time::Instant::now() + timeout;
        while !*self.ready.lock().unwrap() {
            if std::time::Instant::now() > deadline {
                return false;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        true
    }

    /// Drain queued events (oldest first).
    pub fn take_events(&self) -> Vec<Value> {
        self.events.lock().unwrap().drain(..).collect()
    }
}

#[cfg(unix)]
fn socket_pair() -> std::io::Result<(Stream, Stream)> {
    Stream::pair()
}

#[cfg(unix)]
fn into_raw(s: Stream) -> std::io::Result<i32> {
    Ok(s.into_raw_fd())
}

/// A connected pair on the loopback interface, Windows' stand-in for
/// `socketpair()`. Anything else on the machine can also connect to a
/// listening port, so the accepted connection is only kept when its peer
/// address is the exact address our own connecting socket was given,
/// which no other process can be using at the same time. A stranger that
/// wins the race is dropped and the whole thing is tried again, so the
/// monitor is never handed to it.
#[cfg(windows)]
fn socket_pair() -> std::io::Result<(Stream, Stream)> {
    use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener};
    let listener = TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0))?;
    let addr = listener.local_addr()?;
    for _ in 0..16 {
        let ours = Stream::connect(addr)?;
        let mine = ours.local_addr()?;
        let (theirs, peer) = listener.accept()?;
        if peer == mine && matches!(mine, SocketAddr::V4(a) if *a.ip() == Ipv4Addr::LOCALHOST) {
            ours.set_nodelay(true)?;
            theirs.set_nodelay(true)?;
            return Ok((ours, theirs));
        }
        // Someone else got in first: drop both and try a fresh pair.
        drop(theirs);
        drop(ours);
    }
    Err(std::io::Error::other(
        "could not establish a private loopback pair for the QEMU monitor",
    ))
}

/// The socket handle as QEMU's `fd=` wants it: a descriptor in the C runtime
/// *the embed library* links, which is not the same table this crate's
/// `_open_osfhandle` would write to if the two ever linked different CRTs.
/// So the handle crosses the boundary as a handle and the library converts.
#[cfg(windows)]
fn into_raw(s: Stream) -> std::io::Result<i32> {
    let raw = s.into_raw_socket(); // QEMU owns it now; do not close it here
    qemu_embed::socket_to_fd(raw).ok_or_else(|| {
        std::io::Error::other(format!("socket handle {raw} could not be made a file descriptor"))
    })
}

/// Events worth a log line even without PLAYER_QMP=1.
pub fn is_notable(event: &str) -> bool {
    matches!(
        event,
        "SHUTDOWN" | "RESET" | "POWERDOWN" | "STOP" | "RESUME" | "GUEST_PANICKED"
            | "BLOCK_IO_ERROR" | "DEVICE_TRAY_MOVED" | "WATCHDOG"
    )
}
