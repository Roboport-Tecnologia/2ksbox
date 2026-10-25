//! The MIDI byte stream, as it arrives at an MPU-401 data port.
//!
//! A game writes a byte at a time and leaves out everything it can:
//! running status (a status byte stands until another one comes), a
//! System Exclusive dump split across as many writes as it likes, and
//! real-time bytes (`F8`–`FF`) dropped in the middle of *either*. The
//! clock does not wait for a sysex to finish. So this is a state machine
//! over single bytes rather than a message reader, and its output is
//! whole messages the engines can take.

/// What a byte completed, if anything.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// A channel or system-common message: status and up to two data
    /// bytes (unused ones are 0).
    Message(u8, u8, u8),
    /// A complete System Exclusive message, `F0 … F7` included.
    Sysex(Vec<u8>),
}

/// How many data bytes a status byte takes. `None` = not a status byte
/// with a fixed length (sysex, or a real-time byte that has none).
fn data_len(status: u8) -> Option<usize> {
    match status & 0xF0 {
        0x80 | 0x90 | 0xA0 | 0xB0 | 0xE0 => Some(2),
        0xC0 | 0xD0 => Some(1),
        0xF0 => match status {
            0xF1 | 0xF3 => Some(1), // MTC quarter frame, song select
            0xF2 => Some(2),        // song position pointer
            0xF6 => Some(0),        // tune request
            _ => None,              // F0/F7 sysex framing, F8-FF real time
        },
        _ => None,
    }
}

/// The parser. One per port; `write` is the only thing the device calls.
#[derive(Debug, Default)]
pub struct Parser {
    /// The status byte in force, for running status. Cleared by a system
    /// common message (a real-time byte must not clear it).
    running: u8,
    /// The status of the message being assembled right now. Equal to
    /// `running` for a channel message, and a system common status (F1,
    /// F2, F3) for the messages that do not become running status.
    status: u8,
    data: [u8; 2],
    have: usize,
    want: usize,
    sysex: Option<Vec<u8>>,
    dropped: u64,
}

/// A sysex longer than this is a guest that lost its way (an MT-32 patch
/// dump is a few hundred bytes; the largest legitimate one here is a
/// display message). Dropping it beats growing without bound.
const SYSEX_MAX: usize = 65536;

impl Parser {
    pub fn new() -> Parser {
        Parser::default()
    }

    /// Feed one byte. Returns the message it completed, if it completed
    /// one.
    pub fn write(&mut self, byte: u8) -> Option<Event> {
        // Real-time bytes are single bytes that may appear anywhere,
        // including between the two data bytes of a note-on and inside a
        // sysex, and they change nothing else about the state.
        if byte >= 0xF8 {
            return Some(Event::Message(byte, 0, 0));
        }
        if let Some(buf) = &mut self.sysex {
            if byte == 0xF7 {
                let mut done = self.sysex.take().unwrap();
                done.push(0xF7);
                return Some(Event::Sysex(done));
            }
            if byte & 0x80 != 0 {
                // A status byte inside a sysex aborts it: the dump was
                // truncated and what follows is a new message.
                self.sysex = None;
                return self.write(byte);
            }
            if buf.len() < SYSEX_MAX {
                buf.push(byte);
            }
            return None;
        }
        if byte & 0x80 != 0 {
            if byte == 0xF0 {
                // SysEx ends running status and any message half received
                self.running = 0;
                self.status = 0;
                self.have = 0;
                self.sysex = Some(vec![0xF0]);
                return None;
            }
            self.have = 0;
            match data_len(byte) {
                Some(0) => {
                    // A one-byte system common message (tune request):
                    // complete on its own, and it ends running status.
                    self.running = 0;
                    self.status = 0;
                    self.want = 0;
                    return Some(Event::Message(byte, 0, 0));
                }
                Some(n) => {
                    self.want = n;
                    // System common (F1-F3) does not become running
                    // status; a channel message does.
                    self.running = if byte < 0xF0 { byte } else { 0 };
                    self.status = byte;
                }
                // F4, F5, F7 without a sysex open: undefined. Ignore the
                // byte and leave running status alone, which is what a
                // real UART's downstream does with a byte it cannot use.
                None => self.dropped += 1,
            }
            return None;
        }
        // A data byte with no status byte at all (a game that started
        // mid-stream): nothing to attach it to.
        if self.status == 0 {
            if self.running == 0 {
                self.dropped += 1;
                return None;
            }
            self.status = self.running;
            self.want = data_len(self.running).unwrap_or(0);
            self.have = 0;
        }
        self.data[self.have] = byte;
        self.have += 1;
        if self.have < self.want {
            return None;
        }
        let status = self.status;
        let (d1, d2) = (self.data[0], if self.want > 1 { self.data[1] } else { 0 });
        self.have = 0;
        // Running status: the next data byte starts another message of
        // the same kind. A system common message left `running` at 0, so
        // this drops back to "no status" for those.
        self.status = self.running;
        Some(Event::Message(status, d1, d2))
    }

    /// Bytes this parser could attach to nothing: a data byte with no
    /// status byte in force, or a status byte that means nothing here.
    /// A stream that makes sense has none, so a non-zero count is the
    /// parser and the guest disagreeing about where a message starts,
    /// which is the first thing to look for in a captured log.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// Forget everything in flight: an MPU-401 reset, or a machine
    /// reset. A half-written note-on must not complete against the next
    /// byte the guest writes.
    pub fn reset(&mut self) {
        *self = Parser::new();
    }
}
