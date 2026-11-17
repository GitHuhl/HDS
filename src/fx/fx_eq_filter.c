// fx_eq_filter.c -- EQ and filters: EQ3, SSL 4000, MultiFilter, De-esser.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx/fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_EQ3: the Program's own post-filter 3-band EQ, ported to an FX slot --- see
// FX_TYPE_EQ3's own comment above for the param map. Reuses compute_eq_band_coeffs()/
// run_eq_biquad() (see their own comments, far above) completely unchanged -- this is genuinely
// the SAME EQ, not a re-derivation of it, so it can never behave differently from the Program EQ
// version for the same Freq/Gain/Q settings.
typedef struct {
    float z1[3][2], z2[3][2]; // [band][channel] -- band 0=Low Shelf, 1=Mid Peak, 2=High Shelf
} FxEq3State;

// 0.0-1.0 fraction -> 20Hz-20kHz, log-spaced (equal knob distance = equal perceived pitch change,
// same musical mapping a hardware EQ's own frequency knob uses) -- needed because, unlike the
// Program EQ's own EqBandParams.freq field (a real Hz value directly, see that struct's own
// comment in fx_types.h), this FX slot's Freq params are two more generic P1-P10 dials like every
// other effect's params, which always transmit as a plain 0.0-1.0 fraction -- decoding that
// fraction into Hz is this function's job, matching how every other unit conversion in this file
// (dB, ms, Q, ...) already happens backend-side.
static inline float fx_eq3_fraction_to_hz(float fraction) {
    fraction = fmaxf(0.0f, fminf(1.0f, fraction));
    return 20.0f * powf(1000.0f, fraction); // 20 * 1000^0 = 20Hz .. 20 * 1000^1 = 20000Hz
}

void run_fx_eq3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxEq3State *st = (FxEq3State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxEq3State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    EqBandParams bands[3];
    bands[0].freq = fx_eq3_fraction_to_hz(params->param[0]);
    bands[0].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 36.0f;
    bands[0].q = 0.707f; // ignored for shelf bands -- see compute_eq_band_coeffs()'s own comment
    bands[1].freq = fx_eq3_fraction_to_hz(params->param[2]);
    bands[1].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[3])) * 36.0f;
    bands[1].q = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 9.9f;
    bands[2].freq = fx_eq3_fraction_to_hz(params->param[5]);
    bands[2].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[6])) * 36.0f;
    bands[2].q = 0.707f; // ignored for shelf bands

    float sl = *l, sr = *r;
    for (int band = 0; band < 3; band++) {
        EqBiquadCoeffs c = compute_eq_band_coeffs(&bands[band], band);
        sl = run_eq_biquad(sl, &c, &st->z1[band][0], &st->z2[band][0]);
        sr = run_eq_biquad(sr, &c, &st->z1[band][1], &st->z2[band][1]);
    }
    *l = sl;
    *r = sr;
}

// --- FX_TYPE_SSL4K -- see its own comment in fx_types.h for the param map and design summary. ---

// 0.0-1.0 fraction -> [lo_hz, hi_hz], log-spaced -- same musical-taper reasoning as
// fx_eq3_fraction_to_hz() above, just with each band's own frequency range instead of one fixed
// 20-20000Hz span (the console's own bands each only sweep a fraction of the full audio range).
static inline float ssl4k_fraction_to_hz(float fraction, float lo_hz, float hi_hz) {
    fraction = fmaxf(0.0f, fminf(1.0f, fraction));
    return lo_hz * powf(hi_hz / lo_hz, fraction);
}

typedef struct {
    float z1[4][2], z2[4][2]; // [band][channel] -- 0=LF Shelf, 1=LMF Bell, 2=HMF Bell, 3=HF Shelf
    EqBiquadCoeffs hpf_coeffs;
    float hpf_z1[2], hpf_z2[2];
} FxSsl4kState;

// 2-pole (12dB/oct) Butterworth high-pass, Q=0.7071 (maximally flat passband, the standard HPF
// alignment) -- same RBJ-cookbook HPF form already used for FairKid67's own output-transformer
// coloration (fk67_xfmr2_init's own hpf_coeffs, just above), reused here as a plain audible HPF
// rather than a fixed sub-audio corner.
static void ssl4k_compute_hpf_coeffs(EqBiquadCoeffs *c, float freq_hz) {
    const float fs = (float)SAMPLE_RATE;
    const float w0 = 2.0f * (float)M_PI * fmaxf(20.0f, fminf(2000.0f, freq_hz)) / fs;
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float cosw0 = cosf(w0);
    const float a0 = 1.0f + alpha;
    c->b0 = ((1.0f + cosw0) * 0.5f) / a0;
    c->b1 = -(1.0f + cosw0) / a0;
    c->b2 = c->b0;
    c->a1 = (-2.0f * cosw0) / a0;
    c->a2 = (1.0f - alpha) / a0;
}

void run_fx_ssl4k(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxSsl4kState *st = (FxSsl4kState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxSsl4kState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    // Console (E/G) -- see fx_types.h's own comment: this selects the LMF/HMF top-of-range
    // (G sweeps noticeably higher on both, per its own reputation as the "extended range,
    // brighter/smoother" revision) and how aggressively gain narrows a bell band's own bandwidth
    // and drives the saturation stage (E: punchier/more aggressive on both counts).
    const bool console_g = (int)(fmaxf(0.0f, fminf(1.0f, params->param[10])) * 1.0f + 0.5f) != 0;
    const float lmf_hi_hz = console_g ? 7000.0f : 2500.0f;
    const float hmf_hi_hz = console_g ? 12000.0f : 7000.0f;
    const float q_narrow_rate = console_g ? 0.02f : 0.05f;
    const float sat_drive_rate = console_g ? 0.015f : 0.03f;

    EqBandParams bands[4];
    bands[0].freq = ssl4k_fraction_to_hz(params->param[1], 30.0f, 450.0f);
    bands[0].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 32.0f;
    bands[0].q = 0.707f; // ignored for shelf bands -- see compute_eq_band_coeffs()'s own comment

    bands[1].freq = ssl4k_fraction_to_hz(params->param[3], 200.0f, lmf_hi_hz);
    bands[1].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 32.0f;
    const float lmf_q_base = 0.3f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 2.7f;

    bands[2].freq = ssl4k_fraction_to_hz(params->param[6], 600.0f, hmf_hi_hz);
    bands[2].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[7])) * 32.0f;
    const float hmf_q_base = 0.7f; // no dedicated Q knob for this band (matches the real console's own HMF, which is fixed-bandwidth on several revisions) -- still narrows/widens dynamically with gain below

    bands[3].freq = ssl4k_fraction_to_hz(params->param[8], 1500.0f, 16000.0f);
    bands[3].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[9])) * 32.0f;
    bands[3].q = 0.707f; // ignored for shelf bands

    // Proportional/"dynamic" bandwidth: as a bell band's own |gain| rises, its Q narrows --
    // a broad boost/cut at low gain, a surgical one at high gain, the "musical", self-scaling
    // bandwidth behavior this whole console style is known for (as opposed to a plain fixed-Q
    // parametric EQ, where the same Q sounds proportionally wider at low gain and narrower at high
    // gain purely as a side effect of the gain itself, with no compensation). This own curve/rate
    // is an original choice here, not measured or reverse-engineered from any specific unit.
    bands[1].q = fmaxf(0.15f, lmf_q_base / (1.0f + q_narrow_rate * fabsf(bands[1].gain_db)));
    bands[2].q = fmaxf(0.15f, hmf_q_base / (1.0f + q_narrow_rate * fabsf(bands[2].gain_db)));

    ssl4k_compute_hpf_coeffs(&st->hpf_coeffs, ssl4k_fraction_to_hz(params->param[0], 20.0f, 500.0f));

    // bands[] index -> compute_eq_band_coeffs()'s own band_index (0=Low Shelf, 1=Mid Peak,
    // 2=High Shelf): LF is a Low Shelf, LMF/HMF are both Mid Peak (bell) bands, HF is a High Shelf.
    static const int SSL4K_EQ_BAND_INDEX[4] = {0, 1, 1, 2};
    float sl = run_eq_biquad(*l, &st->hpf_coeffs, &st->hpf_z1[0], &st->hpf_z2[0]);
    float sr = run_eq_biquad(*r, &st->hpf_coeffs, &st->hpf_z1[1], &st->hpf_z2[1]);
    for (int band = 0; band < 4; band++) {
        EqBiquadCoeffs c = compute_eq_band_coeffs(&bands[band], SSL4K_EQ_BAND_INDEX[band]);
        sl = run_eq_biquad(sl, &c, &st->z1[band][0], &st->z2[band][0]);
        sr = run_eq_biquad(sr, &c, &st->z1[band][1], &st->z2[band][1]);
    }

    // Always-on, subtle saturation standing in for the real console's own transformer/op-amp
    // stage -- a normalized tanh soft-clip (drive in, 1/tanh(drive) back out keeps it unity-gain
    // at DC/low level, so this never quietly pads the signal), whose drive scales with how hard
    // this slot's own bands are being pushed (the sum of every band's own |gain_db|, on top of a
    // small fixed baseline so even a flat/0dB setting still imparts a touch of console character).
    // Purely an original stand-in for a general, widely-described trait of this console style, not
    // a measured/reverse-engineered circuit model of any specific unit.
    const float total_boost_db = fabsf(bands[0].gain_db) + fabsf(bands[1].gain_db) + fabsf(bands[2].gain_db) + fabsf(bands[3].gain_db);
    const float drive = 1.015f + sat_drive_rate * total_boost_db;
    const float inv_tanh_drive = 1.0f / tanhf(drive);
    *l = tanhf(drive * sl) * inv_tanh_drive;
    *r = tanhf(drive * sr) * inv_tanh_drive;
}

// --- FX_TYPE_MULTIFILTER: see its own #define comment above for the full param map and design
// rationale. Building blocks below are a standalone extraction of run_svf_filter's own per-stage
// TPT (topology-preserving-transform) math -- same formulas, same stability floor on resonance --
// generalized to run any number of cascaded stages instead of that function's own hardcoded 1-or-2,
// so this effect can never change ProgramState.filters[5]'s own existing behavior (a completely
// separate function and state struct, sharing only the underlying technique).
#define FX_FILTER_MAX_2POLE_STAGES 8 // 8 stages * 12dB/oct = 96dB/oct, this effect's own steepest Slope option


// One 1-pole TPT stage, for a Slope choice's odd 6dB remainder (Lowpass/Highpass only -- see this
// effect's own #define comment for why Bandpass/Notch never call this). Same trapezoidal-integrator
// family as the 2-pole stage above (and as this file's own fetcomp_highpass/FairKid67 DC blockers),
// just first-order: g_one is tan(pi*fc/fs), the same prewarped coefficient the 2-pole stages use.
static inline float fx_multifilter_1pole_stage(float input, float g_one, int filter_type, float *z) {
    float G = g_one / (1.0f + g_one);
    float v = (input - *z) * G;
    float lp = v + *z;
    *z = lp + v;
    return (filter_type == 2) ? (input - lp) : lp; // HPF: input minus lowpass; LPF: the lowpass itself
}

typedef struct {
    float low_state[FX_FILTER_MAX_2POLE_STAGES][2];  // [stage][channel]
    float band_state[FX_FILTER_MAX_2POLE_STAGES][2];
    float onepole_z[2]; // [channel] -- only used when the Slope choice needs the odd-6dB extra stage
} FxMultiFilterState;

void run_fx_multifilter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxMultiFilterState *st = (FxMultiFilterState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxMultiFilterState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    int filter_type = 1 + (int)(fmaxf(0.0f, fminf(1.0f, params->param[0])) * 3.0f + 0.5f); // 0..3 -> 1..4 (LP/HP/BP/Notch)
    if (filter_type < 1) filter_type = 1; else if (filter_type > 4) filter_type = 4;

    float cutoff_hz = fmaxf(40.0f, fminf(18000.0f, fx_eq3_fraction_to_hz(params->param[1])));
    float res_q = fmaxf(0.0f, fminf(100.0f, params->param[2]));

    static const int slope_db_table[12] = { 6, 12, 18, 24, 30, 36, 42, 48, 60, 72, 84, 96 };
    int slope_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[3])) * 11.0f + 0.5f);
    if (slope_idx < 0) slope_idx = 0; else if (slope_idx > 11) slope_idx = 11;
    int total_poles = slope_db_table[slope_idx] / 6;
    int two_pole_stages = total_poles / 2;
    bool has_extra_onepole = ((total_poles % 2) == 1) && (filter_type == 1 || filter_type == 2);
    // Bandpass/Notch have no natural 1-pole equivalent (see this effect's own #define comment) --
    // an odd Slope choice just rounds down for those two types, and 6dB itself (which would
    // otherwise resolve to zero stages, i.e. a silent full bypass) clamps up to the smallest
    // meaningful bandpass/notch this topology can express: one 2-pole stage, 12dB/oct.
    if ((filter_type == 3 || filter_type == 4) && two_pole_stages < 1) two_pole_stages = 1;
    if (two_pole_stages > FX_FILTER_MAX_2POLE_STAGES) two_pole_stages = FX_FILTER_MAX_2POLE_STAGES;

    const float k = fmaxf(0.05f, 2.0f * (1.0f - res_q / 100.0f));
    const float g = tanf((float)M_PI * cutoff_hz / (float)SAMPLE_RATE);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    float *chan[2] = { l, r };
    for (int ch = 0; ch < 2; ch++) {
        float sig = *chan[ch];
        for (int s = 0; s < two_pole_stages; s++) {
            sig = fx_multifilter_2pole_stage(sig, k, a1, a2, a3, filter_type, &st->low_state[s][ch], &st->band_state[s][ch]);
        }
        if (has_extra_onepole) {
            sig = fx_multifilter_1pole_stage(sig, g, filter_type, &st->onepole_z[ch]);
        }
        *chan[ch] = sig;
    }
}

// --- FX_TYPE_DEESSER: see its own #define comment above for the full design. ---
typedef struct {
    float low_state[2];  // [channel] -- the crossover highpass's own 2-pole TPT state
    float band_state[2]; // [channel]
    float gain_smoothed; // starts at 1.0 -- see FxGateState's own comment on why calloc's 0.0 would be wrong
    bool initialized;
    float last_gr_db;
} FxDeEsserState;

void run_fx_deesser(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDeEsserState *st = (FxDeEsserState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDeEsserState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain_smoothed = 1.0f; st->initialized = true; }

    // 1kHz * 16^fraction -> 1kHz..16kHz, log-mapped -- same style as fx_eq3_fraction_to_hz's own
    // 20Hz-20kHz range, just narrowed to where sibilance actually lives.
    const float freq_hz = 1000.0f * powf(16.0f, fmaxf(0.0f, fminf(1.0f, params->param[0])));
    const float threshold_db = -40.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 40.0f; // -40..0dB
    const float ratio = 1.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 19.0f;          // 1..20
    const float attack_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[3])) * 19.9f;      // 0.1..20ms
    const float release_ms = 5.0f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 295.0f;    // 5..300ms
    const bool listen = params->param[5] >= 0.5f;

    // Crossover: one fixed-Q (Butterworth, no resonance -- this is a clean split, not a tone-shaping
    // filter) 2-pole TPT highpass reused directly from FX_TYPE_MULTIFILTER, giving the high
    // (sibilance) band; the low band is `input - high`, exact by construction (see this effect's
    // own #define comment for why that needs no separately-designed lowpass at all).
    const float k = 1.41421356f; // Q = 1/k = 0.7071, Butterworth
    const float g = tanf((float)M_PI * freq_hz / (float)SAMPLE_RATE);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    const float high_l = fx_multifilter_2pole_stage(*l, k, a1, a2, a3, 2 /* HPF */, &st->low_state[0], &st->band_state[0]);
    const float high_r = fx_multifilter_2pole_stage(*r, k, a1, a2, a3, 2 /* HPF */, &st->low_state[1], &st->band_state[1]);
    const float low_l = *l - high_l;
    const float low_r = *r - high_r;

    // Same hard-knee power-law gain computer as FX_TYPE_OPTOCOMP, applied to the high band's own
    // level only (self-detecting) -- but this file's own native exponential attack/release
    // convention (FX_TYPE_GATE/FX_TYPE_LIMITER's own `1-exp(-1/(0.001*ms*SAMPLE_RATE))`), not
    // 4a2a's specific calibration, since nothing here is emulating that plugin.
    const float threshold_gain = powf(10.0f, threshold_db / 20.0f);
    const float comp_slope = 1.0f - 1.0f / ratio;
    const float level = fmaxf(fabsf(high_l), fabsf(high_r));
    const float gain_target = fminf(1.0f, powf(level / threshold_gain, -comp_slope));
    const float rate_ms = (gain_target < st->gain_smoothed) ? attack_ms : release_ms;
    const float rate = 1.0f - expf(-1.0f / (0.001f * rate_ms * (float)SAMPLE_RATE));
    st->gain_smoothed += (gain_target - st->gain_smoothed) * rate;

    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain_smoothed)));

    if (listen) {
        // Solo the RAW (pre-gain-reduction) high band -- for dialing in Frequency by ear, so
        // Threshold/Ratio can then be trusted to actually be listening to the sibilance.
        *l = high_l;
        *r = high_r;
    } else {
        *l = low_l + high_l * st->gain_smoothed;
        *r = low_r + high_r * st->gain_smoothed;
    }
}

// Meter readback for process_fx_chain() (keeps FxDeEsserState private to this file).
float fx_deesser_gr_db(const void *state) { return ((const FxDeEsserState *)state)->last_gr_db; }
