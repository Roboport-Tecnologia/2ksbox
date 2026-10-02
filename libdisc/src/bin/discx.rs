//! discx: the libdisc exerciser (doc 17 §6.1).
//!
//!   discx selftest <outdir>            write synthetic images, check the model through them
//!   discx info <image>                 sessions, tracks, indices, extents
//!   discx scan <image> [first] [count] every sector classified and L-EC verified: kinds, failures, ranges
//!   discx subscan <image> [first] [count]  every sector's stored subchannel: Q CRC failures and how they
//!                                      cluster, and how often subq::synthesize reproduces the disc's own frames
//!   discx dump <image> <what> [args]   hex of one answer: readraw <lba> | readcooked <lba> | sub <lba> | info <lba>
//!                                      | toc <format> [msf] [start] | subq <lba> [format] [msf] [track] | discinfo
//!                                      | readcd <lba> <type> <byte9> <byte10>
//!   discx repair <image> <outdir>      a copy whose failing sectors verify: the negative control for a
//!                                      protection check (user data untouched, only EDC/ECC regenerated)
//!   discx convert <in.iso> <out.cue> [--audio a.wav ...]   cooked ISO → MODE1/2352 cue/bin (+ audio tracks)
//!   discx export <image> <out.iso>     the cooked 2048-byte view, sector by sector: what a folder disc
//!                                      (`isodir:<dir>`) looks like to an ISO 9660 reader
//!   discx mktree <dir>                 write the fixture tree the `dirdisc` check and scripts/test.sh share
//!
//! Every check prints PASS/FAIL with a name; the exit code is 1 on any FAIL.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use std::ffi::CString;

use libdisc::capi::{self, LibdiscSectorInfo, LibdiscTrackInfo};
use libdisc::msf::Msf;
use libdisc::{sector, subq, Disc, TrackMode};

/// A disc opened through the C API in `capi.rs`, the boundary QEMU's
/// block driver and atapi.c use; every check goes through it.
struct CDisc(*mut Disc);

impl CDisc {
    fn open(path: &Path) -> Result<CDisc, String> {
        let c = CString::new(path.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
        let mut err = [0i8; 256];
        let p = unsafe { capi::libdisc_open(c.as_ptr(), err.as_mut_ptr() as *mut _, err.len()) };
        if p.is_null() {
            let msg: Vec<u8> = err.iter().take_while(|&&b| b != 0).map(|&b| b as u8).collect();
            return Err(String::from_utf8_lossy(&msg).into_owned());
        }
        Ok(CDisc(p))
    }
    fn sector_count(&self) -> u32 {
        unsafe { capi::libdisc_sector_count(self.0) }
    }
    fn track_count(&self) -> u8 {
        unsafe { capi::libdisc_track_count(self.0) }
    }
    fn track_info(&self, n: u8) -> Result<LibdiscTrackInfo, i32> {
        let mut t = LibdiscTrackInfo::default();
        let rc = unsafe { capi::libdisc_track_info(self.0, n, &mut t) };
        if rc == 0 { Ok(t) } else { Err(rc) }
    }
    fn sector_info(&self, lba: u32) -> Result<LibdiscSectorInfo, i32> {
        let mut i = LibdiscSectorInfo::default();
        let rc = unsafe { capi::libdisc_sector_info(self.0, lba, &mut i) };
        if rc == 0 { Ok(i) } else { Err(rc) }
    }
    fn read_raw(&self, lba: u32) -> Result<[u8; 2352], i32> {
        let mut b = [0u8; 2352];
        let rc = unsafe { capi::libdisc_read_raw(self.0, lba, b.as_mut_ptr()) };
        if rc == 0 { Ok(b) } else { Err(rc) }
    }
    fn read_cooked(&self, lba: u32) -> Result<[u8; 2048], i32> {
        let mut b = [0u8; 2048];
        let rc = unsafe { capi::libdisc_read_cooked(self.0, lba, b.as_mut_ptr()) };
        if rc == 0 { Ok(b) } else { Err(rc) }
    }
    fn read_sub(&self, lba: u32) -> Result<[u8; 96], i32> {
        let mut b = [0u8; 96];
        let rc = unsafe { capi::libdisc_read_sub(self.0, lba, b.as_mut_ptr()) };
        if rc == 0 { Ok(b) } else { Err(rc) }
    }
    fn toc(&self, format: u8, msf: bool, start: u8) -> Result<Vec<u8>, i32> {
        let mut b = vec![0u8; 4096];
        let rc = unsafe { capi::libdisc_mmc_read_toc(self.0, format, msf as i32, start, b.as_mut_ptr(), b.len()) };
        if rc < 0 { Err(rc) } else { b.truncate(rc as usize); Ok(b) }
    }
    fn subchannel(&self, lba: u32, msf: bool, subq: bool, format: u8, track: u8, status: u8) -> Result<Vec<u8>, i32> {
        let mut b = vec![0u8; 64];
        let rc = unsafe { capi::libdisc_mmc_read_subchannel(self.0, lba, msf as i32, subq as i32, format, track, status, b.as_mut_ptr(), b.len()) };
        if rc < 0 { Err(rc) } else { b.truncate(rc as usize); Ok(b) }
    }
    fn disc_information(&self) -> Result<Vec<u8>, i32> {
        let mut b = vec![0u8; 64];
        let rc = unsafe { capi::libdisc_mmc_read_disc_information(self.0, b.as_mut_ptr(), b.len()) };
        if rc < 0 { Err(rc) } else { b.truncate(rc as usize); Ok(b) }
    }
    fn read_cd(&self, lba: u32, ty: u8, b9: u8, b10: u8) -> Result<Vec<u8>, i32> {
        let mut b = vec![0u8; 2744];
        let rc = unsafe { capi::libdisc_mmc_read_cd_sector(self.0, lba, ty, b9, b10, b.as_mut_ptr(), b.len()) };
        if rc < 0 { Err(rc) } else { b.truncate(rc as usize); Ok(b) }
    }
}

impl Drop for CDisc {
    fn drop(&mut self) {
        unsafe { capi::libdisc_close(self.0) }
    }
}

fn read_cd_length(ty: u8, b9: u8, b10: u8) -> i32 {
    capi::libdisc_mmc_read_cd_length(ty, b9, b10)
}

const DATA_SECTORS: i32 = 2000;
const T2_PREGAP: i32 = 150; // in the file (INDEX 00)
const T2_LEN: i32 = 3000;
const T3_PREGAP: i32 = 150; // PREGAP, not in the file
const T3_LEN: i32 = 1500;
const MCN: &str = "1234567890123";
const ISRC: &str = "USABC0912345";

/// stdout writes that survive a closed pipe (`discx dump … | head`)
macro_rules! outln {
    ($($arg:tt)*) => {{
        use std::io::Write;
        let _ = writeln!(std::io::stdout(), $($arg)*);
    }};
}
macro_rules! out {
    ($($arg:tt)*) => {{
        use std::io::Write;
        let _ = write!(std::io::stdout(), $($arg)*);
    }};
}

struct Checks {
    fails: u32,
    passes: u32,
}

impl Checks {
    fn report(&mut self, name: &str, r: Result<(), String>) {
        match r {
            Ok(()) => {
                self.passes += 1;
                println!("PASS {name}");
            }
            Err(e) => {
                self.fails += 1;
                println!("FAIL {name}: {e}");
            }
        }
    }
}

fn xorshift(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

/// The 2000 × 2048 user bytes of track 1, deterministic.
fn user_data() -> Vec<u8> {
    let mut s = 0x1234_5678u32;
    let mut v = vec![0u8; DATA_SECTORS as usize * 2048];
    for chunk in v.chunks_exact_mut(4) {
        chunk.copy_from_slice(&xorshift(&mut s).to_le_bytes());
    }
    // a recognisable primary volume descriptor at sector 16 so sniffing works
    let pvd = &mut v[16 * 2048..17 * 2048];
    pvd[0] = 1;
    pvd[1..6].copy_from_slice(b"CD001");
    pvd[6] = 1;
    v
}

/// `count` audio sectors of a `freq` Hz tone, phase continuous from `start_sector`.
fn tone(freq: f64, start_sector: i64, count: i32, out: &mut Vec<u8>) {
    for s in 0..count as i64 {
        for i in 0..588i64 {
            let n = (start_sector + s) * 588 + i;
            let v = (8000.0 * (2.0 * std::f64::consts::PI * freq * n as f64 / 44100.0).sin()) as i16;
            out.extend_from_slice(&v.to_le_bytes());
            out.extend_from_slice(&v.to_le_bytes());
        }
    }
}

fn msf_str(lba_file: i32) -> String {
    let m = Msf::from_lba(lba_file - libdisc::msf::MSF_OFFSET);
    format!("{:02}:{:02}:{:02}", m.m, m.s, m.f)
}

/// Write the synthetic images; returns the raw track-1 sectors (for the checks).
fn generate(dir: &Path) -> Result<Vec<u8>, String> {
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let data = user_data();
    let mut raw1 = vec![0u8; DATA_SECTORS as usize * 2352];
    for lba in 0..DATA_SECTORS {
        let mut sec = [0u8; 2352];
        sector::build_mode1(lba, &data[lba as usize * 2048..(lba as usize + 1) * 2048], &mut sec);
        raw1[lba as usize * 2352..(lba as usize + 1) * 2352].copy_from_slice(&sec);
    }
    let mut audio = Vec::with_capacity((T2_PREGAP + T2_LEN + T3_LEN) as usize * 2352);
    audio.resize(T2_PREGAP as usize * 2352, 0);
    tone(1000.0, 0, T2_LEN, &mut audio);
    tone(440.0, 0, T3_LEN, &mut audio);

    // 1. mixed.cue/.bin
    let mut bin = raw1.clone();
    bin.extend_from_slice(&audio);
    fs::write(dir.join("mixed.bin"), &bin).map_err(|e| e.to_string())?;
    let t2_i0 = DATA_SECTORS;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_file = t2_i1 + T2_LEN;
    let cue = format!(
        "CATALOG {MCN}\nFILE \"mixed.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    ISRC {ISRC}\n    INDEX 00 {}\n    INDEX 01 {}\n  TRACK 03 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n",
        msf_str(t2_i0),
        msf_str(t2_i1),
        msf_str(t3_file)
    );
    fs::write(dir.join("mixed.cue"), cue).map_err(|e| e.to_string())?;

    // 3. cooked.cue/.bin: track 1 as MODE1/2048 in the same file
    let mut cbin = data.clone();
    cbin.extend_from_slice(&audio);
    fs::write(dir.join("cooked.bin"), &cbin).map_err(|e| e.to_string())?;
    let cue = format!(
        "CATALOG {MCN}\nFILE \"cooked.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    ISRC {ISRC}\n    INDEX 00 {}\n    INDEX 01 {}\n  TRACK 03 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n",
        msf_str(t2_i0),
        msf_str(t2_i1),
        msf_str(t3_file)
    );
    fs::write(dir.join("cooked.cue"), cue).map_err(|e| e.to_string())?;

    // 4. plain.iso
    fs::write(dir.join("plain.iso"), &data).map_err(|e| e.to_string())?;

    // 5. tone.wav: 3 s of the 1 kHz tone as a WAVE, for `convert --audio`
    let mut wav = Vec::new();
    tone(1000.0, 0, 225, &mut wav);
    let mut riff = Vec::new();
    riff.extend_from_slice(b"RIFF");
    riff.extend_from_slice(&(36 + wav.len() as u32).to_le_bytes());
    riff.extend_from_slice(b"WAVEfmt ");
    riff.extend_from_slice(&16u32.to_le_bytes());
    riff.extend_from_slice(&1u16.to_le_bytes());
    riff.extend_from_slice(&2u16.to_le_bytes());
    riff.extend_from_slice(&44100u32.to_le_bytes());
    riff.extend_from_slice(&(44100u32 * 4).to_le_bytes());
    riff.extend_from_slice(&4u16.to_le_bytes());
    riff.extend_from_slice(&16u16.to_le_bytes());
    riff.extend_from_slice(b"data");
    riff.extend_from_slice(&(wav.len() as u32).to_le_bytes());
    riff.extend_from_slice(&wav);
    fs::write(dir.join("tone.wav"), &riff).map_err(|e| e.to_string())?;

    // 2. mixed.ccd/.img/.sub: the same disc as CloneCD, written from the model (checked by `ccd`)
    let disc = Disc::open(&dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let mut img = Vec::with_capacity(n as usize * 2352);
    let mut sub = Vec::with_capacity(n as usize * 96);
    for lba in 0..n {
        let mut r = [0u8; 2352];
        let mut s = [0u8; 96];
        disc.read_raw(lba, &mut r).map_err(|e| e.to_string())?;
        disc.read_sub(lba, &mut s).map_err(|e| e.to_string())?;
        img.extend_from_slice(&r);
        sub.extend_from_slice(&s);
    }
    fs::write(dir.join("mixed.img"), &img).map_err(|e| e.to_string())?;
    fs::write(dir.join("mixed.sub"), &sub).map_err(|e| e.to_string())?;
    let mut ccd = String::new();
    let session = &disc.sessions[0];
    let entries: Vec<(u8, u8, u8, i32)> = {
        let first = &session.tracks[0];
        let last = session.tracks.last().unwrap();
        let mut v = vec![
            (0xA0, 1, first.control, (first.number as i32) << 16), // PMIN = first track, PSEC = disc type 0
            (0xA1, 1, last.control, (last.number as i32) << 16),
            (0xA2, 1, last.control, session.leadout_lba),
        ];
        for t in &session.tracks {
            v.push((t.number, 1, t.control, t.start_lba));
        }
        v
    };
    ccd.push_str("[CloneCD]\nVersion=3\n[Disc]\n");
    ccd.push_str(&format!("TocEntries={}\nSessions=1\nDataTracksScrambled=0\nCDTextLength=0\nCATALOG={MCN}\n", entries.len()));
    ccd.push_str("[Session 1]\nPreGapMode=2\nPreGapSubC=1\n");
    for (i, &(point, adr, control, p)) in entries.iter().enumerate() {
        ccd.push_str(&format!("[Entry {i}]\nSession=1\nPoint=0x{point:02x}\nADR=0x{adr:02x}\nControl=0x{control:02x}\nTrackNo=0\nAMin=0\nASec=0\nAFrame=0\nALBA=-150\nZero=0\n"));
        if point >= 0xA0 && point != 0xA2 {
            ccd.push_str(&format!("PMin={}\nPSec={}\nPFrame=0\nPLBA=0\n", p >> 16, (p >> 8) & 0xFF));
        } else {
            let m = Msf::from_lba(p);
            ccd.push_str(&format!("PMin={}\nPSec={}\nPFrame={}\nPLBA={}\n", m.m, m.s, m.f, p));
        }
    }
    for t in &session.tracks {
        ccd.push_str(&format!("[TRACK {}]\nMODE={}\n", t.number, if t.mode.is_data() { 1 } else { 0 }));
        for &(i, lba) in &t.indices {
            ccd.push_str(&format!("INDEX {i}={lba}\n"));
        }
        if let Some(isrc) = t.isrc {
            ccd.push_str(&format!("ISRC={}\n", String::from_utf8_lossy(&isrc)));
        }
    }
    fs::write(dir.join("mixed.ccd"), ccd).map_err(|e| e.to_string())?;
    Ok(raw1)
}

fn expect<T: PartialEq + std::fmt::Debug>(what: &str, got: T, want: T) -> Result<(), String> {
    if got == want {
        Ok(())
    } else {
        Err(format!("{what}: got {got:?}, want {want:?}"))
    }
}

fn check_msf() -> Result<(), String> {
    expect("00:02:00", Msf { m: 0, s: 2, f: 0 }.to_lba(), 0)?;
    expect("lba 0", Msf::from_lba(0), Msf { m: 0, s: 2, f: 0 })?;
    for lba in [-150, -1, 0, 1, 74, 75, 4500, 333_000, 449_849] {
        expect(&format!("round trip {lba}"), Msf::from_lba(lba).to_lba(), lba)?;
    }
    expect("74:00:00", Msf { m: 74, s: 0, f: 0 }.to_lba(), 332_850)?;
    // Past either end an MSF saturates instead of failing: the openers
    // refuse a disc that long, and nothing inside QEMU may panic on an
    // address a guest chose (see `msf.rs`).
    expect("last addressable", Msf::from_lba(libdisc::msf::MAX_LBA), Msf { m: 99, s: 59, f: 74 })?;
    expect("past the end", Msf::from_lba(libdisc::msf::MAX_LBA + 1), Msf { m: 99, s: 59, f: 74 })?;
    expect("past the end, far", Msf::from_lba(i32::MAX), Msf { m: 99, s: 59, f: 74 })?;
    expect("before the lead-in", Msf::from_lba(-1000), Msf { m: 0, s: 0, f: 0 })?;
    expect("00:00:00", Msf { m: 0, s: 0, f: 0 }.to_lba(), -150)?;
    expect("bcd", sector::bcd(59), 0x59)?;
    expect("header lba 16", sector::header(16, 1), [0x00, 0x02, 0x16, 0x01])?;
    Ok(())
}

fn check_raw_synth(dir: &Path, raw1: &[u8]) -> Result<(), String> {
    let mixed = CDisc::open(&dir.join("mixed.cue"))?;
    let cooked = CDisc::open(&dir.join("cooked.cue"))?;
    let plain = CDisc::open(&dir.join("plain.iso"))?;
    let data = user_data();
    for lba in 0..DATA_SECTORS as u32 {
        let want = &raw1[lba as usize * 2352..(lba as usize + 1) * 2352];
        let r = cooked.read_raw(lba).map_err(|e| format!("cooked read_raw {lba}: {e}"))?;
        if r[..] != *want {
            return Err(format!("cooked.cue synthesized sector {lba} differs from the stored one"));
        }
        let r = plain.read_raw(lba).map_err(|e| format!("iso read_raw {lba}: {e}"))?;
        if r[..] != *want {
            return Err(format!("plain.iso synthesized sector {lba} differs from the stored one"));
        }
        let c = mixed.read_cooked(lba).map_err(|e| format!("mixed read_cooked {lba}: {e}"))?;
        if c[..] != data[lba as usize * 2048..(lba as usize + 1) * 2048] {
            return Err(format!("mixed.cue cooked sector {lba} differs from the user data"));
        }
    }
    // sync, header, non-zero EDC / P / Q on a synthesized sector
    let r = plain.read_raw(16).map_err(|e| e.to_string())?;
    expect("sync", r[..12].to_vec(), sector::SYNC.to_vec())?;
    expect("header", r[12..16].to_vec(), vec![0x00, 0x02, 0x16, 0x01])?;
    if r[2064..2068] == [0, 0, 0, 0] || r[2076..2248].iter().all(|&b| b == 0) || r[2248..].iter().all(|&b| b == 0) {
        return Err("EDC / P / Q of sector 16 are zero".into());
    }
    // audio sectors through both images are identical
    let n = mixed.sector_count();
    for lba in (DATA_SECTORS as u32..n).step_by(97) {
        let r = mixed.read_raw(lba).map_err(|e| e.to_string())?;
        let r2 = cooked.read_raw(lba).map_err(|e| e.to_string())?;
        if r != r2 {
            return Err(format!("audio sector {lba} differs between mixed.cue and cooked.cue"));
        }
    }
    Ok(())
}

fn check_lec(dir: &Path) -> Result<(), String> {
    let good = fs::read(dir.join("mixed.bin")).map_err(|e| e.to_string())?;
    let cue = fs::read_to_string(dir.join("mixed.cue")).map_err(|e| e.to_string())?.replace("mixed.bin", "lec.bin");
    fs::write(dir.join("lec.cue"), cue).map_err(|e| e.to_string())?;
    let at = 1000 * 2352;
    let want: Vec<u8> = good[at + 16..at + 16 + 2048].to_vec();

    // Damage a copy of the image and open it. Everything below asks the same
    // question of it: what does a *drive* hand over for this sector?
    let damaged = |edit: &dyn Fn(&mut Vec<u8>)| -> Result<CDisc, String> {
        let mut bin = good.clone();
        edit(&mut bin);
        fs::write(dir.join("lec.bin"), &bin).map_err(|e| e.to_string())?;
        CDisc::open(&dir.join("lec.cue"))
    };
    let flip = |off: usize, n: usize| move |bin: &mut Vec<u8>| {
        for i in 0..n {
            bin[at + off + i] ^= 0x5A;
        }
    };

    // A drive's L-EC decoder repairs what the P and Q parity can locate
    // (one wrong symbol per codeword) and hands the *original* bytes over.
    // Consecutive sector bytes fall in different codewords, so a burst is
    // spread across them and a long one is still repairable; past that two
    // errors land in one codeword and the sector is unreadable, which is
    // what a protection band looks like (doc 17 §2.5).
    for n in [1usize, 2, 8, 32, 96] {
        let d = damaged(&flip(500, n))?;
        expect(&format!("{n}-byte burst corrected"), d.read_cooked(1000).map(|b| b.to_vec()), Ok(want.clone()))?;
    }
    for n in [128usize, 600] {
        let d = damaged(&flip(500, n))?;
        expect(&format!("{n}-byte burst unreadable"), d.read_cooked(1000).err(), Some(capi::LIBDISC_EMEDIUM))?;
    }
    // the same for one wrong byte in each of the fields the parity covers
    for (off, name) in [(13usize, "header"), (2064, "EDC"), (2100, "P parity"), (2300, "Q parity")] {
        let d = damaged(&flip(off, 1))?;
        expect(&format!("one wrong {name} byte corrected"), d.read_cooked(1000).map(|b| b.to_vec()), Ok(want.clone()))?;
    }
    // Parity destroyed wholesale while the EDC stays intact: the bytes a
    // cooked read delivers are provably good (the EDC is a CRC-32 over
    // exactly those), so a drive hands the sector over and the parity is
    // never consulted. This is the shape of a real dump (a Warcraft 3
    // disc, 92 sectors, every one of them EDC-clean).
    let d = damaged(&|bin: &mut Vec<u8>| bin[at + 2076..at + 2352].fill(0x5A))?;
    expect("wrong parity with an intact EDC still reads", d.read_cooked(1000).map(|b| b.to_vec()), Ok(want.clone()))?;
    expect("... and read_cd cooked too", d.read_cd(1000, 2, 0x10, 0).map(|v| v[..2048].to_vec()), Ok(want.clone()))?;
    // but a wrong EDC with intact parity is data that changed: unreadable
    let d = damaged(&|bin: &mut Vec<u8>| {
        bin[at + 2064..at + 2068].copy_from_slice(&[0x5A, 0x5A, 0x5A, 0x5A]);
        bin[at + 2076..at + 2352].fill(0x5A);
    })?;
    expect("a wrong EDC the decoder cannot repair", d.read_cooked(1000).err(), Some(capi::LIBDISC_EMEDIUM))?;

    // two errors far enough apart to be in different codewords: both go
    let d = damaged(&|bin: &mut Vec<u8>| {
        bin[at + 100] ^= 0x5A;
        bin[at + 1500] ^= 0xA5;
    })?;
    expect("two scattered errors corrected", d.read_cooked(1000).map(|b| b.to_vec()), Ok(want.clone()))?;

    // What a dump tool writes over a sector it could not read (the whole
    // body one filler byte, the header left alone, DiscImageCreator's
    // "replaced at 0x55 except header") is exactly what a protection
    // check reads, and it must stay unreadable.
    let d = damaged(&|bin: &mut Vec<u8>| bin[at + 16..at + 2352].fill(0x55))?;
    expect("a filled sector body is unreadable", d.read_cooked(1000).err(), Some(capi::LIBDISC_EMEDIUM))?;
    expect("read_cd cooked of a filled body", d.read_cd(1000, 2, 0x10, 0).err(), Some(capi::LIBDISC_EMEDIUM))?;
    // A *raw* read of it is unreadable too, and that is the whole protection
    // question: Crimson Skies' SafeDisc 1.50.020 reads its band with exactly
    // this CDB (byte 9 = 0xF8) and it is the read failing that it looks for.
    // Delivering the stored bytes told it the disc was clean and it refused to
    // start (doc 17 §2.6c).
    expect("read_cd raw of a filled body", d.read_cd(1000, 2, 0xF8, 0).err(), Some(capi::LIBDISC_EMEDIUM))?;
    // ... unless C2 error flags were asked for, which is how a dumping tool
    // gets an unreadable sector's bytes out of a real drive: the sector comes
    // over with the field that says which bytes the drive could not trust.
    expect("read_cd raw+C2 of a filled body", d.read_cd(1000, 2, 0xFA, 0).map(|v| v.len()), Ok(2352 + 294))?;

    // Back to the single flipped byte: correcting it must not change what a
    // *raw* read delivers. Dumping a disc and reading a protection band
    // both depend on the stored bytes coming back as stored.
    let disc = damaged(&flip(500, 1))?;
    let r = disc.read_raw(1000).map_err(|e| e.to_string())?;
    expect("raw read still holds the damage", r[500], good[at + 500] ^ 0x5A)?;
    expect("read_cd raw still holds the damage", disc.read_cd(1000, 2, 0xF8, 0).map(|v| v[500]), Ok(good[at + 500] ^ 0x5A))?;
    // ... and sector_info still reports the stored sector as failing, which
    // is what `discx scan` counts when a dump is being diagnosed
    let info = disc.sector_info(1000).map_err(|e| e.to_string())?;
    expect("sector_info", info, LibdiscSectorInfo { kind: 1, track: 1, index: 1, lec: 0 })?;
    // every cooked shape of READ CD corrects, every raw one does not
    expect("read_cd cooked corrected", disc.read_cd(1000, 2, 0x10, 0).map(|v| v.len()), Ok(2048))?;
    expect("read_cd type 0 cooked corrected", disc.read_cd(1000, 0, 0x10, 0).map(|v| v[..2048].to_vec()), Ok(want.clone()))?;
    expect("read_cd header+user corrected", disc.read_cd(1000, 2, 0x30, 0).map(|v| v.len()), Ok(2052))?;
    expect("read_cd user+edc of the flipped sector", disc.read_cd(1000, 2, 0x18, 0).map(|v| v.len()), Ok(2336))?;
    // READ CD with C2 pointers: >= 1 bit set, block error byte set; the raw bytes still delivered
    let rc = disc.read_cd(1000, 2, 0xFA, 0).map_err(|e| format!("read_cd c2: {e}"))?;
    expect("read_cd c2 length", rc.len(), 2352 + 294)?;
    if rc[..2352] != r[..] {
        return Err("read_cd with C2 did not deliver the raw sector".into());
    }
    if rc[2352..].iter().all(|&b| b == 0) {
        return Err("C2 bits of the flipped sector are all clear".into());
    }
    let rc = disc.read_cd(1000, 2, 0xFC, 0).map_err(|e| format!("read_cd c2+block: {e}"))?;
    expect("read_cd c2+block length", rc.len(), 2352 + 296)?;
    expect("block error byte", rc[2352 + 294], 0x80)?;
    let ok = disc.read_cd(999, 2, 0xFC, 0).map_err(|e| e.to_string())?;
    if ok[2352..].iter().any(|&b| b != 0) {
        return Err("C2 bits of a good sector are not clear".into());
    }
    for lba in [999, 1001] {
        disc.read_cooked(lba).map_err(|e| format!("neighbour {lba}: {e}"))?;
        expect(&format!("neighbour {lba} lec"), disc.sector_info(lba).map_err(|e| e.to_string())?.lec, 1)?;
    }
    drop(disc);

    // a zero-filled sector (what other dump tools write for an unreadable
    // one) has no sync pattern to anchor the addresses on: nothing to decode
    let d = damaged(&|bin: &mut Vec<u8>| bin[1200 * 2352..1201 * 2352].fill(0))?;
    expect("zero-filled sector cooked", d.read_cooked(1200).err(), Some(capi::LIBDISC_EMEDIUM))?;
    expect("zero-filled sector info", d.sector_info(1200).map(|i| i.lec), Ok(0))?;
    let rc = d.read_cd(1200, 2, 0xFA, 0).map_err(|e| e.to_string())?;
    if rc[2352..].iter().any(|&b| b != 0xFF) {
        return Err("C2 bits of a zero-filled sector are not all set".into());
    }
    drop(d);

    // Leave the image the guest tools boot with carrying one of each: LBA
    // 1000 has the body a dump tool writes over a sector it could not read
    // (unreadable, whatever the decoder tries), and LBA 1010 one wrong byte
    // (which a drive repairs and hands over). tools/atapi-guest-test.py
    // asks a real guest for both.
    let mut bin = good.clone();
    bin[at + 16..at + 2352].fill(0x55);
    bin[1010 * 2352 + 500] ^= 0x5A;
    fs::write(dir.join("lec.bin"), &bin).map_err(|e| e.to_string())?;
    let d = CDisc::open(&dir.join("lec.cue"))?;
    expect("the fixture's unreadable sector", d.read_cooked(1000).err(), Some(capi::LIBDISC_EMEDIUM))?;
    expect("the fixture's correctable sector", d.read_cooked(1010).map(|b| b.to_vec()),
           Ok(good[1010 * 2352 + 16..1010 * 2352 + 16 + 2048].to_vec()))?;
    Ok(())
}
fn check_repair(dir: &Path) -> Result<(), String> {
    let mut bin = fs::read(dir.join("mixed.bin")).map_err(|e| e.to_string())?;
    let at = 1000 * 2352 + 500;
    bin[at] ^= 0x5A;
    fs::write(dir.join("rep.bin"), &bin).map_err(|e| e.to_string())?;
    let cue = fs::read_to_string(dir.join("mixed.cue")).map_err(|e| e.to_string())?.replace("mixed.bin", "rep.bin");
    fs::write(dir.join("rep.cue"), cue).map_err(|e| e.to_string())?;

    let src = dir.join("rep.cue");
    let disc = Disc::open(&src).map_err(|e| e.to_string())?;
    let out = dir.join("repaired");
    let r = repair(&disc, &src, &out)?;
    expect("sectors repaired", (r.sectors, r.no_sync, r.unstored), (1, 0, 0))?;

    let d = CDisc::open(&out.join("rep.cue"))?;
    d.read_cooked(1000).map_err(|e| format!("repaired sector still unreadable: {e}"))?;
    expect("repaired sector lec", d.sector_info(1000).map(|i| i.lec), Ok(1))?;
    let raw = d.read_raw(1000).map_err(|e| e.to_string())?;
    expect("user data kept as dumped", raw[500], bin[at])?;

    let after = fs::read(out.join("rep.bin")).map_err(|e| e.to_string())?;
    expect("file length", after.len(), bin.len())?;
    for (i, (a, b)) in bin.iter().zip(after.iter()).enumerate() {
        if a == b {
            continue;
        }
        let (sec, off) = (i / 2352, i % 2352);
        // Mode 1 parity: EDC 2064..2068, reserved 2068..2076, ECC 2076..2352.
        if sec != 1000 || off < 2064 {
            return Err(format!("byte {i} changed (sector {sec} offset {off}): repair touched more than parity"));
        }
    }
    // The sectors either side are untouched and still verify.
    for lba in [999, 1001] {
        expect(&format!("neighbour {lba}"), d.sector_info(lba).map(|i| i.lec), Ok(1))?;
    }
    Ok(())
}

fn check_edges(dir: &Path) -> Result<(), String> {
    let disc = CDisc::open(&dir.join("mixed.cue"))?;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_i0 = t2_i1 + T2_LEN;
    let t3_i1 = t3_i0 + T3_PREGAP;
    let leadout = t3_i1 + T3_LEN;
    expect("sector_count", disc.sector_count() as i32, leadout)?;
    expect("track_count", disc.track_count(), 3)?;
    expect("session_count", unsafe { capi::libdisc_session_count(disc.0) }, 1)?;
    expect("track 1", disc.track_info(1), Ok(LibdiscTrackInfo { number: 1, session: 1, control: 4, mode: 1, start_lba: 0, pregap_lba: 0, end_lba: DATA_SECTORS }))?;
    expect("track 2", disc.track_info(2), Ok(LibdiscTrackInfo { number: 2, session: 1, control: 0, mode: 0, start_lba: t2_i1, pregap_lba: DATA_SECTORS, end_lba: t3_i0 }))?;
    expect("track 3", disc.track_info(3), Ok(LibdiscTrackInfo { number: 3, session: 1, control: 0, mode: 0, start_lba: t3_i1, pregap_lba: t3_i0, end_lba: leadout }))?;
    expect("track 4", disc.track_info(4), Err(capi::LIBDISC_ERANGE))?;
    expect("track 0", disc.track_info(0), Err(capi::LIBDISC_ERANGE))?;
    let k = |lba: i32| disc.sector_info(lba as u32).map(|i| (i.kind, i.track, i.index)).map_err(|e| format!("info {lba}: {e}"));
    expect("lba 0", k(0)?, (1, 1, 1))?;
    expect("lba 1999", k(1999)?, (1, 1, 1))?;
    expect("t2 index 0 start", k(DATA_SECTORS)?, (0, 2, 0))?;
    expect("t2 index 0 end", k(t2_i1 - 1)?, (0, 2, 0))?;
    expect("t2 index 1", k(t2_i1)?, (0, 2, 1))?;
    expect("t3 pregap start", k(t3_i0)?, (0, 3, 0))?;
    expect("t3 pregap end", k(t3_i1 - 1)?, (0, 3, 0))?;
    expect("t3 index 1", k(t3_i1)?, (0, 3, 1))?;
    expect("last", k(leadout - 1)?, (0, 3, 1))?;
    for (lba, name) in [(leadout as u32, "lead-out"), (u32::MAX, "0xFFFFFFFF"), (0x8000_0000u32, "0x80000000")] {
        expect(&format!("read_raw {name}"), disc.read_raw(lba).err(), Some(capi::LIBDISC_ERANGE))?;
        expect(&format!("read_cooked {name}"), disc.read_cooked(lba).err(), Some(capi::LIBDISC_ERANGE))?;
        expect(&format!("read_sub {name}"), disc.read_sub(lba).err(), Some(capi::LIBDISC_ERANGE))?;
        expect(&format!("sector_info {name}"), disc.sector_info(lba).err(), Some(capi::LIBDISC_ERANGE))?;
        expect(&format!("read_cd {name}"), disc.read_cd(lba, 0, 0xF8, 0).err(), Some(capi::LIBDISC_ERANGE))?;
    }
    expect("read_cooked of an audio sector", disc.read_cooked(DATA_SECTORS as u32).err(), Some(capi::LIBDISC_EMODE))?;
    expect("read_cd type 2 on audio", disc.read_cd(t2_i1 as u32, 2, 0xF8, 0).err(), Some(capi::LIBDISC_EMODE))?;
    expect("read_cd type 1 on data", disc.read_cd(10, 1, 0xF8, 0).err(), Some(capi::LIBDISC_EMODE))?;
    expect("read_cd type 4 on mode 1", disc.read_cd(10, 4, 0xF8, 0).err(), Some(capi::LIBDISC_EMODE))?;
    expect("read_cd type 0 user only on audio", disc.read_cd(t2_i1 as u32, 0, 0x10, 0).err(), Some(capi::LIBDISC_EMODE))?;
    expect("read_cd type 0 raw on audio", disc.read_cd(t2_i1 as u32, 0, 0xF8, 0).map(|v| v.len()), Ok(2352))?;
    let r = disc.read_raw((t3_i0 + 10) as u32).map_err(|e| e.to_string())?;
    if r.iter().any(|&b| b != 0) {
        return Err("synthesized pregap sector is not silent".into());
    }
    let r = disc.read_raw((t2_i1 + 10) as u32).map_err(|e| e.to_string())?;
    if r.iter().all(|&b| b == 0) {
        return Err("tone sector is silent".into());
    }
    Ok(())
}

fn check_panic_safety(dir: &Path) -> Result<(), String> {
    // a corrupt cue must be an error string, never an abort
    fs::write(dir.join("bad.cue"), "FILE \"mixed.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:99\n").map_err(|e| e.to_string())?;
    match CDisc::open(&dir.join("bad.cue")) {
        Err(e) if e.contains("bad time") => {}
        Err(e) => return Err(format!("bad.cue: unexpected message {e:?}")),
        Ok(_) => return Err("bad.cue opened".into()),
    }
    fs::write(dir.join("bad2.cue"), "FILE \"nonexistent.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n").map_err(|e| e.to_string())?;
    match CDisc::open(&dir.join("bad2.cue")) {
        Err(e) if e.contains("not found") => {}
        Err(e) => return Err(format!("bad2.cue: unexpected message {e:?}")),
        Ok(_) => return Err("bad2.cue opened".into()),
    }
    // a bin whose size is not a multiple of the stride
    fs::write(dir.join("short.bin"), vec![0u8; 2352 * 3 + 7]).map_err(|e| e.to_string())?;
    fs::write(dir.join("short.cue"), "FILE \"short.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n").map_err(|e| e.to_string())?;
    match CDisc::open(&dir.join("short.cue")) {
        Err(e) if e.contains("multiple") => {}
        other => return Err(format!("short.cue: {:?}", other.map(|_| ()))),
    }
    // an ISO that is not one
    fs::write(dir.join("junk.iso"), vec![1u8; 4095]).map_err(|e| e.to_string())?;
    if CDisc::open(&dir.join("junk.iso")).is_ok() {
        return Err("junk.iso opened".into());
    }
    // a missing file, a NULL path, an empty error buffer
    if CDisc::open(&dir.join("missing.cue")).is_ok() {
        return Err("missing.cue opened".into());
    }
    let p = unsafe { capi::libdisc_open(std::ptr::null(), std::ptr::null_mut(), 0) };
    if !p.is_null() {
        return Err("NULL path opened".into());
    }
    // NULL handles and NULL buffers on every entry point
    let mut i = LibdiscSectorInfo::default();
    let mut t = LibdiscTrackInfo::default();
    unsafe {
        expect("sector_count(NULL)", capi::libdisc_sector_count(std::ptr::null()), 0)?;
        expect("track_info(NULL)", capi::libdisc_track_info(std::ptr::null(), 1, &mut t), capi::LIBDISC_EINVAL)?;
        expect("sector_info(NULL)", capi::libdisc_sector_info(std::ptr::null(), 0, &mut i), capi::LIBDISC_EINVAL)?;
        expect("read_raw(NULL)", capi::libdisc_read_raw(std::ptr::null(), 0, std::ptr::null_mut()), capi::LIBDISC_EINVAL)?;
        expect("read_toc(NULL)", capi::libdisc_mmc_read_toc(std::ptr::null(), 0, 0, 0, std::ptr::null_mut(), 0), capi::LIBDISC_EINVAL)?;
        capi::libdisc_close(std::ptr::null_mut());
    }
    let disc = CDisc::open(&dir.join("mixed.cue"))?;
    unsafe {
        expect("read_raw(NULL out)", capi::libdisc_read_raw(disc.0, 0, std::ptr::null_mut()), capi::LIBDISC_EINVAL)?;
        expect("toc into 4 bytes", capi::libdisc_mmc_read_toc(disc.0, 0, 0, 0, [0u8; 4].as_mut_ptr(), 4), capi::LIBDISC_EINVAL)?;
        expect("read_cd into 100 bytes", capi::libdisc_mmc_read_cd_sector(disc.0, 0, 0, 0xF8, 0, [0u8; 100].as_mut_ptr(), 100), capi::LIBDISC_EINVAL)?;
        expect("api version", capi::libdisc_api_version(), capi::LIBDISC_API_VERSION)?;
    }
    // probe: cue / ccd 100 with the right extension and content, iso 0, junk 0
    let probe = |name: &str, head: &[u8]| -> i32 {
        let c = CString::new(name).unwrap();
        unsafe { capi::libdisc_probe(head.as_ptr(), head.len(), c.as_ptr()) }
    };
    let cue = fs::read(dir.join("mixed.cue")).map_err(|e| e.to_string())?;
    let ccd = fs::read(dir.join("mixed.ccd")).map_err(|e| e.to_string())?;
    let mut iso_head = vec![0u8; 40 * 2048];
    iso_head[16 * 2048 + 1..16 * 2048 + 6].copy_from_slice(b"CD001");
    expect("probe cue", probe("x.cue", &cue), 100)?;
    expect("probe CUE upper", probe("X.CUE", &cue), 100)?;
    expect("probe ccd", probe("x.ccd", &ccd), 100)?;
    expect("probe iso", probe("x.iso", &iso_head), 0)?;
    expect("probe cue named iso", probe("x.iso", &cue), 0)?;
    expect("probe junk cue", probe("x.cue", b"hello"), 0)?;
    expect("probe empty", probe("x.cue", &[]), 0)?;
    Ok(())
}

fn check_subq(dir: &Path) -> Result<(), String> {
    let disc = CDisc::open(&dir.join("mixed.cue"))?;
    let stored = fs::read(dir.join("mixed.sub")).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_i0 = t2_i1 + T2_LEN;
    let t3_i1 = t3_i0 + T3_PREGAP;
    let mut samples: Vec<i32> = (0..n).step_by(37).collect();
    samples.extend([0, 1, 98, 99, 100, DATA_SECTORS - 1, DATA_SECTORS, t2_i1 - 1, t2_i1, t3_i0 - 1, t3_i0, t3_i1 - 1, t3_i1, n - 1]);
    for &lba in &samples {
        let s = disc.read_sub(lba as u32).map_err(|e| e.to_string())?;
        if s[..] != stored[lba as usize * 96..(lba as usize + 1) * 96] {
            return Err(format!("sub of {lba} differs from mixed.sub"));
        }
        let q = &s[12..24];
        if !subq::q_crc_ok(q) {
            return Err(format!("Q CRC of {lba} does not verify"));
        }
        let info = disc.sector_info(lba as u32).map_err(|e| e.to_string())?;
        let t = disc.track_info(info.track).map_err(|e| e.to_string())?;
        let adr = q[0] & 0x0F;
        expect(&format!("{lba} control"), q[0] >> 4, t.control)?;
        match lba % 100 {
            98 => expect(&format!("{lba} adr"), adr, 2)?,
            99 if info.track == 2 => expect(&format!("{lba} adr"), adr, 3)?,
            _ => {
                expect(&format!("{lba} adr"), adr, 1)?;
                expect(&format!("{lba} tno"), sector::from_bcd(q[1]), info.track)?;
                expect(&format!("{lba} index"), sector::from_bcd(q[2]), info.index)?;
                let abs = Msf { m: sector::from_bcd(q[7]), s: sector::from_bcd(q[8]), f: sector::from_bcd(q[9]) };
                expect(&format!("{lba} abs"), abs.to_lba(), lba)?;
                let rel = Msf { m: sector::from_bcd(q[3]), s: sector::from_bcd(q[4]), f: sector::from_bcd(q[5]) }.to_lba() + 150;
                let want = if info.index == 0 { t.start_lba - lba } else { lba - t.start_lba };
                expect(&format!("{lba} rel"), rel, want)?;
            }
        }
        let pause = info.index == 0 || disc.track_info(info.track + 1).map(|nt| nt.start_lba - lba <= 150).unwrap_or(false);
        expect(&format!("{lba} P"), s[0], if pause { 0xFF } else { 0 })?;
        // the same bytes through READ CD: subch 1 (raw, interleaved), 2 (Q formatted), 4 (R-W de-interleaved)
        let ty = if info.kind == 1 { 2 } else { 1 };
        let rc = disc.read_cd(lba as u32, ty, 0xF8, 1).map_err(|e| format!("read_cd sub1 {lba}: {e}"))?;
        expect(&format!("{lba} readcd sub1 length"), rc.len(), 2448)?;
        let mut de = [0u8; 96];
        let mut inter = [0u8; 96];
        inter.copy_from_slice(&rc[2352..]);
        subq::deinterleave(&inter, &mut de);
        if de != s {
            return Err(format!("READ CD raw subchannel of {lba} does not de-interleave to read_sub"));
        }
        let rc = disc.read_cd(lba as u32, ty, 0x00, 2).map_err(|e| format!("read_cd sub2 {lba}: {e}"))?;
        expect(&format!("{lba} readcd sub2"), rc, [&s[12..24], &[0u8; 4][..]].concat())?;
        let rc = disc.read_cd(lba as u32, ty, 0x00, 4).map_err(|e| format!("read_cd sub4 {lba}: {e}"))?;
        expect(&format!("{lba} readcd sub4"), rc, s.to_vec())?;
    }
    // MCN frame content, and the READ SUB-CHANNEL formats
    let s = disc.read_sub(98).map_err(|e| e.to_string())?;
    expect("mcn packed", s[13..20].to_vec(), vec![0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x30])?;
    let r = disc.subchannel(0, false, false, 1, 0, 0x15).map_err(|e| e.to_string())?;
    expect("subq=0 header", r, vec![0, 0x15, 0, 0])?;
    let r = disc.subchannel(t2_i1 as u32 + 75, true, true, 1, 0, 0x11).map_err(|e| e.to_string())?;
    expect("position msf", r, vec![0, 0x11, 0, 12, 1, 0x10, 2, 1, 0, 0, 31, 50, 0, 0, 1, 0])?;
    let r = disc.subchannel(t2_i1 as u32 + 75, false, true, 1, 0, 0x11).map_err(|e| e.to_string())?;
    expect("position lba", r, [vec![0, 0x11, 0, 12, 1, 0x10, 2, 1], (t2_i1 + 75).to_be_bytes().to_vec(), 75i32.to_be_bytes().to_vec()].concat())?;
    let r = disc.subchannel(DATA_SECTORS as u32 + 10, false, true, 1, 0, 0x15).map_err(|e| e.to_string())?;
    expect("position in index 0 (negative relative)", r, [vec![0, 0x15, 0, 12, 1, 0x10, 2, 0], (DATA_SECTORS + 10).to_be_bytes().to_vec(), (-140i32).to_be_bytes().to_vec()].concat())?;
    let r = disc.subchannel(0, false, true, 2, 0, 0x15).map_err(|e| e.to_string())?;
    expect("mcn", r, [vec![0, 0x15, 0, 20, 2, 0, 0, 0, 0x80], MCN.as_bytes().to_vec(), vec![0, 0]].concat())?;
    let r = disc.subchannel(0, false, true, 3, 2, 0x15).map_err(|e| e.to_string())?;
    expect("isrc", r, [vec![0, 0x15, 0, 20, 3, 0x10, 2, 0, 0x80], ISRC.as_bytes().to_vec(), vec![0, 0, 0]].concat())?;
    let r = disc.subchannel(0, false, true, 3, 1, 0x15).map_err(|e| e.to_string())?;
    expect("isrc of a track without one", r, [vec![0, 0x15, 0, 20, 3, 0x14, 1, 0, 0], vec![b'0'; 12], vec![0, 0, 0]].concat())?;
    expect("format 4", disc.subchannel(0, false, true, 4, 0, 0).err(), Some(capi::LIBDISC_EINVAL))?;
    expect("isrc track 9", disc.subchannel(0, false, true, 3, 9, 0).err(), Some(capi::LIBDISC_EINVAL))?;
    Ok(())
}

fn check_toc(dir: &Path) -> Result<(), String> {
    let mixed = CDisc::open(&dir.join("mixed.cue"))?;
    let cooked = CDisc::open(&dir.join("cooked.cue"))?;
    let plain = CDisc::open(&dir.join("plain.iso"))?;
    let t2_i1 = DATA_SECTORS + T2_PREGAP;
    let t3_i1 = t2_i1 + T2_LEN + T3_PREGAP;
    let leadout = t3_i1 + T3_LEN;
    // identical across the raw and the cooked image, every format
    for (format, msf, start) in [(0, false, 0), (0, true, 0), (0, false, 1), (0, true, 2), (0, false, 0xAA), (1, false, 0), (1, true, 0), (2, false, 0), (2, true, 1)] {
        let a = mixed.toc(format, msf, start).map_err(|e| format!("mixed toc {format} {msf} {start}: {e}"))?;
        let b = cooked.toc(format, msf, start).map_err(|e| e.to_string())?;
        if a != b {
            return Err(format!("toc format {format} msf {msf} start {start} differs between mixed.cue and cooked.cue"));
        }
    }
    // format 0, hand-coded
    let msf4 = |lba: i32| {
        let m = Msf::from_lba(lba);
        vec![0, m.m, m.s, m.f]
    };
    let want = [
        vec![0, 34, 1, 3],
        vec![0, 0x14, 1, 0], msf4(0),
        vec![0, 0x10, 2, 0], msf4(t2_i1),
        vec![0, 0x10, 3, 0], msf4(t3_i1),
        vec![0, 0x10, 0xAA, 0], msf4(leadout),
    ].concat();
    expect("toc0 msf", mixed.toc(0, true, 0).map_err(|e| e.to_string())?, want)?;
    let want = [
        vec![0, 34, 1, 3],
        vec![0, 0x14, 1, 0], 0i32.to_be_bytes().to_vec(),
        vec![0, 0x10, 2, 0], t2_i1.to_be_bytes().to_vec(),
        vec![0, 0x10, 3, 0], t3_i1.to_be_bytes().to_vec(),
        vec![0, 0x10, 0xAA, 0], leadout.to_be_bytes().to_vec(),
    ].concat();
    expect("toc0 lba", mixed.toc(0, false, 0).map_err(|e| e.to_string())?, want)?;
    let want = [vec![0, 18, 1, 3], vec![0, 0x10, 3, 0], t3_i1.to_be_bytes().to_vec(), vec![0, 0x10, 0xAA, 0], leadout.to_be_bytes().to_vec()].concat();
    expect("toc0 from track 3", mixed.toc(0, false, 3).map_err(|e| e.to_string())?, want)?;
    let want = [vec![0, 10, 1, 3], vec![0, 0x10, 0xAA, 0], leadout.to_be_bytes().to_vec()].concat();
    expect("toc0 lead-out only", mixed.toc(0, false, 0xAA).map_err(|e| e.to_string())?, want)?;
    expect("toc0 from track 4", mixed.toc(0, false, 4).err(), Some(capi::LIBDISC_EINVAL))?;
    // single-track ISO: the layout QEMU's cdrom_read_toc produces, but with the lead-out control a
    // real drive reports (0x14: QEMU says 0x16)
    let want = [vec![0, 18, 1, 1], vec![0, 0x14, 1, 0], msf4(0), vec![0, 0x14, 0xAA, 0], msf4(DATA_SECTORS)].concat();
    expect("toc0 iso msf", plain.toc(0, true, 0).map_err(|e| e.to_string())?, want)?;
    let want = [vec![0, 18, 1, 1], vec![0, 0x14, 1, 0], vec![0, 0, 0, 0], vec![0, 0x14, 0xAA, 0], DATA_SECTORS.to_be_bytes().to_vec()].concat();
    expect("toc0 iso lba", plain.toc(0, false, 0).map_err(|e| e.to_string())?, want)?;
    // format 1
    expect("toc1", mixed.toc(1, false, 0).map_err(|e| e.to_string())?, [vec![0, 10, 1, 1], vec![0, 0x14, 1, 0], vec![0, 0, 0, 0]].concat())?;
    // format 2: MSF regardless of the msf bit
    let e = |ctl: u8, point: u8, p: Vec<u8>| [vec![1, ctl, 0, point, 0, 0, 0, 0], p].concat();
    let msf3 = |lba: i32| {
        let m = Msf::from_lba(lba);
        vec![m.m, m.s, m.f]
    };
    let want = [
        vec![0, 68, 1, 1],
        e(0x14, 0xA0, vec![1, 0, 0]),
        e(0x10, 0xA1, vec![3, 0, 0]),
        e(0x10, 0xA2, msf3(leadout)),
        e(0x14, 1, msf3(0)),
        e(0x10, 2, msf3(t2_i1)),
        e(0x10, 3, msf3(t3_i1)),
    ].concat();
    expect("toc2", mixed.toc(2, false, 0).map_err(|e| e.to_string())?, want.clone())?;
    expect("toc2 msf", mixed.toc(2, true, 0).map_err(|e| e.to_string())?, want)?;
    expect("toc3", mixed.toc(3, false, 0).err(), Some(capi::LIBDISC_EINVAL))?;
    expect("toc5", mixed.toc(5, false, 0).err(), Some(capi::LIBDISC_EINVAL))?;
    // READ DISC INFORMATION
    let mut want = vec![0u8; 34];
    want[1] = 32; want[2] = 0x0E; want[3] = 1; want[4] = 1; want[5] = 1; want[6] = 3; want[7] = 0x20;
    want[16..24].fill(0xFF);
    expect("disc information", mixed.disc_information().map_err(|e| e.to_string())?, want)?;
    Ok(())
}

fn check_ccd(dir: &Path) -> Result<(), String> {
    let cue = CDisc::open(&dir.join("mixed.cue"))?;
    let ccd = CDisc::open(&dir.join("mixed.ccd"))?;
    let cooked = CDisc::open(&dir.join("cooked.cue"))?;
    let n = cue.sector_count();
    expect("sector_count", ccd.sector_count(), n)?;
    expect("track_count", ccd.track_count(), cue.track_count())?;
    for t in 1..=3 {
        expect(&format!("track {t}"), ccd.track_info(t), cue.track_info(t))?;
    }
    // TOC formats identical across cue, ccd (raw_toc replayed) and cooked cue
    for (format, msf, start) in [(0, false, 0), (0, true, 0), (1, false, 0), (2, false, 0), (2, true, 0)] {
        let a = cue.toc(format, msf, start).map_err(|e| e.to_string())?;
        let b = ccd.toc(format, msf, start).map_err(|e| format!("ccd toc {format}: {e}"))?;
        let c = cooked.toc(format, msf, start).map_err(|e| e.to_string())?;
        if a != b || a != c {
            return Err(format!("toc format {format} msf {msf} differs between mixed.cue, mixed.ccd and cooked.cue"));
        }
    }
    expect("disc information", ccd.disc_information(), cue.disc_information())?;
    for f in [1u8, 2, 3] {
        expect(&format!("subchannel format {f}"), ccd.subchannel(2300, false, true, f, 2, 0x15), cue.subchannel(2300, false, true, f, 2, 0x15))?;
    }
    // every sector: raw identical, sub identical (stored .sub vs synthesized), cooked identical
    for lba in 0..n {
        let a = cue.read_raw(lba).map_err(|e| e.to_string())?;
        let b = ccd.read_raw(lba).map_err(|e| format!("ccd read_raw {lba}: {e}"))?;
        if a != b {
            return Err(format!("raw sector {lba} differs between mixed.cue and mixed.ccd"));
        }
        let a = cue.read_sub(lba).map_err(|e| e.to_string())?;
        let b = ccd.read_sub(lba).map_err(|e| format!("ccd read_sub {lba}: {e}"))?;
        if a != b {
            return Err(format!("subchannel of {lba} differs between mixed.cue (synthesized) and mixed.ccd (stored)"));
        }
        expect(&format!("info {lba}"), ccd.sector_info(lba), cue.sector_info(lba))?;
    }
    for lba in [0u32, 16, 1999] {
        expect(&format!("cooked {lba}"), ccd.read_cooked(lba), cue.read_cooked(lba))?;
    }
    expect("cooked audio", ccd.read_cooked(3000).err(), Some(capi::LIBDISC_EMODE))?;
    expect("lead-out", ccd.read_raw(n).err(), Some(capi::LIBDISC_ERANGE))?;
    // a CCD without .sub opens and synthesizes; a truncated .sub synthesizes past its end
    fs::copy(dir.join("mixed.ccd"), dir.join("nosub.ccd")).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(dir.join("nosub.img"));
    fs::hard_link(dir.join("mixed.img"), dir.join("nosub.img")).or_else(|_| fs::copy(dir.join("mixed.img"), dir.join("nosub.img")).map(|_| ())).map_err(|e| e.to_string())?;
    let nosub = CDisc::open(&dir.join("nosub.ccd"))?;
    for lba in [0u32, 2149, 2150, 5299, 5300, n - 1] {
        expect(&format!("nosub {lba}"), nosub.read_sub(lba), cue.read_sub(lba))?;
    }
    let stored = fs::read(dir.join("mixed.sub")).map_err(|e| e.to_string())?;
    fs::copy(dir.join("mixed.ccd"), dir.join("trunc.ccd")).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(dir.join("trunc.img"));
    fs::hard_link(dir.join("mixed.img"), dir.join("trunc.img")).or_else(|_| fs::copy(dir.join("mixed.img"), dir.join("trunc.img")).map(|_| ())).map_err(|e| e.to_string())?;
    let mut half = stored[..3000 * 96].to_vec();
    half[2000 * 96 + 20] ^= 0xFF; // a stored byte that synthesis would not produce
    fs::write(dir.join("trunc.sub"), &half).map_err(|e| e.to_string())?;
    let trunc = CDisc::open(&dir.join("trunc.ccd"))?;
    let s = trunc.read_sub(2000).map_err(|e| e.to_string())?;
    expect("stored sub replayed", s[20], half[2000 * 96 + 20])?;
    expect("past the .sub end", trunc.read_sub(4000), cue.read_sub(4000))?;
    // refusals
    fs::write(dir.join("scr.ccd"), "[CloneCD]\nVersion=3\n[Disc]\nTocEntries=0\nSessions=1\nDataTracksScrambled=1\n").map_err(|e| e.to_string())?;
    match CDisc::open(&dir.join("scr.ccd")) {
        Err(e) if e.contains("Scrambled") => {}
        other => return Err(format!("scrambled ccd: {:?}", other.map(|_| ()))),
    }
    fs::write(dir.join("noimg.ccd"), fs::read(dir.join("mixed.ccd")).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    match CDisc::open(&dir.join("noimg.ccd")) {
        Err(e) if e.contains("not found") => {}
        other => return Err(format!("ccd without img: {:?}", other.map(|_| ()))),
    }
    Ok(())
}

fn check_read_cd_lengths() -> Result<(), String> {
    // (expected type, byte 9, byte 10) → bytes per sector, MMC-3 tables 356..360
    let table: &[((u8, u8, u8), i32)] = &[
        ((0, 0x10, 0), 2048), ((0, 0xF8, 0), 2352), ((0, 0x00, 0), 0),
        ((1, 0x10, 0), 2352), ((1, 0xF8, 0), 2352), ((1, 0x80, 0), 2352), ((1, 0x00, 0), 0),
        ((2, 0x10, 0), 2048), ((2, 0x08, 0), 288), ((2, 0x18, 0), 2336), ((2, 0x20, 0), 4), ((2, 0x30, 0), 2052),
        ((2, 0x38, 0), 2340), ((2, 0x80, 0), 12), ((2, 0xA0, 0), 16), ((2, 0xB0, 0), 2064), ((2, 0xB8, 0), 2352),
        ((2, 0xF8, 0), 2352), ((2, 0xF0, 0), 2064), ((2, 0x50, 0), 2048), ((2, 0x58, 0), 2336), ((2, 0x40, 0), 0),
        ((2, 0x28, 0), capi::LIBDISC_EINVAL), ((2, 0x90, 0), capi::LIBDISC_EINVAL), ((2, 0x88, 0), capi::LIBDISC_EINVAL), ((2, 0xA8, 0), capi::LIBDISC_EINVAL),
        ((3, 0x10, 0), 2336), ((3, 0x30, 0), 2340), ((3, 0xB0, 0), 2352), ((3, 0xF8, 0), 2352), ((3, 0x18, 0), 2336),
        ((4, 0x10, 0), 2048), ((4, 0x18, 0), 2328), ((4, 0x40, 0), 8), ((4, 0x50, 0), 2056), ((4, 0x58, 0), 2336),
        ((4, 0x70, 0), 2060), ((4, 0x78, 0), 2340), ((4, 0xF8, 0), 2352), ((4, 0xF0, 0), 2072), ((4, 0xB0, 0), capi::LIBDISC_EINVAL),
        ((4, 0x08, 0), 280), ((4, 0x30, 0), capi::LIBDISC_EINVAL),
        ((5, 0x10, 0), 2324), ((5, 0x18, 0), 2328), ((5, 0x58, 0), 2336), ((5, 0xF8, 0), 2352), ((5, 0x08, 0), 4),
        ((2, 0xFA, 0), 2352 + 294), ((2, 0xFC, 0), 2352 + 296), ((2, 0xFE, 0), capi::LIBDISC_EINVAL), ((2, 0x12, 0), 2048 + 294),
        ((2, 0xF8, 1), 2448), ((2, 0xF8, 2), 2368), ((2, 0xF8, 4), 2448), ((2, 0x00, 2), 16), ((2, 0xF8, 3), capi::LIBDISC_EINVAL),
        ((2, 0xF8, 5), capi::LIBDISC_EINVAL), ((2, 0xFA, 1), 2352 + 294 + 96),
        ((6, 0xF8, 0), capi::LIBDISC_EINVAL), ((7, 0x10, 0), capi::LIBDISC_EINVAL),
    ];
    for &((ty, b9, b10), want) in table {
        expect(&format!("type {ty} byte9 {b9:#04x} byte10 {b10}"), read_cd_length(ty, b9, b10), want)?;
    }
    // the fill delivers exactly those bytes, from the right offsets
    let dir = std::env::var("DISCX_DIR").unwrap_or_default();
    let _ = dir;
    Ok(())
}

fn check_read_cd_fill(dir: &Path) -> Result<(), String> {
    let disc = CDisc::open(&dir.join("mixed.cue"))?;
    let raw = disc.read_raw(16).map_err(|e| e.to_string())?;
    let t2 = (DATA_SECTORS + T2_PREGAP + 5) as u32;
    let audio = disc.read_raw(t2).map_err(|e| e.to_string())?;
    let cases: &[(u8, u8, &[std::ops::Range<usize>])] = &[
        (2, 0x10, &[16..2064]), (2, 0xF8, &[0..2352]), (2, 0x08, &[2064..2352]), (2, 0x18, &[16..2352]),
        (2, 0x20, &[12..16]), (2, 0x30, &[12..2064]), (2, 0x80, &[0..12]), (2, 0xA0, &[0..16]), (2, 0xB0, &[0..2064]),
        (0, 0x10, &[16..2064]), (0, 0xF8, &[0..2352]), (0, 0x30, &[12..2064]),
    ];
    for &(ty, b9, ranges) in cases {
        let got = disc.read_cd(16, ty, b9, 0).map_err(|e| format!("type {ty} {b9:#04x}: {e}"))?;
        let want: Vec<u8> = ranges.iter().flat_map(|r| raw[r.clone()].iter().copied()).collect();
        if got != want {
            return Err(format!("type {ty} byte9 {b9:#04x}: {} bytes, wrong content", got.len()));
        }
    }
    for b9 in [0x10u8, 0xF8, 0x80, 0x08] {
        let got = disc.read_cd(t2, 1, b9, 0).map_err(|e| format!("audio {b9:#04x}: {e}"))?;
        if got[..] != audio[..] {
            return Err(format!("CD-DA byte9 {b9:#04x} did not deliver the whole sector"));
        }
    }
    let got = disc.read_cd(t2, 0, 0xF8, 0).map_err(|e| e.to_string())?;
    if got[..] != audio[..] {
        return Err("type 0 raw on audio did not deliver the whole sector".into());
    }
    expect("audio nothing", disc.read_cd(t2, 1, 0x00, 0).map(|v| v.len()), Ok(0))?;
    Ok(())
}

fn selftest(dir: &Path) -> i32 {
    let mut ck = Checks { fails: 0, passes: 0 };
    let raw1 = match generate(dir) {
        Ok(r) => r,
        Err(e) => {
            println!("FAIL generate: {e}");
            return 1;
        }
    };
    ck.report("msf", check_msf());
    ck.report("raw-synth", check_raw_synth(dir, &raw1));
    ck.report("lec", check_lec(dir));
    ck.report("repair", check_repair(dir));
    ck.report("edges", check_edges(dir));
    ck.report("subq-synth", check_subq(dir));
    ck.report("toc", check_toc(dir));
    ck.report("ccd", check_ccd(dir));
    ck.report("read-cd-length", check_read_cd_lengths());
    ck.report("read-cd-fill", check_read_cd_fill(dir));
    ck.report("dirdisc", check_dirdisc(dir));
    ck.report("panic-safety", check_panic_safety(dir));
    println!("{} passed, {} failed", ck.passes, ck.fails);
    if ck.fails > 0 {
        1
    } else {
        0
    }
}

fn hex(bytes: &[u8]) {
    for (i, row) in bytes.chunks(16).enumerate() {
        out!("{:06x} ", i * 16);
        for b in row {
            out!(" {b:02x}");
        }
        outln!();
    }
}

fn info(disc: &Disc) {
    outln!("sectors {}  sessions {}  tracks {}", disc.sector_count(), disc.sessions.len(), disc.track_count());
    if let Some(m) = disc.mcn {
        outln!("catalog {}", String::from_utf8_lossy(&m));
    }
    for s in &disc.sessions {
        outln!("session {}  lead-out {}", s.number, s.leadout_lba);
        for t in &s.tracks {
            let mode = match t.mode {
                TrackMode::Audio => "audio",
                TrackMode::Mode1 => "mode1",
                TrackMode::Mode2 => "mode2",
                TrackMode::Mode2Form1 => "mode2/f1",
                TrackMode::Mode2Form2 => "mode2/f2",
                TrackMode::Mode2Formless => "cdi",
            };
            let idx: Vec<String> = t.indices.iter().map(|(i, l)| format!("{i:02}@{l}")).collect();
            out!("  track {:02} {mode:<8} ctl {:x}  [{} .. {})  indices {}", t.number, t.control, t.first_lba(), t.end_lba, idx.join(" "));
            if let Some(i) = t.isrc {
                out!("  isrc {}", String::from_utf8_lossy(&i));
            }
            outln!();
            for e in &t.extents {
                outln!("    extent {} +{}  {:?}{}", e.lba, e.count, e.source, if e.sub.is_some() { " +sub" } else { "" });
            }
        }
    }
}

/// Walk the disc: kind histogram, L-EC failures (listed as ranges), and
/// whether stored subchannel frames verify.
fn scan(disc: &Disc, first: i32, count: Option<i32>) -> Result<(), String> {
    let n = disc.sector_count() as i32;
    let end = count.map(|c| (first + c).min(n)).unwrap_or(n);
    let mut kinds = [0u64; 6];
    let mut lec_fail: Vec<i32> = Vec::new();
    let mut edc_only = 0u64;
    let mut no_sync = 0u64;
    let mut q_bad = 0u64;
    let mut q_seen = 0u64;
    let mut edc_intact = 0u64;
    let mut correctable = 0u64;
    let mut hard: Vec<i32> = Vec::new();
    let mut raw = [0u8; 2352];
    let mut sub = [0u8; 96];
    let t0 = std::time::Instant::now();
    for lba in first..end {
        let (kind, _, _) = disc.classify(lba).map_err(|e| format!("{lba}: {e}"))?;
        kinds[kind.code() as usize] += 1;
        if kind.is_data() {
            disc.read_raw(lba, &mut raw).map_err(|e| format!("{lba}: {e}"))?;
            match sector::verify(&raw, kind) {
                libdisc::ecc::Lec::Ok => {}
                libdisc::ecc::Lec::EdcMismatch => {
                    edc_only += 1;
                    lec_fail.push(lba);
                }
                libdisc::ecc::Lec::EccMismatch => lec_fail.push(lba),
                libdisc::ecc::Lec::NoSync => {
                    no_sync += 1;
                    lec_fail.push(lba);
                }
            }
            if lec_fail.last() == Some(&lba) {
                // What a guest actually sees. The EDC covers exactly the
                // bytes a cooked read delivers, so a sector whose EDC comes
                // out is read as it stands however wrong its parity is;
                // otherwise the P/Q decoder gets a go, and only what it
                // cannot repair is a medium error, which is what a
                // protection check is looking for.
                if libdisc::ecc::edc_ok(&raw, kind) && sector::has_sync_header(&raw, kind) {
                    edc_intact += 1;
                } else {
                    let mut work = raw;
                    if libdisc::ecc::correct(&mut work, kind).is_some() {
                        correctable += 1;
                    } else {
                        hard.push(lba);
                    }
                }
            }
        }
        if lba % 7 == 0 {
            disc.read_sub(lba, &mut sub).map_err(|e| format!("{lba}: {e}"))?;
            q_seen += 1;
            if !subq::q_crc_ok(&sub[12..24]) {
                q_bad += 1;
            }
        }
    }
    let names = ["audio", "mode1", "mode2 form1", "mode2 form2", "mode2 formless", "gap"];
    outln!("sectors {first}..{end} of {n} in {:.1} s", t0.elapsed().as_secs_f64());
    for (i, k) in kinds.iter().enumerate() {
        if *k > 0 {
            outln!("  {:<15} {k}", names[i]);
        }
    }
    outln!("  L-EC failures   {} ({} with the EDC wrong too, {} without a sync pattern)", lec_fail.len(), edc_only, no_sync);
    if !lec_fail.is_empty() {
        outln!("  a guest reads   {edc_intact} anyway (the EDC is intact, only the parity disagrees)");
        outln!("                  {correctable} after the P/Q decoder repairs them");
        outln!("  unreadable      {}", hard.len());
    }
    if !hard.is_empty() {
        let mut ranges: Vec<(i32, i32)> = Vec::new();
        for &l in &hard {
            match ranges.last_mut() {
                Some(r) if r.1 + 1 == l => r.1 = l,
                _ => ranges.push((l, l)),
            }
        }
        let shown: Vec<String> = ranges.iter().take(40).map(|(a, b)| if a == b { a.to_string() } else { format!("{a}-{b}") }).collect();
        outln!("  unreadable LBAs {}{}", shown.join(" "), if ranges.len() > 40 { format!(" … ({} ranges)", ranges.len()) } else { String::new() });
    }
    if !lec_fail.is_empty() {
        let mut ranges: Vec<(i32, i32)> = Vec::new();
        for &l in &lec_fail {
            match ranges.last_mut() {
                Some(r) if r.1 + 1 == l => r.1 = l,
                _ => ranges.push((l, l)),
            }
        }
        let shown: Vec<String> = ranges.iter().take(40).map(|(a, b)| if a == b { a.to_string() } else { format!("{a}-{b}") }).collect();
        outln!("  failing LBAs    {}{}", shown.join(" "), if ranges.len() > 40 { format!(" … ({} ranges)", ranges.len()) } else { String::new() });
    }
    outln!("  Q frames        {q_seen} sampled, {q_bad} with a bad CRC");
    Ok(())
}

/// Write a copy of an image in which every sector whose L-EC fails now
/// verifies: **the negative control for a protection check** (doc 17 §2.6).
///
/// A protected title passing on the real dump only proves the check was
/// satisfied if the same check *rejects* a disc it should reject; otherwise a
/// pass could mean the check never ran. This builds that disc. The user data
/// of every failing sector is left exactly as dumped (SafeDisc's 0x55 fill
/// stays 0x55) and only EDC and ECC are regenerated over it, so the copy
/// differs from the original in nothing but the one signal the protection
/// reads. Sectors with no sync pattern are left alone: they are run-out, not
/// a protection band, and there is no sector there to rebuild.
struct Repaired {
    /// Sectors whose EDC/ECC was regenerated.
    sectors: u64,
    /// Failing sectors left alone for want of a sync pattern (run-out).
    no_sync: u64,
    /// Failing sectors stored in a layout that keeps no parity of its own.
    unstored: u64,
    out_image: PathBuf,
}

fn repair(disc: &Disc, image: &Path, outdir: &Path) -> Result<Repaired, String> {
    let n = disc.sector_count() as i32;
    let mut raw = [0u8; 2352];
    // (file index, offset in that file, lba, kind) for every repairable sector.
    let mut work: Vec<(usize, u64, i32, libdisc::SectorKind)> = Vec::new();
    let mut no_sync = 0u64;
    let mut unstored = 0u64;

    for lba in 0..n {
        let (kind, _, _) = disc.classify(lba).map_err(|e| format!("{lba}: {e}"))?;
        if !kind.is_data() {
            continue;
        }
        disc.read_raw(lba, &mut raw).map_err(|e| format!("{lba}: {e}"))?;
        match sector::verify(&raw, kind) {
            libdisc::ecc::Lec::Ok => continue,
            libdisc::ecc::Lec::NoSync => {
                no_sync += 1;
                continue;
            }
            libdisc::ecc::Lec::EdcMismatch | libdisc::ecc::Lec::EccMismatch => {}
        }
        // Where the sector is stored. Only a raw layout keeps parity of its
        // own; a cooked one has none to be wrong, so it can never fail here.
        let Some((_, t, _)) = disc.locate(lba) else { continue };
        let Some(e) = t.extents.iter().find(|e| lba >= e.lba && lba < e.end()) else {
            return Err(format!("{lba}: no extent"));
        };
        match e.source {
            libdisc::Source::File { file, offset, layout, swap, .. } => {
                if swap {
                    return Err(format!("{lba}: byte-swapped data track, refusing"));
                }
                match layout {
                    libdisc::Layout::Raw2352 | libdisc::Layout::Raw2352Sub96 => {
                        let off = offset + (lba - e.lba) as u64 * layout.stride() as u64;
                        work.push((file, off, lba, kind));
                    }
                    _ => unstored += 1,
                }
            }
            _ => unstored += 1,
        }
    }

    fs::create_dir_all(outdir).map_err(|e| format!("{}: {e}", outdir.display()))?;
    let copy_in = |src: &Path| -> Result<PathBuf, String> {
        let name = src.file_name().ok_or_else(|| format!("{}: no file name", src.display()))?;
        let dst = outdir.join(name);
        if dst == src {
            return Err(format!("{}: refusing to write over the source", dst.display()));
        }
        fs::copy(src, &dst).map_err(|e| format!("{} -> {}: {e}", src.display(), dst.display()))?;
        Ok(dst)
    };

    // Every payload the descriptor names, then the descriptor itself, so the
    // copy opens by the same name in the new directory.
    let mut copies: Vec<PathBuf> = Vec::new();
    for i in 0..disc.file_count() {
        copies.push(copy_in(&disc.file(i).path)?);
    }
    let out_image = copy_in(image)?;

    let mut handles: BTreeMap<usize, fs::File> = BTreeMap::new();
    let mut done = 0u64;
    for &(file, off, lba, kind) in &work {
        disc.read_raw(lba, &mut raw).map_err(|e| format!("{lba}: {e}"))?;
        match kind {
            libdisc::SectorKind::Mode1 => libdisc::ecc::generate_mode1(&mut raw),
            libdisc::SectorKind::Mode2Form1 => libdisc::ecc::generate_mode2_form1(&mut raw),
            libdisc::SectorKind::Mode2Form2 => libdisc::ecc::generate_mode2_form2(&mut raw),
            // Formless mode 2 carries no parity, so nothing here can be wrong.
            _ => continue,
        }
        let h = match handles.get_mut(&file) {
            Some(h) => h,
            None => {
                let f = fs::OpenOptions::new()
                    .write(true)
                    .open(&copies[file])
                    .map_err(|e| format!("{}: {e}", copies[file].display()))?;
                handles.entry(file).or_insert(f)
            }
        };
        write_all_at(h, &raw, off).map_err(|e| format!("{}: at {off}: {e}", copies[file].display()))?;
        done += 1;
    }
    for (_, h) in handles.iter_mut() {
        h.sync_all().map_err(|e| format!("sync: {e}"))?;
    }

    Ok(Repaired { sectors: done, no_sync, unstored, out_image })
}

/// Positional write, the mirror of `Payload::read_at` in the library: the
/// two platforms spell it differently and neither moves the file cursor,
/// which is what makes `repair` safe to interleave across files.
fn write_all_at(h: &mut fs::File, buf: &[u8], offset: u64) -> std::io::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileExt;
        h.write_all_at(buf, offset)
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::FileExt;
        let mut done = 0;
        while done < buf.len() {
            let n = h.seek_write(&buf[done..], offset + done as u64)?;
            if n == 0 {
                return Err(std::io::Error::new(std::io::ErrorKind::WriteZero, "short write"));
            }
            done += n;
        }
        Ok(())
    }
}

/// Walk every sector's stored subchannel and characterise the Q frames that
/// fail their CRC (doc 17 §2.6). On a real dump a failure is one of two very
/// different things, and everything printed here exists to tell them apart:
/// the drive's own read noise (scattered singletons, subchannel is delivered
/// with no error correction) or our frame layout (clustered, systematic, or
/// concentrated in one track / one ADR). The second half checks the opposite
/// direction, how often `subq::synthesize` reproduces what the disc itself
/// carries, which is the only real-disc test our synthesizer gets.
fn subscan(disc: &Disc, first: i32, count: Option<i32>) -> Result<(), String> {
    let n = disc.sector_count() as i32;
    let end = count.map(|c| (first + c).min(n)).unwrap_or(n);

    let mut seen = 0u64;
    let mut bad: Vec<i32> = Vec::new();
    let mut seen_kind = [0u64; 2]; // 0 = audio, 1 = data
    let mut bad_kind = [0u64; 2];
    let mut adr_good = [0u64; 16];
    let mut adr_bad = [0u64; 16];
    let mut seen_track: BTreeMap<u8, u64> = BTreeMap::new();
    let mut bad_track: BTreeMap<u8, u64> = BTreeMap::new();
    let mut seen_pause = 0u64;
    let mut bad_pause = 0u64;
    let mut zero = 0u64;   // all 96 bytes zero: the drive delivered nothing
    let mut raw_ok = 0u64; // passes CRC *without* deinterleaving = wrong layout
    let mut pos_ok = 0u64; // a bad frame whose position still matches ours
    let mut samples: Vec<(i32, [u8; 12])> = Vec::new();

    // the other direction: does our synthesizer agree with the disc?
    let mut syn_seen = 0u64;
    let mut syn_match = 0u64;
    let mut syn_diff = [0u64; 10];
    let mut syn_bad: Vec<i32> = Vec::new();

    let mut de = [0u8; 96];
    let mut inter = [0u8; 96];
    let mut syn = [0u8; 96];
    let t0 = std::time::Instant::now();

    for lba in first..end {
        disc.read_sub(lba, &mut de).map_err(|e| format!("{lba}: {e}"))?;
        seen += 1;

        let (kind, _, _) = disc.classify(lba).map_err(|e| format!("{lba}: {e}"))?;
        let k = usize::from(kind.is_data());
        seen_kind[k] += 1;

        let (track, index) = match disc.locate(lba) {
            Some((_, t, i)) => (t.number, i),
            None => (0, 0),
        };
        *seen_track.entry(track).or_default() += 1;
        if index == 0 {
            seen_pause += 1;
        }

        let adr = (de[12] & 0x0F) as usize;

        if subq::q_crc_ok(&de[12..24]) {
            adr_good[adr] += 1;
            // ADR 1 is the position frame our synthesizer builds; MCN/ISRC
            // frames (2/3) depend on data no dump has to carry, skip them.
            if adr == 1 {
                subq::synthesize(disc, lba, &mut syn);
                syn_seen += 1;
                match (0..10).find(|&i| de[12 + i] != syn[12 + i]) {
                    None => syn_match += 1,
                    Some(i) => {
                        syn_diff[i] += 1;
                        syn_bad.push(lba);
                    }
                }
            }
            continue;
        }

        bad.push(lba);
        bad_kind[k] += 1;
        adr_bad[adr] += 1;
        *bad_track.entry(track).or_default() += 1;
        if index == 0 {
            bad_pause += 1;
        }
        if de.iter().all(|&b| b == 0) {
            zero += 1;
        }
        // the bytes as they sit in the file, before deinterleaving: if *those*
        // carry a valid frame then we are deinterleaving something already
        // deinterleaved (the CloneCD-vs-Alcohol trap, doc 17 §2.6)
        subq::interleave(&de, &mut inter);
        if subq::q_crc_ok(&inter[12..24]) {
            raw_ok += 1;
        }
        // a corrupt CRC over an otherwise correct position is drive noise in
        // the CRC bytes themselves; a wrong position is a layout problem
        subq::synthesize(disc, lba, &mut syn);
        if de[19..22] == syn[19..22] {
            pos_ok += 1;
        }
        if samples.len() < 8 {
            let mut s = [0u8; 12];
            s.copy_from_slice(&de[12..24]);
            samples.push((lba, s));
        }
    }

    let pct = |a: u64, b: u64| if b == 0 { 0.0 } else { 100.0 * a as f64 / b as f64 };
    outln!("subchannel {first}..{end} of {n} in {:.1} s", t0.elapsed().as_secs_f64());
    outln!("  frames          {seen}");
    outln!("  bad CRC         {} ({:.3} %)", bad.len(), pct(bad.len() as u64, seen));
    outln!("    audio         {} of {} ({:.3} %)", bad_kind[0], seen_kind[0], pct(bad_kind[0], seen_kind[0]));
    outln!("    data          {} of {} ({:.3} %)", bad_kind[1], seen_kind[1], pct(bad_kind[1], seen_kind[1]));
    outln!("    in index 00   {} of {} ({:.3} %)", bad_pause, seen_pause, pct(bad_pause, seen_pause));
    outln!("    all-zero      {zero}");
    outln!("    valid if not deinterleaved  {raw_ok}");
    outln!("    position still ours         {pos_ok}");

    let adrs: Vec<String> = (0..16)
        .filter(|&a| adr_good[a] + adr_bad[a] > 0)
        .map(|a| format!("{a}: {}/{}", adr_bad[a], adr_good[a] + adr_bad[a]))
        .collect();
    outln!("  bad/total by ADR  {}", adrs.join("  "));

    let tracks: Vec<String> = seen_track
        .iter()
        .map(|(t, s)| format!("{t:02}:{:.2}%", pct(bad_track.get(t).copied().unwrap_or(0), *s)))
        .collect();
    outln!("  bad rate by track {}", tracks.join(" "));

    // clustering: consecutive bad LBAs are a systematic fault, singletons are noise
    let mut runs: Vec<(i32, i32)> = Vec::new();
    for &l in &bad {
        match runs.last_mut() {
            Some(r) if r.1 + 1 == l => r.1 = l,
            _ => runs.push((l, l)),
        }
    }
    let singles = runs.iter().filter(|(a, b)| a == b).count();
    let mut longest: Vec<&(i32, i32)> = runs.iter().collect();
    longest.sort_by_key(|(a, b)| -(b - a));
    let shown: Vec<String> = longest
        .iter()
        .take(6)
        .filter(|(a, b)| b > a)
        .map(|(a, b)| format!("{a}-{b} ({})", b - a + 1))
        .collect();
    outln!("  runs            {} ({singles} single sectors), longest {}", runs.len(), if shown.is_empty() { "none".into() } else { shown.join(" ") });

    outln!("  synthesizer     {syn_match} of {syn_seen} ADR 1 frames reproduced exactly ({:.3} %)", pct(syn_match, syn_seen));
    let names = ["ctl/adr", "track", "index", "rel m", "rel s", "rel f", "zero", "abs m", "abs s", "abs f"];
    let diffs: Vec<String> = (0..10).filter(|&i| syn_diff[i] > 0).map(|i| format!("{}: {}", names[i], syn_diff[i])).collect();
    if !diffs.is_empty() {
        outln!("    first mismatch  {}", diffs.join("  "));
        let mut r: Vec<(i32, i32)> = Vec::new();
        for &l in &syn_bad {
            match r.last_mut() {
                Some(x) if x.1 + 1 == l => x.1 = l,
                _ => r.push((l, l)),
            }
        }
        let shown: Vec<String> = r.iter().take(12).map(|(a, b)| if a == b { a.to_string() } else { format!("{a}-{b}") }).collect();
        outln!("    where           {}{}", shown.join(" "), if r.len() > 12 { format!(" … ({} ranges)", r.len()) } else { String::new() });
    }
    for (lba, q) in &samples {
        let hex: Vec<String> = q.iter().map(|b| format!("{b:02x}")).collect();
        outln!("    bad frame {lba:>8}  {}", hex.join(" "));
    }
    Ok(())
}

fn dump(disc: &Disc, what: &str, args: &[String]) -> Result<(), String> {
    let lba = |i: usize| -> Result<i32, String> {
        let s = args.get(i).ok_or("missing LBA")?;
        if let Some(h) = s.strip_prefix("0x") {
            i64::from_str_radix(h, 16).map(|v| v as i32).map_err(|e| e.to_string())
        } else {
            s.parse::<i64>().map(|v| v as i32).map_err(|e| e.to_string())
        }
    };
    match what {
        "readraw" => {
            let mut r = [0u8; 2352];
            disc.read_raw(lba(0)?, &mut r).map_err(|e| e.to_string())?;
            hex(&r);
        }
        "readcooked" => {
            let mut c = [0u8; 2048];
            disc.read_cooked(lba(0)?, &mut c).map_err(|e| e.to_string())?;
            hex(&c);
        }
        "sub" => {
            let mut s = [0u8; 96];
            disc.read_sub(lba(0)?, &mut s).map_err(|e| e.to_string())?;
            hex(&s);
        }
        "info" => {
            let i = disc.sector_info(lba(0)?).map_err(|e| e.to_string())?;
            outln!("{i:?}");
        }
        "toc" => {
            // toc <format> [msf] [start]
            let format = lba(0)? as u8;
            let msf = args.get(1).map(|s| s != "0").unwrap_or(false);
            let start = args.get(2).map(|s| s.parse::<u8>().unwrap_or(0)).unwrap_or(0);
            let mut v = Vec::new();
            libdisc::mmc::read_toc(disc, format, msf, start, &mut v).map_err(|e| e.to_string())?;
            hex(&v);
        }
        "subq" => {
            // subq <lba> [format] [msf] [track]
            let l = lba(0)?;
            let format = args.get(1).map(|s| s.parse::<u8>().unwrap_or(1)).unwrap_or(1);
            let msf = args.get(2).map(|s| s != "0").unwrap_or(false);
            let track = args.get(3).map(|s| s.parse::<u8>().unwrap_or(1)).unwrap_or(1);
            let mut v = Vec::new();
            libdisc::mmc::read_subchannel(disc, l, msf, true, format, track, 0x15, &mut v).map_err(|e| e.to_string())?;
            hex(&v);
        }
        "discinfo" => {
            let mut v = Vec::new();
            libdisc::mmc::read_disc_information(disc, &mut v).map_err(|e| e.to_string())?;
            hex(&v);
        }
        "readcd" => {
            // readcd <lba> <type> <byte9> <byte10>
            let l = lba(0)?;
            let ty = lba(1)? as u8;
            let b9 = lba(2)? as u8;
            let b10 = lba(3)? as u8;
            let mut buf = vec![0u8; 2744];
            let n = libdisc::mmc::read_cd_sector(disc, l, ty, b9, b10, &mut buf).map_err(|e| e.to_string())?;
            hex(&buf[..n]);
        }
        other => return Err(format!("unknown dump request {other}")),
    }
    Ok(())
}

/// Cooked ISO → MODE1/2352 cue/bin, plus one AUDIO track per WAVE file.
fn convert(input: &Path, out_cue: &Path, wavs: &[PathBuf]) -> Result<(), String> {
    let disc = Disc::open(input).map_err(|e| e.to_string())?;
    let stem = out_cue.file_stem().and_then(|s| s.to_str()).ok_or("bad output name")?.to_string();
    let bin_path = out_cue.with_extension("bin");
    let mut bin = fs::File::create(&bin_path).map_err(|e| e.to_string())?;
    let n = disc.sector_count() as i32;
    let mut r = [0u8; 2352];
    for lba in 0..n {
        disc.read_raw(lba, &mut r).map_err(|e| format!("sector {lba}: {e}"))?;
        bin.write_all(&r).map_err(|e| e.to_string())?;
    }
    let mut cue = format!("FILE \"{stem}.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    let mut pos = n;
    for (i, w) in wavs.iter().enumerate() {
        let (off, len) = libdisc::cue::wave_data(w).map_err(|e| e.to_string())?;
        let bytes = fs::read(w).map_err(|e| format!("{}: {}", w.display(), e))?;
        let pcm = &bytes[off as usize..(off + len) as usize];
        bin.write_all(pcm).map_err(|e| e.to_string())?;
        let pad = (2352 - pcm.len() % 2352) % 2352;
        bin.write_all(&vec![0u8; pad]).map_err(|e| e.to_string())?;
        let count = ((pcm.len() + pad) / 2352) as i32;
        cue.push_str(&format!("  TRACK {:02} AUDIO\n    PREGAP 00:02:00\n    INDEX 01 {}\n", i + 2, msf_str(pos)));
        pos += count;
    }
    fs::write(out_cue, cue).map_err(|e| e.to_string())?;
    outln!("{}: {} data sectors, {} audio tracks", out_cue.display(), n, wavs.len());
    Ok(())
}

fn usage() -> i32 {
    eprintln!("usage: discx selftest <outdir> | info <image> | scan <image> [first] [count] | subscan <image> [first] [count] | repair <image> <outdir> | dump <image> <what> [args] | convert <in.iso> <out.cue> [--audio a.wav ...]");
    2
}

/// The cooked view of a disc, sector by sector, as a file. For a folder
/// disc this is the oracle's input: an ISO 9660 reader that is not ours
/// (`bsdtar`) has to find the tree we laid out.
fn export(disc: &Disc, out: &Path) -> Result<(), String> {
    let mut f = fs::File::create(out).map_err(|e| format!("{}: {}", out.display(), e))?;
    let mut buf = [0u8; 2048];
    for lba in 0..disc.sector_count() as i32 {
        disc.read_cooked(lba, &mut buf).map_err(|e| format!("sector {lba}: {e}"))?;
        f.write_all(&buf).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// The fixture tree for the folder-disc checks: everything that makes an
/// ISO 9660 layout awkward, in one directory. Deterministic, and
/// rewritten from scratch on every run.
fn mktree(dir: &Path) -> Result<(), String> {
    let _ = fs::remove_dir_all(dir);
    let mkdir = |p: &Path| fs::create_dir_all(p).map_err(|e| format!("{}: {}", p.display(), e));
    let write = |p: &Path, b: &[u8]| fs::write(p, b).map_err(|e| format!("{}: {}", p.display(), e));
    mkdir(dir)?;

    write(&dir.join("readme.txt"), b"a folder in the drive\r\n")?;
    // Empty file: a directory record with no extent at all.
    write(&dir.join("EMPTY.BIN"), b"")?;
    // Exactly a sector, and one byte past one: the extent arithmetic and
    // the zero-filled tail sector.
    write(&dir.join("exact2048.bin"), &vec![0xA5u8; 2048])?;
    write(&dir.join("odd2049.bin"), &vec![0x5Au8; 2049])?;
    // Big enough to cross many extents, with content no zero-fill could fake.
    let mut st = 0x1234_5678u32;
    let mut big = vec![0u8; 3 << 20];
    for b in big.iter_mut() {
        *b = (xorshift(&mut st) >> 13) as u8;
    }
    write(&dir.join("big.bin"), &big)?;

    // Names an ISO 9660 identifier cannot hold, and a pair that mangles
    // to the same 8.3 name.
    write(&dir.join("Program Files.txt"), b"space")?;
    write(&dir.join("caf\u{e9}.txt"), b"accent")?;
    // A folder on Windows cannot hold a `*` at all, so there is no such
    // name to serve from one.
    if !cfg!(windows) {
        write(&dir.join("star*name.txt"), b"forbidden in joliet")?;
    }
    write(&dir.join("semi;colon.txt"), b"forbidden in joliet")?;
    write(&dir.join("collision-one.txt"), b"one")?;
    write(&dir.join("collision-two.txt"), b"two")?;
    // 62 characters plus ";1" is exactly Joliet's 64.
    let long: String = std::iter::repeat('L').take(58).collect();
    write(&dir.join(format!("{long}.txt")), b"long")?;

    // Nine levels: one past ISO 9660's limit, which is a warning and not
    // a refusal, and Windows reads it through the Joliet tree.
    let mut deep = dir.join("deep");
    for i in 1..=8 {
        deep = deep.join(format!("d{i}"));
    }
    mkdir(&deep)?;
    write(&deep.join("bottom.txt"), b"nine levels down")?;
    // An empty directory still needs its own extent.
    mkdir(&dir.join("empty-dir"))?;
    // More records than fit in one sector.
    let many = dir.join("many");
    mkdir(&many)?;
    for i in 0..300 {
        write(&many.join(format!("f{i:04}.dat")), format!("{i}\n").as_bytes())?;
    }
    Ok(())
}

/// The folder disc, through the C API: the layout is deterministic, the
/// volume describes itself, every file reads back exactly as it is on
/// the host, the padding is there, and a file that changes underneath an
/// open disc is a read error rather than a torn read.
fn check_dirdisc(dir: &Path) -> Result<(), String> {
    let src = dir.join("dirsrc");
    mktree(&src)?;
    let arg = format!("isodir:{}", src.display());
    let disc = Disc::open(Path::new(&arg)).map_err(|e| e.to_string())?;
    let c = CDisc::open(Path::new(&arg))?;
    expect("sector count", c.sector_count(), disc.sector_count())?;
    expect("tracks", c.track_count(), 1)?;
    let rd = |lba: u32| c.read_cooked(lba).map_err(|rc| format!("read_cooked {lba}: rc {rc}"));

    // The primary volume descriptor describes the disc the drive reports.
    let pvd = rd(16)?;
    expect("pvd type", pvd[0], 1)?;
    expect("pvd id", &pvd[1..6], b"CD001".as_slice())?;
    let space = u32::from_le_bytes([pvd[80], pvd[81], pvd[82], pvd[83]]);
    expect("volume space size", space, c.sector_count())?;
    expect("volume space size (be)", u32::from_be_bytes([pvd[84], pvd[85], pvd[86], pvd[87]]), space)?;
    expect("logical block size", u16::from_le_bytes([pvd[128], pvd[129]]), 2048)?;
    let svd = rd(17)?;
    expect("svd type", svd[0], 2)?;
    expect("joliet escape", &svd[88..91], b"%/E".as_slice())?;
    expect("terminator", rd(18)?[0], 255)?;

    // The names: one mangling of each kind, in the tree that carries it.
    let first_file = disc
        .tracks()
        .flat_map(|(_, t)| t.extents.iter())
        .find(|e| matches!(e.source, libdisc::Source::File { .. }))
        .map(|e| e.lba)
        .ok_or("no file extents")?;
    let meta: Vec<u8> = (16..first_file as u32).flat_map(|lba| c.read_cooked(lba).unwrap_or([0; 2048]).to_vec()).collect();
    for want in [b"COLLISIO.TXT;1".as_slice(), b"COLLIS~1.TXT;1".as_slice(), b"PROGRAM_.TXT;1".as_slice()] {
        if !meta.windows(want.len()).any(|w| w == want) {
            return Err(format!("primary tree has no {}", String::from_utf8_lossy(want)));
        }
    }
    let joliet: Vec<u8> = "café.txt;1".encode_utf16().flat_map(|u| u.to_be_bytes()).collect();
    if !meta.windows(joliet.len()).any(|w| w == joliet) {
        return Err("joliet tree has no café.txt".into());
    }
    let starred: Vec<u8> = "star_name.txt;1".encode_utf16().flat_map(|u| u.to_be_bytes()).collect();
    if !cfg!(windows) && !meta.windows(starred.len()).any(|w| w == starred) {
        return Err("joliet tree kept the * in star*name.txt".into());
    }

    // Every file extent, read through the C API, is the host file.
    let mut files = 0;
    for (_, t) in disc.tracks() {
        for e in &t.extents {
            let libdisc::Source::File { file, .. } = e.source else { continue };
            let p = disc.file(file);
            let want = fs::read(&p.path).map_err(|e| format!("{}: {}", p.path.display(), e))?;
            let mut got = Vec::with_capacity(want.len());
            for lba in e.lba..e.end() {
                got.extend_from_slice(&rd(lba as u32)?);
            }
            got.truncate(want.len());
            if got != want {
                return Err(format!("{}: content differs through the disc", p.path.display()));
            }
            files += 1;
        }
    }
    if files < 300 {
        return Err(format!("only {files} file extents; the fixture tree has more"));
    }

    // The tail padding a guest's read-ahead runs into.
    let last = c.sector_count() - 1;
    if rd(last)? != [0u8; 2048] {
        return Err("the last sector is not padding".into());
    }

    // Two opens of an unchanged tree are the same image, byte for byte.
    let iso1 = dir.join("dirsrc-1.iso");
    let iso2 = dir.join("dirsrc-2.iso");
    export(&disc, &iso1)?;
    let again = Disc::open(Path::new(&arg)).map_err(|e| e.to_string())?;
    export(&again, &iso2)?;
    let (a, b) = (fs::read(&iso1).map_err(|e| e.to_string())?, fs::read(&iso2).map_err(|e| e.to_string())?);
    if a != b {
        return Err("two opens of the same tree produced different images".into());
    }

    // A file that changes under an open disc: a read error, not a torn read.
    let victim = src.join("readme.txt");
    let victim_lba = disc
        .tracks()
        .flat_map(|(_, t)| t.extents.iter())
        .find_map(|e| match e.source {
            libdisc::Source::File { file, .. } if disc.file(file).path == victim => Some(e.lba),
            _ => None,
        })
        .ok_or("readme.txt has no extent")?;
    fs::write(&victim, b"changed underneath the drive, and longer than it was\r\n").map_err(|e| e.to_string())?;
    match c.read_cooked(victim_lba as u32) {
        Err(capi::LIBDISC_EMEDIUM) => {}
        Err(rc) => return Err(format!("changed file: rc {rc}, wanted EMEDIUM")),
        Ok(_) => return Err("a file changed under the disc was served anyway".into()),
    }

    // What a shared folder is refused for, and what it is only warned
    // about: a tree nobody meant to share, and a file no directory record
    // can address. Both are the kind of thing a user's own folder has in
    // it, so both have to say which path they mean.
    let deep = dir.join("dirdeep");
    let _ = fs::remove_dir_all(&deep);
    let mut p = deep.clone();
    for i in 0..32 {
        p = p.join(format!("d{i}"));
    }
    fs::create_dir_all(&p).map_err(|e| e.to_string())?;
    match Disc::open(&deep) {
        Err(e) if e.to_string().contains("deeper than") => {}
        Err(e) => return Err(format!("a 32-level tree: {e}")),
        Ok(_) => return Err("a 32-level tree was served".into()),
    }
    let _ = fs::remove_dir_all(&deep);

    let huge = dir.join("dirhuge");
    let _ = fs::remove_dir_all(&huge);
    fs::create_dir_all(&huge).map_err(|e| e.to_string())?;
    // Sparse: the bytes are never written, only the size is claimed.
    let big = fs::File::create(huge.join("huge.bin")).map_err(|e| e.to_string())?;
    if big.set_len(4 << 30).is_ok() {
        match Disc::open(&huge) {
            Err(e) if e.to_string().contains("too large") => {}
            Err(e) => return Err(format!("a 4 GiB file: {e}")),
            Ok(_) => return Err("a 4 GiB file was given an extent".into()),
        }
    }
    drop(big);
    let _ = fs::remove_dir_all(&huge);

    // A folder bigger than any disc: refused by size, and named, rather
    // than laid out into sectors nothing can address. Pointing the shelf
    // at a directory of disc dumps is how a user meets this.
    let over = dir.join("dirover");
    let _ = fs::remove_dir_all(&over);
    fs::create_dir_all(&over).map_err(|e| e.to_string())?;
    let mut sparse = Vec::new();
    for i in 0..3 {
        let f = fs::File::create(over.join(format!("part{i}.bin"))).map_err(|e| e.to_string())?;
        // Just under the single-extent limit each, so what refuses this
        // folder is its total and not one file in it.
        if f.set_len(3 << 30).is_err() {
            sparse.clear();
            break;
        }
        sparse.push(f);
    }
    if !sparse.is_empty() {
        match Disc::open(&over) {
            Err(e) if e.to_string().contains("a disc holds at most") => {}
            Err(e) => return Err(format!("a 9 GiB folder: {e}")),
            Ok(_) => return Err("a folder bigger than a disc was served".into()),
        }
    }
    drop(sparse);
    let _ = fs::remove_dir_all(&over);

    // And the other side of that line: a folder past an 80-minute CD is
    // laid out and served, because the drive reports it as a DVD-ROM
    // (`CD_MAX_SECTORS` in QEMU's atapi.c is 360,000 of our sectors).
    // The file is sparse, so this costs no disk and reads as zeros.
    let dvd = dir.join("dirdvd");
    let _ = fs::remove_dir_all(&dvd);
    fs::create_dir_all(&dvd).map_err(|e| e.to_string())?;
    fs::write(dvd.join("readme.txt"), b"past the CD ceiling\r\n").map_err(|e| e.to_string())?;
    let filler = fs::File::create(dvd.join("filler.bin")).map_err(|e| e.to_string())?;
    if filler.set_len(1000 << 20).is_ok() {
        drop(filler);
        let c = CDisc::open(&dvd)?;
        if c.sector_count() <= 360_000 {
            return Err(format!("a 1000 MiB folder came out {} sectors", c.sector_count()));
        }
        let pvd = c.read_cooked(16).map_err(|rc| format!("pvd: rc {rc}"))?;
        expect("dvd pvd id", &pvd[1..6], b"CD001".as_slice())?;
        expect(
            "dvd volume space size",
            u32::from_le_bytes([pvd[80], pvd[81], pvd[82], pvd[83]]),
            c.sector_count(),
        )?;
        // The far end of the volume answers too: the tail padding is the
        // last thing a guest's read-ahead runs into, past 878 MiB where a
        // CD's own addressing would have stopped.
        let last = c.sector_count() - 1;
        if c.read_cooked(last).map_err(|rc| format!("last sector: rc {rc}"))? != [0u8; 2048] {
            return Err("the last sector of a DVD-sized folder is not padding".into());
        }
    }
    let _ = fs::remove_dir_all(&dvd);

    // A name that is not UTF-8 cannot be a Joliet identifier at all, so
    // it is skipped with a warning rather than mangled into something
    // the user never named.
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        let odd = dir.join("dirodd");
        let _ = fs::remove_dir_all(&odd);
        fs::create_dir_all(&odd).map_err(|e| e.to_string())?;
        fs::write(odd.join("keep.txt"), b"kept").map_err(|e| e.to_string())?;
        let bad = odd.join(std::ffi::OsStr::from_bytes(b"bad\xff.txt"));
        if fs::write(&bad, b"dropped").is_ok() {
            let d = Disc::open(&odd).map_err(|e| e.to_string())?;
            let files = d.tracks().flat_map(|(_, t)| t.extents.iter()).filter(|e| matches!(e.source, libdisc::Source::File { .. })).count();
            if files != 1 {
                return Err(format!("a name that is not UTF-8 was not skipped ({files} file extents)"));
            }
        }
        let _ = fs::remove_dir_all(&odd);
    }

    // A symlink loop is refused rather than walked.
    let loopdir = dir.join("dirloop");
    let _ = fs::remove_dir_all(&loopdir);
    fs::create_dir_all(loopdir.join("sub")).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        let link = loopdir.join("sub").join("up");
        let _ = std::os::unix::fs::symlink("..", &link);
        match Disc::open(&loopdir) {
            Err(e) if e.to_string().contains("symlink loop") => {}
            Err(e) => return Err(format!("symlink loop: {e}")),
            Ok(_) => return Err("a symlink loop was walked".into()),
        }
    }
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        std::process::exit(usage());
    }
    let rc = match args[1].as_str() {
        "selftest" => selftest(Path::new(&args[2])),
        "scan" => match Disc::open(Path::new(&args[2])) {
            Ok(d) => {
                let first = args.get(3).and_then(|v| v.parse().ok()).unwrap_or(0);
                let count = args.get(4).and_then(|v| v.parse().ok());
                match scan(&d, first, count) {
                    Ok(()) => 0,
                    Err(e) => {
                        eprintln!("{e}");
                        1
                    }
                }
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "repair" => {
            if args.len() < 4 {
                std::process::exit(usage());
            }
            let image = Path::new(&args[2]);
            match Disc::open(image) {
                Ok(d) => match repair(&d, image, Path::new(&args[3])) {
                    Ok(r) => {
                        outln!("{} -> {}", image.display(), r.out_image.display());
                        outln!("  sectors repaired  {} (EDC and ECC regenerated, user data untouched)", r.sectors);
                        if r.no_sync > 0 {
                            outln!("  left alone        {} without a sync pattern (run-out, not a band)", r.no_sync);
                        }
                        if r.unstored > 0 {
                            outln!("  left alone        {} failing in a layout that stores no parity", r.unstored);
                        }
                        outln!("  the copy is the negative control: a check that reads the band must now refuse it");
                        0
                    }
                    Err(e) => {
                        eprintln!("{e}");
                        1
                    }
                },
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            }
        }
        "subscan" => match Disc::open(Path::new(&args[2])) {
            Ok(d) => {
                let first = args.get(3).and_then(|v| v.parse().ok()).unwrap_or(0);
                let count = args.get(4).and_then(|v| v.parse().ok());
                match subscan(&d, first, count) {
                    Ok(()) => 0,
                    Err(e) => {
                        eprintln!("{e}");
                        1
                    }
                }
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "info" => match Disc::open(Path::new(&args[2])) {
            Ok(d) => {
                info(&d);
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "dump" if args.len() >= 4 => match Disc::open(Path::new(&args[2])) {
            Ok(d) => match dump(&d, &args[3], &args[4..]) {
                Ok(()) => 0,
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            },
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "export" if args.len() >= 4 => match Disc::open(Path::new(&args[2])) {
            Ok(d) => match export(&d, Path::new(&args[3])) {
                Ok(()) => {
                    outln!("{}: {} sectors", args[3], d.sector_count());
                    0
                }
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            },
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "mktree" => match mktree(Path::new(&args[2])) {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "convert" if args.len() >= 4 => {
            let mut wavs = Vec::new();
            let mut i = 4;
            while i < args.len() {
                if args[i] == "--audio" {
                    i += 1;
                } else {
                    wavs.push(PathBuf::from(&args[i]));
                    i += 1;
                }
            }
            match convert(Path::new(&args[2]), Path::new(&args[3]), &wavs) {
                Ok(()) => 0,
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            }
        }
        _ => usage(),
    };
    std::process::exit(rc);
}
