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
| libtpms 0.10.2 | `01-strstr-const` | recent glibc (2.44 on the Linux box) makes `strstr()` on a const string return `const char *` (C23), and libtpms builds with a fixed `-Werror`, so `TPMLIB_GetPlaintext()` stops the build. One declaration made `const`, the same change as libtpms's master | libtpms moves past 0.10.2 |
| libslirp 4.9.5 | `01-windows-iconv-optional` | on Windows libslirp's meson requires iconv, for a mingw static glib whose `.pc` leaves it out; the glib `build-deps.sh` builds for MSVC's runtime (Windows) converts with its own win_iconv, so there is no iconv to find. Optional, so that build configures; mingw still finds and links its own | libslirp asks for iconv only when glib needs it |
