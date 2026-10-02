//! The abstract gamepad: what a pad's controls are, and how a raw axis
//! becomes a decision (M13, `docs/tracks/m13-gamepads.md`).
//!
//! Its own crate, shared the way `shader-chain` is, because the two
//! crates that need it cannot depend on each other: `player` reads the
//! hardware, `launcher-core` decides what the controls mean for a
//! machine, and the player must not depend on the launcher. The player
//! is the runtime and the launcher the manager that spawns it.
//!
//! What is *here* is the model both must agree on. What stays in
//! `launcher-core` is every choice ADR-014 keeps out of a front end: which
//! setting a family starts on (`bundle::Pad`), and from path C on, which
//! of these bindings a given machine runs with. Nothing in this crate
//! reads a device; `gilrs` belongs to the player alone.
//!
//! Two halves:
//!
//! * `Control`, the *abstract* pad. Every physical pad the host can see
//!   is reported in these terms, so nothing downstream of the player's
//!   `pad.rs` knows what a gilrs button id is, and the same binding
//!   works on an Xbox pad, a DualShock and a 1998 Sidewinder.
//! * `Shaping` and `Binding`, which turn a raw axis into a decision.
//!   They hold the deadzone, the hysteresis that stops an axis held near the
//!   threshold from chattering, and (for `Pad::Keys`) which key each
//!   control presses.

use serde::{Deserialize, Serialize};

/// One control on the abstract pad.
///
/// Deliberately the common subset of what every pad since the PlayStation
/// has: two sticks, a d-pad, four face buttons, two shoulders, two
/// triggers and two menu buttons. A pad with more (paddles, a touchpad
/// click) reports nothing for them; a pad with fewer (a period 2-button
/// stick on the gameport) never sends them.
///
/// Face buttons are named by *position*, not by letter, because the
/// letters move: A is the bottom button on an Xbox pad and the right one
/// on a Nintendo one, and a binding written against "A" would land on a
/// different thumb depending on what is plugged in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Control {
    /// Bottom face button (Xbox A, DualShock ✕).
    South,
    /// Right face button (Xbox B, DualShock ○).
    East,
    /// Left face button (Xbox X, DualShock □).
    West,
    /// Top face button (Xbox Y, DualShock △).
    North,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftShoulder,
    RightShoulder,
    /// Analog on most pads; reported as a button once past the deadzone.
    LeftTrigger,
    RightTrigger,
    /// Back / Select / Share.
    Select,
    Start,
    LeftStickPress,
    RightStickPress,
    /// Axes. Negative is left / up, positive is right / down. That is the
    /// screen convention, not the joystick one, so a stick pushed *up* gives a
    /// negative Y the way a mouse moved up does.
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
}

impl Control {
    pub const ALL: [Control; 20] = [
        Control::South,
        Control::East,
        Control::West,
        Control::North,
        Control::DpadUp,
        Control::DpadDown,
        Control::DpadLeft,
        Control::DpadRight,
        Control::LeftShoulder,
        Control::RightShoulder,
        Control::LeftTrigger,
        Control::RightTrigger,
        Control::Select,
        Control::Start,
        Control::LeftStickPress,
        Control::RightStickPress,
        Control::LeftStickX,
        Control::LeftStickY,
        Control::RightStickX,
        Control::RightStickY,
    ];

    /// True for the four analog axes, which carry a value in -1.0..=1.0.
    /// Everything else is a button and carries 0.0 or 1.0.
    pub fn is_axis(self) -> bool {
        matches!(
            self,
            Control::LeftStickX
                | Control::LeftStickY
                | Control::RightStickX
                | Control::RightStickY
        )
    }

    /// The name used in `PLAYER_PAD_SCRIPT` and in the player's log, and
    /// the serde representation. Short because a script line is written
    /// by hand.
    pub fn name(self) -> &'static str {
        match self {
            Control::South => "south",
            Control::East => "east",
            Control::West => "west",
            Control::North => "north",
            Control::DpadUp => "dpad_up",
            Control::DpadDown => "dpad_down",
            Control::DpadLeft => "dpad_left",
            Control::DpadRight => "dpad_right",
            Control::LeftShoulder => "l1",
            Control::RightShoulder => "r1",
            Control::LeftTrigger => "l2",
            Control::RightTrigger => "r2",
            Control::Select => "select",
            Control::Start => "start",
            Control::LeftStickPress => "l3",
            Control::RightStickPress => "r3",
            Control::LeftStickX => "lx",
            Control::LeftStickY => "ly",
            Control::RightStickX => "rx",
            Control::RightStickY => "ry",
        }
    }

    pub fn from_name(name: &str) -> Option<Control> {
        Control::ALL.into_iter().find(|c| c.name() == name)
    }

    /// What a person sees in a front end.
    pub fn label(self) -> &'static str {
        match self {
            Control::South => "Bottom face button (A / ✕)",
            Control::East => "Right face button (B / ○)",
            Control::West => "Left face button (X / □)",
            Control::North => "Top face button (Y / △)",
            Control::DpadUp => "D-pad up",
            Control::DpadDown => "D-pad down",
            Control::DpadLeft => "D-pad left",
            Control::DpadRight => "D-pad right",
            Control::LeftShoulder => "Left shoulder",
            Control::RightShoulder => "Right shoulder",
            Control::LeftTrigger => "Left trigger",
            Control::RightTrigger => "Right trigger",
            Control::Select => "Select / Back",
            Control::Start => "Start",
            Control::LeftStickPress => "Left stick click",
            Control::RightStickPress => "Right stick click",
            Control::LeftStickX => "Left stick, horizontal",
            Control::LeftStickY => "Left stick, vertical",
            Control::RightStickX => "Right stick, horizontal",
            Control::RightStickY => "Right stick, vertical",
        }
    }
}

/// How a raw axis reading becomes a decision.
///
/// Three numbers, and the third is the one that matters most. A stick is
/// never still: it rests a percent or two off centre and jitters there,
/// so `deadzone` is what stops a resting stick from meaning anything.
/// `threshold` is how far it has to go before a *digital* consumer (a
/// key, a d-pad direction, the gameport's button bits) calls it pressed.
/// And `release` is the value it must fall back below before that
/// consumer calls it let go, which must be **lower** than `threshold`:
/// with one number, a stick held right at it chatters press/release at
/// the poll rate, which a guest sees as a key repeating hundreds of times
/// a second. Two numbers with a gap between them is hysteresis, and the
/// gap is the whole point.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Shaping {
    /// Below this the axis reads as exactly centred. 0.0..1.0.
    pub deadzone: f32,
    /// A digital consumer sees "pressed" at or above this.
    pub threshold: f32,
    /// ...and "released" only below this. Strictly less than `threshold`.
    pub release: f32,
}

impl Default for Shaping {
    /// Measured against a worn Xbox 360 pad, which is the pessimistic
    /// case: 0.30 clears its resting jitter with room to spare, and the
    /// 0.55/0.40 pair gives a 0.15 gap. That is wide enough that no stick this
    /// side of a fault chatters across it, narrow enough that a
    /// deliberate half-push still registers.
    fn default() -> Self {
        Shaping {
            deadzone: 0.30,
            threshold: 0.55,
            release: 0.40,
        }
    }
}

impl Shaping {
    /// The raw reading with the deadzone taken out and the remainder
    /// stretched back over the full range, so the first movement past
    /// the deadzone is a small output rather than a jump to 0.30.
    ///
    /// The obvious version, a plain `if v.abs() < deadzone { 0 }`,
    /// leaves a step at the edge that an analog consumer feels as the
    /// stick snapping. This is the same rescale every controller driver
    /// does, and it matters because path B makes an axis mean a position
    /// rather than a direction.
    pub fn shape(self, raw: f32) -> f32 {
        let v = raw.clamp(-1.0, 1.0);
        let m = v.abs();
        if m <= self.deadzone {
            return 0.0;
        }
        let scaled = (m - self.deadzone) / (1.0 - self.deadzone);
        if v < 0.0 {
            -scaled
        } else {
            scaled
        }
    }

    /// Whether a digital consumer should now call **one half** of an axis
    /// pressed, given the shaped value and whether that half was pressed
    /// a moment ago. `was` is what makes it hysteresis rather than a
    /// comparison.
    ///
    /// Per half, not per axis, because the two halves are different
    /// things to everything downstream: stick-left and stick-right are
    /// two keys, and an axis swung straight through centre from one to
    /// the other has to release the first before pressing the second.
    /// A magnitude test cannot express that. It says "pressed" the whole
    /// way across, and the guest would hold both arrow keys at once.
    ///
    /// `positive` picks the half: `true` is right / down, `false` is left
    /// / up. A value on the other side of centre is never that half's
    /// press, whatever its magnitude.
    pub fn half_pressed(self, shaped: f32, positive: bool, was: bool) -> bool {
        let m = if positive { shaped } else { -shaped };
        if m <= 0.0 {
            return false;
        }
        if was {
            m >= self.release
        } else {
            m >= self.threshold
        }
    }

    /// A sanity check for a shaping someone edited: the three numbers in
    /// range and in the right order. Returns the complaint, or `None`.
    pub fn problem(self) -> Option<&'static str> {
        if !(0.0..1.0).contains(&self.deadzone) {
            return Some("the deadzone must be at least 0 and less than 1");
        }
        if !(0.0..=1.0).contains(&self.threshold) || self.threshold <= 0.0 {
            return Some("the press threshold must be above 0 and at most 1");
        }
        if self.release >= self.threshold {
            return Some(
                "the release threshold must be below the press threshold, \
                 or an axis held near it chatters",
            );
        }
        if self.release < 0.0 {
            return Some("the release threshold cannot be negative");
        }
        None
    }
}

// --- the USB HID report (M13 path A) --------------------------------

/// The buttons of the HID report, in bit order. Button 1 is bit 0.
///
/// This *is* the contract with `gamepad/qemu/dev-gamepad.c`'s report
/// descriptor: the guest sees "button 5" and means whatever is fifth
/// here. It is therefore the order a person will see in `joy.cpl`'s
/// button lights, and reordering it silently rebinds every game anyone
/// has configured. The order is the one every pad of the DirectInput era
/// used: the four face buttons, the shoulders, the triggers, then the
/// menu and stick buttons.
pub const HID_BUTTONS: [Control; 12] = [
    Control::South,
    Control::East,
    Control::West,
    Control::North,
    Control::LeftShoulder,
    Control::RightShoulder,
    Control::LeftTrigger,
    Control::RightTrigger,
    Control::Select,
    Control::Start,
    Control::LeftStickPress,
    Control::RightStickPress,
];

/// The axes of the report, in byte order: X, Y, Z, Rz.
pub const HID_AXES: [Control; 4] = [
    Control::LeftStickX,
    Control::LeftStickY,
    Control::RightStickX,
    Control::RightStickY,
];

/// The hat's released position, matching `USB_GAMEPAD_HAT_NULL`.
pub const HID_HAT_NULL: u8 = 8;

/// A shaped axis (-1.0..=1.0) as the byte the report carries: 0..255
/// with 0x80 centred.
///
/// `128` and not `127` for centre, because that is what the device's own
/// reset writes and what a guest calibrating the stick will see as rest.
/// The two halves are therefore very slightly asymmetric (127 of range
/// below, 127 above), which no guest of this era can perceive and every
/// real pad has too.
pub fn hid_axis(shaped: f32) -> u8 {
    let v = shaped.clamp(-1.0, 1.0);
    (128.0 + v * 127.0).round().clamp(0.0, 255.0) as u8
}

/// The four d-pad directions as a HID hat position: 0..7 clockwise from
/// north, or [`HID_HAT_NULL`] for released.
///
/// Opposite directions held together cancel. A real d-pad cannot do it
/// and a guest handed "north and south" would have to invent an answer.
pub fn hid_hat(up: bool, right: bool, down: bool, left: bool) -> u8 {
    let (up, down) = if up && down { (false, false) } else { (up, down) };
    let (left, right) = if left && right { (false, false) } else { (left, right) };
    match (up, right, down, left) {
        (true, false, false, false) => 0,
        (true, true, false, false) => 1,
        (false, true, false, false) => 2,
        (false, true, true, false) => 3,
        (false, false, true, false) => 4,
        (false, false, true, true) => 5,
        (false, false, false, true) => 6,
        (true, false, false, true) => 7,
        _ => HID_HAT_NULL,
    }
}

/// What one control does, for a machine whose pad is `Pad::Keys`.
///
/// An axis binds *twice*, once per direction, because a key has no
/// sign. `Binding::key` holds the AT set-1 scancode the player hands to
/// `qemu_embed_key`, which is the same currency `PLAYER_KEYS` already
/// speaks (`player-core/src/qemu_vm.rs`), so the two scripted input paths
/// cannot drift apart.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Binding {
    pub control: Control,
    /// For an axis: which half of it this binding is. `false` = the
    /// negative half (left / up), `true` = the positive half. Ignored for
    /// buttons.
    pub positive: bool,
    /// AT set-1 scancode, 0xE0-prefixed codes as 0xE0xx.
    pub key: u32,
    /// What to call the key in a front end and in the log.
    pub key_name: &'static str,
}

/// The default `Pad::Keys` map: the arrow keys, and the four buttons a
/// DOS or early-Windows action game actually reads.
///
/// Ctrl / Alt / Space / Enter is the era's own convention, the one Doom,
/// Duke Nukem, Commander Keen and
/// most of what a DOS machine exists to run already default to. Both the
/// d-pad and the left stick drive the arrows so a pad works whichever
/// the person reaches for, and Start is Escape because that is the menu
/// key in the same games.
///
/// The right stick is deliberately unbound: it means mouselook, and a
/// mouselook binding is a *rate*, not a key. That is path C's second
/// half, once there is something to feel it against.
pub fn default_key_bindings() -> Vec<Binding> {
    const ESC: u32 = 0x01;
    const ENTER: u32 = 0x1C;
    const CTRL: u32 = 0x1D;
    const ALT: u32 = 0x38;
    const SPACE: u32 = 0x39;
    const UP: u32 = 0xE048;
    const LEFT: u32 = 0xE04B;
    const RIGHT: u32 = 0xE04D;
    const DOWN: u32 = 0xE050;

    let b = |control, positive, key, key_name| Binding {
        control,
        positive,
        key,
        key_name,
    };
    vec![
        b(Control::DpadUp, true, UP, "up"),
        b(Control::DpadDown, true, DOWN, "down"),
        b(Control::DpadLeft, true, LEFT, "left"),
        b(Control::DpadRight, true, RIGHT, "right"),
        // The stick's negative Y is up, per `Control`'s screen convention.
        b(Control::LeftStickY, false, UP, "up"),
        b(Control::LeftStickY, true, DOWN, "down"),
        b(Control::LeftStickX, false, LEFT, "left"),
        b(Control::LeftStickX, true, RIGHT, "right"),
        b(Control::South, true, CTRL, "ctrl"),
        b(Control::East, true, ALT, "alt"),
        b(Control::West, true, SPACE, "space"),
        b(Control::North, true, ENTER, "enter"),
        b(Control::Start, true, ESC, "esc"),
    ]
}
