# Track M23: shared folders and the clipboard

Opened 2026-10-02 (user: "let's start working on folder sharing and
clipboard integration"). ADR-027, design in doc 24. User decisions at
the start: **Windows 11 first**, and a host folder reaches the guest
through **an SMB server in the player** (not virtiofs, Samba, a guest
filesystem driver or a virtual disk).

## Scope and files

- `libsmb/` (root workspace, new): the SMB2 server, no QEMU dependency.
- `guest-agent/` (new): the Windows 11 guest agent, x64 and ARM64.
- A QEMU patch: `guestfwd=…-unix:<path>` → `slirp_add_unix`.
- Later: a `patches/deps/libslirp/` patch (`AF_UNIX` on Windows hosts),
  spice-protocol in `scripts/build-deps.sh`, `--disable-spice-protocol`
  dropped in `scripts/configure-qemu.sh` (shared with M21), the embed
  API's clipboard peer (`embed/`, shared), the clipboard bridge in
  `player-core/` (shared with M22), `vioserial` on the drivers disc
  (`scripts/build-virtio-win.sh`, with M20), and the form's "Shared
  folder" in `launcher-core` (with M6).
- Doc 24 and this doc.

## Steps

1. **Open the track** (2026-10-02): doc 24, ADR-027, this doc.
2. **The SMB spike.** `libsmb` with NEGOTIATE (2.1), NTLMv2 session
   setup and signing, tree connect, and enough of CREATE /
   QUERY_DIRECTORY / READ / CLOSE to list and read a folder. It is
   checked first against `smbclient` / the `smb` crate over TCP on
   localhost, then against Windows 11 on Arm on the Air through the
   `unix:` guestfwd. **The question to answer: does 24H2 accept 2.1 with
   signing, or does it need 3.1.1?**
3. **Read-write.** WRITE, SET_INFO (rename, delete, size, times),
   create and overwrite dispositions, directories, and Explorer copying
   a tree both ways. A tool goes into `scripts/test.sh`, driving a
   client against the server on the host.
4. **The clipboard's host side.** spice-protocol headers, `qemu-vdagent`
   built, an embed clipboard peer, and the player bridge.
5. **The guest agent.** The clipboard, mapping the share, the drivers
   disc's `vioserial` and the agent's autostart.
6. **The launcher.** The form's "Shared folder" and "Clipboard" rows,
   the arguments, and their sentences in `launcher-core`.
7. **Windows hosts.** The libslirp `AF_UNIX` patch, and the PC.
8. **Vintage** (to be scoped): SMB1 for Win98 / XP, and a C agent over a
   COM port (doc 24 §5).

## Test loop

To be written in step 2: a host-only check (server plus client, no
guest) and the Windows 11 on Arm check on the Air. Neither touches the
user's machines: the guest runs on a qcow2 overlay of an installed
Windows 11.

## Open items

- Which dialect 24H2 needs (step 2).
- Case-insensitive lookup on Linux hosts, and names Windows forbids
  (`:` and the like) that a host folder may hold.
- CHANGE_NOTIFY through a host watcher, so Explorer refreshes by itself.
