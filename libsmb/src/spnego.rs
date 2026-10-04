//! SPNEGO (RFC 4178) as far as an NTLM-only acceptor needs it: the
//! NegTokenInit2 hint in NEGOTIATE, the client's NegTokenInit, and the
//! NegTokenResp answers. Hand-rolled DER, definite lengths only.

pub const OID_SPNEGO: &[u8] = &[0x2b, 0x06, 0x01, 0x05, 0x05, 0x02];
pub const OID_NTLMSSP: &[u8] = &[0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a];

/// One DER element: its tag, its contents, and everything after it.
fn tlv(b: &[u8]) -> Option<(u8, &[u8], &[u8])> {
    let tag = *b.first()?;
    let first = *b.get(1)? as usize;
    let (len, head) = if first < 0x80 {
        (first, 2)
    } else {
        let n = first & 0x7f;
        if n == 0 || n > 4 {
            return None;
        }
        let mut len = 0usize;
        for i in 0..n {
            len = (len << 8) | *b.get(2 + i)? as usize;
        }
        (len, 2 + n)
    };
    let body = b.get(head..head.checked_add(len)?)?;
    Some((tag, body, &b[head + len..]))
}

fn enc(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut v = vec![tag];
    let n = body.len();
    if n < 0x80 {
        v.push(n as u8);
    } else if n < 0x100 {
        v.extend_from_slice(&[0x81, n as u8]);
    } else if n < 0x10000 {
        v.extend_from_slice(&[0x82, (n >> 8) as u8, n as u8]);
    } else {
        v.extend_from_slice(&[0x83, (n >> 16) as u8, (n >> 8) as u8, n as u8]);
    }
    v.extend_from_slice(body);
    v
}

fn oid(o: &[u8]) -> Vec<u8> {
    enc(0x06, o)
}

/// What a client's token carried.
pub struct Init<'a> {
    /// The mechToken (an NTLMSSP message), if any.
    pub token: Option<&'a [u8]>,
    /// The whole DER MechTypeList, which the mechListMIC covers.
    pub mech_types: Option<&'a [u8]>,
    /// A NegTokenResp's mechListMIC.
    pub mic: Option<&'a [u8]>,
    /// The client's first mech is NTLMSSP (or the token was a reply).
    pub ntlm_ok: bool,
}

/// A client's SESSION_SETUP blob: a GSS-wrapped NegTokenInit, a
/// NegTokenResp, or (from older clients) raw NTLMSSP. `None` for
/// anything else.
pub fn parse(blob: &[u8]) -> Option<Init<'_>> {
    if blob.starts_with(crate::ntlm::SIGNATURE) {
        return Some(Init { token: Some(blob), mech_types: None, mic: None, ntlm_ok: true });
    }
    let (tag, body, _) = tlv(blob)?;
    match tag {
        // [APPLICATION 0] { thisMech OID, [0] NegTokenInit }
        0x60 => {
            let (t, o, rest) = tlv(body)?;
            if t != 0x06 || o != OID_SPNEGO {
                return None;
            }
            let (t, init, _) = tlv(rest)?;
            if t != 0xa0 {
                return None;
            }
            let (t, mut seq, _) = tlv(init)?;
            if t != 0x30 {
                return None;
            }
            let mut out = Init { token: None, mech_types: None, mic: None, ntlm_ok: false };
            while let Some((t, v, rest)) = tlv(seq) {
                match t {
                    0xa0 => {
                        out.mech_types = Some(v);
                        let (_, list, _) = tlv(v)?;
                        let mut l = list;
                        let mut has = false;
                        while let Some((_, o, r)) = tlv(l) {
                            has |= o == OID_NTLMSSP;
                            l = r;
                        }
                        out.ntlm_ok = has;
                    }
                    0xa2 => out.token = Some(tlv(v)?.1),
                    _ => {}
                }
                seq = rest;
            }
            Some(out)
        }
        // [1] NegTokenResp
        0xa1 => {
            let (t, mut seq, _) = tlv(body)?;
            if t != 0x30 {
                return None;
            }
            let mut out = Init { token: None, mech_types: None, mic: None, ntlm_ok: true };
            while let Some((t, v, rest)) = tlv(seq) {
                match t {
                    0xa2 => out.token = Some(tlv(v)?.1),
                    0xa3 => out.mic = Some(tlv(v)?.1),
                    _ => {}
                }
                seq = rest;
            }
            Some(out)
        }
        _ => None,
    }
}

/// NEGOTIATE's security buffer: a NegTokenInit2 offering NTLMSSP alone.
pub fn hint() -> Vec<u8> {
    let mechs = enc(0xa0, &enc(0x30, &oid(OID_NTLMSSP)));
    let hints = enc(0xa3, &enc(0x30, &enc(0xa0, &enc(0x1b, b"not_defined_in_RFC4178@please_ignore"))));
    let init = enc(0x30, &[mechs, hints].concat());
    enc(0x60, &[oid(OID_SPNEGO), enc(0xa0, &init)].concat())
}

/// accept-completed = 0, accept-incomplete = 1, reject = 2.
pub fn resp(state: u8, token: Option<&[u8]>, mic: Option<&[u8]>, with_mech: bool) -> Vec<u8> {
    let mut seq = enc(0xa0, &enc(0x0a, &[state]));
    if with_mech {
        seq.extend(enc(0xa1, &oid(OID_NTLMSSP)));
    }
    if let Some(t) = token {
        seq.extend(enc(0xa2, &enc(0x04, t)));
    }
    if let Some(m) = mic {
        seq.extend(enc(0xa3, &enc(0x04, m)));
    }
    enc(0xa1, &enc(0x30, &seq))
}
