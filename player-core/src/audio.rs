//! Host audio output: an SPSC byte ring fed by QEMU's `embed` audiodev
//! (f32 interleaved stereo at the rate we tell QEMU) drained by a cpal
//! stream.
//!
//! The producer drains the guest at the guest's own pace and holds the
//! ring's minimum at the cushion (`embed/embedaudio.c` says why it never
//! hurries); this side starts a stream only once the ring holds one of the
//! device's periods plus that cushion, and after an underrun waits for it
//! again, so a start or a stall costs one clean gap instead of a sputter.
//!
//! **Why f32, and the limiter.** QEMU's mixer adds every voice of the
//! machine (the sound card, the FM chip, the MIDI synth, the CD drive's
//! audio) at full scale. Its `sb16` stores the mixer's volume registers
//! and applies none of them, and the CD plays at the drive's own full
//! volume. A real card scaled those by the Volume Control sliders and left
//! headroom. So a race's engine over its CD music sums past full scale, and
//! in s16 QEMU saturates it, a crackle on every peak. QEMU's float output
//! does not saturate, so the sum arrives here intact, and a look-ahead
//! limiter turns it down for as long as it does not fit. That gain moves
//! over milliseconds, where a clip moves within a sample.
//!
//! Two knobs make what the host hears inspectable without a speaker:
//! `PLAYER_AUDIO_NULL=<frames>` replaces the device by a thread that drains
//! the ring like a DAC with that period (`1` = 1024 frames, what PipeWire
//! hands a client by default) at 48 kHz, and `PLAYER_AUDIO_TAP=<file.wav>`
//! records exactly what the consumer handed the device (16-bit, after the
//! limiter), padded silence included. `tools/audio-glitch-test.py`
//! counts clicks in that file.

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::collections::VecDeque;
use std::io::{Seek, SeekFrom, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::{Duration, Instant};

/// 128 KiB ≈ 340 ms of 48 kHz stereo f32.
pub const RING_BYTES: usize = 1 << 17;
/// One stereo f32 frame.
const FRAME: usize = 8;

/// The cushion QEMU keeps in the ring under the device's own pull (ms): how
/// late its main loop may be before the host hears a gap, and the latency
/// on top of the device's period. `PLAYER_AUDIO_MS`, 40 by default.
pub fn cushion_ms() -> u32 {
    std::env::var("PLAYER_AUDIO_MS")
        .ok()
        .and_then(|v| v.parse::<u32>().ok())
        .filter(|&ms| ms > 0)
        .unwrap_or(40)
}

pub struct Ring {
    pub buf: Box<[u8]>,
    pub wr: AtomicU32, // written by QEMU
    pub rd: AtomicU32, // written by us
}

impl Ring {
    pub fn new() -> Arc<Ring> {
        Arc::new(Ring {
            buf: vec![0u8; RING_BYTES].into_boxed_slice(),
            wr: AtomicU32::new(0),
            rd: AtomicU32::new(0),
        })
    }
    fn available(&self) -> usize {
        let wr = self.wr.load(Ordering::Acquire) as usize;
        let rd = self.rd.load(Ordering::Relaxed) as usize;
        (wr.wrapping_sub(rd)) & (RING_BYTES - 1)
    }
    /// Pop up to `out.len()` f32 samples (whole frames); returns how many.
    fn pop_f32(&self, out: &mut [f32]) -> usize {
        let mut rd = self.rd.load(Ordering::Relaxed) as usize;
        let n = (self.available() / FRAME * 2).min(out.len() & !1);
        for s in out.iter_mut().take(n) {
            let mut b = [0u8; 4];
            for (i, x) in b.iter_mut().enumerate() {
                *x = self.buf[(rd + i) & (RING_BYTES - 1)];
            }
            *s = f32::from_le_bytes(b);
            rd = (rd + 4) & (RING_BYTES - 1);
        }
        self.rd.store(rd as u32, Ordering::Release);
        n
    }
    /// Discard `bytes` (whole frames) from the read side.
    fn skip(&self, bytes: usize) {
        let rd = self.rd.load(Ordering::Relaxed) as usize;
        let bytes = bytes.min(self.available()) / FRAME * FRAME;
        self.rd
            .store(((rd + bytes) & (RING_BYTES - 1)) as u32, Ordering::Release);
    }
}

/// A look-ahead peak limiter. The gain for output frame t is the mean,
/// over a window of `D + 1` frames, of forward-window minima of the gain
/// each frame needs. Every term of that mean has seen frame t's own need,
/// so the output never exceeds the ceiling, and a gain change is a ramp
/// `D` frames long instead of a step. Release is a slow exponential back
/// to unity. Below the ceiling it is the identity, delayed by `D` frames.
struct Limiter {
    ceiling: f32,
    d: usize,
    /// The input, delayed by `d` frames.
    delay: VecDeque<[f32; 2]>,
    /// (frame, need) with rising needs: the sliding minimum.
    mins: VecDeque<(u64, f32)>,
    /// The last `d + 1` forward minima, and their sum.
    box_w: VecDeque<f32>,
    box_sum: f64,
    n: u64,
    gain: f32,
    release: f32,
}

impl Limiter {
    fn new(rate: u32) -> Limiter {
        let d = (rate as usize / 500).max(1); // 2 ms
        Limiter {
            ceiling: 0.97,
            d,
            delay: VecDeque::with_capacity(d + 2),
            mins: VecDeque::with_capacity(d + 2),
            box_w: VecDeque::with_capacity(d + 2),
            box_sum: 0.0,
            n: 0,
            gain: 1.0,
            // back up by 1/e in ~150 ms
            release: 1.0 - (-1.0 / (0.15 * rate as f32)).exp(),
        }
    }

    /// One frame in, one frame out (`d` frames late) and its gain.
    fn run(&mut self, x: [f32; 2]) -> ([f32; 2], f32) {
        let peak = x[0].abs().max(x[1].abs());
        let need = if peak > self.ceiling {
            self.ceiling / peak
        } else {
            1.0
        };
        let n = self.n;
        self.n += 1;
        while self.mins.back().is_some_and(|&(_, v)| v >= need) {
            self.mins.pop_back();
        }
        self.mins.push_back((n, need));
        let d = self.d as u64;
        while self.mins.front().is_some_and(|&(i, _)| i + d < n) {
            self.mins.pop_front();
        }
        // the minimum over need(n - d ..= n), i.e. w(n - d)
        let w = self.mins.front().map_or(1.0, |&(_, v)| v);
        self.box_w.push_back(w);
        self.box_sum += w as f64;
        if self.box_w.len() > self.d + 1 {
            self.box_sum -= self.box_w.pop_front().unwrap() as f64;
        }
        let smooth = if self.box_w.len() == self.d + 1 {
            (self.box_sum / (self.d + 1) as f64) as f32
        } else {
            // warming up: the window is not full yet, be conservative
            self.box_w.iter().cloned().fold(1.0, f32::min)
        };
        self.gain = smooth.min(self.gain + (1.0 - self.gain) * self.release);
        self.delay.push_back(x);
        let y = if self.delay.len() > self.d {
            self.delay.pop_front().unwrap()
        } else {
            [0.0, 0.0]
        };
        ([y[0] * self.gain, y[1] * self.gain], self.gain)
    }
}

/// The host's side of the ring: what every device callback runs.
struct Consumer {
    ring: Arc<Ring>,
    tap: Option<mpsc::Sender<Vec<i16>>>,
    limiter: Limiter,
    /// Bytes the ring must hold, on top of a callback's own, before a stream
    /// starts.
    cushion: usize,
    /// Bytes of stall the ring may soak up before the backlog is skipped.
    slack: usize,
    bytes_per_ms: usize,
    rate: u32,
    /// The device's callback size, frames.
    frames: usize,
    /// A stream is flowing; false while one is waited for.
    playing: bool,
    /// When the ring last ran dry mid-callback.
    dry_at: Option<Instant>,
    /// Since the last report: streams that ran dry and came back, the
    /// silence padded, the backlog skipped.
    underruns: u32,
    padded: usize,
    skipped: usize,
    /// Since the last report: samples the guest's mix put past full scale
    /// (what s16 would have clipped), frames the limiter turned down, and
    /// the least gain it used.
    over: usize,
    limited: usize,
    least_gain: f32,
    last_report: Instant,
}

impl Consumer {
    fn new(ring: Arc<Ring>, rate: u32) -> Consumer {
        let bytes_per_ms = rate as usize * FRAME / 1000;
        Consumer {
            ring,
            tap: tap(rate),
            limiter: Limiter::new(rate),
            cushion: cushion_ms() as usize * bytes_per_ms,
            slack: 100 * bytes_per_ms,
            bytes_per_ms,
            rate,
            frames: 0,
            playing: false,
            dry_at: None,
            underruns: 0,
            padded: 0,
            skipped: 0,
            over: 0,
            limited: 0,
            least_gain: 1.0,
            last_report: Instant::now(),
        }
    }

    /// Fill `out` (interleaved stereo f32) for the device.
    fn fill(&mut self, out: &mut [f32]) {
        if self.frames != out.len() / 2 {
            // how chunky the device drains the ring: the first thing to know
            // about a host that crackles
            self.frames = out.len() / 2;
            eprintln!("[audio] device asks for {} frames a callback", self.frames);
        }
        let want = out.len() / 2 * FRAME;
        let avail = self.ring.available();
        if !self.playing {
            if avail >= want + self.cushion {
                self.playing = true;
                if self
                    .dry_at
                    .take()
                    .is_some_and(|t| t.elapsed() < Duration::from_secs(1))
                {
                    self.underruns += 1;
                }
            }
        } else if avail > 2 * want + self.cushion + self.slack {
            // The device stalled, or started late, and the ring holds more
            // than it will ever need: one skip back to where a stream starts
            // rather than that much latency for the rest of the session.
            let excess = avail - (want + self.cushion);
            self.ring.skip(excess);
            self.skipped += excess;
        }
        let n = if self.playing {
            let n = self.ring.pop_f32(out);
            if n < out.len() {
                self.playing = false;
                self.padded += (out.len() - n) / 2;
                self.dry_at = Some(Instant::now());
            }
            n
        } else {
            0
        };
        out[n..].fill(0.0);
        for f in out.chunks_exact_mut(2) {
            self.over += (f[0].abs() > 1.0) as usize + (f[1].abs() > 1.0) as usize;
            let (y, g) = self.limiter.run([f[0], f[1]]);
            if g < 0.999 {
                self.limited += 1;
                self.least_gain = self.least_gain.min(g);
            }
            f[0] = y[0];
            f[1] = y[1];
        }
        if let Some(tx) = &self.tap {
            let _ = tx.send(out.iter().map(|&v| to_i16(v)).collect());
        }
        if self.last_report.elapsed() >= Duration::from_secs(5) {
            if self.underruns > 0 || self.skipped > 0 {
                eprintln!(
                    "[audio] {} underruns ({} frames of silence), {} ms skipped",
                    self.underruns,
                    self.padded,
                    self.skipped / self.bytes_per_ms
                );
            }
            if self.limited > 0 {
                eprintln!(
                    "[audio] the guest's mix went past full scale ({} samples): limited for {} ms \
                     in 5 s, down to {:.1} dB",
                    self.over,
                    self.limited as u64 * 1000 / self.rate as u64,
                    20.0 * self.least_gain.log10()
                );
            }
            self.underruns = 0;
            self.padded = 0;
            self.skipped = 0;
            self.over = 0;
            self.limited = 0;
            self.least_gain = 1.0;
            self.last_report = Instant::now();
        }
    }
}

fn to_i16(v: f32) -> i16 {
    (v * 32767.0).round().clamp(-32768.0, 32767.0) as i16
}

/// `PLAYER_AUDIO_TAP=<file.wav>`: a writer thread, so the device callback
/// only hands over a buffer. The header's sizes are rewritten every second
/// because a player is as often killed as it is closed.
fn tap(rate: u32) -> Option<mpsc::Sender<Vec<i16>>> {
    let path = std::env::var("PLAYER_AUDIO_TAP").ok()?;
    let mut f = match std::fs::File::create(&path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("[audio] PLAYER_AUDIO_TAP {path}: {e}");
            return None;
        }
    };
    eprintln!("[audio] tap: {path}");
    let (tx, rx) = mpsc::channel::<Vec<i16>>();
    std::thread::spawn(move || {
        let header = |data: u32| {
            let mut h = Vec::with_capacity(44);
            h.extend_from_slice(b"RIFF");
            h.extend_from_slice(&(36 + data).to_le_bytes());
            h.extend_from_slice(b"WAVEfmt ");
            h.extend_from_slice(&16u32.to_le_bytes());
            h.extend_from_slice(&1u16.to_le_bytes()); // PCM
            h.extend_from_slice(&2u16.to_le_bytes());
            h.extend_from_slice(&rate.to_le_bytes());
            h.extend_from_slice(&(rate * 4).to_le_bytes());
            h.extend_from_slice(&4u16.to_le_bytes());
            h.extend_from_slice(&16u16.to_le_bytes());
            h.extend_from_slice(b"data");
            h.extend_from_slice(&data.to_le_bytes());
            h
        };
        let _ = f.write_all(&header(0));
        let mut data = 0u32;
        let mut last = Instant::now();
        let mut bytes = Vec::new();
        while let Ok(buf) = rx.recv() {
            bytes.clear();
            for s in buf {
                bytes.extend_from_slice(&s.to_le_bytes());
            }
            if f.write_all(&bytes).is_err() {
                return;
            }
            data += bytes.len() as u32;
            if last.elapsed() >= Duration::from_secs(1) {
                let _ = f.seek(SeekFrom::Start(0));
                let _ = f.write_all(&header(data));
                let _ = f.seek(SeekFrom::End(0));
                last = Instant::now();
            }
        }
    });
    Some(tx)
}

pub struct Output {
    _stream: Option<cpal::Stream>,
    pub sample_rate: u32,
}

/// Open the default output device. Returns the rate/channels QEMU must be
/// configured with (we don't resample: `-audiodev embed,out.frequency=<rate>`).
pub fn start(ring: Arc<Ring>) -> Option<Output> {
    if let Ok(v) = std::env::var("PLAYER_AUDIO_NULL") {
        // headless: a thread drains the ring like a DAC with this period
        let period: usize = match v.parse() {
            Ok(n) if n > 1 => n,
            _ => 1024,
        };
        let rate = 48000;
        eprintln!("[audio] PLAYER_AUDIO_NULL: no device, a {period}-frame DAC at {rate} Hz");
        let mut consumer = Consumer::new(ring, rate);
        std::thread::spawn(move || {
            let mut buf = vec![0f32; period * 2];
            let step = Duration::from_secs_f64(period as f64 / rate as f64);
            let mut next = Instant::now() + step;
            loop {
                let now = Instant::now();
                if next > now {
                    std::thread::sleep(next - now);
                }
                consumer.fill(&mut buf);
                next += step;
            }
        });
        return Some(Output {
            _stream: None,
            sample_rate: rate,
        });
    }
    let host = cpal::default_host();
    let device = host.default_output_device()?;
    let default = device.default_output_config().ok()?;
    let sample_rate = default.sample_rate();
    let channels: u16 = 2;
    let config = cpal::StreamConfig {
        channels,
        sample_rate,
        buffer_size: cpal::BufferSize::Default,
    };
    let mut consumer = Consumer::new(ring, sample_rate);
    let stream = match default.sample_format() {
        cpal::SampleFormat::I16 => {
            let mut tmp: Vec<f32> = Vec::new();
            device
                .build_output_stream(
                    config,
                    move |data: &mut [i16], _| {
                        tmp.resize(data.len(), 0.0);
                        consumer.fill(&mut tmp);
                        for (d, s) in data.iter_mut().zip(&tmp) {
                            *d = to_i16(*s);
                        }
                    },
                    |e| eprintln!("[audio] stream error: {e}"),
                    None,
                )
                .ok()?
        }
        _ => device
            .build_output_stream(
                config,
                move |data: &mut [f32], _| consumer.fill(data),
                |e| eprintln!("[audio] stream error: {e}"),
                None,
            )
            .ok()?,
    };
    stream.play().ok()?;
    eprintln!(
        "[audio] output {} Hz, {} ch, {:?}",
        sample_rate,
        channels,
        default.sample_format()
    );
    Some(Output {
        _stream: Some(stream),
        sample_rate,
    })
}
