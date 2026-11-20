// render.c -- the per-sample voice renderer: render_audio_block, the FM oscillator and FM algorithm table, and the DSP building blocks it inlines (oscillator shapes, PolyBLEP, unison saw, SVF filters, envelope ticking, LFO waveforms and rate/per-key resolution, program EQ). Kept in one file on purpose: everything here runs per sample per voice, and splitting it would turn inlined calls into real ones.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"
#include "core/string_model.h"

// FIXED: Restored 'p->s' to reference sustain data properties correctly
// Everything tick_adsr() needs BESIDES the envelope instance's own dynamic state (current_level/
// state/timers/release_level) -- all of it derived purely from one EG *config* (a PackEGParams +
// its mode), which is shared by every voice's own instance of that same EG slot. These values can
// only change in response to an OSC message drained between blocks, never mid-block -- so
// compute_eg_derived_rates() is called once per EG config per BLOCK (see render_audio_block),
// not once per VOICE per SAMPLE the way this used to be computed inline inside tick_adsr() itself.
// With up to MAX_VOICES voices x ENV_COUNT=10 envelopes each, that redundancy (several fmaxf/
// fminf clamps and divisions, recomputed identically for every voice sharing the same EG slot)
// was a real, measured bottleneck that scaled directly with polyphony -- confirmed the dominant
// single cost in a profile of a plain two-oscillator patch at moderate voice counts, well before
// oscillator/filter work alone would explain any strain. (EGDerivedRates itself now lives above,
// just ahead of ADSR -- see that typedef's own comment for why it had to move.)

static void compute_eg_derived_rates(const PackEGParams *p, int mode, EGDerivedRates *out) {
    out->mode = mode;
    out->sustain_level = (mode == 1 || mode == 2 || mode == 4) ? 0.0f : (p->s / 100.0f);
    // Attack Start Level: the level the envelope *starts at* the instant a new note triggers it
    // (set in handle_note_on), before Attack ramps up toward Peak Level. At 0 (the default) this
    // is a plain envelope; raising it adds an instant "click"/punch. Controlled only via the
    // draggable (0,0) node in the GUI, not a dedicated knob.
    out->atk_level = fmaxf(0.0f, fminf(1.0f, p->al / 100.0f));
    // Attack Peak Level: the level Attack ramps *up to* (100% is the classic full-swing default).
    // Controlled via the same peak node's vertical drag that already sets Attack time (x).
    out->peak_level = fmaxf(0.0f, fminf(1.0f, p->pk / 100.0f));

    out->t_dly = (mode == 5) ? fmaxf(0.001f, p->d / 1000.0f) : 0.0f;
    out->t_hld = (mode == 4 || mode == 6) ? fmaxf(0.001f, p->h / 1000.0f) : 0.0f;
    float t_att = fmaxf(0.001f, p->a / 1000.0f);
    float t_dec = fmaxf(0.001f, p->dec / 1000.0f);
    float t_rel = fmaxf(0.001f, p->r / 1000.0f);

    // Attack climbs from its (possibly nonzero) start level up to Peak Level.
    out->attack_rate = fmaxf(0.0f, out->peak_level - out->atk_level) / (t_att * SAMPLE_RATE);
    out->decay_rate = fmaxf(0.0f, out->peak_level - out->sustain_level) / (t_dec * SAMPLE_RATE);
    // FIXED (kept from the pre-refactor version): release ramps from the level captured at the
    // moment note-off fired (release_level, per-instance), not from sustain_level. Previously
    // this was always 0 for any mode whose sustain_level is 0 (AR, AD, AHD), so those envelopes
    // never released. release_rate_per_level is the knob-derived part only; the per-instance
    // multiply by release_level happens in tick_adsr() below.
    out->release_rate_per_level = 1.0f / (t_rel * SAMPLE_RATE);
}

// Advances one envelope INSTANCE by one sample, using its EG config's precomputed rates (see
// compute_eg_derived_rates()/EGDerivedRates above) -- the only per-instance work left is the
// state-machine step itself (a handful of adds/compares), no clamps/divides/transcendentals.
// sustain_chase_rate is passed in rather than recomputed here since it's a pure function of
// SAMPLE_RATE (not even knob-dependent) -- see its own one-time computation in render_audio_block.
void tick_adsr(ADSR *env, const EGDerivedRates *r, float sustain_chase_rate) {
    env->env_type = r->mode;
    switch (env->state) {
        case STATE_IDLE: break;
        case STATE_DELAY:
            env->delay_timer += 1.0f / SAMPLE_RATE;
            if (env->delay_timer >= r->t_dly) { env->state = STATE_ATTACK; env->delay_timer = 0.0f; }
            break;
        case STATE_ATTACK:
            env->current_level += r->attack_rate;
            if (env->current_level >= r->peak_level) {
                env->current_level = r->peak_level;
                if (r->t_hld > 0.0f) { env->state = STATE_HOLD; env->hold_timer = 0.0f; }
                else { env->state = (r->mode == 1 || r->mode == 3) ? STATE_SUSTAIN : STATE_DECAY; }
            }
            break;
        case STATE_HOLD:
            env->current_level = r->peak_level;
            env->hold_timer += 1.0f / SAMPLE_RATE;
            if (env->hold_timer >= r->t_hld) { env->state = STATE_DECAY; env->hold_timer = 0.0f; }
            break;
        case STATE_DECAY:
            env->current_level -= r->decay_rate;
            if (env->current_level <= r->sustain_level) {
                env->current_level = r->sustain_level;
                env->state = (r->mode == 2 || r->mode == 4) ? STATE_IDLE : STATE_SUSTAIN;
            }
            break;
        case STATE_SUSTAIN:
            // Realtime sustain control: continuously chase the live Sustain knob (over a
            // ~15ms time constant, so moving it doesn't zipper/click) instead of freezing at
            // whatever level Decay left it at -- lets Sustain be ridden like a pedal while a
            // note is held, with no need to retrigger.
            // AR (mode 1) is excluded: its sustain_level is pinned to 0 purely for
            // release-ramp bookkeeping (see above) -- AR has no real sustain segment, it must
            // stay frozen at Peak Level (where Attack left it) until Release fires, not chase
            // down to 0.
            if (r->mode != 1) {
                env->current_level += (r->sustain_level - env->current_level) * sustain_chase_rate;
            }
            break;
        case STATE_RELEASE:
            env->current_level -= env->release_level * r->release_rate_per_level;
            if (env->current_level <= 0.0f) { env->current_level = 0.0f; env->state = STATE_IDLE; }
            break;
    }
}
float poly_blep_step(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    else if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// Shared by WAVE_SQUARE (fixed 50% duty, kept exactly as before for patch compatibility) and
// WAVE_PULSE (variable duty via pulse_width_frac, 0..1). A band-limited rectangle: two poly-BLEP
// corrected edges, one at phase 0 and one at phase == duty width.
static float band_limited_pulse(float phase, float dt, float duty_frac) {
    float n = (phase < duty_frac) ? 1.0f : -1.0f; n += poly_blep_step(phase, dt);
    float pf = phase + (1.0f - duty_frac); if (pf >= 1.0f) pf -= 1.0f;
    return n - poly_blep_step(pf, dt);
}

// Runs one logical filter for a single sample: a State-Variable Filter (SVF) core (Low/High/
// Band pass), with slope_poles selecting between a single 2-pole (12dB/oct) pass or the same
// core cascaded twice back-to-back for a 4-pole (24dB/oct) slope. low_state/band_state must be
// 2-element arrays (per-voice, persistent between calls); only element 0 is touched at 2-pole,
// both at 4-pole -- so switching slopes live doesn't require resetting anything.
static float run_svf_filter(float input, float cutoff_hz, float res_q, int filter_type, int slope_poles, float low_state[2], float band_state[2]) {
    // "Off" (filter_type==0) means true bypass: the signal passes through completely unmodified
    // -- with 5 independent filter slots (see ProgramState.filters[5]) now able to each be
    // independently on/off, an unused slot must contribute nothing but a pass-through, not silence.
    if (filter_type == 0) return input;
    cutoff_hz = fmaxf(40.0f, fminf(18000.0f, cutoff_hz));
    // FIXED (catastrophic bug -- "total audio failure when changing filter knobs"): this used to
    // be the classic "naive" Chamberlin SVF (forward-Euler integration of the analog SVF ODEs,
    // coefficient 2*sin(pi*fc/fs)), which is only numerically stable for cutoff frequencies up to
    // roughly SAMPLE_RATE/6 (~8kHz at 48kHz) -- ABOVE that, its own internal state (low_state/
    // band_state) diverges to NaN/Inf within a handful of samples, at ANY resonance including
    // zero (confirmed directly: cutoff=8000 blew up by sample 22, cutoff=18000 by sample 6).
    // Since the Cutoff knob's own range is [40,18000], almost the entire "bright" half of the
    // knob's travel was landing in the unstable zone -- once triggered, NaN propagates through
    // every downstream mix/sum forever (nothing recovers it) until the voice is retriggered,
    // which is exactly the reported symptom.
    //
    // Replaced with Vadim Zavalishin's Topology-Preserving Transform (TPT / zero-delay-feedback)
    // SVF ("The Art of VA Filter Design") -- a trapezoidal (bilinear-consistent) discretization
    // of the same analog SVF prototype instead of forward-Euler. It is unconditionally stable for
    // any cutoff strictly below Nyquist (g = tan(pi*fc/fs) stays finite and well-bounded for this
    // knob's whole range -- max is tan(0.375*pi) ~= 2.414, nowhere near tan's pi/2 asymptote) and
    // for any resonance from 0 up to and including self-oscillation (k=0). This is a standard,
    // widely-used replacement (the reference design behind most modern software synth filters),
    // not an exotic one -- low_state/band_state keep their existing per-channel-per-stage array
    // layout and calling convention exactly (now holding the TPT integrators' ic2eq/ic1eq state
    // instead of the old Euler integrators' state, but still one lowpass-flavored and one
    // bandpass-flavored value per stage, so no caller or struct layout needed to change).
    // Damping: floored well above 0 rather than allowing Res=100 to reach EXACTLY k=0. Exact
    // zero damping is only marginally (not strictly) stable -- a single stage stays bounded, but
    // cascading two of them back-to-back at 4-pole slope has no energy dissipation anywhere in
    // the loop to correct for ordinary floating-point rounding, so it slowly drifts into runaway
    // growth (confirmed directly, and NOT just a theoretical concern: 4-pole/Res=100 diverged
    // anywhere from ~5k to ~90k samples in depending on cutoff, and a first attempt at a tiny
    // k=0.001 floor -- Q~1000 -- only delayed that, it didn't fix it, since 0.001 is still close
    // enough to marginal for the same drift to eventually win). k=0.05 (Q=20, an audibly extreme,
    // "nearly screaming" resonance already) gives enough real margin that the system is robustly
    // exponentially stable rather than borderline. Combined with the hard state clamp below.
    float k = fmaxf(0.05f, 2.0f * (1.0f - (fmaxf(0.0f, fminf(100.0f, res_q)) / 100.0f)));
    float g = tanf((float)M_PI * cutoff_hz / (float)SAMPLE_RATE); // prewarped coefficient
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    int stages = (slope_poles != 0) ? 2 : 1;
    float sig = input;
    for (int s = 0; s < stages; s++) {
        float v3 = sig - low_state[s];
        float v1 = a1 * band_state[s] + a2 * v3; // bandpass output
        float v2 = low_state[s] + a2 * band_state[s] + a3 * v3; // lowpass output
        band_state[s] = 2.0f * v1 - band_state[s];
        low_state[s] = 2.0f * v2 - low_state[s];
        // Defense in depth: even with damping floored above, a real-time recursive filter running
        // continuously for minutes at a time (a held pad note) is hard to prove stable for every
        // possible knob/modulation combination purely from theory -- clamp the state to a range
        // enormously wider than anything musically meaningful (real signals here live in roughly
        // [-5,5]) but far short of actually reaching Inf/NaN, so a still-undiscovered edge case
        // degrades to a loud-but-recoverable clip instead of the permanent NaN lockup this whole
        // fix exists to eliminate.
        band_state[s] = fmaxf(-1.0e6f, fminf(1.0e6f, band_state[s]));
        low_state[s] = fmaxf(-1.0e6f, fminf(1.0e6f, low_state[s]));
        float high = sig - k * v1 - v2; // highpass output
        // Exclusive pick -- no blending between taps (see FilterParams.filter_type's own
        // comment). Notch (= Low + High) is a free standard SVF identity; filter_type==0
        // (Off) already bypassed above, so only 1-4 reach here.
        switch (filter_type) {
            case 1:  sig = v2; break;        // LPF
            case 2:  sig = high; break;      // HPF
            case 3:  sig = v1; break;        // BPF
            default: sig = v2 + high; break; // Notch (filter_type==4)
        }
    }
    return sig;
}

// Small convenience wrapper: applies knob+mod combination for cutoff/res, matching the existing
// "*_mods[]" additive-modulation convention used everywhere else in this file. Every call site
// that drives one of the 5 filter slots goes through this.
static float run_svf_filter_p(float input, const FilterParams *p, float cutoff_mod, float res_mod, float low_state[2], float band_state[2]) {
    return run_svf_filter(input, p->cutoff + cutoff_mod, p->res_q + res_mod, (int)p->filter_type, (int)p->slope_poles, low_state, band_state);
}

// apply_program_eq() (cascades all 3 bands over one Program's bus) lives further down, right
// before render_audio_block -- it needs HdsEngineState, which isn't a complete type yet at this
// point in the file (see HdsEngineState's own comment on why it's declared so late).

#include "fx/fx_chain.h" // FX bus DSP, compiled separately (src/fx_chain.c)

// --- Curve/Drive: a single physical pot per oscillator/LFO (curve_drive, -1..1 in the struct),
// reinterpreted by the currently-active waveform -- see lookup_osc_sample()/lookup_lfo_waveform()
// below, the two places that dispatch on shape and therefore know which meaning applies:
//   Sawtooth/Triangle: bipolar Curve, center detent at 0 = perfectly linear ramp/slope. Negative
//   morphs into a concave (fast-start/slow-finish) curve, positive into a convex (slow-start/
//   fast-finish) curve -- see apply_wave_curve().
//   Sine: only the unipolar 0..1 half of the same stored value is meaningful (UI clamps the
//   knob's own range to 0..1 when a Sine shape is selected, so a negative stored value can only
//   arise from switching shapes with the knob left turned down-left -- apply_sine_drive() clamps
//   it right back to 0 in that case, reproducing "no drive" rather than doing anything strange).
//   0 = pure clean sine (the default for every sine osc/LFO). Toward 1 = soft-clip saturation,
//   flattening the peaks/troughs -- warm harmonic drive for audio, longer peak/trough dwell time
//   for an LFO. See apply_sine_drive().
// Every other shape (Square, Pulse, Unison Saw, Noise, Sample, Off) ignores curve_drive entirely
// -- out of scope for this control, matching the literal "Triangle/Sawtooth" and "Sine" ask.

// Warps a linear ramp t in [0,1] into a concave/convex curve. curve==0 is an exact identity (t
// unchanged, so Sawtooth/Triangle's curve=0 default reproduces their historical output exactly).
// 2^(curve*CURVE_STRENGTH) is always a strictly positive, finite exponent across the whole -1..1
// range (no singularity to guard), and a curve/-curve pair are exact reciprocal exponents of one
// another -- the standard mirror relationship between a matched concave/convex curve pair.
#define WAVE_CURVE_STRENGTH 3.0f
static inline float apply_wave_curve(float t, float curve) {
    if (curve == 0.0f) return t;
    float c = fmaxf(-1.0f, fminf(1.0f, curve));
    float exponent = powf(2.0f, c * WAVE_CURVE_STRENGTH);
    return powf(fmaxf(0.0f, fminf(1.0f, t)), exponent);
}

// Soft-clip saturation for Sine. drive<=0 is an exact identity (pure sine, the default). pre_gain
// scales linearly from 0 (at drive=0) so the tanh-based shaping smoothly reproduces the identity
// in the drive->0 limit (tanh(y)/tanh(y) -> linear as y->0, avoiding any discontinuity right as
// the knob leaves its detent) up to a deeply-saturating gain of 8 at drive=1. Dividing by
// tanh(pre_gain) renormalizes the shaped peak back to exactly +-1 (sine's own peak), so the wave
// visibly squares off toward drive=1 without its overall level dropping.
static inline float apply_sine_drive(float sine_val, float drive) {
    float d = fmaxf(0.0f, fminf(1.0f, drive));
    if (d <= 0.0f) return sine_val;
    float pre_gain = d * 8.0f;
    return tanhf(sine_val * pre_gain) / tanhf(pre_gain);
}

float lookup_osc_sample(WaveType type, float phase, float dt, float pulse_width_frac, float *tri_integrator, float curve_drive) {
    switch (type) {
        case WAVE_SAW: { float warped = apply_wave_curve(phase, curve_drive); float n = (warped * 2.0f) - 1.0f; return n - poly_blep_step(phase, dt); }
        // Same construction as WAVE_SAW, mirrored: apply_wave_curve reads from (1-phase) instead
        // of phase, so the ramp runs +1 -> -1 across the cycle instead of -1 -> +1 (matching
        // lookup_lfo_waveform's own existing descending-Saw case). WAVE_SAW's own discontinuity
        // JUMPS DOWN at wrap (subtracting poly_blep_step corrects a downward jump); this shape's
        // discontinuity JUMPS UP at the exact same point instead (value is -1 just before phase=1,
        // +1 just after phase=0) -- the opposite direction, so the correction is ADDED, the same
        // sign band_limited_pulse already uses for its own upward edge at phase 0.
        case WAVE_REVSAW: { float warped = apply_wave_curve(1.0f - phase, curve_drive); float n = (warped * 2.0f) - 1.0f; return n + poly_blep_step(phase, dt); }
        case WAVE_SQUARE: return band_limited_pulse(phase, dt, 0.5f);
        case WAVE_PULSE: {
            // Clamped away from the extremes: at duty 0 or 1 the pulse degenerates to DC (no
            // edges left to BLEP-correct), so keep a sliver of pulse width on both sides.
            float duty = fmaxf(0.02f, fminf(0.98f, pulse_width_frac));
            return band_limited_pulse(phase, dt, duty);
        }
        case WAVE_TRIANGLE: {
            if (curve_drive == 0.0f) {
                // Curve at center: exact original anti-aliased (leaky-integrator, BLEP-corrected
                // at both slope-reversal corners) Triangle -- byte-identical historical output.
                float n = (phase < 0.5f) ? 1.0f : -1.0f; n += poly_blep_step(phase, dt);
                float pf = phase + 0.5f; if (pf >= 1.0f) pf -= 1.0f; n -= poly_blep_step(pf, dt);
                *tri_integrator = 0.99f * (*tri_integrator) + (n * dt * 4.0f); return *tri_integrator;
            }
            // Curve engaged: warp each ramp half's own sub-phase through apply_wave_curve()
            // before reading it as a direct linear ramp -- a non-linear ramp has no simple
            // derivative-domain equivalent the leaky-integrator trick above could reuse, so this
            // trades that path's BLEP-corrected corners for a plain phase-domain construction
            // while curve != 0 (a triangle's corners are a slope discontinuity, not a value jump
            // like Saw/Square's, so the added aliasing risk is far smaller -- an accepted,
            // documented tradeoff, not a silent regression, since curve=0 stays exact).
            bool rising = phase < 0.5f;
            float sub = rising ? (phase * 2.0f) : ((phase - 0.5f) * 2.0f);
            float warped = apply_wave_curve(sub, curve_drive);
            return rising ? (warped * 2.0f - 1.0f) : (1.0f - warped * 2.0f);
        }
        case WAVE_SINE: return apply_sine_drive(sinf(phase * 2.0f * M_PI), curve_drive);
        case WAVE_NOISE: return (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
        default: return 0.0f;
    }
}

// Renders a 7-voice detuned unison stack. Returns the mono mix (used everywhere the rest of the
// engine expects a single sample: FM feed, filter input, envelope multiply -- unchanged behavior
// from the caller's point of view). Also writes out_l/out_r: a pan-law-weighted stereo pair driven
// by "width". At width=0 every voice is panned dead center so out_l == out_r == mono*0.7071 and
// (out_r - out_l) is exactly zero; the caller uses that difference as a pure "side" signal to widen
// the stereo image on top of the existing (unchanged) mono signal path, so there is no change in
// tone or level when width is 0 or the oscillator isn't set to Unison Saw.
static const float UNISON_SAW_VOICE_SPREAD[UNISON_SAW_VOICES] = {-1.0f, -0.667f, -0.333f, 0.0f, 0.333f, 0.667f, 1.0f};

// Per-voice effective phase increment for one of the 7 detuned unison voices, given the shared
// base_dt/detune_cents -- extracted from render_unison_saw()'s own inline math so the hard-sync
// reset logic in render_audio_block can reuse the exact same per-voice rate when it needs to
// reset vco2_unison_saw_phases[] fractionally-accurately, instead of duplicating this formula.
static inline float unison_saw_voice_dt(float base_dt, float detune_cents, int u) {
    float voice_cents = UNISON_SAW_VOICE_SPREAD[u] * detune_cents;
    return base_dt * powf(2.0f, voice_cents / 1200.0f);
}

// Generalizes poly_blep_step()'s two branches from "fixed height-2 jump positioned at the
// oscillator's own phase-0 edge" to "arbitrary jump height, positioned at an externally-imposed
// fractional-sample instant" -- used to BLEP-correct the amplitude discontinuity a hard sync
// reset creates in Osc 2's output (see render_audio_block). t_wrap is the fraction of the
// sample-to-sample interval at which the sync edge actually falls (0 = edge right after this
// sample, 1 = edge right before the next). shape_before applies to the sample rendered with the
// PRE-reset phase (this sample); shape_after applies to the sample rendered with the POST-reset
// phase (next sample) -- both scaled by (jump height / 2) by the caller, matching how this
// reduces to today's poly_blep_step()-based corrections for a natural height-2 edge.
static inline float sync_blep_shape_before(float t_wrap) { float x = 1.0f - t_wrap; return x * x; }
static inline float sync_blep_shape_after(float t_wrap) { return -(t_wrap * t_wrap); }

// PERF: per-unison pan (cosf/sinf) only ever depends on the Width knob, which is constant
// across an entire buffer -- it was previously recomputed from scratch inside render_unison_saw
// on every single sample, for every voice, for both oscillators (up to 64 voices * 128 samples
// * 2 oscillators * 7 unisons = ~114k needless trig calls per audio block). Called once per
// render_audio_block instead, and the resulting tables are handed to render_unison_saw.
static void compute_unison_saw_pan(float width_pct, float pan_l[UNISON_SAW_VOICES], float pan_r[UNISON_SAW_VOICES]) {
    float width_frac = width_pct / 100.0f;
    for (int u = 0; u < UNISON_SAW_VOICES; u++) {
        float pan_pos = UNISON_SAW_VOICE_SPREAD[u] * width_frac;
        pan_l[u] = cosf((pan_pos + 1.0f) * (M_PI / 4.0f));
        pan_r[u] = sinf((pan_pos + 1.0f) * (M_PI / 4.0f));
    }
}

float render_unison_saw(float phases[UNISON_SAW_VOICES], float base_dt, float detune_cents, const float pan_l[UNISON_SAW_VOICES], const float pan_r[UNISON_SAW_VOICES], float phase_offset, float *out_l, float *out_r) {
    float sum_mono = 0.0f, sum_l = 0.0f, sum_r = 0.0f;
    for (int u = 0; u < UNISON_SAW_VOICES; u++) {
        float dt = unison_saw_voice_dt(base_dt, detune_cents, u);
        // Phase Modulation shifts only the READ point in the cycle, not the stored accumulator
        // below -- so it never disturbs this voice's actual detune/pitch tracking.
        float read_phase = fmodf(phases[u] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
        float raw_saw = (read_phase * 2.0f) - 1.0f;
        float sample = raw_saw - poly_blep_step(read_phase, dt);
        sum_mono += sample;
        sum_l += sample * pan_l[u];
        sum_r += sample * pan_r[u];

        phases[u] += dt;
        if (phases[u] >= 1.0f) phases[u] -= 1.0f;
    }
    // 1/sqrt(N), not 1/N: the voices are detuned and start at random phases, so they add up in
    // power (level ~ sqrt(N) x one voice), not in amplitude. Dividing by N left the supersaw
    // ~8.5 dB below a plain saw; this puts it level with one (measured, held note).
    const float inv_n = 0.37796447f; // 1/sqrt(UNISON_SAW_VOICES)
    _Static_assert(UNISON_SAW_VOICES == 7, "update inv_n to 1/sqrt(UNISON_SAW_VOICES)");
    if (out_l) *out_l = sum_l * inv_n;
    if (out_r) *out_r = sum_r * inv_n;
    return sum_mono * inv_n;
}

// wave_type: 0=Triangle, 1=Sine, 2=Rev Saw (descending ramp), 3=Square, 4=Off (never reaches here --
// callers short-circuit type==4 before calling), 5=Saw (ascending ramp -- appended after Off, same
// "don't shift existing indices" reasoning as WaveType's own WAVE_REVSAW). Rising vs. falling matters
// here even though it wouldn't for a lone audio oscillator (see WAVE_REVSAW's own comment): an LFO
// driving, say, filter cutoff gives an audibly opposite contour depending on which ramp direction is
// chosen -- rise-then-snap vs. snap-then-rise -- the classic reason hardware like the Moog Model D/
// Roland SE-02 offers a dedicated reverse-saw option on a modulation oscillator. curve_drive: same
// single-pot, shape-reinterpreted value as lookup_osc_sample's own -- meaningful for Triangle/Saw
// (bipolar Curve) and Sine (unipolar Drive, clamped internally), ignored for Square.
float lookup_lfo_waveform(int type, float phase, float curve_drive) {
    switch (type) {
        case 1: return apply_sine_drive(sinf(phase * 2.0f * M_PI), curve_drive);
        case 2: { float warped = apply_wave_curve(1.0f - phase, curve_drive); return (warped * 2.0f) - 1.0f; }
        case 3: return (phase < 0.5f) ? 1.0f : -1.0f;
        case 5: { float warped = apply_wave_curve(phase, curve_drive); return (warped * 2.0f) - 1.0f; }
        case 0:
        default: {
            if (curve_drive == 0.0f) return (phase < 0.5f) ? (phase * 4.0f) - 1.0f : 3.0f - (phase * 4.0f);
            bool rising = phase < 0.5f;
            float sub = rising ? (phase * 2.0f) : ((phase - 0.5f) * 2.0f);
            float warped = apply_wave_curve(sub, curve_drive);
            return rising ? (warped * 2.0f - 1.0f) : (1.0f - warped * 2.0f);
        }
    }
}

// Renders one sample of preview playback for a single Sequencer audio lane (see the "Preview"
// button comment on SeqLaneState.audio_preview_trigger), applying the lane's own volume/pan
// knobs, and advances its play_frame. Mono/stereo pan law mirrors this file's existing
// convention: a mono source uses the plain (un-normalized) cos/sin law used for oscillator pan
// (correct there since it's placing a mono source into stereo -- reads ~-3dB at center, which is
// the point); an already-stereo source uses the sqrt(2)-normalized law used for Filter A/B's
// Parallel combine (reads unity at center, since attenuating an already-stereo signal at its
// default pan position would be a real level regression, not a style choice). Sets is_playing
// false once the buffer's end is reached (checked both before and after, so a lane that was
// already finished contributes silence this call, and one that finishes mid-call stops cleanly
// for the next).
static void render_seq_audio_preview_sample(SeqAudioLane *lane, float volume_0_100, float pan, float *out_l, float *out_r) {
    *out_l = 0.0f; *out_r = 0.0f;
    if (!lane->is_playing || lane->play_frame >= lane->frame_count) { lane->is_playing = false; return; }

    float vol = fmaxf(0.0f, volume_0_100) / 100.0f;
    float clamped_pan = fmaxf(-1.0f, fminf(1.0f, pan));
    float angle = (clamped_pan + 1.0f) * ((float)M_PI / 4.0f);
    long frame_base = lane->play_frame * lane->channels;

    if (lane->channels >= 2) {
        float gain_l = cosf(angle) * 1.41421356f;
        float gain_r = sinf(angle) * 1.41421356f;
        *out_l = lane->pcm[frame_base] * gain_l * vol;
        *out_r = lane->pcm[frame_base + 1] * gain_r * vol;
    } else {
        float in_mono = lane->pcm[frame_base];
        *out_l = in_mono * cosf(angle) * vol;
        *out_r = in_mono * sinf(angle) * vol;
    }

    lane->play_frame++;
    if (lane->play_frame >= lane->frame_count) lane->is_playing = false;
}

// Renders one oscillator (any of Osc 1-4) for one voice for one sample, generalizing what used
// to be separate Osc1-only (FM carrier) / Osc2-only (FM modulator + hard-sync slave) code into a
// single shared path all 4 oscillators run through identically. `modulator_sum` is the sum of
// every incoming FM edge's (source oscillator's own modulator output * that edge's depth) for
// THIS oscillator this sample -- 0.0f when nothing feeds it (e.g. an independent operator, or
// any oscillator under FM Algorithm 11 "All independent"), in which case this call reproduces
// that oscillator's plain unmodulated output bit-for-bit: the phase-delta-based adaptive BLEP
// width collapses to exactly `dt` every sample (see the fmaxf(...,dt) floor below, applied
// last -- the same proof shape as the original Osc1-only fm_dt1), matching lookup_osc_sample's
// own dt exactly. `do_sync`/`t_wrap` is this oscillator's own hard-sync trigger this sample
// (always false/0 with hard_sync_mode Off, or for a sync master, which is never called with
// do_sync=true for itself). `unison_pan_l/r` and `unison_saw_phases` are only read when
// shape==WAVE_UNISON_SAW; `sample_stream/ptr/preroll` only when shape==WAVE_SAMPLE.
// `pulse_or_tilt_input` is the oscillator's own dual-duty Pulse-Width/Tilt knob value already
// summed with any modulation, in its ORIGINAL knob range ([2,98] for Pulse Width percent or
// [-100,100] for Tilt) -- this function applies the /100 scaling itself so callers never need to
// know which of the two meanings currently applies.
static float render_fm_oscillator(
    HdsSampleEngine *se,
    WaveType shape, float own_phase, float dt, float phase_offset, float modulator_sum,
    float *prev_fm_phase, float pulse_or_tilt_input, float *tri_state, float *tilt_lp,
    float noise_tilt_coeff, float unison_saw_phases[UNISON_SAW_VOICES], float unison_detune_final,
    const float unison_pan_l[UNISON_SAW_VOICES], const float unison_pan_r[UNISON_SAW_VOICES],
    int *sample_stream, double *sample_ptr, bool *sample_preroll, float hz,
    bool do_sync, float t_wrap, float *pending_sync_correction,
    bool mono_noise_active, float mono_noise_value,
    float curve_drive,
    float *out_side_l, float *out_side_r)
{
    if (shape == WAVE_UNISON_SAW) {
        return render_unison_saw(unison_saw_phases, dt, unison_detune_final, unison_pan_l, unison_pan_r, phase_offset, out_side_l, out_side_r);
    }
    if (shape == WAVE_SAMPLE) {
        return render_sample_osc(se, sample_stream, sample_ptr, sample_preroll, hz, out_side_l, out_side_r);
    }

    float mod_phase = own_phase + modulator_sum;
    mod_phase = fmodf(mod_phase + phase_offset, 1.0f); if (mod_phase < 0.0f) mod_phase += 1.0f;

    // FM-aware adaptive PolyBLEP window width, generalized from the original Osc1-only fm_dt1:
    // the wrapped finite difference between this sample's and the previous sample's fully-
    // computed mod_phase estimates the true instantaneous phase velocity under modulation, capped
    // at 0.499f (raw modulation term only) with the fmaxf(...,dt) floor applied LAST.
    float phase_delta = mod_phase - *prev_fm_phase;
    phase_delta -= roundf(phase_delta);
    *prev_fm_phase = mod_phase;
    float adaptive_dt = fmaxf(fminf(fabsf(phase_delta), 0.499f), dt);

    if (shape == WAVE_NOISE) {
        *out_side_l = *out_side_r = 0.0f;
        // Monophonic Noise: every voice reads the exact same precomputed sample this sample
        // (computed once per Program+oscillator, before the voice loop -- see mono_noise_out[]
        // in render_audio_block) instead of each voice running its own independent generator/
        // filter below. This voice's own tilt_lp state is simply left untouched while mono is
        // active, so it picks up wherever it left off if the checkbox is ever turned back off.
        if (mono_noise_active) return mono_noise_value;
        // Tilt-shaped noise (unified across all 4 oscillators -- see the Tilt-noise decision):
        // white noise through a one-pole low-pass; tilt_frac in [-1,1] crossfades low-passed
        // (dark, -1) against its high-passed complement (bright/hissy, +1), 0 = flat white
        // (low+high sums back to the original signal, unity gain).
        float white = (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
        *tilt_lp += noise_tilt_coeff * (white - *tilt_lp);
        float tilt_high = white - *tilt_lp;
        float tilt_frac = fmaxf(-1.0f, fminf(1.0f, pulse_or_tilt_input / 100.0f));
        return *tilt_lp * (1.0f - tilt_frac) + tilt_high * (1.0f + tilt_frac);
    }

    float out = lookup_osc_sample(shape, mod_phase, adaptive_dt, pulse_or_tilt_input / 100.0f, tri_state, curve_drive);

    // Full alias-free hard sync amplitude correction (Saw/Square/Pulse only), generalized from
    // the original Osc2-only block -- see that block's own original comment (still present on
    // the mixer's history) for the full derivation. Consumes any "after" correction left pending
    // by a sync detected on the PREVIOUS sample, then, if THIS sample detects a new sync,
    // BLEP-corrects the discontinuity that reset will create.
    out += *pending_sync_correction;
    *pending_sync_correction = 0.0f;
    if (do_sync && (shape == WAVE_SAW || shape == WAVE_REVSAW || shape == WAVE_SQUARE || shape == WAVE_PULSE)) {
        float phase_at_sync = fmodf(own_phase + t_wrap * dt, 1.0f); if (phase_at_sync < 0.0f) phase_at_sync += 1.0f;
        float val_before, val_after;
        if (shape == WAVE_SAW) { val_before = phase_at_sync * 2.0f - 1.0f; val_after = -1.0f; }
        // Mirror image of WAVE_SAW's own case: the ramp runs from +1 (at phase_at_sync=0) down to
        // -1 (as phase_at_sync->1), and a sync reset snaps phase back to 0 -- i.e. back UP to +1 --
        // so val_after is +1 here where WAVE_SAW's own is -1.
        else if (shape == WAVE_REVSAW) { val_before = 1.0f - phase_at_sync * 2.0f; val_after = 1.0f; }
        else {
            float duty = (shape == WAVE_SQUARE) ? 0.5f : fmaxf(0.02f, fminf(0.98f, pulse_or_tilt_input / 100.0f));
            val_before = (phase_at_sync < duty) ? 1.0f : -1.0f; val_after = 1.0f;
        }
        float half_height = (val_after - val_before) * 0.5f;
        out += half_height * sync_blep_shape_before(t_wrap);
        *pending_sync_correction = half_height * sync_blep_shape_after(t_wrap);
    }
    *out_side_l = out; *out_side_r = out; // no independent side-channel outside Unison Saw/Sample
    return out;
}

// --- FM Algorithm system: a fixed, hand-verified-acyclic set of 12 selectable 4-operator FM
// routing topologies -- deliberately NOT a general routing matrix (which risks cycles no fixed
// per-sample ordering could resolve) and NOT a DX7-style feedback/self-modulation trick
// (unnecessary complexity at only 4 operators). Oscillator indices below are 0-based (0=Osc1,
// 1=Osc2, 2=Osc3, 3=Osc4). Algorithm 0 ("Classic") is byte-identical to this engine's original
// fixed Osc2->Osc1 FM pair and stays the default. Each row's `render_order` MUST list every
// edge's `from_osc` before its `to_osc` (verified once at startup by
// assert_fm_algorithms_acyclic(), below) -- the dispatch loop in render_audio_block walks
// render_order in order, and for each oscillator sums its incoming edges' (already-rendered
// source's modulator output * depth) before rendering that oscillator itself. ---
typedef struct {
    int from_osc;
    int to_osc;
    size_t depth_offset; // offsetof(SynthSharedControls, fm_intensity/fm_depth_*)
} FmEdge;

typedef struct {
    int render_order[4];
    int edge_count;
    FmEdge edges[3];
} FmAlgorithm;

#define FM_ALGORITHM_COUNT 12
static const FmAlgorithm FM_ALGORITHMS[FM_ALGORITHM_COUNT] = {
    // 0: Classic (default) -- 2->1
    { {1,0,2,3}, 1, { {1,0, offsetof(SynthSharedControls, programs[0].fm_intensity)} } },
    // 1: 3-Op Chain, Osc4 independent -- 3->2->1
    { {2,1,0,3}, 2, { {2,1, offsetof(SynthSharedControls, programs[0].fm_depth_3_2)}, {1,0, offsetof(SynthSharedControls, programs[0].fm_intensity)} } },
    // 2: 4-Op Chain -- 4->3->2->1
    { {3,2,1,0}, 3, { {3,2, offsetof(SynthSharedControls, programs[0].fm_depth_4_3)}, {2,1, offsetof(SynthSharedControls, programs[0].fm_depth_3_2)}, {1,0, offsetof(SynthSharedControls, programs[0].fm_intensity)} } },
    // 3: Dual Serial Pairs -- 4->3, 2->1
    { {3,2,1,0}, 2, { {3,2, offsetof(SynthSharedControls, programs[0].fm_depth_4_3)}, {1,0, offsetof(SynthSharedControls, programs[0].fm_intensity)} } },
    // 4: Many-to-One into 1 -- 3->1, 4->1
    { {2,3,0,1}, 2, { {2,0, offsetof(SynthSharedControls, programs[0].fm_depth_3_1)}, {3,0, offsetof(SynthSharedControls, programs[0].fm_depth_4_1)} } },
    // 5: One-to-Many from 3 -- 3->1, 3->2
    { {2,0,1,3}, 2, { {2,0, offsetof(SynthSharedControls, programs[0].fm_depth_3_1)}, {2,1, offsetof(SynthSharedControls, programs[0].fm_depth_3_2)} } },
    // 6: Many-to-One into 2 -- 1->2, 3->2
    { {0,2,1,3}, 2, { {0,1, offsetof(SynthSharedControls, programs[0].fm_depth_1_2)}, {2,1, offsetof(SynthSharedControls, programs[0].fm_depth_3_2)} } },
    // 7: 4->1 direct, 2/3 independent
    { {3,0,1,2}, 1, { {3,0, offsetof(SynthSharedControls, programs[0].fm_depth_4_1)} } },
    // 8: Dual Independent Pairs -- 1->2, 3->4
    { {0,1,2,3}, 2, { {0,1, offsetof(SynthSharedControls, programs[0].fm_depth_1_2)}, {2,3, offsetof(SynthSharedControls, programs[0].fm_depth_3_4)} } },
    // 9: 1->3->4, Osc2 independent
    { {0,2,3,1}, 2, { {0,2, offsetof(SynthSharedControls, programs[0].fm_depth_1_3)}, {2,3, offsetof(SynthSharedControls, programs[0].fm_depth_3_4)} } },
    // 10: Star: 2/3/4 all into 1
    { {1,2,3,0}, 3, { {1,0, offsetof(SynthSharedControls, programs[0].fm_intensity)}, {2,0, offsetof(SynthSharedControls, programs[0].fm_depth_3_1)}, {3,0, offsetof(SynthSharedControls, programs[0].fm_depth_4_1)} } },
    // 11: All independent
    { {0,1,2,3}, 0, { {0,0,0}, {0,0,0}, {0,0,0} } },
};

// Debug-only startup sanity check (see main()): confirms every algorithm row's edges only ever
// point from an earlier render_order slot to a later one -- a bad hand-edited row would otherwise
// silently read a not-yet-rendered oscillator's stale modulator_output from the previous sample
// instead of failing loudly.
void assert_fm_algorithms_acyclic(void) {
    for (int a = 0; a < FM_ALGORITHM_COUNT; a++) {
        const FmAlgorithm *alg = &FM_ALGORITHMS[a];
        int position[4];
        for (int slot = 0; slot < 4; slot++) position[alg->render_order[slot]] = slot;
        for (int e = 0; e < alg->edge_count; e++) {
            if (position[alg->edges[e].from_osc] >= position[alg->edges[e].to_osc]) {
                fprintf(stderr, "[fm] FATAL: FM_ALGORITHMS[%d] edge %d->%d violates render order\n", a, alg->edges[e].from_osc, alg->edges[e].to_osc);
                abort();
            }
        }
    }
}

// Looks up the currently-claimed zone's own Pan setting for a WAVE_SAMPLE oscillator, so
// render_audio_block's pan1/pan2 computation (which runs BEFORE render_sample_osc is called each
// sample, since it also feeds the periodic-oscillator branches) can fold it in without needing
// render_sample_osc itself to return anything beyond its plain mono sample. Mirrors
// render_sample_osc's own stream->slot lookup exactly (same relaxed atomic read of slot_index) --
// a small duplicated lookup rather than threading pan out through render_sample_osc's signature,
// since that function is shared with the unison-saw/periodic-wave branches which depend on state
// (fm_phase, phase_offset) not available until later in the per-voice loop; special-casing WAVE_
// SAMPLE there to emit early would be a bigger, riskier change than this one small duplication.
static float sample_osc_zone_pan_offset(HdsSampleEngine *se, int stream_idx) {
    if (stream_idx < 0) return 0.0f;
    SampleStream *stream = &se->streams->sample_streams[stream_idx];
    if (!stream->in_use) return 0.0f;
    int slot_idx = atomic_load_explicit(&stream->slot_index, memory_order_relaxed);
    if (slot_idx < 0 || slot_idx >= SAMPLE_SLOT_CAPACITY) return 0.0f;
    SampleSlot *slot = &se->slots->sample_slots[slot_idx];
    return slot->loaded ? slot->pan : 0.0f;
}

// LFO Rate/Depth/Phase-as-mod-destination target -> LFO index mapping. Each of the three
// destination "families" spans two disjoint target-number ranges -- the original LFO 1-4 slots
// (14-17/18-21/28-31, from before LFO_COUNT grew to 16) and the LFO 5-16 slots appended after
// target 47 (48-59/60-71/72-83, never renumbering anything live) -- so one small helper per
// family maps either range back to a single 0-15 LFO index; returns -1 if target isn't in either
// range for that family. LFO 1-4 Rate/Depth (14-21) were previously unwired dead targets in the
// dispatch loops below despite being selectable in the frontend's destination dropdown -- this
// pass wires them up for real, for all 16 LFOs, alongside Phase (which LFOs 1-4 already had).
static inline int lfo_rate_target_index(int target) {
    if (target >= 14 && target <= 17) return target - 14;
    if (target >= 48 && target <= 59) return target - 44;
    return -1;
}
static inline int lfo_depth_target_index(int target) {
    if (target >= 18 && target <= 21) return target - 18;
    if (target >= 60 && target <= 71) return target - 56;
    return -1;
}
static inline int lfo_phase_target_index(int target) {
    if (target >= 28 && target <= 31) return target - 28;
    if (target >= 72 && target <= 83) return target - 68;
    return -1;
}

// LFO Tempo Sync divisions -- how many beats (quarter notes) one full LFO cycle takes at each
// selectable division. Ordered by note NAME (2 Bars / 2 Bars Dotted / 2 Bars Triplet, then 1 Bar
// / ../.., down to 1/64 / ../..), matching the sync-rate menu convention most DAWs/synths already
// use, rather than sorted strictly by the resulting speed (a dotted note and the next triplet
// down can interleave in raw speed terms -- irrelevant to how a musician looks this up by name).
// A straight note's own length in beats is halved at each step (2 bars = 8 beats down to 1/64 =
// 1/16 beat); dotted = 1.5x that length, triplet = 2/3 of it -- the standard definitions.
// PackLfoParams.tempo_sync_division is 0 = Free (rate_hz used directly, today's only behavior),
// 1..LFO_SYNC_DIVISION_COUNT = a 1-based index into this table -- see resolve_lfo_rate_hz().
#define LFO_SYNC_DIVISION_COUNT 24
static const float LFO_SYNC_BEATS_PER_CYCLE[LFO_SYNC_DIVISION_COUNT] = {
    8.0f,      12.0f,     5.333333f,  // 2 Bars, 2 Bars Dotted, 2 Bars Triplet
    4.0f,      6.0f,      2.666667f,  // 1 Bar,  1 Bar Dotted,  1 Bar Triplet
    2.0f,      3.0f,      1.333333f,  // 1/2,    1/2 Dotted,    1/2 Triplet
    1.0f,      1.5f,      0.666667f,  // 1/4,    1/4 Dotted,    1/4 Triplet
    0.5f,      0.75f,     0.333333f,  // 1/8,    1/8 Dotted,    1/8 Triplet
    0.25f,     0.375f,    0.166667f,  // 1/16,   1/16 Dotted,   1/16 Triplet
    0.125f,    0.1875f,   0.083333f,  // 1/32,   1/32 Dotted,   1/32 Triplet
    0.0625f,   0.09375f,  0.041667f,  // 1/64,   1/64 Dotted,   1/64 Triplet
};

// Resolves one LFO's effective rate in Hz for this block/sample. Free mode (tempo_sync_division
// <= 0) reproduces the exact pre-existing formula (rate_hz + a mod-matrix rate offset, clamped to
// [0.1, 200] -- the same clamp already applied unconditionally before this helper existed, kept
// identical so it still bounds a runaway Synced rate too). Synced mode instead derives Hz from
// tempo_bpm_smoothed (the same block-granularity smoothed tempo poll_arpeggiator's own
// arp_tempo_smoothed already uses, to avoid a hard discontinuity in an LFO's own cycle rate on a
// fast Tempo-knob move) and the selected note division. rate_mod_state is the raw, un-multiplied
// one-sample-delayed mod-matrix value (lfo_rate_mod_state[p][l] at the Global call site,
// compute_per_key_lfo_output's own rate_mod parameter at the Per-Key one) -- the existing *20.0f
// scale is applied here, once, rather than duplicated at each of this function's two call sites.
static float resolve_lfo_rate_hz(const PackLfoParams *lp, float rate_mod_state, float tempo_bpm_smoothed) {
    int sync_idx = (int)lp->tempo_sync_division;
    float base_hz;
    if (sync_idx <= 0) {
        base_hz = lp->rate_hz;
    } else {
        int div_idx = sync_idx - 1;
        if (div_idx >= LFO_SYNC_DIVISION_COUNT) div_idx = LFO_SYNC_DIVISION_COUNT - 1;
        base_hz = tempo_bpm_smoothed / (60.0f * LFO_SYNC_BEATS_PER_CYCLE[div_idx]);
    }
    return fmaxf(0.1f, fminf(200.0f, base_hz + rate_mod_state * 20.0f));
}

// Per-Key LFO (see PackLfoParams.per_key_mode): computes one sample of ONE LFO for ONE voice,
// using that voice's own phase/decimation state (voice->lfo_phase/lfo_decim_*) instead of the
// Program-shared global_lfo_phases/lfo_out this same math normally reads/writes -- see
// render_audio_block's per-voice LFO dispatch, the only caller. ALWAYS decimated (mirrors the
// Program-level computation's LFO-3-16 branch exactly, see LFO_DECIMATION_FACTOR), even for LFO
// slots 1/2 which are full audio-rate in Global mode -- per-voice computation multiplies cost by
// polyphony, so Per-Key mode always pays the cheap path, never the exact one, regardless of slot.
// rate_mod/depth_mod/phase_mod are the exact same one-sample-delayed Program-level cross-
// modulation state (lfo_rate_mod_state/lfo_depth_mod_state/lfo_phase_mod_state) every LFO --
// Global or Per-Key -- already reads; a source LFO/EG modulating "this LFO's Rate/Depth/Phase"
// contributes the same broadcast value to every voice's own independent Per-Key cycle rather
// than needing its own per-voice derivation (a deliberate simplification: the source itself may
// well be a Global-mode LFO with no per-voice identity of its own to begin with).
static float compute_per_key_lfo_output(SynthVoice *voice, const PackLfoParams *lp, int lfo_idx,
                                          float rate_mod, float depth_mod, float phase_mod,
                                          const MidiLiveState *midi_live, float tempo_bpm_smoothed) {
    int wave_type = (int)lp->wave_type;
    if (wave_type == 4) return 0.0f; // Off
    float effective_mod_depth = fmaxf(0.0f, fminf(1.0f, lp->mod_depth + depth_mod));
    // Keyboard tracking: +-100 = a full octave-per-octave-of-keyboard multiplier, centered on
    // note 60 (middle C). Velocity tracking: linear, centered on velocity 64 ("normal"). Applied
    // on top of resolve_lfo_rate_hz's own result -- key/vel tracking scales whatever base rate
    // resolves, Free Hz or Tempo Synced alike.
    float key_mult = powf(2.0f, ((float)(voice->midi_note - 60) / 12.0f) * (lp->key_track / 100.0f));
    float vel_mult = 1.0f + (lp->vel_track / 100.0f) * ((float)(voice->midi_velocity - 64) / 63.0f);
    float effective_rate_hz = resolve_lfo_rate_hz(lp, rate_mod, tempo_bpm_smoothed) * key_mult * vel_mult;

    if (voice->lfo_decim_counter[lfo_idx] == 0) {
        voice->lfo_decim_last_value[lfo_idx] = voice->lfo_decim_target_value[lfo_idx];
        float phase_offset = fmodf((lp->phase_offset / 360.0f) + phase_mod, 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
        float read_phase = fmodf(voice->lfo_phase[lfo_idx] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
        int mod_src = (int)lp->mod_source_cc;
        float cc_source_scale = 1.0f;
        if (mod_src == 128) cc_source_scale = midi_live->channel_pressure;
        else if (mod_src >= 0 && mod_src < 128) cc_source_scale = midi_live->cc[mod_src];
        voice->lfo_decim_target_value[lfo_idx] = lookup_lfo_waveform(wave_type, read_phase, lp->curve_drive) * effective_mod_depth * cc_source_scale;
        voice->lfo_phase[lfo_idx] += (effective_rate_hz / (float)SAMPLE_RATE) * (float)LFO_DECIMATION_FACTOR;
        if (voice->lfo_phase[lfo_idx] >= 1.0f) voice->lfo_phase[lfo_idx] = fmodf(voice->lfo_phase[lfo_idx], 1.0f);
    }
    float frac = (float)voice->lfo_decim_counter[lfo_idx] / (float)LFO_DECIMATION_FACTOR;
    float out = voice->lfo_decim_last_value[lfo_idx] + (voice->lfo_decim_target_value[lfo_idx] - voice->lfo_decim_last_value[lfo_idx]) * frac;
    voice->lfo_decim_counter[lfo_idx] = (voice->lfo_decim_counter[lfo_idx] + 1) % LFO_DECIMATION_FACTOR;
    return out;
}

// Cascades all 3 EQ bands (Low Shelf -> Mid Peak -> High Shelf) over both channels of one
// Program's already-mixed/filtered bus -- see render_audio_block's "Multi-source bus dispatch"
// section for the one call site (right after each Program's bus_l/bus_r is computed, before FX).
static void apply_program_eq(HdsEngineState *engine, int p, const EqBiquadCoeffs coeffs[3], float *l, float *r) {
    for (int b = 0; b < 3; b++) {
        *l = run_eq_biquad(*l, &coeffs[b], &engine->eq_z1[p][b][0], &engine->eq_z2[p][b][0]);
        *r = run_eq_biquad(*r, &coeffs[b], &engine->eq_z1[p][b][1], &engine->eq_z2[p][b][1]);
    }
}

// per_program_out (NULL-able): when non-NULL, receives this block's fully-processed (post-EQ,
// post-FX -- the exact same signal Combi Mix itself sums) per-Combi-Program stereo bus, laid out
// as per_program_out[frame * COMBI_PROGRAM_COUNT * 2 + program * 2 + channel] -- i.e. the caller
// owns a buffer of at least (frames handed to this call) * COMBI_PROGRAM_COUNT * 2 floats. Exists
// for the CLAP plugin's multi-out audio ports (see clap_audio_ports_get() in clap_plugin.c) --
// the standalone app has no equivalent need (its own Routing tab already sends any Program to any
// real hardware output pair via out_buffer/output_channels) and always passes NULL here, at zero
// extra cost beyond one null check per sample.
// gr_db_out[FX_SLOT_COUNT] (required, not NULL-able -- same convention as ext_in_peak_out just
// above): each slot's current gain-reduction reading in dB (0 for a bypassed/off/non-GR-capable
// slot). Unlike ext_in_peak_out/dump_avail_out, this is NOT a raw per-block value overwritten
// from scratch every call -- it's persistent, caller-owned METER-BALLISTICS state, chased toward
// this block's own raw last-sample reading (a local scratch array, gr_db_raw, is what actually
// gets zeroed-and-refilled fresh every call and handed to process_fx_chain) via the same per-block
// exponential chase idiom lfo_tempo_smoothed/eg_sustain_chase_rate already use elsewhere in this
// function, just with its own fast-attack/slow-release time constants (see the chase itself,
// right before this function's own `return peak`). Needed because the raw reading is the true
// INSTANTANEOUS envelope value as of the last sample rendered -- exactly right for the DSP itself,
// but for a fast compressor (Gate, Limiter) it changes visibly every single redraw of a live
// meter, too fast for a human to actually read the dB number off of (reported directly: "the
// readouts are so fast I can't actually see the number of decibels"). Smoothing only the DISPLAY
// copy here, not the real last_gr_db each effect's own detector/gain-computer uses internally,
// keeps the actual compression sound completely unaffected -- this is purely a meter readability
// fix.
// dump_avail_out[FX_SLOT_COUNT] (required, not NULL-able -- same convention/lifetime as
// ext_in_peak_out): each FX_TYPE_BROADCAST_DELAY slot's currently-buffered delay, in seconds (0
// for every other type). Deliberately NOT smoothed like gr_db_out above -- it already changes
// slowly and smoothly on its own (a double-precision accumulator ramping toward a target, never a
// fast audio-rate envelope), so a raw per-block reading is already perfectly readable.
float render_audio_block(HdsSampleEngine *se, HdsEngineState *engine, SynthVoice voices[], const SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], const float *ext_in_buffer, int ext_in_channels, int output_channels, const MidiLiveState *midi_live, float *out_buffer, float current_tempo_bpm, float ext_in_peak_out[EXT_IN_PAIR_COUNT], float *per_program_out, float gr_db_out[FX_SLOT_COUNT], float dump_avail_out[FX_SLOT_COUNT]) {
    float peak = 0.0f;
    float gr_db_raw[FX_SLOT_COUNT]; // this block's own raw, unsmoothed readings -- see gr_db_out's own comment above
    for (int s = 0; s < FX_SLOT_COUNT; s++) gr_db_raw[s] = 0.0f;
    for (int s = 0; s < FX_SLOT_COUNT; s++) dump_avail_out[s] = 0.0f;
    // Routing tab's per-ext-in-pair VU meters (see gui_routing_tab_visible's own comment): a true
    // no-op -- not just "computed but ignored" -- whenever the Routing tab isn't on-screen, so
    // this costs nothing for the life of the process unless a player is actually looking at it.
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) ext_in_peak_out[p] = 0.0f;
    bool meter_ext_in = (knobs->gui_routing_tab_visible != 0.0f);
    // Combi mode: how many of the 8 Program slots are actually "live" this block -- 1 (just
    // program 0) outside Combi mode, all 8 inside it. Every per-program precompute below loops
    // over this many slots, NOT over which voices happen to be currently sounding -- LFOs must
    // keep advancing continuously per Program regardless of note activity (so phase doesn't
    // reset/freeze while idle, exactly like today's single-Program behavior), and the cost of
    // computing for an idle Program slot is negligible next to gating logic that could regress
    // that phase-continuity guarantee. Outside Combi mode this is always 1 -- byte-identical cost
    // and behavior to before this pass.
    int active_program_count = ((int)knobs->performance_mode == 1) ? COMBI_PROGRAM_COUNT : 1;

    // Each oscillator is panned to stereo AT THE MIXER, before the signal reaches the filter
    // chain -- each of the 5 filter slots then runs as two fully independent mono filters (one
    // per channel, see voices[v].f_low/f_band's [channel] dimension) so that placement survives
    // filtering instead of collapsing to mono first. Pan is a mod destination (targets
    // 32/33/37/40 -- see the per-voice mod accumulation below), so unlike most per-block
    // constants here it's computed per-voice, per-sample instead of once for the whole buffer.

    // PERF: Unison Saw's per-unison pan (cosf/sinf) only depends on the Width knob, which is
    // constant across the whole buffer -- computed once here instead of every sample, for
    // every voice, for both oscillators. See compute_unison_saw_pan's comment for the numbers.
    // Per-Program now (one Program's Width knob doesn't affect another's) -- computed for every
    // Program slot unconditionally (not just "active" ones): 8 Programs x 4 oscillators x one
    // cheap cosf/sinf fill is negligible once per block, and avoids needing active_program_count
    // gating here at all.
    float vco1_unison_pan_l[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES], vco1_unison_pan_r[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES];
    float vco2_unison_pan_l[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES], vco2_unison_pan_r[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES];
    float vco3_unison_pan_l[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES], vco3_unison_pan_r[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES];
    float vco4_unison_pan_l[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES], vco4_unison_pan_r[COMBI_PROGRAM_COUNT + 1][UNISON_SAW_VOICES];
    // <= COMBI_PROGRAM_COUNT (not <): covers slot SINGLE_PROGRAM_SLOT too, so Single Program
    // mode's own precompute is valid wherever this table is read by program index later --
    // otherwise slot 8 would sit at all-zero and Single Program mode's Unison Saw pan would
    // silently read Combi lane 1's width knob (before this fix) or nothing at all.
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) {
        compute_unison_saw_pan(knobs->programs[p].vco1_unison_saw_width, vco1_unison_pan_l[p], vco1_unison_pan_r[p]);
        compute_unison_saw_pan(knobs->programs[p].vco2_unison_saw_width, vco2_unison_pan_l[p], vco2_unison_pan_r[p]);
        compute_unison_saw_pan(knobs->programs[p].vco3_unison_saw_width, vco3_unison_pan_l[p], vco3_unison_pan_r[p]);
        compute_unison_saw_pan(knobs->programs[p].vco4_unison_saw_width, vco4_unison_pan_l[p], vco4_unison_pan_r[p]);
    }

    // Noise Osc's tilt-color filter: a simple one-pole low/high split with a fixed ~800Hz pivot,
    // linearly crossfaded by the Tilt knob. The coefficient depends only on SAMPLE_RATE and the
    // fixed pivot, so it's a compile-time-equivalent constant -- computed once here too.
    const float noise_tilt_coeff = 1.0f - expf(-2.0f * (float)M_PI * 800.0f / (float)SAMPLE_RATE);

    // Pitch Bend: applies equally to both oscillators (like a real keyboard's wheel bends the
    // whole voice, not one oscillator), with separately specifiable up/down range in semitones.
    // midi_live->pitch_bend is one shared physical wheel (constant across this whole block), but
    // pb_range_up/down are Program-scoped -- so each Program's own bend-in-cents is precomputed
    // once per block here.
    float pb_bend_cents[COMBI_PROGRAM_COUNT + 1];
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) { // <= : covers SINGLE_PROGRAM_SLOT too, see the unison-pan loop's comment above
        pb_bend_cents[p] = (midi_live->pitch_bend >= 0.0f)
            ? midi_live->pitch_bend * knobs->programs[p].pb_range_up * 100.0f
            : midi_live->pitch_bend * knobs->programs[p].pb_range_down * 100.0f;
    }

    // Post-filter 3-band EQ coefficients (see EqBandParams/compute_eq_band_coeffs): the 3 knobs
    // per band are constant for the whole block (no mod-matrix destination targets them), so
    // computing each Program's biquad coefficients once here -- not once per sample -- is both
    // correct and far cheaper than redoing the trig 3 bands x up to 9 Programs x every sample.
    EqBiquadCoeffs eq_coeffs[COMBI_PROGRAM_COUNT + 1][3];
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) {
        for (int b = 0; b < 3; b++) eq_coeffs[p][b] = compute_eq_band_coeffs(&knobs->programs[p].eq_bands[b], b);
    }

    // One-sample-delayed feedback state for LFO Phase modulation: holds each LFO's phase-shift
    // contribution from other LFOs and from its own Program's reference voice's assignable EGs
    // (voices[p] -- see the LFO-as-destination block below for why that's the reference voice).
    // Per-Program (LFOs are a resource shared across every voice OF THE SAME PROGRAM, not across
    // Programs). Was a function-local `static` array (safe only because this program ran exactly
    // one synth engine instance per process) -- now an alias bound to g_engine's own copy, part
    // of the multi-instance refactor (see HdsEngineState's own comment). Every read/write site
    // below keeps using the bare name unchanged; only where the storage lives changed.
    float (*lfo_phase_mod_state)[LFO_COUNT] = engine->lfo_phase_mod_state;
    // Same one-sample-delayed pattern, for LFO Rate and Depth as mod destinations (targets
    // 14-21/48-71 -- see lfo_rate_target_index/lfo_depth_target_index). Applied to rate_hz/
    // mod_depth in the main per-sample LFO loop below.
    float (*lfo_rate_mod_state)[LFO_COUNT] = engine->lfo_rate_mod_state;
    float (*lfo_depth_mod_state)[LFO_COUNT] = engine->lfo_depth_mod_state;

    // Control-rate decimation state for LFOs 3-16 (index 2-15) -- see LFO_DECIMATION_FACTOR's
    // comment. counter cycles 0..LFO_DECIMATION_FACTOR-1; at 0, last_value/target_value/phase are
    // all advanced by one full decimated step, then every sample in between linearly interpolates
    // from last_value to target_value. Per-Program, same reason as lfo_phase_mod_state above.
    int (*lfo_decim_counter)[LFO_COUNT] = engine->lfo_decim_counter;
    float (*lfo_decim_last_value)[LFO_COUNT] = engine->lfo_decim_last_value;
    float (*lfo_decim_target_value)[LFO_COUNT] = engine->lfo_decim_target_value;

    // Monophonic Noise tilt-filter state (see ProgramState.vco1_mono_noise's own comment) --
    // one shared generator per Program+oscillator, computed once per sample below (before the
    // voice loop) rather than once per voice.
    float (*mono_noise_tilt_lp)[4] = engine->mono_noise_tilt_lp;

    // FX bus engine state: persists across calls (and across FX_TYPE changes -- see
    // process_fx_chain) for the same "one synth engine instance" reason as above.
    FXSlotEngine *fx_engine = engine->fx_engine;
    if (!engine->fx_engine_initialized) { reset_fx_engine(fx_engine, FX_SLOT_COUNT); engine->fx_engine_initialized = true; }

    // Always-on master safety limiter gain, one per output pair (evaluated after every source
    // routed to that pair has been summed in -- see its use below). Persists across calls for the
    // same reason as the state above.
    float *limiter_gain = engine->limiter_gain;
    if (!engine->limiter_gain_initialized) {
        for (int p = 0; p < OUTPUT_PAIR_MAX; p++) limiter_gain[p] = 1.0f;
        engine->limiter_gain_initialized = true;
    }

    // Master Volume smoothing: chases the live knob value over a ~10ms time constant instead of
    // applying it instantly (see its use below) -- fixes an audible click on every Volume knob
    // move. Snaps straight to the current knob value on the very first call (not ramping up from
    // 0), same "initialize to the right starting point, don't fade in at startup" reasoning
    // limiter_gain/fx_engine's own init blocks already use just above.
    float *master_volume_smoothed = &engine->master_volume_smoothed;
    if (!engine->master_volume_smoothed_initialized) {
        *master_volume_smoothed = knobs->master_volume;
        engine->master_volume_smoothed_initialized = true;
    }
    const float master_volume_chase_rate = 1.0f - expf(-1.0f / (0.010f * SAMPLE_RATE));

    // FX slot type-change bookkeeping (free a slot's old effect state when its Type actually
    // changed) and "which sources own any FX slot at all" both only ever change in response to an
    // OSC control message -- drained once per BLOCK in main()'s loop, never mid-block -- so both
    // are computed here exactly once per block, not once per sample (previously this bookkeeping
    // ran inside process_fx_chain itself, once per SOURCE per SAMPLE: with many sources now
    // dispatched independently, that had silently become a per-sample-per-slot cost purely from
    // scanning for a type change that could not have happened since the last block anyway --
    // a real, measured underrun regression at any voice count, since it's a flat per-sample tax
    // paid regardless of patch complexity). See process_fx_chain()'s own comment for how the two
    // pieces fit together now.
    sync_fx_engine_types(knobs->fx, fx_engine, FX_SLOT_COUNT);
#if HDS_NAM_ENABLED
    // Same "once per block, right after sync_fx_engine_types" precondition as that call itself --
    // see poll_fx_nam_requests()'s own comment in fx_chain.c. Hoisted up here (rather than each
    // caller polling separately, the way hds_standalone.c's own main loop used to) so BOTH
    // hds_standalone.c and clap_plugin.c get working NAM model/cab-IR loading for free, the same
    // way they already share every other piece of render_audio_block -- this was the actual fix
    // for the CLAP-plugin-side gap fx_nam_reload_model/fx_nam_reload_ir's own comment used to
    // document as a known, unaddressed limitation.
    poll_fx_nam_requests(knobs->fx, fx_engine, FX_SLOT_COUNT);
#endif
    bool source_has_fx_slots[SOURCE_COUNT] = {false};
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        int src = (int)knobs->fx[s].fx_source;
        if (src >= 0 && src < SOURCE_COUNT) source_has_fx_slots[src] = true;
    }

    // Envelope rates: one EGDerivedRates per EG *config* per Program (ENV_COUNT of them each),
    // shared by every voice's own instance of that EG -- see compute_eg_derived_rates()'s own
    // comment for why this moved here (once per block) instead of being recomputed inside
    // tick_adsr() itself (once per voice per sample -- a redundancy that scaled directly, and
    // expensively, with both polyphony and envelope count). Computed for every Program slot
    // unconditionally, same reasoning as the unison-pan tables above (cheap, once per block).
    EGDerivedRates eg_rates[COMBI_PROGRAM_COUNT + 1][ENV_COUNT];
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) { // <= : covers SINGLE_PROGRAM_SLOT too, see the unison-pan loop's comment above
        for (int e = 0; e < ENV_COUNT; e++) {
            compute_eg_derived_rates(&knobs->programs[p].eg[e], (int)knobs->programs[p].eg_mode[e], &eg_rates[p][e]);
        }
    }
    // Sustain-chase rate: not even knob-dependent (a pure function of SAMPLE_RATE) -- a plain
    // local, recomputed every call, rather than a function-local `static` cached "once for the
    // life of the process" as before. Bit-identical result every call (same formula, same
    // compile-time-constant inputs), so this is a pure simplification with zero behavior change,
    // made possible by dropping the now-unnecessary per-instance cache as part of the
    // multi-instance refactor (this value never needed to be instance state at all).
    const float eg_sustain_chase_rate = 1.0f - expf(-1.0f / (0.015f * SAMPLE_RATE));

    int voice_limit = effective_voice_limit(knobs);
    int block_size = effective_block_size(knobs);

    // LFO Tempo Sync: chases current_tempo_bpm (already resolved once per block by the caller,
    // same compute_current_tempo_bpm() the arpeggiator uses) over the exact same ~120ms
    // block-granularity time constant poll_arpeggiator's own arp_tempo_smoothed already uses --
    // so a fast Tempo-knob move can't slam a Synced LFO's cycle rate into a discontinuity any
    // more than it can the arp's step clock. Snaps straight to the current tempo on the very
    // first call, same "don't fade in at startup" reasoning as master_volume_smoothed above.
    float *lfo_tempo_smoothed = &engine->lfo_tempo_smoothed;
    if (!engine->lfo_tempo_smoothed_initialized) {
        *lfo_tempo_smoothed = current_tempo_bpm;
        engine->lfo_tempo_smoothed_initialized = true;
    } else {
        const float lfo_tempo_chase_rate = 1.0f - expf(-(float)block_size / (SAMPLE_RATE * 0.120f));
        *lfo_tempo_smoothed += (current_tempo_bpm - *lfo_tempo_smoothed) * lfo_tempo_chase_rate;
    }

    // Portamento glide coefficient, per voice: depends on that voice's OWN Program's
    // portamento_time_ms (a knob, block-invariant) and voices[v].glide_time_multiplier (per-voice,
    // but only rerolled at a glide event in handle_note_on/off -- called once per block from
    // main()'s loop, never mid-block) -- so, same as the EG rates above, this only needs computing
    // once per block, not once per voice per SAMPLE. It was previously recomputed (with an expf()
    // call) on every sample for every active voice regardless of whether a glide was even in
    // progress -- for a held note between glide events that's potentially millions of samples of
    // pure waste.
    float portamento_coeff[MAX_VOICES];
    for (int v = 0; v < voice_limit; v++) {
        const ProgramState *voice_prog = &knobs->programs[voices[v].program_index];
        float effective_glide_ms = fmaxf(1.0f, voice_prog->portamento_time_ms * voices[v].glide_time_multiplier);
        portamento_coeff[v] = 1.0f - expf(-1.0f / ((effective_glide_ms / 1000.0f) * SAMPLE_RATE));
    }

    // Static-pan fallback: pan1_l/r..pan4_l/r (below, per voice per sample) reduce to just
    // that voice's own Program's vcoN_pan -- the same value for every voice OF THAT PROGRAM, every
    // sample -- whenever nothing is actively modulating that oscillator's pan (mod_vcoN_pan == 0,
    // the common case) and it isn't a WAVE_SAMPLE oscillator carrying its own zone pan offset.
    // Precomputed once per block, per Program slot (unconditionally, same reasoning as the
    // unison-pan tables above) so the per-voice per-sample loop can skip the cosf/sinf pair
    // entirely in that case instead of recomputing an unchanged angle up to MAX_VOICES x
    // block_size times for nothing.
    float static_pan1_l[COMBI_PROGRAM_COUNT + 1], static_pan1_r[COMBI_PROGRAM_COUNT + 1];
    float static_pan2_l[COMBI_PROGRAM_COUNT + 1], static_pan2_r[COMBI_PROGRAM_COUNT + 1];
    float static_pan3_l[COMBI_PROGRAM_COUNT + 1], static_pan3_r[COMBI_PROGRAM_COUNT + 1];
    float static_pan4_l[COMBI_PROGRAM_COUNT + 1], static_pan4_r[COMBI_PROGRAM_COUNT + 1];
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) { // <= : covers SINGLE_PROGRAM_SLOT too, see the unison-pan loop's comment above
        float angle1 = (fmaxf(-1.0f, fminf(1.0f, knobs->programs[p].vco1_pan)) + 1.0f) * ((float)M_PI / 4.0f);
        float angle2 = (fmaxf(-1.0f, fminf(1.0f, knobs->programs[p].vco2_pan)) + 1.0f) * ((float)M_PI / 4.0f);
        float angle3 = (fmaxf(-1.0f, fminf(1.0f, knobs->programs[p].vco3_pan)) + 1.0f) * ((float)M_PI / 4.0f);
        float angle4 = (fmaxf(-1.0f, fminf(1.0f, knobs->programs[p].vco4_pan)) + 1.0f) * ((float)M_PI / 4.0f);
        static_pan1_l[p] = cosf(angle1); static_pan1_r[p] = sinf(angle1);
        static_pan2_l[p] = cosf(angle2); static_pan2_r[p] = sinf(angle2);
        static_pan3_l[p] = cosf(angle3); static_pan3_r[p] = sinf(angle3);
        static_pan4_l[p] = cosf(angle4); static_pan4_r[p] = sinf(angle4);
    }

    // Combi lane Level/Pan (seq_lanes[p].volume/pan -- the same fader/pan widgets Combi's own
    // mixer strip UI already shows) applied per-Program-bus, Combi mode only. Gated on
    // performance_mode so Single Program/Sequencer mode's output stays byte-identical to before
    // this existed (programs[0]'s own bus there is never scaled by seq_lanes[0]'s fader, which
    // defaults to 80 -- applying it unconditionally would silently quiet every existing patch by
    // default). An already-stereo source (a Program's own oscillators are already panned) uses
    // the sqrt(2)-normalized pan law -- same convention as Filter A/B's Parallel combine and the
    // Sequencer audio-lane preview mixer's own "already-stereo source" case -- so pan=0 is a true
    // no-op. Precomputed once per block (knob values, block-invariant) rather than per sample.
    float combi_lane_gain_l[COMBI_PROGRAM_COUNT], combi_lane_gain_r[COMBI_PROGRAM_COUNT];
    bool combi_mode_active = ((int)knobs->performance_mode == 1);
    for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
        if (!combi_mode_active) { combi_lane_gain_l[p] = 1.0f; combi_lane_gain_r[p] = 1.0f; continue; }
        float lvl = fmaxf(0.0f, knobs->seq_lanes[p].volume) / 100.0f;
        float lane_angle = (fmaxf(-1.0f, fminf(1.0f, knobs->seq_lanes[p].pan)) + 1.0f) * ((float)M_PI / 4.0f);
        combi_lane_gain_l[p] = lvl * cosf(lane_angle) * 1.41421356f;
        combi_lane_gain_r[p] = lvl * sinf(lane_angle) * 1.41421356f;
    }

    for (int i = 0; i < block_size; i++) {
        float mixed_l[COMBI_PROGRAM_COUNT + 1] = {0.0f}, mixed_r[COMBI_PROGRAM_COUNT + 1] = {0.0f};
        // Drum Bus routing (see DRUM_BUS_COUNT's own comment): a second, session-level (not
        // per-Program) set of accumulators, filled directly by individual oscillators inside the
        // voice loop below (bypassing this Program's own Filter/EQ pipeline entirely) instead of
        // through mixed_l/mixed_r -- see the stereo-mixer block's own drum-bus interception.
        float drum_bus_l[DRUM_BUS_COUNT] = {0.0f}, drum_bus_r[DRUM_BUS_COUNT] = {0.0f};
        float lfo_out[COMBI_PROGRAM_COUNT + 1][LFO_COUNT] = {{0.0f}};
        // LFOs are computed per active Program slot (see active_program_count's own comment --
        // always advancing continuously per Program, never gated on voice activity, to preserve
        // today's exact phase-continuity behavior). Outside Combi mode this is just p=0, the
        // exact same single pass as before this restructuring.
        for (int p_loop = 0; p_loop < active_program_count; p_loop++) {
        // Resolve which REAL programs[]/mixed_l[]/lfo_out[]/state-array slot this iteration
        // actually touches: identity in Combi mode (p_loop IS the Combi lane's own array index,
        // 0-7); always SINGLE_PROGRAM_SLOT outside it, where this loop only ever runs once
        // (active_program_count == 1) -- see ProgramState's own comment on the mutual-exclusivity
        // fix this resolves. Every `[p]` reference in the rest of this loop body is unchanged.
        int p = combi_mode_active ? p_loop : SINGLE_PROGRAM_SLOT;
        for (int l = 0; l < LFO_COUNT; l++) {
            int wave_type = (int)knobs->programs[p].lfo[l].wave_type;
            // Off: complete compute bypass -- no phase_offset/mod_source_cc/waveform lookup, and
            // no phase advance either (nothing else in this engine reads an LFO's raw phase, so
            // freezing it is safe and avoids wasted work on an LFO nobody's listening to -- this
            // is the actual CPU concern with 16 always-otherwise-running LFOs).
            if (wave_type == 4) { lfo_out[p][l] = 0.0f; continue; }

            // Rate/Depth as mod destinations (targets 14-21/48-71): one-sample-delayed
            // contributions from lfo_rate_mod_state/lfo_depth_mod_state (computed once per
            // sample, below the voice loop -- see lfo_rate_target_index's comment), applied on
            // top of this LFO's own knobs. Rate mod is scaled (a unity-depth modulator sweeps
            // +/-20Hz -- a musically useful range without letting one LFO drag another across
            // its whole 0.1-200Hz dial range); Depth mod adds directly (mod_depth is already a
            // plain 0-1 fraction, same convention mod_source_cc's multiplicative scale uses).
            // Both stay independent of and coexist with mod_source_cc's own CC/aftertouch scale.
            float effective_rate_hz = resolve_lfo_rate_hz(&knobs->programs[p].lfo[l], lfo_rate_mod_state[p][l], *lfo_tempo_smoothed);
            float effective_mod_depth = fmaxf(0.0f, fminf(1.0f, knobs->programs[p].lfo[l].mod_depth + lfo_depth_mod_state[p][l]));

            if (l < 2) {
                // LFOs 1/2: full audio-rate, unchanged from the original 4-LFO design.
                // Phase pot (0-360 degrees) plus any modulation, shifting only the read-phase
                // used for this sample's waveform lookup -- the underlying running phase
                // (advanced below) is never touched, so Rate/sync stay completely unaffected.
                float phase_offset = fmodf((knobs->programs[p].lfo[l].phase_offset / 360.0f) + lfo_phase_mod_state[p][l], 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
                float read_phase = fmodf(global_lfo_phases[p][l] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
                // Modulation Source: scales this LFO's contribution by a live MIDI controller
                // (e.g. gating vibrato depth with the mod wheel) on top of the plain Depth knob.
                // Unassigned (-1, the default) is a pure no-op -- cc_source_scale stays 1.0.
                int mod_src = (int)knobs->programs[p].lfo[l].mod_source_cc;
                float cc_source_scale = 1.0f;
                if (mod_src == 128) cc_source_scale = midi_live->channel_pressure;
                else if (mod_src >= 0 && mod_src < 128) cc_source_scale = midi_live->cc[mod_src];
                lfo_out[p][l] = lookup_lfo_waveform(wave_type, read_phase, knobs->programs[p].lfo[l].curve_drive) * effective_mod_depth * cc_source_scale;
                global_lfo_phases[p][l] += effective_rate_hz / (float)SAMPLE_RATE;
                if (global_lfo_phases[p][l] >= 1.0f) global_lfo_phases[p][l] -= 1.0f;
            } else {
                // LFOs 3-16: control-rate decimated (see LFO_DECIMATION_FACTOR's comment). Only
                // recompute the waveform/phase every LFO_DECIMATION_FACTOR samples; every sample
                // in between linearly interpolates from the last recomputed value toward the new
                // one, avoiding a zipper-noise step at each recompute boundary.
                if (lfo_decim_counter[p][l] == 0) {
                    lfo_decim_last_value[p][l] = lfo_decim_target_value[p][l];
                    float phase_offset = fmodf((knobs->programs[p].lfo[l].phase_offset / 360.0f) + lfo_phase_mod_state[p][l], 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
                    float read_phase = fmodf(global_lfo_phases[p][l] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
                    int mod_src = (int)knobs->programs[p].lfo[l].mod_source_cc;
                    float cc_source_scale = 1.0f;
                    if (mod_src == 128) cc_source_scale = midi_live->channel_pressure;
                    else if (mod_src >= 0 && mod_src < 128) cc_source_scale = midi_live->cc[mod_src];
                    lfo_decim_target_value[p][l] = lookup_lfo_waveform(wave_type, read_phase, knobs->programs[p].lfo[l].curve_drive) * effective_mod_depth * cc_source_scale;
                    // Phase advances by DECIMATION_FACTOR steps at once, since we're about to
                    // skip that many samples' worth of recomputation -- this keeps the LFO's
                    // real-world rate correct despite only sampling it every 24th sample.
                    global_lfo_phases[p][l] += (effective_rate_hz / (float)SAMPLE_RATE) * (float)LFO_DECIMATION_FACTOR;
                    if (global_lfo_phases[p][l] >= 1.0f) global_lfo_phases[p][l] = fmodf(global_lfo_phases[p][l], 1.0f);
                }
                float frac = (float)lfo_decim_counter[p][l] / (float)LFO_DECIMATION_FACTOR;
                lfo_out[p][l] = lfo_decim_last_value[p][l] + (lfo_decim_target_value[p][l] - lfo_decim_last_value[p][l]) * frac;
                lfo_decim_counter[p][l] = (lfo_decim_counter[p][l] + 1) % LFO_DECIMATION_FACTOR;
            }
        }
        }

        // Monophonic Noise: one shared white-noise/tilt-filter generator per Program+oscillator,
        // computed once per sample HERE (before the voice loop, not inside it) so every voice
        // sharing this Program's oscillator reads the exact same sample this sample -- the
        // classic "one physical noise source feeding the whole keyboard" behavior, as opposed to
        // each voice running its own independent, uncorrelated generator (today's default,
        // still what happens whenever the checkbox is off). Tilt uses the Program's own knob
        // value only (no per-voice EG modulation) -- a deliberate simplification, since a single
        // shared generator has no single per-voice modulated value to defer to. Meaningless/
        // unused for any oscillator not currently WAVE_NOISE with mono_noise engaged -- cheap to
        // just always compute for the (at most 8) active Programs regardless.
        float mono_noise_out[COMBI_PROGRAM_COUNT + 1][4] = {{0.0f}};
        for (int p_loop = 0; p_loop < active_program_count; p_loop++) {
        int p = combi_mode_active ? p_loop : SINGLE_PROGRAM_SLOT; // see the LFO loop's own comment on this resolution
            const ProgramState *mprog = &knobs->programs[p];
            float mono_flag[4] = { mprog->vco1_mono_noise, mprog->vco2_mono_noise, mprog->vco3_mono_noise, mprog->vco4_mono_noise };
            WaveType mono_shape[4] = { (WaveType)(int)mprog->vco1_shape, (WaveType)(int)mprog->vco2_shape, (WaveType)(int)mprog->vco3_shape, (WaveType)(int)mprog->vco4_shape };
            float mono_pulse_or_tilt[4] = { mprog->vco1_pulse_width, mprog->vco2_pulse_width, mprog->vco3_pulse_width, mprog->vco4_pulse_width };
            for (int osc = 0; osc < 4; osc++) {
                if (mono_flag[osc] == 0.0f || mono_shape[osc] != WAVE_NOISE) continue;
                float white = (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
                mono_noise_tilt_lp[p][osc] += noise_tilt_coeff * (white - mono_noise_tilt_lp[p][osc]);
                float tilt_high = white - mono_noise_tilt_lp[p][osc];
                float tilt_frac = fmaxf(-1.0f, fminf(1.0f, mono_pulse_or_tilt[osc] / 100.0f));
                mono_noise_out[p][osc] = mono_noise_tilt_lp[p][osc] * (1.0f - tilt_frac) + tilt_high * (1.0f + tilt_frac);
            }
        }

        for (int v = 0; v < voice_limit; v++) {
            if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].vco3_eg.state == STATE_IDLE && voices[v].vco4_eg.state == STATE_IDLE) continue;

            // This voice's own Program -- resolved once here and reused for every Program-scoped
            // knob read for the rest of this voice's own per-sample rendering below (Combi mode's
            // whole "shared voice pool, N independent Programs" design comes down to this one
            // resolution: the voice-claim/steal logic in handle_note_on stays completely
            // program-agnostic, only this read needs to know which Program a voice belongs to).
            int pidx = voices[v].program_index;
            const ProgramState *prog = &knobs->programs[pidx];

            // SFZ per-zone amp envelope override (see ProgramState.vco1_force_own_amp_eg's own
            // comment): a voice whose vcoN_using_own_ampeg is true ticks against ITS OWN
            // vcoN_own_eg_rates (computed once at this note's own note-on) instead of this
            // Program's shared eg_rates[pidx][e] -- unaffected (always the Program's own shared
            // rates) for every oscillator not currently playing a zone with its own ampeg_*
            // opcodes, and for every assignable EG (e>=4), which this override never touches.
            for(int e=0; e<ENV_COUNT; e++) {
                if(e==0) tick_adsr(&voices[v].vco1_eg, voices[v].vco1_using_own_ampeg ? &voices[v].vco1_own_eg_rates : &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==1) tick_adsr(&voices[v].vco2_eg, voices[v].vco2_using_own_ampeg ? &voices[v].vco2_own_eg_rates : &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==2) tick_adsr(&voices[v].vco3_eg, voices[v].vco3_using_own_ampeg ? &voices[v].vco3_own_eg_rates : &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==3) tick_adsr(&voices[v].vco4_eg, voices[v].vco4_using_own_ampeg ? &voices[v].vco4_own_eg_rates : &eg_rates[pidx][e], eg_sustain_chase_rate);
                else tick_adsr(&voices[v].assign_eg[e-4], &eg_rates[pidx][e], eg_sustain_chase_rate);
            }
            release_idle_sample_streams(se, &voices[v]); // WAVE_SAMPLE: free any DFD stream this voice's oscillator(s) no longer need
            // WAVE_STRING: give back a string whose oscillator went quiet or stopped being a string.
            {
                const ADSR *egs[4] = { &voices[v].vco1_eg, &voices[v].vco2_eg, &voices[v].vco3_eg, &voices[v].vco4_eg };
                float shapes[4] = { prog->vco1_shape, prog->vco2_shape, prog->vco3_shape, prog->vco4_shape };
                for (int o = 0; o < 4; o++) {
                    if (voices[v].str_slot[o] >= 0 && (egs[o]->state == STATE_IDLE || (WaveType)(int)shapes[o] != WAVE_STRING))
                        hds_string_release(&engine->strings, &voices[v].str_slot[o]);
                }
            }

            // Mono Legato glide randomization: each voice's own glide_time_multiplier (rerolled
            // per glide event in handle_note_on/off) scales the plain Glide Time knob, so
            // successive legato glides don't all take exactly the same amount of time. See
            // portamento_coeff[]'s own comment above (once per block, not once per sample).
            float portamento_coefficient = portamento_coeff[v];
            voices[v].vco1_current_freq += (voices[v].vco1_target_freq - voices[v].vco1_current_freq) * portamento_coefficient;
            voices[v].vco2_current_freq += (voices[v].vco2_target_freq - voices[v].vco2_current_freq) * portamento_coefficient;
            voices[v].vco3_current_freq += (voices[v].vco3_target_freq - voices[v].vco3_current_freq) * portamento_coefficient;
            voices[v].vco4_current_freq += (voices[v].vco4_target_freq - voices[v].vco4_current_freq) * portamento_coefficient;

            // vco{N}_pitch: the manual Pitch knob (cents, +-2400 = +-24 semitones), added in
            // alongside fine_tune/pitch_offset/pitch-bend -- an LFO/EG targeting "Osc N Pitch"
            // sums into this same accumulator further down, so knob + live modulation just add.
            // The global performance Transpose knob is deliberately NOT here any more -- see
            // SynthSharedControls.transpose_semitones' own comment for why it moved to
            // dispatch_midi_event() instead, adding directly onto the incoming MIDI note number
            // rather than retuning already-selected voices live.
            // Combi per-lane transpose (octave layering): an additive-cents term, exactly the
            // shape the global Transpose knob used to be, but per-Combi-lane instead of global --
            // see SynthSharedControls.combi_lane_transpose's own comment for why THIS one stays a
            // live cents offset (it's a deliberate live layering/detune effect, not a stand-in for
            // "the performer pressed a different key," so the Multisample sample-zone-selection
            // problem that moved the global knob doesn't apply to it the same way). Only
            // meaningful for the 8 real Combi lanes (pidx 0-7); SINGLE_PROGRAM_SLOT
            // (pidx==COMBI_PROGRAM_COUNT) has no lane to look up, so it contributes nothing there,
            // same as today.
            float combi_transpose_cents = (pidx < COMBI_PROGRAM_COUNT) ? knobs->combi_lane_transpose[pidx] * 100.0f : 0.0f;
            float mod_vco1_pitch = prog->vco1_fine_tune + prog->vco1_pitch + pb_bend_cents[pidx] + combi_transpose_cents;
            float mod_vco2_pitch = prog->vco2_fine_tune + (prog->vco2_pitch_offset * 100.0f) + prog->vco2_pitch + pb_bend_cents[pidx] + combi_transpose_cents;
            float mod_vco3_pitch = prog->vco3_fine_tune + (prog->vco3_pitch_offset * 100.0f) + prog->vco3_pitch + pb_bend_cents[pidx] + combi_transpose_cents;
            float mod_vco4_pitch = prog->vco4_fine_tune + (prog->vco4_pitch_offset * 100.0f) + prog->vco4_pitch + pb_bend_cents[pidx] + combi_transpose_cents;
            float mod_vco1_amp = 0.0f, mod_vco2_amp = 0.0f, fm_mod = 0.0f, mix_bal = 0.0f;
            // Filter cutoff/resonance mod destinations -- one flat slot per filter (0-4), targets
            // 90-99 (see the frontend's matrix_shared_routes comment). Key Track and ext-MIDI CC/
            // aftertouch modulation (see FilterParams.key_track/cutoff_mod_source_cc) are folded
            // additively into filter_cutoff_mods too, right before the filter dispatch below --
            // same accumulator, so a knob's own key-track/CC setting and live LFO/EG modulation
            // of the same filter's cutoff simply sum.
            float filter_cutoff_mods[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            float filter_res_mods[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            // Phase Modulation (new): a bipolar mod value shifts where in the cycle each
            // oscillator currently reads from, without touching its actual running phase
            // accumulator (that distinction matters -- it keeps pitch, hard sync, and portamento
            // completely unaffected by phase modulation).
            float mod_vco1_phase = 0.0f, mod_vco2_phase = 0.0f, mod_vco3_phase = 0.0f, mod_vco4_phase = 0.0f;
            // Unison Saw Detune Width as a mod destination (new).
            float mod_vco1_unison_detune = 0.0f, mod_vco2_unison_detune = 0.0f, mod_vco3_unison_detune = 0.0f, mod_vco4_unison_detune = 0.0f;
            // Pan/Pulse Width/Noise/Sub as mod destinations (targets 32-48 -- see
            // hds_lfo_dest_label()/hds_dest_list_build() in hds_clap_play_draw.c for the full,
            // current list of what each target number means). Pan mods are
            // applied directly (same convention as Phase); the rest get a scale factor sized so
            // a unity-depth LFO/EG sweeps the whole musically useful range.
            float mod_vco1_pan = 0.0f, mod_vco2_pan = 0.0f;
            float mod_vco1_pulse_width = 0.0f, mod_vco2_pulse_width = 0.0f;
            float mod_vco3_amp = 0.0f, mod_vco3_pan = 0.0f, mod_vco3_tilt = 0.0f;
            float mod_vco4_amp = 0.0f, mod_vco4_pan = 0.0f, mod_vco4_tilt = 0.0f;
            // Curve/Drive as a mod destination (targets 10-13, one per oscillator -- reclaimed from
            // the retired 8-13 placeholder range, see hds_lfo_dest_label()'s own comment in
            // hds_clap_play_draw.c). Applied directly (curve_drive is already -1..1, same convention
            // Pan already uses above) at the curve_drive_arr construction below, clamped there.
            float mod_vco1_curve = 0.0f, mod_vco2_curve = 0.0f, mod_vco3_curve = 0.0f, mod_vco4_curve = 0.0f;

            for (int l = 0; l < LFO_COUNT; l++) {
                int target = (int)prog->lfo[l].dest_target;
                // PERF: target 0 ("None") is the common case for any LFO not actually routed
                // to a destination -- skip straight past the dispatch below instead of evaluating
                // it for nothing. This loop runs per ACTIVE voice per sample, so on a plain
                // unmodulated patch this was burning a measurable chunk of every block's CPU
                // budget (found via real-world underrun diagnostics: a 33-voice plain sawtooth
                // patch, no modulation configured at all, was already blowing the block budget).
                if (target == 0) continue;
                // Per-Key mode (see PackLfoParams.per_key_mode): this voice's own independent
                // phase/rate instead of the Program-shared lfo_out every OTHER voice of this
                // Program reads -- the whole point of Per-Key mode (overlapping notes each run
                // their own unsynced cycle, and Rate can track this voice's own key/velocity).
                float val = (prog->lfo[l].per_key_mode != 0.0f)
                    ? compute_per_key_lfo_output(&voices[v], &prog->lfo[l], l, lfo_rate_mod_state[pidx][l], lfo_depth_mod_state[pidx][l], lfo_phase_mod_state[pidx][l], midi_live, *lfo_tempo_smoothed)
                    : lfo_out[pidx][l];
                // PERF: a real switch (not a flat if-chain) so the compiler can build a jump
                // table -- O(1) dispatch per routed target instead of a linear scan through every
                // earlier case. Targets are mutually exclusive by construction (target can only
                // equal one value), so this is a behavior-preserving transform of the original
                // if-chain, not a semantic change. This matters specifically for a heavily-
                // modulated patch: every one of LFO_COUNT + ASSIGNABLE_EG_COUNT routed sources
                // pays this dispatch once per voice per sample, so a long if-chain here was real,
                // scaling cost on top of whatever the modulation itself costs.
                switch (target) {
                    case 1: mod_vco1_pitch += val * 1200.0f; break;
                    case 2: mod_vco2_pitch += val * 1200.0f; break;
                    case 3: mod_vco1_pitch += val * 1200.0f; mod_vco2_pitch += val * 1200.0f; break;
                    case 4: mod_vco1_pitch += val * 100.0f; break;
                    case 5: mod_vco2_pitch += val * 100.0f; break;
                    case 6: mod_vco1_amp += val; break;
                    case 7: mod_vco2_amp += val; break;
                    // Osc 3/4 Fine (reclaimed from the retired 8/9 placeholders) -- same smaller-
                    // range pitch-mod convention as targets 4/5 above, just for the two oscillators
                    // that never had an equivalent until now.
                    case 8: mod_vco3_pitch += val * 100.0f; break;
                    case 9: mod_vco4_pitch += val * 100.0f; break;
                    // Osc 1-4 Curve/Drive (reclaimed from the retired 10-13 placeholders) -- see
                    // mod_vco1_curve's own declaration comment above.
                    case 10: mod_vco1_curve += val; break;
                    case 11: mod_vco2_curve += val; break;
                    case 12: mod_vco3_curve += val; break;
                    case 13: mod_vco4_curve += val; break;
                    // FIXED: this used to check target 20/21 (which are actually "LFO 3/4 Depth"
                    // in the GUI's destination list), so FM Depth and Mix Balance (the real 22/23)
                    // could never be reached as destinations, while picking "LFO 3/4 Depth"
                    // silently hijacked them.
                    case 22: fm_mod += val; break;
                    case 23: mix_bal += val; break;
                    case 24: mod_vco1_phase += val; break;
                    case 25: mod_vco2_phase += val; break;
                    case 26: mod_vco1_unison_detune += val * 2000.0f; break;
                    case 27: mod_vco2_unison_detune += val * 2000.0f; break;
                    case 32: mod_vco1_pan += val; break;
                    case 33: mod_vco2_pan += val; break;
                    case 34: mod_vco1_pulse_width += val * 50.0f; break;
                    case 35: mod_vco2_pulse_width += val * 50.0f; break;
                    case 36: mod_vco3_amp += val; break;
                    case 37: mod_vco3_pan += val; break;
                    case 38: mod_vco3_tilt += val * 100.0f; break;
                    case 39: mod_vco4_amp += val; break;
                    case 40: mod_vco4_pan += val; break;
                    case 41: mod_vco3_pitch += val * 1200.0f; break;
                    case 42: mod_vco4_pitch += val * 1200.0f; break;
                    case 43: mod_vco3_phase += val; break;
                    case 44: mod_vco4_phase += val; break;
                    case 45: mod_vco3_unison_detune += val * 2000.0f; break;
                    case 46: mod_vco4_unison_detune += val * 2000.0f; break;
                    case 47: mod_vco4_tilt += val * 100.0f; break;
                    // Filter 1-5 Cutoff/Resonance (targets 90-99, one flat slot per filter --
                    // the old Filter A/B/Multi-unit targets 8-13/84-89 are retired, see the
                    // filter section simplification's own comments on ProgramState.filters).
                    case 90: filter_cutoff_mods[0] += val * 5000.0f; break;
                    case 91: filter_res_mods[0] += val * 50.0f; break;
                    case 92: filter_cutoff_mods[1] += val * 5000.0f; break;
                    case 93: filter_res_mods[1] += val * 50.0f; break;
                    case 94: filter_cutoff_mods[2] += val * 5000.0f; break;
                    case 95: filter_res_mods[2] += val * 50.0f; break;
                    case 96: filter_cutoff_mods[3] += val * 5000.0f; break;
                    case 97: filter_res_mods[3] += val * 50.0f; break;
                    case 98: filter_cutoff_mods[4] += val * 5000.0f; break;
                    case 99: filter_res_mods[4] += val * 50.0f; break;
                    default: break;
                }
            }

            // FIXED: each assignable EG now routes using its own destination combo (eg_dest_target),
            // not the corresponding LFO's destination -- the GUI already had a per-EG destination
            // dropdown, but it was never wired to anything back here.
            for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) {
                int target = (int)prog->eg_dest_target[e];
                // PERF: same early-out as the LFO loop above -- target 0 ("None") is the
                // common case for an assignable EG not routed anywhere.
                if (target == 0) continue;
                float val = voices[v].assign_eg[e].current_level * prog->eg_intensity[e];
                // PERF: switch, not a flat if-chain -- see the LFO loop's own comment above for why.
                switch (target) {
                    case 1: mod_vco1_pitch += val * 1200.0f; break;
                    case 2: mod_vco2_pitch += val * 1200.0f; break;
                    case 3: mod_vco1_pitch += val * 1200.0f; mod_vco2_pitch += val * 1200.0f; break;
                    case 4: mod_vco1_pitch += val * 100.0f; break;
                    case 5: mod_vco2_pitch += val * 100.0f; break;
                    case 6: mod_vco1_amp += val; break;
                    case 7: mod_vco2_amp += val; break;
                    case 8: mod_vco3_pitch += val * 100.0f; break;
                    case 9: mod_vco4_pitch += val * 100.0f; break;
                    case 10: mod_vco1_curve += val; break;
                    case 11: mod_vco2_curve += val; break;
                    case 12: mod_vco3_curve += val; break;
                    case 13: mod_vco4_curve += val; break;
                    case 22: fm_mod += val; break;
                    case 23: mix_bal += val; break;
                    case 24: mod_vco1_phase += val; break;
                    case 25: mod_vco2_phase += val; break;
                    case 26: mod_vco1_unison_detune += val * 2000.0f; break;
                    case 27: mod_vco2_unison_detune += val * 2000.0f; break;
                    case 32: mod_vco1_pan += val; break;
                    case 33: mod_vco2_pan += val; break;
                    case 34: mod_vco1_pulse_width += val * 50.0f; break;
                    case 35: mod_vco2_pulse_width += val * 50.0f; break;
                    case 36: mod_vco3_amp += val; break;
                    case 37: mod_vco3_pan += val; break;
                    case 38: mod_vco3_tilt += val * 100.0f; break;
                    case 39: mod_vco4_amp += val; break;
                    case 40: mod_vco4_pan += val; break;
                    case 41: mod_vco3_pitch += val * 1200.0f; break;
                    case 42: mod_vco4_pitch += val * 1200.0f; break;
                    case 43: mod_vco3_phase += val; break;
                    case 44: mod_vco4_phase += val; break;
                    case 45: mod_vco3_unison_detune += val * 2000.0f; break;
                    case 46: mod_vco4_unison_detune += val * 2000.0f; break;
                    case 47: mod_vco4_tilt += val * 100.0f; break;
                    // Filter 1-5 Cutoff/Resonance (targets 90-99, one flat slot per filter --
                    // the old Filter A/B/Multi-unit targets 8-13/84-89 are retired, see the
                    // filter section simplification's own comments on ProgramState.filters).
                    case 90: filter_cutoff_mods[0] += val * 5000.0f; break;
                    case 91: filter_res_mods[0] += val * 50.0f; break;
                    case 92: filter_cutoff_mods[1] += val * 5000.0f; break;
                    case 93: filter_res_mods[1] += val * 50.0f; break;
                    case 94: filter_cutoff_mods[2] += val * 5000.0f; break;
                    case 95: filter_res_mods[2] += val * 50.0f; break;
                    case 96: filter_cutoff_mods[3] += val * 5000.0f; break;
                    case 97: filter_res_mods[3] += val * 50.0f; break;
                    case 98: filter_cutoff_mods[4] += val * 5000.0f; break;
                    case 99: filter_res_mods[4] += val * 50.0f; break;
                    default: break;
                }
            }

            // Phase pot (0-360 degrees) plus any modulation, converted to a 0..1 fractional-cycle
            // offset. Wrapped defensively since modulation can push it negative or past 1.
            float phase_offset1 = fmodf((prog->vco1_phase_offset / 360.0f) + mod_vco1_phase, 1.0f); if (phase_offset1 < 0.0f) phase_offset1 += 1.0f;
            float phase_offset2 = fmodf((prog->vco2_phase_offset / 360.0f) + mod_vco2_phase, 1.0f); if (phase_offset2 < 0.0f) phase_offset2 += 1.0f;
            float phase_offset3 = fmodf((prog->vco3_phase_offset / 360.0f) + mod_vco3_phase, 1.0f); if (phase_offset3 < 0.0f) phase_offset3 += 1.0f;
            float phase_offset4 = fmodf((prog->vco4_phase_offset / 360.0f) + mod_vco4_phase, 1.0f); if (phase_offset4 < 0.0f) phase_offset4 += 1.0f;

            // Unison Saw Detune Width, base knob plus any modulation, clamped back into its valid range.
            float vco1_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, prog->vco1_unison_saw_detune + mod_vco1_unison_detune));
            float vco2_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, prog->vco2_unison_saw_detune + mod_vco2_unison_detune));
            float vco3_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, prog->vco3_unison_saw_detune + mod_vco3_unison_detune));
            float vco4_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, prog->vco4_unison_saw_detune + mod_vco4_unison_detune));

            // Oscillator shapes, hoisted here (from their own call sites further below) purely so
            // the Pan block right after can tell whether a WAVE_SAMPLE zone's own Pan setting
            // should fold into the knob-based pan below -- both are plain reads of a knob value
            // with no ordering dependency on anything between here and their old declaration
            // sites, so hoisting is safe.
            WaveType vco1_shape_now = (WaveType)((int)prog->vco1_shape);
            WaveType vco2_shape_now = (WaveType)((int)prog->vco2_shape);
            WaveType vco3_shape_now = (WaveType)((int)prog->vco3_shape);
            WaveType vco4_shape_now = (WaveType)((int)prog->vco4_shape);
            // WAVE_OFF: skip pan trig + render + mix for this oscillator entirely (phase advance
            // and envelope ticking stay unconditional -- see the WAVE_OFF decision's rationale).
            bool osc1_off = vco1_shape_now == WAVE_OFF, osc2_off = vco2_shape_now == WAVE_OFF;
            bool osc3_off = vco3_shape_now == WAVE_OFF, osc4_off = vco4_shape_now == WAVE_OFF;

            // Pan (targets 32/33/37/40), base knob plus any modulation, clamped back into
            // [-1,1] -- computed per-voice/per-sample now that it's modulatable (see the
            // comment where this used to be a per-block constant, above the voice loop). A
            // WAVE_SAMPLE oscillator's zone can carry its own Pan (Mapping tab's Zone Inspector),
            // which ADDS to the oscillator's own knob here rather than overriding it -- see
            // sample_osc_zone_pan_offset(). Off oscillators skip the trig calls entirely (left at
            // 0.0f -- unused, since their render/mix is skipped too).
            float zone_pan1 = (vco1_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(se, voices[v].vco1_sample_stream) : 0.0f;
            float zone_pan2 = (vco2_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(se, voices[v].vco2_sample_stream) : 0.0f;
            float zone_pan3 = (vco3_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(se, voices[v].vco3_sample_stream) : 0.0f;
            float zone_pan4 = (vco4_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(se, voices[v].vco4_sample_stream) : 0.0f;
            float pan1_l = 0.0f, pan1_r = 0.0f, pan2_l = 0.0f, pan2_r = 0.0f;
            float pan3_l = 0.0f, pan3_r = 0.0f, pan4_l = 0.0f, pan4_r = 0.0f;
            // Fall back to the once-per-block static angle (see its own comment above) whenever
            // nothing is actually contributing on top of the plain Pan knob this sample -- skips
            // the cosf/sinf pair entirely in the common (unmodulated) case.
            if (!osc1_off) {
                if (mod_vco1_pan == 0.0f && zone_pan1 == 0.0f) { pan1_l = static_pan1_l[pidx]; pan1_r = static_pan1_r[pidx]; }
                else {
                    pan1_l = cosf((fmaxf(-1.0f, fminf(1.0f, prog->vco1_pan + mod_vco1_pan + zone_pan1)) + 1.0f) * (M_PI / 4.0f));
                    pan1_r = sinf((fmaxf(-1.0f, fminf(1.0f, prog->vco1_pan + mod_vco1_pan + zone_pan1)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc2_off) {
                if (mod_vco2_pan == 0.0f && zone_pan2 == 0.0f) { pan2_l = static_pan2_l[pidx]; pan2_r = static_pan2_r[pidx]; }
                else {
                    pan2_l = cosf((fmaxf(-1.0f, fminf(1.0f, prog->vco2_pan + mod_vco2_pan + zone_pan2)) + 1.0f) * (M_PI / 4.0f));
                    pan2_r = sinf((fmaxf(-1.0f, fminf(1.0f, prog->vco2_pan + mod_vco2_pan + zone_pan2)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc3_off) {
                if (mod_vco3_pan == 0.0f && zone_pan3 == 0.0f) { pan3_l = static_pan3_l[pidx]; pan3_r = static_pan3_r[pidx]; }
                else {
                    pan3_l = cosf((fmaxf(-1.0f, fminf(1.0f, prog->vco3_pan + mod_vco3_pan + zone_pan3)) + 1.0f) * (M_PI / 4.0f));
                    pan3_r = sinf((fmaxf(-1.0f, fminf(1.0f, prog->vco3_pan + mod_vco3_pan + zone_pan3)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc4_off) {
                if (mod_vco4_pan == 0.0f && zone_pan4 == 0.0f) { pan4_l = static_pan4_l[pidx]; pan4_r = static_pan4_r[pidx]; }
                else {
                    pan4_l = cosf((fmaxf(-1.0f, fminf(1.0f, prog->vco4_pan + mod_vco4_pan + zone_pan4)) + 1.0f) * (M_PI / 4.0f));
                    pan4_r = sinf((fmaxf(-1.0f, fminf(1.0f, prog->vco4_pan + mod_vco4_pan + zone_pan4)) + 1.0f) * (M_PI / 4.0f));
                }
            }

            // Pulse Width/Tilt (targets 34/35/38/47), base knob plus any modulation. Dual-duty:
            // clamped to [2,98] (percent duty) when the shape is Pulse, or [-100,100] (tilt
            // amount) when the shape is Noise -- the same range render_fm_oscillator's Noise
            // branch expects (it divides by 100 itself). Any other shape doesn't consume this
            // value at all, so the exact clamp choice there is moot.
            float vco1_pulse_width_final = (vco1_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, prog->vco1_pulse_width + mod_vco1_pulse_width)) : fmaxf(2.0f, fminf(98.0f, prog->vco1_pulse_width + mod_vco1_pulse_width));
            float vco2_pulse_width_final = (vco2_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, prog->vco2_pulse_width + mod_vco2_pulse_width)) : fmaxf(2.0f, fminf(98.0f, prog->vco2_pulse_width + mod_vco2_pulse_width));
            float vco3_pulse_width_final = (vco3_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, prog->vco3_pulse_width + mod_vco3_tilt)) : fmaxf(2.0f, fminf(98.0f, prog->vco3_pulse_width + mod_vco3_tilt));
            float vco4_pulse_width_final = (vco4_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, prog->vco4_pulse_width + mod_vco4_tilt)) : fmaxf(2.0f, fminf(98.0f, prog->vco4_pulse_width + mod_vco4_tilt));

            // Skip the powf() call entirely when there's nothing to bend by (no fine-tune/pitch
            // knob offset, no pitch bend, no live pitch modulation routed here) -- the overwhelmingly
            // common case for a basic patch, where powf(2, 0) would just compute 1.0 the expensive
            // way. A real, measured per-voice-per-sample cost that scales directly with polyphony.
            float hz1 = (mod_vco1_pitch != 0.0f) ? voices[v].vco1_current_freq * powf(2.0f, mod_vco1_pitch / 1200.0f) : voices[v].vco1_current_freq;
            float hz2 = (mod_vco2_pitch != 0.0f) ? voices[v].vco2_current_freq * powf(2.0f, mod_vco2_pitch / 1200.0f) : voices[v].vco2_current_freq;
            float hz3 = (mod_vco3_pitch != 0.0f) ? voices[v].vco3_current_freq * powf(2.0f, mod_vco3_pitch / 1200.0f) : voices[v].vco3_current_freq;
            float hz4 = (mod_vco4_pitch != 0.0f) ? voices[v].vco4_current_freq * powf(2.0f, mod_vco4_pitch / 1200.0f) : voices[v].vco4_current_freq;
            float dt1 = hz1 / (float)SAMPLE_RATE, dt2 = hz2 / (float)SAMPLE_RATE;
            float dt3 = hz3 / (float)SAMPLE_RATE, dt4 = hz4 / (float)SAMPLE_RATE;

            // Hard Sync: 0=Off, 1=Parallel, 2=Serial (see hard_sync_mode's own comment). Computed
            // here (right after dt1-4, before any oscillator's audio for this sample is rendered)
            // so a sync-triggered discontinuity can be BLEP-corrected on THIS sample (the
            // "before" half, inside render_fm_oscillator) as well as on next sample (the "after"
            // half, carried forward via each slave's own pending_sync_correction). Does NOT
            // advance any voices[v].vcoN_phase yet -- that stays at the bottom of the loop, since
            // the FM render loop below still needs every oscillator's pre-advance phase. Osc 1 is
            // always the sync master in both topologies -- it is never itself reset.
            int hard_sync_mode = (int)prog->hard_sync_mode;
            float vco1_phase_before = voices[v].vco1_phase;
            float new_vco1_phase = vco1_phase_before + dt1;
            bool do_sync1 = (hard_sync_mode != 0 && new_vco1_phase >= 1.0f);
            float t_wrap1 = do_sync1 ? fmaxf(0.0f, fminf(1.0f, (1.0f - vco1_phase_before) / dt1)) : 0.0f;

            bool do_sync2 = false, do_sync3 = false, do_sync4 = false;
            float t_wrap2 = 0.0f, t_wrap3 = 0.0f, t_wrap4 = 0.0f;
            if (hard_sync_mode == 1) {
                // Parallel: every slave shares Osc 1's exact do_sync/t_wrap, independent of its
                // own frequency -- Osc 1 fully dictates every reset.
                do_sync2 = do_sync1; t_wrap2 = t_wrap1;
                do_sync3 = do_sync1; t_wrap3 = t_wrap1;
                do_sync4 = do_sync1; t_wrap4 = t_wrap1;
            } else if (hard_sync_mode == 2) {
                // Serial: 1->2->3->4 cascade. Each stage inherits its upstream neighbor's exact
                // t_wrap when the upstream synced this sample (the cascade is one physical
                // instant propagating down); otherwise it still fires on its OWN natural wrap
                // when its own phase independently crosses 1.0 this sample. Inherited sync always
                // takes priority over a coincidental same-sample natural wrap (a deliberate,
                // documented simplification).
                float vco2_phase_before = voices[v].vco2_phase;
                bool own_sync2 = (vco2_phase_before + dt2) >= 1.0f;
                float own_t_wrap2 = own_sync2 ? fmaxf(0.0f, fminf(1.0f, (1.0f - vco2_phase_before) / dt2)) : 0.0f;
                do_sync2 = do_sync1 || own_sync2;
                t_wrap2 = do_sync1 ? t_wrap1 : own_t_wrap2;

                float vco3_phase_before = voices[v].vco3_phase;
                bool own_sync3 = (vco3_phase_before + dt3) >= 1.0f;
                float own_t_wrap3 = own_sync3 ? fmaxf(0.0f, fminf(1.0f, (1.0f - vco3_phase_before) / dt3)) : 0.0f;
                do_sync3 = do_sync2 || own_sync3;
                t_wrap3 = do_sync2 ? t_wrap2 : own_t_wrap3;

                float vco4_phase_before = voices[v].vco4_phase;
                bool own_sync4 = (vco4_phase_before + dt4) >= 1.0f;
                float own_t_wrap4 = own_sync4 ? fmaxf(0.0f, fminf(1.0f, (1.0f - vco4_phase_before) / dt4)) : 0.0f;
                do_sync4 = do_sync3 || own_sync4;
                t_wrap4 = do_sync3 ? t_wrap3 : own_t_wrap4;
            }

            // --- FM Algorithm dispatch: renders all 4 oscillators in the selected algorithm's
            // render_order, accumulating each oscillator's own modulator output (its rendered
            // signal * its own vol-based gain, ignoring mute -- see render_fm_oscillator's
            // caller-side gain comment, generalized from the original Osc2-only gain2_for_fm) so
            // any later-rendered oscillator can be fed by it via that algorithm's edges. Algorithm
            // 0 ("Classic") reproduces this engine's original fixed Osc2->Osc1 FM pair exactly. ---
            float vco1_dummy_sync_correction = 0.0f; // Osc1 is never a hard-sync slave, so it has no real pending_sync_correction field
            WaveType shape_arr[4] = { vco1_shape_now, vco2_shape_now, vco3_shape_now, vco4_shape_now };
            float own_phase_arr[4] = { voices[v].vco1_phase, voices[v].vco2_phase, voices[v].vco3_phase, voices[v].vco4_phase };
            float dt_arr[4] = { dt1, dt2, dt3, dt4 };
            float phase_offset_arr[4] = { phase_offset1, phase_offset2, phase_offset3, phase_offset4 };
            float pulse_or_tilt_arr[4] = { vco1_pulse_width_final, vco2_pulse_width_final, vco3_pulse_width_final, vco4_pulse_width_final };
            // Curve/Drive mod (targets 10-13, mod_vco1_curve etc. -- see that declaration's own
            // comment) sums directly onto the base knob value, clamped to curve_drive's own real
            // -1..1 storage range, same convention Pan already uses just below.
            float curve_drive_arr[4] = {
                fmaxf(-1.0f, fminf(1.0f, prog->vco1_curve_drive + mod_vco1_curve)),
                fmaxf(-1.0f, fminf(1.0f, prog->vco2_curve_drive + mod_vco2_curve)),
                fmaxf(-1.0f, fminf(1.0f, prog->vco3_curve_drive + mod_vco3_curve)),
                fmaxf(-1.0f, fminf(1.0f, prog->vco4_curve_drive + mod_vco4_curve)),
            };
            float unison_detune_arr[4] = { vco1_unison_detune_final, vco2_unison_detune_final, vco3_unison_detune_final, vco4_unison_detune_final };
            float hz_arr[4] = { hz1, hz2, hz3, hz4 };
            bool do_sync_arr[4] = { false, do_sync2, do_sync3, do_sync4 };
            float t_wrap_arr[4] = { 0.0f, t_wrap2, t_wrap3, t_wrap4 };
            float *prev_fm_phase_ptr[4] = { &voices[v].vco1_prev_fm_phase, &voices[v].vco2_prev_fm_phase, &voices[v].vco3_prev_fm_phase, &voices[v].vco4_prev_fm_phase };
            float *tri_state_ptr[4] = { &voices[v].vco1_tri_state, &voices[v].vco2_tri_state, &voices[v].vco3_tri_state, &voices[v].vco4_tri_state };
            float *tilt_lp_ptr[4] = { &voices[v].vco1_tilt_lp, &voices[v].vco2_tilt_lp, &voices[v].vco3_tilt_lp, &voices[v].vco4_tilt_lp };
            float *unison_phases_ptr[4] = { voices[v].vco1_unison_saw_phases, voices[v].vco2_unison_saw_phases, voices[v].vco3_unison_saw_phases, voices[v].vco4_unison_saw_phases };
            const float *unison_pan_l_ptr[4] = { vco1_unison_pan_l[pidx], vco2_unison_pan_l[pidx], vco3_unison_pan_l[pidx], vco4_unison_pan_l[pidx] };
            const float *unison_pan_r_ptr[4] = { vco1_unison_pan_r[pidx], vco2_unison_pan_r[pidx], vco3_unison_pan_r[pidx], vco4_unison_pan_r[pidx] };
            int *sample_stream_ptr[4] = { &voices[v].vco1_sample_stream, &voices[v].vco2_sample_stream, &voices[v].vco3_sample_stream, &voices[v].vco4_sample_stream };
            double *sample_ptr_ptr[4] = { &voices[v].vco1_sample_ptr, &voices[v].vco2_sample_ptr, &voices[v].vco3_sample_ptr, &voices[v].vco4_sample_ptr };
            bool *sample_preroll_ptr[4] = { &voices[v].vco1_sample_preroll, &voices[v].vco2_sample_preroll, &voices[v].vco3_sample_preroll, &voices[v].vco4_sample_preroll };
            float *pending_sync_correction_ptr[4] = { &vco1_dummy_sync_correction, &voices[v].vco2_pending_sync_correction, &voices[v].vco3_pending_sync_correction, &voices[v].vco4_pending_sync_correction };
            // SFZ first-class opcode support: crossfade-LAYER partner stream + static xfade gains
            // (see find_matching_sample_slot's own comment) and the delayed-start countdown (SFZ's
            // delay=) -- only ever non-default for a WAVE_SAMPLE oscillator whose currently-claimed
            // zone actually uses xfin_*/xfout_*/delay=; every other oscillator's own entries here
            // are simply never consulted below.
            int *layer_stream_ptr[4] = { &voices[v].vco1_layer_sample_stream, &voices[v].vco2_layer_sample_stream, &voices[v].vco3_layer_sample_stream, &voices[v].vco4_layer_sample_stream };
            double *layer_sample_ptr_ptr[4] = { &voices[v].vco1_layer_sample_ptr, &voices[v].vco2_layer_sample_ptr, &voices[v].vco3_layer_sample_ptr, &voices[v].vco4_layer_sample_ptr };
            bool *layer_preroll_ptr[4] = { &voices[v].vco1_layer_sample_preroll, &voices[v].vco2_layer_sample_preroll, &voices[v].vco3_layer_sample_preroll, &voices[v].vco4_layer_sample_preroll };
            float xfade_gain_arr[4] = { voices[v].vco1_xfade_gain, voices[v].vco2_xfade_gain, voices[v].vco3_xfade_gain, voices[v].vco4_xfade_gain };
            float layer_xfade_gain_arr[4] = { voices[v].vco1_layer_xfade_gain, voices[v].vco2_layer_xfade_gain, voices[v].vco3_layer_xfade_gain, voices[v].vco4_layer_xfade_gain };
            int *sample_delay_remaining_ptr[4] = { &voices[v].vco1_sample_delay_remaining, &voices[v].vco2_sample_delay_remaining, &voices[v].vco3_sample_delay_remaining, &voices[v].vco4_sample_delay_remaining };

            // Vol-based gain used ONLY for FM modulator scaling (ignores mute, matching the
            // original gain2_for_fm's "muting the sound must not silence the FM modulator"
            // design). Osc1/Osc2 deliberately exclude mix_bal here too (a final-mix-only control).
            float gain_for_fm_arr[4] = {
                (prog->vco1_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco1_vol + mod_vco1_amp)),
                (prog->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco2_vol + mod_vco2_amp)),
                (prog->vco3_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco3_vol + mod_vco3_amp)),
                (prog->vco4_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco4_vol + mod_vco4_amp)),
            };
            float eg_level_arr[4] = { voices[v].vco1_eg.current_level, voices[v].vco2_eg.current_level, voices[v].vco3_eg.current_level, voices[v].vco4_eg.current_level };

            int fm_algorithm_idx = (int)prog->fm_algorithm;
            if (fm_algorithm_idx < 0) fm_algorithm_idx = 0;
            if (fm_algorithm_idx >= FM_ALGORITHM_COUNT) fm_algorithm_idx = FM_ALGORITHM_COUNT - 1;
            const FmAlgorithm *alg = &FM_ALGORITHMS[fm_algorithm_idx];

            float osc_out[4] = {0.0f, 0.0f, 0.0f, 0.0f}, osc_side_l[4] = {0.0f, 0.0f, 0.0f, 0.0f}, osc_side_r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float modulator_output[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool mono_noise_flag_arr[4] = { prog->vco1_mono_noise != 0.0f, prog->vco2_mono_noise != 0.0f, prog->vco3_mono_noise != 0.0f, prog->vco4_mono_noise != 0.0f };
            for (int slot = 0; slot < 4; slot++) {
                int osc = alg->render_order[slot];
                if (shape_arr[osc] == WAVE_OFF) { *pending_sync_correction_ptr[osc] = 0.0f; continue; } // WAVE_OFF: skip render entirely (phase advances unconditionally below)
                // SFZ delay=: hold this oscillator fully silent for its own remaining delay frames,
                // without touching its already-claimed stream/ptr/preroll (or its layer partner's)
                // at all -- they resume exactly where start_sample_osc seeded them once this hits 0.
                // A total no-op (0 remaining) for every zone that never sets delay= at all.
                if (shape_arr[osc] == WAVE_SAMPLE && *sample_delay_remaining_ptr[osc] > 0) {
                    (*sample_delay_remaining_ptr[osc])--;
                    osc_out[osc] = 0.0f; osc_side_l[osc] = 0.0f; osc_side_r[osc] = 0.0f;
                    continue;
                }
                float modulator_sum = 0.0f;
                for (int e = 0; e < alg->edge_count; e++) {
                    if (alg->edges[e].to_osc != osc) continue;
                    int src = alg->edges[e].from_osc;
                    float depth = *(const float*)((const char*)knobs + alg->edges[e].depth_offset + (size_t)pidx * sizeof(ProgramState));
                    if (alg->edges[e].depth_offset == offsetof(SynthSharedControls, programs[0].fm_intensity)) depth += fm_mod;
                    depth = fmaxf(0.0f, depth);
                    modulator_sum += modulator_output[src] * depth;
                }
                if (shape_arr[osc] == WAVE_STRING) {
                    const ADSR *str_eg[4] = { &voices[v].vco1_eg, &voices[v].vco2_eg, &voices[v].vco3_eg, &voices[v].vco4_eg };
                    osc_out[osc] = hds_string_render(&engine->strings, &voices[v].str_slot[osc], &voices[v].str_trigger[osc],
                        &prog->str[osc], hz_arr[osc], modulator_sum, str_eg[osc]->state == STATE_RELEASE,
                        voices[v].midi_velocity, &osc_side_l[osc], &osc_side_r[osc]);
                } else
                osc_out[osc] = render_fm_oscillator(
                    se,
                    shape_arr[osc], own_phase_arr[osc], dt_arr[osc], phase_offset_arr[osc], modulator_sum,
                    prev_fm_phase_ptr[osc], pulse_or_tilt_arr[osc], tri_state_ptr[osc], tilt_lp_ptr[osc],
                    noise_tilt_coeff, unison_phases_ptr[osc], unison_detune_arr[osc],
                    unison_pan_l_ptr[osc], unison_pan_r_ptr[osc],
                    sample_stream_ptr[osc], sample_ptr_ptr[osc], sample_preroll_ptr[osc], hz_arr[osc],
                    do_sync_arr[osc], t_wrap_arr[osc], pending_sync_correction_ptr[osc],
                    mono_noise_flag_arr[osc], mono_noise_out[pidx][osc],
                    curve_drive_arr[osc],
                    &osc_side_l[osc], &osc_side_r[osc]);
                // SFZ crossfade-LAYER mixing (xfin_*/xfout_*): scale the primary zone's own output
                // by its static per-note xfade gain, then mix in an optional second, simultaneously
                // -sounding partner zone's own independent stream at ITS OWN static gain -- see
                // find_matching_sample_slot's own comment. xfade_gain_arr[osc] is always exactly
                // 1.0 and layer_stream_ptr[osc] always -1 for every zone that never sets xfin_*/
                // xfout_* at all, reproducing today's exact pre-crossfade-layering output
                // byte-for-byte in that (vast majority) case.
                if (shape_arr[osc] == WAVE_SAMPLE) {
                    osc_out[osc] *= xfade_gain_arr[osc];
                    osc_side_l[osc] *= xfade_gain_arr[osc];
                    osc_side_r[osc] *= xfade_gain_arr[osc];
                    if (*layer_stream_ptr[osc] >= 0) {
                        float layer_l = 0.0f, layer_r = 0.0f;
                        float layer_mono = render_sample_osc(se, layer_stream_ptr[osc], layer_sample_ptr_ptr[osc], layer_preroll_ptr[osc], hz_arr[osc], &layer_l, &layer_r);
                        osc_out[osc] += layer_mono * layer_xfade_gain_arr[osc];
                        osc_side_l[osc] += layer_l * layer_xfade_gain_arr[osc];
                        osc_side_r[osc] += layer_r * layer_xfade_gain_arr[osc];
                    }
                }
                modulator_output[osc] = osc_out[osc] * gain_for_fm_arr[osc] * eg_level_arr[osc];
            }

            // --- Stereo mixer: each oscillator is panned to L/R here, before anything reaches
            // the filters, so its placement survives filtering (each of the 5 filter slots runs
            // as two independent mono filters, one per channel -- see the f_low/f_band state). Unison
            // Saw/Sample's own internal stereo pair (side_l/r) is used directly as the
            // oscillator's L/R image, with its Pan knob then applied as an overall balance shift
            // on top of that existing width, rather than collapsing it to mono first. A WAVE_OFF
            // oscillator naturally contributes exactly 0 here (its pan was never computed above,
            // staying 0.0f, and its osc_out/side stay 0.0f too since render was skipped). ---
            bool osc_uses_side[4] = {
                shape_arr[0] == WAVE_UNISON_SAW || shape_arr[0] == WAVE_SAMPLE || shape_arr[0] == WAVE_STRING,
                shape_arr[1] == WAVE_UNISON_SAW || shape_arr[1] == WAVE_SAMPLE || shape_arr[1] == WAVE_STRING,
                shape_arr[2] == WAVE_UNISON_SAW || shape_arr[2] == WAVE_SAMPLE || shape_arr[2] == WAVE_STRING,
                shape_arr[3] == WAVE_UNISON_SAW || shape_arr[3] == WAVE_SAMPLE || shape_arr[3] == WAVE_STRING,
            };
            float gain_audible_arr[4] = {
                (prog->vco1_mute != 0.0f || prog->vco1_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco1_vol - mix_bal + mod_vco1_amp)),
                (prog->vco2_mute != 0.0f || prog->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco2_vol + mix_bal + mod_vco2_amp)),
                (prog->vco3_mute != 0.0f || prog->vco3_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco3_vol + mod_vco3_amp)),
                (prog->vco4_mute != 0.0f || prog->vco4_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->vco4_vol + mod_vco4_amp)),
            };
            float pan_l_arr[4] = { pan1_l, pan2_l, pan3_l, pan4_l };
            float pan_r_arr[4] = { pan1_r, pan2_r, pan3_r, pan4_r };
            // Velocity -> Amp (see ProgramState.vco{1-4}_vel_to_amp's own comment): a per-voice,
            // per-oscillator gain multiplier. depth=0 is always exactly 1.0 (full level regardless
            // of velocity). Positive depth anchors velocity 127 at that same 1.0 and pulls softer
            // velocities down from there -- an ordinary velocity-sensitivity curve, reaching a
            // straight 0..1 ramp at depth=100 (silent at velocity 0, unchanged/full at 127).
            // Negative depth anchors velocity 0 at 1.0 instead and pulls HARDER velocities down --
            // the mirror-image curve. Provably stays in [0,1] for every depth/velocity combination
            // (dist below is always in [0,127], |depth fraction| in [0,1]), so no clamp is needed
            // the way the old velocity-64-anchored formula's occasional small negative overshoot did.
            float vel_to_amp_arr[4] = { prog->vco1_vel_to_amp, prog->vco2_vel_to_amp, prog->vco3_vel_to_amp, prog->vco4_vel_to_amp };
            float vel_amp_mult_arr[4];
            for (int osc = 0; osc < 4; osc++) {
                float depth = vel_to_amp_arr[osc] / 100.0f; // -1..1
                float dist = (depth >= 0.0f) ? (127.0f - (float)voices[v].midi_velocity) : (float)voices[v].midi_velocity;
                vel_amp_mult_arr[osc] = 1.0f - fabsf(depth) * dist / 127.0f;
            }
            float osc_l[4], osc_r[4];
            int drum_bus_arr[4] = { voices[v].vco1_drum_bus, voices[v].vco2_drum_bus, voices[v].vco3_drum_bus, voices[v].vco4_drum_bus };
            float zone_pan_arr[4] = { zone_pan1, zone_pan2, zone_pan3, zone_pan4 };
            for (int osc = 0; osc < 4; osc++) {
                // Drum Bus routing (see DRUM_BUS_COUNT's own comment): a zone-routed oscillator
                // contributes NOTHING to this Program's own osc_l/osc_r (so it never reaches the
                // Filter/EQ pipeline just below, and completely ignores this oscillator's own
                // Volume/Mute/Pan/vel-to-amp knobs) -- instead its raw render output (already
                // carrying its own gain_db, amp_velcurve_N, and any crossfade-layer mix, all
                // applied earlier at this same oscillator's own osc_out/osc_side_l/r computation)
                // times ONLY its amp EG level, panned by ONLY its own zone pan (equal-power, same
                // law as pan_l_arr/pan_r_arr above but with no oscillator knob/modulation added
                // in), sums directly into that Drum Bus. A zone that never sets drum_bus at all
                // (the ordinary case) always has drum_bus_arr[osc] == 0 and takes the untouched
                // else-branch below, byte-identical to pre-this-feature behavior.
                int db = (shape_arr[osc] == WAVE_SAMPLE) ? drum_bus_arr[osc] : 0;
                if (db > 0) {
                    float sig_l = osc_uses_side[osc] ? osc_side_l[osc] : osc_out[osc];
                    float sig_r = osc_uses_side[osc] ? osc_side_r[osc] : osc_out[osc];
                    float pan_gain_mult = osc_uses_side[osc] ? 1.41421356f : 1.0f;
                    float db_pan_angle = (fmaxf(-1.0f, fminf(1.0f, zone_pan_arr[osc])) + 1.0f) * (M_PI / 4.0f);
                    float db_pan_l = cosf(db_pan_angle), db_pan_r = sinf(db_pan_angle);
                    float db_amp = eg_level_arr[osc] * pan_gain_mult;
                    drum_bus_l[db - 1] += sig_l * db_amp * db_pan_l;
                    drum_bus_r[db - 1] += sig_r * db_amp * db_pan_r;
                    osc_l[osc] = 0.0f;
                    osc_r[osc] = 0.0f;
                } else {
                    float amp = gain_audible_arr[osc] * eg_level_arr[osc] * vel_amp_mult_arr[osc];
                    // Unison Saw/Sample's side_l/r are ALREADY a full stereo image (unlike every
                    // other shape's osc_out, which is mono until this Pan multiply gives it width) --
                    // reusing the plain mono-source pan law here would silently cut an untouched
                    // (centered) Unison Saw/Sample oscillator by -3dB, the same "already-stereo
                    // source" bug this codebase has already fixed for Filter A/B's Parallel combine
                    // and the Sequencer/Combi lane mixer's own Level/Pan (see those for the identical
                    // sqrt(2)-normalization fix) -- reused here for the same reason: pan_l_arr/
                    // pan_r_arr read exactly 0.70711 at center (correct for a mono source split two
                    // ways), so an already-stereo source needs the sqrt(2)-normalized law instead,
                    // which reads exactly 1.0 at center and only narrows/widens away from it.
                    float pan_gain_mult = osc_uses_side[osc] ? 1.41421356f : 1.0f;
                    osc_l[osc] = (osc_uses_side[osc] ? osc_side_l[osc] : osc_out[osc]) * amp * pan_l_arr[osc] * pan_gain_mult;
                    osc_r[osc] = (osc_uses_side[osc] ? osc_side_r[osc] : osc_out[osc]) * amp * pan_r_arr[osc] * pan_gain_mult;
                }
            }

            // Phase advance (unconditional, even for WAVE_OFF -- phase bookkeeping stays live so
            // Hard Sync/FM keep working through an Off oscillator with zero special-casing).
            // do_syncN/t_wrapN were computed earlier, before any oscillator's audio for this
            // sample was rendered -- see that comment for why. Only the actual phase mutations
            // happen here.
            voices[v].vco1_phase = fmodf(new_vco1_phase, 1.0f);
            voices[v].vco2_phase = fmodf(voices[v].vco2_phase + dt2, 1.0f);
            voices[v].vco3_phase = fmodf(voices[v].vco3_phase + dt3, 1.0f);
            voices[v].vco4_phase = fmodf(voices[v].vco4_phase + dt4, 1.0f);
            // Sub-sample-accurate sync: reset each slave's phase to where it would be if it had
            // been snapped to exactly 0 at the true fractional-sample instant t_wrap, then
            // continued advancing at its own rate for the remaining (1 - t_wrap) of this sample --
            // instead of a flat 0.0f, which quantizes the sync point to the next sample boundary
            // and introduces phase jitter. The amplitude discontinuity this reset creates is
            // separately BLEP-corrected inside render_fm_oscillator (Saw/Square/Pulse only);
            // Triangle's tri_state is deliberately left untouched so its leaky integrator smooths
            // through the reset instead of snapping.
            // Whenever a slave's phase is reset, its own prev_fm_phase tracking (used by
            // render_fm_oscillator's FM-aware adaptive BLEP width) must be rebased to match --
            // otherwise the next sample's finite-difference sees the reset itself as a huge
            // "modulator" jump and spuriously widens that oscillator's BLEP window, even with
            // zero incoming FM. Rebasing to the same mod_phase formula render_fm_oscillator uses
            // makes next sample's delta collapse to ~0 (when unmodulated and phase_offset is
            // unchanged), so the fmaxf(...,dt) floor gives back exactly dt -- provably identical
            // to a plain (non-adaptive) dt, matching this oscillator's pre-FM-algorithm behavior.
            if (do_sync2) {
                voices[v].vco2_phase = fmodf((1.0f - t_wrap2) * dt2, 1.0f);
                for (int u = 0; u < UNISON_SAW_VOICES; u++) {
                    float voice_dt = unison_saw_voice_dt(dt2, vco2_unison_detune_final, u);
                    voices[v].vco2_unison_saw_phases[u] = fmodf((1.0f - t_wrap2) * voice_dt, 1.0f);
                }
                float rebased2 = fmodf(voices[v].vco2_phase + phase_offset2, 1.0f); if (rebased2 < 0.0f) rebased2 += 1.0f;
                voices[v].vco2_prev_fm_phase = rebased2;
            }
            if (do_sync3) {
                voices[v].vco3_phase = fmodf((1.0f - t_wrap3) * dt3, 1.0f);
                for (int u = 0; u < UNISON_SAW_VOICES; u++) {
                    float voice_dt = unison_saw_voice_dt(dt3, vco3_unison_detune_final, u);
                    voices[v].vco3_unison_saw_phases[u] = fmodf((1.0f - t_wrap3) * voice_dt, 1.0f);
                }
                float rebased3 = fmodf(voices[v].vco3_phase + phase_offset3, 1.0f); if (rebased3 < 0.0f) rebased3 += 1.0f;
                voices[v].vco3_prev_fm_phase = rebased3;
            }
            if (do_sync4) {
                voices[v].vco4_phase = fmodf((1.0f - t_wrap4) * dt4, 1.0f);
                for (int u = 0; u < UNISON_SAW_VOICES; u++) {
                    float voice_dt = unison_saw_voice_dt(dt4, vco4_unison_detune_final, u);
                    voices[v].vco4_unison_saw_phases[u] = fmodf((1.0f - t_wrap4) * voice_dt, 1.0f);
                }
                float rebased4 = fmodf(voices[v].vco4_phase + phase_offset4, 1.0f); if (rebased4 < 0.0f) rebased4 += 1.0f;
                voices[v].vco4_prev_fm_phase = rebased4;
            }

            // The "summer/mixer": all 4 oscillators combined into a stereo pair (each already
            // panned above), THEN that stereo signal goes into Filter A -> Filter B.
            float stage_in_l = osc_l[0] + osc_l[1] + osc_l[2] + osc_l[3];
            float stage_in_r = osc_r[0] + osc_r[1] + osc_r[2] + osc_r[3];

            // --- Filters: 5 flat, interchangeable slots, each an exclusive Off/LPF/HPF/BPF/Notch
            // pick (FilterParams.filter_type), chained in Series (0->1->2->3->4) or summed in
            // Parallel (prog->filter_chain_mode). Each slot runs once per channel -- every
            // channel keeps fully independent filter state (voices[v].f_low/f_band's [channel]
            // dimension) so stereo placement isn't lost going through the filters. Key Track and
            // ext-MIDI CC/aftertouch modulation (FilterParams.key_track/cutoff_mod_source_cc) are
            // additive cutoff offsets folded into the same filter_cutoff_mods[] accumulator the
            // mod-matrix (targets 90-99) already writes into, so a knob's own key-track/CC
            // setting and live LFO/EG modulation of the same filter's cutoff simply sum. ---
            for (int i = 0; i < 5; i++) {
                float kt = prog->filters[i].key_track;
                if (kt != 0.0f) {
                    // Same formula PackLfoParams' own key_track already uses for LFO rate (see
                    // compute_per_key_lfo_output) -- centered on note 60 (middle C), +-100 = a
                    // full octave-per-octave-of-keyboard multiplier. Applied as a cutoff OFFSET
                    // (cutoff * (multiplier - 1)) rather than a separate multiplicative path, so
                    // it composes additively with LFO/EG modulation and the raw knob value.
                    float key_mult = powf(2.0f, ((float)(voices[v].midi_note - 60) / 12.0f) * (kt / 100.0f));
                    filter_cutoff_mods[i] += prog->filters[i].cutoff * (key_mult - 1.0f);
                }
                int mod_src = (int)prog->filters[i].cutoff_mod_source_cc;
                float cc_val = 0.0f;
                if (mod_src == 128) cc_val = midi_live->channel_pressure;
                else if (mod_src >= 0 && mod_src < 128) cc_val = midi_live->cc[mod_src];
                // Unipolar (0 at CC=0, full range at CC=127) -- "more mod wheel/aftertouch =
                // brighter" is the near-universal expectation, unlike LFO/EG modulation's own
                // bipolar +-5000Hz range.
                if (cc_val != 0.0f) filter_cutoff_mods[i] += cc_val * 5000.0f;
            }

            int chain_mode = (int)prog->filter_chain_mode; // 0 = Series, 1 = Parallel
            if (chain_mode == 0) {
                // Series: 0 -> 1 -> 2 -> 3 -> 4, each fed by the previous stage's output. An Off
                // slot (filter_type==0) is a true bypass (run_svf_filter's own guard), so it just
                // passes the previous stage's signal through unchanged.
                float sig_l = stage_in_l, sig_r = stage_in_r;
                for (int i = 0; i < 5; i++) {
                    sig_l = run_svf_filter_p(sig_l, &prog->filters[i], filter_cutoff_mods[i], filter_res_mods[i], voices[v].f_low[0][i], voices[v].f_band[0][i]);
                    sig_r = run_svf_filter_p(sig_r, &prog->filters[i], filter_cutoff_mods[i], filter_res_mods[i], voices[v].f_low[1][i], voices[v].f_band[1][i]);
                }
                mixed_l[pidx] += sig_l;
                mixed_r[pidx] += sig_r;
            } else {
                // Parallel: each slot reads the SAME shared mixer input directly. An Off slot is
                // silent -- excluded from both the sum and the divisor (per the user's explicit
                // choice) -- but run_svf_filter_p is still called unconditionally for every slot
                // (cheap; keeps state consistent if the slot is re-enabled later, same "always
                // tick, conditionally use" posture this codebase already uses for WAVE_OFF
                // oscillators). This is what makes 5x Parallel BPF work: 5 filters all set to BPF
                // at independent cutoffs, each contributing its own band, averaged down so 5
                // active bands don't sum to 5x the level of 1.
                float sum_l = 0.0f, sum_r = 0.0f;
                int active_count = 0;
                for (int i = 0; i < 5; i++) {
                    float out_l = run_svf_filter_p(stage_in_l, &prog->filters[i], filter_cutoff_mods[i], filter_res_mods[i], voices[v].f_low[0][i], voices[v].f_band[0][i]);
                    float out_r = run_svf_filter_p(stage_in_r, &prog->filters[i], filter_cutoff_mods[i], filter_res_mods[i], voices[v].f_low[1][i], voices[v].f_band[1][i]);
                    if ((int)prog->filters[i].filter_type != 0) { sum_l += out_l; sum_r += out_r; active_count++; }
                }
                if (active_count > 0) { sum_l /= (float)active_count; sum_r /= (float)active_count; }
                mixed_l[pidx] += sum_l;
                mixed_r[pidx] += sum_r;
            }
        }
        // LFO Phase/Rate/Depth as mod destinations (targets 14-21/28-31/48-71/72-83): computed
        // once per sample per active Program (not per voice, since LFOs are one resource shared
        // by every voice OF THE SAME PROGRAM) from that Program's own LFOs' outputs and its own
        // reference voice's assignable EGs. The reference voice for Program slot p is voices[p]
        // -- a direct generalization of today's single-Program "voice 0 is the reference"
        // convention, and matches the same dedicated-slot convention handle_note_on's own
        // mono-mode branch already uses. Outside Combi mode this resolves to SINGLE_PROGRAM_SLOT
        // (voices[8]), not voices[0] -- see the LFO computation loop's own comment on this same
        // resolution and why (mutual exclusivity between Single Program mode and Combi lane 1).
        // Applied to affect the NEXT sample's read-phase/rate/depth above -- a one-sample delay
        // that avoids any ordering ambiguity between "which LFO's output is current" within a
        // single sample. See lfo_rate_target_index/lfo_depth_target_index/lfo_phase_target_index's
        // comment for the target-number -> LFO-index mapping.
        for (int p_loop = 0; p_loop < active_program_count; p_loop++) {
            int p = combi_mode_active ? p_loop : SINGLE_PROGRAM_SLOT;
            const ProgramState *rprog = &knobs->programs[p];
            SynthVoice *ref_voice = &voices[p];
            float new_lfo_phase_mod[LFO_COUNT] = {0.0f};
            float new_lfo_rate_mod[LFO_COUNT] = {0.0f};
            float new_lfo_depth_mod[LFO_COUNT] = {0.0f};
            for (int l = 0; l < LFO_COUNT; l++) {
                int target = (int)rprog->lfo[l].dest_target;
                int ppidx = lfo_phase_target_index(target); if (ppidx >= 0) new_lfo_phase_mod[ppidx] += lfo_out[p][l];
                int pridx = lfo_rate_target_index(target); if (pridx >= 0) new_lfo_rate_mod[pridx] += lfo_out[p][l];
                int pdidx = lfo_depth_target_index(target); if (pdidx >= 0) new_lfo_depth_mod[pdidx] += lfo_out[p][l];
            }
            if (ref_voice->vco1_eg.state != STATE_IDLE || ref_voice->vco2_eg.state != STATE_IDLE) {
                for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) {
                    int target = (int)rprog->eg_dest_target[e];
                    float val = ref_voice->assign_eg[e].current_level * rprog->eg_intensity[e];
                    int ppidx = lfo_phase_target_index(target); if (ppidx >= 0) new_lfo_phase_mod[ppidx] += val;
                    int pridx = lfo_rate_target_index(target); if (pridx >= 0) new_lfo_rate_mod[pridx] += val;
                    int pdidx = lfo_depth_target_index(target); if (pdidx >= 0) new_lfo_depth_mod[pdidx] += val;
                }
            }
            for (int l = 0; l < LFO_COUNT; l++) {
                lfo_phase_mod_state[p][l] = new_lfo_phase_mod[l];
                lfo_rate_mod_state[p][l] = new_lfo_rate_mod[l];
                lfo_depth_mod_state[p][l] = new_lfo_depth_mod[l];
            }
        }

        // --- Multi-source bus dispatch ---
        // Sources 0..COMBI_PROGRAM_COUNT-1 are the 8 Combi Programs, each exactly its own
        // mixed_l/r as computed above (oscillators summed and run through Filter A/B together,
        // untouched by any of this) -- source 0 is byte-identical to today's single Program bus.
        // Source COMBI_PROGRAM_COUNT ("Combi Mix") is a new synthetic bus: the whole stack's raw
        // dry pre-FX sum, unconditionally including every Program regardless of whatever other
        // individual FX routing those same Programs also have -- lets one FX slot serially treat
        // the whole stack while other slots still individually target a single Program, a
        // deliberate, intentional overlap ("let them route"). Each external-input pair (the
        // remaining EXT_IN_PAIR_COUNT source ids) gets its own independent dry bus, tapped
        // directly from its own pair of capture channels (2p/2p+1) -- generalizes what used to be
        // a single hardcoded tap at capture channels 0/1, and is never summed into any Program's
        // own bus.
        // master_volume is session-level (scales the WHOLE session's output uniformly), applied
        // here to each Program's own bus individually -- mathematically equivalent to applying it
        // once globally, while also correctly scaling any Program routed individually (not via
        // Combi Mix) and keeping Combi Mix's own sum consistent with what each Program bus
        // actually outputs (applied BEFORE the Combi Mix sum below, not after, so a Program's
        // contribution to Combi Mix always matches its own individually-routed bus). Generalizes
        // today's Program-only `bus_l[0] *= knobs->master_volume;` cleanly. Ext-in pairs rely
        // solely on their own per-pair gain (below) as their equivalent "volume" knob, unchanged.
        // Uses the SMOOTHED value (chased toward the live knob every sample, see its own comment
        // above) rather than knobs->master_volume directly -- the raw knob value is constant for
        // an entire block and can jump by a large amount between two consecutive blocks (e.g. a
        // fast slider drag), which applied instantly is a real discontinuity in a continuous
        // audio signal -- an audible click, not just a theoretical one. Chasing it removes the
        // discontinuity while still reaching the target in ~10ms, imperceptible as a fade.
        // SFZ trigger=release/release_key one-shot players (see fire_release_trigger_zones's own
        // comment) -- summed directly into their own owner_program's mixed_l/r bus, exactly like
        // any of that Program's own voices, so they pick up the exact same master_volume/EQ/FX/
        // routing pipeline everything else on that bus already gets. A no-op scan (every player
        // inactive) for every session that never uses trigger=release at all, the ordinary case.
        for (int rt_i = 0; rt_i < HDS_RELEASE_TAIL_POOL_SIZE; rt_i++) {
            HdsReleaseTailPlayer *rt = &se->pending->release_tails[rt_i];
            if (!rt->active) continue;
            float rl = 0.0f, rr = 0.0f;
            render_sample_osc(se, &rt->stream_idx, &rt->ptr, &rt->preroll, rt->play_hz, &rl, &rr);
            if (rt->stream_idx < 0) { rt->active = false; continue; } // reached its own natural end-of-file
            rt->gain *= rt->decay_per_sample;
            mixed_l[rt->owner_program] += rl * rt->gain;
            mixed_r[rt->owner_program] += rr * rt->gain;
        }

        *master_volume_smoothed += (knobs->master_volume - *master_volume_smoothed) * master_volume_chase_rate;
        float bus_l[SOURCE_COUNT], bus_r[SOURCE_COUNT];
        float combi_mix_l = 0.0f, combi_mix_r = 0.0f;
        for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
            bus_l[p] = mixed_l[p] * (*master_volume_smoothed) * combi_lane_gain_l[p];
            bus_r[p] = mixed_r[p] * (*master_volume_smoothed) * combi_lane_gain_r[p];
            // Post-filter, pre-FX 3-band EQ -- see EqBandParams/apply_program_eq's own comment.
            // Gain-order-independent (both master_volume/combi_lane_gain above and this EQ are
            // linear), so applying it here vs. before the *master_volume_smoothed scaling is
            // equivalent -- done here since bus_l/bus_r are what's actually already in hand.
            apply_program_eq(engine, p, eq_coeffs[p], &bus_l[p], &bus_r[p]);
            combi_mix_l += bus_l[p];
            combi_mix_r += bus_r[p];
        }
        // Outside Combi mode, mixed_l[0..7] are all silent (the actual per-sample synthesis above
        // only ever wrote into mixed_l[SINGLE_PROGRAM_SLOT] -- see combi_mode_active's own
        // resolution comment), so the loop above left bus_l[0]/combi_mix silent too. Source 0
        // ("Program" in the FX/Routing UI) and Combi Mix must still carry Single Program mode's
        // own audio here, exactly as they always have -- pull it from its real home instead.
        if (!combi_mode_active) {
            bus_l[0] = mixed_l[SINGLE_PROGRAM_SLOT] * (*master_volume_smoothed);
            bus_r[0] = mixed_r[SINGLE_PROGRAM_SLOT] * (*master_volume_smoothed);
            apply_program_eq(engine, SINGLE_PROGRAM_SLOT, eq_coeffs[SINGLE_PROGRAM_SLOT], &bus_l[0], &bus_r[0]);
            combi_mix_l = bus_l[0];
            combi_mix_r = bus_r[0];
        }
        bus_l[COMBI_PROGRAM_COUNT] = combi_mix_l;
        bus_r[COMBI_PROGRAM_COUNT] = combi_mix_r;
        for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) {
            // Ext-in pairs now sit after the 8 Programs + Combi Mix (source ids 9-16, was 1-8 --
            // see SOURCE_COUNT's own BREAKING CHANGE comment).
            int src = COMBI_PROGRAM_COUNT + 1 + p;
            bus_l[src] = 0.0f;
            bus_r[src] = 0.0f;
            if (knobs->ext_in_enable[p] != 0.0f && 2 * p < ext_in_channels) {
                float l = ext_in_buffer[i * ext_in_channels + 2 * p];
                // Stereo (default, ext_in_stereo != 0): read the real R channel when the device
                // provides one, degrading to mono-duplicate only if it doesn't (e.g. a mono
                // capture device, or an odd channel count's last pair) -- matches this engine's
                // original implicit convention exactly (see the old single-pair tap this
                // generalizes, which did the same thing for channels 0/1). Mono (explicit,
                // ext_in_stereo == 0): always center channel 2p alone to both L/R, even when the
                // device does provide a channel 2p+1 -- lets a user say "this pair is genuinely
                // one mono source" instead of that choice being made implicitly by hardware
                // channel count alone (see this field's own comment on SynthSharedControls).
                float r = (knobs->ext_in_stereo[p] != 0.0f && 2 * p + 1 < ext_in_channels)
                              ? ext_in_buffer[i * ext_in_channels + 2 * p + 1]
                              : l;
                bus_l[src] = l * knobs->ext_in_gain[p];
                bus_r[src] = r * knobs->ext_in_gain[p];
                if (meter_ext_in) {
                    float pair_peak = fmaxf(fabsf(bus_l[src]), fabsf(bus_r[src]));
                    if (pair_peak > ext_in_peak_out[p]) ext_in_peak_out[p] = pair_peak;
                }
            }
        }
        // Drum Buses (see DRUM_BUS_COUNT's own comment): appended after every ext-in pair (ids
        // DRUM_BUS_SOURCE_BASE..SOURCE_COUNT-1), populated directly from drum_bus_l/r (filled by
        // individual oscillators inside the voice loop above, entirely bypassing this Program's own
        // Filter/EQ pipeline) rather than from mixed_l/ext_in_buffer. master_volume still applies --
        // it's session-level, scaling the whole session's output uniformly, same reasoning as every
        // Program's own bus just above (ext-in pairs are the one exception, relying solely on their
        // own per-pair gain instead). drum_bus_gain is this bus's own 0-200% trim (see its own
        // comment on SynthSharedControls) -- useful since a Drum Bus sums however many zones got
        // assigned to it, with no other per-bus level control otherwise.
        for (int b = 0; b < DRUM_BUS_COUNT; b++) {
            int src = DRUM_BUS_SOURCE_BASE + b;
            bus_l[src] = drum_bus_l[b] * (*master_volume_smoothed) * knobs->drum_bus_gain[b];
            bus_r[src] = drum_bus_r[b] * (*master_volume_smoothed) * knobs->drum_bus_gain[b];
        }

        // Sidechain snapshot: every source's own pre-FX dry signal THIS SAME SAMPLE, taken now
        // (bus_l/bus_r are fully populated for every source by this point, and the loop below is
        // about to start mutating them in place one source at a time) -- see process_fx_chain's
        // own sc_snapshot_l/r comment for why an external sidechain key must read this frozen copy
        // rather than the live array.
        float sc_snapshot_l[SOURCE_COUNT], sc_snapshot_r[SOURCE_COUNT];
        memcpy(sc_snapshot_l, bus_l, sizeof(sc_snapshot_l));
        memcpy(sc_snapshot_r, bus_r, sizeof(sc_snapshot_r));

        // FX Bus: each source runs its own chain independently, on its own bus -- see
        // process_fx_chain's own comment for the routing rationale and the source_routing comment
        // on SynthSharedControls for the bus/source model this generalizes to. A source that owns
        // no slots at all (source_has_fx_slots[], precomputed once per block above) is already a
        // guaranteed no-op dry passthrough per process_fx_chain's own zero-slot-count guard --
        // skip the call (and its FX_SLOT_COUNT-iteration scan) entirely rather than pay for that
        // guarantee 128 times a block for nothing, on every ext-in pair nobody has assigned an
        // effect to.
        for (int src = 0; src < SOURCE_COUNT; src++) {
            if (!source_has_fx_slots[src]) continue;
            process_fx_chain(knobs->fx, fx_engine, FX_SLOT_COUNT, /* always Parallel, matches HDS FX */ 1, src, sc_snapshot_l, sc_snapshot_r, SOURCE_COUNT, &bus_l[src], &bus_r[src], gr_db_raw, dump_avail_out, engine->fx_slot_out_l, engine->fx_slot_out_r);
        }

        // CLAP multi-out (see this function's own per_program_out comment) -- captured here,
        // after FX, so each port carries the exact same fully-processed signal Combi Mix itself
        // sums, not a pre-FX tap.
        if (per_program_out) {
            for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
                per_program_out[(size_t)i * COMBI_PROGRAM_COUNT * 2 + (size_t)p * 2 + 0] = bus_l[p];
                per_program_out[(size_t)i * COMBI_PROGRAM_COUNT * 2 + (size_t)p * 2 + 1] = bus_r[p];
            }
        }

        // No extra fixed headroom pad here beyond master_volume itself -- there used to be a
        // hardcoded *0.15 (~-16.5dB) on top of it, left over from before the always-on limiter
        // below existed (it dates back to this engine's very first commit; the limiter came
        // later). That pad was this engine's ONLY protection against summing-related clipping
        // at the time, but the limiter now does that job properly (instant-attack, smooth-
        // release gain reduction, explicitly designed for "several voices lining up" and
        // "12 FX slots summed in Parallel routing" -- see its own comment right below), making
        // the old pad pure redundant attenuation: quietly cutting every patch by ~16.5dB more
        // than master_volume alone would, for no remaining safety benefit. master_volume itself
        // was already applied above, to each Program bus (and so, transitively, to Combi Mix) --
        // see that comment for why it happens there now instead of here.

        // Sum every source into whichever output pair(s) it's routed to -- many-to-many: a source
        // can feed multiple pairs at once, several sources can share one pair.
        float pair_l[OUTPUT_PAIR_MAX] = {0.0f}, pair_r[OUTPUT_PAIR_MAX] = {0.0f};
        for (int src = 0; src < SOURCE_COUNT; src++) {
            for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
                if (knobs->source_routing[src].enable[p] != 0.0f) {
                    pair_l[p] += bus_l[src];
                    pair_r[p] += bus_r[src];
                }
            }
        }

        // Sequencer audio-lane playback (Preview button OR the real transport, see
        // render_seq_audio_preview_sample) -- summed straight into output pair 0, bypassing FX/
        // routing entirely (a real per-lane routing model is future work). Several lanes play
        // back simultaneously since each lane's own SeqAudioLane state is fully independent --
        // proving multitrack playback actually works, not just one lane in isolation. A
        // transport-driven lane (driven_by_transport) pauses/stops with the transport; a
        // manually Preview-triggered lane always plays through regardless of transport state.
        for (int l = 0; l < SEQ_LANE_COUNT; l++) {
            SeqAudioLane *lane = &g_seq_audio[l];
            if (!lane->is_playing) continue;
            if (lane->driven_by_transport && g_transport_state != TRANSPORT_PLAYING) continue;
            // Off (seq_mode == 1) on a transport-driven lane is a real compute bypass -- skip the
            // render call entirely (position frozen, not just silenced) -- per SeqLaneState's own
            // precedence comment. A manually Preview-triggered lane is never affected, same as
            // it's never affected by Pause/Stop -- Preview always plays straight through.
            if (lane->driven_by_transport && (int)knobs->seq_lanes[l].seq_mode == 1) continue;
            float sample_l, sample_r;
            render_seq_audio_preview_sample(lane, knobs->seq_lanes[l].volume, knobs->seq_lanes[l].pan, &sample_l, &sample_r);
            // Mute on a transport-driven lane silences it live, right now, rather than only
            // preventing it from auto-starting on the next Play -- but the render call above
            // still ran, so play_frame still advanced in step with the transport. Un-muting
            // therefore resumes exactly in sync with everything else, instead of picking up from
            // wherever it happened to be when it was muted. A manually Preview-triggered lane
            // (driven_by_transport == false) is never affected by Mute, same as it's never
            // affected by Pause/Stop -- Preview always plays straight through regardless.
            if (lane->driven_by_transport && knobs->seq_lanes[l].mute != 0.0f) continue;
            pair_l[0] += sample_l;
            pair_r[0] += sample_r;
        }

        // Metronome click (see poll_metronome, which schedules g_metronome_click_samples_left):
        // a raw, hardcoded 400Hz square wave, summed straight into output pair 0 same as the
        // Sequencer audio-lane playback right above -- deliberately not a Program voice, so it
        // never touches the oscillator/filter/envelope machinery and costs nothing when idle
        // (the overwhelmingly common case, checked with one cheap comparison).
        if (g_metronome_click_samples_left > 0) {
            int half_period_samples = (int)((float)SAMPLE_RATE / (METRONOME_CLICK_HZ * 2.0f));
            if (half_period_samples < 1) half_period_samples = 1;
            float click_val = ((g_metronome_click_phase / half_period_samples) % 2 == 0) ? METRONOME_CLICK_GAIN : -METRONOME_CLICK_GAIN;
            pair_l[0] += click_val;
            pair_r[0] += click_val;
            g_metronome_click_phase++;
            g_metronome_click_samples_left--;
        }

        // Always-on master safety limiter, now evaluated once per output pair (after every source
        // routed to that pair has been summed in -- its whole purpose, the DAC never seeing an
        // out-of-range sample, only makes sense evaluated after a pair's final sum, not per
        // source). The hard clamp below guarantees the DAC never sees a sample outside
        // [-ceiling, ceiling], but on its own that's brick-wall clipping -- ugly, harmonically
        // harsh distortion on any over. This applies smooth gain reduction instead: instant attack
        // (so a sudden peak -- e.g. several voices lining up, or 12 FX slots summed in Parallel
        // routing -- still can never clip) but a short release so the reduction isn't audible as
        // pumping on an isolated transient. Both channels of a given pair always share one gain
        // value so the stereo image isn't skewed.
        const float limiter_ceiling = 0.98f;
        for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
            float peak_now = fmaxf(fabsf(pair_l[p]), fabsf(pair_r[p]));
            // Already fully open and under the ceiling -- multiplying by 1.0 is a no-op, so skip
            // the transcendental release-rate math (expf) entirely rather than pay for it on
            // every one of the OUTPUT_PAIR_MAX pairs every sample regardless of whether anything
            // is even routed there. Bit-identical to running the full math below, since the
            // release update's own delta (needed_gain - limiter_gain) is exactly 0 in this case.
            if (peak_now <= limiter_ceiling && limiter_gain[p] == 1.0f) continue;
            float needed_gain = (peak_now > limiter_ceiling) ? (limiter_ceiling / peak_now) : 1.0f;
            if (needed_gain < limiter_gain[p]) {
                limiter_gain[p] = needed_gain; // attack: instant, so THIS sample can never clip
            } else {
                float release_rate = 1.0f - expf(-1.0f / (0.050f * SAMPLE_RATE)); // ~50ms release
                limiter_gain[p] += (needed_gain - limiter_gain[p]) * release_rate;
            }
            pair_l[p] *= limiter_gain[p];
            pair_r[p] *= limiter_gain[p];
        }

        // Write each computed output pair into out_buffer, interleaved at output_channels stride --
        // one real multichannel ALSA device opened wide (see query_output_device_channel_count()/
        // open_pcm_stream() in main()), generalizing what used to be a single hardcoded stereo
        // pair. output_channels==2 with only source_routing[0].enable[0] set (today's only
        // default) reproduces the old single-stereo-output behavior bit-for-bit.
        int output_pairs = output_channels / 2;
        if (output_pairs > OUTPUT_PAIR_MAX) output_pairs = OUTPUT_PAIR_MAX;
        for (int p = 0; p < output_pairs; p++) {
            float l = fmaxf(-limiter_ceiling, fminf(limiter_ceiling, pair_l[p]));
            float r = fmaxf(-limiter_ceiling, fminf(limiter_ceiling, pair_r[p]));
            out_buffer[i * output_channels + 2 * p] = l;
            out_buffer[i * output_channels + 2 * p + 1] = r;
            if (fabsf(pair_l[p]) > peak) peak = fabsf(pair_l[p]);
            if (fabsf(pair_r[p]) > peak) peak = fabsf(pair_r[p]);
        }
        // Any leftover channel(s) beyond the last full pair (an odd total channel count, or a
        // pathological <2-channel device) have no pair partner in the source-routing model --
        // left silent rather than guessing at a downmix.
        for (int c = 2 * output_pairs; c < output_channels; c++) {
            out_buffer[i * output_channels + c] = 0.0f;
        }
    }

    // GR meter ballistics -- chases gr_db_out (persistent, caller-owned) toward this block's own
    // gr_db_raw reading, same per-block exponential-chase formula as lfo_tempo_chase_rate above,
    // just with its own two time constants instead of one: fast attack (~15ms, so a real gain-
    // reduction event still shows up quickly, not just a slowly-fading-in number) and much slower
    // release (~350ms, long enough for a human eye to actually read a two/three-digit dB value
    // before it moves again) -- the standard fast-attack/slow-release shape a hardware VU/GR meter
    // ballistics circuit already uses, for exactly the same reason. See gr_db_out's own comment.
    {
        const float gr_attack_rate = 1.0f - expf(-(float)block_size / (SAMPLE_RATE * 0.015f));
        const float gr_release_rate = 1.0f - expf(-(float)block_size / (SAMPLE_RATE * 0.35f));
        for (int s = 0; s < FX_SLOT_COUNT; s++) {
            float rate = (gr_db_raw[s] > gr_db_out[s]) ? gr_attack_rate : gr_release_rate;
            gr_db_out[s] += (gr_db_raw[s] - gr_db_out[s]) * rate;
        }
    }
    return peak;
}
