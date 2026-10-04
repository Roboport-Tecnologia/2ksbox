//! The NTLMSSP acceptor (MS-NLMP): CHALLENGE out, AUTHENTICATE in,
//! NTLMv2 checked against the one account, and the session key that
//! signs SMB. Plus NTLM's own signature, for SPNEGO's mechListMIC.

use crate::wire::{u16_at, u32_at, utf16, W};
use hmac::{Hmac, Mac};
use md4::{Digest, Md4};
use md5::Md5;

const NEGOTIATE_UNICODE: u32 = 0x0000_0001;
const REQUEST_TARGET: u32 = 0x0000_0004;
const NEGOTIATE_SIGN: u32 = 0x0000_0010;
const NEGOTIATE_SEAL: u32 = 0x0000_0020;
const NEGOTIATE_NTLM: u32 = 0x0000_0200;
const ALWAYS_SIGN: u32 = 0x0000_8000;
const TARGET_TYPE_SERVER: u32 = 0x0002_0000;
const EXTENDED_SESSIONSECURITY: u32 = 0x0008_0000;
const NEGOTIATE_TARGET_INFO: u32 = 0x0080_0000;
const NEGOTIATE_VERSION: u32 = 0x0200_0000;
const NEGOTIATE_128: u32 = 0x2000_0000;
const NEGOTIATE_KEY_EXCH: u32 = 0x4000_0000;
const NEGOTIATE_56: u32 = 0x8000_0000;

/// What this acceptor agrees to of a client's NEGOTIATE flags.
const OFFERED: u32 = NEGOTIATE_UNICODE
    | REQUEST_TARGET
    | NEGOTIATE_SIGN
    | NEGOTIATE_SEAL
    | NEGOTIATE_NTLM
    | ALWAYS_SIGN
    | EXTENDED_SESSIONSECURITY
    | NEGOTIATE_VERSION
    | NEGOTIATE_128
    | NEGOTIATE_KEY_EXCH
    | NEGOTIATE_56;

pub const SIGNATURE: &[u8] = b"NTLMSSP\0";

type HmacMd5 = Hmac<Md5>;

fn hmac_md5(key: &[u8], parts: &[&[u8]]) -> [u8; 16] {
    let mut m = <HmacMd5 as Mac>::new_from_slice(key).unwrap();
    for p in parts {
        m.update(p);
    }
    m.finalize().into_bytes().into()
}

fn md5(parts: &[&[u8]]) -> [u8; 16] {
    let mut h = Md5::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

/// RC4, which NTLM still uses for the key exchange and its signatures.
pub struct Rc4 {
    s: [u8; 256],
    i: u8,
    j: u8,
}

impl Rc4 {
    pub fn new(key: &[u8]) -> Rc4 {
        let mut s = [0u8; 256];
        for (i, v) in s.iter_mut().enumerate() {
            *v = i as u8;
        }
        let mut j = 0u8;
        for i in 0..256 {
            j = j.wrapping_add(s[i]).wrapping_add(key[i % key.len()]);
            s.swap(i, j as usize);
        }
        Rc4 { s, i: 0, j: 0 }
    }
    pub fn apply(&mut self, data: &mut [u8]) {
        for b in data {
            self.i = self.i.wrapping_add(1);
            self.j = self.j.wrapping_add(self.s[self.i as usize]);
            self.s.swap(self.i as usize, self.j as usize);
            let k = self.s[(self.s[self.i as usize].wrapping_add(self.s[self.j as usize])) as usize];
            *b ^= k;
        }
    }
}

/// One login in progress.
pub struct Acceptor {
    flags: u32,
    challenge: [u8; 8],
}

/// A login that succeeded.
pub struct Authenticated {
    pub user: String,
    pub domain: String,
    /// ExportedSessionKey: SMB's session key.
    pub session_key: [u8; 16],
    flags: u32,
}

#[derive(Debug)]
#[allow(dead_code)] // the names are for the log
pub enum AuthError {
    Malformed,
    Anonymous,
    UnknownUser(String),
    BadPassword(String),
}

fn field<'a>(msg: &'a [u8], at: usize) -> Option<&'a [u8]> {
    let len = u16_at(msg, at) as usize;
    let off = u32_at(msg, at + 4) as usize;
    if len == 0 {
        return Some(&[]);
    }
    msg.get(off..off.checked_add(len)?)
}

fn av(w: &mut W, id: u16, v: &[u8]) {
    w.u16(id).u16(v.len() as u16).bytes(v);
}

impl Acceptor {
    /// A client's NEGOTIATE_MESSAGE in, the CHALLENGE_MESSAGE to answer it.
    pub fn challenge(negotiate: &[u8], server_name: &str) -> Option<(Acceptor, Vec<u8>)> {
        if !negotiate.starts_with(SIGNATURE) || u32_at(negotiate, 8) != 1 {
            return None;
        }
        let client = u32_at(negotiate, 12);
        let flags = (client & OFFERED) | TARGET_TYPE_SERVER | NEGOTIATE_TARGET_INFO | NEGOTIATE_UNICODE;
        let challenge = crate::wire::random::<8>();

        let name = utf16(&server_name.to_ascii_uppercase());
        let dns = utf16(&server_name.to_ascii_lowercase());
        let mut info = W::new();
        av(&mut info, 2, &name); // MsvAvNbDomainName
        av(&mut info, 1, &name); // MsvAvNbComputerName
        av(&mut info, 4, &dns); // MsvAvDnsDomainName
        av(&mut info, 3, &dns); // MsvAvDnsComputerName
        av(&mut info, 7, &crate::wire::now().to_le_bytes()); // MsvAvTimestamp
        av(&mut info, 0, &[]); // MsvAvEOL

        let head = 56u32;
        let mut w = W::new();
        w.bytes(SIGNATURE).u32(2);
        w.u16(name.len() as u16).u16(name.len() as u16).u32(head);
        w.u32(flags).bytes(&challenge).zeros(8);
        w.u16(info.len() as u16).u16(info.len() as u16).u32(head + name.len() as u32);
        // Version: 10.0, build 0, NTLMSSP_REVISION_W2K3
        w.bytes(&[10, 0, 0, 0, 0, 0, 0, 15]);
        w.bytes(&name).bytes(&info.0);
        Some((Acceptor { flags, challenge }, w.0))
    }

    /// A client's AUTHENTICATE_MESSAGE, checked with NTLMv2 against the
    /// one account (the user name compared case-insensitively).
    pub fn authenticate(&self, msg: &[u8], user: &str, password: &str) -> Result<Authenticated, AuthError> {
        if !msg.starts_with(SIGNATURE) || u32_at(msg, 8) != 3 {
            return Err(AuthError::Malformed);
        }
        let nt = field(msg, 20).ok_or(AuthError::Malformed)?;
        let domain = field(msg, 28).ok_or(AuthError::Malformed)?;
        let name = field(msg, 36).ok_or(AuthError::Malformed)?;
        let enc_key = field(msg, 52).ok_or(AuthError::Malformed)?;
        let flags = u32_at(msg, 60);
        let (name_s, domain_s) = (crate::wire::from_utf16(name), crate::wire::from_utf16(domain));
        if nt.is_empty() && name.is_empty() {
            return Err(AuthError::Anonymous);
        }
        if !name_s.eq_ignore_ascii_case(user) {
            return Err(AuthError::UnknownUser(name_s));
        }
        // NTLMv2 only: a 16-byte NTProofStr, then the client's blob
        if nt.len() < 16 + 28 {
            return Err(AuthError::Malformed);
        }
        let nt_hash: [u8; 16] = Md4::digest(utf16(password)).into();
        let who = utf16(&format!("{}{}", name_s.to_uppercase(), domain_s));
        let owf = hmac_md5(&nt_hash, &[&who]);
        let (proof, blob) = nt.split_at(16);
        let expect = hmac_md5(&owf, &[&self.challenge, blob]);
        if expect != proof {
            return Err(AuthError::BadPassword(name_s));
        }
        let base = hmac_md5(&owf, &[proof]);
        let mut session_key = base;
        if self.flags & flags & NEGOTIATE_KEY_EXCH != 0 && enc_key.len() == 16 {
            session_key.copy_from_slice(enc_key);
            Rc4::new(&base).apply(&mut session_key);
        }
        Ok(Authenticated { user: name_s, domain: domain_s, session_key, flags: self.flags & flags })
    }
}

impl Authenticated {
    /// The acceptor's NTLM signature over `data` with sequence number 0,
    /// which is what SPNEGO's mechListMIC is (MS-NLMP 3.4.4.2).
    pub fn server_mic(&self, data: &[u8]) -> [u8; 16] {
        let sign = md5(&[&self.session_key, b"session key to server-to-client signing key magic constant\0"]);
        let seal = md5(&[&self.session_key, b"session key to server-to-client sealing key magic constant\0"]);
        let seq = 0u32.to_le_bytes();
        let mut sum = [0u8; 8];
        sum.copy_from_slice(&hmac_md5(&sign, &[&seq, data])[..8]);
        if self.flags & NEGOTIATE_KEY_EXCH != 0 {
            Rc4::new(&seal).apply(&mut sum);
        }
        let mut out = [0u8; 16];
        out[..4].copy_from_slice(&1u32.to_le_bytes());
        out[4..12].copy_from_slice(&sum);
        out[12..].copy_from_slice(&seq);
        out
    }
}
