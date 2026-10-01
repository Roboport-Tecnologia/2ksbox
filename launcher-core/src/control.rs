//! Live control of a running machine: media swap and snapshots on a
//! guest that's already up (doc 07's "How the launcher reaches a running
//! machine" and "Snapshots").
//!
//! **No new protocol and no player change.** The launcher adds
//! `-qmp unix:<path>,server,nowait` to the arguments it spawns the
//! player with and talks QMP to that socket itself, as `tools/qmpc.py`
//! does to drive a guest. QEMU allows several monitors, so the player's
//! own in-process one (`player/src/qmp.rs`, on a socketpair with no
//! filesystem path) is untouched and neither binary needs IPC of its
//! own. A hand-written bundle run straight through `player` has no
//! launcher socket, which is the documented "the launcher is optional"
//! path (doc 07).
//!
//! The monitor is a Unix-domain socket on **every** host, Windows
//! included. QEMU's Windows build binds `unix:` addresses (Windows 10
//! has had AF_UNIX since 1803, and QEMU 9.2 requires newer), so the
//! argument is the same string everywhere and only this end differs:
//! std has no `UnixStream` on Windows, so `imp` there makes the socket
//! through Winsock. Not a loopback port, because any local process can
//! reach one and a QMP monitor is complete control of the machine; a
//! socket file in the user's own runtime or temp directory is guarded by
//! that directory's permissions on both kinds of host. Not a named pipe
//! either: QEMU's `pipe` chardev on Windows waits for its one client
//! inside machine start-up, so the player would not come up until the
//! launcher connected, and it takes no second connection.

use serde_json::Value;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Where a machine's monitor socket lives: the platform runtime dir
/// (`/run/user/<uid>/2ksbox` on Linux) or the temp dir, plus a
/// name derived from the bundle directory. Unix socket paths are capped
/// around 108 bytes, so the name is the bundle's own directory name cut
/// short plus a hash of its full path. That stays short and readable and
/// is unique across two libraries holding a same-named bundle.
pub fn socket_path(bundle_dir: &Path) -> PathBuf {
    let dir = crate::paths::runtime_dir();
    // FNV-1a over the full path: not security, just collision avoidance.
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in bundle_dir.as_os_str().as_encoded_bytes() {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(0x1000_0000_01b3);
    }
    let stem: String =
        bundle_dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default().chars().take(24).collect();
    dir.join(format!("{stem}-{hash:016x}.qmp"))
}

/// The flat shelf file this machine's ATAPI drive reads
/// (`cdshelf/cdshelf_proto.h`), beside its monitor socket: both are
/// per-run, per-machine host state that belongs in the runtime dir
/// rather than in the bundle.
pub fn shelf_path(bundle_dir: &Path) -> PathBuf {
    socket_path(bundle_dir).with_extension("shelf")
}

/// The `-qmp` argument that makes QEMU listen on `path`, or `None` when
/// the path is too long for a socket address. Also removes a stale socket
/// file left by a player that was killed rather than shut down.
///
/// A QMP monitor is complete control of the machine, so the directory
/// holding these sockets is owner-only. The platform runtime dir already
/// is (`/run/user/<uid>` is 0700, macOS's per-user `$TMPDIR` likewise),
/// but our own subdirectory under it is created here, so it sets 0700
/// itself rather than inheriting the umask.
pub fn qmp_args(path: &Path) -> Option<Vec<String>> {
    // `sun_path` is 108 bytes with its NUL on Linux and Windows (104 on
    // macOS), and QEMU refuses a longer path, which would stop the
    // machine from starting. A long temp directory (Windows puts the
    // user name in it) costs live control, never the machine.
    let max = if cfg!(target_os = "macos") { 103 } else { 107 };
    if path.as_os_str().len() > max {
        eprintln!("live control off: the monitor socket path is too long for a Unix socket: {}", path.display());
        return None;
    }
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(parent, std::fs::Permissions::from_mode(0o700));
        }
    }
    // A player that was killed rather than shut down leaves the socket
    // file behind, and QEMU refuses to bind over one.
    let _ = std::fs::remove_file(path);
    // QEMU refuses to start a machine whose monitor it cannot bind, and a
    // Windows PC can fail that where a Unix host never does (a build
    // before AF_UNIX, a temp directory on a filesystem that cannot hold a
    // socket file, or Wine, which has no AF_UNIX). Bound here first, so
    // such a host loses live control and keeps the machine.
    #[cfg(windows)]
    if let Err(e) = imp::bind_probe(path) {
        eprintln!("live control off: this host cannot bind a Unix socket at {}: {e}", path.display());
        return None;
    }
    Some(vec!["-qmp".into(), format!("unix:{},server,nowait", path.display())])
}

/// Whether something is listening on `path`, meaning a player is up,
/// whoever started it. It connects and reads no greeting.
pub fn listening(path: &Path) -> bool {
    imp::connect(path).is_ok()
}

#[cfg(unix)]
mod imp {
    use std::os::unix::net::UnixStream;
    use std::path::Path;

    pub type Stream = UnixStream;

    pub fn connect(path: &Path) -> std::io::Result<Stream> {
        UnixStream::connect(path)
    }
}

/// Winsock's AF_UNIX. The connected socket is handed to std as a
/// `TcpStream`, whose reads, writes and timeouts are plain `recv` /
/// `send` / `SO_RCVTIMEO` on the handle and so do not care about the
/// address family; nothing here asks it for a peer address, and it is
/// never cloned (`WSADuplicateSocket` is the one call AF_UNIX does not
/// take), which is why `Control` reads and writes through one stream.
#[cfg(windows)]
mod imp {
    use std::net::TcpStream;
    use std::os::windows::io::FromRawSocket;
    use std::path::Path;
    use windows_sys::Win32::Networking::WinSock::{
        bind, closesocket, connect as ws_connect, socket, WSAGetLastError, WSAStartup, AF_UNIX, INVALID_SOCKET,
        SOCKADDR, SOCKADDR_UN, SOCKET, SOCK_STREAM, WSADATA,
    };

    pub type Stream = TcpStream;

    /// A stream socket in the Unix family and the address of `path`.
    /// WSAStartup is reference-counted, and std calls it lazily on its
    /// first std::net use, which may not have happened yet, so this call
    /// is ours.
    fn unix_socket(path: &Path) -> std::io::Result<(SOCKET, SOCKADDR_UN)> {
        let bytes = path.as_os_str().as_encoded_bytes();
        let mut addr = SOCKADDR_UN { sun_family: AF_UNIX, sun_path: [0; 108] };
        if bytes.len() >= addr.sun_path.len() {
            return Err(std::io::Error::other("socket path too long"));
        }
        for (d, s) in addr.sun_path.iter_mut().zip(bytes) {
            *d = *s as i8;
        }
        // SAFETY: plain Winsock calls with a zeroed out-parameter.
        unsafe {
            let mut data: WSADATA = std::mem::zeroed();
            let rc = WSAStartup(0x0202, &mut data);
            if rc != 0 {
                return Err(std::io::Error::from_raw_os_error(rc));
            }
            let s = socket(AF_UNIX as i32, SOCK_STREAM, 0);
            if s == INVALID_SOCKET {
                return Err(std::io::Error::from_raw_os_error(WSAGetLastError()));
            }
            Ok((s, addr))
        }
    }

    const ADDR_LEN: i32 = std::mem::size_of::<SOCKADDR_UN>() as i32;

    pub fn connect(path: &Path) -> std::io::Result<Stream> {
        let (s, addr) = unix_socket(path)?;
        // SAFETY: `s` is a socket we own; the address outlives the call.
        unsafe {
            if ws_connect(s, &addr as *const SOCKADDR_UN as *const SOCKADDR, ADDR_LEN) != 0 {
                let err = std::io::Error::from_raw_os_error(WSAGetLastError());
                closesocket(s);
                return Err(err);
            }
            Ok(TcpStream::from_raw_socket(s as _))
        }
    }

    /// Bind `path` and let it go again, leaving no socket file behind.
    pub fn bind_probe(path: &Path) -> std::io::Result<()> {
        let (s, addr) = unix_socket(path)?;
        // SAFETY: as in `connect`.
        let result = unsafe {
            let rc = bind(s, &addr as *const SOCKADDR_UN as *const SOCKADDR, ADDR_LEN);
            let err = (rc != 0).then(|| std::io::Error::from_raw_os_error(WSAGetLastError()));
            closesocket(s);
            err.map_or(Ok(()), Err)
        };
        let _ = std::fs::remove_file(path);
        result
    }
}

/// A connected QMP session. Synchronous and single-threaded: the launcher
/// is the only client of this socket and issues one command at a time, so
/// replies are read inline (events in between are dropped, since nothing
/// here subscribes to any). One stream for both directions, written
/// through the reader: a `BufReader` only buffers what it has read.
pub struct Control {
    stream: BufReader<imp::Stream>,
    next_id: u64,
}

impl Control {
    /// Connect, answer the greeting with `qmp_capabilities` (QEMU rejects
    /// every other command before that) and return the session. A refused
    /// connection means the machine isn't running (or was started outside
    /// the launcher).
    pub fn connect(path: &Path) -> Result<Control, String> {
        let stream = imp::connect(path).map_err(|e| format!("{}: {e}", path.display()))?;
        // Bounded so a wedged QEMU can't hang the UI thread forever;
        // generous because a snapshot command runs on QEMU's main loop,
        // which a busy guest can hold briefly.
        let timeout = Duration::from_secs(10);
        stream.set_read_timeout(Some(timeout)).map_err(|e| e.to_string())?;
        stream.set_write_timeout(Some(timeout)).map_err(|e| e.to_string())?;
        let mut control = Control { stream: BufReader::new(stream), next_id: 1 };
        control.read_until(|v| v.get("QMP").is_some())?; // the greeting
        control.execute("qmp_capabilities", Value::Null)?;
        Ok(control)
    }

    /// Read lines until `want` accepts one. Events and replies to commands
    /// we've stopped waiting for are skipped.
    fn read_until(&mut self, want: impl Fn(&Value) -> bool) -> Result<Value, String> {
        let mut line = String::new();
        loop {
            line.clear();
            match self.stream.read_line(&mut line) {
                Ok(0) => return Err("monitor closed the connection".into()),
                Ok(_) => {}
                Err(e) => return Err(format!("reading the monitor: {e}")),
            }
            let Ok(v) = serde_json::from_str::<Value>(line.trim()) else {
                continue;
            };
            if want(&v) {
                return Ok(v);
            }
        }
    }

    /// Run one command and return its `return` payload, or QEMU's own error
    /// description, which is what a window should show ("Device
    /// 'ide1-cd0' is not removable" says more than a code).
    pub fn execute(&mut self, cmd: &str, args: Value) -> Result<Value, String> {
        let id = self.next_id;
        self.next_id += 1;
        let mut msg = serde_json::json!({"execute": cmd, "id": id});
        if !args.is_null() {
            msg["arguments"] = args;
        }
        let mut line = msg.to_string();
        line.push('\n');
        self.stream.get_mut().write_all(line.as_bytes()).map_err(|e| format!("writing to the monitor: {e}"))?;
        let reply = self.read_until(|v| v.get("id").and_then(Value::as_u64) == Some(id))?;
        if let Some(r) = reply.get("return") {
            return Ok(r.clone());
        }
        if let Some(e) = reply.get("error") {
            return Err(format!(
                "{}: {}",
                e.get("class").and_then(Value::as_str).unwrap_or("Error"),
                e.get("desc").and_then(Value::as_str).unwrap_or("?")
            ));
        }
        Err(format!("malformed reply: {reply}"))
    }
}

/// The qdev id `bundle::Machine::qemu_args` gives the CD-ROM, so a live
/// medium change can name it.
pub const CDROM_ID: &str = "ide1-cd0";

impl Control {
    /// Put `disc` in the CD-ROM tray, replacing whatever is there.
    /// `blockdev-change-medium` does open/eject/insert/close as one
    /// command, which is what a guest expects to see from a disc swap.
    ///
    /// `force` overrides the tray lock, and it is required here. A guest
    /// with a volume mounted holds the medium locked (PREVENT ALLOW
    /// MEDIUM REMOVAL; XP does it for every open handle on the disc).
    /// For an unforced swap QEMU sends the guest an eject request,
    /// refuses the command and leaves the old disc in the drive, so the
    /// new one appears when the guest releases the lock (when the
    /// program holding it closes) rather than when the user clicked
    /// Insert. `eject_disc` below forces too, so the two halves of one
    /// gesture agree.
    pub fn insert_disc(&mut self, disc: &Path) -> Result<(), String> {
        // No `format` argument: QEMU probes, so a `.cue`/`.ccd` lands on
        // the `cdimage` driver (doc 17) as it does on the command line.
        self.execute(
            "blockdev-change-medium",
            // A folder goes in the drive the same way an image does, under
            // the prefix that makes it one (`disc_library::qemu_medium`).
            // No comma doubling here: this is a JSON string, not a QEMU
            // option string.
            serde_json::json!({"id": CDROM_ID, "filename": crate::disc_library::qemu_medium(disc), "force": true}),
        )
        .map(|_| ())
    }

    /// Open the tray and leave it empty.
    pub fn eject_disc(&mut self) -> Result<(), String> {
        self.execute("eject", serde_json::json!({"id": CDROM_ID, "force": true})).map(|_| ())
    }

    /// The block node holding `disk`, and the snapshots already in it.
    /// Snapshot commands address node names, which QEMU generates for a
    /// `-drive` (`#block123`). They are looked up rather than set in
    /// `qemu_args`, which would pin an implementation detail of the
    /// command line into the bundle format.
    ///
    /// The match is on the filename QEMU reports; if that fails (a
    /// relative path in the bundle, a symlink resolved on the way in)
    /// the first writable qcow2 node stands in, which for our
    /// single-disk machines is the same node.
    pub fn disk_node(&mut self, disk: &Path) -> Result<(String, Vec<crate::snapshots::Snapshot>), String> {
        let nodes = self.execute("query-named-block-nodes", serde_json::Value::Null)?;
        let nodes = nodes.as_array().ok_or("query-named-block-nodes: not an array")?;
        let wanted = disk.display().to_string();
        // A qcow2 file shows up as two nodes, the qcow2 format node and
        // the `file` protocol node under it, both reporting the same
        // filename. Only the format node can hold a snapshot, so the
        // driver is part of the match.
        let is_qcow2 = |n: &&Value| n["drv"].as_str() == Some("qcow2");
        let pick = nodes
            .iter()
            .find(|n| is_qcow2(n) && n["image"]["filename"].as_str() == Some(wanted.as_str()))
            .or_else(|| nodes.iter().find(|n| is_qcow2(n) && n["ro"].as_bool() != Some(true)))
            .ok_or_else(|| format!("the running machine has no qcow2 block node for {wanted}"))?;
        let name = pick["node-name"].as_str().ok_or("a block node without a node-name")?.to_string();
        Ok((name, crate::snapshots::parse(pick["image"].get("snapshots"))))
    }

    /// Start a snapshot job. `snapshot-save`/`-load`/`-delete` are jobs,
    /// not synchronous commands: they return as soon as the job is
    /// created and finish later (saving a 512 MB guest's RAM takes a
    /// visible moment), so the caller polls `job` below instead of
    /// blocking the UI thread on QEMU's main loop.
    ///
    /// `also` are more block nodes the snapshot covers beside the disk: a
    /// Windows 11 machine's firmware variable store (`snaps.rs`).
    pub fn start_snapshot_job(
        &mut self,
        command: &str,
        job_id: &str,
        tag: &str,
        node: &str,
        also: &[String],
    ) -> Result<(), String> {
        let mut devices = vec![node.to_string()];
        devices.extend(also.iter().cloned());
        let mut args = serde_json::json!({"job-id": job_id, "tag": tag, "devices": devices});
        if command != "snapshot-delete" {
            // The VM state goes in the same qcow2 as the disk, as with
            // `savevm`, so `qemu-img snapshot -a` (the offline path) can
            // roll back to it.
            args["vmstate"] = serde_json::Value::String(node.to_string());
        }
        self.execute(command, args).map(|_| ())
    }

    /// `(status, error)` for a job, or `None` once it's gone. A
    /// concluded job stays until dismissed, which is how its error is
    /// collected.
    pub fn job(&mut self, job_id: &str) -> Result<Option<(String, Option<String>)>, String> {
        let jobs = self.execute("query-jobs", serde_json::Value::Null)?;
        let Some(jobs) = jobs.as_array() else {
            return Ok(None);
        };
        Ok(jobs.iter().find(|j| j["id"].as_str() == Some(job_id)).map(|j| {
            (
                j["status"].as_str().unwrap_or("?").to_string(),
                j["error"].as_str().map(str::to_string),
            )
        }))
    }

    pub fn dismiss_job(&mut self, job_id: &str) -> Result<(), String> {
        self.execute("job-dismiss", serde_json::json!({"id": job_id})).map(|_| ())
    }

    /// Pause / resume the guest around a snapshot load: `snapshot-load`
    /// replaces the running machine's CPU and RAM state, and QEMU
    /// requires it to be stopped first.
    pub fn set_running(&mut self, run: bool) -> Result<(), String> {
        self.execute(if run { "cont" } else { "stop" }, serde_json::Value::Null).map(|_| ())
    }

    /// Whether the guest's CPUs are running. Asked before a snapshot load
    /// stops them, so a machine the user had already paused isn't resumed
    /// afterwards.
    pub fn is_running(&mut self) -> Result<bool, String> {
        Ok(self.execute("query-status", serde_json::Value::Null)?["running"].as_bool().unwrap_or(false))
    }
}
