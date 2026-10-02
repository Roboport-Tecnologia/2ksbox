//! winit's keys for the guest: the keys a host keymap moves, and the AT
//! set-1 scancode for a physical key (`player_core::keys`).

use winit::keyboard::KeyCode as K;
use winit::keyboard::{Key, KeyLocation, NamedKey};

/// The key the host's own keymap reads this one as, for the keys a keymap
/// option moves (xkb's `ctrl:swapcaps`, `ctrl:nocaps`, `caps:escape`,
/// `altwin:swap_alt_win`, …): a Caps Lock the host reads as Control is
/// Control to the guest too. `None` for every other key, which goes by
/// where it sits. The guest has a layout of its own, and a letter
/// translated by both would be a different letter.
pub fn as_host_reads(logical: &Key, location: KeyLocation) -> Option<K> {
    let Key::Named(named) = logical else {
        return None;
    };
    let right = location == KeyLocation::Right;
    let side = |l, r| if right { r } else { l };
    Some(match named {
        NamedKey::Control => side(K::ControlLeft, K::ControlRight),
        NamedKey::Shift => side(K::ShiftLeft, K::ShiftRight),
        NamedKey::Alt => side(K::AltLeft, K::AltRight),
        NamedKey::AltGraph => K::AltRight,
        NamedKey::Super => side(K::SuperLeft, K::SuperRight),
        NamedKey::CapsLock => K::CapsLock,
        NamedKey::Escape => K::Escape,
        _ => return None,
    })
}

/// The scancode for a winit key: `player_core::keys`' table, by the
/// key's W3C `code` name, which is what winit names its variants.
pub fn atset1(code: K) -> Option<u32> {
    player_core::keys::atset1(&format!("{code:?}"))
}
