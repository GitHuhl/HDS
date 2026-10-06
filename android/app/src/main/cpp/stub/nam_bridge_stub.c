// Stand-in for nam_bridge.cpp, for the Android build ONLY -- see android/README.md.
//
// backend.c's #include "nam_bridge.h" (a quote-include, src/backend.c:71) always resolves to the
// REAL header first (quote-includes search the including file's own directory before any -I
// path, so the stub/lo/lo.h header-shadowing trick used for liblo does not apply here) -- and
// that's fine, nam_bridge.h is pure `extern "C"` declarations with no C++/Eigen types exposed, so
// backend.c including it directly is already completely safe. What's missing on Android is the
// IMPLEMENTATION: nam_bridge.cpp needs a real C++ toolchain plus NeuralAmpModelerCore/Eigen
// cross-compiled for Android, none of which this build does. This file provides the three
// functions instead, entirely within their own already-documented contract (see nam_bridge.h's
// own comments): hds_nam_create() returning NULL is explicitly "any failure, including an
// unreadable file," hds_nam_process() with a NULL model is explicitly "a silent no-op," and
// hds_nam_destroy() with NULL is explicitly "a safe no-op" -- so every .nam model load on this
// build simply reports as if the file couldn't be read, which is real, in-spec behavior, not a
// fabricated success. FX_TYPE_NAM itself is otherwise unaffected (it's still selectable, still
// run_fx_nam() gets called -- it just never has a model to process, the same as a real build the
// moment before any .nam file is loaded). Every OTHER FX type (Gate, EQ3, Limiter, HT76,
// Fairchild670, PeakMax, LA2A, the multi-slope filter, DeEsser, HardTune, FDNVerb, DBX120X,
// BroadcastDelay) is pure C, part of fx_chain.c, and fully functional here -- NAM is the only one
// that needed a C++ bridge at all.
//
// To actually support NAM on Android later: cross-compile NeuralAmpModelerCore + Eigen for
// android-arm64 with the NDK's clang++, compile the real nam_bridge.cpp against them, and drop
// this file instead of linking it -- no changes needed to backend.c, fx_chain.c, or nam_bridge.h.
#include <stddef.h>

void *hds_nam_create(const char *path, double sample_rate, int max_block_size) {
    (void)path; (void)sample_rate; (void)max_block_size;
    return NULL;
}

void hds_nam_process(void *model, float *inout, int num_frames) {
    (void)model; (void)inout; (void)num_frames;
}

void hds_nam_destroy(void *model) {
    (void)model;
}
