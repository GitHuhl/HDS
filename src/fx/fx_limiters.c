// fx_limiters.c -- Gate and limiters: Gate, brick-wall Limiter, PeakMax (L2).
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx/fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_GATE: a simple, zero-latency noise gate --- see FX_TYPE_GATE's own comment above for
// the param map. No lookahead buffer anywhere in this function -- the ONLY thing smoothed is the
// gain itself (an envelope follower), never the audio, so nothing here ever delays the signal
// path. Both channels' peak feeds ONE shared envelope/gain value (never independent per-channel
// gains), the same reasoning FX_TYPE_LIMITER/the master bus limiter below both already use: a
// signal panned other than dead-center would otherwise open/close each channel at a slightly
// different moment, audibly skewing the stereo image every time the gate moved at all.
typedef struct {
    float gain; // 0.0 (fully closed, at Range) .. 1.0 (fully open) -- starts at 1.0 (calloc gives
                 // 0.0, which would wrongly gate the very first block's audio before any real
                 // envelope has been established), fixed up on first use below.
    bool initialized;
    // Schmitt-trigger open/close state (see this section's own header comment on Hysteresis) --
    // which of the two thresholds (Threshold itself, or Threshold-Hysteresis) governs the NEXT
    // transition depends on which state the gate is already in, so this can't be derived fresh
    // from the instantaneous peak alone the way the old single-threshold version could.
    bool gate_open;
    // Hold countdown, in samples (see this section's own header comment on Hold) -- counts down
    // only while gate_open is true AND the signal has actually dropped below the close threshold;
    // refilled back to the full Hold time on every sample that's still above the close threshold,
    // so a sustained signal never "uses up" hold time it might need for its own next brief dip.
    float hold_samples_left;
    float last_gr_db; // -20*log10(gain), clamped >=0 -- this slot's current gain reduction in dB,
                        // read back by process_fx_chain() for the /hds/status/fx_gr/<slot> meter.
} FxGateState;

void run_fx_gate(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxGateState *st = (FxGateState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxGateState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain = 1.0f; st->gate_open = true; st->initialized = true; } // open by default, not closed

    float threshold_db = -60.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 60.0f;
    float attack_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 19.9f;
    float release_ms = 5.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 495.0f;
    float range_db = -80.0f * fmaxf(0.0f, fminf(1.0f, params->param[3]));
    float hysteresis_db = fmaxf(0.0f, fminf(1.0f, params->param[4])) * 12.0f;
    float hold_ms = fmaxf(0.0f, fminf(1.0f, params->param[5])) * 500.0f;

    // Schmitt trigger: a CLOSED gate only reopens once the signal reaches the (higher) Threshold
    // itself; an OPEN gate only starts to close once the signal drops below the (lower) Threshold
    // - Hysteresis point. A signal hovering anywhere between those two points is stable in
    // whichever state the gate is already in -- the standard fix for a signal sitting right at a
    // single fixed threshold chattering open/closed on every small fluctuation.
    float open_threshold_lin = powf(10.0f, threshold_db / 20.0f);
    float close_threshold_lin = powf(10.0f, (threshold_db - hysteresis_db) / 20.0f);
    float range_lin = powf(10.0f, range_db / 20.0f);
    float peak = fmaxf(fabsf(*l), fabsf(*r));
    float hold_samples = hold_ms * 0.001f * (float)SAMPLE_RATE;

    if (!st->gate_open) {
        if (peak >= open_threshold_lin) { st->gate_open = true; st->hold_samples_left = hold_samples; }
    } else if (peak >= close_threshold_lin) {
        st->hold_samples_left = hold_samples; // still legitimately open -- keep Hold fully topped up
    } else if (st->hold_samples_left > 0.0f) {
        st->hold_samples_left -= 1.0f; // below the close point, but still within its own Hold window
    } else {
        st->gate_open = false; // Hold expired with the signal still below the close point
    }
    float target_gain = st->gate_open ? 1.0f : range_lin;

    // Same "instant attack, exponential release toward target" shape as the master bus limiter
    // below, just with both directions smoothed here (a gate's own attack must still be a short
    // ramp, not a literal single-sample jump, or opening/closing would click audibly -- "zero-
    // latency" means no signal delay, not an instant, click-prone gain step).
    float rate_ms = (target_gain > st->gain) ? attack_ms : release_ms;
    float rate = 1.0f - expf(-1.0f / (0.001f * rate_ms * (float)SAMPLE_RATE));
    st->gain += (target_gain - st->gain) * rate;

    *l *= st->gain;
    *r *= st->gain;
    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain)));
}

// Meter readback for process_fx_chain() (keeps FxGateState private to this file).
float fx_gate_gr_db(const void *state) { return ((const FxGateState *)state)->last_gr_db; }

// --- FX_TYPE_LIMITER: a digital brick-wall peak limiter --- see FX_TYPE_LIMITER's own comment
// above for the param map and why this is an original implementation of the same technique the
// master bus safety limiter below already uses, not a literal port of anything. Both channels
// always share one gain value, same reasoning as FX_TYPE_GATE above -- the whole point of a
// limiter is protecting levels without being audible as anything OTHER than "it got a little
// quieter for a moment," and an independent per-channel gain would smear the stereo image on
// every single gain-reduction event, not just an occasional one.
typedef struct {
    float gain; // starts at 1.0 -- see FxGateState's own comment on why calloc's 0.0 default would
                 // be wrong here too (a fully-closed limiter on the very first block).
    bool initialized;
    float last_gr_db; // -20*log10(gain), clamped >=0 -- see FxGateState.last_gr_db's own comment.
} FxLimiterState;

void run_fx_limiter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxLimiterState *st = (FxLimiterState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxLimiterState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain = 1.0f; st->initialized = true; }

    float ceiling_db = -20.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 20.0f;
    float input_gain_db = fmaxf(0.0f, fminf(1.0f, params->param[1])) * 24.0f;
    float release_ms = 10.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 490.0f;

    float ceiling = powf(10.0f, ceiling_db / 20.0f);
    float input_gain = powf(10.0f, input_gain_db / 20.0f);
    float sl = *l * input_gain, sr = *r * input_gain;

    // Identical shape to the master bus limiter further down (see its own comment for the full
    // rationale) -- instant attack ("this sample can never exceed ceiling" is only true if the
    // gain reduction is computed and applied to the SAME sample it's measuring, not a future one),
    // exponential release so the reduction isn't audible as pumping on an isolated transient.
    float peak = fmaxf(fabsf(sl), fabsf(sr));
    if (peak > ceiling || st->gain != 1.0f) {
        float needed_gain = (peak > ceiling) ? (ceiling / peak) : 1.0f;
        if (needed_gain < st->gain) {
            st->gain = needed_gain; // attack: instant
        } else {
            float release_rate = 1.0f - expf(-1.0f / (0.001f * release_ms * (float)SAMPLE_RATE));
            st->gain += (needed_gain - st->gain) * release_rate;
        }
        sl *= st->gain;
        sr *= st->gain;
    }
    *l = sl;
    *r = sr;
    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain)));
}

// Meter readback for process_fx_chain() (keeps FxLimiterState private to this file).
float fx_limiter_gr_db(const void *state) { return ((const FxLimiterState *)state)->last_gr_db; }

// --- FX_TYPE_PEAKMAX: ported from JClones_L2 (github.com/JClones/JSFXClones, MIT license) --- see
// FX_TYPE_PEAKMAX's own comment above for the param map. Unlike every earlier effect in this file,
// this one is LOOKAHEAD: it delays its own audio by a fixed number of samples (~1.3ms, see
// PEAKMAX_MAX_LOOKAHEAD's own comment) so gain reduction can already account for peaks that haven't
// reached the output yet -- true brickwall behavior, not just fast-attack limiting. That delay is
// real, unavoidable added latency on whichever slot uses this effect; nothing else in this file adds
// latency, and this engine has no plugin-delay-compensation mechanism of its own for mixing a
// delayed source's bus against an undelayed one live (a DAW host CAN compensate a CLAP instance for
// its own reported latency, but this codebase doesn't report any yet -- see the comment on
// clap_plugin.c's own process() call site, a real follow-up if this matters in practice).
//
// KEPT FAITHFUL: the "boost input by -Threshold dB, brickwall at a fixed 0dBFS-equivalent point,
// trim by Ceiling on the way out" gain-staging shape; the lookahead peak detector (an EXACT minimum
// of the raw per-sample gain-needed value over the upcoming lookahead window -- see
// PeakMaxMinWindow's own comment for why a different exact algorithm than the original's own is used
// here); the two-stage triangular-window smoothing after the release envelope (two cascaded boxcar
// moving averages, whose convolution IS a triangular window); the manual release's simple one-pole
// toward the lookahead minimum; the ENTIRE auto-release cascade (slow/fast/dynamic 3-stage detector,
// the piecewise-linear release-time-vs-"dynamic factor" curve, the 3 fixed calibration time
// constants 0.281s/0.057s/0.00054s) -- this multi-stage adaptive release is the original's own
// signature "smart" behavior and is ported verbatim, no simplification; and the full IDR-style
// dither/noise-shaping stage (the type-1 and type-2 PRNGs, the 9-tap per-sample-rate-bucket FIR
// noise-shaping filter and its complete coefficient table, the truncate-to-N-bits-with-error-
// feedback structure) -- included in full even though it's arguably a mastering/bit-depth-delivery
// feature orthogonal to this engine's always-float internal signal path, because it's still a
// well-defined, self-contained coloration effect on a float signal regardless of what's downstream
// (the same reasoning any bit-crusher effect already relies on), and the reference material specifies
// it completely -- there was no principled partial version to build instead.
//
// DELIBERATELY CHANGED:
//   1. No JSFX/EEL2 runtime -- allocate()'s dynamic memory pool becomes fixed-size C arrays sized to
//      PEAKMAX_MAX_LOOKAHEAD (see its own comment), same "opt out of dynamic allocation in the
//      real-time path" posture this whole codebase already uses everywhere else.
//   2. DelayLineMin's own bespoke branching-factor-8 min-tree is replaced with a standard monotonic
//      deque sliding-window minimum (PeakMaxMinWindow) -- a DIFFERENT exact algorithm for the
//      IDENTICAL well-defined quantity ("the minimum of the last N pushed values"), not an
//      approximation of it, the same class of substitution as this file's own reuse of one RBJ
//      biquad implementation for multiple filter roles elsewhere. Simpler to verify correct than
//      porting the original's own multi-level padded-array tree structure line for line.
//   3. JSFX's implicit `srate`-keyed recomputation (@init's own `sample_rate != srate` guard) becomes
//      a one-time computation on first slot activation, matching fk67_init/fetcomp_init's own
//      "compute derived constants once, not per sample" convention -- this engine's SAMPLE_RATE is a
//      fixed compile-time constant, so there is no runtime sample-rate change to react to at all.
//   4. Dither type-1's 32-bit LCG is implemented with plain uint32_t wraparound arithmetic rather
//      than EEL2's own double-precision-value-reinterpreted-as-a-bitwise-integer semantics -- the
//      same modular arithmetic either way (multiplication and addition mod 2^32 don't care whether
//      the ring's elements are labeled as signed or unsigned representatives), so this produces the
//      identical sequence of values, just expressed in C's own native integer types.
//   5. No internal Mix param and no sidechain input -- like the original, this works on its own
//      stereo signal only; the slot's own generic Mix knob already covers dry/wet if ever wanted.

// ~1.3ms at any sample rate this engine plausibly runs at (64 samples at 48kHz, the original's own
// `floor((0.0014*srate)/64+0.6)*64` formula, computed once at slot activation from the fixed
// SAMPLE_RATE -- see fx_peakmax_activate()). 256 gives a large safety margin (up to ~384kHz) for a
// value that's actually always 64 today, matching this codebase's own general "bound generously,
// clamp defensively" posture elsewhere (e.g. OSC_DISPATCH_TABLE_SIZE).
#define PEAKMAX_MAX_LOOKAHEAD 256
#define PEAKMAX_MAX_MA_SIZE (PEAKMAX_MAX_LOOKAHEAD / 2 + 1)
#define PEAKMAX_DITHER_HISTORY 9

// One-pole coefficient/apply helpers, calibrated the same way the original does: time_ref_20 is
// "seconds to settle to -20dB (10%) of a step's full excursion" -- ref20_mult bakes srate and that
// -20dB reference point together once so every later call is just one expf(). Not reused from
// anything else in this file since every other effect's own one-pole helper uses a DIFFERENT
// reference convention (e.g. run_fx_gate's attack/release use -60dB/e-folding style time constants)
// -- porting THIS specific convention exactly is what keeps the auto-release cascade's own 3
// calibration constants (0.281s/0.057s/0.00054s) meaning what the original intends.
static inline float peakmax_filter1_g(float time_ref_20, float ref20_mult) {
    return expf(-1.0f / (time_ref_20 * ref20_mult));
}
static inline float peakmax_filter1(float s, float g, float x) {
    return x + g * (s - x);
}

// Exact sliding-window minimum of the last `window_size` pushed values, via a standard monotonic
// deque (values kept increasing front-to-back; the front is always the current window's minimum) --
// see this effect's own top comment (point 2) for why this replaces the original's own min-tree
// structure. Capacity-bounded ring buffer of (value, push-index) pairs; at most `window_size`
// entries can ever be live in a monotonic deque over a window of that size, so PEAKMAX_MAX_LOOKAHEAD
// capacity is always enough.
typedef struct {
    float val[PEAKMAX_MAX_LOOKAHEAD];
    int64_t idx[PEAKMAX_MAX_LOOKAHEAD];
    int head, count;
    int64_t sample_pos;
    int window_size;
} PeakMaxMinWindow;

static float peakmax_minwindow_push(PeakMaxMinWindow *w, float x) {
    int64_t pos = w->sample_pos++;
    while (w->count > 0 && w->val[(w->head + w->count - 1) % PEAKMAX_MAX_LOOKAHEAD] >= x) {
        w->count--; // strictly-increasing invariant: anything >= the new value can never win again
    }
    int back = (w->head + w->count) % PEAKMAX_MAX_LOOKAHEAD;
    w->val[back] = x;
    w->idx[back] = pos;
    w->count++;
    while (w->count > 0 && w->idx[w->head] <= pos - w->window_size) {
        w->head = (w->head + 1) % PEAKMAX_MAX_LOOKAHEAD; // expire anything that's aged out of the window
        w->count--;
    }
    return w->val[w->head];
}

// Pre-seeds the window with `window_size` copies of default_value, matching DelayLineMin_reset's
// own "already full" starting state (so the very first real sample's getMin() reads default_value,
// not an empty/undefined result) -- identical repeated values simply collapse to one deque entry.
static void peakmax_minwindow_reset(PeakMaxMinWindow *w, int window_size, float default_value) {
    w->window_size = window_size;
    w->head = 0; w->count = 0; w->sample_pos = 0;
    for (int i = 0; i < window_size; i++) peakmax_minwindow_push(w, default_value);
}

// Plain boxcar moving average via a ring buffer + running sum -- two cascaded instances (see
// run_fx_peakmax) give a triangular smoothing window, exactly as the original's own two
// MovingAverageFilter instances do.
typedef struct {
    float buf[PEAKMAX_MAX_MA_SIZE];
    int head;
    float sum;
    int size;
} PeakMaxMovingAvg;

static void peakmax_ma_reset(PeakMaxMovingAvg *ma, int size, float default_value) {
    ma->size = size;
    ma->head = 0;
    for (int i = 0; i < size; i++) ma->buf[i] = default_value;
    ma->sum = default_value * (float)size;
}

static float peakmax_ma_push(PeakMaxMovingAvg *ma, float x) {
    ma->sum -= ma->buf[ma->head];
    ma->buf[ma->head] = x;
    ma->head++;
    if (ma->head >= ma->size) ma->head = 0;
    ma->sum += x;
    return ma->sum / (float)ma->size;
}

// Plain circular buffer delaying the actual audio signal by `size` samples, so gain reduction
// (computed from the lookahead window above) reaches each sample before -- not after -- it's
// applied. One instance per channel.
typedef struct {
    float buf[PEAKMAX_MAX_LOOKAHEAD];
    int head;
    int size;
} PeakMaxDelayLine;

static void peakmax_delay_reset(PeakMaxDelayLine *d, int size) {
    d->size = size;
    d->head = 0;
    for (int i = 0; i < size; i++) d->buf[i] = 0.0f;
}

static float peakmax_delay_push(PeakMaxDelayLine *d, float x) {
    float y = d->buf[d->head];
    d->buf[d->head] = x;
    d->head++;
    if (d->head >= d->size) d->head = 0;
    return y;
}

// 9-tap noise-shaping FIR filter coefficients, 4 shaping modes (None/Moderate/Normal/Ultra) x 4
// sample-rate buckets (<=44100/<=48000/<=88200/>88200 Hz) -- ported verbatim from the original's own
// table (this engine's fixed SAMPLE_RATE=48000 always selects the second bucket, but every bucket is
// kept so this table matches its source exactly rather than being pruned to "what's reachable
// today"). IR = PEAKMAX_DITHER_IR + 9*(shaping_type + 4*sample_rate_index).
static const float PEAKMAX_DITHER_IR[144] = {
    // bucket 0 (<=44100 Hz): None, Moderate, Normal, Ultra (9 taps each)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    -0.054383024f, -0.20654947f, -0.3822527f, -0.39219186f, -0.36627461f, -0.49556099f, -0.29294844f, 0.61599216f, 1.3177642f,
    -0.070082023f, -0.29561589f, -0.5451639f, -0.5487125f, -0.55320585f, -0.75886502f, -0.25751337f, 1.2202875f, 1.9089777f,
    -0.10739672f, -0.43569984f, -0.72708491f, -0.66456244f, -0.73261389f, -1.0359325f, -0.16065249f, 1.8239374f, 2.3525222f,
    // bucket 1 (<=48000 Hz -- the only bucket this engine's fixed SAMPLE_RATE ever selects)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    -0.1002716f, -0.29584958f, -0.40982519f, -0.38606261f, -0.44715518f, -0.52985251f, -0.10806594f, 0.82622085f, 1.3482654f,
    -0.11617057f, -0.41429373f, -0.69819106f, -0.84679904f, -0.99510928f, -0.8137319f, 0.33485791f, 1.8264681f, 2.072573f,
    -0.15380712f, -0.5469209f, -0.91673806f, -1.1670967f, -1.4463654f, -1.0427872f, 0.84138339f, 2.7803198f, 2.624423f,
    // bucket 2 (<=88200 Hz)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.088072546f, 0.36459012f, 0.88040251f, 1.5499839f, 2.1484938f, 2.4151576f, 2.2070139f, 1.5982079f,
    -0.00390274f, 0.1073065f, 0.63257259f, 1.8682127f, 3.5575584f, 5.014459f, 5.3379144f, 4.3814994f, 2.5787734f,
    -0.021631265f, 0.1594025f, 1.0213817f, 2.9718205f, 5.5339018f, 7.4154889f, 7.5202313f, 5.7418745f, 3.1130463f,
    // bucket 3 (>88200 Hz)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.15717715f, 0.54336716f, 1.1581471f, 1.8591082f, 2.4045727f, 2.5688185f, 2.2673639f, 1.6110487f,
    -0.011899905f, 0.12947208f, 0.63891136f, 1.7815208f, 3.4063984f, 4.8728995f, 5.2856435f, 4.3677134f, 2.6052934f,
    -0.021628401f, 0.15915684f, 1.0202237f, 2.9688732f, 5.5289454f, 7.4093702f, 7.5145789f, 5.7382228f, 3.1116338f,
};

typedef struct {
    uint32_t type1_state;
    float type2_a, type2_b;
    float history_left[PEAKMAX_DITHER_HISTORY];
    float history_right[PEAKMAX_DITHER_HISTORY];
    int history_index;
    int dither_type;       // 0=None, 1=Type1, 2=Type2
    int shaping_type;      // 0=None, 1=Moderate, 2=Normal, 3=Ultra
    int sample_rate_index; // 0..3, from SAMPLE_RATE
    float scaling;         // 1 << (24 - bits)
} PeakMaxDither;

static void peakmax_dither_reset(PeakMaxDither *d) {
    d->type1_state = 0;
    d->type2_a = 0.0f;
    d->type2_b = 0.25f;
    for (int i = 0; i < PEAKMAX_DITHER_HISTORY; i++) { d->history_left[i] = 0.0f; d->history_right[i] = 0.0f; }
    d->history_index = 0;
}

static void peakmax_dither_set_mode(PeakMaxDither *d, int bits, int dither_type, int shaping_type) {
    d->dither_type = dither_type;
    d->shaping_type = shaping_type;
    d->scaling = (float)(1 << (24 - bits));
}

// Standard 32-bit LCG, kept in plain unsigned arithmetic (wraps mod 2^32 exactly like the original's
// own explicit `&4294967295`) -- see this effect's own top comment (point 4) for why the signed
// reinterpretation only needs to happen where the result is actually used below.
static int32_t peakmax_dither_type1_rand(PeakMaxDither *d) {
    d->type1_state = d->type1_state * 10368889u + 1u;
    return (int32_t)d->type1_state; // two's-complement reinterpretation -- this codebase already assumes it elsewhere
}

static float peakmax_dither_process_channel(PeakMaxDither *d, float input, float rand_type1, float rand_type2, float *history) {
    const float *ir = PEAKMAX_DITHER_IR + 9 * (d->shaping_type + 4 * d->sample_rate_index);
    float filtered_error = 0.0f;
    int k = 0, i = d->history_index;
    for (int n = 0; n < PEAKMAX_DITHER_HISTORY; n++) {
        filtered_error += history[i] * ir[k];
        k++;
        i++;
        if (i >= PEAKMAX_DITHER_HISTORY) i -= PEAKMAX_DITHER_HISTORY;
    }

    input = fmaxf(-1.0f, fminf(1.0f, input));

    float x1 = input * 8388608.0f; // 0x800000 -- to 24-bit integer scale
    float x2 = x1 + rand_type2 * d->scaling;
    float x3 = x2 + (rand_type1 + 0.5f) * d->scaling + filtered_error;

    float out_fp = floorf(x3);
    out_fp = fmaxf(-8388608.0f, fminf(8388607.0f, out_fp));
    if (d->scaling != 1.0f) out_fp = floorf(out_fp / d->scaling) * d->scaling;

    float error = x2 - out_fp;
    history[d->history_index] = error;

    return out_fp / 8388608.0f;
}

static void peakmax_dither_process(PeakMaxDither *d, float *l, float *r) {
    float rand_type1_left = 0.0f, rand_type1_right = 0.0f;
    float rand_type2_left = 0.0f, rand_type2_right = 0.0f;

    if (d->dither_type == 1) {
        int32_t a = peakmax_dither_type1_rand(d);
        int32_t b = peakmax_dither_type1_rand(d);
        rand_type1_left = (float)((double)((int64_t)a + (int64_t)b) / 4294967296.0);
        rand_type1_right = (float)((double)((int64_t)a - (int64_t)b) / 4294967296.0);
    } else if (d->dither_type == 2) {
        d->type2_a -= 0.16666667f;
        if (d->type2_a < -0.16666667f) d->type2_a = 0.16666667f;
        d->type2_b = -d->type2_b;
        rand_type2_left = d->type2_a + d->type2_b;
        rand_type2_right = d->type2_a - d->type2_b;
    }

    float out_l = peakmax_dither_process_channel(d, *l, rand_type1_left, rand_type2_left, d->history_left);
    float out_r = peakmax_dither_process_channel(d, *r, rand_type1_right, rand_type2_right, d->history_right);

    d->history_index++;
    if (d->history_index >= PEAKMAX_DITHER_HISTORY) d->history_index -= PEAKMAX_DITHER_HISTORY;

    *l = out_l;
    *r = out_r;
}

typedef struct {
    int lookahead_size;   // computed once, from SAMPLE_RATE -- see fx_peakmax_activate()
    float ref20_mult;     // srate / -log(0.1), for peakmax_filter1_g()

    PeakMaxMinWindow lookahead_min;
    PeakMaxMovingAvg smooth1, smooth2;
    PeakMaxDelayLine delay_left, delay_right;
    PeakMaxDither dither;

    float slow_gr_s, fast_gr_s, dynamic_gr_s;
    float dynamic_release_g;

    // Fixed auto-release calibration constants (0.281s/0.057s/0.00054s), computed once.
    float dynamic_g1, dynamic_g2, dynamic_g3;

    float last_gr_db;
    bool initialized;
} FxPeakMaxState;

static void fx_peakmax_activate(FxPeakMaxState *st) {
    // Same formula the original computes on every samplerate change (@init's own `sample_rate !=
    // srate` guard) -- this engine's SAMPLE_RATE is a fixed compile-time constant, so there's only
    // ever one value to compute, once, here.
    int lookahead = (int)(floorf((0.0014f * (float)SAMPLE_RATE) / 64.0f + 0.6f) * 64.0f);
    if (lookahead < 64) lookahead = 64; // never less than one 64-sample chunk
    if (lookahead > PEAKMAX_MAX_LOOKAHEAD) lookahead = PEAKMAX_MAX_LOOKAHEAD; // defensive clamp -- see PEAKMAX_MAX_LOOKAHEAD's own comment
    st->lookahead_size = lookahead;
    st->ref20_mult = (float)SAMPLE_RATE / -logf(0.1f);

    peakmax_minwindow_reset(&st->lookahead_min, st->lookahead_size, 1.0f);
    int ma_size = st->lookahead_size / 2 + 1;
    if (ma_size > PEAKMAX_MAX_MA_SIZE) ma_size = PEAKMAX_MAX_MA_SIZE;
    peakmax_ma_reset(&st->smooth1, ma_size, 1.0f);
    peakmax_ma_reset(&st->smooth2, ma_size, 1.0f);
    peakmax_delay_reset(&st->delay_left, st->lookahead_size);
    peakmax_delay_reset(&st->delay_right, st->lookahead_size);

    st->slow_gr_s = st->fast_gr_s = st->dynamic_gr_s = 1.0f;
    st->dynamic_release_g = peakmax_filter1_g(0.001f, st->ref20_mult);

    st->dynamic_g1 = peakmax_filter1_g(0.281f, st->ref20_mult);
    st->dynamic_g2 = peakmax_filter1_g(0.057f, st->ref20_mult);
    st->dynamic_g3 = peakmax_filter1_g(0.00054f, st->ref20_mult);

    // sample-rate bucket for the dither's own noise-shaping table (see PEAKMAX_DITHER_IR) --
    // SAMPLE_RATE is fixed today, but this mirrors the original's own bucketing exactly rather than
    // hardcoding "always bucket 1".
    int sr_idx = (SAMPLE_RATE <= 44100) ? 0 : (SAMPLE_RATE <= 48000) ? 1 : (SAMPLE_RATE <= 88200) ? 2 : 3;
    peakmax_dither_reset(&st->dither);
    st->dither.sample_rate_index = sr_idx;

    st->last_gr_db = 0.0f;
    st->initialized = true;
}

void run_fx_peakmax(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxPeakMaxState *st = (FxPeakMaxState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxPeakMaxState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        fx_peakmax_activate(st);
    }

    const float threshold_db = -30.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 30.0f; // -30..0dB
    const float ceiling_db = -30.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 30.0f;   // -30..0dB
    const bool use_auto_release = params->param[2] >= 0.5f;
    int release_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[3])) * 5.0f + 0.5f);
    if (release_idx < 0) release_idx = 0; else if (release_idx > 5) release_idx = 5;
    int quantize_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[4])) * 4.0f + 0.5f);
    if (quantize_idx < 0) quantize_idx = 0; else if (quantize_idx > 4) quantize_idx = 4;
    int dither_type = (int)(fmaxf(0.0f, fminf(1.0f, params->param[5])) * 2.0f + 0.5f);
    if (dither_type < 0) dither_type = 0; else if (dither_type > 2) dither_type = 2;
    int shaping_type = (int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 3.0f + 0.5f);
    if (shaping_type < 0) shaping_type = 0; else if (shaping_type > 3) shaping_type = 3;

    const float input_gain_k = powf(10.0f, -threshold_db / 20.0f);
    const float output_gain_k = powf(10.0f, ceiling_db / 20.0f);
    // {0.01, 0.1, 1.0, 10.0, 100, 1000} ms, from pow(10, index-5) seconds -- the original's own
    // exact release_idx-to-seconds mapping (its slider4's own labeled option values).
    const float release_time_sec = powf(10.0f, (float)release_idx - 5.0f);
    const float fast_gr_g = peakmax_filter1_g(release_time_sec, st->ref20_mult);

    peakmax_dither_set_mode(&st->dither, 16 + quantize_idx * 2, dither_type, shaping_type);

    float in_l = *l * input_gain_k;
    float in_r = *r * input_gain_k;

    const float level = fmaxf(fabsf(in_l), fabsf(in_r));
    const float raw_gr = (level < 1.0f) ? 1.0f : (1.0f / level);

    const float min_gr = peakmax_minwindow_push(&st->lookahead_min, raw_gr);

    float output_gr;
    if (!use_auto_release) {
        if (min_gr < st->fast_gr_s) st->fast_gr_s = min_gr; // attack: instant
        else st->fast_gr_s = peakmax_filter1(st->fast_gr_s, fast_gr_g, min_gr); // release: fixed
        output_gr = st->fast_gr_s;
    } else {
        // 1. slow detector (0.250s attack / 0.500s release -- fixed regardless of the Release param)
        const float slow_attack_g = peakmax_filter1_g(0.250f, st->ref20_mult);
        const float slow_release_g = peakmax_filter1_g(0.500f, st->ref20_mult);
        if (min_gr < st->slow_gr_s) st->slow_gr_s = peakmax_filter1(st->slow_gr_s, slow_attack_g, min_gr);
        else st->slow_gr_s = peakmax_filter1(st->slow_gr_s, slow_release_g, min_gr);

        // 2. combine both detectors
        const float combined_gr = fminf(min_gr, st->slow_gr_s);
        if (combined_gr < st->dynamic_gr_s) {
            st->dynamic_gr_s = combined_gr; // attack: instant
        } else {
            st->dynamic_gr_s = peakmax_filter1(st->dynamic_gr_s, st->dynamic_release_g, combined_gr); // release: adaptive
        }
        output_gr = st->dynamic_gr_s;

        // 3. adapt dynamic_release_g for the NEXT sample, from THIS sample's own result -- a
        // one-sample-delayed adaptive coefficient, exactly matching the original's own order.
        const float dynamic_release = powf(1.0f - output_gr, 4.0f);
        float dynamic_release_g1 = fmaxf(peakmax_filter1_g(fmaxf(dynamic_release, 1e-9f), st->ref20_mult), expf(-1.0f));
        const float dynamic_factor = fminf(st->slow_gr_s * 2.0f - output_gr, 1.0f);
        float dynamic_release_g2;
        if (dynamic_factor < 0.25f) {
            dynamic_release_g2 = 1.0f;
        } else if (dynamic_factor < 0.5f) {
            dynamic_release_g2 = (dynamic_factor - 0.25f) / 0.25f * (st->dynamic_g1 - 1.0f) + 1.0f;
        } else if (dynamic_factor < 0.71f) {
            dynamic_release_g2 = (dynamic_factor - 0.5f) / 0.21f * (st->dynamic_g2 - st->dynamic_g1) + st->dynamic_g1;
        } else {
            dynamic_release_g2 = (dynamic_factor - 0.71f) / 0.29f * (st->dynamic_g3 - st->dynamic_g2) + st->dynamic_g2;
        }
        st->dynamic_release_g = dynamic_release_g1 * dynamic_release_g2;
    }

    // Triangular smoothing (two cascaded boxcars) -- removes the stepping/zipper artifacts a raw
    // sliding-window-minimum's own discrete jumps would otherwise leave in the gain curve.
    output_gr = peakmax_ma_push(&st->smooth1, output_gr);
    output_gr = peakmax_ma_push(&st->smooth2, output_gr);

    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, output_gr)));

    const float delayed_l = peakmax_delay_push(&st->delay_left, in_l);
    const float delayed_r = peakmax_delay_push(&st->delay_right, in_r);

    float out_l = delayed_l * output_gr * output_gain_k;
    float out_r = delayed_r * output_gr * output_gain_k;

    peakmax_dither_process(&st->dither, &out_l, &out_r);

    *l = out_l;
    *r = out_r;
}

// Meter readback for process_fx_chain() (keeps FxPeakMaxState private to this file).
float fx_peakmax_gr_db(const void *state) { return ((const FxPeakMaxState *)state)->last_gr_db; }
