// Thin C API over NeuralAmpModelerCore (third_party/NeuralAmpModelerCore), a C++/Eigen neural amp
// modeling library with no C API of its own. This header (and nam_bridge.cpp, its only
// implementation) is the ENTIRE reason any C++ exists in HDS -- everything else, including how a
// slot calls into this bridge, stays plain C. See FX_TYPE_NAM's own comment in backend.c for the
// effect this backs, and the Makefile's NAM_CXX_OBJS rule for how this gets compiled and linked
// into an otherwise-C binary.
//
// Each handle returned by hds_nam_create() is one loaded model instance with its own internal
// state (conv/LSTM history, etc). Never share one handle between two audio channels that must stay
// independent -- create a separate handle per channel instead (see FxNamState in backend.c).
#ifndef HDS_NAM_BRIDGE_H
#define HDS_NAM_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Loads a .nam model file and resets it for the given sample rate / max block size (HDS always
// calls this with num_frames=1 per hds_nam_process() call -- see run_fx_nam()'s own comment on why
// this whole FX bus is dispatched one sample at a time, not in blocks -- but Reset() still wants a
// real max-block-size hint for any internal buffer sizing). Returns NULL on any failure (bad path,
// unreadable file, unrecognized architecture) -- never throws past this boundary.
void *hds_nam_create(const char *path, double sample_rate, int max_block_size);

// In-place mono process: inout[0..num_frames) in, same buffer holds the model's output on return.
// A NULL model is a silent no-op (caller's buffer is left untouched), matching every other
// "not loaded yet" case in this file's own FX slots.
void hds_nam_process(void *model, float *inout, int num_frames);

// Releases a handle from hds_nam_create(). NULL is a safe no-op. This is the ONLY correct way to
// free one of these -- it owns real C++ objects (Eigen matrices, weight vectors) that a plain libc
// free() would leak rather than release; see fx_nam_free_state()'s own comment in backend.c.
void hds_nam_destroy(void *model);

#ifdef __cplusplus
}
#endif

#endif // HDS_NAM_BRIDGE_H
