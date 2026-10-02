//! One client connection: the direct-TCP framing, SMB2 headers and
//! compounds, signing, and every command.

use crate::fs::{self as hostfs, Entry};
use crate::ntlm::{Acceptor, Authenticated};
use crate::status::*;
use crate::wire::{from_utf16, slice, u16_at, u32_at, u64_at, utf16, W};
use crate::{spnego, Config, Dialect};
use aes::Aes128;
use cmac::Cmac;
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256, Sha512};
use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::path::PathBuf;
use std::sync::Arc;

const NEGOTIATE: u16 = 0x00;
const SESSION_SETUP: u16 = 0x01;
const LOGOFF: u16 = 0x02;
const TREE_CONNECT: u16 = 0x03;
const TREE_DISCONNECT: u16 = 0x04;
const CREATE: u16 = 0x05;
const CLOSE: u16 = 0x06;
const FLUSH: u16 = 0x07;
const READ: u16 = 0x08;
const WRITE: u16 = 0x09;
const LOCK: u16 = 0x0a;
const IOCTL: u16 = 0x0b;
const CANCEL: u16 = 0x0c;
const ECHO: u16 = 0x0d;
const QUERY_DIRECTORY: u16 = 0x0e;
const CHANGE_NOTIFY: u16 = 0x0f;
const QUERY_INFO: u16 = 0x10;
const SET_INFO: u16 = 0x11;

const FLAG_RESPONSE: u32 = 0x01;
const FLAG_RELATED: u32 = 0x04;
const FLAG_SIGNED: u32 = 0x08;

const CAP_LARGE_MTU: u32 = 0x04;
const MAX_IO: u32 = 1 << 20;
const HDR: usize = 64;

const FSCTL_DFS_GET_REFERRALS: u32 = 0x0006_0194;
const FSCTL_DFS_GET_REFERRALS_EX: u32 = 0x0006_01b0;
const FSCTL_VALIDATE_NEGOTIATE_INFO: u32 = 0x0014_0204;
const FSCTL_GET_REPARSE_POINT: u32 = 0x0009_00a8;

/// Access bits that need the share to be writable.
const WRITE_ACCESS: u32 = 0x0000_0002 | 0x0000_0004 | 0x0000_0010 | 0x0000_0100 | 0x0001_0000 | 0x4000_0000 | 0x1000_0000;

fn name(cmd: u16) -> &'static str {
    [
        "NEGOTIATE", "SESSION_SETUP", "LOGOFF", "TREE_CONNECT", "TREE_DISCONNECT", "CREATE", "CLOSE", "FLUSH",
        "READ", "WRITE", "LOCK", "IOCTL", "CANCEL", "ECHO", "QUERY_DIRECTORY", "CHANGE_NOTIFY", "QUERY_INFO",
        "SET_INFO", "OPLOCK_BREAK",
    ]
    .get(cmd as usize)
    .copied()
    .unwrap_or("?")
}

struct Hdr {
    credit_charge: u16,
    command: u16,
    credits: u16,
    flags: u32,
    next: u32,
    message_id: u64,
    pid: u32,
    tree_id: u32,
    session_id: u64,
}

fn parse_hdr(m: &[u8]) -> Option<Hdr> {
    if m.len() < HDR || &m[0..4] != b"\xfeSMB" || u16_at(m, 4) != 64 {
        return None;
    }
    Some(Hdr {
        credit_charge: u16_at(m, 6),
        command: u16_at(m, 12),
        credits: u16_at(m, 14),
        flags: u32_at(m, 16),
        next: u32_at(m, 20),
        message_id: u64_at(m, 24),
        pid: u32_at(m, 32),
        tree_id: u32_at(m, 36),
        session_id: u64_at(m, 40),
    })
}

/// A handler's answer: a status and the body after the header, plus the
/// session or tree a SESSION_SETUP or TREE_CONNECT created.
struct Reply {
    status: u32,
    body: Vec<u8>,
    session_id: Option<u64>,
    tree_id: Option<u32>,
}

fn ok(body: Vec<u8>) -> Result<Reply, u32> {
    Ok(Reply { status: STATUS_SUCCESS, body, session_id: None, tree_id: None })
}

fn with(status: u32, body: Vec<u8>) -> Result<Reply, u32> {
    Ok(Reply { status, body, session_id: None, tree_id: None })
}

fn error_body() -> Vec<u8> {
    vec![9, 0, 0, 0, 0, 0, 0, 0, 0]
}

enum SignKey {
    Hmac([u8; 16]),
    Cmac([u8; 16]),
}

impl SignKey {
    fn sign(&self, msg: &[u8]) -> [u8; 16] {
        let mut out = [0u8; 16];
        match self {
            SignKey::Hmac(k) => {
                let mut m = <Hmac<Sha256> as Mac>::new_from_slice(k).unwrap();
                m.update(&msg[..48]);
                m.update(&[0u8; 16]);
                m.update(&msg[64..]);
                out.copy_from_slice(&m.finalize().into_bytes()[..16]);
            }
            SignKey::Cmac(k) => {
                let mut m = <Cmac<Aes128> as Mac>::new_from_slice(k).unwrap();
                m.update(&msg[..48]);
                m.update(&[0u8; 16]);
                m.update(&msg[64..]);
                out.copy_from_slice(&m.finalize().into_bytes());
            }
        }
        out
    }
}

/// SP800-108 in counter mode with HMAC-SHA256, 128 bits (MS-SMB2 3.1.4.2).
fn kdf(key: &[u8], label: &[u8], context: &[u8]) -> [u8; 16] {
    let mut m = <Hmac<Sha256> as Mac>::new_from_slice(key).unwrap();
    m.update(&1u32.to_be_bytes());
    m.update(label);
    m.update(&[0]);
    m.update(context);
    m.update(&128u32.to_be_bytes());
    let mut out = [0u8; 16];
    out.copy_from_slice(&m.finalize().into_bytes()[..16]);
    out
}

enum SessionState {
    /// Between the CHALLENGE and the AUTHENTICATE.
    Pending { acceptor: Option<Acceptor>, mech_types: Option<Vec<u8>>, raw: bool },
    Done { key: SignKey, user: String },
}

struct Session {
    state: SessionState,
    preauth: [u8; 64],
    trees: HashMap<u32, Option<usize>>,
}

struct Open {
    session_id: u64,
    tree_id: u32,
    share: usize,
    path: PathBuf,
    rel: String,
    is_dir: bool,
    file: Option<File>,
    delete_on_close: bool,
    listing: Option<(Vec<Entry>, usize, String)>,
    access: u32,
}

pub struct Conn {
    cfg: Arc<Config>,
    peer: String,
    dialect: u16,
    server_guid: [u8; 16],
    client_guid: [u8; 16],
    client_caps: u32,
    client_secmode: u16,
    preauth: [u8; 64],
    sessions: HashMap<u64, Session>,
    next_session: u64,
    next_tree: u32,
    opens: HashMap<u64, Open>,
    next_file: u64,
}

fn sha512(parts: &[&[u8]]) -> [u8; 64] {
    let mut h = Sha512::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

fn read_frame<S: Read>(s: &mut S) -> io::Result<Option<Vec<u8>>> {
    let mut head = [0u8; 4];
    match s.read_exact(&mut head) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e),
    }
    if head[0] != 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "not a direct-TCP frame"));
    }
    let len = (head[1] as usize) << 16 | (head[2] as usize) << 8 | head[3] as usize;
    let mut buf = vec![0u8; len];
    s.read_exact(&mut buf)?;
    Ok(Some(buf))
}

fn write_frame<S: Write>(s: &mut S, msg: &[u8]) -> io::Result<()> {
    let n = msg.len();
    let mut out = Vec::with_capacity(n + 4);
    out.extend_from_slice(&[0, (n >> 16) as u8, (n >> 8) as u8, n as u8]);
    out.extend_from_slice(msg);
    s.write_all(&out)?;
    s.flush()
}

impl Conn {
    pub fn new(cfg: Arc<Config>, peer: String) -> Conn {
        Conn {
            cfg,
            peer,
            dialect: 0,
            server_guid: crate::wire::random::<16>(),
            client_guid: [0; 16],
            client_caps: 0,
            client_secmode: 0,
            preauth: [0; 64],
            sessions: HashMap::new(),
            next_session: 1,
            next_tree: 1,
            opens: HashMap::new(),
            next_file: 1,
        }
    }

    fn log(&self, verbose: bool, msg: &str) {
        self.cfg.log(verbose, &format!("[{}] {}", self.peer, msg));
    }

    pub fn run<S: Read + Write>(&mut self, mut s: S) -> io::Result<()> {
        self.log(false, "connected");
        while let Some(msg) = read_frame(&mut s)? {
            if let Some(out) = self.message(&msg) {
                write_frame(&mut s, &out)?;
            }
        }
        self.log(false, "disconnected");
        for (_, o) in self.opens.drain() {
            finish(&self.cfg, o);
        }
        Ok(())
    }

    /// One frame in, the frame to answer it (none for a lone CANCEL).
    fn message(&mut self, msg: &[u8]) -> Option<Vec<u8>> {
        if msg.starts_with(b"\xffSMB") {
            return self.smb1_negotiate(msg);
        }
        let mut out = Vec::new();
        let mut off = 0usize;
        let mut last_file: Option<[u8; 16]> = None;
        let mut last_session = 0u64;
        let mut last_tree = 0u32;
        let mut last_status = STATUS_SUCCESS;
        let mut pieces: Vec<(usize, Option<u64>)> = Vec::new();
        loop {
            let rest = &msg[off..];
            let Some(h) = parse_hdr(rest) else {
                self.log(false, "malformed header, dropping the frame");
                return None;
            };
            let end = if h.next == 0 { rest.len() } else { (h.next as usize).min(rest.len()) };
            let req = &rest[..end];
            let related = h.flags & FLAG_RELATED != 0;
            let session_id = if related { last_session } else { h.session_id };
            let tree_id = if related { last_tree } else { h.tree_id };
            if !related {
                last_file = None;
            }

            if h.flags & FLAG_SIGNED != 0 {
                self.check_signature(session_id, req);
            }

            let reply = if h.command == CANCEL {
                None
            } else if related && last_status != STATUS_SUCCESS && last_status != STATUS_BUFFER_OVERFLOW {
                Some(Err(last_status))
            } else {
                let ctx = Ctx { req, session_id, tree_id, last_file };
                Some(self.dispatch(&h, &ctx))
            };

            if let Some(reply) = reply {
                let (status, body, sid, tid) = match reply {
                    Ok(r) => (r.status, r.body, r.session_id, r.tree_id),
                    Err(st) => (st, error_body(), None, None),
                };
                let sid = sid.unwrap_or(session_id);
                let tid = tid.unwrap_or(tree_id);
                if h.command == CREATE && status == STATUS_SUCCESS {
                    let mut fid = [0u8; 16];
                    fid.copy_from_slice(&body[64..80]);
                    last_file = Some(fid);
                }
                last_session = sid;
                last_tree = tid;
                last_status = status;
                self.log(
                    status != STATUS_SUCCESS && h.command != QUERY_DIRECTORY && status != STATUS_MORE_PROCESSING_REQUIRED,
                    &format!("{} {:#010x} mid={}", name(h.command), status, h.message_id),
                );

                let start = out.len();
                if start > 0 {
                    while out.len() % 8 != 0 {
                        out.push(0);
                    }
                }
                let at = out.len();
                // the previous piece's NextCommand now that its padding is known
                if let Some(&(prev, _)) = pieces.last() {
                    let n = (at - prev) as u32;
                    out[prev + 20..prev + 24].copy_from_slice(&n.to_le_bytes());
                }
                let credits = h.credits.max(1).min(512);
                let mut flags = FLAG_RESPONSE | (h.flags & FLAG_RELATED);
                let sign = self.signs(sid, h.command, status);
                if sign {
                    flags |= FLAG_SIGNED;
                }
                let mut w = W::new();
                w.bytes(b"\xfeSMB").u16(64).u16(h.credit_charge).u32(status).u16(h.command).u16(credits);
                w.u32(flags).u32(0).u64(h.message_id).u32(h.pid).u32(tid).u64(sid).zeros(16);
                w.bytes(&body);
                out.extend_from_slice(&w.0);
                pieces.push((at, if sign { Some(sid) } else { None }));

                if h.command == SESSION_SETUP && status == STATUS_MORE_PROCESSING_REQUIRED && self.dialect == 0x0311 {
                    if let Some(s) = self.sessions.get_mut(&sid) {
                        s.preauth = sha512(&[&s.preauth, &out[at..]]);
                    }
                }
                if h.command == NEGOTIATE && self.dialect == 0x0311 {
                    self.preauth = sha512(&[&self.preauth, &out[at..]]);
                }
            }
            if h.next == 0 || off + (h.next as usize) >= msg.len() {
                break;
            }
            off += h.next as usize;
        }
        if out.is_empty() {
            return None;
        }
        // sign each piece over its bytes, padding included
        for i in 0..pieces.len() {
            let (at, sid) = pieces[i];
            let end = pieces.get(i + 1).map_or(out.len(), |p| p.0);
            if let Some(sid) = sid {
                if let Some(Session { state: SessionState::Done { key, .. }, .. }) = self.sessions.get(&sid) {
                    let sig = key.sign(&out[at..end]);
                    out[at + 48..at + 64].copy_from_slice(&sig);
                }
            }
        }
        Some(out)
    }

    fn signs(&self, sid: u64, command: u16, status: u32) -> bool {
        if command == NEGOTIATE || status == STATUS_MORE_PROCESSING_REQUIRED {
            return false;
        }
        matches!(self.sessions.get(&sid), Some(Session { state: SessionState::Done { .. }, .. }))
    }

    fn check_signature(&self, sid: u64, req: &[u8]) {
        if let Some(Session { state: SessionState::Done { key, .. }, .. }) = self.sessions.get(&sid) {
            if key.sign(req)[..] != req[48..64] {
                self.log(false, &format!("bad signature on {}", name(u16_at(req, 12))));
            }
        }
    }

    fn dispatch(&mut self, h: &Hdr, c: &Ctx) -> Result<Reply, u32> {
        match h.command {
            NEGOTIATE => self.negotiate(c),
            SESSION_SETUP => self.session_setup(c),
            ECHO => ok(vec![4, 0, 0, 0]),
            _ => {
                if !matches!(self.sessions.get(&c.session_id), Some(Session { state: SessionState::Done { .. }, .. })) {
                    return Err(STATUS_USER_SESSION_DELETED);
                }
                match h.command {
                    LOGOFF => {
                        self.sessions.remove(&c.session_id);
                        let gone: Vec<u64> =
                            self.opens.iter().filter(|(_, o)| o.session_id == c.session_id).map(|(k, _)| *k).collect();
                        for k in gone {
                            let o = self.opens.remove(&k).unwrap();
                            finish(&self.cfg, o);
                        }
                        ok(vec![4, 0, 0, 0])
                    }
                    TREE_CONNECT => self.tree_connect(c),
                    _ => {
                        let share = *self
                            .sessions
                            .get(&c.session_id)
                            .and_then(|s| s.trees.get(&c.tree_id))
                            .ok_or(STATUS_NETWORK_NAME_DELETED)?;
                        match h.command {
                            TREE_DISCONNECT => {
                                self.sessions.get_mut(&c.session_id).unwrap().trees.remove(&c.tree_id);
                                ok(vec![4, 0, 0, 0])
                            }
                            CREATE => match share {
                                Some(i) => self.create(c, i),
                                None => Err(STATUS_OBJECT_NAME_NOT_FOUND),
                            },
                            CLOSE => self.close(c),
                            FLUSH => {
                                let o = self.open(c, 8)?;
                                if let Some(f) = &o.file {
                                    let _ = f.sync_all();
                                }
                                ok(vec![4, 0, 0, 0])
                            }
                            READ => self.read(c),
                            WRITE => self.write(c),
                            LOCK => {
                                self.open(c, 8)?;
                                ok(vec![4, 0, 0, 0])
                            }
                            IOCTL => self.ioctl(c),
                            QUERY_DIRECTORY => self.query_directory(c),
                            CHANGE_NOTIFY => Err(STATUS_NOT_SUPPORTED),
                            QUERY_INFO => self.query_info(c),
                            SET_INFO => self.set_info(c),
                            _ => Err(STATUS_NOT_SUPPORTED),
                        }
                    }
                }
            }
        }
    }

    // --- negotiation and login ---------------------------------------------

    fn smb1_negotiate(&mut self, msg: &[u8]) -> Option<Vec<u8>> {
        if msg.get(4) != Some(&0x72) || msg.len() < 35 {
            return None;
        }
        let data = &msg[35..];
        let dialects: Vec<&[u8]> = data.split(|&b| b == 0).filter_map(|d| d.strip_prefix(&[2u8][..])).collect();
        let dialect = if dialects.iter().any(|d| *d == b"SMB 2.???") && self.cfg.max_dialect >= Dialect::Smb210 {
            0x02ff
        } else if dialects.iter().any(|d| *d == b"SMB 2.002") {
            0x0202
        } else {
            self.log(false, "SMB1-only client, refused");
            return None;
        };
        if dialect == 0x0202 {
            self.dialect = 0x0202;
        }
        self.log(false, &format!("SMB1 negotiate, answered with dialect {:#06x}", dialect));
        let body = self.negotiate_body(dialect, &[]);
        let mut w = W::new();
        w.bytes(b"\xfeSMB").u16(64).u16(0).u32(0).u16(NEGOTIATE).u16(1);
        w.u32(FLAG_RESPONSE).u32(0).u64(0).u32(0).u32(0).u64(0).zeros(16);
        w.bytes(&body);
        Some(w.0)
    }

    fn negotiate_body(&self, dialect: u16, contexts: &[Vec<u8>]) -> Vec<u8> {
        let blob = spnego::hint();
        let mut w = W::new();
        w.u16(65).u16(0x01 | 0x02).u16(dialect).u16(contexts.len() as u16);
        w.bytes(&self.server_guid).u32(if dialect >= 0x0210 { CAP_LARGE_MTU } else { 0 });
        w.u32(MAX_IO).u32(MAX_IO).u32(MAX_IO).u64(crate::wire::now()).u64(0);
        w.u16((HDR + 64) as u16).u16(blob.len() as u16).u32(0);
        w.bytes(&blob);
        if !contexts.is_empty() {
            w.align(8);
            let off = HDR + w.len();
            w.put_u32(60, off as u32);
            for (i, c) in contexts.iter().enumerate() {
                if i > 0 {
                    w.align(8);
                }
                w.bytes(c);
            }
        }
        w.0
    }

    fn negotiate(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let count = u16_at(r, HDR + 2) as usize;
        self.client_secmode = u16_at(r, HDR + 4);
        self.client_caps = u32_at(r, HDR + 8);
        self.client_guid.copy_from_slice(slice(r, HDR + 12, 16).ok_or(STATUS_INVALID_PARAMETER)?);
        let offered: Vec<u16> = (0..count).map(|i| u16_at(r, HDR + 36 + 2 * i)).collect();
        let dialect = [0x0311u16, 0x0302, 0x0300, 0x0210, 0x0202]
            .into_iter()
            .filter(|d| *d <= self.cfg.max_dialect as u16)
            .find(|d| offered.contains(d))
            .ok_or(STATUS_NOT_SUPPORTED)?;
        self.dialect = dialect;
        self.log(false, &format!("dialect {:#06x} (client offered {:x?})", dialect, offered));

        let mut contexts = Vec::new();
        if dialect == 0x0311 {
            self.preauth = sha512(&[&[0u8; 64], r]);
            let ctx_off = u32_at(r, HDR + 28) as usize;
            let ctx_count = u16_at(r, HDR + 32) as usize;
            let mut at = ctx_off;
            let mut preauth = false;
            for _ in 0..ctx_count {
                at = (at + 7) & !7;
                let ty = u16_at(r, at);
                let len = u16_at(r, at + 2) as usize;
                let data = slice(r, at + 8, len).ok_or(STATUS_INVALID_PARAMETER)?;
                match ty {
                    1 => {
                        // PREAUTH_INTEGRITY: SHA-512 is the only one
                        let n = u16_at(data, 0) as usize;
                        if !(0..n).any(|i| u16_at(data, 4 + 2 * i) == 1) {
                            return Err(STATUS_INVALID_PARAMETER);
                        }
                        preauth = true;
                        let mut w = W::new();
                        w.u16(1).u16(38).u32(0).u16(1).u16(32).u16(1).bytes(&crate::wire::random::<32>());
                        contexts.push(w.0);
                    }
                    2 => {
                        // ENCRYPTION: no cipher in common
                        let mut w = W::new();
                        w.u16(2).u16(4).u32(0).u16(1).u16(0);
                        contexts.push(w.0);
                    }
                    8 => {
                        // SIGNING: AES-CMAC
                        let n = u16_at(data, 0) as usize;
                        let algs: Vec<u16> = (0..n).map(|i| u16_at(data, 2 + 2 * i)).collect();
                        let pick = if algs.contains(&1) { 1 } else { 0 };
                        let mut w = W::new();
                        w.u16(8).u16(4).u32(0).u16(1).u16(pick);
                        contexts.push(w.0);
                    }
                    _ => {}
                }
                at += 8 + len;
            }
            if !preauth {
                return Err(STATUS_INVALID_PARAMETER);
            }
        }
        ok(self.negotiate_body(dialect, &contexts))
    }

    fn session_setup(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let off = u16_at(r, HDR + 12) as usize;
        let len = u16_at(r, HDR + 14) as usize;
        let blob = slice(r, off, len).ok_or(STATUS_INVALID_PARAMETER)?;
        let sid = if c.session_id == 0 || !self.sessions.contains_key(&c.session_id) {
            let sid = self.next_session;
            self.next_session += 1;
            self.sessions.insert(
                sid,
                Session {
                    state: SessionState::Pending { acceptor: None, mech_types: None, raw: false },
                    preauth: self.preauth,
                    trees: HashMap::new(),
                },
            );
            sid
        } else {
            c.session_id
        };
        let dialect = self.dialect;
        let cfg = self.cfg.clone();
        let sess = self.sessions.get_mut(&sid).unwrap();
        if dialect == 0x0311 {
            sess.preauth = sha512(&[&sess.preauth, r]);
        }
        let Some(tok) = spnego::parse(blob) else {
            self.sessions.remove(&sid);
            return Err(STATUS_INVALID_PARAMETER);
        };
        let raw = blob.starts_with(crate::ntlm::SIGNATURE);
        let reply = |status: u32, buf: Vec<u8>| -> Result<Reply, u32> {
            let mut w = W::new();
            w.u16(9).u16(0).u16((HDR + 8) as u16).u16(buf.len() as u16).bytes(&buf);
            Ok(Reply { status, body: w.0, session_id: Some(sid), tree_id: None })
        };
        let state = std::mem::replace(&mut sess.state, SessionState::Pending { acceptor: None, mech_types: None, raw });
        match state {
            SessionState::Pending { acceptor: None, .. } => {
                let neg = tok.token.filter(|_| tok.ntlm_ok).ok_or(STATUS_LOGON_FAILURE);
                let Some((acc, challenge)) = neg.ok().and_then(|n| Acceptor::challenge(n, &cfg.server_name)) else {
                    self.sessions.remove(&sid);
                    return Err(STATUS_LOGON_FAILURE);
                };
                let mech_types = tok.mech_types.map(|m| m.to_vec());
                sess.state = SessionState::Pending { acceptor: Some(acc), mech_types, raw };
                let out = if raw { challenge } else { spnego::resp(1, Some(&challenge), None, true) };
                reply(STATUS_MORE_PROCESSING_REQUIRED, out)
            }
            SessionState::Pending { acceptor: Some(acc), mech_types, raw } => {
                let auth = tok.token.ok_or(STATUS_LOGON_FAILURE);
                let result = auth.and_then(|a| {
                    acc.authenticate(a, &cfg.account.user, &cfg.account.password).map_err(|e| {
                        cfg.log(false, &format!("login refused: {:?}", e));
                        STATUS_LOGON_FAILURE
                    })
                });
                let auth: Authenticated = match result {
                    Ok(a) => a,
                    Err(st) => {
                        self.sessions.remove(&sid);
                        return Err(st);
                    }
                };
                let k = auth.session_key;
                let key = match dialect {
                    0x0311 => SignKey::Cmac(kdf(&k, b"SMBSigningKey\0", &sess.preauth)),
                    0x0300 | 0x0302 => SignKey::Cmac(kdf(&k, b"SMB2AESCMAC\0", b"SmbSign\0")),
                    _ => SignKey::Hmac(k),
                };
                let mic = match (&mech_types, tok.mic) {
                    (Some(m), Some(_)) => Some(auth.server_mic(m)),
                    _ => None,
                };
                let who = format!("{}\\{}", auth.domain, auth.user);
                sess.state = SessionState::Done { key, user: auth.user };
                cfg.log(false, &format!("session {} logged in as {}", sid, who));
                let out = if raw { Vec::new() } else { spnego::resp(0, None, mic.as_ref().map(|m| &m[..]), false) };
                reply(STATUS_SUCCESS, out)
            }
            SessionState::Done { key, user } => {
                // a re-authentication: not supported, keep the session
                sess.state = SessionState::Done { key, user };
                Err(STATUS_NOT_SUPPORTED)
            }
        }
    }

    fn tree_connect(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let off = u16_at(r, HDR + 4) as usize;
        let len = u16_at(r, HDR + 6) as usize;
        let path = from_utf16(slice(r, off, len).ok_or(STATUS_INVALID_PARAMETER)?);
        let share_name = path.rsplit('\\').next().unwrap_or("");
        let share = if share_name.eq_ignore_ascii_case("IPC$") {
            None
        } else {
            match self.cfg.shares.iter().position(|s| s.name.eq_ignore_ascii_case(share_name)) {
                Some(i) => Some(i),
                None => {
                    self.log(false, &format!("no share {:?}", path));
                    return Err(STATUS_BAD_NETWORK_NAME);
                }
            }
        };
        let tid = self.next_tree;
        self.next_tree += 1;
        self.sessions.get_mut(&c.session_id).unwrap().trees.insert(tid, share);
        let (ty, access) = match share {
            None => (2u8, 0x001f_01ffu32),
            Some(i) if self.cfg.shares[i].read_only => (1, 0x0012_00a9),
            Some(_) => (1, 0x001f_01ff),
        };
        self.log(false, &format!("tree {} = {:?}", tid, path));
        let mut w = W::new();
        // no client-side caching (Offline Files) of a host folder
        w.u16(16).u8(ty).u8(0).u32(0x30).u32(0).u32(access);
        Ok(Reply { status: STATUS_SUCCESS, body: w.0, session_id: None, tree_id: Some(tid) })
    }

    // --- files ---------------------------------------------------------------

    fn file_id(&self, c: &Ctx, at: usize) -> Result<u64, u32> {
        let mut fid = [0u8; 16];
        fid.copy_from_slice(slice(c.req, HDR + at, 16).ok_or(STATUS_INVALID_PARAMETER)?);
        if fid == [0xff; 16] {
            fid = c.last_file.ok_or(STATUS_FILE_CLOSED)?;
        }
        let id = u64::from_le_bytes(fid[8..].try_into().unwrap());
        match self.opens.get(&id) {
            Some(o) if o.session_id == c.session_id && o.tree_id == c.tree_id => Ok(id),
            _ => Err(STATUS_FILE_CLOSED),
        }
    }

    fn open(&mut self, c: &Ctx, at: usize) -> Result<&mut Open, u32> {
        let id = self.file_id(c, at)?;
        Ok(self.opens.get_mut(&id).unwrap())
    }

    fn create(&mut self, c: &Ctx, share: usize) -> Result<Reply, u32> {
        let r = c.req;
        let access = u32_at(r, HDR + 24);
        let disposition = u32_at(r, HDR + 36);
        let options = u32_at(r, HDR + 40);
        let noff = u16_at(r, HDR + 44) as usize;
        let nlen = u16_at(r, HDR + 46) as usize;
        let rel = from_utf16(if nlen == 0 { &[] } else { slice(r, noff, nlen).ok_or(STATUS_INVALID_PARAMETER)? });
        let sh = &self.cfg.shares[share];
        let want_dir = options & 0x01 != 0;
        let non_dir = options & 0x40 != 0;
        let delete_on_close = options & 0x1000 != 0;
        if sh.read_only && (access & WRITE_ACCESS != 0 || delete_on_close || !matches!(disposition, 1 | 3)) {
            return Err(STATUS_ACCESS_DENIED);
        }
        let path = hostfs::resolve(&sh.path, &rel)?;
        let existing = fs::metadata(&path).ok();
        let mut action = 1u32; // FILE_OPENED
        match (disposition, &existing) {
            (1 | 4, None) => return Err(STATUS_OBJECT_NAME_NOT_FOUND),
            (2, Some(_)) => return Err(STATUS_OBJECT_NAME_COLLISION),
            (0..=5, _) => {}
            _ => return Err(STATUS_INVALID_PARAMETER),
        }
        if let Some(m) = &existing {
            if want_dir && !m.is_dir() {
                return Err(STATUS_NOT_A_DIRECTORY);
            }
            if non_dir && m.is_dir() {
                return Err(STATUS_FILE_IS_A_DIRECTORY);
            }
            if matches!(disposition, 0 | 4 | 5) {
                if m.is_dir() {
                    return Err(STATUS_FILE_IS_A_DIRECTORY);
                }
                if sh.read_only {
                    return Err(STATUS_ACCESS_DENIED);
                }
                OpenOptions::new().write(true).truncate(true).open(&path).map_err(|e| from_io(&e))?;
                action = if disposition == 0 { 0 } else { 3 };
            }
        } else {
            if sh.read_only {
                return Err(STATUS_ACCESS_DENIED);
            }
            if want_dir {
                fs::create_dir(&path).map_err(|e| from_io(&e))?;
            } else {
                OpenOptions::new().write(true).create_new(true).open(&path).map_err(|e| from_io(&e))?;
            }
            action = 2;
        }
        let meta = fs::metadata(&path).map_err(|e| from_io(&e))?;
        let file = if meta.is_dir() {
            None
        } else {
            let rw = !sh.read_only && (access & WRITE_ACCESS != 0 || access & 0x0200_0000 != 0);
            match OpenOptions::new().read(true).write(rw).open(&path) {
                Ok(f) => Some(f),
                Err(_) if rw && access & 0x0200_0000 != 0 => OpenOptions::new().read(true).open(&path).ok(),
                Err(e) if access & !(0x0010_0080 | 0x0002_0000) == 0 => {
                    let _ = e;
                    None
                }
                Err(e) => return Err(from_io(&e)),
            }
        };
        let leaf = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let info = hostfs::info(&leaf, &meta);
        let id = self.next_file;
        self.next_file += 1;
        self.opens.insert(
            id,
            Open {
                session_id: c.session_id,
                tree_id: c.tree_id,
                share,
                path,
                rel,
                is_dir: meta.is_dir(),
                file,
                delete_on_close,
                listing: None,
                access,
            },
        );
        let mut w = W::new();
        w.u16(89).u8(0).u8(0).u32(action);
        w.u64(info.created).u64(info.accessed).u64(info.written).u64(info.changed);
        w.u64(info.alloc).u64(info.size).u32(info.attrs).u32(0);
        w.u64(id).u64(id).u32(0).u32(0);
        ok(w.0)
    }

    fn close(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let id = self.file_id(c, 8)?;
        let flags = u16_at(c.req, HDR + 2);
        let o = self.opens.remove(&id).unwrap();
        let info = if flags & 1 != 0 { fs::metadata(&o.path).ok().map(|m| hostfs::info("", &m)) } else { None };
        finish(&self.cfg, o);
        let mut w = W::new();
        w.u16(60).u16(flags & 1).u32(0);
        match info {
            Some(i) => {
                w.u64(i.created).u64(i.accessed).u64(i.written).u64(i.changed).u64(i.alloc).u64(i.size).u32(i.attrs);
            }
            None => {
                w.zeros(52);
            }
        }
        ok(w.0)
    }

    fn read(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let len = u32_at(r, HDR + 4).min(MAX_IO) as usize;
        let offset = u64_at(r, HDR + 8);
        let min = u32_at(r, HDR + 32) as usize;
        let o = self.open(c, 16)?;
        if o.is_dir {
            return Err(STATUS_INVALID_DEVICE_REQUEST);
        }
        let f = o.file.as_ref().ok_or(STATUS_ACCESS_DENIED)?;
        let mut buf = vec![0u8; len];
        let mut got = 0;
        while got < len {
            match read_at(f, &mut buf[got..], offset + got as u64) {
                Ok(0) => break,
                Ok(n) => got += n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(e) => return Err(from_io(&e)),
            }
        }
        if got == 0 && len > 0 || got < min {
            return Err(STATUS_END_OF_FILE);
        }
        let mut w = W::new();
        w.u16(17).u8(80).u8(0).u32(got as u32).u32(0).u32(0);
        w.bytes(&buf[..got]);
        ok(w.0)
    }

    fn write(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let doff = u16_at(r, HDR + 2) as usize;
        let len = u32_at(r, HDR + 4) as usize;
        let mut offset = u64_at(r, HDR + 8);
        let data = slice(r, doff, len).ok_or(STATUS_INVALID_PARAMETER)?;
        let read_only = {
            let o = self.open(c, 16)?;
            o.share
        };
        if self.cfg.shares[read_only].read_only {
            return Err(STATUS_MEDIA_WRITE_PROTECTED);
        }
        let o = self.open(c, 16)?;
        let f = o.file.as_ref().ok_or(STATUS_ACCESS_DENIED)?;
        if offset == u64::MAX {
            offset = f.metadata().map(|m| m.len()).unwrap_or(0);
        }
        write_all_at(f, data, offset).map_err(|e| from_io(&e))?;
        let mut w = W::new();
        w.u16(17).u16(0).u32(len as u32).u32(0).u16(0).u16(0);
        ok(w.0)
    }

    fn ioctl(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let code = u32_at(r, HDR + 4);
        let in_off = u32_at(r, HDR + 24) as usize;
        let in_len = u32_at(r, HDR + 28) as usize;
        let fid = slice(r, HDR + 8, 16).ok_or(STATUS_INVALID_PARAMETER)?.to_vec();
        let output = match code {
            FSCTL_VALIDATE_NEGOTIATE_INFO => {
                let input = slice(r, in_off, in_len).ok_or(STATUS_INVALID_PARAMETER)?;
                let caps = u32_at(input, 0);
                let secmode = u16_at(input, 20);
                if caps != self.client_caps || input[4..20] != self.client_guid || secmode != self.client_secmode {
                    self.log(false, "validate negotiate: mismatch, dropping");
                    return Err(STATUS_ACCESS_DENIED);
                }
                let mut w = W::new();
                w.u32(if self.dialect >= 0x0210 { CAP_LARGE_MTU } else { 0 }).bytes(&self.server_guid);
                w.u16(0x01 | 0x02).u16(self.dialect);
                w.0
            }
            FSCTL_DFS_GET_REFERRALS | FSCTL_DFS_GET_REFERRALS_EX => return Err(STATUS_FS_DRIVER_REQUIRED),
            FSCTL_GET_REPARSE_POINT => return Err(STATUS_NOT_A_REPARSE_POINT),
            _ => {
                self.log(true, &format!("IOCTL {:#010x} not supported", code));
                return Err(STATUS_NOT_SUPPORTED);
            }
        };
        let mut w = W::new();
        w.u16(49).u16(0).u32(code).bytes(&fid);
        let out_at = (HDR + 48) as u32;
        w.u32(out_at).u32(0).u32(out_at).u32(output.len() as u32).u32(0).u32(0);
        w.bytes(&output);
        ok(w.0)
    }

    fn query_directory(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let class = r.get(HDR + 2).copied().unwrap_or(0);
        let flags = r.get(HDR + 3).copied().unwrap_or(0);
        let noff = u16_at(r, HDR + 24) as usize;
        let nlen = u16_at(r, HDR + 26) as usize;
        let max = u32_at(r, HDR + 28) as usize;
        let pattern = from_utf16(slice(r, noff, nlen).unwrap_or(&[]));
        let pattern = if pattern.is_empty() { "*".to_string() } else { pattern };
        let o = self.open(c, 8)?;
        if !o.is_dir {
            return Err(STATUS_INVALID_PARAMETER);
        }
        let fresh = flags & (0x01 | 0x10) != 0 || o.listing.as_ref().is_none_or(|l| l.2 != pattern && flags & 0x10 != 0);
        if fresh || o.listing.is_none() {
            o.listing = Some((list(&o.path, &o.rel, &pattern)?, 0, pattern.clone()));
        }
        let (entries, pos, _) = o.listing.as_mut().unwrap();
        if *pos == 0 && entries.is_empty() {
            return Err(STATUS_NO_SUCH_FILE);
        }
        if *pos >= entries.len() {
            return Err(STATUS_NO_MORE_FILES);
        }
        let mut out = W::new();
        let mut prev: Option<usize> = None;
        while *pos < entries.len() {
            let e = hostfs::dir_entry(class, *pos as u32, &entries[*pos]).ok_or(STATUS_INVALID_INFO_CLASS)?;
            let start = (out.len() + 7) & !7;
            if start + e.len() > max {
                if prev.is_none() {
                    return Err(STATUS_BUFFER_TOO_SMALL);
                }
                break;
            }
            out.align(8);
            if let Some(p) = prev {
                out.put_u32(p, (start - p) as u32);
            }
            prev = Some(start);
            out.bytes(&e);
            *pos += 1;
            if flags & 0x02 != 0 {
                break;
            }
        }
        let mut w = W::new();
        w.u16(9).u16((HDR + 8) as u16).u32(out.len() as u32).bytes(&out.0);
        ok(w.0)
    }

    fn query_info(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let ty = r.get(HDR + 2).copied().unwrap_or(0);
        let class = r.get(HDR + 3).copied().unwrap_or(0);
        let max = u32_at(r, HDR + 4) as usize;
        let additional = u32_at(r, HDR + 16);
        let id = self.file_id(c, 24)?;
        let o = &self.opens[&id];
        let sh = &self.cfg.shares[o.share];
        let meta = fs::metadata(&o.path).map_err(|e| from_io(&e))?;
        let leaf = o.path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let i = hostfs::info(&leaf, &meta);
        let mut w = W::new();
        let mut variable = false;
        match (ty, class) {
            (1, 4) => i.basic(&mut w),
            (1, 5) => i.standard(&mut w, o.delete_on_close),
            (1, 6) => {
                w.u64(i.id);
            }
            (1, 7) => {
                w.u32(0);
            }
            (1, 8) => {
                w.u32(o.access);
            }
            (1, 14) => {
                w.u64(0);
            }
            (1, 16) => {
                w.u32(0);
            }
            (1, 17) => {
                w.u32(0);
            }
            (1, 9) | (1, 48) => {
                let n = utf16(&format!("\\{}", o.rel.trim_start_matches('\\')));
                w.u32(n.len() as u32).bytes(&n);
                variable = true;
            }
            (1, 18) => {
                i.basic(&mut w);
                i.standard(&mut w, o.delete_on_close);
                w.u64(i.id).u32(0).u32(o.access).u64(0).u32(0).u32(0);
                let n = utf16(&format!("\\{}", o.rel.trim_start_matches('\\')));
                w.u32(n.len() as u32).bytes(&n);
                variable = true;
            }
            (1, 22) => {
                if !i.is_dir {
                    let n = utf16("::$DATA");
                    w.u32(0).u32(n.len() as u32).u64(i.size).u64(i.alloc).bytes(&n);
                }
                variable = true;
            }
            (1, 28) => {
                w.u64(i.size).u16(0).u8(0).u8(0).u8(0).zeros(3);
            }
            (1, 34) => i.network_open(&mut w),
            (1, 35) => {
                w.u32(i.attrs).u32(0);
            }
            (1, 59) => {
                w.u64(volume_serial(&sh.name) as u64).u64(i.id).u64(0);
            }
            (2, 1) => {
                let label = utf16(&sh.name);
                w.u64(0).u32(volume_serial(&sh.name)).u32(label.len() as u32).u8(0).u8(0).bytes(&label);
                variable = true;
            }
            (2, 3) | (2, 7) => {
                let (total, avail) = hostfs::space(&sh.path);
                let unit = 4096u64;
                w.u64(total / unit).u64(avail / unit);
                if class == 7 {
                    w.u64(avail / unit);
                }
                w.u32(8).u32(512);
            }
            (2, 4) => {
                w.u32(7).u32(0x20);
            }
            (2, 5) => {
                let fsname = utf16("NTFS");
                // case-preserving, Unicode names
                w.u32(0x02 | 0x04).u32(255).u32(fsname.len() as u32).bytes(&fsname);
                variable = true;
            }
            (2, 11) => {
                w.u32(512).u32(512).u32(4096).u32(512).u32(0x3).u32(0).u32(0);
            }
            (3, _) => {
                w.bytes(&hostfs::security_descriptor(additional, i.is_dir));
                if w.len() > max {
                    // the client asks again with the size it needs
                    let mut e = W::new();
                    e.u16(9).u8(0).u8(0).u32(4).u32(w.len() as u32);
                    return with(STATUS_BUFFER_TOO_SMALL, e.0);
                }
            }
            _ => {
                self.log(true, &format!("QUERY_INFO {}/{} not supported", ty, class));
                return Err(if ty == 1 || ty == 2 { STATUS_INVALID_INFO_CLASS } else { STATUS_NOT_SUPPORTED });
            }
        }
        let mut status = STATUS_SUCCESS;
        if w.len() > max {
            if !variable {
                return Err(STATUS_INFO_LENGTH_MISMATCH);
            }
            w.0.truncate(max);
            status = STATUS_BUFFER_OVERFLOW;
        }
        let mut out = W::new();
        out.u16(9).u16((HDR + 8) as u16).u32(w.len() as u32).bytes(&w.0);
        with(status, out.0)
    }

    fn set_info(&mut self, c: &Ctx) -> Result<Reply, u32> {
        let r = c.req;
        let ty = r.get(HDR + 2).copied().unwrap_or(0);
        let class = r.get(HDR + 3).copied().unwrap_or(0);
        let blen = u32_at(r, HDR + 4) as usize;
        let boff = u16_at(r, HDR + 8) as usize;
        let buf = slice(r, boff, blen).ok_or(STATUS_INVALID_PARAMETER)?.to_vec();
        let id = self.file_id(c, 16)?;
        let share = self.opens[&id].share;
        let sh = self.cfg.shares[share].clone();
        if sh.read_only {
            return Err(STATUS_MEDIA_WRITE_PROTECTED);
        }
        let o = self.opens.get_mut(&id).unwrap();
        match (ty, class) {
            (1, 4) => {
                // times, then the read-only attribute
                let mtime = crate::wire::from_filetime(u64_at(&buf, 16));
                let atime = crate::wire::from_filetime(u64_at(&buf, 8));
                if mtime.is_some() || atime.is_some() {
                    let m = fs::metadata(&o.path).map_err(|e| from_io(&e))?;
                    let mt = mtime.map(filetime::FileTime::from_system_time).unwrap_or_else(|| filetime::FileTime::from_last_modification_time(&m));
                    let at = atime.map(filetime::FileTime::from_system_time).unwrap_or_else(|| filetime::FileTime::from_last_access_time(&m));
                    filetime::set_file_times(&o.path, at, mt).map_err(|e| from_io(&e))?;
                }
                let attrs = u32_at(&buf, 32);
                if attrs != 0 && !o.is_dir {
                    let mut p = fs::metadata(&o.path).map_err(|e| from_io(&e))?.permissions();
                    let ro = attrs & hostfs::ATTR_READONLY != 0;
                    if p.readonly() != ro {
                        #[allow(clippy::permissions_set_readonly_false)]
                        p.set_readonly(ro);
                        fs::set_permissions(&o.path, p).map_err(|e| from_io(&e))?;
                    }
                }
            }
            (1, 10) => {
                let replace = buf.first().copied().unwrap_or(0) != 0;
                let nlen = u32_at(&buf, 16) as usize;
                let target = from_utf16(slice(&buf, 20, nlen).ok_or(STATUS_INVALID_PARAMETER)?);
                let to = hostfs::resolve(&sh.path, &target)?;
                if to != o.path && fs::symlink_metadata(&to).is_ok() {
                    // a case-only rename finds the file itself on a case-insensitive host
                    let same = fs::canonicalize(&to).ok() == fs::canonicalize(&o.path).ok();
                    if !replace && !same {
                        return Err(STATUS_OBJECT_NAME_COLLISION);
                    }
                }
                let to = if fs::canonicalize(&to).ok() == fs::canonicalize(&o.path).ok() {
                    o.path.with_file_name(target.rsplit('\\').next().unwrap_or(""))
                } else {
                    to
                };
                fs::rename(&o.path, &to).map_err(|e| from_io(&e))?;
                self.cfg.log(false, &format!("renamed {:?} -> {:?}", o.rel, target));
                o.path = to;
                o.rel = target;
            }
            (1, 13) | (1, 64) => {
                let delete = if class == 13 { buf.first().copied().unwrap_or(0) != 0 } else { u32_at(&buf, 0) & 1 != 0 };
                if delete && o.is_dir && fs::read_dir(&o.path).map(|mut d| d.next().is_some()).unwrap_or(false) {
                    return Err(STATUS_DIRECTORY_NOT_EMPTY);
                }
                o.delete_on_close = delete;
            }
            (1, 19) => {}
            (1, 20) => {
                let f = o.file.as_ref().ok_or(STATUS_ACCESS_DENIED)?;
                f.set_len(u64_at(&buf, 0)).map_err(|e| from_io(&e))?;
            }
            (1, 14) | (1, 16) | (3, _) => {}
            _ => {
                self.log(true, &format!("SET_INFO {}/{} not supported", ty, class));
                return Err(STATUS_NOT_SUPPORTED);
            }
        }
        ok(vec![2, 0])
    }
}

struct Ctx<'a> {
    req: &'a [u8],
    session_id: u64,
    tree_id: u32,
    last_file: Option<[u8; 16]>,
}

/// A handle's end: its pending delete happens now.
fn finish(cfg: &Config, o: Open) {
    drop(o.file);
    if o.delete_on_close {
        let r = if o.is_dir { fs::remove_dir(&o.path) } else { fs::remove_file(&o.path) };
        match r {
            Ok(()) => cfg.log(false, &format!("deleted {:?}", o.rel)),
            Err(e) => cfg.log(false, &format!("delete {:?} failed: {}", o.rel, e)),
        }
    }
}

fn list(dir: &std::path::Path, rel: &str, pattern: &str) -> Result<Vec<Entry>, u32> {
    let mut out = Vec::new();
    for (n, p) in [(".", dir.to_path_buf()), ("..", dir.parent().map(|p| p.to_path_buf()).unwrap_or(dir.to_path_buf()))] {
        let p = if n == ".." && rel.trim_matches('\\').is_empty() { dir.to_path_buf() } else { p };
        if hostfs::wildcard(pattern, n) {
            if let Ok(m) = fs::metadata(&p) {
                out.push(Entry { name: n.to_string(), info: hostfs::info(n, &m) });
            }
        }
    }
    let mut rest = Vec::new();
    for e in fs::read_dir(dir).map_err(|e| from_io(&e))?.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if hostfs::components(&name).map(|c| c.len() != 1).unwrap_or(true) || !hostfs::wildcard(pattern, &name) {
            continue;
        }
        // follows symlinks; one that dangles is left out
        let Ok(m) = fs::metadata(e.path()) else { continue };
        rest.push(Entry { info: hostfs::info(&name, &m), name });
    }
    rest.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    out.extend(rest);
    Ok(out)
}

fn volume_serial(name: &str) -> u32 {
    name.bytes().fold(0x2b5d_c0de_u32, |h, b| h.rotate_left(5) ^ u32::from(b))
}

#[cfg(unix)]
fn read_at(f: &File, buf: &mut [u8], off: u64) -> io::Result<usize> {
    std::os::unix::fs::FileExt::read_at(f, buf, off)
}

#[cfg(windows)]
fn read_at(f: &File, buf: &mut [u8], off: u64) -> io::Result<usize> {
    std::os::windows::fs::FileExt::seek_read(f, buf, off)
}

#[cfg(unix)]
fn write_all_at(f: &File, buf: &[u8], off: u64) -> io::Result<()> {
    std::os::unix::fs::FileExt::write_all_at(f, buf, off)
}

#[cfg(windows)]
fn write_all_at(f: &File, mut buf: &[u8], mut off: u64) -> io::Result<()> {
    while !buf.is_empty() {
        let n = std::os::windows::fs::FileExt::seek_write(f, buf, off)?;
        buf = &buf[n..];
        off += n as u64;
    }
    Ok(())
}

