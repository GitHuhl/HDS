// fx_nam.c -- Neural Amp Modeler slot: model/cab-IR loading and per-sample processing.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HDS_NAM_ENABLED
#include "../nam_bridge.h"
#endif

#if HDS_NAM_ENABLED
// --- FX_TYPE_NAM: Neural Amp Modeler (NeuralAmpModelerCore, MIT license) --- see FX_TYPE_NAM's
// own comment above for the param map and why this needed a small C++ bridge (nam_bridge.h/.cpp)
// at all. model_l/model_r are opaque nam::DSP* handles from that bridge -- genuinely two separate
// model instances loaded from the SAME file, never one instance shared between channels, because
// each carries real internal state (WaveNet dilated-conv history, LSTM hidden state) that must not
// leak between L and R the same way this file's other stereo effects (Cabs2's dual iirPosition/
// iirAngle arrays above, PointyGuitar's own per-channel bands) never share state either.
//
// Cab IR convolution is plain C, not more C++: NeuralAmpModelerCore's own wav.h only exposes a raw
// decode helper (load_wav_ir), not a ready convolution engine, and this file's own
// seq_audio_load_wav_from_disk() just above already decodes WAVs -- reusing it here avoids a
// second file-format dependency for the one new thing actually needed: the convolution loop
// itself, a plain per-sample ring-buffer dot product like every other hand-rolled effect in this
// file.
typedef struct {
    void *model_l, *model_r;                 // bridge-owned nam::DSP*, NULL until a model is loaded
    char loaded_model_path[SAMPLE_PATH_LEN];   // "" if none -- compared against fx.nam_model_path
    bool model_load_failed;                    // sticky until the path changes again -- avoids
                                                // retrying a known-bad file every single block

    float *ir_l, *ir_r;                        // coefficients, read-only once loaded -- NULL if no
                                                // cab loaded; ir_r == ir_l for a mono IR (safe to
                                                // alias since neither is ever written after load)
    int ir_taps;
    // Per-channel convolution HISTORY -- unlike ir_l/ir_r above, these are never aliased even for
    // a mono IR: each ring holds that channel's own past INPUT samples, which differ between L and
    // R regardless of whether they share one set of coefficients. Sharing these the way ir_l/ir_r
    // can be shared would have L's and R's samples overwriting each other in the same ring.
    // Each is allocated at 2*ir_taps, not ir_taps -- a doubled/mirrored ring buffer (every write
    // stores the same sample at both `pos` and `pos+ir_taps`) so fx_nam_convolve_tap()'s own hot
    // loop can walk a single contiguous range instead of wrapping the index every tap -- see that
    // function's own comment for why the wrapping version was a real, measured performance problem.
    float *ir_hist_l, *ir_hist_r;
    int ir_hist_pos_l, ir_hist_pos_r;
    char loaded_ir_path[SAMPLE_PATH_LEN];
    bool ir_load_failed;
} FxNamState;

// A direct time-domain FIR: real guitar cab IRs decay within tens of milliseconds, so this cap
// (~85ms at 48kHz) comfortably covers them while bounding the O(taps) per-sample cost this
// approach has -- see poll_fx_nam_requests()'s own comment on why this is loaded on the main
// thread rather than the realtime audio thread despite that cost.
#define FX_NAM_IR_MAX_TAPS 4096

static void fx_nam_free_ir(FxNamState *st) {
    if (st->ir_r && st->ir_r != st->ir_l) free(st->ir_r);
    free(st->ir_l);
    st->ir_l = NULL; st->ir_r = NULL;
    // ir_hist_l/ir_hist_r are always two independent allocations (see the struct's own comment on
    // why they're never aliased even when ir_l/ir_r are) -- both are simply freed, no alias check.
    free(st->ir_hist_l); free(st->ir_hist_r);
    st->ir_hist_l = NULL; st->ir_hist_r = NULL;
    st->ir_taps = 0;
    st->ir_hist_pos_l = 0; st->ir_hist_pos_r = 0;
}

// Releases everything an FxNamState owns OTHER than the struct itself. Called from
// sync_fx_engine_types() right before ITS OWN generic free(fx_engine[s].state) whenever a slot
// changes away from FX_TYPE_NAM -- that generic free() is a plain libc free(), correct for every
// other effect's plain-old-data state struct, but model_l/model_r point at real C++ objects
// (owning Eigen matrices and weight vectors of their own) that a bare free() would leak rather than
// destroy. This function's job is exactly that: run the proper destructor (via the bridge) on
// anything that needs one, so the generic free() right after it only has to reclaim the outer
// struct's own flat memory, same as any other effect's state.
static void fx_nam_free_state(FxNamState *st) {
    hds_nam_destroy(st->model_l);
    hds_nam_destroy(st->model_r);
    fx_nam_free_ir(st);
}

// fx_nam_reload_model/fx_nam_reload_ir/poll_fx_nam_requests -- genuinely shared now, not just
// backend.c's own concern: originally lived in hds_standalone.c (moved there under the mistaken
// assumption that ONLY the standalone binary would ever poll for a path change), which meant a
// CLAP plugin's own FX_TYPE_NAM slots could load a model file at all -- neither hds.clap
// (clap_plugin.c never called poll_fx_nam_requests()) nor a plugin like HDS FX could ever have
// working NAM support, a real, confirmed gap, not a hypothetical one. Moved back here, right next
// to run_fx_nam()/FxNamState/fx_nam_free_state above (their only real home), so every translation
// unit that runs the FX bus -- backend.c (hds_standalone.c/clap_plugin.c) AND hds_fx_plugin.c
// directly -- gets working NAM for free by calling poll_fx_nam_requests() once per block, the same
// way they already all share process_fx_chain()/sync_fx_engine_types(). Takes a plain
// `FXSlotParams fx[FX_SLOT_COUNT]` array directly (not a SynthSharedControls*, which only backend.c
// has) -- the same convention sync_fx_engine_types()/process_fx_chain() already use, precisely so
// hds_fx_plugin.c's own flat pool (no wrapping struct at all) can call it identically.
static void fx_nam_reload_model(FxNamState *st, const char *path) {
    hds_nam_destroy(st->model_l); st->model_l = NULL;
    hds_nam_destroy(st->model_r); st->model_r = NULL;
    snprintf(st->loaded_model_path, SAMPLE_PATH_LEN, "%s", path);
    st->model_load_failed = false;
    if (path[0] == '\0') return; // cleared -- leave both NULL, run_fx_nam() then passes dry
    // num_frames=1 is this call's own real usage pattern (see run_fx_nam()), but Reset() still
    // wants a genuine max-block-size hint for any internal buffer sizing the model does up front.
    st->model_l = hds_nam_create(path, (double)SAMPLE_RATE, 1);
    st->model_r = hds_nam_create(path, (double)SAMPLE_RATE, 1);
    if (!st->model_l || !st->model_r) {
        fprintf(stderr, "[nam] failed to load model '%s'\n", path);
        hds_nam_destroy(st->model_l); st->model_l = NULL;
        hds_nam_destroy(st->model_r); st->model_r = NULL;
        st->model_load_failed = true;
    }
}

static void fx_nam_reload_ir(FxNamState *st, const char *path) {
    fx_nam_free_ir(st);
    snprintf(st->loaded_ir_path, SAMPLE_PATH_LEN, "%s", path);
    st->ir_load_failed = false;
    if (path[0] == '\0') return; // cleared -- leave NULL, run_fx_nam() then skips the cab stage

    int frames = 0, channels = 0, sample_rate = 0;
    float *pcm = seq_audio_load_wav_from_disk(path, &frames, &channels, &sample_rate);
    if (!pcm || frames <= 0) { st->ir_load_failed = true; free(pcm); return; }

    int taps = frames;
    bool truncated = false;
    if (taps > FX_NAM_IR_MAX_TAPS) { taps = FX_NAM_IR_MAX_TAPS; truncated = true; }

    st->ir_l = malloc(sizeof(float) * (size_t)taps);
    bool stereo = channels >= 2;
    st->ir_r = stereo ? malloc(sizeof(float) * (size_t)taps) : st->ir_l;
    if (!st->ir_l || (stereo && !st->ir_r)) {
        fx_nam_free_ir(st); st->ir_load_failed = true; free(pcm); return;
    }
    for (int i = 0; i < taps; i++) {
        st->ir_l[i] = pcm[(size_t)i * channels];
        if (stereo) st->ir_r[i] = pcm[(size_t)i * channels + 1];
    }
    free(pcm);

    st->ir_taps = taps;
    // Always two independent rings, even for a mono IR -- see the struct's own comment on why
    // ir_hist_l/ir_hist_r must never alias the way ir_l/ir_r (read-only coefficients) safely can.
    // 2*taps, not taps -- see the struct's own comment on the doubled-buffer trick.
    st->ir_hist_l = calloc((size_t)taps * 2, sizeof(float));
    st->ir_hist_r = calloc((size_t)taps * 2, sizeof(float));
    st->ir_hist_pos_l = 0; st->ir_hist_pos_r = 0;
    if (!st->ir_hist_l || !st->ir_hist_r) { fx_nam_free_ir(st); st->ir_load_failed = true; return; }

    if (truncated) fprintf(stderr, "[nam] cab IR '%s' truncated to %d taps (was %d)\n", path, taps, frames);
    if (sample_rate != 0 && sample_rate != SAMPLE_RATE) {
        fprintf(stderr, "[nam] cab IR '%s' is %d Hz, engine runs at %d Hz -- loaded as-is, uncorrected\n", path, sample_rate, SAMPLE_RATE);
    }
}

// Same "once-per-block change detection" shape as poll_seq_audio_load_requests() (hds_standalone.c,
// standalone-only -- Sequencer audio lanes are a real, separate, already-documented CLAP gap, this
// one just got fixed). Notices when an FX slot's nam_model_path/nam_ir_path differs from what that
// slot's own state already has loaded, and (re)loads it. Runs the load SYNCHRONOUSLY on whatever
// thread calls it (this plugin's own audio/main thread, same one process() itself runs on for both
// CLAP plugins) rather than spawning a loader thread: a .nam model file is a small, bounded
// JSON+weights blob (nothing like a whole song-length audio recording), and a cab IR is capped at
// FX_NAM_IR_MAX_TAPS frames -- both comfortably finish within one block's worth of stall.
//
// Only meaningful once a slot's own engine state actually exists as an FxNamState -- gated on
// active_type == FX_TYPE_NAM, which sync_fx_engine_types() (the caller's own responsibility to run
// first, same block -- see e.g. hds_fx_plugin_process()'s own call order) guarantees is current for
// any type change already applied by the time this runs; a slot switched to FX_TYPE_NAM within the
// SAME block this runs simply picks up its load on the next block instead, same one-block grace
// every other live-knob check in a per-block poll like this already has.
void poll_fx_nam_requests(const FXSlotParams *fx, FXSlotEngine *fx_engine, int slot_count) {
    for (int s = 0; s < slot_count; s++) {
        if (fx_engine[s].active_type != FX_TYPE_NAM || !fx_engine[s].state) continue;
        FxNamState *st = (FxNamState *)fx_engine[s].state;
        const char *want_model = fx[s].nam_model_path;
        if (strncmp(want_model, st->loaded_model_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_model(st, want_model);
        }
        const char *want_ir = fx[s].nam_ir_path;
        if (strncmp(want_ir, st->loaded_ir_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_ir(st, want_ir);
        }
    }
}

// One tap of the direct-form FIR: writes this sample into the ring, then walks it newest-to-oldest
// against the IR (coeffs[0] is the IR's first/earliest tap, multiplying the sample just written --
// standard y[n] = sum_k h[k]*x[n-k] convolution). `hist` is a DOUBLED ring (2*taps floats, see the
// struct's own comment): this sample is written at both `pos` and `pos+taps`, which lets the walk
// below read the contiguous range hist[pos+1 .. pos+taps] (via the `base[-i]` indexing) instead of
// wrapping the array index every single tap. That wrap-check used to sit inside this loop's own
// body (`idx--; if (idx < 0) idx = taps - 1;`) -- a data-dependent branch on EVERY one of up to
// 4096 taps, run once per output sample, which the "optimize" attribute below needs GONE before
// it can vectorize this loop at all.
// __attribute__((optimize("O3","fast-math"))): this whole file otherwise builds at the project's
// normal -O2 with strict IEEE FP (see Makefile's own CFLAGS) -- this ONE hot loop gets a
// function-scoped exception instead of a global compiler-flag change, so nothing else in this
// codebase's numerical behavior is affected. "fast-math" lets the compiler reassociate the
// summation (required for it to auto-vectorize a reduction at all -- floating-point addition
// isn't associative under strict IEEE rules, so a compiler can't reorder it otherwise); the
// result can differ from a strict left-to-right sum by less than 1ULP per sample, which is
// standard, expected, and inaudible for an FIR convolution -- every real-world audio convolution
// engine relies on exactly this reordering. `restrict` on hist/coeffs (they never alias each
// other) is what actually lets the vectorizer act on that permission. Measured, not assumed:
// baseline (plain -O2, no restrict/fast-math) cost ~0.28x of one core's realtime budget per
// channel at the full 4096-tap cap; this combination measures ~0.08x -- both figures for ONE
// channel of ONE FX_TYPE_NAM slot's cab stage, so a stereo instance (let alone several tracks
// each running one) is the actual multiplier that made this show up as system-wide CPU distress.
__attribute__((optimize("O3","fast-math")))
static inline float fx_nam_convolve_tap(float *restrict hist, int taps, int *pos, const float *restrict coeffs, float in) {
    int p = *pos;
    hist[p] = in;
    hist[p + taps] = in;
    const float *restrict base = hist + p + taps;
    float acc = 0.0f;
    for (int i = 0; i < taps; i++) {
        acc += coeffs[i] * base[-i];
    }
    p += 1; if (p >= taps) p = 0;
    *pos = p;
    return acc;
}

void run_fx_nam(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxNamState *st = (FxNamState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxNamState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    // param[0]/param[1] arrive as the same 0.0-1.0 fraction every other effect's param[] uses
    // (see FX_SLOT_PARAM_COUNT's own comment and e.g. Cabs2/Density3 above) -- this effect's own
    // meaning for that fraction is a +/-20dB trim, linearly mapped (0.5 = 0dB, matching the
    // frontend dial's own default-at-midpoint convention for a centered knob).
    float input_gain_db = (params->param[0] - 0.5f) * 40.0f;
    float output_gain_db = (params->param[1] - 0.5f) * 40.0f;
    float input_gain = powf(10.0f, input_gain_db / 20.0f);
    float output_gain = powf(10.0f, output_gain_db / 20.0f);
    bool cab_enabled = params->param[2] != 0.0f;

    float sl = *l * input_gain, sr = *r * input_gain;
    if (st->model_l && st->model_r) {
        hds_nam_process(st->model_l, &sl, 1);
        hds_nam_process(st->model_r, &sr, 1);
    } // no model loaded (or load failed) -- passes the (gain-adjusted) dry signal straight through

    if (cab_enabled && st->ir_l && st->ir_r) {
        sl = fx_nam_convolve_tap(st->ir_hist_l, st->ir_taps, &st->ir_hist_pos_l, st->ir_l, sl);
        sr = fx_nam_convolve_tap(st->ir_hist_r, st->ir_taps, &st->ir_hist_pos_r, st->ir_r, sr);
    }

    *l = sl * output_gain;
    *r = sr * output_gain;
}
// Frees a NAM slot's state, C++ objects included (process_fx_chain() calls this when a slot's type changes).
void fx_nam_free(void *state) { fx_nam_free_state((FxNamState *)state); }
#endif // HDS_NAM_ENABLED
