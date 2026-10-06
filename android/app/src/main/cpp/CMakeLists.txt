cmake_minimum_required(VERSION 3.22.1)
project(hds_native C)

# hds_android.c #includes backend.c directly (this project's established single-translation-unit-
# per-binary convention -- see clap_plugin.c/hds_fx_plugin.c for the same pattern on other
# platforms), which itself #includes fx_chain.c -- so hds_android.c alone compiles the whole
# engine. nam_bridge_stub.c stands in for the real nam_bridge.cpp (see its own header comment) --
# backend.c's #include "nam_bridge.h" is a quote-include, so it always resolves to the real header
# (pure declarations, safe to include as-is) regardless of any -I path; what's missing on Android
# is nam_bridge.cpp's IMPLEMENTATION (needs a cross-built C++/Eigen toolchain this build doesn't
# have), which this plain-C stub provides instead, compiled as an ordinary second source file, not
# a header-shadowing trick.
set(HDS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)

add_library(hds_native SHARED
    ${HDS_SRC_DIR}/hds_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stub/nam_bridge_stub.c
)

# stub/lo/lo.h stands in for the real liblo for now -- backend.c #includes <lo/lo.h> directly for
# its OSC control protocol (34 handler functions, ~70 call sites), none of which hds_android.c
# ever actually invokes (nothing here starts a server or registers a handler), so this header
# gives backend.c the real type/function shapes to compile and link cleanly against, as inert
# no-ops, with zero changes to backend.c itself. See stub/lo/lo.h's own header comment for exactly
# why and how to swap in the real, cross-built liblo later (a separate, contained step once basic
# audio+MIDI is verified on a real phone) -- listed first so it's found ahead of anything else
# that might also provide <lo/lo.h>.
target_include_directories(hds_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${HDS_SRC_DIR}
)

find_library(log-lib log)
find_library(aaudio-lib aaudio)

target_link_libraries(hds_native PRIVATE ${log-lib} ${aaudio-lib})
