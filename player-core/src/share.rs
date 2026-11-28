//! The machine's shared folder (track M23, doc 24 §2): libsmb in this
//! process, on a Unix socket in a directory only this user can open.
//! QEMU's slirp forwards each of the guest's connections to
//! `10.0.2.4:445` to that socket (`guestfwd=…-unix:`, QEMU patch 79), so
//! the guest sees `\\10.0.2.4\host`. The account is fixed (`2ksbox`,
//! password `2ksbox`): the socket is the boundary, and the account is
//! what Windows' client needs to sign. The guest agent's `--map` maps it.
//!
//! On Windows the socket is Winsock's `AF_UNIX` (Windows 10 1803 and
//! later), which slirp connects to through `patches/deps/libslirp`'s
//! `03-windows-unix-guestfwd`; `std` has no listener for it there, so
//! [`listen`] binds and accepts by hand.

use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// The private directory holding the socket, removed by [`stop`].
static RUNTIME: Mutex<Option<PathBuf>> = Mutex::new(None);

pub const SHARE_NAME: &str = "host";
pub const USER: &str = "2ksbox";
pub const PASSWORD: &str = "2ksbox";

/// Serve `dir` and point the machine's user-mode network at it: the
/// `-netdev user,…` in `qemu_args` gains the forward. Before `Qemu::new`.
pub fn start(dir: &Path, qemu_args: &mut [String]) -> Result<(), String> {
    if !dir.is_dir() {
        return Err(format!("the shared folder {} is not a folder", dir.display()));
    }
    let netdev = qemu_args
        .iter()
        .position(|a| a.starts_with("user,") || a == "user")
        .filter(|&i| i > 0 && qemu_args[i - 1] == "-netdev")
        .ok_or("a shared folder needs the machine's network (a user-mode -netdev)")?;
    let run = private_dir()?;
    let sock = run.join("smb.sock");
    *RUNTIME.lock().unwrap() = Some(run);

    let verbose = std::env::var_os("PLAYER_SMB_LOG").is_some();
    let cfg = libsmb::Config::new("2ksbox", libsmb::Account::new(USER, PASSWORD))
        .share(libsmb::Share::new(SHARE_NAME, dir))
        .logger(std::sync::Arc::new(move |chatty, msg| {
            if verbose || !chatty {
                eprintln!("[share] {msg}");
            }
        }));
    // bound before QEMU starts, so no guest connection finds nothing
    let _ = std::fs::remove_file(&sock);
    listen(libsmb::Server::new(cfg), &sock)?;
    qemu_args[netdev].push_str(&format!(",guestfwd=tcp:10.0.2.4:445-unix:{}", sock.display()));
    eprintln!("[share] {} as \\\\10.0.2.4\\{}", dir.display(), SHARE_NAME);
    Ok(())
}

/// A directory for the socket that only this user can open: mode 0700
/// on Unix; on Windows the user's own temporary folder, whose ACL (the
/// user, SYSTEM and administrators) the new folder inherits, and which an
/// `AF_UNIX` connect needs write access to.
fn private_dir() -> Result<PathBuf, String> {
    let run = std::env::temp_dir().join(format!("2ksbox-{}", std::process::id()));
    std::fs::create_dir_all(&run).map_err(|e| format!("{}: {e}", run.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&run, std::fs::Permissions::from_mode(0o700))
            .map_err(|e| format!("{}: {e}", run.display()))?;
    }
    Ok(run)
}

/// Binds the socket now and accepts on a thread of its own.
#[cfg(unix)]
fn listen(server: libsmb::Server, sock: &Path) -> Result<(), String> {
    let listener = std::os::unix::net::UnixListener::bind(sock).map_err(|e| format!("{}: {e}", sock.display()))?;
    let path = sock.to_path_buf();
    std::thread::Builder::new()
        .name("share".into())
        .spawn(move || {
            if let Err(e) = server.serve_listener(listener) {
                eprintln!("[share] {}: {e}", path.display());
            }
        })
        .map_err(|e| e.to_string())?;
    Ok(())
}

/// The same with Winsock: `socket(AF_UNIX)`, `bind`, `listen`, and each
/// accepted socket read and written through `std`'s `TcpStream`, whose
/// `recv` / `send` are any stream socket's. One stream is shared by the
/// connection's reader and writer (an `Arc`, not `try_clone`, which
/// duplicates through `WSADuplicateSocket`).
#[cfg(windows)]
fn listen(server: libsmb::Server, sock: &Path) -> Result<(), String> {
    use std::os::windows::io::{FromRawSocket, RawSocket};
    use std::sync::Arc;
    use windows_sys::Win32::Networking::WinSock as ws;

    let name = sock.to_str().ok_or_else(|| format!("{}: not a UTF-8 path", sock.display()))?.as_bytes();
    let mut sa: ws::SOCKADDR_UN = unsafe { std::mem::zeroed() };
    if name.len() >= sa.sun_path.len() {
        return Err(format!("{}: too long for a Unix socket", sock.display()));
    }
    sa.sun_family = ws::AF_UNIX;
    sa.sun_path[..name.len()].copy_from_slice(name);
    let listener = unsafe {
        // reference-counted; QEMU and std start Winsock too
        let mut wsa: ws::WSADATA = std::mem::zeroed();
        ws::WSAStartup(0x0202, &mut wsa);
        let l = ws::socket(ws::AF_UNIX as i32, ws::SOCK_STREAM, 0);
        if l == ws::INVALID_SOCKET {
            return Err(format!("a Unix socket: Winsock error {}", ws::WSAGetLastError()));
        }
        let len = std::mem::size_of::<ws::SOCKADDR_UN>() as i32;
        if ws::bind(l, &sa as *const _ as *const ws::SOCKADDR, len) != 0 || ws::listen(l, 16) != 0 {
            let e = ws::WSAGetLastError();
            ws::closesocket(l);
            return Err(format!("{}: Winsock error {e}", sock.display()));
        }
        l
    };

    struct Shared(Arc<std::net::TcpStream>);
    impl std::io::Read for Shared {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            (&*self.0).read(buf)
        }
    }
    impl std::io::Write for Shared {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            (&*self.0).write(buf)
        }
        fn flush(&mut self) -> std::io::Result<()> {
            (&*self.0).flush()
        }
    }

    let path = sock.to_path_buf();
    std::thread::Builder::new()
        .name("share".into())
        .spawn(move || {
            let mut n = 0u64;
            loop {
                let s = unsafe { ws::accept(listener, std::ptr::null_mut(), std::ptr::null_mut()) };
                if s == ws::INVALID_SOCKET {
                    eprintln!("[share] {}: accept: Winsock error {}", path.display(), unsafe { ws::WSAGetLastError() });
                    return;
                }
                n += 1;
                let stream = Arc::new(unsafe { std::net::TcpStream::from_raw_socket(s as RawSocket) });
                let (r, w) = (Shared(stream.clone()), Shared(stream));
                let server = server.clone();
                let peer = format!("unix#{n}");
                std::thread::spawn(move || {
                    if let Err(e) = server.serve(r, w, &peer) {
                        eprintln!("[share] [{peer}] {e}");
                    }
                });
            }
        })
        .map_err(|e| e.to_string())?;
    Ok(())
}

/// At exit: the socket's directory goes.
pub fn stop() {
    if let Some(run) = RUNTIME.lock().unwrap().take() {
        let _ = std::fs::remove_dir_all(run);
    }
}
