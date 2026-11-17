// fx_reverb_delay.c -- Reverb and delay: FDNVerb, Broadcast (profanity) Delay.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx/fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_FDNVERB: see its own #define comment above for the full design, provenance, and the
// two dead reference-source params deliberately not carried over. Rate ratio is fixed at compile
// time (unlike the reference's own runtime sampleRate) since this engine's own SAMPLE_RATE always
// is -- every delay length below is the reference's own 44100Hz table scaled by this ratio once.
static const float FX_FDNVERB_RATE_RATIO = (float)SAMPLE_RATE / 44100.0f;

#define FX_FDNVERB_NUM_DELAYS 8
// Power of 2 for cheap masking; comfortably covers this effect's own largest possible base delay
// (2251 samples @ 44100Hz * this engine's rate ratio * Size's own max 1.5x scale, ~3674 samples
// @ 48kHz) plus modulation's own few-sample wobble, with real margin -- deliberately NOT the
// reference's own wasteful flat "2 seconds" allocation, which this design never needs (see the
// max-base-delay arithmetic above).
#define FX_FDNVERB_BUF_SIZE 4096
#define FX_FDNVERB_BUF_MASK (FX_FDNVERB_BUF_SIZE - 1)
#define FX_FDNVERB_NUM_DIFFUSERS 4
// Covers every scaled diffuser delay used below (input diffusers up to ~195 samples, Early
// Reflections' own up to ~178) with real margin -- these are small, fixed, never-modulated
// integer delays (a true allpass comb, not a fractional/interpolated read), so plain modulo
// indexing against each instance's own capacity (not a power-of-2 mask) matches the reference
// exactly.
#define FX_FDNVERB_SMALL_DIFFUSER_CAP 256
#define FX_FDNVERB_ER_TAPS 16
#define FX_FDNVERB_ER_DIFFUSERS 2
// Power of 2 covering the largest scaled Early Reflections tap delay (87.5ms reference tap *
// Size's own max 1.5x scale * 48 samples/ms, ~6300 samples) with real margin.
#define FX_FDNVERB_ER_BUF_SIZE 8192
#define FX_FDNVERB_ER_BUF_MASK (FX_FDNVERB_ER_BUF_SIZE - 1)
// Power of 2 covering this effect's own max Pre-Delay (250ms * 48kHz = 12000 samples) with margin.
#define FX_FDNVERB_PREDELAY_BUF_SIZE 16384
#define FX_FDNVERB_PREDELAY_MASK (FX_FDNVERB_PREDELAY_BUF_SIZE - 1)

// Hadamard 8x8 matrix, pre-normalized by 1/sqrt(8) -- energy-preserving (orthogonal) mixing, the
// same matrix the reference uses verbatim.
static const float FX_FDNVERB_NORM = 0.35355339059327379f; // 1/sqrt(8)
static const float FX_FDNVERB_HADAMARD[FX_FDNVERB_NUM_DELAYS][FX_FDNVERB_NUM_DELAYS] = {
    { 1, 1, 1, 1, 1, 1, 1, 1},
    { 1,-1, 1,-1, 1,-1, 1,-1},
    { 1, 1,-1,-1, 1, 1,-1,-1},
    { 1,-1,-1, 1, 1,-1,-1, 1},
    { 1, 1, 1, 1,-1,-1,-1,-1},
    { 1,-1, 1,-1,-1, 1,-1, 1},
    { 1, 1,-1,-1,-1,-1, 1, 1},
    { 1,-1,-1, 1,-1, 1, 1,-1},
}; // each entry above is implicitly * FX_FDNVERB_NORM (applied once at point of use, not stored
   // 64 times over -- the reference stores it pre-multiplied; this is numerically identical)

// Prime-length delays at 44100Hz (20-51ms range) -- scaled by FX_FDNVERB_RATE_RATIO and Size's own
// sizeScale at point of use, since Size can change live.
static const int FX_FDNVERB_BASE_DELAYS_44100[FX_FDNVERB_NUM_DELAYS] = {
    887, 1097, 1361, 1559, 1709, 1867, 2053, 2251
};
// Per-delay-line LFO rate multipliers and starting phases (radians) -- fixed, decorrelated offsets
// so all 8 lines' modulation never lines up, the reference's own anti-metallic-ringing trick.
static const float FX_FDNVERB_LFO_RATES[FX_FDNVERB_NUM_DELAYS] = {
    0.31f, 0.37f, 0.43f, 0.53f, 0.61f, 0.71f, 0.79f, 0.89f
};
static const float FX_FDNVERB_LFO_PHASES[FX_FDNVERB_NUM_DELAYS] = {
    0.0f, 0.79f, 1.57f, 2.36f, 3.14f, 3.93f, 4.71f, 5.50f
};
// Input diffuser delays at 44100Hz (prime), chained in series ahead of the FDN's own 8 lines.
static const int FX_FDNVERB_DIFFUSER_DELAYS_44100[FX_FDNVERB_NUM_DIFFUSERS] = {53, 89, 127, 179};

// Early Reflections: 16 taps, different L/R times for stereo imaging, alternating polarity gains
// simulating real wall reflections (odd bounce counts invert, same idea a real room's own early
// echoes have). All in ms at 44100Hz-equivalent (scaled by Size, like everything else here).
static const float FX_FDNVERB_ER_TAP_MS_L[FX_FDNVERB_ER_TAPS] = {
    3.5f, 7.1f, 11.2f, 15.7f, 19.3f, 24.1f, 29.8f, 33.5f,
    38.7f, 43.2f, 49.1f, 55.3f, 61.7f, 68.4f, 76.2f, 85.0f
};
static const float FX_FDNVERB_ER_TAP_MS_R[FX_FDNVERB_ER_TAPS] = {
    5.2f, 9.3f, 13.8f, 17.1f, 22.0f, 26.4f, 31.5f, 36.2f,
    41.0f, 46.8f, 52.3f, 58.1f, 64.5f, 71.9f, 79.0f, 87.5f
};
static const float FX_FDNVERB_ER_TAP_GAIN_BASE[FX_FDNVERB_ER_TAPS] = {
    0.85f, -0.72f, 0.65f, -0.58f, 0.52f, -0.46f, 0.41f, -0.37f,
    0.33f, -0.29f, 0.26f, -0.23f, 0.20f, -0.17f, 0.15f, -0.13f
};
// Early Reflections' own 2 input diffusers per channel (different L/R delays for stereo).
static const int FX_FDNVERB_ER_DIFFUSER_DELAYS_L[FX_FDNVERB_ER_DIFFUSERS] = {75, 137};
static const int FX_FDNVERB_ER_DIFFUSER_DELAYS_R[FX_FDNVERB_ER_DIFFUSERS] = {93, 163};

// A small, fixed-integer-delay allpass comb -- used for both the FDN's own input diffusers and
// Early Reflections' own per-channel diffusers (identical structure in the reference, just
// different delay tables). Unlike the FDN's own modulated delay lines, delaySamples here is a
// fixed integer set once at init, never modulated, so plain modulo indexing (against this
// instance's own capacity) needs no interpolation -- matches the reference's AllpassFilter exactly.
typedef struct {
    float buf[FX_FDNVERB_SMALL_DIFFUSER_CAP];
    int cap;           // buffer capacity actually in use (delay_samples + 4, <= the array's own fixed size)
    int write_pos;
    int delay_samples;
    float coeff;
} FdnverbAllpass;

static float fdnverb_allpass_process(FdnverbAllpass *ap, float input) {
    int read_pos = ap->write_pos - ap->delay_samples;
    read_pos %= ap->cap; if (read_pos < 0) read_pos += ap->cap;
    float delayed = ap->buf[read_pos];
    float output = delayed - ap->coeff * input;
    ap->buf[ap->write_pos] = input + ap->coeff * delayed;
    ap->write_pos = (ap->write_pos + 1) % ap->cap;
    return output;
}

// Bounded-offset interpolated circular-buffer read, identical in spirit to hardtune_read_interp
// (see its own comment on why the small relative offset -- never the absolute, ever-growing
// write_pos -- is what goes through float math) but against this effect's own FX_FDNVERB_BUF_MASK,
// kept as this effect's own small helper rather than reaching into HardTune's private one so a
// future change to either effect's own buffer size can never silently affect the other.
static float fdnverb_read_interp(const float *buf, uint32_t write_pos, float rel_offset) {
    float floor_offset = floorf(rel_offset);
    int32_t int_offset = (int32_t)floor_offset;
    float frac = rel_offset - floor_offset;
    uint32_t base_idx = (uint32_t)((int32_t)write_pos + int_offset);
    float s0 = buf[base_idx & FX_FDNVERB_BUF_MASK];
    float s1 = buf[(base_idx + 1) & FX_FDNVERB_BUF_MASK];
    return s0 + (s1 - s0) * frac;
}

typedef struct {
    // --- Pre-delay: plain stereo ring buffer, interpolated read (Pre-Delay is knob-adjustable
    // live, so the read offset is a float, not a fixed integer) ---
    float predelay_buf[2][FX_FDNVERB_PREDELAY_BUF_SIZE];
    uint32_t predelay_write_pos;

    // --- FDN core ---
    float fdn_buf[FX_FDNVERB_NUM_DELAYS][FX_FDNVERB_BUF_SIZE];
    uint32_t fdn_write_pos;  // shared: all 8 lines advance in lockstep, exactly 1/sample, like the reference
    float fdn_lfo_phase[FX_FDNVERB_NUM_DELAYS];
    float fdn_damping_lp_state[FX_FDNVERB_NUM_DELAYS]; // one-pole HF-damping filter, in the feedback path
    FdnverbAllpass input_diffusers[FX_FDNVERB_NUM_DIFFUSERS];

    // --- Early Reflections ---
    float er_buf[2][FX_FDNVERB_ER_BUF_SIZE];
    uint32_t er_write_pos; // shared L/R, same lockstep reasoning as fdn_write_pos
    FdnverbAllpass er_diffusers[2][FX_FDNVERB_ER_DIFFUSERS]; // [channel][diffuser]
    int er_tap_delay[2][FX_FDNVERB_ER_TAPS];
    float er_tap_gain[2][FX_FDNVERB_ER_TAPS];

    bool initialized;
} FxFdnverbState;

void run_fx_fdnverb(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxFdnverbState *st = (FxFdnverbState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxFdnverbState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) {
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) st->fdn_lfo_phase[i] = FX_FDNVERB_LFO_PHASES[i];
        for (int i = 0; i < FX_FDNVERB_NUM_DIFFUSERS; i++) {
            int d = (int)(FX_FDNVERB_DIFFUSER_DELAYS_44100[i] * FX_FDNVERB_RATE_RATIO);
            st->input_diffusers[i].delay_samples = d;
            st->input_diffusers[i].cap = d + 4;
        }
        for (int ch = 0; ch < 2; ch++) {
            const int *dl = (ch == 0) ? FX_FDNVERB_ER_DIFFUSER_DELAYS_L : FX_FDNVERB_ER_DIFFUSER_DELAYS_R;
            for (int i = 0; i < FX_FDNVERB_ER_DIFFUSERS; i++) {
                int d = (int)(dl[i] * FX_FDNVERB_RATE_RATIO);
                st->er_diffusers[ch][i].delay_samples = d;
                st->er_diffusers[ch][i].cap = d + 4;
            }
        }
        st->initialized = true;
    }

    const float rt60 = 0.3f * powf(12.0f / 0.3f, fmaxf(0.0f, fminf(1.0f, params->param[0])));   // 0.3..12s, log taper
    const float room_size = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    const float predelay_ms = fmaxf(0.0f, fminf(1.0f, params->param[2])) * 250.0f;
    const float damping = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float diffusion = fmaxf(0.0f, fminf(1.0f, params->param[4]));
    const float mod_rate_hz = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 9.9f;
    const float mod_depth = fmaxf(0.0f, fminf(1.0f, params->param[6]));
    const float early_level = fmaxf(0.0f, fminf(1.0f, params->param[7]));
    const float early_decay = fmaxf(0.0f, fminf(1.0f, params->param[8]));
    const bool freeze = params->param[9] >= 0.5f;

    // --- Pre-delay ---
    uint32_t pd_idx = st->predelay_write_pos & FX_FDNVERB_PREDELAY_MASK;
    st->predelay_buf[0][pd_idx] = *l;
    st->predelay_buf[1][pd_idx] = *r;
    const float predelay_samples = predelay_ms * (float)SAMPLE_RATE / 1000.0f;
    // Reuses fdnverb_read_interp's own bounded-offset trick even though this buffer's mask
    // differs -- the function only ever masks with FX_FDNVERB_BUF_MASK, so a raw manual
    // interpolated read is done here instead rather than parametrizing the mask for one call site.
    {
        float floor_off = floorf(-predelay_samples);
        int32_t int_off = (int32_t)floor_off;
        float frac = -predelay_samples - floor_off;
        uint32_t base = (uint32_t)((int32_t)st->predelay_write_pos + int_off);
        float l0 = st->predelay_buf[0][base & FX_FDNVERB_PREDELAY_MASK];
        float l1 = st->predelay_buf[0][(base + 1) & FX_FDNVERB_PREDELAY_MASK];
        float r0 = st->predelay_buf[1][base & FX_FDNVERB_PREDELAY_MASK];
        float r1 = st->predelay_buf[1][(base + 1) & FX_FDNVERB_PREDELAY_MASK];
        *l = l0 + (l1 - l0) * frac;
        *r = r0 + (r1 - r0) * frac;
    }
    st->predelay_write_pos++;
    const float predelayed_l = *l, predelayed_r = *r;

    // --- Early Reflections (operates on the pre-delayed signal, see this effect's own top
    // comment for why Level applies once, to dry+taps together, rather than the reference's own
    // double-counted version) ---
    {
        float size_scale = 0.3f + room_size * 1.2f;
        float ms_to_samples = (float)SAMPLE_RATE / 1000.0f;
        for (int t = 0; t < FX_FDNVERB_ER_TAPS; t++) {
            int dl = (int)(FX_FDNVERB_ER_TAP_MS_L[t] * size_scale * ms_to_samples); if (dl < 1) dl = 1;
            int dr = (int)(FX_FDNVERB_ER_TAP_MS_R[t] * size_scale * ms_to_samples); if (dr < 1) dr = 1;
            st->er_tap_delay[0][t] = dl;
            st->er_tap_delay[1][t] = dr;
            float time_norm = (float)t / (float)(FX_FDNVERB_ER_TAPS - 1);
            float decay_factor = powf(1.0f - early_decay * 0.6f, time_norm * 3.0f);
            st->er_tap_gain[0][t] = FX_FDNVERB_ER_TAP_GAIN_BASE[t] * decay_factor;
            st->er_tap_gain[1][t] = FX_FDNVERB_ER_TAP_GAIN_BASE[t] * decay_factor;
        }
    }
    float er_result[2];
    for (int ch = 0; ch < 2; ch++) {
        float in = (ch == 0) ? predelayed_l : predelayed_r;
        float diffused = in;
        for (int i = 0; i < FX_FDNVERB_ER_DIFFUSERS; i++) diffused = fdnverb_allpass_process(&st->er_diffusers[ch][i], diffused);
        uint32_t idx = st->er_write_pos & FX_FDNVERB_ER_BUF_MASK;
        st->er_buf[ch][idx] = diffused;
        float tap_sum = 0.0f;
        for (int t = 0; t < FX_FDNVERB_ER_TAPS; t++) {
            uint32_t ridx = (uint32_t)((int32_t)st->er_write_pos - st->er_tap_delay[ch][t]) & FX_FDNVERB_ER_BUF_MASK;
            tap_sum += st->er_buf[ch][ridx] * st->er_tap_gain[ch][t];
        }
        er_result[ch] = (in + tap_sum) * early_level;
    }
    st->er_write_pos++;

    // --- FDN8x8 late reverb (operates on the SAME pre-delayed signal) ---
    float fdn_out_l, fdn_out_r;
    {
        float mono_in = (predelayed_l + predelayed_r) * 0.5f;
        float diff_coeff = 0.3f + diffusion * 0.4f;
        for (int i = 0; i < FX_FDNVERB_NUM_DIFFUSERS; i++) {
            st->input_diffusers[i].coeff = diff_coeff;
            mono_in = fdnverb_allpass_process(&st->input_diffusers[i], mono_in);
        }

        float size_scale = 0.3f + room_size * 1.2f;
        float lp_freq = 1000.0f + (1.0f - damping) * 14000.0f;
        lp_freq = fmaxf(200.0f, fminf((float)SAMPLE_RATE * 0.49f, lp_freq));
        float lp_coeff = expf(-2.0f * (float)M_PI * lp_freq / (float)SAMPLE_RATE);

        float delay_out[FX_FDNVERB_NUM_DELAYS];
        float base_delay[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            base_delay[i] = (float)FX_FDNVERB_BASE_DELAYS_44100[i] * FX_FDNVERB_RATE_RATIO * size_scale;
            // Phase advances THEN is read this same sample, matching the reference's own order.
            float rate_hz = FX_FDNVERB_LFO_RATES[i] * mod_rate_hz;
            st->fdn_lfo_phase[i] += 2.0f * (float)M_PI * rate_hz / (float)SAMPLE_RATE;
            if (st->fdn_lfo_phase[i] >= 2.0f * (float)M_PI) st->fdn_lfo_phase[i] -= 2.0f * (float)M_PI;
            float depth_samples = 0.5f + mod_depth * 3.0f;
            float mod_offset = sinf(st->fdn_lfo_phase[i]) * depth_samples;
            delay_out[i] = fdnverb_read_interp(st->fdn_buf[i], st->fdn_write_pos, -base_delay[i] + mod_offset);
        }

        float filtered[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            st->fdn_damping_lp_state[i] = delay_out[i] * (1.0f - lp_coeff) + st->fdn_damping_lp_state[i] * lp_coeff;
            float feedback_gain = freeze ? 1.0f : powf(10.0f, -3.0f * base_delay[i] / (rt60 * (float)SAMPLE_RATE));
            filtered[i] = st->fdn_damping_lp_state[i] * feedback_gain;
        }

        float mixed[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            float matrix_sum = 0.0f;
            for (int j = 0; j < FX_FDNVERB_NUM_DELAYS; j++) matrix_sum += FX_FDNVERB_HADAMARD[i][j] * FX_FDNVERB_NORM * filtered[j];
            mixed[i] = (1.0f - diffusion) * filtered[i] + diffusion * matrix_sum;
        }

        uint32_t widx = st->fdn_write_pos & FX_FDNVERB_BUF_MASK;
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            float new_sample = tanhf(mixed[i] + mono_in * 0.25f);
            if (!freeze) st->fdn_buf[i][widx] = new_sample;
        }
        st->fdn_write_pos++;

        fdn_out_l = 0.0f; fdn_out_r = 0.0f;
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            if (i % 2 == 0) { fdn_out_l += delay_out[i]; fdn_out_r -= delay_out[i] * 0.25f; }
            else            { fdn_out_r += delay_out[i]; fdn_out_l -= delay_out[i] * 0.25f; }
        }
        fdn_out_l *= 0.2f; fdn_out_r *= 0.2f;
    }

    *l = er_result[0] + fdn_out_l;
    *r = er_result[1] + fdn_out_r;
}

// Engine-wide count of MIDI notes CURRENTLY held (incremented/decremented at dispatch_midi_event()'s
// own single note-on/off choke point in backend.c, same "static global fx_chain.c owns, backend.c
// writes directly because #include makes them one translation unit" pattern g_hardtune_held_note
// etc. already use above for HardTune's own live-MIDI vibrato) -- exists purely to feed
// FX_TYPE_BROADCAST_DELAY's On-Air Override MIDI Trigger mode (param[7]==1, see
// run_fx_broadcastdelay()'s own comment): that mode engages the override for as long as this is
// nonzero, i.e. for as long as ANY note anywhere is currently held, not any particular one. Declared
// here rather than back with the other HardTune globals since nothing else needs it. A build with no
// note port at all (hds_fx.clap -- see its own header comment on why) never calls
// dispatch_midi_event(), so this simply sits at a permanent, honest 0 there -- MIDI Trigger mode is
// still selectable in that build for GUI parity, it just never fires, exactly like the Dump-button/
// On-Air Override plumbing hds_fx_plugin.c already documents needing no MIDI at all.
int g_bcastdly_notes_held = 0;

// Same idea as g_bcastdly_notes_held just above, but for the sustain pedal (CC64) instead of note
// events -- set directly from dispatch_midi_event()'s own CC64 handling in backend.c, right
// alongside midi_live->sustain_held, using that same polarity-corrected "currently held" boolean
// (not the raw CC value), so param[7]'s Inverted polarity setting is honored here too. On-Air
// Override MIDI Trigger mode (param[8]) engages the tone whenever EITHER this or
// g_bcastdly_notes_held is true -- a foot switch wired to CC64 (sustain) is just as valid a
// hands-free trigger source as a held key, and the two are purely additive, same as every other
// pair of gates this effect already combines.
bool g_bcastdly_sustain_held = false;

// --- FX_TYPE_BROADCAST_DELAY: an original implementation of the on-air "profanity delay"/dump
// buffer -- see this effect's own FX_TYPE_BROADCAST_DELAY comment in fx_types.h for the full
// param map and the restocking design's overall rationale. Buffer sized for the full 60s max
// delay at this engine's own fixed SAMPLE_RATE -- a fixed array embedded in the state struct,
// calloc'd once on first use, same convention FX_TYPE_TAPEDELAY2's own dL/dR arrays already use
// for a (much smaller) fixed-size delay line.
// HDS_BCASTDLY_MAX_SEC is in fx_chain.h (the GUI's delay-time knob range reads it too).
#define HDS_BCASTDLY_MAX_SAMPLES (SAMPLE_RATE * HDS_BCASTDLY_MAX_SEC + 8) // +8 guard past the nominal max for the interpolated read never quite reaching the write cursor

typedef struct {
    float buf_l[HDS_BCASTDLY_MAX_SAMPLES];
    float buf_r[HDS_BCASTDLY_MAX_SAMPLES];
    int write_pos;
    // The REAL, currently-buffered delay, in samples -- distinct from nominal_delay_samples (the
    // Delay knob's own target, recomputed fresh from param[0] every call): this is what actually
    // separates the write and read cursors right now, and is what a dump instantly shrinks and
    // restocking gradually regrows back toward the nominal target. Fractional (not an int) because
    // both restocking and ordinary knob glide move it by a sub-sample amount per sample -- and a
    // real `double`, not `float`: at Restock Speed's own 1% minimum this accumulates a +0.01
    // sample/sample increment while itself sitting at a value up to ~2.88M (60s at 48kHz), and
    // float32's mantissa can't represent that increment at that magnitude at all (its own ULP is
    // already ~0.016 samples by 144K and ~0.25 by 2.88M) -- confirmed by a real test showing the
    // buffer restocking at 1.5x its intended rate, silently, from rounding alone. double's far
    // larger mantissa keeps the true increment exact across this whole range.
    double current_delay_samples;
    bool restocking; // true only while actively recovering from a dump -- see run_fx_broadcastdelay()'s own comment on why this needs a separate, much slower rate than ordinary knob glide
    // Edge-detect memory for the three one-shot dump buttons (param[1..3]) -- same idiom as
    // SeqLaneState.audio_preview_trigger: seeded to whatever the incoming param already reads the
    // instant this state is allocated (first call after a type switch, or a project/state reload),
    // so a nonzero trigger value loaded from a saved project can never fire a phantom dump the
    // moment playback starts.
    float last_dump7, last_dump15, last_dump30;
    float dump_avail_sec; // = current_delay_samples / SAMPLE_RATE, read back by process_fx_chain() for the dump-availability meter, same convention as last_gr_db
    float tone_phase; // "On-Air Override" 1kHz tone (param[5]) -- see run_fx_broadcastdelay()'s own comment
    bool tone_active_prev; // edge-detect so each fresh press restarts the tone at a clean zero-crossing instead of an arbitrary leftover phase
} FxBroadcastDelayState;

void run_fx_broadcastdelay(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxBroadcastDelayState *st = (FxBroadcastDelayState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxBroadcastDelayState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->last_dump7 = params->param[1];
        st->last_dump15 = params->param[2];
        st->last_dump30 = params->param[3];
    }

    double nominal_delay_samples = (double)fmaxf(0.0f, fminf(1.0f, params->param[0])) * (double)(HDS_BCASTDLY_MAX_SEC * SAMPLE_RATE);

    // Dump buttons: an edge (any change, not just an increase, so this is robust even if a wire
    // value ever wraps) instantly removes that many seconds of buffered delay, clamped so a dump
    // can never go negative -- pressing 30s Dump with only 10s actually buffered just empties it.
    if (params->param[1] != st->last_dump7)  { st->current_delay_samples = fmax(0.0, st->current_delay_samples - 7.0  * (double)SAMPLE_RATE); st->last_dump7  = params->param[1]; st->restocking = true; }
    if (params->param[2] != st->last_dump15) { st->current_delay_samples = fmax(0.0, st->current_delay_samples - 15.0 * (double)SAMPLE_RATE); st->last_dump15 = params->param[2]; st->restocking = true; }
    if (params->param[3] != st->last_dump30) { st->current_delay_samples = fmax(0.0, st->current_delay_samples - 30.0 * (double)SAMPLE_RATE); st->last_dump30 = params->param[3]; st->restocking = true; }

    // Auto-restock (mandatory -- see fx_types.h's own comment for the full derivation): while
    // recovering from a dump, current_delay_samples grows back toward nominal at Restock Speed's
    // own slow, deliberately near-inaudible rate (the read cursor briefly falls a hair behind the
    // write cursor, i.e. output plays back fractionally SLOWER than real time -- the opposite of
    // "faster", which would only pull the two cursors closer together and drain the buffer
    // further). Ordinary knob glide (the Delay knob simply being turned, no dump involved) uses a
    // much faster fixed rate instead -- still smoothed enough to avoid a click, but fast enough
    // that turning the knob actually feels responsive. All of this math is done in double (see
    // current_delay_samples' own comment) -- float32 cannot hold a sub-1-sample increment at the
    // magnitudes this accumulator reaches.
    double restock_frac = (double)fmaxf(0.0f, fminf(1.0f, params->param[4]));
    double restock_rate = 0.01 + restock_frac * 0.24; // 1%..25% of a sample per sample -- i.e. output runs 1-25% slower than real-time while restocking
    const double knob_glide_rate = 240.0; // samples of delay-time change per sample processed -- covers the full 60s range in 0.25s, plenty responsive for a manual knob drag
    if (st->current_delay_samples < nominal_delay_samples - 0.5) {
        st->current_delay_samples += st->restocking ? restock_rate : knob_glide_rate;
        if (st->current_delay_samples >= nominal_delay_samples) { st->current_delay_samples = nominal_delay_samples; st->restocking = false; }
    } else if (st->current_delay_samples > nominal_delay_samples + 0.5) {
        st->restocking = false; // shrinking the gap is always a knob action (Delay turned down), never a dump recovery
        st->current_delay_samples -= knob_glide_rate;
        if (st->current_delay_samples < nominal_delay_samples) st->current_delay_samples = nominal_delay_samples;
    } else {
        st->restocking = false; // caught all the way up
    }
    if (st->current_delay_samples < 0.0) st->current_delay_samples = 0.0;
    if (st->current_delay_samples > (double)(HDS_BCASTDLY_MAX_SAMPLES - 4)) st->current_delay_samples = (double)(HDS_BCASTDLY_MAX_SAMPLES - 4);
    st->dump_avail_sec = (float)(st->current_delay_samples / (double)SAMPLE_RATE);

    st->buf_l[st->write_pos] = *l;
    st->buf_r[st->write_pos] = *r;

    // Linearly-interpolated read (current_delay_samples is fractional whenever restocking or
    // gliding) -- the interpolation itself is what makes both of those sound like a smooth, tape-
    // style time change rather than a stepped/aliased one. The read POSITION is computed in
    // double (see current_delay_samples' own comment); only the final interpolation weight
    // (`frac`) is narrowed to float, which is fine -- that's a 0.0-1.0 crossfade weight applied to
    // audio samples, not an accumulator, so float precision there is exactly as good as everywhere
    // else audio math in this file already uses it.
    double read_pos_f = (double)st->write_pos - st->current_delay_samples;
    while (read_pos_f < 0.0) read_pos_f += (double)HDS_BCASTDLY_MAX_SAMPLES;
    int read_i0 = (int)read_pos_f;
    if (read_i0 >= HDS_BCASTDLY_MAX_SAMPLES) read_i0 -= HDS_BCASTDLY_MAX_SAMPLES;
    int read_i1 = read_i0 + 1; if (read_i1 >= HDS_BCASTDLY_MAX_SAMPLES) read_i1 = 0;
    float frac = (float)(read_pos_f - floor(read_pos_f));
    *l = st->buf_l[read_i0] * (1.0f - frac) + st->buf_l[read_i1] * frac;
    *r = st->buf_r[read_i0] * (1.0f - frac) + st->buf_r[read_i1] * frac;

    // "On-Air Override" tone: a LIVE self-censoring tool, deliberately NOT a delayed one. This whole
    // effect's point is that its output lags the live input by however many seconds the Delay knob
    // sets -- so overriding what feeds INTO the buffer wouldn't reach the output until the delay
    // caught up, long after whatever prompted the override in the first place. Overriding the OUTPUT
    // sample directly instead makes it instant, exactly like a real on-air panic button. The buffer
    // write/read above still ran completely untouched (still fed from the real live input), so the
    // instant the gate goes back to false the delayed program audio picks back up exactly where it
    // already was, with nothing lost or skipped -- true whichever of the three param[7] modes below
    // is actually driving that gate.
    // param[5] (mouse hold) and param[8] (MIDI Trigger Enabled, gating "is any note held" OR "is the
    // sustain pedal held" -- see g_bcastdly_notes_held/g_bcastdly_sustain_held's own comments) are
    // independent, additive gates -- either one alone is enough to engage the tone, and enabling
    // MIDI Trigger never takes the mouse away. param[7] Disabled overrides both: when true the gate
    // is forced false no matter what either of them say. See fx_types.h's own param[5]/param[7]/
    // param[8] comments.
    bool tone_active = (params->param[7] != 0.0f) ? false
        : (params->param[5] != 0.0f) || (params->param[8] != 0.0f && (g_bcastdly_notes_held > 0 || g_bcastdly_sustain_held));
    if (tone_active) {
        if (!st->tone_active_prev) st->tone_phase = 0.0f; // clean zero-crossing start each press, not an arbitrary leftover phase
        float vol = fmaxf(0.0f, fminf(1.0f, params->param[6])); // param[6] Volume -- see fx_types.h's own comment
        *l = sinf(st->tone_phase) * vol;
        *r = *l;
        st->tone_phase += 2.0f * (float)M_PI * 1000.0f / (float)SAMPLE_RATE;
        if (st->tone_phase >= 2.0f * (float)M_PI) st->tone_phase -= 2.0f * (float)M_PI;
    }
    st->tone_active_prev = tone_active;

    st->write_pos++;
    if (st->write_pos >= HDS_BCASTDLY_MAX_SAMPLES) st->write_pos = 0;
}

// Meter readback for process_fx_chain() (keeps FxBroadcastDelayState private to this file).
float fx_broadcastdelay_dump_avail_sec(const void *state) { return ((const FxBroadcastDelayState *)state)->dump_avail_sec; }
