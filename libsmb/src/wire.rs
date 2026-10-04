//! Little-endian reading and writing, UTF-16 names and FILETIMEs.

use std::time::{SystemTime, UNIX_EPOCH};

pub fn u16_at(b: &[u8], o: usize) -> u16 {
    b.get(o..o + 2).map_or(0, |s| u16::from_le_bytes([s[0], s[1]]))
}

pub fn u32_at(b: &[u8], o: usize) -> u32 {
    b.get(o..o + 4)
        .map_or(0, |s| u32::from_le_bytes(s.try_into().unwrap()))
}

pub fn u64_at(b: &[u8], o: usize) -> u64 {
    b.get(o..o + 8)
        .map_or(0, |s| u64::from_le_bytes(s.try_into().unwrap()))
}

/// `len` bytes at `off`, or `None` when the buffer is short.
pub fn slice(b: &[u8], off: usize, len: usize) -> Option<&[u8]> {
    b.get(off..off.checked_add(len)?)
}

/// A growing little-endian buffer.
#[derive(Default)]
pub struct W(pub Vec<u8>);

impl W {
    pub fn new() -> W {
        W(Vec::new())
    }
    pub fn u8(&mut self, v: u8) -> &mut W {
        self.0.push(v);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut W {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut W {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut W {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn bytes(&mut self, v: &[u8]) -> &mut W {
        self.0.extend_from_slice(v);
        self
    }
    pub fn zeros(&mut self, n: usize) -> &mut W {
        self.0.resize(self.0.len() + n, 0);
        self
    }
    pub fn align(&mut self, a: usize) -> &mut W {
        while self.0.len() % a != 0 {
            self.0.push(0);
        }
        self
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn put_u32(&mut self, at: usize, v: u32) {
        self.0[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
}

pub fn utf16(s: &str) -> Vec<u8> {
    s.encode_utf16().flat_map(|c| c.to_le_bytes()).collect()
}

pub fn from_utf16(b: &[u8]) -> String {
    let units: Vec<u16> = b
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .collect();
    String::from_utf16_lossy(&units)
}

/// 100 ns ticks since 1601, what every SMB time is.
pub fn filetime(t: SystemTime) -> u64 {
    const EPOCH_1601: u64 = 11_644_473_600;
    match t.duration_since(UNIX_EPOCH) {
        Ok(d) => (d.as_secs() + EPOCH_1601) * 10_000_000 + u64::from(d.subsec_nanos()) / 100,
        Err(_) => 0,
    }
}

pub fn from_filetime(ft: u64) -> Option<SystemTime> {
    const EPOCH_1601: u64 = 11_644_473_600 * 10_000_000;
    // 0 and -1 (and -2) mean "leave this time alone"
    if ft == 0 || ft >= u64::MAX - 1 || ft < EPOCH_1601 {
        return None;
    }
    let t = ft - EPOCH_1601;
    Some(UNIX_EPOCH + std::time::Duration::new(t / 10_000_000, (t % 10_000_000) as u32 * 100))
}

pub fn now() -> u64 {
    filetime(SystemTime::now())
}

pub fn random<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    getrandom::fill(&mut b).expect("getrandom");
    b
}
