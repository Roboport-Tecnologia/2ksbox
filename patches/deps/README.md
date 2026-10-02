# Patches on the libraries `scripts/build-deps.sh` builds

`build-deps.sh` unpacks each pinned tarball into `build/deps/src/` and,
when `patches/deps/<name>/` exists, applies its `*.patch` files in
filename order (`patch -p1`, git-format diffs) to that fresh tree. The
tree remembers which set it carries (`.patches`, a hash of the files);
a changed set unpacks the tarball again and reapplies, and the package's
build stamp carries the same hash, so editing a patch rebuilds that one
package on the next `scripts/build.sh` and nothing else.

| Package | Patch | What / why | Drop when |
|---|---|---|---|
| qtdeclarative 6.9.3 | `01-macos-button-title-centered` | upstream 8bb39ac (6.10.1): the Quick Controls macOS style's push-button title margins were 5 pt top / 9 pt bottom, tuned for the pre-Tahoe bevel; on macOS 26's symmetric capsule every button label sat 2 pt high. qtbase 6.9.3 already has `qt_apple_runningWithLiquidGlass()`; only Quick's private copy of the style was stale. The open-source 6.9 branch closed after 6.9.3 and never got it | Qt moves past 6.9 (6.10.1+ carries it) |
| qtdeclarative 6.9.3 | `02-macos-button-layout-margins` | upstream d10da51 (6.10.1): the layout-item rect (what the focus ring surrounds) for the same capsule, so the pair matches 6.10.1 | same |
| libtpms 0.10.2 | `01-strstr-const` | recent glibc (2.44 on the Linux box) makes `strstr()` on a const string return `const char *` (C23), and libtpms builds with a fixed `-Werror`, so `TPMLIB_GetPlaintext()` stops the build. One declaration made `const`, the same change as libtpms's master | libtpms moves past 0.10.2 |
