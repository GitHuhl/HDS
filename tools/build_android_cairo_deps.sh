#!/usr/bin/env bash
# tools/build_android_cairo_deps.sh: cross-builds freetype/pixman/cairo for the NDK aarch64
# Android target, into third_party/android-deps/ -- NOT checked into git (rerun this after a
# clean checkout, or whenever third_party/android-deps/ is deleted). Same reason as
# build_windows_deps.sh's own: no prebuilt Android cairo exists anywhere, and it's cairo's own
# real build system (meson), not something tools/build_android_manual.sh's aapt2/d8/javac
# pipeline can produce.
#
# Built with FreeType enabled and Fontconfig explicitly disabled (-Dfreetype=enabled
# -Dfontconfig=disabled, not left at either option's own 'auto' default): cairo's meson.build
# marks fontconfig "required" (not auto-disabled the way it is on windows/darwin) on every other
# host_machine.system(), including 'android' -- auto-detecting it would either fail outright or,
# worse, silently pick up this BUILD MACHINE's own native x86_64 fontconfig via a stray
# pkg-config match (the same "auto picks up a host-native lib during a cross build" failure mode
# build_windows_deps.sh's own comment calls out for cairo's glib/spectre/symbol-lookup options).
# Fontconfig is never wanted here regardless: cairo's toy-font API
# (cairo_select_font_face()/"sans-serif") that fontconfig would back is the one this whole port
# bypasses -- confirmed directly against cairo's own source that its toy-font backend is wired to
# NULL when fontconfig=disabled -- in favor of cairo_ft_font_face_create_for_ft_face() loading the
# two fonts this repo embeds itself (third_party/dejavu-fonts/, see embed_font.S) via a plain
# FT_Face, no Fontconfig involved at any point. See hds_fx_gui_draw.c's own font-selection macro
# for the call-site side of this.
#
# Prerequisite: `nix-shell` at the repo root (see shell.nix's own Android section) -- provides the
# real NDK (ANDROID_NDK_ROOT) AND exports HDS_ANDROID_{PIXMAN,FREETYPE,CAIRO}_SRC, each pointing at
# the exact Nix store tarball nixpkgs' own ordinary (non-cross) pixman/freetype/cairo package
# already fetches via fetchurl -- an already-hash-pinned, already-correct upstream URL/version
# nixpkgs itself verified, the same version this project's native Linux build (`make clap`/
# `make hds-fx`) already links against. Needs meson/ninja too (both already in shell.nix's
# buildInputs, used for the Windows cross-build above).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/third_party/android-deps"
SRC="$DEPS/src"
BUILD="$DEPS/build"

mkdir -p "$SRC" "$DEPS/lib" "$DEPS/include"

if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
    echo "ANDROID_NDK_ROOT is not set -- run this from inside 'nix-shell' at the repo root" >&2
    echo "(same prerequisite tools/build_android_manual.sh documents)." >&2
    exit 1
fi
NDK_BIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
for tool in aarch64-linux-android26-clang aarch64-linux-android26-clang++ llvm-ar llvm-strip; do
    [ -x "$NDK_BIN/$tool" ] || { echo "missing $NDK_BIN/$tool -- check ANDROID_NDK_ROOT" >&2; exit 1; }
done

# Extracts upstream source for $1 (pixman/freetype/cairo) into $SRC from the nixpkgs-provided
# tarball named by $2 (an HDS_ANDROID_*_SRC env var -- see this script's own header comment) --
# same posture as build_windows_deps.sh's own hds_fetch_and_extract(), minus that function's
# apt-get fallback: this target has no apt-cross-package equivalent to fall back to at all, so
# nix-shell isn't optional here the way it (sort of) is there.
hds_fetch_and_extract() {
    local pkg="$1"
    local nix_src_var="$2"
    if ls -d "$SRC/$pkg"-*/ >/dev/null 2>&1; then
        return 0   # already extracted from a previous run
    fi
    local nix_src="${!nix_src_var:-}"
    if [ -z "$nix_src" ]; then
        echo "\$$nix_src_var is not set -- run this from inside 'nix-shell' at the repo root" >&2
        echo "(shell.nix's own Android section exports it)." >&2
        exit 1
    fi
    echo "Extracting $pkg from nix-shell-provided source: $nix_src"
    tar xf "$nix_src" -C "$SRC"
}

CROSS_FILE="$DEPS/android-cross.ini"
cat > "$CROSS_FILE" << EOF
[binaries]
c = '$NDK_BIN/aarch64-linux-android26-clang'
cpp = '$NDK_BIN/aarch64-linux-android26-clang++'
ar = '$NDK_BIN/llvm-ar'
strip = '$NDK_BIN/llvm-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
pkg_config_libdir = '$DEPS/lib/pkgconfig'
needs_exe_wrapper = true
EOF

# --- freetype: built with brotli/bzip2/harfbuzz/png all explicitly disabled (each is an 'auto'
# feature upstream, so left alone they risk the same "cross build quietly links a host-native lib
# via pkg-config" failure mode build_windows_deps.sh's own cairo comment calls out) and zlib set
# to freetype's own bundled copy ('internal', so it needs no cross-built system zlib either) --
# none of brotli(WOFF2)/bzip2(some old compressed PCF bitmap fonts)/harfbuzz(complex text
# shaping)/PNG(embedded color bitmap glyphs, e.g. emoji) are needed to render plain TTF outline
# glyphs from DejaVu Sans, the one and only font this port ever loads (see embed_font.S).
if [ ! -f "$DEPS/lib/libfreetype.a" ]; then
    echo "== Building freetype =="
    hds_fetch_and_extract freetype HDS_ANDROID_FREETYPE_SRC
    FREETYPE_DIR="$(ls -d "$SRC"/freetype-*/ | head -1)"
    rm -rf "$BUILD/freetype"
    (cd "$FREETYPE_DIR" && meson setup "$BUILD/freetype" --cross-file "$CROSS_FILE" --prefix="$DEPS" \
        --default-library=static \
        -Dbrotli=disabled -Dbzip2=disabled -Dharfbuzz=disabled -Dpng=disabled -Dzlib=internal \
        -Dtests=disabled)
    ninja -C "$BUILD/freetype"
    ninja -C "$BUILD/freetype" install
fi

# --- pixman: cairo's own genuinely-required low-level compositing dependency, same role as in
# build_windows_deps.sh's own pixman step -- but the dllexport-leak that script patches around is
# a Windows/PE-only concern (a .drectve linker directive), not applicable to an ELF .so target,
# so no equivalent patch is needed here.
if [ ! -f "$DEPS/lib/libpixman-1.a" ]; then
    echo "== Building pixman =="
    hds_fetch_and_extract pixman HDS_ANDROID_PIXMAN_SRC
    PIXMAN_DIR="$(ls -d "$SRC"/pixman-*/ | head -1)"
    rm -rf "$BUILD/pixman"
    (cd "$PIXMAN_DIR" && meson setup "$BUILD/pixman" --cross-file "$CROSS_FILE" --prefix="$DEPS" \
        --default-library=static -Dtests=disabled)
    ninja -C "$BUILD/pixman"
    ninja -C "$BUILD/pixman" install
fi

# --- cairo: freetype enabled, fontconfig explicitly disabled (see this script's own header
# comment for why 'auto' is never safe to leave either one at here), every other surface/font
# backend and the glib/spectre/symbol-lookup "auto picks up a host-native lib" risks disabled for
# the same reasons build_windows_deps.sh's own cairo step documents -- this GUI needs only the
# plain image surface (cairo_image_surface_create(), pointed directly at the locked
# ANativeWindow buffer -- see hds_android.c's own render pipeline) and FreeType-backed text, no
# PNG/zlib/X11/GDI/GObject/libbacktrace integration of any kind.
#
# A real, pre-existing cairo build-script bug (independent of Android, confirmed directly against
# cairo's own source, not assumed) is patched here, once, right after extracting a fresh source
# tree: HAVE_FT_COLR_V1/HAVE_FT_PALETTE_SELECT get set whenever the cross freetype exports
# FT_Palette_Select (true for any freetype >= 2.10, this one included), but the FT_Color type
# those two macros gate code around is only ever pulled in under a DIFFERENT, unrelated
# HAVE_FT_SVG_DOCUMENT guard a few lines later in the same header -- so src/cairo-ft-private.h
# fails to compile with "unknown type name 'FT_Color'" the moment HAVE_FT_SVG_DOCUMENT is 0 (which
# it always is here -- this freetype build has no SVG-in-font support, and doesn't need any).
if [ ! -f "$DEPS/lib/libcairo.a" ]; then
    echo "== Building cairo =="
    hds_fetch_and_extract cairo HDS_ANDROID_CAIRO_SRC
    CAIRO_DIR="$(ls -d "$SRC"/cairo-*/ | head -1)"
    CAIRO_FT_PRIVATE_H="$CAIRO_DIR/src/cairo-ft-private.h"
    if ! grep -q "include FT_COLOR_H" "$CAIRO_FT_PRIVATE_H"; then
        sed -i '/#include "cairo-ft.h"/a #include FT_COLOR_H' "$CAIRO_FT_PRIVATE_H"
    fi
    rm -rf "$BUILD/cairo"
    (cd "$CAIRO_DIR" && PKG_CONFIG_PATH="$DEPS/lib/pkgconfig" meson setup "$BUILD/cairo" \
        --cross-file "$CROSS_FILE" --prefix="$DEPS" --default-library=static \
        -Dfreetype=enabled -Dfontconfig=disabled \
        -Dpng=disabled -Dzlib=disabled -Dxlib=disabled -Dxcb=disabled -Dtee=disabled \
        -Dtests=disabled -Dquartz=disabled -Ddwrite=disabled -Dglib=disabled \
        -Dspectre=disabled -Dsymbol-lookup=disabled)
    # The default 'all' ninja target also builds util/malloc-stats.c, a debug-tooling binary that
    # calls backtrace_symbols() -- undeclared on Bionic (Android's libc), and never linked into
    # libhds_native.so regardless. Building the one real target this port needs instead
    # sidesteps that unrelated failure entirely rather than patching around it.
    ninja -C "$BUILD/cairo" src/libcairo.a
    ninja -C "$BUILD/cairo" install-pkgconfigDATA install-includeHEADERS install-cairoincludeHEADERS 2>/dev/null || true
    # meson's own 'install' target depends on 'all' (which includes malloc-stats), so this copies
    # the one real build product + the headers every other ninja install-* subtarget above may
    # have missed, by hand, matching exactly what the earlier scratchpad verification installed
    # (confirmed working: a real ARM64 test program compiled+linked against this exact layout).
    cp "$BUILD/cairo/src/libcairo.a" "$DEPS/lib/libcairo.a"
    mkdir -p "$DEPS/include/cairo"
    cp "$CAIRO_DIR"/src/cairo.h "$CAIRO_DIR"/src/cairo-deprecated.h "$CAIRO_DIR"/src/cairo-ft.h \
        "$CAIRO_DIR"/src/cairo-version.h "$DEPS/include/cairo/"
    cp "$BUILD/cairo/src/cairo-features.h" "$DEPS/include/cairo/"
fi

echo "== Android cairo/pixman/freetype dependencies ready under $DEPS =="
echo "-I$DEPS/include -I$DEPS/include/freetype2 -I$DEPS/include/pixman-1"
echo "-L$DEPS/lib -lcairo -lfreetype -lpixman-1"
