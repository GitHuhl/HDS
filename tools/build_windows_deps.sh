#!/bin/bash
# Cross-builds liblo (OSC) and libwavpack (recording) as static libraries for the Windows target
# (x86_64-w64-mingw32), and installs them under third_party/win64-deps/{liblo,wavpack}/. Run this
# ONCE before `make clap-windows` -- it's a separate step (not folded into the Makefile itself) so
# a plain `make clap-windows` stays fast and doesn't need network access on every build, only the
# first.
#
# These two are the only non-Windows-native dependencies hds-windows.clap needs -- the CLAP plugin
# has no platform audio/MIDI code of its own (the host supplies buffers/note-events instead, see
# src/clap_plugin.c's own header comment). liblo and wavpack are plain,
# portable C libraries fetched straight from their real upstream release tarballs (SourceForge for
# liblo, wavpack.com for WavPack -- both real "make dist" releases shipping a working ./configure
# already, deliberately not a raw GitHub source snapshot, which doesn't) and cross-built from
# source -- no distro package manager involved, so this runs the same way on Ubuntu, Fedora, Arch,
# NixOS, etc. On NixOS specifically, run this from inside `nix-shell` (see shell.nix at the repo
# root, which provides the mingw-w64 cross toolchain + curl/tar/make this script needs) rather
# than trying to install anything globally.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
DEPS_DIR="$REPO_ROOT/third_party/win64-deps"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

LIBLO_VERSION=0.32
WAVPACK_VERSION=5.6.0
# Matches whatever mingw-w64 headers/CRT version your cross-compiler was already built against
# (check with `x86_64-w64-mingw32-gcc -print-search-dirs` -- look for a
# "mingw-w64-x86_64-w64-mingw32-<version>" store path in the "libraries" line) -- winpthreads'
# ABI is stable across nearby releases, but keeping this aligned avoids any doubt. Override with
# `MINGW_W64_VERSION=... tools/build_windows_deps.sh` if yours differs from the current default.
MINGW_W64_VERSION="${MINGW_W64_VERSION:-13.0.0}"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "error: x86_64-w64-mingw32-gcc not found -- install the mingw-w64 cross toolchain first" >&2
    echo "  (Debian/Ubuntu: sudo apt-get install mingw-w64)" >&2
    echo "  (NixOS/Nix: run 'nix-shell' from the repo root first -- see shell.nix)" >&2
    exit 1
fi

# Some mingw-w64 cross-toolchain builds (confirmed: nixpkgs' pkgsCross.mingwW64, at least as of
# some revisions) ship only the "win32" threading model -- no winpthreads, no pthread.h at all --
# rather than the "posix" model most Linux-distro mingw-w64 packages default to. backend.c uses
# pthread_create/join/mutexes uniformly across platforms (the sample disk thread, Sequencer
# loader threads), relying on winpthreads' real POSIX-threads-on-Windows implementation, so this
# is a hard requirement, not optional. Detect the gap here rather than failing deep inside a
# `make clap-windows` compile with a bare "pthread.h: No such file or directory".
NEED_WINPTHREADS=0
if ! echo '#include <pthread.h>' | x86_64-w64-mingw32-gcc -x c -E - >/dev/null 2>&1; then
    NEED_WINPTHREADS=1
    echo "--- x86_64-w64-mingw32-gcc has no pthread.h (win32 threading model) -- will cross-build winpthreads from source ---"
fi

echo "=== Fetching liblo $LIBLO_VERSION ==="
cd "$WORK_DIR"
curl -fsSL -o liblo.tar.gz \
    "https://sourceforge.net/projects/liblo/files/liblo/$LIBLO_VERSION/liblo-$LIBLO_VERSION.tar.gz/download"
tar xzf liblo.tar.gz

echo "=== Fetching WavPack $WAVPACK_VERSION ==="
# From WavPack's own real maintainer-published release tarball (wavpack.com/downloads.html), NOT
# GitHub's auto-generated per-tag source snapshot -- a plain git checkout snapshot like that one
# doesn't include the autotools-generated files (./configure, Makefile.in, aclocal.m4, the
# gettext/po/ scaffolding) a real "make dist" release does, so bootstrapping it via
# autoreconf/autopoint turned out to be a real rabbit hole (mismatched-gettext-version config.rpath
# conflicts that --force alone didn't resolve cleanly across environments). The real release
# tarball sidesteps all of that -- it already ships a working ./configure, same as liblo's own
# SourceForge tarball just below. If your network can't reach wavpack.com, download
# wavpack-$WAVPACK_VERSION.tar.bz2 by hand from https://www.wavpack.com/downloads.html and save it
# as "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.bz2" before re-running this script.
if ! ls "$WORK_DIR"/wavpack-"$WAVPACK_VERSION".tar.bz2 >/dev/null 2>&1; then
    curl -fsSL -o "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.bz2" \
        "https://www.wavpack.com/wavpack-$WAVPACK_VERSION.tar.bz2"
fi
tar xjf "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.bz2"

if [ "$NEED_WINPTHREADS" = "1" ]; then
    echo "=== Fetching mingw-w64 $MINGW_W64_VERSION source (winpthreads only) ==="
    # winpthreads is a self-contained subdirectory of the official mingw-w64 project release, with
    # its own ./configure, buildable standalone against an already-existing cross-compiler (exactly
    # our situation: x86_64-w64-mingw32-gcc + headers/CRT already on PATH, just missing this one
    # component) -- the same technique Debian/Fedora's own mingw-w64 packaging uses internally to
    # produce a posix-threaded toolchain from a win32-threaded stage-1 gcc. Only extracting this one
    # subdirectory from the full tarball, not the whole mingw-w64-headers/crt/tools tree (already
    # have those from the cross-compiler itself).
    curl -fsSL -o "$WORK_DIR/mingw-w64.tar.bz2" \
        "https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-v$MINGW_W64_VERSION.tar.bz2/download"
    tar xjf "$WORK_DIR/mingw-w64.tar.bz2" -C "$WORK_DIR" \
        "mingw-w64-v$MINGW_W64_VERSION/mingw-w64-libraries/winpthreads"
fi

echo "=== Cross-building liblo ==="
mkdir -p "$DEPS_DIR/liblo"
cd "$WORK_DIR/liblo-$LIBLO_VERSION"
./configure --host=x86_64-w64-mingw32 --prefix="$DEPS_DIR/liblo" \
    --enable-static --disable-shared --disable-tools --disable-tests --disable-examples --disable-network-tests \
    CC=x86_64-w64-mingw32-gcc
make -j"$(nproc)"
make install

echo "=== Cross-building WavPack (library only) ==="
mkdir -p "$DEPS_DIR/wavpack"
cd "$WORK_DIR/wavpack-$WAVPACK_VERSION"
# The real release tarball fetched above ships a working ./configure already -- no autoreconf/
# autopoint bootstrap needed (see the "Fetching WavPack" comment above for why that path was
# dropped). If this ever fails because a hand-downloaded tarball turned out to be a raw source
# snapshot after all, that's the signal to go download the real release tarball instead, not to
# resume patching the bootstrap chain.
if [ ! -x ./configure ]; then
    echo "error: no ./configure in $PWD -- this isn't the real WavPack release tarball" >&2
    echo "  (a raw git source snapshot doesn't ship one). Get the real release tarball from" >&2
    echo "  https://www.wavpack.com/downloads.html instead." >&2
    exit 1
fi
./configure --host=x86_64-w64-mingw32 --prefix="$DEPS_DIR/wavpack" \
    --enable-static --disable-shared --disable-apps \
    CC=x86_64-w64-mingw32-gcc
make -j"$(nproc)"
make install

if [ "$NEED_WINPTHREADS" = "1" ]; then
    echo "=== Cross-building winpthreads (pthread.h + libwinpthread) ==="
    mkdir -p "$DEPS_DIR/winpthreads"
    cd "$WORK_DIR/mingw-w64-v$MINGW_W64_VERSION/mingw-w64-libraries/winpthreads"
    ./configure --host=x86_64-w64-mingw32 --prefix="$DEPS_DIR/winpthreads" \
        --enable-static --disable-shared \
        CC=x86_64-w64-mingw32-gcc
    make -j"$(nproc)"
    make install
    echo "=== Done. liblo, wavpack, and winpthreads installed under $DEPS_DIR ==="
    echo "IMPORTANT: your toolchain needed winpthreads built from source (see above) -- make sure"
    echo "you're using the updated Makefile that adds \$(DEPS_DIR)/winpthreads/include and"
    echo "-lwinpthread for the Windows targets, not a plain -lpthread."
else
    echo "=== Done. liblo and wavpack installed under $DEPS_DIR ==="
fi
echo "You can now run: make clap-windows"
