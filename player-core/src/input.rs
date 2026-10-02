//! What the guest is holding: the keys pressed through the window, the
//! Ctrl+Alt+Del chord, and the host gamepad (M13). A front end turns its
//! toolkit's key into an AT set-1 scancode and its focus loss into
//! `lift_all`; everything after that is here, so every front end lets go
//! of the same keys at the same moments.

use crate::pad;
use crate::session::Session;
use qemu_embed::Qemu;

#[derive(Default)]
pub struct Input {
    /// Keys currently held in the guest (QEMU qcodes). Lifted when the window
    /// loses focus: a host shortcut (Cmd+Tab on macOS) delivers the modifier's
    /// press to us and its release to the app that took over, and the guest
    /// would otherwise keep the Windows key down forever.
    keys_down: Vec<u32>,
    /// Ctrl+Alt+Del is down and the guest holds Delete for it; the
    /// modifiers it pressed because the guest had none are in `cad_extra`.
    cad_held: bool,
    cad_extra: Vec<u32>,
    /// The host gamepad, when this run has one to read (M13). `None` is
    /// the ordinary case: no controller plugged in, no script, or a host
    /// with no input access at all.
    pads: Option<pad::Pads>,
    /// What the machine says a pad does (`--pad`, from `bundle::Pad`).
    pad_mode: pad::Mode,
    /// The pad's keys, while `pad_mode` is `Keys` and there is a pad.
    pad_keys: Option<pad::KeyMap>,
}

impl Input {
    /// Opens the host's gamepads. Call it once the UI thread runs its
    /// event loop: on macOS a HID source wants the run loop.
    pub fn new(pad_mode: pad::Mode) -> Input {
        let pads = pad::Pads::from_env();
        let pad_keys = (pads.is_some() && pad_mode == pad::Mode::Keys)
            .then(|| pad::KeyMap::new(gamepad::default_key_bindings()));
        Input {
            pads,
            pad_mode,
            pad_keys,
            ..Default::default()
        }
    }

    /// An AT set-1 scancode pressed or released in the guest.
    pub fn key(&mut self, vm: Option<Qemu>, atset1: u32, down: bool) {
        let Some(vm) = vm else { return };
        let qcode = qemu_embed::atset1_to_qcode(atset1);
        if qcode == 0 {
            return;
        }
        vm.key(qcode, down);
        vm.input_flush();
        if down {
            if !self.keys_down.contains(&qcode) {
                self.keys_down.push(qcode);
            }
        } else {
            self.keys_down.retain(|&k| k != qcode);
        }
    }

    /// Whether the chord holds Delete in the guest (its key's release ends
    /// it).
    pub fn cad_held(&self) -> bool {
        self.cad_held
    }

    /// Ctrl+Alt+Del in the guest, down or up. The real chord is the host's
    /// (on Windows no program can have it, on Linux the desktop takes it),
    /// so the guest gets it from one nobody else uses (Ctrl+Alt+Shift+D).
    /// The hand is holding Ctrl and Alt, so the guest has them already:
    /// this lets Shift go and presses Delete, and D's release lets Delete
    /// go. That is a press as long as the hand's, never a zero-length one.
    /// From a menu, nothing is held: the chord presses Ctrl and Alt too,
    /// and lets go of them with Delete.
    pub fn ctrl_alt_del(&mut self, vm: Option<Qemu>, down: bool) {
        let Some(vm) = vm else { return };
        let q = qemu_embed::atset1_to_qcode;
        let del = q(0xE053);
        if down {
            if self.cad_held {
                return; // key repeat
            }
            for shift in [q(0x2A), q(0x36)] {
                if self.keys_down.contains(&shift) {
                    vm.key(shift, false);
                    self.keys_down.retain(|&k| k != shift);
                }
            }
            // a modifier pressed before the window had focus never reached
            // the guest: press one for it
            for pair in [[q(0x1D), q(0xE01D)], [q(0x38), q(0xE038)]] {
                if !pair.iter().any(|k| self.keys_down.contains(k)) {
                    vm.key(pair[0], true);
                    self.keys_down.push(pair[0]);
                    self.cad_extra.push(pair[0]);
                }
            }
            vm.key(del, true);
            self.keys_down.push(del);
        } else {
            let extra = std::mem::take(&mut self.cad_extra);
            for k in std::iter::once(del).chain(extra.into_iter().rev()) {
                vm.key(k, false);
                self.keys_down.retain(|&d| d != k);
            }
        }
        self.cad_held = down;
        vm.input_flush();
    }

    /// Release every key the guest still sees as held (focus loss).
    pub fn lift_all(&mut self, vm: Option<Qemu>) {
        // The pad's keys too, and through the `KeyMap` rather than behind
        // its back: a pad holding a direction when the window loses focus
        // must both stop pressing it in the guest *and* have the map
        // agree it is no longer down, or the next poll sees no change and
        // never presses it again.
        self.release_pad_keys(vm);
        // Delete and any modifier the chord pressed are in `keys_down`
        self.cad_held = false;
        self.cad_extra.clear();
        let keys = std::mem::take(&mut self.keys_down);
        if keys.is_empty() {
            return;
        }
        if let Some(vm) = vm {
            for qcode in keys {
                vm.key(qcode, false);
            }
            vm.input_flush();
        }
    }

    /// The gamepad, once per published guest frame: from the front end's
    /// wake and not its redraw, because a wake arrives for every publish
    /// but an occluded or minimized window is redrawn for none of them,
    /// and a pad that stopped being read whenever the window went behind a
    /// terminal would fail in exactly the headless runs the scripted
    /// source exists for. Then what the machine says that means. The two
    /// are split because the pad is read whatever the setting: a machine
    /// with the pad off still logs under PLAYER_PAD_LOG, which is how
    /// someone works out whether the controller is seen at all before
    /// deciding to turn it on.
    pub fn poll_pads(&mut self, session: &Session) {
        if let (Some(pads), Some(display)) = (self.pads.as_mut(), session.display()) {
            pads.poll(display.published_seq());
        }
        self.apply_pad_keys(session.vm());
        self.apply_pad_device(session.vm());
    }

    /// The pad's current controls as key presses in the guest. Does
    /// nothing unless the machine asked for `--pad keys`.
    fn apply_pad_keys(&mut self, vm: Option<Qemu>) {
        let (Some(pads), Some(km)) = (self.pads.as_ref(), self.pad_keys.as_mut()) else {
            return;
        };
        let changes = km.apply(pads);
        if changes.is_empty() {
            return;
        }
        let log = std::env::var("PLAYER_PAD_LOG").is_ok();
        let named: Vec<(u32, bool, &'static str)> =
            changes.iter().map(|&(sc, d)| (sc, d, km.key_name(sc))).collect();
        let Some(vm) = vm else { return };
        for (sc, down, name) in named {
            let qcode = qemu_embed::atset1_to_qcode(sc);
            if qcode == 0 {
                continue;
            }
            if log {
                eprintln!("[pad] key {name} {}", if down { "down" } else { "up" });
            }
            vm.key(qcode, down);
        }
        // One flush for the whole batch: a stick crossing centre is a
        // release and a press together, and the guest should see them in
        // the same drain rather than a frame apart.
        vm.input_flush();
    }

    /// Send the pad's current state to whichever pad device the machine
    /// has, the `usb-gamepad` (M13 path A) or the `gameport` (path B). One
    /// call for both: `qemu_embed_pad_state` offers the state to each and
    /// the absent one ignores it, so the player never has to know which
    /// device the bundle chose, only that there is one.
    ///
    /// The whole pad every time, not a change: each device compares
    /// against what it holds and does nothing when they match, so an
    /// untouched controller costs one comparison per published frame and
    /// never wakes the guest. Sending changes instead would put the "did
    /// anything move" question on this side of a queue that is allowed to
    /// drop, and a dropped button-up is a button held down in the guest
    /// forever.
    fn apply_pad_device(&mut self, vm: Option<Qemu>) {
        if !self.pad_mode.is_device() {
            return;
        }
        let Some(pads) = self.pads.as_ref() else { return };
        let (axes, hat, buttons) = pads.hid_state();
        let Some(vm) = vm else { return };
        vm.pad_state(axes, hat, buttons);
        vm.input_flush();
    }

    /// Let go of everything the pad is holding in the guest.
    fn release_pad_keys(&mut self, vm: Option<Qemu>) {
        let Some(km) = self.pad_keys.as_mut() else { return };
        let changes = km.release_all();
        if changes.is_empty() {
            return;
        }
        let Some(vm) = vm else { return };
        for (sc, _) in changes {
            let qcode = qemu_embed::atset1_to_qcode(sc);
            if qcode != 0 {
                vm.key(qcode, false);
            }
        }
        vm.input_flush();
    }
}
