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
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <wavpack/wavpack.h>
#include <lo/lo.h>

#define SAMPLE_RATE 48000
// BUFFER_SIZE and MAX_VOICES are now hard CEILINGS for array allocation only -- how many
// frames actually get rendered per block, and how many voices are actually allowed to be
// simultaneously active, are live user controls (SynthSharedControls.block_size_frames and
// .active_voice_limit, both on the Audio tab) rather than a number picked once at compile time
// and shipped fixed. This exists because the right value for either one is a property of the
// ACTUAL machine this is running on -- a fast desktop and a constrained embedded board don't
// have the same answer, and guessing one number for every deployment either wastes headroom on
// fast hardware or underruns on slow hardware. The diagnostics on write_audio_block/
// handle_note_on/the main loop's [perf] check exist so that tuning these live is an informed
// process (raise it, watch for [perf]/[voices]/[audio] warnings, back off if you see them) --
// see effective_voice_limit()/effective_block_size() for how the live values get clamped into
// these ceilings.
#define BUFFER_SIZE 1024
#define MAX_VOICES  256
#define LFO_COUNT   16
// LFOs 3-16 (0-indexed 2-15) run at a decimated control rate instead of full audio rate -- 16
// LFOs are effectively 16 always-running oscillators, and most modulation uses don't need
// audio-rate smoothness. Recomputed every LFO_DECIMATION_FACTOR samples (24 samples = 2kHz
// effective control rate at 48kHz -- still ~80 recomputes/cycle even at a fast 25Hz LFO), with
// linear interpolation between recompute points to avoid zipper noise. LFOs 1/2 (index 0/1) stay
// full audio-rate, since they're the ones most likely used for audible-rate FM/AM effects.
#define LFO_DECIMATION_FACTOR 24
#define ASSIGNABLE_EG_COUNT 6  // EG 1-6 (in addition to Osc 1-4 Amp)
#define ENV_COUNT   (4 + ASSIGNABLE_EG_COUNT)  // Osc 1 Amp, Osc 2 Amp, Osc 3 Amp, Osc 4 Amp, + assignable EGs
#define UNISON_SAW_VOICES 7
#define FX_SLOT_COUNT 12
// Multi-source/multi-output audio routing: EXT_IN_PAIR_COUNT mirrors RECORD_MAX_CHANNELS/2 (the
// capture side's own existing channel ceiling, see RECORD_MAX_CHANNELS below) -- each pair p reads
// capture channels 2p/2p+1 as one independently enable/gain/routable stereo source, generalizing
// what used to be a single hardcoded-to-channels-0/1 ext_in tap. SOURCE_COUNT is 1 (the Program --
// today's single patch; a future multi-Program "Combi" mode would add more source ids here without
// changing this model) plus every ext-in pair. OUTPUT_MAX_CHANNELS/OUTPUT_PAIR_MAX mirror the same
// ceiling on the OUTPUT side, symmetric with the input side's existing RECORD_MAX_CHANNELS -- see
// query_output_device_channel_count()/open_pcm_stream() in main().
#define EXT_IN_PAIR_COUNT 8
#define SOURCE_COUNT (1 + EXT_IN_PAIR_COUNT)
#define OUTPUT_MAX_CHANNELS 16
#define OUTPUT_PAIR_MAX (OUTPUT_MAX_CHANNELS / 2)
#define AUDIO_DEVICE_NAME_LEN 64
// Recording: hard ceiling on how many raw input channels can be captured/selected at once
// (generous for a class-compliant multichannel interface -- 2/4/8 are the common real cases).
// Also doubles as the channel count actually requested when opening the capture PCM stream --
// see query_device_channel_count()/open_pcm_stream() in main().
#define RECORD_MAX_CHANNELS 16
// Fixed-size output path buffer for the WAV recorder -- bigger than AUDIO_DEVICE_NAME_LEN since
// a real filesystem path (not just an ALSA device name) needs the room.
#define RECORD_PATH_LEN 256
// Programmable tempo: hard ceiling on how many (time, bpm) tempo markers the tempo-map can hold
// at once -- see the Tempo section on SynthSharedControls and compute_current_tempo_bpm().
#define TEMPO_MARKER_MAX 16
// DFD (direct-from-disk) sample playback: the Mapping tab's Sample Bank has no product-facing
// cap on how many zones can be loaded -- the catalog now travels over its own dedicated,
// event-driven socket (see zone_upsert_handler()/zone_remove_handler()), not the
// fixed SynthSharedControls struct, specifically so it can scale with a real multisample
// instrument's zone count instead of a compile-time constant. g_sample_slots[] is still a single
// preallocated-once array sized SAMPLE_SLOT_CAPACITY, though, rather than a realloc()'d/growable
// one: sample_disk_thread_main() dereferences &g_sample_slots[index] directly with no lock, which
// is only safe because that array's base address never moves for the life of the process --
// letting it move (via realloc) while the disk thread concurrently reads from it would be a
// genuine use-after-free race, not a style preference. SAMPLE_SLOT_CAPACITY is deliberately huge
// (~34MB total at ~340 bytes/slot) so it is never the real constraint in practice, while keeping
// the "never moves" safety property. See the Sample Bank section on SynthSharedControls and
// find_matching_sample_slot().
#define SAMPLE_SLOT_CAPACITY 100000
#define SAMPLE_PATH_LEN 256
// How many independent round-robin groups (Mapping tab's Round-Robin Group control) can exist at
// once -- a real, small, legitimately-boundable UI concept (like MAX_VOICES/MAX_SAMPLE_STREAMS),
// entirely unrelated to how many zones the Sample Bank can hold. See find_matching_sample_slot().
#define MAX_ROUND_ROBIN_GROUPS 256
// How many voices/oscillators can be actively DFD-streaming at once, independent of MAX_VOICES --
// a fixed pool (like hd10's own MAX_VOICES=64) rather than one ring buffer per SynthVoice
// oscillator slot, since embedding a ring buffer per voice would reserve its ~256KB for every one
// of MAX_VOICES(256)*2 oscillators whether or not that voice ever plays a sample. See
// SampleStream/g_sample_streams.
#define MAX_SAMPLE_STREAMS 32
#define SAMPLE_RING_SIZE (1 << 15) // 32768 frames per channel -- matches the DFD engine this was ported from
#define SAMPLE_RING_MASK (SAMPLE_RING_SIZE - 1)
#define SAMPLE_XFADE_SIZE 1024
#define SAMPLE_PRE_ROLL_LIMIT 7200 // frames cached in RAM per loaded sample for zero-latency note-on (~150ms @ 48kHz)

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
    WAVE_PULSE, // appended (not inserted) so existing saved patches' numeric shape indices don't shift
    WAVE_SAMPLE, // DFD-streamed multisample playback -- see render_sample_osc()/SampleStream. Also appended.
    WAVE_OFF // Compute-bypass: skips pan trig + render + mix for this oscillator. Phase still
             // advances and its envelope still ticks (Hard Sync/FM and voice-idle logic depend on
             // both staying live), so nothing downstream needs to special-case an Off oscillator.
} WaveType;

typedef struct {
    float d; float h; float a; float dec; float s; float r; float al; float pk;
} PackEGParams;

typedef struct {
    float wave_type; float rate_hz; float mod_depth; float key_sync; float dest_target; float phase_offset;
    // Modulation Source (right-click on the depth pot in the GUI): scales this LFO's output by
    // a live MIDI controller, e.g. gating vibrato depth with the mod wheel. -1 = None (no
    // scaling -- the LFO always runs at its knob depth). 0-127 = that MIDI CC number. 128 =
    // channel (monophonic) aftertouch. See render_audio_block's LFO loop.
    float mod_source_cc;
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
#define FX_SLOT_PARAM_COUNT 8
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
} FXSlotParams;

// One source's assignment onto the hardware output pairs -- a many-to-many grid (a source can
// feed multiple output pairs at once, e.g. Program to both a main mix and a monitor/cue pair).
// Wrapped in its own struct (rather than a bare 2D array directly on SynthSharedControls) purely
// so the existing OSC_NESTED macro -- which addresses an array-of-structs' own array field via
// offsetof -- applies unmodified; see OSC_PARAMS[]'s "/hds/routing/%d/out/%d" entry.
typedef struct {
    float enable[OUTPUT_PAIR_MAX];
} SourceRouting;

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
// floats; see AUDIO_DEVICE_NAME_LEN). Running total after that change was 285 float items + 128
// string bytes.
// BREAKING CHANGE (FX routing): added 1 float (fx_routing_mode, Series/Parallel -- see the
// comment on that field). Running total after that change was 286 float items + 128 string
// bytes.
// BREAKING CHANGE (MIDI device selection): added 1 more fixed 64-byte string (midi_device --
// not a float; see AUDIO_DEVICE_NAME_LEN). New total: 286 float items + 192 string bytes. The
// Python side's struct.pack format changes from "286f64s64s" to "286f64s64s64s".
// BREAKING CHANGE (pitch bend range, sustain pedal polarity, LFO Modulation Source): added 3
// trailing floats (pb_range_up, pb_range_down, sustain_pedal_polarity) + grew PackLfoParams from
// 6 to 7 fields (added mod_source_cc), i.e. +4 floats across the 4 LFOs = +7 floats total. New
// total: 293 float items + 192 string bytes. The Python side's struct.pack format changes from
// "286f64s64s64s" to "293f64s64s64s". (Mod matrix destinations 32-40, added the same round of
// work, are new *values* an existing float (dest_target) can take -- they don't change the wire
// layout.)
// BREAKING CHANGE (6 assignable EGs, up from 4): ASSIGNABLE_EG_COUNT went 4 -> 6, so ENV_COUNT
// (3 + ASSIGNABLE_EG_COUNT) went 7 -> 9. That grows eg[ENV_COUNT] by 2 entries (+16 floats, 8
// params each) and eg_mode[ENV_COUNT] by 2 (+2 floats), plus eg_intensity/eg_dest_target each
// grow from 4 to 6 elements (+2 floats each) -- total +22 floats. New total: 315 float items +
// 192 string bytes. The Python side's struct.pack format changes from "293f64s64s64s" to
// "315f64s64s64s".
// BREAKING CHANGE (Filter A/B routing audit): filter_ab_routing_mode grew from a 2-way
// Series/Parallel switch to a 3-way switch (0=Series A->B, 1=Series B->A [new], 2=Parallel
// [was 1]), and Parallel's fixed 50/50 blend became the adjustable filter_ab_blend (+1 new
// trailing float). New total: 316 float items + 192 string bytes. The Python side's
// struct.pack format changes from "315f64s64s64s" to "316f64s64s64s".
// BREAKING CHANGE (adjustable audio latency): added audio_latency_ms (+1 float), the Audio
// tab's Latency control -- see open_pcm_stream(). New total: 317 float items + 192 string
// bytes. The Python side's struct.pack format changes from "316f64s64s64s" to "317f64s64s64s".
// BREAKING CHANGE (live polyphony + block size): MAX_VOICES and BUFFER_SIZE became hard
// allocation ceilings (256 and 1024 respectively) instead of fixed values; added
// active_voice_limit and block_size_frames (+2 floats), the Audio tab's Polyphony and Block
// Size controls -- see effective_voice_limit()/effective_block_size(). New total: 319 float
// items + 192 string bytes. The Python side's struct.pack format changes from "317f64s64s64s"
// to "319f64s64s64s".
// BREAKING CHANGE (Recording tab): added record_enable, record_include_synth_output,
// record_downmix_stereo, record_input_channel_enable[RECORD_MAX_CHANNELS=16] (+19 floats) and
// one new trailing fixed string, record_path[RECORD_PATH_LEN=256] (appended after midi_device,
// so string bytes go from 192 to 448) -- see the WavRecorder functions/the recording block in
// main(). New total: 338 float items + 448 string bytes. The Python side's struct.pack format
// changes from "319f64s64s64s" to "338f64s64s64s256s".
// BREAKING CHANGE (programmable Tempo): added tempo_bpm (the Tempo knob, global sidebar) +
// tempo_marker_count + tempo_marker_time[TEMPO_MARKER_MAX=16] + tempo_marker_bpm[TEMPO_MARKER_MAX]
// (+34 floats) -- see compute_current_tempo_bpm() and the Tempo section below. No new string
// fields. New total: 372 float items + 448 string bytes. The Python side's struct.pack format
// changes from "338f64s64s64s256s" to "372f64s64s64s256s".
// BREAKING CHANGE (DFD sample playback / WAVE_SAMPLE): added sample_slot_root_note[
// MAX_SAMPLE_SLOTS=16] (+16 floats) and one new trailing fixed string block,
// sample_slot_path[MAX_SAMPLE_SLOTS][SAMPLE_PATH_LEN=256] (appended after record_path -- packs
// as one flat 4096-byte blob, string bytes go from 448 to 4544) -- see load_sample_slot()/the
// live-reload check in main(). This is deliberately the minimal wire shape needed to load and
// test WAVE_SAMPLE end to end (see MAX_SAMPLE_SLOTS's comment) -- NOT the real per-note/velocity
// zone mapping system, which is future work. New total: 388 float items + 4544 string bytes. The
// Python side's struct.pack format changes from "372f64s64s64s256s" to "388f64s64s64s256s4096s".
// BREAKING CHANGE (Sample Bank zone mapping / Mapping tab): added sample_slot_min_note[
// MAX_SAMPLE_SLOTS], sample_slot_max_note[MAX_SAMPLE_SLOTS], sample_slot_min_velocity[
// MAX_SAMPLE_SLOTS], sample_slot_max_velocity[MAX_SAMPLE_SLOTS], sample_slot_unpitched[
// MAX_SAMPLE_SLOTS] (+80 floats, right after sample_slot_root_note[] -- see find_matching_
// sample_slot()/apply_sample_slot_zone_metadata() and the Mapping tab in frontend.py). This is
// the real per-note/velocity zone mapping the previous entry called out as future work: a note
// now only sounds through a loaded slot whose key range AND velocity range both cover it
// (find_matching_sample_slot()) -- a note outside every zone is silent, rather than the old
// "nearest root note always plays something" behavior. sample_slot_unpitched (the Mapping tab's
// Root Key dropdown's "DRUM (Unpitched)" entry) forces that slot's playback rate to 1.0 regardless
// of which note in its zone triggered it, instead of pitch-shifting relative to root_note -- see
// render_sample_osc's ratio calculation. No new string fields. New total: 468 float items + 4544
// string bytes. The Python side's struct.pack format changes from "388f64s64s64s256s4096s" to
// "468f64s64s64s256s4096s".
// BREAKING CHANGE (Sampler feature expansion: loop editor + round-robin): added
// sample_slot_loop_start[MAX_SAMPLE_SLOTS], sample_slot_loop_end[MAX_SAMPLE_SLOTS],
// sample_slot_loop_enabled[MAX_SAMPLE_SLOTS], sample_slot_round_robin_group[MAX_SAMPLE_SLOTS]
// (+64 floats, right after sample_slot_unpitched[]). Loop start/end travel as fractions (0.0-1.0)
// of the file's own total_frames -- see apply_sample_slot_zone_metadata()'s conversion to real
// frame indices and the Mapping tab's waveform loop editor in frontend.py, which never needs to
// know a file's actual frame count. loop_enabled replaces load_sample_slot()'s old hardcoded
// "always loop 40%-85%" default -- a slot with looping off now genuinely one-shots and releases
// its stream at natural end (see render_sample_osc). round_robin_group lets several slots sharing
// a group id and an overlapping zone cycle through each other on successive hits instead of the
// same one always winning -- see find_matching_sample_slot(). No new string fields. New total:
// 532 float items + 4544 string bytes. The Python side's struct.pack format changes from
// "468f64s64s64s256s4096s" to "532f64s64s64s256s4096s".
// BREAKING CHANGE (uncapped Sample Bank): removed every sample_slot_* field and the
// sample_slot_path[][] blob entirely -- the whole Sample Bank now travels over its own dedicated,
// event-driven socket (see zone_upsert_handler()/zone_remove_handler() and the
// ZONE_OP_UPSERT/ZONE_OP_REMOVE messages), not this struct. It existed here only because the
// catalog used to be capped at a small compile-time MAX_SAMPLE_SLOTS and could be broadcast in
// full on every change like every other knob; a real multisample instrument's zone count has no
// such product-facing cap now (see SAMPLE_SLOT_CAPACITY's comment), and resending thousands of
// zones' worth of floats every time an unrelated filter knob moves was never sound design even
// before the cap was lifted. apply_sample_slot_zone_metadata() and the MAX_SAMPLE_SLOTS-bounded
// live-reload scan in main() are gone with it -- reconciliation now happens once per catalog
// message, at receipt (or once a throttled deferred load completes), not every audio block. New
// total: 372 float items + 448 string bytes (back to the shape just before the DFD/Sample Bank
// work). The Python side's struct.pack format changes from "532f64s64s64s256s4096s" to
// "372f64s64s64s256s".
// BREAKING CHANGE (4-oscillator architecture + FM algorithms + dual Hard Sync + 16 LFOs +
// WavPack recording): a large consolidated pass, folding in the still-undocumented "Osc3 +
// initial FM algorithm fields" WIP work (fm_algorithm/fm_depth_3_1/fm_depth_3_2 + vco3_shape/
// fine_tune/pitch_offset/phase_offset/unison_saw_detune/unison_saw_width -- vco3_vol/pan/mute/
// pulse_width already existed, renamed in place from the old dedicated Noise oscillator's
// noise_vol/pan/mute/tilt, net 0 field-count change) together with this pass's own additions:
// - Sub retired outright, replaced by Osc4 -- a full peer of Osc1/2/3. vco4_vol/pan/mute/
//   pulse_width (4, net 0 vs the old sub_shape/vol/pan/mute) plus 6 brand-new identity fields
//   (vco4_shape/fine_tune/pitch_offset/phase_offset/unison_saw_detune/unison_saw_width).
// - 5 new FM depth fields (fm_depth_4_1/4_3/1_2/3_4/1_3) for the 12-algorithm FM system: +5.
// - ENV_COUNT (4 + ASSIGNABLE_EG_COUNT) grew from 9 to 10 for Osc4's own fixed amp envelope:
//   eg[ENV_COUNT] +8 floats, eg_mode[ENV_COUNT] +1 float: +9.
// - hard_sync renamed hard_sync_mode (0=Off/1=Parallel/2=Serial) -- net 0 field-count change.
// - LFO_COUNT went 4 -> 16: PackLfoParams lfo[LFO_COUNT] (7 fields/LFO) grows by 12 LFOs * 7:
//   +84. New wave_type value 4 = Off; LFOs 3-16 run at a decimated control rate -- neither
//   changes the wire layout, only what values/behavior an existing field carries.
// - record_format (WAV/WavPack format selector, Recording tab): +1.
// Total added: 6 (vco3 identity) + 6 (vco4 identity) + 5 (new FM depths) + 9 (ENV_COUNT 9->10)
// + 84 (LFO_COUNT 4->16) + 1 (record_format) = 111, plus the WIP work's fm_algorithm/
// fm_depth_3_1/fm_depth_3_2 (+3, that WIP commit never updated this comment) = 114 floats. No
// new string fields. New total: 486 float items + 448 string bytes. The Python side's
// struct.pack format changes from "372f64s64s64s256s" to "486f64s64s64s256s".
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

    // 72 Envelope Parameter Items (ENV_COUNT=9 Envelopes * 8 Parameters, incl. attack start/peak level)
    PackEGParams eg[ENV_COUNT];

    // --- FILTER SECTION (overhauled): 4 oscillators -> mixer -> Filter A -> Filter B, with
    // A and B routable in Series (either order) or Parallel with each other (24 floats total).
    //
    // Filter A / Filter B outer routing: 0 = Series A->B (mixer -> A -> B -> output),
    // 1 = Series B->A (mixer -> B -> A -> output -- same two filters, reversed order, e.g. to
    // put Filter A's Multi mode after Filter B instead of before it), 2 = Parallel (mixer -> A,
    // mixer -> B independently, blended by filter_ab_blend -> output). (+1 float)
    float filter_ab_routing_mode;

    // Parallel-mode blend between Filter A and Filter B's independent outputs: 0 = all A,
    // 100 = all B, 50 (default) = even blend, matching the old fixed-50/50 behavior. Unused in
    // either Series mode, where the chain order alone decides which filter is "heard last".
    // (+1 float)
    float filter_ab_blend;

    // Filter A: a single Normal filter by default. Switching filter_a_mode to 1 ("Multi")
    // replaces it with the 3-filter Multi engine below instead of filter_a -- the original
    // 3-filter-stack design, now scoped down to being one of Filter A's two modes.
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

    // 28 Active LFO Mapping Items (4 LFOs * 7 Properties, incl. phase offset + mod source CC)
    PackLfoParams lfo[LFO_COUNT];

    // ASSIGNABLE_EG_COUNT (6) Custom EG Intensity Modulators
    float eg_intensity[ASSIGNABLE_EG_COUNT];

    // ASSIGNABLE_EG_COUNT (6) Custom EG Destination Targets (each assignable EG routes
    // independently of the LFOs)
    float eg_dest_target[ASSIGNABLE_EG_COUNT];

    // ENV_COUNT (9) Active Envelope Topology Selection Mode Indices
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

    // Hard Sync: 0=Off, 1=Parallel (Osc 1 resets Osc 2, 3, AND 4 independently -- each slave
    // shares Osc 1's fundamental but keeps its own shape/detune), 2=Serial (Osc 1 resets Osc 2,
    // Osc 2's reset resets Osc 3, Osc 3's reset resets Osc 4 -- a cascading "telephone game" of
    // phase truncation). See render_audio_block's do_sync/t_wrap blocks.
    float hard_sync_mode;

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

    // Osc 3: a full peer of Osc 1/2 (own shape/pitch/pan/phase/unison controls below), subsuming
    // what used to be a separate "dedicated Noise oscillator" -- vco3_pulse_width dual-duties as
    // Pulse Width [2,98] when Shape=Pulse or Tilt [-100,100] when Shape=Noise, exactly mirroring
    // vco1/vco2_pulse_width's own shape-conditional reuse (see render_audio_block). Has its own
    // amp envelope (eg[2]/eg_mode[2]), same as vco1_eg/vco2_eg.
    float vco3_vol;
    float vco3_pan;
    float vco3_mute;
    float vco3_pulse_width;

    // Osc 4: a full peer of Osc 1/2/3 in every respect (own shape/pitch/pan/phase/unison
    // controls below, own amp envelope eg[3]/eg_mode[3]). Formerly a fixed "Sub Oscillator"
    // locked one octave below Osc 1 with no controls of its own and sharing Osc 1's envelope;
    // that automatic-lockstep behavior is gone, replaced by an explicit vco4_pitch_offset knob
    // (set to -12 semitones to reproduce the old Sub behavior manually).
    float vco4_vol;
    float vco4_pan;
    float vco4_mute;
    float vco4_pulse_width; // dual-duty Pulse Width/Tilt, mirrors vco3_pulse_width

    // --- FX Bus + Multi-Source Routing Architecture ---

    // External Audio Input, per stereo pair (appended: generalized from a single hardcoded-to-
    // capture-channels-0/1 tap into EXT_IN_PAIR_COUNT independently enable/gain/routable stereo
    // sources -- pair p reads capture channels 2p/2p+1). Each pair is its own routable source (see
    // fx[].fx_source/source_routing below) -- no pan control here, same as before: capture is
    // taken as an already-stereo L/R pair and passed through as-is rather than re-panned from mono.
    float ext_in_enable[EXT_IN_PAIR_COUNT];
    float ext_in_gain[EXT_IN_PAIR_COUNT];

    // FX Bus: FX_SLOT_COUNT slots, each now assigned to one source (fx[].fx_source, 0=Program,
    // 1..EXT_IN_PAIR_COUNT=an ext-in pair) rather than implicitly processing one shared mix --
    // see process_fx_chain()'s source_id parameter. Each ported effect fills in its own case in
    // that function's dispatch switch; until then every slot is a pass-through (see FX_TYPE_OFF).
    FXSlotParams fx[FX_SLOT_COUNT];

    // FX Bus routing, per source (appended: generalized from one tab-wide switch into one
    // Series/Parallel choice per source, applying only to that source's own assigned slots): 0 =
    // Series (that source's slots run in array order, each fed the previous one's output), 1 =
    // Parallel (every one of that source's slots fed the SAME pre-FX input independently, their
    // outputs summed and averaged -- see process_fx_chain()).
    float fx_source_routing_mode[SOURCE_COUNT];

    // Multi-output hardware routing: which physical output pair(s) each source's final signal (its
    // dry bus if it owns no FX slots, or the result of its own Series/Parallel chain above) lands
    // on -- a many-to-many grid, since a source can feed multiple output pairs at once (e.g.
    // Program to both a main mix and a monitor/cue pair). source_routing[0] is the Program;
    // source_routing[1..EXT_IN_PAIR_COUNT] are the ext-in pairs, same indexing as ext_in_enable/
    // fx_source above. See render_audio_block's per-output-pair summing and the always-on limiter,
    // which moved from once-per-block to once-per-output-pair to match.
    SourceRouting source_routing[SOURCE_COUNT];

    // Pitch Bend range, in semitones, applied equally to both oscillators. Separately
    // specifiable up/down since real-world PB ranges are often asymmetric (e.g. a guitar-style
    // patch bending up a whole step but down a minor third). See pb_bend_cents in
    // render_audio_block.
    float pb_range_up;
    float pb_range_down;

    // Sustain pedal polarity: 0 = Normal (CC64 >= 64 means held, matching essentially every
    // sustain pedal), 1 = Inverted (for the rare pedal wired backwards, sending 0 when
    // pressed). See handle_sustain_pedal_change()/the CC64 handling in process_midi_input.
    float sustain_pedal_polarity;

    // Requested ALSA output/input buffer depth, in milliseconds (passed straight through as
    // snd_pcm_set_params's `latency` argument in open_pcm_stream() -- see the comment there).
    // This is the actual floor on output latency (NOT BUFFER_SIZE, which only controls the
    // internal render chunk size). Default 25ms. Raising it doesn't fix a genuine CPU-throughput
    // shortfall (a block that consistently takes too long to render will eventually underrun no
    // matter how deep the buffer is) -- it buys a bigger safety cushion against occasional,
    // marginal overruns (a block that only sometimes takes a little too long) at the cost of
    // added latency. See the Audio tab's Latency control.
    float audio_latency_ms;

    // Live polyphony ceiling: how many voices are allowed to be simultaneously ACTIVE, clamped
    // into [1, MAX_VOICES] by effective_voice_limit(). MAX_VOICES itself is just the hard array
    // allocation size now -- this is the real, tunable ceiling, because the right number is a
    // property of the actual machine this is running on (see the comment on MAX_VOICES/
    // BUFFER_SIZE). Idle voice slots stay near-free regardless of this value; raising it only
    // costs real CPU once that many voices are genuinely active at once. See the Audio tab's
    // Polyphony control, and the [voices]/[perf]/[audio] diagnostics for tuning it live.
    float active_voice_limit;

    // Live render block size, in samples, clamped into [1, BUFFER_SIZE] by
    // effective_block_size(). Smaller = snappier MIDI-to-audio response (a fresh note waits at
    // most one block before it's reflected in the output) and finer-grained modulation, at the
    // cost of more per-block call/syscall overhead; larger = less overhead per sample rendered,
    // at the cost of coarser MIDI timing. Independent of audio_latency_ms (the ALSA buffer
    // depth) -- this only controls how many frames get computed and handed to ALSA per
    // main-loop iteration, not how deep ALSA's own ring buffer is. See the Audio tab's Block
    // Size control.
    float block_size_frames;

    // --- Recording (Recording tab) ---
    // Edge-triggered start/stop: 0->1 opens record_path and begins writing, 1->0 finalizes and
    // closes the file. See the WavRecorder functions and the recording block in main().
    float record_enable;
    // Include the synth's own final post-FX stereo output (exactly what's going to the DAC,
    // including any processed External Input already summed into that bus -- see ext_in_enable)
    // as two of the recorded channels.
    float record_include_synth_output;
    // 0 = write every selected channel as its own channel in a genuine multichannel WAV file.
    // 1 = sum every selected channel equally into a single L/R stereo pair instead (scaled down
    // to avoid clipping -- see the downmix code in main()).
    float record_downmix_stereo;
    // Per-channel record enable for the RAW input device capture (independent of
    // ext_in_enable/ext_in_gain -- this taps the capture stream directly, not the synth's
    // internal stereo mix, so it works even with no notes playing: "just a recorder for a
    // class-compliant device"). Only the first N entries matter, where N is whatever
    // query_device_channel_count() last detected for the current input_device (see
    // detected_input_channels in the status feedback the backend sends back).
    float record_input_channel_enable[RECORD_MAX_CHANNELS];

    // --- Tempo (Global panel) ---
    // A single flat "Tempo" knob for the common case, plus a programmable tempo-MAP system: up
    // to TEMPO_MARKER_MAX (time, bpm) marker pairs, linearly interpolated between whichever two
    // markers bracket the engine's current elapsed time -- see compute_current_tempo_bpm().
    // Elapsed time here means seconds since this engine process started (main()'s running
    // sample count / SAMPLE_RATE); there's no other transport/song-position concept in this
    // engine. tempo_marker_count == 0 means "ignore the marker arrays entirely, tempo_bpm alone
    // is the whole story" -- currently the only value the frontend ever sends, since there's no
    // marker editor UI yet. The marker arrays are still reserved in the wire layout so a future
    // editor (or anything else driving the protocol directly) can populate them without another
    // breaking change. Nothing in the DSP consumes the computed tempo yet -- this lays the
    // groundwork for future tempo-synced features (e.g. delay time, LFO rate).
    float tempo_bpm;
    float tempo_marker_count;
    float tempo_marker_time[TEMPO_MARKER_MAX]; // seconds since engine start
    float tempo_marker_bpm[TEMPO_MARKER_MAX];

    // --- Osc 3 identity/tuning (appended, not inserted, per this file's own numeric-shape-index
    // precedent -- see WAVE_PULSE/WAVE_SAMPLE's comment) -- vco3_vol/pan/mute/pulse_width already
    // exist above (reused in place from the old dedicated Noise oscillator's fields). ---
    float vco3_shape; // WaveType, full palette -- matches vco1_shape/vco2_shape
    float vco3_fine_tune; // cents, mirrors vco1_fine_tune
    float vco3_pitch_offset; // semitones, mirrors vco2_pitch_offset -- Osc3 is a full peer
    float vco3_phase_offset; // degrees, mirrors vco1/vco2_phase_offset
    float vco3_unison_saw_detune;
    float vco3_unison_saw_width;

    // --- Osc 4 identity/tuning (appended), mirrors the Osc 3 block above exactly -- replaces
    // the old fixed Sub Oscillator entirely. vco4_vol/pan/mute/pulse_width already exist above. ---
    float vco4_shape; // WaveType, full palette -- matches vco1/2/3_shape
    float vco4_fine_tune; // cents
    float vco4_pitch_offset; // semitones (set to -12 to reproduce the old Sub's fixed octave-down)
    float vco4_phase_offset; // degrees
    float vco4_unison_saw_detune;
    float vco4_unison_saw_width;

    // --- FM Algorithm system: a fixed, cycle-free set of selectable 4-operator routing
    // topologies (see FM_ALGORITHMS/render_fm_oscillator) -- NOT a general routing matrix, to
    // keep every topology provably acyclic. fm_intensity (existing field, above) keeps its exact
    // existing meaning (edge Osc2->Osc1) in every algorithm that uses that edge. Algorithm 0
    // ("Classic", 2->1 only) reproduces today's exact behavior and is the default. ---
    float fm_algorithm; // 0-11, see FM_ALGORITHMS
    float fm_depth_3_1; // Osc3 -> Osc1
    float fm_depth_3_2; // Osc3 -> Osc2
    float fm_depth_4_1; // Osc4 -> Osc1
    float fm_depth_4_3; // Osc4 -> Osc3
    float fm_depth_1_2; // Osc1 -> Osc2
    float fm_depth_3_4; // Osc3 -> Osc4
    float fm_depth_1_3; // Osc1 -> Osc3

    // --- Manual oscillator Pitch knobs (appended) --- a per-oscillator pitch offset the user can
    // dial by hand, independent of fine_tune (cents trim) and pitch_offset (the semitone-octave
    // knob Osc2/3/4 already have). Range +-24 semitones (+-2 octaves), stored directly in cents
    // to match mod_vcoN_pitch's own units. Summed into the exact same mod_vcoN_pitch accumulator
    // an LFO/EG already modulating "Osc N Pitch" writes into (see render_audio_block) -- a knob
    // and a live modulator both targeting pitch just add, no new mod-matrix target needed. Also
    // the same underlying control an LFO/EG's "Osc N Pitch" destination has always modulated, now
    // additionally reachable by hand and via its own right-click assignment menu (frontend.py).
    float vco1_pitch;
    float vco2_pitch;
    float vco3_pitch;
    float vco4_pitch;

    // Recording format: 0=WAV (32-bit float, lossless, larger files), 1=WavPack (lossless
    // compression, ~40-60% smaller, converted to 24-bit PCM before encoding since WavPack -- like
    // FLAC -- losslessly compresses integer PCM, not float). Frozen into the writer thread's
    // RecordEngine.cmd_record_format at the same record-start edge as record_path, so a change
    // mid-take never desyncs an already-open file. See wavpack_recorder_*/WavPackRecorder.
    // Declared here (float #486, immediately before the 4 fixed-size string fields below) to
    // match the Python side's "486f64s64s64s256s" packing order -- it was originally declared
    // after record_path (the struct's actual last field), which put it 4 bytes into the string
    // region once memcpy'd from the wire, shifting output_device/input_device/midi_device/
    // record_path each by 4 bytes and truncating record_path's null-terminated content down to
    // whatever garbage/zero bytes preceded it -- the cause of "[record] failed to open '' for
    // writing" even when the frontend sent a real path.
    float record_format;

    // --- Sample Bank (DFD/WAVE_SAMPLE) + Mapping tab zones ---
    // The Sample Bank itself (which files are loaded, their key/velocity zones, loop points,
    // round-robin groups) is NOT part of this struct -- it travels over its own dedicated,
    // event-driven socket instead (see zone_upsert_handler()/zone_remove_handler()
    // and the ZONE_OP_UPSERT/ZONE_OP_REMOVE messages), sent only when the catalog itself actually
    // changes, not broadcast in full on every unrelated knob turn like the fields in this struct
    // are. See g_sample_slots[]/find_matching_sample_slot() for the backend-side catalog, and the
    // Mapping tab in frontend.py for the editor.

    // Audio device selection: fixed-size ALSA PCM device name strings (e.g. "default",
    // "hw:1,0"), always null-terminated. main() compares these against whichever device name
    // it last opened each stream with, and closes/reopens that stream on a live change via
    // open_pcm_stream(). Deliberately plain fixed-size char buffers rather than more float
    // items: a device name is text, not a knob value.
    char output_device[AUDIO_DEVICE_NAME_LEN];
    char input_device[AUDIO_DEVICE_NAME_LEN];

    // MIDI input source selection: "auto" (default) connects to every real (kernel-backed, not
    // "Midi Through", not the ALSA System client) MIDI source currently present AND keeps
    // listening for new ones plugged in later -- see midi_autoconnect_scan()/the announce-port
    // subscription in main(). Any other value is taken as an exact ALSA sequencer client name
    // (as shown in the Audio tab's MIDI dropdown) to connect to instead of auto-detecting --
    // still reconnects automatically if that same-named device is unplugged and replugged.
    char midi_device[AUDIO_DEVICE_NAME_LEN];

    // Full filesystem path (not an ALSA device name, hence the bigger buffer than
    // AUDIO_DEVICE_NAME_LEN) the recorder writes to on the next record_enable 0->1 transition.
    // The frontend regenerates this to a fresh timestamped filename in the chosen Recordings
    // folder after every stop, so consecutive takes never collide/overwrite.
    char record_path[RECORD_PATH_LEN];
} SynthSharedControls;

// Clamp the live polyphony/block-size controls into their hard array-allocation ceilings.
// Centralized here so every loop bound and buffer-fill site derives the same clamped value the
// same way, rather than each repeating its own (fmaxf/fminf + cast) inline.
static inline int effective_voice_limit(const SynthSharedControls *knobs) {
    int n = (int)knobs->active_voice_limit;
    if (n < 1) n = 1;
    if (n > MAX_VOICES) n = MAX_VOICES;
    return n;
}
static inline int effective_block_size(const SynthSharedControls *knobs) {
    int n = (int)knobs->block_size_frames;
    if (n < 1) n = 1;
    if (n > BUFFER_SIZE) n = BUFFER_SIZE;
    return n;
}

// Programmable tempo: resolves the Tempo knob (tempo_bpm) plus an optional tempo-map
// (tempo_marker_count time/bpm pairs) into a single "what's the tempo right now" value.
//
// With no markers programmed (the common case, and currently the only case the frontend ever
// sends), this is just tempo_bpm -- a plain flat tempo. With markers, it finds whichever two
// markers bracket `elapsed_seconds` and linearly interpolates between their bpm values; before
// the first marker it holds that marker's bpm flat (rather than extrapolating backwards), and
// after the last it holds the last marker's bpm flat -- the same convention a DAW tempo track
// uses. Markers don't need to be given in time order: this scans for the nearest-before and
// nearest-after markers directly, so out-of-order input still resolves correctly.
static float compute_current_tempo_bpm(const SynthSharedControls *knobs, double elapsed_seconds) {
    int count = (int)knobs->tempo_marker_count;
    if (count < 0) count = 0;
    if (count > TEMPO_MARKER_MAX) count = TEMPO_MARKER_MAX;
    if (count == 0) return knobs->tempo_bpm;

    int prev = -1; double prev_time = -1e18;
    int next = -1; double next_time = 1e18;
    for (int i = 0; i < count; i++) {
        double t = (double)knobs->tempo_marker_time[i];
        if (t <= elapsed_seconds && t > prev_time) { prev = i; prev_time = t; }
        if (t > elapsed_seconds && t < next_time) { next = i; next_time = t; }
    }
    if (prev < 0 && next < 0) return knobs->tempo_bpm; // defensive fallback, shouldn't happen when count > 0
    if (prev < 0) return knobs->tempo_marker_bpm[next]; // before the first marker: hold its bpm flat
    if (next < 0) return knobs->tempo_marker_bpm[prev]; // after the last marker: hold its bpm flat

    double span = next_time - prev_time;
    double frac = (span > 1e-9) ? (elapsed_seconds - prev_time) / span : 0.0;
    double bpm = (double)knobs->tempo_marker_bpm[prev] + frac * ((double)knobs->tempo_marker_bpm[next] - (double)knobs->tempo_marker_bpm[prev]);
    return (float)bpm;
}

typedef struct {
    EnvelopeState state;
    int env_type; // 0:ADSR, 1:AR, 2:AD, 3:ASR, 4:AHD, 5:DADSR, 6:AHDSR
    float current_level;
    float delay_timer;
    float hold_timer;
    float release_level; // level captured at the instant note-off triggers release; release ramps from here to 0
} ADSR;

typedef struct {
    int midi_note;
    int midi_velocity; // 0-127, captured at note-on -- currently used only by the Sample Bank's
                        // velocity-layer zone matching (find_matching_sample_slot); not yet
                        // applied to amp/filter response for the periodic oscillators (no
                        // engine-wide velocity sensitivity exists yet, see handle_note_on's
                        // comment on this).

    float vco1_phase;
    float vco1_target_freq;
    float vco1_current_freq;
    float vco1_tri_state;
    // Previous sample's fully-computed fm_phase (post phase_offset1, post-wrap) -- used to
    // derive the true instantaneous phase velocity under FM (phase modulation from Osc 2), so
    // the PolyBLEP window width used for Osc 1's waveform lookup tracks the actual local phase
    // derivative instead of just the carrier's own nominal dt1. See render_audio_block.
    float vco1_prev_fm_phase;
    float vco1_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco1_eg;

    float vco2_phase;
    float vco2_target_freq;
    float vco2_current_freq;
    float vco2_tri_state;
    // Same role as vco1_prev_fm_phase, above -- Osc2 gained its own FM-target capability once
    // the FM Algorithm system let it be modulated too (algorithms where Osc3 -> Osc2).
    float vco2_prev_fm_phase;
    // "After" half of a hard-sync amplitude-discontinuity BLEP correction, carried one sample
    // forward from the sample that detected the sync to the first sample rendered with the
    // post-reset phase (Saw/Square/Pulse only -- see render_audio_block/sync_blep_shape_after).
    float vco2_pending_sync_correction;
    float vco2_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco2_eg;

    float vco3_phase;
    float vco3_target_freq;
    float vco3_current_freq;
    float vco3_tri_state;
    float vco3_prev_fm_phase;
    float vco3_pending_sync_correction; // Osc3 is a hard-sync slave too now -- see vco2's own field
    float vco3_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco3_eg;

    float vco4_phase;
    float vco4_target_freq;
    float vco4_current_freq;
    float vco4_tri_state;
    float vco4_prev_fm_phase;
    float vco4_pending_sync_correction; // Osc4 is a hard-sync slave too -- see vco2's own field
    float vco4_unison_saw_phases[UNISON_SAW_VOICES];
    ADSR vco4_eg;

    // Filter A's SVF register banks: [channel: 0=L,1=R][unit: 3 "units", unit 0 used alone in
    // Normal mode, all 3 used as the Multi engine in Multi mode][cascade stage: 2, 2nd stage
    // only used when a unit's slope is set to 4-pole]. Independent per-voice AND per-channel --
    // Filter A/B now run as two fully independent mono filters (one per stereo channel) so that
    // each oscillator's Pan placement survives the trip through the filters instead of being
    // collapsed to mono first.
    float fa_low[2][3][2];
    float fa_band[2][3][2];

    // Filter B's SVF register bank: [channel][cascade stage].
    float fb_low[2][2];
    float fb_band[2][2];

    // Assignable envelope instances 1-ASSIGNABLE_EG_COUNT
    ADSR assign_eg[ASSIGNABLE_EG_COUNT];

    // One-pole tilt-filter low-pass state, one per oscillator now that Tilt-shaped noise is
    // available on all 4 (see render_audio_block's tilt branch). vco3_eg/vco4_eg (Osc 3/4's amp
    // envelopes) are declared above, alongside their other per-oscillator state.
    float vco1_tilt_lp;
    float vco2_tilt_lp;
    float vco3_tilt_lp;
    float vco4_tilt_lp;

    // Mono Legato glide time randomization: rerolled each time this voice starts an actual
    // pitch glide (an overlapping legato note, or falling back to a still-held note on release).
    // 1.0 = no variation (uses the plain Glide Time knob exactly).
    float glide_time_multiplier;

    // Sustain pedal: set instead of actually releasing when a note-off arrives while the pedal
    // is held (see handle_note_off/handle_sustain_release). Cleared on any fresh retrigger of
    // this voice slot, since a new note-on makes whatever was pending before moot.
    bool sustain_pending;

    // DFD (direct-from-disk) sample playback state, one per oscillator -- see render_sample_osc()
    // and the SampleStream pool below. Only meaningful when vco1_shape/vco2_shape == WAVE_SAMPLE;
    // otherwise vco*_sample_stream stays -1 and these are untouched. This voice OWNS the playback
    // position (where in the sample we are); the SampleStream it points at owns the actual ring
    // buffer/file handle (shared, disk-thread-managed resources drawn from a fixed pool -- see
    // MAX_SAMPLE_STREAMS).
    int vco1_sample_stream;   // index into g_sample_streams[], or -1 if none claimed
    double vco1_sample_ptr;   // absolute frame position into the sample (pre-roll array while
                               // vco1_sample_preroll, then the disk-streamed ring buffer after)
    bool vco1_sample_preroll;
    int vco2_sample_stream;
    double vco2_sample_ptr;
    bool vco2_sample_preroll;
    int vco3_sample_stream;
    double vco3_sample_ptr;
    bool vco3_sample_preroll;
    int vco4_sample_stream;
    double vco4_sample_ptr;
    bool vco4_sample_preroll;
} SynthVoice;
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
// oscillator/filter work alone would explain any strain.
typedef struct {
    int mode;
    float atk_level;
    float peak_level;
    float sustain_level;
    float attack_rate;
    float decay_rate;
    float release_rate_per_level; // multiply by env->release_level for the actual release decrement
    float t_dly;
    float t_hld;
} EGDerivedRates;

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

// Filter A's "Multi" mode: the original 3-filter engine, now scoped down to being one of Filter
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

// --- FX_TYPE_CREAMCOAT: ported from Airwindows CreamCoat (MIT license) ---
// A genuine reverb (unlike Ensemble above): a 4-stage, 4-way Householder feedback delay
// network -- 16 delay lines (A-P), each of the two stereo channels using all 16 physical delay
// lengths but reading/writing them in a different rotation per stage, so L and R decorrelate
// while still sharing one set of tuned lengths. The network runs at a continuously variable,
// reduced internal "tank" rate (DeRez) and reconstructs a full-rate output via a 3-point
// bezier-style blend of the tank's last few ticks (the bez_* fields below -- ported from the
// original's bez[] array, named here for readability, same meaning/order).
// param[0] Select: 17 room-size presets (delay lengths swapped live on change, mid-tank-content
//   reset but feedback/reconstruction state deliberately left alone, matching the original).
// param[1] Regen: feedback amount. param[2] DeRez: tank downsample rate (1.0 = full rate/
//   quality; lower = grittier, more aliased tank, an intentional character control, not just a
//   CPU knob). param[3] Predelay. param[4] Wetness: NOT a crossfade -- 0.5 is both dry and wet
//   at full strength (see the original's own comment, kept below), so raising it adds reverb
//   level without pulling back the dry signal.
#define CREAMCOAT_PREDELAY 15000
// Compile-time max length for each of the 16 delay lines (+5 headroom, matching the original
// header). The runtime length actually in use (shortA..shortP below) depends on the Select
// preset and is always within these maximums.
#define CC_KA 350
#define CC_KB 1710
#define CC_KC 1610
#define CC_KD 835
#define CC_KE 700
#define CC_KF 1260
#define CC_KG 1110
#define CC_KH 1768
#define CC_KI 280
#define CC_KJ 2645
#define CC_KK 1410
#define CC_KL 1175
#define CC_KM 12
#define CC_KN 3110
#define CC_KO 120
#define CC_KP 2370

// 17 room-size presets (Select), each giving all 16 delay-line lengths in A..P order --
// transcribed as-is from the original's switch/case table.
static const int CREAMCOAT_PRESETS[17][16] = {
    {65,124,83,180,200,291,108,189,73,410,479,310,11,928,23,654},
    {114,205,498,195,205,318,143,254,64,721,512,324,11,782,26,394},
    {118,272,292,145,200,241,204,504,50,678,424,412,11,1124,47,766},
    {19,474,301,275,260,321,371,571,50,410,697,414,11,986,47,522},
    {112,387,452,289,173,476,321,593,73,343,829,91,11,1055,43,862},
    {60,368,295,272,210,284,326,830,125,236,737,486,11,1178,75,902},
    {73,311,472,251,134,509,393,591,124,1070,340,525,11,1367,75,816},
    {159,518,514,165,275,494,296,667,75,1101,116,414,11,1261,79,998},
    {41,741,274,59,306,332,291,767,42,881,959,422,11,1237,45,958},
    {251,437,783,189,130,272,244,761,128,1190,320,491,11,1409,58,455},
    {316,510,1087,349,359,74,79,1269,34,693,749,511,11,1751,93,403},
    {254,651,845,316,373,267,182,857,215,1535,1127,315,11,1649,97,829},
    {113,101,673,357,340,229,278,1008,265,1890,155,267,11,2233,116,600},
    {218,1058,862,505,297,580,532,1387,120,576,1409,473,11,1991,76,685},
    {78,760,982,528,445,1128,130,708,22,2144,354,1169,11,2782,58,1515},
    {330,107,1110,371,620,143,1014,1763,184,2068,1406,595,11,2639,33,1594},
    {336,1660,386,623,693,1079,891,1574,24,2641,1239,775,11,3104,55,2366},
};

typedef struct {
    float aAL[CC_KA+5], aBL[CC_KB+5], aCL[CC_KC+5], aDL[CC_KD+5];
    float aEL[CC_KE+5], aFL[CC_KF+5], aGL[CC_KG+5], aHL[CC_KH+5];
    float aIL[CC_KI+5], aJL[CC_KJ+5], aKL[CC_KK+5], aLL[CC_KL+5];
    float aML[CC_KM+5], aNL[CC_KN+5], aOL[CC_KO+5], aPL[CC_KP+5];

    float aAR[CC_KA+5], aBR[CC_KB+5], aCR[CC_KC+5], aDR[CC_KD+5];
    float aER[CC_KE+5], aFR[CC_KF+5], aGR[CC_KG+5], aHR[CC_KH+5];
    float aIR[CC_KI+5], aJR[CC_KJ+5], aKR[CC_KK+5], aLR[CC_KL+5];
    float aMR[CC_KM+5], aNR[CC_KN+5], aOR[CC_KO+5], aPR[CC_KP+5];

    float aZL[CREAMCOAT_PREDELAY+5], aZR[CREAMCOAT_PREDELAY+5];

    float feedbackAL, feedbackBL, feedbackCL, feedbackDL;
    float feedbackDR, feedbackHR, feedbackLR, feedbackPR;

    int countAL, countBL, countCL, countDL, countEL, countFL, countGL, countHL;
    int countIL, countJL, countKL, countLL, countML, countNL, countOL, countPL;
    int countAR, countBR, countCR, countDR, countER, countFR, countGR, countHR;
    int countIR, countJR, countKR, countLR, countMR, countNR, countOR, countPR;
    int countZ;

    int shortA, shortB, shortC, shortD, shortE, shortF, shortG, shortH;
    int shortI, shortJ, shortK, shortL, shortM, shortN, shortO, shortP;
    int prevclearcoat;

    // Bezier-curve tank-rate reconstruction state (ported from the original's bez[] array,
    // indices bez_AL..bez_cycle -- flattened into named fields here for readability, same
    // meaning/order otherwise).
    float bez_AL, bez_AR, bez_BL, bez_BR, bez_CL, bez_CR;
    float bez_InL, bez_InR, bez_UnInL, bez_UnInR, bez_SampL, bez_SampR;
    float bez_cycle;

    uint32_t fpdL, fpdR;
} FxCreamCoatState;

static void run_fx_creamcoat(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxCreamCoatState *st = (FxCreamCoatState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxCreamCoatState));
        engine->state = st;
        if (!st) return;
        st->countAL = st->countBL = st->countCL = st->countDL = 1;
        st->countEL = st->countFL = st->countGL = st->countHL = 1;
        st->countIL = st->countJL = st->countKL = st->countLL = 1;
        st->countML = st->countNL = st->countOL = st->countPL = 1;
        st->countAR = st->countBR = st->countCR = st->countDR = 1;
        st->countER = st->countFR = st->countGR = st->countHR = 1;
        st->countIR = st->countJR = st->countKR = st->countLR = 1;
        st->countMR = st->countNR = st->countOR = st->countPR = 1;
        st->countZ = 1;
        st->prevclearcoat = -1;
        // Matches the original constructor's bez[bez_cycle] = 1.0 (all other bez[] slots 0) --
        // combined with "+= derez" happening before the tick check every call, this guarantees
        // the very first sample processed triggers an initial tank tick.
        st->bez_cycle = 1.0f;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    float A = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Select
    float B = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Regen
    float C = fmaxf(0.0f, fminf(1.0f, params->param[2])); // DeRez
    float D = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Predelay
    float E = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Wetness

    float overallscale = (float)SAMPLE_RATE / 44100.0f;

    int clearcoat = (int)(A * 16.999f);
    if (clearcoat != st->prevclearcoat) {
        // Room preset changed live: clear the 16 delay lines' content and their L/R counters
        // (matching the original exactly) -- but deliberately NOT the feedback/bez/predelay
        // state, same scope the original resets, presumably so the transition isn't a hard mute.
        memset(st->aAL, 0, sizeof(st->aAL)); memset(st->aAR, 0, sizeof(st->aAR));
        memset(st->aBL, 0, sizeof(st->aBL)); memset(st->aBR, 0, sizeof(st->aBR));
        memset(st->aCL, 0, sizeof(st->aCL)); memset(st->aCR, 0, sizeof(st->aCR));
        memset(st->aDL, 0, sizeof(st->aDL)); memset(st->aDR, 0, sizeof(st->aDR));
        memset(st->aEL, 0, sizeof(st->aEL)); memset(st->aER, 0, sizeof(st->aER));
        memset(st->aFL, 0, sizeof(st->aFL)); memset(st->aFR, 0, sizeof(st->aFR));
        memset(st->aGL, 0, sizeof(st->aGL)); memset(st->aGR, 0, sizeof(st->aGR));
        memset(st->aHL, 0, sizeof(st->aHL)); memset(st->aHR, 0, sizeof(st->aHR));
        memset(st->aIL, 0, sizeof(st->aIL)); memset(st->aIR, 0, sizeof(st->aIR));
        memset(st->aJL, 0, sizeof(st->aJL)); memset(st->aJR, 0, sizeof(st->aJR));
        memset(st->aKL, 0, sizeof(st->aKL)); memset(st->aKR, 0, sizeof(st->aKR));
        memset(st->aLL, 0, sizeof(st->aLL)); memset(st->aLR, 0, sizeof(st->aLR));
        memset(st->aML, 0, sizeof(st->aML)); memset(st->aMR, 0, sizeof(st->aMR));
        memset(st->aNL, 0, sizeof(st->aNL)); memset(st->aNR, 0, sizeof(st->aNR));
        memset(st->aOL, 0, sizeof(st->aOL)); memset(st->aOR, 0, sizeof(st->aOR));
        memset(st->aPL, 0, sizeof(st->aPL)); memset(st->aPR, 0, sizeof(st->aPR));
        st->countAL = st->countBL = st->countCL = st->countDL = 1;
        st->countEL = st->countFL = st->countGL = st->countHL = 1;
        st->countIL = st->countJL = st->countKL = st->countLL = 1;
        st->countML = st->countNL = st->countOL = st->countPL = 1;
        st->countAR = st->countBR = st->countCR = st->countDR = 1;
        st->countER = st->countFR = st->countGR = st->countHR = 1;
        st->countIR = st->countJR = st->countKR = st->countLR = 1;
        st->countMR = st->countNR = st->countOR = st->countPR = 1;
        const int *preset = CREAMCOAT_PRESETS[clearcoat];
        st->shortA = preset[0];  st->shortB = preset[1];  st->shortC = preset[2];  st->shortD = preset[3];
        st->shortE = preset[4];  st->shortF = preset[5];  st->shortG = preset[6];  st->shortH = preset[7];
        st->shortI = preset[8];  st->shortJ = preset[9];  st->shortK = preset[10]; st->shortL = preset[11];
        st->shortM = preset[12]; st->shortN = preset[13]; st->shortO = preset[14]; st->shortP = preset[15];
        st->prevclearcoat = clearcoat;
    }

    float regen = (1.0f - powf(1.0f - B, 2.0f)) * 0.0625f;
    float derez = C / overallscale;
    if (derez < 0.0005f) derez = 0.0005f;
    if (derez > 1.0f) derez = 1.0f;
    derez = 1.0f / (float)((int)(1.0f / derez)); // hard-locks derez to exact subdivisions of 1.0
    int adjPredelay = (int)(CREAMCOAT_PREDELAY * D * derez);
    float wet = E * 2.0f;
    float dry = 2.0f - wet;
    if (wet > 1.0f) wet = 1.0f; else wet *= wet;
    if (wet < 0.0f) wet = 0.0f;
    if (dry > 1.0f) dry = 1.0f;
    if (dry < 0.0f) dry = 0.0f;
    // This reverb submixes at 50% full dry AND full wet, not crossfaded -- so it can sit on a
    // send/submix without cutting the dry signal back as Wetness is raised (see param[4] doc
    // above). Ported verbatim from the original's own comment.

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;
    float drySampleL = inputSampleL, drySampleR = inputSampleR;

    st->bez_cycle += derez;
    st->bez_SampL += ((inputSampleL + st->bez_InL) * derez);
    st->bez_SampR += ((inputSampleR + st->bez_InR) * derez);
    st->bez_InL = inputSampleL; st->bez_InR = inputSampleR;

    if (st->bez_cycle > 1.0f) { // hit the end point -- run one reverb-tank tick
        st->bez_cycle = 0.0f;

        // Predelay (operates at tank rate -- see adjPredelay's derivation above).
        st->aZL[st->countZ] = st->bez_SampL;
        st->aZR[st->countZ] = st->bez_SampR;
        st->countZ++; if (st->countZ < 0 || st->countZ > adjPredelay) st->countZ = 0;
        st->bez_SampL = st->aZL[st->countZ - ((st->countZ > adjPredelay) ? adjPredelay + 1 : 0)];
        st->bez_SampR = st->aZR[st->countZ - ((st->countZ > adjPredelay) ? adjPredelay + 1 : 0)];

        // Stage 0: input + feedback taps.
        st->aAL[st->countAL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackAL * regen);
        st->aBL[st->countBL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackBL * regen);
        st->aCL[st->countCL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackCL * regen);
        st->aDL[st->countDL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackDL * regen);
        st->bez_UnInL = st->bez_SampL;

        st->aDR[st->countDR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackDR * regen);
        st->aHR[st->countHR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackHR * regen);
        st->aLR[st->countLR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackLR * regen);
        st->aPR[st->countPR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackPR * regen);
        st->bez_UnInR = st->bez_SampR;

        st->countAL++; if (st->countAL < 0 || st->countAL > st->shortA) st->countAL = 0;
        st->countBL++; if (st->countBL < 0 || st->countBL > st->shortB) st->countBL = 0;
        st->countCL++; if (st->countCL < 0 || st->countCL > st->shortC) st->countCL = 0;
        st->countDL++; if (st->countDL < 0 || st->countDL > st->shortD) st->countDL = 0;

        st->countDR++; if (st->countDR < 0 || st->countDR > st->shortD) st->countDR = 0;
        st->countHR++; if (st->countHR < 0 || st->countHR > st->shortH) st->countHR = 0;
        st->countLR++; if (st->countLR < 0 || st->countLR > st->shortL) st->countLR = 0;
        st->countPR++; if (st->countPR < 0 || st->countPR > st->shortP) st->countPR = 0;

        float outAL = st->aAL[st->countAL - ((st->countAL > st->shortA) ? st->shortA + 1 : 0)];
        float outBL = st->aBL[st->countBL - ((st->countBL > st->shortB) ? st->shortB + 1 : 0)];
        float outCL = st->aCL[st->countCL - ((st->countCL > st->shortC) ? st->shortC + 1 : 0)];
        float outDL = st->aDL[st->countDL - ((st->countDL > st->shortD) ? st->shortD + 1 : 0)];

        float outDR = st->aDR[st->countDR - ((st->countDR > st->shortD) ? st->shortD + 1 : 0)];
        float outHR = st->aHR[st->countHR - ((st->countHR > st->shortH) ? st->shortH + 1 : 0)];
        float outLR = st->aLR[st->countLR - ((st->countLR > st->shortL) ? st->shortL + 1 : 0)];
        float outPR = st->aPR[st->countPR - ((st->countPR > st->shortP) ? st->shortP + 1 : 0)];

        // Stage 1: 4-way Householder reflection.
        st->aEL[st->countEL] = outAL - (outBL + outCL + outDL);
        st->aFL[st->countFL] = outBL - (outAL + outCL + outDL);
        st->aGL[st->countGL] = outCL - (outAL + outBL + outDL);
        st->aHL[st->countHL] = outDL - (outAL + outBL + outCL);

        st->aCR[st->countCR] = outDR - (outHR + outLR + outPR);
        st->aGR[st->countGR] = outHR - (outDR + outLR + outPR);
        st->aKR[st->countKR] = outLR - (outDR + outHR + outPR);
        st->aOR[st->countOR] = outPR - (outDR + outHR + outLR);

        st->countEL++; if (st->countEL < 0 || st->countEL > st->shortE) st->countEL = 0;
        st->countFL++; if (st->countFL < 0 || st->countFL > st->shortF) st->countFL = 0;
        st->countGL++; if (st->countGL < 0 || st->countGL > st->shortG) st->countGL = 0;
        st->countHL++; if (st->countHL < 0 || st->countHL > st->shortH) st->countHL = 0;

        st->countCR++; if (st->countCR < 0 || st->countCR > st->shortC) st->countCR = 0;
        st->countGR++; if (st->countGR < 0 || st->countGR > st->shortG) st->countGR = 0;
        st->countKR++; if (st->countKR < 0 || st->countKR > st->shortK) st->countKR = 0;
        st->countOR++; if (st->countOR < 0 || st->countOR > st->shortO) st->countOR = 0;

        float outEL = st->aEL[st->countEL - ((st->countEL > st->shortE) ? st->shortE + 1 : 0)];
        float outFL = st->aFL[st->countFL - ((st->countFL > st->shortF) ? st->shortF + 1 : 0)];
        float outGL = st->aGL[st->countGL - ((st->countGL > st->shortG) ? st->shortG + 1 : 0)];
        float outHL = st->aHL[st->countHL - ((st->countHL > st->shortH) ? st->shortH + 1 : 0)];

        float outCR = st->aCR[st->countCR - ((st->countCR > st->shortC) ? st->shortC + 1 : 0)];
        float outGR = st->aGR[st->countGR - ((st->countGR > st->shortG) ? st->shortG + 1 : 0)];
        float outKR = st->aKR[st->countKR - ((st->countKR > st->shortK) ? st->shortK + 1 : 0)];
        float outOR = st->aOR[st->countOR - ((st->countOR > st->shortO) ? st->shortO + 1 : 0)];

        // Stage 2.
        st->aIL[st->countIL] = outEL - (outFL + outGL + outHL);
        st->aJL[st->countJL] = outFL - (outEL + outGL + outHL);
        st->aKL[st->countKL] = outGL - (outEL + outFL + outHL);
        st->aLL[st->countLL] = outHL - (outEL + outFL + outGL);

        st->aBR[st->countBR] = outCR - (outGR + outKR + outOR);
        st->aFR[st->countFR] = outGR - (outCR + outKR + outOR);
        st->aJR[st->countJR] = outKR - (outCR + outGR + outOR);
        st->aNR[st->countNR] = outOR - (outCR + outGR + outKR);

        st->countIL++; if (st->countIL < 0 || st->countIL > st->shortI) st->countIL = 0;
        st->countJL++; if (st->countJL < 0 || st->countJL > st->shortJ) st->countJL = 0;
        st->countKL++; if (st->countKL < 0 || st->countKL > st->shortK) st->countKL = 0;
        st->countLL++; if (st->countLL < 0 || st->countLL > st->shortL) st->countLL = 0;

        st->countBR++; if (st->countBR < 0 || st->countBR > st->shortB) st->countBR = 0;
        st->countFR++; if (st->countFR < 0 || st->countFR > st->shortF) st->countFR = 0;
        st->countJR++; if (st->countJR < 0 || st->countJR > st->shortJ) st->countJR = 0;
        st->countNR++; if (st->countNR < 0 || st->countNR > st->shortN) st->countNR = 0;

        float outIL = st->aIL[st->countIL - ((st->countIL > st->shortI) ? st->shortI + 1 : 0)];
        float outJL = st->aJL[st->countJL - ((st->countJL > st->shortJ) ? st->shortJ + 1 : 0)];
        float outKL = st->aKL[st->countKL - ((st->countKL > st->shortK) ? st->shortK + 1 : 0)];
        float outLL = st->aLL[st->countLL - ((st->countLL > st->shortL) ? st->shortL + 1 : 0)];

        float outBR = st->aBR[st->countBR - ((st->countBR > st->shortB) ? st->shortB + 1 : 0)];
        float outFR = st->aFR[st->countFR - ((st->countFR > st->shortF) ? st->shortF + 1 : 0)];
        float outJR = st->aJR[st->countJR - ((st->countJR > st->shortJ) ? st->shortJ + 1 : 0)];
        float outNR = st->aNR[st->countNR - ((st->countNR > st->shortN) ? st->shortN + 1 : 0)];

        // Stage 3.
        st->aML[st->countML] = outIL - (outJL + outKL + outLL);
        st->aNL[st->countNL] = outJL - (outIL + outKL + outLL);
        st->aOL[st->countOL] = outKL - (outIL + outJL + outLL);
        st->aPL[st->countPL] = outLL - (outIL + outJL + outKL);

        st->aAR[st->countAR] = outBR - (outFR + outJR + outNR);
        st->aER[st->countER] = outFR - (outBR + outJR + outNR);
        st->aIR[st->countIR] = outJR - (outBR + outFR + outNR);
        st->aMR[st->countMR] = outNR - (outBR + outFR + outJR);

        st->countML++; if (st->countML < 0 || st->countML > st->shortM) st->countML = 0;
        st->countNL++; if (st->countNL < 0 || st->countNL > st->shortN) st->countNL = 0;
        st->countOL++; if (st->countOL < 0 || st->countOL > st->shortO) st->countOL = 0;
        st->countPL++; if (st->countPL < 0 || st->countPL > st->shortP) st->countPL = 0;

        st->countAR++; if (st->countAR < 0 || st->countAR > st->shortA) st->countAR = 0;
        st->countER++; if (st->countER < 0 || st->countER > st->shortE) st->countER = 0;
        st->countIR++; if (st->countIR < 0 || st->countIR > st->shortI) st->countIR = 0;
        st->countMR++; if (st->countMR < 0 || st->countMR > st->shortM) st->countMR = 0;

        float outML = st->aML[st->countML - ((st->countML > st->shortM) ? st->shortM + 1 : 0)];
        float outNL = st->aNL[st->countNL - ((st->countNL > st->shortN) ? st->shortN + 1 : 0)];
        float outOL = st->aOL[st->countOL - ((st->countOL > st->shortO) ? st->shortO + 1 : 0)];
        float outPL = st->aPL[st->countPL - ((st->countPL > st->shortP) ? st->shortP + 1 : 0)];

        float outAR = st->aAR[st->countAR - ((st->countAR > st->shortA) ? st->shortA + 1 : 0)];
        float outER = st->aER[st->countER - ((st->countER > st->shortE) ? st->shortE + 1 : 0)];
        float outIR = st->aIR[st->countIR - ((st->countIR > st->shortI) ? st->shortI + 1 : 0)];
        float outMR = st->aMR[st->countMR - ((st->countMR > st->shortM) ? st->shortM + 1 : 0)];

        // Final Householder stage -> next tick's feedback taps.
        st->feedbackAL = outML - (outNL + outOL + outPL);
        st->feedbackDR = outAR - (outER + outIR + outMR);
        st->feedbackBL = outNL - (outML + outOL + outPL);
        st->feedbackHR = outER - (outAR + outIR + outMR);
        st->feedbackCL = outOL - (outML + outNL + outPL);
        st->feedbackLR = outIR - (outAR + outER + outMR);
        st->feedbackDL = outPL - (outML + outNL + outOL);
        st->feedbackPR = outMR - (outAR + outER + outIR);

        // Final combined sum of outputs, corrected for Householder gain and averaging.
        inputSampleL = (outML + outNL + outOL + outPL) / 32.0f;
        inputSampleR = (outAR + outER + outIR + outMR) / 32.0f;

        st->bez_CL = st->bez_BL; st->bez_BL = st->bez_AL; st->bez_AL = inputSampleL; st->bez_SampL = 0.0f;
        st->bez_CR = st->bez_BR; st->bez_BR = st->bez_AR; st->bez_AR = inputSampleR; st->bez_SampR = 0.0f;
    }

    // 3-point bezier-style reconstruction of the tank's last few ticks into a full-rate signal.
    float CBL = (st->bez_CL * (1.0f - st->bez_cycle)) + (st->bez_BL * st->bez_cycle);
    float CBR = (st->bez_CR * (1.0f - st->bez_cycle)) + (st->bez_BR * st->bez_cycle);
    float BAL = (st->bez_BL * (1.0f - st->bez_cycle)) + (st->bez_AL * st->bez_cycle);
    float BAR = (st->bez_BR * (1.0f - st->bez_cycle)) + (st->bez_AR * st->bez_cycle);
    float CBAL = (st->bez_BL + (CBL * (1.0f - st->bez_cycle)) + (BAL * st->bez_cycle)) * 0.125f;
    float CBAR = (st->bez_BR + (CBR * (1.0f - st->bez_cycle)) + (BAR * st->bez_cycle)) * 0.125f;
    float outSampleL = CBAL, outSampleR = CBAR;

    outSampleL = fmaxf(-1.0f, fminf(1.0f, outSampleL));
    outSampleR = fmaxf(-1.0f, fminf(1.0f, outSampleR));

    if (wet < 1.0f) { outSampleL *= wet; outSampleR *= wet; }
    if (dry < 1.0f) { drySampleL *= dry; drySampleR *= dry; }
    outSampleL += drySampleL;
    outSampleR += drySampleR;

    int expon;
    frexpf(outSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    outSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(outSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    outSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = outSampleL;
    *r = outSampleR;
}

// --- FX_TYPE_DENSITY3: ported from Airwindows Density3 (MIT license) ---
// Highpass (a leaky-integrator lowpass subtracted from the input) feeding a density/waveshaping
// saturator. `density` (0..5, from param[0]*5) selects between two Taylor-series curves: above
// 1.0, an increasingly hard sine-based clip (the higher density goes, the more the input is
// pre-scaled before the sine approximation, so it clips harder); below 1.0, a softer
// arcsine-family curve; density itself blends smoothly through 1.0 (a pure highpass, no added
// coloration). This is a direct per-sample port of the original's processReplacing (the
// float-precision path, matching this engine's own float sample pipeline) -- only VST host
// plumbing (chunks, program names, canDo) was dropped, along with the original's OWN internal
// Dry/Wet param (see the FX_TYPE_DENSITY3 comment on why).
typedef struct {
    float iirSampleL, iirSampleR;
    uint32_t fpdL, fpdR;
} FxDensity3State;

// Shared waveshaping core for one channel, factored out of the original's copy-pasted L/R
// blocks (both channels ran byte-for-byte identical math on their own sample). `x` is the
// signal AFTER the highpass subtraction; returns the same density-blended result the original
// computed inline.
static inline float density3_waveshape_sample(float x, float density) {
    float altered = x;
    if (density > 1.0f) {
        float half_pi = (float)(M_PI * 0.5);
        altered = fmaxf(fminf(x * density * half_pi, half_pi), -half_pi);
        float xx = altered * altered;
        float temp = altered * xx;
        altered -= temp / 6.0f;      temp *= xx;
        altered += temp / 120.0f;    temp *= xx;
        altered -= temp / 5040.0f;   temp *= xx;
        altered += temp / 362880.0f; temp *= xx;
        altered -= temp / 39916800.0f;
    }
    if (density < 1.0f) {
        float clamped = fmaxf(fminf(x, 1.0f), -1.0f);
        float polarity = clamped;
        float xx = x * clamped;
        float temp = xx;
        altered = temp / 2.0f;       temp *= xx;
        altered -= temp / 24.0f;     temp *= xx;
        altered += temp / 720.0f;    temp *= xx;
        altered -= temp / 40320.0f;  temp *= xx;
        altered += temp / 3628800.0f;
        altered *= (polarity < 0.0f) ? -1.0f : 1.0f;
    }
    if (density > 2.0f) return altered;
    float blend = fabsf(density - 1.0f);
    return x * (1.0f - blend) + altered * blend;
}

static void run_fx_density3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDensity3State *st = (FxDensity3State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDensity3State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0] Density, param[1] Highpass, param[2] Output -- all 0..1 (see the FX_TYPE_DENSITY3
    // comment above for what each becomes internally).
    float density = fmaxf(0.0f, fminf(1.0f, params->param[0])) * 5.0f;
    float highpass = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float output = fmaxf(0.0f, fminf(1.0f, params->param[2]));

    // overallscale/iirAmount match the original exactly: the highpass's leaky-integrator
    // coefficient is scaled so its character stays consistent across sample rates, referenced
    // against 44100Hz. highpass^3 (not highpass itself) gives most of the knob's travel to the
    // low end of its range, where the audible difference per turn is largest.
    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float iir_amount = (highpass * highpass * highpass) / overallscale;
    // The original resets its leaky-integrator state to silence once per processReplacing call
    // whenever Highpass is fully off, so a stale nonzero iir sample can never leak back in as a
    // DC-ish offset if Highpass is turned back up later. Called once per sample here rather than
    // once per host buffer, but functionally identical: once iir_amount is 0, this keeps forcing
    // the state to 0 every sample, same end state as the original's once-per-buffer reset.
    if (iir_amount <= 0.0f) { st->iirSampleL = 0.0f; st->iirSampleR = 0.0f; }

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->iirSampleL = st->iirSampleL * (1.0f - iir_amount) + inputSampleL * iir_amount;
    inputSampleL -= st->iirSampleL;
    st->iirSampleR = st->iirSampleR * (1.0f - iir_amount) + inputSampleR * iir_amount;
    inputSampleR -= st->iirSampleR;

    inputSampleL = density3_waveshape_sample(inputSampleL, density);
    inputSampleR = density3_waveshape_sample(inputSampleR, density);

    inputSampleL *= output;
    inputSampleR *= output;

    // 32-bit-float dither, scaled relative to each sample's own magnitude (frexpf's exponent) --
    // same convention (and constant, 5.5e-36f) already used by run_fx_ensemble/run_fx_creamcoat
    // above, rather than the original plugin's own literal (Airwindows' per-plugin dither
    // constants vary release to release; this codebase standardizes on one).
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

// --- FX_TYPE_STEREOCHORUS: ported from Airwindows StereoChorus (MIT license) ---
// Two independent LFO-swept delay reads (L and R start their sweep phase at different points --
// pi/2.7 vs pi -- so they never modulate in lockstep, widening the stereo image) into a single
// ring buffer quantized to roughly 24-bit integer resolution (the original stores samples as
// `int`, not float/double -- an intentional part of this effect's character, ported as-is), plus
// an "air" comb-filter stage that restores some of the top end a short modulated delay's linear
// interpolation naturally smears away.
typedef struct {
    int pL[65536], pR[65536];
    float sweepL, sweepR;
    int gcount;
    float airPrevL, airEvenL, airOddL, airFactorL;
    float airPrevR, airEvenR, airOddR, airFactorR;
    bool flip;
    float lastRefL[7], lastRefR[7];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxStereoChorusState;

static void run_fx_stereochorus(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxStereoChorusState *st = (FxStereoChorusState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxStereoChorusState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->sweepL = (float)(M_PI / 2.7);
        st->sweepR = (float)M_PI;
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    // param[0] Speed, param[1] Depth -- both 0..1 (see the FX_TYPE_STEREOCHORUS comment above).
    // This exact speed/depth curve is ported straight from the original: speed stays very small
    // across the whole knob range (a tenth-power curve), so depth (which divides by speed)
    // naturally gets large at low Speed settings -- that's intentional/original behavior, not a
    // bug, and stays safely within pL/pR's bounds (see the defensive clamp below regardless).
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float speed = powf(0.32f + (a / 6.0f), 10.0f);
    float depth = (b / 60.0f) / speed;
    float tupi = (float)(M_PI * 2.0);

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        st->airFactorL = st->airPrevL - inputSampleL;
        if (st->flip) { st->airEvenL += st->airFactorL; st->airOddL -= st->airFactorL; st->airFactorL = st->airEvenL; }
        else { st->airOddL += st->airFactorL; st->airEvenL -= st->airFactorL; st->airFactorL = st->airOddL; }
        st->airOddL = (st->airOddL - ((st->airOddL - st->airEvenL) / 256.0f)) / 1.0001f;
        st->airEvenL = (st->airEvenL - ((st->airEvenL - st->airOddL) / 256.0f)) / 1.0001f;
        st->airPrevL = inputSampleL;
        inputSampleL += st->airFactorL;

        st->airFactorR = st->airPrevR - inputSampleR;
        if (st->flip) { st->airEvenR += st->airFactorR; st->airOddR -= st->airFactorR; st->airFactorR = st->airEvenR; }
        else { st->airOddR += st->airFactorR; st->airEvenR -= st->airFactorR; st->airFactorR = st->airOddR; }
        st->airOddR = (st->airOddR - ((st->airOddR - st->airEvenR) / 256.0f)) / 1.0001f;
        st->airEvenR = (st->airEvenR - ((st->airEvenR - st->airOddR) / 256.0f)) / 1.0001f;
        st->airPrevR = inputSampleR;
        inputSampleR += st->airFactorR;
        st->flip = !st->flip;

        int tempL = 0, tempR = 0;
        if (st->gcount < 1 || st->gcount > 32760) st->gcount = 32760;

        int count = st->gcount;
        st->pL[count + 32760] = st->pL[count] = (int)(inputSampleL * 8388352.0f);
        float offsetL = depth + (depth * sinf(st->sweepL));
        count += (int)floorf(offsetL);
        if (count < 0) count = 0;
        if (count > 65533) count = 65533; // defensive: keep well inside pL[]/pR[]
        tempL += (int)((float)st->pL[count] * (1.0f - (offsetL - floorf(offsetL))));
        tempL += st->pL[count + 1];
        tempL += (int)((float)st->pL[count + 2] * (offsetL - floorf(offsetL)));
        tempL -= (int)(((float)(st->pL[count] - st->pL[count + 1]) - (float)(st->pL[count + 1] - st->pL[count + 2])) / 50.0f);

        count = st->gcount;
        st->pR[count + 32760] = st->pR[count] = (int)(inputSampleR * 8388352.0f);
        float offsetR = depth + (depth * sinf(st->sweepR));
        count += (int)floorf(offsetR);
        if (count < 0) count = 0;
        if (count > 65533) count = 65533;
        tempR += (int)((float)st->pR[count] * (1.0f - (offsetR - floorf(offsetR))));
        tempR += st->pR[count + 1];
        tempR += (int)((float)st->pR[count + 2] * (offsetR - floorf(offsetR)));
        tempR -= (int)(((float)(st->pR[count] - st->pR[count + 1]) - (float)(st->pR[count + 1] - st->pR[count + 2])) / 50.0f);

        st->sweepL += speed; st->sweepR += speed;
        if (st->sweepL > tupi) st->sweepL -= tupi;
        if (st->sweepR > tupi) st->sweepR -= tupi;
        st->gcount--;

        inputSampleL = (float)tempL / 16776704.0f;
        inputSampleR = (float)tempR / 16776704.0f;

        // Downsampling-ladder reconstruction -- see run_fx_ensemble's identical pattern/comment.
        // At this engine's fixed 48kHz, cycle_end is always 1, so this degenerates to
        // lastRefL[0]=inputSampleL every call; kept generic in case SAMPLE_RATE ever changes.
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

// --- FX_TYPE_STEREODOUBLER: ported from Airwindows StereoDoubler (MIT license) ---
// A granular micro-pitch detuner. 8 simultaneous delay-read "voices" per channel walk through a
// ring buffer at slightly different speeds (L and R detuned in opposite directions from Detune),
// each voice wrapping back to the buffer start at a different moment; whenever one wraps, this
// crossfade-splices to whichever OTHER voice is currently closest in value (`bestsplice`) rather
// than jumping straight back, hiding the seam that would otherwise click. The "air" comb-filter
// stage and downsampling-ladder reconstruction are the same techniques used in
// run_fx_stereochorus above.
typedef struct {
    float pL[5001], pR[5001];
    int gcountL, lastcountL, gcountR, lastcountR;
    float trackingL[8], tempL[8], positionL[8], lastpositionL[8];
    float trackingR[8], tempR[8], positionR[8], lastpositionR[8];
    int activeL, bestspliceL, activeR, bestspliceR;
    float airPrevL, airEvenL, airOddL, airFactorL;
    float airPrevR, airEvenR, airOddR, airFactorR;
    bool flip;
    float lastRefL[7], lastRefR[7];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxStereoDoublerState;

// This effect's `width` (the ring buffer span the 8 voices walk around) is a fixed constant in
// the original -- never actually configurable at runtime despite the original tracking a
// "did width change" flag for it -- so its one-time position-ladder setup happens here, at
// state-allocation time, instead of being checked on every call.
#define STEREODOUBLER_WIDTH 2300

static void run_fx_stereodoubler(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxStereoDoublerState *st = (FxStereoDoublerState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxStereoDoublerState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        int w = STEREODOUBLER_WIDTH;
        st->positionL[0] = 0; st->positionL[1] = (float)(w / 3); st->positionL[2] = (float)((w / 3) * 2);
        st->positionL[3] = (float)(w / 5); st->positionL[4] = (float)((w / 5) * 2); st->positionL[5] = (float)((w / 5) * 3);
        st->positionL[6] = (float)((w / 5) * 4); st->positionL[7] = (float)(w / 2);
        st->positionR[0] = 0; st->positionR[1] = (float)(w / 3); st->positionR[2] = (float)((w / 3) * 2);
        st->positionR[3] = (float)(w / 5); st->positionR[4] = (float)((w / 5) * 2); st->positionR[5] = (float)((w / 5) * 3);
        st->positionR[6] = (float)((w / 5) * 4); st->positionR[7] = (float)(w / 2);
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;
    int width = STEREODOUBLER_WIDTH;

    // param[0] Detune, param[1] Dry/Wet -- both 0..1 (see the FX_TYPE_STEREODOUBLER comment
    // above for why Dry/Wet stays its own control here, unlike Density3's).
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float wet = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float trim = (a * 2.0f) - 1.0f;
    trim *= fabsf(trim);
    trim /= 40.0f;
    float speedL = trim + 1.0f;
    float speedR = (-trim) + 1.0f;
    if (speedL < 0.0f) speedL = 0.0f;
    if (speedR < 0.0f) speedR = 0.0f;
    float adjust = 1100.0f;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float drySampleL = inputSampleL, drySampleR = inputSampleR;

        st->airFactorL = st->airPrevL - inputSampleL;
        if (st->flip) { st->airEvenL += st->airFactorL; st->airOddL -= st->airFactorL; st->airFactorL = st->airEvenL; }
        else { st->airOddL += st->airFactorL; st->airEvenL -= st->airFactorL; st->airFactorL = st->airOddL; }
        st->airOddL = (st->airOddL - ((st->airOddL - st->airEvenL) / 256.0f)) / 1.0001f;
        st->airEvenL = (st->airEvenL - ((st->airEvenL - st->airOddL) / 256.0f)) / 1.0001f;
        st->airPrevL = inputSampleL;
        inputSampleL += st->airFactorL;

        st->airFactorR = st->airPrevR - inputSampleR;
        if (st->flip) { st->airEvenR += st->airFactorR; st->airOddR -= st->airFactorR; st->airFactorR = st->airEvenR; }
        else { st->airOddR += st->airFactorR; st->airEvenR -= st->airFactorR; st->airFactorR = st->airOddR; }
        st->airOddR = (st->airOddR - ((st->airOddR - st->airEvenR) / 256.0f)) / 1.0001f;
        st->airEvenR = (st->airEvenR - ((st->airEvenR - st->airOddR) / 256.0f)) / 1.0001f;
        st->airPrevR = inputSampleR;
        inputSampleR += st->airFactorR;
        st->flip = !st->flip;

        st->gcountL++; st->gcountR++;
        for (int c = 0; c < 8; c++) { st->positionL[c] += speedL; st->positionR[c] += speedR; }

        int gplusL = st->gcountL + (int)adjust;
        int lastplusL = st->lastcountL + (int)adjust;
        if (gplusL > width) gplusL -= width;
        if (lastplusL > width) lastplusL -= width;

        int gplusR = st->gcountR + (int)adjust;
        int lastplusR = st->lastcountR + (int)adjust;
        if (gplusR > width) gplusR -= width;
        if (lastplusR > width) lastplusR -= width;

        if (st->trackingL[st->activeL] == 0.0f) {
            float posplusL = st->positionL[st->activeL] + adjust;
            float lastposplusL = st->lastpositionL[st->activeL] + adjust;
            if (posplusL > (float)width) posplusL -= (float)width;
            if (lastposplusL > (float)width) lastposplusL -= (float)width;
            if ((gplusL > st->positionL[st->activeL]) && (lastplusL < st->lastpositionL[st->activeL])) st->trackingL[st->activeL] = 1.0f;
            if ((posplusL > (float)st->gcountL) && (lastposplusL < (float)st->lastcountL)) st->trackingL[st->activeL] = 1.0f;
        }
        if (st->trackingR[st->activeR] == 0.0f) {
            float posplusR = st->positionR[st->activeR] + adjust;
            float lastposplusR = st->lastpositionR[st->activeR] + adjust;
            if (posplusR > (float)width) posplusR -= (float)width;
            if (lastposplusR > (float)width) lastposplusR -= (float)width;
            if ((gplusR > st->positionR[st->activeR]) && (lastplusR < st->lastpositionR[st->activeR])) st->trackingR[st->activeR] = 1.0f;
            if ((posplusR > (float)st->gcountR) && (lastposplusR < (float)st->lastcountR)) st->trackingR[st->activeR] = 1.0f;
        }

        for (int c = 0; c < 8; c++) {
            if (st->positionL[c] > (float)width) st->positionL[c] -= (float)width;
            if (st->positionR[c] > (float)width) st->positionR[c] -= (float)width;
            st->lastpositionL[c] = st->positionL[c];
            st->lastpositionR[c] = st->positionR[c];
        }
        if (st->gcountL < 0 || st->gcountL > width) st->gcountL -= width;
        st->lastcountL = st->gcountL;
        if (st->gcountR < 0 || st->gcountR > width) st->gcountR -= width;
        st->lastcountR = st->gcountR;
        // bcountL/bcountR are provably bounded to [0, width] by the wrap-check just above
        // (gcountL only ever grows by exactly 1 per cycle, caught and pulled back by `width` the
        // moment it exceeds it), so bcountL+width stays safely inside pL[5001] on its own; this
        // clamp is a true last-resort backstop only (matching the original's own array-size
        // headroom), never expected to actually trigger in normal operation.
        int bcountL = st->gcountL, bcountR = st->gcountR;
        if (bcountL < 0) bcountL = 0;
        if (bcountL > width) bcountL = width;
        if (bcountR < 0) bcountR = 0;
        if (bcountR > width) bcountR = width;

        st->pL[bcountL + width] = st->pL[bcountL] = inputSampleL;
        st->pR[bcountR + width] = st->pR[bcountR] = inputSampleR;

        for (int c = 0; c < 8; c++) {
            // positionL[c]/positionR[c] are wrapped into [0, width] just above too, so base+2
            // naturally stays well inside pL[5001]'s 2*width+401 headroom -- same backstop-only
            // rationale as bcountL/bcountR.
            int base = (int)floorf(st->positionL[c]);
            if (base < 0) base = 0;
            if (base > 4998) base = 4998;
            st->tempL[c] = st->pL[base] * (1.0f - (st->positionL[c] - (float)base));
            st->tempL[c] += st->pL[base + 1];
            st->tempL[c] += st->pL[base + 2] * (st->positionL[c] - (float)base);
            st->tempL[c] -= ((st->pL[base] - st->pL[base + 1]) - (st->pL[base + 1] - st->pL[base + 2])) / 50.0f;
            st->tempL[c] /= 2.0f;

            base = (int)floorf(st->positionR[c]);
            if (base < 0) base = 0;
            if (base > 4998) base = 4998;
            st->tempR[c] = st->pR[base] * (1.0f - (st->positionR[c] - (float)base));
            st->tempR[c] += st->pR[base + 1];
            st->tempR[c] += st->pR[base + 2] * (st->positionR[c] - (float)base);
            st->tempR[c] -= ((st->pR[base] - st->pR[base + 1]) - (st->pR[base + 1] - st->pR[base + 2])) / 50.0f;
            st->tempR[c] /= 2.0f;
        }

        if (st->trackingL[st->activeL] > 0.0f) {
            float crossfade = sinf(st->trackingL[st->bestspliceL] * 1.57f);
            inputSampleL = (st->tempL[st->activeL] * crossfade) + (st->tempL[st->bestspliceL] * (1.0f - crossfade));
            for (int c = 0; c < 8; c++) {
                float depthv = 0.5f - fabsf(st->tempL[st->activeL] - st->tempL[c]);
                if (depthv > 0.0f && c != st->activeL) { st->trackingL[c] -= (depthv / adjust); st->bestspliceL = c; }
            }
            float bestyetL = 1.0f;
            for (int c = 0; c < 8; c++) if (st->trackingL[c] < bestyetL && c != st->activeL) { st->bestspliceL = c; bestyetL = st->trackingL[c]; }
            if (st->trackingL[st->bestspliceL] < 0.0f) {
                for (int c = 0; c < 8; c++) st->trackingL[c] = 1.0f;
                st->activeL = st->bestspliceL;
                st->trackingL[st->activeL] = 0.0f;
            }
        } else {
            inputSampleL = st->tempL[st->activeL];
        }

        if (st->trackingR[st->activeR] > 0.0f) {
            float crossfade = sinf(st->trackingR[st->bestspliceR] * 1.57f);
            inputSampleR = (st->tempR[st->activeR] * crossfade) + (st->tempR[st->bestspliceR] * (1.0f - crossfade));
            for (int c = 0; c < 8; c++) {
                float depthv = 0.5f - fabsf(st->tempR[st->activeR] - st->tempR[c]);
                if (depthv > 0.0f && c != st->activeR) { st->trackingR[c] -= (depthv / adjust); st->bestspliceR = c; }
            }
            float bestyetR = 1.0f;
            for (int c = 0; c < 8; c++) if (st->trackingR[c] < bestyetR && c != st->activeR) { st->bestspliceR = c; bestyetR = st->trackingR[c]; }
            if (st->trackingR[st->bestspliceR] < 0.0f) {
                for (int c = 0; c < 8; c++) st->trackingR[c] = 1.0f;
                st->activeR = st->bestspliceR;
                st->trackingR[st->activeR] = 0.0f;
            }
        } else {
            inputSampleR = st->tempR[st->activeR];
        }

        inputSampleL = (inputSampleL * wet) + (drySampleL * (1.0f - wet));
        inputSampleR = (inputSampleR * wet) + (drySampleR * (1.0f - wet));

        // Downsampling-ladder reconstruction -- see run_fx_ensemble's identical pattern/comment.
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

// Runs the FX_SLOT_COUNT-slot FX bus in place on one stereo sample. Called once per sample
// (not per voice) on the fully summed mix -- see render_audio_block, where this sits after all
// voices (and the external input) have been added together, and before master_volume is
// applied. Ported effects land as new cases in the inner switch; each one reads/writes its own
// fx_engine[s].state (lazily allocating it on first call after a type change) and is
// responsible for freeing it if it allocated anything, via a matching branch in the "type
// changed" block below. Until effects are ported in, every fx_type other than Off is unreached
// and simply passes the dry signal through unchanged, same as Off.
//
// routing_mode selects how source_id's OWN assigned slots combine (see fx_source_routing_mode's
// comment on SynthSharedControls): 0 = Series, each of this source's slots fed the previous one's
// output (in physical slot-array order -- a slot not assigned to this source is simply skipped,
// so Series order among a source's own slots is still exactly array order), *l/*r updated in
// place after every owned slot. 1 = Parallel, every one of this source's slots fed the SAME
// original dry input independently (an Off/bypassed slot in Parallel contributes its own
// unprocessed dry copy, same convention as run_filter_a_multi's Parallel routing), all of this
// source's results summed and averaged into *l/*r at the end -- averaged over however many slots
// THIS source actually owns, not FX_SLOT_COUNT, so a source using only 1 or 2 slots in Parallel
// isn't attenuated as if all 12 had participated. A source that owns zero slots (Series or
// Parallel alike) is a correct no-op: *l/*r pass through unchanged.
//
// Called once per source per sample (not per voice) -- see render_audio_block, which loops this
// over every source (Program + each ext-in pair) on that source's own dry bus, after all voices
// have been summed (for the Program bus) and before that source's contribution is added into
// whichever output pair(s) it's routed to. fx_engine[] stays indexed by physical slot id
// regardless of which source owns a slot this block, so a slot's lazily-allocated DSP state
// (e.g. a delay's buffered tail) survives a live source reassignment exactly like it already
// survives a live bypass toggle.
// Type-change bookkeeping (free a slot's old effect state when its Type changed) -- a per-
// PHYSICAL-SLOT task, completely independent of which source is dispatching. Used to run inside
// process_fx_chain() itself, once per call; now a separate, explicitly-named step the CALLER runs
// once per block (see render_audio_block, right before its per-sample loop), since a slot's Type
// can only ever change in response to an OSC message drained between blocks, never mid-block --
// with SOURCE_COUNT sources each independently calling process_fx_chain() every single sample,
// redoing this scan from inside that function meant redoing it SOURCE_COUNT times per sample for
// no reason (a real, measured underrun regression at any voice count, since it's a flat per-
// sample tax paid regardless of patch complexity). PRECONDITION for process_fx_chain() below:
// this must be called at least once after any fx_type change and before that slot is dispatched --
// render_audio_block satisfies this once per block; a test or other direct caller driving
// process_fx_chain() outside render_audio_block must call this itself first.
static void sync_fx_engine_types(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT]) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        int type = (int)fx_params[s].fx_type;
        if (type != fx_engine[s].active_type) {
            free(fx_engine[s].state);
            fx_engine[s].state = NULL;
            fx_engine[s].active_type = type;
        }
    }
}

static void process_fx_chain(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT], int routing_mode, int source_id, float *l, float *r) {
    float parallel_in_l = *l, parallel_in_r = *r;
    float parallel_sum_l = 0.0f, parallel_sum_r = 0.0f;
    int parallel_slot_count = 0;

    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        if ((int)fx_params[s].fx_source != source_id) continue; // not this source's slot
        int type = fx_engine[s].active_type;
        if (routing_mode == 1) parallel_slot_count++;

        float dry_l = (routing_mode == 0) ? *l : parallel_in_l;
        float dry_r = (routing_mode == 0) ? *r : parallel_in_r;

        if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            if (routing_mode == 1) { parallel_sum_l += dry_l; parallel_sum_r += dry_r; }
            continue; // Series: *l/*r simply untouched by this slot
        }

        float wet_l = dry_l, wet_r = dry_r;
        switch (type) {
            case FX_TYPE_ENSEMBLE: run_fx_ensemble(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_CREAMCOAT: run_fx_creamcoat(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_DENSITY3: run_fx_density3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_STEREOCHORUS: run_fx_stereochorus(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_STEREODOUBLER: run_fx_stereodoubler(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
            case FX_TYPE_OFF:
            default:
                break; // pass-through
        }
        float mix = fmaxf(0.0f, fminf(100.0f, fx_params[s].mix)) / 100.0f;
        float out_l = dry_l * (1.0f - mix) + wet_l * mix;
        float out_r = dry_r * (1.0f - mix) + wet_r * mix;

        if (routing_mode == 1) { parallel_sum_l += out_l; parallel_sum_r += out_r; continue; }
        *l = out_l;
        *r = out_r;
    }

    if (routing_mode == 1 && parallel_slot_count > 0) {
        *l = parallel_sum_l / (float)parallel_slot_count;
        *r = parallel_sum_r / (float)parallel_slot_count;
    }
    // routing_mode == 1 with parallel_slot_count == 0: *l/*r already untouched (still the
    // original dry input), which is the correct "this source owns no slots" passthrough.
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
// ============================================================================
// OSC Control Protocol
// ============================================================================
// Replaces the old raw-memcpy-of-a-486-float-blob Unix-socket protocol with OSC (Open Sound
// Control, via liblo): every parameter is addressed by name (e.g. "/hds/vco1/shape"), not by
// byte position. This eliminates an entire bug class structurally rather than by hand-kept
// discipline -- a misplaced/mis-ordered field can no longer silently corrupt unrelated data the
// way record_format's wrong struct position once did (it landed 4 bytes into the string region
// and collapsed record_path to an empty string on every recording attempt). Every table entry
// below uses offsetof(SynthSharedControls, ...) directly, so a typo'd field name is a COMPILE
// ERROR, not a silent runtime miswrite -- the core safety property this redesign is for.
//
// Transport: plain UDP via a single non-threaded lo_server, polled once per audio block from
// main()'s loop (see the "while (lo_server_recv_noblock(...))" call site) -- deliberately NOT
// lo_server_thread, preserving the existing invariant that only main()'s own thread ever touches
// active_knobs/g_sample_slots, no new locking required. Incoming control listens on
// --osc-listen-port (default 9000); outgoing status (see send_osc_status()) goes to
// --osc-reply-host/--osc-reply-port (default 127.0.0.1:9001) -- two independent, explicitly
// configured ports, matching how real OSC tools (TouchOSC, Max, Pd, python-osc) already expect
// to be wired up, rather than "reply to whoever last sent a message."

typedef enum { OSC_KIND_FLOAT, OSC_KIND_STRING } OscParamKind;

// One row = one address template. A plain scalar has num_indices=0 (address matched verbatim).
// A single-index array (e.g. one LFO property across all LFO_COUNT LFOs) has num_indices=1:
// `address` contains one "%d", `stride` is sizeof(one array element), `count` is the valid
// index range. The one genuinely nested case (FX slot param banks, fx[FX_SLOT_COUNT].
// param[FX_SLOT_PARAM_COUNT]) has num_indices=2: `address` contains two "%d"s, `stride`/`count`
// index the outer array, `stride2`/`count2` index the inner one.
typedef struct {
    const char *address;
    size_t base_offset;
    size_t stride;
    int count;
    int num_indices;
    size_t stride2;
    int count2;
    OscParamKind kind;
    size_t str_len; // OSC_KIND_STRING only: size of the fixed char buffer, for safe truncation
} OscParamEntry;

#define OSC_SCALAR(addr, field) \
    { addr, offsetof(SynthSharedControls, field), 0, 1, 0, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_ARRAY(addr, field, cnt) \
    { addr, offsetof(SynthSharedControls, field), sizeof(((SynthSharedControls *)0)->field[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_SUBARRAY(addr, arrfield, subtype, subfield, cnt) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_NESTED(addr, arrfield, subtype, subfield, cnt1, cnt2) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt1, 2, sizeof(float), cnt2, OSC_KIND_FLOAT, 0 }
#define OSC_STRING(addr, field, len) \
    { addr, offsetof(SynthSharedControls, field), 0, 1, 0, 0, 0, OSC_KIND_STRING, len }

static const OscParamEntry OSC_PARAMS[] = {
    // --- Baseline ---
    OSC_SCALAR("/hds/is_polyphonic", is_polyphonic),
    OSC_SCALAR("/hds/vco1/shape", vco1_shape),
    OSC_SCALAR("/hds/vco2/shape", vco2_shape),
    OSC_SCALAR("/hds/fm/intensity", fm_intensity),
    OSC_SCALAR("/hds/vco1/vol", vco1_vol),
    OSC_SCALAR("/hds/vco2/vol", vco2_vol),
    OSC_SCALAR("/hds/portamento_time_ms", portamento_time_ms),
    OSC_SCALAR("/hds/master_volume", master_volume),
    OSC_SCALAR("/hds/vco1/pan", vco1_pan),
    OSC_SCALAR("/hds/vco2/pan", vco2_pan),
    OSC_SCALAR("/hds/vco1/fine_tune", vco1_fine_tune),
    OSC_SCALAR("/hds/vco2/pitch_offset", vco2_pitch_offset),
    OSC_SCALAR("/hds/vco2/fine_tune", vco2_fine_tune),

    // --- Envelopes (eg[ENV_COUNT], PackEGParams: d=delay h=hold a=attack dec=decay s=sustain
    // r=release al=attack_start_pct pk=attack_peak_pct -- see tick_adsr()) ---
    OSC_SUBARRAY("/hds/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/release", eg, PackEGParams, r, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT),
    OSC_SUBARRAY("/hds/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT),
    OSC_ARRAY("/hds/eg/%d/topology_mode", eg_mode, ENV_COUNT),

    // --- Filter A/B ---
    OSC_SCALAR("/hds/filter/ab_routing_mode", filter_ab_routing_mode),
    OSC_SCALAR("/hds/filter/ab_blend", filter_ab_blend),
    OSC_SCALAR("/hds/filter/a/mode", filter_a_mode),
    OSC_SCALAR("/hds/filter/a/cutoff", filter_a.cutoff),
    OSC_SCALAR("/hds/filter/a/res_q", filter_a.res_q),
    OSC_SCALAR("/hds/filter/a/slope", filter_a.slope_poles),
    OSC_SCALAR("/hds/filter/a/type", filter_a.filter_type),
    OSC_SCALAR("/hds/filter/a_multi/routing", filter_a_multi_routing),
    OSC_SUBARRAY("/hds/filter/a_multi/%d/cutoff", filter_a_multi, FilterParams, cutoff, 3),
    OSC_SUBARRAY("/hds/filter/a_multi/%d/res_q", filter_a_multi, FilterParams, res_q, 3),
    OSC_SUBARRAY("/hds/filter/a_multi/%d/slope", filter_a_multi, FilterParams, slope_poles, 3),
    OSC_SUBARRAY("/hds/filter/a_multi/%d/type", filter_a_multi, FilterParams, filter_type, 3),
    OSC_SCALAR("/hds/filter/b/cutoff", filter_b.cutoff),
    OSC_SCALAR("/hds/filter/b/res_q", filter_b.res_q),
    OSC_SCALAR("/hds/filter/b/slope", filter_b.slope_poles),
    OSC_SCALAR("/hds/filter/b/type", filter_b.filter_type),

    // --- LFOs (lfo[LFO_COUNT], PackLfoParams) ---
    OSC_SUBARRAY("/hds/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT),
    OSC_SUBARRAY("/hds/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT),

    // --- Assignable EG modulators ---
    OSC_ARRAY("/hds/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT),
    OSC_ARRAY("/hds/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT),

    // --- Unison Saw ---
    OSC_SCALAR("/hds/vco1/unison/detune", vco1_unison_saw_detune),
    OSC_SCALAR("/hds/vco1/unison/width", vco1_unison_saw_width),
    OSC_SCALAR("/hds/vco2/unison/detune", vco2_unison_saw_detune),
    OSC_SCALAR("/hds/vco2/unison/width", vco2_unison_saw_width),
    OSC_SCALAR("/hds/vco3/unison/detune", vco3_unison_saw_detune),
    OSC_SCALAR("/hds/vco3/unison/width", vco3_unison_saw_width),
    OSC_SCALAR("/hds/vco4/unison/detune", vco4_unison_saw_detune),
    OSC_SCALAR("/hds/vco4/unison/width", vco4_unison_saw_width),

    OSC_SCALAR("/hds/mono_glide_random", mono_glide_random),
    OSC_SCALAR("/hds/vco1/mute", vco1_mute),
    OSC_SCALAR("/hds/vco2/mute", vco2_mute),
    OSC_SCALAR("/hds/hard_sync_mode", hard_sync_mode),
    OSC_SCALAR("/hds/vco1/phase_offset", vco1_phase_offset),
    OSC_SCALAR("/hds/vco2/phase_offset", vco2_phase_offset),
    OSC_SCALAR("/hds/vco1/pulse_width", vco1_pulse_width),
    OSC_SCALAR("/hds/vco2/pulse_width", vco2_pulse_width),

    OSC_SCALAR("/hds/vco3/vol", vco3_vol),
    OSC_SCALAR("/hds/vco3/pan", vco3_pan),
    OSC_SCALAR("/hds/vco3/mute", vco3_mute),
    OSC_SCALAR("/hds/vco3/pulse_width", vco3_pulse_width),
    OSC_SCALAR("/hds/vco4/vol", vco4_vol),
    OSC_SCALAR("/hds/vco4/pan", vco4_pan),
    OSC_SCALAR("/hds/vco4/mute", vco4_mute),
    OSC_SCALAR("/hds/vco4/pulse_width", vco4_pulse_width),

    OSC_ARRAY("/hds/ext_in/%d/enable", ext_in_enable, EXT_IN_PAIR_COUNT),
    OSC_ARRAY("/hds/ext_in/%d/gain", ext_in_gain, EXT_IN_PAIR_COUNT),

    // --- FX Bus (fx[FX_SLOT_COUNT], FXSlotParams) ---
    OSC_SUBARRAY("/hds/fx/%d/type", fx, FXSlotParams, fx_type, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/mix", fx, FXSlotParams, mix, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/bypass", fx, FXSlotParams, bypass, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/source", fx, FXSlotParams, fx_source, FX_SLOT_COUNT),
    OSC_NESTED("/hds/fx/%d/param/%d", fx, FXSlotParams, param, FX_SLOT_COUNT, FX_SLOT_PARAM_COUNT),
    OSC_ARRAY("/hds/fx_routing/%d/mode", fx_source_routing_mode, SOURCE_COUNT),

    // --- Multi-output hardware routing: which output pair(s) each source lands on ---
    OSC_NESTED("/hds/routing/%d/out/%d", source_routing, SourceRouting, enable, SOURCE_COUNT, OUTPUT_PAIR_MAX),

    OSC_SCALAR("/hds/pb/range_up", pb_range_up),
    OSC_SCALAR("/hds/pb/range_down", pb_range_down),
    OSC_SCALAR("/hds/sustain/polarity", sustain_pedal_polarity),
    OSC_SCALAR("/hds/audio/latency_ms", audio_latency_ms),
    OSC_SCALAR("/hds/audio/polyphony", active_voice_limit),
    OSC_SCALAR("/hds/audio/block_size", block_size_frames),

    OSC_SCALAR("/hds/record/enable", record_enable),
    OSC_SCALAR("/hds/record/include_synth_output", record_include_synth_output),
    OSC_SCALAR("/hds/record/downmix_stereo", record_downmix_stereo),
    OSC_ARRAY("/hds/record/channel/%d/enable", record_input_channel_enable, RECORD_MAX_CHANNELS),
    OSC_SCALAR("/hds/record/format", record_format),

    OSC_SCALAR("/hds/tempo/bpm", tempo_bpm),
    OSC_SCALAR("/hds/tempo/marker_count", tempo_marker_count),
    OSC_ARRAY("/hds/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX),
    OSC_ARRAY("/hds/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX),

    OSC_SCALAR("/hds/vco3/shape", vco3_shape),
    OSC_SCALAR("/hds/vco3/fine_tune", vco3_fine_tune),
    OSC_SCALAR("/hds/vco3/pitch_offset", vco3_pitch_offset),
    OSC_SCALAR("/hds/vco3/phase_offset", vco3_phase_offset),
    OSC_SCALAR("/hds/vco4/shape", vco4_shape),
    OSC_SCALAR("/hds/vco4/fine_tune", vco4_fine_tune),
    OSC_SCALAR("/hds/vco4/pitch_offset", vco4_pitch_offset),
    OSC_SCALAR("/hds/vco4/phase_offset", vco4_phase_offset),

    OSC_SCALAR("/hds/fm/algorithm", fm_algorithm),
    OSC_SCALAR("/hds/fm/depth/3_1", fm_depth_3_1),
    OSC_SCALAR("/hds/fm/depth/3_2", fm_depth_3_2),
    OSC_SCALAR("/hds/fm/depth/4_1", fm_depth_4_1),
    OSC_SCALAR("/hds/fm/depth/4_3", fm_depth_4_3),
    OSC_SCALAR("/hds/fm/depth/1_2", fm_depth_1_2),
    OSC_SCALAR("/hds/fm/depth/3_4", fm_depth_3_4),
    OSC_SCALAR("/hds/fm/depth/1_3", fm_depth_1_3),

    OSC_SCALAR("/hds/vco1/pitch", vco1_pitch),
    OSC_SCALAR("/hds/vco2/pitch", vco2_pitch),
    OSC_SCALAR("/hds/vco3/pitch", vco3_pitch),
    OSC_SCALAR("/hds/vco4/pitch", vco4_pitch),

    OSC_STRING("/hds/device/output", output_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/input", input_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/midi", midi_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/record/path", record_path, RECORD_PATH_LEN),
};
#define OSC_PARAMS_COUNT (sizeof(OSC_PARAMS) / sizeof(OSC_PARAMS[0]))

// Rebuild-and-compare match: sscanf's %d is lenient about what follows (it would happily accept
// "/hds/eg/3abc/attack" as index 3, then keep trying to match the literal "/attack" against
// "abc/attack" and fail there -- but only after consuming "3"). Re-serializing the parsed index
// with the SAME template and comparing byte-for-byte against the original path removes any
// ambiguity: if the address wasn't shaped exactly like the template with a real integer in that
// slot, this simply won't match, full stop.
static bool osc_match_one_index(const char *path, const char *tmpl, int *out_idx) {
    int idx;
    if (sscanf(path, tmpl, &idx) != 1) return false;
    char rebuilt[128];
    snprintf(rebuilt, sizeof(rebuilt), tmpl, idx);
    if (strcmp(rebuilt, path) != 0) return false;
    *out_idx = idx;
    return true;
}
static bool osc_match_two_index(const char *path, const char *tmpl, int *out_idx1, int *out_idx2) {
    int idx1, idx2;
    if (sscanf(path, tmpl, &idx1, &idx2) != 2) return false;
    char rebuilt[128];
    snprintf(rebuilt, sizeof(rebuilt), tmpl, idx1, idx2);
    if (strcmp(rebuilt, path) != 0) return false;
    *out_idx1 = idx1; *out_idx2 = idx2;
    return true;
}

// The single generic OSC method (registered with path=NULL, type=NULL -- liblo calls this for
// every incoming message that no more specific method claims). A linear scan over OSC_PARAMS is
// completely fine here: control messages arrive at UI-interaction rates (dozens/sec at most),
// nowhere near the audio thread's hot path.
static int generic_osc_handler(const char *path, const char *types, lo_arg **argv, int argc,
                                lo_message msg, void *user_data) {
    (void)msg;
    SynthSharedControls *knobs = (SynthSharedControls *)user_data;
    for (size_t i = 0; i < OSC_PARAMS_COUNT; i++) {
        const OscParamEntry *e = &OSC_PARAMS[i];
        size_t offset = e->base_offset;
        if (e->num_indices == 0) {
            if (strcmp(path, e->address) != 0) continue;
        } else if (e->num_indices == 1) {
            int idx;
            if (!osc_match_one_index(path, e->address, &idx)) continue;
            if (idx < 0 || idx >= e->count) { fprintf(stderr, "[osc] index %d out of range for '%s'\n", idx, path); return 0; }
            offset += (size_t)idx * e->stride;
        } else { // num_indices == 2
            int idx1, idx2;
            if (!osc_match_two_index(path, e->address, &idx1, &idx2)) continue;
            if (idx1 < 0 || idx1 >= e->count || idx2 < 0 || idx2 >= e->count2) { fprintf(stderr, "[osc] index out of range for '%s'\n", path); return 0; }
            offset += (size_t)idx1 * e->stride + (size_t)idx2 * e->stride2;
        }
        if (e->kind == OSC_KIND_FLOAT) {
            if (argc != 1 || types[0] != 'f') { fprintf(stderr, "[osc] '%s' expects one float arg, got types='%s'\n", path, types); return 0; }
            *(float *)((char *)knobs + offset) = argv[0]->f;
        } else { // OSC_KIND_STRING
            if (argc != 1 || types[0] != 's') { fprintf(stderr, "[osc] '%s' expects one string arg, got types='%s'\n", path, types); return 0; }
            snprintf((char *)knobs + offset, e->str_len, "%s", (const char *)&argv[0]->s);
        }
        return 0;
    }
    fprintf(stderr, "[osc] unrecognized address '%s' (types='%s') ignored\n", path, types);
    return 0;
}

static void osc_error_handler(int num, const char *msg, const char *path) {
    fprintf(stderr, "[osc] liblo error %d (%s): %s\n", num, path ? path : "?", msg);
}

// FIXED: Restored 'init_default_controls' macro mapping setup routine missing from compilation stack
static void init_default_controls(SynthSharedControls *knobs) {
    knobs->is_polyphonic = 1.0f; knobs->vco1_shape = 0.0f; knobs->vco2_shape = 3.0f;
    knobs->fm_intensity = 0.0f; knobs->vco1_vol = 0.8f; knobs->vco2_vol = 0.5f;
    knobs->portamento_time_ms = 150.0f; knobs->master_volume = 0.50f;
    knobs->vco1_pan = 0.0f; knobs->vco2_pan = 0.0f;
    knobs->vco1_fine_tune = 0.0f; knobs->vco2_pitch_offset = 0.0f; knobs->vco2_fine_tune = 0.0f;
    knobs->filter_ab_routing_mode = 0.0f; // Series A->B: mixer -> A -> B
    knobs->filter_ab_blend = 50.0f; // even blend, only used in Parallel mode
    knobs->filter_a_mode = 0.0f; // Normal (single filter)
    knobs->filter_a.cutoff = 800.0f; knobs->filter_a.res_q = 20.0f; knobs->filter_a.slope_poles = 0.0f; knobs->filter_a.filter_type = 1.0f;
    knobs->filter_a_multi_routing = 0.0f; // Series, matching the old default 3-filter behavior
    for(int f=0; f<3; f++) { knobs->filter_a_multi[f].cutoff = 400.0f + (f*200); knobs->filter_a_multi[f].res_q = 20.0f; knobs->filter_a_multi[f].slope_poles = 0.0f; knobs->filter_a_multi[f].filter_type = 1.0f; }
    knobs->filter_b.cutoff = 4000.0f; knobs->filter_b.res_q = 20.0f; knobs->filter_b.slope_poles = 0.0f; knobs->filter_b.filter_type = 0.0f; // Off by default -- Filter A alone matches old single-filter-ish defaults
    for(int l=0; l<LFO_COUNT; l++) { knobs->lfo[l].wave_type = 0.0f; knobs->lfo[l].rate_hz = 5.0f; knobs->lfo[l].mod_depth = 0.0f; knobs->lfo[l].key_sync = 1.0f; knobs->lfo[l].dest_target = 0.0f; knobs->lfo[l].phase_offset = 0.0f; knobs->lfo[l].mod_source_cc = -1.0f; }
    for(int e=0; e<ENV_COUNT; e++) { knobs->eg[e].d = 0.0f; knobs->eg[e].h = 0.0f; knobs->eg[e].a = 10.0f; knobs->eg[e].dec = 150.0f; knobs->eg[e].s = 80.0f; knobs->eg[e].r = 300.0f; knobs->eg[e].al = 0.0f; knobs->eg[e].pk = 100.0f; knobs->eg_mode[e] = 0.0f; }
    for(int i=0; i<ASSIGNABLE_EG_COUNT; i++) { knobs->eg_intensity[i] = 1.0f; knobs->eg_dest_target[i] = 0.0f; }
    // Subtle default detune (matches the old fixed +/-1.5% spread) and no width until the user asks for it
    knobs->vco1_unison_saw_detune = 25.0f; knobs->vco1_unison_saw_width = 50.0f;
    knobs->vco2_unison_saw_detune = 25.0f; knobs->vco2_unison_saw_width = 50.0f;
    knobs->mono_glide_random = 0.0f;
    knobs->vco1_mute = 0.0f; knobs->vco2_mute = 0.0f;
    knobs->hard_sync_mode = 0.0f;
    knobs->vco1_phase_offset = 0.0f; knobs->vco2_phase_offset = 0.0f;

    knobs->vco1_pulse_width = 50.0f; knobs->vco2_pulse_width = 50.0f;
    knobs->vco3_vol = 0.0f; knobs->vco3_pan = 0.0f; knobs->vco3_mute = 0.0f; knobs->vco3_pulse_width = 0.0f;
    knobs->vco3_shape = (float)WAVE_NOISE; knobs->vco3_fine_tune = 0.0f; knobs->vco3_pitch_offset = 0.0f;
    knobs->vco3_phase_offset = 0.0f; knobs->vco3_unison_saw_detune = 25.0f; knobs->vco3_unison_saw_width = 50.0f;
    knobs->vco4_vol = 0.0f; knobs->vco4_pan = 0.0f; knobs->vco4_mute = 0.0f; knobs->vco4_pulse_width = 50.0f;
    knobs->vco4_shape = (float)WAVE_OFF; knobs->vco4_fine_tune = 0.0f; knobs->vco4_pitch_offset = 0.0f;
    knobs->vco4_phase_offset = 0.0f; knobs->vco4_unison_saw_detune = 25.0f; knobs->vco4_unison_saw_width = 50.0f;
    knobs->fm_algorithm = 0.0f;
    knobs->fm_depth_3_1 = 0.0f; knobs->fm_depth_3_2 = 0.0f;
    knobs->fm_depth_4_1 = 0.0f; knobs->fm_depth_4_3 = 0.0f;
    knobs->fm_depth_1_2 = 0.0f; knobs->fm_depth_3_4 = 0.0f; knobs->fm_depth_1_3 = 0.0f;
    knobs->vco1_pitch = 0.0f; knobs->vco2_pitch = 0.0f; knobs->vco3_pitch = 0.0f; knobs->vco4_pitch = 0.0f;
    // Osc 3/4 Amp envelopes live at eg[2]/eg[3] (and eg_mode[2]/eg_mode[3]) and were covered by
    // the generic `for(int e=0; e<ENV_COUNT; e++)` default-fill loop above -- nothing extra
    // needed here.

    // ext_in_gain defaults to 0.0 (silence/-inf), not unity: an upstream mic pre or interface
    // could already be set hot, and External Input must never be able to burst into the FX bus
    // the moment ext_in_enable goes live (e.g. right after this engine (re)starts, before the
    // frontend's own dial -- which also always inits to 0 -- has sent its first packet). The
    // user always has to bring this up manually.
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) { knobs->ext_in_enable[p] = 0.0f; knobs->ext_in_gain[p] = 0.0f; }
    for (int f = 0; f < FX_SLOT_COUNT; f++) {
        knobs->fx[f].fx_type = FX_TYPE_OFF;
        for (int p = 0; p < FX_SLOT_PARAM_COUNT; p++) knobs->fx[f].param[p] = 0.0f;
        knobs->fx[f].mix = 100.0f; // fully wet once a real effect is assigned to this slot
        knobs->fx[f].bypass = 0.0f;
        knobs->fx[f].fx_source = 0.0f; // Program -- reproduces today's exact behavior by default
    }
    // Routing defaults: only the Program is routed anywhere (to output pair 0, i.e. today's only
    // output pair), every ext-in pair unrouted, every source's own FX chain in Series -- this
    // combination reproduces today's exact single-output-pair, Program-only behavior bit-for-bit.
    for (int s = 0; s < SOURCE_COUNT; s++) {
        knobs->fx_source_routing_mode[s] = 0.0f; // Series
        for (int o = 0; o < OUTPUT_PAIR_MAX; o++) knobs->source_routing[s].enable[o] = 0.0f;
    }
    knobs->source_routing[0].enable[0] = 1.0f; // Program -> output pair 0 (today's only output)
    knobs->pb_range_up = 2.0f; knobs->pb_range_down = 2.0f; // standard +/-2 semitone default
    knobs->sustain_pedal_polarity = 0.0f; // Normal
    knobs->audio_latency_ms = 25.0f; // matches the old hardcoded 25ms target -- no behavior change by default
    knobs->active_voice_limit = 32.0f; // matches the last-measured-safe default -- live-adjustable from here
    knobs->block_size_frames = 128.0f; // matches the old hardcoded BUFFER_SIZE -- no behavior change by default
    knobs->record_enable = 0.0f;
    knobs->record_include_synth_output = 1.0f;
    knobs->record_downmix_stereo = 0.0f;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) knobs->record_input_channel_enable[c] = 0.0f;
    knobs->record_path[0] = '\0';
    knobs->record_format = 0.0f; // WAV
    knobs->tempo_bpm = 120.0f; // a common, neutral default tempo
    knobs->tempo_marker_count = 0.0f; // no tempo map programmed -- tempo_bpm alone applies
    for (int t = 0; t < TEMPO_MARKER_MAX; t++) { knobs->tempo_marker_time[t] = 0.0f; knobs->tempo_marker_bpm[t] = 0.0f; }
    // No sample-bank defaults to fill here -- the Sample Bank isn't part of this struct anymore
    // (see its own dedicated socket/g_sample_slots[]).
    snprintf(knobs->output_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->input_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->midi_device, AUDIO_DEVICE_NAME_LEN, "auto");
}

static void init_voice(SynthVoice *voice) {
    voice->midi_note = -1;
    voice->midi_velocity = 100;
    voice->vco1_phase = 0.0f; voice->vco1_target_freq = 0.0f; voice->vco1_current_freq = 0.0f; voice->vco1_tri_state = 0.0f; voice->vco1_prev_fm_phase = 0.0f;
    voice->vco2_phase = 0.0f; voice->vco2_target_freq = 0.0f; voice->vco2_current_freq = 0.0f; voice->vco2_tri_state = 0.0f; voice->vco2_prev_fm_phase = 0.0f; voice->vco2_pending_sync_correction = 0.0f;
    voice->vco3_phase = 0.0f; voice->vco3_target_freq = 0.0f; voice->vco3_current_freq = 0.0f; voice->vco3_tri_state = 0.0f; voice->vco3_prev_fm_phase = 0.0f; voice->vco3_pending_sync_correction = 0.0f;
    voice->vco4_phase = 0.0f; voice->vco4_target_freq = 0.0f; voice->vco4_current_freq = 0.0f; voice->vco4_tri_state = 0.0f; voice->vco4_prev_fm_phase = 0.0f; voice->vco4_pending_sync_correction = 0.0f;
    for(int u=0; u<UNISON_SAW_VOICES; u++) { voice->vco1_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco2_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco3_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco4_unison_saw_phases[u] = (float)rand()/RAND_MAX; }
    for(int c=0; c<2; c++) { for(int f=0; f<3; f++) { voice->fa_low[c][f][0]=0.0f; voice->fa_low[c][f][1]=0.0f; voice->fa_band[c][f][0]=0.0f; voice->fa_band[c][f][1]=0.0f; } voice->fb_low[c][0]=0.0f; voice->fb_low[c][1]=0.0f; voice->fb_band[c][0]=0.0f; voice->fb_band[c][1]=0.0f; }
    voice->glide_time_multiplier = 1.0f;
    voice->sustain_pending = false;
    voice->vco1_eg.state = STATE_IDLE; voice->vco1_eg.current_level = 0.0f; voice->vco1_eg.release_level = 0.0f;
    voice->vco2_eg.state = STATE_IDLE; voice->vco2_eg.current_level = 0.0f; voice->vco2_eg.release_level = 0.0f;
    voice->vco3_eg.state = STATE_IDLE; voice->vco3_eg.current_level = 0.0f; voice->vco3_eg.release_level = 0.0f;
    voice->vco4_eg.state = STATE_IDLE; voice->vco4_eg.current_level = 0.0f; voice->vco4_eg.release_level = 0.0f;
    voice->vco1_tilt_lp = 0.0f; voice->vco2_tilt_lp = 0.0f; voice->vco3_tilt_lp = 0.0f; voice->vco4_tilt_lp = 0.0f;
    for(int e=0; e<ASSIGNABLE_EG_COUNT; e++) { voice->assign_eg[e].state = STATE_IDLE; voice->assign_eg[e].current_level = 0.0f; voice->assign_eg[e].release_level = 0.0f; }
    voice->vco1_sample_stream = -1; voice->vco1_sample_ptr = 0.0; voice->vco1_sample_preroll = false;
    voice->vco2_sample_stream = -1; voice->vco2_sample_ptr = 0.0; voice->vco2_sample_preroll = false;
    voice->vco3_sample_stream = -1; voice->vco3_sample_ptr = 0.0; voice->vco3_sample_preroll = false;
    voice->vco4_sample_stream = -1; voice->vco4_sample_ptr = 0.0; voice->vco4_sample_preroll = false;
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
    int velocities[MONO_STACK_SIZE]; // parallel to notes[] -- each note's own velocity, so
                                      // falling back to an earlier still-held key (handle_note_off)
                                      // restores THAT key's velocity rather than leaking whichever
                                      // note was struck last (see the fallback in handle_note_off).
    int count;
} MonoNoteStack;

// Live performance-controller state -- continuously updated from incoming MIDI Control Change/
// Pitch Bend/Channel Pressure messages (see process_midi_input), read every sample by
// render_audio_block for pitch bend and by run_lfo-adjacent code for CC-scaled LFO depth (see
// FX_TYPE_*-style "Modulation Source" on each LFO). Deliberately NOT part of SynthSharedControls
// -- this is performance data driven by the player's controller, not a GUI-set knob value.
typedef struct {
    float cc[128];          // 0..1, last received value of each MIDI CC number
    float pitch_bend;       // -1..1, 0 = centered
    float channel_pressure; // 0..1, monophonic (channel-wide) aftertouch
    bool sustain_held;      // CC64, after applying sustain_pedal_polarity -- see process_midi_input
} MidiLiveState;

static void mono_stack_push(MonoNoteStack *stack, int note, int velocity) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
            stack->count--;
            break;
        }
    }
    if (stack->count < MONO_STACK_SIZE) { stack->notes[stack->count] = note; stack->velocities[stack->count] = velocity; stack->count++; }
    else {
        for (int j = 0; j < MONO_STACK_SIZE - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
        stack->notes[MONO_STACK_SIZE - 1] = note;
        stack->velocities[MONO_STACK_SIZE - 1] = velocity;
    }
}

static void mono_stack_remove(MonoNoteStack *stack, int note) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
            stack->count--;
            return;
        }
    }
}

// --- DFD (direct-from-disk) sample playback: WAVE_SAMPLE oscillator source ---
//
// A loaded .wav's audio data is far too large to keep resident in RAM (a multi-second sample at
// 48kHz/stereo/float is tens of MB), so instead: the first SAMPLE_PRE_ROLL_LIMIT frames are
// decoded into RAM once at load time (a few hundred KB, giving zero-latency note-on), and
// everything after that streams from disk in real time on a dedicated thread into a lock-free
// ring buffer -- the SAME architecture, and the same realtime-safety rationale, as the WAV
// recorder above: fopen/fseek/fread are all blocking-time-unbounded, and this engine's main loop
// paces live audio output via blocking ALSA writes that cannot tolerate a stall. The audio
// thread here only ever does bounded, allocation-free work: a wait-free ring read plus linear
// interpolation (render_sample_osc), exactly mirroring record_ring_push/record_ring_pop's role
// in the recording engine. Ownership follows the identical convention: the disk thread is the
// ONLY thing that ever opens/reads/closes a FILE* or writes into a ring buffer; the audio thread
// only ever reads a ring buffer and publishes tiny atomic start/stop commands.
//
// Sample playback itself is adapted to HDS's own mature EG/filter/LFO/mod-matrix/FX-bus pipeline
// (see render_sample_osc's comment) rather than bringing in a parallel one. Zone selection --
// which loaded slot a given (note, velocity) actually plays -- is the real per-note/velocity
// mapping system (SFZ-style): see find_matching_sample_slot() and the Mapping tab in frontend.py.

// One loaded sample file's metadata plus its RAM pre-roll cache. Loaded via load_sample_slot(),
// called from the audio thread only when the corresponding wire-protocol sample-slot path
// actually changes (see the live-reload check in main()) -- never touched by the disk thread.
typedef struct {
    bool loaded;
    char file_path[SAMPLE_PATH_LEN];
    int root_note;               // MIDI note this sample plays back at its native (unpitched) rate
    // Zone range this slot answers to -- see find_matching_sample_slot(). Applied every block from
    // the wire's sample_slot_min_note[]/max_note[]/min_velocity[]/max_velocity[] regardless of
    // whether the file path itself changed (apply_sample_slot_zone_metadata() in main()), unlike
    // root_note/file_path/everything below which only update on an actual load (path change).
    int min_note, max_note;
    int min_velocity, max_velocity;
    bool unpitched; // playback rate forced to 1.0 regardless of note -- see render_sample_osc
    int round_robin_group; // 0 = independent zone; >0 = cycles with every other loaded, currently
                            // zone-matching slot sharing this same group id -- see
                            // find_matching_sample_slot()/g_round_robin_hit_count[].
    int zone_id;  // stable identity assigned by the frontend, used to reconcile ZONE_OP_UPSERT/
                  // ZONE_OP_REMOVE messages against this array -- see the OSC zone handlers.
    bool active;  // false = a tombstoned (removed) array position, permanently skipped; distinct
                  // from `loaded`, which just means "this slot's file has been successfully
                  // parsed" -- an active-but-not-yet-loaded slot is one whose load is still
                  // sitting in the throttled pending-loads queue.
    // --- SFZ-style per-zone params (Mapping tab's Zone Inspector), all read fresh from this live
    // struct every render_sample_osc()/start_sample_osc() call, same convention as root_note/
    // unpitched above -- see their call sites for exactly where each is applied.
    float tune_cents;        // fine pitch offset in cents, applied on top of the root-note ratio
                              // (and even when unpitched -- a nudge independent of pitch tracking)
    float gain_db;           // per-zone level trim in dB, multiplies render_sample_osc's output
    float pan;                // -1.0 (full left) .. 1.0 (full right), ADDS to the oscillator's own
                              // vco1_pan/vco2_pan knob rather than overriding it -- see
                              // sample_osc_zone_pan_offset()
    float start_offset_frac; // 0.0-1.0, where in the file playback begins (in the travel
                              // direction -- see `reverse` below); converted to a frame index once
                              // at note-on by start_sample_osc(), not stored pre-converted here
    bool reverse;             // plays the file/loop region backward -- see sample_disk_thread_main
    unsigned int data_start_offset;
    unsigned int total_frames;
    unsigned short num_channels;
    unsigned short bits_per_sample;
    unsigned short audio_format; // WAV fmt tag: 1 = PCM integer, 3 = IEEE float
    unsigned int pre_roll_frames;
    unsigned int loop_start, loop_end;
    bool loop_enabled;
    // Full SFZ loop_sustain/end= support (appended). loop_release_exit: SFZ's loop_sustain
    // (true) vs loop_continuous (false) -- when true, the owning voice's SampleStream.
    // release_requested flag (set once, at the note's STATE_RELEASE transition -- see
    // trigger_voice_release/request_sample_stream_release) makes sample_disk_thread_main stop
    // honoring loop_enabled for that stream's session, falling through to the exact same
    // natural-end-of-file path a plain non-looping zone already takes -- so the disk thread
    // keeps reading forward past loop_end into the file's own post-loop material (the release
    // tail) instead of wrapping back to loop_start again. playback_end_frame: SFZ's end=,
    // converted from a 0-1 fraction (see apply_playback_end(), mirroring apply_loop_points())
    // to an exclusive frame bound -- defaults to total_frames (no truncation) when end= isn't
    // given. Used ONLY at the disk thread's natural-end-of-file comparisons (never at the
    // loop-wrap comparisons, which still need the real total_frames-derived loop points).
    bool loop_release_exit;
    unsigned int playback_end_frame;
    float *pre_roll_l;
    float *pre_roll_r;
} SampleSlot;

// A plain static array (not malloc'd/realloc'd) sized SAMPLE_SLOT_CAPACITY -- its base address
// never moves for the life of the process, which sample_disk_thread_main() depends on (it
// dereferences &g_sample_slots[index] directly, with no lock; see SAMPLE_SLOT_CAPACITY's
// comment). g_sample_slot_count tracks the next append position; entries are append-only and
// never compacted/reused after a ZONE_OP_REMOVE (see the OSC zone handlers) -- an
// inactive (.active == false) entry is just permanently skipped everywhere.
static SampleSlot g_sample_slots[SAMPLE_SLOT_CAPACITY];
static int g_sample_slot_count = 0;

#define SAMPLE_CHECK_READ(ptr, size, count, stream) \
    do { if (fread(ptr, size, count, stream) != (size_t)(count)) { fclose(stream); return false; } } while (0)

// Parses a standard RIFF/WAVE header (skipping any unrecognized chunks rather than assuming a
// fixed layout), accepts PCM (16/24/32-bit int) or IEEE float (32-bit) data, and caches the
// first pre_roll_frames frames into RAM as float -1..1. Only the first two channels of a
// multichannel file are read (render_sample_osc downmixes to mono anyway -- see its comment);
// any further channels are skipped, not left to desync subsequent frame reads. Loop points are
// reset to a safe "off, full-range" placeholder here -- the real per-zone loop points get
// reapplied by the caller immediately after this returns (see the OSC zone handlers'
// throttled load drain), once total_frames (just parsed below) makes that conversion possible.
static bool load_sample_slot(int index, const char *path, int root_note) {
    if (index < 0 || index >= SAMPLE_SLOT_CAPACITY) return false;
    SampleSlot *slot = &g_sample_slots[index];

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[sample] failed to open '%s': %s\n", path, strerror(errno)); return false; }

    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }

    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    SAMPLE_CHECK_READ(&subchunk1_size, 4, 1, f);
    SAMPLE_CHECK_READ(&audio_format, 2, 1, f);
    SAMPLE_CHECK_READ(&num_channels, 2, 1, f);
    SAMPLE_CHECK_READ(&sample_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&byte_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&block_align, 2, 1, f);
    SAMPLE_CHECK_READ(&bits_per_sample, 2, 1, f);
    (void)sample_rate; (void)byte_rate; (void)block_align;

    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "[sample] '%s': unsupported WAV format tag %u (need 1=PCM or 3=float)\n", path, audio_format);
        fclose(f); return false;
    }
    if (audio_format == 1 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "[sample] '%s': unsupported %u-bit PCM\n", path, bits_per_sample);
        fclose(f); return false;
    }
    if (audio_format == 3 && bits_per_sample != 32) {
        fprintf(stderr, "[sample] '%s': float WAV must be 32-bit\n", path);
        fclose(f); return false;
    }
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);

    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }
    unsigned int data_size;
    SAMPLE_CHECK_READ(&data_size, 4, 1, f);

    int bytes_per_sample = bits_per_sample / 8;
    unsigned int total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);

    // Free whatever this slot held before -- a live reload (the GUI pointing this slot at a new
    // file) replaces the cache outright. This runs on the audio thread, same as every other live
    // wire-protocol change (device switches, etc.) -- see the live-reload check in main().
    free(slot->pre_roll_l); free(slot->pre_roll_r);
    slot->loaded = false;

    snprintf(slot->file_path, SAMPLE_PATH_LEN, "%s", path);
    slot->root_note = root_note;
    slot->total_frames = total_frames;
    slot->num_channels = num_channels;
    slot->bits_per_sample = bits_per_sample;
    slot->audio_format = audio_format;
    slot->data_start_offset = (unsigned int)ftell(f);
    // Loop points/enable are real per-zone data (the Mapping tab's waveform loop editor), not
    // computed here -- the caller (the OSC zone handlers' throttled load drain) applies
    // the real values from the zone record it queued this load with, immediately after this
    // function returns, using total_frames just assigned above. These are just safe placeholders
    // for the moment in between (and for any caller that loads a slot directly, e.g. a test): no
    // looping, full range, so a slot is never silently looped somewhere unintended.
    slot->loop_enabled = false;
    slot->loop_start = 0;
    slot->loop_end = total_frames;
    // Same reasoning as loop_start/loop_end above: a safe default for playback_end_frame (SFZ
    // end=), consulted on EVERY non-looping stream (unlike loop_start/loop_end, which stay
    // dormant while loop_enabled is false) -- so unlike those, this can't just default to the
    // BSS zero-init 0, which would make the disk thread think every non-looping zone had already
    // reached its end before streaming a single frame. The real caller (apply_zone_upsert/
    // drain_pending_sample_loads) overwrites this via apply_playback_end() immediately after this
    // function returns, same as loop points above.
    slot->playback_end_frame = total_frames;
    slot->loop_release_exit = false;

    unsigned int pre_roll_frames = (total_frames < SAMPLE_PRE_ROLL_LIMIT) ? total_frames : SAMPLE_PRE_ROLL_LIMIT;
    slot->pre_roll_frames = pre_roll_frames;
    slot->pre_roll_l = calloc((size_t)pre_roll_frames + 2, sizeof(float)); // +2: render_sample_osc reads one frame ahead
    slot->pre_roll_r = calloc((size_t)pre_roll_frames + 2, sizeof(float));
    if (!slot->pre_roll_l || !slot->pre_roll_r) { fclose(f); return false; }

    short raw16; int raw32; float fsamp = 0.0f; unsigned char b24[3];
    for (unsigned int i = 0; i < pre_roll_frames; i++) {
        if (audio_format == 1) {
            if (bits_per_sample == 16) { SAMPLE_CHECK_READ(&raw16, 2, 1, f); fsamp = (float)raw16 / 32768.0f; }
            else if (bits_per_sample == 24) { SAMPLE_CHECK_READ(b24, 1, 3, f); int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fsamp = (float)v / 2147483648.0f; }
            else { SAMPLE_CHECK_READ(&raw32, 4, 1, f); fsamp = (float)raw32 / 2147483648.0f; }
        } else { SAMPLE_CHECK_READ(&fsamp, 4, 1, f); }
        slot->pre_roll_l[i] = fsamp;

        if (num_channels >= 2) {
            if (audio_format == 1) {
                if (bits_per_sample == 16) { SAMPLE_CHECK_READ(&raw16, 2, 1, f); fsamp = (float)raw16 / 32768.0f; }
                else if (bits_per_sample == 24) { SAMPLE_CHECK_READ(b24, 1, 3, f); int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fsamp = (float)v / 2147483648.0f; }
                else { SAMPLE_CHECK_READ(&raw32, 4, 1, f); fsamp = (float)raw32 / 2147483648.0f; }
            } else { SAMPLE_CHECK_READ(&fsamp, 4, 1, f); }
            slot->pre_roll_r[i] = fsamp;
            if (num_channels > 2) fseek(f, (long)(num_channels - 2) * bytes_per_sample, SEEK_CUR);
        } else {
            slot->pre_roll_r[i] = fsamp;
        }
    }
    fclose(f);
    slot->loaded = true;
    // A successful load always un-tombstones the slot -- the catalog path (process_zone_catalog_
    // message) already sets this before enqueueing, but load_sample_slot() is also a valid entry
    // point on its own (tests, anything else loading a slot directly), and a loaded-but-inactive
    // slot would otherwise silently never match in find_matching_sample_slot().
    slot->active = true;
    fprintf(stderr, "[sample] loaded slot %d: '%s' (%u frames, root note %d)\n", index, path, total_frames, root_note);
    return true;
}

// A fixed pool of "currently streaming from disk" slots (see MAX_SAMPLE_STREAMS's comment on
// why this is a pool, not one embedded per voice/oscillator). A voice's oscillator claims one at
// note-on (if its shape is WAVE_SAMPLE) and releases it once that oscillator's amp envelope goes
// idle -- see claim_sample_stream/release_sample_stream and their call sites in handle_note_on/
// render_audio_block. Ownership mirrors RecordEngine/RecordRing exactly: the disk thread is the
// ONLY thing that ever opens/reads/closes disk_file_handle or writes into the ring buffer; the
// audio thread is the sole consumer (read_ptr, advanced by render_sample_osc). Start/stop is the
// same tiny atomic command handshake used there -- the audio thread publishes a command, the
// disk thread performs the actual I/O.
typedef enum { SAMPLE_CMD_NONE = 0, SAMPLE_CMD_START, SAMPLE_CMD_STOP } SampleStreamCommand;

typedef struct {
    bool in_use;         // audio-thread-owned: is this pool entry currently claimed by a voice
    _Atomic int slot_index; // which g_sample_slots[] entry -- set by the audio thread before
                     // publishing START, read by the disk thread when it processes that command
                     // (same handoff convention as RecordEngine.cmd_path). Atomic rather than
                     // plain: the release-store on `command` right after this write normally
                     // makes a plain field safe to read (standard release/acquire handoff), but a
                     // pool slot can be released and reclaimed again before the disk thread gets
                     // to run at all -- a THIRD claim's write can then land while the disk thread
                     // is mid-read of the SECOND claim's value, which a plain int can't survive
                     // race-free even though it's a rare timing window in practice. Atomic closes
                     // that gap outright (no tearing, no UB) rather than relying on how unlikely
                     // the window is.
    _Atomic unsigned int start_frame; // which file frame to seed playback at -- computed ONCE by
                     // start_sample_osc() (the only place that reads the live, mutable
                     // SampleSlot.start_offset_frac) and handed across on this same claim, exactly
                     // like slot_index above, rather than having the disk thread independently
                     // re-derive it from start_offset_frac at some later, unsynchronized moment --
                     // a live catalog edit landing in that gap would otherwise let the audio
                     // thread's *ptr seed and the disk thread's disk_read_frame_pos seed disagree
                     // about what ring index 0 means, an audible glitch on that one note.

    float ring_l[SAMPLE_RING_SIZE];
    float ring_r[SAMPLE_RING_SIZE];
    _Atomic unsigned int write_ptr; // disk thread only
    _Atomic unsigned int read_ptr;  // reset to 0 once by the disk thread at START (a new session
                                     // begins there); owned by the audio thread from then on, but
                                     // the disk thread also reads it (to compute free ring space)
                                     // -- hence atomic, not plain.

    _Atomic int command;              // SampleStreamCommand
    FILE *disk_file_handle;           // disk-thread-owned exclusively
    unsigned int disk_read_frame_pos; // disk-thread-owned: how far into the source file it's read
    int active_slot_index;            // disk-thread-owned: private copy of slot_index, captured
                                       // only once, at the instant the disk thread actually
                                       // consumes SAMPLE_CMD_START (that read of slot_index IS
                                       // properly synchronized, via the atomic acquire-exchange on
                                       // `command` pairing with the audio thread's release-store
                                       // in claim_sample_stream). Every other place the disk
                                       // thread needs to know which slot it's streaming uses this
                                       // copy instead of re-reading the audio-thread-owned
                                       // `slot_index` field directly -- that plain field is only
                                       // ever safe to read at the moment of consuming START; on
                                       // every other loop pass (ordinary ongoing streaming, cmd ==
                                       // NONE) there is no synchronization at all protecting it,
                                       // so a fast release+reclaim of the same pool slot could
                                       // have the audio thread overwrite it out from under an
                                       // in-flight disk-thread read (caught by ThreadSanitizer).
    unsigned int active_start_frame;  // disk-thread-owned: private copy of start_frame, captured
                                       // once at SAMPLE_CMD_START consumption, same reasoning and
                                       // same synchronization as active_slot_index above.
    bool disk_reverse_exhausted;      // disk-thread-owned only. Forward's "nothing left to
                                       // stream" (disk_read_frame_pos >= total_frames) is self-
                                       // sustaining forever once true, since pos only grows;
                                       // reverse has no such natural resting state (pos can't go
                                       // below 0), so this flag stands in for "already read frame
                                       // 0 of a non-looping reverse zone, nothing further to give."
                                       // Reset false on SAMPLE_CMD_START.
    _Atomic bool release_requested;  // audio-thread-owned write (request_sample_stream_release(),
                                       // called once from trigger_voice_release() at the owning
                                       // voice's STATE_RELEASE transition), disk-thread-owned read
                                       // (sample_disk_thread_main's loop-continuation checks) --
                                       // same simple atomic-flag handoff shape as the rest of this
                                       // struct. Reset false on SAMPLE_CMD_START (claim_sample_
                                       // stream) so a reclaimed pool entry never inherits a stale
                                       // true from whatever note last used it.
} SampleStream;

static SampleStream g_sample_streams[MAX_SAMPLE_STREAMS];
static pthread_t g_sample_disk_thread;
static _Atomic bool g_sample_disk_thread_shutdown = false;

// Claims a free pool slot for a NEW stream session (audio-thread only, called from
// handle_note_on). Returns -1 if the pool is exhausted -- a real possibility if someone tries to
// play more simultaneous WAVE_SAMPLE oscillators than MAX_SAMPLE_STREAMS; the caller treats -1
// as "this oscillator stays silent until a slot frees up," never a crash.
static int claim_sample_stream(int slot_index, unsigned int start_frame) {
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        if (!g_sample_streams[i].in_use) {
            g_sample_streams[i].in_use = true;
            atomic_store_explicit(&g_sample_streams[i].slot_index, slot_index, memory_order_relaxed);
            atomic_store_explicit(&g_sample_streams[i].start_frame, start_frame, memory_order_relaxed);
            // Fresh note, fresh session -- a reclaimed pool entry must not inherit a stale `true`
            // left over from whatever earlier note last released this same slot.
            atomic_store_explicit(&g_sample_streams[i].release_requested, false, memory_order_relaxed);
            atomic_store_explicit(&g_sample_streams[i].command, SAMPLE_CMD_START, memory_order_release);
            return i;
        }
    }
    return -1;
}

// Releases a claimed stream back to the pool (audio-thread only) and tells the disk thread to
// close its file. Safe to call even if a STOP is already pending/being processed.
static void release_sample_stream(int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&g_sample_streams[stream_index].command, SAMPLE_CMD_STOP, memory_order_release);
    g_sample_streams[stream_index].in_use = false;
}

// Marks a claimed stream's note as having entered RELEASE (audio-thread only, called once from
// trigger_voice_release() per oscillator). Read by sample_disk_thread_main() to decide whether a
// loop_release_exit zone should keep honoring its loop or fall through to its natural end -- see
// SampleSlot.loop_release_exit's own comment. A no-op on an already-released/invalid stream index,
// same defensive shape as release_sample_stream() above.
static void request_sample_stream_release(int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&g_sample_streams[stream_index].release_requested, true, memory_order_release);
}

static void *sample_disk_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        bool work_done = false;
        for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
            SampleStream *st = &g_sample_streams[i];

            int cmd = atomic_exchange_explicit(&st->command, SAMPLE_CMD_NONE, memory_order_acquire);
            if (cmd == SAMPLE_CMD_START) {
                if (st->disk_file_handle) { fclose(st->disk_file_handle); st->disk_file_handle = NULL; }
                // This is the ONE place slot_index/start_frame are safe to read: the acquire-
                // exchange above pairs with claim_sample_stream's release-store of `command`,
                // which happens after it writes both -- so this read is guaranteed to see those
                // writes. Captured into disk-thread-owned copies for every later use this
                // session, rather than re-reading the audio-thread-owned fields again.
                st->active_slot_index = atomic_load_explicit(&st->slot_index, memory_order_acquire);
                st->active_start_frame = atomic_load_explicit(&st->start_frame, memory_order_acquire);
                st->disk_reverse_exhausted = false;
                SampleSlot *slot = (st->active_slot_index >= 0 && st->active_slot_index < SAMPLE_SLOT_CAPACITY) ? &g_sample_slots[st->active_slot_index] : NULL;
                if (slot && slot->loaded) {
                    st->disk_file_handle = fopen(slot->file_path, "rb");
                    // Forward zones still get the pre-roll optimization (always covers frames
                    // [0, pre_roll_frames)): if the requested start falls inside that window,
                    // begin exactly at the existing pre_roll_frames boundary as before; otherwise
                    // -- or for ANY reverse zone, which skips pre-roll entirely (see
                    // start_sample_osc) -- begin exactly at the requested frame.
                    st->disk_read_frame_pos = (!slot->reverse && st->active_start_frame <= slot->pre_roll_frames)
                        ? slot->pre_roll_frames : st->active_start_frame;
                }
                atomic_store_explicit(&st->write_ptr, 0, memory_order_relaxed);
                atomic_store_explicit(&st->read_ptr, 0, memory_order_release);
                work_done = true;
            } else if (cmd == SAMPLE_CMD_STOP) {
                if (st->disk_file_handle) { fclose(st->disk_file_handle); st->disk_file_handle = NULL; }
                work_done = true;
            }

            if (!st->disk_file_handle) continue;
            SampleSlot *slot = &g_sample_slots[st->active_slot_index];
            int bytes_per_frame_unit = slot->bits_per_sample / 8;
            int bytes_per_frame = bytes_per_frame_unit * slot->num_channels;

            // SFZ loop_sustain: once this stream's owning voice has entered release AND the zone
            // is loop_release_exit, stop honoring loop_enabled for the rest of this session --
            // falls through to the exact same natural-end-of-file path (below) a plain
            // non-looping zone already takes, so playback continues forward past loop_end into
            // the file's own release tail instead of wrapping back to loop_start again.
            bool release_requested = atomic_load_explicit(&st->release_requested, memory_order_acquire);
            bool loop_active = slot->loop_enabled && !(slot->loop_release_exit && release_requested);

            unsigned int r_ptr = atomic_load_explicit(&st->read_ptr, memory_order_acquire);
            unsigned int w_ptr = atomic_load_explicit(&st->write_ptr, memory_order_relaxed);
            unsigned int filled = (w_ptr - r_ptr) & SAMPLE_RING_MASK;
            unsigned int free_space = SAMPLE_RING_SIZE - 1 - filled;
            if (free_space < (unsigned int)BUFFER_SIZE) continue; // plenty already buffered -- nothing to do this pass

            unsigned int chunk = (free_space < (unsigned int)BUFFER_SIZE) ? free_space : (unsigned int)BUFFER_SIZE;

            if (slot->reverse) {
                // --- Reverse read: parallel structure to the forward branch below, walking the
                // file backward instead. disk_read_frame_pos means "the next frame to read," same
                // convention as forward, just decreasing. Position updates are always decided by
                // an EQUALITY check before any decrement, never by decrementing first and
                // comparing after -- disk_read_frame_pos is unsigned, and decrementing it past 0
                // wraps to a huge value that a `< loop_start` check would then never catch again
                // (permanently stuck issuing a garbage fseek offset every pass thereafter).
                if (st->disk_reverse_exhausted) continue; // nothing left to stream (non-looping, already hit frame 0)
                for (unsigned int c = 0; c < chunk; c++) {
                    // Per-frame seek -- unlike forward's batched sequential reads, successive
                    // reverse frames are NOT contiguous with the previous read position, so each
                    // one needs its own fseek. This thread already does blocking I/O by design;
                    // more fseeks per frame is a real throughput cost under many concurrent
                    // reverse voices, not a realtime-safety concern -- documented trade-off, not
                    // optimized further here.
                    long byte_offset = (long)slot->data_start_offset + (long)st->disk_read_frame_pos * bytes_per_frame;
                    fseek(st->disk_file_handle, byte_offset, SEEK_SET);

                    float fl = 0.0f, fr = 0.0f; short raw16; int raw32; unsigned char b24[3];
                    bool ok = true;
                    if (slot->audio_format == 1) {
                        if (slot->bits_per_sample == 16) {
                            if (fread(&raw16, 2, 1, st->disk_file_handle) != 1) { ok = false; }
                            else {
                                fl = (float)raw16 / 32768.0f;
                                if (slot->num_channels >= 2) { if (fread(&raw16, 2, 1, st->disk_file_handle) != 1) ok = false; else fr = (float)raw16 / 32768.0f; } else fr = fl;
                            }
                        } else if (slot->bits_per_sample == 24) {
                            if (fread(b24, 1, 3, st->disk_file_handle) != 3) { ok = false; }
                            else {
                                int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fl = (float)v / 2147483648.0f;
                                if (slot->num_channels >= 2) { if (fread(b24, 1, 3, st->disk_file_handle) != 3) ok = false; else { v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fr = (float)v / 2147483648.0f; } } else fr = fl;
                            }
                        } else {
                            if (fread(&raw32, 4, 1, st->disk_file_handle) != 1) { ok = false; }
                            else {
                                fl = (float)raw32 / 2147483648.0f;
                                if (slot->num_channels >= 2) { if (fread(&raw32, 4, 1, st->disk_file_handle) != 1) ok = false; else fr = (float)raw32 / 2147483648.0f; } else fr = fl;
                            }
                        }
                    } else {
                        if (fread(&fl, 4, 1, st->disk_file_handle) != 1) { ok = false; }
                        else if (slot->num_channels >= 2) { if (fread(&fr, 4, 1, st->disk_file_handle) != 1) ok = false; } else fr = fl;
                    }
                    if (!ok) break;

                    unsigned int w = atomic_load_explicit(&st->write_ptr, memory_order_relaxed);
                    st->ring_l[w & SAMPLE_RING_MASK] = fl;
                    st->ring_r[w & SAMPLE_RING_MASK] = fr;
                    atomic_store_explicit(&st->write_ptr, w + 1, memory_order_release);

                    // Decide the NEXT position -- loop-wrap check first (correctly covers
                    // loop_start == 0 too, since it's checked before the floor check below),
                    // then the absolute floor (frame 0: either a genuine non-looping end, or a
                    // reverse zone seeded outside its own loop region on the "wrong" side -- see
                    // the Mapping tab's Start Offset documentation), then a normal decrement.
                    if (loop_active && st->disk_read_frame_pos == slot->loop_start) {
                        st->disk_read_frame_pos = slot->loop_end - 1;
                    } else if (st->disk_read_frame_pos == 0) {
                        st->disk_reverse_exhausted = true;
                        break;
                    } else {
                        st->disk_read_frame_pos--;
                    }
                }
            } else {
                if (loop_active && st->disk_read_frame_pos >= slot->loop_end) {
                    st->disk_read_frame_pos = slot->loop_start;
                } else if (!loop_active && st->disk_read_frame_pos >= slot->playback_end_frame) {
                    continue; // nothing left to stream -- render_sample_osc handles end-of-sample itself
                }

                long byte_offset = (long)slot->data_start_offset + (long)st->disk_read_frame_pos * bytes_per_frame;
                fseek(st->disk_file_handle, byte_offset, SEEK_SET);

                for (unsigned int c = 0; c < chunk; c++) {
                    if (loop_active && st->disk_read_frame_pos >= slot->loop_end) {
                        st->disk_read_frame_pos = slot->loop_start;
                        byte_offset = (long)slot->data_start_offset + (long)st->disk_read_frame_pos * bytes_per_frame;
                        fseek(st->disk_file_handle, byte_offset, SEEK_SET);
                    }
                    // playback_end_frame (SFZ end=, defaults to total_frames when unset): the
                    // natural end-of-file bound for non-looping playback -- see its own comment
                    // on SampleSlot. Never substitutes for total_frames at the loop-wrap
                    // comparisons above, which still need the real file length.
                    if (st->disk_read_frame_pos >= slot->playback_end_frame) break;

                    float fl = 0.0f, fr = 0.0f; short raw16; int raw32; unsigned char b24[3];
                    if (slot->audio_format == 1) {
                        if (slot->bits_per_sample == 16) {
                            if (fread(&raw16, 2, 1, st->disk_file_handle) != 1) break;
                            fl = (float)raw16 / 32768.0f;
                            if (slot->num_channels >= 2) { if (fread(&raw16, 2, 1, st->disk_file_handle) != 1) break; fr = (float)raw16 / 32768.0f; } else fr = fl;
                        } else if (slot->bits_per_sample == 24) {
                            if (fread(b24, 1, 3, st->disk_file_handle) != 3) break;
                            int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fl = (float)v / 2147483648.0f;
                            if (slot->num_channels >= 2) { if (fread(b24, 1, 3, st->disk_file_handle) != 3) break; v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); fr = (float)v / 2147483648.0f; } else fr = fl;
                        } else {
                            if (fread(&raw32, 4, 1, st->disk_file_handle) != 1) break;
                            fl = (float)raw32 / 2147483648.0f;
                            if (slot->num_channels >= 2) { if (fread(&raw32, 4, 1, st->disk_file_handle) != 1) break; fr = (float)raw32 / 2147483648.0f; } else fr = fl;
                        }
                    } else {
                        if (fread(&fl, 4, 1, st->disk_file_handle) != 1) break;
                        if (slot->num_channels >= 2) { if (fread(&fr, 4, 1, st->disk_file_handle) != 1) break; } else fr = fl;
                    }
                    if (slot->num_channels > 2) fseek(st->disk_file_handle, (long)(slot->num_channels - 2) * bytes_per_frame_unit, SEEK_CUR);

                    unsigned int w = atomic_load_explicit(&st->write_ptr, memory_order_relaxed);
                    st->ring_l[w & SAMPLE_RING_MASK] = fl;
                    st->ring_r[w & SAMPLE_RING_MASK] = fr;
                    atomic_store_explicit(&st->write_ptr, w + 1, memory_order_release);
                    st->disk_read_frame_pos++;
                }
            }
            work_done = true;
        }

        if (atomic_load_explicit(&g_sample_disk_thread_shutdown, memory_order_acquire)) break;
        if (!work_done) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); } // 1ms idle poll
    }
    return NULL;
}

// --- Sample Bank catalog protocol: two dedicated OSC addresses ---
//
// The Mapping tab's zone catalog travels as its own pair of OSC messages, entirely separate from
// SynthSharedControls/OSC_PARAMS[] above (see the historical BREAKING CHANGE comment on
// SynthSharedControls for why it was already kept out of that struct). Each message carries
// exactly ONE zone -- a drag/edit that touches several zones just sends several messages back to
// back, rather than one batched wire record the way the old two-phase TCP framing (accumulate a
// header, size a body buffer from its claimed count, accumulate the body) needed to. That whole
// framing layer is gone: liblo already delivers each UDP datagram as one complete, self-contained
// message, so there is no partial-message state to track between polls.
//   "/hds/zone/upsert" (types "ifffffffffffffffffs") -- add-or-update one zone by zone_id (on
//     fresh connect the frontend sends one upsert per zone in its current list, since this
//     side's catalog starts empty).
//   "/hds/zone/remove" (types "i") -- delete one zone by zone_id.
// See zone_upsert_handler()/zone_remove_handler() below, registered in main().

// One zone's fields, built directly from OSC message args (zone_upsert_handler) rather than
// parsed off raw wire bytes -- apply_zone_upsert()'s own logic is unchanged from the old
// wire-parsed version, it just receives an already-populated struct now.
typedef struct {
    int32_t zone_id;
    float root_note, min_note, max_note, min_velocity, max_velocity;
    float unpitched, loop_start, loop_end, loop_enabled, round_robin_group;
    // SFZ-style per-zone params (Mapping tab's Zone Inspector) -- see SampleSlot's own fields of
    // the same names for what each does.
    float tune_cents, gain_db, pan, start_offset_frac, reverse;
    // Full SFZ loop_sustain/end= support (appended, 2 more floats -- 17 total): see
    // SampleSlot.loop_release_exit/playback_end_frame's own comment.
    float loop_release_exit, end_frac;
    char path[SAMPLE_PATH_LEN];
} ZoneRecordWire;

// A zone whose file needs (re)loading, queued rather than loaded immediately -- see
// drain_pending_sample_loads()'s comment. A plain ring buffer, audio-thread-only (both the
// enqueue and the drain run on main()'s single thread), so no cross-thread synchronization is
// needed here at all, unlike the disk-streaming ring buffers elsewhere in this file.
typedef struct {
    int slot_index;
    int zone_id; // carried through only for the error message if the load fails
    char path[SAMPLE_PATH_LEN];
    int root_note, min_note, max_note, min_velocity, max_velocity;
    bool unpitched;
    float loop_start_frac, loop_end_frac;
    bool loop_enabled;
    int round_robin_group;
    float tune_cents, gain_db, pan, start_offset_frac;
    bool reverse;
    bool loop_release_exit;
    float end_frac;
} PendingSampleLoad;
#define PENDING_SAMPLE_LOAD_CAPACITY 4096
static PendingSampleLoad g_pending_loads[PENDING_SAMPLE_LOAD_CAPACITY];
static int g_pending_load_head = 0, g_pending_load_tail = 0, g_pending_load_count = 0;
// How many queued sample loads (each a bounded, one-file blocking file-open+pre-roll-decode) the
// audio thread will perform per block -- the throttle that keeps a large bulk-add from stalling
// real-time audio in one long burst; see SAMPLE_SLOT_CAPACITY's comment for the accepted
// trade-off (one genuinely slow file, e.g. a cold network mount, still stalls the whole audio
// thread for its own load duration -- this bounds how many such stalls can pile up per block, not
// how long any single one can take).
#define SAMPLE_LOADS_PER_BLOCK 1

static void enqueue_pending_sample_load(int slot_index, const ZoneRecordWire *rec) {
    if (g_pending_load_count >= PENDING_SAMPLE_LOAD_CAPACITY) {
        fprintf(stderr, "[samples] pending-load queue full -- dropping load for zone_id %d (re-send to retry)\n", rec->zone_id);
        return;
    }
    PendingSampleLoad *p = &g_pending_loads[g_pending_load_tail];
    p->slot_index = slot_index;
    p->zone_id = rec->zone_id;
    snprintf(p->path, SAMPLE_PATH_LEN, "%s", rec->path);
    p->root_note = (int)rec->root_note;
    p->min_note = (int)rec->min_note; p->max_note = (int)rec->max_note;
    p->min_velocity = (int)rec->min_velocity; p->max_velocity = (int)rec->max_velocity;
    p->unpitched = rec->unpitched != 0.0f;
    p->loop_start_frac = rec->loop_start; p->loop_end_frac = rec->loop_end;
    p->loop_enabled = rec->loop_enabled != 0.0f;
    p->round_robin_group = (int)rec->round_robin_group;
    p->tune_cents = rec->tune_cents; p->gain_db = rec->gain_db; p->pan = rec->pan;
    p->start_offset_frac = rec->start_offset_frac;
    p->reverse = rec->reverse != 0.0f;
    p->loop_release_exit = rec->loop_release_exit != 0.0f;
    p->end_frac = rec->end_frac;
    g_pending_load_tail = (g_pending_load_tail + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
    g_pending_load_count++;
}

// Converts loop-point fractions to real frame indices (shared by drain_pending_sample_loads() and
// the "path unchanged" branch of an UPSERT, both of which need total_frames -- only known once a
// file is actually loaded -- to do this conversion). Enforces a minimum loop length of one full
// crossfade window, same reasoning as before: a loop shorter than SAMPLE_XFADE_SIZE would have
// render_sample_osc's loop-splice crossfade permanently active instead of a brief transition.
static void apply_loop_points(SampleSlot *slot, float start_frac, float end_frac) {
    if (!slot->loaded || slot->total_frames <= 1) return;
    unsigned int total = slot->total_frames;
    start_frac = fmaxf(0.0f, fminf(1.0f, start_frac));
    end_frac = fmaxf(0.0f, fminf(1.0f, end_frac));
    unsigned int loop_start = (unsigned int)(start_frac * (float)(total - 1));
    unsigned int loop_end = (unsigned int)(end_frac * (float)(total - 1));
    unsigned int min_len = (unsigned int)SAMPLE_XFADE_SIZE;
    if (total - 1 < min_len) min_len = (total > 1) ? total - 1 : 0; // degenerate tiny file
    if (loop_end < loop_start + min_len) {
        loop_end = loop_start + min_len;
        if (loop_end > total - 1) { loop_end = total - 1; loop_start = (loop_end > min_len) ? loop_end - min_len : 0; }
    }
    slot->loop_start = loop_start;
    slot->loop_end = loop_end;
}

// Converts SFZ's end= (a 0-1 fraction, same convention as loop_start/loop_end/start_offset_frac)
// to an exclusive frame bound -- mirrors apply_loop_points()'s shape and call sites exactly.
// end_frac == 1.0 (the default when a zone specifies no end=) reproduces the pre-existing
// behavior of using the full file length verbatim (playback_end_frame == total_frames).
static void apply_playback_end(SampleSlot *slot, float end_frac) {
    if (!slot->loaded || slot->total_frames == 0) { slot->playback_end_frame = slot->total_frames; return; }
    end_frac = fmaxf(0.0f, fminf(1.0f, end_frac));
    unsigned int frame = (unsigned int)(end_frac * (float)slot->total_frames + 0.5f);
    if (frame < 1) frame = 1;
    if (frame > slot->total_frames) frame = slot->total_frames;
    slot->playback_end_frame = frame;
}

// Drains up to SAMPLE_LOADS_PER_BLOCK pending loads per call (see its #define). Immediately
// reapplies the queued zone record's loop points once total_frames is known post-load -- doing
// this only at message-receipt time (like the old apply_sample_slot_zone_metadata() did every
// block) would leave a sample that was still queued when its metadata arrived stuck on the
// load-time placeholder loop points forever.
static void drain_pending_sample_loads(void) {
    for (int i = 0; i < SAMPLE_LOADS_PER_BLOCK && g_pending_load_count > 0; i++) {
        PendingSampleLoad *p = &g_pending_loads[g_pending_load_head];
        g_pending_load_head = (g_pending_load_head + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
        g_pending_load_count--;

        // A ZONE_OP_REMOVE can tombstone this slot_index while its load is still sitting in the
        // queue (added, then removed again before its throttled turn comes up) -- the removal
        // wins: skip the load rather than have load_sample_slot() resurrect a tombstoned slot by
        // setting active back to true. Safe under the append-only invariant (a slot_index is
        // never reused for a different zone_id), so this can only be the same zone this load was
        // originally queued for.
        if (!g_sample_slots[p->slot_index].active) continue;

        if (load_sample_slot(p->slot_index, p->path, p->root_note)) {
            SampleSlot *slot = &g_sample_slots[p->slot_index];
            slot->min_note = p->min_note; slot->max_note = p->max_note;
            slot->min_velocity = p->min_velocity; slot->max_velocity = p->max_velocity;
            slot->unpitched = p->unpitched;
            slot->round_robin_group = p->round_robin_group;
            slot->loop_enabled = p->loop_enabled;
            apply_loop_points(slot, p->loop_start_frac, p->loop_end_frac);
            slot->tune_cents = p->tune_cents; slot->gain_db = p->gain_db; slot->pan = p->pan;
            slot->start_offset_frac = p->start_offset_frac;
            slot->reverse = p->reverse;
            slot->loop_release_exit = p->loop_release_exit;
            apply_playback_end(slot, p->end_frac);
        } else {
            fprintf(stderr, "[samples] failed to load queued sample '%s' (zone_id %d)\n", p->path, p->zone_id);
            g_sample_slots[p->slot_index].active = false;
        }
    }
}

// Applies one zone upsert -- unchanged logic from the old wire-parsed version, just fed an
// already-populated ZoneRecordWire (built from OSC args in zone_upsert_handler() below) instead
// of parsing one out of a raw byte buffer.
static void apply_zone_upsert(const ZoneRecordWire *rec) {
    int found_index = -1;
    for (int s = 0; s < g_sample_slot_count; s++) {
        if (g_sample_slots[s].active && g_sample_slots[s].zone_id == rec->zone_id) { found_index = s; break; }
    }

    if (found_index < 0) {
        // Brand new zone -- append. g_sample_slots is a plain static array (zero-initialized
        // BSS), so a never-before-used position already has loaded=false, pre_roll_l/r=NULL,
        // etc.; only zone_id/active need setting explicitly here.
        if (g_sample_slot_count >= SAMPLE_SLOT_CAPACITY) {
            fprintf(stderr, "[samples] Sample Bank at its %d-zone capacity -- ignoring zone_id %d\n", SAMPLE_SLOT_CAPACITY, rec->zone_id);
            return;
        }
        found_index = g_sample_slot_count++;
        g_sample_slots[found_index].zone_id = rec->zone_id;
        g_sample_slots[found_index].active = true;
        enqueue_pending_sample_load(found_index, rec);
        return;
    }

    SampleSlot *slot = &g_sample_slots[found_index];
    if (strncmp(slot->file_path, rec->path, SAMPLE_PATH_LEN) != 0 || !slot->loaded) {
        // Path changed (or a previous load of this same slot never finished/failed) -- (re)queue
        // rather than apply cheap fields in place.
        enqueue_pending_sample_load(found_index, rec);
        return;
    }

    // Path unchanged and already loaded: apply the cheap fields immediately, no reload.
    slot->root_note = (int)rec->root_note;
    slot->min_note = (int)rec->min_note; slot->max_note = (int)rec->max_note;
    slot->min_velocity = (int)rec->min_velocity; slot->max_velocity = (int)rec->max_velocity;
    slot->unpitched = rec->unpitched != 0.0f;
    slot->round_robin_group = (int)rec->round_robin_group;
    slot->loop_enabled = rec->loop_enabled != 0.0f;
    apply_loop_points(slot, rec->loop_start, rec->loop_end);
    slot->tune_cents = rec->tune_cents; slot->gain_db = rec->gain_db; slot->pan = rec->pan;
    slot->start_offset_frac = rec->start_offset_frac;
    slot->reverse = rec->reverse != 0.0f;
    slot->loop_release_exit = rec->loop_release_exit != 0.0f;
    apply_playback_end(slot, rec->end_frac);
}

static void apply_zone_remove(int32_t zone_id) {
    for (int s = 0; s < g_sample_slot_count; s++) {
        if (g_sample_slots[s].active && g_sample_slots[s].zone_id == zone_id) {
            g_sample_slots[s].active = false;
            g_sample_slots[s].loaded = false; // matches the pre-existing "empty path" unload behavior
            free(g_sample_slots[s].pre_roll_l); g_sample_slots[s].pre_roll_l = NULL;
            free(g_sample_slots[s].pre_roll_r); g_sample_slots[s].pre_roll_r = NULL;
            break;
        }
    }
}

// "/hds/zone/upsert" i + 17 f's + s -- zone_id + 17 floats in ZoneRecordWire's own field order
// (root_note, min_note, max_note, min_velocity, max_velocity, unpitched, loop_start, loop_end,
// loop_enabled, round_robin_group, tune_cents, gain_db, pan, start_offset_frac, reverse,
// loop_release_exit, end_frac) + path. liblo only dispatches here when the incoming type string
// matches exactly, so argc/types are already guaranteed correct -- no manual validation needed,
// unlike generic_osc_handler's table.
static int zone_upsert_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg; (void)user_data;
    ZoneRecordWire rec;
    rec.zone_id = argv[0]->i;
    float *f = &rec.root_note; // root_note..end_frac are 17 consecutive floats
    for (int i = 0; i < 17; i++) f[i] = argv[1 + i]->f;
    snprintf(rec.path, SAMPLE_PATH_LEN, "%s", (const char *)&argv[18]->s);
    apply_zone_upsert(&rec);
    return 0;
}
// "/hds/zone/remove" i -- a single zone_id.
static int zone_remove_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg; (void)user_data;
    apply_zone_remove(argv[0]->i);
    return 0;
}

// Round-robin hit counters, one per possible group id (index 0 unused -- group 0 means "not
// grouped", see SampleSlot.round_robin_group). Persists across calls the same way limiter_gain/
// lfo_phase_mod_state do elsewhere in this file (one synth instance, one audio thread -- no
// atomics needed, this is audio-thread-only state).
static unsigned int g_round_robin_hit_count[MAX_ROUND_ROBIN_GROUPS + 1] = {0};

// Finds the loaded sample slot whose zone (key range AND velocity range, both inclusive) covers
// this (note, velocity) -- the real per-note/velocity mapping (SFZ-style): a note outside every
// loaded zone is silent, not "nearest root note always plays something." Overlapping zones (the
// Mapping tab's rectangles are freeform -- nothing stops the user from drawing two that overlap)
// are resolved by preferring the SMALLEST matching zone (key range * velocity range), the most
// deliberately-specific one. Among ties at that smallest area (the normal case for several
// round-robin variations all dropped at the exact same key+velocity range), slots sharing a
// non-zero round_robin_group cycle through each other one hit at a time instead of the same one
// always winning; an ungrouped tie (or a genuine unintentional overlap the user hasn't grouped)
// keeps the original deterministic behavior. Returns -1 if nothing loaded matches.
//
// Uses O(1) extra memory (three linear passes over the catalog) instead of building candidate/
// group-member arrays sized to a compile-time constant, since the catalog no longer has one.
// "First encountered while scanning in ascending array-index order" doubles as "lowest zone_id"
// here because g_sample_slots is strictly append-only (see its own comment) -- array index order
// and zone_id creation order are the same thing. That invariant would need revisiting if slot
// reuse/compaction is ever added later.
static int find_matching_sample_slot(int note, int velocity) {
    long best_area = -1; // -1 sentinel: "no candidate yet", distinct from a real zero-area zone
    int first_best_idx = -1;
    int tie_count = 0;
    for (int i = 0; i < g_sample_slot_count; i++) {
        SampleSlot *slot = &g_sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (best_area < 0 || area < best_area) { best_area = area; first_best_idx = i; tie_count = 1; }
        else if (area == best_area) { tie_count++; }
    }
    if (tie_count == 0) return -1;
    if (tie_count == 1) return first_best_idx;

    int group = g_sample_slots[first_best_idx].round_robin_group;
    if (group <= 0) return first_best_idx;

    int member_count = 0;
    for (int i = 0; i < g_sample_slot_count; i++) {
        SampleSlot *slot = &g_sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (area == best_area && slot->round_robin_group == group) member_count++;
    }
    if (member_count <= 1) return first_best_idx;

    int group_idx = group;
    if (group_idx < 1) group_idx = 1; else if (group_idx > MAX_ROUND_ROBIN_GROUPS) group_idx = MAX_ROUND_ROBIN_GROUPS;
    unsigned int hit = g_round_robin_hit_count[group_idx]++;
    unsigned int target_ordinal = hit % (unsigned int)member_count;
    unsigned int ordinal = 0;
    for (int i = 0; i < g_sample_slot_count; i++) {
        SampleSlot *slot = &g_sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (area == best_area && slot->round_robin_group == group) {
            if (ordinal == target_ordinal) return i;
            ordinal++;
        }
    }
    return first_best_idx; // unreachable given member_count > 0, but keeps the compiler happy
}

// Starts (or restarts) an oscillator's sample playback for a fresh note -- releases whatever
// stream it might already hold (a retrigger/voice-steal case) and claims a new one if
// find_matching_sample_slot finds a zone covering this note+velocity. Leaves *stream_idx at -1
// (silent) if nothing matches or the stream pool is exhausted, rather than failing the whole
// note-on.
static void start_sample_osc(int *stream_idx, double *ptr, bool *preroll, int note, int velocity) {
    if (*stream_idx >= 0) { release_sample_stream(*stream_idx); *stream_idx = -1; }
    int slot_idx = find_matching_sample_slot(note, velocity);
    unsigned int start_frame = 0;
    if (slot_idx >= 0) {
        SampleSlot *slot = &g_sample_slots[slot_idx];
        // start_offset_frac is where in the file playback begins, in the travel direction (see
        // SampleSlot.reverse's comment) -- computed ONCE here, on the audio thread, from the one
        // live copy of this field, then handed to the disk thread atomically via
        // claim_sample_stream (see SampleStream.start_frame's comment for why: independently
        // re-deriving this on both threads from the same mutable field at two different,
        // unsynchronized moments could disagree if a catalog edit landed in between).
        float frac = fmaxf(0.0f, fminf(1.0f, slot->start_offset_frac));
        start_frame = (unsigned int)(frac * (float)(slot->total_frames > 0 ? slot->total_frames - 1 : 0));
        if (slot->reverse) {
            // Reverse zones skip the pre-roll fast path entirely (it's only ever decoded forward
            // from file frame 0, useless for a zone that wants to start near the end) -- go
            // straight to ring consumption, accepting a brief real disk-catch-up latency window,
            // the same class of trade-off already accepted elsewhere in this file.
            *preroll = false;
            *ptr = 0.0;
        } else {
            // Forward: if start_frame falls inside the pre-roll window, seed ptr there directly
            // and let normal pre-roll interpolation proceed; otherwise seed ptr at EXACTLY the
            // pre_roll_frames boundary so render_sample_osc's existing "ptr >= pre_roll_frames ->
            // hand off to ring" logic fires on the very first call, against a ring the disk
            // thread has ALSO been told (via start_frame) to start filling from start_frame.
            *ptr = (double)(start_frame < slot->pre_roll_frames ? start_frame : slot->pre_roll_frames);
            *preroll = true;
        }
    } else {
        *ptr = 0.0;
        *preroll = true;
    }
    *stream_idx = (slot_idx >= 0) ? claim_sample_stream(slot_idx, start_frame) : -1;
}

// Called once per active voice per sample from render_audio_block, right after the EG tick loop
// -- if an oscillator's amp envelope has gone idle but it's still holding a claimed sample
// stream, releases it. Idempotent (stream_index stays -1 afterward, so this is a cheap no-op on
// every subsequent call) and cheap enough to run unconditionally.
static void release_idle_sample_streams(SynthVoice *voice) {
    if (voice->vco1_eg.state == STATE_IDLE && voice->vco1_sample_stream >= 0) {
        release_sample_stream(voice->vco1_sample_stream);
        voice->vco1_sample_stream = -1;
    }
    if (voice->vco2_eg.state == STATE_IDLE && voice->vco2_sample_stream >= 0) {
        release_sample_stream(voice->vco2_sample_stream);
        voice->vco2_sample_stream = -1;
    }
    if (voice->vco3_eg.state == STATE_IDLE && voice->vco3_sample_stream >= 0) {
        release_sample_stream(voice->vco3_sample_stream);
        voice->vco3_sample_stream = -1;
    }
    if (voice->vco4_eg.state == STATE_IDLE && voice->vco4_sample_stream >= 0) {
        release_sample_stream(voice->vco4_sample_stream);
        voice->vco4_sample_stream = -1;
    }
}

// Drop-in mono waveform source for a WAVE_SAMPLE oscillator, mirroring lookup_osc_sample's role
// (called once per voice per sample from render_audio_block; its return value flows into the
// exact same amp-EG/pan/filter/FX pipeline every other oscillator uses -- that's the entire
// point of this design, per the user's direction: adapt sample playback to HDS's existing mature
// architecture, not bring in a parallel one). `hz` is the SAME fully-modulated frequency (fine
// tune, pitch bend, every mod-matrix pitch destination, portamento) already computed for the
// periodic oscillators -- sample playback rate is just that divided by the sample's own
// root-note frequency, so pitch bend/vibrato/EG-to-pitch all work on a sample voice for free,
// with zero duplicated pitch logic.
//
// A natively stereo file's real L/R pair is also handed back via the optional out_l/out_r
// pointers (NULL if the caller doesn't want them), exactly mirroring render_unison_saw's
// side_l/side_r convention -- the mono return value stays available for FM-modulator use and
// mono-file callers. The pre-roll cache and ring buffer already track L and R completely
// independently end to end (interpolation, loop-splice crossfade, everything); mono is only
// ever collapsed at the very last step here, so a stereo drum loop or pad keeps its real width
// instead of being downmixed, the same fix WAVE_UNISON_SAW's own stereo image needed earlier.
static float render_sample_osc(int *stream_idx, double *ptr_inout, bool *preroll_inout, float hz, float *out_l_ptr, float *out_r_ptr) {
    if (*stream_idx < 0) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }
    SampleStream *stream = &g_sample_streams[*stream_idx];
    if (!stream->in_use) { *stream_idx = -1; if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; } // reclaimed elsewhere -- defensive, shouldn't happen
    SampleSlot *slot = &g_sample_slots[atomic_load_explicit(&stream->slot_index, memory_order_relaxed)];
    if (!slot->loaded) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }

    float ratio;
    if (slot->unpitched) {
        ratio = 1.0f; // DRUM (Unpitched) -- always plays at its native recorded rate
    } else {
        float root_hz = 440.0f * powf(2.0f, (float)(slot->root_note - 69) / 12.0f);
        ratio = (root_hz > 1.0f) ? fmaxf(0.01f, hz / root_hz) : 1.0f;
    }
    // Per-zone fine tune (cents) -- applied even when unpitched, a nudge independent of the
    // root-note-relative pitch tracking `unpitched` specifically disables.
    if (slot->tune_cents != 0.0f) ratio *= powf(2.0f, slot->tune_cents / 1200.0f);

    double ptr = *ptr_inout;
    bool preroll = *preroll_inout;
    float out_l = 0.0f, out_r = 0.0f;

    if (preroll) {
        unsigned int int_ptr = (unsigned int)ptr;
        if (int_ptr >= slot->pre_roll_frames) {
            // Hand off to the ring buffer. Carry the fractional remainder across the boundary
            // (rather than hard-resetting to 0.0) so pitched playback (ratio != 1.0) doesn't lose
            // or duplicate a fraction of a sample here -- ring index 0 is exactly file frame
            // pre_roll_frames (see sample_disk_thread_main's SAMPLE_CMD_START handler), so this
            // stays sample-accurate with the pre-roll cache's own indexing.
            preroll = false;
            ptr -= (double)slot->pre_roll_frames;
        } else {
            // Last valid pre-roll index (pre_roll_frames - 1) has no real "next" sample in this
            // cache -- pre_roll_l/r are only populated up to pre_roll_frames - 1 (the trailing +2
            // calloc'd frames are zero-padding purely to make this interpolation read safe, not
            // real audio). Hold flat there instead of interpolating toward that zero pad, which
            // would otherwise dip the output toward silence right before the transition.
            unsigned int next_idx = int_ptr + 1;
            bool has_next = next_idx < slot->pre_roll_frames;
            float next_l = has_next ? slot->pre_roll_l[next_idx] : slot->pre_roll_l[int_ptr];
            float next_r = has_next ? slot->pre_roll_r[next_idx] : slot->pre_roll_r[int_ptr];
            float frac = (float)(ptr - (double)int_ptr);
            out_l = slot->pre_roll_l[int_ptr] + frac * (next_l - slot->pre_roll_l[int_ptr]);
            out_r = slot->pre_roll_r[int_ptr] + frac * (next_r - slot->pre_roll_r[int_ptr]);
            ptr += ratio;
        }
    }

    if (!preroll) {
        unsigned int r_ptr = atomic_load_explicit(&stream->read_ptr, memory_order_acquire);
        unsigned int w_ptr = atomic_load_explicit(&stream->write_ptr, memory_order_acquire);
        unsigned int available = (w_ptr - r_ptr) & SAMPLE_RING_MASK;

        if (available < (unsigned int)(SAMPLE_XFADE_SIZE + 2)) {
            if (!slot->loop_enabled) {
                // Natural end of a non-looping sample -- release the stream cleanly rather than
                // read past what's actually been written. A genuinely-behind disk thread on a
                // LOOPING sample instead just holds silent below rather than tearing the voice
                // down (vanishingly rare given the pre-roll head start and the ring's depth).
                release_sample_stream(*stream_idx);
                *stream_idx = -1;
                *ptr_inout = ptr; *preroll_inout = preroll;
                if (out_l_ptr) *out_l_ptr = 0.0f;
                if (out_r_ptr) *out_r_ptr = 0.0f;
                return 0.0f;
            }
            *ptr_inout = ptr; *preroll_inout = preroll;
            if (out_l_ptr) *out_l_ptr = 0.0f;
            if (out_r_ptr) *out_r_ptr = 0.0f;
            return 0.0f;
        }

        unsigned int int_ptr = (unsigned int)ptr;
        float frac = (float)(ptr - (double)int_ptr);
        unsigned int idx0 = (r_ptr + int_ptr) & SAMPLE_RING_MASK;
        unsigned int idx1 = (idx0 + 1) & SAMPLE_RING_MASK;

        unsigned int frames_left = available - int_ptr;
        if (slot->loop_enabled && frames_left <= (unsigned int)SAMPLE_XFADE_SIZE && frames_left > 1) {
            float fade_in = (float)(SAMPLE_XFADE_SIZE - frames_left) / (float)SAMPLE_XFADE_SIZE;
            float fade_out = 1.0f - fade_in;
            unsigned int widx0 = (r_ptr + int_ptr + (unsigned int)SAMPLE_XFADE_SIZE) & SAMPLE_RING_MASK;
            unsigned int widx1 = (widx0 + 1) & SAMPLE_RING_MASK;
            float base_l = stream->ring_l[idx0] + frac * (stream->ring_l[idx1] - stream->ring_l[idx0]);
            float wrap_l = stream->ring_l[widx0] + frac * (stream->ring_l[widx1] - stream->ring_l[widx0]);
            out_l = base_l * fade_out + wrap_l * fade_in;
            float base_r = stream->ring_r[idx0] + frac * (stream->ring_r[idx1] - stream->ring_r[idx0]);
            float wrap_r = stream->ring_r[widx0] + frac * (stream->ring_r[widx1] - stream->ring_r[widx0]);
            out_r = base_r * fade_out + wrap_r * fade_in;
        } else {
            out_l = stream->ring_l[idx0] + frac * (stream->ring_l[idx1] - stream->ring_l[idx0]);
            out_r = stream->ring_r[idx0] + frac * (stream->ring_r[idx1] - stream->ring_r[idx0]);
        }

        ptr += ratio;
        while (ptr >= 1.0) { ptr -= 1.0; r_ptr++; }
        atomic_store_explicit(&stream->read_ptr, r_ptr, memory_order_release);
    }

    *ptr_inout = ptr;
    *preroll_inout = preroll;
    if (slot->gain_db != 0.0f) {
        float gain_mult = powf(10.0f, slot->gain_db / 20.0f);
        out_l *= gain_mult; out_r *= gain_mult;
    }
    if (out_l_ptr) *out_l_ptr = out_l;
    if (out_r_ptr) *out_r_ptr = out_r;
    return (out_l + out_r) * 0.5f;
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
    WaveType shape, float own_phase, float dt, float phase_offset, float modulator_sum,
    float *prev_fm_phase, float pulse_or_tilt_input, float *tri_state, float *tilt_lp,
    float noise_tilt_coeff, float unison_saw_phases[UNISON_SAW_VOICES], float unison_detune_final,
    const float unison_pan_l[UNISON_SAW_VOICES], const float unison_pan_r[UNISON_SAW_VOICES],
    int *sample_stream, double *sample_ptr, bool *sample_preroll, float hz,
    bool do_sync, float t_wrap, float *pending_sync_correction,
    float *out_side_l, float *out_side_r)
{
    if (shape == WAVE_UNISON_SAW) {
        return render_unison_saw(unison_saw_phases, dt, unison_detune_final, unison_pan_l, unison_pan_r, phase_offset, out_side_l, out_side_r);
    }
    if (shape == WAVE_SAMPLE) {
        return render_sample_osc(sample_stream, sample_ptr, sample_preroll, hz, out_side_l, out_side_r);
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
        // Tilt-shaped noise (unified across all 4 oscillators -- see the Tilt-noise decision):
        // white noise through a one-pole low-pass; tilt_frac in [-1,1] crossfades low-passed
        // (dark, -1) against its high-passed complement (bright/hissy, +1), 0 = flat white
        // (low+high sums back to the original signal, unity gain).
        float white = (((float)rand() / (float)RAND_MAX) * 2.0f) - 1.0f;
        *tilt_lp += noise_tilt_coeff * (white - *tilt_lp);
        float tilt_high = white - *tilt_lp;
        float tilt_frac = fmaxf(-1.0f, fminf(1.0f, pulse_or_tilt_input / 100.0f));
        *out_side_l = *out_side_r = 0.0f;
        return *tilt_lp * (1.0f - tilt_frac) + tilt_high * (1.0f + tilt_frac);
    }

    float out = lookup_osc_sample(shape, mod_phase, adaptive_dt, pulse_or_tilt_input / 100.0f, tri_state);

    // Full alias-free hard sync amplitude correction (Saw/Square/Pulse only), generalized from
    // the original Osc2-only block -- see that block's own original comment (still present on
    // the mixer's history) for the full derivation. Consumes any "after" correction left pending
    // by a sync detected on the PREVIOUS sample, then, if THIS sample detects a new sync,
    // BLEP-corrects the discontinuity that reset will create.
    out += *pending_sync_correction;
    *pending_sync_correction = 0.0f;
    if (do_sync && (shape == WAVE_SAW || shape == WAVE_SQUARE || shape == WAVE_PULSE)) {
        float phase_at_sync = fmodf(own_phase + t_wrap * dt, 1.0f); if (phase_at_sync < 0.0f) phase_at_sync += 1.0f;
        float val_before, val_after;
        if (shape == WAVE_SAW) { val_before = phase_at_sync * 2.0f - 1.0f; val_after = -1.0f; }
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
    { {1,0,2,3}, 1, { {1,0, offsetof(SynthSharedControls, fm_intensity)} } },
    // 1: 3-Op Chain, Osc4 independent -- 3->2->1
    { {2,1,0,3}, 2, { {2,1, offsetof(SynthSharedControls, fm_depth_3_2)}, {1,0, offsetof(SynthSharedControls, fm_intensity)} } },
    // 2: 4-Op Chain -- 4->3->2->1
    { {3,2,1,0}, 3, { {3,2, offsetof(SynthSharedControls, fm_depth_4_3)}, {2,1, offsetof(SynthSharedControls, fm_depth_3_2)}, {1,0, offsetof(SynthSharedControls, fm_intensity)} } },
    // 3: Dual Serial Pairs -- 4->3, 2->1
    { {3,2,1,0}, 2, { {3,2, offsetof(SynthSharedControls, fm_depth_4_3)}, {1,0, offsetof(SynthSharedControls, fm_intensity)} } },
    // 4: Many-to-One into 1 -- 3->1, 4->1
    { {2,3,0,1}, 2, { {2,0, offsetof(SynthSharedControls, fm_depth_3_1)}, {3,0, offsetof(SynthSharedControls, fm_depth_4_1)} } },
    // 5: One-to-Many from 3 -- 3->1, 3->2
    { {2,0,1,3}, 2, { {2,0, offsetof(SynthSharedControls, fm_depth_3_1)}, {2,1, offsetof(SynthSharedControls, fm_depth_3_2)} } },
    // 6: Many-to-One into 2 -- 1->2, 3->2
    { {0,2,1,3}, 2, { {0,1, offsetof(SynthSharedControls, fm_depth_1_2)}, {2,1, offsetof(SynthSharedControls, fm_depth_3_2)} } },
    // 7: 4->1 direct, 2/3 independent
    { {3,0,1,2}, 1, { {3,0, offsetof(SynthSharedControls, fm_depth_4_1)} } },
    // 8: Dual Independent Pairs -- 1->2, 3->4
    { {0,1,2,3}, 2, { {0,1, offsetof(SynthSharedControls, fm_depth_1_2)}, {2,3, offsetof(SynthSharedControls, fm_depth_3_4)} } },
    // 9: 1->3->4, Osc2 independent
    { {0,2,3,1}, 2, { {0,2, offsetof(SynthSharedControls, fm_depth_1_3)}, {2,3, offsetof(SynthSharedControls, fm_depth_3_4)} } },
    // 10: Star: 2/3/4 all into 1
    { {1,2,3,0}, 3, { {1,0, offsetof(SynthSharedControls, fm_intensity)}, {2,0, offsetof(SynthSharedControls, fm_depth_3_1)}, {3,0, offsetof(SynthSharedControls, fm_depth_4_1)} } },
    // 11: All independent
    { {0,1,2,3}, 0, { {0,0,0}, {0,0,0}, {0,0,0} } },
};

// Debug-only startup sanity check (see main()): confirms every algorithm row's edges only ever
// point from an earlier render_order slot to a later one -- a bad hand-edited row would otherwise
// silently read a not-yet-rendered oscillator's stale modulator_output from the previous sample
// instead of failing loudly.
static void assert_fm_algorithms_acyclic(void) {
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
static float sample_osc_zone_pan_offset(int stream_idx) {
    if (stream_idx < 0) return 0.0f;
    SampleStream *stream = &g_sample_streams[stream_idx];
    if (!stream->in_use) return 0.0f;
    int slot_idx = atomic_load_explicit(&stream->slot_index, memory_order_relaxed);
    if (slot_idx < 0 || slot_idx >= SAMPLE_SLOT_CAPACITY) return 0.0f;
    SampleSlot *slot = &g_sample_slots[slot_idx];
    return slot->loaded ? slot->pan : 0.0f;
}

static void handle_note_on(SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], int note, int velocity, bool poly_mode_active, MonoNoteStack *mono_stack) {
    int slot = -1;
    if (poly_mode_active) {
        // BUG FIX: this used to only check vco1_eg/vco2_eg, so a voice whose vco3_eg was
        // still audibly releasing (Osc 3 Amp envelope) could get stolen and reset mid-tail
        // -- an audible click, not just a modulation glitch, since Osc 3 is a
        // real sound source. vco3_eg must be idle too before a voice counts as free.
        int voice_limit = effective_voice_limit(knobs);
        for (int v = 0; v < voice_limit; v++) { if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].vco3_eg.state == STATE_IDLE && voices[v].vco4_eg.state == STATE_IDLE) { slot = v; break; } }
        if (slot == -1) {
            // Polyphony ceiling hit (active_voice_limit simultaneously active voices) -- every
            // slot is busy. FIXED: this used to unconditionally steal slot 0, resetting
            // whatever was playing there regardless of how audible it currently was -- a hard,
            // audible click/cutoff on an arbitrary note, every time. Instead, steal the voice a
            // listener is least likely to notice disappearing: prefer one already in RELEASE
            // (fading out on its own) with the lowest combined level, and only fall back to a
            // still-sustaining/attacking voice (by lowest level) if literally none are
            // releasing.
            float best_score = 1e30f; // lower = safer to steal
            for (int v = 0; v < voice_limit; v++) {
                float level = voices[v].vco1_eg.current_level + voices[v].vco2_eg.current_level + voices[v].vco3_eg.current_level + voices[v].vco4_eg.current_level;
                bool releasing = (voices[v].vco1_eg.state == STATE_RELEASE || voices[v].vco2_eg.state == STATE_RELEASE || voices[v].vco3_eg.state == STATE_RELEASE || voices[v].vco4_eg.state == STATE_RELEASE);
                // A releasing voice always beats a non-releasing one regardless of level --
                // it's already fading out by the player's own intent (key lifted), so cutting
                // it a little early reads as natural rather than as a glitch.
                float score = releasing ? level : (level + 1000.0f);
                if (score < best_score) { best_score = score; slot = v; }
            }
            // DIAGNOSTIC: confirms whether you're actually hitting the polyphony ceiling at
            // all -- if this never prints, the Polyphony control isn't the thing to look at for
            // an underrun/glitch you're chasing.
            fprintf(stderr, "[voices] polyphony ceiling hit (%d active) -- stole slot %d (score %.4f)\n", voice_limit, slot, best_score);
        }
    } else { slot = 0; }

    float target_hz = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    voices[slot].midi_note = note;
    voices[slot].midi_velocity = velocity;
    voices[slot].vco1_target_freq = target_hz;
    voices[slot].vco2_target_freq = target_hz;
    voices[slot].vco3_target_freq = target_hz;
    voices[slot].vco4_target_freq = target_hz;
    // Any note-on onto this slot makes a previously-deferred sustain release moot -- either the
    // slot was stolen from a sustain-pending voice (about to be fully reset below regardless),
    // or this is a fresh key that has nothing to do with whatever the pedal was holding.
    voices[slot].sustain_pending = false;

    // Mono Legato: only retrigger (reset phase/filters/envelopes) when this is genuinely the
    // first key of a phrase. If another key is already held when this one comes down, it's an
    // overlapping legato note -- just glide pitch (portamento) into the still-running envelope.
    bool is_first_key_of_phrase = poly_mode_active || (mono_stack->count == 0);

    if (is_first_key_of_phrase) {
        voices[slot].vco1_phase = 0.0f; voices[slot].vco2_phase = 0.0f; voices[slot].vco3_phase = 0.0f; voices[slot].vco4_phase = 0.0f;
        voices[slot].vco1_prev_fm_phase = 0.0f; voices[slot].vco3_prev_fm_phase = 0.0f; voices[slot].vco4_prev_fm_phase = 0.0f;
        voices[slot].vco1_tri_state = 0.0f; voices[slot].vco2_tri_state = 0.0f; voices[slot].vco3_tri_state = 0.0f; voices[slot].vco4_tri_state = 0.0f;
        voices[slot].vco2_pending_sync_correction = 0.0f; voices[slot].vco3_pending_sync_correction = 0.0f; voices[slot].vco4_pending_sync_correction = 0.0f;
        voices[slot].vco1_current_freq = target_hz; voices[slot].vco2_current_freq = target_hz;
        voices[slot].vco3_current_freq = target_hz; voices[slot].vco4_current_freq = target_hz;
        voices[slot].glide_time_multiplier = 1.0f; // fresh note -- no glide happening, nothing to randomize

        // WAVE_SAMPLE: (re)claim a DFD stream for whichever oscillator is currently set to it,
        // and release any stream a stolen/retriggered voice was still holding from a previous
        // note even if this new note doesn't need one -- see start_sample_osc()'s comment.
        if ((WaveType)((int)knobs->vco1_shape) == WAVE_SAMPLE) {
            start_sample_osc(&voices[slot].vco1_sample_stream, &voices[slot].vco1_sample_ptr, &voices[slot].vco1_sample_preroll, note, velocity);
        } else if (voices[slot].vco1_sample_stream >= 0) {
            release_sample_stream(voices[slot].vco1_sample_stream);
            voices[slot].vco1_sample_stream = -1;
        }
        if ((WaveType)((int)knobs->vco2_shape) == WAVE_SAMPLE) {
            start_sample_osc(&voices[slot].vco2_sample_stream, &voices[slot].vco2_sample_ptr, &voices[slot].vco2_sample_preroll, note, velocity);
        } else if (voices[slot].vco2_sample_stream >= 0) {
            release_sample_stream(voices[slot].vco2_sample_stream);
            voices[slot].vco2_sample_stream = -1;
        }
        if ((WaveType)((int)knobs->vco3_shape) == WAVE_SAMPLE) {
            start_sample_osc(&voices[slot].vco3_sample_stream, &voices[slot].vco3_sample_ptr, &voices[slot].vco3_sample_preroll, note, velocity);
        } else if (voices[slot].vco3_sample_stream >= 0) {
            release_sample_stream(voices[slot].vco3_sample_stream);
            voices[slot].vco3_sample_stream = -1;
        }
        if ((WaveType)((int)knobs->vco4_shape) == WAVE_SAMPLE) {
            start_sample_osc(&voices[slot].vco4_sample_stream, &voices[slot].vco4_sample_ptr, &voices[slot].vco4_sample_preroll, note, velocity);
        } else if (voices[slot].vco4_sample_stream >= 0) {
            release_sample_stream(voices[slot].vco4_sample_stream);
            voices[slot].vco4_sample_stream = -1;
        }
        for(int c=0; c<2; c++) { for(int f=0; f<3; f++) { voices[slot].fa_low[c][f][0]=0.0f; voices[slot].fa_low[c][f][1]=0.0f; voices[slot].fa_band[c][f][0]=0.0f; voices[slot].fa_band[c][f][1]=0.0f; } voices[slot].fb_low[c][0]=0.0f; voices[slot].fb_low[c][1]=0.0f; voices[slot].fb_band[c][0]=0.0f; voices[slot].fb_band[c][1]=0.0f; }
        for (int l = 0; l < LFO_COUNT; l++) { if (knobs->lfo[l].key_sync != 0.0f) global_lfo_phases[l] = 0.0f; }

        int m0 = (int)knobs->eg_mode[0]; voices[slot].vco1_eg.state = (m0 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco1_eg.current_level = (m0 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[0].al / 100.0f));
        int m1 = (int)knobs->eg_mode[1]; voices[slot].vco2_eg.state = (m1 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco2_eg.current_level = (m1 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[1].al / 100.0f));
        // Osc 3/4 Amp envelopes: eg[2]/eg_mode[2] and eg[3]/eg_mode[3].
        int mn = (int)knobs->eg_mode[2]; voices[slot].vco3_eg.state = (mn == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco3_eg.current_level = (mn == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[2].al / 100.0f));
        int m3 = (int)knobs->eg_mode[3]; voices[slot].vco4_eg.state = (m3 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco4_eg.current_level = (m3 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[3].al / 100.0f));
        for(int e=0; e<ASSIGNABLE_EG_COUNT; e++) {
            int me = (int)knobs->eg_mode[4 + e]; voices[slot].assign_eg[e].state = (me == 5) ? STATE_DELAY : STATE_ATTACK;
            voices[slot].assign_eg[e].current_level = (me == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->eg[4 + e].al / 100.0f));
        }
    } else {
        // An overlapping legato note: this is an actual audible glide, so reroll its time.
        voices[slot].glide_time_multiplier = roll_glide_multiplier(knobs->mono_glide_random);
    }
    if (!poly_mode_active) mono_stack_push(mono_stack, note, velocity);
}

// Puts a voice's envelopes into RELEASE from wherever they currently are. Factored out since
// it's needed from three places: a normal note-off (poly and mono), and releasing every voice
// that was deferred by the sustain pedal once it lifts (see handle_sustain_release).
static void trigger_voice_release(SynthVoice *voice) {
    voice->vco1_eg.release_level = voice->vco1_eg.current_level; voice->vco1_eg.state = STATE_RELEASE;
    voice->vco2_eg.release_level = voice->vco2_eg.current_level; voice->vco2_eg.state = STATE_RELEASE;
    voice->vco3_eg.release_level = voice->vco3_eg.current_level; voice->vco3_eg.state = STATE_RELEASE;
    voice->vco4_eg.release_level = voice->vco4_eg.current_level; voice->vco4_eg.state = STATE_RELEASE;
    for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) { voice->assign_eg[e].release_level = voice->assign_eg[e].current_level; voice->assign_eg[e].state = STATE_RELEASE; }
    // SFZ loop_sustain support: tell each oscillator's claimed sample stream (if any) that this
    // voice just entered release -- sample_disk_thread_main reads this to decide whether a
    // loop_release_exit zone should keep looping. A no-op for any oscillator not currently
    // playing a WAVE_SAMPLE zone (request_sample_stream_release ignores stream_index -1).
    request_sample_stream_release(voice->vco1_sample_stream);
    request_sample_stream_release(voice->vco2_sample_stream);
    request_sample_stream_release(voice->vco3_sample_stream);
    request_sample_stream_release(voice->vco4_sample_stream);
}

static void handle_note_off(SynthVoice voices[], const SynthSharedControls *knobs, const MidiLiveState *midi_live, int note, bool poly_mode_active, MonoNoteStack *mono_stack) {
    if (poly_mode_active) {
        int voice_limit = effective_voice_limit(knobs);
        for (int v = 0; v < voice_limit; v++) { if (voices[v].midi_note == note) {
            // Sustain pedal held: defer the actual release until it lifts (see
            // handle_sustain_release) instead of releasing right now.
            if (midi_live->sustain_held) voices[v].sustain_pending = true;
            else trigger_voice_release(&voices[v]);
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
        voices[0].midi_velocity = mono_stack->velocities[mono_stack->count - 1];
        voices[0].vco1_target_freq = target_hz;
        voices[0].vco2_target_freq = target_hz;
        voices[0].vco3_target_freq = target_hz;
        voices[0].vco4_target_freq = target_hz;
        // Falling back to a still-held note is also a real audible glide -- reroll its time too.
        voices[0].glide_time_multiplier = roll_glide_multiplier(knobs->mono_glide_random);
        // Falling back to a still-held note never retriggers or releases -- the sound never stopped.
    } else if (midi_live->sustain_held) {
        voices[0].sustain_pending = true;
    } else {
        trigger_voice_release(&voices[0]);
    }
}

// Called when the sustain pedal transitions held -> released: actually releases every voice
// that had a note-off deferred while it was held.
static void handle_sustain_release(SynthVoice voices[], const SynthSharedControls *knobs) {
    int voice_limit = effective_voice_limit(knobs);
    for (int v = 0; v < voice_limit; v++) {
        if (voices[v].sustain_pending) {
            voices[v].sustain_pending = false;
            trigger_voice_release(&voices[v]);
        }
    }
}

// --- MIDI auto-connect: no more manually running aconnect ---
//
// Keeps track of which ALSA sequencer client:port pairs are currently subscribed to feed our
// own MIDI input port, so a full rescan (on startup, or when the user's midi_device selection
// changes) can cleanly tear down exactly what it previously connected before reconnecting under
// the new selection.
#define MAX_TRACKED_MIDI_SOURCES 32
typedef struct {
    int count;
    int client[MAX_TRACKED_MIDI_SOURCES];
    int port[MAX_TRACKED_MIDI_SOURCES];
} MidiSourceList;

static void midi_source_list_add(MidiSourceList *list, int client, int port) {
    if (list->count >= MAX_TRACKED_MIDI_SOURCES) return; // more than 32 simultaneous MIDI sources is not a real scenario
    list->client[list->count] = client;
    list->port[list->count] = port;
    list->count++;
}

// Drops every tracked entry for a given client (a client can, in principle, have contributed
// more than one port) -- used when that client disappears, so a later-reused client id can't be
// mistaken for the device that just left.
static void midi_source_list_remove_client(MidiSourceList *list, int client) {
    for (int i = 0; i < list->count; i++) {
        if (list->client[i] == client) {
            for (int j = i; j < list->count - 1; j++) { list->client[j] = list->client[j+1]; list->port[j] = list->port[j+1]; }
            list->count--;
            i--;
        }
    }
}

// Pure decision logic, deliberately factored out from the ALSA client-info lookup that feeds it
// so it can be unit-tested without a live sequencer connection. client_id==SND_SEQ_CLIENT_SYSTEM
// (0) is always rejected (the kernel's own Timer/Announce ports, not a MIDI source); a
// non-kernel (SND_SEQ_KERNEL_CLIENT is the type given to real hardware- or driver-backed clients
// -- USB MIDI class devices among them) client is rejected since it's some other application's
// virtual port, not a physical device; "Midi Through" is ALSA's own generic passthrough client,
// present on essentially every Linux install, and connecting to it would just be a no-op patch
// cable to nothing, so it's rejected by name. pref NULL/""/"auto" means auto-detect (matches any
// client that clears those checks); anything else must match `name` exactly.
static bool midi_client_should_autoconnect(int client_id, int client_type, const char *name, const char *pref) {
    if (client_id == SND_SEQ_CLIENT_SYSTEM) return false;
    if (client_type != SND_SEQ_KERNEL_CLIENT) return false;
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, "Midi Through") == 0) return false;
    bool auto_mode = (!pref || pref[0] == '\0' || strcmp(pref, "auto") == 0);
    return auto_mode || strcmp(name, pref) == 0;
}

// Same reasoning, factored out for the same testability: a port must be readable (a source we
// can receive FROM) and subscribable, and must not be marked NO_EXPORT (aconnect itself skips
// NO_EXPORT ports when listing connectable clients -- they're for the owning application's
// internal use only).
static bool midi_port_caps_are_connectable_source(unsigned int caps) {
    return (caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT);
}

// Looks up client_id's name/type live and delegates to midi_client_should_autoconnect(). Split
// out from that pure function only because this half needs a live seq handle.
static bool midi_client_matches(snd_seq_t *seq, int client_id, const char *pref, char *name_out, size_t name_out_len) {
    snd_seq_client_info_t *info;
    snd_seq_client_info_alloca(&info);
    if (snd_seq_get_any_client_info(seq, client_id, info) < 0) return false;
    const char *name = snd_seq_client_info_get_name(info);
    bool matches = midi_client_should_autoconnect(client_id, snd_seq_client_info_get_type(info), name, pref);
    if (matches && name_out) snprintf(name_out, name_out_len, "%s", name ? name : "");
    return matches;
}

// Finds the first connectable source port on a client and subscribes our port to it (i.e.
// "connects from" it, in aconnect's terminology). Best-effort: does nothing if the client has
// no such port, or the subscription fails (e.g. it raced and just disappeared).
static void midi_connect_client(snd_seq_t *seq, int our_port, int client_id, MidiSourceList *tracked) {
    snd_seq_port_info_t *pinfo;
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_port_info_set_client(pinfo, client_id);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(seq, pinfo) >= 0) {
        if (!midi_port_caps_are_connectable_source(snd_seq_port_info_get_capability(pinfo))) continue;
        int port = snd_seq_port_info_get_port(pinfo);
        if (snd_seq_connect_from(seq, our_port, client_id, port) == 0) {
            midi_source_list_add(tracked, client_id, port);
            fprintf(stderr, "MIDI: connected from client %d:%d\n", client_id, port);
        }
        return; // one port per client is enough -- most keyboards expose exactly one anyway
    }
}

// Full rescan: drops every previously auto-connected source, then reconnects everything that
// currently matches `midi_device_pref` ("auto", or an exact client name pinned from the Audio
// tab). Used at startup and whenever that preference changes live.
static void midi_autoconnect_scan(snd_seq_t *seq, int our_port, const char *midi_device_pref, MidiSourceList *tracked) {
    for (int i = 0; i < tracked->count; i++) snd_seq_disconnect_from(seq, our_port, tracked->client[i], tracked->port[i]);
    tracked->count = 0;

    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client_id = snd_seq_client_info_get_client(cinfo);
        if (midi_client_matches(seq, client_id, midi_device_pref, NULL, 0)) {
            midi_connect_client(seq, our_port, client_id, tracked);
        }
    }
}

// our_midi_port/midi_sources are only needed for hotplug auto-connect (SND_SEQ_EVENT_PORT_START/
// PORT_EXIT) -- these arrive on the same event stream as NOTEON/NOTEOFF because main() subscribes
// our port to the ALSA announce port at startup (see main()'s comment on that subscription).
static void process_midi_input(snd_seq_t *seq_handle, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], bool poly_mode_active, MonoNoteStack *mono_stack, int our_midi_port, MidiSourceList *midi_sources, MidiLiveState *midi_live) {
    snd_seq_event_t *ev = NULL;
    while (snd_seq_event_input_pending(seq_handle, 1) > 0) {
        snd_seq_event_input(seq_handle, &ev); if (ev == NULL) continue;
        if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
            int note = ev->data.note.note;
            if (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0) { handle_note_on(voices, knobs, global_lfo_phases, note, ev->data.note.velocity, poly_mode_active, mono_stack); }
            else { handle_note_off(voices, knobs, midi_live, note, poly_mode_active, mono_stack); }
        } else if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
            int cc_num = ev->data.control.param;
            if (cc_num >= 0 && cc_num < 128) midi_live->cc[cc_num] = fmaxf(0.0f, fminf(1.0f, ev->data.control.value / 127.0f));
            if (cc_num == 64) {
                // Sustain pedal. polarity: 0 = Normal (>=64 means held, essentially every real
                // pedal), 1 = Inverted (for the rare one wired backwards).
                bool raw_held = ev->data.control.value >= 64;
                bool now_held = (knobs->sustain_pedal_polarity != 0.0f) ? !raw_held : raw_held;
                if (midi_live->sustain_held && !now_held) handle_sustain_release(voices, knobs); // pedal just lifted
                midi_live->sustain_held = now_held;
            }
        } else if (ev->type == SND_SEQ_EVENT_PITCHBEND) {
            // ALSA's own sequencer convention: value is already signed, -8192..8191, 0 = centered.
            midi_live->pitch_bend = fmaxf(-1.0f, fminf(1.0f, ev->data.control.value / 8192.0f));
        } else if (ev->type == SND_SEQ_EVENT_CHANPRESS) {
            midi_live->channel_pressure = fmaxf(0.0f, fminf(1.0f, ev->data.control.value / 127.0f));
        } else if (ev->type == SND_SEQ_EVENT_PORT_START) {
            // A new ALSA sequencer client/port just appeared -- e.g. a USB MIDI keyboard was
            // just plugged in. Auto-connect immediately if it matches the current selection
            // (which reconnects a pinned device after unplug/replug too, not just "auto" mode).
            if (midi_client_matches(seq_handle, ev->data.addr.client, knobs->midi_device, NULL, 0)) {
                midi_connect_client(seq_handle, our_midi_port, ev->data.addr.client, midi_sources);
            }
        } else if (ev->type == SND_SEQ_EVENT_PORT_EXIT) {
            // Keep our bookkeeping in sync -- ALSA already tears down the subscription itself,
            // but a stale tracked entry could otherwise be mistaken for a different device that
            // later reuses the same now-free client id.
            midi_source_list_remove_client(midi_sources, ev->data.addr.client);
        }
        snd_seq_free_event(ev);
    }
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
static float render_audio_block(SynthVoice voices[], const SynthSharedControls *knobs, float global_lfo_phases[LFO_COUNT], const float *ext_in_buffer, int ext_in_channels, int output_channels, const MidiLiveState *midi_live, float *out_buffer) {
    float peak = 0.0f;
    // Each oscillator is panned to stereo AT THE MIXER, before the signal reaches Filter A/B --
    // Filter A/B then run as two fully independent mono filters (one per channel, see fa_low/
    // fb_low's [channel] dimension) so that placement survives filtering instead of collapsing
    // to mono first. Pan is a mod destination (targets 32/33/37/40 -- see the per-voice mod
    // accumulation below), so unlike most per-block constants here it's computed per-voice,
    // per-sample instead of once for the whole buffer.

    // PERF: Unison Saw's per-unison pan (cosf/sinf) only depends on the Width knob, which is
    // constant across the whole buffer -- computed once here instead of every sample, for
    // every voice, for both oscillators. See compute_unison_saw_pan's comment for the numbers.
    float vco1_unison_pan_l[UNISON_SAW_VOICES], vco1_unison_pan_r[UNISON_SAW_VOICES];
    float vco2_unison_pan_l[UNISON_SAW_VOICES], vco2_unison_pan_r[UNISON_SAW_VOICES];
    float vco3_unison_pan_l[UNISON_SAW_VOICES], vco3_unison_pan_r[UNISON_SAW_VOICES];
    float vco4_unison_pan_l[UNISON_SAW_VOICES], vco4_unison_pan_r[UNISON_SAW_VOICES];
    compute_unison_saw_pan(knobs->vco1_unison_saw_width, vco1_unison_pan_l, vco1_unison_pan_r);
    compute_unison_saw_pan(knobs->vco2_unison_saw_width, vco2_unison_pan_l, vco2_unison_pan_r);
    compute_unison_saw_pan(knobs->vco3_unison_saw_width, vco3_unison_pan_l, vco3_unison_pan_r);
    compute_unison_saw_pan(knobs->vco4_unison_saw_width, vco4_unison_pan_l, vco4_unison_pan_r);

    // Noise Osc's tilt-color filter: a simple one-pole low/high split with a fixed ~800Hz pivot,
    // linearly crossfaded by the Tilt knob. The coefficient depends only on SAMPLE_RATE and the
    // fixed pivot, so it's a compile-time-equivalent constant -- computed once here too.
    const float noise_tilt_coeff = 1.0f - expf(-2.0f * (float)M_PI * 800.0f / (float)SAMPLE_RATE);

    // Pitch Bend: applies equally to both oscillators (like a real keyboard's wheel bends the
    // whole voice, not one oscillator), with separately specifiable up/down range in semitones.
    // midi_live->pitch_bend is constant across this whole block (only process_midi_input, called
    // once before render_audio_block each block, updates it) so it's computed once here too.
    float pb_bend_cents = (midi_live->pitch_bend >= 0.0f)
        ? midi_live->pitch_bend * knobs->pb_range_up * 100.0f
        : midi_live->pitch_bend * knobs->pb_range_down * 100.0f;

    // One-sample-delayed feedback state for LFO Phase modulation: holds each LFO's phase-shift
    // contribution from other LFOs and from voice 0's assignable EGs (the reference source for
    // LFO modulation, since LFOs are a single resource shared across every voice rather than a
    // per-voice one). Function-local static is safe here -- this program runs one synth engine
    // instance on a single audio thread.
    static float lfo_phase_mod_state[LFO_COUNT] = {0.0f};
    // Same one-sample-delayed pattern, for LFO Rate and Depth as mod destinations (targets
    // 14-21/48-71 -- see lfo_rate_target_index/lfo_depth_target_index). Applied to rate_hz/
    // mod_depth in the main per-sample LFO loop below.
    static float lfo_rate_mod_state[LFO_COUNT] = {0.0f};
    static float lfo_depth_mod_state[LFO_COUNT] = {0.0f};

    // Control-rate decimation state for LFOs 3-16 (index 2-15) -- see LFO_DECIMATION_FACTOR's
    // comment. counter cycles 0..LFO_DECIMATION_FACTOR-1; at 0, last_value/target_value/phase are
    // all advanced by one full decimated step, then every sample in between linearly interpolates
    // from last_value to target_value. Function-local static for the same single-instance reason
    // as lfo_phase_mod_state above.
    static int lfo_decim_counter[LFO_COUNT] = {0};
    static float lfo_decim_last_value[LFO_COUNT] = {0.0f};
    static float lfo_decim_target_value[LFO_COUNT] = {0.0f};

    // FX bus engine state: persists across calls (and across FX_TYPE changes -- see
    // process_fx_chain) for the same "single synth engine instance" reason as above.
    static FXSlotEngine fx_engine[FX_SLOT_COUNT];
    static bool fx_engine_initialized = false;
    if (!fx_engine_initialized) { reset_fx_engine(fx_engine); fx_engine_initialized = true; }

    // Always-on master safety limiter gain, one per output pair (evaluated after every source
    // routed to that pair has been summed in -- see its use below). Persists across calls for the
    // same reason as the state above.
    static float limiter_gain[OUTPUT_PAIR_MAX];
    static bool limiter_gain_initialized = false;
    if (!limiter_gain_initialized) {
        for (int p = 0; p < OUTPUT_PAIR_MAX; p++) limiter_gain[p] = 1.0f;
        limiter_gain_initialized = true;
    }

    // FX slot type-change bookkeeping (free a slot's old effect state when its Type actually
    // changed) and "which sources own any FX slot at all" both only ever change in response to an
    // OSC control message -- drained once per BLOCK in main()'s loop, never mid-block -- so both
    // are computed here exactly once per block, not once per sample (previously this bookkeeping
    // ran inside process_fx_chain itself, once per SOURCE per SAMPLE: with SOURCE_COUNT=9 sources
    // now dispatched independently, that had silently become a 9x-per-sample-per-slot cost purely
    // from scanning for a type change that could not have happened since the last block anyway --
    // a real, measured underrun regression at any voice count, since it's a flat per-sample tax
    // paid regardless of patch complexity). See process_fx_chain()'s own comment for how the two
    // pieces fit together now.
    sync_fx_engine_types(knobs->fx, fx_engine);
    bool source_has_fx_slots[SOURCE_COUNT] = {false};
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        int src = (int)knobs->fx[s].fx_source;
        if (src >= 0 && src < SOURCE_COUNT) source_has_fx_slots[src] = true;
    }

    // Envelope rates: one EGDerivedRates per EG *config* (ENV_COUNT of them), shared by every
    // voice's own instance of that EG -- see compute_eg_derived_rates()'s own comment for why
    // this moved here (once per block) instead of being recomputed inside tick_adsr() itself
    // (once per voice per sample -- a redundancy that scaled directly, and expensively, with
    // both polyphony and envelope count).
    EGDerivedRates eg_rates[ENV_COUNT];
    for (int e = 0; e < ENV_COUNT; e++) {
        compute_eg_derived_rates(&knobs->eg[e], (int)knobs->eg_mode[e], &eg_rates[e]);
    }
    // Sustain-chase rate: not even knob-dependent (a pure function of SAMPLE_RATE), so computed
    // exactly once for the life of the process rather than once per block.
    static float eg_sustain_chase_rate;
    static bool eg_sustain_chase_rate_initialized = false;
    if (!eg_sustain_chase_rate_initialized) {
        eg_sustain_chase_rate = 1.0f - expf(-1.0f / (0.015f * SAMPLE_RATE));
        eg_sustain_chase_rate_initialized = true;
    }

    int voice_limit = effective_voice_limit(knobs);
    int block_size = effective_block_size(knobs);

    // Portamento glide coefficient, per voice: depends only on knobs->portamento_time_ms (a
    // knob, block-invariant) and voices[v].glide_time_multiplier (per-voice, but only rerolled at
    // a glide event in handle_note_on/off -- called once per block from main()'s loop, never
    // mid-block) -- so, same as the EG rates above, this only needs computing once per block, not
    // once per voice per SAMPLE. It was previously recomputed (with an expf() call) on every
    // sample for every active voice regardless of whether a glide was even in progress -- for a
    // held note between glide events that's potentially millions of samples of pure waste.
    float portamento_coeff[MAX_VOICES];
    for (int v = 0; v < voice_limit; v++) {
        float effective_glide_ms = fmaxf(1.0f, knobs->portamento_time_ms * voices[v].glide_time_multiplier);
        portamento_coeff[v] = 1.0f - expf(-1.0f / ((effective_glide_ms / 1000.0f) * SAMPLE_RATE));
    }

    // Static-pan fallback: pan1_l/r..pan4_l/r (below, per voice per sample) reduce to just
    // knobs->vcoN_pan -- the same value for every voice, every sample -- whenever nothing is
    // actively modulating that oscillator's pan (mod_vcoN_pan == 0, the common case) and it isn't
    // a WAVE_SAMPLE oscillator carrying its own zone pan offset. Precomputed once per block so the
    // per-voice per-sample loop can skip the cosf/sinf pair entirely in that case instead of
    // recomputing an unchanged angle up to MAX_VOICES x block_size times for nothing.
    float static_pan_angle1 = (fmaxf(-1.0f, fminf(1.0f, knobs->vco1_pan)) + 1.0f) * ((float)M_PI / 4.0f);
    float static_pan_angle2 = (fmaxf(-1.0f, fminf(1.0f, knobs->vco2_pan)) + 1.0f) * ((float)M_PI / 4.0f);
    float static_pan_angle3 = (fmaxf(-1.0f, fminf(1.0f, knobs->vco3_pan)) + 1.0f) * ((float)M_PI / 4.0f);
    float static_pan_angle4 = (fmaxf(-1.0f, fminf(1.0f, knobs->vco4_pan)) + 1.0f) * ((float)M_PI / 4.0f);
    float static_pan1_l = cosf(static_pan_angle1), static_pan1_r = sinf(static_pan_angle1);
    float static_pan2_l = cosf(static_pan_angle2), static_pan2_r = sinf(static_pan_angle2);
    float static_pan3_l = cosf(static_pan_angle3), static_pan3_r = sinf(static_pan_angle3);
    float static_pan4_l = cosf(static_pan_angle4), static_pan4_r = sinf(static_pan_angle4);

    for (int i = 0; i < block_size; i++) {
        float mixed_l = 0.0f, mixed_r = 0.0f;
        float lfo_out[LFO_COUNT] = {0.0f};
        for (int l = 0; l < LFO_COUNT; l++) {
            int wave_type = (int)knobs->lfo[l].wave_type;
            // Off: complete compute bypass -- no phase_offset/mod_source_cc/waveform lookup, and
            // no phase advance either (nothing else in this engine reads an LFO's raw phase, so
            // freezing it is safe and avoids wasted work on an LFO nobody's listening to -- this
            // is the actual CPU concern with 16 always-otherwise-running LFOs).
            if (wave_type == 4) { lfo_out[l] = 0.0f; continue; }

            // Rate/Depth as mod destinations (targets 14-21/48-71): one-sample-delayed
            // contributions from lfo_rate_mod_state/lfo_depth_mod_state (computed once per
            // sample, below the voice loop -- see lfo_rate_target_index's comment), applied on
            // top of this LFO's own knobs. Rate mod is scaled (a unity-depth modulator sweeps
            // +/-20Hz -- a musically useful range without letting one LFO drag another across
            // its whole 0.1-200Hz dial range); Depth mod adds directly (mod_depth is already a
            // plain 0-1 fraction, same convention mod_source_cc's multiplicative scale uses).
            // Both stay independent of and coexist with mod_source_cc's own CC/aftertouch scale.
            float effective_rate_hz = fmaxf(0.1f, fminf(200.0f, knobs->lfo[l].rate_hz + lfo_rate_mod_state[l] * 20.0f));
            float effective_mod_depth = fmaxf(0.0f, fminf(1.0f, knobs->lfo[l].mod_depth + lfo_depth_mod_state[l]));

            if (l < 2) {
                // LFOs 1/2: full audio-rate, unchanged from the original 4-LFO design.
                // Phase pot (0-360 degrees) plus any modulation, shifting only the read-phase
                // used for this sample's waveform lookup -- the underlying running phase
                // (advanced below) is never touched, so Rate/sync stay completely unaffected.
                float phase_offset = fmodf((knobs->lfo[l].phase_offset / 360.0f) + lfo_phase_mod_state[l], 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
                float read_phase = fmodf(global_lfo_phases[l] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
                // Modulation Source: scales this LFO's contribution by a live MIDI controller
                // (e.g. gating vibrato depth with the mod wheel) on top of the plain Depth knob.
                // Unassigned (-1, the default) is a pure no-op -- cc_source_scale stays 1.0.
                int mod_src = (int)knobs->lfo[l].mod_source_cc;
                float cc_source_scale = 1.0f;
                if (mod_src == 128) cc_source_scale = midi_live->channel_pressure;
                else if (mod_src >= 0 && mod_src < 128) cc_source_scale = midi_live->cc[mod_src];
                lfo_out[l] = lookup_lfo_waveform(wave_type, read_phase) * effective_mod_depth * cc_source_scale;
                global_lfo_phases[l] += effective_rate_hz / (float)SAMPLE_RATE;
                if (global_lfo_phases[l] >= 1.0f) global_lfo_phases[l] -= 1.0f;
            } else {
                // LFOs 3-16: control-rate decimated (see LFO_DECIMATION_FACTOR's comment). Only
                // recompute the waveform/phase every LFO_DECIMATION_FACTOR samples; every sample
                // in between linearly interpolates from the last recomputed value toward the new
                // one, avoiding a zipper-noise step at each recompute boundary.
                if (lfo_decim_counter[l] == 0) {
                    lfo_decim_last_value[l] = lfo_decim_target_value[l];
                    float phase_offset = fmodf((knobs->lfo[l].phase_offset / 360.0f) + lfo_phase_mod_state[l], 1.0f); if (phase_offset < 0.0f) phase_offset += 1.0f;
                    float read_phase = fmodf(global_lfo_phases[l] + phase_offset, 1.0f); if (read_phase < 0.0f) read_phase += 1.0f;
                    int mod_src = (int)knobs->lfo[l].mod_source_cc;
                    float cc_source_scale = 1.0f;
                    if (mod_src == 128) cc_source_scale = midi_live->channel_pressure;
                    else if (mod_src >= 0 && mod_src < 128) cc_source_scale = midi_live->cc[mod_src];
                    lfo_decim_target_value[l] = lookup_lfo_waveform(wave_type, read_phase) * effective_mod_depth * cc_source_scale;
                    // Phase advances by DECIMATION_FACTOR steps at once, since we're about to
                    // skip that many samples' worth of recomputation -- this keeps the LFO's
                    // real-world rate correct despite only sampling it every 24th sample.
                    global_lfo_phases[l] += (effective_rate_hz / (float)SAMPLE_RATE) * (float)LFO_DECIMATION_FACTOR;
                    if (global_lfo_phases[l] >= 1.0f) global_lfo_phases[l] = fmodf(global_lfo_phases[l], 1.0f);
                }
                float frac = (float)lfo_decim_counter[l] / (float)LFO_DECIMATION_FACTOR;
                lfo_out[l] = lfo_decim_last_value[l] + (lfo_decim_target_value[l] - lfo_decim_last_value[l]) * frac;
                lfo_decim_counter[l] = (lfo_decim_counter[l] + 1) % LFO_DECIMATION_FACTOR;
            }
        }

        for (int v = 0; v < voice_limit; v++) {
            if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].vco3_eg.state == STATE_IDLE && voices[v].vco4_eg.state == STATE_IDLE) continue;

            for(int e=0; e<ENV_COUNT; e++) {
                if(e==0) tick_adsr(&voices[v].vco1_eg, &eg_rates[e], eg_sustain_chase_rate);
                else if(e==1) tick_adsr(&voices[v].vco2_eg, &eg_rates[e], eg_sustain_chase_rate);
                else if(e==2) tick_adsr(&voices[v].vco3_eg, &eg_rates[e], eg_sustain_chase_rate);
                else if(e==3) tick_adsr(&voices[v].vco4_eg, &eg_rates[e], eg_sustain_chase_rate);
                else tick_adsr(&voices[v].assign_eg[e-4], &eg_rates[e], eg_sustain_chase_rate);
            }
            release_idle_sample_streams(&voices[v]); // WAVE_SAMPLE: free any DFD stream this voice's oscillator(s) no longer need

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
            float mod_vco1_pitch = knobs->vco1_fine_tune + knobs->vco1_pitch + pb_bend_cents;
            float mod_vco2_pitch = knobs->vco2_fine_tune + (knobs->vco2_pitch_offset * 100.0f) + knobs->vco2_pitch + pb_bend_cents;
            float mod_vco3_pitch = knobs->vco3_fine_tune + (knobs->vco3_pitch_offset * 100.0f) + knobs->vco3_pitch + pb_bend_cents;
            float mod_vco4_pitch = knobs->vco4_fine_tune + (knobs->vco4_pitch_offset * 100.0f) + knobs->vco4_pitch + pb_bend_cents;
            float mod_vco1_amp = 0.0f, mod_vco2_amp = 0.0f, fm_mod = 0.0f, mix_bal = 0.0f;
            // Filter cutoff/resonance mod destinations, 4 slots:
            //   slot 0 (targets 8/9)   = Filter A's PRIMARY filter: filter_a when Normal, or
            //                            filter_a_multi[0] (1st internal filter) when Multi
            //   slot 1 (targets 10/11) = Filter B
            //   slot 2 (targets 12/13) = filter_a_multi[2] (3rd internal filter, Multi mode only)
            //   slot 3 (targets 84/85) = filter_a_multi[1] (2nd/middle internal filter, Multi
            //                            mode only -- appended after target 47, the previously
            //                            unmoddable "middle sub-filter" gap this pass closes)
            // FIXED: resonance destinations (odd targets 9/11/13) used to fall into the same
            // slot as their neighboring Freq destination via integer division, so picking e.g.
            // "F1 Res" as an LFO destination silently modulated cutoff instead of resonance and
            // resonance itself was never actually modulatable by anything.
            float filter_cutoff_mods[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float filter_res_mods[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            // Phase Modulation (new): a bipolar mod value shifts where in the cycle each
            // oscillator currently reads from, without touching its actual running phase
            // accumulator (that distinction matters -- it keeps pitch, hard sync, and portamento
            // completely unaffected by phase modulation).
            float mod_vco1_phase = 0.0f, mod_vco2_phase = 0.0f, mod_vco3_phase = 0.0f, mod_vco4_phase = 0.0f;
            // Unison Saw Detune Width as a mod destination (new).
            float mod_vco1_unison_detune = 0.0f, mod_vco2_unison_detune = 0.0f, mod_vco3_unison_detune = 0.0f, mod_vco4_unison_detune = 0.0f;
            // Pan/Pulse Width/Noise/Sub as mod destinations (targets 32-48 -- see the
            // matrix_shared_routes comment in frontend.py for the full list). Pan mods are
            // applied directly (same convention as Phase); the rest get a scale factor sized so
            // a unity-depth LFO/EG sweeps the whole musically useful range.
            float mod_vco1_pan = 0.0f, mod_vco2_pan = 0.0f;
            float mod_vco1_pulse_width = 0.0f, mod_vco2_pulse_width = 0.0f;
            float mod_vco3_amp = 0.0f, mod_vco3_pan = 0.0f, mod_vco3_tilt = 0.0f;
            float mod_vco4_amp = 0.0f, mod_vco4_pan = 0.0f, mod_vco4_tilt = 0.0f;

            for (int l = 0; l < LFO_COUNT; l++) {
                int target = (int)knobs->lfo[l].dest_target;
                // PERF: target 0 ("None") is the common case for any LFO not actually routed
                // to a destination -- skip straight past the ~19-branch dispatch chain below
                // instead of evaluating every one of them for nothing. This loop runs per
                // ACTIVE voice per sample, so on a plain unmodulated patch this was burning a
                // measurable chunk of every block's CPU budget on comparisons that could never
                // match (found via real-world underrun diagnostics: a 33-voice plain sawtooth
                // patch, no modulation configured at all, was already blowing the block budget).
                if (target == 0) continue;
                float val = lfo_out[l];
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
                if (target == 32) mod_vco1_pan += val;
                if (target == 33) mod_vco2_pan += val;
                if (target == 34) mod_vco1_pulse_width += val * 50.0f;
                if (target == 35) mod_vco2_pulse_width += val * 50.0f;
                if (target == 36) mod_vco3_amp += val;
                if (target == 37) mod_vco3_pan += val;
                if (target == 38) mod_vco3_tilt += val * 100.0f;
                if (target == 39) mod_vco4_amp += val;
                if (target == 40) mod_vco4_pan += val;
                if (target == 41) mod_vco3_pitch += val * 1200.0f;
                if (target == 42) mod_vco4_pitch += val * 1200.0f;
                if (target == 43) mod_vco3_phase += val;
                if (target == 44) mod_vco4_phase += val;
                if (target == 45) mod_vco3_unison_detune += val * 2000.0f;
                if (target == 46) mod_vco4_unison_detune += val * 2000.0f;
                if (target == 47) mod_vco4_tilt += val * 100.0f;
                // Filter A-M2 (Multi mode's middle sub-filter) -- see filter_cutoff_mods' slot-3
                // comment above.
                if (target == 84) filter_cutoff_mods[3] += val * 5000.0f;
                if (target == 85) filter_res_mods[3] += val * 50.0f;
            }

            // FIXED: each assignable EG now routes using its own destination combo (eg_dest_target),
            // not the corresponding LFO's destination -- the GUI already had a per-EG destination
            // dropdown, but it was never wired to anything back here.
            for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) {
                int target = (int)knobs->eg_dest_target[e];
                // PERF: same early-out as the LFO loop above -- target 0 ("None") is the
                // common case for an assignable EG not routed anywhere.
                if (target == 0) continue;
                float val = voices[v].assign_eg[e].current_level * knobs->eg_intensity[e];
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
                if (target == 32) mod_vco1_pan += val;
                if (target == 33) mod_vco2_pan += val;
                if (target == 34) mod_vco1_pulse_width += val * 50.0f;
                if (target == 35) mod_vco2_pulse_width += val * 50.0f;
                if (target == 36) mod_vco3_amp += val;
                if (target == 37) mod_vco3_pan += val;
                if (target == 38) mod_vco3_tilt += val * 100.0f;
                if (target == 39) mod_vco4_amp += val;
                if (target == 40) mod_vco4_pan += val;
                if (target == 41) mod_vco3_pitch += val * 1200.0f;
                if (target == 42) mod_vco4_pitch += val * 1200.0f;
                if (target == 43) mod_vco3_phase += val;
                if (target == 44) mod_vco4_phase += val;
                if (target == 45) mod_vco3_unison_detune += val * 2000.0f;
                if (target == 46) mod_vco4_unison_detune += val * 2000.0f;
                if (target == 47) mod_vco4_tilt += val * 100.0f;
                if (target == 84) filter_cutoff_mods[3] += val * 5000.0f;
                if (target == 85) filter_res_mods[3] += val * 50.0f;
            }

            // Phase pot (0-360 degrees) plus any modulation, converted to a 0..1 fractional-cycle
            // offset. Wrapped defensively since modulation can push it negative or past 1.
            float phase_offset1 = fmodf((knobs->vco1_phase_offset / 360.0f) + mod_vco1_phase, 1.0f); if (phase_offset1 < 0.0f) phase_offset1 += 1.0f;
            float phase_offset2 = fmodf((knobs->vco2_phase_offset / 360.0f) + mod_vco2_phase, 1.0f); if (phase_offset2 < 0.0f) phase_offset2 += 1.0f;
            float phase_offset3 = fmodf((knobs->vco3_phase_offset / 360.0f) + mod_vco3_phase, 1.0f); if (phase_offset3 < 0.0f) phase_offset3 += 1.0f;
            float phase_offset4 = fmodf((knobs->vco4_phase_offset / 360.0f) + mod_vco4_phase, 1.0f); if (phase_offset4 < 0.0f) phase_offset4 += 1.0f;

            // Unison Saw Detune Width, base knob plus any modulation, clamped back into its valid range.
            float vco1_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco1_unison_saw_detune + mod_vco1_unison_detune));
            float vco2_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco2_unison_saw_detune + mod_vco2_unison_detune));
            float vco3_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco3_unison_saw_detune + mod_vco3_unison_detune));
            float vco4_unison_detune_final = fmaxf(0.0f, fminf(4200.0f, knobs->vco4_unison_saw_detune + mod_vco4_unison_detune));

            // Oscillator shapes, hoisted here (from their own call sites further below) purely so
            // the Pan block right after can tell whether a WAVE_SAMPLE zone's own Pan setting
            // should fold into the knob-based pan below -- both are plain reads of a knob value
            // with no ordering dependency on anything between here and their old declaration
            // sites, so hoisting is safe.
            WaveType vco1_shape_now = (WaveType)((int)knobs->vco1_shape);
            WaveType vco2_shape_now = (WaveType)((int)knobs->vco2_shape);
            WaveType vco3_shape_now = (WaveType)((int)knobs->vco3_shape);
            WaveType vco4_shape_now = (WaveType)((int)knobs->vco4_shape);
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
            float zone_pan1 = (vco1_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(voices[v].vco1_sample_stream) : 0.0f;
            float zone_pan2 = (vco2_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(voices[v].vco2_sample_stream) : 0.0f;
            float zone_pan3 = (vco3_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(voices[v].vco3_sample_stream) : 0.0f;
            float zone_pan4 = (vco4_shape_now == WAVE_SAMPLE) ? sample_osc_zone_pan_offset(voices[v].vco4_sample_stream) : 0.0f;
            float pan1_l = 0.0f, pan1_r = 0.0f, pan2_l = 0.0f, pan2_r = 0.0f;
            float pan3_l = 0.0f, pan3_r = 0.0f, pan4_l = 0.0f, pan4_r = 0.0f;
            // Fall back to the once-per-block static angle (see its own comment above) whenever
            // nothing is actually contributing on top of the plain Pan knob this sample -- skips
            // the cosf/sinf pair entirely in the common (unmodulated) case.
            if (!osc1_off) {
                if (mod_vco1_pan == 0.0f && zone_pan1 == 0.0f) { pan1_l = static_pan1_l; pan1_r = static_pan1_r; }
                else {
                    pan1_l = cosf((fmaxf(-1.0f, fminf(1.0f, knobs->vco1_pan + mod_vco1_pan + zone_pan1)) + 1.0f) * (M_PI / 4.0f));
                    pan1_r = sinf((fmaxf(-1.0f, fminf(1.0f, knobs->vco1_pan + mod_vco1_pan + zone_pan1)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc2_off) {
                if (mod_vco2_pan == 0.0f && zone_pan2 == 0.0f) { pan2_l = static_pan2_l; pan2_r = static_pan2_r; }
                else {
                    pan2_l = cosf((fmaxf(-1.0f, fminf(1.0f, knobs->vco2_pan + mod_vco2_pan + zone_pan2)) + 1.0f) * (M_PI / 4.0f));
                    pan2_r = sinf((fmaxf(-1.0f, fminf(1.0f, knobs->vco2_pan + mod_vco2_pan + zone_pan2)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc3_off) {
                if (mod_vco3_pan == 0.0f && zone_pan3 == 0.0f) { pan3_l = static_pan3_l; pan3_r = static_pan3_r; }
                else {
                    pan3_l = cosf((fmaxf(-1.0f, fminf(1.0f, knobs->vco3_pan + mod_vco3_pan + zone_pan3)) + 1.0f) * (M_PI / 4.0f));
                    pan3_r = sinf((fmaxf(-1.0f, fminf(1.0f, knobs->vco3_pan + mod_vco3_pan + zone_pan3)) + 1.0f) * (M_PI / 4.0f));
                }
            }
            if (!osc4_off) {
                if (mod_vco4_pan == 0.0f && zone_pan4 == 0.0f) { pan4_l = static_pan4_l; pan4_r = static_pan4_r; }
                else {
                    pan4_l = cosf((fmaxf(-1.0f, fminf(1.0f, knobs->vco4_pan + mod_vco4_pan + zone_pan4)) + 1.0f) * (M_PI / 4.0f));
                    pan4_r = sinf((fmaxf(-1.0f, fminf(1.0f, knobs->vco4_pan + mod_vco4_pan + zone_pan4)) + 1.0f) * (M_PI / 4.0f));
                }
            }

            // Pulse Width/Tilt (targets 34/35/38/47), base knob plus any modulation. Dual-duty:
            // clamped to [2,98] (percent duty) when the shape is Pulse, or [-100,100] (tilt
            // amount) when the shape is Noise -- the same range render_fm_oscillator's Noise
            // branch expects (it divides by 100 itself). Any other shape doesn't consume this
            // value at all, so the exact clamp choice there is moot.
            float vco1_pulse_width_final = (vco1_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, knobs->vco1_pulse_width + mod_vco1_pulse_width)) : fmaxf(2.0f, fminf(98.0f, knobs->vco1_pulse_width + mod_vco1_pulse_width));
            float vco2_pulse_width_final = (vco2_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, knobs->vco2_pulse_width + mod_vco2_pulse_width)) : fmaxf(2.0f, fminf(98.0f, knobs->vco2_pulse_width + mod_vco2_pulse_width));
            float vco3_pulse_width_final = (vco3_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, knobs->vco3_pulse_width + mod_vco3_tilt)) : fmaxf(2.0f, fminf(98.0f, knobs->vco3_pulse_width + mod_vco3_tilt));
            float vco4_pulse_width_final = (vco4_shape_now == WAVE_NOISE) ? fmaxf(-100.0f, fminf(100.0f, knobs->vco4_pulse_width + mod_vco4_tilt)) : fmaxf(2.0f, fminf(98.0f, knobs->vco4_pulse_width + mod_vco4_tilt));

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
            int hard_sync_mode = (int)knobs->hard_sync_mode;
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
            float unison_detune_arr[4] = { vco1_unison_detune_final, vco2_unison_detune_final, vco3_unison_detune_final, vco4_unison_detune_final };
            float hz_arr[4] = { hz1, hz2, hz3, hz4 };
            bool do_sync_arr[4] = { false, do_sync2, do_sync3, do_sync4 };
            float t_wrap_arr[4] = { 0.0f, t_wrap2, t_wrap3, t_wrap4 };
            float *prev_fm_phase_ptr[4] = { &voices[v].vco1_prev_fm_phase, &voices[v].vco2_prev_fm_phase, &voices[v].vco3_prev_fm_phase, &voices[v].vco4_prev_fm_phase };
            float *tri_state_ptr[4] = { &voices[v].vco1_tri_state, &voices[v].vco2_tri_state, &voices[v].vco3_tri_state, &voices[v].vco4_tri_state };
            float *tilt_lp_ptr[4] = { &voices[v].vco1_tilt_lp, &voices[v].vco2_tilt_lp, &voices[v].vco3_tilt_lp, &voices[v].vco4_tilt_lp };
            float *unison_phases_ptr[4] = { voices[v].vco1_unison_saw_phases, voices[v].vco2_unison_saw_phases, voices[v].vco3_unison_saw_phases, voices[v].vco4_unison_saw_phases };
            const float *unison_pan_l_ptr[4] = { vco1_unison_pan_l, vco2_unison_pan_l, vco3_unison_pan_l, vco4_unison_pan_l };
            const float *unison_pan_r_ptr[4] = { vco1_unison_pan_r, vco2_unison_pan_r, vco3_unison_pan_r, vco4_unison_pan_r };
            int *sample_stream_ptr[4] = { &voices[v].vco1_sample_stream, &voices[v].vco2_sample_stream, &voices[v].vco3_sample_stream, &voices[v].vco4_sample_stream };
            double *sample_ptr_ptr[4] = { &voices[v].vco1_sample_ptr, &voices[v].vco2_sample_ptr, &voices[v].vco3_sample_ptr, &voices[v].vco4_sample_ptr };
            bool *sample_preroll_ptr[4] = { &voices[v].vco1_sample_preroll, &voices[v].vco2_sample_preroll, &voices[v].vco3_sample_preroll, &voices[v].vco4_sample_preroll };
            float *pending_sync_correction_ptr[4] = { &vco1_dummy_sync_correction, &voices[v].vco2_pending_sync_correction, &voices[v].vco3_pending_sync_correction, &voices[v].vco4_pending_sync_correction };

            // Vol-based gain used ONLY for FM modulator scaling (ignores mute, matching the
            // original gain2_for_fm's "muting the sound must not silence the FM modulator"
            // design). Osc1/Osc2 deliberately exclude mix_bal here too (a final-mix-only control).
            float gain_for_fm_arr[4] = {
                (knobs->vco1_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco1_vol + mod_vco1_amp)),
                (knobs->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco2_vol + mod_vco2_amp)),
                (knobs->vco3_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco3_vol + mod_vco3_amp)),
                (knobs->vco4_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco4_vol + mod_vco4_amp)),
            };
            float eg_level_arr[4] = { voices[v].vco1_eg.current_level, voices[v].vco2_eg.current_level, voices[v].vco3_eg.current_level, voices[v].vco4_eg.current_level };

            int fm_algorithm_idx = (int)knobs->fm_algorithm;
            if (fm_algorithm_idx < 0) fm_algorithm_idx = 0;
            if (fm_algorithm_idx >= FM_ALGORITHM_COUNT) fm_algorithm_idx = FM_ALGORITHM_COUNT - 1;
            const FmAlgorithm *alg = &FM_ALGORITHMS[fm_algorithm_idx];

            float osc_out[4] = {0.0f, 0.0f, 0.0f, 0.0f}, osc_side_l[4] = {0.0f, 0.0f, 0.0f, 0.0f}, osc_side_r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float modulator_output[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int slot = 0; slot < 4; slot++) {
                int osc = alg->render_order[slot];
                if (shape_arr[osc] == WAVE_OFF) { *pending_sync_correction_ptr[osc] = 0.0f; continue; } // WAVE_OFF: skip render entirely (phase advances unconditionally below)
                float modulator_sum = 0.0f;
                for (int e = 0; e < alg->edge_count; e++) {
                    if (alg->edges[e].to_osc != osc) continue;
                    int src = alg->edges[e].from_osc;
                    float depth = *(const float*)((const char*)knobs + alg->edges[e].depth_offset);
                    if (alg->edges[e].depth_offset == offsetof(SynthSharedControls, fm_intensity)) depth += fm_mod;
                    depth = fmaxf(0.0f, depth);
                    modulator_sum += modulator_output[src] * depth;
                }
                osc_out[osc] = render_fm_oscillator(
                    shape_arr[osc], own_phase_arr[osc], dt_arr[osc], phase_offset_arr[osc], modulator_sum,
                    prev_fm_phase_ptr[osc], pulse_or_tilt_arr[osc], tri_state_ptr[osc], tilt_lp_ptr[osc],
                    noise_tilt_coeff, unison_phases_ptr[osc], unison_detune_arr[osc],
                    unison_pan_l_ptr[osc], unison_pan_r_ptr[osc],
                    sample_stream_ptr[osc], sample_ptr_ptr[osc], sample_preroll_ptr[osc], hz_arr[osc],
                    do_sync_arr[osc], t_wrap_arr[osc], pending_sync_correction_ptr[osc],
                    &osc_side_l[osc], &osc_side_r[osc]);
                modulator_output[osc] = osc_out[osc] * gain_for_fm_arr[osc] * eg_level_arr[osc];
            }

            // --- Stereo mixer: each oscillator is panned to L/R here, before anything reaches
            // the filters, so its placement survives filtering (Filter A/B run as two
            // independent mono filters, one per channel -- see the fa_low/fb_low state). Unison
            // Saw/Sample's own internal stereo pair (side_l/r) is used directly as the
            // oscillator's L/R image, with its Pan knob then applied as an overall balance shift
            // on top of that existing width, rather than collapsing it to mono first. A WAVE_OFF
            // oscillator naturally contributes exactly 0 here (its pan was never computed above,
            // staying 0.0f, and its osc_out/side stay 0.0f too since render was skipped). ---
            bool osc_uses_side[4] = {
                shape_arr[0] == WAVE_UNISON_SAW || shape_arr[0] == WAVE_SAMPLE,
                shape_arr[1] == WAVE_UNISON_SAW || shape_arr[1] == WAVE_SAMPLE,
                shape_arr[2] == WAVE_UNISON_SAW || shape_arr[2] == WAVE_SAMPLE,
                shape_arr[3] == WAVE_UNISON_SAW || shape_arr[3] == WAVE_SAMPLE,
            };
            float gain_audible_arr[4] = {
                (knobs->vco1_mute != 0.0f || knobs->vco1_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco1_vol - mix_bal + mod_vco1_amp)),
                (knobs->vco2_mute != 0.0f || knobs->vco2_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco2_vol + mix_bal + mod_vco2_amp)),
                (knobs->vco3_mute != 0.0f || knobs->vco3_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco3_vol + mod_vco3_amp)),
                (knobs->vco4_mute != 0.0f || knobs->vco4_vol <= 0.0f) ? 0.0f : fmaxf(0.0f, fminf(1.0f, knobs->vco4_vol + mod_vco4_amp)),
            };
            float pan_l_arr[4] = { pan1_l, pan2_l, pan3_l, pan4_l };
            float pan_r_arr[4] = { pan1_r, pan2_r, pan3_r, pan4_r };
            float osc_l[4], osc_r[4];
            for (int osc = 0; osc < 4; osc++) {
                float amp = gain_audible_arr[osc] * eg_level_arr[osc];
                osc_l[osc] = (osc_uses_side[osc] ? osc_side_l[osc] : osc_out[osc]) * amp * pan_l_arr[osc];
                osc_r[osc] = (osc_uses_side[osc] ? osc_side_r[osc] : osc_out[osc]) * amp * pan_r_arr[osc];
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

            // --- Filter A/B: Filter A is Normal (single filter) or Multi (3-filter engine);
            // Filter B is always a single Normal filter. Each runs once per channel -- every
            // channel keeps fully independent filter state (fa_low/fa_band/fb_low/fb_band's
            // [channel] dimension) so stereo placement isn't lost going through the filters.
            //
            // All 3 units of Filter A's Multi engine now have a dedicated mod destination (see
            // the comment by filter_cutoff_mods above) -- unit 1 (the middle sub-filter) reads
            // from slot 3 (targets 84/85), appended after target 47 in this pass.
            float multi_cutoff_mods[3] = { filter_cutoff_mods[0], filter_cutoff_mods[3], filter_cutoff_mods[2] };
            float multi_res_mods[3] = { filter_res_mods[0], filter_res_mods[3], filter_res_mods[2] };

            float a_out_l, a_out_r, b_out_l, b_out_r;
            int ab_mode = (int)knobs->filter_ab_routing_mode; // 0=Series A->B, 1=Series B->A, 2=Parallel
            if (ab_mode == 1) {
                // Series B -> A: run B first on the raw mix, then A on B's output -- the same
                // two filters as mode 0, just reversed, e.g. to put Filter A's Multi engine
                // after Filter B instead of before it.
                b_out_l = run_svf_filter(stage_in_l, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[0], voices[v].fb_band[0]);
                b_out_r = run_svf_filter(stage_in_r, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[1], voices[v].fb_band[1]);
                if ((int)knobs->filter_a_mode == 0) {
                    a_out_l = run_svf_filter(b_out_l, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[0][0], voices[v].fa_band[0][0]);
                    a_out_r = run_svf_filter(b_out_r, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[1][0], voices[v].fa_band[1][0]);
                } else {
                    a_out_l = run_filter_a_multi(b_out_l, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[0], voices[v].fa_band[0]);
                    a_out_r = run_filter_a_multi(b_out_r, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[1], voices[v].fa_band[1]);
                }
            } else {
                // Series A -> B (0), or Parallel (2): run A first on the raw mix.
                if ((int)knobs->filter_a_mode == 0) {
                    a_out_l = run_svf_filter(stage_in_l, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[0][0], voices[v].fa_band[0][0]);
                    a_out_r = run_svf_filter(stage_in_r, knobs->filter_a.cutoff + filter_cutoff_mods[0], knobs->filter_a.res_q + filter_res_mods[0], (int)knobs->filter_a.filter_type, (int)knobs->filter_a.slope_poles, voices[v].fa_low[1][0], voices[v].fa_band[1][0]);
                } else {
                    a_out_l = run_filter_a_multi(stage_in_l, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[0], voices[v].fa_band[0]);
                    a_out_r = run_filter_a_multi(stage_in_r, (int)knobs->filter_a_multi_routing, knobs->filter_a_multi, multi_cutoff_mods, multi_res_mods, voices[v].fa_low[1], voices[v].fa_band[1]);
                }
                // Series: B fed from A's output. Parallel: B fed straight from the mixer, same as A.
                float b_in_l = (ab_mode == 0) ? a_out_l : stage_in_l;
                float b_in_r = (ab_mode == 0) ? a_out_r : stage_in_r;
                b_out_l = run_svf_filter(b_in_l, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[0], voices[v].fb_band[0]);
                b_out_r = run_svf_filter(b_in_r, knobs->filter_b.cutoff + filter_cutoff_mods[1], knobs->filter_b.res_q + filter_res_mods[1], (int)knobs->filter_b.filter_type, (int)knobs->filter_b.slope_poles, voices[v].fb_low[1], voices[v].fb_band[1]);
            }

            // --- Combine A and B: either Series mode means whichever filter ran last is simply
            // the final stage; Parallel means both are heard side by side, blended by
            // filter_ab_blend (0=all A, 100=all B, 50=even -- matches the old fixed 50/50). ---
            if (ab_mode == 2) {
                float blend = fmaxf(0.0f, fminf(1.0f, knobs->filter_ab_blend / 100.0f));
                mixed_l += a_out_l * (1.0f - blend) + b_out_l * blend;
                mixed_r += a_out_r * (1.0f - blend) + b_out_r * blend;
            } else {
                mixed_l += (ab_mode == 1) ? a_out_l : b_out_l;
                mixed_r += (ab_mode == 1) ? a_out_r : b_out_r;
            }
        }
        // LFO Phase/Rate/Depth as mod destinations (targets 14-21/28-31/48-71/72-83): computed
        // once per sample (not per voice, since LFOs are one resource shared by every voice) from
        // other LFOs' outputs and voice 0's assignable EGs (the reference source, for the same
        // reason). Applied to affect the NEXT sample's read-phase/rate/depth above -- a
        // one-sample delay that avoids any ordering ambiguity between "which LFO's output is
        // current" within a single sample. See lfo_rate_target_index/lfo_depth_target_index/
        // lfo_phase_target_index's comment for the target-number -> LFO-index mapping.
        float new_lfo_phase_mod[LFO_COUNT] = {0.0f};
        float new_lfo_rate_mod[LFO_COUNT] = {0.0f};
        float new_lfo_depth_mod[LFO_COUNT] = {0.0f};
        for (int l = 0; l < LFO_COUNT; l++) {
            int target = (int)knobs->lfo[l].dest_target;
            int pidx = lfo_phase_target_index(target); if (pidx >= 0) new_lfo_phase_mod[pidx] += lfo_out[l];
            int ridx = lfo_rate_target_index(target); if (ridx >= 0) new_lfo_rate_mod[ridx] += lfo_out[l];
            int didx = lfo_depth_target_index(target); if (didx >= 0) new_lfo_depth_mod[didx] += lfo_out[l];
        }
        if (voices[0].vco1_eg.state != STATE_IDLE || voices[0].vco2_eg.state != STATE_IDLE) {
            for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) {
                int target = (int)knobs->eg_dest_target[e];
                float val = voices[0].assign_eg[e].current_level * knobs->eg_intensity[e];
                int pidx = lfo_phase_target_index(target); if (pidx >= 0) new_lfo_phase_mod[pidx] += val;
                int ridx = lfo_rate_target_index(target); if (ridx >= 0) new_lfo_rate_mod[ridx] += val;
                int didx = lfo_depth_target_index(target); if (didx >= 0) new_lfo_depth_mod[didx] += val;
            }
        }
        for (int l = 0; l < LFO_COUNT; l++) {
            lfo_phase_mod_state[l] = new_lfo_phase_mod[l];
            lfo_rate_mod_state[l] = new_lfo_rate_mod[l];
            lfo_depth_mod_state[l] = new_lfo_depth_mod[l];
        }

        // --- Multi-source bus dispatch ---
        // Source 0 (Program) is exactly mixed_l/r as computed above -- oscillators summed and run
        // through Filter A/B together, untouched by any of this. Each external-input pair (source
        // 1..EXT_IN_PAIR_COUNT) gets its own independent dry bus, tapped directly from its own
        // pair of capture channels (2p/2p+1) -- generalizes what used to be a single hardcoded tap
        // at capture channels 0/1, and is never summed into Program's own bus.
        float bus_l[SOURCE_COUNT], bus_r[SOURCE_COUNT];
        bus_l[0] = mixed_l;
        bus_r[0] = mixed_r;
        for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) {
            int src = 1 + p;
            bus_l[src] = 0.0f;
            bus_r[src] = 0.0f;
            if (knobs->ext_in_enable[p] != 0.0f && 2 * p < ext_in_channels) {
                // If this pair's second channel doesn't exist (e.g. a mono capture device, or an
                // odd channel count's last pair), duplicate the first into both L/R -- matches
                // this engine's existing mono-capture-device convention exactly (see the old
                // single-pair tap this generalizes, which did the same thing for channels 0/1).
                float l = ext_in_buffer[i * ext_in_channels + 2 * p];
                float r = (2 * p + 1 < ext_in_channels) ? ext_in_buffer[i * ext_in_channels + 2 * p + 1] : l;
                bus_l[src] = l * knobs->ext_in_gain[p];
                bus_r[src] = r * knobs->ext_in_gain[p];
            }
        }

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
            process_fx_chain(knobs->fx, fx_engine, (int)knobs->fx_source_routing_mode[src], src, &bus_l[src], &bus_r[src]);
        }

        // No extra fixed headroom pad here beyond master_volume itself -- there used to be a
        // hardcoded *0.15 (~-16.5dB) on top of it, left over from before the always-on limiter
        // below existed (it dates back to this engine's very first commit; the limiter came
        // later). That pad was this engine's ONLY protection against summing-related clipping
        // at the time, but the limiter now does that job properly (instant-attack, smooth-
        // release gain reduction, explicitly designed for "several voices lining up" and
        // "12 FX slots summed in Parallel routing" -- see its own comment right below), making
        // the old pad pure redundant attenuation: quietly cutting every patch by ~16.5dB more
        // than master_volume alone would, for no remaining safety benefit.
        // master_volume stays Program-only, applied here before Program's bus is summed into any
        // output-pair accumulator -- ext-in pairs rely solely on their own per-pair gain above as
        // their equivalent "volume" knob (unchanged from today's behavior).
        bus_l[0] *= knobs->master_volume;
        bus_r[0] *= knobs->master_volume;

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
    return peak;
}
// DIAGNOSTIC: active_voices_count and render_ms are purely informational (see their computation
// in main()'s loop) -- passed through just so an actual ALSA underrun/xrun can be reported
// alongside what the engine was doing at that exact moment, instead of a bare ALSA error with
// no way to tell whether it was a CPU-load problem (too many active voices / too expensive a
// patch to render in time) or something unrelated (OS scheduling jitter, another process,
// a flaky USB audio interface, etc).
static void write_audio_block(snd_pcm_t *pcm_handle, const float *buffer, int channels, snd_pcm_uframes_t frame_count, int active_voices_count, double render_ms) {
    // Guards the pathological case where even the fallback-to-last-known-good reopen in main()
    // failed (e.g. a USB audio interface was physically unplugged) -- silently drop this block
    // rather than crash; main() will keep retrying the device on the next control packet.
    if (!pcm_handle) return;
    snd_pcm_uframes_t written = 0;
    while (written < frame_count && g_synth_running) {
        snd_pcm_sframes_t res = snd_pcm_writei(pcm_handle, buffer + (written * (size_t)channels), frame_count - written);
        if (res < 0) {
            fprintf(stderr, "[audio] ALSA underrun/xrun (%s) -- %d active voices, last render_audio_block took %.3fms\n",
                    snd_strerror((int)res), active_voices_count, render_ms);
            res = snd_pcm_recover(pcm_handle, res, 0); if (res < 0) break;
        }
        else { written += res; }
    }
}

// Queries how many capture channels an ALSA device actually offers (opens it briefly just to
// ask, then closes it again -- ALSA has no "ask without opening" API). Used to size the raw
// capture stream/buffer for the Recording tab's per-channel checkboxes: "checkboxes per channel
// detected" needs a real detected count, not a guess. Falls back to 2 (plain stereo) on any
// failure -- an unopenable device will fail again, loudly, when open_pcm_stream() itself tries.
static int query_device_channel_count(const char *device_name) {
    snd_pcm_t *probe = NULL;
    if (snd_pcm_open(&probe, device_name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0) return 2;
    snd_pcm_hw_params_t *hw_params = NULL;
    int result = 2;
    if (snd_pcm_hw_params_malloc(&hw_params) >= 0) {
        if (snd_pcm_hw_params_any(probe, hw_params) >= 0) {
            unsigned int max_channels = 2;
            if (snd_pcm_hw_params_get_channels_max(hw_params, &max_channels) >= 0) {
                if (max_channels < 1) max_channels = 1;
                if (max_channels > (unsigned int)RECORD_MAX_CHANNELS) max_channels = (unsigned int)RECORD_MAX_CHANNELS;
                result = (int)max_channels;
            }
        }
        snd_pcm_hw_params_free(hw_params);
    }
    snd_pcm_close(probe);
    return result;
}

// Symmetric with query_device_channel_count() above, but for the playback stream -- used to size
// the output PCM stream/buffer for the multi-source routing model's output-pair grid (see
// SynthSharedControls.source_routing and OUTPUT_PAIR_MAX): a class-compliant multichannel audio
// interface's extra outputs need to actually be reachable, not just its first stereo pair. Falls
// back to 2 (plain stereo) on any failure, same convention as the input side.
static int query_output_device_channel_count(const char *device_name) {
    snd_pcm_t *probe = NULL;
    if (snd_pcm_open(&probe, device_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return 2;
    snd_pcm_hw_params_t *hw_params = NULL;
    int result = 2;
    if (snd_pcm_hw_params_malloc(&hw_params) >= 0) {
        if (snd_pcm_hw_params_any(probe, hw_params) >= 0) {
            unsigned int max_channels = 2;
            if (snd_pcm_hw_params_get_channels_max(hw_params, &max_channels) >= 0) {
                if (max_channels < 1) max_channels = 1;
                if (max_channels > (unsigned int)OUTPUT_MAX_CHANNELS) max_channels = (unsigned int)OUTPUT_MAX_CHANNELS;
                result = (int)max_channels;
            }
        }
        snd_pcm_hw_params_free(hw_params);
    }
    snd_pcm_close(probe);
    return result;
}

// (Re)opens a float PCM stream by ALSA device name, closing *handle first if it was already
// open. Used both at startup and whenever the Audio tab's device fields change live (see
// main()'s loop) -- a single place so open/configure/error-reporting can't drift between the
// call sites. `channels` is 2 for the (always-stereo) output stream, and whatever
// query_device_channel_count() detected for the capture stream -- see the Recording tab.
// BUG FIX: the original startup code never checked snd_pcm_open()/snd_pcm_set_params()'s
// return values at all -- an unavailable "default" device (no sound card, no PulseAudio/ALSA
// routing) left pcm_handle uninitialized/unusable and the very first snd_pcm_writei() call
// would segfault or hang instead of failing with a clear message.
static bool open_pcm_stream(const char *device_name, snd_pcm_stream_t stream, snd_pcm_t **handle, float latency_ms, int channels) {
    if (*handle) { snd_pcm_close(*handle); *handle = NULL; }
    int err = snd_pcm_open(handle, device_name, stream, 0);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to open: %s\n", stream == SND_PCM_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
        *handle = NULL;
        return false;
    }
    // LATENCY: this target -- not BUFFER_SIZE -- is what actually sets the floor on output
    // latency. BUFFER_SIZE (128 samples = ~2.7ms) only controls how small the chunks are that
    // we hand to ALSA each main-loop iteration (so a fresh MIDI event waits at most one block
    // before it's reflected in the stream, plus small per-write syscall overhead) -- it does
    // NOT change how deep the ALSA ring buffer itself is. Lowering latency_ms shrinks the
    // safety margin against underruns/xruns (audible crackling/dropouts) exactly when CPU load
    // spikes hardest -- dense chords, heavy Unison Saw + Filter A Multi mode + lots of LFO/EG
    // modulation. It's user-adjustable (SynthSharedControls.audio_latency_ms, the Audio tab's
    // Latency control) specifically because that safety/responsiveness tradeoff is a real one
    // that depends on the target hardware's own headroom, not something to freeze at one value
    // for every machine this ever runs on.
    unsigned int latency_us = (unsigned int)fmaxf(1000.0f, latency_ms * 1000.0f);
    err = snd_pcm_set_params(*handle, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned int)channels, SAMPLE_RATE, 1, latency_us);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to configure: %s\n", stream == SND_PCM_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
        snd_pcm_close(*handle);
        *handle = NULL;
        return false;
    }
    return true;
}

// --- WAV recording (Recording tab) ---
//
// Writes 32-bit float PCM (WAVE_FORMAT_IEEE_FLOAT, format tag 3) -- matches the engine's own
// native sample format exactly, so no int conversion/dithering is needed, and is natively
// readable by every modern DAW/editor (Audacity, REAPER, ffmpeg, etc). RIFF/data/fact chunk
// sizes are written as placeholders and patched in on stop, once the real length is known.
//
// REALTIME SAFETY: every function in this section that touches the filesystem (fopen/fwrite/
// fseek/fclose) runs ONLY on the dedicated writer thread below (record_writer_thread_main), never
// on the main audio thread. Buffered stdio's fwrite() is not bounded-time-safe -- it periodically
// triggers a blocking write(2) syscall once its internal buffer fills, and disk I/O latency
// (especially USB/SD/network storage, or under system I/O contention) is not bounded the way the
// audio thread's per-block real-time budget requires. A stall there would show up as exactly the
// kind of underrun this file works hard to avoid everywhere else. So the audio thread only ever
// does bounded, allocation-free work for recording: computing the gather/downmix arithmetic
// (wav_recorder_gather_block, pure math, no I/O) and a wait-free ring-buffer push
// (record_ring_push, never blocks -- see RecordRing). All the actual file I/O, including opening
// the file and writing the header on start and patching/closing it on stop, happens on the writer
// thread instead, driven by a tiny atomic command handshake (RecordEngine) from the audio thread.
typedef struct {
    FILE *file;
    int channels;
    uint32_t frames_written;
    bool active;
    long data_size_offset;  // ftell() position of the "data" chunk's 4-byte size field
    long fact_count_offset; // ftell() position of the "fact" chunk's 4-byte sample-count field

    // Channel selection is frozen at record-start time from the live knobs, and used for every
    // block written until the recording stops -- so toggling a checkbox mid-take can't desync
    // the interleaving the file header already committed to. Start a new recording to pick up
    // a changed selection.
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count; // detected_input_channels at record-start, for indexing capture_buffer's stride
} WavRecorder;

// Lock-free single-producer/single-consumer ring buffer of raw interleaved float samples, used
// to hand recorded audio from the audio thread (producer) to the writer thread (consumer)
// without either side ever blocking on the other. Capacity is a power of two so index wraparound
// is a cheap mask; read_idx/write_idx are monotonically increasing counters (never actually
// wrapped themselves), which is what makes "how much is available/free" a plain subtraction with
// no ambiguity between empty and full. 1<<22 floats (16MB) gives ~4.8s of backlog headroom even
// at the worst case of 18 channels @ 48kHz -- far more than any realistic disk stall -- while
// still being cheap enough to just keep as a static allocation (see the "static" rationale on
// capture_buffer/ext_in_stereo above: this is far too large to put on main()'s stack).
#define RECORD_RING_CAPACITY (1 << 22)
typedef struct {
    float data[RECORD_RING_CAPACITY];
    _Atomic size_t write_idx;       // only ever written by the audio thread (producer)
    _Atomic size_t read_idx;        // only ever written by the writer thread (consumer)
    _Atomic uint64_t dropped_samples; // diagnostic only: incremented by the producer when full
} RecordRing;

// Wait-free: never blocks, never loops waiting for the consumer. If there isn't room, the new
// data is dropped (and counted) rather than overwriting unread data or stalling the caller --
// on the audio thread, dropping a moment of the *recording* is far preferable to risking an
// audible dropout in the *live output*, which is the one thing that must never stall.
static void record_ring_push(RecordRing *ring, const float *data, size_t n) {
    if (n == 0) return;
    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);
    size_t used = w - r;
    size_t free_space = RECORD_RING_CAPACITY - used;
    if (n > free_space) {
        atomic_fetch_add_explicit(&ring->dropped_samples, n, memory_order_relaxed);
        return;
    }
    for (size_t i = 0; i < n; i++) ring->data[(w + i) & (RECORD_RING_CAPACITY - 1)] = data[i];
    atomic_store_explicit(&ring->write_idx, w + n, memory_order_release);
}

// Consumer side (writer thread only). Pops up to max_n samples into out[], returns how many were
// actually available. Never blocks -- if the ring is empty, returns 0 immediately.
static size_t record_ring_pop(RecordRing *ring, float *out, size_t max_n) {
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_relaxed);
    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_acquire);
    size_t available = w - r;
    size_t n = available < max_n ? available : max_n;
    for (size_t i = 0; i < n; i++) out[i] = ring->data[(r + i) & (RECORD_RING_CAPACITY - 1)];
    atomic_store_explicit(&ring->read_idx, r + n, memory_order_release);
    return n;
}

static void wav_write_u32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; fwrite(b, 1, 4, f); }
static void wav_write_u16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v>>8) }; fwrite(b, 1, 2, f); }

// Pure arithmetic, no I/O -- how many channels a given selection resolves to (0 means "nothing
// selected"). Used by the audio thread (to size its gather buffer synchronously, before the
// writer thread has even seen the start request) and by the writer thread's actual file-open
// (wav_recorder_start below), so both sides are guaranteed to agree on the channel count without
// either one having to wait on the other.
static int wav_recorder_compute_channels(bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    int selected = include_synth_output ? 2 : 0;
    for (int c = 0; c < input_channel_count; c++) if (input_channel_enable[c]) selected++;
    if (selected <= 0) return 0;
    return downmix_stereo ? 2 : selected;
}

// Computes the selected channel count and freezes the selection into *rec, opens the WAV file,
// and writes its header. Returns false (leaving *rec untouched, still inactive) if nothing is
// selected or the file can't be opened -- the caller should treat either as "did not start."
// Writer-thread-only: performs blocking file I/O.
static bool wav_recorder_start(WavRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    int channels = wav_recorder_compute_channels(include_synth_output, downmix_stereo, input_channel_enable, input_channel_count);
    if (channels <= 0) { fprintf(stderr, "[record] cannot start: no channels selected\n"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[record] failed to open '%s' for writing: %s\n", path, strerror(errno)); return false; }

    fwrite("RIFF", 1, 4, f); wav_write_u32(f, 0); // total size, patched on stop
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f); wav_write_u32(f, 18); // fmt chunk body size (extended, cbSize=0)
    wav_write_u16(f, 3);                            // wFormatTag: IEEE float
    wav_write_u16(f, (uint16_t)channels);
    wav_write_u32(f, SAMPLE_RATE);
    wav_write_u32(f, (uint32_t)(SAMPLE_RATE * channels * 4)); // avg bytes/sec
    wav_write_u16(f, (uint16_t)(channels * 4));                // block align
    wav_write_u16(f, 32);                                       // bits per sample
    wav_write_u16(f, 0);                                        // cbSize

    fwrite("fact", 1, 4, f); wav_write_u32(f, 4);
    rec->fact_count_offset = ftell(f);
    wav_write_u32(f, 0); // total frames, patched on stop

    fwrite("data", 1, 4, f);
    rec->data_size_offset = ftell(f);
    wav_write_u32(f, 0); // data byte count, patched on stop

    rec->file = f;
    rec->channels = channels;
    rec->frames_written = 0;
    rec->active = true;
    rec->sel_include_synth_output = include_synth_output;
    rec->sel_downmix_stereo = downmix_stereo;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) rec->sel_input_channel[c] = (c < input_channel_count) && input_channel_enable[c];
    rec->sel_input_channel_count = input_channel_count;
    return true;
}

static void wav_recorder_write(WavRecorder *rec, const float *interleaved, int num_frames) {
    if (!rec->active || num_frames <= 0) return;
    fwrite(interleaved, sizeof(float), (size_t)num_frames * (size_t)rec->channels, rec->file);
    rec->frames_written += (uint32_t)num_frames;
}

static void wav_recorder_stop(WavRecorder *rec) {
    if (!rec->active) return;
    long final_size = ftell(rec->file);
    uint32_t data_bytes = rec->frames_written * (uint32_t)rec->channels * 4;

    fseek(rec->file, 4, SEEK_SET);
    wav_write_u32(rec->file, (uint32_t)(final_size - 8)); // RIFF chunk size excludes "RIFF"+size itself

    fseek(rec->file, rec->fact_count_offset, SEEK_SET);
    wav_write_u32(rec->file, rec->frames_written);

    fseek(rec->file, rec->data_size_offset, SEEK_SET);
    wav_write_u32(rec->file, data_bytes);

    fclose(rec->file);
    rec->file = NULL;
    rec->active = false;
    fprintf(stderr, "[record] stopped: %u frames (%.2fs), %d channel(s)\n", rec->frames_written, rec->frames_written / (float)SAMPLE_RATE, rec->channels);
}

// --- WavPack recording: an additional lossless format alongside WAV, reusing the exact same
// RecordRing/writer-thread architecture unchanged -- same wait-free push from the audio thread,
// same writer-thread-only file I/O discipline as WavRecorder above. WavPack (like FLAC)
// losslessly compresses integer PCM, not float, so incoming float samples are converted to
// 24-bit (packed in int32_t) just before handing them to the encoder. Uses libwavpack's
// callback-based block output (WavpackOpenFileOutput) rather than the RIFF-style
// open-now/backpatch-on-stop approach WAV uses -- WavPack's own format has no equivalent
// backpatch step, so the encoder is simply flushed and closed on stop. ---
typedef struct {
    WavpackContext *ctx;
    FILE *file;
    int channels;
    uint32_t frames_written;
    bool active;
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count;
} WavPackRecorder;

// libwavpack's WavpackBlockOutput callback: just appends the encoder's already-formatted block
// bytes to our file. Writer-thread-only (the encoder calls this synchronously from inside
// WavpackPackSamples/WavpackFlushSamples, both called only from the writer thread).
static int wavpack_block_output(void *id, void *data, int32_t bcount) {
    FILE *f = (FILE *)id;
    return fwrite(data, 1, (size_t)bcount, f) == (size_t)bcount;
}

static bool wavpack_recorder_start(WavPackRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    int channels = wav_recorder_compute_channels(include_synth_output, downmix_stereo, input_channel_enable, input_channel_count);
    if (channels <= 0) { fprintf(stderr, "[record] cannot start (WavPack): no channels selected\n"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[record] failed to open '%s' for writing: %s\n", path, strerror(errno)); return false; }

    WavpackContext *ctx = WavpackOpenFileOutput(wavpack_block_output, f, NULL);
    if (!ctx) { fprintf(stderr, "[record] WavpackOpenFileOutput failed\n"); fclose(f); return false; }

    WavpackConfig config; memset(&config, 0, sizeof(config));
    config.bytes_per_sample = 3;   // 24-bit PCM, packed 3 bytes/sample into each int32_t
    config.bits_per_sample = 24;
    config.num_channels = channels;
    config.sample_rate = SAMPLE_RATE;

    // (uint32_t)-1 total_samples signals "length not known in advance" (this is a live, streamed
    // recording of unknown duration) -- WavPack supports this the same way its CLI does when
    // encoding from a pipe/stdin.
    if (!WavpackSetConfiguration(ctx, &config, (uint32_t)-1) || !WavpackPackInit(ctx)) {
        fprintf(stderr, "[record] WavPack configuration/init failed: %s\n", WavpackGetErrorMessage(ctx));
        WavpackCloseFile(ctx); fclose(f); return false;
    }

    rec->ctx = ctx;
    rec->file = f;
    rec->channels = channels;
    rec->frames_written = 0;
    rec->active = true;
    rec->sel_include_synth_output = include_synth_output;
    rec->sel_downmix_stereo = downmix_stereo;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) rec->sel_input_channel[c] = (c < input_channel_count) && input_channel_enable[c];
    rec->sel_input_channel_count = input_channel_count;
    return true;
}

static void wavpack_recorder_write(WavPackRecorder *rec, const float *interleaved, int num_frames) {
    if (!rec->active || num_frames <= 0) return;
    static int32_t pcm_buf[8192];
    int total_values = num_frames * rec->channels;
    int max_values = (int)(sizeof(pcm_buf) / sizeof(pcm_buf[0]));
    // Chunk defensively in case a single drain hands us more than pcm_buf can hold at once (the
    // writer thread's own pop_buf is already this size, so in practice this never loops more
    // than once, but there's no reason to assume that ordering holds forever).
    int offset = 0;
    while (offset < total_values) {
        int chunk_values = total_values - offset; if (chunk_values > max_values) chunk_values = max_values;
        // Round down to a whole number of frames so a chunk boundary never splits one frame's
        // channels across two WavpackPackSamples calls.
        chunk_values -= chunk_values % rec->channels;
        if (chunk_values <= 0) break;
        for (int i = 0; i < chunk_values; i++) {
            float s = interleaved[offset + i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            pcm_buf[i] = (int32_t)(s * 8388607.0f); // 24-bit signed full scale (2^23 - 1)
        }
        WavpackPackSamples(rec->ctx, pcm_buf, (uint32_t)(chunk_values / rec->channels));
        offset += chunk_values;
    }
    rec->frames_written += (uint32_t)num_frames;
}

static void wavpack_recorder_stop(WavPackRecorder *rec) {
    if (!rec->active) return;
    WavpackFlushSamples(rec->ctx);
    WavpackCloseFile(rec->ctx);
    rec->ctx = NULL;
    fclose(rec->file);
    rec->file = NULL;
    rec->active = false;
    fprintf(stderr, "[record] stopped (WavPack): %u frames (%.2fs), %d channel(s)\n", rec->frames_written, rec->frames_written / (float)SAMPLE_RATE, rec->channels);
}

// Audio-thread-side mirror of a WavRecorder's selection (NOT its file state -- the audio thread
// never touches a FILE*). Set synchronously on the record-start edge from the same live knobs
// the writer thread's copy is derived from, via the identical wav_recorder_compute_channels(), so
// the two sides can never disagree about the channel count despite computing it independently.
typedef struct {
    bool active;
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count;
    int channels;
} RecordGatherState;

typedef enum { RECORD_CMD_NONE = 0, RECORD_CMD_START, RECORD_CMD_STOP } RecordCommand;

// Cross-thread interface between the audio thread and the dedicated recording writer thread.
// The audio thread only ever: (1) writes cmd_* fields then publishes `command` with a release
// store, (2) pushes gathered audio into `ring` (wait-free), (3) reads the is_active/start_failed/
// frames_written status fields for the GUI's EngineStatus feedback. Everything else --
// wav_recorder_start/write/stop and all their file I/O -- happens only on the writer thread.
typedef struct {
    RecordRing ring;
    pthread_t thread;
    _Atomic int command;   // RecordCommand
    _Atomic bool shutdown;

    // Parameters for a pending START, valid once `command` is observed as RECORD_CMD_START by
    // the writer thread. Plain (non-atomic) fields are safe here because the release store to
    // `command` (audio thread) paired with the acquire exchange of `command` (writer thread)
    // establishes a happens-before edge covering everything written before that store.
    char cmd_path[RECORD_PATH_LEN];
    bool cmd_include_synth_output;
    bool cmd_downmix_stereo;
    bool cmd_input_channel_enable[RECORD_MAX_CHANNELS];
    int cmd_input_channel_count;
    int cmd_record_format; // RecordFormat: 0=WAV, 1=WavPack

    // Writer thread -> audio thread status, polled for the GUI's EngineStatus feedback.
    _Atomic bool is_active;
    _Atomic bool start_failed;
    _Atomic uint32_t frames_written;
} RecordEngine;

// Gathers one block's worth of frames from the synth's own post-FX stereo output and the raw
// N-channel capture buffer, per the SELECTION FROZEN AT RECORD-START (sel->sel_* -- not the live
// knobs, so a checkbox toggled mid-take can't desync the interleaving the writer thread's file
// header already committed to), and pushes the result into the ring for the writer thread to
// actually write to disk. `capture_buffer`'s actual stride must still match
// sel->sel_input_channel_count -- the caller is responsible for stopping the recording first if
// the live capture device's channel count has changed since it started (see the safety check in
// main()). Pure arithmetic plus a wait-free ring push -- no I/O, safe to call every block from
// the audio thread.
static void wav_recorder_gather_block(RecordGatherState *sel, RecordEngine *eng, const float *synth_buffer, int synth_channels, const float *capture_buffer, int block_size) {
    if (!sel->active) return;
    static float frame_buf[BUFFER_SIZE * RECORD_MAX_CHANNELS];
    int stride = sel->sel_input_channel_count;
    if (sel->sel_downmix_stereo) {
        // n = how many signals get summed into EACH side (not the total signal count): the
        // synth output contributes exactly one signal per side (synth_L into L, synth_R into
        // R -- never both into the same side), and every selected raw input channel (mono)
        // contributes one signal to BOTH sides. So "Synth Output only" is a pure passthrough
        // (n=1, scale=1.0, no attenuation) rather than needlessly getting -3dB as if two
        // signals were being summed on each side when only one actually is.
        int n = sel->sel_include_synth_output ? 1 : 0;
        for (int c = 0; c < stride; c++) if (sel->sel_input_channel[c]) n++;
        float scale = (n > 0) ? (1.0f / sqrtf((float)n)) : 1.0f;
        for (int i = 0; i < block_size; i++) {
            float l = 0.0f, r = 0.0f;
            if (sel->sel_include_synth_output) { l += synth_buffer[i*synth_channels]; r += synth_buffer[i*synth_channels+1]; }
            for (int c = 0; c < stride; c++) {
                if (sel->sel_input_channel[c]) { float s = capture_buffer[i*stride + c]; l += s; r += s; }
            }
            frame_buf[i*2] = l * scale;
            frame_buf[i*2+1] = r * scale;
        }
    } else {
        for (int i = 0; i < block_size; i++) {
            int ch = 0;
            if (sel->sel_include_synth_output) {
                frame_buf[i*sel->channels + ch++] = synth_buffer[i*synth_channels];
                frame_buf[i*sel->channels + ch++] = synth_buffer[i*synth_channels+1];
            }
            for (int c = 0; c < stride; c++) {
                if (sel->sel_input_channel[c]) frame_buf[i*sel->channels + ch++] = capture_buffer[i*stride + c];
            }
        }
    }
    record_ring_push(&eng->ring, frame_buf, (size_t)block_size * (size_t)sel->channels);
}

// Pops and writes everything currently available in the ring to whichever recorder is active
// (at most one of rec/wprec is ever active at a time -- only one file is open per take) -- or
// just discards it (if neither is active; e.g. a START is still in flight, or the recording
// already stopped) so the ring's indices keep moving and a producer that's slightly ahead of us
// doesn't spuriously see "full". Writer-thread-only.
static void record_writer_drain(RecordEngine *eng, WavRecorder *rec, WavPackRecorder *wprec) {
    static float pop_buf[8192];
    int channels = rec->active ? rec->channels : (wprec->active ? wprec->channels : 0);
    if (channels > 0) {
        size_t chunk_frames = (sizeof(pop_buf) / sizeof(float)) / (size_t)channels;
        size_t max_floats = chunk_frames * (size_t)channels;
        size_t n;
        while ((n = record_ring_pop(&eng->ring, pop_buf, max_floats)) > 0) {
            int num_frames = (int)(n / (size_t)channels);
            if (rec->active) { wav_recorder_write(rec, pop_buf, num_frames); atomic_store_explicit(&eng->frames_written, rec->frames_written, memory_order_relaxed); }
            else { wavpack_recorder_write(wprec, pop_buf, num_frames); atomic_store_explicit(&eng->frames_written, wprec->frames_written, memory_order_relaxed); }
        }
    } else {
        while (record_ring_pop(&eng->ring, pop_buf, sizeof(pop_buf) / sizeof(float)) > 0) {}
    }
}

// Entry point for the dedicated recording writer thread -- owns the ONLY WavRecorder/
// WavPackRecorder instances that ever touch a real FILE*. Polls for a command every
// RECORD_WRITER_POLL_MS via nanosleep (rather than being condvar-signaled from the audio thread)
// specifically to avoid putting even a brief mutex/futex wait on the audio thread's side of the
// handshake; the audio thread's only synchronization primitives are the wait-free ring and plain
// atomic loads/stores.
#define RECORD_WRITER_POLL_MS 5
static void *record_writer_thread_main(void *arg) {
    RecordEngine *eng = (RecordEngine *)arg;
    WavRecorder rec = {0};
    WavPackRecorder wprec = {0};

    for (;;) {
        int cmd = atomic_exchange_explicit(&eng->command, RECORD_CMD_NONE, memory_order_acquire);
        if (cmd == RECORD_CMD_START) {
            if (rec.active) wav_recorder_stop(&rec); // shouldn't happen, but never leak a handle
            if (wprec.active) wavpack_recorder_stop(&wprec);
            bool ok;
            if (eng->cmd_record_format == 1) {
                ok = wavpack_recorder_start(&wprec, eng->cmd_path, eng->cmd_include_synth_output, eng->cmd_downmix_stereo,
                                             eng->cmd_input_channel_enable, eng->cmd_input_channel_count);
            } else {
                ok = wav_recorder_start(&rec, eng->cmd_path, eng->cmd_include_synth_output, eng->cmd_downmix_stereo,
                                         eng->cmd_input_channel_enable, eng->cmd_input_channel_count);
            }
            atomic_store_explicit(&eng->frames_written, 0, memory_order_relaxed);
            if (!ok) atomic_store_explicit(&eng->start_failed, true, memory_order_relaxed);
            atomic_store_explicit(&eng->is_active, rec.active || wprec.active, memory_order_release);
        } else if (cmd == RECORD_CMD_STOP) {
            if (rec.active) wav_recorder_stop(&rec);
            if (wprec.active) wavpack_recorder_stop(&wprec);
            atomic_store_explicit(&eng->is_active, false, memory_order_release);
        }

        record_writer_drain(eng, &rec, &wprec);

        bool shutdown_requested = atomic_load_explicit(&eng->shutdown, memory_order_acquire);
        if (shutdown_requested && atomic_load_explicit(&eng->command, memory_order_acquire) == RECORD_CMD_NONE) {
            record_writer_drain(eng, &rec, &wprec); // final catch-all in case the audio thread pushed between our drain and observing shutdown
            if (rec.active) wav_recorder_stop(&rec);
            if (wprec.active) wavpack_recorder_stop(&wprec);
            break;
        }

        struct timespec poll_delay = { .tv_sec = 0, .tv_nsec = RECORD_WRITER_POLL_MS * 1000L * 1000L };
        nanosleep(&poll_delay, NULL);
    }
    return NULL;
}

// Status feedback (see vu_decimator below) -- used to be a bare peak float; grew into this small
// fixed struct so the Recording tab can show live state (is a take actually running, how long,
// did the last start attempt fail) and the detected input channel count. Sent as one OSC bundle
// (send_osc_status()) to the configured --osc-reply-host/--osc-reply-port rather than raw bytes
// over a connected socket -- this struct is now just a convenient in-memory container for the
// values between computing them and handing them to send_osc_status().
typedef struct {
    float peak;
    float is_recording;
    float elapsed_seconds;
    float record_error;
    float detected_input_channels;
    float detected_output_channels;
    float tempo_bpm_live; // compute_current_tempo_bpm()'s result -- equals tempo_bpm whenever no tempo markers are programmed
} EngineStatus;

// Sends the GUI-status snapshot as one OSC bundle (one UDP datagram, 7 messages) to the reply
// address configured at startup. Fire-and-forget, same as the rest of this UDP-based protocol:
// if nothing is listening on the reply port, lo_send_bundle simply has nowhere to deliver to --
// a no-op, not an error, same as the old stream-socket's behavior whenever client_fd wasn't
// connected.
static void send_osc_status(lo_address reply_addr, const EngineStatus *status) {
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, status->peak); lo_bundle_add_message(b, "/hds/status/peak", m);
    m = lo_message_new(); lo_message_add_float(m, status->is_recording); lo_bundle_add_message(b, "/hds/status/is_recording", m);
    m = lo_message_new(); lo_message_add_float(m, status->elapsed_seconds); lo_bundle_add_message(b, "/hds/status/elapsed_seconds", m);
    m = lo_message_new(); lo_message_add_float(m, status->record_error); lo_bundle_add_message(b, "/hds/status/record_error", m);
    m = lo_message_new(); lo_message_add_float(m, status->detected_input_channels); lo_bundle_add_message(b, "/hds/status/detected_input_channels", m);
    m = lo_message_new(); lo_message_add_float(m, status->detected_output_channels); lo_bundle_add_message(b, "/hds/status/detected_output_channels", m);
    m = lo_message_new(); lo_message_add_float(m, status->tempo_bpm_live); lo_bundle_add_message(b, "/hds/status/tempo_bpm_live", m);
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b); // frees the bundle AND every message added to it
}

int main(int argc, char **argv) {
    assert_fm_algorithms_acyclic();
    signal(SIGINT, handle_sigint);

    // OSC transport configuration -- see the "OSC Control Protocol" comment above OSC_PARAMS[].
    // Two independent, explicitly-configured ports (not "reply to whoever last sent a message"),
    // matching how real OSC tools (TouchOSC, Max, Pd, python-osc) already expect to be wired up.
    int osc_listen_port = 9000;
    const char *osc_reply_host = "127.0.0.1";
    int osc_reply_port = 9001;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--osc-listen-port") == 0 && i + 1 < argc) osc_listen_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--osc-reply-host") == 0 && i + 1 < argc) osc_reply_host = argv[++i];
        else if (strcmp(argv[i], "--osc-reply-port") == 0 && i + 1 < argc) osc_reply_port = atoi(argv[++i]);
    }

    SynthSharedControls active_knobs; init_default_controls(&active_knobs);

    // Non-threaded lo_server, polled once per audio block from the main loop below (see the
    // "while (lo_server_recv_noblock(...))" call site) -- deliberately NOT lo_server_thread, so
    // only this thread ever touches active_knobs/g_sample_slots, exactly like the poll_*_socket()
    // functions this replaces. The zone methods are registered with explicit type strings (liblo
    // tries the most specific registration first), the knob table's generic method last with
    // path=NULL/type=NULL as the catch-all for everything else.
    char osc_listen_port_str[16]; snprintf(osc_listen_port_str, sizeof(osc_listen_port_str), "%d", osc_listen_port);
    lo_server osc_server = lo_server_new_with_proto(osc_listen_port_str, LO_UDP, osc_error_handler);
    if (!osc_server) { fprintf(stderr, "Cannot continue without an OSC listen socket on port %d.\n", osc_listen_port); return 1; }
    lo_server_add_method(osc_server, "/hds/zone/upsert", "ifffffffffffffffffs", zone_upsert_handler, NULL);
    lo_server_add_method(osc_server, "/hds/zone/remove", "i", zone_remove_handler, NULL);
    lo_server_add_method(osc_server, NULL, NULL, generic_osc_handler, &active_knobs);

    char osc_reply_port_str[16]; snprintf(osc_reply_port_str, sizeof(osc_reply_port_str), "%d", osc_reply_port);
    lo_address osc_reply_addr = lo_address_new(osc_reply_host, osc_reply_port_str);
    fprintf(stderr, "[osc] listening on UDP port %d, status replies to %s:%d\n", osc_listen_port, osc_reply_host, osc_reply_port);

    // Output: opened with however many channels query_output_device_channel_count() detects --
    // see the multi-source routing model's output-pair grid (SynthSharedControls.source_routing)
    // -- not hardcoded stereo, so a class-compliant multichannel interface's extra outputs are
    // actually reachable, not just its first pair. Symmetric with the input side just below.
    int detected_output_channels = query_output_device_channel_count(active_knobs.output_device);
    snd_pcm_t *pcm_handle = NULL;
    if (!open_pcm_stream(active_knobs.output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels)) {
        fprintf(stderr, "Cannot continue without an output device.\n");
        lo_address_free(osc_reply_addr); lo_server_free(osc_server); return 1;
    }
    char applied_output_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);

    // External Audio Input: best-effort. Unlike output, a capture device failing to open isn't
    // fatal -- external input just stays silent (ext_in_enable has nothing to mix in) until a
    // working device name is picked from the Audio tab. Opened with however many channels
    // query_device_channel_count() detects -- see the Recording tab's per-channel checkboxes --
    // not hardcoded stereo, so a class-compliant multichannel interface's extra inputs are
    // actually reachable, not just its first two.
    snd_pcm_t *pcm_capture_handle = NULL;
    int detected_input_channels = query_device_channel_count(active_knobs.input_device);
    open_pcm_stream(active_knobs.input_device, SND_PCM_STREAM_CAPTURE, &pcm_capture_handle, active_knobs.audio_latency_ms, detected_input_channels);
    char applied_input_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_input_device, sizeof(applied_input_device), "%s", active_knobs.input_device);
    float applied_audio_latency_ms = active_knobs.audio_latency_ms;

    // Recording engine: `record_ring`/writer thread state must be static -- RecordEngine embeds
    // a RECORD_RING_CAPACITY(1<<22)-float ring buffer (16MB), far too large for main()'s stack.
    // The writer thread is started once here and lives for the process's whole lifetime; the
    // audio thread only ever talks to it through record_engine's atomics/ring (see the
    // REALTIME SAFETY comment above the WAV recording section).
    static RecordEngine record_engine;
    memset(&record_engine, 0, sizeof(record_engine));
    bool record_writer_thread_ok = (pthread_create(&record_engine.thread, NULL, record_writer_thread_main, &record_engine) == 0);
    if (!record_writer_thread_ok) {
        fprintf(stderr, "[record] failed to start writer thread -- recording will be unavailable this session\n");
    }
    RecordGatherState record_gather = {0};
    bool applied_record_enable = false;
    int recorder_input_channels_at_start = 0; // detected_input_channels when recording began, for the mid-take device-change safety check
    uint64_t record_last_reported_drops = 0;

    // Sample-streaming disk thread (DFD/WAVE_SAMPLE) -- same "started once, lives for the
    // process's lifetime, talked to only via atomics" pattern as the recording writer thread
    // above. g_sample_streams/g_sample_slots are static/global (not local here) since they're
    // shared with handle_note_on/render_sample_osc, which don't have access to main()'s locals.
    bool sample_disk_thread_ok = (pthread_create(&g_sample_disk_thread, NULL, sample_disk_thread_main, NULL) == 0);
    if (!sample_disk_thread_ok) {
        fprintf(stderr, "[sample] failed to start disk streaming thread -- WAVE_SAMPLE playback will be unavailable this session\n");
    }

    snd_seq_t *seq_handle;
    int seq_err = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_INPUT, 0);
    if (seq_err < 0) {
        fprintf(stderr, "MIDI sequencer failed to open: %s\n", snd_strerror(seq_err));
        snd_pcm_close(pcm_handle); if (pcm_capture_handle) snd_pcm_close(pcm_capture_handle);
        lo_address_free(osc_reply_addr); lo_server_free(osc_server); return 1;
    }
    snd_seq_set_client_name(seq_handle, "YourSynth Socket Engine");
    int our_midi_port = snd_seq_create_simple_port(seq_handle, "Input", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    // Subscribe our own port to the ALSA System client's Announce port -- this makes every
    // client/port creation and removal on the whole system show up as SND_SEQ_EVENT_PORT_START/
    // PORT_EXIT events in our own input stream (handled in process_midi_input), which is what
    // lets a USB MIDI keyboard plugged in after startup auto-connect too, with no aconnect ever
    // needed. Best-effort: if it fails, MIDI auto-connect still works at startup, just not live.
    snd_seq_connect_from(seq_handle, our_midi_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

    MidiSourceList midi_sources = {0};
    MidiLiveState midi_live = {0}; // pitch_bend/channel_pressure/cc[] all start at their "centered/off" 0.0
    midi_autoconnect_scan(seq_handle, our_midi_port, active_knobs.midi_device, &midi_sources);
    char applied_midi_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_midi_device, sizeof(applied_midi_device), "%s", active_knobs.midi_device);

    float global_lfo_phases[LFO_COUNT] = {0.0f}; int vu_decimator = 0;

    SynthVoice voices[MAX_VOICES]; for (int i = 0; i < MAX_VOICES; i++) init_voice(&voices[i]);
    MonoNoteStack mono_stack = {0};

    // Engine "uptime" in samples, purely for the Tempo system's elapsed-time axis (see
    // compute_current_tempo_bpm()) -- there's no other transport/song-position concept here.
    uint64_t total_frames_rendered = 0;

    while (g_synth_running) {
        // Drain every pending OSC datagram this block -- generic_osc_handler/zone_upsert_handler/
        // zone_remove_handler write directly into active_knobs/g_sample_slots as each message is
        // dispatched, so nothing further is needed here once this returns 0 (no more pending).
        while (lo_server_recv_noblock(osc_server, 0) > 0) {}
        bool poly_mode_active = (active_knobs.is_polyphonic != 0.0f);

        // Live device switching: the Audio tab writes these as plain fields on the same control
        // packet as every other knob. Only reopen the stream whose name actually changed.
        // A latency change reopens BOTH streams (it's not tied to one device name), since
        // snd_pcm_set_params' latency target can only be applied at stream-open time.
        bool latency_changed = active_knobs.audio_latency_ms != applied_audio_latency_ms;
        // Defensive: an empty device name is never a real selection (unlike midi_device, which
        // has a genuine "auto" meaning for ""; see midi_client_should_autoconnect) -- it can only
        // mean a control packet arrived with this field not meaningfully set yet. Treat it as "no
        // change requested" rather than trying to open a PCM device named "", which fails every
        // time and, if it kept recurring on every poll, would flood stderr with ALSA errors and
        // starve the audio thread's attention on nothing but failed reopen attempts.
        bool output_device_valid = active_knobs.output_device[0] != '\0';
        if (output_device_valid && (latency_changed || strncmp(active_knobs.output_device, applied_output_device, AUDIO_DEVICE_NAME_LEN) != 0)) {
            // Re-detect channel count too -- a device switch can mean a completely different
            // interface with a different channel count, not just a renamed same one.
            int candidate_output_channels = query_output_device_channel_count(active_knobs.output_device);
            if (open_pcm_stream(active_knobs.output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, candidate_output_channels)) {
                detected_output_channels = candidate_output_channels;
                snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);
            } else {
                // Failed -- fall back to the last device that was actually working rather than
                // leaving pcm_handle NULL, and stop retrying this same bad name every block.
                open_pcm_stream(applied_output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels);
                snprintf(active_knobs.output_device, AUDIO_DEVICE_NAME_LEN, "%s", applied_output_device);
            }
        } else if (latency_changed) {
            // Latency-only change with no valid device name yet -- still worth reapplying to
            // whatever's already open rather than silently doing nothing.
            open_pcm_stream(applied_output_device, SND_PCM_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels);
        }
        bool input_device_valid = active_knobs.input_device[0] != '\0';
        if (input_device_valid && (latency_changed || strncmp(active_knobs.input_device, applied_input_device, AUDIO_DEVICE_NAME_LEN) != 0)) {
            // Re-detect channel count too -- a device switch can mean a completely different
            // interface with a different channel count, not just a renamed same one.
            detected_input_channels = query_device_channel_count(active_knobs.input_device);
            open_pcm_stream(active_knobs.input_device, SND_PCM_STREAM_CAPTURE, &pcm_capture_handle, active_knobs.audio_latency_ms, detected_input_channels); // best-effort
            snprintf(applied_input_device, sizeof(applied_input_device), "%s", active_knobs.input_device);
        }
        if (latency_changed) applied_audio_latency_ms = active_knobs.audio_latency_ms;
        if (strncmp(active_knobs.midi_device, applied_midi_device, AUDIO_DEVICE_NAME_LEN) != 0) {
            // Selection changed live from the Audio tab -- drop whatever was auto-connected
            // under the old selection and rescan under the new one.
            midi_autoconnect_scan(seq_handle, our_midi_port, active_knobs.midi_device, &midi_sources);
            snprintf(applied_midi_device, sizeof(applied_midi_device), "%s", active_knobs.midi_device);
        }

        // Sample Bank: reconciliation now happens once per "/hds/zone/upsert"/"/hds/zone/remove"
        // OSC message (dispatched inline inside the lo_server_recv_noblock() drain above, at
        // receipt), not every audio block -- see apply_zone_upsert()/apply_zone_remove(). The one
        // piece of work that genuinely can't happen at message-receipt time is finishing a load
        // that got throttled into the pending-loads queue; that's drained here, at most
        // SAMPLE_LOADS_PER_BLOCK entries per
        // block (see its #define for the real-time trade-off this bounds).
        drain_pending_sample_loads();

        process_midi_input(seq_handle, voices, &active_knobs, global_lfo_phases, poly_mode_active, &mono_stack, our_midi_port, &midi_sources, &midi_live);

        // block_size/voice_limit are computed fresh every iteration since both are live,
        // user-adjustable controls (Audio tab: Block Size, Polyphony) -- see
        // effective_block_size()/effective_voice_limit(). capture_buffer/buffer stay declared
        // at the hard BUFFER_SIZE ceiling; only the first block_size frames of each are
        // actually read/written/rendered this iteration.
        int block_size = effective_block_size(&active_knobs);

        // Programmable tempo: resolved once per block from the Tempo knob/tempo-map (see
        // compute_current_tempo_bpm()) against this engine's own running uptime. Nothing in the
        // DSP consumes this yet -- it's reported back to the GUI (EngineStatus.tempo_bpm_live)
        // purely so the Tempo knob visibly does something end-to-end even before a tempo-synced
        // feature exists to actually use it.
        double engine_elapsed_seconds = (double)total_frames_rendered / (double)SAMPLE_RATE;
        float current_tempo_bpm = compute_current_tempo_bpm(&active_knobs, engine_elapsed_seconds);

        // Read one block of external input, ALL detected_input_channels of it -- interleaved,
        // stride detected_input_channels per frame (not always 2 anymore; see
        // query_device_channel_count()). Silence (the zero-init) if there's no working capture
        // device, or a read comes back short/broken -- external input degrading to silence
        // should never be able to stall or crash the synth's own audio output.
        static float capture_buffer[BUFFER_SIZE * RECORD_MAX_CHANNELS];
        memset(capture_buffer, 0, sizeof(float) * (size_t)block_size * (size_t)detected_input_channels);
        if (pcm_capture_handle) {
            snd_pcm_sframes_t got = snd_pcm_readi(pcm_capture_handle, capture_buffer, block_size);
            if (got < 0) {
                snd_pcm_recover(pcm_capture_handle, (int)got, 0);
            } else if ((snd_pcm_uframes_t)got < (snd_pcm_uframes_t)block_size) {
                memset(capture_buffer + ((size_t)got * (size_t)detected_input_channels), 0,
                       ((size_t)block_size - (size_t)got) * (size_t)detected_input_channels * sizeof(float));
            }
        }

        // DIAGNOSTIC: cheap (active_voice_limit simple state checks, negligible next to the
        // render itself) count of currently-active voices, purely to report alongside any
        // underrun/over-budget warning below -- see the comment on write_audio_block.
        int voice_limit = effective_voice_limit(&active_knobs);
        int active_voices_count = 0;
        for (int v = 0; v < voice_limit; v++) {
            if (voices[v].vco1_eg.state != STATE_IDLE || voices[v].vco2_eg.state != STATE_IDLE || voices[v].vco3_eg.state != STATE_IDLE) active_voices_count++;
        }

        // Static + declared at the hard OUTPUT_MAX_CHANNELS ceiling, same convention as
        // capture_buffer above -- only the first block_size*detected_output_channels floats are
        // actually rendered/written this iteration.
        static float buffer[BUFFER_SIZE * OUTPUT_MAX_CHANNELS];
        struct timespec render_t0, render_t1;
        clock_gettime(CLOCK_MONOTONIC, &render_t0);
        float block_peak = render_audio_block(voices, &active_knobs, global_lfo_phases, capture_buffer, detected_input_channels, detected_output_channels, &midi_live, buffer);
        clock_gettime(CLOCK_MONOTONIC, &render_t1);
        double render_ms = (render_t1.tv_sec - render_t0.tv_sec) * 1000.0 + (render_t1.tv_nsec - render_t0.tv_nsec) / 1e6;
        double budget_ms = (block_size / (double)SAMPLE_RATE) * 1000.0;
        // DIAGNOSTIC: if rendering this block alone took longer than the block's own real-time
        // budget, that's a root cause an ALSA underrun can be pinned on directly, independent of
        // whatever else might be going on in the OS -- the engine itself couldn't keep up.
        if (render_ms > budget_ms) {
            fprintf(stderr, "[perf] render_audio_block took %.3fms (budget %.3fms, block size %d) with %d active voices -- too slow, will cause underruns\n",
                    render_ms, budget_ms, block_size, active_voices_count);
        }

        // --- Recording (Recording tab) ---
        // Edge-triggered start/stop on record_enable, exactly like the device-switch checks
        // above. A device change mid-take that alters detected_input_channels would desync the
        // raw capture buffer's stride from what the open file committed to at start -- safer to
        // stop cleanly than silently scramble the channel interleaving.
        //
        // Everything here is bounded, non-blocking work: this thread only computes the
        // gather/downmix arithmetic and issues atomic commands/ring pushes to the writer thread
        // (record_engine, started once above) -- it never touches a FILE* itself. See the
        // REALTIME SAFETY comment above the WAV recording section for why.
        if (record_gather.active && detected_input_channels != recorder_input_channels_at_start) {
            fprintf(stderr, "[record] input device channel count changed mid-recording -- stopping for safety\n");
            atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
            record_gather.active = false;
        }
        bool want_recording = record_writer_thread_ok && active_knobs.record_enable != 0.0f;
        if (want_recording && !applied_record_enable) {
            bool sel_include_synth_output = active_knobs.record_include_synth_output != 0.0f;
            bool sel_downmix_stereo = active_knobs.record_downmix_stereo != 0.0f;
            bool sel_input_channel[RECORD_MAX_CHANNELS];
            for (int c = 0; c < RECORD_MAX_CHANNELS; c++) sel_input_channel[c] = (c < detected_input_channels) && (active_knobs.record_input_channel_enable[c] != 0.0f);
            int channels = wav_recorder_compute_channels(sel_include_synth_output, sel_downmix_stereo, sel_input_channel, detected_input_channels);

            if (channels <= 0) {
                fprintf(stderr, "[record] cannot start: no channels selected\n");
                atomic_store_explicit(&record_engine.start_failed, true, memory_order_relaxed);
            } else {
                // Publish the command's parameters BEFORE the atomic release-store of `command`
                // below -- see RecordEngine's comment on why that ordering makes these plain
                // writes safely visible to the writer thread.
                snprintf(record_engine.cmd_path, sizeof(record_engine.cmd_path), "%s", active_knobs.record_path);
                record_engine.cmd_include_synth_output = sel_include_synth_output;
                record_engine.cmd_downmix_stereo = sel_downmix_stereo;
                record_engine.cmd_input_channel_count = detected_input_channels;
                record_engine.cmd_record_format = (int)active_knobs.record_format;
                for (int c = 0; c < RECORD_MAX_CHANNELS; c++) record_engine.cmd_input_channel_enable[c] = sel_input_channel[c];

                record_gather.sel_include_synth_output = sel_include_synth_output;
                record_gather.sel_downmix_stereo = sel_downmix_stereo;
                record_gather.sel_input_channel_count = detected_input_channels;
                for (int c = 0; c < RECORD_MAX_CHANNELS; c++) record_gather.sel_input_channel[c] = sel_input_channel[c];
                record_gather.channels = channels;
                record_gather.active = true;
                recorder_input_channels_at_start = detected_input_channels;

                atomic_store_explicit(&record_engine.start_failed, false, memory_order_relaxed);
                atomic_store_explicit(&record_engine.command, RECORD_CMD_START, memory_order_release);
                fprintf(stderr, "[record] start requested: '%s', %d channel(s)%s\n", active_knobs.record_path, channels,
                        sel_downmix_stereo ? " (downmixed)" : "");
            }
        } else if (!want_recording && applied_record_enable) {
            atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
            record_gather.active = false;
        }
        applied_record_enable = want_recording;
        if (record_gather.active) wav_recorder_gather_block(&record_gather, &record_engine, buffer, detected_output_channels, capture_buffer, block_size);

        // DIAGNOSTIC: mirrors [perf]/[voices]/[audio] above -- reported at most once per status
        // tick (not every block) so a sustained stall doesn't spam stderr, same rationale as
        // those. A rising count means the writer thread can't keep up with disk I/O in real
        // time; the audio thread itself is never at risk (dropped samples come out of the
        // *recording*, never the live output).
        uint64_t record_dropped_now = atomic_load_explicit(&record_engine.ring.dropped_samples, memory_order_relaxed);
        if (record_dropped_now != record_last_reported_drops) {
            fprintf(stderr, "[record] writer thread falling behind -- %llu sample(s) dropped so far (disk too slow to keep up in real time)\n",
                    (unsigned long long)record_dropped_now);
            record_last_reported_drops = record_dropped_now;
        }

        vu_decimator++;
        if (vu_decimator >= 12) {
            bool engine_active = atomic_load_explicit(&record_engine.is_active, memory_order_acquire);
            EngineStatus status = {
                .peak = block_peak,
                .is_recording = engine_active ? 1.0f : 0.0f,
                .elapsed_seconds = engine_active ? (atomic_load_explicit(&record_engine.frames_written, memory_order_relaxed) / (float)SAMPLE_RATE) : 0.0f,
                .record_error = atomic_load_explicit(&record_engine.start_failed, memory_order_relaxed) ? 1.0f : 0.0f,
                .detected_input_channels = (float)detected_input_channels,
                .detected_output_channels = (float)detected_output_channels,
                .tempo_bpm_live = current_tempo_bpm,
            };
            send_osc_status(osc_reply_addr, &status);
            vu_decimator = 0;
        }
        write_audio_block(pcm_handle, buffer, detected_output_channels, block_size, active_voices_count, render_ms);
        total_frames_rendered += (uint64_t)block_size;
    }
    // Flush/finalize a still-running take on shutdown: signal the writer thread to stop (if a
    // recording was active) and to exit, then join it so its final fclose actually happens
    // before the process exits.
    if (record_gather.active) atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
    if (record_writer_thread_ok) {
        atomic_store_explicit(&record_engine.shutdown, true, memory_order_release);
        pthread_join(record_engine.thread, NULL);
    }
    if (sample_disk_thread_ok) {
        atomic_store_explicit(&g_sample_disk_thread_shutdown, true, memory_order_release);
        pthread_join(g_sample_disk_thread, NULL);
    }
    for (int s = 0; s < MAX_SAMPLE_STREAMS; s++) if (g_sample_streams[s].disk_file_handle) fclose(g_sample_streams[s].disk_file_handle);
    for (int s = 0; s < g_sample_slot_count; s++) { free(g_sample_slots[s].pre_roll_l); free(g_sample_slots[s].pre_roll_r); }
    lo_server_free(osc_server);
    lo_address_free(osc_reply_addr);
    snd_seq_close(seq_handle);
    snd_pcm_close(pcm_handle);
    if (pcm_capture_handle) snd_pcm_close(pcm_capture_handle);
    return 0;
}
