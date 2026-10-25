//! General MIDI over a SoundFont bank.
//!
//! What "the game's MIDI music actually plays" means on a machine with
//! no wavetable daughterboard: the guest's note-ons land on a bank of
//! recorded instruments and come back as PCM. The bank is a file (ours
//! by default, the user's when they say so), and which one it is decides
//! how the music sounds far more than anything in this file does.

use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::Arc;

use rustysynth::{SoundFont, Synthesizer, SynthesizerSettings};

use crate::{clip, Voice};

/// The rate the synthesizer runs at. Any rate works; this one is the
/// audiodev's own on every host we ship to, so the mixer's resampler has
/// nothing to do.
pub const RATE: u32 = 48000;

/// How many notes can sound at once. rustysynth's own default, and about
/// what a period wavetable card could hold; the ceiling only matters on
/// an arrangement that stacks more, where the alternative to a limit is
/// a note stealing another. `synthx midilog` names it when a capture
/// holds that many notes down at once.
pub const POLYPHONY: usize = 64;

pub struct Gm {
    synth: Synthesizer,
    /// The master volume as a multiplier, kept because `reset()` puts
    /// the synthesizer's own back to its default.
    gain: f32,
    left: Vec<f32>,
    right: Vec<f32>,
}

impl Gm {
    /// Open a bank. The error is the sentence the device fails to
    /// realize with, so it says which file and what was wrong with it.
    pub fn new(soundfont: &Path) -> Result<Gm, String> {
        let mut file = File::open(soundfont)
            .map_err(|e| format!("SoundFont {}: {e}", soundfont.display()))?;
        check_chunks(&mut file).map_err(|e| format!("SoundFont {}: {e}", soundfont.display()))?;
        file.seek(SeekFrom::Start(0)).map_err(|e| format!("SoundFont {}: {e}", soundfont.display()))?;
        let mut reader = BufReader::new(file);
        let sf = SoundFont::new(&mut reader)
            .map_err(|e| format!("SoundFont {}: {e}", soundfont.display()))?;
        let mut settings = SynthesizerSettings::new(RATE as i32);
        settings.maximum_polyphony = POLYPHONY;
        settings.enable_reverb_and_chorus = true;
        let mut synth = Synthesizer::new(&Arc::new(sf), &settings)
            .map_err(|e| format!("SoundFont {}: {e}", soundfont.display()))?;
        // rustysynth's own default is 0.5, which is 6 dB below a bank
        // that was mastered to be played at full scale, and against the
        // sound card in the same mixer that reads as "the music is too
        // quiet". Full scale here, `gain` for the trim, `clip` for the
        // arrangement that asks for more than there is.
        synth.set_master_volume(1.0);
        Ok(Gm {
            synth,
            gain: 1.0,
            left: Vec::new(),
            right: Vec::new(),
        })
    }
}

impl Voice for Gm {
    fn rate(&self) -> u32 {
        RATE
    }

    fn message(&mut self, status: u8, d1: u8, d2: u8) {
        // Real-time and system-common bytes have no channel and nothing
        // in a General MIDI bank answers them.
        if status < 0x80 || status >= 0xF0 {
            return;
        }
        self.synth.process_midi_message(
            (status & 0x0F) as i32,
            (status & 0xF0) as i32,
            d1 as i32,
            d2 as i32,
        );
    }

    fn sysex(&mut self, data: &[u8]) {
        // The synthesizer has no sysex of its own, but the two resets
        // every sequencer opens with mean something here: they are how a
        // game says "forget the last game's controllers". GM reset is
        // F0 7E 7F 09 01 F7; Roland's GS reset ends 40 00 7F 00 41 F7.
        let gm_reset = data.len() >= 6 && data[1] == 0x7E && data[3] == 0x09;
        let gs_reset = data.len() >= 10
            && data[1] == 0x41
            && data.windows(4).any(|w| w == [0x40, 0x00, 0x7F, 0x00]);
        if gm_reset || gs_reset {
            self.reset();
        }
    }

    fn render(&mut self, out: &mut [i16]) {
        let frames = out.len() / 2;
        if frames == 0 {
            return;
        }
        // Grown once and then reused: the render path allocates nothing
        // after the first block, which is the one QEMU calls with the
        // BQL held.
        if self.left.len() < frames {
            self.left.resize(frames, 0.0);
            self.right.resize(frames, 0.0);
        }
        self.synth
            .render(&mut self.left[..frames], &mut self.right[..frames]);
        for (i, frame) in out.chunks_exact_mut(2).enumerate() {
            frame[0] = clip(self.left[i]);
            frame[1] = clip(self.right[i]);
        }
    }

    fn reset(&mut self) {
        self.synth.reset();
        // `reset` puts the master volume back to rustysynth's default,
        // so the trim has to be re-applied. A GM reset sysex in the
        // middle of a game must not change how loud it is.
        self.synth.set_master_volume(self.gain);
    }

    fn gain(&mut self, percent: u32) {
        self.gain = percent.min(400) as f32 / 100.0;
        self.synth.set_master_volume(self.gain);
    }
}

/// The bank's RIFF chunk headers walked before rustysynth reads it, which
/// allocates every chunk's declared size before reading it (a few bytes
/// of file asked for 4 GiB, an abort no `catch_unwind` takes) and reads
/// an odd `smpl` size one byte past its buffer. Each chunk must fit inside
/// its parent and the file, and the sample data be whole 16-bit samples.
fn check_chunks(f: &mut File) -> Result<(), String> {
    let len = f.metadata().map_err(|e| e.to_string())?.len();
    let mut head = [0u8; 12];
    f.read_exact(&mut head).map_err(|_| "not a SoundFont (too short)".to_string())?;
    if &head[0..4] != b"RIFF" || &head[8..12] != b"sfbk" {
        return Err("not a SoundFont (no RIFF sfbk header)".into());
    }
    let riff_end = 8 + u32::from_le_bytes([head[4], head[5], head[6], head[7]]) as u64;
    if riff_end > len {
        return Err(format!("the RIFF chunk says {riff_end} bytes, the file has {len}"));
    }
    let chunk = |f: &mut File, at: u64| -> Result<([u8; 4], u64), String> {
        let mut h = [0u8; 8];
        f.seek(SeekFrom::Start(at)).map_err(|e| e.to_string())?;
        f.read_exact(&mut h).map_err(|e| e.to_string())?;
        Ok(([h[0], h[1], h[2], h[3]], u32::from_le_bytes([h[4], h[5], h[6], h[7]]) as u64))
    };
    let mut at = 12;
    while at + 8 <= riff_end {
        let (id, size) = chunk(f, at)?;
        let end = at + 8 + size;
        if end > riff_end {
            return Err(format!("chunk {} runs past the file", String::from_utf8_lossy(&id)));
        }
        if &id == b"LIST" {
            let mut sub = at + 12;
            while sub + 8 <= end {
                let (sid, ssize) = chunk(f, sub)?;
                let send = sub + 8 + ssize;
                if send > end {
                    return Err(format!("chunk {} runs past its list", String::from_utf8_lossy(&sid)));
                }
                if &sid == b"smpl" && ssize % 2 != 0 {
                    return Err(format!("{ssize} bytes of samples is not whole 16-bit samples"));
                }
                sub = send;
            }
        }
        at = end;
    }
    Ok(())
}
