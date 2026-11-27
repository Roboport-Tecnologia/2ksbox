"""The third-party crates and licence texts the packages ship.

    python3 tools/third-party-notices.py list [--write THIRD-PARTY-NOTICES.md]
    python3 tools/third-party-notices.py stage DEST --host linux|macos|windows
                                          [--arch ARCH] [--drivers] [--vulkan]

`list` prints the crate listing of THIRD-PARTY-NOTICES.md, one section per
shipped binary, grouped by declared licence. It asks cargo for each
workspace a package builds from, filtered to the platforms the packages
are for.
`--write` replaces the file's listing (everything from its "## Crates"
heading on) instead of printing. It needs the network the first time, to
fetch the manifests of other platforms' crates.

`stage` fills a package's `licenses/` directory (scripts/package-*.sh):
the licence texts THIRD-PARTY-NOTICES.md names, copied from the source
tree, the libraries' unpacked sources in build/deps/src and
packaging/licenses/, and every shipped crate's own licence files for this
host. It fails, naming the file, when one is missing, so a package never
ships without a text its notices point at. `--drivers` adds what Windows
11's drivers disc carries (virtio-win's licence, the agent's crates),
`--vulkan` the macOS app's Vulkan loader and KosmicKrisp.
"""
import argparse
import collections
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The binaries a package ships that cargo builds, by workspace: the root
# workspace's two staticlibs linked into QEMU, the launcher and the player
# (each their own workspace, ADR-023/025), and the agent on the drivers
# disc, which is a Windows program whatever the host.
WORKSPACES = [
    # (title, directory, roots, fixed target or None for the host's)
    ("`libdisc` and `libsynth`, linked into QEMU", ".", ["libdisc", "libsynth"], None),
    ("`launcher-mitsuami`, the launcher", "launcher-mitsuami", ["launcher-mitsuami"], None),
    ("`player-mitsuami`, the player", "player-mitsuami", ["player-mitsuami"], None),
    ("`guest-agent`, on Windows 11's drivers disc", "guest-agent", ["guest-agent"], "x86_64-pc-windows-gnu"),
]
# What the packages are built for: Linux (and its Flatpak), the two Macs,
# and Windows (MSVC).
LIST_TARGETS = [
    "x86_64-unknown-linux-gnu",
    "aarch64-unknown-linux-gnu",
    "aarch64-apple-darwin",
    "x86_64-apple-darwin",
    "x86_64-pc-windows-msvc",
]

# Generic texts, for the expressions THIRD-PARTY-NOTICES.md names and for
# a crate whose package carries no licence file of its own. Staged at the
# top of licenses/.
GENERIC = {
    "GPL-2.0.txt": "COPYING",
    "LGPL-2.1.txt": "qemu/COPYING.LIB",
    "GPL-3.0.txt": "packaging/licenses/GPL-3.0.txt",
    "LGPL-3.0.txt": "packaging/licenses/LGPL-3.0.txt",
    "Apache-2.0.txt": "packaging/licenses/Apache-2.0.txt",
    "MIT.txt": "packaging/licenses/MIT.txt",
    "Zlib.txt": "packaging/licenses/Zlib.txt",
    "BSL-1.0.txt": "packaging/licenses/BSL-1.0.txt",
    "MPL-2.0.txt": "packaging/licenses/MPL-2.0.txt",
}
SPDX_TEXT = {
    "GPL-2.0": "GPL-2.0.txt", "GPL-2.0-only": "GPL-2.0.txt", "GPL-2.0-or-later": "GPL-2.0.txt",
    "LGPL-2.1": "LGPL-2.1.txt", "LGPL-2.1-only": "LGPL-2.1.txt", "LGPL-2.1-or-later": "LGPL-2.1.txt",
    "GPL-3.0": "GPL-3.0.txt", "GPL-3.0-only": "GPL-3.0.txt", "GPL-3.0-or-later": "GPL-3.0.txt",
    "LGPL-3.0": "LGPL-3.0.txt", "LGPL-3.0-only": "LGPL-3.0.txt",
    "Apache-2.0": "Apache-2.0.txt", "MIT": "MIT.txt", "Zlib": "Zlib.txt",
    "BSL-1.0": "BSL-1.0.txt", "MPL-2.0": "MPL-2.0.txt",
}

# Everything else, per component: the directory under licenses/, its files
# (relative to the repository; `{name}` is that library's unpacked source in
# build/deps/src, at the version scripts/build-deps.sh pins), and the hosts
# whose packages carry it. Keep in step with THIRD-PARTY-NOTICES.md.
ALL = ("linux", "macos", "windows")
COMPONENTS = [
    ("qemu", ["qemu/LICENSE", "qemu/COPYING", "qemu/COPYING.LIB"], ALL),
    ("edk2", ["qemu/pc-bios/edk2-licenses.txt"], ALL),
    ("dxvk", ["third_party/dxvk/LICENSE"], ALL),
    ("qemu-3dfx", ["third_party/qemu-3dfx/LICENSE"], ALL),
    ("uide", ["third_party/uide/uide.lsm", "third_party/uide/UPSTREAM"], ALL),
    # vendored, so cargo lists it as ours (a path dependency)
    ("inno", ["third_party/inno/LICENSE-MIT.md", "third_party/inno/LICENSE-APACHE.md"], ALL),
    # mingw-w64's runtime, static in what mingw builds: the guest
    # programs, the agent and the executor's Wine pair
    ("mingw-w64", ["packaging/licenses/mingw-w64-runtime.txt",
                   "packaging/licenses/mingw-w64-winpthreads.txt"], ALL),
    # QEMU's libraries, static in libqemu-embed (scripts/build-deps.sh). On
    # Linux only glib, pcre2, libslirp, libcrypto and libtpms are ours; the
    # rest are the distribution's.
    # COPYING is a symlink to this, which Windows' unpack makes a .lnk
    ("glib", ["{glib}/LICENSES/LGPL-2.1-or-later.txt"], ALL),
    ("pcre2", ["{pcre2}/LICENCE.md"], ALL),
    ("libslirp", ["{libslirp}/COPYRIGHT", "{libslirp}/LICENSE"], ALL),
    ("openssl", ["{openssl}/LICENSE.txt"], ALL),
    ("libtpms", ["{libtpms}/LICENSE"], ALL),
    ("spice-protocol", ["{spice-protocol}/COPYING"], ALL),
    ("pixman", ["{pixman}/COPYING"], ("macos", "windows")),
    ("zstd", ["{zstd}/LICENSE"], ("macos",)),
    ("zlib", ["{zlib}/LICENSE"], ("windows",)),
    ("libepoxy", ["{libepoxy}/COPYING"], ("windows",)),
    # glib's own subprojects, where the system has neither (meson unpacks
    # them inside glib's source)
    ("libffi", ["{glib}/subprojects/libffi-*/LICENSE"], ("macos", "windows")),
    ("proxy-libintl", ["{glib}/subprojects/proxy-libintl-*/COPYING"], ("macos", "windows")),
]
DRIVERS = [("virtio-win", ["packaging/licenses/virtio-win.txt"])]
VULKAN = [("mesa", ["packaging/licenses/Mesa-KosmicKrisp.txt"])]  # the loader is Apache-2.0.txt

LICENCE_FILE = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)", re.I)


def die(msg):
    sys.exit("third-party-notices: " + msg)


def metadata(directory, targets):
    cmd = ["cargo", "metadata", "--format-version", "1", "--locked", "--all-features"]
    for t in targets:
        cmd += ["--filter-platform", t]
    try:
        out = subprocess.run(cmd, cwd=os.path.join(ROOT, directory), check=True,
                             stdout=subprocess.PIPE).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        die("cargo metadata in %s failed: %s" % (directory, e))
    return json.loads(out)


def shipped(m, roots):
    """The third-party packages the roots link: normal dependencies only,
    since a build script's or a test's do not end up in the binary."""
    pkgs = {p["id"]: p for p in m["packages"]}
    nodes = {n["id"]: n for n in m["resolve"]["nodes"]}
    stack = [i for i, p in pkgs.items() if p["name"] in roots and p.get("source") is None]
    if len(stack) != len(roots):
        die("%s: not all of %s are workspace packages" % (m["workspace_root"], roots))
    seen = set()
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        for d in nodes[i]["deps"]:
            if any(k.get("kind") is None for k in d["dep_kinds"]):
                stack.append(d["pkg"])
    return [pkgs[i] for i in seen if pkgs[i].get("source") is not None]


def host_target(host, arch):
    arch = {"arm64": "aarch64", "AMD64": "x86_64"}.get(arch, arch)
    return {"linux": "%s-unknown-linux-gnu" % arch,
            "macos": "%s-apple-darwin" % arch,
            "windows": "x86_64-pc-windows-msvc"}[host]


def cmd_list(args):
    out = ["## Crates, by declared licence", ""]
    out.append("Generated by `python3 tools/third-party-notices.py list --write "
               "THIRD-PARTY-NOTICES.md` for %s (`guest-agent`: Windows). Each crate's "
               "own licence files are in the packages' `licenses/crates/`." %
               ", ".join("`%s`" % t for t in LIST_TARGETS))
    for title, directory, roots, target in WORKSPACES:
        m = metadata(directory, [target] if target else LIST_TARGETS)
        lic = collections.defaultdict(set)
        for p in shipped(m, roots):
            lic[p.get("license") or "(see the crate's own licence file)"].add(p["name"])
        total = len({n for v in lic.values() for n in v})
        out.append("")
        out.append("### %s: %d third-party crates" % (title, total))
        for k in sorted(lic, key=lambda k: (-len(lic[k]), k)):
            names = ", ".join("`%s`" % n for n in sorted(lic[k]))
            out.append("")
            out.append("**%s** (%d): %s" % (k, len(lic[k]), names))
    text = "\n".join(out) + "\n"
    if not args.write:
        sys.stdout.write(text)
        return
    old = open(args.write, encoding="utf-8").read()
    at = old.find("\n## Crates, by declared licence")
    if at < 0:
        die("%s has no '## Crates, by declared licence' heading" % args.write)
    with open(args.write, "w", encoding="utf-8") as f:
        f.write(old[: at + 1] + text)


def dep_versions():
    """name -> unpacked directory, from scripts/build-deps.sh's PKGS table."""
    dirs = {}
    src = open(os.path.join(ROOT, "scripts/build-deps.sh"), encoding="utf-8").read()
    for name, ver in re.findall(r"^([a-z0-9-]+) (\d[\w.]*) \S+ [0-9a-f]{64} https://", src, re.M):
        for d in ("%s-%s" % (name, ver), "%s-v%s" % (name, ver)):
            if os.path.isdir(os.path.join(ROOT, "build/deps/src", d)):
                dirs[name] = "build/deps/src/" + d
    return dirs


def resolve(pattern, deps):
    def sub(m):
        if m.group(1) not in deps:
            die("no unpacked %s in build/deps/src (scripts/build-deps.sh), for %s"
                % (m.group(1), pattern))
        return deps[m.group(1)]
    path = re.sub(r"\{([a-z0-9-]+)\}", sub, pattern)
    if "*" in path:
        hits = sorted(glob.glob(os.path.join(ROOT, path)))
        if not hits:
            die("no %s (glib's meson subproject; scripts/build-deps.sh)" % path)
        return hits[-1]
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        die("missing %s" % path)
    return full


def stage_crates(dest, m, roots, made):
    """Each crate's own licence files into crates/<name>-<version>/; a crate
    whose package has none gets a note naming its declared licence and the
    generic text that stands for it."""
    for p in shipped(m, roots):
        key = "%s-%s" % (p["name"], p["version"])
        if key in made:
            continue
        made.add(key)
        src = os.path.dirname(p["manifest_path"])
        out = os.path.join(dest, "crates", key)
        os.makedirs(out, exist_ok=True)
        files = [f for f in sorted(os.listdir(src))
                 if LICENCE_FILE.match(f) and os.path.isfile(os.path.join(src, f))]
        if p.get("license_file") and os.path.isfile(os.path.join(src, p["license_file"])):
            files.append(p["license_file"])
        for f in files:
            shutil.copyfile(os.path.join(src, f), os.path.join(out, os.path.basename(f)))
        if files:
            continue
        expr = p.get("license") or ""
        ids = [t for t in re.split(r"[\s()/]+|\bOR\b|\bAND\b", expr) if t]
        if " AND " in expr:
            texts = [SPDX_TEXT.get(i) for i in ids]
            texts = texts if all(texts) else []
        else:
            texts = [SPDX_TEXT[i] for i in ids if i in SPDX_TEXT][:1]
        if not texts:
            die("%s ships no licence file and declares %r, which has no text in "
                "packaging/licenses/" % (key, expr))
        authors = "; ".join(p.get("authors") or []) or "not named"
        with open(os.path.join(out, "LICENSE-NOTE.txt"), "w", encoding="utf-8") as f:
            f.write("%s %s declares the licence %s and its package carries no\n"
                    "licence file. Its authors: %s.\nThe text: %s\n"
                    % (p["name"], p["version"], expr, authors,
                       ", ".join("../../" + t for t in texts)))


def cmd_stage(args):
    dest = os.path.abspath(args.dest)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    for name, src in GENERIC.items():
        shutil.copyfile(resolve(src, {}), os.path.join(dest, name))
    deps = dep_versions()
    comps = [(d, f) for d, f, hosts in COMPONENTS if args.host in hosts]
    comps += DRIVERS if args.drivers else []
    comps += VULKAN if args.vulkan else []
    for directory, files in comps:
        os.makedirs(os.path.join(dest, directory), exist_ok=True)
        for f in files:
            src = resolve(f, deps)
            shutil.copyfile(src, os.path.join(dest, directory, os.path.basename(src)))
    target = host_target(args.host, args.arch or platform.machine())
    made = set()
    for _, directory, roots, fixed in WORKSPACES:
        if directory == "guest-agent" and not args.drivers:
            continue
        stage_crates(dest, metadata(directory, [fixed or target]), roots, made)
    print("licenses       %d components, %d crates (%s)" % (len(comps), len(made), target))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("list")
    lp.add_argument("--write", metavar="FILE")
    sp = sub.add_parser("stage")
    sp.add_argument("dest")
    sp.add_argument("--host", required=True, choices=ALL)
    sp.add_argument("--arch")
    sp.add_argument("--drivers", action="store_true")
    sp.add_argument("--vulkan", action="store_true")
    args = ap.parse_args()
    cmd_list(args) if args.cmd == "list" else cmd_stage(args)


if __name__ == "__main__":
    main()
