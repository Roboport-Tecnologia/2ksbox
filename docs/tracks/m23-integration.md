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
2. **The SMB spike** (done 2026-10-02). `libsmb`, a crate kept
   independent of 2ksbox (its own metadata, a generic API, `MIT OR
   Apache-2.0`; the user will publish it separately), and `smbserve`, its
   command-line wrapper. It already reads and writes (step 3's list),
   so step 3 is hardening. QEMU patch 79 adds `guestfwd=…-unix:<path>`.
   **The answer: Windows 11 accepts both 2.1 and 3.1.1 with signing.**
   Results below.
3. **Hardening** (scripted part done 2026-10-02). `tools/smb-win11-test.sh`
   covers what was meant to be checked by hand: Explorer's copy engine
   both ways, a large file, editing in place, and Explorer's window.
   CHANGE_NOTIFY is implemented. Left for the user once the launcher has
   the setting (step 6): browsing and dragging by mouse. Left here:
   leases (below) and Linux's `smbclient` in the host check.
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

- `tools/smb-host-test.sh` (`smb` in `scripts/test.sh`, host stage):
  `smbserve` against the host's own client, at 2.1 and 3.1.1, signing
  required (docs/testing.md).
- `tools/smb-win11-test.sh [build/w11d]` (step 3): Windows 11 on Arm on
  the Air, on a fresh overlay of an installed spike machine in
  `build/w11s`, running `tools/win11-spike/smb-explorer.ps1`. It prints
  PASS / FAIL per item, checks the guest's upload on the host, and lists
  what the server did not support. About 10 minutes. The user's
  machines are never booted.

## Step 2's results (2026-10-02)

| Client | Dialect | Signing | Read, write, copy, rename, delete |
|---|---|---|---|
| macOS 26 `mount_smbfs` | 3.1.1 (its pick) | AES-CMAC | pass |
| macOS 26 `mount_smbfs` | 2.1 (server's cap) | HMAC-SHA256 | pass |
| Windows 11 on Arm, build 26300 | 3.1.1 (its pick) | AES-CMAC | pass |
| Windows 11 on Arm, build 26300 | 2.1 (server's cap) | HMAC-SHA256 | pass |

On Windows: `net use Z: \\10.0.2.4\host /user:smb smb`, then
`Get-SmbConnection` (signed, not encrypted), a recursive listing, a read,
a write, `notepad.exe` copied and hashed identically, and a folder made,
renamed and removed. About 350 requests per run, and no client signature
failed to verify. Notes:

- Windows (and macOS) open with an SMB1 multi-protocol NEGOTIATE that
  offers `SMB 2.???`; the server answers 0x02FF and the client sends
  SMB2's. Windows 11 still does this.
- After the login, Windows also tries the logged-on user's own
  credentials once (`login refused: UnknownUser("spike")` in the log). It
  does no harm.
- **Not ours, found on the way:** QEMU 11.1's HVF on the Mac aborts on
  `tpm-tis-device` (`HV_BAD_ARGUMENT`, `accel/hvf/hvf-all.c:123`). Its
  `tpm-ppi` RAM region is 1 KiB, smaller than a 16 KiB page, so HVF
  unmaps a range it never mapped. Windows 11 on Arm then does not start
  on the Mac at all, through the launcher too. It belongs to M21's Mac
  item. The spike ran with `TPM_PPI=off` (`ppi=off`, a `win11-spike.py`
  knob).

## Step 3's results (2026-10-02)

`tools/smb-win11-test.sh`, Windows 11 on Arm, 3.1.1 signed. Every item
passes:

| Item | Result |
|---|---|
| Listing a 3000-file folder | 3000 |
| Host tree down through Explorer's engine (`Shell.Application.CopyHere`): Unicode, a 200-character name, 10 levels deep, an empty file, 5 MB | 3006 files identical, 2.8 s |
| Guest tree up the same way, 205 files, 30 MB | identical on both sides, 2.6 s |
| A date kept (2005-05-05), the read-only attribute, a Unicode name | kept |
| 512 MB with `Copy-Item` | up 46 MB/s, down 49 MB/s |
| Append, truncate, `File.Replace` (temp file + backup), overwrite, case-only rename | pass |
| Drive size, NTFS, `Get-SmbConnection` signed | pass |
| FileSystemWatcher on a folder the host adds a file to | `Created tick-60.txt`, 1.1 s |
| Explorer's window on the share (screenshot) | lists it, Unicode names drawn |

Found and fixed on the way:

- **Read-only on create.** Explorer gives a file its attributes in
  CREATE, not afterwards. The server now applies read-only after its own
  open.
- **CHANGE_NOTIFY.** An interim STATUS_PENDING, then a signed async
  completion from a per-connection watcher thread that polls once a
  second. A one-folder watch gets real records (added / removed /
  modified), so .NET's FileSystemWatcher sees Created / Deleted /
  Changed. A whole-tree watch, or records too big for the client's
  buffer, get NOTIFY_ENUM_DIR ("enumerate again"). The watcher raises
  that only as an Error event, so it was not enough on its own. CANCEL
  and CLOSE end a pending watch. `Server::serve` now takes a reader and a
  writer.
- **The harness, not the server.** Two runs never started the probe:
  Windows refused the elevated PowerShell with "Windows cannot access the
  specified device, path, or file". The script then held `cmdkey
  /add … /pass:…`, and Defender scans a script named on a command line
  when the process starts. With `cmdkey` gone, and the script run as
  `main.ps1` under a harmless `stub.ps1` that logs any refusal, it
  launched first time. `win11-spike.py`'s probe now relaunches when
  nothing is written. That Defender caused it is the likely reading, not
  a proven one.

Not a server matter: the guest's clock is 7 hours fast in these runs,
because `win11-spike.py` passes no `-rtc` (the launcher passes
`base=localtime`). So copied files carry the guest's future times, and
folders keep the host's.

## Open items

- **Leases / oplocks.** None are granted, so Windows caches nothing: a
  hash of a 512 MB file read in 4 KiB pieces costs about 131,000 READs. A
  read lease (with a break when the host changes the file, through the
  same watcher) would let it cache. This is about speed only.
- Four IOCTLs are refused: 0x94264 (offload read), 0x900ef, 0x900a0 and
  0x9009c (object IDs). Nothing visible missed them.

- Case-insensitive lookup on Linux hosts, and names Windows forbids
  (`:` and the like) that a host folder may hold.
- CHANGE_NOTIFY through a host watcher, so Explorer refreshes by itself.
