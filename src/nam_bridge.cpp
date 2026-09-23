// The only C++ translation unit in HDS. NeuralAmpModelerCore (vendored under
// third_party/NeuralAmpModelerCore, MIT license) is a C++/Eigen library with no C API of its own,
// so this file is the entire cost of using it from an otherwise-all-C engine: nam_bridge.h declares
// a handful of extern "C" functions, this file implements them against the real C++ API, and
// backend.c calls them exactly like any other C function. Compiled separately (see the Makefile's
// NAM_CXX_OBJS rule) and linked into synth_engine/hds.clap via $(CXX) as the final link driver.
//
// NAM_SAMPLE_FLOAT before any NAM header: NeuralAmpModelerCore defaults NAM_SAMPLE to `double`,
// but every signal in backend.c's own audio path is `float` -- defining this makes NAM_SAMPLE
// `float` too, matching HDS's own pipeline exactly with no per-sample cast at the boundary. This is
// NAM's own officially-supported real-time build configuration, not a HDS-specific hack.
#define NAM_SAMPLE_FLOAT

#include "nam_bridge.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include <filesystem>

extern "C" void *hds_nam_create(const char *path, double sample_rate, int max_block_size) {
    if (!path || path[0] == '\0') return nullptr;
    try {
        std::unique_ptr<nam::DSP> dsp = nam::get_dsp(std::filesystem::path(path));
        if (!dsp) return nullptr;
        dsp->Reset(sample_rate, max_block_size);
        return dsp.release();
    } catch (...) {
        // Bad path, corrupt JSON, unrecognized architecture -- any of these throws inside
        // nam::get_dsp(); the only sane response at a C boundary is "failed to load," never a
        // C++ exception escaping into backend.c's plain C.
        return nullptr;
    }
}

extern "C" void hds_nam_process(void *model, float *inout, int num_frames) {
    if (!model || !inout || num_frames <= 0) return;
    nam::DSP *dsp = static_cast<nam::DSP *>(model);
    NAM_SAMPLE *chans[1] = { inout };
    dsp->process(chans, chans, num_frames);
}

extern "C" void hds_nam_destroy(void *model) {
    delete static_cast<nam::DSP *>(model);
}
