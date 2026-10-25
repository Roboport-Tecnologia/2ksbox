//! Cue sheet parser (doc 17 §2.3): `CATALOG`, `FILE … BINARY|MOTOROLA|WAVE`,
//! `TRACK`, `FLAGS`, `ISRC`, `PREGAP`, `INDEX`, `POSTGAP`; `REM` ignored.
//! INDEX times are file-relative; PREGAP/POSTGAP sectors are not in the
//! file; files are concatenated; track 1 starts at LBA 0.

use std::path::Path;

use crate::msf::{Msf, FRAMES_PER_SECOND};
use crate::{Disc, Error, Extent, Layout, Result, Session, Source, Track, TrackMode};

struct CueTrack {
    number: u8,
    mode: TrackMode,
    layout: Layout,
    control: u8,
    isrc: Option<[u8; 12]>,
    /// (index, file-relative frame)
    indices: Vec<(u8, i32)>,
    pregap: i32,
    postgap: i32,
}

struct CueFile {
    payload: usize,
    /// byte offset of the first sector (WAVE data chunk)
    data_offset: u64,
    data_len: u64,
    swap: bool,
    tracks: Vec<usize>,
}

fn tokens(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quoted = false;
    let mut had = false;
    for c in line.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                had = true;
            }
            c if c.is_whitespace() && !quoted => {
                if had || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                    had = false;
                }
            }
            c => cur.push(c),
        }
    }
    if had || !cur.is_empty() {
        out.push(cur);
    }
    out
}

fn parse_msf(s: &str, what: &str, line: usize) -> Result<i32> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 3 {
        return Err(Error::Invalid(format!("line {line}: {what}: bad time {s:?}")));
    }
    let mut v = [0u32; 3];
    for (i, p) in parts.iter().enumerate() {
        v[i] = p.parse().map_err(|_| Error::Invalid(format!("line {line}: {what}: bad time {s:?}")))?;
    }
    if v[1] >= 60 || v[2] >= FRAMES_PER_SECOND {
        return Err(Error::Invalid(format!("line {line}: {what}: bad time {s:?}")));
    }
    Ok(Msf { m: v[0] as u8, s: v[1] as u8, f: v[2] as u8 }.to_lba() + crate::msf::MSF_OFFSET)
}

/// The RIFF `data` chunk of a WAVE file: (offset, length). Requires
/// 44100 Hz, 16-bit, stereo.
pub fn wave_data(path: &Path) -> Result<(u64, u64)> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path)?;
    let mut hdr = [0u8; 12];
    f.read_exact(&mut hdr)?;
    if &hdr[..4] != b"RIFF" || &hdr[8..12] != b"WAVE" {
        return Err(Error::Invalid(format!("{}: not a RIFF WAVE file", path.display())));
    }
    let mut pos = 12u64;
    let mut fmt_ok = false;
    loop {
        let mut ch = [0u8; 8];
        if f.read_exact(&mut ch).is_err() {
            return Err(Error::Invalid(format!("{}: no data chunk", path.display())));
        }
        let len = u32::from_le_bytes([ch[4], ch[5], ch[6], ch[7]]) as u64;
        pos += 8;
        if &ch[..4] == b"fmt " {
            // a PCM format chunk is 16 bytes (18 or 40 with extensions)
            if !(16..=4096).contains(&len) {
                return Err(Error::Invalid(format!("{}: a {len}-byte format chunk", path.display())));
            }
            let mut fm = vec![0u8; len as usize];
            f.read_exact(&mut fm)?;
            let tag = u16::from_le_bytes([fm[0], fm[1]]);
            let channels = u16::from_le_bytes([fm[2], fm[3]]);
            let rate = u32::from_le_bytes([fm[4], fm[5], fm[6], fm[7]]);
            let bits = u16::from_le_bytes([fm[14], fm[15]]);
            if tag != 1 || channels != 2 || rate != 44100 || bits != 16 {
                return Err(Error::Invalid(format!(
                    "{}: WAVE must be PCM 44100 Hz 16-bit stereo (is tag {tag}, {channels} ch, {rate} Hz, {bits} bit)",
                    path.display()
                )));
            }
            fmt_ok = true;
        } else if &ch[..4] == b"data" {
            if !fmt_ok {
                return Err(Error::Invalid(format!("{}: data chunk before fmt", path.display())));
            }
            return Ok((pos, len));
        } else {
            f.seek(SeekFrom::Current(len as i64))?;
        }
        pos += len;
        if len % 2 == 1 {
            f.seek(SeekFrom::Current(1))?;
            pos += 1;
        }
    }
}

pub fn parse(text: &str, cue_path: &Path) -> Result<Disc> {
    let mut disc = Disc::new();
    let mut files: Vec<CueFile> = Vec::new();
    let mut tracks: Vec<CueTrack> = Vec::new();
    for (n, raw) in text.lines().enumerate() {
        let line = n + 1;
        let tk = tokens(raw);
        if tk.is_empty() {
            continue;
        }
        let kw = tk[0].to_ascii_uppercase();
        let arg = |i: usize| -> Result<&str> {
            tk.get(i).map(|s| s.as_str()).ok_or_else(|| Error::Invalid(format!("line {line}: {kw} needs an argument")))
        };
        match kw.as_str() {
            "REM" | "TITLE" | "PERFORMER" | "SONGWRITER" | "CDTEXTFILE" => {
                if kw == "REM" && tk.get(1).map(|s| s.eq_ignore_ascii_case("SESSION")).unwrap_or(false) {
                    if tk.get(2).map(|s| s != "1" && s != "01").unwrap_or(false) {
                        return Err(Error::Invalid(format!("line {line}: multi-session cue sheets (REM SESSION) are not supported; use a CloneCD image")));
                    }
                }
            }
            "CATALOG" => {
                let s = arg(1)?;
                if s.len() != 13 || !s.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(Error::Invalid(format!("line {line}: CATALOG must be 13 digits")));
                }
                let mut m = [0u8; 13];
                m.copy_from_slice(s.as_bytes());
                disc.mcn = Some(m);
            }
            "FILE" => {
                let name = arg(1)?;
                let kind = tk.get(2).map(|s| s.to_ascii_uppercase()).unwrap_or_else(|| "BINARY".into());
                let path = crate::resolve_payload(cue_path, name)?;
                let payload = disc.add_file(&path)?;
                let (data_offset, data_len, swap) = match kind.as_str() {
                    "BINARY" => (0, disc.file(payload).len, false),
                    "MOTOROLA" => (0, disc.file(payload).len, true),
                    "WAVE" => {
                        let (o, l) = wave_data(&path)?;
                        (o, l, false)
                    }
                    other => return Err(Error::Invalid(format!("line {line}: FILE type {other} not supported (BINARY, MOTOROLA, WAVE)"))),
                };
                files.push(CueFile { payload, data_offset, data_len, swap, tracks: Vec::new() });
            }
            "TRACK" => {
                if files.is_empty() {
                    return Err(Error::Invalid(format!("line {line}: TRACK before any FILE")));
                }
                let number: u8 = arg(1)?.parse().map_err(|_| Error::Invalid(format!("line {line}: bad track number")))?;
                if number == 0 || number > 99 {
                    return Err(Error::Invalid(format!("line {line}: track number {number} out of 1..99")));
                }
                if let Some(prev) = tracks.last() {
                    if number != prev.number + 1 {
                        return Err(Error::Invalid(format!("line {line}: track {number} does not follow track {}", prev.number)));
                    }
                } else if number != 1 {
                    return Err(Error::Invalid(format!("line {line}: the first track must be track 1")));
                }
                let (mode, layout) = match arg(2)?.to_ascii_uppercase().as_str() {
                    "AUDIO" => (TrackMode::Audio, Layout::Raw2352),
                    "MODE1/2048" => (TrackMode::Mode1, Layout::Cooked2048),
                    "MODE1/2352" => (TrackMode::Mode1, Layout::Raw2352),
                    "MODE2/2336" => (TrackMode::Mode2, Layout::Mode2_2336),
                    "MODE2/2352" => (TrackMode::Mode2, Layout::Raw2352),
                    "CDI/2336" => (TrackMode::Mode2Formless, Layout::Mode2_2336),
                    "CDI/2352" => (TrackMode::Mode2Formless, Layout::Raw2352),
                    other => return Err(Error::Invalid(format!("line {line}: track type {other} not supported"))),
                };
                let control = if mode.is_data() { 0x4 } else { 0x0 };
                files.last_mut().unwrap().tracks.push(tracks.len());
                tracks.push(CueTrack { number, mode, layout, control, isrc: None, indices: Vec::new(), pregap: 0, postgap: 0 });
            }
            "FLAGS" | "ISRC" | "PREGAP" | "POSTGAP" | "INDEX" => {
                let t = tracks.last_mut().ok_or_else(|| Error::Invalid(format!("line {line}: {kw} before any TRACK")))?;
                match kw.as_str() {
                    "FLAGS" => {
                        for f in &tk[1..] {
                            match f.to_ascii_uppercase().as_str() {
                                "PRE" => t.control |= 0x1,
                                "DCP" => t.control |= 0x2,
                                "4CH" => t.control |= 0x8,
                                "SCMS" => {}
                                other => return Err(Error::Invalid(format!("line {line}: unknown flag {other}"))),
                            }
                        }
                    }
                    "ISRC" => {
                        let s = arg(1)?;
                        if s.len() != 12 {
                            return Err(Error::Invalid(format!("line {line}: ISRC must be 12 characters")));
                        }
                        let mut i = [0u8; 12];
                        i.copy_from_slice(s.to_ascii_uppercase().as_bytes());
                        t.isrc = Some(i);
                    }
                    "PREGAP" => t.pregap = parse_msf(arg(1)?, "PREGAP", line)?,
                    "POSTGAP" => t.postgap = parse_msf(arg(1)?, "POSTGAP", line)?,
                    _ => {
                        let idx: u8 = arg(1)?.parse().map_err(|_| Error::Invalid(format!("line {line}: bad index number")))?;
                        let frame = parse_msf(arg(2)?, "INDEX", line)?;
                        if let Some(last) = t.indices.last() {
                            if idx <= last.0 || frame < last.1 {
                                return Err(Error::Invalid(format!("line {line}: INDEX {idx:02} out of order")));
                            }
                        }
                        t.indices.push((idx, frame));
                    }
                }
            }
            other => return Err(Error::Invalid(format!("line {line}: unknown keyword {other}"))),
        }
    }
    if tracks.is_empty() {
        return Err(Error::Invalid("no TRACK in cue sheet".into()));
    }
    for t in &tracks {
        if !t.indices.iter().any(|i| i.0 == 1) {
            return Err(Error::Invalid(format!("track {} has no INDEX 01", t.number)));
        }
    }

    // Lay the tracks out: file by file, track by track, with the running
    // absolute LBA carrying across files and across PREGAP/POSTGAP sectors.
    let mut out_tracks: Vec<Track> = Vec::new();
    let mut running: i32 = 0; // absolute LBA of the next sector to place
    for (fi, f) in files.iter().enumerate() {
        if f.tracks.is_empty() {
            return Err(Error::Invalid(format!("FILE {} has no tracks", disc.file(f.payload).path.display())));
        }
        // file-relative first sector of each track in this file; the first
        // track of the first file owns everything from frame 0
        let mut firsts: Vec<i32> = f.tracks.iter().map(|&ti| tracks[ti].indices[0].1).collect();
        if fi == 0 {
            firsts[0] = 0;
        }
        let mut byte_off: u64 = f.data_offset;
        let file_base = running;
        let mut shift = 0i32; // pregap/postgap sectors inserted so far within this file
        for (k, &ti) in f.tracks.iter().enumerate() {
            let t = &tracks[ti];
            let stride = t.layout.stride() as u64;
            let first = firsts[k];
            let count: u32 = if k + 1 < f.tracks.len() {
                let next = firsts[k + 1];
                if next < first {
                    return Err(Error::Invalid(format!("track {} starts before track {} in the file", t.number + 1, t.number)));
                }
                (next - first) as u32
            } else {
                let Some(rest) = (f.data_offset + f.data_len).checked_sub(byte_off) else {
                    return Err(Error::Invalid(format!(
                        "{}: track {} starts past the end of the file",
                        disc.file(f.payload).path.display(),
                        t.number
                    )));
                };
                if rest % stride != 0 {
                    return Err(Error::Invalid(format!(
                        "{}: size {} is not a multiple of {} bytes per sector for track {}",
                        disc.file(f.payload).path.display(),
                        f.data_len,
                        stride,
                        t.number
                    )));
                }
                (rest / stride) as u32
            };
            let mut extents = Vec::new();
            let mut indices: Vec<(u8, i32)> = Vec::new();
            if t.pregap > 0 {
                let lba = file_base + shift + first;
                extents.push(Extent { lba, count: t.pregap as u32, source: if t.mode.is_data() { Source::ZeroData } else { Source::Silence }, sub: None });
                indices.push((0, lba));
                shift += t.pregap;
            }
            let in_file_lba = file_base + shift + first;
            for &(idx, frame) in &t.indices {
                if frame < first {
                    return Err(Error::Invalid(format!("track {}: INDEX {idx:02} precedes the track's first sector", t.number)));
                }
                let lba = file_base + shift + frame;
                if idx == 0 && indices.iter().any(|i| i.0 == 0) {
                    continue; // PREGAP already opened index 0
                }
                indices.push((idx, lba));
            }
            if first < t.indices[0].1 && !indices.iter().any(|i| i.0 == 0) {
                // the first track's leading sectors before its INDEX 01 are its index 0
                indices.insert(0, (0, in_file_lba));
            }
            if count > 0 {
                extents.push(Extent {
                    lba: in_file_lba,
                    count,
                    source: Source::File { file: f.payload, offset: byte_off, layout: t.layout, swap: f.swap, eof_pad: false },
                    sub: None,
                });
            }
            byte_off += count as u64 * stride;
            let mut end = in_file_lba + count as i32;
            if t.postgap > 0 {
                extents.push(Extent { lba: end, count: t.postgap as u32, source: if t.mode.is_data() { Source::ZeroData } else { Source::Silence }, sub: None });
                end += t.postgap;
                shift += t.postgap;
            }
            let start_lba = indices.iter().find(|i| i.0 == 1).unwrap().1;
            if start_lba >= end && count > 0 {
                return Err(Error::Invalid(format!("track {}: INDEX 01 at LBA {start_lba} is past the track's end {end}", t.number)));
            }
            let track = Track { number: t.number, mode: t.mode, control: t.control, isrc: t.isrc, indices, start_lba, end_lba: end, extents };
            if let Some(prev) = out_tracks.last() {
                if track.first_lba() != prev.end_lba {
                    return Err(Error::Invalid(format!("track {} does not start where track {} ends", track.number, prev.number)));
                }
            }
            out_tracks.push(track);
            running = end;
        }
        let _ = fi;
    }
    if out_tracks[0].first_lba() != 0 {
        return Err(Error::Invalid("track 1 does not start at LBA 0".into()));
    }
    let leadout = running;
    disc.sessions.push(Session { number: 1, tracks: out_tracks, leadout_lba: leadout });
    Ok(disc)
}

pub fn open(path: &Path) -> Result<Disc> {
    let bytes = std::fs::read(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
    let text = String::from_utf8_lossy(&bytes);
    parse(&text, path).map_err(|e| match e {
        Error::Invalid(s) => Error::Invalid(format!("{}: {}", path.display(), s)),
        e => e,
    })
}
