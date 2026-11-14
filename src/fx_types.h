// fx_types.h: the FX slot data model -- FX_TYPE_* (which effect a slot runs), FXSlotParams (a
// slot's user-facing knobs: type, its generic param[] bank, mix, bypass, routing), and
// FXSlotEngine (a slot's persistent runtime state, opaque per effect) -- plus EqBandParams/
// EqBiquadCoeffs/compute_eq_band_coeffs/run_eq_biquad, the generic RBJ-cookbook biquad EQ math
// FX_TYPE_EQ3 needs (the exact same math the synth's own per-Program post-filter EQ already uses,
// via apply_program_eq() in backend.c -- one generic implementation, two callers). Shared between
// backend.c (the synth's own embedded FX bus, driven over OSC) and src/hds_fx_plugin.c (the
// standalone HDS FX CLAP plugin, driven by native CLAP params, no OSC/networking at all). Both
// link the same separately compiled fx_chain.o (see fx_chain.h) -- so the exact same ported effects
// (HT-76, Fairchild 670, LA-2A, FDNVerb, NAM, ...) run identically in both, and
// a slot's param layout is byte-for-byte interchangeable between them.
//
// Pure code motion out of backend.c: nothing here was rewritten, just lifted out (verbatim, own
// doc comments intact) so a second includer could exist at all -- see fx_chain.c's own header
// comment for why IT already lived as a separate #include'd-by-backend.c file, and
// hds_fx_plugin.c's own header comment for the new plugin this made possible.
#ifndef HDS_FX_TYPES_H
#define HDS_FX_TYPES_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "hds_config.h"

// One band of a Program's post-filter 3-band EQ (see ProgramState.eq_bands[3] and
// run_eq_band()/render_audio_block's EQ dispatch for the DSP). Band 0 = Low Shelf, band 1 = Mid
// Peak, band 2 = High Shelf -- a standard channel-strip topology, RBJ Audio-EQ-Cookbook biquad
// forms (shelf bands ignore `q`, using a fixed shelf slope instead -- see run_eq_band()). All
// gain in dB, +-18dB range (generous for tone-shaping without inviting a full mixing-console's
// worth of gain-staging risk); freq in Hz.
typedef struct {
    float freq;
    float gain_db;
    float q; // Mid Peak only; ignored for the two Shelf bands
} EqBandParams;

// --- Post-filter 3-band EQ (see EqBandParams/ProgramState.eq_bands) ---
// RBJ Audio-EQ-Cookbook biquad coefficients, Direct Form II Transposed (2 state variables: y =
// b0*x + z1; z1' = b1*x - a1*y + z2; z2' = b2*x - a2*y -- the standard, numerically robust
// implementation for a musical shelf/peak EQ band). Every coefficient here is already normalized
// by the cookbook's own a0 (a0 == 1 implicitly), so run_eq_biquad() never needs it.
typedef struct { float b0, b1, b2, a1, a2; } EqBiquadCoeffs;

// band_index: 0 = Low Shelf, 1 = Mid Peak, 2 = High Shelf (see EqBandParams' own comment). Shelf
// slope is fixed at S=1 (a gentle, musical shelf -- the cookbook's own suggested default): this
// engine exposes freq/gain per shelf band, not a separate slope control, matching a typical
// hardware channel-strip EQ rather than a fully general parametric-shelf design. gain_db==0 is a
// true identity filter algebraically (b0=a0/b1=a1/b2=a2 before normalizing falls out of the same
// formulas at A=1), but skipped here anyway as a cheap, explicit bypass -- no trig needed at all
// for a band a player has left flat.
static inline EqBiquadCoeffs compute_eq_band_coeffs(const EqBandParams *band, int band_index) {
    EqBiquadCoeffs c = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // transparent passthrough
    float gain_db = fmaxf(-18.0f, fminf(18.0f, band->gain_db));
    if (gain_db == 0.0f) return c;
    float freq_hz = fmaxf(20.0f, fminf(20000.0f, band->freq));
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq_hz / (float)SAMPLE_RATE;
    float cos_w0 = cosf(w0), sin_w0 = sinf(w0);
    float b0, b1, b2, a0, a1, a2;
    if (band_index == 1) {
        float q = fmaxf(0.1f, fminf(10.0f, band->q));
        float alpha = sin_w0 / (2.0f * q);
        b0 = 1.0f + alpha * A;   b1 = -2.0f * cos_w0;   b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;   a1 = -2.0f * cos_w0;   a2 = 1.0f - alpha / A;
    } else {
        const float S = 1.0f; // shelf slope, see this function's own comment
        float alpha = sin_w0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float sqrtA2alpha = 2.0f * sqrtf(A) * alpha;
        if (band_index == 0) { // Low Shelf
            b0 =        A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + sqrtA2alpha);
            b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0);
            b2 =        A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - sqrtA2alpha);
            a0 =             (A + 1.0f) + (A - 1.0f) * cos_w0 + sqrtA2alpha;
            a1 =       -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0);
            a2 =             (A + 1.0f) + (A - 1.0f) * cos_w0 - sqrtA2alpha;
        } else { // High Shelf (band_index == 2)
            b0 =        A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + sqrtA2alpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
            b2 =        A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - sqrtA2alpha);
            a0 =             (A + 1.0f) - (A - 1.0f) * cos_w0 + sqrtA2alpha;
            a1 =        2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
            a2 =             (A + 1.0f) - (A - 1.0f) * cos_w0 - sqrtA2alpha;
        }
    }
    float inv_a0 = 1.0f / a0;
    c.b0 = b0 * inv_a0; c.b1 = b1 * inv_a0; c.b2 = b2 * inv_a0;
    c.a1 = a1 * inv_a0; c.a2 = a2 * inv_a0;
    return c;
}

static inline float run_eq_biquad(float input, const EqBiquadCoeffs *c, float *z1, float *z2) {
    float y = c->b0 * input + *z1;
    float new_z1 = c->b1 * input - c->a1 * y + *z2;
    float new_z2 = c->b2 * input - c->a2 * y;
    // Same defense-in-depth hard state clamp as run_svf_filter's own low_state/band_state -- see
    // its comment for the rationale (a held note running this recursively for minutes should
    // degrade to a recoverable clip, never a permanent NaN/Inf lockup, however unlikely).
    *z1 = fmaxf(-1.0e6f, fminf(1.0e6f, new_z1));
    *z2 = fmaxf(-1.0e6f, fminf(1.0e6f, new_z2));
    return y;
}

// A single FX bus slot's parameters. FX_SLOT_COUNT of these run in series (slot 0 first) on
// the master stereo bus -- see the FX section comment on SynthSharedControls and
// process_fx_chain() for the routing and dispatch. fx_type is a generic float index rather
// than its own enum for now: 0 is the only defined value (Off/bypass, a pure pass-through),
// deliberately left open so each ported effect can claim the next index without touching the
// wire layout. param[] is an intentionally generic bank -- headroom for whatever a given
// effect's own algorithm needs (a delay's time/feedback/tone, a reverb's size/damp/predelay,
// etc.); each effect interprets its own subset however it needs to once ported in.
#define FX_TYPE_OFF 0
// Ported from Airwindows StereoEnsemble (MIT license). Not a reverb -- a 12-tap fixed-delay
// ensemble/chorus (no feedback loop). param[0] = Depth (0..1, widens/lengthens the 12 delay
// taps), param[1] = FX Level (0..1, this effect's OWN internal wet/dry balance -- separate
// from and applied in addition to the slot's generic `mix` above). See run_fx_ensemble().
#define FX_TYPE_ENSEMBLE 1
// Ported from Airwindows CreamCoat (MIT license). A genuine reverb (16-line Householder FDN).
// param[0] Select (room preset, 0..1 -> 17 presets), param[1] Regen (feedback, 0..1),
// param[2] DeRez (tank downsample rate, 0..1), param[3] Predelay (0..1), param[4] Wetness
// (0..1 -- NOT a crossfade, see run_fx_creamcoat's comment). See run_fx_creamcoat().
#define FX_TYPE_CREAMCOAT 2
// Ported from Airwindows Density3 (MIT license). A highpass (leaky-integrator subtraction) into
// a density/waveshaping saturator. param[0] Density (0..1 -> 0..5 internally; >0.2 pushes into
// an increasingly hard sine-based clipper, <0.2 into a softer arcsine-family curve, blending
// smoothly through the neutral point), param[1] Highpass (0..1, how much of a leaky-integrator
// lowpass is subtracted from the signal before saturating), param[2] Output (0..1, linear
// makeup gain applied after saturation). The original plugin's own internal Dry/Wet (its 4th
// param) is deliberately NOT exposed as its own control -- the slot's generic `mix` above
// already provides an equivalent crossfade, and exposing both would just be two knobs doing the
// same job. See run_fx_density3().
#define FX_TYPE_DENSITY3 3
// Ported from Airwindows StereoChorus (MIT license). A stereo-widened flanger/chorus: two
// independently LFO-swept delay reads (deliberately phase-offset between L/R) into a single
// integer-quantized ring buffer, plus an "air" comb-filter stage restoring some of the top end a
// short modulated delay naturally smears away. param[0] Speed, param[1] Depth. See
// run_fx_stereochorus().
#define FX_TYPE_STEREOCHORUS 4
// Ported from Airwindows StereoDoubler (MIT license). A granular micro-pitch detuner: 8
// simultaneous delay-read "voices" per channel at slightly different (and, between L/R,
// oppositely-signed) speeds, crossfade-spliced between whichever pair conflicts least whenever
// one wraps, avoiding the audible clicks a naive single-tap pitch shifter would produce.
// param[0] Detune (trim direction/amount, split oppositely between L and R), param[1] Dry/Wet
// (this effect's own internal crossfade). Kept as its own control here -- unlike Density3's,
// which was dropped as redundant with the slot's generic `mix` -- because this is one of only
// two params the original exposes at all (matching Ensemble's 2-knob shape rather than
// Density3's 3), and because doubling/detuning effects are conventionally dialed in as their own
// self-contained mix, independent of whatever the slot's outer Mix knob is doing. See
// run_fx_stereodoubler().
#define FX_TYPE_STEREODOUBLER 5
// Ported from Airwindows TapeDelay2 (MIT license). A single tape-style echo: one modulated
// (tape-flutter) delay line per channel, with a resonant lowpass INSIDE the feedback loop
// (progressively darkening each successive repeat, the classic tape-echo "regen" filter) and a
// second, slightly-higher-resonance lowpass on the way OUT of the delay (an "off the tape head"
// tone-shaping stage). param[0] Time (counter-intuitively inverted, exactly as the original: 0.0
// is the LONGEST delay, 1.0 the shortest -- the knob really controls tape speed, not delay time
// directly), param[1] Regen (feedback amount), param[2] Freq (regen/out filter cutoff),
// param[3] Reso (regen filter resonance -- the out filter's own resonance is always this times a
// fixed golden-ratio multiplier, matching the original), param[4] Flutter (delay-speed vibrato
// depth, scaled by Freq so brighter/faster settings flutter a little more, matching the
// original), param[5] Dry/Wet (this effect's own internal blend -- NOT a crossfade: raising it
// from 0 first fades in only the wet signal without turning down dry at all, matching the
// original's own "submix echo" design so mixing this into a bus doesn't quietly pull back
// everything else -- see run_fx_tapedelay2's own comment). param[6] Ping-Pong: NOT part of the
// original TapeDelay2 -- a 2-way choice addition (see run_fx_tapedelay2's own comment), default
// Off reproducing the original algorithm exactly, bit for bit. "Stereo Ping-Pong" cross-feeds each
// channel's delay output into the OTHER channel's feedback path AND diverges L/R's own delay time
// -- both together, not just the cross-feed alone (tried and cut -- a cross-feed with no time
// divergence is nearly inaudible on anything but a strongly asymmetric source, since L and R would
// still land on the exact same beat).
#define FX_TYPE_TAPEDELAY2 6
// Ported from Airwindows PointyGuitar (MIT license). A guitar-amp-style distortion: the input
// runs through a cascade of `poles` (6-16, driven by Drive) identical stages, each stage itself
// splitting the signal into 9 successively-slower one-pole lowpass "bands" (a fixed 0.618 ratio
// apart -- the golden ratio, same constant used elsewhere in these ports) and re-summing them
// weighted by 5 independently-controlled band gains (Presence/High/Mid/Low/Sub, with the 4
// in-between weights each the plain average of their neighbors) before a hard clip -- so the
// harmonic content the distortion generates is shaped by those 5 gains on the way through, not
// just after the fact. A zero-crossing-aware noise gate runs on the CLEAN input (before
// distortion) but is applied to the DISTORTED output (after), so the gate's timing tracks the
// real signal while its "click-free" closing blends toward a soft cosine curve instead of hard
// silence. Two speaker-cabinet-style filters finish the chain: HSpeakr (a genuine multi-pole
// lowpass -- a cabinet's natural top-end rolloff) then LSpeakr (a partial bass cut -- a small
// speaker's inability to reproduce sub content). This effect needs 10 real, independent params
// (5 of them are the distortion's own core tone-shaping, not optional extras the way a plain
// Dry/Wet would be) -- see FX_SLOT_PARAM_COUNT's own comment just below on why it was raised from
// 8 to 10 to fit this port without dropping any of them.
// param[0] Drive, param[1] Presence, param[2] High, param[3] Mid, param[4] Low, param[5] Sub,
// param[6] HSpeakr, param[7] LSpeakr, param[8] Gate (threshold), param[9] Output (makeup gain) --
// the original's own A-J order, unchanged.
#define FX_TYPE_POINTYGUITAR 7
// Ported from Airwindows GuitarConditioner (MIT license). A parameterless (no knobs at all, same
// as the original -- kNumParameters == 0) treble/bass conditioner: the signal is split into a
// treble path and a bass path, each run through its own dynamically-adaptive one-pole highpass
// (the pole coefficient itself depends on the CURRENT sample's own level -- "offset" below --
// rather than being fixed, a signal-dependent filter rather than a plain static one), then each
// path's sample-to-sample delta is slew-limited (clamped to a fixed max step) before the two
// paths are summed back together. Both the treble and bass highpasses alternate between two
// independent one-pole memories every other sample (`fpFlip`) -- ported as-is; this is the
// original's own technique, not something this port added. No Mix-equivalent knob to drop or
// keep here -- there's simply nothing to expose, the slot's generic Mix/Bypass are the only
// controls this effect has.
#define FX_TYPE_GUITARCONDITIONER 8
// Ported from Airwindows Cabs2 (MIT license). A simple speaker-cabinet-style tone shaper: a
// 21-stage highpass (built, like GuitarConditioner's own filters, from cascaded one-pole lowpass
// stages subtracted from the signal rather than a direct highpass topology) followed by a
// 13-stage direct one-pole lowpass. Both stages self-bypass at their own "wide open" extreme
// (Hipass at its minimum, Lowpass at its maximum) -- and when a stage transitions INTO bypass, it
// zeroes its own filter memory once, so re-engaging it later starts clean rather than resuming
// from stale state. param[0] Lowpass (0 = darkest/most filtered, 1 = fully open/bypassed),
// param[1] Hipass (0 = fully open/bypassed, 1 = most filtered).
#define FX_TYPE_CABS2 9
// Neural Amp Modeler (NeuralAmpModelerCore, MIT license, vendored under
// third_party/NeuralAmpModelerCore -- see nam_bridge.h/.cpp, the only C++ this codebase touches).
// Runs a trained .nam amp-model file (a WaveNet or LSTM network, NOT an EQ/filter approximation
// like GuitarConditioner/Cabs2 above) through two independent model instances, one per channel, so
// each side's own convolution/hidden-state history stays genuinely separate -- see run_fx_nam()'s
// own comment on why sharing one instance between L/R would be wrong. An optional cabinet
// impulse-response convolution stage follows the model (NeuralAmpModelerCore itself only ships a
// raw WAV-decode helper, not a ready convolution engine -- see run_fx_nam()'s own comment on why
// this file's existing seq_audio_load_wav_from_disk() plus a hand-rolled direct-form FIR was used
// instead of a second file-format dependency). param[0] Input Gain (0.0-1.0 like every other
// effect's param[] -- see run_fx_nam()'s own comment for the +/-20dB mapping, pre-model trim: most
// trained models expect a specific input level, this is how you match a hotter or quieter source
// to what the model was trained on), param[1] Output Gain (same +/-20dB mapping, post-model/
// post-cab makeup gain), param[2] Cab Enable (0/1 -- lets a loaded cab IR be A/B'd against the bare
// model without unloading it). Model and cab IR are loaded via two dedicated string fields, nam_model_path and
// nam_ir_path (see their own OSC_STRING_SUBARRAY registration below and poll_fx_nam_requests()'s
// own comment on why a plain generic string field, not a dedicated load message, is exactly the
// right mechanism here -- it's the same one Sequencer audio lanes already use for their own
// audio_path).
#define FX_TYPE_NAM 10
// A simple, zero-latency noise gate -- no lookahead buffer anywhere (see run_fx_gate()'s own
// comment), so nothing here ever delays the signal path; only the GAIN this applies is smoothed
// (an envelope, not a delay), the same "instant reaction, no added latency" posture the master bus
// safety limiter below already uses. param[0] Threshold (-60..0dB -- the level a signal must be
// AT OR ABOVE for the gate to open), param[1] Attack (0.1..20ms -- how fast the gate opens once
// above threshold), param[2] Release (5..500ms -- how fast it closes once below threshold, slow
// enough to avoid audibly chopping a decaying note's own tail), param[3] Range (0..-80dB -- how
// much the signal is attenuated while closed; 0 makes this whole effect a no-op by design, a real
// "Range" knob's own honest zero position, not a magic bypass case), param[4] Hysteresis (0..12dB
// -- once open, the gate only re-closes after the signal drops this many dB BELOW Threshold, not
// at Threshold itself; the standard Schmitt-trigger fix for a signal hovering right at Threshold
// chattering open/closed on every little fluctuation -- see run_fx_gate()'s own comment for the
// open/close state machine), param[5] Hold (0..500ms -- once the signal actually does drop below
// the (Threshold - Hysteresis) close point, the gate stays fully open this much longer before its
// Release ramp even starts, so a single quiet gap inside an otherwise-sustained sound can't
// re-trigger the gate mid-phrase). Both channels always share one gain value (see run_fx_gate()'s
// own comment) so the stereo image is never skewed by one side gating independently of the other.
// hds_fx_dropdown_apply() seeds Hysteresis to a typical hardware-gate default (see its own
// comment) the moment a slot is first switched to this type, since 0 (this param's own honest
// "off" position, like Range's) is a worse default for a brand-new gate than most players would
// expect -- every other param here still starts at its own honest zero/minimum.
#define FX_TYPE_GATE 11
// The Program's own post-filter 3-band EQ (see EqBandParams/compute_eq_band_coeffs/run_eq_biquad
// above), ported here unchanged -- same RBJ Audio-EQ-Cookbook biquad forms, same Low Shelf/Mid
// Peak/High Shelf topology, just callable per FX slot instead of only once per Program. param[0]
// Low Shelf Freq, param[1] Low Shelf Gain, param[2] Mid Peak Freq, param[3] Mid Peak Gain,
// param[4] Mid Peak Q, param[5] High Shelf Freq, param[6] High Shelf Gain -- Freq params use the
// same 0.0-1.0 fraction every other effect's param[] does (see run_fx_eq3()'s own comment for the
// log-frequency mapping this decodes it with, 20Hz-20kHz); Gain is +/-18dB, matching
// compute_eq_band_coeffs()'s own clamp exactly (so this can never do anything the Program EQ
// itself couldn't already do). Shelf bands ignore Q entirely, same as the original.
#define FX_TYPE_EQ3 12
// A digital brick-wall peak limiter: the DAC (or the next thing in the chain) is mathematically
// guaranteed to never see a sample above Ceiling, the same "instant attack, so THIS sample can
// never clip" technique the always-on master bus safety limiter (see its own comment, further
// down near render_audio_block) already uses for the exact same guarantee, just user-controllable
// per slot here instead of a fixed, hidden safety net. NOT a port of Waves' L1 Ultramaximizer --
// L1 is closed-source with no public algorithm reference, unlike the MIT-licensed Airwindows
// effects this file otherwise ports verbatim -- this is an original implementation of the same
// well-known DSP TECHNIQUE L1 popularized (instant peak-limiting to a hard ceiling with no
// lookahead, so zero added latency, unlike a true lookahead limiter which needs a delay line to
// see a transient coming). param[0] Ceiling (-20..0dB -- the hard output ceiling), param[1] Input
// Gain (0..+24dB -- drives the signal INTO the limiter harder for a louder, denser result without
// raising the ceiling, the classic "loudness maximizer" use of this exact knob pair), param[2]
// Release (10..500ms). Attack is NOT a param -- always instant, like the master limiter, since
// that's the one thing that actually makes this "brick wall" (a slower attack would let a fast
// transient through above Ceiling before the gain reduction catches up).
#define FX_TYPE_LIMITER 13
// Ported from fetcomp-dsp (github.com/Paulllux/fetcomp-dsp, MIT license) -- the DSP behind
// FetComp, a circuit-derived model of the UREI 1176LN FET limiting amplifier: a
// voltage-divider gain cell (a JFET solved via a closed-form quadratic every sample, the same
// equation the real Rev D schematic's own gain-reduction stage implies), a diode detector/two-node
// timing network feeding it back (the sidechain taps AFTER the cell, closing the loop the way the
// real unit does -- see run_fx_fetcomp()'s own comment for exactly what's ported faithfully and what's
// simplified off the original's JUCE dependency). param[0] Input (-20..+40dB -- the hardware has no
// separate Threshold; you drive INTO a fixed one with this knob, which is genuinely how the real
// 1176 works and why input level/gain-reduction/distortion are inseparable here, same as there),
// param[1] Output (-20..+20dB, makeup gain after the cell), param[2] Ratio (choice: 4:1/8:1/12:1/
// 20:1/All -- All shorts every ratio position's own resistor tap in parallel, the real unit's own
// "all buttons in" trick, steepest slope and lowest threshold of the five), param[3] Attack
// (20..800us), param[4] Release (50..1100ms), param[5] Limiting (choice: Limit/Character Only --
// the real unit's Attack knob has an OFF detent past its slowest setting where "signal is passing
// through the circuitry but with a compression ratio of 1:1" -- all the gain-cell/Class-A/
// transformer coloration, none of the gain reduction), param[6] Character (0..0.5 -- the LN
// linearisation network's feedback fraction; 0.5 exactly cancels the cell's own 2nd harmonic, 0.0 is
// the earlier, dirtier Rev A response with the full harmonic present), param[7] Iron (0..1 -- how
// hard the Class A line amp and output transformer are driven; 0 takes both out of the signal path
// entirely, leaving just the bare gain cell). No separate Mix param -- the slot's own generic Mix
// knob already does that job, same convention as Density3/EQ3 above.
#define FX_TYPE_FETCOMP 14
// Ported from PHU-FAIR-KID-67 (github.com/huberp/phu-fair-kid-67, MIT license, DSP core from its
// sibling github.com/huberp/phu-audio-lib, also MIT) -- a circuit-derived model of a classic
// vari-mu tube leveling amplifier's own gain stage: a warm-started Newton-Raphson solve of each
// triode's Koren-model plate/cathode KCL equations (not a closed-form quadratic like
// FX_TYPE_FETCOMP's own gain cell -- see run_fx_fairkid67()'s own comment for exactly what's kept
// faithful and what's simplified off the original's JUCE dependency), a push-pull pair of these
// gain stages for even-harmonic cancellation, a tube sidechain amplifier cascade driving a
// diode-rectifier detector, and input/interstage/output transformer coloration. param[0] Input
// (-20..+40dB -- drives both the main signal path and the sidechain detector harder together,
// same "no separate Threshold, you drive into a fixed one" posture as FX_TYPE_FETCOMP), param[1]
// Output (-20..+20dB, makeup gain after the output transformer), param[2] Threshold (0..5V --
// subtracted from the raw detector voltage before it can drive any gain reduction at all), param[3]
// Time Constant (choice: 6 positions matching the original's own timing-preset table, P1 fastest
// release through P6 slowest, P5/P6 using a program-dependent dual-branch release), param[4] Link
// Mode (choice: Independent/Linked-Max/Linked-Avg/Mid-Side -- how the L/R detector voltages combine
// before driving both channels' gain stages), param[5] Bias (0..2V, an always-on additive offset to
// the detector voltage -- lets a slot sit in permanent light gain reduction if desired), param[6]
// Iron (0.5..3.0x -- scales all three transformers' own saturation drive together), param[7]
// Sidechain Gain (0..2.0x -- scales the tube cascade's own drive into the detector, independent of
// Input). No separate Ratio param -- like the original hardware, compression amount emerges
// entirely from Input/Threshold/tube nonlinearity, not a dialled-in slope. Sidechain Source is NOT
// one of these params -- see FXSlotParams.sidechain_source's own comment for the dedicated per-slot
// field (matching fx_source's own convention) that lets this effect's detector key off a
// DIFFERENT source (e.g. an ext-in pair) than the audio it's actually compressing.
#define FX_TYPE_FAIRKID67 15
// Ported from JClones_L2 (github.com/JClones/JSFXClones, MIT license) -- a clone of a widely-used
// mastering peak limiter/loudness maximizer, itself the reference this port's own reasoning keeps
// pointing back to. Structurally different from every earlier FX slot in this file: it's the first
// one that's LOOKAHEAD (it delays its own audio by a fixed number of samples so gain reduction can
// be computed from future peaks before they arrive) rather than zero-latency -- see run_fx_peakmax()
// and PEAKMAX_MAX_LOOKAHEAD's own comments for exactly what that costs. param[0] Threshold (-30..0dB
// -- really an input gain boost of -Threshold dB, so limiting always happens against a fixed 0dBFS
// ceiling internally, the same "boost in, brickwall at a fixed point, trim on the way out" shape the
// original uses), param[1] Ceiling (-30..0dB, final output trim -- keeps genuine peaks at or below
// this level), param[2] Auto Release (choice: Off/On -- On replaces param[3]'s fixed release time
// with a 3-stage adaptive detector that reads its own release speed off the program material, the
// original's own signature behavior), param[3] Release (choice: 6 fixed times, 0.01/0.1/1.0/10.0/
// 100/1000ms -- ignored while Auto Release is On), param[4] Quantize (choice: 16/18/20/22/24-bit --
// simulates truncating the signal to that bit depth, independent of the engine's own always-float
// internal signal path or whatever bit depth actually reaches the audio device/a WAV export; this is
// a deliberate, self-contained coloration effect, same as a bit-crusher, not something that requires
// this slot to be "the last thing before output" to make sense), param[5] Dither (choice: None/
// Type1/Type2 -- None still truncates+noise-shapes at Quantize's bit depth, just without added
// dither noise, a distinct and deliberately audible truncation-distortion mode, not a bypass),
// param[6] Shaping (choice: None/Moderate/Normal/Ultra -- shapes the dither noise's own spectrum
// away from the most audible range). No Mix-redundant param and no sidechain input -- like the
// original, this works on its own stereo signal only.
#define FX_TYPE_PEAKMAX 16
// A general-purpose steep/shallow filter for the FX bus -- distinct from the per-Program filter
// slots (ProgramState.filters[5]/FilterParams), which stay exactly as they are; this is its own
// self-contained implementation reusing the SAME proven TPT state-variable filter math (Vadim
// Zavalishin's "Art of VA Filter Design", the same reference run_svf_filter's own comment already
// credits) rather than sharing that function directly, so this addition can never change the
// existing per-voice filters' own behavior. param[0] Type (choice: Lowpass/Highpass/Bandpass/Notch,
// same exclusive-pick vocabulary as FilterParams.filter_type), param[1] Cutoff (20Hz-20kHz,
// log-mapped, same fx_eq3_fraction_to_hz() convention as FX_TYPE_EQ3's own Freq params), param[2]
// Resonance (0-100, same k=max(0.05,2*(1-res/100)) damping-floor formula as run_svf_filter's own
// Res knob, so it feels identical to the synth's own filters), param[3] Slope (choice: 12 steps,
// 6/12/18/24/30/36/42/48/60/72/84/96 dB/oct -- built by cascading N identical 2-pole TPT SVF
// stages, one 12dB/oct step each, plus a single extra 1-pole stage for an odd 6dB remainder on
// Lowpass/Highpass only; Bandpass/Notch have no natural 1-pole equivalent, so an odd slope choice
// on those two types rounds DOWN to the next 12dB step, and 6dB itself clamps up to 12dB -- the
// smallest meaningful bandpass/notch this topology can express). Every stage shares the same
// Cutoff/Resonance, so resonance compounds across stages at steep slopes -- the same cascaded-
// identical-stages character run_svf_filter's own existing 12->24dB step already has, just carried
// further; this is expected, not a bug (the same "analog ladder cascade" flavor, not a textbook
// per-stage-tuned Butterworth design).
#define FX_TYPE_MULTIFILTER 17
// Ported from 4a2a (github.com/aim-qmul/4a2a, MPL-2.0 license) -- a feed-forward digital
// approximation of a classic optical leveling amplifier/compressor. Genuinely small: a hard-knee
// power-law gain computer (no separate detector/envelope stage -- the instantaneous absolute sample
// value IS the level reading) followed by one asymmetric-coefficient one-pole smoother on the
// resulting gain value itself, then makeup gain. param[0] Threshold (-60..0dB), param[1] Ratio
// (1..20 -- 1 is the original's own default, i.e. no compression at all until turned up), param[2]
// Attack (0.1..100ms), param[3] Release (100..1000ms), param[4] Makeup Gain (-12..+12dB). The
// original's own sixth ("peakReduction", a single-knob macro mapped onto these five in its GUI
// layer) and seventh ("limitMode", declared but never read by its own DSP) params are both left out
// -- the macro is UI convenience, not DSP, and limitMode has no effect in the source this was ported
// from. The original is mono-only (its own input/output buses are both `.mono()`); this stereo
// adaptation follows the exact same "one shared gain value off max(|L|,|R|), never independent
// per-channel" posture as FX_TYPE_GATE/FX_TYPE_LIMITER above, for the identical reason (an
// independent per-channel envelope would skew the stereo image every time the compressor moved).
#define FX_TYPE_OPTOCOMP 18
// A split-band de-esser -- not a clone of any specific hardware/plugin (there isn't the same
// "coveted vintage character" angle here that justified porting a specific reference for the other
// compressors above), just this engine's own original implementation of the standard modern
// technique: split into a low band and a high (sibilance) band, compress only the high band, then
// recombine. The split reuses ONE highpass -- FX_TYPE_MULTIFILTER's own 2-pole TPT stage
// (fx_multifilter_2pole_stage(), fixed at Butterworth Q, no resonance) -- to get the high band, and
// derives the low band as `input - high` rather than running a second, separately-designed lowpass:
// since low+high==input is then true BY CONSTRUCTION regardless of the highpass's own shape, this
// guarantees exact, phase-coherent recombination with only one filter running, no matched-crossover
// design needed at all. The high band's own level (self-detecting, no external sidechain) drives a
// power-law gain computer identical in shape to FX_TYPE_OPTOCOMP's, using this file's OWN native
// exponential attack/release convention (matching FX_TYPE_GATE/FX_TYPE_LIMITER) rather than 4a2a's
// own specific calibration, since nothing here is emulating that plugin. param[0] Frequency
// (1-16kHz, log -- the crossover/detection point, typically 4-8kHz for sibilance), param[1]
// Threshold (-40..0dB, keyed off the high band's own level), param[2] Ratio (1..20), param[3] Attack
// (0.1..20ms -- fast, sibilance is a short transient), param[4] Release (5..300ms -- also fast, to
// avoid audibly pumping the top end), param[5] Listen (choice: Off/On -- solos the RAW (pre-gain-
// reduction) high band in place of the normal output, so Frequency can be dialed in by ear before
// trusting Threshold/Ratio to do anything useful).
#define FX_TYPE_DEESSER 19
// Real-time hard-tune pitch correction -- like FX_TYPE_MULTIFILTER/FX_TYPE_DEESSER, this is an
// original implementation of a well-established, published, non-proprietary technique (not a clone
// of any specific commercial product -- the exact algorithms a specific flagship pitch-correction
// product uses internally aren't public, and some of its retune-speed/formant-preserving behavior is
// patented; what's implemented here is the standard, decades-old public technique every open pitch
// corrector is built from): YIN (de Cheveigne & Kawahara, 2002) for pitch detection, scale/key (or
// live MIDI note) quantization, and a period-synchronous two-tap crossfaded variable-speed delay-line
// read for the actual pitch shifting -- PSOLA's own core mechanism, without needing explicit
// pitch-mark placement (the tap phase IS the pitch mark, by construction). See run_fx_hardtune()'s
// own (long) comment for the full algorithm. This is the first FX type whose ADDED LATENCY is
// structural and unconditional: the lookahead/tap-delay buffer runs continuously whenever this slot
// is active (not bypassed), whether or not a pitch is currently being corrected, exactly like
// FX_TYPE_PEAKMAX's own lookahead limiter -- there is no way to "engage the delay only when
// correcting" without the buffer already being primed to catch a pitch the instant one starts.
// param[0] Key (choice: 12 notes, C..B -- only used when Target Source is Scale/Key), param[1]
// Scale (choice: Chromatic/Major/Minor/Major Pentatonic/Minor Pentatonic -- ditto), param[2] Target
// Source (choice: Scale/Key vs. MIDI -- mutually exclusive with Key/Scale; MIDI snaps to whatever
// note is currently held on any connected MIDI input instead, monophonic last-note-priority, the
// same convention this engine's own Mono mode already uses via MonoNoteStack), param[3] Amount
// (0-100%, blends the correction ratio between none and fully-snapped, in log/semitone space),
// param[4] Retune Speed (glide time from the input's own pitch toward the corrected target -- 0 is
// the instant "hard-tune"/2006-era snap, higher values are a natural, musical portamento into the
// note), param[5] Tracking Speed (a SEPARATE smoothing stage on the RAW detected pitch itself,
// before quantization -- controls how quickly the detector "believes" a new note has started vs.
// treating a fast pitch wobble as noise; distinct from Retune Speed, which smooths the already-
// quantized correction), param[6] Voice Range (choice: Bass/Tenor/Alto/Soprano/Wide -- narrows the
// detector's own min/max Hz search window to the expected range for that voice type, which reduces
// octave-detection errors, same reason real pitch correctors offer this; not just a cosmetic preset).
// param[7] Vibrato Rate (0.1..10Hz), param[8] Vibrato Depth (0..1.5 semitones peak, default 0 = off),
// param[9] Vibrato Mod Source (choice: None/Mod Wheel (CC1)/Aftertouch -- gates Depth by a live
// controller, same idea as PackLfoParams.mod_source_cc, but for this effect's own dedicated
// per-slot sine LFO rather than the per-Program LFO matrix, which has no destination for a global
// FX effect with no single Program/voice of its own). Three inputs apply UNCONDITIONALLY (regardless
// of Target Source), as a final multiplicative adjustment layered on top of the already-corrected
// pitch, never fed back into detection or quantization above -- so none of them can ever fight the
// correction: live MIDI pitch bend (see g_hardtune_bend_semitones's own comment, reusing Program 0's
// own pb_range_up/down), the Vibrato LFO just described, and Target Source (MIDI mode) itself, which
// reads g_hardtune_held_note, an engine-wide monophonic last-note-priority tracker (see its own
// comment) updated by handle_note_on/handle_note_off regardless of which Program a note targets or
// whether that Program is in Poly or Mono mode -- deliberately separate from any Program's own
// mono_stack.
//
// KNOWN LIMIT, precisely measured (not a bug): this two-tap crossfade is genuinely accurate
// (<0.1% frequency error, verified against a real periodic test signal) for corrections up to
// about 3 semitones (a minor third) either direction, then degrades in a characteristic way, not a
// vague "gets worse": the wrap/crossfade rate is exactly `target_hz - input_hz` (the two windows'
// own modulation frequency), and once that's comparable to the target pitch itself, the perceived
// output locks onto a rational sub-multiple of the intended target instead of the target itself --
// measured exactly 2/3 of target for 4-9 semitones of correction, exactly 1/2 (an octave low) for
// 10+ semitones. This can NEVER happen in Scale/Key mode: snapping to the nearest note in any of
// the 5 scales offered is at most ~1.5-2 semitones away by construction (hardtune_snap_to_scale's
// own search never needs to look further for any of them), well inside the clean range -- only MIDI
// Target Source with a held note more than a minor third from the input (a deliberate wide-interval
// harmony, not "correction"), or a large pitch bend stacked on top, can reach this. A cleaner fix
// (tying grain_period to the TARGET period rather than the source's own detected period, the more
// involved PSOLA design real harmonizer/pitch-shifter plugins use for large intervals) is a real,
// scoped follow-up if wide-interval MIDI harmony ever becomes a use case here; not attempted yet
// since it's a materially different, larger change than what pitch CORRECTION itself needs.
#define FX_TYPE_HARDTUNE 20
// Algorithmic hall/plate/room reverb built on an 8x8 Feedback Delay Network (FDN) -- like
// MultiFilter/DeEsser/HardTune, an original implementation of a well-established, generic,
// published DSP architecture (Jot/Stautner-Puckette-style FDNs with an energy-preserving
// Hadamard mixing matrix -- textbook material, e.g. Valimaki et al.'s reverb literature), not a
// clone of any specific commercial unit's own internals. Ported from and informed by
// github.com/ninuxi/MAI_verb (MIT licensed) -- specifically its FDN8x8 and EarlyReflections
// engines -- restructured from JUCE's block-oriented API into this engine's own per-sample
// dispatch (see run_fx_fdnverb()'s own comment for the conversion, the same kind HardTune's own
// PSOLA needed).
//
// Two real, audible-nothing quirks found in the reference source while reading its own call
// graph (not guessed) and deliberately NOT carried over as knobs here: (1) its Decay Low/Decay
// High controls are stored but never read by anything that touches audio -- its own
// updateFeedbackGains() only ever reads rt60Mid, so only ONE decay time does anything acoustically;
// (2) its Damping Low control computes an hpFreq/hpCoeff that its own FeedbackFilter::process()
// never applies (only that one-pole filter's lowpass half is ever used) -- Damping Low is
// silently inert too. Porting either as a real param here would just be two dead pots; the
// param budget saved is spent on Early Reflections Decay instead, a control the reference's own
// panel already exposes and one that IS real.
//
// param[0] Decay (RT60 -- the actual -60dB decay time, 0.3-12s, log taper: feedback gain per
// delay line is 10^(-3*delayLenSamples/(RT60*sampleRate)), the standard exponential-decay-per-
// round-trip formula), param[1] Size (0-100%, scales all 8 delay lengths 0.3x-1.5x -- bigger room,
// longer pre-mix time before the tail proper), param[2] Pre-Delay (0-250ms, a plain delay ahead
// of both Early Reflections and the FDN -- the gap before any reflection arrives, reads as
// physical room distance), param[3] Damping (0-100%, a one-pole lowpass in the feedback path's
// own loop -- HIGHER damping = darker, faster-decaying highs, exactly like real room air
// absorption and the one damping control that's actually real), param[4] Diffusion (0-100%,
// blends each delay's raw tap against the full Hadamard-mixed sum, AND sets the input allpass
// diffusers' own coefficient -- low is glassy/metallic with audible early echoes, high is a dense,
// smooth wash), param[5] Mod Rate (0.1-10Hz) and param[6] Mod Depth (0-100%, together a per-delay-
// line sinusoidal read-position wobble, decorrelated per line via fixed rate/phase offsets -- this
// is what keeps a long, high-diffusion tail from ringing at a fixed metallic pitch, the same
// "chorused feedback" trick most algorithmic reverbs use), param[7] Early Reflections Level
// (0-100%) and param[8] Early Reflections Decay (0-100%, shapes how quickly the 16 discrete
// early-reflection taps -- alternating polarity, decorrelated L/R timing, simulating real wall
// bounces -- fall off; Level near 0 isolates the FDN's own smooth tail, useful for hearing it in
// isolation), param[9] Freeze (choice: Off/On -- forces every feedback gain to 1.0 and stops new
// input from entering the delay lines, the standard "infinite sustain" reverb-freeze trick:
// whatever's already circulating loops forever, unchanged, until Freeze is released).
//
// Deliberately preserved from the source rather than "cleaned up": Early Level's mix is NOT a
// clean crossfade of just the reflections -- the source's own ER working buffer starts as a copy
// of the (pre-delayed) dry signal, EarlyReflections adds its taps ONTO that copy in place, and the
// WHOLE thing (dry copy + taps together) is what gets scaled by Early Level before summing with
// the FDN tail. Raising Early Level therefore also proportionally reintroduces a scaled copy of
// the dry signal into the wet bus, on top of this slot's own generic Mix -- a real, audible
// characteristic of how the reference algorithm actually sounds, kept here on purpose.
// One genuine redundancy WAS quietly corrected, though: the reference bakes Early Level into each
// tap's own gain (EarlyReflections::updateTaps) AND THEN multiplies the already-scaled result by
// Early Level a second time (M7ReverbEngine::process's own `erOut * currentEarlyLevel`) -- so its
// knob actually shapes the taps by Level^2 while the dry leak above only sees Level^1, a
// mismatched, almost certainly unintentional double-application (nothing in the source explains
// or relies on it). Level is applied exactly ONCE here (as that single final multiply on dry+taps
// together), giving a knob that responds linearly, the way every other knob in this effect (and
// this whole engine) does.
#define FX_TYPE_FDNVERB 21
// An original design (not ported from any single codebase) modeling the general, publicly-
// documented behavior of a classic large-format console's E/G-series channel EQ: 4 bands + HPF,
// RBJ-cookbook shelf/peak biquads (compute_eq_band_coeffs/run_eq_biquad above -- the exact same
// math FX_TYPE_EQ3 already uses, just with per-band frequency ranges/Q behavior matched to this
// hardware style instead of a plain generic 3-band EQ), plus two specific console characteristics
// widely described as this design's signature: bandwidth that narrows as a bell band's own gain
// increases ("proportional Q" -- see run_fx_ssl4k()'s own comment for the exact formula, a
// deliberately chosen, non-reverse-engineered curve, not lifted from any specific implementation),
// and a subtle always-on saturation stage standing in for the real hardware's transformer/op-amp
// coloration, whose drive scales with how hard the bands are pushed. param[0] HPF Freq (20-500Hz,
// 2-pole/12dB-oct Butterworth), param[1] LF Freq (30-450Hz, shelf) param[2] LF Gain (+-16dB),
// param[3] LMF Freq (200Hz-2.5kHz on E / 200Hz-7kHz on G, bell), param[4] LMF Gain (+-16dB),
// param[5] LMF Q (bandwidth, 0.3-3.0 base value before the proportional-Q adjustment), param[6]
// HMF Freq (600Hz-7kHz on E / 600Hz-12kHz on G, bell, fixed base Q), param[7] HMF Gain (+-16dB),
// param[8] HF Freq (1.5kHz-16kHz, shelf), param[9] HF Gain (+-16dB), param[10] Console (choice:
// 0=E, 1=G -- E narrows bandwidth more aggressively under gain and drives the saturation stage
// harder, matching E's reputation as the punchier/more aggressive of the two; G's bell bands stay
// wider-bandwidth and its saturation is gentler, matching G's reputation as the smoother, more
// "hi-fi" revision). See run_fx_ssl4k().
#define FX_TYPE_SSL4K 22
// Raised from 8 (originally sized for every earlier port's needs) to fit FX_TYPE_POINTYGUITAR's
// 10 real params without dropping any of them -- unlike Density3's redundant internal Dry/Wet
// (already covered by this slot's own generic Mix below) or TapeDelay2's kept Dry/Wet, none of
// PointyGuitar's 10 controls is a crossfade-equivalent safe to fold away; 5 of them (Presence/
// High/Mid/Low/Sub) are the distortion's own core per-band tone shaping, not optional extras.
// A purely additive, backward-compatible wire change: every existing effect's own param[0..7]
// addresses are untouched, param[8]/param[9] are simply new addresses (defaulting to 0.0, like
// any other never-set knob) that no earlier-ported effect type ever reads.
//
// Raised again, 10 -> 11, for FX_TYPE_SSL4K's own E/G console switch (param[10]) -- same
// purely-additive reasoning as the raise above: every existing effect's param[0..9] addresses are
// untouched, param[10] is simply a new address no earlier-ported effect type ever reads.
#define FX_TYPE_DBX160 23
// An original design (not ported from any single codebase, and NOT reverse-engineered from any
// real dbx 160-family schematic) modeling general, widely-known characteristics of that hardware
// family's basic architecture: a minimal 3-control interface (Threshold/Ratio/Output Gain, no
// Attack or Release knob at all -- real 160-family units are famous for exactly this simplicity),
// an RMS-linked stereo detector (max(L,R) power, one-pole smoothed) whose own smoothing time IS
// the effective attack (no separate attack stage), an instantaneous jump to the needed gain
// reduction the moment the smoothed level crosses Threshold, and a fixed (non-adjustable, to match
// that same real hardware minimalism) linear-dB/second release ramp. Two extras go beyond any real
// 160-family unit: a Saturation knob (a VCA-style cubic waveshaper whose drive scales up with how
// hard the gain cell is currently pulling down -- the general "the harder a VCA compresses, the
// more it audibly grinds" behavior common to VCA designs, not a measured-from-hardware curve) run
// through this file's own 4x oversampling idiom (two cascaded 2x zero-stuff+biquad stages, see
// run_fx_dbx160()'s own comment -- genuine 4x, not the fake "4x" from this effect's own first draft,
// see below) to keep its harmonics from aliasing, and a sidechain highpass (a plain one-pole, tuned
// low, for keeping a low end-heavy source's kick/bass energy from needlessly ducking the detector).
// param[0] Threshold (-60..0dB), param[1] Ratio (1:1..20:1), param[2] Makeup Gain (+-12dB),
// param[3] Saturation (0..1 drive), param[4] SC-HPF (20-300Hz -- 20Hz is this knob's own honest
// "off" position, same convention as e.g. FX_TYPE_GATE's Range: far enough below any bass content
// that matters that it's a no-op by design, not a special-cased bypass). See run_fx_dbx160().
//
// This effect's first draft came from an outside AI assistant (Gemini), not from any specific
// existing DSP codebase -- porting it here meant fixing two real problems rather than a style
// pass: (1) its sidechain highpass computed `state = alpha*(state+x)` where the standard one-pole
// lowpass-for-a-DC-blocker needs `state = alpha*x + (1-alpha)*state` -- the missing retention term
// makes it an unbounded integrator, not a filter (feed it a steady input and the state diverges
// within a handful of samples); replaced with a plain, correctly-normalized one-pole. (2) its
// claimed "4x oversampling" never actually upsampled anything: every one of its 4 "subsample"
// iterations recomputed the exact same unchanged value (same input sample, same gain, and its
// waveshaper is memoryless) and pushed it through one fixed filter's persistent state 4 times in a
// row, averaging the result -- four times the CPU for a number a single filtered call already
// approximates, with no new inter-sample harmonic content ever generated. Real 4x oversampling (see
// run_fx_dbx160()) is built the same way this file already does 2x for FX_TYPE_FETCOMP -- zero-stuff
// + RBJ-cookbook Butterworth lowpass biquad up, process, lowpass + decimate down -- just cascaded
// twice (1x->2x->4x on the way up, 4x->2x->1x on the way down) instead of run once, which is also
// why it's a cleaner way to reach 4x than one filter stage recomputed at a 4x rate directly: each
// stage only has to reject the ONE nearest set of spectral images, so the two 2nd-order sections
// together give a steeper effective stopband than a single 2nd-order section would front-load all
// of that rejection onto by itself.
// An on-air "profanity delay"/broadcast-delay dump buffer: audio continuously runs through a
// circular delay line (up to 60s), with three fixed "dump" buttons that instantly skip the
// delay's own read pointer forward (discarding that much buffered audio) the moment something
// needs to be cut before it reaches air -- the classic radio/TV delay-box workflow. Not ported
// from any specific hardware/plugin -- an original implementation of that well-known, generic
// broadcast-engineering technique.
//
// Auto-restocking (mandatory -- see run_fx_broadcastdelay()'s own comment for the derivation):
// after a dump (or after the Delay knob itself is simply turned UP), the buffered delay sits
// below the nominal Delay setting. Rather than leaving it there permanently (which would mean
// every dump ratchets the safety margin down for good) or requiring a manual "recover" step, the
// read pointer is made to advance very slightly SLOWER than the write pointer -- real time keeps
// flowing in at a normal 1x rate, but played-back output falls behind it by a hair, so the GAP
// between them (which is exactly what "the delay" means) grows back on its own, with no user
// action needed. This is the same restocking trick real on-air delay units use, and it's the
// opposite of "playing faster": speeding up the read side would only drain the buffer further,
// since it would pull the read pointer even closer to the write pointer. A separate, much faster
// glide handles ordinary Delay-knob changes (both up and down) so turning the knob feels
// responsive; only a post-dump shortfall uses the slow, deliberately-inaudible restock rate.
//
// param[0] Delay (0-60s, nominal/target delay -- how far behind live the output normally sits),
// param[1] Dump 7s, param[2] Dump 15s, param[3] Dump 30s (each an edge-triggered one-shot
// counter, same idiom as SeqLaneState.audio_preview_trigger -- the GUI increments it by 1 on
// every click; run_fx_broadcastdelay() detects the change and instantly drops that much delay,
// clamped so a dump can never take the buffer below 0), param[4] Restock Speed (0..1 -- how
// aggressively, and therefore how audible, the post-dump catch-up slowdown is; higher restocks
// faster at the cost of a more noticeable brief pitch/time wobble on whatever's playing while it
// catches up), param[5] On-Air Override (plain 0/1 gate, held for as long as the GUI button is
// down -- not an edge-triggered one-shot like the Dump buttons): while the EFFECTIVE gate --
// param[5] OR (param[8] AND the live MIDI-key state), unless param[7] forces it off -- is true,
// run_fx_broadcastdelay() replaces this slot's OUTPUT with a continuous 1kHz tone at param[6]'s own
// amplitude, live, deliberately NOT delayed like everything else this effect does -- see that
// function's own comment on why the override has to hit the output directly rather than the
// buffer's input to be useful for its actual purpose (an instant, on-the-spot self-censor).
// param[6] On-Air Override Volume (0..1, default 0.5 -- seeded on type switch same as param[0]/
// param[4] below) -- the tone's own amplitude, replacing what used to be a flat hardcoded 0.5. Only
// ever scales how loud an already-active tone is; 0% is a legitimate "silent but still engaged"
// setting (e.g. testing a trigger source without being heard), never a way to disable the override
// itself -- that's param[7]'s own job, kept deliberately independent. param[7] On-Air Override
// Disabled (plain 0/1): when true, the effective gate above is forced false no matter what param[5]/
// param[8]/MIDI say, AND the GUI button grays out and stops responding to mouse clicks entirely,
// same visual convention as the Dump buttons at Delay=0 -- the only thing in this whole param map
// that can take the button's own clickability away. param[8] On-Air Override MIDI Trigger Enabled
// (plain 0/1): when true, ANY MIDI note currently held anywhere in this engine, OR the sustain
// pedal (CC64) currently held, ALSO engages the gate -- via the engine-wide g_bcastdly_notes_held/
// g_bcastdly_sustain_held state fx_chain.c's own dispatch_midi_event() call site maintains (see
// each one's own comment) -- purely additive, never a replacement for the mouse: a MIDI foot
// switch, pad, or a sustain pedal can trigger the same override the GUI button's own mouse-hold
// does, at the same time, with none of them blocking each other. Both param[7] and
// param[8] are set via a right-click menu on the button itself (two independent checkable toggles,
// not a mutually-exclusive mode picker -- there's no separate "Normal" state to pick, since the
// mouse simply always works unless param[7] says otherwise), not a knob -- see
// HDS_FX_DROPDOWN_ONAIR_MODE's own comment in hds_fx_gui_draw.c. Exists specifically so two
// FX_TYPE_BROADCAST_DELAY slots can run at once with each one's On-Air Override independently
// wired to whichever trigger source(s) that particular instance actually needs.
#define FX_TYPE_BROADCAST_DELAY 24
// An original design (not ported from any single codebase, and NOT reverse-engineered from any
// real dbx 120/120X-family schematic -- same posture as FX_TYPE_DBX160's own comment) modeling
// the general, widely-known architectural idea behind that hardware family's subharmonic
// synthesizer: rather than trying to run a general-purpose monophonic pitch tracker (YIN,
// autocorrelation, etc.) across whatever program material arrives -- which is exactly where every
// "unmusical on polyphonic material" digital subharmonic generator falls apart, since those
// algorithms have to resolve ONE precise pitch out of a signal that may contain several, and they
// fail by picking the wrong one, or the wrong octave, or chattering between candidates -- this
// effect never attempts that at all. It heavily LOW-PASSES the signal first (param[0] Track Freq,
// deliberately narrow, 30-300Hz) so that only the dominant sub-bass fundamental survives before
// detection even starts: above that band, a polyphonic mix's own complexity (chords, vocals,
// cymbals, whatever else is playing) is gone, and in practice there's rarely more than one real
// fundamental with meaningful ENERGY left in a 30-300Hz window at any instant, even in dense
// program material -- a kick drum and a bass guitar playing different notes still leaves one of
// them clearly dominant moment to moment. Within that narrowed band, pitch is measured the same
// simple, robust way analog octave dividers and the real hardware this models are built around:
// zero-crossing period measurement (time between consecutive positive-going crossings, with a
// refractory window tied to Track Freq itself to reject double-triggers/ripple), not spectral
// analysis -- a technique that is structurally IMMUNE to the octave-detection ambiguity
// autocorrelation/YIN-style trackers suffer from, because it never searches a lag range or scores
// candidate periods at all; it just reads the waveform's own crossing rate directly. See
// run_fx_dbx120x()'s own comment for the full per-sample mechanics, including how the detected
// period is smoothed in TWO separate stages (an event-driven, user-controlled glide, then a short
// fixed per-sample anti-click chase -- the same two-stage lesson HardTune's own grain_period/
// smoothed_grain_period fix needed, applied here from the start) so the generated tone's pitch
// never steps or clicks, only glides.
//
// The generated subharmonic itself is a clean, phase-continuous SINE tone (not a raw divided
// square wave, which is what most dumb analog-style octave dividers actually output and which
// reads as buzzy rather than a clean sub-bass layer) at exactly Half (param[4]=0, one octave down)
// or Quarter (param[4]=1, two octaves down) of the tracked fundamental frequency, gated in/out by
// param[1] Threshold/param[2] Release (a Schmitt-trigger hysteresis engagement gain, the same
// open/close idiom FX_TYPE_GATE already uses, just gating how much synthesized tone is ADDED
// rather than gating the dry signal itself) so it only appears when there's real low-frequency
// energy present, never as a constant hum/rumble under silence or non-bass passages. param[3]
// Synthesis Level (0-100%) is this effect's own internal gain on the generated tone before it's
// summed with the untouched dry signal -- same "own internal level, separate from and applied in
// addition to the slot's generic mix" convention as FX_TYPE_ENSEMBLE's own FX Level. param[5]
// Tracking Glide (10-500ms) is the user-facing control for the actual polyphony/stability tradeoff
// this whole design is built around: slower glide times make the tracked pitch far more resistant
// to being knocked around by a few stray hits of other low-frequency content leaking through the
// pre-filter, at the cost of responding more slowly to a genuinely new bass note; faster glide
// times track a fast bass line more snappily but are more exposed to whatever else survives Track
// Freq's own narrow window. See run_fx_dbx120x().
#define FX_TYPE_DBX120X 25
#define FX_SLOT_PARAM_COUNT 11
typedef struct {
    float fx_type;
    float param[FX_SLOT_PARAM_COUNT];
    float mix;     // 0-100, dry/wet blend applied uniformly around whatever the slot does
    float bypass;  // 0/1, hard bypass (distinct from Off -- keeps the slot's state alive so
                    // flipping it back on mid-note doesn't lose e.g. a delay's buffered tail)
    // Multi-source routing (appended): which source (0 = Program, 1..EXT_IN_PAIR_COUNT = ext-in
    // pair) feeds this slot -- see process_fx_chain()'s source_id parameter. Default 0 (Program)
    // reproduces today's exact behavior (every slot processing the one shared Program mix) when
    // no ext-in sources are in use.
    float fx_source;
    // Chain To Next (0/1, default 0) -- every caller always runs process_fx_chain() in Parallel
    // mode (no separate Series/Parallel toggle exists anymore, see that function's own comment):
    // when set, the NEXT owned slot (this source's own next slot in array order, not necessarily
    // the physically-adjacent one) is fed THIS slot's own output instead of starting a fresh
    // parallel branch from the shared dry input, continuing the same branch in series. Lets
    // several slots be chained in series while that whole chain still runs as ONE branch alongside
    // a source's other parallel slots/chains -- e.g. two independent 3-slot serial chains summed
    // together, or every slot on a source chained to reproduce a single, plain serial chain.
    // Meaningless on the last slot in the array (there is no "next" to chain into). See
    // process_fx_chain()'s own comment.
    float chain_to_next;
    // Sidechain Source (default -1 = "Same as Source" -- keys off this slot's OWN dry input,
    // reproducing ordinary non-external-sidechain compression, e.g. FX_TYPE_FAIRKID67's real
    // hardware ancestor, which has no external key input at all). Any other value is a source id
    // (same numbering as fx_source/SOURCE_NAMES -- 0..COMBI_PROGRAM_COUNT-1 = a Combi Program,
    // COMBI_PROGRAM_COUNT = Combi Mix, the rest = ext-in pairs) naming a DIFFERENT source's own
    // pre-FX dry signal to drive the detector with instead -- e.g. compressing a synth pad while
    // keying off a kick drum routed in on an ext-in pair. Harmless dead bytes on any effect type
    // that doesn't read a sidechain key at all, same posture as param[] entries an effect ignores.
    // See process_fx_chain()'s own sc_snapshot_l/r comment for why this reads a FROZEN snapshot of
    // every source's dry signal rather than the live, in-place-mutated bus_l/bus_r array.
    float sidechain_source;
    // Explicit cross-slot Input override (default -1 = "not used, resolve Input from fx_source/
    // chain_to_next as normal"). Any other value is a POOL SLOT INDEX (0..FX_SLOT_COUNT-1, never
    // this slot's own index -- the GUI disallows it, and process_fx_chain() also refuses it
    // defensively) whose own last-computed POST-MIX output feeds this slot's dry input instead,
    // completely independent of fx_source/chain_to_next -- lets one slot read directly from
    // another slot anywhere in the shared pool (even one owned by a different source/port), not
    // just the next array-position slot sharing its own fx_source the way chain_to_next requires.
    // Always wins over chain_to_next/continue_branch when set -- see process_fx_chain()'s own
    // comment on fx_slot_out_l/r for how this is actually resolved.
    float input_from_slot;
    // FX_TYPE_NAM's own loaded-file paths (unused by every other effect type -- harmless dead
    // bytes on their slots, same posture as param[] entries an effect doesn't read). Plain string
    // fields, not a dedicated load message: see poll_fx_nam_requests()'s own comment for why this
    // exact mechanism (already used by Sequencer audio lanes' own audio_path) is the right one
    // here, including how it makes a CLAP state restore reload correctly for free.
    char nam_model_path[SAMPLE_PATH_LEN];
    char nam_ir_path[SAMPLE_PATH_LEN];
} FXSlotParams;

// Persistent per-slot engine state for the FX bus -- deliberately kept OUT of the wire struct
// (SynthSharedControls only carries each slot's user-facing params). `state` is an opaque
// pointer each effect owns entirely: allocated on first use and freed whenever the slot's type
// changes, so a ported effect can size/shape its own private buffers (e.g. a delay line) freely
// without anything here needing to know what they look like.
typedef struct {
    int active_type;
    void *state;
} FXSlotEngine;


#endif // HDS_FX_TYPES_H
