//! DCE/RPC over the IPC$ named pipes, as far as Windows' shell needs it:
//! `srvsvc`'s share list and share information (MS-SRVS), and
//! `wkssvc`'s workstation information (MS-WKST). Without them Explorer
//! cannot list `\\server` and, on every open through a `\\server\share`
//! path, falls back to other ways of asking and waits each one out.
//!
//! Connection-oriented PDUs (C706 chapter 12) in little-endian NDR, no
//! authentication (the SMB session is the security), one presentation
//! context at a time.

use crate::wire::{u16_at, u32_at, utf16, W};

const BIND: u8 = 11;
const BIND_ACK: u8 = 12;
const ALTER_CONTEXT: u8 = 14;
const ALTER_CONTEXT_RESP: u8 = 15;
const REQUEST: u8 = 0;
const RESPONSE: u8 = 2;
const FAULT: u8 = 3;

const PFC_FIRST: u8 = 0x01;
const PFC_LAST: u8 = 0x02;

/// NDR's transfer syntax, 8a885d04-1ceb-11c9-9fe8-08002b104860 v2.
const NDR: [u8; 16] = [
    0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60,
];
/// Bind-time feature negotiation's syntax starts 6cb71c2c-9812-4540.
const BTFN_PREFIX: [u8; 8] = [0x2c, 0x1c, 0xb7, 0x6c, 0x12, 0x98, 0x40, 0x45];

/// srvsvc, 4b324fc8-1670-01d3-1278-5a47bf6ee188.
const SRVSVC: [u8; 16] = [
    0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88,
];
/// wkssvc, 6bffd098-a112-3610-9833-46c3f87e345a.
const WKSSVC: [u8; 16] = [
    0x98, 0xd0, 0xff, 0x6b, 0x12, 0xa1, 0x10, 0x36, 0x98, 0x33, 0x46, 0xc3, 0xf8, 0x7e, 0x34, 0x5a,
];

const NCA_S_OP_RNG_ERROR: u32 = 0x1c01_0002;
const NCA_S_UNKNOWN_IF: u32 = 0x1c01_0003;

const ERROR_ACCESS_DENIED: u32 = 5;
const ERROR_INVALID_LEVEL: u32 = 124;
const NERR_NET_NAME_NOT_FOUND: u32 = 2310;

/// What a pipe can answer for: the names a client may open on IPC$.
pub fn serves(name: &str) -> bool {
    let n = name.trim_start_matches('\\').to_ascii_lowercase();
    n == "srvsvc" || n == "wkssvc"
}

/// A share as the RPC calls describe it.
pub struct ShareDesc {
    pub name: String,
    pub remark: String,
    pub ipc: bool,
}

/// What the RPC calls need to know of the server.
pub struct Ctx<'a> {
    pub server_name: &'a str,
    pub shares: Vec<ShareDesc>,
}

/// One open pipe: PDUs written in, PDUs waiting to be read out.
pub struct Pipe {
    pub name: String,
    iface: Option<[u8; 16]>,
    max_frag: usize,
    partial: Vec<u8>,
    pub out: Vec<u8>,
}

impl Pipe {
    pub fn new(name: &str) -> Pipe {
        Pipe {
            name: name.trim_start_matches('\\').to_ascii_lowercase(),
            iface: None,
            max_frag: 4280,
            partial: Vec::new(),
            out: Vec::new(),
        }
    }

    /// Bytes the client wrote (one or more whole PDUs); the answers are
    /// appended to `out`. Returns a line for the log.
    pub fn write(&mut self, data: &[u8], ctx: &Ctx) -> String {
        let mut log = Vec::new();
        let mut rest = data;
        while rest.len() >= 16 {
            let len = (u16_at(rest, 8) as usize).min(rest.len());
            if len < 16 {
                break;
            }
            let pdu = &rest[..len];
            log.push(self.pdu(pdu, ctx));
            rest = &rest[len..];
        }
        log.join("; ")
    }

    fn pdu(&mut self, p: &[u8], ctx: &Ctx) -> String {
        let ptype = p[2];
        let flags = p[3];
        let call_id = u32_at(p, 12);
        match ptype {
            BIND | ALTER_CONTEXT => {
                self.max_frag = (u16_at(p, 18) as usize).clamp(1024, 65535);
                let n = p.get(24).copied().unwrap_or(0) as usize;
                let mut at = 28;
                let mut results = W::new();
                let mut accepted = None;
                for _ in 0..n {
                    let cont_id = u16_at(p, at);
                    let ntrans = p.get(at + 2).copied().unwrap_or(0) as usize;
                    let mut iface = [0u8; 16];
                    if let Some(s) = p.get(at + 4..at + 20) {
                        iface.copy_from_slice(s);
                    }
                    let mut res = (2u16, 2u16, [0u8; 20]); // provider rejection: syntax not supported
                    for t in 0..ntrans {
                        let ts = at + 24 + 20 * t;
                        let Some(syn) = p.get(ts..ts + 16) else { break };
                        if syn == NDR && u32_at(p, ts + 16) == 2 && accepted.is_none() {
                            let mut s = [0u8; 20];
                            s[..16].copy_from_slice(&NDR);
                            s[16..].copy_from_slice(&2u32.to_le_bytes());
                            res = (0, 0, s);
                            accepted = Some((cont_id, iface));
                            break;
                        }
                        if syn[..8] == BTFN_PREFIX {
                            res = (3, 0, [0u8; 20]); // negotiate ack, no features
                        }
                    }
                    results.u16(res.0).u16(res.1).bytes(&res.2);
                    at += 24 + 20 * ntrans;
                }
                let known = accepted.map(|(_, i)| i == SRVSVC || i == WKSSVC).unwrap_or(false);
                if let Some((_, iface)) = accepted {
                    self.iface = Some(iface);
                }
                let mut b = W::new();
                b.u16(self.max_frag as u16).u16(self.max_frag as u16).u32(0x5326);
                if ptype == BIND {
                    let port = format!("\\PIPE\\{}\0", self.name);
                    b.u16(port.len() as u16).bytes(port.as_bytes());
                } else {
                    b.u16(0);
                }
                b.align(4);
                b.u8(n as u8).u8(0).u16(0).bytes(&results.0);
                let reply = if ptype == BIND { BIND_ACK } else { ALTER_CONTEXT_RESP };
                self.emit(reply, PFC_FIRST | PFC_LAST, call_id, &b.0);
                format!("bind {} {}", self.name, if known { "accepted" } else { "unknown interface" })
            }
            REQUEST => {
                let opnum = u16_at(p, 22);
                let cont_id = u16_at(p, 20);
                let stub_at = if flags & 0x80 != 0 { 40 } else { 24 };
                self.partial.extend_from_slice(p.get(stub_at..).unwrap_or(&[]));
                if flags & PFC_LAST == 0 {
                    return format!("request op {} (fragment)", opnum);
                }
                let stub = std::mem::take(&mut self.partial);
                let answer = match self.iface {
                    Some(i) if i == SRVSVC => srvsvc(opnum, &stub, ctx),
                    Some(i) if i == WKSSVC => wkssvc(opnum, &stub, ctx),
                    _ => Err(NCA_S_UNKNOWN_IF),
                };
                match answer {
                    Ok(out) => {
                        // fragments of at most max_frag
                        let room = self.max_frag - 24;
                        let chunks: Vec<&[u8]> = if out.is_empty() { vec![&[][..]] } else { out.chunks(room).collect() };
                        for (i, c) in chunks.iter().enumerate() {
                            let mut f = 0;
                            if i == 0 {
                                f |= PFC_FIRST;
                            }
                            if i + 1 == chunks.len() {
                                f |= PFC_LAST;
                            }
                            let mut b = W::new();
                            b.u32(out.len() as u32).u16(cont_id).u8(0).u8(0).bytes(c);
                            self.emit(RESPONSE, f, call_id, &b.0);
                        }
                        format!("{} op {} answered ({} bytes)", self.name, opnum, out.len())
                    }
                    Err(status) => {
                        let mut b = W::new();
                        b.u32(0).u16(cont_id).u8(0).u8(0).u32(status).u32(0);
                        self.emit(FAULT, PFC_FIRST | PFC_LAST, call_id, &b.0);
                        format!("{} op {} faulted {:#x}", self.name, opnum, status)
                    }
                }
            }
            other => format!("pdu type {} ignored", other),
        }
    }

    fn emit(&mut self, ptype: u8, flags: u8, call_id: u32, body: &[u8]) {
        let mut w = W::new();
        w.u8(5).u8(0).u8(ptype).u8(flags).bytes(&[0x10, 0, 0, 0]);
        w.u16((16 + body.len()) as u16).u16(0).u32(call_id).bytes(body);
        self.out.extend_from_slice(&w.0);
    }
}

/// An NDR stub reader: 4-byte aligned reads from the stub's start.
struct R<'a> {
    b: &'a [u8],
    at: usize,
}

impl R<'_> {
    fn align(&mut self, a: usize) {
        self.at = (self.at + a - 1) & !(a - 1);
    }
    fn u32(&mut self) -> u32 {
        self.align(4);
        let v = u32_at(self.b, self.at);
        self.at += 4;
        v
    }
    /// A conformant varying string (max, offset, actual, UTF-16 units).
    fn string(&mut self) -> String {
        let _max = self.u32();
        let _off = self.u32();
        let n = self.u32() as usize;
        let s = crate::wire::from_utf16(self.b.get(self.at..self.at + 2 * n).unwrap_or(&[]));
        self.at += 2 * n;
        s.trim_end_matches('\0').to_string()
    }
    /// A unique pointer to a string: absent when its referent id is 0.
    fn opt_string(&mut self) -> Option<String> {
        if self.u32() == 0 {
            None
        } else {
            Some(self.string())
        }
    }
}

/// An NDR stub writer: pointers get increasing referent ids.
struct N {
    w: W,
    next_ref: u32,
}

impl N {
    fn new() -> N {
        N { w: W::new(), next_ref: 0x0002_0000 }
    }
    fn u32(&mut self, v: u32) {
        self.w.align(4);
        self.w.u32(v);
    }
    fn ptr(&mut self) {
        let r = self.next_ref;
        self.next_ref += 4;
        self.u32(r);
    }
    fn string(&mut self, s: &str) {
        let u = utf16(&format!("{}\0", s));
        let n = (u.len() / 2) as u32;
        self.u32(n);
        self.u32(0);
        self.u32(n);
        self.w.bytes(&u);
        self.w.align(4);
    }
}

const STYPE_DISKTREE: u32 = 0;
const STYPE_IPC_SPECIAL: u32 = 0x8000_0003;

fn share_type(s: &ShareDesc) -> u32 {
    if s.ipc {
        STYPE_IPC_SPECIAL
    } else {
        STYPE_DISKTREE
    }
}

fn srvsvc(opnum: u16, stub: &[u8], ctx: &Ctx) -> Result<Vec<u8>, u32> {
    let mut r = R { b: stub, at: 0 };
    let mut n = N::new();
    match opnum {
        // NetrShareEnum
        15 => {
            let _server = r.opt_string();
            let level = r.u32();
            n.u32(level);
            match level {
                0 | 1 => {
                    n.u32(level); // the union's switch
                    n.ptr(); // the container
                    n.u32(ctx.shares.len() as u32);
                    n.ptr(); // its array
                    n.u32(ctx.shares.len() as u32);
                    for s in &ctx.shares {
                        n.ptr();
                        if level == 1 {
                            n.u32(share_type(s));
                            n.ptr();
                        }
                    }
                    for s in &ctx.shares {
                        n.string(&s.name);
                        if level == 1 {
                            n.string(&s.remark);
                        }
                    }
                    n.u32(ctx.shares.len() as u32); // TotalEntries
                    n.u32(0); // no resume handle
                    n.u32(0);
                }
                _ => {
                    n.u32(level);
                    n.u32(0); // a null container
                    n.u32(0);
                    n.u32(0);
                    n.u32(ERROR_INVALID_LEVEL);
                }
            }
            Ok(n.w.0)
        }
        // NetrShareGetInfo
        16 => {
            let _server = r.opt_string();
            let name = r.string();
            let level = r.u32();
            let Some(s) = ctx.shares.iter().find(|s| s.name.eq_ignore_ascii_case(&name)) else {
                n.u32(level);
                n.u32(0);
                n.u32(NERR_NET_NAME_NOT_FOUND);
                return Ok(n.w.0);
            };
            n.u32(level);
            match level {
                0 => {
                    n.ptr();
                    n.ptr();
                    n.string(&s.name);
                }
                1 | 501 => {
                    n.ptr();
                    n.ptr();
                    n.u32(share_type(s));
                    n.ptr();
                    if level == 501 {
                        n.u32(0x30); // no client-side caching
                    }
                    n.string(&s.name);
                    n.string(&s.remark);
                }
                1005 => {
                    n.ptr();
                    n.u32(0x30); // SHI1005_FLAGS: no client-side caching
                }
                2 | 502 | 503 => {
                    n.u32(0);
                    n.u32(ERROR_ACCESS_DENIED);
                    return Ok(n.w.0);
                }
                _ => {
                    n.u32(0);
                    n.u32(ERROR_INVALID_LEVEL);
                    return Ok(n.w.0);
                }
            }
            n.u32(0);
            Ok(n.w.0)
        }
        // NetrServerGetInfo
        21 => {
            let _server = r.opt_string();
            let level = r.u32();
            n.u32(level);
            match level {
                100 | 101 => {
                    n.ptr();
                    n.u32(500); // PLATFORM_ID_NT
                    n.ptr();
                    if level == 101 {
                        n.u32(10);
                        n.u32(0);
                        n.u32(0x0000_1003); // workstation, server, NT
                        n.ptr();
                    }
                    n.string(&ctx.server_name.to_ascii_uppercase());
                    if level == 101 {
                        n.string("");
                    }
                    n.u32(0);
                }
                _ => {
                    n.u32(0);
                    n.u32(ERROR_ACCESS_DENIED);
                }
            }
            Ok(n.w.0)
        }
        _ => Err(NCA_S_OP_RNG_ERROR),
    }
}

fn wkssvc(opnum: u16, stub: &[u8], ctx: &Ctx) -> Result<Vec<u8>, u32> {
    let mut r = R { b: stub, at: 0 };
    let mut n = N::new();
    match opnum {
        // NetrWkstaGetInfo, level 100
        0 => {
            let _server = r.opt_string();
            let level = r.u32();
            n.u32(level);
            if level == 100 {
                n.ptr();
                n.u32(500);
                n.ptr();
                n.ptr();
                n.u32(10);
                n.u32(0);
                n.string(&ctx.server_name.to_ascii_uppercase());
                n.string("WORKGROUP");
                n.u32(0);
            } else {
                n.u32(0);
                n.u32(ERROR_ACCESS_DENIED);
            }
            Ok(n.w.0)
        }
        _ => Err(NCA_S_OP_RNG_ERROR),
    }
}
