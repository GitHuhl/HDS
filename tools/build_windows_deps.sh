#!/bin/bash
# tools/build_windows_deps.sh: builds every third-party dependency the Windows CLAP targets
# (`make clap-windows`/`make hds-fx-windows`, see the Makefile's own "Windows CLAP plugins" section)
# need, into third_party/win64-deps/ -- NOT checked into git (rerun this after a clean checkout, or
# whenever third_party/win64-deps/ is deleted). Resurrects and substantially extends the old
# (removed) same-named script from before this project first dropped Windows support: that version
# only ever had to cross-build liblo + WavPack for a headless (no GUI) plugin. This one also builds
# the real dependency Cairo needs (pixman) from scratch, since NO mingw-w64 package for Cairo exists
# anywhere (checked apt; upstream ships no Windows cross build either) -- see this script's own
# Cairo step for exactly why freetype/fontconfig/libpng are never needed at all on this target.
#
# Prerequisites, either of:
#   - `nix-shell` at the repo root (see shell.nix's own Windows section): provides the whole
#     toolchain (mingw cross gcc/g++, meson, ninja, autoconf/automake/libtool/gettext, pkg-config)
#     AND exports HDS_WIN_{PIXMAN,CAIRO,LIBLO}_SRC pointing at nixpkgs' own already-fetched upstream
#     source tarballs for these three packages -- this script's own hds_fetch_and_extract() uses
#     those directly instead of apt-get, which does not exist at all on a NixOS/nix-shell host (a
#     real nix-shell run confirmed this: without this route, pixman/cairo were silently never built
#     at all, since `apt-get source` failing aborted this whole script under `set -e`).
#   - apt, installed manually: mingw-w64, g++-mingw-w64-x86-64, gcc-mingw-w64-x86-64 (posix
#     threading model -- see this script's own toolchain-check step for why), libz-mingw-w64-dev,
#     meson, ninja-build, autoconf, automake, libtoolize, gettext, autopoint, pkg-config. Also needs
#     a `deb-src` entry enabled (Ubuntu's default sources.list ships none) so `apt-get source
#     pixman`/`cairo`/`liblo` below can fetch upstream's own source packages -- add one for
#     archive.ubuntu.com pointing at the running release's suites if `apt-get source` fails with
#     "You must put some 'deb-src' URIs in your sources.list".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/third_party/win64-deps"
SRC="$DEPS/src"
BUILD="$DEPS/build"

echo "== Checking toolchain =="
for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ meson ninja autoreconf pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing: $tool -- see this script's own header comment for what to apt-get install" >&2; exit 1; }
done
# mingw-w64 ships TWO gcc/g++ variants (posix/win32 threading model) behind update-alternatives --
# only the posix one has a real pthread_create/pthread_join implementation, which backend.c's own
# sample_disk_thread genuinely needs (the win32-threading-model variant silently lacks it). Confirmed
# directly: a plain pthread smoke test only ran correctly after switching to the -posix alternative.
if ! x86_64-w64-mingw32-gcc -dM -E - < /dev/null 2>/dev/null | grep -q "_REENTRANT\|_MT"; then
    :  # not a reliable enough signal to auto-detect/auto-switch -- just document the requirement
fi
echo "NOTE: if pthread_create/pthread_join misbehave later, run:"
echo "  update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix"
echo "  update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix"

mkdir -p "$SRC" "$DEPS/lib" "$DEPS/include"

# A tiny pkg-config wrapper scoped to ONLY this cross sysroot + our own installed deps -- meson/
# liblo's own configure both look for "x86_64-w64-mingw32-pkg-config" by convention (the standard
# mingw-w64 autotools/meson cross naming scheme), which doesn't exist as a real binary anywhere.
PKG_CONFIG_WRAPPER=/usr/local/bin/x86_64-w64-mingw32-pkg-config
if [ ! -x "$PKG_CONFIG_WRAPPER" ]; then
    echo "== Installing $PKG_CONFIG_WRAPPER =="
    cat > "$PKG_CONFIG_WRAPPER" << EOF
#!/bin/bash
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig:$DEPS/lib/pkgconfig
export PKG_CONFIG_PATH=
exec pkg-config "\$@"
EOF
    chmod +x "$PKG_CONFIG_WRAPPER"
fi

# Fetches upstream source for $1 (pixman/cairo/liblo) and extracts it into $SRC, via one of two
# routes:
#
#   1. Inside `nix-shell` (the officially supported dev environment, see shell.nix): shell.nix's
#      own shellHook exports HDS_WIN_{PIXMAN,CAIRO,LIBLO}_SRC, each pointing at the exact Nix store
#      tarball nixpkgs' OWN ordinary (non-cross) pixman/cairo/liblo package already fetches via
#      fetchurl -- i.e. an already-hash-pinned, already-correct upstream URL that nixpkgs itself
#      verified, needing zero URL-guessing here. This is REQUIRED on a NixOS/nix-shell host: `apt-get`
#      does not exist there at all. Confirmed as the real, live failure mode (not hypothetical): a
#      real nix-shell run hit `cairo.h: No such file or directory` because the old apt-get-only
#      version of this function silently produced no source tree at all on that host (apt-get
#      missing -> the pipeline failed -> `set -e` aborted the *script*, not just this function, so
#      pixman/cairo were simply never built and third_party/win64-deps/include/cairo never existed).
#   2. On an apt-based host (Ubuntu/Debian, no HDS_WIN_*_SRC set): the original `apt-get source`
#      route, unchanged -- needs deb-src enabled (see this script's own header comment on adding one
#      for archive.ubuntu.com if `apt-get source` fails with "You must put some 'deb-src' URIs in
#      your sources.list"). WITHOUT pinning an exact version: Ubuntu's own packaged version drifts
#      release to release, and pinning one that doesn't exist errors out ("Unable to find a source
#      package") -- this fetches whatever's current.
#
# Either way, the extracted directory is later located via `ls -d "$SRC"/$pkg-*/` (pixman-X.Y.Z/,
# cairo-X.Y.Z/, liblo-X.Y/) -- both routes produce exactly that naming, since nixpkgs' own fetchurl
# tarballs for all three unpack to the plain upstream "$pname-$version/" layout, same as Ubuntu's.
hds_fetch_and_extract() {
    local pkg="$1"
    local nix_src_var="$2"   # e.g. HDS_WIN_PIXMAN_SRC -- unset outside this repo's own nix-shell
    if ls -d "$SRC/$pkg"-*/ >/dev/null 2>&1; then
        return 0   # already extracted from a previous run
    fi
    local nix_src="${!nix_src_var:-}"
    if [ -n "$nix_src" ]; then
        echo "Extracting $pkg from nix-shell-provided source: $nix_src"
        tar xf "$nix_src" -C "$SRC"
    elif command -v apt-get >/dev/null 2>&1; then
        if ! ls "$SRC/$pkg"_*.dsc >/dev/null 2>&1; then
            (cd "$SRC" && apt-get source --download-only "$pkg" 2>&1 | tail -15)
        fi
        local dsc
        dsc="$(ls "$SRC/$pkg"_*.dsc | head -1)"
        (cd "$SRC" && dpkg-source -x "$(basename "$dsc")" 2>&1 | tail -10)
    else
        echo "Cannot fetch $pkg source: no apt-get on PATH and \$$nix_src_var is not set." >&2
        echo "Run this from inside 'nix-shell' at the repo root (shell.nix exports \$$nix_src_var)," >&2
        echo "or on an apt-based system with deb-src enabled." >&2
        exit 1
    fi
}

CROSS_FILE="$DEPS/mingw-w64-cross.ini"
cat > "$CROSS_FILE" << EOF
[binaries]
c = 'x86_64-w64-mingw32-gcc'
cpp = 'x86_64-w64-mingw32-g++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
pkg-config = 'x86_64-w64-mingw32-pkg-config'
windres = 'x86_64-w64-mingw32-windres'
exe_wrapper = 'wine'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

# --- zlib: NOT built here -- Ubuntu's own libz-mingw-w64-dev package already provides a real
# cross-built static lib at /usr/x86_64-w64-mingw32/lib/libz.a (the one real mingw-w64-cross-built
# dependency that exists pre-packaged; apt-get install libz-mingw-w64-dev if missing).

# --- pixman: cairo's own genuinely-required low-level compositing dependency -- no mingw-w64
# package exists, built from source via meson (pixman dropped autotools around 0.40).
if [ ! -f "$DEPS/lib/libpixman-1.a" ]; then
    echo "== Building pixman =="
    hds_fetch_and_extract pixman HDS_WIN_PIXMAN_SRC
    PIXMAN_DIR="$(ls -d "$SRC"/pixman-*/ | head -1)"
    # Upstream pixman's own meson.build forces -DPIXMAN_API=__declspec(dllexport) unconditionally
    # whenever the compiler supports the attribute -- regardless of --default-library=static -- so
    # a plain static build still bakes a .drectve "-export:symbolname" linker directive into every
    # object file (confirmed directly via objdump -s -j .drectve on a built .o), which the FINAL
    # DLL link then re-exports as if pixman itself were the shared library, polluting hds.clap's own
    # export table with ~150 unrelated pixman_* symbols (harmless on Windows specifically -- PE
    # imports are resolved per-DLL-by-name at load time, unlike ELF's flat-namespace symbol
    # interposition that made the analogous NAM/Eigen export leak a real, confirmed cross-plugin
    # crash on Linux -- but still worth suppressing for the same reason -fvisibility=hidden was
    # applied there: no reason for a private, statically-linked implementation detail to be
    # DLL-exported at all). Patched here, once, right after extracting a fresh source tree.
    sed -i "s/if cc.has_function_attribute('dllexport')\$/if cc.has_function_attribute('dllexport') and get_option('default_library') != 'static'/" \
        "$PIXMAN_DIR/pixman/meson.build"
    rm -rf "$BUILD/pixman"
    (cd "$PIXMAN_DIR" && meson setup "$BUILD/pixman" --cross-file "$CROSS_FILE" --prefix="$DEPS" \
        --default-library=static -Dtests=disabled)
    ninja -C "$BUILD/pixman"
    ninja -C "$BUILD/pixman" install
fi

# --- Cairo: configured with png/freetype/fontconfig/xlib/xcb/quartz all disabled -- on
# host_machine.system()=='windows' cairo's own meson.build auto-enables CAIRO_HAS_WIN32_SURFACE and
# CAIRO_HAS_WIN32_FONT unconditionally (native GDI surface + fonts, needing only gdi32/msimg32,
# both bundled with mingw-w64's own import libraries) -- so freetype/fontconfig/libpng are never
# needed at all: nothing in this GUI's own production code (hds_clap_gui_draw.c/
# hds_fx_gui_draw.c/either Win32 platform shim) ever calls cairo_surface_write_to_png either, only
# this repo's own Linux-only screenshot test scaffolding does.
if [ ! -f "$DEPS/lib/libcairo.a" ]; then
    echo "== Building cairo =="
    hds_fetch_and_extract cairo HDS_WIN_CAIRO_SRC
    CAIRO_DIR="$(ls -d "$SRC"/cairo-*/ | head -1)"
    rm -rf "$BUILD/cairo"
    (cd "$CAIRO_DIR" && meson setup "$BUILD/cairo" --cross-file "$CROSS_FILE" --prefix="$DEPS" \
        --default-library=static \
        -Dpng=disabled -Dzlib=disabled -Dxlib=disabled -Dxcb=disabled -Dtee=disabled -Dtests=disabled \
        -Dfontconfig=disabled -Dfreetype=disabled -Dquartz=disabled -Ddwrite=disabled)
    ninja -C "$BUILD/cairo"
    ninja -C "$BUILD/cairo" install
fi

# --- liblo: OSC transport, needed by both plugins. Ships only configure.ac (no generated
# ./configure) -- autoreconf regenerates it; a modern autoconf/automake/libtool/gettext combo (as
# opposed to whatever ships in liblo's own release tarball's cached generated files) needs no
# special workarounds here (the old, removed version of this script had autopoint/config.rpath
# workarounds for an older toolchain combination that no longer apply).
if [ ! -f "$DEPS/lib/liblo.a" ]; then
    echo "== Building liblo =="
    hds_fetch_and_extract liblo HDS_WIN_LIBLO_SRC
    LIBLO_DIR="$(ls -d "$SRC"/liblo-*/ | head -1)"
    (cd "$LIBLO_DIR" && autoreconf -fiv)
    (cd "$LIBLO_DIR" && PKG_CONFIG="$PKG_CONFIG_WRAPPER" ./configure \
        --host=x86_64-w64-mingw32 --prefix="$DEPS" \
        --disable-shared --enable-static --disable-examples --disable-tests --disable-tools \
        CC=x86_64-w64-mingw32-gcc)
    (cd "$LIBLO_DIR" && make -j"$(nproc)")
    (cd "$LIBLO_DIR" && make install)
fi

echo "== Windows dependencies ready under $DEPS =="
echo "Build with: make clap-windows   (or: make hds-fx-windows)"
