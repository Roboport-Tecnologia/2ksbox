//! Alcohol 120% / Daemon Tools MDS + MDF (doc 17 M5e, brought forward
//! because real dumps exist): the `.mds` descriptor names sessions, tracks
//! (lead-in entries included, replayed as the raw TOC), per-track sector
//! size (2048, 2352, 2448 = raw + 96 bytes interleaved subchannel), file
//! offsets and lengths; the `.mdf` holds the sectors. DPM blocks (timing)
//! are ignored for now.

use std::path::Path;

use crate::{Disc, Error, Extent, Layout, Result, Session, Source, TocEntry, Track, TrackMode};

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

struct MdsTrack {
    mode: u8,
    subchannel: u8,
    adr_ctl: u8,
    point: u8,
    msf: [u8; 3],
    zero: u8,
    pmsf: [u8; 3],
    sector_size: u16,
    start_sector: u32,
    start_offset: u64,
    pregap: u32,
    length: u32,
    filename: Option<String>,
}

pub fn parse(mds: &[u8], mds_path: &Path) -> Result<Disc> {
    let bad = |what: &str| Error::Invalid(format!("{}: {}", mds_path.display(), what));
    if mds.len() < 0x58 || &mds[..16] != b"MEDIA DESCRIPTOR" {
        return Err(bad("not an MDS file"));
    }
    if mds[16] != 1 {
        return Err(bad(&format!("MDS version {}.{} not supported", mds[16], mds[17])));
    }
    let medium = u16le(mds, 0x12);
    if medium >= 0x10 {
        return Err(bad("a DVD image, not a CD"));
    }
    let num_sessions = u16le(mds, 0x14) as usize;
    let sessions_off = u32le(mds, 0x50) as usize;
    if num_sessions == 0 || sessions_off + num_sessions * 24 > mds.len() {
        return Err(bad("bad session table"));
    }
    let mut disc = Disc::new();
    let mut raw_toc = Vec::new();
    let mut sessions = Vec::new();
    let mut files: Vec<(String, usize)> = Vec::new();
    for si in 0..num_sessions {
        let sb = sessions_off + si * 24;
        let session_number = u16le(mds, sb + 8);
        let num_all = mds[sb + 10] as usize;
        let tracks_off = u32le(mds, sb + 20) as usize;
        if tracks_off + num_all * 80 > mds.len() {
            return Err(bad("bad track table"));
        }
        let mut entries: Vec<MdsTrack> = Vec::new();
        for ti in 0..num_all {
            let tb = tracks_off + ti * 80;
            let t = &mds[tb..tb + 80];
            let extra_off = u32le(t, 12) as usize;
            let footer_off = u32le(t, 36 + 4 + 8 + 4 - 4) as usize; // 0x30
            let mut e = MdsTrack {
                mode: t[0],
                subchannel: t[1],
                adr_ctl: t[2],
                point: t[4],
                msf: [t[5], t[6], t[7]],
                zero: t[8],
                pmsf: [t[9], t[10], t[11]],
                sector_size: u16le(t, 16),
                start_sector: u32le(t, 36),
                start_offset: u64le(t, 40),
                pregap: 0,
                length: 0,
                filename: None,
            };
            if extra_off != 0 && extra_off + 8 <= mds.len() && (1..=99).contains(&e.point) {
                e.pregap = u32le(mds, extra_off);
                e.length = u32le(mds, extra_off + 4);
            }
            if footer_off != 0 && footer_off + 16 <= mds.len() && (1..=99).contains(&e.point) {
                let name_off = u32le(mds, footer_off) as usize;
                let wide = u32le(mds, footer_off + 4) != 0;
                if name_off < mds.len() {
                    let name = if wide {
                        let mut s = String::new();
                        let mut o = name_off;
                        while o + 1 < mds.len() {
                            let c = u16le(mds, o);
                            if c == 0 {
                                break;
                            }
                            s.push(char::from_u32(c as u32).unwrap_or('?'));
                            o += 2;
                        }
                        s
                    } else {
                        let end = mds[name_off..].iter().position(|&b| b == 0).map(|p| name_off + p).unwrap_or(mds.len());
                        String::from_utf8_lossy(&mds[name_off..end]).into_owned()
                    };
                    e.filename = Some(name);
                }
            }
            entries.push(e);
        }
        // raw TOC entries, verbatim (lead-in points and tracks)
        for e in &entries {
            if e.adr_ctl == 0 && e.point == 0 {
                continue;
            }
            raw_toc.push(TocEntry {
                session: session_number as u8,
                point: e.point,
                adr: e.adr_ctl >> 4,
                control: e.adr_ctl & 0x0F,
                min: e.msf[0],
                sec: e.msf[1],
                frame: e.msf[2],
                zero: e.zero,
                pmin: e.pmsf[0],
                psec: e.pmsf[1],
                pframe: e.pmsf[2],
            });
        }
        let leadout = entries
            .iter()
            .find(|e| e.point == 0xA2 && e.adr_ctl >> 4 == 1)
            .map(|e| crate::msf::Msf { m: e.pmsf[0], s: e.pmsf[1], f: e.pmsf[2] }.to_lba())
            .ok_or_else(|| bad(&format!("session {session_number}: no lead-out entry")))?;
        let mut tracks: Vec<Track> = Vec::new();
        let mut track_entries: Vec<&MdsTrack> = entries.iter().filter(|e| (1..=99).contains(&e.point)).collect();
        track_entries.sort_by_key(|e| e.point);
        for (i, e) in track_entries.iter().enumerate() {
            let (mode, layout) = match (e.mode, e.sector_size) {
                (0xA9, _) => (TrackMode::Audio, Layout::Raw2352),
                (0xAA, 2048) => (TrackMode::Mode1, Layout::Cooked2048),
                (0xAA, _) => (TrackMode::Mode1, Layout::Raw2352),
                (0xAB, 2336) | (0xAC, 2336) | (0xAD, 2336) | (0xEC, 2336) => (TrackMode::Mode2, Layout::Mode2_2336),
                // 0xEC is Alcohol's mixed mode 2 (form per sector, XA), not
                // audio: read as audio until NFS Porsche Unleashed's v1.3 MDS
                // showed control 4 and mode-2 headers in every sector.
                (0xAB, _) | (0xEC, _) => (TrackMode::Mode2, Layout::Raw2352),
                (0xAC, _) => (TrackMode::Mode2Form1, Layout::Raw2352),
                (0xAD, _) => (TrackMode::Mode2Form2, Layout::Raw2352),
                (m, sz) => return Err(bad(&format!("track {}: mode {m:#04x} with {sz}-byte sectors not supported", e.point))),
            };
            let layout = match (layout, e.sector_size) {
                (Layout::Raw2352, 2448) => Layout::Raw2352Sub96,
                (Layout::Raw2352, 2352) | (Layout::Cooked2048, 2048) | (Layout::Mode2_2336, 2336) => layout,
                (_, sz) => return Err(bad(&format!("track {}: {sz}-byte sectors with mode {:#04x} not supported", e.point, e.mode))),
            };
            // The mode byte and the TOC control bits must agree. A track read
            // with the wrong kind is silently unusable (every sector
            // misclassified, no L-EC verified), so a contradiction is an
            // error, not a guess (this is what 0xEC-as-audio looked like).
            if mode.is_data() != (e.adr_ctl & 0x04 != 0) {
                return Err(bad(&format!(
                    "track {}: mode {:#04x} is {} but the TOC control {:#x} says {}",
                    e.point,
                    e.mode,
                    if mode.is_data() { "data" } else { "audio" },
                    e.adr_ctl & 0x0F,
                    if e.adr_ctl & 0x04 != 0 { "data" } else { "audio" }
                )));
            }
            if e.subchannel != 0 && layout != Layout::Raw2352Sub96 {
                return Err(bad(&format!("track {}: subchannel flag {:#04x} with {}-byte sectors", e.point, e.subchannel, e.sector_size)));
            }
            let name = e.filename.clone().unwrap_or_else(|| {
                let stem = mds_path.file_stem().and_then(|s| s.to_str()).unwrap_or("disc");
                format!("{stem}.mdf")
            });
            let name = if name.starts_with('*') { let stem = mds_path.file_stem().and_then(|s| s.to_str()).unwrap_or("disc"); format!("{stem}{}", &name[1..]) } else { name };
            let file = match files.iter().find(|(n, _)| *n == name) {
                Some((_, idx)) => *idx,
                None => {
                    let p = crate::resolve_payload(mds_path, &name)?;
                    let idx = disc.add_file(&p)?;
                    files.push((name.clone(), idx));
                    idx
                }
            };
            // The .mdf holds each track from its index 1 (start_sector) for
            // `length` sectors at start_offset; the `pregap` sectors before it
            // are not in the file (checked against the Q frames of a real
            // RAW+SUB dump: file sector = LBA - 150 from the second track on).
            // Track 1's 150-sector pregap is the unaddressable lead-in pause.
            let start = e.start_sector as i32;
            let length = e.length as i32;
            let pregap = if i == 0 && start == 0 { 0 } else { e.pregap as i32 };
            let first = start - pregap;
            if first < 0 {
                return Err(bad(&format!("track {}: pregap {pregap} before LBA {start}", e.point)));
            }
            let next_first = match track_entries.get(i + 1) {
                Some(n) => n.start_sector as i32 - n.pregap as i32,
                None => leadout,
            };
            let end = if length > 0 { (start + length).max(start) } else { next_first };
            let end = end.max(next_first.min(end)).max(start);
            let end = if track_entries.get(i + 1).is_some() { next_first.max(start) } else { end.max(start) };
            let in_file = if length > 0 { length.min(end - start) } else { end - start };
            let mut indices = Vec::new();
            let mut extents = Vec::new();
            if pregap > 0 {
                indices.push((0, first));
                extents.push(Extent { lba: first, count: pregap as u32, source: if mode.is_data() { Source::ZeroData } else { Source::Silence }, sub: None });
            }
            indices.push((1, start));
            if in_file > 0 {
                extents.push(Extent { lba: start, count: in_file as u32, source: Source::File { file, offset: e.start_offset, layout, swap: false, eof_pad: false }, sub: None });
            }
            if start + in_file < end {
                // the dump stops short of the next track: the rest reads as a gap of this track's kind
                extents.push(Extent { lba: start + in_file, count: (end - start - in_file) as u32, source: if mode.is_data() { Source::ZeroData } else { Source::Silence }, sub: None });
            }
            tracks.push(Track { number: e.point, mode, control: e.adr_ctl & 0x0F, isrc: None, indices, start_lba: start, end_lba: end, extents });
        }
        if tracks.is_empty() {
            return Err(bad(&format!("session {session_number}: no tracks")));
        }
        sessions.push(Session { number: session_number as u8, tracks, leadout_lba: leadout });
    }
    if sessions[0].tracks[0].first_lba() != 0 {
        return Err(bad("track 1 does not start at LBA 0"));
    }
    // sanity: every extent inside its file
    for s in &sessions {
        for t in &s.tracks {
            for e in &t.extents {
                if let Source::File { file, offset, layout, .. } = e.source {
                    let need = offset.checked_add(e.count as u64 * layout.stride() as u64);
                    let need = need.unwrap_or(u64::MAX);
                    if need > disc.file(file).len {
                        return Err(bad(&format!("track {}: needs {} bytes of {}, which has {}", t.number, need, disc.file(file).path.display(), disc.file(file).len)));
                    }
                }
            }
        }
    }
    disc.sessions = sessions;
    disc.raw_toc = Some(raw_toc);
    Ok(disc)
}

pub fn open(path: &Path) -> Result<Disc> {
    // a descriptor is a few KiB: not a whole big file read for one named .mds
    let len = std::fs::metadata(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?.len();
    if len > 16 << 20 {
        return Err(Error::Invalid(format!("{}: {len} bytes is no MDS descriptor", path.display())));
    }
    let bytes = std::fs::read(path).map_err(|e| Error::Invalid(format!("{}: {}", path.display(), e)))?;
    parse(&bytes, path)
}
