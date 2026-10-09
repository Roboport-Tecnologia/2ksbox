# 24. Integration: shared folders and the clipboard

Track M23 (`tracks/m23-integration.md`), ADR-027. Doc 01 had these as
later nice-to-haves and doc 07 left them out of v1. ADR-024 makes them
due sooner: a modern guest's user expects them on day one. **Windows 11
comes first** (user decision, 2026-10-02). The vintage families follow
on the same host side (§5).

## 1. What the user gets

- **A host folder as a network drive.** The machine form has a "Shared
  folder" path (on its Network page; it needs Networking on). While the machine runs, the guest sees it as
  `\\10.0.2.4\host`, read-write and live, mapped to a drive letter by our
  guest agent. It needs no guest driver, because Windows' own SMB client
  does the work.
- **The clipboard both ways**, text first. You copy on the host and paste
  in the guest, and the reverse.

Both are per machine. The shared folder is off by default, the way the
network is; the clipboard is on for a new Windows 11 machine (a bundle
without the field has it off).

## 2. Shared folders: an SMB server in the player

**Decision (user):** an SMB server written in Rust, running inside the
player process and reached through slirp. These were rejected:

- **virtiofs.** It needs `virtiofsd`, a separate vhost-user daemon that
  only runs on Linux. Nothing serves it on macOS or Windows hosts.
  virtio-win does ship `viofs` for ARM64 Windows 11, so the guest half
  exists.
- **QEMU's `-netdev user,smb=`.** It runs the host's Samba `smbd`, which
  is a separate install that we cannot ship (GPLv3 and very large). It
  is also not built for Windows hosts.
- **A filesystem driver of ours in the guest** (an NT minifilter or
  redirector, a 9x IFS VxD). Each one is a project the size of a display
  driver, and every guest family would need its own.
- **A writable virtual FAT disk** (vvfat-like). It is not live, and the
  host and guest both writing to it can conflict.

No Rust SMB *server* crate exists. The `smb` and `smb2` crates are
clients. So the server is ours: `libsmb/`, a library with no QEMU
dependency, kept independent of 2ksbox so it can be published on its own
(user, 2026-10-02), and tested against real clients by itself.

### 2.1 The path from the guest

```
guest SMB client ─TCP→ 10.0.2.4:445 (slirp) ─AF_UNIX→ <runtime dir>/smb.sock ─→ libsmb (player thread)
```

- libslirp 4.9.5 has `slirp_add_unix(slirp, path, guest_addr, port)`.
  For each guest connection it opens a new connection to a Unix socket,
  so the server sees ordinary per-connection streams. QEMU's
  `guestfwd=` knows only `cmd:` and a chardev, and a chardev carries one
  connection for the machine's whole life. **QEMU patch 79** adds
  `guestfwd=tcp:10.0.2.4:445-unix:<path>`. The launcher gives the player
  `--share <dir>` when the machine has a shared folder, and the player
  adds the `guestfwd` to the machine's `-netdev` and owns the listener
  (`player-core/src/share.rs`).
- `slirp_add_unix` is `G_OS_UNIX` only. Windows 10 and later have
  `AF_UNIX` (`afunix.h`), so Windows hosts need a `patches/deps/libslirp`
  patch that turns it on there. That patch is not written yet (M23 step
  7); the shared folder works on macOS and Linux hosts.
- **The socket is the security boundary.** It sits in a per-run
  directory only the user can open (mode 0700), and slirp exposes it
  only at 10.0.2.4 on the guest's NAT. So the credentials can be a fixed
  pair (`2ksbox` / `2ksbox`). They exist because Windows 11 refuses guest
  logons, not to keep anyone out.

### 2.2 The protocol subset

Windows 11 24H2 requires **signing** on its client by default (Pro and
up), and with signing required it does not fall back to a guest logon.
So the first server needs:

- NEGOTIATE, 2.0.2 through 3.1.1: HMAC-SHA256 signing for 2.x, and
  AES-CMAC for 3.x, with 3.1.1's SHA-512 preauthentication hash. Windows
  11 picks 3.1.1, and takes 2.1 too when that is the server's highest
  (step 2). An SMB1 NEGOTIATE that offers `SMB 2.???` is answered with
  0x02FF; Windows and macOS still open that way.
- SESSION_SETUP over SPNEGO / NTLMSSP with NTLMv2 checked against the
  fixed password. The session key comes from it and signs every message.
- TREE_CONNECT (`IPC$` and the one share), CREATE, CLOSE, READ, WRITE,
  QUERY_DIRECTORY, QUERY_INFO / SET_INFO (basic, standard, rename,
  disposition, end of file), FLUSH, ECHO, and IOCTL refusing everything
  except validate-negotiate.
- Credits enough for Explorer's pipelining. **Read-caching leases**
  (`libsmb/src/lease.rs`, M23 2026-10-03): without one Windows reads
  every few kilobytes over the wire, however often it read the bytes
  before. Only read caching, whose break needs no acknowledgement, so no
  request ever waits on a client's flush. Broken when another open (another
  lease key, or none) writes, resizes, renames or deletes the file, or
  when the file changes on the host (the connection's watcher compares
  size and time once a second). No oplocks.
- `CHANGE_NOTIFY`: an interim STATUS_PENDING, then a signed async
  completion from a watcher thread that polls the folder once a second
  (no file-watching dependency). A one-folder watch gets real change
  records, and a tree watch gets "enumerate again" (M23 step 3).

Path handling is where the safety lives. Every name is resolved under
the share's root with no `..` and no symlink escaping it (the user's
folder, nothing above it). Windows names are case-insensitive, while
Linux hosts are case-sensitive: the server looks up names
case-insensitively on case-sensitive hosts and refuses ambiguous
matches.

### 2.3 The vintage guests later

Win98 and XP speak SMB1 (`NT LM 0.12`), and Win98 also uses share-level
security. That is a second dialect on the same filesystem layer and the
same socket. It is M23's step 8, not scoped yet. Until then a vintage machine keeps the folder
disc (`isodir:`, track M5g).

## 3. The clipboard: the SPICE agent protocol, QEMU's host side

QEMU 11.1 carries the host half already. `-chardev
qemu-vdagent,id=vda,clipboard=on` speaks the SPICE agent protocol to a
guest agent and feeds `ui/clipboard.c`, where front ends join as
*clipboard peers* (`qemu_clipboard_peer_register`). The guest end is a
virtio-serial port named `com.redhat.spice.0`:

```
-device virtio-serial-pci -device virtserialport,chardev=vda,name=com.redhat.spice.0
```

- **QEMU build:** `vdagent.c` builds only `when: spice_protocol`. That
  means the spice-protocol **headers** (no SPICE server), so
  `build-deps.sh` builds spice-protocol from a pinned tarball and
  `configure-qemu.sh` passes `--enable-spice-protocol` (macOS and Linux;
  Windows hosts still build without it, M23 step 7). `--disable-spice`
  stays.
- **The embed API** has a clipboard peer for the player (API version
  11): the guest grabbing, the host's text arriving, and requests both
  ways.
- **The player** bridges that peer to the host's clipboard in
  `player-core/src/clipboard.rs`, through the `arboard` crate: a thread
  polls the host's clipboard twice a second, and each side remembers
  what it last took from the other so nothing bounces back. Only for a
  machine whose command line has a `qemu-vdagent`. A front end with a
  toolkit clipboard (mitsuami) can take over later. Text only: QEMU's
  peer interface has `QEMU_CLIPBOARD_TYPE_TEXT` and nothing richer.
- **The guest driver:** virtio-win's `vioserial`, which has a signed
  ARM64 Windows 11 build (`vioserial/w11/ARM64`), joins our drivers disc
  (`scripts/build-virtio-win.sh`) beside NetKVM and viogpudo. x64
  Windows 11 needs the same driver, from the same ISO: its own disc,
  `2ksbox-drivers-x64.iso`, holds `vioserial/w11/amd64`,
  `viogpudo/w11/amd64` (for the virtio-gpu, since 2026-10-08) and the
  agent.
- **The host's side on Linux** reads and writes the clipboard through
  the Wayland compositor's data-control protocol where there is one
  (`arboard`'s `wayland-data-control`), X11 otherwise.
- **The guest agent is ours** (§4). Red Hat's `vdagent-win` exists, but
  its builds are x86 / x64 only. It runs as a service plus a session
  process, and it ships inside an installer (`virtio-win-guest-tools.exe`).

## 4. The guest agent

`guest-agent/` is a small Rust program for modern Windows (its own cargo
workspace). It is built for x64 (`x86_64-pc-windows-gnu`, mingw-w64 on
the build host); Windows 11 on Arm runs it under its x64 emulation, since
no ARM64 Windows toolchain is on the Mac yet. It runs in the user's
session **with the user's elevated token**: `vioser` lets only SYSTEM and
Administrators open the port (an unelevated agent gets error 5), and
in the user's session it shares that session's clipboard. Red Hat's
agent solves the same with a SYSTEM service plus a per-session process;
ours is one process, started at logon by a task set to run with highest
privileges, so the user must be an administrator (Windows 11's first
user is). `2ksbox\install.cmd` on the drivers disc installs it
(`install.ps1`: the drivers, the agent in `C:\Program Files\2ksbox`,
and its logon tasks; and, track M24, viogpudo's resolution service from
the disc's `$WinPEDriver$\viogpudo` in `C:\Program Files\2ksbox\viogpu`,
without which the desktop takes the window's size only at boot:
`vgpusrv` starts `viogpuap` in the console session, which applies each
new size with `SetDisplayConfig`). It does three jobs:

1. **Clipboard:** it opens `\\.\Global\com.redhat.spice.0` and speaks
   the agent protocol subset QEMU's `vdagent.c` implements: it announces
   only CLIPBOARD_BY_DEMAND, so GRAB / REQUEST / CLIPBOARD carry no
   selection byte or serial; `CF_UNICODETEXT` ↔ UTF-8, CRLF ↔ LF. It
   watches the clipboard with `AddClipboardFormatListener` and reads
   150 ms after the last change notice (a program still finishing its
   own write, .NET's for one, fails if the clipboard is opened under it),
   and does not send back a change it made itself.
2. **The share:** `2ksbox-agent --map`, from a second logon task that
   runs *unelevated*, since a drive mapped from the elevated token's
   session is invisible to Explorer's. If `10.0.2.4:445` answers within a
   minute it maps the share to the first free letter from Z: down
   (`WNetAddConnection2`, the fixed credentials) and exits. It shows no
   UI, and a machine without a shared folder simply has no drive.
3. **The screen (track M24):** a Windows 11 machine keeps a second card
   (the standard VGA on x64, ramfb on Arm) for setup, which has no virtio
   driver, and for recovery, and Windows extends the desktop onto both
   with the other card's screen as the primary, which is the one DWM
   paces on, while the player shows the virtio-gpu's. Once Explorer's
   taskbar exists, the agent makes the virtio-gpu's screen
   (`VEN_1AF4&DEV_1050` in the adapter's path) the only one with
   `SetDisplayConfig`, saved to Windows' display database; with no
   working virtio-gpu screen it changes nothing. The database keeps a
   layout per set of connected screens, so a recovery boot or a broken
   driver gets the other card's screen back by itself. Logged as
   `display:` lines.

Its log goes to `C:\2KSBOX\agent.log`, the guest-output convention.

## 5. Order and what the vintage families reuse

Windows 11 on Arm on the Air went first, then x64 Windows 11 (on Linux,
2026-10-04); Windows hosts and the vintage families are next (M23 steps
7 and 8). The SMB server and the clipboard peer are host code
that every family shares. A vintage agent (C, mingw, Win98 / XP) would
speak the same agent protocol over a **COM port**: `qemu-vdagent` is a
chardev, so `-serial chardev:vda` works with no driver, and the Win32
serial API is in every Windows. The track doc carries that plan once it
is scheduled.
