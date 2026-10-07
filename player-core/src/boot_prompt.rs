//! Windows setup's "Press any key to boot from CD or DVD" (track M20),
//! answered for the user.
//!
//! Microsoft's install discs boot through `cdboot.efi`, which waits five
//! seconds for a key and otherwise hands back to the firmware, which goes
//! on to an empty disk and the EFI shell. On a new machine nobody is
//! watching for it: its screen is black. The disc's `cdboot_noprompt.efi`
//! does not run from the firmware's view of the disc (it returns at once),
//! so the prompt is answered instead.
//!
//! A Windows 11 machine's command line names the chardev [`CHARDEV`] as
//! its serial port (`bundle::Machine::boot_prompt_args`), which carries
//! the firmware's console both ways. EDK2 prints every boot option it
//! starts there (`PlatformBmPrintScLib`, in ArmVirtQemu and OVMF alike,
//! release builds too): when it starts a DVD-ROM, a carriage return goes
//! back down the same line, and its terminal driver turns it into the
//! Enter key `cdboot.efi` waits for. A key sent before `cdboot.efi` reads
//! stays queued, so no delay is needed, and nothing depends on the
//! window having the keyboard. An installed machine boots its disk
//! first (`bootindex`), so the disc never starts and nothing is sent.

use std::io::{BufRead, BufReader, Write};

/// The chardev a machine's `-serial chardev:<id>` names; the player
/// creates it on a socketpair, as it does the monitor's.
pub const CHARDEV: &str = "fwcon";

/// Before `Qemu::new`: when the command line wants [`CHARDEV`], supply it
/// and start the thread that watches the firmware's console.
pub fn attach(args: &mut Vec<String>) {
    let serial = format!("chardev:{CHARDEV}");
    if !args.iter().any(|a| *a == serial) {
        return;
    }
    let (ours, theirs) = match crate::qmp::socket_pair() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("[boot] socketpair failed: {e}; the disc's prompt is the user's to answer");
            return;
        }
    };
    let writer = match ours.try_clone() {
        Ok(w) => w,
        Err(e) => {
            eprintln!("[boot] {e}; the disc's prompt is the user's to answer");
            return;
        }
    };
    match crate::qmp::into_raw(theirs) {
        Ok(fd) => args.extend(["-chardev".into(), format!("socket,id={CHARDEV},fd={fd}")]),
        Err(e) => {
            // QEMU would refuse the dangling `-serial chardev:` reference
            eprintln!("[boot] {e}; no firmware console");
            if let Some(i) = args.iter().position(|a| *a == serial).filter(|&i| i > 0) {
                args.drain(i - 1..=i);
            }
            return;
        }
    }
    std::thread::Builder::new()
        .name("boot-prompt".into())
        .spawn(move || watch(ours, writer))
        .expect("spawn boot-prompt");
}

/// Read the firmware's console until QEMU closes it; answer each disc
/// boot.
fn watch(reader: crate::qmp::Stream, mut writer: crate::qmp::Stream) {
    let mut rd = BufReader::new(reader);
    let mut line = Vec::new();
    loop {
        line.clear();
        match rd.read_until(b'\n', &mut line) {
            Ok(0) | Err(_) => return,
            Ok(_) => {}
        }
        // `BdsDxe: starting Boot0001 "UEFI QEMU DVD-ROM QM00003 " from …`
        let text = String::from_utf8_lossy(&line);
        if let Some(at) = text.find("BdsDxe:") {
            // which option the firmware tried, and why it went on
            eprintln!("[firmware] {}", text[at..].trim_end());
        }
        if text.contains("BdsDxe: starting Boot") && text.contains("DVD-ROM") {
            eprintln!("[boot] the firmware starts a disc; answering its prompt");
            if let Err(e) = writer.write_all(b"\r") {
                eprintln!("[boot] {e}");
            }
        }
    }
}
