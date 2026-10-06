cmake_minimum_required(VERSION 3.22.1)
project(hds_native C)

# hds_android.c #includes backend.c directly (this project's established single-translation-unit-
# per-binary convention -- see clap_plugin.c/hds_fx_plugin.c for the same pattern on other
# platforms), which itself #includes fx_chain.c -- so this one add_library call compiles the whole
# engine, nothing else to list here.
set(HDS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)

add_library(hds_native SHARED
    ${HDS_SRC_DIR}/hds_android.c
)

target_include_directories(hds_native PRIVATE ${HDS_SRC_DIR})

find_library(log-lib log)
find_library(aaudio-lib aaudio)

# TODO (blocked on NDK access, see android/README.md): backend.c #includes <lo/lo.h> directly
# (its own OSC control protocol -- see backend.c's header comment), so this will fail to LINK,
# not compile, until liblo is cross-built for android-arm64 and added here, e.g.:
#   add_subdirectory(${HDS_SRC_DIR}/../third_party/liblo-android ${CMAKE_BINARY_DIR}/liblo)
#   target_link_libraries(hds_native PRIVATE lo)
# The existing third_party/win64-deps/src/liblo-0.31 tree is NOT reused as-is here: it's already
# configured (config.status/Makefile) for the mingw-w64 cross-build from task #111, so pointing
# this at it directly would link a Windows-configured liblo into an Android .so. A clean checkout
# of that same liblo source, configured fresh for aarch64-linux-android via the NDK's own CMake
# toolchain file, is a contained, separate step -- deliberately not guessed at here blind.
target_link_libraries(hds_native PRIVATE ${log-lib} ${aaudio-lib})
