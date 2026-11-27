# inno, vendored

The [`inno`](https://crates.io/crates/inno) crate 0.7.6
(<https://github.com/russellbanks/inno>, MIT OR Apache-2.0), which
`launcher-core/src/installer.rs` reads game installers with.

Three changes from the published crate. A string or blob the installer
gives a length for is read 10 KiB at a time, so a damaged or hostile
length cannot make it allocate gigabytes up front (upstream's own fix,
commit 00dcc77, after 0.7.6; `src/read/ext.rs`, `src/string.rs`). And a
file entry keeps its conditions
(`File::condition()`: `Components`, `Tasks`, `Languages`, `Check`,
`BeforeInstall`, `AfterInstall`). The crate reads and drops them, and
GOG's newer installers name each file and its parts only there (the
Galaxy layout, `installer.rs`). The change is in
`src/entry/condition.rs` (`Eq`, `PartialEq` derived) and
`src/entry/file/mod.rs` (the `condition` field and its accessor).

A third, smaller one: a slice's alternate base name from the header has
`:` replaced as well as `/` and `\` (`src/slice.rs`), since on Windows
`C:foo` names a path on another drive and `x:y` a stream of `x`.

The crate's tests and dev-dependencies are left out (they download
installers), and the root workspace excludes this directory.

Drop the copy for the published crate once a release has both.
