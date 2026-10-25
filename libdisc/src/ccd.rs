//! CloneCD parser (doc 17 §2.4): `.ccd` (an INI file) next to `.img` (2352
//! bytes per sector from LBA 0, every session) and an optional `.sub` (96
//! bytes per sector, deinterleaved P..W). Every `[Entry]` is kept verbatim
//! in `Disc::raw_toc` for READ TOC format 2.

use std::collections::BTreeMap;
use std::path::Path;

use crate::{Disc, Error, Extent, Layout, Result, Session, Source, SubSource, TocEntry, Track, TrackMode};

type Section = BTreeMap<String, String>;

fn parse_ini(text: &str) -> Vec<(String, Section)> {
    let mut out: Vec<(String, Section)> = Vec::new();
    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with(';') || line.starts_with('#') {
            continue;
        }
        if let Some(name) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            out.push((name.trim().to_ascii_uppercase(), Section::new()));
        } else if let Some((k, v)) = line.split_once('=') {
            if let Some((_, sec)) = out.last_mut() {
                sec.insert(k.trim().to_ascii_uppercase(), v.trim().to_string());
            }
        }
    }
    out
}

fn int(sec: &Section, key: &str, what: &str) -> Result<i64> {
    let v = sec.get(key).ok_or_else(|| Error::Invalid(format!("{what}: missing {key}")))?;
    let (neg, body) = match v.strip_prefix('-') {
        Some(b) => (true, b),
        None => (false, v.as_str()),
    };
    let n = if let Some(h) = body.strip_prefix("0x").or_else(|| body.strip_prefix("0X")) {
        i64::from_str_radix(h, 16)
    } else {
        body.parse::<i64>()
    }
    .map_err(|_| Error::Invalid(format!("{what}: bad number {key}={v}")))?;
    Ok(if neg { -n } else { n })
}

fn int_or(sec: &Section, key: &str, default: i64) -> i64 {
    int(sec, key, "").unwrap_or(default)
}

pub fn parse(text: &str, ccd_path: &Path) -> Result<Disc> {
    let ini = parse_ini(text);
    if !ini.iter().any(|(n, _)| n == "CLONECD") {
        return Err(Error::Invalid("no [CloneCD] section".into()));
    }
    let mut disc = Disc::new();
    let empty = Section::new();
    let disc_sec = ini.iter().find(|(n, _)| n == "DISC").map(|(_, s)| s).unwrap_or(&empty);
    if int_or(disc_sec, "DATATRACKSSCRAMBLED", 0) != 0 {
        return Err(Error::Invalid("DataTracksScrambled=1 images are not supported (dump again without scrambling)".into()));
    }
    if let Some(c) = disc_sec.get("CATALOG") {
        if c.len() == 13 && c.bytes().all(|b| b.is_ascii_digit()) {
            let mut m = [0u8; 13];
            m.copy_from_slice(c.as_bytes());
            disc.mcn = Some(m);
        }
    }

    // payload files: <stem>.img (required), <stem>.sub (optional)
    let stem = ccd_path.file_stem().and_then(|s| s.to_str()).unwrap_or("disc");
    let img_path = crate::resolve_payload(ccd_path, &format!("{stem}.img"))?;
    let img = disc.add_file(&img_path)?;
    let img_len = disc.file(img).len;
    if img_len % 2352 != 0 {
        return Err(Error::Invalid(format!("{}: size {} is not a multiple of 2352", img_path.display(), img_len)));
    }
    let img_sectors = (img_len / 2352) as i32;
    let sub = match crate::resolve_payload(ccd_path, &format!("{stem}.sub")) {
        Ok(p) => Some(disc.add_file(&p)?),
        Err(_) => None,
    };

    // entries, in file order
    let mut entries: Vec<TocEntry> = Vec::new();
    for (name, sec) in &ini {
        if !name.starts_with("ENTRY ") {
            continue;
        }
        let e = TocEntry {
            session: int(sec, "SESSION", name)? as u8,
            point: int(sec, "POINT", name)? as u8,
            adr: int(sec, "ADR", name)? as u8,
            control: int(sec, "CONTROL", name)? as u8,
            min: int_or(sec, "AMIN", 0) as u8,
            sec: int_or(sec, "ASEC", 0) as u8,
            frame: int_or(sec, "AFRAME", 0) as u8,
            zero: int_or(sec, "ZERO", 0) as u8,
            pmin: int_or(sec, "PMIN", 0) as u8,
            psec: int_or(sec, "PSEC", 0) as u8,
            pframe: int_or(sec, "PFRAME", 0) as u8,
        };
        entries.push(e);
    }
    if entries.is_empty() {
        return Err(Error::Invalid("no [Entry] sections".into()));
    }
    // PLBA per entry (needed for tracks and lead-outs; not part of the raw TOC)
    let plba: Vec<i64> = ini
        .iter()
        .filter(|(n, _)| n.starts_with("ENTRY "))
        .map(|(n, s)| int(s, "PLBA", n).unwrap_or_else(|_| -1))
        .collect();

    // per-track sections
    let mut track_secs: BTreeMap<u8, &Section> = BTreeMap::new();
    for (name, sec) in &ini {
        if let Some(n) = name.strip_prefix("TRACK ") {
            if let Ok(n) = n.trim().parse::<u8>() {
                track_secs.insert(n, sec);
            }
        }
    }

    // sessions from the entries
    let mut session_numbers: Vec<u8> = entries.iter().map(|e| e.session).collect();
    session_numbers.sort_unstable();
    session_numbers.dedup();
    let mut sessions: Vec<Session> = Vec::new();
    let mut last_end = 0i32;
    for &sn in &session_numbers {
        let leadout = entries
            .iter()
            .zip(&plba)
            .find(|(e, _)| e.session == sn && e.point == 0xA2 && e.adr == 1)
            .map(|(_, &p)| p)
            .ok_or_else(|| Error::Invalid(format!("session {sn}: no lead-out entry (Point=0xa2)")))?;
        let leadout = leadout as i32;
        let mut tracks: Vec<Track> = Vec::new();
        let mut track_entries: Vec<(&TocEntry, i64)> = entries.iter().zip(&plba).filter(|(e, _)| e.session == sn && e.adr == 1 && (1..=99).contains(&e.point)).map(|(e, &p)| (e, p)).collect();
        track_entries.sort_by_key(|(e, _)| e.point);
        for (i, (e, p)) in track_entries.iter().enumerate() {
            if *p < 0 {
                return Err(Error::Invalid(format!("track {}: missing PLBA", e.point)));
            }
            let start = *p as i32;
            let tsec = track_secs.get(&e.point).copied().unwrap_or(&empty);
            let mode = match int_or(tsec, "MODE", if e.control & 0x4 != 0 { 1 } else { 0 }) {
                0 => TrackMode::Audio,
                1 => TrackMode::Mode1,
                2 => TrackMode::Mode2,
                m => return Err(Error::Invalid(format!("track {}: MODE={m} not supported", e.point))),
            };
            let mut indices: Vec<(u8, i32)> = Vec::new();
            for (k, v) in tsec.iter() {
                if let Some(n) = k.strip_prefix("INDEX ") {
                    if let (Ok(n), Ok(lba)) = (n.trim().parse::<u8>(), v.parse::<i32>()) {
                        indices.push((n, lba));
                    }
                }
            }
            indices.sort_unstable();
            if !indices.iter().any(|i| i.0 == 1) {
                indices.push((1, start));
                indices.sort_unstable();
            }
            let first = indices[0].1;
            if i == 0 && sessions.is_empty() && first > 0 && !indices.iter().any(|i| i.0 == 0) {
                indices.insert(0, (0, 0));
            }
            let first = indices[0].1;
            if first < last_end {
                return Err(Error::Invalid(format!("track {} starts at {first}, before the previous track ends at {last_end}", e.point)));
            }
            let end = match track_entries.get(i + 1) {
                Some((ne, np)) => {
                    let nsec = track_secs.get(&ne.point).copied().unwrap_or(&empty);
                    let nfirst = nsec.get("INDEX 0").and_then(|v| v.parse::<i32>().ok()).unwrap_or(*np as i32);
                    nfirst
                }
                None => leadout,
            };
            if first < 0 || end < first {
                return Err(Error::Invalid(format!("track {}: sectors {first} to {end}", e.point)));
            }
            if end > img_sectors {
                return Err(Error::Invalid(format!("track {}: ends at {end} but {} has {img_sectors} sectors", e.point, img_path.display())));
            }
            let isrc = tsec.get("ISRC").and_then(|s| if s.len() == 12 { let mut a = [0u8; 12]; a.copy_from_slice(s.to_ascii_uppercase().as_bytes()); Some(a) } else { None });
            let extents = vec![Extent {
                lba: first,
                count: (end - first) as u32,
                source: Source::File { file: img, offset: first as u64 * 2352, layout: Layout::Raw2352, swap: false, eof_pad: false },
                sub: sub.map(|f| SubSource::File { file: f, offset: first as u64 * 96 }),
            }];
            tracks.push(Track { number: e.point, mode, control: e.control, isrc, indices, start_lba: start, end_lba: end, extents });
            last_end = end;
        }
        if tracks.is_empty() {
            return Err(Error::Invalid(format!("session {sn}: no track entries")));
        }
        sessions.push(Session { number: sn, tracks, leadout_lba: leadout });
        last_end = leadout;
    }
    if sessions[0].tracks[0].first_lba() != 0 {
        return Err(Error::Invalid("track 1 does not start at LBA 0".into()));
    }
    disc.sessions = sessions;
    disc.raw_toc = Some(entries);
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
