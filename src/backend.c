#include <alsa/asoundlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 48000
#define BUFFER_SIZE 128
#define MAX_VOICES  64
#define LFO_COUNT   4
#define ENV_COUNT   7  // Osc 1 Amp, Osc 2 Amp, Noise Osc Amp, + EG 1-4
#define UNISON_SAW_VOICES 7
#define FX_SLOT_COUNT 12
#define AUDIO_DEVICE_NAME_LEN 64

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

volatile sig_atomic_t g_synth_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    g_synth_running = 0;
}

typedef enum {
    STATE_IDLE,
    STATE_DELAY,
    STATE_ATTACK,
    STATE_HOLD,
    STATE_DECAY,
    STATE_SUSTAIN,
    STATE_RELEASE
} EnvelopeState;

typedef enum {
    WAVE_SAW,
    WAVE_SQUARE,
    WAVE_TRIANGLE,
    WAVE_SINE,
    WAVE_UNISON_SAW,
    WAVE_NOISE,
    WAVE_PULSE  // appended (not inserted) so existing saved patches' numeric shape indices don't shift
} WaveType;

// Sub Oscillator waveform choices. Deliberately a separate, smaller enum from WaveType -- the
// sub osc has no Unison Saw/Noise/Pulse option, so it can't reuse WaveType's numbering.
typedef enum {
    SUB_SAW,
    SUB_SQUARE,
    SUB_TRIANGLE,
    SUB_SINE
} SubWaveType;

typedef struct {
    float d; float h; float a; float dec; float s; float r; float al; float pk;
} PackEGParams;

typedef struct {
    float wave_type; float rate_hz; float mod_depth; float key_sync; float dest_target; float phase_offset;
} PackLfoParams;

// A single filter's parameters: State-Variable Filter (SVF) core, cutoff/resonance/type, plus
// a slope switch. slope_poles cascades the same 2-pole (12dB/oct) SVF core twice back-to-back
// to get a 4-pole (24dB/oct) slope -- see run_svf_filter().
typedef struct {
    float cutoff;
    float res_q;
    float slope_poles; // 0 = 2-pole (12dB/oct), 1 = 4-pole (24dB/oct)
    float filter_type;  // 0 = Off (bypass -- passes the signal through unfiltered), 1 = Low Pass, 2 = High Pass, 3 = Band Pass
} FilterParams;

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
#define FX_SLOT_PARAM_COUNT 8
typedef struct {
    float fx_type;
    float param[FX_SLOT_PARAM_COUNT];
    float mix;     // 0-100, dry/wet blend applied uniformly around whatever the slot does
    float bypass;  // 0/1, hard bypass (distinct from Off -- keeps the slot's state alive so
                    // flipping it back on mid-note doesn't lose e.g. a delay's buffered tail)
} FXSlotParams;

// PACKET STRUCT: This is the exact byte layout the Python GUI packs.
// BREAKING CHANGE (4-oscillator architecture): the envelope count grew from 6 to 7 (a new
// dedicated Noise Osc Amp envelope was inserted as eg[2]/eg_mode[2], pushing the old EG 1-4
// slots from eg[2..5] to eg[3..6]), and 10 new trailing float fields were added. The Python
// side's struct.pack/unpack format MUST be regenerated to match -- this is not a compatible
// wire change.
// BREAKING CHANGE (filter overhaul): the old flat "3 stacked filters + 1 routing mode" section
// (13 floats) was replaced by a 2-filter (A/B) architecture (23 floats) -- see the comment right
// above the filter fields in this struct for the full layout. Running total after that change
// was 151 float items:
// 13 (baseline) + 56 (7 EGs * 8 params incl. attack start/peak level) + 23 (Filter A/B section, see below)
// + 24 (4 LFOs * 6, incl. phase offset) + 4 (EG intensity) + 4 (EG dest target) + 7 (EG mode)
// + 4 (unison saw width/detune) + 1 (mono glide randomization) + 2 (vco1/vco2 mute, decoupled from FM)
// + 1 (hard sync) + 2 (vco1/vco2 phase offset)
// + 2 (vco1/vco2 pulse width) + 4 (noise osc: vol/pan/mute/tilt) + 4 (sub osc: shape/vol/pan/mute) = 151
// BREAKING CHANGE (FX bus + external input + device selection): added 2 floats (ext_in_enable,
// ext_in_gain) + 132 floats (FX_SLOT_COUNT=12 slots * 11 floats each: fx_type, 8 generic params,
// mix, bypass) + 2 fixed 64-byte device name strings (output_device, input_device -- not
// floats; see AUDIO_DEVICE_NAME_LEN). New total: 285 float items + 128 string bytes. The
// Python side's struct.pack format changes from "151f" to "285f64s64s".
typedef struct {
    // 13 Baseline Control Items
    float is_polyphonic;
    float vco1_shape;
    float vco2_shape;
    float fm_intensity;
    float vco1_vol;
    float vco2_vol;
    float portamento_time_ms;
    float master_volume;
    float vco1_pan;
    float vco2_pan;
    float vco1_fine_tune;
    float vco2_pitch_offset;
    float vco2_fine_tune;

    // 48 Envelope Parameter Items (6 Envelopes * 8 Parameters, incl. attack start/peak level)
    PackEGParams eg[ENV_COUNT];

    // --- FILTER SECTION (overhauled): 4 oscillators -> mixer -> Filter A -> Filter B, with
    // A and B routable in Series or Parallel with each other (23 floats total).
    //
    // Filter A / Filter B outer routing: 0 = Series (mixer -> A -> B -> output),
    // 1 = Parallel (mixer -> A, mixer -> B independently, summed 50/50 -> output). (+1 float)
    float filter_ab_routing_mode;

    // Filter A: a single Normal filter by default. Switching filter_a_mode to 1 ("Multi")
    // replaces it with the legacy 3-filter engine below instead of filter_a -- i.e. this is
    // the old 3-filter-stack behavior, now scoped down to being one of Filter A's two modes.
    // (+1 float for the mode switch, +4 for filter_a's own params)
    float filter_a_mode; // 0 = Normal (uses filter_a below), 1 = Multi (uses filter_a_multi* below)
    FilterParams filter_a;

    // Filter A's internal 3-filter "Multi" engine (used only when filter_a_mode == 1). Because
    // the 4 oscillators are already summed into one signal before reaching Filter A, all 3 of
    // these take the SAME post-mixer input -- unlike the old engine, they can no longer see
    // individual oscillators separately, so there's no more "independent per-oscillator" option.
    // (+1 float for its own internal routing, +12 for its 3 filters' params)
    float filter_a_multi_routing; // 0 = Series (in0->in1->in2), 1 = Parallel (all 3, summed),
                                   // 2 = Split (in0 // in1 summed, -> in2)
    FilterParams filter_a_multi[3];

    // Filter B: always a single, normal filter -- no Multi mode of its own. (+4 floats)
    FilterParams filter_b;

    // 24 Active LFO Mapping Items (4 LFOs * 6 Properties, incl. phase offset)
    PackLfoParams lfo[LFO_COUNT];

    // 4 Custom EG Intensity Modulators
    float eg_intensity[4];

    // 4 Custom EG Destination Targets (each assignable EG routes independently of the LFOs)
    float eg_dest_target[4];

    // 6 Active Envelope Topology Selection Mode Indices
    float eg_mode[ENV_COUNT];

    // Unison Saw Shape Controls
    float vco1_unison_saw_detune;
    float vco1_unison_saw_width;
    float vco2_unison_saw_detune;
    float vco2_unison_saw_width;

    // Mono Legato glide time randomization amount (0-100%): how much each glide's actual time
    // varies from the plain Glide Time knob, rerolled per glide event.
    float mono_glide_random;

    // Mute flags, deliberately separate from vco1_vol/vco2_vol: muting Osc 2 silences its
    // audible output but must NOT silence it as an FM modulator (Osc 2 -> Osc 1 FM uses vco2_vol
    // directly and ignores these flags entirely, by design).
    float vco1_mute;
    float vco2_mute;

    // Hard Sync: when enabled, Osc 2's phase is force-reset every time Osc 1 completes a cycle.
    float hard_sync;

    // Phase pot per oscillator (degrees, 0-360). Modulatable by LFO/EG via the mod matrix
    // (targets 24/25); the modulated result shifts only the oscillator's read-phase, never its
    // actual running phase accumulator, so pitch/hard-sync/portamento are unaffected.
    float vco1_phase_offset;
    float vco2_phase_offset;

    // --- 4-Oscillator Architecture Additions ---

    // Pulse width (0-100%, 50=symmetric square) for Osc 1/2, used only when the shape knob is
    // set to WAVE_PULSE. Unrelated to WAVE_SQUARE, which stays a fixed 50% duty cycle for
    // patch compatibility.
    float vco1_pulse_width;
    float vco2_pulse_width;

    // Dedicated Noise Oscillator: a third sound source, independent of leaving WAVE_NOISE
    // available as a shape choice on Osc 1/2. Has its own amp envelope (eg[2]/eg_mode[2]).
    float noise_vol;
    float noise_pan;
    float noise_mute;
    float noise_tilt; // -100 (bass-heavy/dark) .. 0 (flat white) .. +100 (treble-heavy/bright)

    // Sub Oscillator: fixed exactly one octave below Osc 1. It has no pitch controls of its
    // own -- it implicitly tracks Osc 1's fully-modulated frequency (fine tune, portamento,
    // every LFO/EG pitch mod) by design, and shares Osc 1's amplitude envelope (vco1_eg)
    // rather than getting its own, so it behaves as one fattened voice with Osc 1 rather than
    // an independently-triggerable layer.
    float sub_shape; // 0:Saw 1:Square 2:Triangle 3:Sine (SubWaveType)
    float sub_vol;
    float sub_pan;
    float sub_mute;

    // --- FX Bus + External Input Architecture ---

    // External Audio Input: a live-captured signal (see setup/read in main()) summed directly
    // into the same post-voice stereo bus the FX chain runs on -- so external audio passes
    // through all FX_SLOT_COUNT slots exactly like the synth's own voices do, by design. No
    // pan control here: capture is taken as an already-stereo L/R pair and passed through as-is
    // rather than re-panned from mono.
    float ext_in_enable;
    float ext_in_gain;

    // FX Bus: FX_SLOT_COUNT slots run in series on the summed stereo mix (voices + external
    // input), after Filter A/B (which stay per-voice) and before master_volume -- see
    // process_fx_chain(). Each ported effect fills in its own case in that function's dispatch
    // switch; until then every slot is a pass-through (see FX_TYPE_OFF).
    FXSlotParams fx[FX_SLOT_COUNT];

    // Audio device selection: fixed-size ALSA PCM device name strings (e.g. "default",
    // "hw:1,0"), always null-terminated. main() compares these against whichever device name
    // it last opened each stream with, and closes/reopens that stream on a live change via
    // open_pcm_stream(). Deliberately plain fixed-size char buffers rather than more float
    // items: a device name is text, not a knob value.
    char output_device[AUDIO_DEVICE_NAME_LEN];
    char input_device[AUDIO_DEVICE_NAME_LEN];
} SynthSharedControls;
typedef struct {
    EnvelopeState state;
    int env_type; // 0:ADSR, 1:AR, 2:AD, 3:ASR, 4:AHD, 5:DADSR, 6:AHDSR
    float current_level;
    float delay_timer;
    float hold_timer;
    float attack_rate;
    float decay_rate;
    float sustain_level;
    float release_rate;
    float release_level; // level captured at the instant note-off triggers release; release ramps from here to 0
} ADSR;

typedef struct {
    int midi_note;

    float vco1_phase;
    float vco1_target_freq;
    float vco1_current_freq;
    float vco1_tri_state;
    float vco1_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco1_eg;

    float vco2_phase;
    float vco2_target_freq;
    float vco2_current_freq;
    float vco2_tri_state;
    float vco2_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco2_eg;

    // Filter A's SVF register banks: [channel: 0=L,1=R][unit: 3 "units", unit 0 used alone in
    // Normal mode, all 3 used as the legacy engine in Multi mode][cascade stage: 2, 2nd stage
    // only used when a unit's slope is set to 4-pole]. Independent per-voice AND per-channel --
    // Filter A/B now run as two fully independent mono filters (one per stereo channel) so that
    // each oscillator's Pan placement survives the trip through the filters instead of being
    // collapsed to mono first.
    float fa_low[2][3][2];
    float fa_band[2][3][2];

    // Filter B's SVF register bank: [channel][cascade stage].
    float fb_low[2][2];
    float fb_band[2][2];

    // Assignable envelope instances 1-4
    ADSR assign_eg[4];

    // Dedicated Noise Oscillator: its own amp envelope, plus one-pole low-passed state used to
    // derive the "low" half of the tilt-EQ color control (see render_audio_block).
    ADSR noise_eg;
    float noise_tilt_lp;

    // Sub Oscillator: its own running phase (base_dt is derived each sample from Osc 1's fully
    // modulated frequency, halved) and its own triangle integrator state, kept separate from
    // vco1_tri_state so the two don't corrupt each other's triangle-wave integration.
    float sub_phase;
    float sub_tri_state;

    // Mono Legato glide time randomization: rerolled each time this voice starts an actual
    // pitch glide (an overlapping legato note, or falling back to a still-held note on release).
    // 1.0 = no variation (uses the plain Glide Time knob exactly).
    float glide_time_multiplier;
} SynthVoice;
// FIXED: Restored 'p->s' to reference sustain data properties correctly
void tick_adsr(ADSR *env, const PackEGParams *p, int mode) {
    env->env_type = mode;
    env->sustain_level = (mode == 1 || mode == 2 || mode == 4) ? 0.0f : (p->s / 100.0f);
    // Attack Start Level: the level the envelope *starts at* the instant a new note triggers it
    // (set in handle_note_on), before Attack ramps up toward Peak Level. At 0 (the default) this
    // is a plain envelope; raising it adds an instant "click"/punch. Controlled only via the
    // draggable (0,0) node in the GUI, not a dedicated knob.
    float atk_level = fmaxf(0.0f, fminf(1.0f, p->al / 100.0f));
    // Attack Peak Level: the level Attack ramps *up to* (100% is the classic full-swing default).
    // Controlled via the same peak node's vertical drag that already sets Attack time (x).
    float peak_level = fmaxf(0.0f, fminf(1.0f, p->pk / 100.0f));

    float t_dly = (mode == 5) ? fmaxf(0.001f, p->d / 1000.0f) : 0.0f;
    float t_hld = (mode == 4 || mode == 6) ? fmaxf(0.001f, p->h / 1000.0f) : 0.0f;
    float t_att = fmaxf(0.001f, p->a / 1000.0f);
    float t_dec = fmaxf(0.001f, p->dec / 1000.0f);
    float t_rel = fmaxf(0.001f, p->r / 1000.0f);

    // Attack climbs from its (possibly nonzero) start level up to Peak Level.
    env->attack_rate = fmaxf(0.0f, peak_level - atk_level) / (t_att * SAMPLE_RATE);
    env->decay_rate = fmaxf(0.0f, peak_level - env->sustain_level) / (t_dec * SAMPLE_RATE);
    // FIXED: release now ramps from the level captured at the moment note-off fired
    // (release_level), not from sustain_level. Previously this was always 0 for any mode
    // whose sustain_level is 0 (AR, AD, AHD), so those envelopes never released.
    env->release_rate = env->release_level / (t_rel * SAMPLE_RATE);

    switch (env->state) {
        case STATE_IDLE: break;
        case STATE_DELAY:
            env->delay_timer += 1.0f / SAMPLE_RATE;
            if (env->delay_timer >= t_dly) { env->state = STATE_ATTACK; env->delay_timer = 0.0f; }
            break;
        case STATE_ATTACK:
            env->current_level += env->attack_rate;
            if (env->current_level >= peak_level) {
                env->current_level = peak_level;
                if (t_hld > 0.0f) { env->state = STATE_HOLD; env->hold_timer = 0.0f; }
                else { env->state = (mode == 1 || mode == 3) ? STATE_SUSTAIN : STATE_DECAY; }
            }
            break;
        case STATE_HOLD:
            env->current_level = peak_level;
            env->hold_timer += 1.0f / SAMPLE_RATE;
            if (env->hold_timer >= t_hld) { env->state = STATE_DECAY; env->hold_timer = 0.0f; }
            break;
        case STATE_DECAY:
            env->current_level -= env->decay_rate;
            if (env->current_level <= env->sustain_level) {
                env->current_level = env->sustain_level;
                env->state = (mode == 2 || mode == 4) ? STATE_IDLE : STATE_SUSTAIN;
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
            if (mode != 1) {
                float sustain_chase_rate = 1.0f - expf(-1.0f / (0.015f * SAMPLE_RATE));
                env->current_level += (env->sustain_level - env->current_level) * sustain_chase_rate;
            }
            break;
        case STATE_RELEASE:
            env->current_level -= env->release_rate;
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

// Sub Osc reuses the same core wave rendering as Osc 1/2 -- this just maps its smaller
// SubWaveType enum onto the matching WaveType cases (Saw/Square/Triangle/Sine only).
static WaveType sub_wave_to_osc_wave(int sub_shape) {
    switch ((SubWaveType)sub_shape) {
        case SUB_SAW: return WAVE_SAW;
        case SUB_SQUARE: return WAVE_SQUARE;
        case SUB_TRIANGLE: return WAVE_TRIANGLE;
        case SUB_SINE: default: return WAVE_SINE;
    }
}

// Runs one logical filter for a single sample: a State-Variable Filter (SVF) core (Low/High/
// Band pass), with slope_poles selecting between a single 2-pole (12dB/oct) pass or the same
// core cascaded twice back-to-back for a 4-pole (24dB/oct) slope. low_state/band_state must be
// 2-element arrays (per-voice, persistent between calls); only element 0 is touched at 2-pole,
// both at 4-pole -- so switching slopes live doesn't require resetting anything.
static float run_svf_filter(float input, float cutoff_hz, float res_q, int filter_type, int slope_poles, float low_state[2], float band_state[2]) {
    // FIXED (catastrophic bug): "Off" used to return 0.0f -- silencing the signal entirely --
    // carried over unexamined from the old 3-filter engine's convention. In the new 2-filter
    // (A/B) architecture, Filter B defaults to Off, and EVERY voice's signal passes through
    // both A and B (in series by default) -- so any filter left in its default Off state
    // multiplied the whole synth's output by zero, and there was no way to hear anything with
    // both filters off. "Off" now means bypass: the signal passes through completely
    // unmodified, matching what "Off" obviously should mean.
    if (filter_type == 0) return input;
    cutoff_hz = fmaxf(40.0f, fminf(18000.0f, cutoff_hz));
    float damp = 2.0f * (1.0f - (fmaxf(0.0f, fminf(100.0f, res_q)) / 100.0f));
    float coeff = 2.0f * sinf((float)M_PI * cutoff_hz / (float)SAMPLE_RATE);
    int stages = (slope_poles != 0) ? 2 : 1;
    float sig = input;
    for (int s = 0; s < stages; s++) {
        float high = sig - low_state[s] - (damp * band_state[s]);
        band_state[s] += coeff * high;
        low_state[s] += coeff * band_state[s];
        sig = (filter_type == 1) ? low_state[s] : ((filter_type == 2) ? high : band_state[s]);
    }
    return sig;
}

// Filter A's "Multi" mode: the legacy 3-filter engine, now scoped down to being one of Filter
// A's two modes (see the FilterParams/filter_a_multi comments in the struct). Processes a
// single channel (called once for L, once for R -- see render_audio_block); each channel keeps
// its own independent state so panning survives filtering.
static float run_filter_a_multi(float in, int routing, const FilterParams multi[3], const float cutoff_mods[3], const float res_mods[3], float low[3][2], float band[3][2]) {
    float u0 = run_svf_filter(in, multi[0].cutoff + cutoff_mods[0], multi[0].res_q + res_mods[0], (int)multi[0].filter_type, (int)multi[0].slope_poles, low[0], band[0]);

    if (routing == 0) {
        // Series: unit 0 -> unit 1 -> unit 2
        float u1 = run_svf_filter(u0, multi[1].cutoff + cutoff_mods[1], multi[1].res_q + res_mods[1], (int)multi[1].filter_type, (int)multi[1].slope_poles, low[1], band[1]);
        return run_svf_filter(u1, multi[2].cutoff + cutoff_mods[2], multi[2].res_q + res_mods[2], (int)multi[2].filter_type, (int)multi[2].slope_poles, low[2], band[2]);
    } else if (routing == 1) {
        // Parallel: all 3 units take the same input directly, summed evenly
        float u1 = run_svf_filter(in, multi[1].cutoff + cutoff_mods[1], multi[1].res_q + res_mods[1], (int)multi[1].filter_type, (int)multi[1].slope_poles, low[1], band[1]);
        float u2 = run_svf_filter(in, multi[2].cutoff + cutoff_mods[2], multi[2].res_q + res_mods[2], (int)multi[2].filter_type, (int)multi[2].slope_poles, low[2], band[2]);
        return (u0 + u1 + u2) / 3.0f;
    } else {
        // Split: unit 0 // unit 1 (both take the same input directly), summed 50/50, -> unit 2
        float u1 = run_svf_filter(in, multi[1].cutoff + cutoff_mods[1], multi[1].res_q + res_mods[1], (int)multi[1].filter_type, (int)multi[1].slope_poles, low[1], band[1]);
        float blended = (u0 + u1) * 0.5f;
        return run_svf_filter(blended, multi[2].cutoff + cutoff_mods[2], multi[2].res_q + res_mods[2], (int)multi[2].filter_type, (int)multi[2].slope_poles, low[2], band[2]);
    }
}

// Persistent per-slot engine state for the FX bus -- deliberately kept OUT of the wire struct
// (SynthSharedControls only carries each slot's user-facing params). `state` is an opaque
// pointer each effect owns entirely: allocated on first use and freed whenever the slot's type
// changes, so a ported effect can size/shape its own private buffers (e.g. a delay line) freely
// without anything here needing to know what they look like.
typedef struct {
    int active_type;
    void *state;
} FXSlotEngine;

static void reset_fx_engine(FXSlotEngine engine[FX_SLOT_COUNT]) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) { engine[s].active_type = FX_TYPE_OFF; engine[s].state = NULL; }
}

// --- FX_TYPE_ENSEMBLE: ported from Airwindows StereoEnsemble (MIT license) ---
// A 12-tap fixed-delay ensemble/chorus: 6 delay pairs (A/B, C/D, ... K/L), one tap of each pair
// reading Left and the other reading Right, each pair's two lengths deliberately mismatched
// (e.g. 7481 vs 7523 samples) so the two channels decorrelate into a wide stereo image, summed
// with fixed per-tap gain weights. No feedback loop -- this is NOT a reverb.
//
// Buffer sizes are each exactly (base tap length + 10), matching the original header, giving
// headroom over the largest length any tap can reach (base * ~0.9933, at Depth=1.0 -- see
// delayfactor below).
typedef struct {
    float dA[7491], dB[7533];
    float dC[5789], dD[5747];
    float dE[4841], dF[4871];
    float dG[3119], dH[3089];
    float dI[2213], dJ[2223];
    float dK[1337], dL[1331];

    int oneA, oneB, oneC, oneD, oneE, oneF;
    int oneG, oneH, oneI, oneJ, oneK, oneL;
    int maxdelayA, maxdelayB, maxdelayC, maxdelayD, maxdelayE, maxdelayF;
    int maxdelayG, maxdelayH, maxdelayI, maxdelayJ, maxdelayK, maxdelayL;

    // Downsampling ladder: the original plugin only actually renders a fresh ensemble sample
    // every cycle_end samples (1 at 44.1/48k, up to 4 at 176/192k) to save CPU at high sample
    // rates, interpolating the skipped samples from lastRef*. cycle_end is derived once from
    // SAMPLE_RATE (a compile-time constant here) and never changes again for this slot's life.
    float lastRefL[7], lastRefR[7];
    int cycle;
    int cycle_end;

    uint32_t fpdL, fpdR;
} FxEnsembleState;

static void run_fx_ensemble(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxEnsembleState *st = (FxEnsembleState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxEnsembleState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->oneA = st->oneB = st->oneC = st->oneD = st->oneE = st->oneF = 1;
        st->oneG = st->oneH = st->oneI = st->oneJ = st->oneK = st->oneL = 1;
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0] = Depth, param[1] = FX Level -- both 0..1 (see the FX_TYPE_ENSEMBLE comment above).
    float depth = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float outlevel = fmaxf(0.0f, fminf(1.0f, params->param[1]));

    float delayfactor = 0.66f + (depth / 3.0f);
    float dryL = 1.0f - (outlevel * 0.65f);
    float dryR = dryL;

    st->maxdelayA = (int)(7481.0f * delayfactor); st->maxdelayB = (int)(7523.0f * delayfactor);
    st->maxdelayC = (int)(5779.0f * delayfactor); st->maxdelayD = (int)(5737.0f * delayfactor);
    st->maxdelayE = (int)(4831.0f * delayfactor); st->maxdelayF = (int)(4861.0f * delayfactor);
    st->maxdelayG = (int)(3109.0f * delayfactor); st->maxdelayH = (int)(3079.0f * delayfactor);
    st->maxdelayI = (int)(2203.0f * delayfactor); st->maxdelayJ = (int)(2213.0f * delayfactor);
    st->maxdelayK = (int)(1327.0f * delayfactor); st->maxdelayL = (int)(1321.0f * delayfactor);
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    float inputSampleL = *l, inputSampleR = *r;
    // Anti-denormal: substitute a tiny noise floor instead of an actual all-zero/near-zero
    // sample, so the delay lines never carry a denormal float (which can be very slow on some
    // CPUs without flush-to-zero enabled).
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float drySampleL = inputSampleL, drySampleR = inputSampleR;
        float outL = 0.0f, outR = 0.0f, temp = 0.0f;

        st->dA[st->oneA] = inputSampleL; st->oneA--; if (st->oneA < 0 || st->oneA > st->maxdelayA) st->oneA = st->maxdelayA;
        temp = st->dA[st->oneA]; outL += temp * 0.75f;
        st->dB[st->oneB] = inputSampleR; st->oneB--; if (st->oneB < 0 || st->oneB > st->maxdelayB) st->oneB = st->maxdelayB;
        temp = st->dB[st->oneB]; outR += temp * 0.15f;

        st->dC[st->oneC] = inputSampleL; st->oneC--; if (st->oneC < 0 || st->oneC > st->maxdelayC) st->oneC = st->maxdelayC;
        temp = st->dC[st->oneC]; outL += temp * 0.15f;
        st->dD[st->oneD] = inputSampleR; st->oneD--; if (st->oneD < 0 || st->oneD > st->maxdelayD) st->oneD = st->maxdelayD;
        temp = st->dD[st->oneD]; outR += temp * 0.75f;

        st->dE[st->oneE] = inputSampleL; st->oneE--; if (st->oneE < 0 || st->oneE > st->maxdelayE) st->oneE = st->maxdelayE;
        temp = st->dE[st->oneE]; outL += temp * 0.63f;
        st->dF[st->oneF] = inputSampleR; st->oneF--; if (st->oneF < 0 || st->oneF > st->maxdelayF) st->oneF = st->maxdelayF;
        temp = st->dF[st->oneF]; outR += temp * 0.27f;

        st->dG[st->oneG] = inputSampleL; st->oneG--; if (st->oneG < 0 || st->oneG > st->maxdelayG) st->oneG = st->maxdelayG;
        temp = st->dG[st->oneG]; outL += temp * 0.27f;
        st->dH[st->oneH] = inputSampleR; st->oneH--; if (st->oneH < 0 || st->oneH > st->maxdelayH) st->oneH = st->maxdelayH;
        temp = st->dH[st->oneH]; outR += temp * 0.63f;

        st->dI[st->oneI] = inputSampleL; st->oneI--; if (st->oneI < 0 || st->oneI > st->maxdelayI) st->oneI = st->maxdelayI;
        temp = st->dI[st->oneI]; outL += temp * 0.48f;
        st->dJ[st->oneJ] = inputSampleR; st->oneJ--; if (st->oneJ < 0 || st->oneJ > st->maxdelayJ) st->oneJ = st->maxdelayJ;
        temp = st->dJ[st->oneJ]; outR += temp * 0.32f;

        st->dK[st->oneK] = inputSampleL; st->oneK--; if (st->oneK < 0 || st->oneK > st->maxdelayK) st->oneK = st->maxdelayK;
        temp = st->dK[st->oneK]; outL += temp * 0.32f;
        st->dL[st->oneL] = inputSampleR; st->oneL--; if (st->oneL < 0 || st->oneL > st->maxdelayL) st->oneL = st->maxdelayL;
        temp = st->dL[st->oneL]; outR += temp * 0.48f;

        inputSampleL = (outL * outlevel) + (drySampleL * dryL);
        inputSampleR = (outR * outlevel) + (drySampleR * dryR);

        if (st->cycle_end == 4) {
            st->lastRefL[0] = st->lastRefL[4];
            st->lastRefL[2] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[1] = (st->lastRefL[0] + st->lastRefL[2]) * 0.5f;
            st->lastRefL[3] = (st->lastRefL[2] + inputSampleL) * 0.5f;
            st->lastRefL[4] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[4];
            st->lastRefR[2] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[1] = (st->lastRefR[0] + st->lastRefR[2]) * 0.5f;
            st->lastRefR[3] = (st->lastRefR[2] + inputSampleR) * 0.5f;
            st->lastRefR[4] = inputSampleR;
        } else if (st->cycle_end == 3) {
            st->lastRefL[0] = st->lastRefL[3];
            st->lastRefL[2] = (st->lastRefL[0] + st->lastRefL[0] + inputSampleL) / 3.0f;
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL + inputSampleL) / 3.0f;
            st->lastRefL[3] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[3];
            st->lastRefR[2] = (st->lastRefR[0] + st->lastRefR[0] + inputSampleR) / 3.0f;
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR + inputSampleR) / 3.0f;
            st->lastRefR[3] = inputSampleR;
        } else if (st->cycle_end == 2) {
            st->lastRefL[0] = st->lastRefL[2];
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[2] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[2];
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[2] = inputSampleR;
        } else {
            st->lastRefL[0] = inputSampleL;
            st->lastRefR[0] = inputSampleR;
        }
        st->cycle = 0;
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    } else {
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    }

    // 32-bit-float dither, scaled relative to each sample's own magnitude (frexpf's exponent).
    int expon;
    frexpf(inputSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    inputSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(inputSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    inputSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = inputSampleL;
    *r = inputSampleR;
}

// Runs the FX_SLOT_COUNT-slot serial FX bus in place on one stereo sample. Called once per
// sample (not per voice) on the fully summed mix -- see render_audio_block, where this sits
// after all voices (and the external input) have been added together, and before
// master_volume is applied. Ported effects land as new cases in the inner switch; each one
// reads/writes its own fx_engine[s].state (lazily allocating it on first call after a type
// change) and is responsible for freeing it if it allocated anything, via a matching branch in
// the "type changed" block below. Until effects are ported in, every fx_type other than Off is
// unreached and simply passes the dry signal through unchanged, same as Off.
static void process_fx_chain(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT], float *l, float *r) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        int type = (int)fx_params[s].fx_type;
        if (type != fx_engine[s].active_type) {
            // Type changed live -- release whatever the previous effect had allocated. (No
            // per-type free() branches exist yet since no effect has been ported in; add one
            // here alongside each new case below if that effect allocates its own state.)
            free(fx_engine[s].state);
            fx_engine[s].state = NULL;
            fx_engine[s].active_type = type;
        }
        if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) continue;

        float dry_l = *l, dry_r = *r;
        float wet_l = dry_l, wet_r = dry_r;
        switch (type) {
            case FX_TYPE_ENSEMBLE: run_fx_ensemble(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_OFF:
            default:
                break; // pass-through
        }
        float mix = fmaxf(0.0f, fminf(100.0f, fx_params[s].mix)) / 100.0f;
        *l = dry_l * (1.0f - mix) + wet_l * mix;
        *r = dry_r * (1.0f - mix) + wet_r * mix;
    }
}

float lookup_osc_sample(WaveType type, float phase, float dt, float pulse_width_frac, float *tri_integrator) {
    switch (type) {
        case WAVE_SAW: { float n = (phase * 2.0f) - 1.0f; return n - poly_blep_step(phase, dt); }
        case WAVE_SQUARE: return band_limited_pulse(phase, dt, 0.5f);
        case WAVE_PULSE: {
            // Clamped away from the extremes: at duty 0 or 1 the pulse degenerates to DC (no
            // edges left to BLEP-correct), so keep a sliver of pulse width on both sides.
            float duty = fmaxf(0.02f, fminf(0.98f, pulse_width_frac));
            return band_limited_pulse(phase, dt, duty);
        }
        case WAVE_TRIANGLE: {
            float n = (phase < 0.5f) ? 1.0f : -1.0f; n += poly_blep_step(phase, dt);
            float pf = phase + 0.5f; if (pf >= 1.0f) pf -= 1.0f; n -= poly_blep_step(pf, dt);
            *tri_integrator = 0.99f * (*tri_integrator) + (n * dt * 4.0f); return *tri_integrator;
        }
        case WAVE_SINE: return sinf(phase * 2.0f * M_PI);
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
        float voice_cents = UNISON_SAW_VOICE_SPREAD[u] * detune_cents;
        float dt = base_dt * powf(2.0f, voice_cents / 1200.0f);
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
    float inv_n = 1.0f / (float)UNISON_SAW_VOICES;
    if (out_l) *out_l = sum_l * inv_n;
    if (out_r) *out_r = sum_r * inv_n;
    return sum_mono * inv_n;
}

float lookup_lfo_waveform(int type, float phase) {
    switch (type) {
        case 1:  return sinf(phase * 2.0f * M_PI);
        case 2:  return 1.0f - (phase * 2.0f);
        case 3:  return (phase < 0.5f) ? 1.0f : -1.0f;
        case 0:
        default: return (phase < 0.5f) ? (phase * 4.0f) - 1.0f : 3.0f - (phase * 4.0f);
    }
}
static int setup_control_socket(const char *socket_path) {
    unlink(socket_path);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_un local_addr;
    memset(&local_addr, 0, sizeof(struct sockaddr_un));
    local_addr.sun_family = AF_UNIX;
    strncpy(local_addr.sun_path, socket_path, sizeof(local_addr.sun_path) - 1);
    if (bind(server_fd, (struct sockaddr *)&local_addr, sizeof(struct sockaddr_un)) < 0) { close(server_fd); return -1; }
    listen(server_fd, 5); return server_fd;
}

static void poll_control_socket(int server_fd, int *client_fd, char *packet_buf, size_t *bytes_so_far, SynthSharedControls *knobs) {
    if (*client_fd < 0) {
        *client_fd = accept(server_fd, NULL, NULL);
        if (*client_fd >= 0) { int c_flags = fcntl(*client_fd, F_GETFL, 0); fcntl(*client_fd, F_SETFL, c_flags | O_NONBLOCK); }
        return;
    }
    ssize_t bytes_received = recv(*client_fd, packet_buf + *bytes_so_far, sizeof(SynthSharedControls) - *bytes_so_far, 0);
    if (bytes_received > 0) {
        *bytes_so_far += (size_t)bytes_received;
        if (*bytes_so_far == sizeof(SynthSharedControls)) {
            memcpy(knobs, packet_buf, sizeof(SynthSharedControls));
            // Defensive: guarantee null-termination regardless of what the GUI actually sent,
            // since these two fields get handed straight to snd_pcm_open() as C strings.
            knobs->output_device[AUDIO_DEVICE_NAME_LEN - 1] = '\0';
            knobs->input_device[AUDIO_DEVICE_NAME_LEN - 1] = '\0';
            *bytes_so_far = 0;
        }
    } else if (bytes_received == 0 || (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(*client_fd); *client_fd = -1; *bytes_so_far = 0;
    }
}

// FIXED: Restored 'init_default_controls' macro mapping setup routine missing from compilation stack
static void init_default_controls(SynthSharedControls *knobs) {
    knobs->is_polyphonic = 1.0f; knobs->vco1_shape = 0.0f; knobs->vco2_shape = 3.0f;
    knobs->fm_intensity = 0.0f; knobs->vco1_vol = 0.8f; knobs->vco2_vol = 0.5f;
    knobs->portamento_time_ms = 150.0f; knobs->master_volume = 0.50f;
    knobs->vco1_pan = 0.0f; knobs->vco2_pan = 0.0f;
    knobs->vco1_fine_tune = 0.0f; knobs->vco2_pitch_offset = 0.0f; knobs->vco2_fine_tune = 0.0f;
    knobs->filter_ab_routing_mode = 0.0f; // Series: mixer -> A -> B
    knobs->filter_a_mode = 0.0f; // Normal (single filter)
    knobs->filter_a.cutoff = 800.0f; knobs->filter_a.res_q = 20.0f; knobs->filter_a.slope_poles = 0.0f; knobs->filter_a.filter_type = 1.0f;
    knobs->filter_a_multi_routing = 0.0f; // Series, matching the old default 3-filter behavior
    for(int f=0; f<3; f++) { knobs->filter_a_multi[f].cutoff = 400.0f + (f*200); knobs->filter_a_multi[f].res_q = 20.0f; knobs->filter_a_multi[f].slope_poles = 0.0f; knobs->filter_a_multi[f].filter_type = 1.0f; }
    knobs->filter_b.cutoff = 4000.0f; knobs->filter_b.res_q = 20.0f; knobs->filter_b.slope_poles = 0.0f; knobs->filter_b.filter_type = 0.0f; // Off by default -- Filter A alone matches old single-filter-ish defaults
    for(int l=0; l<LFO_COUNT; l++) { knobs->lfo[l].wave_type = 0.0f; knobs->lfo[l].rate_hz = 5.0f; knobs->lfo[l].mod_depth = 0.0f; knobs->lfo[l].key_sync = 1.0f; knobs->lfo[l].dest_target = 0.0f; knobs->lfo[l].phase_offset = 0.0f; }
    for(int e=0; e<ENV_COUNT; e++) { knobs->eg[e].d = 0.0f; knobs->eg[e].h = 0.0f; knobs->eg[e].a = 10.0f; knobs->eg[e].dec = 150.0f; knobs->eg[e].s = 80.0f; knobs->eg[e].r = 300.0f; knobs->eg[e].al = 0.0f; knobs->eg[e].pk = 100.0f; knobs->eg_mode[e] = 0.0f; }
    for(int i=0; i<4; i++) { knobs->eg_intensity[i] = 1.0f; knobs->eg_dest_target[i] = 0.0f; }
    // Subtle default detune (matches the old fixed +/-1.5% spread) and no width until the user asks for it
    knobs->vco1_unison_saw_detune = 25.0f; knobs->vco1_unison_saw_width = 50.0f;
    knobs->vco2_unison_saw_detune = 25.0f; knobs->vco2_unison_saw_width = 50.0f;
    knobs->mono_glide_random = 0.0f;
    knobs->vco1_mute = 0.0f; knobs->vco2_mute = 0.0f;
    knobs->hard_sync = 0.0f;
    knobs->vco1_phase_offset = 0.0f; knobs->vco2_phase_offset = 0.0f;

    knobs->vco1_pulse_width = 50.0f; knobs->vco2_pulse_width = 50.0f;
    knobs->noise_vol = 0.0f; knobs->noise_pan = 0.0f; knobs->noise_mute = 0.0f; knobs->noise_tilt = 0.0f;
    knobs->sub_shape = (float)SUB_SQUARE; knobs->sub_vol = 0.0f; knobs->sub_pan = 0.0f; knobs->sub_mute = 0.0f;
    // Noise Osc Amp envelope lives at eg[2]/eg_mode[2] and was covered by the generic
    // `for(int e=0; e<ENV_COUNT; e++)` default-fill loop above -- nothing extra needed here.

    knobs->ext_in_enable = 0.0f; knobs->ext_in_gain = 1.0f;
    for (int f = 0; f < FX_SLOT_COUNT; f++) {
        knobs->fx[f].fx_type = FX_TYPE_OFF;
        for (int p = 0; p < FX_SLOT_PARAM_COUNT; p++) knobs->fx[f].param[p] = 0.0f;
        knobs->fx[f].mix = 100.0f; // fully wet once a real effect is assigned to this slot
        knobs->fx[f].bypass = 0.0f;
    }
    snprintf(knobs->output_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->input_device, AUDIO_DEVICE_NAME_LEN, "default");
}

static void init_voice(SynthVoice *voice) {
    voice->midi_note = -1;
    voice->vco1_phase = 0.0f; voice->vco1_target_freq = 0.0f; voice->vco1_current_freq = 0.0f; voice->vco1_tri_state = 0.0f;
    voice->vco2_phase = 0.0f; voice->vco2_target_freq = 0.0f; voice->vco2_current_freq = 0.0f; voice->vco2_tri_state = 0.0f;
    for(int u=0; u<UNISON_SAW_VOICES; u++) { voice->vco1_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco2_unison_saw_phases[u] = (float)rand()/RAND_MAX; }
    for(int c=0; c<2; c++) { for(int f=0; f<3; f++) { voice->fa_low[c][f][0]=0.0f; voice->fa_low[c][f][1]=0.0f; voice->fa_band[c][f][0]=0.0f; voice->fa_band[c][f][1]=0.0f; } voice->fb_low[c][0]=0.0f; voice->fb_low[c][1]=0.0f; voice->fb_band[c][0]=0.0f; voice->fb_band[c][1]=0.0f; }
    voice->glide_time_multiplier = 1.0f;
    voice->vco1_eg.state = STATE_IDLE; voice->vco1_eg.current_level = 0.0f; voice->vco1_eg.release_level = 0.0f;
    voice->vco2_eg.state = STATE_IDLE; voice->vco2_eg.current_level = 0.0f; voice->vco2_eg.release_level = 0.0f;
    voice->noise_eg.state = STATE_IDLE; voice->noise_eg.current_level = 0.0f; voice->noise_eg.release_level = 0.0f;
    voice->noise_tilt_lp = 0.0f;
    voice->sub_phase = (float)rand()/RAND_MAX; voice->sub_tri_state = 0.0f;
    for(int e=0; e<4; e++) { voice->assign_eg[e].state = STATE_IDLE; voice->assign_eg[e].current_level = 0.0f; voice->assign_eg[e].release_level = 0.0f; }
}
// Rolls a randomized glide-time multiplier for Mono Legato: 1.0 = no variation. glide_random_pct
// (0-100) controls how far the multiplier can wander from 1.0 in either direction; floored well
// above 0 so a glide can never become instant/negative-time from an unlucky roll.
static float roll_glide_multiplier(float glide_random_pct) {
    float amount = fmaxf(0.0f, fminf(100.0f, glide_random_pct)) / 100.0f;
    if (amount <= 0.0f) return 1.0f;
    float rand_frac = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f; // -1..1
    return fmaxf(0.05f, 1.0f + (rand_frac * amount));
}

// A held-note stack for mono mode: releasing a key falls back to whichever other held key was
// pressed most recently, instead of cutting the sound just because the *last-pressed* key (not
// necessarily the currently-held one) was released -- that mismatch was the core "mono mode
// doesn't work right" bug: playing two overlapping notes and releasing the newer one would kill
// the sound entirely even though the older key was still physically held down.
#define MONO_STACK_SIZE 8
typedef struct {
    int notes[MONO_STACK_SIZE];
    int count;
} MonoNoteStack;

static void mono_stack_push(MonoNoteStack *stack, int note) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) stack->notes[j] = stack->notes[j + 1];
            stack->count--;
            break;
        }
    }
    if (stack->count < MONO_STACK_SIZE) { stack->notes[stack->count++] = note; }
    else {
        for (int j = 0; j < MONO_STACK_SIZE - 1; j++) stack->notes[j] = stack->notes[j + 1];
        stack->notes[MONO_STACK_SIZE - 1] = note;
    }
}

static void mono_stack_remove(MonoNoteStack *stack, int note) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) stack->notes[j] = stack->notes[j + 1];
            stack->count--;
            return;
        }
    }
}
static void handle_note_on(SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], int note, bool poly_mode_active, MonoNoteStack *mono_stack) {
    int slot = -1;
    if (poly_mode_active) {
        // BUG FIX: this used to only check vco1_eg/vco2_eg, so a voice whose noise_eg was
        // still audibly releasing (Noise Osc Amp envelope) could get stolen and reset mid-tail
        // -- an audible click, not just a modulation glitch, since the noise oscillator is a
        // real sound source. noise_eg must be idle too before a voice counts as free.
        for (int v = 0; v < MAX_VOICES; v++) { if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].noise_eg.state == STATE_IDLE) { slot = v; break; } }
        if (slot == -1) slot = 0;
    } else { slot = 0; }

    float target_hz = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    voices[slot].midi_note = note;
    voices[slot].vco1_target_freq = target_hz;
    voices[slot].vco2_target_freq = target_hz;

    // Mono Legato: only retrigger (reset phase/filters/envelopes) when this is genuinely the
    // first key of a phrase. If another key is already held when this one comes down, it's an
    // overlapping legato note -- just glide pitch (portamento) into the still-running envelope.
    bool is_first_key_of_phrase = poly_mode_active || (mono_stack->count == 0);

    if (is_first_key_of_phrase) {
        voices[slot].vco1_phase = 0.0f; voices[slot].vco2_phase = 0.0f;
        voices[slot].vco1_tri_state = 0.0f; voices[slot].vco2_tri_state = 0.0f;
        voices[slot].vco1_current_freq = target_hz; voices[slot].vco2_current_freq = target_hz;
        // Sub Osc has no independent pitch state -- it derives its frequency from Osc 1's
        // fully-modulated freq every sample -- but its own phase accumulator and triangle
        // integrator still need a fresh-note reset, same as Osc 1/2.
        voices[slot].sub_phase = 0.0f; voices[slot].sub_tri_state = 0.0f;
        voices[slot].glide_time_multiplier = 1.0f; // fresh note -- no glide happening, nothing to randomize
        for(int c=0; c<2; c++) { for(int f=0; f<3; f++) { voices[slot].fa_low[c][f][0]=0.0f; voices[slot].fa_low[c][f][1]=0.0f; voices[slot].fa_band[c][f][0]=0.0f; voices[slot].fa_band[c][f][1]=0.0f; } voices[slot].fb_low[c][0]=0.0f; voices[slot].fb_low[c][1]=0.0f; voices[slot].fb_band[c][0]=0.0f; voices[slot].fb_band[c][1]=0.0f; }
        for (int l = 0; l < LFO_COUNT; l++) { if (knobs->lfo[l].key_sync != 0.0f) global_lfo_phases[l] = 0.0f; }

        int m0 = (int)knobs->eg_mode[0]; voices[slot].vco1_eg.state = (m0 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco1_eg.current_level = (m0 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[0].al / 100.0f));
        int m1 = (int)knobs->eg_mode[1]; voices[slot].vco2_eg.state = (m1 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco2_eg.current_level = (m1 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[1].al / 100.0f));
        // Noise Osc Amp envelope: eg[2]/eg_mode[2]. (Sub Osc deliberately has no envelope of
        // its own here -- it shares vco1_eg, already retriggered above.)
        int mn = (int)knobs->eg_mode[2]; voices[slot].noise_eg.state = (mn == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].noise_eg.current_level = (mn == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[2].al / 100.0f));
        for(int e=0; e<4; e++) {
            int me = (int)knobs->eg_mode[3 + e]; voices[slot].assign_eg[e].state = (me == 5) ? STATE_DELAY : STATE_ATTACK;
            voices[slot].assign_eg[e].current_level = (me == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[3 + e].al / 100.0f));
        }
    } else {
        // An overlapping legato note: this is an actual audible glide, so reroll its time.
        voices[slot].glide_time_multiplier = roll_glide_multiplier(knobs->mono_glide_random);
    }
    if (!poly_mode_active) mono_stack_push(mono_stack, note);
}

static void handle_note_off(SynthVoice voices[], const SynthSharedControls *knobs, int note, bool poly_mode_active, MonoNoteStack *mono_stack) {
    if (poly_mode_active) {
        for (int v = 0; v < MAX_VOICES; v++) { if (voices[v].midi_note == note) {
            voices[v].vco1_eg.release_level = voices[v].vco1_eg.current_level; voices[v].vco1_eg.state = STATE_RELEASE;
            voices[v].vco2_eg.release_level = voices[v].vco2_eg.current_level; voices[v].vco2_eg.state = STATE_RELEASE;
            voices[v].noise_eg.release_level = voices[v].noise_eg.current_level; voices[v].noise_eg.state = STATE_RELEASE;
            for(int e=0; e<4; e++) { voices[v].assign_eg[e].release_level = voices[v].assign_eg[e].current_level; voices[v].assign_eg[e].state = STATE_RELEASE; }
        } }
        return;
    }

    mono_stack_remove(mono_stack, note);
    if (mono_stack->count > 0) {
        // FIXED: another key is still physically held -- fall back to it (the note most
        // recently pressed among those still down) instead of cutting the sound. This is the
        // core mono-mode bug: releasing whichever key came down last used to kill the note
        // even when an earlier key was still held.
        int fallback_note = mono_stack->notes[mono_stack->count - 1];
        float target_hz = 440.0f * powf(2.0f, (fallback_note - 69) / 12.0f);
        voices[0].midi_note = fallback_note;
        voices[0].vco1_target_freq = target_hz;
        voices[0].vco2_target_freq = target_hz;
        // Falling back to a still-held note is also a real audible glide -- reroll its time too.
        voices[0].glide_time_multiplier = roll_glide_multiplier(knobs->mono_glide_random);
        // Falling back to a still-held note never retriggers or releases -- the sound never stopped.
    } else {
        voices[0].vco1_eg.release_level = voices[0].vco1_eg.current_level; voices[0].vco1_eg.state = STATE_RELEASE;
        voices[0].vco2_eg.release_level = voices[0].vco2_eg.current_level; voices[0].vco2_eg.state = STATE_RELEASE;
        voices[0].noise_eg.release_level = voices[0].noise_eg.current_level; voices[0].noise_eg.state = STATE_RELEASE;
        for(int e=0; e<4; e++) { voices[0].assign_eg[e].release_level = voices[0].assign_eg[e].current_level; voices[0].assign_eg[e].state = STATE_RELEASE; }
    }
}

static void process_midi_input(snd_seq_t *seq_handle, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], bool poly_mode_active, MonoNoteStack *mono_stack) {
    snd_seq_event_t *ev = NULL;
    while (snd_seq_event_input_pending(seq_handle, 1) > 0) {
        snd_seq_event_input(seq_handle, &ev); if (ev == NULL) continue;
        if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
            int note = ev->data.note.note;
            if (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0) { handle_note_on(voices, knobs, global_lfo_phases, note, poly_mode_active, mono_stack); }
            else { handle_note_off(voices, knobs, note, poly_mode_active, mono_stack); }
        }
        snd_seq_free_event(ev);
    }
}
static float render_audio_block(SynthVoice voices[], const SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], const float *ext_in_buffer, float *out_buffer) {
    float peak = 0.0f;
    // Each oscillator is panned to stereo AT THE MIXER, before the signal reaches Filter A/B --
    // Filter A/B then run as two fully independent mono filters (one per channel, see fa_low/
    // fb_low's [channel] dimension) so that placement survives filtering instead of collapsing
    // to mono first. These knobs are constant across a whole buffer, so computed once here.
    float pan1_l = cosf((knobs->vco1_pan + 1.0f) * (M_PI / 4.0f)), pan1_r = sinf((knobs->vco1_pan + 1.0f) * (M_PI / 4.0f));
    float pan2_l = cosf((knobs->vco2_pan + 1.0f) * (M_PI / 4.0f)), pan2_r = sinf((knobs->vco2_pan + 1.0f) * (M_PI / 4.0f));
    float pan_noise_l = cosf((knobs->noise_pan + 1.0f) * (M_PI / 4.0f)), pan_noise_r = sinf((knobs->noise_pan + 1.0f) * (M_PI / 4.0f));
    float pan_sub_l = cosf((knobs->sub_pan + 1.0f) * (M_PI / 4.0f)), pan_sub_r = sinf((knobs->sub_pan + 1.0f) * (M_PI / 4.0f));

    // PERF: Unison Saw's per-unison pan (cosf/sinf) only depends on the Width knob, which is
    // constant across the whole buffer -- computed once here instead of every sample, for
    // every voice, for both oscillators. See compute_unison_saw_pan's comment for the numbers.
    float vco1_unison_pan_l[UNISON_SAW_VOICES], vco1_unison_pan_r[UNISON_SAW_VOICES];
    float vco2_unison_pan_l[UNISON_SAW_VOICES], vco2_unison_pan_r[UNISON_SAW_VOICES];
    compute_unison_saw_pan(knobs->vco1_unison_saw_width, vco1_unison_pan_l, vco1_unison_pan_r);
    compute_unison_saw_pan(knobs->vco2_unison_saw_width, vco2_unison_pan_l, vco2_unison_pan_r);

    // Noise Osc's tilt-color filter: a simple one-pole low/high split with a fixed ~800Hz pivot,
    // linearly crossfaded by the Tilt knob. The coefficient depends only on SAMPLE_RATE and the
    // fixed pivot, so it's a compile-time-equivalent constant -- computed once here too.
    const float noise_tilt_coeff = 1.0f - expf(-2.0f * (float)M_PI * 800.0f / (float)SAMPLE_RATE);

    // One-sample-delayed feedback state for LFO Phase modulation: holds each LFO's phase-shift
    // contribution from other LFOs and from voice 0's assignable EGs (the reference source for
    // LFO modulation, since LFOs are a single resource shared across every voice rather than a
    // per-voice one). Function-local static is safe here -- this program runs one synth engine
    // instance on a single audio thread.
    static float lfo_phase_mod_state[LFO_COUNT] = {0.0f};

    // FX bus engine state: persists across calls (and across FX_TYPE changes -- see
    // process_fx_chain) for the same "single synth engine instance" reason as above.
    static FXSlotEngine fx_engine[FX_SLOT_COUNT];
    static bool fx_engine_initialized = false;
    if (!fx_engine_initialized) { reset_fx_engine(fx_engine); fx_engine_initialized = true; }

    for (int i = 0; i < BUFFER_SIZE; i++) {
        float mixed_l = 0.0f, mixed_r = 0.0f;
        float lfo_out[LFO_COUNT] = {0.0f};
        for (int l = 0; l < LFO_COUNT; l++) {
            // Phase pot (0-360 degrees) plus any modulation, shifting only the read-phase used
            // for this sample's waveform lookup -- the underlying running phase (advanced below)
            // is never touched, so Rate/sync stay completely unaffected by this.
            float phase_offset = fmodf((knobs->lfo[l].phase_offset / 360.0f) + lfo_phase_mod_state[l], 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
            float read_phase = fmodf(global_lfo_phases[l] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
            lfo_out[l] = lookup_lfo_waveform((int)knobs->lfo[l].wave_type, read_phase) * knobs->lfo[l].mod_depth;
            global_lfo_phases[l] += knobs->lfo[l].rate_hz / (float)SAMPLE_RATE;
            if (global_lfo_phases[l] >= 1.0f) global_lfo_phases[l] -= 1.0f;
        }

        for (int v = 0; v < MAX_VOICES; v++) {
            // Sub Osc has no idle-check of its own -- it's silent whenever Osc 1's own amp
            // envelope (which it shares) is silent, so it doesn't need to factor in here.
            if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].noise_eg.state == STATE_IDLE) continue;

            for(int e=0; e<ENV_COUNT; e++) {
                if(e==0) tick_adsr(&voices[v].vco1_eg, &knobs->eg[e], (int)knobs->eg_mode[e]);
                else if(e==1) tick_adsr(&voices[v].vco2_eg, &knobs->eg[e], (int)knobs->eg_mode[e]);
                else if(e==2) tick_adsr(&voices[v].noise_eg, &knobs->eg[e], (int)knobs->eg_mode[e]);
                else tick_adsr(&voices[v].assign_eg[e-3], &knobs->eg[e], (int)knobs->eg_mode[e]);
            }

            // Mono Legato glide randomization: each voice's own glide_time_multiplier (rerolled
            // per glide event in handle_note_on/off) scales the plain Glide Time knob, so
            // successive legato glides don't all take exactly the same amount of time.
            float effective_glide_ms = fmaxf(1.0f, knobs->portamento_time_ms * voices[v].glide_time_multiplier);
            float portamento_coefficient = 1.0f - expf(-1.0f / ((effective_glide_ms / 1000.0f) * SAMPLE_RATE));
            voices[v].vco1_current_freq += (voices[v].vco1_target_freq - voices[v].vco1_current_freq) * portamento_coefficient;
            voices[v].vco2_current_freq += (voices[v].vco2_target_freq - voices[v].vco2_current_freq) * portamento_coefficient;

            float mod_vco1_pitch = knobs->vco1_fine_tune, mod_vco2_pitch = knobs->vco2_fine_tune + (knobs->vco2_pitch_offset * 100.0f);
            float mod_vco1_amp = 0.0f, mod_vco2_amp = 0.0f, fm_mod = 0.0f, mix_bal = 0.0f;
            // Filter cutoff/resonance mod destinations (targets 8-13), 3 slots each:
            //   slot 0 (targets 8/9)   = Filter A's PRIMARY filter: filter_a when Normal, or
            //                            filter_a_multi[0] (1st internal filter) when Multi
            //   slot 1 (targets 10/11) = Filter B
            //   slot 2 (targets 12/13) = filter_a_multi[2] (3rd internal filter, Multi mode only)
            // filter_a_multi[1] (the middle internal filter) has no dedicated mod destination
            // for now -- a reasonable trim given only 3 filter-mod slots exist; still fully
            // controllable by hand.
            // FIXED: resonance destinations (odd targets 9/11/13) used to fall into the same
            // slot as their neighboring Freq destination via integer division, so picking e.g.
            // "F1 Res" as an LFO destination silently modulated cutoff instead of resonance and
            // resonance itself was never actually modulatable by anything.
            float filter_cutoff_mods[3] = {0.0f, 0.0f, 0.0f};
            float filter_res_mods[3] = {0.0f, 0.0f, 0.0f};
            // Phase Modulation (new): a bipolar mod value shifts where in the cycle each
            // oscillator currently reads from, without touching its actual running phase
            // accumulator (that distinction matters -- it keeps pitch, hard sync, and portamento
            // completely unaffected by phase modulation).
            float mod_vco1_phase = 0.0f, mod_vco2_phase = 0.0f;
            // Unison Saw Detune Width as a mod destination (new).
            float mod_vco1_unison_detune = 0.0f, mod_vco2_unison_detune = 0.0f;

            for (int l = 0; l < LFO_COUNT; l++) {
                int target = (int)knobs->lfo[l].dest_target; float val = lfo_out[l];
                if (target == 1) mod_vco1_pitch += val * 1200.0f;
                if (target == 2) mod_vco2_pitch += val * 1200.0f;
                if (target == 3) { mod_vco1_pitch += val * 1200.0f; mod_vco2_pitch += val * 1200.0f; }
                if (target == 4) mod_vco1_pitch += val * 100.0f;
                if (target == 5) mod_vco2_pitch += val * 100.0f;
                if (target == 6) mod_vco1_amp += val;
                if (target == 7) mod_vco2_amp += val;
                if (target >= 8 && target <= 13) { int slot = (target - 8) / 2; if (((target - 8) % 2) == 1) filter_res_mods[slot] += val * 50.0f; else filter_cutoff_mods[slot] += val * 5000.0f; }
                // FIXED: this was checking target 20/21 (which are actually "LFO 3/4 Depth" in the
                // GUI's destination list), so FM Depth and Mix Balance (the real 22/23) could never
                // be reached as destinations, while picking "LFO 3/4 Depth" silently hijacked them.
                if (target == 22) fm_mod += val;
                if (target == 23) mix_bal += val;
                if (target == 24) mod_vco1_phase += val;
                if (target == 25) mod_vco2_phase += val;
                if (target == 26) mod_vco1_unison_detune += val * 2000.0f;
                if (target == 27) mod_vco2_unison_detune += val * 2000.0f;
            }

            // FIXED: each assignable EG now routes using its own destination combo (eg_dest_target),
            // not the corresponding LFO's destination -- the GUI already had a per-EG destination
            // dropdown, but it was never wired to anything back here.
            for (int e = 0; e < 4; e++) {
                int target = (int)knobs->eg_dest_target[e]; float val = voices[v].assign_eg[e].current_level * knobs->eg_intensity[e];
                if (target == 1) mod_vco1_pitch += val * 1200.0f;
                if (target == 2) mod_vco2_pitch += val * 1200.0f;
                if (target == 3) { mod_vco1_pitch += val * 1200.0f; mod_vco2_pitch += val * 1200.0f; }
                if (target == 4) mod_vco1_pitch += val * 100.0f;
                if (target == 5) mod_vco2_pitch += val * 100.0f;
                if (target == 6) mod_vco1_amp += val;
                if (target == 7) mod_vco2_amp += val;
                if (target >= 8 && target <= 13) { int slot = (target - 8) / 2; if (((target - 8) % 2) == 1) filter_res_mods[slot] += val * 50.0f; else filter_cutoff_mods[slot] += val * 5000.0f; }
                if (target == 22) fm_mod += val;
                if (target == 23) mix_bal += val;
                if (target == 24) mod_vco1_phase += val;
                if (target == 25) mod_vco2_phase += val;
                if (target == 26) mod_vco1_unison_detune += val * 2000.0f;
                if (target == 27) mod_vco2_unison_detune += val * 2000.0f;
            }

            // Phase pot (0-360 degrees) plus any modulation, converted to a 0..1 fractional-cycle
            // offset. Wrapped defensively since modulation can push it negative or past 1.
            float phase_offset1 = fmodf((knobs->vco1_phase_offset / 360.0f) + mod_vco1_phase, 1.0f); if (phase_offset1 < 0.0f) phase_offset1 += 1.0f;
            float phase_offset2 = fmodf((knobs->vco2_phase_offset / 360.0f) + mod_vco2_phase, 1.0f); if (phase_offset2 < 0.0f) phase_offset2 += 1.0f;

            // Unison Saw Detune Width, base knob plus any modulation, clamped back into its valid range.
            float vco1_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco1_unison_saw_detune + mod_vco1_unison_detune));
            float vco2_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco2_unison_saw_detune + mod_vco2_unison_detune));

            float hz1 = voices[v].vco1_current_freq * powf(2.0f, mod_vco1_pitch / 1200.0f);
            float hz2 = voices[v].vco2_current_freq * powf(2.0f, mod_vco2_pitch / 1200.0f);
            float dt1 = hz1 / (float)SAMPLE_RATE, dt2 = hz2 / (float)SAMPLE_RATE;

            float v2_out = 0.0f, v2_side_l = 0.0f, v2_side_r = 0.0f;
            if ((WaveType)((int)knobs->vco2_shape) == WAVE_UNISON_SAW) v2_out = render_unison_saw(voices[v].vco2_unison_saw_phases, dt2, vco2_unison_detune_final, vco2_unison_pan_l, vco2_unison_pan_r, phase_offset2, &v2_side_l, &v2_side_r);
            else {
                float vco2_read_phase = fmodf(voices[v].vco2_phase + phase_offset2, 1.0f); if (vco2_read_phase < 0.0f) vco2_read_phase += 1.0f;
                v2_out = lookup_osc_sample((WaveType)((int)knobs->vco2_shape), vco2_read_phase, dt2, knobs->vco2_pulse_width / 100.0f, &voices[v].vco2_tri_state);
            }
            float gain2 = (knobs->vco2_mute != 0.0f || knobs->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco2_vol + mix_bal + mod_vco2_amp));
            float osc2_amp = gain2 * voices[v].vco2_eg.current_level;

            // FIXED: FM now modulates Osc 1 with Osc 2's actual post-modulated signal -- its own
            // volume/mute, its own amplitude-modulation routing, and its own amplitude envelope
            // all shape the modulator before it reaches Osc 1, matching how FM synthesis normally
            // works (a modulator's envelope directly shapes the resulting brightness over time).
            // Previously this used the bare raw waveform sample, so muting Osc 2 or being in a
            // silent part of its envelope had no effect on the FM sound at all.
            // Deliberately excludes mix_bal (a final-mix-only balance control) AND vco2_mute
            // (muting Osc 2's audible output must not silence it as an FM modulator -- that's the
            // whole point of using Osc 2 purely as a modulator). Still responds to the plain
            // volume knob at 0, matching classic FM operator behavior where Level and modulation
            // depth are coupled.
            float gain2_for_fm = (knobs->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco2_vol + mod_vco2_amp));
            float v2_modulator = v2_out * gain2_for_fm * voices[v].vco2_eg.current_level;

            float fm_phase = voices[v].vco1_phase + (v2_modulator * fmaxf(0.0f, knobs->fm_intensity + fm_mod));
            fm_phase = fmodf(fm_phase + phase_offset1, 1.0f); if (fm_phase < 0.0f) fm_phase += 1.0f;
            float v1_out = 0.0f, v1_side_l = 0.0f, v1_side_r = 0.0f;
            bool osc1_is_unison_saw = (WaveType)((int)knobs->vco1_shape) == WAVE_UNISON_SAW;
            if (osc1_is_unison_saw) v1_out = render_unison_saw(voices[v].vco1_unison_saw_phases, dt1, vco1_unison_detune_final, vco1_unison_pan_l, vco1_unison_pan_r, phase_offset1, &v1_side_l, &v1_side_r);
            else v1_out = lookup_osc_sample((WaveType)((int)knobs->vco1_shape), fm_phase, dt1, knobs->vco1_pulse_width / 100.0f, &voices[v].vco1_tri_state);
            float gain1 = (knobs->vco1_mute != 0.0f || knobs->vco1_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco1_vol - mix_bal + mod_vco1_amp));
            float osc1_amp = gain1 * voices[v].vco1_eg.current_level;

            // --- Stereo mixer: each oscillator is panned to L/R here, before anything reaches
            // the filters, so its placement survives filtering (Filter A/B run as two
            // independent mono filters, one per channel -- see the fa_low/fb_low state). Osc
            // 1/2's Unison Saw mode already produces a real stereo pair from its own internal
            // unison spread (v1_side_l/r, v2_side_l/r) -- that's used directly as the
            // oscillator's L/R image, with its Pan knob then applied as an overall balance
            // shift on top of that existing width, rather than collapsing it to mono first. ---
            bool osc2_is_unison_saw = (WaveType)((int)knobs->vco2_shape) == WAVE_UNISON_SAW;
            float vco1_l = (osc1_is_unison_saw ? v1_side_l : v1_out) * osc1_amp * pan1_l;
            float vco1_r = (osc1_is_unison_saw ? v1_side_r : v1_out) * osc1_amp * pan1_r;
            float vco2_l = (osc2_is_unison_saw ? v2_side_l : v2_out) * osc2_amp * pan2_l;
            float vco2_r = (osc2_is_unison_saw ? v2_side_r : v2_out) * osc2_amp * pan2_r;

            // Sub Oscillator: exactly one octave below Osc 1, derived from hz1 (already fully
            // modulated -- fine tune, portamento, every LFO/EG pitch mod, all included) so it
            // implicitly tracks everything Osc 1's pitch does with zero pitch controls of its own.
            float dt_sub = (hz1 * 0.5f) / (float)SAMPLE_RATE;
            float sub_out = lookup_osc_sample(sub_wave_to_osc_wave((int)knobs->sub_shape), voices[v].sub_phase, dt_sub, 0.5f, &voices[v].sub_tri_state);
            float gain_sub = (knobs->sub_mute != 0.0f || knobs->sub_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->sub_vol));
            // Shares Osc 1's own amp envelope by design (see the struct comment) rather than a
            // separate one -- it's meant to always move exactly in lockstep with Osc 1.
            float sub_amp = sub_out * gain_sub * voices[v].vco1_eg.current_level;
            float sub_l = sub_amp * pan_sub_l, sub_r = sub_amp * pan_sub_r;

            // Dedicated Noise Oscillator: white noise through a simple one-pole tilt-color
            // filter (see noise_tilt_coeff above). tilt_frac in [-1,1]: -1 leans fully on the
            // low-passed component (dark), 0 is flat white (low+high == the original signal,
            // unity gain), +1 leans fully on the high (complementary) component (bright/hissy).
            float white = (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
            voices[v].noise_tilt_lp += noise_tilt_coeff * (white - voices[v].noise_tilt_lp);
            float noise_high = white - voices[v].noise_tilt_lp;
            float tilt_frac = fmaxf(-1.0f, fminf(1.0f, knobs->noise_tilt / 100.0f));
            float noise_out = voices[v].noise_tilt_lp * (1.0f - tilt_frac) + noise_high * (1.0f + tilt_frac);
            float gain_noise = (knobs->noise_mute != 0.0f || knobs->noise_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->noise_vol));
            float noise_amp = noise_out * gain_noise * voices[v].noise_eg.current_level;
            float noise_l = noise_amp * pan_noise_l, noise_r = noise_amp * pan_noise_r;

            float new_vco1_phase = voices[v].vco1_phase + dt1;
            bool vco1_wrapped = new_vco1_phase >= 1.0f;
            voices[v].vco1_phase = fmodf(new_vco1_phase, 1.0f);
            voices[v].vco2_phase = fmodf(voices[v].vco2_phase + dt2, 1.0f);
            voices[v].sub_phase = fmodf(voices[v].sub_phase + dt_sub, 1.0f);
            if (knobs->hard_sync != 0.0f && vco1_wrapped) {
                voices[v].vco2_phase = 0.0f;
                for (int u = 0; u < UNISON_SAW_VOICES; u++) voices[v].vco2_unison_saw_phases[u] = 0.0f;
            }

            // The "summer/mixer": all 4 oscillators combined into a stereo pair (each already
            // panned above), THEN that stereo signal goes into Filter A -> Filter B.
            float stage_in_l = vco1_l + vco2_l + sub_l + noise_l;
            float stage_in_r = vco1_r + vco2_r + sub_r + noise_r;

            // --- Filter A: Normal (single filter) or Multi (legacy 3-filter engine). Run once
            // per channel -- each channel keeps fully independent filter state (fa_low/fa_band's
            // [channel] dimension) so stereo placement isn't lost going through the filter. ---
            float a_out_l, a_out_r;
            if ((int)knobs->filter_a_mode == 0) {
                a_out_l = run_svf_filter(stage_in_l, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[0][0], voices[v].fa_band[0][0]);
                a_out_r = run_svf_filter(stage_in_r, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[1][0], voices[v].fa_band[1][0]);
            } else {
                // Only unit 0 and unit 2 have a dedicated mod destination (see the comment by
                // filter_cutoff_mods above) -- unit 1's mods are always 0.
                float multi_cutoff_mods[3] = { filter_cutoff_mods[0], 0.0f, filter_cutoff_mods[2] };
                float multi_res_mods[3] = { filter_res_mods[0], 0.0f, filter_res_mods[2] };
                a_out_l = run_filter_a_multi(stage_in_l, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[0], voices[v].fa_band[0]);
                a_out_r = run_filter_a_multi(stage_in_r, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[1], voices[v].fa_band[1]);
            }

            // --- Filter B: always a single Normal filter, also run once per channel ---
            bool ab_series = (int)knobs->filter_ab_routing_mode == 0;
            float b_in_l = ab_series ? a_out_l : stage_in_l; // Series: chained from A's output. Parallel: fed straight from the mixer, same as A.
            float b_in_r = ab_series ? a_out_r : stage_in_r;
            float b_out_l = run_svf_filter(b_in_l, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[0], voices[v].fb_band[0]);
            float b_out_r = run_svf_filter(b_in_r, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[1], voices[v].fb_band[1]);

            // --- Combine A and B: Series means B is simply the final stage; Parallel means both
            // are heard side by side, summed evenly ---
            mixed_l += ab_series ? b_out_l : (a_out_l + b_out_l) * 0.5f;
            mixed_r += ab_series ? b_out_r : (a_out_r + b_out_r) * 0.5f;
        }
        // LFO Phase as a mod destination (targets 28-31): computed once per sample (not per
        // voice, since LFOs are one resource shared by every voice) from other LFOs' outputs and
        // voice 0's assignable EGs (the reference source, for the same reason). Applied to affect
        // the NEXT sample's read-phase above -- a one-sample delay that avoids any ordering
        // ambiguity between "which LFO's output is current" within a single sample.
        float new_lfo_phase_mod[LFO_COUNT] = {0.0f};
        for (int l = 0; l < LFO_COUNT; l++) {
            int target = (int)knobs->lfo[l].dest_target;
            if (target >= 28 && target <= 31) new_lfo_phase_mod[target - 28] += lfo_out[l];
        }
        if (voices[0].vco1_eg.state != STATE_IDLE || voices[0].vco2_eg.state != STATE_IDLE) {
            for (int e = 0; e < 4; e++) {
                int target = (int)knobs->eg_dest_target[e];
                if (target >= 28 && target <= 31) new_lfo_phase_mod[target - 28] += voices[0].assign_eg[e].current_level * knobs->eg_intensity[e];
            }
        }
        for (int l = 0; l < LFO_COUNT; l++) lfo_phase_mod_state[l] = new_lfo_phase_mod[l];

        // External Audio Input: summed in here, after every voice but before the FX bus, so it
        // passes through the same FX_SLOT_COUNT chain the synth's own signal does. Taken as an
        // already-stereo pair straight from the capture buffer -- no per-voice-style panning,
        // see the ext_in_gain/ext_in_enable comment on the struct.
        if (knobs->ext_in_enable != 0.0f) {
            mixed_l += ext_in_buffer[i*2] * knobs->ext_in_gain;
            mixed_r += ext_in_buffer[i*2+1] * knobs->ext_in_gain;
        }

        // FX Bus: runs last, on the fully summed mix, before master_volume -- see
        // process_fx_chain's own comment for the routing rationale.
        process_fx_chain(knobs->fx, fx_engine, &mixed_l, &mixed_r);

        float f_l = mixed_l * knobs->master_volume * 0.15f, f_r = mixed_r * knobs->master_volume * 0.15f;
        out_buffer[i*2] = fmaxf(-0.98f, fminf(0.98f, f_l)); out_buffer[i*2+1] = fmaxf(-0.98f, fminf(0.98f, f_r));
        if (fabsf(f_l) > peak) peak = fabsf(f_l);
        if (fabsf(f_r) > peak) peak = fabsf(f_r);
    }
    return peak;
}
static void write_audio_block(snd_pcm_t *pcm_handle, const float *buffer, snd_pcm_uframes_t frame_count) {
    // Guards the pathological case where even the fallback-to-last-known-good reopen in main()
    // failed (e.g. a USB audio interface was physically unplugged) -- silently drop this block
    // rather than crash; main() will keep retrying the device on the next control packet.
    if (!pcm_handle) return;
    snd_pcm_uframes_t written = 0;
    while (written < frame_count && g_synth_running) {
        snd_pcm_sframes_t res = snd_pcm_writei(pcm_handle, buffer + (written * 2), frame_count - written);
        if (res < 0) { res = snd_pcm_recover(pcm_handle, res, 0); if (res < 0) break; }
        else { written += res; }
    }
}

// (Re)opens a stereo float PCM stream by ALSA device name, closing *handle first if it was
// already open. Used both at startup and whenever the Audio tab's device fields change live
// (see main()'s loop) -- a single place so open/configure/error-reporting can't drift between
// the two call sites.
// BUG FIX: the original startup code never checked snd_pcm_open()/snd_pcm_set_params()'s
// return values at all -- an unavailable "default" device (no sound card, no PulseAudio/ALSA
// routing) left pcm_handle uninitialized/unusable and the very first snd_pcm_writei() call
// would segfault or hang instead of failing with a clear message.
static bool open_pcm_stream(const char *device_name, snd_pcm_stream_t stream, snd_pcm_t **handle) {
    if (*handle) { snd_pcm_close(*handle); *handle = NULL; }
    int err = snd_pcm_open(handle, device_name, stream, 0);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to open: %s\n", stream == SND_PCM_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
        *handle = NULL;
        return false;
    }
    err = snd_pcm_set_params(*handle, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 1, 25000);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to configure: %s\n", stream == SND_PCM_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
        snd_pcm_close(*handle);
        *handle = NULL;
        return false;
    }
    return true;
}

int main() {
    const char *socket_path = "/tmp/synth.sock"; int client_fd = -1;
    signal(SIGINT, handle_sigint);
    int server_fd = setup_control_socket(socket_path); if (server_fd < 0) return 1;

    SynthSharedControls active_knobs; init_default_controls(&active_knobs);

    snd_pcm_t *pcm_handle = NULL;
    if (!open_pcm_stream(active_knobs.output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle)) {
        fprintf(stderr, "Cannot continue without an output device.\n");
        close(server_fd); unlink(socket_path); return 1;
    }
    char applied_output_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);

    // External Audio Input: best-effort. Unlike output, a capture device failing to open isn't
    // fatal -- external input just stays silent (ext_in_enable has nothing to mix in) until a
    // working device name is picked from the Audio tab.
    snd_pcm_t *pcm_capture_handle = NULL;
    open_pcm_stream(active_knobs.input_device, SND_PCM_STREAM_CAPTURE, &pcm_capture_handle);
    char applied_input_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_input_device, sizeof(applied_input_device), "%s", active_knobs.input_device);

    snd_seq_t *seq_handle;
    int seq_err = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_INPUT, 0);
    if (seq_err < 0) {
        fprintf(stderr, "MIDI sequencer failed to open: %s\n", snd_strerror(seq_err));
        snd_pcm_close(pcm_handle); if (pcm_capture_handle) snd_pcm_close(pcm_capture_handle);
        close(server_fd); unlink(socket_path); return 1;
    }
    snd_seq_set_client_name(seq_handle, "YourSynth Socket Engine");
    snd_seq_create_simple_port(seq_handle, "Input", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    char socket_packet_buffer[sizeof(SynthSharedControls)]; size_t control_bytes_received = 0;
    float global_lfo_phases[LFO_COUNT] = {0.0f}; int vu_decimator = 0;

    SynthVoice voices[MAX_VOICES]; for (int i = 0; i < MAX_VOICES; i++) init_voice(&voices[i]);
    MonoNoteStack mono_stack = {0};

    while (g_synth_running) {
        poll_control_socket(server_fd, &client_fd, socket_packet_buffer, &control_bytes_received, &active_knobs);
        bool poly_mode_active = (active_knobs.is_polyphonic != 0.0f);

        // Live device switching: the Audio tab writes these as plain fields on the same control
        // packet as every other knob. Only reopen the stream whose name actually changed.
        if (strncmp(active_knobs.output_device, applied_output_device, AUDIO_DEVICE_NAME_LEN) != 0) {
            if (open_pcm_stream(active_knobs.output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle)) {
                snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);
            } else {
                // Failed -- fall back to the last device that was actually working rather than
                // leaving pcm_handle NULL, and stop retrying this same bad name every block.
                open_pcm_stream(applied_output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle);
                snprintf(active_knobs.output_device, AUDIO_DEVICE_NAME_LEN, "%s", applied_output_device);
            }
        }
        if (strncmp(active_knobs.input_device, applied_input_device, AUDIO_DEVICE_NAME_LEN) != 0) {
            open_pcm_stream(active_knobs.input_device, SND_PCM_STREAM_CAPTURE, &pcm_capture_handle); // best-effort
            snprintf(applied_input_device, sizeof(applied_input_device), "%s", active_knobs.input_device);
        }

        process_midi_input(seq_handle, voices, &active_knobs, global_lfo_phases, poly_mode_active, &mono_stack);

        // Read one block of external input. Silence (the zero-init) if there's no working
        // capture device, or a read comes back short/broken -- external input degrading to
        // silence should never be able to stall or crash the synth's own audio output.
        float capture_buffer[BUFFER_SIZE * 2] = {0.0f};
        if (pcm_capture_handle) {
            snd_pcm_sframes_t got = snd_pcm_readi(pcm_capture_handle, capture_buffer, BUFFER_SIZE);
            if (got < 0) {
                snd_pcm_recover(pcm_capture_handle, (int)got, 0);
            } else if ((snd_pcm_uframes_t)got < BUFFER_SIZE) {
                memset(capture_buffer + (got * 2), 0, (BUFFER_SIZE - (size_t)got) * 2 * sizeof(float));
            }
        }

        float buffer[BUFFER_SIZE * 2];
        float block_peak = render_audio_block(voices, &active_knobs, global_lfo_phases, capture_buffer, buffer);

        vu_decimator++;
        if (client_fd >= 0 && vu_decimator >= 12) { send(client_fd, &block_peak, sizeof(float), 0); vu_decimator = 0; }
        write_audio_block(pcm_handle, buffer, BUFFER_SIZE);
    }
    if (client_fd >= 0) close(client_fd);
    if (server_fd >= 0) close(server_fd);
    unlink(socket_path);
    snd_seq_close(seq_handle);
    snd_pcm_close(pcm_handle);
    if (pcm_capture_handle) snd_pcm_close(pcm_capture_handle);
    return 0;
}
