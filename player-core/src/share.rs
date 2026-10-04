//! The machine's shared folder (track M23, doc 24 §2): libsmb in this
//! process, on a Unix socket in a directory only this user can open.
//! QEMU's slirp forwards each of the guest's connections to
//! `10.0.2.4:445` to that socket (`guestfwd=…-unix:`, QEMU patch 79), so
//! the guest sees `\\10.0.2.4\host`. The account is fixed (`2ksbox`,
//! password `2ksbox`): the socket is the boundary, and the account is
//! what Windows' client needs to sign. The guest agent's `--map` maps it.

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
    #[cfg(not(unix))]
    {
        let _ = (dir, qemu_args);
        Err("a shared folder needs a Unix socket, which this host's QEMU does not forward to yet".into())
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if !dir.is_dir() {
            return Err(format!("the shared folder {} is not a folder", dir.display()));
        }
        let netdev = qemu_args
            .iter()
            .position(|a| a.starts_with("user,") || a == "user")
            .filter(|&i| i > 0 && qemu_args[i - 1] == "-netdev")
            .ok_or("a shared folder needs the machine's network (a user-mode -netdev)")?;
        let run = std::env::temp_dir().join(format!("2ksbox-{}", std::process::id()));
        std::fs::create_dir_all(&run).map_err(|e| format!("{}: {e}", run.display()))?;
        std::fs::set_permissions(&run, std::fs::Permissions::from_mode(0o700))
            .map_err(|e| format!("{}: {e}", run.display()))?;
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
        let server = libsmb::Server::new(cfg);
        let path = sock.clone();
        // bind before QEMU starts, so no guest connection finds nothing
        let _ = std::fs::remove_file(&sock);
        let listener = std::os::unix::net::UnixListener::bind(&sock).map_err(|e| format!("{}: {e}", sock.display()))?;
        std::thread::Builder::new()
            .name("share".into())
            .spawn(move || {
                if let Err(e) = server.serve_listener(listener) {
                    eprintln!("[share] {}: {e}", path.display());
                }
            })
            .map_err(|e| e.to_string())?;
        qemu_args[netdev].push_str(&format!(",guestfwd=tcp:10.0.2.4:445-unix:{}", sock.display()));
        eprintln!("[share] {} as \\\\10.0.2.4\\{}", dir.display(), SHARE_NAME);
        Ok(())
    }
}

/// At exit: the socket's directory goes.
pub fn stop() {
    if let Some(run) = RUNTIME.lock().unwrap().take() {
        let _ = std::fs::remove_dir_all(run);
    }
}
