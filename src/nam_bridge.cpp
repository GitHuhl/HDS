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
//
// Now REDUNDANT with (but harmless alongside) the Makefile's own -DNAM_SAMPLE_FLOAT on
// $(NAM_CXXFLAGS) -- kept here so this file still builds correctly on its own (e.g. this session's
// own ad hoc test compiles) even without that flag. It used to be the ONLY place this was defined,
// which was itself a real, confirmed bug: every other NAM_SRCS file (container.cpp,
// wavenet/model.cpp, conv1d.cpp, ...) compiled with the default NAM_SAMPLE=double, so their real,
// already-compiled process()/etc. functions all took `double**` -- while this file, and only this
// file, called through those same (virtual) functions believing they took `float**`. That link
// succeeded (pointers are the same width regardless), so every real call quietly read/wrote 8 bytes
// through a pointer backed by only a 4-byte float local on the CALLER's stack (ours, e.g.
// hds_nam_process()'s own `chans`/the caller's per-sample float in fx_chain.c's run_fx_nam()) --
// silent stack corruption on every single sample, confirmed with ASan as a stack-buffer-overflow
// inside WaveNet::_set_condition_array() reading 8 bytes out of a real 4-byte float. See the
// Makefile's own comment on NAM_CXXFLAGS for the full story; the real fix is there, not here.
// #ifndef guard: the Makefile already passes this on the command line for every NAM_SRCS file
// (this one included) -- the guard just avoids a harmless but noisy "macro redefined" warning
// while still leaving this file safe to compile standalone (outside the Makefile) on its own.
#ifndef NAM_SAMPLE_FLOAT
  #define NAM_SAMPLE_FLOAT
#endif

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
        // hds_nam_process() below hands the model exactly ONE float in / ONE float out per
        // channel object (this bridge always runs two separate mono model instances, one per
        // stereo channel -- see FxNamState's own comment in fx_chain.c) -- it can only ever be
        // safe for a model whose own NumInputChannels()/NumOutputChannels() are genuinely 1/1.
        // Most real guitar-amp .nam files are (WaveNet's own a2_fast.cpp asserts this at load
        // time for its architecture already), but get_dsp() also accepts OTHER real, valid
        // configurations this bridge was never built for -- a raw multi-channel .wav loaded as a
        // "Linear" model (get_dsp.cpp's own out_channels-from-the-file path) is a real, confirmed
        // case: a stereo IR-as-model produces out_channels==2, and the fixed 1-element channel
        // array hds_nam_process() builds below would then read a garbage second pointer and write
        // through it -- a real stack-smashing crash a live DAW test hit. Refuse anything that
        // isn't 1-in/1-out here, at the one place that already knows both numbers, exactly like
        // any other unsupported/corrupt file: a clean "failed to load," never a wild pointer.
        if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1) {
            return nullptr;
        }
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
    // Safe ONLY because hds_nam_create() above already refused any model that isn't genuinely
    // 1 input channel / 1 output channel -- never assume that from this function alone.
    NAM_SAMPLE *chans[1] = { inout };
    dsp->process(chans, chans, num_frames);
}

extern "C" void hds_nam_destroy(void *model) {
    delete static_cast<nam::DSP *>(model);
}
