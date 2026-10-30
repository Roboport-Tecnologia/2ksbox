#!/usr/bin/env python3
"""Parse the usb-gamepad HID report descriptor and check it says what the
device's packer writes (M13 path A, docs/tracks/m13-gamepads.md).

A HID report descriptor is a byte string a guest's driver parses, and a
wrong one does not fail: Windows enumerates the device, joy.cpl shows it,
and it has no axes, or it has axes whose values come from the wrong bits.
Nothing on this side notices, because nothing on this side reads it.

So this reads the bytes the device actually ships (out of the C array in
gamepad/qemu/dev-gamepad.c, not a copy that could drift) and checks the
three things that would each produce a plausible-looking, broken pad:

  * the collections balance, and the descriptor ends at depth 0;
  * the input items total exactly GAMEPAD_REPORT_LEN bytes, which is what
    usb_gamepad_poll() writes. A mismatch means the driver reads fields
    out of a report that does not have them;
  * the usages the guest looks for are declared: four axes, a hat *with a
    null state* (without it a released hat reads as north), and twelve
    buttons.

Exits non-zero with the complaint. The `pad` check in scripts/test.sh.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "gamepad" / "qemu" / "dev-gamepad.c"

# Generic Desktop usages we expect on the axes, in report order.
AXIS_USAGES = [0x30, 0x31, 0x32, 0x35]  # X, Y, Z, Rz
HAT_USAGE = 0x39


def read_descriptor(path):
    """The bytes of qemu_gamepad_hid_report_descriptor[] as shipped."""
    text = path.read_text()
    m = re.search(
        r"qemu_gamepad_hid_report_descriptor\[\]\s*=\s*\{(.*?)\n\};",
        text,
        re.S,
    )
    if not m:
        raise SystemExit(f"{path}: no qemu_gamepad_hid_report_descriptor[]")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body)]


def report_len(path):
    """GAMEPAD_REPORT_LEN, so the two cannot drift apart."""
    m = re.search(r"#define\s+GAMEPAD_REPORT_LEN\s+(\d+)", path.read_text())
    if not m:
        raise SystemExit(f"{path}: no GAMEPAD_REPORT_LEN")
    return int(m.group(1))


def parse(desc):
    """Walk the short items. Returns (bits, depth_ok, usages, hat_null)."""
    i = 0
    depth = 0
    max_depth_seen = 0
    bits = 0
    size = count = 0
    usage_page = None
    pending_usages = []
    axis_usages = []
    hat_null = False
    button_count = 0
    problems = []

    while i < len(desc):
        item = desc[i]
        # Long items (0xfe) do not appear in a descriptor like this one.
        if item == 0xFE:
            problems.append("long items are not expected here")
            break
        tag, typ, n = item >> 4, (item >> 2) & 3, item & 3
        n = 4 if n == 3 else n
        data = 0
        for k in range(n):
            data |= desc[i + 1 + k] << (8 * k)
        i += 1 + n

        if typ == 1:  # Global
            if tag == 0:  # Usage Page
                usage_page = data
            elif tag == 7:  # Report Size
                size = data
            elif tag == 9:  # Report Count
                count = data
        elif typ == 2:  # Local
            if tag == 0:  # Usage
                pending_usages.append((usage_page, data))
            elif tag == 1:  # Usage Minimum
                pending_usages.append((usage_page, data))
            elif tag == 2:  # Usage Maximum
                pending_usages.append((usage_page, data))
        elif typ == 0:  # Main
            if tag == 8:  # Input
                bits += size * count
                # Usage page 1 = Generic Desktop, 9 = Button.
                for page, usage in pending_usages:
                    if page == 0x01 and usage in AXIS_USAGES:
                        axis_usages.append(usage)
                    if page == 0x01 and usage == HAT_USAGE:
                        # bit 6 of an Input item is the null-state flag
                        if data & (1 << 6):
                            hat_null = True
                if usage_page == 0x09:
                    button_count += count
                pending_usages = []
            elif tag == 10:  # Collection
                depth += 1
                max_depth_seen = max(max_depth_seen, depth)
                pending_usages = []
            elif tag == 12:  # End Collection
                depth -= 1
                if depth < 0:
                    problems.append("End Collection without a Collection")
                    break
            else:
                pending_usages = []

    if i != len(desc):
        problems.append(f"descriptor did not parse cleanly: stopped at byte {i} of {len(desc)}")
    return bits, depth, axis_usages, hat_null, button_count, max_depth_seen, problems


def main():
    # An argument points it at another copy of the file, which is how the
    # check proves it can still fail: scripts/test.sh mutates a scratch
    # copy and requires a complaint.
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC
    desc = read_descriptor(src)
    want_bytes = report_len(src)
    bits, depth, axes, hat_null, buttons, max_depth, problems = parse(desc)

    if depth != 0:
        problems.append(f"collections do not balance: ended at depth {depth}")
    if max_depth < 2:
        problems.append(
            "expected an application collection with a physical one inside it; "
            f"deepest nesting was {max_depth}"
        )
    if bits % 8:
        problems.append(f"the input report is {bits} bits, which is not a whole number of bytes")
    elif bits // 8 != want_bytes:
        problems.append(
            f"the descriptor says {bits // 8} bytes but usb_gamepad_poll writes "
            f"GAMEPAD_REPORT_LEN = {want_bytes}"
        )
    if axes != AXIS_USAGES:
        problems.append(
            "expected axes X, Y, Z, Rz (0x30, 0x31, 0x32, 0x35) in that order, got "
            + (", ".join(hex(a) for a in axes) or "none")
        )
    if not hat_null:
        problems.append(
            "the hat switch has no null state: a released hat would read as north"
        )
    if buttons != 12:
        problems.append(f"expected 12 buttons, the descriptor declares {buttons}")

    if problems:
        print(f"hid-descriptor-check: {src}: FAIL")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(
        f"hid-descriptor-check: {len(desc)} bytes, {bits // 8}-byte report, "
        f"{len(axes)} axes, hat with null state, {buttons} buttons: OK"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
