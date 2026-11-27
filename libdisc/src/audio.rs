//! Audio tracks stored compressed: a cue sheet's `FILE` that is an OGG
//! Vorbis, FLAC, MP3, AIFF or non-CD WAV file (doc 17 §2.3). GOG's
//! releases of CD games ship their CD audio this way, the data track as a
//! `.gog` / `.bin` and each music track an `.ogg`, for DOSBox.
//!
//! The file is decoded as the guest reads it, never ahead and never to
//! disk, into the 44.1 kHz 16-bit stereo little-endian PCM a raw audio
//! sector holds (`Source::Audio`). The rules:
//!
//! * **Its length is known at open.** The container's frame count (after
//!   the encoder's delay and padding, which the decoder trims: an OGG or
//!   MP3 of a CD track comes back exactly as many frames as the track
//!   had), or, when a file does not say, the frames counted by decoding
//!   it once. The TOC needs it before the first read.
//! * **44.1 kHz only, mono or stereo.** A CD has no other rate, and a
//!   resampled track would be a different length than its cue sheet's
//!   times say. Mono is played on both channels.
//! * **A read jumps by seeking, and lands sample-exact.** Reads that follow
//!   on (a play) keep decoding; one that goes back, or more than
//!   `AHEAD` forward, seeks the container to `PREROLL` frames before it
//!   and decodes up to it, since a lossy decoder needs a few packets
//!   before its output is right (MP3's overlap and bit reservoir). Each
//!   packet's output is placed by where the packet ends, as the first
//!   packet after a reset may give nothing (Vorbis). A sector read twice,
//!   or out of order, has the same bytes as in a play from the start.
//! * **Two decoders open at once**, the two most recently read files (a
//!   track and the next), so a disc of thirty `.ogg` files holds two.
//! * **A file that changed since the disc was opened** fails its reads as
//!   a changed medium, as an image file does (`Disc::payload_handle`).

use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use symphonia::core::codecs::audio::{AudioDecoder, AudioDecoderOptions};
use symphonia::core::formats::probe::Hint;
use symphonia::core::formats::{FormatOptions, FormatReader, SeekMode, SeekTo, TrackType};
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::units::Timestamp;

use crate::{Error, Result};

/// The rate of a CD.
const RATE: u32 = 44100;
/// Bytes per stereo frame of 16-bit PCM.
const FRAME_BYTES: u64 = 4;
/// How far past what is decoded a read may land and still be decoded up
/// to rather than seeked to: five seconds.
const AHEAD: u64 = 5 * RATE as u64;
/// Frames decoded before a seek's target and thrown away.
const PREROLL: u64 = 8192;
/// Decoded frames kept behind the read position, for a sector read again.
const BEHIND: u64 = RATE as u64;
/// Decoders kept open across all of a disc's audio files.
const OPEN: usize = 2;
/// The longest audio file taken: a whole CD's worth, 99:59:74, which a
/// single file holding every track could be. A file that claims more
/// (or counts to more) is refused rather than laid out.
const MAX_FRAMES: u64 = (99 * 60 + 59) * RATE as u64 + 74 * 588;
/// The most silence a damaged packet stands for: longer than any packet
/// of these codecs, so a duration past it is the file lying.
const MAX_PACKET: u64 = 65536;

/// The formats `FILE` lines may name, for messages.
pub const FORMATS: &str = "OGG Vorbis, FLAC, MP3, WAV, AIFF";

/// One compressed audio file.
pub struct AudioFile {
    pub path: PathBuf,
    /// Stereo frames once decoded.
    pub frames: u64,
    len: u64,
    mtime: Option<std::time::SystemTime>,
    mono: bool,
}

impl AudioFile {
    /// Bytes of PCM the file decodes to.
    pub fn pcm_len(&self) -> u64 {
        self.frames * FRAME_BYTES
    }
}

/// Whether the first bytes of a file declared `BINARY` say it is really a
/// compressed audio file: a cue sheet written for DOSBox may name an
/// `.ogg` that way. Only container magic counts; raw PCM can begin with
/// anything.
pub fn sniff(path: &Path) -> bool {
    use std::io::Read;
    let mut head = [0u8; 12];
    let Ok(mut f) = File::open(path) else { return false };
    if f.read_exact(&mut head).is_err() {
        return false;
    }
    &head[..4] == b"OggS"
        || &head[..4] == b"fLaC"
        || &head[..3] == b"ID3"
        || (&head[..4] == b"RIFF" && &head[8..12] == b"WAVE")
        || (&head[..4] == b"FORM" && (&head[8..12] == b"AIFF" || &head[8..12] == b"AIFC"))
}

/// The decoder state of one open file.
struct Decoding {
    file: usize,
    format: Box<dyn FormatReader>,
    decoder: Box<dyn AudioDecoder>,
    track: u32,
    /// Decoded PCM not yet dropped, and the frame its first byte is.
    pcm: Vec<u8>,
    start: u64,
    /// The decoder ran out: everything past `start + pcm` is silence.
    ended: bool,
}

impl Decoding {
    fn end(&self) -> u64 {
        self.start + self.pcm.len() as u64 / FRAME_BYTES
    }
}

/// A disc's compressed audio files and the decoders reading them.
#[derive(Default)]
pub struct AudioFiles {
    files: Vec<AudioFile>,
    /// Most recently read first; at most `OPEN`.
    open: Mutex<Vec<Decoding>>,
}

fn open_reader(path: &Path) -> Result<(Box<dyn FormatReader>, Box<dyn AudioDecoder>, u32, Option<u64>, u32, usize)> {
    let invalid = |what: String| Error::Invalid(format!("{}: {what}", path.display()));
    let file = File::open(path).map_err(|e| invalid(e.to_string()))?;
    let mss = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let format = symphonia::default::get_probe()
        .probe(&hint, mss, FormatOptions::default(), MetadataOptions::default())
        .map_err(|_| invalid(format!("not an audio file this can play ({FORMATS})")))?;
    let track = format.default_track(TrackType::Audio).ok_or_else(|| invalid("has no audio track".into()))?;
    let params = track
        .codec_params
        .as_ref()
        .and_then(|p| p.audio())
        .ok_or_else(|| invalid("has no audio track".into()))?;
    let rate = params.sample_rate.unwrap_or(0);
    let channels = params.channels.as_ref().map_or(0, |c| c.count());
    let decoder = symphonia::default::get_codecs()
        .make_audio_decoder(params, &AudioDecoderOptions::default())
        .map_err(|e| invalid(format!("cannot decode it: {e}")))?;
    let (id, frames) = (track.id, track.num_frames);
    Ok((format, decoder, id, frames, rate, channels))
}

impl AudioFiles {
    /// Register `path`, read its format and length; its index for
    /// `Source::Audio`.
    pub fn add(&mut self, path: &Path) -> Result<usize> {
        let md = std::fs::metadata(path).map_err(|e| Error::Invalid(format!("{}: {e}", path.display())))?;
        let (format, decoder, track, frames, rate, channels) = open_reader(path)?;
        if rate != RATE {
            return Err(Error::Invalid(format!(
                "{}: audio must be 44100 Hz, as on a CD (is {rate} Hz)",
                path.display()
            )));
        }
        if channels != 1 && channels != 2 {
            return Err(Error::Invalid(format!("{}: audio must be mono or stereo (is {channels} channels)", path.display())));
        }
        let mono = channels == 1;
        let too_long = || Error::Invalid(format!("{}: longer than a CD can hold", path.display()));
        let frames = match frames {
            Some(n) if n > MAX_FRAMES => return Err(too_long()),
            Some(n) => n,
            // The container does not say: count by decoding it once.
            None => {
                let mut d = Decoding { file: 0, format, decoder, track, pcm: Vec::new(), start: 0, ended: false };
                let mut n = 0u64;
                while !d.ended {
                    decode_packet(&mut d, mono, path)?;
                    n += d.pcm.len() as u64 / FRAME_BYTES;
                    d.pcm.clear();
                    if n > MAX_FRAMES {
                        return Err(too_long());
                    }
                }
                n
            }
        };
        self.files.push(AudioFile { path: path.to_path_buf(), frames, len: md.len(), mtime: md.modified().ok(), mono });
        Ok(self.files.len() - 1)
    }

    pub fn file(&self, index: usize) -> &AudioFile {
        &self.files[index]
    }

    /// `buf.len()` bytes of file `index`'s PCM from byte `offset`; past
    /// the decoded end, silence.
    pub fn read(&self, index: usize, offset: u64, buf: &mut [u8]) -> Result<()> {
        let info = &self.files[index];
        let frame = offset / FRAME_BYTES;
        let want = buf.len() as u64 / FRAME_BYTES;
        let mut open = self.open.lock().unwrap_or_else(|e| e.into_inner());
        let at = match open.iter().position(|d| d.file == index) {
            Some(at) => at,
            None => {
                open.insert(0, self.start(index)?);
                open.truncate(OPEN);
                0
            }
        };
        let mut d = open.remove(at);
        let result = (|| {
            if frame < d.start || frame > d.end() + AHEAD {
                self.seek(&mut d, frame)?;
            }
            while !d.ended && d.end() < frame + want {
                decode_packet(&mut d, info.mono, &info.path)?;
            }
            // Past what decoded (a header that claimed more frames than
            // the file holds, as a cut-short FLAC's does): silence.
            let from = (((frame - d.start) * FRAME_BYTES) as usize).min(d.pcm.len());
            let have = d.pcm.len().saturating_sub(from).min(buf.len());
            buf[..have].copy_from_slice(&d.pcm[from..from + have]);
            buf[have..].fill(0);
            // Keep a second behind where this read ended, for a sector
            // read again.
            let keep_from = (frame + want).saturating_sub(BEHIND).max(d.start);
            let drop = ((keep_from - d.start) * FRAME_BYTES) as usize;
            d.pcm.drain(..drop.min(d.pcm.len()));
            d.start = keep_from;
            Ok(())
        })();
        if result.is_ok() {
            open.insert(0, d);
        }
        result
    }

    /// A fresh decoder at the start of file `index`.
    fn start(&self, index: usize) -> Result<Decoding> {
        let info = &self.files[index];
        let md = std::fs::metadata(&info.path).map_err(|e| Error::Io(format!("{}: {e}", info.path.display())))?;
        if md.len() != info.len || md.modified().ok() != info.mtime {
            crate::report_file_error(&format!(
                "{}: changed since the disc was opened: reads fail as a changed medium",
                info.path.display()
            ));
            return Err(Error::Medium);
        }
        let (format, decoder, track, _, _, _) =
            open_reader(&info.path).map_err(|e| Error::Io(e.to_string()))?;
        Ok(Decoding { file: index, format, decoder, track, pcm: Vec::new(), start: 0, ended: false })
    }

    /// Put `d` where the next packet's output lands at or before `frame`,
    /// with `PREROLL` frames decoded and dropped in front of it.
    fn seek(&self, d: &mut Decoding, frame: u64) -> Result<()> {
        let info = &self.files[d.file];
        let from = frame.saturating_sub(PREROLL);
        let to = SeekTo::Timestamp { ts: Timestamp::new(from as i64), track_id: d.track };
        if from == 0 || d.format.seek(SeekMode::Accurate, to).is_err() {
            // From the top: a seek the container refused, or nothing to skip.
            *d = self.start(d.file)?;
        } else {
            d.decoder.reset();
            d.pcm.clear();
            d.ended = false;
            // `start` is set by the first packet that gives output.
            d.start = u64::MAX;
        }
        decode_up_to(d, frame, info)?;
        if d.start > frame {
            // The seek landed past the target, which an accurate seek
            // never should: start over and decode up to it, dropping as
            // it goes, so no more than a packet is ever held before it.
            *d = self.start(d.file)?;
            decode_up_to(d, frame, info)?;
        }
        Ok(())
    }
}

/// Decode until the PCM reaches past `frame`, dropping what comes before
/// it as it arrives; `d.start` is where the kept PCM begins (or `frame`
/// when the file ended first).
fn decode_up_to(d: &mut Decoding, frame: u64, info: &AudioFile) -> Result<()> {
    while !d.ended && (d.start == u64::MAX || d.end() <= frame) {
        decode_packet(d, info.mono, &info.path)?;
        if d.start != u64::MAX && d.end() < frame {
            // All before the target: drop it as it comes.
            d.start = d.end();
            d.pcm.clear();
        }
    }
    if d.start == u64::MAX {
        d.start = frame;
    }
    Ok(())
}

/// Decode the next packet onto `d.pcm`, or mark the end.
fn decode_packet(d: &mut Decoding, mono: bool, path: &Path) -> Result<()> {
    let packet = loop {
        match d.format.next_packet() {
            Ok(Some(p)) if p.track_id == d.track => break p,
            Ok(Some(_)) => continue,
            Ok(None) => {
                d.ended = true;
                return Ok(());
            }
            Err(symphonia::core::errors::Error::IoError(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                d.ended = true;
                return Ok(());
            }
            Err(e) => {
                let msg = format!("{}: {e}", path.display());
                crate::report_file_error(&msg);
                return Err(Error::Io(msg));
            }
        }
    };
    let buf = match d.decoder.decode(&packet) {
        Ok(buf) => buf,
        // A damaged packet is a click, not a stopped track; its frames
        // are silence.
        Err(symphonia::core::errors::Error::DecodeError(_)) => {
            let trimmed = packet.trim_start.get().saturating_add(packet.trim_end.get());
            let n = packet.dur.get().saturating_sub(trimmed);
            if n > MAX_PACKET {
                let msg = format!("{}: a damaged packet claims {n} frames", path.display());
                crate::report_file_error(&msg);
                return Err(Error::Io(msg));
            }
            place(d, &packet, n);
            d.pcm.resize(d.pcm.len() + (n * FRAME_BYTES) as usize, 0);
            return Ok(());
        }
        Err(e) => {
            let msg = format!("{}: {e}", path.display());
            crate::report_file_error(&msg);
            return Err(Error::Io(msg));
        }
    };
    let n = buf.frames() as u64;
    if n == 0 {
        return Ok(());
    }
    let mut samples = vec![0i16; buf.samples_interleaved()];
    buf.copy_to_slice_interleaved(&mut samples);
    place(d, &packet, n);
    d.pcm.reserve((n * FRAME_BYTES) as usize);
    if mono {
        for s in samples {
            let b = s.to_le_bytes();
            d.pcm.extend_from_slice(&[b[0], b[1], b[0], b[1]]);
        }
    } else {
        for s in samples {
            d.pcm.extend_from_slice(&s.to_le_bytes());
        }
    }
    Ok(())
}

/// After a seek, the first packet with output says where the PCM starts:
/// where it ends (its timestamp and length, less its trimmed tail) less
/// what it gave.
fn place(d: &mut Decoding, packet: &symphonia::core::packet::Packet, n: u64) {
    if d.start == u64::MAX {
        let dur = i64::try_from(packet.dur.get()).unwrap_or(i64::MAX);
        let trim = i64::try_from(packet.trim_end.get()).unwrap_or(i64::MAX);
        let end = packet.pts.get().saturating_add(dur).saturating_sub(trim);
        d.start = end.saturating_sub(n as i64).max(0) as u64;
    }
}
