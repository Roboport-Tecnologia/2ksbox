//! GDK's hardware keycode (evdev + 8 on Wayland and X11) to an AT set-1
//! scancode. evdev's codes 1 to 88 are set 1's own; the rest is the
//! 0xE0-prefixed block.

pub fn atset1(gdk_keycode: u32) -> Option<u32> {
    let evdev = gdk_keycode.checked_sub(8)?;
    Some(match evdev {
        1..=88 => evdev,
        96 => 0xE01C,  // KP Enter
        97 => 0xE01D,  // Right Ctrl
        98 => 0xE035,  // KP /
        100 => 0xE038, // Right Alt
        102 => 0xE047, // Home
        103 => 0xE048, // Up
        104 => 0xE049, // Page Up
        105 => 0xE04B, // Left
        106 => 0xE04D, // Right
        107 => 0xE04F, // End
        108 => 0xE050, // Down
        109 => 0xE051, // Page Down
        110 => 0xE052, // Insert
        111 => 0xE053, // Delete
        125 => 0xE05B, // Left Meta
        126 => 0xE05C, // Right Meta
        127 => 0xE05D, // Menu
        _ => return None,
    })
}
