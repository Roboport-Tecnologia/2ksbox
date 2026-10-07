#!/usr/bin/env bash
# Build everything this host can build, in dependency order, with the
# per-platform flags:
#
#   scripts/build.sh                 everything this host can build
#   scripts/build.sh qemu rust       only those stages
#   scripts/build.sh --test          everything, then scripts/test.sh host
#   scripts/build.sh --x86_64        on an Apple Silicon Mac: the Intel build,
#                                    under Rosetta (docs/build-macos.md,
#                                    "The Intel build")
#
# Stages, in the order they must run:
#
#   deps    scripts/build-deps.sh into build/deps/<arch>. macOS: the
#           libraries QEMU links (glib, pixman, libslirp, zstd; static),
#           built from source for the floor (docs/build-macos.md, "The
#           libraries"). Linux: QEMU's own
#           glib (pcre2, glib, libslirp; static), so QEMU never shares a
#           glib with the process it is embedded in (QEMU_DEPS in
#           docs/development.md), and libtpms with its libcrypto; the
#           rest is the distribution's.
#   qemu    prepare-qemu.sh (overlay + patch queue) -> configure-qemu.sh
#           -> ninja: qemu-system-i386, qemu-system-x86_64 (not on a Mac), qemu-img,
#           qemu-io, libqemu-embed-{i386,x86_64}.{so,dylib} (x86_64 is
#           Windows 11's, linked by a player of its own; see `rust`), and
#           on an Arm host qemu-system-aarch64 and libqemu-embed-aarch64
#           (Windows 11 on Arm)
#   edk2    Arm hosts: build-edk2.sh, the firmware of Windows 11 on Arm
#           (EDK2's ArmVirtQemu with Secure Boot and AHCI) into
#           qemu/pc-bios. Needs clang and lld (Homebrew's on a Mac)
#   virtio  build-virtio-win.sh, Windows 11's drivers disc for the host's
#           processor (on Arm virtio-win's ARM64 network, display and
#           serial drivers; on x86 its x64 serial driver, the clipboard's;
#           both with the 2ksbox agent) from a pinned download into
#           build/virtio-win. Not on an Intel Mac. Needs xorriso and
#           mingw-w64
#   rust    cargo build --release: player, libdisc/discx, launcher-core
#           (with its `launcherx` verb binary), qemu-embed, shader-chain.
#           Runs after `qemu`, because the player links libqemu-embed from
#           build/qemu. Then `cargo check --release --workspace` keeps the
#           one non-default member, `launcher-capi`, compiling. On Linux
#           also Windows 11's player, into target/qemu-x86_64; on an Arm
#           host Windows 11 on Arm's, into target/qemu-aarch64.
#   mitsuami cargo build --release in launcher-mitsuami/ and
#           player-mitsuami/ (each its own workspace): the launcher and the
#           player every package ships (ADR-023, ADR-025), on AppKit on a
#           Mac and GTK 4 on Linux, with Windows 11's player as the `rust`
#           stage builds the winit one (player-mitsuami/target/qemu-x86_64
#           on Linux, qemu-aarch64 on an Arm host). Linux needs GTK 4 (4.10+)
#           development files; without them the stage is skipped and this
#           host can build no package.
#   dxvk    prepare-dxvk.sh -> configure-dxvk.sh -> ninja
#   exec    build-d3dpt-exec.sh: libd3dpt_exec, the D3D executor. Runs
#           after `dxvk`, whose headers it compiles against.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO (it also
#           calls build-driver.sh for the XP display driver), with the
#           Windows 7 WDDM driver the PC published for these sources
#           (scripts/wddm-prebuilt.sh fetch; needs the network once per
#           driver change, WDDM_PREBUILT=0 skips it)
#
# A stage whose tools are missing is skipped with the reason, or fails if
# it was named on the command line. The summary at the end lists what
# this host built and what it could not.
#
# A partial rebuild is the project's most expensive mistake. A stale
# libd3dpt_exec or guest-tools ISO after a D3DPT_PROTO_VERSION bump reads
# as "the guest will not boot", not as "you forgot a command".
#
# With everything up to date a run takes a couple of seconds thanks to the
# stamps below, and needs no network once the WDDM driver for the
# checkout's sources is fetched (until the PC publishes it, each run asks
# once and goes on without it).
#
# `launcher-mitsuami/` stays its own cargo workspace (ADR-023), so a plain
# `cargo build` at the root never needs GTK. The `rust` stage, the test
# suite and a CI checkout then work on a host with no GTK.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

case "$(uname -s)" in Darwin) SO=dylib;; *) SO=so;; esac

JOBS=()
RUN_TEST=""
FORCE=""
STAGES=()
X86_64=""
ARGS=("$@")

usage() {
  sed -n '2,56p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  -j N            parallel jobs for ninja and cargo (default: auto)
  -f, --force     re-run every prepare and configure step, ignoring the
                  stamps (use it after editing a tree by hand)
  -t, --test      run scripts/test.sh host when the build succeeds
  --x86_64        the Intel build, on an Apple Silicon Mac (see above)
  -h, --help      this text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -f|--force) FORCE=1; shift ;;
    -t|--test) RUN_TEST=1; shift ;;
    --x86_64) X86_64=1; shift ;;
    -h|--help) usage; exit 0 ;;
    deps|qemu|edk2|virtio|rust|mitsuami|dxvk|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done

# --- the Intel build on an Apple Silicon Mac ----------------------------
# ADR-019's community build for Intel Macs, made on this one under Rosetta
# (docs/build-macos.md, "The Intel build"): the whole script re-runs itself
# as an x86_64 process. From there on nothing is told the architecture:
# uname and the compiler answer x86_64, every stage script recognises the
# translated process (sysctl.proc_translated) and keeps to its own
# directories, build/x86_64, build/deps/x86_64 and
# target/x86_64-apple-darwin, beside the native build's, and the
# libraries are built for x86_64 by the same deps stage. So `arch -x86_64
# scripts/build.sh` is the same build; the flag only spares the typing.
# An Intel Mac never runs translated and builds natively.
ROSETTA=""
if [ "$(uname -s)" = Darwin ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
  ROSETTA=1
fi
if [ -n "$X86_64" ] && [ -z "$ROSETTA" ]; then
  [ "$(uname -s)" = Darwin ] || { echo "build.sh: --x86_64 is the Intel build of the macOS app; this is not a Mac" >&2; exit 1; }
  exec arch -x86_64 "$0" "${ARGS[@]}"
fi
# Where this build's outputs go, and the cargo target that puts them
# there. The native build's are the defaults every doc names.
QB=build/qemu; TD=target/release; LTD=launcher-mitsuami/target/release
PTD=player-mitsuami/target/release; CT=()
if [ -n "$ROSETTA" ]; then
  QB=build/x86_64/qemu; TD=target/x86_64-apple-darwin/release
  LTD=launcher-mitsuami/target/x86_64-apple-darwin/release
  PTD=player-mitsuami/target/x86_64-apple-darwin/release; CT=(--target x86_64-apple-darwin)
  echo "==> the Intel build, under Rosetta: $QB, $TD"
fi

EXPLICIT=""
if [ ${#STAGES[@]} -eq 0 ]; then
  STAGES=(deps qemu edk2 virtio rust mitsuami dxvk exec guest)
else
  EXPLICIT=1
fi

BUILT=(); SKIPPED=(); T0=$SECONDS

want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# A stage this host cannot run: an error when it was asked for by name, a
# recorded skip when we were just building everything.
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"
  SKIPPED+=("$1 ($2)")
  return 1
}

# --- stamps -----------------------------------------------------------
# Every "prepare" step here restores its tree and re-applies a patch
# queue, which hands the build system a few thousand fresh mtimes and a
# from-scratch rebuild every run (four minutes for a tree with nothing to
# do). So each is guarded by a hash of what feeds it: the patch queue, the
# overlaid sources, the submodule commits and the prepare script. Skipping
# a prepare leaves the tree as the last one left it, which the "never git
# checkout inside qemu/ by hand" rule already protects. -f is the escape
# hatch for when that rule was broken.
if have sha256sum; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

stamp_value() { # $STAMP_GITS holds git repos to pin; arguments are paths
  {
    local g
    for g in ${STAMP_GITS:-}; do git -C "$g" rev-parse HEAD 2>/dev/null || echo none; done
    find "$@" -type f 2>/dev/null | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat 2>/dev/null
  } | "${SHA[@]}" | cut -d' ' -f1
}

# usage: if stamp_stale <name> <paths...>; then <prepare>; stamp_save; fi
stamp_stale() {
  STAMP_FILE="build/.stamp-$1"; shift
  mkdir -p build
  STAMP_VALUE="$(stamp_value "$@")"
  [ -n "$FORCE" ] && return 0
  [ "$(cat "$STAMP_FILE" 2>/dev/null || true)" != "$STAMP_VALUE" ]
}
stamp_save() { printf '%s\n' "$STAMP_VALUE" > "$STAMP_FILE"; }

# macOS: everything is built for the floor (scripts/macos-floor.sh), the
# oldest macOS the app runs on (docs/build-macos.md, "The floor").
# Every stage (configure-qemu.sh, cargo, DXVK, the executor, the wrapper)
# gets the same value, or ld warns "built for newer macOS version" on every
# link. A preset MACOSX_DEPLOYMENT_TARGET wins.
if [ "$(uname -s)" = Darwin ]; then
  MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
  case "$MACOSX_DEPLOYMENT_TARGET" in *.*) ;; *) MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET.0" ;; esac
  export MACOSX_DEPLOYMENT_TARGET
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET (every stage alike)"
  # The target is not part of cargo's fingerprint, so a tree built for a
  # newer macOS keeps objects that may call what the floor lacks. When a
  # binary was linked for a *newer* macOS than the target, its workspace
  # starts over. Older is fine (without this variable cargo links for
  # rustc's default, 11.0). A workspace this run will not rebuild is left
  # alone, since cleaning it would leave no binary at all (`build.sh guest`
  # once took the player with it).
  for spec in "rust:$TD/player" "mitsuami:$LTD/launcher-mitsuami" "mitsuami:$PTD/player-mitsuami"; do
    bin=${spec#*:}
    want "${spec%%:*}" && [ -f "$bin" ] || continue
    built=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
    if [ -n "$built" ] && [ "$(printf '%s\n' "$built" "$MACOSX_DEPLOYMENT_TARGET" | sort -V | tail -1)" != "$MACOSX_DEPLOYMENT_TARGET" ]; then
      ws=${bin%%target/*}; ws=${ws:-.}
      echo "==> $bin was built for macOS $built: cargo clean --release in $ws"
      (cd "$ws" && cargo clean --release ${CT[@]+"${CT[@]}"})
    fi
  done
fi

# --- submodules -------------------------------------------------------
# Cloning without --recurse-submodules is the most common first failure.
if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/qemu-1/hw/mesa/meson.build ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# --- deps -------------------------------------------------------------
# The libraries QEMU links, ours (scripts/build-deps.sh). Stamped on the
# script (its versions and checksums are in it), the floor and the
# architecture; a change rebuilds them and then reconfigures QEMU, since
# meson recorded the old archives' paths and flags.
DEPS_FRESH=""
if want deps; then
  if [ "$(uname -s)" = Linux ] && [ "${QEMU_DEPS:-}" = system ]; then
    echo "    deps: QEMU_DEPS=system, QEMU links the distribution's glib"
  elif [ "$(uname -s)" = Linux ]; then
    if ! have meson || ! have ninja || ! have pkg-config; then
      skip deps "needs meson, ninja and pkg-config" || true
    elif stamp_stale "deps-$(uname -m)" scripts/build-deps.sh \
       || ! ls "build/deps/$(uname -m)"/.built-glib-*-linux >/dev/null 2>&1 \
       || ! ls "build/deps/$(uname -m)"/.built-libslirp-*-linux >/dev/null 2>&1; then
      say "deps: pcre2, glib and libslirp for QEMU ($(uname -m), static)"
      scripts/build-deps.sh
      stamp_save
      DEPS_FRESH=1
      BUILT+=(deps)
    else
      echo "    script and architecture unchanged - skipping"
    fi
  elif [ "$(uname -s)" != Darwin ]; then
    echo "    deps: the distribution's libraries"
  elif ! have meson || ! have ninja || ! have pkg-config; then
    skip deps "needs meson, ninja and pkg-config" || true
  else
    say "deps: glib, pixman, libslirp and zstd ($(uname -m), macOS $MACOSX_DEPLOYMENT_TARGET)"
    # The script's own stamps (name, version, patch set, floor) skip what
    # is built, so a patch edit rebuilds its package alone; a recipe
    # change for the same version wants `build-deps.sh --clean` by hand.
    if stamp_stale "deps-$(uname -m)" scripts/build-deps.sh patches/deps \
       || [ ! -f "build/deps/$(uname -m)/lib/pkgconfig/glib-2.0.pc" ] \
       || ! ls "build/deps/$(uname -m)"/.built-glib-*-"$MACOSX_DEPLOYMENT_TARGET" >/dev/null 2>&1; then
      scripts/build-deps.sh
      stamp_save
      DEPS_FRESH=1
      BUILT+=(deps)
    else
      echo "    script, floor and architecture unchanged - skipping"
    fi
  fi
fi

# --- qemu -------------------------------------------------------------
if want qemu; then
  if ! have ninja; then skip qemu "no ninja" || true
  else
    say "qemu"
    if STAMP_GITS="qemu third_party/qemu-3dfx" \
       stamp_stale qemu-prepare patches/qemu embed d3dpt/hw d3dpt/d3dpt_proto.h \
         d3dpt/d3dpt_fb.h d3dpt/exec/d3dpt_exec.h libdisc/qemu libdisc/libdisc.h \
         libsynth/qemu libsynth/libsynth.h gamepad/qemu tpm/qemu voodoo firmware \
         scripts/prepare-qemu.sh patches/qemu-3dfx; then
      scripts/prepare-qemu.sh
      stamp_save
    else
      echo "    patch queue, overlays and submodules unchanged - skipping prepare"
    fi

    # QEMU 9.2 carries no slirp of its own, so `-netdev user`, which every
    # machine this project makes uses, exists only if libslirp was there at
    # configure time. Meson's `slirp` option is `auto`, so its absence is
    # silent until a guest has no network and QEMU refuses the launcher's
    # command line.
    slirp_pkg=""; have pkg-config && pkg-config --exists slirp && slirp_pkg=1
    # On macOS and Linux slirp is one of ours, in the prefix
    # configure-qemu.sh points pkg-config at.
    [ -f "build/deps/$(uname -m)/lib/pkgconfig/slirp.pc" ] && slirp_pkg=1
    slirp_built=""
    grep -q '^#define CONFIG_SLIRP' "$QB/config-host.h" 2>/dev/null && slirp_built=1

    # configure resets meson options and is slow, so only when needed.
    # prepare-qemu.sh deliberately preserves meson.build mtimes when the
    # content is unchanged, which is what makes this comparison meaningful.
    needs_configure=""
    [ -n "$FORCE" ] && needs_configure=1
    [ -n "$DEPS_FRESH" ] && needs_configure=1
    [ -f "$QB/build.ninja" ] || needs_configure=1
    # libslirp installed since the last configure: configure again, or
    # installing it would look like it had done nothing.
    [ -n "$slirp_pkg" ] && [ -z "$slirp_built" ] && [ -f "$QB/build.ninja" ] && needs_configure=1
    # And the configure script itself: a flag it gained since the last
    # configure (a library disabled, say) is otherwise not applied until
    # something else forces a configure, and the `no-optionals` check
    # fails on a build that was made correctly at the time.
    for f in qemu/meson.build qemu/hw/3dfx/meson.build qemu/hw/mesa/meson.build scripts/configure-qemu.sh; do
      if [ -f "$f" ] && [ -f "$QB/build.ninja" ] && [ "$f" -nt "$QB/build.ninja" ]; then
        needs_configure=1
      fi
    done
    # Configured on the other glib (Linux): ours hides its symbols with
    # --exclude-libs, so build.ninja says which one the last configure
    # used. A build configured by hand the other way is otherwise kept
    # until something else forces a configure.
    if [ "$(uname -s)" = Linux ] && [ -f "$QB/build.ninja" ]; then
      own=""; grep -q -- '--exclude-libs,libglib-2.0.a' "$QB/build.ninja" && own=1
      if [ "${QEMU_DEPS:-}" = system ]; then [ -n "$own" ] && needs_configure=1
      else [ -z "$own" ] && needs_configure=1; fi
    fi
    # Configured for another macOS: configure-qemu.sh passes the target as
    # a compiler flag, so configuring again is what recompiles for it.
    if [ "$(uname -s)" = Darwin ] && [ -f "$QB/config-meson.cross" ] \
       && ! grep -q -- "'-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET'" "$QB/config-meson.cross"; then
      needs_configure=1
    fi
    # Configured for another CPU: meson takes the machine its Python runs
    # on for the host, and a configure that once ran on the Intel build's
    # x86_64 interpreter leaves a build directory that compiles every file
    # with -msse2 and fails on this arm64 Mac; a plain reconfigure keeps
    # that answer. Only a fresh directory forgets it.
    if [ "$(uname -s)" = Darwin ] && [ -f "$QB/meson-logs/meson-log.txt" ]; then
      case "$(uname -m)" in arm64) want=aarch64 ;; *) want=$(uname -m) ;; esac
      got=$(grep -m1 '^Host machine cpu family:' "$QB/meson-logs/meson-log.txt" | awk '{print $NF}')
      if [ -n "$got" ] && [ "$got" != "$want" ]; then
        echo "    $QB was configured for $got, this is $want: configuring from scratch"
        rm -rf "$QB"; needs_configure=1
      fi
    fi
    if [ -n "$needs_configure" ]; then
      if [ -z "${QEMU_PYTHON:-}" ] && [ -z "$ROSETTA" ] && ! have uv; then
        skip qemu "configure needs uv (or QEMU_PYTHON=<python 3.8-3.13>)" || true
      else
        say "qemu: configure"
        scripts/configure-qemu.sh
      fi
    else
      echo "    $QB is configured and no meson file moved - skipping configure"
    fi

    if [ -z "$slirp_pkg" ]; then
      case "$(uname -s)" in
        Darwin) echo "    no libslirp: guests will have no networking (brew install libslirp, then re-run)";;
        *)      echo "    no libslirp: guests will have no networking (install libslirp-dev / libslirp-devel, then re-run)";;
      esac
    fi

    if [ -f "$QB/build.ninja" ]; then
      # block/cdimage.c links libdisc's Rust staticlib (patch 50), and meson
      # takes it from target/release, which the rust stage below writes too
      # late for this link. So build that crate here. Otherwise a pull that
      # only touched libdisc/src leaves qemu-system-i386 on the previous
      # staticlib until the *next* build.sh, and it fails at run time, not
      # at the link ("isodir: I/O error: Is a directory" is what an old
      # libdisc says about a folder disc). libsynth (patch 60) likewise, and
      # a `cargo clean` above for a new macOS target removed both.
      if have cargo; then
        say "qemu: cargo build --release -p libdisc -p libsynth (linked into qemu)"
        cargo build --release -p libdisc -p libsynth ${CT[@]+"${CT[@]}"} ${JOBS[@]+"${JOBS[@]}"}
      fi
      say "qemu: ninja"
      # Windows 11's QEMUs, where configure-qemu.sh made them: x86_64
      # everywhere but a Mac, aarch64 on an Arm host.
      W11=()
      for t in x86_64 aarch64; do
        if grep -q "^build qemu-system-$t" "$QB/build.ninja"; then
          W11+=("qemu-system-$t" "libqemu-embed-$t.$SO")
        fi
      done
      ninja -C "$QB" ${JOBS[@]+"${JOBS[@]}"} \
        qemu-system-i386 qemu-img qemu-io "libqemu-embed-i386.$SO" ${W11[@]+"${W11[@]}"}
      BUILT+=(qemu)
    fi
  fi
fi

# --- edk2 -------------------------------------------------------------
# Windows 11 on Arm's firmware, where QEMU has the aarch64 target. Its own
# stamp (build/edk2/.stamp) makes an unchanged run a no-op.
if want edk2; then
  if [ -n "$ROSETTA" ] || ! case "$(uname -m)" in arm64|aarch64) true ;; *) false ;; esac; then
    skip edk2 "an x86 host runs no Windows 11 on Arm" || true
  elif [ "$(uname -s)" = Darwin ] && ! [ -x /opt/homebrew/opt/lld/bin/ld.lld ]; then
    skip edk2 "no lld (brew install llvm lld; build tools only)" || true
  elif [ "$(uname -s)" != Darwin ] && ! { have clang && have ld.lld; }; then
    skip edk2 "no clang / lld" || true
  else
    say "edk2: build-edk2.sh"
    scripts/build-edk2.sh
    BUILT+=(edk2)
  fi
fi

# --- virtio -----------------------------------------------------------
# Windows 11's drivers disc for the host's processor: on an Arm host
# Windows 11 on Arm's, beside its firmware; elsewhere x64 Windows 11's
# (the clipboard's driver). Its own stamps (build/virtio-win/.stamp-*);
# the first run downloads virtio-win's ISO.
if want virtio; then
  if [ "$(uname -s)" = Darwin ] && ! case "$(uname -m)" in arm64|aarch64) true ;; *) false ;; esac; then
    skip virtio "a Mac runs no x64 Windows 11" || true
  elif ! have x86_64-w64-mingw32-gcc; then
    skip virtio "no mingw-w64 (x86_64-w64-mingw32-gcc) for the guest agent" || true
  elif ! have xorriso; then
    skip virtio "no xorriso" || true
  else
    say "virtio: build-virtio-win.sh"
    scripts/build-virtio-win.sh
    BUILT+=(virtio)
  fi
fi

# --- rust -------------------------------------------------------------
# After qemu: the player links libqemu-embed with an rpath into
# build/qemu, and qemu-embed/build.rs warns when qemu/embed/ is a stale
# copy of embed/ (which is why prepare-qemu.sh ran first).
if want rust; then
  if ! have cargo; then skip rust "no cargo" || true
  else
    say "rust: cargo build --release (default members)"
    cargo build --release ${CT[@]+"${CT[@]}"} ${JOBS[@]+"${JOBS[@]}"}
    # The Windows 11 player (track M20): the same player linked to
    # libqemu-embed-x86_64. A build of its own, in its own target
    # directory, because a feature is shared by everything one cargo run
    # builds. Linked rather than opened at run time for patch 63 (doc 22
    # §5.0). Linux only for now: the family runs nowhere else yet.
    if [ "$(uname -s)" = Linux ]; then
      say "rust: the x86_64 player (Windows 11)"
      cargo build --release -p player --features qemu-x86_64 --target-dir target/qemu-x86_64 ${JOBS[@]+"${JOBS[@]}"}
    fi
    # Windows 11 on Arm's player, where QEMU has the aarch64 target (an
    # Arm host, configure-qemu.sh). On a Mac it needs Hypervisor.framework's
    # entitlement even ad-hoc signed, and cargo's relink drops it, so it
    # is signed again on every build.
    if [ -f "build/qemu/libqemu-embed-aarch64.$SO" ] && [ -z "$ROSETTA" ]; then
      say "rust: the aarch64 player (Windows 11 on Arm)"
      cargo build --release -p player --features qemu-aarch64 --target-dir target/qemu-aarch64 ${JOBS[@]+"${JOBS[@]}"}
      if [ "$(uname -s)" = Darwin ]; then
        codesign --force --sign - --entitlements packaging/macos/hypervisor.entitlements target/qemu-aarch64/release/player
      fi
    fi
    # The member that is not a default member (Cargo.toml):
    # `launcher-capi`, a cdylib + staticlib of the whole launcher that
    # nothing installs. Checked rather than built, so it cannot rot. Once
    # per checkout: the Intel build of it would prove nothing more.
    if [ -z "$ROSETTA" ]; then
      say "rust: cargo check --release --workspace (launcher-capi)"
      cargo check --release --workspace ${JOBS[@]+"${JOBS[@]}"}
    fi
    BUILT+=(rust)
  fi
fi

# --- mitsuami ---------------------------------------------------------
# The launcher and the player every package ships (ADR-023, ADR-025).
# Each its own cargo workspace, so a stage of its own rather than members
# of the one above, which keeps GTK off the default build path. On a Mac
# they are AppKit and need nothing; on Linux GTK 4, found through
# pkg-config by gtk4-rs. Runs after `qemu`: the player links
# libqemu-embed.
if want mitsuami; then
  if ! have cargo; then skip mitsuami "no cargo" || true
  elif [ "$(uname -s)" != Darwin ] && ! pkg-config --atleast-version=4.10 gtk4 2>/dev/null; then
    skip mitsuami "no GTK 4.10+ development files (gtk4)" || true
  else
    say "mitsuami: cargo build --release (launcher-mitsuami)"
    ( cd launcher-mitsuami && cargo build --release ${CT[@]+"${CT[@]}"} ${JOBS[@]+"${JOBS[@]}"} )
    say "mitsuami: cargo build --release (player-mitsuami)"
    ( cd player-mitsuami && cargo build --release ${CT[@]+"${CT[@]}"} ${JOBS[@]+"${JOBS[@]}"} )
    # Windows 11's players, as the `rust` stage builds the winit ones: a
    # second libqemu-embed is a second binary, in a target dir of its own.
    if [ "$(uname -s)" = Linux ]; then
      say "mitsuami: the x86_64 player (Windows 11)"
      ( cd player-mitsuami && cargo build --release --features qemu-x86_64 --target-dir target/qemu-x86_64 ${JOBS[@]+"${JOBS[@]}"} )
    fi
    if [ -f "build/qemu/libqemu-embed-aarch64.$SO" ] && [ -z "$ROSETTA" ]; then
      say "mitsuami: the aarch64 player (Windows 11 on Arm)"
      ( cd player-mitsuami && cargo build --release --features qemu-aarch64 --target-dir target/qemu-aarch64 ${JOBS[@]+"${JOBS[@]}"} )
      # Hypervisor.framework refuses an unentitled process even ad hoc
      # signed, and cargo's relink drops the entitlement.
      if [ "$(uname -s)" = Darwin ]; then
        codesign --force --sign - --entitlements packaging/macos/hypervisor.entitlements player-mitsuami/target/qemu-aarch64/release/player-mitsuami
      fi
    fi
    BUILT+=(mitsuami)
  fi
fi

# --- dxvk -------------------------------------------------------------
if want dxvk; then
  # No Vulkan driver exists for an Intel Mac (ADR-019: KosmicKrisp is Apple
  # Silicon only, MoltenVK refused), so the Intel build has no DXVK; its
  # Direct3D is the executor on Wine, which the exec stage builds.
  if [ -n "$ROSETTA" ]; then skip dxvk "no Vulkan on an Intel Mac (ADR-019); Direct3D is the executor on Wine" || true
  elif ! have meson || ! have ninja; then skip dxvk "needs meson and ninja" || true
  elif ! have glslangValidator && ! have glslang; then
    skip dxvk "needs glslang (vulkan-headers, vulkan-loader, glslang)" || true
  else
    say "dxvk"
    if STAMP_GITS="third_party/dxvk" \
       stamp_stale dxvk-prepare patches/dxvk scripts/prepare-dxvk.sh; then
      scripts/prepare-dxvk.sh
      stamp_save
    else
      echo "    patch queue and submodule unchanged - skipping prepare"
    fi
    # Configured for another macOS: configure-dxvk.sh passes the target as
    # a compiler flag, which build.ninja therefore names.
    dxvk_retarget=""
    if [ "$(uname -s)" = Darwin ] && [ -f build/dxvk/build.ninja ] \
       && ! grep -q -- "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" build/dxvk/build.ninja; then
      dxvk_retarget=1
    fi
    if [ -n "$FORCE" ] || [ ! -f build/dxvk/build.ninja ] || [ -n "$dxvk_retarget" ]; then
      say "dxvk: configure"
      scripts/configure-dxvk.sh
    fi
    say "dxvk: ninja"
    ninja -C build/dxvk ${JOBS[@]+"${JOBS[@]}"}
    BUILT+=(dxvk)
  fi
fi

# --- exec -------------------------------------------------------------
# Compiles against third_party/dxvk's native headers; the DXVK library
# itself is dlopened at runtime, so this needs the tree prepared, not built.
if want exec; then
  if [ -n "$ROSETTA" ]; then
    # Only the out-of-process library and the PE pair (build-d3dpt-exec.sh
    # under Rosetta), which need no DXVK headers.
    say "exec: libd3dpt_exec_remote (the executor on Wine; the Intel build)"
    scripts/build-d3dpt-exec.sh
    BUILT+=(exec)
  elif [ ! -f third_party/dxvk/include/native/windows/windows_base.h ]; then
    skip exec "third_party/dxvk not prepared (run the dxvk stage first)" || true
  else
    say "exec: libd3dpt_exec (the D3D decoder + DXVK executor)"
    scripts/build-d3dpt-exec.sh
    BUILT+=(exec)
  fi
fi

# --- guest ------------------------------------------------------------
# The guest-tools ISO carries the D3DPT guest DLLs, so a protocol bump
# makes it stale exactly as it does the executor. build-wrappers.sh also
# calls build-driver.sh, so the XP display driver rides along.
GUEST_STALE=""
if want guest; then
  # The Windows 7 WDDM driver builds only on Windows; the PC publishes it
  # by a hash of its sources, and the one for this checkout is fetched
  # into build/wddm/x86 before the stamp reads it. Nothing published
  # means an ISO without WDDM\, as before (WDDM_PREBUILT=0 never asks).
  scripts/wddm-prebuilt.sh fetch || true
  # the stamp is computed before the tool check, so a host that cannot
  # build the ISO can still say whether the one it has is out of date
  if STAMP_GITS="third_party/qemu-3dfx" \
     stamp_stale guest-tools guest-tools/src d3dpt/d3dpt_proto.h \
       d3dpt/d3dpt_fb.h cdshelf/cdshelf_proto.h \
       guest-tools/build-wrappers.sh guest-tools/build-driver.sh \
       guest-tools/build-driver9x.sh \
       build/wddm/x86/d3dptkmd.sys build/wddm/x86/d3dptumd.dll build/wddm/x86/d3dptkmd.inf; then
    # (the WDDM driver is built only on Windows, build-windows.sh's wddm
    # stage, and fetched here; absent it adds nothing, so both scripts agree)
    GUEST_STALE=1
  fi
  # a stamp is no good without the artifacts it claims are current
  [ -e guest-tools/out/d3dpt-driver.iso ] || GUEST_STALE=1
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || GUEST_STALE=1

  if ! have i686-w64-mingw32-gcc; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! have xorriso && ! have genisoimage && ! have mkisofs; then
    skip guest "needs xorriso (or genisoimage/mkisofs) for the ISO" || true
  elif [ -n "$GUEST_STALE" ]; then
    say "guest: guest-tools ISO + XP display driver"
    guest-tools/build-wrappers.sh
    stamp_save
    GUEST_STALE=""
    BUILT+=(guest)
  else
    say "guest"
    echo "    guest sources, protocol headers and qemu-3dfx unchanged - skipping"
  fi
fi

# --- summary ----------------------------------------------------------
say "summary after $((SECONDS - T0)) s"
if [ ${#BUILT[@]} -gt 0 ]; then printf '    built: %s\n' "${BUILT[*]}"; fi
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '    skipped:\n'
  printf '      %s\n' "${SKIPPED[@]}"
fi

# Warn about artifacts left behind by a stage this host could not run. A
# stage that ran refreshed its own output; a skipped one is the case nobody
# looks at. A guest-tools ISO older than the protocol it speaks does not
# announce itself. It reads as an XP guest that will not attach.
stale=()
if [ -n "$GUEST_STALE" ]; then
  for iso in guest-tools/out/*.iso; do
    [ -e "$iso" ] || continue
    stale+=("$iso (guest-tools/build-wrappers.sh)")
  done
fi
for s in ${SKIPPED[@]+"${SKIPPED[@]}"}; do
  case "$s" in
    exec*) [ -f "build/d3dpt/libd3dpt_exec.$SO" ] \
             && stale+=("build/d3dpt/libd3dpt_exec.$SO (scripts/build-d3dpt-exec.sh)") ;;
    # A host with no GTK builds everything except the front end the
    # packages install, and nothing else here would say so.
    mitsuami*) echo "    note: no launcher, so no package from this host" ;;
  esac
done
if [ ${#stale[@]} -gt 0 ]; then
  printf '\n    WARNING: these are older than the sources they are built from, and\n'
  printf '    this host cannot rebuild them. A stale guest-tools ISO after a\n'
  printf '    D3DPT_PROTO_VERSION bump reads as a guest that will not attach:\n'
  printf '      %s\n' "${stale[@]}"
fi

if [ -n "$RUN_TEST" ]; then
  say "scripts/test.sh host"
  exec scripts/test.sh host
fi

echo
if [ -n "$ROSETTA" ]; then
  echo "    next: scripts/package-macos.sh --x86_64 --no-sign --no-dmg   (stage and check the Intel app)"
else
  echo "    next: scripts/test.sh        (host stage, ~30 s)"
  echo "          scripts/test.sh all    (adds the XP and DOS guests, ~2 min)"
fi
