#!/usr/bin/env bash
# Regenerate packaging/flatpak/cargo-sources.json from Cargo.lock.
#
# Flathub builds have no network, so the Flatpak cannot let cargo fetch
# crates: every one has to be a declared source with a checksum, which is
# what this file is (every crate + its .cargo-checksum.json + the cargo
# config that redirects crates-io at the vendor directory).
#
# Two lock files, one vendor directory. `launcher-mitsuami/` is its own
# cargo workspace (ADR-023) and the launcher the Flatpak installs, so its
# crates are declared too, mitsuami itself as a git source at its pinned
# rev. The generator takes one lock file at a time, so it
# runs once per lock and the results are merged on their `dest`. The two
# share most crates at identical versions, and one `cargo/config` covers
# both because `CARGO_HOME` is the same for both builds.
#
# Run it after any dependency change in either workspace, and commit the
# result:
#   scripts/gen-flatpak-cargo-sources.sh
#
# The generator is upstream's (flatpak/flatpak-builder-tools, MIT), pinned
# to a commit and checked against its hash, since this repo executes it.
# It needs aiohttp and
# tomlkit, supplied by uv so nothing is installed on the host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

COMMIT=f03a673abe6ce189cea1c2857e2b44af2dd79d1f   # 2025-08-16, "cargo: unwrap tomldoc"
SHA256=b373c8ab1a05378ec5d8ed0645c7b127bcec7d2f7a1798694fbc627d570d856c
URL="https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/$COMMIT/cargo/flatpak-cargo-generator.py"
TOOL="$ROOT/build/flatpak-tools/flatpak-cargo-generator.py"
OUT="$ROOT/packaging/flatpak/cargo-sources.json"

command -v uv >/dev/null || { echo "uv not found — https://docs.astral.sh/uv/" >&2; exit 1; }
mkdir -p "$(dirname "$TOOL")"
if ! [ -f "$TOOL" ] || ! echo "$SHA256  $TOOL" | sha256sum -c --status; then
  echo "==> fetching flatpak-cargo-generator ($COMMIT)"
  curl -fsSL -o "$TOOL" "$URL"
  echo "$SHA256  $TOOL" | sha256sum -c --status || {
    echo "gen-flatpak-cargo-sources.sh: the generator's hash does not match; refusing to run it" >&2
    rm -f "$TOOL"; exit 1; }
fi

LOCKS=(Cargo.lock launcher-mitsuami/Cargo.lock)
PARTS=()
for lock in "${LOCKS[@]}"; do
  part="$ROOT/build/flatpak-tools/$(echo "$lock" | tr / -).json"
  echo "==> generating from $lock"
  uv run --quiet --python 3.12 --with aiohttp --with tomlkit --with PyYAML \
    python "$TOOL" "$lock" -o "$part"
  PARTS+=("$part")
done

echo "==> merging into $OUT"
python3 - "$OUT" "${PARTS[@]}" <<'EOF'
import json, sys
out, parts = sys.argv[1], sys.argv[2:]
merged, seen, clashes = [], {}, []
for part in parts:
    for e in json.load(open(part)):
        # A source is identified by where it lands. The same crate at the
        # same version in both lock files is one entry; the same path with
        # different contents is a real conflict (two versions of a crate
        # would land in differently named directories, so this can only be
        # the cargo config, and both generators write the same one).
        key = (e.get("dest"), e.get("dest-filename"), e["type"])
        if key in seen:
            if seen[key] != e:
                clashes.append(key)
            continue
        seen[key] = e
        merged.append(e)
assert not clashes, f"sources disagree about {clashes}"
kinds = {}
for e in merged:
    kinds[e["type"]] = kinds.get(e["type"], 0) + 1
crates = [e for e in merged if e["type"] == "archive"]
assert crates, "no crate archives generated"
assert all(e["url"].startswith("https://") and e.get("sha256") for e in crates), \
    "a crate source has no https url or no checksum"
cfg = [e for e in merged if e.get("dest") == "cargo" and e.get("dest-filename", "").startswith("config")]
assert len(cfg) == 1, f"expected exactly one cargo config entry, got {len(cfg)}"
# mitsuami is what the second lock file is here for; a merge that lost it
# would be an offline build that fails hours in.
assert any("mitsuami" in e.get("url", "") for e in merged), "no mitsuami source: launcher-mitsuami's lock did not make it in"
json.dump(merged, open(out, "w"), indent=4)
open(out, "a").write("\n")
print(f"{len(merged)} sources: {kinds}")
EOF
