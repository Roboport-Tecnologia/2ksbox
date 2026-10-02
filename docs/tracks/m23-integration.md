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
3. **Hardening.** Explorer by hand (copying a tree both ways, opening
   files in place, a large file), CHANGE_NOTIFY, and Linux's
   `smbclient` in the host check.
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
- Windows 11 on Arm on the Air, through `tools/win11-spike.py`'s `SMB=`
  and `PROBE_PS1=tools/win11-spike/smb.ps1` on a scratch overlay of
  `build/w11d` (`OUT=build/w11s`). The user's machines are never booted.

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

## Open items

- Case-insensitive lookup on Linux hosts, and names Windows forbids
  (`:` and the like) that a host folder may hold.
- CHANGE_NOTIFY through a host watcher, so Explorer refreshes by itself.
