#!/bin/bash
# Cross-builds liblo (OSC) and libwavpack (recording) as static libraries for the Windows target
# (x86_64-w64-mingw32), and installs them under third_party/win64-deps/{liblo,wavpack}/. Run this
# ONCE before `make windows` -- it's a separate step (not folded into the Makefile itself) so a
# plain `make windows` stays fast and doesn't need network access on every build, only the first.
#
# These two are the only non-Windows-native dependencies synth_engine.exe needs: audio (WASAPI)
# and MIDI (WinMM) come from the Windows SDK itself via mingw-w64, no extra library required (see
# the "Windows audio/MIDI platform shim" comments in src/backend.c). liblo and wavpack are plain,
# portable C libraries fetched straight from their real upstream release tarballs (SourceForge for
# liblo, GitHub Releases for WavPack) and cross-built from source -- no distro package manager
# involved, so this runs the same way on Ubuntu, Fedora, Arch, NixOS, etc. On NixOS specifically,
# run this from inside `nix-shell` (see shell.nix at the repo root, which provides the mingw-w64
# cross toolchain + curl/tar/make this script needs) rather than trying to install anything globally.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
DEPS_DIR="$REPO_ROOT/third_party/win64-deps"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

LIBLO_VERSION=0.32
WAVPACK_VERSION=5.6.0

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "error: x86_64-w64-mingw32-gcc not found -- install the mingw-w64 cross toolchain first" >&2
    echo "  (Debian/Ubuntu: sudo apt-get install mingw-w64)" >&2
    echo "  (NixOS/Nix: run 'nix-shell' from the repo root first -- see shell.nix)" >&2
    exit 1
fi

echo "=== Fetching liblo $LIBLO_VERSION ==="
cd "$WORK_DIR"
curl -fsSL -o liblo.tar.gz \
    "https://sourceforge.net/projects/liblo/files/liblo/$LIBLO_VERSION/liblo-$LIBLO_VERSION.tar.gz/download"
tar xzf liblo.tar.gz

echo "=== Fetching WavPack $WAVPACK_VERSION ==="
# Straight from WavPack's own GitHub Releases (github.com/dbry/WavPack) -- a plain, versioned
# tarball URL with no distro package manager involved (this used to shell out to
# `apt-get source wavpack`, which only exists on Debian/Ubuntu and breaks on every other distro,
# NixOS included -- fixed here, not worked around per-distro). If your network blocks GitHub,
# download the same tarball by hand from
# https://github.com/dbry/WavPack/archive/refs/tags/$WAVPACK_VERSION.tar.gz and save it as
# "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.gz" before re-running this script.
if ! ls "$WORK_DIR"/wavpack-"$WAVPACK_VERSION".tar.gz >/dev/null 2>&1; then
    curl -fsSL -o "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.gz" \
        "https://github.com/dbry/WavPack/archive/refs/tags/$WAVPACK_VERSION.tar.gz"
fi
tar xzf "$WORK_DIR/wavpack-$WAVPACK_VERSION.tar.gz"
# GitHub's source-archive tarballs extract to "<repo>-<tag>/" (capitalized "WavPack-5.6.0", not
# lowercase) -- normalize to the same "wavpack-$WAVPACK_VERSION" directory name the rest of this
# script (and the Cross-building step below) already expects, matching what a real upstream
# release tarball from wavpack.com would have been named.
if [ -d "WavPack-$WAVPACK_VERSION" ] && [ ! -d "wavpack-$WAVPACK_VERSION" ]; then
    mv "WavPack-$WAVPACK_VERSION" "wavpack-$WAVPACK_VERSION"
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
# Unlike liblo's SourceForge tarball (a real "make dist" release including the generated
# ./configure), this is a plain GitHub source snapshot -- autotools projects routinely don't
# commit their generated configure/Makefile.in/aclocal.m4 to git, so it may need bootstrapping
# first. Needs autoconf/automake/libtool on PATH when this branch runs (shell.nix provides them).
if [ ! -x ./configure ]; then
    echo "--- no ./configure in the WavPack tarball, bootstrapping via autoreconf ---"
    # AUTOPOINT="autopoint --force": the checked-in config.rpath in this GitHub source snapshot
    # differs from what the locally-installed gettext's autopoint would generate itself (they were
    # bootstrapped against a different gettext version), so autopoint's default safety check
    # ("File config.rpath has been locally modified... Not overwriting") refuses to proceed.
    # autoreconf's own -f doesn't propagate down to the autopoint sub-step -- it needs its own
    # --force, passed through via the AUTOPOINT env var autoreconf reads for this exact purpose.
    if [ -x ./autogen.sh ]; then ./autogen.sh; else AUTOPOINT="autopoint --force" autoreconf -fi; fi
fi
./configure --host=x86_64-w64-mingw32 --prefix="$DEPS_DIR/wavpack" \
    --enable-static --disable-shared --disable-apps \
    CC=x86_64-w64-mingw32-gcc
make -j"$(nproc)"
make install

echo "=== Done. liblo and wavpack installed under $DEPS_DIR ==="
echo "You can now run: make windows"
