# The Windows cross-build image (docs/build-windows.md, M11).
#
# Every Windows artefact is built in here, so a Windows package is
# reproducible from any Linux box and nobody grows a mingw sysroot by
# hand. Arch (this project's Linux host) has mingw-w64-gcc but no mingw
# glib2/pixman/epoxy, which QEMU needs. Fedora packages all of them, and
# QEMU's own Windows CI uses the same base
# (qemu/tests/docker/dockerfiles/fedora-win64-cross.docker), so a QEMU
# build failure here is one upstream would see too.
#
# What it adds over QEMU's image: rustup with the x86_64-pc-windows-gnu
# target (the player, the launcher and libdisc are Rust and must match
# the mingw ABI of the QEMU DLL they link) and the packaging tools.
#
#   scripts/win-cross.sh --build        # build/refresh this image
#   scripts/win-cross.sh <command...>   # run a command in it, in the checkout
ARG FEDORA=43
FROM registry.fedoraproject.org/fedora:${FEDORA}

# Native side: what configure, meson and the generators run on.
RUN dnf install -y --setopt=install_weak_deps=False \
        bash bison bzip2 ca-certificates diffutils file findutils flex gcc gcc-c++ \
        git-core make meson ninja-build patch pkgconf \
        python3 python3-pip python3-setuptools rsync sed tar which xorriso zip zstd \
    && dnf clean all -y

# QEMU 11.1's mkvenv supports Python 3.9-3.13 and Fedora's default is newer
# (3.14 breaks it), so the build gets its own interpreter. It is the
# constraint scripts/configure-qemu.sh enforces on every host, met here by
# a package rather than by uv. `distlib` is installed for real because
# QEMU's fallback to pip's vendored copy is broken on pip >= 26
# (CLAUDE.md's "found no usable distlib"), and this image has no network
# during a build.
RUN dnf install -y --setopt=install_weak_deps=False python3.13 \
    && python3.13 -m ensurepip \
    && python3.13 -m pip install --no-cache-dir distlib setuptools \
    && dnf clean all -y
ENV QEMU_PYTHON=/usr/bin/python3.13

# Cross side: the mingw64 sysroot QEMU and the executor compile against.
# libepoxy is the Mesa pass-through's GL loader; gettext/iconv arrive as
# glib's own dependencies but are named here because the packaging step
# copies their DLLs. No SDL2. QEMU is configured --disable-sdl because
# nothing we ship opens a QEMU window, and Fedora's mingw64-SDL2 is
# sdl2-compat, which dragged SDL3.dll along at run time as well.
RUN dnf install -y --setopt=install_weak_deps=False \
        mingw-w64-tools \
        mingw64-gcc mingw64-gcc-c++ mingw64-headers mingw64-winpthreads \
        mingw64-glib2 mingw64-pixman mingw64-zlib mingw64-libepoxy \
        mingw64-gettext mingw64-win-iconv mingw64-pkg-config \
    && dnf clean all -y

# libslirp, which Fedora does not package for mingw. QEMU's `-netdev
# user` is a *compiled-in* backend, and every machine the launcher
# generates asks for one. Without it QEMU answers "network backend 'user'
# is not compiled into this binary". Same release the Flatpak manifest
# pins, for the same reason (the runtime lacks it too).
ARG LIBSLIRP=4.9.4
RUN curl -fsSL -o /tmp/libslirp.tar.gz \
      "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v${LIBSLIRP}/libslirp-v${LIBSLIRP}.tar.gz" \
    && echo "3998863b020aeda34bddc567097c6efba55a78cdf6eeee6bcd42c11ef23967da  /tmp/libslirp.tar.gz" \
       | sha256sum -c - \
    && tar -C /tmp -xf /tmp/libslirp.tar.gz \
    && mkdir -p /tmp/libslirp-v${LIBSLIRP}/build \
    && (cd /tmp/libslirp-v${LIBSLIRP} && mingw64-meson --buildtype=release build) \
    && ninja -C /tmp/libslirp-v${LIBSLIRP}/build install \
    && rm -rf /tmp/libslirp.tar.gz /tmp/libslirp-v${LIBSLIRP}

# Rust, in the image rather than on the host. The same stable channel the
# native build uses, plus the windows-gnu target. CARGO_HOME/RUSTUP_HOME
# live under /opt so a bind-mounted checkout never inherits (or writes)
# the host user's cargo state.
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable \
          --component rustfmt --component clippy \
          --target x86_64-unknown-linux-gnu --target x86_64-pc-windows-gnu \
    && chmod -R a+rwX /opt/rustup /opt/cargo

# The linker rustc calls for x86_64-pc-windows-gnu, and the pkg-config
# that answers for the mingw sysroot rather than for Fedora itself.
ENV CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
    PKG_CONFIG=/usr/bin/x86_64-w64-mingw32-pkg-config \
    LANG=C.UTF-8

# DXVK's d3d9 for Windows (ADR-007). The executor runs on the same DXVK
# on every host, so the image builds it too. DXVK compiles its
# GLSL to SPIR-V at build time with a *native* glslang; everything else it
# needs (the Vulkan headers, dxbc-spirv, libdisplay-info) is a submodule
# or a meson subproject. Last, so the layers above stay cached.
RUN dnf install -y --setopt=install_weak_deps=False glslang \
    && dnf clean all -y

# clang + lld build the Windows QEMU (patch 68, configure-qemu.sh
# --windows), because mingw GCC 15 has only emulated TLS and every
# __thread access is a call.
RUN dnf install -y --setopt=install_weak_deps=False clang lld \
    && dnf clean all -y
