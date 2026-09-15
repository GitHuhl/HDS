#!/bin/bash
# Cross-builds liblo (OSC) and libwavpack (recording) as static libraries for the Windows target
# (x86_64-w64-mingw32), and installs them under third_party/win64-deps/{liblo,wavpack}/. Run this
# ONCE before `make windows` -- it's a separate step (not folded into the Makefile itself) so a
# plain `make windows` stays fast and doesn't need network access on every build, only the first.
#
# These two are the only non-Windows-native dependencies synth_engine.exe needs: audio (WASAPI)
# and MIDI (WinMM) come from the Windows SDK itself via mingw-w64, no extra library required (see
# the "Windows audio/MIDI platform shim" comments in src/backend.c). liblo and wavpack are plain,
# portable C libraries with no Windows-specific packaging available through apt, so this script
# builds them from their real upstream/distro source the same way any other Linux distro would.
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
    exit 1
fi

echo "=== Fetching liblo $LIBLO_VERSION ==="
cd "$WORK_DIR"
curl -fsSL -o liblo.tar.gz \
    "https://sourceforge.net/projects/liblo/files/liblo/$LIBLO_VERSION/liblo-$LIBLO_VERSION.tar.gz/download"
tar xzf liblo.tar.gz

echo "=== Fetching WavPack $WAVPACK_VERSION ==="
# WavPack has no single canonical source-tarball URL as stable as liblo's SourceForge release, so
# this uses the Debian/Ubuntu source package (same upstream release, repackaged) via apt. If your
# system's apt sources don't have deb-src lines enabled, enable them first (e.g. add
# "deb-src http://archive.ubuntu.com/ubuntu noble main universe" to /etc/apt/sources.list, or the
# equivalent deb822 Types: line) and run `sudo apt-get update` -- or download
# wavpack-$WAVPACK_VERSION.tar.bz2 manually from https://www.wavpack.com/downloads.html and drop
# it in this directory before re-running this script.
if ! ls "$WORK_DIR"/wavpack*"$WAVPACK_VERSION"*.tar.bz2 >/dev/null 2>&1; then
    (cd "$WORK_DIR" && apt-get source wavpack) || true # piping this into grep for a status check risks SIGPIPE aborting a still-in-progress download -- check the resulting file directly instead
fi
# Debian's apt-get source names this wavpack_<ver>.orig.tar.bz2 (underscore); a manually
# downloaded upstream tarball from wavpack.com is typically wavpack-<ver>.tar.bz2 (hyphen) --
# accept either.
WAVPACK_TARBALL=$(ls "$WORK_DIR"/wavpack*"$WAVPACK_VERSION"*.tar.bz2 2>/dev/null | head -1)
if [ -z "$WAVPACK_TARBALL" ]; then
    echo "error: couldn't fetch WavPack source via 'apt-get source wavpack'." >&2
    echo "  Enable deb-src in your apt sources and 'sudo apt-get update', or manually download" >&2
    echo "  wavpack-$WAVPACK_VERSION.tar.bz2 from https://www.wavpack.com/downloads.html into $WORK_DIR" >&2
    exit 1
fi
tar xjf "$WAVPACK_TARBALL"

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
./configure --host=x86_64-w64-mingw32 --prefix="$DEPS_DIR/wavpack" \
    --enable-static --disable-shared --disable-apps \
    CC=x86_64-w64-mingw32-gcc
make -j"$(nproc)"
make install

echo "=== Done. liblo and wavpack installed under $DEPS_DIR ==="
echo "You can now run: make windows"
