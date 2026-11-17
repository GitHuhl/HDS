// fx_pitch.c -- Pitch: HardTune, dbx 120X sub-harmonic synthesizer.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_HARDTUNE: see its own #define comment above for the full design and what it is/isn't
// a clone of. Three stages: (1) YIN pitch detection, run once per HOP on a mono-summed analysis
// window; (2) scale/key (or MIDI) quantization of the detected pitch to a target ratio, smoothed in
// two independent stages (Tracking Speed on the raw pitch, Retune Speed on the final ratio); (3) a
// period-synchronous two-tap crossfaded delay-line read, PSOLA's own core resynthesis mechanism,
// applied identically to both channels from the one shared detected pitch/ratio (mono detector,
// same "one shared value, never independent per-channel" posture as every other dynamics effect in
// this file, for the same reason -- a real vocal source is usually a single pitched voice, not two
// independently-pitched channels).
//
// PSOLA without explicit pitch marks: the classic algorithm places synthesis grains at hand-picked
// glottal-pulse-aligned "marks." This implementation gets the same period-synchronous effect more
// simply, without ever detecting individual pulses: a phase accumulator advances by `ratio` (the
// pitch-shift factor) each sample and wraps modulo `grain_period` (the CURRENT detected input
// period, in samples) -- two read taps, 50% of a period apart in phase, each windowed by a Hann
// function of its own phase (0 at wrap, peak at mid-period). Because a Hann window's value at phase
// x and at phase (x+0.5 mod 1) sum to exactly 1 (the standard 50%-overlap constant-overlap-add
// property), the two taps crossfade into each other with no gain bump or dip at the wrap point, no
// extra normalization needed. Each tap reads from a per-channel delay line at a small, BOUNDED
// offset behind the write pointer (`-DELAY + phase - grain_period/2`, always within
// `[-DELAY-grain_period/2, -DELAY+grain_period/2]`) -- bounded forever regardless of how long a
// non-1.0 ratio persists, since `phase` wraps rather than accumulating; DELAY is fixed at this
// engine's own absolute-lowest-supported-pitch period, comfortably covering every Voice Range
// preset's own longer periods too.
//
// FX_HARDTUNE_MAX_LAG/MIN_LAG bound the compile-time scratch arrays (sized for this engine's own
// absolute widest search range, 70-1100Hz); the actual per-call search range narrows to whichever
// Voice Range preset is selected.
// -1 = no note currently held (MIDI Target Source mode then falls back to no correction, same as an
// unvoiced input); else the current monophonic MIDI note number. Set by handle_note_on/
// handle_note_off (via g_hardtune_note_stack, declared later where MonoNoteStack is defined --
// these two plain floats live here instead, before run_fx_hardtune's own definition, since that
// function needs them and is defined well before MonoNoteStack is).
float g_hardtune_held_note = -1.0f;
// Live pitch bend, in SEMITONES (already scaled by Program 0's own pb_range_up/pb_range_down, the
// same convention render_audio_block's own per-Program pitch bend already uses -- see
// dispatch_midi_event's own HDS_MIDI_PITCH_BEND branch). Applied by FX_TYPE_HARDTUNE as a final
// multiplicative adjustment layered on top of its ALREADY-corrected pitch, unconditionally
// (regardless of Target Source) -- see that effect's own comment for why it's never fed back into
// detection or quantization.
float g_hardtune_bend_semitones = 0.0f;
// Mod Wheel (CC1) and channel aftertouch, mirrored from MidiLiveState purely so FX_TYPE_HARDTUNE's
// own Vibrato Depth can be scaled by a live controller (same "gate an LFO depth with the mod
// wheel" idea as PackLfoParams.mod_source_cc, but this global effect has no MidiLiveState of its
// own to read -- same reason g_hardtune_bend_semitones/g_hardtune_held_note are globals instead).
// Updated by dispatch_midi_event's HDS_MIDI_CC/HDS_MIDI_CHANNEL_PRESSURE branches, right alongside
// their canonical MidiLiveState update.
float g_hardtune_modwheel = 0.0f;
float g_hardtune_channel_pressure = 0.0f;

#define FX_HARDTUNE_ABS_MIN_HZ 70.0f
#define FX_HARDTUNE_ABS_MAX_HZ 1100.0f
#define FX_HARDTUNE_MIN_LAG 44  // SAMPLE_RATE/1100Hz
#define FX_HARDTUNE_MAX_LAG 686 // SAMPLE_RATE/70Hz
#define FX_HARDTUNE_YIN_WINDOW 1400 // > FX_HARDTUNE_MAX_LAG with real margin for the difference function's own overlap sum
#define FX_HARDTUNE_YIN_HOP 480     // 10ms at 48kHz -- how often pitch is re-detected
#define FX_HARDTUNE_BUF_SIZE 4096   // power of 2 (cheap masking); comfortably covers YIN_WINDOW + PSOLA's own read-behind depth
#define FX_HARDTUNE_BUF_MASK (FX_HARDTUNE_BUF_SIZE - 1)
#define FX_HARDTUNE_PSOLA_DELAY FX_HARDTUNE_MAX_LAG // samples the PSOLA read stays behind the write pointer -- this IS the effect's fixed added latency

// YIN (de Cheveigne & Kawahara, 2002): difference function -> cumulative mean normalized difference
// function -> absolute-threshold local-minimum search (falling back to the global minimum if no
// candidate clears the threshold) -> parabolic interpolation for sub-sample period accuracy.
// *out_confidence is 1 - (the CMNDF value at the chosen lag), i.e. how convincingly periodic the
// window is -- low for noise/silence/unvoiced consonants, used by the caller as a voiced/unvoiced
// gate. window_size/min_lag/max_lag are all runtime values (Voice Range's own narrowed search
// range), bounded by the compile-time FX_HARDTUNE_MAX_LAG-sized scratch arrays below.
static float hardtune_yin_detect(const float *window, int window_size, int min_lag, int max_lag, float *out_confidence) {
    static const float THRESHOLD = 0.15f;
    float d[FX_HARDTUNE_MAX_LAG + 1];
    float d_prime[FX_HARDTUNE_MAX_LAG + 1];
    // Defensive clamp -- every real caller guarantees 1 <= min_lag <= max_lag <= FX_HARDTUNE_MAX_LAG
    // (Voice Range presets always have min_hz < max_hz), but this keeps every d_prime[] index below
    // provably in-bounds and initialized regardless, rather than relying on that being upheld.
    if (min_lag < 1) min_lag = 1;
    if (max_lag < min_lag) max_lag = min_lag;

    d[0] = 0.0f;
    for (int tau = 1; tau <= max_lag; tau++) {
        float sum = 0.0f;
        int n = window_size - tau;
        for (int i = 0; i < n; i++) {
            float diff = window[i] - window[i + tau];
            sum += diff * diff;
        }
        d[tau] = sum;
    }

    d_prime[0] = 1.0f;
    float running_sum = 0.0f;
    for (int tau = 1; tau <= max_lag; tau++) {
        running_sum += d[tau];
        d_prime[tau] = (running_sum > 1e-9f) ? (d[tau] * (float)tau / running_sum) : 1.0f;
    }

    int tau_est = min_lag; // always a valid, initialized d_prime[] index by construction (see the clamp above)
    bool found = false;
    for (int tau = min_lag; tau <= max_lag; tau++) {
        if (d_prime[tau] < THRESHOLD) {
            while (tau + 1 <= max_lag && d_prime[tau + 1] < d_prime[tau]) tau++;
            tau_est = tau;
            found = true;
            break;
        }
    }
    if (!found) {
        int best = min_lag;
        for (int tau = min_lag + 1; tau <= max_lag; tau++) {
            if (d_prime[tau] < d_prime[best]) best = tau;
        }
        tau_est = best;
    }

    float tau_refined = (float)tau_est;
    if (tau_est > min_lag && tau_est < max_lag) {
        float s0 = d_prime[tau_est - 1], s1 = d_prime[tau_est], s2 = d_prime[tau_est + 1];
        float denom = s0 - 2.0f * s1 + s2;
        if (fabsf(denom) > 1e-9f) {
            float delta = 0.5f * (s0 - s2) / denom;
            tau_refined += fmaxf(-1.0f, fminf(1.0f, delta));
        }
    }

    *out_confidence = 1.0f - fminf(1.0f, d_prime[tau_est]);
    return (float)SAMPLE_RATE / fmaxf(1.0f, tau_refined);
}

// Scale membership tables (1 = in-scale semitone, indexed 0-11 from the root) -- Chromatic accepts
// everything (Key is meaningless in that mode, matching every real pitch corrector's own behavior).
static const int8_t HARDTUNE_SCALE_TABLES[5][12] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // Chromatic
    {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1}, // Major
    {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0}, // Natural Minor
    {1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0}, // Major Pentatonic
    {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0}, // Minor Pentatonic
};

// Nearest in-scale MIDI note to a (possibly fractional) detected MIDI note number -- searches
// outward by semitone (0, +1, -1, +2, -2, ...) since every one of the 5 scales above has a member
// within a handful of semitones of any starting point.
static float hardtune_snap_to_scale(float midi_note, int key, int scale_idx) {
    int nearest = (int)floorf(midi_note + 0.5f);
    for (int offset = 0; offset <= 6; offset++) {
        int up = nearest + offset;
        int pc_up = ((up - key) % 12 + 12) % 12;
        if (HARDTUNE_SCALE_TABLES[scale_idx][pc_up]) return (float)up;
        if (offset > 0) {
            int down = nearest - offset;
            int pc_down = ((down - key) % 12 + 12) % 12;
            if (HARDTUNE_SCALE_TABLES[scale_idx][pc_down]) return (float)down;
        }
    }
    return (float)nearest; // unreachable for these 5 tables (all have members within +-6 semitones)
}

// Voice Range presets: (min_hz, max_hz) search windows -- deliberately generous, pragmatic bounds
// (like the real hardware/software presets this mirrors), not strict vocal-science boundaries.
static const float HARDTUNE_VOICE_RANGE_HZ[5][2] = {
    { 70.0f, 350.0f },  // Bass
    { 90.0f, 500.0f },  // Tenor
    { 150.0f, 700.0f }, // Alto
    { 180.0f, 1100.0f },// Soprano
    { 70.0f, 1000.0f }, // Wide
};

typedef struct {
    float buf[2][FX_HARDTUNE_BUF_SIZE]; // [channel][sample], circular via write_pos & MASK
    uint32_t write_pos;

    float yin_window[FX_HARDTUNE_YIN_WINDOW]; // mono-summed scratch, refilled every hop
    int hop_counter;

    float tracked_f0;        // Tracking-Speed-smoothed detected pitch, Hz (held during unvoiced gaps)
    float grain_period;      // current PSOLA grain period in samples, from tracked_f0 (held likewise) -- only ever updated once per HOP, see smoothed_grain_period's own comment
    float smoothed_grain_period; // grain_period chased at the SAMPLE rate -- see its own comment at this effect's per-sample PSOLA block
    bool voiced;

    float hop_target_ratio;  // this hop's correction goal (1.0 = none), before Retune Speed smoothing
    float target_ratio;      // Retune-Speed-smoothed ratio actually applied, updated every sample
    float phase;             // 0..grain_period, advances by target_ratio every sample

    float vibrato_phase;     // 0..1, this effect's own dedicated vibrato LFO (see its own comment)

    bool initialized;
} FxHardTuneState;

// Reads a small, BOUNDED (see this effect's own top comment) offset behind the write pointer with
// linear interpolation. Deliberately does NOT convert the absolute (ever-growing) write_pos to a
// float directly -- past roughly 2^24 samples (~5.8 minutes at 48kHz) that would silently lose
// precision and introduce read-position jitter. Only the small bounded `rel_offset` (always within
// a few hundred samples) goes through float math; the actual buffer index stays pure integer/
// unsigned arithmetic, which wraps correctly (and indefinitely) via 2's-complement modular
// arithmetic regardless of how long this effect keeps running.
static float hardtune_read_interp(const float *chan_buf, uint32_t write_pos, float rel_offset) {
    float floor_offset = floorf(rel_offset);
    int32_t int_offset = (int32_t)floor_offset;
    float frac = rel_offset - floor_offset;
    uint32_t base_idx = (uint32_t)((int32_t)write_pos + int_offset);
    float s0 = chan_buf[base_idx & FX_HARDTUNE_BUF_MASK];
    float s1 = chan_buf[(base_idx + 1) & FX_HARDTUNE_BUF_MASK];
    return s0 + (s1 - s0) * frac;
}

void run_fx_hardtune(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxHardTuneState *st = (FxHardTuneState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxHardTuneState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) {
        st->tracked_f0 = 200.0f; // arbitrary reasonable starting guess, overwritten by the first real detection
        st->grain_period = (float)SAMPLE_RATE / st->tracked_f0;
        st->smoothed_grain_period = st->grain_period;
        st->target_ratio = 1.0f;
        st->hop_target_ratio = 1.0f;
        st->phase = 0.0f;
        st->voiced = false;
        st->initialized = true;
    }

    const int key = (int)(fmaxf(0.0f, fminf(1.0f, params->param[0])) * 11.0f + 0.5f);
    const int scale_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[1])) * 4.0f + 0.5f);
    const bool midi_target = params->param[2] >= 0.5f; // Target Source: Scale/Key (0) vs MIDI (1)
    const float amount = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float retune_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 199.9f;   // 0.1..200ms
    const float tracking_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 499.9f; // 0.1..500ms
    int voice_range_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 4.0f + 0.5f);
    if (voice_range_idx < 0) voice_range_idx = 0; else if (voice_range_idx > 4) voice_range_idx = 4;
    const float vibrato_rate_hz = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[7])) * 9.9f;  // 0.1..10Hz
    const float vibrato_depth_semitones = fmaxf(0.0f, fminf(1.0f, params->param[8])) * 1.5f; // 0..1.5 semitones peak
    const int vibrato_mod_choice = (int)(fmaxf(0.0f, fminf(1.0f, params->param[9])) * 2.0f + 0.5f); // 0=None,1=Mod Wheel,2=Aftertouch
    const float range_min_hz = HARDTUNE_VOICE_RANGE_HZ[voice_range_idx][0];
    const float range_max_hz = HARDTUNE_VOICE_RANGE_HZ[voice_range_idx][1];
    int min_lag = (int)((float)SAMPLE_RATE / range_max_hz);
    int max_lag = (int)((float)SAMPLE_RATE / range_min_hz);
    if (min_lag < FX_HARDTUNE_MIN_LAG) min_lag = FX_HARDTUNE_MIN_LAG;
    if (max_lag > FX_HARDTUNE_MAX_LAG) max_lag = FX_HARDTUNE_MAX_LAG;

    // Write this sample into both channels' delay lines.
    uint32_t idx = st->write_pos & FX_HARDTUNE_BUF_MASK;
    st->buf[0][idx] = *l;
    st->buf[1][idx] = *r;

    // --- Analysis: YIN once per hop, on a mono-summed window of the most recent YIN_WINDOW samples ---
    st->hop_counter++;
    if (st->hop_counter >= FX_HARDTUNE_YIN_HOP) {
        st->hop_counter = 0;
        float peak = 0.0f;
        for (int i = 0; i < FX_HARDTUNE_YIN_WINDOW; i++) {
            uint32_t src = (uint32_t)((int32_t)st->write_pos - (FX_HARDTUNE_YIN_WINDOW - 1 - i));
            float mono = 0.5f * (st->buf[0][src & FX_HARDTUNE_BUF_MASK] + st->buf[1][src & FX_HARDTUNE_BUF_MASK]);
            st->yin_window[i] = mono;
            peak = fmaxf(peak, fabsf(mono));
        }

        float confidence = 0.0f;
        float detected_hz = hardtune_yin_detect(st->yin_window, FX_HARDTUNE_YIN_WINDOW, min_lag, max_lag, &confidence);
        // Voiced gate: a real level floor (skip near-silence entirely, matching every other effect's
        // own "nothing meaningful to process" posture) AND YIN's own confidence, AND the result must
        // actually land inside this call's search range (a sanity check against the parabolic
        // interpolation nudging a boundary lag estimate just outside it).
        st->voiced = (peak > 0.003f) && (confidence > 0.5f) && detected_hz >= range_min_hz * 0.98f && detected_hz <= range_max_hz * 1.02f;

        if (st->voiced) {
            // Tracking Speed: one-pole smoother in log-frequency domain, updated once per hop (this
            // smoother's own "sample rate" is hops/second, not SAMPLE_RATE -- see its own rate calc).
            const float tracking_hop_rate_hz = (float)SAMPLE_RATE / (float)FX_HARDTUNE_YIN_HOP;
            const float tracking_rate = 1.0f - expf(-1.0f / (0.001f * tracking_ms * tracking_hop_rate_hz));
            float log_tracked = log2f(st->tracked_f0) + (log2f(detected_hz) - log2f(st->tracked_f0)) * tracking_rate;
            st->tracked_f0 = exp2f(log_tracked);
            float period = (float)SAMPLE_RATE / st->tracked_f0;
            st->grain_period = fmaxf((float)min_lag, fminf((float)max_lag, period));
        }
        // Unvoiced: hold tracked_f0/grain_period at their last good values (avoids a spurious jump
        // from noisy detection during a breath/consonant/silence).

        float raw_target_ratio = 1.0f;
        if (st->voiced) {
            if (midi_target) {
                // g_hardtune_held_note (see its own comment) is this engine-wide monophonic
                // last-note-priority tracker, updated by handle_note_on/handle_note_off regardless
                // of which Program a note targets -- if nothing is currently held, fall back to no
                // correction, same posture as an unvoiced input.
                if (g_hardtune_held_note >= 0.0f) {
                    float midi = 69.0f + 12.0f * log2f(st->tracked_f0 / 440.0f);
                    raw_target_ratio = exp2f((g_hardtune_held_note - midi) / 12.0f);
                }
            } else {
                float midi = 69.0f + 12.0f * log2f(st->tracked_f0 / 440.0f);
                float target_midi = hardtune_snap_to_scale(midi, key, scale_idx);
                raw_target_ratio = exp2f((target_midi - midi) / 12.0f);
            }
        }
        // Amount blends in log/semitone space (perceptually correct interpolation, not linear ratio).
        st->hop_target_ratio = exp2f(log2f(raw_target_ratio) * amount);
    }

    // --- Per-sample: Retune Speed glides target_ratio toward this hop's goal ---
    const float retune_rate = 1.0f - expf(-1.0f / (0.001f * retune_ms * (float)SAMPLE_RATE));
    float log_cur = log2f(st->target_ratio) + (log2f(st->hop_target_ratio) - log2f(st->target_ratio)) * retune_rate;
    st->target_ratio = exp2f(log_cur);

    // Live vibrato: this effect's own dedicated sine LFO (a per-slot instance, not the per-Program
    // LFO matrix -- HardTune is a global FX effect with no single Program/voice of its own to hang
    // a routing-matrix destination off of, same reason the MIDI note tracker/pitch bend above are
    // plain globals rather than routed through that matrix). Optional Mod Source scaling (Mod Wheel
    // CC1 or channel aftertouch) gates Depth the same way PackLfoParams.mod_source_cc gates a
    // regular LFO's depth -- unassigned (None, the default) is a pure no-op at full knob depth.
    // Like pitch bend, this is a FINAL multiplicative adjustment on top of the already-corrected
    // ratio, computed after Retune Speed's own smoothing and never fed back into detection/
    // quantization above, so it can never fight the correction either.
    float vibrato_cc_scale = 1.0f;
    if (vibrato_mod_choice == 1) vibrato_cc_scale = g_hardtune_modwheel;
    else if (vibrato_mod_choice == 2) vibrato_cc_scale = g_hardtune_channel_pressure;
    const float vibrato_semitones = sinf(2.0f * (float)M_PI * st->vibrato_phase) * vibrato_depth_semitones * vibrato_cc_scale;
    st->vibrato_phase += vibrato_rate_hz / (float)SAMPLE_RATE;
    if (st->vibrato_phase >= 1.0f) st->vibrato_phase -= 1.0f;

    // Live pitch bend (see g_hardtune_bend_semitones's own comment): a final multiplicative
    // adjustment layered on TOP of the already-corrected ratio, unconditionally -- regardless of
    // Target Source, and never fed back into detection/quantization above, so it can never fight
    // the correction. Not smoothed here beyond whatever smoothing the MIDI pitch-bend wheel's own
    // physical motion already has -- a real-time performance control, not a quantized target.
    const float output_ratio = st->target_ratio * exp2f((g_hardtune_bend_semitones + vibrato_semitones) / 12.0f);

    // smoothed_grain_period chases grain_period at the SAMPLE rate, not the hop rate grain_period
    // itself updates at (once per FX_HARDTUNE_YIN_HOP samples, in the analysis block above). This
    // matters because every piece of math below -- the phase wrap boundary, frac_a/frac_b,
    // offset_a/offset_b -- depends on grain_period, and a HELD vocal note's own detected period
    // still wobbles by a sample or two hop-to-hop (ordinary YIN cycle-to-cycle jitter, not
    // mistracking) -- stepping grain_period abruptly once per hop was a genuine, confirmed
    // discontinuity in the PSOLA read position at every single hop boundary: a real click, not a
    // perceptual artifact, repeating at the hop rate (100Hz at the default 10ms hop) and heard as a
    // persistent "crunchy"/granular texture rather than one clean pop. A short (~3ms) one-pole chase
    // here removes that step entirely for any ORDINARY jitter-scale change (a sample or two) --
    // effectively fully settled well before the next hop, no audible pitch lag. A large, sudden
    // real change (a genuine pitch jump, or a mistracked octave) takes a few hops to fully settle
    // the rest of the way instead of one -- an intentional, inaudible trade: it only ever smooths
    // where this value feeds a continuously-read PSOLA position, never anything perceived as a
    // discrete pitch-correction decision (that's target_ratio/Retune Speed's own job, upstream).
    const float period_smooth_rate = 1.0f - expf(-1.0f / (0.003f * (float)SAMPLE_RATE));
    st->smoothed_grain_period += (st->grain_period - st->smoothed_grain_period) * period_smooth_rate;

    // --- PSOLA resynthesis: two Hann-crossfaded taps, phase wraps mod smoothed_grain_period (see
    // this effect's own top comment for why this never drifts unboundedly even under a sustained
    // non-1.0 ratio) ---
    // Phase advances by (output_ratio - 1), NOT output_ratio: the actual buffer read position is
    // write_pos + offset, and write_pos ITSELF already advances by 1 every sample (real time) --
    // phase only needs to supply the DIFFERENCE from real-time so the read position's own total
    // rate of advance through the buffer's content comes out to exactly output_ratio (1 from
    // write_pos's own motion + (output_ratio-1) from phase = output_ratio). At output_ratio=1 this
    // correctly makes phase never move at all: a fixed offset behind write_pos is already a
    // transparent, undelayed-in-pitch copy, needing no wrapping/resets whatsoever.
    st->phase += (output_ratio - 1.0f);
    if (st->phase >= st->smoothed_grain_period) st->phase -= st->smoothed_grain_period;
    if (st->phase < 0.0f) st->phase += st->smoothed_grain_period; // pitch-DOWN corrections drift phase negative

    const float frac_a = st->phase / st->smoothed_grain_period;
    float frac_b = frac_a + 0.5f;
    if (frac_b >= 1.0f) frac_b -= 1.0f;
    const float win_a = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * frac_a);
    const float win_b = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * frac_b);

    const float offset_a = -(float)FX_HARDTUNE_PSOLA_DELAY + (frac_a * st->smoothed_grain_period - st->smoothed_grain_period * 0.5f);
    const float offset_b = -(float)FX_HARDTUNE_PSOLA_DELAY + (frac_b * st->smoothed_grain_period - st->smoothed_grain_period * 0.5f);

    *l = hardtune_read_interp(st->buf[0], st->write_pos, offset_a) * win_a
       + hardtune_read_interp(st->buf[0], st->write_pos, offset_b) * win_b;
    *r = hardtune_read_interp(st->buf[1], st->write_pos, offset_a) * win_a
       + hardtune_read_interp(st->buf[1], st->write_pos, offset_b) * win_b;

    st->write_pos++;
}

// --- FX_TYPE_DBX120X: see its own #define comment above for the full design rationale (why
// zero-crossing period measurement on a heavily band-limited mono signal, not a general pitch
// tracker, is what makes this robust on polyphonic/complex program material) and param map.
// FX_DBX120X_TRACK_MIN_HZ/MAX_HZ are in fx_chain.h (the GUI's pitch-tracker readout reads them too).

static inline float fx_dbx120x_fraction_to_track_hz(float fraction) {
    fraction = fmaxf(0.0f, fminf(1.0f, fraction));
    return FX_DBX120X_TRACK_MIN_HZ * powf(FX_DBX120X_TRACK_MAX_HZ / FX_DBX120X_TRACK_MIN_HZ, fraction); // 30 * 10^0=30 .. 30 * 10^1=300
}

typedef struct {
    // Two cascaded fx_multifilter_2pole_stage() LPF stages (4-pole/24dB-oct -- see this effect's
    // own #define comment on why isolating the sub-bass fundamental from everything else sharing
    // the mix is the single biggest lever for staying robust under polyphony), run on the MONO
    // sum of L+R -- the detector and the envelope gate below both read this one band-limited mono
    // signal; only the final synthesized tone (identical on both channels, standard mono-sub
    // practice) and the untouched dry L/R ever differ per channel.
    float lp_low_state[2], lp_band_state[2]; // [stage]
    float dc_lp; // dbx160_hpf()'s own lowpass-state convention -- see where it's applied below for why

    bool initialized;
    bool crossing_armed; // Schmitt-trigger "seen genuinely negative since the last crossing" latch
                           // -- see the zero-crossing detection block's own comment for why this
                           // replaced a naive single-previous-sample comparison.
    uint32_t samples_since_crossing; // also doubles as the refractory counter

    float tracked_hz; // Tracking-Glide-smoothed detected fundamental, Hz -- updated once per
                        // qualifying zero crossing (an EVENT-driven update, exactly like
                        // FX_TYPE_HARDTUNE's own tracked_f0/Tracking Speed)
    float smoothed_hz; // tracked_hz chased at the SAMPLE rate with a short, fixed (~3ms) time
                        // constant -- NOT user-exposed, purely the anti-click fix HardTune's own
                        // grain_period/smoothed_grain_period needed (see that effect's own comment
                        // for why a value that steps at irregular, sparse update events must still
                        // be chased every sample before it ever reaches an oscillator's phase).
    float osc_phase; // 0..1, the synthesized subharmonic's own free-running phase accumulator

    // Engagement gain: a Schmitt-trigger hysteresis gate (same open/close idiom as FX_TYPE_GATE's
    // own gate_open/gain, see that effect's own comment) on the band-limited mono signal's peak
    // envelope, smoothly fading the SYNTHESIZED TONE in/out (never the dry signal) so it only
    // appears when there's real low-frequency energy present.
    float band_env; // one-pole peak envelope follower on the band-limited mono signal
    bool gate_open;
    float engage_gain; // 0.0 (no synthesis) .. 1.0 (full synthesis level)
} FxDbx120xState;

void run_fx_dbx120x(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDbx120xState *st = (FxDbx120xState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDbx120xState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) {
        st->tracked_hz = 80.0f; // arbitrary reasonable bass-fundamental starting guess, overwritten by the first real detection
        st->smoothed_hz = st->tracked_hz;
        st->engage_gain = 0.0f;
        st->gate_open = false;
        st->initialized = true;
    }

    float track_hz = fx_dbx120x_fraction_to_track_hz(params->param[0]);
    float threshold_db = -60.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 60.0f;
    float release_ms = 10.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 990.0f; // 10..1000ms
    float synthesis_level = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    int divide_ratio = (params->param[4] >= 0.5f) ? 4 : 2; // choice: 0=Half(-1 octave), 1=Quarter(-2 octaves)
    float glide_ms = 10.0f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 490.0f; // 10..500ms

    // --- DC block first: a plain lowpass (see "Band-limit" just below) passes DC through at
    // essentially unity gain -- it does NOT remove it. The zero-crossing detector further down
    // depends on genuinely symmetric crossings through TRUE zero; any DC offset riding on the
    // input (not unheard of from some interfaces/upstream plugins/asymmetric waveshaping elsewhere
    // in a chain feeding this slot) would bias that reference point, which can suppress detected
    // crossings outright (if the offset is large enough that the signal never actually goes
    // negative) or skew their timing asymmetrically into a wrong period reading either way -- a
    // real correctness gap, not a cosmetic one. Fixed at 5Hz (well below even Track Freq's own
    // 30Hz floor, so it can never audibly touch real program material) using the same correctly-
    // normalized one-pole DC-blocker already used for FX_TYPE_DBX160's own sidechain HPF (see
    // dbx160_hpf's own comment).
    float mono_in = (*l + *r) * 0.5f;
    const float dc_alpha = 1.0f - expf(-2.0f * (float)M_PI * 5.0f / (float)SAMPLE_RATE);
    mono_in = dbx160_hpf(mono_in, &st->dc_lp, dc_alpha);

    // --- Band-limit: 4-pole (24dB/oct) lowpass on the DC-blocked mono sum, feeding BOTH the
    // detector and the envelope gate below. Deliberately non-resonant (k fixed, no Resonance knob)
    // -- this is a pre-filter for tracking/gating, not a tone-shaping tool; a player wanting
    // resonance should chain a real MultiFilter slot instead.
    const float k = 1.4142135f; // Butterworth-ish damping for a plain, non-resonant lowpass
    const float g = tanf((float)M_PI * track_hz / (float)SAMPLE_RATE);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    float lp = mono_in;
    for (int s = 0; s < 2; s++) lp = fx_multifilter_2pole_stage(lp, k, a1, a2, a3, 1 /* LPF */, &st->lp_low_state[s], &st->lp_band_state[s]);

    // --- Envelope gate on the band-limited signal (see FxDbx120xState's own comment): fixed fast
    // attack (synthesis fades IN quickly once real bass arrives), Release is the one knob exposed
    // (param[2]) -- same asymmetric attack/release-rate convention as FX_TYPE_GATE's own gain
    // smoother. A fixed 3dB hysteresis band (not its own knob -- this effect's panel stays close
    // to the real hardware's own minimal control set) stops the gate chattering on a signal
    // hovering right at Threshold.
    float band_peak = fabsf(lp);
    const float env_attack_rate = 1.0f - expf(-1.0f / (0.001f * 5.0f * (float)SAMPLE_RATE)); // fixed 5ms
    const float env_release_rate = 1.0f - expf(-1.0f / (0.001f * 80.0f * (float)SAMPLE_RATE)); // fixed 80ms, just for envelope smoothing (separate from the gate's own Release below)
    float env_rate = (band_peak > st->band_env) ? env_attack_rate : env_release_rate;
    st->band_env += (band_peak - st->band_env) * env_rate;

    float open_threshold_lin = powf(10.0f, threshold_db / 20.0f);
    float close_threshold_lin = powf(10.0f, (threshold_db - 3.0f) / 20.0f);
    if (!st->gate_open) {
        if (st->band_env >= open_threshold_lin) st->gate_open = true;
    } else if (st->band_env < close_threshold_lin) {
        st->gate_open = false;
    }
    float target_gain = st->gate_open ? 1.0f : 0.0f;
    const float gate_attack_rate = 1.0f - expf(-1.0f / (0.001f * 10.0f * (float)SAMPLE_RATE)); // fixed 10ms
    const float gate_release_rate = 1.0f - expf(-1.0f / (0.001f * release_ms * (float)SAMPLE_RATE));
    float gate_rate = (target_gain > st->engage_gain) ? gate_attack_rate : gate_release_rate;
    st->engage_gain += (target_gain - st->engage_gain) * gate_rate;

    // --- Zero-crossing period detection (see this effect's own #define comment for why this,
    // not a general pitch tracker, is the actual robustness mechanism here). Positive-going
    // crossings only (low->high, i.e. one full period between consecutive qualifying crossings),
    // with a small hysteresis dead-band (must have been genuinely negative, not just near zero)
    // and a refractory window tied to Track Freq itself (a band-limited signal physically cannot
    // contain energy above its own cutoff, so any "crossing" sooner than that is noise/ripple,
    // never a real new cycle).
    //
    // A proper two-state Schmitt trigger (crossing_armed), NOT a bare "compare this sample against
    // the previous one" check -- a real, confirmed bug that WAS here: comparing only adjacent
    // samples against the two fixed thresholds misses a crossing whenever the signal takes MORE
    // THAN ONE sample to cross the dead-band itself (completely ordinary for a quieter or heavily
    // attenuated signal moving slowly relative to the sample rate, not a contrived edge case) --
    // e.g. sample N sits at +0.0002 (already above zero but short of +hyst) and sample N+1 jumps to
    // +0.0023: neither "prev < -hyst" nor "this transition" fires, so that whole real cycle's
    // crossing is silently dropped, corrupting the measured period for that cycle (and, caught on a
    // live DC-offset test, happened to drop the SAME cycle repeatedly, landing on a steady half-
    // frequency reading instead of occasional jitter). Tracking "have we been low enough at some
    // point since the last crossing" as its own persistent boolean, armed the instant lp dips below
    // -hyst and fired the instant it's armed AND lp rises past +hyst (however many samples that
    // takes), is immune to this regardless of how slowly the signal crosses.
    st->samples_since_crossing++;
    uint32_t refractory_samples = (uint32_t)((float)SAMPLE_RATE / (track_hz * 2.5f));
    const float hyst = 0.0005f; // small dead-band, well below any real signal of interest
    if (lp < -hyst) st->crossing_armed = true;
    if (st->crossing_armed && lp >= hyst && st->samples_since_crossing >= refractory_samples) {
        float measured_period_samples = (float)st->samples_since_crossing;
        float measured_hz = (float)SAMPLE_RATE / measured_period_samples;
        if (measured_hz >= FX_DBX120X_TRACK_MIN_HZ * 0.5f && measured_hz <= track_hz * 1.5f) {
            // Event-driven one-pole smoother in log-frequency domain -- Tracking Glide's own
            // "sample rate" is crossing-events/second (irregular), so the rate uses the REAL
            // elapsed time since the last update (measured_period_samples / SAMPLE_RATE), not
            // a fixed hop duration the way FX_TYPE_HARDTUNE's hop-driven smoother can.
            float dt_sec = measured_period_samples / (float)SAMPLE_RATE;
            float glide_rate = 1.0f - expf(-dt_sec / (0.001f * glide_ms));
            float log_tracked = log2f(st->tracked_hz) + (log2f(measured_hz) - log2f(st->tracked_hz)) * glide_rate;
            st->tracked_hz = exp2f(log_tracked);
        }
        st->samples_since_crossing = 0;
        st->crossing_armed = false;
    }

    // Per-sample anti-click chase (see FxDbx120xState's own comment) -- always runs, regardless
    // of whether a crossing happened this sample, so smoothed_hz glides continuously toward
    // whatever tracked_hz last became rather than snapping at each sparse update event.
    const float smooth_rate = 1.0f - expf(-1.0f / (0.003f * (float)SAMPLE_RATE)); // fixed ~3ms
    st->smoothed_hz += (st->tracked_hz - st->smoothed_hz) * smooth_rate;

    // --- Synthesis: a clean, phase-continuous sine at smoothed_hz/divide_ratio (Half = 1 octave
    // down, Quarter = 2 octaves down) -- deliberately NOT a divided square wave (what most dumb
    // analog-style octave dividers actually output, and the "buzzy rather than clean sub" sound
    // that's part of why so many digital subharmonic generators read as unmusical).
    float osc_freq = st->smoothed_hz / (float)divide_ratio;
    st->osc_phase += osc_freq / (float)SAMPLE_RATE;
    if (st->osc_phase >= 1.0f) st->osc_phase -= floorf(st->osc_phase);
    float sub_tone = sinf(2.0f * (float)M_PI * st->osc_phase) * st->engage_gain * synthesis_level;

    // Added identically to both channels (mono sub, standard practice) -- the untouched dry L/R
    // underneath it are never altered by this effect at all, only this slot's own generic Mix
    // (applied by process_fx_chain() around the wet value this function returns) can fade the
    // ADDED tone back out, same "own internal level, separate from the slot's generic mix"
    // convention as FX_TYPE_ENSEMBLE's own FX Level.
    *l += sub_tone;
    *r += sub_tone;
}
