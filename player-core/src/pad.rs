//! Reading a host gamepad (M13 step 0, `docs/tracks/m13-gamepads.md`).
//!
//! This is the *whole* host end and nothing else: enumerate what is
//! plugged in, notice what is unplugged, turn whatever the platform calls
//! its buttons into the abstract [`Control`]s the `gamepad` crate
//! defines, and shape the analog readings. What those controls then
//! *mean* for a machine is not decided here. That is `launcher-core`'s
//! (ADR-014), and reaches the player as arguments.
//!
//! Two sources behind one interface:
//!
//! * [`Gilrs`]: real hardware, through one crate that covers evdev on
//!   Linux, XInput and DirectInput on Windows and IOKit on macOS.
//! * [`Script`]: `PLAYER_PAD_SCRIPT`, a recorded sequence replayed
//!   against the guest's own frame counter.
//!
//! The second is what the tests run on. No machine that runs
//! `scripts/test.sh` has a controller plugged into it, so without a
//! synthetic pad every check in this track would need a person and a
//! device, and the project's testing policy (integration and end-to-end
//! only, wired into the suite) could not be met at all. The rest of M13
//! is built on it, which is why step 0 exists.

use gamepad::{Binding, Control, Shaping};
use std::collections::{BTreeMap, BTreeSet};

/// One control moved. `value` is the shaped reading: -1.0..=1.0 for an
/// axis, 0.0 or 1.0 for a button.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Event {
    pub control: Control,
    pub value: f32,
}

/// Where readings come from. Both sources are polled once per published
/// guest frame from the UI thread (see [`Pads::poll`]).
trait Source {
    /// Everything that changed since the last call. `frame` is the
    /// guest's published frame count, which only the script uses.
    fn poll(&mut self, frame: u64, out: &mut Vec<Event>);
    fn describe(&self) -> String;
}

// --- the synthetic pad ----------------------------------------------

/// `PLAYER_PAD_SCRIPT="30:lx=1.0,45:south=1,51:south=0"` sets `control`
/// to `value` at guest frame `n`.
///
/// Frame numbers, not milliseconds, for the same reason `PLAYER_KEYS`
/// uses them: a guest under TCG runs at whatever rate the host manages
/// that minute, and a script written in wall-clock time would land in a
/// different place in the guest's execution on every run. Against the
/// frame counter it lands in the same place every time.
///
/// A button is `0` or `1`; an axis takes any value in -1.0..=1.0 and is
/// shaped on the way out, so a script says what the stick *is* and the
/// deadzone still applies. A script can therefore prove the deadzone
/// works, which is most of what step 0 has to prove.
struct Script {
    /// (frame, control, raw value), sorted by frame.
    steps: Vec<(u64, Control, f32)>,
    next: usize,
}

impl Script {
    fn from_env() -> Option<Result<Script, String>> {
        let spec = std::env::var("PLAYER_PAD_SCRIPT").ok()?;
        Some(Script::parse(&spec))
    }

    fn parse(spec: &str) -> Result<Script, String> {
        let mut steps = Vec::new();
        for item in spec.split(',') {
            let item = item.trim();
            if item.is_empty() {
                continue;
            }
            let (frame, rest) = item
                .split_once(':')
                .ok_or_else(|| format!("{item:?}: expected <frame>:<control>=<value>"))?;
            let (name, value) = rest
                .split_once('=')
                .ok_or_else(|| format!("{item:?}: expected <frame>:<control>=<value>"))?;
            let frame: u64 = frame
                .trim()
                .parse()
                .map_err(|_| format!("{item:?}: {:?} is not a frame number", frame.trim()))?;
            let control = Control::from_name(name.trim())
                .ok_or_else(|| format!("{item:?}: no control called {:?}", name.trim()))?;
            let value: f32 = value
                .trim()
                .parse()
                .map_err(|_| format!("{item:?}: {:?} is not a number", value.trim()))?;
            if !(-1.0..=1.0).contains(&value) {
                return Err(format!("{item:?}: {value} is outside -1.0..=1.0"));
            }
            if !control.is_axis() && value != 0.0 && value != 1.0 {
                return Err(format!(
                    "{item:?}: {} is a button, so its value must be 0 or 1",
                    control.name()
                ));
            }
            steps.push((frame, control, value));
        }
        if steps.is_empty() {
            return Err("PLAYER_PAD_SCRIPT is empty".into());
        }
        steps.sort_by_key(|&(f, c, _)| (f, c));
        Ok(Script { steps, next: 0 })
    }
}

impl Source for Script {
    fn poll(&mut self, frame: u64, out: &mut Vec<Event>) {
        while self.next < self.steps.len() && self.steps[self.next].0 <= frame {
            let (_, control, value) = self.steps[self.next];
            out.push(Event { control, value });
            self.next += 1;
        }
    }

    fn describe(&self) -> String {
        format!("scripted pad, {} step(s)", self.steps.len())
    }
}

// --- real hardware ---------------------------------------------------

#[cfg(feature = "gilrs")]
mod hardware {
    use super::{Event, Source};
    use gamepad::Control;

    pub struct Gilrs {
        inner: gilrs::Gilrs,
    }

    impl Gilrs {
        pub fn new() -> Result<Gilrs, String> {
            let inner = gilrs::Gilrs::new().map_err(|e| e.to_string())?;
            Ok(Gilrs { inner })
        }

        pub fn pads(&self) -> Vec<String> {
            self.inner
                .gamepads()
                .map(|(_, g)| g.name().to_string())
                .collect()
        }

        /// gilrs's own button/axis names to ours. The face buttons map by
        /// *position*: gilrs already normalizes South/East/West/North for
        /// this exact reason, so an Xbox pad and a DualShock give the same
        /// `Control` for the same thumb.
        fn button(b: gilrs::Button) -> Option<Control> {
            use gilrs::Button as B;
            Some(match b {
                B::South => Control::South,
                B::East => Control::East,
                B::West => Control::West,
                B::North => Control::North,
                B::DPadUp => Control::DpadUp,
                B::DPadDown => Control::DpadDown,
                B::DPadLeft => Control::DpadLeft,
                B::DPadRight => Control::DpadRight,
                B::LeftTrigger => Control::LeftShoulder,
                B::RightTrigger => Control::RightShoulder,
                B::LeftTrigger2 => Control::LeftTrigger,
                B::RightTrigger2 => Control::RightTrigger,
                B::Select => Control::Select,
                B::Start => Control::Start,
                B::LeftThumb => Control::LeftStickPress,
                B::RightThumb => Control::RightStickPress,
                // Mode/C/Z and anything else this pad invents: a control
                // we have no name for. Ignoring it silently is right, since
                // a guest of this era has nothing to bind it to.
                _ => return None,
            })
        }

        fn axis(a: gilrs::Axis) -> Option<Control> {
            use gilrs::Axis as A;
            Some(match a {
                A::LeftStickX => Control::LeftStickX,
                A::LeftStickY => Control::LeftStickY,
                A::RightStickX => Control::RightStickX,
                A::RightStickY => Control::RightStickY,
                _ => return None,
            })
        }
    }

    impl Source for Gilrs {
        fn poll(&mut self, _frame: u64, out: &mut Vec<Event>) {
            while let Some(ev) = self.inner.next_event() {
                match ev.event {
                    gilrs::EventType::ButtonPressed(b, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: 1.0 });
                        }
                    }
                    gilrs::EventType::ButtonReleased(b, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: 0.0 });
                        }
                    }
                    // A trigger on most pads is analog and arrives here
                    // rather than as a button; past the deadzone it is a
                    // press, which `Pads` works out from the value.
                    gilrs::EventType::ButtonChanged(b, v, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: v });
                        }
                    }
                    gilrs::EventType::AxisChanged(a, v, _) => {
                        if let Some(c) = Self::axis(a) {
                            // gilrs reports a stick pushed up as +1, the
                            // joystick convention. `Control` documents the
                            // screen convention (up is negative), so the
                            // vertical axes are flipped here, once, rather
                            // than in every consumer.
                            let v = if matches!(c, Control::LeftStickY | Control::RightStickY) {
                                -v
                            } else {
                                v
                            };
                            out.push(Event { control: c, value: v });
                        }
                    }
                    gilrs::EventType::Connected => {
                        eprintln!("[pad] connected");
                    }
                    gilrs::EventType::Disconnected => {
                        // Everything this pad was holding is now released:
                        // a guest must not be left with a key stuck down
                        // because a cable came out.
                        eprintln!("[pad] disconnected; releasing every control");
                        for c in Control::ALL {
                            out.push(Event { control: c, value: 0.0 });
                        }
                    }
                    _ => {}
                }
            }
        }

        fn describe(&self) -> String {
            let pads = self.pads();
            if pads.is_empty() {
                "gilrs, no pad connected yet".to_string()
            } else {
                format!("gilrs: {}", pads.join(", "))
            }
        }
    }
}

// --- the reader ------------------------------------------------------

/// The host end of a gamepad, however it is being read.
pub struct Pads {
    source: Box<dyn Source>,
    shaping: Shaping,
    /// The shaped value each control last reported, so `poll` can emit a
    /// change rather than a level and a consumer never sees a repeat.
    values: BTreeMap<Control, f32>,
    /// Which control *halves* a digital consumer currently calls pressed,
    /// as `(control, positive)`. Kept here rather than worked out per
    /// event because hysteresis needs the previous answer
    /// (`Shaping::half_pressed`), and per half rather than per control
    /// because the two halves of a stick are two different keys.
    pressed: BTreeMap<(Control, bool), bool>,
    log: bool,
}

impl Pads {
    /// Builds the reader, or returns `None` when there is nothing to read
    /// and nothing was asked for.
    ///
    /// `PLAYER_PAD_SCRIPT` wins over real hardware when both are present,
    /// because a run that sets it is a test and must not be perturbed by
    /// whatever is plugged into the machine running it.
    pub fn from_env() -> Option<Pads> {
        let log = std::env::var("PLAYER_PAD_LOG").is_ok();
        let shaping = shaping_from_env()?;
        let source: Box<dyn Source> = match Script::from_env() {
            Some(Ok(s)) => Box::new(s),
            Some(Err(e)) => {
                eprintln!("[pad] PLAYER_PAD_SCRIPT: {e}");
                return None;
            }
            None => hardware_source()?,
        };
        eprintln!("[pad] {}", source.describe());
        Some(Pads {
            source,
            shaping,
            values: BTreeMap::new(),
            pressed: BTreeMap::new(),
            log,
        })
    }

    /// Everything that changed since the last call, shaped. Call once per
    /// published guest frame from the UI thread.
    ///
    /// The UI thread and not QEMU's: on macOS a HID source needs the
    /// process's run loop, which is the main thread's, and the QEMU
    /// thread has none. It costs nothing, because a pad polled at the guest's
    /// frame rate is polled far faster than a guest of this era reads
    /// one.
    pub fn poll(&mut self, frame: u64) -> Vec<Event> {
        let mut raw = Vec::new();
        self.source.poll(frame, &mut raw);
        let mut out = Vec::new();
        for ev in raw {
            let shaped = if ev.control.is_axis() {
                self.shaping.shape(ev.value)
            } else {
                ev.value
            };
            // A change and not a level: an axis resting inside the
            // deadzone shapes to 0.0 every poll, and a consumer that saw
            // all of them would be handed the same event sixty times a
            // second forever.
            //
            // A control not in the map yet counts as **centred**, not as
            // unknown. Treating it as unknown is the obvious version and
            // it is wrong: a stick resting a little off centre reports
            // itself the moment the pad is plugged in, that reading
            // shapes to 0.0, and the consumer is handed a "change" to the
            // value the control already had. Harmless for a key, and not
            // harmless at all where path B makes an axis a position the
            // guest polls.
            let previous = self.values.get(&ev.control).copied().unwrap_or(0.0);
            self.values.insert(ev.control, shaped);
            if previous == shaped {
                continue;
            }
            // Both halves, every time. An axis swung straight across
            // centre changes them both in one reading, and the release
                // of the half being left has to be seen as well as the
            // press of the half being entered.
            let mut note = String::new();
            for positive in [false, true] {
                let half = (ev.control, positive);
                let was = self.pressed.get(&half).copied().unwrap_or(false);
                let now = self.shaping.half_pressed(shaped, positive, was);
                self.pressed.insert(half, now);
                if was != now {
                    note.push(' ');
                    note.push_str(half_name(ev.control, positive));
                    note.push_str(if now { " press" } else { " release" });
                }
            }
            if self.log {
                eprintln!(
                    "[pad] frame {frame} {} raw {:+.3} shaped {:+.3}{note}",
                    ev.control.name(),
                    ev.value,
                    shaped,
                );
            }
            out.push(Event {
                control: ev.control,
                value: shaped,
            });
        }
        out
    }

    /// Whether a digital consumer should currently call one half of a
    /// control pressed. The key mapping reads this rather than the value,
    /// so the hysteresis is applied in exactly one place.
    pub fn is_pressed(&self, control: Control, positive: bool) -> bool {
        self.pressed.get(&(control, positive)).copied().unwrap_or(false)
    }

    /// The pad as the USB HID report carries it (M13 path A): four axis
    /// bytes, a hat position and a bitmap of twelve buttons.
    ///
    /// The axes go out **shaped**, so the deadzone applies to the guest's
    /// analog reading too, not only to the key mapping. That is the right
    /// answer and not the obvious one: a guest calibrating the stick in
    /// `joy.cpl` calibrates what it is given, and handing it the raw
    /// value would make it discover the drift and compensate for it
    /// twice.
    pub fn hid_state(&self) -> ([u8; 4], u8, u16) {
        let mut axes = [0x80u8; 4];
        for (i, c) in gamepad::HID_AXES.iter().enumerate() {
            axes[i] = gamepad::hid_axis(self.values.get(c).copied().unwrap_or(0.0));
        }
        let hat = gamepad::hid_hat(
            self.is_pressed(Control::DpadUp, true),
            self.is_pressed(Control::DpadRight, true),
            self.is_pressed(Control::DpadDown, true),
            self.is_pressed(Control::DpadLeft, true),
        );
        let mut buttons = 0u16;
        for (bit, c) in gamepad::HID_BUTTONS.iter().enumerate() {
            if self.is_pressed(*c, true) {
                buttons |= 1 << bit;
            }
        }
        (axes, hat, buttons)
    }

    /// Every half a digital consumer currently calls pressed, in a stable
    /// order.
    fn pressed_now(&self) -> Vec<&'static str> {
        let mut out = Vec::new();
        for c in Control::ALL {
            for positive in [false, true] {
                if self.is_pressed(c, positive) {
                    out.push(half_name(c, positive));
                }
            }
        }
        out
    }
}

/// What to call one half of a control in a log line.
///
/// A button has only the positive half and is just its own name; an axis
/// names the direction, because "lx" alone in a line about a key press
/// does not say which key.
fn half_name(control: Control, positive: bool) -> &'static str {
    if !control.is_axis() {
        return control.name();
    }
    match (control, positive) {
        (Control::LeftStickX, false) => "lx-",
        (Control::LeftStickX, true) => "lx+",
        (Control::LeftStickY, false) => "ly-",
        (Control::LeftStickY, true) => "ly+",
        (Control::RightStickX, false) => "rx-",
        (Control::RightStickX, true) => "rx+",
        (Control::RightStickY, false) => "ry-",
        (Control::RightStickY, true) => "ry+",
        _ => control.name(),
    }
}

// --- path C: the pad presses keys -----------------------------------

/// Turns the pad's pressed halves into key presses (M13 path C).
///
/// The whole of it is one idea: **recompute the wanted set every poll and
/// diff it against what is held.** The obvious alternative, reacting to
/// each transition as it arrives, has to get every one of them right
/// forever, and the failure mode is a key stuck down in the guest, which
/// outlives the mistake and cannot be cleared from the host. A diff
/// cannot drift: whatever the pad did, one poll later the guest's keys
/// are exactly what the bindings say they should be.
///
/// It also gets *shared keys* right for free, which is not a corner case:
/// the default map binds both the d-pad and the left stick to the arrow
/// keys, so `left` has two holders. Counting presses and releases would
/// release the key when either let go; a set union releases it when the
/// last one does.
pub struct KeyMap {
    bindings: Vec<Binding>,
    /// AT set-1 scancodes currently down in the guest because of the pad.
    held: BTreeSet<u32>,
}

impl KeyMap {
    pub fn new(bindings: Vec<Binding>) -> KeyMap {
        KeyMap {
            bindings,
            held: BTreeSet::new(),
        }
    }

    /// What the guest's keys should now be, given the pad, as the
    /// changes needed to get there: `(scancode, down)`.
    ///
    /// Releases are emitted before presses. It matters on exactly one
    /// motion and that motion is common: a stick swung from one side to
    /// the other crosses centre inside a single poll, and pressing
    /// `right` before releasing `left` leaves a guest that samples
    /// between the two calls holding both.
    pub fn apply(&mut self, pads: &Pads) -> Vec<(u32, bool)> {
        let want: BTreeSet<u32> = self
            .bindings
            .iter()
            .filter(|b| pads.is_pressed(b.control, b.positive))
            .map(|b| b.key)
            .collect();
        let mut out: Vec<(u32, bool)> =
            self.held.difference(&want).map(|&k| (k, false)).collect();
        out.extend(want.difference(&self.held).map(|&k| (k, true)));
        self.held = want;
        out
    }

    /// Let go of everything, for the moments when the guest must not be
    /// left holding a key the pad can no longer release: the window
    /// losing focus, and the pad being unplugged.
    pub fn release_all(&mut self) -> Vec<(u32, bool)> {
        let out = self.held.iter().map(|&k| (k, false)).collect();
        self.held.clear();
        out
    }

    /// What a binding calls a scancode, for the log.
    pub fn key_name(&self, key: u32) -> &'static str {
        self.bindings
            .iter()
            .find(|b| b.key == key)
            .map(|b| b.key_name)
            .unwrap_or("?")
    }
}

/// What the machine told the player to do with a pad: `--pad none` or
/// `--pad keys`, resolved by `launcher-core` from `bundle::Pad` and
/// passed on the command line the way `--shader` is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Mode {
    #[default]
    None,
    /// The pad is a USB HID device on the machine (path A). The player
    /// sends it state; the guest's own driver does the rest.
    Usb,
    /// The pad is the machine's gameport at 0x201 (path B). Identical
    /// work on this side (the same state through the same call), because
    /// the shim feeds whichever device the machine has and the port's own
    /// shape (two axes, four buttons, the d-pad folded onto the axes) is
    /// the device's business, not the player's.
    Gameport,
    Keys,
}

impl Mode {
    pub fn parse(name: &str) -> Option<Mode> {
        match name {
            "none" => Some(Mode::None),
            "usb" => Some(Mode::Usb),
            "gameport" => Some(Mode::Gameport),
            "keys" => Some(Mode::Keys),
            _ => None,
        }
    }

    /// Whether this setting means "there is a pad device in the guest to
    /// send state to", as opposed to keys or nothing.
    pub fn is_device(self) -> bool {
        matches!(self, Mode::Usb | Mode::Gameport)
    }
}

/// `player --pads`: what this host can actually read, out of the binary
/// that has to read it.
///
/// The first question anyone asks when a pad does nothing, and the one a
/// person cannot answer from the outside: whether the controller was seen
/// at all is a fact about this build's `gilrs` feature, this host's
/// permissions and this sandbox's device access, and each of those fails
/// silently. The Flatpak without `--device=input` prints its refusal
/// here and nowhere else.
pub fn report() {
    if cfg!(not(feature = "gilrs")) {
        println!("gamepads: built without the gilrs feature; PLAYER_PAD_SCRIPT only");
        return;
    }
    match hardware_source() {
        Some(s) => println!("gamepads: {}", s.describe()),
        None => println!("gamepads: none readable on this host"),
    }
}

/// `player --pad-sweep <frames>`: run `PLAYER_PAD_SCRIPT` against a
/// counted sequence of guest frames and print what came out. No window,
/// no QEMU, no guest. This is the `pad` check in `scripts/test.sh`.
///
/// This is the whole host end under test: the script parser, the deadzone
/// rescale, the change filter and the hysteresis, in the same code the
/// player runs against real hardware. What it cannot test is gilrs, which
/// needs a device; everything downstream of a reading it can.
pub fn sweep(frames: u64) -> i32 {
    if std::env::var("PLAYER_PAD_SCRIPT").is_err() {
        eprintln!("--pad-sweep needs PLAYER_PAD_SCRIPT");
        return 2;
    }
    // The log is the output here, not a debug aid.
    unsafe { std::env::set_var("PLAYER_PAD_LOG", "1") };
    let Some(mut pads) = Pads::from_env() else {
        return 2;
    };
    // `--pad keys` makes the sweep show the key mapping too, which is how
    // path C is checked without a guest: the same `KeyMap` the player
    // runs, against the same scripted pad.
    let mode = mode_from_env();
    let mut keys = match mode {
        Mode::Keys => Some(KeyMap::new(gamepad::default_key_bindings())),
        Mode::Usb | Mode::Gameport | Mode::None => None,
    };
    let mut total = 0usize;
    let mut last_hid: Option<([u8; 4], u8, u16)> = None;
    for frame in 1..=frames {
        total += pads.poll(frame).len();
        // `--pad usb` shows the report the guest would be handed, so the
        // packing is checked without a guest: this is the same
        // `hid_state()` the player sends through qemu_embed_pad_state.
        // `--pad gameport` prints the very same line and that is not an
        // oversight. The state the player sends is identical, and what
        // the gameport makes of it (two of the axes, four of the
        // buttons, the d-pad folded onto its own X/Y) happens in the
        // device, where the port's shape belongs. There is deliberately
        // no second copy of that rule on this side to print from.
        if mode.is_device() {
            let hid = pads.hid_state();
            if last_hid != Some(hid) {
                last_hid = Some(hid);
                let (axes, hat, buttons) = hid;
                println!(
                    "pad-hid: frame {frame} axes {:02x} {:02x} {:02x} {:02x} hat {} buttons {:012b}",
                    axes[0], axes[1], axes[2], axes[3], hat, buttons
                );
            }
        }
        if let Some(km) = keys.as_mut() {
            for (sc, down) in km.apply(&pads) {
                println!(
                    "pad-key: frame {frame} {} {:#06x} {}",
                    km.key_name(sc),
                    sc,
                    if down { "down" } else { "up" }
                );
            }
        }
    }
    let held = pads.pressed_now();
    println!("pad-sweep: {frames} frames, {total} events");
    println!("pad-sweep: held at end: {}", if held.is_empty() { "none".to_string() } else { held.join(" ") });
    if let Some(km) = keys.as_mut() {
        // Whatever the pad is still holding at the end is still down in
        // the guest, so say so: a sweep that ends with keys held is a
        // script that did not let go, not a leak.
        let stuck: Vec<&str> = km.held.iter().map(|&k| km.key_name(k)).collect();
        println!(
            "pad-sweep: keys down at end: {}",
            if stuck.is_empty() { "none".to_string() } else { stuck.join(" ") }
        );
    }
    0
}

fn mode_from_env() -> Mode {
    resolve_mode(None)
}

/// What this run does with a pad: `--pad keys` if given, else
/// `PLAYER_PAD`, else nothing. `launcher-core` writes the argument from
/// `bundle::Pad`, the same way it writes `--shader` from the machine's
/// shader profile; the variable is for driving the player by hand.
///
/// An unknown name is a complaint and then `none`, not a refusal: a
/// bundle from a later launcher can name a setting this player has never
/// heard of, and the machine must still run.
pub fn resolve_mode(cli: Option<&str>) -> Mode {
    let named = match cli {
        Some(v) => Some(v.to_string()),
        None => std::env::var("PLAYER_PAD").ok(),
    };
    match named {
        None => Mode::None,
        Some(v) => Mode::parse(&v).unwrap_or_else(|| {
            eprintln!("[pad] no such gamepad setting {v:?}; using none");
            Mode::None
        }),
    }
}

/// `PLAYER_PAD_SHAPING="deadzone,threshold,release"` overrides the three
/// numbers, for finding a worn stick's deadzone without a rebuild.
/// Absent means the defaults; malformed is refused loudly rather than
/// silently ignored, because a typo here reads as a broken pad.
fn shaping_from_env() -> Option<Shaping> {
    let Ok(spec) = std::env::var("PLAYER_PAD_SHAPING") else {
        return Some(Shaping::default());
    };
    let nums: Vec<&str> = spec.split(',').map(str::trim).collect();
    let [dz, th, rl] = nums.as_slice() else {
        eprintln!("[pad] PLAYER_PAD_SHAPING wants three numbers: deadzone,threshold,release");
        return None;
    };
    let (Ok(deadzone), Ok(threshold), Ok(release)) =
        (dz.parse::<f32>(), th.parse::<f32>(), rl.parse::<f32>())
    else {
        eprintln!("[pad] PLAYER_PAD_SHAPING: {spec:?} is not three numbers");
        return None;
    };
    let shaping = Shaping {
        deadzone,
        threshold,
        release,
    };
    if let Some(problem) = shaping.problem() {
        eprintln!("[pad] PLAYER_PAD_SHAPING: {problem}");
        return None;
    }
    Some(shaping)
}

#[cfg(feature = "gilrs")]
fn hardware_source() -> Option<Box<dyn Source>> {
    match hardware::Gilrs::new() {
        Ok(g) => Some(Box::new(g)),
        // Not fatal and not silent. A host with no input permission (the
        // Flatpak without `--device=input`) fails exactly here, and the
        // player must still run the machine. A guest is worth more than
        // a controller.
        Err(e) => {
            eprintln!("[pad] no gamepad support on this host: {e}");
            None
        }
    }
}

#[cfg(not(feature = "gilrs"))]
fn hardware_source() -> Option<Box<dyn Source>> {
    None
}
