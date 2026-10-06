cmake_minimum_required(VERSION 3.22.1)
project(hds_native C ASM)

# hds_android.c #includes backend.c directly (this project's established single-translation-unit-
# per-binary convention -- see clap_plugin.c/hds_fx_plugin.c for the same pattern on other
# platforms), which itself #includes fx_chain.c -- so hds_android.c alone compiles the whole
# engine (now including the native GUI too -- see that file's own HDS_ANDROID_GUI_ENABLED comment
# and hds_android_gui.c's own header comment -- via the SAME single #include chain, not a
# separate source). nam_bridge_stub.c stands in for the real nam_bridge.cpp (see its own header
# comment) -- backend.c's #include "nam_bridge.h" is a quote-include, so it always resolves to
# the real header (pure declarations, safe to include as-is) regardless of any -I path; what's
# missing on Android is nam_bridge.cpp's IMPLEMENTATION (needs a cross-built C++/Eigen toolchain
# this build doesn't have), which this plain-C stub provides instead, compiled as an ordinary
# second source file, not a header-shadowing trick. embed_font.S .incbin's the two DejaVu Sans
# TTFs (third_party/dejavu-fonts/) straight into this .so's own .rodata -- see that file's own
# header comment for why HDS_FONT_REGULAR_PATH/HDS_FONT_BOLD_PATH (below) exist.
set(HDS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
set(HDS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)
# Cross-built by tools/build_android_cairo_deps.sh (run once, outside this CMake build -- see
# that script's own header comment) into third_party/android-deps/ at the repo root, same
# relationship the Makefile's own WIN_DEPS has to tools/build_windows_deps.sh for the Windows
# CLAP targets. NOT produced by this CMakeLists.txt itself: no mingw-equivalent meson/ninja
# toolchain setup exists in a plain CMake Android Gradle build.
set(HDS_ANDROID_DEPS ${HDS_REPO_ROOT}/third_party/android-deps)

add_library(hds_native SHARED
    ${HDS_SRC_DIR}/hds_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stub/nam_bridge_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/embed_font.S
)

target_compile_definitions(hds_native PRIVATE
    HDS_ANDROID_GUI_ENABLED=1
    HDS_FONT_REGULAR_PATH="${HDS_REPO_ROOT}/third_party/dejavu-fonts/DejaVuSans.ttf"
    HDS_FONT_BOLD_PATH="${HDS_REPO_ROOT}/third_party/dejavu-fonts/DejaVuSans-Bold.ttf"
)

# stub/lo/lo.h stands in for the real liblo for now -- backend.c #includes <lo/lo.h> directly for
# its OSC control protocol (34 handler functions, ~70 call sites), none of which hds_android.c
# ever actually invokes (nothing here starts a server or registers a handler), so this header
# gives backend.c the real type/function shapes to compile and link cleanly against, as inert
# no-ops, with zero changes to backend.c itself. See stub/lo/lo.h's own header comment for exactly
# why and how to swap in the real, cross-built liblo later (a separate, contained step once basic
# audio+MIDI is verified on a real phone) -- listed first so it's found ahead of anything else
# that might also provide <lo/lo.h>.
#
# HDS_ANDROID_DEPS/include AND .../include/cairo BOTH: see tools/build_android_manual.sh's own
# identical comment -- this codebase's own draw files are inconsistent about `#include <cairo.h>`
# vs `#include <cairo/cairo.h>`, and unlike the native desktop build, nothing here falls back to
# a host-system cairo install.
target_include_directories(hds_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${HDS_SRC_DIR}
    ${HDS_ANDROID_DEPS}/include
    ${HDS_ANDROID_DEPS}/include/cairo
    ${HDS_ANDROID_DEPS}/include/freetype2
    ${HDS_ANDROID_DEPS}/include/pixman-1
)

find_library(log-lib log)
find_library(aaudio-lib aaudio)
# ANativeWindow_fromSurface/_lock/_unlockAndPost/_release/_setBuffersGeometry (the GUI's own
# direct-buffer render pipeline -- see hds_android_gui.c's own header comment) live here, not in
# aaudio-lib/log-lib.
find_library(android-lib android)
find_library(cairo-lib cairo PATHS ${HDS_ANDROID_DEPS}/lib NO_DEFAULT_PATH)
find_library(freetype-lib freetype PATHS ${HDS_ANDROID_DEPS}/lib NO_DEFAULT_PATH)
find_library(pixman-lib pixman-1 PATHS ${HDS_ANDROID_DEPS}/lib NO_DEFAULT_PATH)

target_link_libraries(hds_native PRIVATE
    ${cairo-lib} ${freetype-lib} ${pixman-lib}
    ${log-lib} ${aaudio-lib} ${android-lib}
)
