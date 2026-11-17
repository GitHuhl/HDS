// fx_effects.h -- internal interface between fx_chain.c (slot lifecycle + process_fx_chain()) and
// the per-family effect files in src/fx/. Not part of the public FX API (that's fx_chain.h).
#ifndef HDS_FX_EFFECTS_H
#define HDS_FX_EFFECTS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../fx_chain.h"

// --- Small DSP building blocks shared by effects in different files ---

// One 2-pole TPT SVF stage. a1/a2/a3/k are precomputed once per sample (same cutoff/resonance for
// every cascaded stage) and passed in rather than recomputed per stage.
static inline float fx_multifilter_2pole_stage(float input, float k, float a1, float a2, float a3, int filter_type, float *low_state, float *band_state) {
    float v3 = input - *low_state;
    float v1 = a1 * (*band_state) + a2 * v3; // bandpass output
    float v2 = *low_state + a2 * (*band_state) + a3 * v3; // lowpass output
    *band_state = 2.0f * v1 - *band_state;
    *low_state = 2.0f * v2 - *low_state;
    // Same defense-in-depth hard state clamp as run_svf_filter's own -- see its comment.
    *band_state = fmaxf(-1.0e6f, fminf(1.0e6f, *band_state));
    *low_state = fmaxf(-1.0e6f, fminf(1.0e6f, *low_state));
    float high = input - k * v1 - v2;
    switch (filter_type) {
        case 1:  return v2;        // LPF
        case 2:  return high;      // HPF
        case 3:  return v1;        // BPF
        default: return v2 + high; // Notch (filter_type == 4)
    }
}

// A plain, correctly-normalized one-pole highpass (DC-blocker style: highpass = input - lowpass).
// See FX_TYPE_DBX160's own comment in fx_types.h for the bug this replaces in the first draft.
static inline float dbx160_hpf(float x, float *lp, float alpha) {
    *lp += alpha * (x - *lp);
    return x - *lp;
}

// --- Per-effect entry points, called once per sample by process_fx_chain() ---
void run_fx_ensemble(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_creamcoat(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_density3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_stereochorus(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_stereodoubler(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_tapedelay2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_pointyguitar(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_guitarconditioner(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_cabs2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_nam(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_gate(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_eq3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_ssl4k(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_limiter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_fetcomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_dbx160(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_fairkid67(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r, float sc_l, float sc_r);
void run_fx_peakmax(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_multifilter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_optocomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_deesser(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_hardtune(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_fdnverb(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_broadcastdelay(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
void run_fx_dbx120x(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r);
float fx_gate_gr_db(const void *state);
float fx_limiter_gr_db(const void *state);
float fx_fetcomp_gr_db(const void *state);
float fx_fairkid67_gr_db(const void *state);
float fx_peakmax_gr_db(const void *state);
float fx_optocomp_gr_db(const void *state);
float fx_deesser_gr_db(const void *state);
float fx_dbx160_gr_db(const void *state);
float fx_broadcastdelay_dump_avail_sec(const void *state);
#if HDS_NAM_ENABLED
void fx_nam_free(void *state);
#endif

#endif // HDS_FX_EFFECTS_H
