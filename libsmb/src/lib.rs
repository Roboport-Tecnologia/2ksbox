//! A small SMB2/3 file server to embed in another program: each share is
//! a folder on the host, one account logs in with NTLMv2, and every
//! response is signed (what Windows 11 24H2's client requires).
//!
//! It speaks dialects 2.0.2 through 3.1.1 without encryption, leases or
//! oplocks, durable handles or DFS. Change notification polls the folder
//! once a second and reports what was added, removed or modified.
//! It is meant for one trusted client on a private transport, such as a
//! virtual machine's guest reaching it through the hypervisor's NAT,
//! rather than for a network: the transport is the security boundary, and
//! the account is what the client's protocol needs.
//!
//! ```no_run
//! use libsmb::{Account, Config, Server, Share};
//! let cfg = Config::new("myhost", Account::new("user", "secret"))
//!     .share(Share::new("host", "/some/folder"));
//! Server::new(cfg).listen_tcp("127.0.0.1:4450").unwrap();
//! ```

mod conn;
mod fs;
mod ntlm;
mod rpc;
mod spnego;
mod status;
mod wire;

use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::thread;

/// The highest dialect the server will agree to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u16)]
pub enum Dialect {
    Smb202 = 0x0202,
    Smb210 = 0x0210,
    Smb300 = 0x0300,
    Smb302 = 0x0302,
    Smb311 = 0x0311,
}

impl std::str::FromStr for Dialect {
    type Err = String;
    fn from_str(s: &str) -> Result<Dialect, String> {
        Ok(match s {
            "2.0.2" | "2.002" => Dialect::Smb202,
            "2.1" => Dialect::Smb210,
            "3.0" => Dialect::Smb300,
            "3.0.2" => Dialect::Smb302,
            "3.1.1" => Dialect::Smb311,
            _ => return Err(format!("unknown dialect {:?}", s)),
        })
    }
}

/// A folder served under a share name.
#[derive(Clone, Debug)]
pub struct Share {
    pub name: String,
    pub path: PathBuf,
    pub read_only: bool,
}

impl Share {
    pub fn new(name: &str, path: impl AsRef<Path>) -> Share {
        Share { name: name.to_string(), path: path.as_ref().to_path_buf(), read_only: false }
    }
    pub fn read_only(mut self, ro: bool) -> Share {
        self.read_only = ro;
        self
    }
}

/// The one account that may log in.
#[derive(Clone, Debug)]
pub struct Account {
    pub user: String,
    pub password: String,
}

impl Account {
    pub fn new(user: &str, password: &str) -> Account {
        Account { user: user.to_string(), password: password.to_string() }
    }
}

/// Receives the server's log lines; `verbose` marks the chatty ones.
pub type Logger = Arc<dyn Fn(bool, &str) + Send + Sync>;

pub struct Config {
    /// The NetBIOS / DNS name the server gives in its NTLM challenge.
    pub server_name: String,
    pub account: Account,
    pub shares: Vec<Share>,
    pub max_dialect: Dialect,
    pub logger: Option<Logger>,
}

impl Config {
    pub fn new(server_name: &str, account: Account) -> Config {
        Config {
            server_name: server_name.to_string(),
            account,
            shares: Vec::new(),
            max_dialect: Dialect::Smb311,
            logger: None,
        }
    }
    pub fn share(mut self, s: Share) -> Config {
        self.shares.push(s);
        self
    }
    pub fn max_dialect(mut self, d: Dialect) -> Config {
        self.max_dialect = d;
        self
    }
    pub fn logger(mut self, l: Logger) -> Config {
        self.logger = Some(l);
        self
    }
    pub(crate) fn log(&self, verbose: bool, msg: &str) {
        if let Some(l) = &self.logger {
            l(verbose, msg);
        }
    }
}

#[derive(Clone)]
pub struct Server {
    cfg: Arc<Config>,
}

impl Server {
    pub fn new(cfg: Config) -> Server {
        Server { cfg: Arc::new(cfg) }
    }

    /// Serves one connection until the client closes it: its reading and
    /// writing halves (for a socket, the socket and its `try_clone`). The
    /// writer is shared with the thread that completes change
    /// notifications. `peer` names the connection in the log.
    pub fn serve<R: Read, W: Write + Send + 'static>(&self, reader: R, writer: W, peer: &str) -> io::Result<()> {
        conn::Conn::new(self.cfg.clone(), peer.to_string()).run(reader, writer)
    }

    /// Accepts connections on a TCP address, each on a thread of its
    /// own; returns only on an accept error.
    pub fn listen_tcp(&self, addr: &str) -> io::Result<()> {
        let l = std::net::TcpListener::bind(addr)?;
        self.cfg.log(false, &format!("listening on tcp {}", l.local_addr()?));
        for s in l.incoming() {
            let s = s?;
            let _ = s.set_nodelay(true);
            let peer = s.peer_addr().map(|a| a.to_string()).unwrap_or_default();
            let w = s.try_clone()?;
            self.spawn(s, w, peer);
        }
        Ok(())
    }

    /// The same on a Unix-domain socket, created at `path`.
    #[cfg(unix)]
    pub fn listen_unix(&self, path: impl AsRef<Path>) -> io::Result<()> {
        let _ = std::fs::remove_file(path.as_ref());
        let l = std::os::unix::net::UnixListener::bind(path.as_ref())?;
        self.cfg.log(false, &format!("listening on {}", path.as_ref().display()));
        let mut n = 0u64;
        for s in l.incoming() {
            n += 1;
            let s = s?;
            let w = s.try_clone()?;
            self.spawn(s, w, format!("unix#{}", n));
        }
        Ok(())
    }

    fn spawn<R: Read + Send + 'static, W: Write + Send + 'static>(&self, r: R, w: W, peer: String) {
        let me = self.clone();
        thread::spawn(move || {
            if let Err(e) = me.serve(r, w, &peer) {
                me.cfg.log(false, &format!("[{}] {}", peer, e));
            }
        });
    }
}
