//! MMC responders (doc 17 §4): READ TOC/PMA/ATIP formats 0/1/2, READ
//! SUB-CHANNEL formats 1/2/3, READ CD / READ CD MSF sector layouts, READ
//! DISC INFORMATION. Reference: MMC-3 (T10 1363-D), which Win9x/XP's
//! cdrom.sys and period protection drivers assume. Multi-byte fields are
//! big-endian; `msf` selects `0, M, S, F` binary times over 32-bit LBAs.

use crate::msf::{Msf, MSF_OFFSET};
use crate::sector::{bcd, from_bcd};
use crate::{subq, Disc, Error, Result, SectorKind, Track, TrackMode};

fn put_time(out: &mut Vec<u8>, lba: i32, msf: bool) {
    if msf {
        let m = Msf::from_lba(lba);
        out.extend_from_slice(&[0, m.m, m.s, m.f]);
    } else {
        out.extend_from_slice(&lba.to_be_bytes());
    }
}

fn adr_control(t: &Track) -> u8 {
    0x10 | t.control
}

/// Disc type byte of the A0 entry: 0x00 CD-DA / CD-ROM, 0x10 CD-I, 0x20 CD-ROM XA.
fn disc_type(first: &Track) -> u8 {
    match first.mode {
        TrackMode::Mode2 | TrackMode::Mode2Form1 | TrackMode::Mode2Form2 => 0x20,
        TrackMode::Mode2Formless => 0x10,
        _ => 0x00,
    }
}

fn finish(out: &mut Vec<u8>) {
    let len = (out.len() - 2) as u16;
    out[0..2].copy_from_slice(&len.to_be_bytes());
}

/// READ TOC/PMA/ATIP (0x43). `start` is the starting track (format 0) or
/// session (formats 1/2); formats 3..5 are refused.
pub fn read_toc(disc: &Disc, format: u8, msf: bool, start: u8, out: &mut Vec<u8>) -> Result<()> {
    out.clear();
    out.extend_from_slice(&[0, 0]);
    match format {
        0 => {
            let first = disc.first_track();
            let last = disc.last_track();
            if start != 0xAA && start > last && start != 0 {
                return Err(Error::Invalid(format!("READ TOC: start track {start} above the last track {last}")));
            }
            out.push(first);
            out.push(last);
            let mut last_data = true;
            for (_, t) in disc.tracks() {
                last_data = t.mode.is_data();
                if start != 0xAA && t.number >= start {
                    out.extend_from_slice(&[0, adr_control(t), t.number, 0]);
                    put_time(out, t.start_lba, msf);
                }
            }
            out.extend_from_slice(&[0, if last_data { 0x14 } else { 0x10 }, 0xAA, 0]);
            put_time(out, disc.sector_count() as i32, msf);
        }
        1 => {
            let first = disc.sessions.first().map(|s| s.number).unwrap_or(1);
            let last_s = disc.sessions.last().ok_or(Error::Range)?;
            out.push(first);
            out.push(last_s.number);
            let t = last_s.tracks.first().ok_or(Error::Range)?;
            out.extend_from_slice(&[0, adr_control(t), t.number, 0]);
            put_time(out, t.start_lba, msf);
        }
        2 => {
            let first = disc.sessions.first().map(|s| s.number).unwrap_or(1);
            let last = disc.sessions.last().map(|s| s.number).unwrap_or(1);
            out.push(first);
            out.push(last);
            if let Some(raw) = &disc.raw_toc {
                for e in raw {
                    if start > 1 && e.session < start {
                        continue;
                    }
                    out.extend_from_slice(&[e.session, (e.adr << 4) | e.control, 0, e.point, e.min, e.sec, e.frame, e.zero, e.pmin, e.psec, e.pframe]);
                }
            } else {
                for s in &disc.sessions {
                    if start > 1 && s.number < start {
                        continue;
                    }
                    let ft = s.tracks.first().ok_or(Error::Range)?;
                    let lt = s.tracks.last().ok_or(Error::Range)?;
                    // A0 carries the first track's control, A1 and A2 the last track's (the
                    // lead-out follows it), as real lead-ins do
                    out.extend_from_slice(&[s.number, adr_control(ft), 0, 0xA0, 0, 0, 0, 0, ft.number, disc_type(ft), 0]);
                    out.extend_from_slice(&[s.number, adr_control(lt), 0, 0xA1, 0, 0, 0, 0, lt.number, 0, 0]);
                    let lo = Msf::from_lba(s.leadout_lba);
                    out.extend_from_slice(&[s.number, adr_control(lt), 0, 0xA2, 0, 0, 0, 0, lo.m, lo.s, lo.f]);
                    for t in &s.tracks {
                        let p = Msf::from_lba(t.start_lba);
                        out.extend_from_slice(&[s.number, adr_control(t), 0, t.number, 0, 0, 0, 0, p.m, p.s, p.f]);
                    }
                }
                if disc.sessions.len() > 1 {
                    let ls = disc.sessions.last().unwrap();
                    let next = Msf::from_lba(ls.leadout_lba + MSF_OFFSET);
                    out.extend_from_slice(&[ls.number, 0x54, 0, 0xB0, next.m, next.s, next.f, 1, 0x4C, 0x2C, 0x00]);
                }
            }
        }
        _ => return Err(Error::Invalid(format!("READ TOC: format {format} not supported"))),
    }
    finish(out);
    Ok(())
}

/// The formatted 12-byte Q of `lba` (ADR 1 frame, synthesized) for the
/// position replies: `(adr_control, track, index, relative_lba)`.
fn position(disc: &Disc, lba: i32) -> (u8, u8, u8, i32) {
    match disc.locate(lba) {
        Some((_, t, index)) => {
            let rel = if index == 0 { -(t.start_lba - lba) } else { lba - t.start_lba };
            (adr_control(t), t.number, index, rel)
        }
        None => (0x10, 0xAA, 1, 0),
    }
}

/// READ SUB-CHANNEL (0x42).
#[allow(clippy::too_many_arguments)]
pub fn read_subchannel(disc: &Disc, pos_lba: i32, msf: bool, subq: bool, format: u8, track: u8, audio_status: u8, out: &mut Vec<u8>) -> Result<()> {
    out.clear();
    out.extend_from_slice(&[0, audio_status, 0, 0]);
    if !subq {
        return Ok(());
    }
    match format {
        1 => {
            let lba = pos_lba.clamp(0, disc.sector_count().saturating_sub(1) as i32);
            let (ac, tno, index, rel) = position(disc, lba);
            out.extend_from_slice(&[1, ac, tno, index]);
            put_time(out, lba, msf);
            if msf {
                let m = Msf::from_lba(rel.abs() - MSF_OFFSET);
                out.extend_from_slice(&[0, m.m, m.s, m.f]);
            } else {
                out.extend_from_slice(&rel.to_be_bytes());
            }
        }
        2 => {
            out.extend_from_slice(&[2, 0, 0, 0]);
            match disc.mcn {
                Some(m) => {
                    out.push(0x80);
                    out.extend_from_slice(&m);
                }
                None => {
                    out.push(0);
                    out.extend_from_slice(&[b'0'; 13]);
                }
            }
            out.extend_from_slice(&[0, 0]);
        }
        3 => {
            let (_, t) = disc.track(track).ok_or_else(|| Error::Invalid(format!("READ SUB-CHANNEL: no track {track}")))?;
            out.extend_from_slice(&[3, adr_control(t), t.number, 0]);
            match t.isrc {
                Some(i) => {
                    out.push(0x80);
                    out.extend_from_slice(&i);
                }
                None => {
                    out.push(0);
                    out.extend_from_slice(&[b'0'; 12]);
                }
            }
            out.extend_from_slice(&[0, 0, 0]);
        }
        _ => return Err(Error::Invalid(format!("READ SUB-CHANNEL: format {format} not supported"))),
    }
    let len = (out.len() - 4) as u16;
    out[2..4].copy_from_slice(&len.to_be_bytes());
    Ok(())
}

/// READ DISC INFORMATION (0x51), the standard 34-byte reply.
pub fn read_disc_information(disc: &Disc, out: &mut Vec<u8>) -> Result<()> {
    out.clear();
    out.resize(34, 0);
    let ls = disc.sessions.last().ok_or(Error::Range)?;
    let ft = ls.tracks.first().ok_or(Error::Range)?;
    let lt = ls.tracks.last().ok_or(Error::Range)?;
    out[1] = 32;
    out[2] = 0x0E;
    out[3] = disc.first_track();
    out[4] = disc.sessions.len() as u8;
    out[5] = ft.number;
    out[6] = lt.number;
    out[7] = 0x20;
    out[8] = disc_type(disc.tracks().next().map(|(_, t)| t).unwrap_or(ft));
    out[16..20].copy_from_slice(&[0xFF; 4]);
    out[20..24].copy_from_slice(&[0xFF; 4]);
    Ok(())
}

/// Field lengths of one sector type in layout order: sync, header,
/// subheader, user data, EDC/ECC (MMC-3 tables 356..360).
fn field_lengths(expected_type: u8) -> Option<[usize; 5]> {
    match expected_type {
        0 | 2 => Some([12, 4, 0, 2048, 288]),
        1 => Some([2352, 0, 0, 0, 0]), // handled specially: any field = the whole sector
        3 => Some([12, 4, 0, 2336, 0]),
        4 => Some([12, 4, 8, 2048, 280]),
        5 => Some([12, 4, 8, 2324, 4]),
        _ => None,
    }
}

/// Which of the five fields byte 9 selects, in layout order.
fn selected(byte9: u8) -> [bool; 5] {
    let hdr = (byte9 >> 5) & 3;
    [byte9 & 0x80 != 0, hdr & 1 != 0, hdr & 2 != 0, byte9 & 0x10 != 0, byte9 & 0x08 != 0]
}

/// Main-channel bytes per sector for this CDB, without C2 and subchannel.
fn main_length(expected_type: u8, byte9: u8) -> Result<usize> {
    let lens = field_lengths(expected_type).ok_or_else(|| Error::Invalid(format!("READ CD: expected sector type {expected_type} reserved")))?;
    let sel = selected(byte9);
    if expected_type == 1 {
        return Ok(if sel.iter().any(|&s| s) { 2352 } else { 0 });
    }
    // the selected non-empty fields must be contiguous in the sector
    let mut total = 0;
    let mut started = false;
    let mut ended = false;
    for i in 0..5 {
        if lens[i] == 0 {
            continue;
        }
        if sel[i] {
            if ended {
                return Err(Error::Invalid(format!("READ CD: byte 9 {byte9:#04x} selects non-contiguous fields for sector type {expected_type}")));
            }
            started = true;
            total += lens[i];
        } else if started {
            ended = true;
        }
    }
    Ok(total)
}

fn c2_length(byte9: u8) -> Result<usize> {
    match (byte9 >> 1) & 3 {
        0 => Ok(0),
        1 => Ok(294),
        2 => Ok(296),
        _ => Err(Error::Invalid("READ CD: C2 error field 3 reserved".into())),
    }
}

fn sub_length(byte10: u8) -> Result<usize> {
    match byte10 & 7 {
        0 => Ok(0),
        1 | 4 => Ok(96),
        2 => Ok(16),
        v => Err(Error::Invalid(format!("READ CD: subchannel selection {v} reserved"))),
    }
}

/// Bytes per sector for a READ CD / READ CD MSF CDB, or `Err(Invalid)`
/// for a combination the MMC-3 tables mark illegal.
pub fn read_cd_length(expected_type: u8, byte9: u8, byte10: u8) -> Result<usize> {
    Ok(main_length(expected_type, byte9)? + c2_length(byte9)? + sub_length(byte10)?)
}

fn kind_matches(expected_type: u8, kind: SectorKind) -> bool {
    match expected_type {
        0 => true,
        1 => kind == SectorKind::Audio,
        2 => kind == SectorKind::Mode1,
        3 => matches!(kind, SectorKind::Mode2Form1 | SectorKind::Mode2Form2 | SectorKind::Mode2Formless),
        4 => kind == SectorKind::Mode2Form1,
        5 => kind == SectorKind::Mode2Form2,
        _ => false,
    }
}

/// Byte ranges of the five fields inside a raw sector of `kind`.
fn field_ranges(kind: SectorKind) -> [(usize, usize); 5] {
    match kind {
        SectorKind::Audio => [(0, 0), (0, 0), (0, 0), (0, 2352), (0, 0)],
        SectorKind::Mode1 => [(0, 12), (12, 16), (16, 16), (16, 2064), (2064, 2352)],
        SectorKind::Mode2Form1 => [(0, 12), (12, 16), (16, 24), (24, 2072), (2072, 2352)],
        SectorKind::Mode2Form2 => [(0, 12), (12, 16), (16, 24), (24, 2348), (2348, 2352)],
        SectorKind::Mode2Formless => [(0, 12), (12, 16), (16, 16), (16, 2352), (2352, 2352)],
        SectorKind::Gap => [(0, 0), (0, 0), (0, 0), (0, 2352), (0, 0)],
    }
}

/// Fill one READ CD sector: the selected fields in order (sync, header,
/// subheader, user data, EDC/ECC), then C2, then subchannel. Returns the
/// number of bytes written (== `read_cd_length`). Expected type 0 uses
/// the Mode 1 field lengths; on a sector of another kind it delivers the
/// whole raw sector when every main field is selected, the sector's own
/// user data for a user-data-only request, and refuses the rest with
/// `Err(Mode)` (a drive's ILLEGAL MODE FOR THIS TRACK). A user-data request
/// without EDC/ECC is a cooked read: L-EC is verified, what the P/Q decoder
/// can fix is corrected and only the rest is `Err(Medium)`. A raw request
/// delivers the stored bytes, uncorrected, but a sector whose L-EC does not
/// hold is still `Err(Medium)` unless C2 error flags were asked for. A drive
/// hands over an unreadable sector's bytes only alongside the C2 field that
/// says which of them it could not trust, and a protection band is read raw
/// precisely to watch the read fail (doc 17 §2.6c).
pub fn read_cd_sector(disc: &Disc, lba: i32, expected_type: u8, byte9: u8, byte10: u8, out: &mut [u8]) -> Result<usize> {
    let total = read_cd_length(expected_type, byte9, byte10)?;
    if out.len() < total {
        return Err(Error::Invalid(format!("READ CD: buffer of {} bytes for a {total}-byte sector", out.len())));
    }
    let mut raw = [0u8; 2352];
    let ((kind, _, _), read) = disc.classify_into(lba, &mut raw)?;
    if !kind_matches(expected_type, kind) {
        return Err(Error::Mode);
    }
    if !read {
        disc.read_raw(lba, &mut raw)?;
    }
    let sel = selected(byte9);
    let main = main_length(expected_type, byte9)?;
    // A request for user data without the EDC/ECC field is a cooked read: L-EC is
    // verified and a mismatch is a MEDIUM ERROR, as READ(10) on a drive.
    //
    // A *raw* request (EDC/ECC selected) is the one a dumping tool makes, and
    // it used to deliver the bytes as dumped whatever their parity said. That was
    // wrong, and Crimson Skies (SafeDisc 1.50.020) is what showed it:
    // its check reads single sectors of its own protection band raw (`READ CD`,
    // byte 9 = 0xf8) and it is the *read failing* that it is looking for. A drive
    // fails it; we answered with the 2352 bytes the dumper had stored, and the
    // game said "Cannot locate the CD-ROM". With the error delivered, it launches.
    // So a raw read of a sector whose L-EC does not hold is a MEDIUM ERROR too,
    // unless the CDB asked for **C2 error flags**, which is how a real
    // dumping tool gets an unreadable sector's bytes out of a real drive: the
    // drive hands over what it read and says in the C2 field which bytes it could
    // not trust. That keeps the dumping case (doc 17 §2.5) working and stops us
    // telling a protection its band is clean.
    //
    // The test is the same one the cooked path applies, so the two agree about
    // which sectors exist: a sector the P/Q decoder can repair is readable and
    // its raw read still delivers the damage as stored (a dump must round-trip),
    // and only a sector `discx scan` calls *unreadable* fails.
    if kind.is_data() && sel[3] {
        if !sel[4] {
            crate::sector::verify_or_correct(&mut raw, kind)?;
        } else if c2_length(byte9)? == 0 {
            let mut probe = raw;
            crate::sector::verify_or_correct(&mut probe, kind)?;
        }
    }
    let mut pos = 0;
    if main > 0 {
        if expected_type == 1 || (expected_type == 0 && kind == SectorKind::Audio) {
            if expected_type == 0 && main != 2352 {
                return Err(Error::Mode);
            }
            out[..2352].copy_from_slice(&raw);
            pos = 2352;
        } else if expected_type == 0 && kind != SectorKind::Mode1 {
            if main == 2352 {
                out[..2352].copy_from_slice(&raw);
                pos = 2352;
            } else if byte9 & 0xF8 == 0x10 {
                let (a, b) = field_ranges(kind)[3];
                let n = (b - a).min(2048);
                out[..n].copy_from_slice(&raw[a..a + n]);
                out[n..2048].fill(0);
                pos = 2048;
            } else {
                return Err(Error::Mode);
            }
        } else {
            let ranges = field_ranges(kind);
            let lens = field_lengths(expected_type).unwrap();
            for i in 0..5 {
                if sel[i] && lens[i] > 0 {
                    let (a, b) = ranges[i];
                    let n = (b - a).min(lens[i]);
                    out[pos..pos + n].copy_from_slice(&raw[a..a + n]);
                    out[pos + n..pos + lens[i]].fill(0);
                    pos += lens[i];
                }
            }
        }
    }
    let c2 = c2_length(byte9)?;
    if c2 > 0 {
        let mut bits = [0u8; 294];
        let n = crate::ecc::c2_bits(&raw, kind, &mut bits);
        out[pos..pos + 294].copy_from_slice(&bits);
        if c2 == 296 {
            out[pos + 294] = if n > 0 { 0x80 } else { 0 };
            out[pos + 295] = 0;
        }
        pos += c2;
    }
    let sub = sub_length(byte10)?;
    if sub > 0 {
        let mut de = [0u8; 96];
        disc.read_sub(lba, &mut de)?;
        match byte10 & 7 {
            1 => {
                let mut inter = [0u8; 96];
                subq::interleave(&de, &mut inter);
                out[pos..pos + 96].copy_from_slice(&inter);
            }
            2 => {
                out[pos..pos + 12].copy_from_slice(&de[12..24]);
                out[pos + 12..pos + 16].fill(0);
            }
            _ => out[pos..pos + 96].copy_from_slice(&de),
        }
        pos += sub;
    }
    debug_assert_eq!(pos, total);
    Ok(pos)
}

/// Decode the BCD `MIN SEC FRAME` of a Q frame into an absolute LBA.
pub fn q_abs_lba(q: &[u8]) -> i32 {
    Msf { m: from_bcd(q[7]), s: from_bcd(q[8]), f: from_bcd(q[9]) }.to_lba()
}

/// BCD MSF of an absolute LBA (for callers building raw Q frames).
pub fn bcd_msf(lba: i32) -> [u8; 3] {
    let m = Msf::from_lba(lba);
    [bcd(m.m), bcd(m.s), bcd(m.f)]
}
