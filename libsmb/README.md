# libsmb

A small SMB2/3 file server to embed in another program. Each share is a
folder on the host, one account logs in with NTLMv2 (inside SPNEGO), and
every response is signed, which is what current Windows clients insist on
(Windows 11 24H2 requires signing and refuses guest logons).

- Dialects 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1; HMAC-SHA256 signing for 2.x,
  AES-128-CMAC for 3.x, with 3.1.1's SHA-512 preauthentication hash.
- Compound requests; read, write, create / overwrite, rename, delete
  (on close or by disposition), directory listing in the classes Windows,
  macOS and Samba clients use, file and volume information, a permissive
  security descriptor.
- The `srvsvc` and `wkssvc` RPC pipes on `IPC$` (DCE/RPC over SMB): the
  share list (`\\server` in Explorer) and share information. Windows asks
  for them on every open through a `\\server\share` path, and without
  them waits out a fallback each time.
- Names resolved under the share's root only: no `..`, no symlink out of
  it, case-insensitive lookup on case-sensitive hosts.
- Change notification from a watcher that polls once a second: the names
  added, removed or modified for a one-folder watch (FileSystemWatcher's
  Created / Deleted / Changed), "enumerate again" for a whole tree.
- Read-caching leases (SMB 2.1+), so a client caches file data: broken
  when another open writes the file, or when it changes on the host (a
  watcher compares size and time once a second). Write and handle
  caching are not granted. `Config::leases(false)` / `--no-leases` turns
  them off.
- Not supported: encryption, oplocks, write and handle leases, durable handles, DFS,
  named streams, SMB1 (an SMB1 negotiate that offers
  SMB2 is answered so the client moves on).

It is meant for one trusted client over a private transport (a virtual
machine reaching the host through its NAT, a Unix socket only the user
can open), not for a network: the transport is the security boundary.

```rust
use libsmb::{Account, Config, Server, Share};
let cfg = Config::new("myhost", Account::new("user", "secret"))
    .share(Share::new("host", "/some/folder"));
Server::new(cfg).listen_tcp("127.0.0.1:4450")?;
```

`smbserve` is a command-line wrapper for trying clients:

```sh
cargo run --release --bin smbserve -- --tcp 127.0.0.1:4450 --user smb --password smb -v /some/folder
mount_smbfs //smb:smb@127.0.0.1:4450/host /tmp/mnt        # macOS
smbclient //127.0.0.1/host -p 4450 -U smb%smb              # Samba
```

Tested against macOS's client (2.1 and 3.1.1) and Windows 11's (2.1
and 3.1.1, build 26300, ARM64): Explorer's copy engine both ways, large
files, editing in place, attributes and times, change notification.

Licensed under either of MIT or Apache-2.0, at your option.
