#!/usr/bin/env bash
# Check that this checkout is ready to release, and tag it.
#
#   scripts/release.sh check     # every check, changes nothing (the default)
#   scripts/release.sh tag       # the checks, then an annotated v<version>
#                                # tag on HEAD, pushed to origin
#
# The version is the root Cargo.toml's. A store build is made from the
# tagged commit, so the tag is the source behind every binary a store
# sells (ADR-028; the GPL's source offer). Tag after the store packages
# are packed and before they are uploaded: docs/release.md.
#
# The checks: on main, no uncommitted change to a tracked file (a
# submodule's patched tree does not count), level with origin/main, no
# tag of that version yet, and the version agreed on by the two
# mitsuami workspaces, the Windows manifest, the Linux metainfo's newest
# release and the manual's release notes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE=${1:-check}
case "$MODE" in
  check|tag) ;;
  *) echo "usage: scripts/release.sh [check|tag]" >&2; exit 2 ;;
esac

fail=0
bad() { echo "  FAIL  $*"; fail=1; }
ok()  { echo "  ok    $*"; }

VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)"
TAG="v$VERSION"
echo "==> 2ksbox $VERSION ($TAG)"

branch="$(git rev-parse --abbrev-ref HEAD)"
[ "$branch" = main ] && ok "on main" || bad "on $branch, not main"

if [ -z "$(git status --porcelain --ignore-submodules=dirty --untracked-files=no)" ]; then
  ok "no uncommitted change"
else
  bad "uncommitted changes:"; git status --short --ignore-submodules=dirty --untracked-files=no | sed 's/^/          /'
fi

git fetch -q origin main --tags
if [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ]; then
  ok "level with origin/main"
else
  bad "HEAD $(git rev-parse --short HEAD) is not origin/main $(git rev-parse --short origin/main)"
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  bad "$TAG exists already, on $(git rev-list -n1 --abbrev-commit "$TAG")"
else
  ok "no $TAG yet"
fi

# expect <what> <found>
expect() {
  [ "$2" = "$VERSION" ] && ok "$1: $2" || bad "$1 says '${2:-nothing}', not $VERSION"
}
expect "launcher-mitsuami/Cargo.toml" "$(sed -n 's/^version = "\(.*\)"/\1/p' launcher-mitsuami/Cargo.toml | head -1)"
expect "player-mitsuami/Cargo.toml" "$(sed -n 's/^version = "\(.*\)"/\1/p' player-mitsuami/Cargo.toml | head -1)"
expect "packaging/windows/app.manifest" \
  "$(sed -n 's/.*assemblyIdentity .*version="\([0-9.]*\)\.0".*/\1/p' packaging/windows/app.manifest)"
# the first <release> is the newest, as AppStream wants them
expect "the Linux metainfo's newest release" \
  "$(grep -o '<release version="[^"]*"' packaging/linux/com._2ksbox.Launcher.metainfo.xml | head -1 | cut -d'"' -f2)"
if grep -qx "## $VERSION" manual/src/content/docs/release-notes.mdx; then
  ok "the manual's release notes have $VERSION"
else
  bad "manual/src/content/docs/release-notes.mdx has no '## $VERSION'"
fi

if [ "$fail" != 0 ]; then
  echo "release.sh: not ready to release $VERSION" >&2
  exit 1
fi
[ "$MODE" = check ] && { echo "==> ready; 'scripts/release.sh tag' tags $(git rev-parse --short HEAD) as $TAG"; exit 0; }

git tag -a "$TAG" -m "2ksbox $VERSION"
git push -q origin "$TAG"
echo "==> tagged $(git rev-parse --short HEAD) as $TAG and pushed it"
