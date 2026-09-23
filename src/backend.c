// This project targets Linux only -- see shell.nix's own header comment for why Windows support
// was dropped entirely (the real, ongoing cost of maintaining a second audio backend, WASAPI vs.
// ALSA). sys/ioctl.h, sys/socket.h, sys/un.h are leftovers from the old pre-OSC Unix-socket
// protocol and are already unused by anything in this file (grep confirms zero call sites) --
// left as-is here since removing them is an unrelated dead-code cleanup, not part of dropping
// Windows support.
#include <alsa/asoundlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <dirent.h> // native SFZ parser's case-insensitive sample-path fallback (sfz_resolve_sample_path)
#include <limits.h> // PATH_MAX, for the same parser's #include cycle-guard/path buffers
#include <ctype.h>  // isalpha/isalnum/isspace, for the same parser's hand-written tokenizer
#include <strings.h> // strcasecmp -- SFZ opcode names are conventionally lowercase, matched case-insensitively here for robustness
// WavPack recording is disabled for now (commented out via this single guard, not deleted --
// flip to 1, re-add -lwavpack to the Makefile's LDLIBS, and re-enable the frontend.py format
// radio button to bring it back). See WavPackRecorder further down for the real implementation
// this guards, kept intact behind #if HDS_WAVPACK_ENABLED rather than removed.
#define HDS_WAVPACK_ENABLED 0
#if HDS_WAVPACK_ENABLED
#include <wavpack/wavpack.h>
#endif
#include <lo/lo.h>

// FX_TYPE_NAM (Neural Amp Modeler) needs a small vendored C++ library (nam_bridge.h/.cpp over
// third_party/NeuralAmpModelerCore) linked in -- same "opt-in dependency, detected/toggled by the
// Makefile" posture as HDS_WAVPACK_ENABLED/HDS_PIPEWIRE_ENABLED just below, needed for one real
// reason: the `arm64` cross-build target (see the Makefile's own comment) has no C++ cross-compiler
// or cross-built Eigen wired up, so it builds with this off, and every bridge-touching line below
// (not the harmless plain-data FX_TYPE_NAM enum value/param layout/wire fields, which stay
// identical either way so a project file is portable between an HDS_NAM_ENABLED and non-enabled
// build) is guarded accordingly. On by default -- unlike PipeWire, this isn't probing for an
// optional system package (Eigen/NeuralAmpModelerCore are vendored, always present), so every
// normal x86_64 build gets it for free with nothing to detect.
#ifndef HDS_NAM_ENABLED
#define HDS_NAM_ENABLED 1
#endif
#if HDS_NAM_ENABLED
#include "nam_bridge.h" // extern "C" shim over NeuralAmpModelerCore -- see that header's own
                         // comment and FX_TYPE_NAM below; this is the ONLY C++ this file touches,
                         // and only indirectly through these few functions.
#endif

// Optional native PipeWire backend, alongside the always-present ALSA backend (see the "Audio
// platform shim" section further down) -- HDS_PIPEWIRE_ENABLED is set by the Makefile via a
// pkg-config auto-detection at build time (1 when libpipewire-0.3 is found, e.g. inside
// nix-shell; 0 on a plain libasound2-dev-only setup, which still gets a normal ALSA-only build
// with nothing to flip by hand). Same "opt-in dependency" posture as HDS_WAVPACK_ENABLED above,
// just detected automatically instead of hand-toggled, since unlike WavPack this one has real
// runtime value (PipeWire's own graph lets multiple apps share the audio device) whenever it's
// available rather than being parked for later. See README.md's "PipeWire backend" section.
#ifndef HDS_PIPEWIRE_ENABLED
#define HDS_PIPEWIRE_ENABLED 0
#endif
#if HDS_PIPEWIRE_ENABLED
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <semaphore.h>
#endif

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
// Combi mode: up to COMBI_PROGRAM_COUNT independently-synthesizing Programs sharing one voice
// pool (see ProgramState/SynthSharedControls.programs below). Defined here, ahead of
// EXT_IN_PAIR_COUNT/SOURCE_COUNT, since the bus/source model below is built directly on top of
// it: source ids 0..COMBI_PROGRAM_COUNT-1 are the Programs themselves, one more id is the
// synthetic "Combi Mix" bus (the whole stack's pre-FX dry sum), and every ext-in pair comes
// after that -- see SOURCE_COUNT's own updated comment.
#define COMBI_PROGRAM_COUNT 8
// Single Program mode's own live-edit buffer is a DEDICATED 9th ProgramState slot, never one of
// the 8 real Combi lanes -- see SynthSharedControls.programs' own comment for why (mutual
// exclusivity: editing in the Oscillators/Filters/LFO/EG tabs must never silently rewrite
// whatever's loaded onto Combi lane 1, and vice versa). COMBI_PROGRAM_COUNT itself still means
// exactly "8 real Combi lanes" everywhere else in this file (SOURCE_COUNT, the Combi rendering
// loops, the OSC address family below) -- untouched by this.
#define SINGLE_PROGRAM_SLOT COMBI_PROGRAM_COUNT
#define EXT_IN_PAIR_COUNT 8
// BREAKING CHANGE (real Combi mode): SOURCE_COUNT grows from 9 (1 Program + 8 ext-in) to 17, and
// existing source ids are RENUMBERED -- ext-in pairs move from ids 1-8 to 9-16, since ids 0-7 are
// now the 8 Combi Programs (0 = today's single Program, byte-identical) and id 8 is the new
// "Combi Mix" bus (sum of all 8 Programs' raw dry pre-FX output -- lets one FX slot serially
// process the whole stack while other slots still individually target a single Program,
// deliberately allowed to overlap, per "let them route"). Any existing fx_source/routing
// configuration referencing ext-in pairs by their old 1-8 ids now points at the wrong source and
// must be re-set -- the same category of break this file's own running BREAKING CHANGE log has
// documented for every prior wire-format-shape change.
#define SOURCE_COUNT (COMBI_PROGRAM_COUNT + 1 + EXT_IN_PAIR_COUNT)
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

// Combi/Sequencer scaffolding: one flexible pool of up to 32 lanes, each independently
// assignable (via SeqLaneState.lane_type) to Program/MIDI or Audio duty, capped at 16 active of
// each kind -- because each type is meant to eventually draw from its own backend resource pool
// (COMBI_PROGRAM_COUNT independent Program-synthesis instances -- now real, see ProgramState/
// SynthSharedControls.programs below; AUDIO_LANE_RESOURCE_COUNT RAM-cached audio buffers, still
// scaffolding). This pass adds the lane data model + OSC wire + GUI; the audio-lane resource pool
// is not yet allocated/used by the DSP.
#define SEQ_LANE_COUNT 32
// COMBI_PROGRAM_COUNT itself is defined earlier now (ahead of SOURCE_COUNT, which is built
// directly on top of it) -- see that definition's own comment.
#define AUDIO_LANE_RESOURCE_COUNT 16
#define SEQ_AUDIO_PATH_LEN 256

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
    // Per-Key mode: 0 = Global (today's only behavior -- one shared phase for the whole
    // Program, the same LFO cycle heard identically by every voice). 1 = Per-Key -- this LFO
    // becomes a per-VOICE resource with its own independent phase (and, when key_sync is also
    // on, a phase that resets at THIS voice's own note-on instead of whichever note last reset
    // the shared phase) -- the standard "per-key LFO" feature real digital synths have, letting
    // overlapping notes each run their own unsynced cycle, and letting Rate track key/velocity
    // per note. ALWAYS uses the decimated/interpolated computation (see LFO_DECIMATION_FACTOR),
    // even for LFO slots 1/2 which are otherwise full audio-rate -- per-voice computation multiplies
    // cost by polyphony, so the cheap path is used unconditionally in this mode, never the exact
    // one. See render_audio_block's per-voice LFO dispatch.
    float per_key_mode;
    // Keyboard tracking (Per-Key mode only): rate scales by 2^((note-60)/12 * key_track/100) --
    // an octave-per-octave-of-keyboard multiplier at key_track=+-100 (matching the standard
    // filter-cutoff-keytrack convention), 0 = no tracking (every note the same rate). Negative
    // values invert direction (higher notes = slower).
    float key_track;
    // Velocity tracking (Per-Key mode only): rate scales linearly by
    // 1 + (key_velocity-64)/63 * vel_track/100 -- 0 = no tracking, +-100 = full range from half
    // to double rate (approx) across the velocity range, centered on a "normal" velocity of 64.
    float vel_track;
    // Tempo Sync: 0 = Free (today's only behavior -- rate_hz sets this LFO's speed directly,
    // in Hz). 1..LFO_SYNC_DIVISION_COUNT = a 1-based index into LFO_SYNC_BEATS_PER_CYCLE --
    // rate_hz is ignored entirely and this LFO's speed instead tracks the Program's own current
    // tempo (compute_current_tempo_bpm(), smoothed the same way arp_tempo_smoothed already is)
    // at the selected note division. See resolve_lfo_rate_hz(), the single place both Free and
    // Synced modes resolve to an actual Hz value.
    float tempo_sync_division;
    // Curve/Drive: one physical pot, reinterpreted by wave_type -- bipolar Curve (-1..1, center
    // detent = linear) for Triangle/Saw, unipolar Drive/Saturation (0..1, default 0 = clean) for
    // Sine, ignored for Square/Off. See apply_wave_curve()/apply_sine_drive() and
    // lookup_lfo_waveform().
    float curve_drive;
} PackLfoParams;

// A single filter's parameters: State-Variable Filter (SVF) core, cutoff/resonance/exclusive
// type, plus a slope switch. slope_poles cascades the same 2-pole (12dB/oct) SVF core twice
// back-to-back to get a 4-pole (24dB/oct) slope -- see run_svf_filter(). Deliberately simple --
// a prior "Multimode blend" design (5-tap weighted Mode1/Mode2/Crossfade mix per filter) plus a
// whole Filter A(Normal/Multi)/Filter B routing system on top of it was scrapped per the user's
// own direct assessment ("genuinely doesn't make sense anymore") in favor of this: 5 flat,
// interchangeable filter slots (see ProgramState.filters[5]), each an exclusive Off/LP/HP/BP/
// Notch pick, chained in Series or summed in Parallel -- see render_audio_block's filter dispatch.
typedef struct {
    float cutoff;
    float res_q;
    float slope_poles; // 0 = 2-pole (12dB/oct), 1 = 4-pole (24dB/oct)
    float filter_type; // 0=Off (bypass -- signal passes through unfiltered, unchanged), 1=LPF,
                        // 2=HPF, 3=BPF, 4=Notch -- exclusive pick, no blending between taps.
    // Cutoff keyboard tracking (-100..100): same formula/convention as PackLfoParams' own
    // key_track (see its use in render_audio_block) -- 0 = no tracking (this filter's cutoff is
    // fixed regardless of which note plays), +-100 = a full octave-per-octave-of-keyboard
    // multiplier, centered on note 60 (middle C). The frontend exposes this as a short preset
    // list (Off/50%/100%/150%) in the Cutoff knob's own right-click menu rather than a separate
    // always-visible knob, but the backend just applies whatever value arrives.
    float key_track;
    // "ext MIDI": additive cutoff modulation from a live MIDI CC or channel aftertouch, same
    // convention as PackLfoParams.mod_source_cc (-1=none, 0-127=CC number, 128=channel
    // aftertouch -- see MidiLiveState.cc[128]/channel_pressure). Unlike mod_source_cc's own
    // multiplicative LFO-depth scale, this is a direct additive cutoff offset (see
    // render_audio_block) -- "more mod wheel/aftertouch = brighter" is the near-universal use.
    float cutoff_mod_source_cc;
} FilterParams;

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

// One lane in the Combi/Sequencer 32-lane pool (see SEQ_LANE_COUNT's own comment above). Purely
// a mixer-strip + assignment data model this pass -- nothing in render_audio_block reads any of
// these fields yet. Combi mode shows only lanes 0-15 (implicitly Program/MIDI, never Audio) with
// midi_channel/seq_mode hidden; Sequencer mode shows all 32 lanes with every field live in the
// GUI, per the plan's lane-topology decision.
typedef struct {
    float lane_type;    // 0 = Program/MIDI, 1 = Audio -- a real per-lane choice, capped at 16
                         // active of each kind (enforced by the frontend's assignment logic this
                         // pass, since neither resource pool exists in the backend yet).
    float volume;        // 0-100
    float pan;            // -1..1, same convention as vco1_pan
    float midi_channel;   // 0 = Omni, 1-16 = specific channel. Filters Sequencer RECORDING only
                           // (see seq_record_midi_event) -- a Rec-armed lane only captures
                           // incoming notes on its own channel, or every channel if Omni. Live
                           // Program dispatch (handle_note_on/off, outside the Sequencer) stays
                           // fully channel-agnostic as before -- this field never touches that
                           // path, only which lane(s) a note gets RECORDED into.
    // seq_mode/mute/rec_armed/rec_overdub are all independent toggles (not one exclusive mode),
    // per the user's own explicit ask -- but "Off" still wins over everything else, since it's a
    // real compute bypass: an Off lane never plays back, never records, never mixes, and is never
    // repositioned/auto-started by the transport, matching this codebase's existing "skip work
    // entirely, don't just silence" philosophy (same idea as WAVE_OFF). Precedence, applied
    // consistently everywhere these fields are read:
    //   1. seq_mode == 1 (Off) -> lane does nothing at all, full stop.
    //   2. else rec_armed != 0 (Program/MIDI lanes only) -> plays no recorded events; instead
    //      records incoming MIDI (filtered by midi_channel) while the transport is PLAYING.
    //   3. else -> normal Seq playback (or Audio-lane transport-driven playback), silenced by
    //      `mute` but still advancing in sync with the transport (so un-muting resumes exactly in
    //      step rather than picking up wherever it happened to be).
    // All ignored entirely in Combi mode (see Combi's own hardcoded-Seq override in frontend.py).
    float seq_mode;      // 0 = Seq, 1 = Off (a real bypass -- see precedence above)
    float mute;           // 0/1, independent of seq_mode/rec_armed (both Program/MIDI + Audio lanes)
    float rec_armed;      // 0/1, independent of seq_mode/mute (Program/MIDI lanes only -- Audio
                           // lanes never record)
    float rec_overdub;    // 0 = Overwrite (a fresh Rec take clears this lane's previously
                           // recorded events -- the original behavior), 1 = Overdub (a fresh take
                           // keeps them; newly recorded notes are inserted into the existing
                           // sequence in time order rather than replacing it -- see
                           // seq_record_midi_event's own comment). Only meaningful on a Rec-armed
                           // Program/MIDI lane; a harmless no-op otherwise.
    // Audio-lane "Load Audio" (Sequencer only -- see g_seq_audio/seq_audio_loader_thread_main).
    // Writing a new path here is just a request: main()'s once-per-block change-detection loop
    // notices the change and spawns a dedicated loader thread to actually read the file, since
    // a full-file disk read must never happen on the realtime audio thread. audio_path itself is
    // never read by the DSP path -- only g_seq_audio[lane]'s resulting RAM buffer would be, once
    // real playback exists.
    char audio_path[SEQ_AUDIO_PATH_LEN];
    float audio_offset_sec; // -10..10, GUI-side range. Currently only used as a start-frame
                             // offset by the one-shot Preview trigger below (positive = skip
                             // that many seconds into the file; negative currently clamps to 0 --
                             // no pre-roll silence yet). Not yet touched by any real transport.
    // "Preview" button (Sequencer only): a plain edge-triggered counter, not a level -- the
    // frontend increments it by 1 on every click and resends the whole packet (same shape as
    // every other knob), so poll_seq_audio_preview_triggers() in main()'s loop can detect "this
    // changed since last block" the same way it already detects an audio_path change, and
    // (re)start that lane's one-shot playback from the beginning (or audio_offset_sec seconds
    // in). Deliberately NOT a full transport -- see g_seq_audio's own playback-state comment.
    float audio_preview_trigger;
} SeqLaneState;

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
// Raised from 8 (originally sized for every earlier port's needs) to fit FX_TYPE_POINTYGUITAR's
// 10 real params without dropping any of them -- unlike Density3's redundant internal Dry/Wet
// (already covered by this slot's own generic Mix below) or TapeDelay2's kept Dry/Wet, none of
// PointyGuitar's 10 controls is a crossfade-equivalent safe to fold away; 5 of them (Presence/
// High/Mid/Low/Sub) are the distortion's own core per-band tone shaping, not optional extras.
// A purely additive, backward-compatible wire change: every existing effect's own param[0..7]
// addresses are untouched, param[8]/param[9] are simply new addresses (defaulting to 0.0, like
// any other never-set knob) that no earlier-ported effect type ever reads.
#define FX_SLOT_PARAM_COUNT 10
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
    // Chain To Next (0/1, default 0): only meaningful when this slot's source is running in
    // Parallel mode (fx_source_routing_mode[source]==1) -- when set, the NEXT owned slot (this
    // source's own next slot in array order, not necessarily the physically-adjacent one) is fed
    // THIS slot's own output instead of starting a fresh parallel branch from the shared dry
    // input, continuing the same branch in series. Lets several slots be chained in series while
    // that whole chain still runs as ONE branch alongside a source's other parallel slots/chains
    // -- e.g. two independent 3-slot serial chains summed together. Ignored entirely in Series
    // mode (every slot there is already chained unconditionally) and on the last slot in the
    // array (there is no "next" to chain into). See process_fx_chain()'s own comment.
    float chain_to_next;
    // FX_TYPE_NAM's own loaded-file paths (unused by every other effect type -- harmless dead
    // bytes on their slots, same posture as param[] entries an effect doesn't read). Plain string
    // fields, not a dedicated load message: see poll_fx_nam_requests()'s own comment for why this
    // exact mechanism (already used by Sequencer audio lanes' own audio_path) is the right one
    // here, including how it makes a CLAP state restore reload correctly for free.
    char nam_model_path[SAMPLE_PATH_LEN];
    char nam_ir_path[SAMPLE_PATH_LEN];
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
// One independently-synthesizing Program (oscillators, filters, EGs, LFOs, FM, tempo -- "the
// patch"): everything frontend.py's _is_program_scope_address() already treats as Program-scoped
// for Save/Load, reused verbatim as this struct's exact field boundary rather than re-deriving it
// -- that boundary is already battle-tested by the save/load feature. Wrapping these fields in a
// real ProgramState[COMBI_PROGRAM_COUNT + 1] array (instead of them living flat on
// SynthSharedControls, as they did before Combi mode was real) is what makes independently-
// configured, simultaneously-playing Programs possible: programs[0..7] are the 8 real Combi
// lanes, addressed under /hds/combi/{0-7}/... (see OSC_PARAMS[]) -- Combi lane 1 is programs[0],
// lane 2 is programs[1], and so on. programs[SINGLE_PROGRAM_SLOT] (index 8, the one extra slot)
// is a DEDICATED 9th ProgramState belonging ONLY to Single Program mode -- the exact same,
// unchanged OSC paths (e.g. /hds/vco1/shape) that used to address programs[0] now address this
// slot instead (see OSC_PSCALAR's own comment). This is a deliberate mutual-exclusivity fix:
// programs[0]/Combi lane 1 and Single Program mode used to be the SAME storage (editing one
// silently rewrote the other); they are now two fully independent ProgramStates that happen to
// share nothing but the field layout. render_audio_block resolves which slot a render pass
// actually reads via combi_mode_active (identity 0..7 in Combi mode, always SINGLE_PROGRAM_SLOT
// outside it) -- see that function's own comments at each resolution point.
//
// FX slots (fx[]/fx_source_routing_mode/source_routing below), ext-in, and every other rig/
// session-level field deliberately stay flat on SynthSharedControls, NOT in this struct -- those
// are shared, physical-insert-like resources any Program (or the whole Combi stack) can be routed
// into via fx_source, not part of any one patch. This is a small, deliberate refinement to
// _is_program_scope_address's own current FX-slot handling (which still treats an FX slot's type/
// mix/bypass/params as Program-scoped for Save/Load, a holdover from when there was only ever one
// Program and the FX pool WAS that Program's own FX) -- see the "Real Combi mode" plan.
typedef struct {
    float is_polyphonic;
    float vco1_shape;
    float vco2_shape;
    float fm_intensity;
    float vco1_vol;
    float vco2_vol;
    float portamento_time_ms;
    float vco1_pan;
    float vco2_pan;
    float vco1_fine_tune;
    float vco2_pitch_offset;
    float vco2_fine_tune;

    // 80 Envelope Parameter Items (ENV_COUNT=10 Envelopes * 8 Parameters, incl. attack start/peak level)
    PackEGParams eg[ENV_COUNT];

    // --- FILTER SECTION: 4 oscillators -> mixer -> 5 flat, interchangeable filter slots, chained
    // in Series (0->1->2->3->4) or summed in Parallel -- see render_audio_block's filter
    // dispatch. Deliberately simple: no A/B distinction, no Multi sub-engine, no blend weights
    // -- each slot is FilterParams' own plain exclusive Off/LPF/HPF/BPF/Notch pick. ---
    float filter_chain_mode; // 0 = Series (0->1->2->3->4), 1 = Parallel (summed; an Off slot
                              // contributes nothing to the sum, and isn't counted in the
                              // divisor that averages however many slots are actually active).
    FilterParams filters[5];

    // Post-filter 3-band EQ (Low Shelf / Mid Peak / High Shelf) -- see EqBandParams' own comment.
    // Deliberately placed after the filter chain in the signal path (render_audio_block applies it
    // to each Program's own fully-mixed/filtered bus, once per sample, not per voice -- see the
    // "Multi-source bus dispatch" section) rather than per-voice like the filters above: an EQ is
    // cheap tone-shaping on an already-summed signal, not a per-voice resonant effect, so doing it
    // once per Program per sample (not once per voice per sample) is both correct and far cheaper.
    EqBandParams eq_bands[3];

    // Active LFO Mapping Items (LFO_COUNT=16 LFOs * 7 Properties, incl. phase offset + mod source CC)
    PackLfoParams lfo[LFO_COUNT];

    // ASSIGNABLE_EG_COUNT (6) Custom EG Intensity Modulators
    float eg_intensity[ASSIGNABLE_EG_COUNT];

    // ASSIGNABLE_EG_COUNT (6) Custom EG Destination Targets (each assignable EG routes
    // independently of the LFOs)
    float eg_dest_target[ASSIGNABLE_EG_COUNT];

    // ENV_COUNT Active Envelope Topology Selection Mode Indices
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

    // --- Arpeggiator: a minimal, from-scratch held-note arpeggiator, in the spirit of the
    // classic "Arpeggino" hardware MIDI arpeggiator (MIT-licensed, github.com/rotenbergrr/
    // Arpeggino / its Midier engine library) -- NOT a literal port, since Arpeggino's own actual
    // engine plays scale-degree chords via a permutation generator, not held MIDI notes in the
    // usual Up/Down synth-arpeggiator sense. See poll_arpeggiator()'s own header comment for the
    // one concrete idea genuinely borrowed from it (the UpDown/DownUp ping-pong index fold). A
    // patch property (like Hard Sync/Poly-Mono above), not a live performance override -- some
    // patches naturally want Arp baked in, others never do. ---
    float arp_enabled;      // 0=Off, 1=On -- while On, real note-on/off events feed the held-note
                             // list (see arp_note_on/arp_note_off) instead of triggering voices
                             // directly; poll_arpeggiator() triggers voices itself, on its own
                             // musical clock.
    float arp_style;        // 0=Up, 1=Down, 2=UpDown, 3=DownUp, 4=Random, 5=AsPlayed -- see
                             // poll_arpeggiator()'s own per-style dispatch.
    float arp_octave_range; // 1-4: how many octaves the held notes are spread across before the
                             // pattern repeats (Up plays every held note in octave 0, then every
                             // held note again in octave 1, etc.).
    float arp_rate;         // 0=1/4, 1=1/8, 2=1/16, 3=1/8 Triplet, 4=1/16 Triplet, 5=1/32 -- note
                             // division of the step clock, synced to this Program's own tempo_bpm
                             // (see ARP_RATE_STEPS_PER_BEAT).
    float arp_gate_pct;     // 1-100: how much of a step's own duration the triggered note stays
                             // held before its own note-off fires (100 = fully legato/overlapping
                             // with the next step's note-on for one sample; less = staccato).
    float arp_hold;         // 0=Off, 1=On ("Latch") -- while On, releasing every physically-held
                             // key does NOT stop the arpeggio; it keeps cycling through whatever
                             // chord was last held. Pressing a fresh note after a fully-released
                             // latch replaces the latched chord rather than adding to it -- see
                             // dispatch_midi_event's own Hold-handling comment.
    float arp_velocity;     // 0="As Played" (use each held note's own captured velocity,
                             // held->velocities[] -- today's exact behavior); 1-127=a fixed
                             // override velocity used for every arp-triggered note instead,
                             // matching the classic hardware-arp "Original vs. Fixed" convention.
                             // See poll_arpeggiator()'s own velocity-selection line.

    // Phase pot per oscillator (degrees, 0-360). Modulatable by LFO/EG via the mod matrix
    // (targets 24/25); the modulated result shifts only the oscillator's read-phase, never its
    // actual running phase accumulator, so pitch/hard-sync/portamento are unaffected.
    float vco1_phase_offset;
    float vco2_phase_offset;

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
    // controls below, own amp envelope eg[3]/eg_mode[3]).
    float vco4_vol;
    float vco4_pan;
    float vco4_mute;
    float vco4_pulse_width; // dual-duty Pulse Width/Tilt, mirrors vco3_pulse_width

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

    // --- Tempo (Global panel) ---
    // A single flat "Tempo" knob for the common case, plus a programmable tempo-MAP system: up
    // to TEMPO_MARKER_MAX (time, bpm) marker pairs, linearly interpolated between whichever two
    // markers bracket the engine's current elapsed time -- see compute_current_tempo_bpm().
    // tempo_marker_count == 0 means "ignore the marker arrays entirely, tempo_bpm alone is the
    // whole story."
    float tempo_bpm;
    float tempo_marker_count;
    float tempo_marker_time[TEMPO_MARKER_MAX]; // seconds since engine start
    float tempo_marker_bpm[TEMPO_MARKER_MAX];

    // --- Osc 3 identity/tuning -- vco3_vol/pan/mute/pulse_width already declared above. ---
    float vco3_shape; // WaveType, full palette -- matches vco1_shape/vco2_shape
    float vco3_fine_tune; // cents, mirrors vco1_fine_tune
    float vco3_pitch_offset; // semitones, mirrors vco2_pitch_offset -- Osc3 is a full peer
    float vco3_phase_offset; // degrees, mirrors vco1/vco2_phase_offset
    float vco3_unison_saw_detune;
    float vco3_unison_saw_width;

    // --- Osc 4 identity/tuning, mirrors the Osc 3 block above exactly. vco4_vol/pan/mute/
    // pulse_width already declared above. ---
    float vco4_shape; // WaveType, full palette -- matches vco1/2/3_shape
    float vco4_fine_tune; // cents
    float vco4_pitch_offset; // semitones (set to -12 to reproduce the historical Sub's fixed octave-down)
    float vco4_phase_offset; // degrees
    float vco4_unison_saw_detune;
    float vco4_unison_saw_width;

    // --- FM Algorithm system: a fixed, cycle-free set of selectable 4-operator routing
    // topologies (see FM_ALGORITHMS/render_fm_oscillator) -- NOT a general routing matrix, to
    // keep every topology provably acyclic. fm_intensity (above) keeps its exact existing
    // meaning (edge Osc2->Osc1) in every algorithm that uses that edge. Algorithm 0 ("Classic",
    // 2->1 only) reproduces the historical default behavior. ---
    float fm_algorithm; // 0-11, see FM_ALGORITHMS
    float fm_depth_3_1; // Osc3 -> Osc1
    float fm_depth_3_2; // Osc3 -> Osc2
    float fm_depth_4_1; // Osc4 -> Osc1
    float fm_depth_4_3; // Osc4 -> Osc3
    float fm_depth_1_2; // Osc1 -> Osc2
    float fm_depth_3_4; // Osc3 -> Osc4
    float fm_depth_1_3; // Osc1 -> Osc3

    // --- Manual oscillator Pitch knobs --- a per-oscillator pitch offset the user can dial by
    // hand, independent of fine_tune (cents trim) and pitch_offset (the semitone-octave knob
    // Osc2/3/4 already have). Range +-24 semitones (+-2 octaves), stored directly in cents to
    // match mod_vcoN_pitch's own units. Summed into the exact same mod_vcoN_pitch accumulator an
    // LFO/EG already modulating "Osc N Pitch" writes into (see render_audio_block).
    float vco1_pitch;
    float vco2_pitch;
    float vco3_pitch;
    float vco4_pitch;

    // Monophonic Noise (Noise shape only -- meaningless/unread for any other shape, matching the
    // dual-duty pulse_width-or-tilt convention's own shape-conditional posture). When set, every
    // currently-sounding voice on this oscillator shares ONE noise generator for the whole
    // Program instead of each voice computing its own independent white noise -- the classic
    // "one physical noise source feeding the whole keyboard" behavior real analog synths have
    // (as opposed to N uncorrelated per-voice generators, which average toward smoother/thicker
    // noise under a chord purely as a side effect of polyphony). Computed once per
    // Program+oscillator+sample, before the voice loop -- see mono_noise_out[]/
    // mono_noise_tilt_lp[] in render_audio_block. Default off (0.0f) reproduces today's exact
    // per-voice-independent-noise behavior.
    float vco1_mono_noise;
    float vco2_mono_noise;
    float vco3_mono_noise;
    float vco4_mono_noise;

    // Velocity -> Amp, per oscillator (-100..100, bipolar -- same convention as PackLfoParams'
    // own vel_track knob). 0 (default) means a note's velocity has zero effect on this
    // oscillator's output level, reproducing today's exact velocity-insensitive behavior; positive
    // makes harder hits louder, negative makes harder hits quieter. Deliberately per-oscillator
    // rather than one global depth: since all 4 oscillators already read the SAME note's velocity
    // (MIDI gives one velocity per note-on, not per-oscillator), independent depths let a patch
    // velocity-crossfade BETWEEN oscillator layers -- e.g. a sub layer staying constant while a
    // brighter overtone layer fades in on harder hits -- not just scale the whole voice uniformly.
    // Applied as a per-voice gain multiplier alongside each oscillator's own Amp EG in
    // render_audio_block (see the vel_mult computation there), not as a mod-matrix destination --
    // velocity is a one-shot value captured at note-on (voice->midi_velocity), not a continuously-
    // varying signal like an LFO/EG, so it doesn't fit that per-sample-modulated system.
    float vco1_vel_to_amp;
    float vco2_vel_to_amp;
    float vco3_vel_to_amp;
    float vco4_vel_to_amp;

    // Curve/Drive: one physical pot per oscillator, reinterpreted by that oscillator's own Shape
    // -- bipolar Curve (-1..1, center detent = perfectly linear ramp/slope) for Sawtooth/
    // Triangle, unipolar Drive/Saturation (0..1, default 0 = pure clean sine) for Sine, ignored
    // for every other shape. See apply_wave_curve()/apply_sine_drive() (lookup_osc_sample).
    float vco1_curve_drive;
    float vco2_curve_drive;
    float vco3_curve_drive;
    float vco4_curve_drive;
} ProgramState;

typedef struct {
    // COMBI_PROGRAM_COUNT (8) independently-synthesizing Programs, plus one dedicated 9th slot
    // for Single Program mode -- see ProgramState's own comment above for the full mutual-
    // exclusivity design. programs[0..7] are Combi mode's 8 real lanes, addressed under
    // /hds/combi/{0-7}/...; programs[SINGLE_PROGRAM_SLOT] is Single Program mode's own Program,
    // addressed via the exact same OSC paths as before this struct existed (e.g.
    // /hds/vco1/shape) -- NOT the same storage as any Combi lane.
    ProgramState programs[COMBI_PROGRAM_COUNT + 1]; // index 8 = SINGLE_PROGRAM_SLOT, see above

    // Session/mix-level output volume -- deliberately NOT part of ProgramState (excluded from
    // Program scope for Save/Load too): a mixing/session-level level, not part of any one patch,
    // so loading a Program never yanks the current output level out from under whatever it was
    // set to.
    float master_volume;

    // Global performance transpose, in semitones (-16..16, centered at 0) -- same "deliberately
    // NOT part of ProgramState" reasoning as master_volume just above: a live performance
    // control (like a hardware keyboard's own Transpose button), not a property of any one
    // patch, so switching Programs never yanks the current transposition out from under
    // whatever the performer set it to. Applied every block, alongside pitch bend, as an
    // additive cents offset (see mod_vco{1-4}_pitch in render_audio_block) -- not baked into
    // voice->midi_note -- so it retunes already-held notes live and never disturbs Key Track,
    // Per-Key LFO, or note-off voice matching, all of which depend on midi_note reflecting the
    // physically played key.
    float transpose_semitones;

    // Microtuning (Scala .scl import -- see hds_note_to_hz()/HdsTuningTable's own comment).
    // Session-level, not per-Program, same rationale as transpose_semitones above -- which scale
    // you're playing in is a property of this performance, not of any one patch. tuning_enable
    // off (default) reproduces standard 12-TET exactly regardless of whatever's loaded.
    // tuning_reference_note/tuning_reference_freq pick which MIDI note is the scale's own 1/1 and
    // what frequency that note should sound at -- defaults (60, 261.6256Hz) are Middle C at
    // standard 12-TET pitch, so loading an ordinary 12-tone scale with unremarkable ratios lands
    // close to familiar tuning rather than transposed to an unexpected register.
    float tuning_enable;         // 0/1
    float tuning_reference_note; // MIDI note 0-127
    float tuning_reference_freq; // Hz

    // --- FX Bus + Multi-Source Routing Architecture --- deliberately session/rig-level, not
    // per-Program (see ProgramState's own comment for why): FX slots are shared, physical-insert-
    // like resources any source (a Program, the whole Combi stack, or an ext-in pair) can be
    // routed into via fx[].fx_source, not part of any one patch.

    // External Audio Input, per stereo pair: EXT_IN_PAIR_COUNT independently enable/gain/routable
    // stereo sources -- pair p reads capture channels 2p/2p+1. Each pair is its own routable
    // source (see fx[].fx_source/source_routing below) -- no pan control here: capture is taken
    // as an already-stereo L/R pair and passed through as-is rather than re-panned from mono.
    float ext_in_enable[EXT_IN_PAIR_COUNT];
    float ext_in_gain[EXT_IN_PAIR_COUNT];

    // Routing tab visibility (sent by the frontend on every QTabWidget.currentChanged, see
    // frontend.py's own comment) -- gates render_audio_block's per-ext-in-pair peak metering
    // (see ext_in_peak_out) so that cost is only ever paid while a player is actually looking at
    // the Routing tab's meters, not on every block for the life of the process. Session-level,
    // not Program-scoped -- a GUI-visibility flag, not a patch or even a rig property.
    float gui_routing_tab_visible; // 0/1

    // FX Bus: FX_SLOT_COUNT slots, each assigned to one source (fx[].fx_source -- see
    // SOURCE_COUNT's own comment for the full source enumeration: Combi Programs, the Combi Mix
    // bus, and ext-in pairs) rather than implicitly processing one shared mix -- see
    // process_fx_chain()'s source_id parameter. Each ported effect fills in its own case in that
    // function's dispatch switch; until then every slot is a pass-through (see FX_TYPE_OFF).
    FXSlotParams fx[FX_SLOT_COUNT];

    // FX Bus routing, per source: 0 = Series (that source's slots run in array order, each fed
    // the previous one's output), 1 = Parallel (every one of that source's slots fed the SAME
    // pre-FX input independently, their outputs summed and averaged -- see process_fx_chain()).
    float fx_source_routing_mode[SOURCE_COUNT];

    // Multi-output hardware routing: which physical output pair(s) each source's final signal
    // (its dry bus if it owns no FX slots, or the result of its own Series/Parallel chain above)
    // lands on -- a many-to-many grid, since a source can feed multiple output pairs at once
    // (e.g. a Program to both a main mix and a monitor/cue pair). See render_audio_block's
    // per-output-pair summing and the always-on limiter (once per output pair).
    SourceRouting source_routing[SOURCE_COUNT];

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

    // Tempo, FM algorithm, Osc 3/4 identity, and the manual oscillator Pitch knobs all moved into
    // ProgramState above (each is Program-scoped) -- see that struct's own field-by-field
    // comments; programs[SINGLE_PROGRAM_SLOT] carries the exact same fields under the exact same
    // OSC addresses as before this struct existed.

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

    // --- Combi/Sequencer scaffolding (data model + wire only -- see SEQ_LANE_COUNT's own
    // comment near the top of this file, and SeqLaneState's comment, for what this does and
    // does NOT do yet). performance_mode: 0 = Single Program (today's only real mode, unchanged),
    // 1 = Combi, 2 = Sequencer. (+1 float for the mode, +5*SEQ_LANE_COUNT for the lanes)
    float performance_mode;
    SeqLaneState seq_lanes[SEQ_LANE_COUNT];

    // Tempo sync source: 0 = Internal (the Tempo knob/tempo-map, today's only behavior, unchanged
    // default), 1 = External MIDI Clock -- slaves current_tempo_bpm to incoming MIDI Clock (0xF8)
    // messages instead (see hds_handle_midi_clock_tick()/main()'s own current_tempo_bpm
    // resolution), and lets incoming MIDI Start/Continue/Stop (0xFA/0xFB/0xFC) drive the
    // transport the same way the Sequencer's own Play/Pause/Stop buttons do (see
    // hds_handle_midi_transport_start/continue/stop()) -- gated behind this flag so a MIDI device
    // that happens to relay clock/realtime bytes can never hijack the transport for someone not
    // using external sync at all. Falls back to Internal automatically if no clock tick has
    // arrived recently (see HDS_MIDI_CLOCK_TIMEOUT_SEC) -- an unplugged/disconnected external
    // source degrades to the Tempo knob rather than freezing at a stale BPM.
    float seq_sync_source;

    // Sequencer transport (Play/Pause/Stop) -- see poll_transport_triggers's own comment. Each
    // is a plain edge-triggered counter, same idiom as SeqLaneState.audio_preview_trigger: the
    // frontend increments it by 1 on every button click and resends the whole packet.
    float seq_transport_play_trigger;
    float seq_transport_pause_trigger;
    float seq_transport_stop_trigger;
    // Seek: same edge-triggered-counter idiom as Play/Pause/Stop above. seek_start jumps the
    // playhead to 0:00 ("very beginning" reset); seek_back/seek_fwd move it by one whole measure
    // (4 beats at the engine's current tempo). All three work in every transport state (Stopped/
    // Paused/Playing), repositioning every lane's own playback position to match, not just the
    // display.
    float seq_transport_seek_start_trigger;
    float seq_transport_seek_back_trigger;
    float seq_transport_seek_fwd_trigger;
    // Seek-to-arbitrary-position: jumps straight to seq_transport_seek_to_target_sec (seconds from
    // the top) instead of a relative one-measure step -- what "jump to marker" (Sequencer tab's
    // Markers list, populated by an imported Standard MIDI File's own text markers, or added by
    // hand) actually sends. Same edge-triggered-counter idiom as every other transport trigger
    // here: the frontend sets the target field, then increments the trigger to fire the jump, so a
    // second click on the SAME marker (same target_sec) still fires (the counter changed even
    // though the target didn't).
    float seq_transport_seek_to_target_sec;
    float seq_transport_seek_to_trigger;

    // Quantize: rounds a Rec-armed lane's recorded note-ON timestamps to the nearest grid line at
    // the engine's current tempo, so a slightly-early/late live take still lines up rhythmically
    // on playback -- the same idea as a DAW's/hardware sequencer's record-quantize. Note-OFF
    // timestamps are deliberately left as-played (only a note's START snaps to the grid) so note
    // durations still reflect how long the key was actually held, rather than every note
    // quantizing to identical grid-aligned lengths. Applied in seq_record_midi_event(), which
    // already has this block's current_tempo_bpm threaded in from main(). seq_quantize_resolution
    // is "grid divisions per beat" (a quarter note = 1 beat, matching every other tempo-based calc
    // in this file, e.g. poll_metronome's beat_sec) -- 1=quarter, 2=eighth, 3=eighth triplet,
    // 4=sixteenth, 6=sixteenth triplet, 8=32nd -- a plain divisor rather than a musical-fraction
    // enum, so the GUI's resolution dropdown can offer exactly these values with no shared
    // note-name vocabulary needed between frontend and backend.
    float seq_quantize_enable;      // 0/1
    float seq_quantize_resolution;  // grid divisions per beat, see above (default 4 = sixteenth notes)

    // Metronome: a plain on/off toggle (not a trigger -- this is a sustained state, unlike the
    // edge-triggered transport buttons above), engaging a cheap, hardcoded 400Hz/8ms square-wave
    // click on each beat while the transport is PLAYING. See poll_metronome()/the click-render
    // block in render_audio_block. Deliberately session-level, not Program-scoped (a monitoring
    // aid, not patch content) -- excluded from Program save/load same as performance_mode.
    float metronome_enable;

    // Combi mode: which incoming MIDI channel each of the 8 Program lanes listens on (0 = Omni,
    // 1-16 = a specific channel) -- same convention as SeqLaneState.midi_channel, filtering LIVE
    // note dispatch (process_midi_input, in Combi mode) to whichever lane(s) match, rather than
    // Sequencer's own recording filter. Session-level, not part of any one Program -- which
    // channel a lane listens on is a property of the Combi performance setup, not the patch
    // itself.
    float combi_lane_midi_channel[COMBI_PROGRAM_COUNT];

    // Combi mode: per-lane key range (MIDI note 0-127, inclusive) and velocity range (1-127,
    // inclusive) -- lets a lane act as a keyboard split ("only sound above C3") and/or a
    // velocity switch ("only sound on hits above 100"), same session-level, not-part-of-any-
    // Program placement as combi_lane_midi_channel just above (a keyboard split is a property of
    // this particular Combi setup, not of the patch itself). Defaults (0-127 / 1-127, the full
    // range) reproduce today's exact "every note/velocity reaches every channel-matched lane"
    // behavior -- see dispatch_midi_event's Combi-mode branch, the only place these are read.
    // Velocity range gates note-ON only, deliberately never note-OFF (a note-off's own "velocity"
    // is release velocity, an unrelated value -- gating release by it would risk a stuck note);
    // key range gates both note-on and note-off identically, since a note's pitch never changes
    // between its own on/off pair.
    float combi_lane_key_lo[COMBI_PROGRAM_COUNT];
    float combi_lane_key_hi[COMBI_PROGRAM_COUNT];
    float combi_lane_vel_lo[COMBI_PROGRAM_COUNT];
    float combi_lane_vel_hi[COMBI_PROGRAM_COUNT];

    // Combi mode: per-lane transpose, in semitones (-48..48, centered at 0) -- lets the same or
    // different Programs be stacked at different octaves for a thicker layered sound ("octave
    // layering"). Session-level, same placement rationale as combi_lane_midi_channel/key_lo above
    // (a property of THIS Combi performance setup, not of any one patch) -- deliberately NOT
    // inside ProgramState, so re-picking a different Program into a lane never disturbs whatever
    // octave that lane was set to play at. Applied as an additive cents offset alongside the
    // global transpose_semitones knob (see mod_vco{1-4}_pitch in render_audio_block) -- the two
    // compose cleanly since both are just more terms in the same additive-cents accumulator pitch
    // bend and pitch_offset already use.
    float combi_lane_transpose[COMBI_PROGRAM_COUNT];

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

    // A user-settable label purely for the Instance Switcher's own scan results (see
    // ping_handler()/subscribe_handler() and README's own section on this) -- lets "port 9012" show
    // up as "Track 3 Guitar Amp" instead. Session/rig identity, not patch content, so it's excluded
    // from Program scope (_is_program_scope_address() in frontend.py) exactly like master_volume/
    // transpose_semitones are -- loading a Program should never rename the instance it's loaded
    // into. Empty by default; the frontend falls back to displaying host:port when empty.
    char instance_name[AUDIO_DEVICE_NAME_LEN];

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
    // Tempo is Program-scoped (see the Program Save/Load allowlist), so "the current tempo" has
    // to resolve to a specific slot -- Combi lane 1 (programs[0]) in Combi mode, matching every
    // other per-Program precompute's own resolution; Single Program mode's own dedicated slot
    // otherwise. Byte-identical to before this fix outside Combi mode (both were programs[0]
    // before Single Program mode and Combi lane 1 were split apart).
    const ProgramState *prog = &knobs->programs[((int)knobs->performance_mode == 1) ? 0 : SINGLE_PROGRAM_SLOT];
    int count = (int)prog->tempo_marker_count;
    if (count < 0) count = 0;
    if (count > TEMPO_MARKER_MAX) count = TEMPO_MARKER_MAX;
    if (count == 0) return prog->tempo_bpm;

    int prev = -1; double prev_time = -1e18;
    int next = -1; double next_time = 1e18;
    for (int i = 0; i < count; i++) {
        double t = (double)prog->tempo_marker_time[i];
        if (t <= elapsed_seconds && t > prev_time) { prev = i; prev_time = t; }
        if (t > elapsed_seconds && t < next_time) { next = i; next_time = t; }
    }
    if (prev < 0 && next < 0) return prog->tempo_bpm; // defensive fallback, shouldn't happen when count > 0
    if (prev < 0) return prog->tempo_marker_bpm[next]; // before the first marker: hold its bpm flat
    if (next < 0) return prog->tempo_marker_bpm[prev]; // after the last marker: hold its bpm flat

    double span = next_time - prev_time;
    double frac = (span > 1e-9) ? (elapsed_seconds - prev_time) / span : 0.0;
    double bpm = (double)prog->tempo_marker_bpm[prev] + frac * ((double)prog->tempo_marker_bpm[next] - (double)prog->tempo_marker_bpm[prev]);
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
    int program_index; // which of knobs->programs[COMBI_PROGRAM_COUNT] this voice's note belongs
                        // to (0 in Single Program / Sequencer modes; 0-7 in Combi mode). Stamped
                        // once at claim time in handle_note_on -- the voice-search/steal logic
                        // itself stays completely program-agnostic, exactly as it is today.

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

    // The 5 filter slots' SVF register banks: [channel: 0=L,1=R][filter slot: 0-4][cascade
    // stage: 2, 2nd stage only used when that slot's slope is set to 4-pole]. Independent per-
    // voice AND per-channel -- each of the 5 filters runs as two fully independent mono filters
    // (one per stereo channel) so that each oscillator's Pan placement survives the trip through
    // the filters instead of being collapsed to mono first.
    float f_low[2][5][2];
    float f_band[2][5][2];

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

    // Per-Key LFO state (see PackLfoParams.per_key_mode) -- one independent phase/decimation
    // state per LFO slot, PER VOICE, used only for LFOs actually in Per-Key mode (meaningless/
    // unread otherwise, exactly like vco*_sample_stream above when that oscillator isn't
    // WAVE_SAMPLE). Mirrors the Program-level lfo_decim_* function-local statics in
    // render_audio_block exactly, just duplicated per voice instead of shared per Program.
    float lfo_phase[LFO_COUNT];
    int lfo_decim_counter[LFO_COUNT];
    float lfo_decim_last_value[LFO_COUNT];
    float lfo_decim_target_value[LFO_COUNT];
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
static EqBiquadCoeffs compute_eq_band_coeffs(const EqBandParams *band, int band_index) {
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

// apply_program_eq() (cascades all 3 bands over one Program's bus) lives further down, right
// before render_audio_block -- it needs HdsEngineState, which isn't a complete type yet at this
// point in the file (see HdsEngineState's own comment on why it's declared so late).

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

// --- FX_TYPE_TAPEDELAY2: ported from Airwindows TapeDelay2 (MIT license) --- see this effect's
// own FX_TYPE_TAPEDELAY2 comment above for the full param map.
//
// Single-tap tape echo: a variable-speed (tape-flutter-modulated) delay line, feedback path run
// through a resonant lowpass ("regen" filter, both channels sharing one set of coefficients) so
// each successive repeat darkens progressively, then a second, slightly higher-resonance lowpass
// on the way OUT of the delay. Both filters are the same biquad topology (state-variable-style
// lowpass, coefficients recomputed from Freq/Reso every call -- cheap, and this dispatch is
// already per-sample, matching every other ported effect here) with their own persistent
// numerator/denominator state (index [7]/[8]) untouched by that recompute.
//
// The variable-speed write/read scheme (delayL/prevSampleL/increment below) is a real
// resampling-safe delay line, not a fixed integer tap: each sample, the write cursor walks
// backward by `speed` positions (fractional), linearly filling in the ring buffer between the
// previous and current cursor position so a later, differently-sped read never hits a stale or
// skipped sample -- this is what makes the tape-flutter pitch modulation sound smooth rather than
// steppy. Buffer size/wrap constant (88200) kept exactly as the original's own 44.1kHz-baseline
// sizing (matching FX_TYPE_ENSEMBLE's own precedent above of never rescaling a ported effect's
// fixed buffer sizes for SAMPLE_RATE) -- Time/Flutter's musical range shifts very slightly at our
// fixed 48kHz vs. the original's variable host rate, exactly the same "close enough, not worth a
// second set of tuning constants" tradeoff already accepted by every other port in this file.
typedef struct {
    float dL[88211], dR[88211];
    float prevSampleL, delayL, sweepL;
    float prevSampleR, delayR, sweepR;
    float regenFilterL[9], outFilterL[9], lastRefL[10];
    float regenFilterR[9], outFilterR[9], lastRefR[10];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxTapeDelay2State;

static void run_fx_tapedelay2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxTapeDelay2State *st = (FxTapeDelay2State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxTapeDelay2State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    // param[0..5] A-F, per this effect's own FX_TYPE_TAPEDELAY2 comment above.
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Time (inverted -- see the comment above)
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Regen
    float c = fmaxf(0.0f, fminf(1.0f, params->param[2])); // Freq
    float d = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Reso
    float e = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Flutter
    float f = fmaxf(0.0f, fminf(1.0f, params->param[5])); // Dry/Wet
    // param[6] Ping-Pong -- see this effect's own comment above and the cross-feed/divergence
    // just below. Off (0, the default) reproduces the pre-existing wiring exactly, bit for bit.
    // Stereo Ping-Pong (1) cross-feeds L/R's feedback AND diverges their delay time -- an
    // in-between "cross-feed only, same shared time" option existed briefly but was cut: it's a
    // strict subset of what Stereo Ping-Pong already does, and without the time divergence it's
    // the same easy-to-miss, barely-audible effect Ping-Pong originally shipped as (see this
    // effect's own comment history) -- no reason to keep a weaker redundant option once the
    // stronger one exists. Only decoded when the slot's Type is actually this effect, per the
    // "choice" param convention (see FX_TYPE_METADATA's own comment in frontend.py).
    bool stereo_pingpong = ((int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 1.999f)) != 0;

    float baseSpeed = (powf(a, 4.0f) * 25.0f) + 1.0f;
    float feedback = b * b;

    st->regenFilterL[0] = st->regenFilterR[0] = (powf(c, 3.0f) * 0.4f) + 0.0001f;
    st->regenFilterL[1] = st->regenFilterR[1] = (d * d) + 0.01f;
    float K = tanf((float)M_PI * st->regenFilterR[0]);
    float norm = 1.0f / (1.0f + K / st->regenFilterR[1] + K * K);
    st->regenFilterL[2] = st->regenFilterR[2] = K / st->regenFilterR[1] * norm;
    st->regenFilterL[4] = st->regenFilterR[4] = -st->regenFilterR[2];
    st->regenFilterL[5] = st->regenFilterR[5] = 2.0f * (K * K - 1.0f) * norm;
    st->regenFilterL[6] = st->regenFilterR[6] = (1.0f - K / st->regenFilterR[1] + K * K) * norm;

    st->outFilterL[0] = st->outFilterR[0] = st->regenFilterR[0];
    st->outFilterL[1] = st->outFilterR[1] = st->regenFilterR[1] * 1.618033988749894848f; // golden ratio, matching the original exactly
    K = tanf((float)M_PI * st->outFilterR[0]);
    norm = 1.0f / (1.0f + K / st->outFilterR[1] + K * K);
    st->outFilterL[2] = st->outFilterR[2] = K / st->outFilterR[1] * norm;
    st->outFilterL[4] = st->outFilterR[4] = -st->outFilterR[2];
    st->outFilterL[5] = st->outFilterR[5] = 2.0f * (K * K - 1.0f) * norm;
    st->outFilterL[6] = st->outFilterR[6] = (1.0f - K / st->outFilterR[1] + K * K) * norm;

    float vibSpeed = powf(e, 5.0f) * baseSpeed * ((st->regenFilterR[0] * 0.09f) + 0.025f);
    float wet = fmaxf(0.0f, fminf(1.0f, f * 2.0f));
    float dry = fmaxf(0.0f, fminf(1.0f, 2.0f - (f * 2.0f)));
    // This echo makes 50% full dry AND full wet, not crossfaded (see the FX_TYPE_TAPEDELAY2
    // comment above) -- so it can sit on a submix without cutting back the dry signal just
    // because this effect's own Dry/Wet got turned up. The slot's own generic Mix knob (applied
    // by process_fx_chain around this function's whole output) is a normal crossfade, layered on
    // top exactly like FX_TYPE_STEREODOUBLER's own kept Dry/Wet already is.

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;
    float drySampleL = inputSampleL, drySampleR = inputSampleR;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float speedL = baseSpeed + (vibSpeed * (sinf(st->sweepL) + 1.0f));
        float speedR = baseSpeed + (vibSpeed * (sinf(st->sweepR) + 1.0f));
        st->sweepL += (0.05f * inputSampleL * inputSampleL); if (st->sweepL > 6.283185307179586f) st->sweepL -= 6.283185307179586f;
        st->sweepR += (0.05f * inputSampleR * inputSampleR); if (st->sweepR > 6.283185307179586f) st->sweepR -= 6.283185307179586f;

        // Stereo Ping-Pong: spread L/R symmetrically around the Time knob's own shared center
        // instead of the two channels always sharing one identical delay rate -- a fixed, tasteful
        // +/-15% (chosen by ear, not derived from anything) rather than a further knob, per the
        // user's own "easily integrate... as an option" ask. Genuinely new time divergence, not
        // just alternating the SAME echo between speakers -- both channels still ultimately feed
        // off the one Time/Regen/Freq/Reso setting, they just no longer land at the same moment.
        // Off: stays at 1.0x, unaffected.
        if (stereo_pingpong) {
            speedL *= 0.85f;
            speedR *= 1.15f;
        }

        // Ping-Pong cross-feed point: capture each channel's CURRENT delay-tap value before
        // either channel's own write step touches it, so the choice of self-feed (Off, default)
        // vs. cross-feed (Stereo Ping-Pong) is a pure selection between two already-computed
        // values -- nothing else about the algorithm changes either way. With stereo_pingpong ==
        // false, feedSampleL/R are exactly dL[posL]/dR[posR] -- byte-for-byte the original's own
        // self-feedback wiring.
        int posL = (int)floorf(st->delayL);
        int posR = (int)floorf(st->delayR);
        float feedFromL = st->dL[posL];
        float feedFromR = st->dR[posR];
        float feedSampleL = stereo_pingpong ? feedFromR : feedFromL;
        float feedSampleR = stereo_pingpong ? feedFromL : feedFromR;

        // begin left channel
        float newSample = inputSampleL + feedSampleL * feedback;
        float tempSample = (newSample * st->regenFilterL[2]) + st->regenFilterL[7];
        st->regenFilterL[7] = -(tempSample * st->regenFilterL[5]) + st->regenFilterL[8];
        st->regenFilterL[8] = (newSample * st->regenFilterL[4]) - (tempSample * st->regenFilterL[6]);
        newSample = tempSample;

        st->delayL -= speedL; if (st->delayL < 0.0f) st->delayL += 88200.0f;
        float increment = (newSample - st->prevSampleL) / speedL;
        int pos = posL;
        st->dL[pos] = st->prevSampleL;
        while (pos != (int)floorf(st->delayL)) {
            st->dL[pos] = st->prevSampleL;
            st->prevSampleL += increment;
            pos--; if (pos < 0) pos += 88200;
        }
        st->prevSampleL = newSample;
        pos = (int)floorf(st->delayL); inputSampleL = st->dL[pos];
        tempSample = (inputSampleL * st->outFilterL[2]) + st->outFilterL[7];
        st->outFilterL[7] = -(tempSample * st->outFilterL[5]) + st->outFilterL[8];
        st->outFilterL[8] = (inputSampleL * st->outFilterL[4]) - (tempSample * st->outFilterL[6]);
        inputSampleL = tempSample;
        // end left channel

        // begin right channel
        newSample = inputSampleR + feedSampleR * feedback;
        tempSample = (newSample * st->regenFilterR[2]) + st->regenFilterR[7];
        st->regenFilterR[7] = -(tempSample * st->regenFilterR[5]) + st->regenFilterR[8];
        st->regenFilterR[8] = (newSample * st->regenFilterR[4]) - (tempSample * st->regenFilterR[6]);
        newSample = tempSample;

        st->delayR -= speedR; if (st->delayR < 0.0f) st->delayR += 88200.0f;
        increment = (newSample - st->prevSampleR) / speedR;
        pos = posR;
        st->dR[pos] = st->prevSampleR;
        while (pos != (int)floorf(st->delayR)) {
            st->dR[pos] = st->prevSampleR;
            st->prevSampleR += increment;
            pos--; if (pos < 0) pos += 88200;
        }
        st->prevSampleR = newSample;
        pos = (int)floorf(st->delayR); inputSampleR = st->dR[pos];
        tempSample = (inputSampleR * st->outFilterR[2]) + st->outFilterR[7];
        st->outFilterR[7] = -(tempSample * st->outFilterR[5]) + st->outFilterR[8];
        st->outFilterR[8] = (inputSampleR * st->outFilterR[4]) - (tempSample * st->outFilterR[6]);
        inputSampleR = tempSample;
        // end right channel

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

    // Multi-pole average using lastRef[7]/[8] (a second, slower-tracking smoothing stage on top
    // of the ladder above) -- a no-op at cycle_end == 1 (always true at this engine's fixed
    // 48kHz), kept generic for the same "in case SAMPLE_RATE ever changes" reason as the ladder.
    switch (st->cycle_end) {
        case 4:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[7]) * 0.5f;
            st->lastRefL[7] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[7]) * 0.5f;
            st->lastRefR[7] = st->lastRefR[8];
            // fallthrough
        case 3:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[6]) * 0.5f;
            st->lastRefL[6] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[6]) * 0.5f;
            st->lastRefR[6] = st->lastRefR[8];
            // fallthrough
        case 2:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[5]) * 0.5f;
            st->lastRefL[5] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[5]) * 0.5f;
            st->lastRefR[5] = st->lastRefR[8];
            // fallthrough
        case 1:
        default:
            break; // no further averaging
    }

    if (wet < 1.0f) { inputSampleL *= wet; inputSampleR *= wet; }
    if (dry < 1.0f) { drySampleL *= dry; drySampleR *= dry; }
    inputSampleL += drySampleL; inputSampleR += drySampleR;
    // this is our submix echo dry/wet: 0.5 is BOTH at FULL VOLUME (see the comment above)

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

// --- FX_TYPE_POINTYGUITAR: ported from Airwindows PointyGuitar (MIT license) --- see this
// effect's own FX_TYPE_POINTYGUITAR comment above for the full param map and algorithm summary.
//
// angS/angA are the per-pole, per-band one-pole filter state (Position/Angle in this codebase's
// usual naming for this "IIR" smoother shape, named angS/angA here to match the original's own
// "angled" band-splitting terminology): angS[x][y] is band y's smoothed level within pole x,
// angA[x][y] its own rate-of-change tracker. angG[0,2,4,6,8] are the 5 real band gains (Presence/
// High/Mid/Low/Sub); angG[1,3,5,7] are the plain average of their neighbors (interpolated
// in-between bands, not independently controlled); angG[9] repeats angG[8] (Sub) as the "band 9"
// weight applied to whatever's left in the lowpass chain after 9 successive splits. angG[] is
// shared by both channels (it depends only on params, not per-channel signal), matching the
// original's own single shared array.
typedef struct {
    float angSL[18][12], angAL[18][12];
    float angSR[18][12], angAR[18][12];
    float iirHPositionL[37], iirHAngleL[37], iirBPositionL[37], iirBAngleL[37];
    float iirHPositionR[37], iirHAngleR[37], iirBPositionR[37], iirBAngleR[37];
    bool was_negative_l, was_negative_r;
    int zero_cross_l, zero_cross_r;
    float gateroller_l, gateroller_r;
    float gate_l, gate_r;
    uint32_t fpdL, fpdR;
} FxPointyGuitarState;

static void run_fx_pointyguitar(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxPointyGuitarState *st = (FxPointyGuitarState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxPointyGuitarState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0..9] A-J, per this effect's own FX_TYPE_POINTYGUITAR comment above.
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Drive
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Presence
    float c = fmaxf(0.0f, fminf(1.0f, params->param[2])); // High
    float d = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Mid
    float e = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Low
    float f = fmaxf(0.0f, fminf(1.0f, params->param[5])); // Sub
    float g = fmaxf(0.0f, fminf(1.0f, params->param[6])); // HSpeakr
    float h = fmaxf(0.0f, fminf(1.0f, params->param[7])); // LSpeakr
    float i_ = fmaxf(0.0f, fminf(1.0f, params->param[8])); // Gate
    float j = fmaxf(0.0f, fminf(1.0f, params->param[9])); // Output

    float overallscale = (float)SAMPLE_RATE / 44100.0f;

    float drive = a + 0.618033988749894f;
    float angG[10];
    angG[0] = sqrtf(b * 2.0f);
    angG[2] = sqrtf(c * 2.0f);
    angG[4] = sqrtf(d * 2.0f);
    angG[6] = sqrtf(e * 2.0f);
    angG[8] = sqrtf(f * 2.0f);
    angG[1] = (angG[0] + angG[2]) * 0.5f;
    angG[3] = (angG[2] + angG[4]) * 0.5f;
    angG[5] = (angG[4] + angG[6]) * 0.5f;
    angG[7] = (angG[6] + angG[8]) * 0.5f;
    angG[9] = angG[8];
    int poles = (int)(drive * 10.0f); // 6-16 across Drive's full 0..1 range
    if (poles > 17) poles = 17; // defensive: stay inside angSL/angAL's 18-row headroom regardless
    float hFreq = powf(g, overallscale);
    float lFreq = powf(h, overallscale + 3.0f);
    // begin Gate
    float onthreshold = (powf(i_, 3.0f) / 3.0f) + 0.00018f;
    float offthreshold = onthreshold * 1.1f;
    float release = 0.028331119964586f;
    float absmax = 220.9f;
    // end Gate
    float output = j;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    // begin Gate (left) -- zero-crossing-aware envelope follower on the CLEAN input, applied to
    // the DISTORTED output further down (see this effect's own comment above for why).
    if (inputSampleL > 0.0f) {
        if (st->was_negative_l) st->zero_cross_l = (int)(absmax * 0.3f);
        st->was_negative_l = false;
    } else {
        st->zero_cross_l += 1; st->was_negative_l = true;
    }
    if (st->zero_cross_l > absmax) st->zero_cross_l = (int)absmax;
    if (st->gateroller_l == 0.0f) {
        if (fabsf(inputSampleL) > onthreshold) {
            if (st->gateroller_l == 0.0f) st->gateroller_l = (float)st->zero_cross_l;
            else st->gateroller_l -= release;
        } else st->gateroller_l -= release;
    } else {
        if (fabsf(inputSampleL) > offthreshold) {
            if (st->gateroller_l < (float)st->zero_cross_l) st->gateroller_l = (float)st->zero_cross_l;
            else st->gateroller_l -= release;
        } else st->gateroller_l -= release;
    }
    if (st->gateroller_l < 0.0f) st->gateroller_l = 0.0f;
    // end Gate (left)

    for (int x = 0; x < poles; x++) {
        float fr = 0.9f / overallscale;
        float band = inputSampleL; inputSampleL = 0.0f;
        for (int y = 0; y < 9; y++) {
            st->angAL[x][y] = (st->angAL[x][y] * (1.0f - fr)) + ((band - st->angSL[x][y]) * fr);
            float temp = band; band = ((st->angSL[x][y] + (st->angAL[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            st->angSL[x][y] = ((st->angSL[x][y] + (st->angAL[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            inputSampleL += ((temp - band) * angG[y]);
            fr *= 0.618033988749894f;
        }
        inputSampleL += (band * angG[9]);
        inputSampleL *= drive;
        inputSampleL -= fminf(fmaxf((inputSampleL * (fabsf(inputSampleL) * 0.654f) * (fabsf(inputSampleL) * 0.654f)), -1.0f), 1.0f);
    }

    if (st->gateroller_l < 1.0f) {
        st->gate_l = st->gateroller_l;
        float bridgerectifier = 1.0f - cosf(fabsf(inputSampleL));
        if (inputSampleL > 0.0f) inputSampleL = (inputSampleL * st->gate_l) + (bridgerectifier * (1.0f - st->gate_l));
        else inputSampleL = (inputSampleL * st->gate_l) - (bridgerectifier * (1.0f - st->gate_l));
        if (st->gate_l == 0.0f) inputSampleL = 0.0f;
    } else st->gate_l = 1.0f;
    // end Gate (left, applied)

    float lowSample = inputSampleL;
    int lPoles = (int)(3.0f + (lFreq * 32.0f));
    for (int count = 0; count < lPoles; count++) {
        st->iirBAngleL[count] = (st->iirBAngleL[count] * (1.0f - lFreq)) + ((lowSample - st->iirBPositionL[count]) * lFreq);
        lowSample = ((st->iirBPositionL[count] + (st->iirBAngleL[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        st->iirBPositionL[count] = ((st->iirBPositionL[count] + (st->iirBAngleL[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        inputSampleL -= (lowSample * (1.0f / (3.0f + (lFreq * 32.0f))));
    }

    int hPoles = (int)(3.0f + (hFreq * 32.0f));
    for (int count = 0; count < hPoles; count++) {
        st->iirHAngleL[count] = (st->iirHAngleL[count] * (1.0f - hFreq)) + ((inputSampleL - st->iirHPositionL[count]) * hFreq);
        inputSampleL = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleL * hFreq);
        st->iirHPositionL[count] = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleL * hFreq);
    } // the lowpass
    inputSampleL *= output;

    // begin Gate (right)
    if (inputSampleR > 0.0f) {
        if (st->was_negative_r) st->zero_cross_r = (int)(absmax * 0.3f);
        st->was_negative_r = false;
    } else {
        st->zero_cross_r += 1; st->was_negative_r = true;
    }
    if (st->zero_cross_r > absmax) st->zero_cross_r = (int)absmax;
    if (st->gateroller_r == 0.0f) {
        if (fabsf(inputSampleR) > onthreshold) {
            if (st->gateroller_r == 0.0f) st->gateroller_r = (float)st->zero_cross_r;
            else st->gateroller_r -= release;
        } else st->gateroller_r -= release;
    } else {
        if (fabsf(inputSampleR) > offthreshold) {
            if (st->gateroller_r < (float)st->zero_cross_r) st->gateroller_r = (float)st->zero_cross_r;
            else st->gateroller_r -= release;
        } else st->gateroller_r -= release;
    }
    if (st->gateroller_r < 0.0f) st->gateroller_r = 0.0f;
    // end Gate (right)

    for (int x = 0; x < poles; x++) {
        float fr = 0.9f / overallscale;
        float band = inputSampleR; inputSampleR = 0.0f;
        for (int y = 0; y < 9; y++) {
            st->angAR[x][y] = (st->angAR[x][y] * (1.0f - fr)) + ((band - st->angSR[x][y]) * fr);
            float temp = band; band = ((st->angSR[x][y] + (st->angAR[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            st->angSR[x][y] = ((st->angSR[x][y] + (st->angAR[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            inputSampleR += ((temp - band) * angG[y]);
            fr *= 0.618033988749894f;
        }
        inputSampleR += (band * angG[9]);
        inputSampleR *= drive;
        inputSampleR -= fminf(fmaxf((inputSampleR * (fabsf(inputSampleR) * 0.654f) * (fabsf(inputSampleR) * 0.654f)), -1.0f), 1.0f);
    }

    if (st->gateroller_r < 1.0f) {
        st->gate_r = st->gateroller_r;
        float bridgerectifier = 1.0f - cosf(fabsf(inputSampleR));
        if (inputSampleR > 0.0f) inputSampleR = (inputSampleR * st->gate_r) + (bridgerectifier * (1.0f - st->gate_r));
        else inputSampleR = (inputSampleR * st->gate_r) - (bridgerectifier * (1.0f - st->gate_r));
        if (st->gate_r == 0.0f) inputSampleR = 0.0f;
    } else st->gate_r = 1.0f;
    // end Gate (right, applied)

    lowSample = inputSampleR;
    for (int count = 0; count < lPoles; count++) {
        st->iirBAngleR[count] = (st->iirBAngleR[count] * (1.0f - lFreq)) + ((lowSample - st->iirBPositionR[count]) * lFreq);
        lowSample = ((st->iirBPositionR[count] + (st->iirBAngleR[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        st->iirBPositionR[count] = ((st->iirBPositionR[count] + (st->iirBAngleR[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        inputSampleR -= (lowSample * (1.0f / (3.0f + (lFreq * 32.0f))));
    }

    for (int count = 0; count < hPoles; count++) {
        st->iirHAngleR[count] = (st->iirHAngleR[count] * (1.0f - hFreq)) + ((inputSampleR - st->iirHPositionR[count]) * hFreq);
        inputSampleR = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleR * hFreq);
        st->iirHPositionR[count] = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleR * hFreq);
    } // the lowpass
    inputSampleR *= output;

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

// --- FX_TYPE_GUITARCONDITIONER: ported from Airwindows GuitarConditioner (MIT license) --- see
// this effect's own FX_TYPE_GUITARCONDITIONER comment above for the full algorithm summary.
// Genuinely parameterless -- `params` is unused, matching the original's own kNumParameters == 0.
typedef struct {
    bool fp_flip;
    float last_sample_tl, last_sample_bl;
    float iir_sample_tal, iir_sample_tbl, iir_sample_bal, iir_sample_bbl;
    float last_sample_tr, last_sample_br;
    float iir_sample_tar, iir_sample_tbr, iir_sample_bar, iir_sample_bbr;
    uint32_t fpdL, fpdR;
} FxGuitarConditionerState;

static void run_fx_guitarconditioner(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    (void)params; // no params -- see this effect's own FX_TYPE_GUITARCONDITIONER comment above
    FxGuitarConditionerState *st = (FxGuitarConditionerState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxGuitarConditionerState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fp_flip = true; // matches the original's own constructor default
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float iirTreble = 0.287496f / overallscale; // tight is -1
    float iirBass = 0.085184f / overallscale;   // tight is 1
    iirTreble += iirTreble;
    iirBass += iirBass; // simple double when tight is -1 or 1
    const float tightBass = 0.6666666666f;
    const float tightTreble = -0.3333333333f;
    float threshTreble = 0.0081f / overallscale;
    float threshBass = 0.0256f / overallscale;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    float trebleL = inputSampleL, bassL = inputSampleL;
    float trebleR = inputSampleR, bassR = inputSampleR;
    trebleL += trebleL; // +3dB on treble as the highpass is higher
    trebleR += trebleR;

    float offset = (1.0f + tightTreble) + ((1.0f - fabsf(trebleL)) * tightTreble); // treble HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_tal = (st->iir_sample_tal * (1.0f - (offset * iirTreble))) + (trebleL * (offset * iirTreble));
        trebleL -= st->iir_sample_tal;
    } else {
        st->iir_sample_tbl = (st->iir_sample_tbl * (1.0f - (offset * iirTreble))) + (trebleL * (offset * iirTreble));
        trebleL -= st->iir_sample_tbl;
    } // done trebleL HP

    offset = (1.0f + tightTreble) + ((1.0f - fabsf(trebleR)) * tightTreble); // treble HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_tar = (st->iir_sample_tar * (1.0f - (offset * iirTreble))) + (trebleR * (offset * iirTreble));
        trebleR -= st->iir_sample_tar;
    } else {
        st->iir_sample_tbr = (st->iir_sample_tbr * (1.0f - (offset * iirTreble))) + (trebleR * (offset * iirTreble));
        trebleR -= st->iir_sample_tbr;
    } // done trebleR HP

    offset = (1.0f - tightBass) + (fabsf(bassL) * tightBass); // bass HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_bal = (st->iir_sample_bal * (1.0f - (offset * iirBass))) + (bassL * (offset * iirBass));
        bassL -= st->iir_sample_bal;
    } else {
        st->iir_sample_bbl = (st->iir_sample_bbl * (1.0f - (offset * iirBass))) + (bassL * (offset * iirBass));
        bassL -= st->iir_sample_bbl;
    } // done bassL HP

    offset = (1.0f - tightBass) + (fabsf(bassR) * tightBass); // bass HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_bar = (st->iir_sample_bar * (1.0f - (offset * iirBass))) + (bassR * (offset * iirBass));
        bassR -= st->iir_sample_bar;
    } else {
        st->iir_sample_bbr = (st->iir_sample_bbr * (1.0f - (offset * iirBass))) + (bassR * (offset * iirBass));
        bassR -= st->iir_sample_bbr;
    } // done bassR HP

    // Slew-limit each band's sample-to-sample delta (a hard cap on how fast it can move) --
    // ported directly, using each band's own value straight (the original round-trips it through
    // a same-value local first; behaviorally identical, just fewer temporaries).
    float clamp = trebleL - st->last_sample_tl;
    if (clamp > threshTreble) trebleL = st->last_sample_tl + threshTreble;
    if (-clamp > threshTreble) trebleL = st->last_sample_tl - threshTreble;
    st->last_sample_tl = trebleL; // trebleL slew

    clamp = trebleR - st->last_sample_tr;
    if (clamp > threshTreble) trebleR = st->last_sample_tr + threshTreble;
    if (-clamp > threshTreble) trebleR = st->last_sample_tr - threshTreble;
    st->last_sample_tr = trebleR; // trebleR slew

    clamp = bassL - st->last_sample_bl;
    if (clamp > threshBass) bassL = st->last_sample_bl + threshBass;
    if (-clamp > threshBass) bassL = st->last_sample_bl - threshBass;
    st->last_sample_bl = bassL; // bassL slew

    clamp = bassR - st->last_sample_br;
    if (clamp > threshBass) bassR = st->last_sample_br + threshBass;
    if (-clamp > threshBass) bassR = st->last_sample_br - threshBass;
    st->last_sample_br = bassR; // bassR slew

    inputSampleL = trebleL + bassL; // final merge
    inputSampleR = trebleR + bassR; // final merge
    st->fp_flip = !st->fp_flip;

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

// --- FX_TYPE_CABS2: ported from Airwindows Cabs2 (MIT license) --- see this effect's own
// FX_TYPE_CABS2 comment above for the full algorithm summary and param map.
typedef struct {
    float iirHPositionL[23], iirHAngleL[23], iirHPositionR[23], iirHAngleR[23];
    bool hBypass;
    float iirLPositionL[15], iirLAngleL[15], iirLPositionR[15], iirLAngleR[15];
    bool lBypass;
    uint32_t fpdL, fpdR;
} FxCabs2State;

static void run_fx_cabs2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxCabs2State *st = (FxCabs2State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxCabs2State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
        // hBypass/lBypass start false (calloc) -- matches the original's own constructor default.
    }

    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Lowpass
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Hipass
    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float lFreq = powf(fmaxf(a, 0.002f), overallscale); // the lowpass
    float hFreq = powf(b, overallscale + 2.0f);          // the highpass

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    if (hFreq > 0.0f) {
        float lowSampleL = inputSampleL, lowSampleR = inputSampleR;
        for (int count = 0; count < 21; count++) {
            st->iirHAngleL[count] = (st->iirHAngleL[count] * (1.0f - hFreq)) + ((lowSampleL - st->iirHPositionL[count]) * hFreq);
            lowSampleL = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleL * hFreq);
            st->iirHPositionL[count] = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleL * hFreq);
            inputSampleL -= (lowSampleL * (1.0f / 21.0f)); // left
            st->iirHAngleR[count] = (st->iirHAngleR[count] * (1.0f - hFreq)) + ((lowSampleR - st->iirHPositionR[count]) * hFreq);
            lowSampleR = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleR * hFreq);
            st->iirHPositionR[count] = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleR * hFreq);
            inputSampleR -= (lowSampleR * (1.0f / 21.0f)); // right
        } // the highpass
        st->hBypass = false;
    } else {
        if (!st->hBypass) {
            st->hBypass = true;
            memset(st->iirHPositionL, 0, sizeof(st->iirHPositionL));
            memset(st->iirHAngleL, 0, sizeof(st->iirHAngleL));
            memset(st->iirHPositionR, 0, sizeof(st->iirHPositionR));
            memset(st->iirHAngleR, 0, sizeof(st->iirHAngleR));
        } // blank out highpass if just switched off
    }

    if (lFreq < 1.0f) {
        for (int count = 0; count < 13; count++) {
            st->iirLAngleL[count] = (st->iirLAngleL[count] * (1.0f - lFreq)) + ((inputSampleL - st->iirLPositionL[count]) * lFreq);
            inputSampleL = ((st->iirLPositionL[count] + (st->iirLAngleL[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleL * lFreq);
            st->iirLPositionL[count] = ((st->iirLPositionL[count] + (st->iirLAngleL[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleL * lFreq); // left
            st->iirLAngleR[count] = (st->iirLAngleR[count] * (1.0f - lFreq)) + ((inputSampleR - st->iirLPositionR[count]) * lFreq);
            inputSampleR = ((st->iirLPositionR[count] + (st->iirLAngleR[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleR * lFreq);
            st->iirLPositionR[count] = ((st->iirLPositionR[count] + (st->iirLAngleR[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleR * lFreq); // right
        } // the lowpass
        st->lBypass = false;
    } else {
        if (!st->lBypass) {
            st->lBypass = true;
            memset(st->iirLPositionL, 0, sizeof(st->iirLPositionL));
            memset(st->iirLAngleL, 0, sizeof(st->iirLAngleL));
            memset(st->iirLPositionR, 0, sizeof(st->iirLPositionR));
            memset(st->iirLAngleR, 0, sizeof(st->iirLAngleR));
        } // blank out lowpass if just switched off
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

#if HDS_NAM_ENABLED
// Forward declaration -- the real definition lives much further down (it's Sequencer audio lanes'
// own WAV loader), but fx_nam_reload_ir() below needs to call it and run_fx_nam() itself needs to
// sit up here, before process_fx_chain()'s switch, matching every other ported effect's own
// position in this file.
static float *seq_audio_load_wav_from_disk(const char *path, int *out_frames, int *out_channels, int *out_sample_rate);

// --- FX_TYPE_NAM: Neural Amp Modeler (NeuralAmpModelerCore, MIT license) --- see FX_TYPE_NAM's
// own comment above for the param map and why this needed a small C++ bridge (nam_bridge.h/.cpp)
// at all. model_l/model_r are opaque nam::DSP* handles from that bridge -- genuinely two separate
// model instances loaded from the SAME file, never one instance shared between channels, because
// each carries real internal state (WaveNet dilated-conv history, LSTM hidden state) that must not
// leak between L and R the same way this file's other stereo effects (Cabs2's dual iirPosition/
// iirAngle arrays above, PointyGuitar's own per-channel bands) never share state either.
//
// Cab IR convolution is plain C, not more C++: NeuralAmpModelerCore's own wav.h only exposes a raw
// decode helper (load_wav_ir), not a ready convolution engine, and this file already has a working
// WAV decoder (seq_audio_load_wav_from_disk, used by Sequencer audio lanes) -- reusing it here
// avoids a second file-format dependency for the one new thing actually needed: the convolution
// loop itself, a plain per-sample ring-buffer dot product like every other hand-rolled effect in
// this file.
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
    st->ir_hist_l = calloc((size_t)taps, sizeof(float));
    st->ir_hist_r = calloc((size_t)taps, sizeof(float));
    st->ir_hist_pos_l = 0; st->ir_hist_pos_r = 0;
    if (!st->ir_hist_l || !st->ir_hist_r) { fx_nam_free_ir(st); st->ir_load_failed = true; return; }

    if (truncated) fprintf(stderr, "[nam] cab IR '%s' truncated to %d taps (was %d)\n", path, taps, frames);
    if (sample_rate != 0 && sample_rate != SAMPLE_RATE) {
        fprintf(stderr, "[nam] cab IR '%s' is %d Hz, engine runs at %d Hz -- loaded as-is, uncorrected\n", path, sample_rate, SAMPLE_RATE);
    }
}

// One tap of the direct-form FIR: writes this sample into the ring, then walks it oldest-to-newest
// against the IR (coeffs[0] is the IR's first/earliest tap) -- a plain per-sample convolution, the
// same shape as any other hand-rolled filter in this file, just with a data-driven (not fixed-size)
// coefficient count.
static inline float fx_nam_convolve_tap(float *hist, int taps, int *pos, const float *coeffs, float in) {
    hist[*pos] = in;
    float acc = 0.0f;
    int idx = *pos;
    for (int i = 0; i < taps; i++) {
        acc += coeffs[i] * hist[idx];
        idx--; if (idx < 0) idx = taps - 1;
    }
    *pos += 1; if (*pos >= taps) *pos = 0;
    return acc;
}

static void run_fx_nam(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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
#endif // HDS_NAM_ENABLED

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
// place after every owned slot. FXSlotParams.chain_to_next is entirely IGNORED in this mode --
// every slot is already chained unconditionally, so there's nothing left for it to express.
//
// 1 = Parallel, GROUPED: each owned slot starts a fresh "branch" fed by the SAME original dry
// input, UNLESS the previous owned slot had chain_to_next set, in which case this slot instead
// continues that branch -- fed by the previous slot's own output, not the shared dry input --
// exactly like Series would, just scoped to this one branch. A branch closes (and its final
// result is added into the running sum) the moment a slot doesn't continue it: either the next
// owned slot didn't ask to (chain_to_next left off, the default), or there is no next owned slot.
// The sum is then divided by the number of BRANCHES that closed, not the number of raw slots that
// participated -- chaining N slots together still counts as exactly one branch in the average, the
// same weight a single unchained slot would carry, matching how a parallel mixer bus should treat
// "one processed signal" whether it took one plugin or three to get there. An Off/bypassed slot
// still passes its own dry input through as its output unchanged (same "contributes its own
// unprocessed dry copy" rule this mode has always had -- unlike the flat filter chain's own
// Parallel mode, see render_audio_block's filter dispatch, where an Off slot is excluded from the
// sum entirely) and the branch it's part of continues exactly as if it had done real work.
// With no slot anywhere setting chain_to_next (every FXSlotParams' own default), every owned slot
// opens and immediately closes its own one-slot branch -- reproducing today's original Parallel
// behavior byte-for-byte, just computed through the more general branch bookkeeping.
//
// Both modes: a source using only 1 or 2 slots in Parallel isn't attenuated as if all 12 had
// participated (the divisor is real branch count, not FX_SLOT_COUNT). A source that owns zero
// slots (Series or Parallel alike) is a correct no-op: *l/*r pass through unchanged.
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
            // FX_TYPE_NAM's state owns real C++ objects (via the bridge) that a bare free() would
            // leak rather than destroy -- see fx_nam_free_state()'s own comment. Every other
            // effect's state is plain data, safe for the generic free() below exactly as before.
#if HDS_NAM_ENABLED
            if (fx_engine[s].active_type == FX_TYPE_NAM && fx_engine[s].state) {
                fx_nam_free_state((FxNamState *)fx_engine[s].state);
            }
#endif
            free(fx_engine[s].state);
            fx_engine[s].state = NULL;
            fx_engine[s].active_type = type;
        }
    }
}

static void process_fx_chain(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT], int routing_mode, int source_id, float *l, float *r) {
    float parallel_in_l = *l, parallel_in_r = *r;
    float parallel_sum_l = 0.0f, parallel_sum_r = 0.0f;
    int parallel_branch_count = 0;

    // Parallel-only branch state (unused, and harmless, in Series mode): branch_l/r is the
    // running output of whichever branch is currently open; branch_open is false until the first
    // owned slot of a new branch runs; continue_branch is true when the PREVIOUS owned slot's own
    // chain_to_next asked THIS slot to continue its branch rather than start a fresh one.
    float branch_l = parallel_in_l, branch_r = parallel_in_r;
    bool branch_open = false;
    bool continue_branch = false;

    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        if ((int)fx_params[s].fx_source != source_id) continue; // not this source's slot
        int type = fx_engine[s].active_type;

        float dry_l, dry_r;
        if (routing_mode == 0) {
            dry_l = *l; dry_r = *r; // Series -- chain_to_next ignored, always the running chain result
        } else if (continue_branch) {
            dry_l = branch_l; dry_r = branch_r; // Parallel, continuing an already-open branch
        } else {
            if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; }
            dry_l = parallel_in_l; dry_r = parallel_in_r; // Parallel, starting a fresh branch
            branch_open = true;
        }

        float out_l, out_r;
        if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            out_l = dry_l; out_r = dry_r; // pass-through, dry
        } else {
            float wet_l = dry_l, wet_r = dry_r;
            switch (type) {
                case FX_TYPE_ENSEMBLE: run_fx_ensemble(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CREAMCOAT: run_fx_creamcoat(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_DENSITY3: run_fx_density3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREOCHORUS: run_fx_stereochorus(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREODOUBLER: run_fx_stereodoubler(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_TAPEDELAY2: run_fx_tapedelay2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_POINTYGUITAR: run_fx_pointyguitar(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_GUITARCONDITIONER: run_fx_guitarconditioner(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CABS2: run_fx_cabs2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#if HDS_NAM_ENABLED
                case FX_TYPE_NAM: run_fx_nam(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#endif
                case FX_TYPE_OFF:
                default:
                    break; // pass-through
            }
            float mix = fmaxf(0.0f, fminf(100.0f, fx_params[s].mix)) / 100.0f;
            out_l = dry_l * (1.0f - mix) + wet_l * mix;
            out_r = dry_r * (1.0f - mix) + wet_r * mix;
        }

        if (routing_mode == 0) {
            *l = out_l;
            *r = out_r;
        } else {
            branch_l = out_l; branch_r = out_r;
            continue_branch = (fx_params[s].chain_to_next != 0.0f);
        }
    }

    if (routing_mode == 1) {
        if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; } // close the final still-open branch
        if (parallel_branch_count > 0) {
            *l = parallel_sum_l / (float)parallel_branch_count;
            *r = parallel_sum_r / (float)parallel_branch_count;
        }
        // parallel_branch_count == 0: *l/*r already untouched (still the original dry input),
        // which is the correct "this source owns no slots" passthrough.
    }
}

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
    float inv_n = 1.0f / (float)UNISON_SAW_VOICES;
    if (out_l) *out_l = sum_l * inv_n;
    if (out_r) *out_r = sum_r * inv_n;
    return sum_mono * inv_n;
}

// wave_type: 0=Triangle, 1=Sine, 2=Saw (descending ramp), 3=Square, 4=Off (never reaches here --
// callers short-circuit type==4 before calling). curve_drive: same single-pot, shape-reinterpreted
// value as lookup_osc_sample's own -- meaningful for Triangle/Saw (bipolar Curve) and Sine
// (unipolar Drive, clamped internally), ignored for Square.
float lookup_lfo_waveform(int type, float phase, float curve_drive) {
    switch (type) {
        case 1: return apply_sine_drive(sinf(phase * 2.0f * M_PI), curve_drive);
        case 2: { float warped = apply_wave_curve(1.0f - phase, curve_drive); return (warped * 2.0f) - 1.0f; }
        case 3: return (phase < 0.5f) ? 1.0f : -1.0f;
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
#define OSC_STRING_SUBARRAY(addr, arrfield, subtype, subfield, cnt, len) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt, 1, 0, 0, OSC_KIND_STRING, len }

// Program-scoped field variants (Combi mode, see ProgramState's own comment): identical shape to
// the macros above, but the base offset threads through SynthSharedControls.programs[N] before
// reaching the field. The _AT forms take an explicit compile-time program index N (used to
// generate the 8 real Combi lane address rows, /hds/combi/{0-7}/...); the plain (no _AT) forms
// default to SINGLE_PROGRAM_SLOT (index 8), so the existing, unprefixed addresses (e.g.
// /hds/vco1/shape) now address Single Program mode's own dedicated ProgramState -- NOT any Combi
// lane's storage, per the mutual-exclusivity fix (see ProgramState's own comment). Single Program
// mode and every existing saved .hdsprog are unaffected by this indirection -- same addresses,
// same wire format, just a different (and now correctly independent) target underneath.
#define OSC_PSCALAR_AT(addr, field, n) \
    { addr, offsetof(SynthSharedControls, programs[n].field), 0, 1, 0, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PARRAY_AT(addr, field, cnt, n) \
    { addr, offsetof(SynthSharedControls, programs[n].field), sizeof(((SynthSharedControls *)0)->programs[n].field[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PSUBARRAY_AT(addr, arrfield, subtype, subfield, cnt, n) \
    { addr, offsetof(SynthSharedControls, programs[n].arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->programs[n].arrfield[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PNESTED_AT(addr, arrfield, subtype, subfield, cnt1, cnt2, n) \
    { addr, offsetof(SynthSharedControls, programs[n].arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->programs[n].arrfield[0]), cnt1, 2, sizeof(float), cnt2, OSC_KIND_FLOAT, 0 }
#define OSC_PSCALAR(addr, field) OSC_PSCALAR_AT(addr, field, SINGLE_PROGRAM_SLOT)
#define OSC_PARRAY(addr, field, cnt) OSC_PARRAY_AT(addr, field, cnt, SINGLE_PROGRAM_SLOT)
#define OSC_PSUBARRAY(addr, arrfield, subtype, subfield, cnt) OSC_PSUBARRAY_AT(addr, arrfield, subtype, subfield, cnt, SINGLE_PROGRAM_SLOT)
#define OSC_PNESTED(addr, arrfield, subtype, subfield, cnt1, cnt2) OSC_PNESTED_AT(addr, arrfield, subtype, subfield, cnt1, cnt2, SINGLE_PROGRAM_SLOT)

static const OscParamEntry OSC_PARAMS[] = {
    // --- Baseline ---
    OSC_PSCALAR("/hds/is_polyphonic", is_polyphonic),
    OSC_PSCALAR_AT("/hds/combi/0/is_polyphonic", is_polyphonic, 0),
    OSC_PSCALAR_AT("/hds/combi/1/is_polyphonic", is_polyphonic, 1),
    OSC_PSCALAR_AT("/hds/combi/2/is_polyphonic", is_polyphonic, 2),
    OSC_PSCALAR_AT("/hds/combi/3/is_polyphonic", is_polyphonic, 3),
    OSC_PSCALAR_AT("/hds/combi/4/is_polyphonic", is_polyphonic, 4),
    OSC_PSCALAR_AT("/hds/combi/5/is_polyphonic", is_polyphonic, 5),
    OSC_PSCALAR_AT("/hds/combi/6/is_polyphonic", is_polyphonic, 6),
    OSC_PSCALAR_AT("/hds/combi/7/is_polyphonic", is_polyphonic, 7),
    OSC_PSCALAR("/hds/vco1/shape", vco1_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/shape", vco1_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/shape", vco1_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/shape", vco1_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/shape", vco1_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/shape", vco1_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/shape", vco1_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/shape", vco1_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/shape", vco1_shape, 7),
    OSC_PSCALAR("/hds/vco2/shape", vco2_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/shape", vco2_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/shape", vco2_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/shape", vco2_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/shape", vco2_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/shape", vco2_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/shape", vco2_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/shape", vco2_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/shape", vco2_shape, 7),
    OSC_PSCALAR("/hds/fm/intensity", fm_intensity),
    OSC_PSCALAR_AT("/hds/combi/0/fm/intensity", fm_intensity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/intensity", fm_intensity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/intensity", fm_intensity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/intensity", fm_intensity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/intensity", fm_intensity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/intensity", fm_intensity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/intensity", fm_intensity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/intensity", fm_intensity, 7),
    OSC_PSCALAR("/hds/vco1/vol", vco1_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/vol", vco1_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/vol", vco1_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/vol", vco1_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/vol", vco1_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/vol", vco1_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/vol", vco1_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/vol", vco1_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/vol", vco1_vol, 7),
    OSC_PSCALAR("/hds/vco2/vol", vco2_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/vol", vco2_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/vol", vco2_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/vol", vco2_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/vol", vco2_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/vol", vco2_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/vol", vco2_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/vol", vco2_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/vol", vco2_vol, 7),
    OSC_PSCALAR("/hds/portamento_time_ms", portamento_time_ms),
    OSC_PSCALAR_AT("/hds/combi/0/portamento_time_ms", portamento_time_ms, 0),
    OSC_PSCALAR_AT("/hds/combi/1/portamento_time_ms", portamento_time_ms, 1),
    OSC_PSCALAR_AT("/hds/combi/2/portamento_time_ms", portamento_time_ms, 2),
    OSC_PSCALAR_AT("/hds/combi/3/portamento_time_ms", portamento_time_ms, 3),
    OSC_PSCALAR_AT("/hds/combi/4/portamento_time_ms", portamento_time_ms, 4),
    OSC_PSCALAR_AT("/hds/combi/5/portamento_time_ms", portamento_time_ms, 5),
    OSC_PSCALAR_AT("/hds/combi/6/portamento_time_ms", portamento_time_ms, 6),
    OSC_PSCALAR_AT("/hds/combi/7/portamento_time_ms", portamento_time_ms, 7),
    OSC_SCALAR("/hds/master_volume", master_volume),
    OSC_SCALAR("/hds/transpose_semitones", transpose_semitones),
    OSC_SCALAR("/hds/tuning/enable", tuning_enable),
    OSC_SCALAR("/hds/tuning/reference_note", tuning_reference_note),
    OSC_SCALAR("/hds/tuning/reference_freq", tuning_reference_freq),
    OSC_PSCALAR("/hds/vco1/pan", vco1_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pan", vco1_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pan", vco1_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pan", vco1_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pan", vco1_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pan", vco1_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pan", vco1_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pan", vco1_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pan", vco1_pan, 7),
    OSC_PSCALAR("/hds/vco2/pan", vco2_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pan", vco2_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pan", vco2_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pan", vco2_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pan", vco2_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pan", vco2_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pan", vco2_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pan", vco2_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pan", vco2_pan, 7),
    OSC_PSCALAR("/hds/vco1/fine_tune", vco1_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/fine_tune", vco1_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/fine_tune", vco1_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/fine_tune", vco1_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/fine_tune", vco1_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/fine_tune", vco1_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/fine_tune", vco1_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/fine_tune", vco1_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/fine_tune", vco1_fine_tune, 7),
    OSC_PSCALAR("/hds/vco2/pitch_offset", vco2_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pitch_offset", vco2_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pitch_offset", vco2_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pitch_offset", vco2_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pitch_offset", vco2_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pitch_offset", vco2_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pitch_offset", vco2_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pitch_offset", vco2_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pitch_offset", vco2_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco2/fine_tune", vco2_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/fine_tune", vco2_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/fine_tune", vco2_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/fine_tune", vco2_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/fine_tune", vco2_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/fine_tune", vco2_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/fine_tune", vco2_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/fine_tune", vco2_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/fine_tune", vco2_fine_tune, 7),

    // --- Envelopes (eg[ENV_COUNT], PackEGParams: d=delay h=hold a=attack dec=decay s=sustain
    // r=release al=attack_start_pct pk=attack_peak_pct -- see tick_adsr()) ---
    OSC_PSUBARRAY("/hds/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/release", eg, PackEGParams, r, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 7),
    OSC_PARRAY("/hds/eg/%d/topology_mode", eg_mode, ENV_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg/%d/topology_mode", eg_mode, ENV_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg/%d/topology_mode", eg_mode, ENV_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg/%d/topology_mode", eg_mode, ENV_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg/%d/topology_mode", eg_mode, ENV_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg/%d/topology_mode", eg_mode, ENV_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg/%d/topology_mode", eg_mode, ENV_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg/%d/topology_mode", eg_mode, ENV_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg/%d/topology_mode", eg_mode, ENV_COUNT, 7),

    // --- Filters: 5 flat, interchangeable slots (filters[5]), chained in Series or
    // summed in Parallel (filter_chain_mode) -- see the filter section simplification's
    // own comments on ProgramState.filters/FilterParams. ---
    OSC_PSCALAR("/hds/filter/chain_mode", filter_chain_mode),
    OSC_PSCALAR_AT("/hds/combi/0/filter/chain_mode", filter_chain_mode, 0),
    OSC_PSCALAR_AT("/hds/combi/1/filter/chain_mode", filter_chain_mode, 1),
    OSC_PSCALAR_AT("/hds/combi/2/filter/chain_mode", filter_chain_mode, 2),
    OSC_PSCALAR_AT("/hds/combi/3/filter/chain_mode", filter_chain_mode, 3),
    OSC_PSCALAR_AT("/hds/combi/4/filter/chain_mode", filter_chain_mode, 4),
    OSC_PSCALAR_AT("/hds/combi/5/filter/chain_mode", filter_chain_mode, 5),
    OSC_PSCALAR_AT("/hds/combi/6/filter/chain_mode", filter_chain_mode, 6),
    OSC_PSCALAR_AT("/hds/combi/7/filter/chain_mode", filter_chain_mode, 7),
    OSC_PSUBARRAY("/hds/filter/%d/cutoff", filters, FilterParams, cutoff, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/res_q", filters, FilterParams, res_q, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/res_q", filters, FilterParams, res_q, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/res_q", filters, FilterParams, res_q, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/res_q", filters, FilterParams, res_q, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/res_q", filters, FilterParams, res_q, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/res_q", filters, FilterParams, res_q, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/res_q", filters, FilterParams, res_q, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/res_q", filters, FilterParams, res_q, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/res_q", filters, FilterParams, res_q, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/slope", filters, FilterParams, slope_poles, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/slope", filters, FilterParams, slope_poles, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/slope", filters, FilterParams, slope_poles, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/slope", filters, FilterParams, slope_poles, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/slope", filters, FilterParams, slope_poles, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/slope", filters, FilterParams, slope_poles, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/slope", filters, FilterParams, slope_poles, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/slope", filters, FilterParams, slope_poles, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/slope", filters, FilterParams, slope_poles, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/type", filters, FilterParams, filter_type, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/type", filters, FilterParams, filter_type, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/type", filters, FilterParams, filter_type, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/type", filters, FilterParams, filter_type, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/type", filters, FilterParams, filter_type, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/type", filters, FilterParams, filter_type, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/type", filters, FilterParams, filter_type, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/type", filters, FilterParams, filter_type, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/type", filters, FilterParams, filter_type, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/key_track", filters, FilterParams, key_track, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/key_track", filters, FilterParams, key_track, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/key_track", filters, FilterParams, key_track, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/key_track", filters, FilterParams, key_track, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/key_track", filters, FilterParams, key_track, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/key_track", filters, FilterParams, key_track, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/key_track", filters, FilterParams, key_track, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/key_track", filters, FilterParams, key_track, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/key_track", filters, FilterParams, key_track, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 7),

    // --- Post-filter 3-band EQ (eq_bands[3], EqBandParams -- 0=Low Shelf, 1=Mid Peak, 2=High
    // Shelf, see EqBandParams' own comment) ---
    OSC_PSUBARRAY("/hds/eq/%d/freq", eq_bands, EqBandParams, freq, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 7),
    OSC_PSUBARRAY("/hds/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 7),
    OSC_PSUBARRAY("/hds/eq/%d/q", eq_bands, EqBandParams, q, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/q", eq_bands, EqBandParams, q, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/q", eq_bands, EqBandParams, q, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/q", eq_bands, EqBandParams, q, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/q", eq_bands, EqBandParams, q, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/q", eq_bands, EqBandParams, q, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/q", eq_bands, EqBandParams, q, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/q", eq_bands, EqBandParams, q, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/q", eq_bands, EqBandParams, q, 3, 7),

    // --- LFOs (lfo[LFO_COUNT], PackLfoParams) ---
    OSC_PSUBARRAY("/hds/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 7),

    // --- Assignable EG modulators ---
    OSC_PARRAY("/hds/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 7),
    OSC_PARRAY("/hds/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 7),

    // --- Unison Saw ---
    OSC_PSCALAR("/hds/vco1/unison/detune", vco1_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/unison/detune", vco1_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/unison/detune", vco1_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/unison/detune", vco1_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/unison/detune", vco1_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/unison/detune", vco1_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/unison/detune", vco1_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/unison/detune", vco1_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/unison/detune", vco1_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco1/unison/width", vco1_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/unison/width", vco1_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/unison/width", vco1_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/unison/width", vco1_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/unison/width", vco1_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/unison/width", vco1_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/unison/width", vco1_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/unison/width", vco1_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/unison/width", vco1_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco2/unison/detune", vco2_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/unison/detune", vco2_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/unison/detune", vco2_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/unison/detune", vco2_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/unison/detune", vco2_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/unison/detune", vco2_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/unison/detune", vco2_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/unison/detune", vco2_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/unison/detune", vco2_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco2/unison/width", vco2_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/unison/width", vco2_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/unison/width", vco2_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/unison/width", vco2_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/unison/width", vco2_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/unison/width", vco2_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/unison/width", vco2_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/unison/width", vco2_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/unison/width", vco2_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco3/unison/detune", vco3_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/unison/detune", vco3_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/unison/detune", vco3_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/unison/detune", vco3_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/unison/detune", vco3_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/unison/detune", vco3_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/unison/detune", vco3_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/unison/detune", vco3_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/unison/detune", vco3_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco3/unison/width", vco3_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/unison/width", vco3_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/unison/width", vco3_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/unison/width", vco3_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/unison/width", vco3_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/unison/width", vco3_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/unison/width", vco3_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/unison/width", vco3_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/unison/width", vco3_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco4/unison/detune", vco4_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/unison/detune", vco4_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/unison/detune", vco4_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/unison/detune", vco4_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/unison/detune", vco4_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/unison/detune", vco4_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/unison/detune", vco4_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/unison/detune", vco4_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/unison/detune", vco4_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco4/unison/width", vco4_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/unison/width", vco4_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/unison/width", vco4_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/unison/width", vco4_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/unison/width", vco4_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/unison/width", vco4_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/unison/width", vco4_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/unison/width", vco4_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/unison/width", vco4_unison_saw_width, 7),

    OSC_PSCALAR("/hds/mono_glide_random", mono_glide_random),
    OSC_PSCALAR_AT("/hds/combi/0/mono_glide_random", mono_glide_random, 0),
    OSC_PSCALAR_AT("/hds/combi/1/mono_glide_random", mono_glide_random, 1),
    OSC_PSCALAR_AT("/hds/combi/2/mono_glide_random", mono_glide_random, 2),
    OSC_PSCALAR_AT("/hds/combi/3/mono_glide_random", mono_glide_random, 3),
    OSC_PSCALAR_AT("/hds/combi/4/mono_glide_random", mono_glide_random, 4),
    OSC_PSCALAR_AT("/hds/combi/5/mono_glide_random", mono_glide_random, 5),
    OSC_PSCALAR_AT("/hds/combi/6/mono_glide_random", mono_glide_random, 6),
    OSC_PSCALAR_AT("/hds/combi/7/mono_glide_random", mono_glide_random, 7),
    OSC_PSCALAR("/hds/vco1/mute", vco1_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/mute", vco1_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/mute", vco1_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/mute", vco1_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/mute", vco1_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/mute", vco1_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/mute", vco1_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/mute", vco1_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/mute", vco1_mute, 7),
    OSC_PSCALAR("/hds/vco2/mute", vco2_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/mute", vco2_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/mute", vco2_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/mute", vco2_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/mute", vco2_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/mute", vco2_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/mute", vco2_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/mute", vco2_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/mute", vco2_mute, 7),
    OSC_PSCALAR("/hds/hard_sync_mode", hard_sync_mode),
    OSC_PSCALAR_AT("/hds/combi/0/hard_sync_mode", hard_sync_mode, 0),
    OSC_PSCALAR_AT("/hds/combi/1/hard_sync_mode", hard_sync_mode, 1),
    OSC_PSCALAR_AT("/hds/combi/2/hard_sync_mode", hard_sync_mode, 2),
    OSC_PSCALAR_AT("/hds/combi/3/hard_sync_mode", hard_sync_mode, 3),
    OSC_PSCALAR_AT("/hds/combi/4/hard_sync_mode", hard_sync_mode, 4),
    OSC_PSCALAR_AT("/hds/combi/5/hard_sync_mode", hard_sync_mode, 5),
    OSC_PSCALAR_AT("/hds/combi/6/hard_sync_mode", hard_sync_mode, 6),
    OSC_PSCALAR_AT("/hds/combi/7/hard_sync_mode", hard_sync_mode, 7),

    // --- Arpeggiator (see the Arpeggiator section on ProgramState + poll_arpeggiator() for the
    // full design) ---
    OSC_PSCALAR("/hds/arp/enabled", arp_enabled),
    OSC_PSCALAR_AT("/hds/combi/0/arp/enabled", arp_enabled, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/enabled", arp_enabled, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/enabled", arp_enabled, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/enabled", arp_enabled, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/enabled", arp_enabled, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/enabled", arp_enabled, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/enabled", arp_enabled, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/enabled", arp_enabled, 7),
    OSC_PSCALAR("/hds/arp/style", arp_style),
    OSC_PSCALAR_AT("/hds/combi/0/arp/style", arp_style, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/style", arp_style, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/style", arp_style, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/style", arp_style, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/style", arp_style, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/style", arp_style, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/style", arp_style, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/style", arp_style, 7),
    OSC_PSCALAR("/hds/arp/octave_range", arp_octave_range),
    OSC_PSCALAR_AT("/hds/combi/0/arp/octave_range", arp_octave_range, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/octave_range", arp_octave_range, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/octave_range", arp_octave_range, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/octave_range", arp_octave_range, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/octave_range", arp_octave_range, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/octave_range", arp_octave_range, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/octave_range", arp_octave_range, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/octave_range", arp_octave_range, 7),
    OSC_PSCALAR("/hds/arp/rate", arp_rate),
    OSC_PSCALAR_AT("/hds/combi/0/arp/rate", arp_rate, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/rate", arp_rate, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/rate", arp_rate, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/rate", arp_rate, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/rate", arp_rate, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/rate", arp_rate, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/rate", arp_rate, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/rate", arp_rate, 7),
    OSC_PSCALAR("/hds/arp/gate_pct", arp_gate_pct),
    OSC_PSCALAR_AT("/hds/combi/0/arp/gate_pct", arp_gate_pct, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/gate_pct", arp_gate_pct, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/gate_pct", arp_gate_pct, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/gate_pct", arp_gate_pct, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/gate_pct", arp_gate_pct, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/gate_pct", arp_gate_pct, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/gate_pct", arp_gate_pct, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/gate_pct", arp_gate_pct, 7),
    OSC_PSCALAR("/hds/arp/hold", arp_hold),
    OSC_PSCALAR_AT("/hds/combi/0/arp/hold", arp_hold, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/hold", arp_hold, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/hold", arp_hold, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/hold", arp_hold, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/hold", arp_hold, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/hold", arp_hold, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/hold", arp_hold, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/hold", arp_hold, 7),
    OSC_PSCALAR("/hds/arp/velocity", arp_velocity),
    OSC_PSCALAR_AT("/hds/combi/0/arp/velocity", arp_velocity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/velocity", arp_velocity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/velocity", arp_velocity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/velocity", arp_velocity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/velocity", arp_velocity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/velocity", arp_velocity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/velocity", arp_velocity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/velocity", arp_velocity, 7),

    OSC_PSCALAR("/hds/vco1/phase_offset", vco1_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/phase_offset", vco1_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/phase_offset", vco1_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/phase_offset", vco1_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/phase_offset", vco1_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/phase_offset", vco1_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/phase_offset", vco1_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/phase_offset", vco1_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/phase_offset", vco1_phase_offset, 7),
    OSC_PSCALAR("/hds/vco2/phase_offset", vco2_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/phase_offset", vco2_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/phase_offset", vco2_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/phase_offset", vco2_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/phase_offset", vco2_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/phase_offset", vco2_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/phase_offset", vco2_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/phase_offset", vco2_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/phase_offset", vco2_phase_offset, 7),
    OSC_PSCALAR("/hds/vco1/pulse_width", vco1_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pulse_width", vco1_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pulse_width", vco1_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pulse_width", vco1_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pulse_width", vco1_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pulse_width", vco1_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pulse_width", vco1_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pulse_width", vco1_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pulse_width", vco1_pulse_width, 7),
    OSC_PSCALAR("/hds/vco2/pulse_width", vco2_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pulse_width", vco2_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pulse_width", vco2_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pulse_width", vco2_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pulse_width", vco2_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pulse_width", vco2_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pulse_width", vco2_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pulse_width", vco2_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pulse_width", vco2_pulse_width, 7),

    OSC_PSCALAR("/hds/vco3/vol", vco3_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/vol", vco3_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/vol", vco3_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/vol", vco3_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/vol", vco3_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/vol", vco3_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/vol", vco3_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/vol", vco3_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/vol", vco3_vol, 7),
    OSC_PSCALAR("/hds/vco3/pan", vco3_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pan", vco3_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pan", vco3_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pan", vco3_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pan", vco3_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pan", vco3_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pan", vco3_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pan", vco3_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pan", vco3_pan, 7),
    OSC_PSCALAR("/hds/vco3/mute", vco3_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/mute", vco3_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/mute", vco3_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/mute", vco3_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/mute", vco3_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/mute", vco3_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/mute", vco3_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/mute", vco3_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/mute", vco3_mute, 7),
    OSC_PSCALAR("/hds/vco3/pulse_width", vco3_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pulse_width", vco3_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pulse_width", vco3_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pulse_width", vco3_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pulse_width", vco3_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pulse_width", vco3_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pulse_width", vco3_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pulse_width", vco3_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pulse_width", vco3_pulse_width, 7),
    OSC_PSCALAR("/hds/vco4/vol", vco4_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/vol", vco4_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/vol", vco4_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/vol", vco4_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/vol", vco4_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/vol", vco4_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/vol", vco4_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/vol", vco4_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/vol", vco4_vol, 7),
    OSC_PSCALAR("/hds/vco4/pan", vco4_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pan", vco4_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pan", vco4_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pan", vco4_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pan", vco4_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pan", vco4_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pan", vco4_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pan", vco4_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pan", vco4_pan, 7),
    OSC_PSCALAR("/hds/vco4/mute", vco4_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/mute", vco4_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/mute", vco4_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/mute", vco4_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/mute", vco4_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/mute", vco4_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/mute", vco4_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/mute", vco4_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/mute", vco4_mute, 7),
    OSC_PSCALAR("/hds/vco4/pulse_width", vco4_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pulse_width", vco4_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pulse_width", vco4_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pulse_width", vco4_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pulse_width", vco4_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pulse_width", vco4_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pulse_width", vco4_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pulse_width", vco4_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pulse_width", vco4_pulse_width, 7),

    OSC_ARRAY("/hds/ext_in/%d/enable", ext_in_enable, EXT_IN_PAIR_COUNT),
    OSC_ARRAY("/hds/ext_in/%d/gain", ext_in_gain, EXT_IN_PAIR_COUNT),
    OSC_SCALAR("/hds/gui/routing_tab_visible", gui_routing_tab_visible),

    // --- FX Bus (fx[FX_SLOT_COUNT], FXSlotParams) ---
    OSC_SUBARRAY("/hds/fx/%d/type", fx, FXSlotParams, fx_type, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/mix", fx, FXSlotParams, mix, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/bypass", fx, FXSlotParams, bypass, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/source", fx, FXSlotParams, fx_source, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/chain_to_next", fx, FXSlotParams, chain_to_next, FX_SLOT_COUNT),
    OSC_NESTED("/hds/fx/%d/param/%d", fx, FXSlotParams, param, FX_SLOT_COUNT, FX_SLOT_PARAM_COUNT),
    // FX_TYPE_NAM's own loaded-file paths -- plain string fields, generically saved/restored by
    // CLAP state (see clap_state_save/load's own K/T-line walk over this whole table) with no
    // special-casing needed there at all. Setting one of these live has no side effect by itself
    // (same as every other OSC_STRING_SUBARRAY field, e.g. Sequencer's own audio_path) -- the
    // actual model/IR (re)load happens once per block in poll_fx_nam_requests(), which notices the
    // path changed and does the real work, whether it changed via a live OSC message or a CLAP
    // state restore replaying this same field. See that function's own comment.
    OSC_STRING_SUBARRAY("/hds/fx/%d/nam_model_path", fx, FXSlotParams, nam_model_path, FX_SLOT_COUNT, SAMPLE_PATH_LEN),
    OSC_STRING_SUBARRAY("/hds/fx/%d/nam_ir_path", fx, FXSlotParams, nam_ir_path, FX_SLOT_COUNT, SAMPLE_PATH_LEN),
    OSC_ARRAY("/hds/fx_routing/%d/mode", fx_source_routing_mode, SOURCE_COUNT),

    // --- Multi-output hardware routing: which output pair(s) each source lands on ---
    OSC_NESTED("/hds/routing/%d/out/%d", source_routing, SourceRouting, enable, SOURCE_COUNT, OUTPUT_PAIR_MAX),

    OSC_PSCALAR("/hds/pb/range_up", pb_range_up),
    OSC_PSCALAR_AT("/hds/combi/0/pb/range_up", pb_range_up, 0),
    OSC_PSCALAR_AT("/hds/combi/1/pb/range_up", pb_range_up, 1),
    OSC_PSCALAR_AT("/hds/combi/2/pb/range_up", pb_range_up, 2),
    OSC_PSCALAR_AT("/hds/combi/3/pb/range_up", pb_range_up, 3),
    OSC_PSCALAR_AT("/hds/combi/4/pb/range_up", pb_range_up, 4),
    OSC_PSCALAR_AT("/hds/combi/5/pb/range_up", pb_range_up, 5),
    OSC_PSCALAR_AT("/hds/combi/6/pb/range_up", pb_range_up, 6),
    OSC_PSCALAR_AT("/hds/combi/7/pb/range_up", pb_range_up, 7),
    OSC_PSCALAR("/hds/pb/range_down", pb_range_down),
    OSC_PSCALAR_AT("/hds/combi/0/pb/range_down", pb_range_down, 0),
    OSC_PSCALAR_AT("/hds/combi/1/pb/range_down", pb_range_down, 1),
    OSC_PSCALAR_AT("/hds/combi/2/pb/range_down", pb_range_down, 2),
    OSC_PSCALAR_AT("/hds/combi/3/pb/range_down", pb_range_down, 3),
    OSC_PSCALAR_AT("/hds/combi/4/pb/range_down", pb_range_down, 4),
    OSC_PSCALAR_AT("/hds/combi/5/pb/range_down", pb_range_down, 5),
    OSC_PSCALAR_AT("/hds/combi/6/pb/range_down", pb_range_down, 6),
    OSC_PSCALAR_AT("/hds/combi/7/pb/range_down", pb_range_down, 7),
    OSC_PSCALAR("/hds/sustain/polarity", sustain_pedal_polarity),
    OSC_PSCALAR_AT("/hds/combi/0/sustain/polarity", sustain_pedal_polarity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/sustain/polarity", sustain_pedal_polarity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/sustain/polarity", sustain_pedal_polarity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/sustain/polarity", sustain_pedal_polarity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/sustain/polarity", sustain_pedal_polarity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/sustain/polarity", sustain_pedal_polarity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/sustain/polarity", sustain_pedal_polarity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/sustain/polarity", sustain_pedal_polarity, 7),
    OSC_SCALAR("/hds/audio/latency_ms", audio_latency_ms),
    OSC_SCALAR("/hds/audio/polyphony", active_voice_limit),
    OSC_SCALAR("/hds/audio/block_size", block_size_frames),

    OSC_SCALAR("/hds/record/enable", record_enable),
    OSC_SCALAR("/hds/record/include_synth_output", record_include_synth_output),
    OSC_SCALAR("/hds/record/downmix_stereo", record_downmix_stereo),
    OSC_ARRAY("/hds/record/channel/%d/enable", record_input_channel_enable, RECORD_MAX_CHANNELS),
    OSC_SCALAR("/hds/record/format", record_format),

    OSC_PSCALAR("/hds/tempo/bpm", tempo_bpm),
    OSC_PSCALAR_AT("/hds/combi/0/tempo/bpm", tempo_bpm, 0),
    OSC_PSCALAR_AT("/hds/combi/1/tempo/bpm", tempo_bpm, 1),
    OSC_PSCALAR_AT("/hds/combi/2/tempo/bpm", tempo_bpm, 2),
    OSC_PSCALAR_AT("/hds/combi/3/tempo/bpm", tempo_bpm, 3),
    OSC_PSCALAR_AT("/hds/combi/4/tempo/bpm", tempo_bpm, 4),
    OSC_PSCALAR_AT("/hds/combi/5/tempo/bpm", tempo_bpm, 5),
    OSC_PSCALAR_AT("/hds/combi/6/tempo/bpm", tempo_bpm, 6),
    OSC_PSCALAR_AT("/hds/combi/7/tempo/bpm", tempo_bpm, 7),
    OSC_PSCALAR("/hds/tempo/marker_count", tempo_marker_count),
    OSC_PSCALAR_AT("/hds/combi/0/tempo/marker_count", tempo_marker_count, 0),
    OSC_PSCALAR_AT("/hds/combi/1/tempo/marker_count", tempo_marker_count, 1),
    OSC_PSCALAR_AT("/hds/combi/2/tempo/marker_count", tempo_marker_count, 2),
    OSC_PSCALAR_AT("/hds/combi/3/tempo/marker_count", tempo_marker_count, 3),
    OSC_PSCALAR_AT("/hds/combi/4/tempo/marker_count", tempo_marker_count, 4),
    OSC_PSCALAR_AT("/hds/combi/5/tempo/marker_count", tempo_marker_count, 5),
    OSC_PSCALAR_AT("/hds/combi/6/tempo/marker_count", tempo_marker_count, 6),
    OSC_PSCALAR_AT("/hds/combi/7/tempo/marker_count", tempo_marker_count, 7),
    OSC_PARRAY("/hds/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX),
    OSC_PARRAY_AT("/hds/combi/0/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 0),
    OSC_PARRAY_AT("/hds/combi/1/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 1),
    OSC_PARRAY_AT("/hds/combi/2/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 2),
    OSC_PARRAY_AT("/hds/combi/3/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 3),
    OSC_PARRAY_AT("/hds/combi/4/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 4),
    OSC_PARRAY_AT("/hds/combi/5/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 5),
    OSC_PARRAY_AT("/hds/combi/6/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 6),
    OSC_PARRAY_AT("/hds/combi/7/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 7),
    OSC_PARRAY("/hds/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX),
    OSC_PARRAY_AT("/hds/combi/0/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 0),
    OSC_PARRAY_AT("/hds/combi/1/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 1),
    OSC_PARRAY_AT("/hds/combi/2/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 2),
    OSC_PARRAY_AT("/hds/combi/3/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 3),
    OSC_PARRAY_AT("/hds/combi/4/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 4),
    OSC_PARRAY_AT("/hds/combi/5/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 5),
    OSC_PARRAY_AT("/hds/combi/6/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 6),
    OSC_PARRAY_AT("/hds/combi/7/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 7),

    OSC_PSCALAR("/hds/vco3/shape", vco3_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/shape", vco3_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/shape", vco3_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/shape", vco3_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/shape", vco3_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/shape", vco3_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/shape", vco3_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/shape", vco3_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/shape", vco3_shape, 7),
    OSC_PSCALAR("/hds/vco3/fine_tune", vco3_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/fine_tune", vco3_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/fine_tune", vco3_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/fine_tune", vco3_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/fine_tune", vco3_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/fine_tune", vco3_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/fine_tune", vco3_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/fine_tune", vco3_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/fine_tune", vco3_fine_tune, 7),
    OSC_PSCALAR("/hds/vco3/pitch_offset", vco3_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pitch_offset", vco3_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pitch_offset", vco3_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pitch_offset", vco3_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pitch_offset", vco3_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pitch_offset", vco3_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pitch_offset", vco3_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pitch_offset", vco3_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pitch_offset", vco3_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco3/phase_offset", vco3_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/phase_offset", vco3_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/phase_offset", vco3_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/phase_offset", vco3_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/phase_offset", vco3_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/phase_offset", vco3_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/phase_offset", vco3_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/phase_offset", vco3_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/phase_offset", vco3_phase_offset, 7),
    OSC_PSCALAR("/hds/vco4/shape", vco4_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/shape", vco4_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/shape", vco4_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/shape", vco4_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/shape", vco4_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/shape", vco4_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/shape", vco4_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/shape", vco4_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/shape", vco4_shape, 7),
    OSC_PSCALAR("/hds/vco4/fine_tune", vco4_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/fine_tune", vco4_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/fine_tune", vco4_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/fine_tune", vco4_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/fine_tune", vco4_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/fine_tune", vco4_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/fine_tune", vco4_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/fine_tune", vco4_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/fine_tune", vco4_fine_tune, 7),
    OSC_PSCALAR("/hds/vco4/pitch_offset", vco4_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pitch_offset", vco4_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pitch_offset", vco4_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pitch_offset", vco4_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pitch_offset", vco4_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pitch_offset", vco4_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pitch_offset", vco4_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pitch_offset", vco4_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pitch_offset", vco4_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco4/phase_offset", vco4_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/phase_offset", vco4_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/phase_offset", vco4_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/phase_offset", vco4_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/phase_offset", vco4_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/phase_offset", vco4_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/phase_offset", vco4_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/phase_offset", vco4_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/phase_offset", vco4_phase_offset, 7),

    OSC_PSCALAR("/hds/fm/algorithm", fm_algorithm),
    OSC_PSCALAR_AT("/hds/combi/0/fm/algorithm", fm_algorithm, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/algorithm", fm_algorithm, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/algorithm", fm_algorithm, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/algorithm", fm_algorithm, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/algorithm", fm_algorithm, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/algorithm", fm_algorithm, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/algorithm", fm_algorithm, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/algorithm", fm_algorithm, 7),
    OSC_PSCALAR("/hds/fm/depth/3_1", fm_depth_3_1),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_1", fm_depth_3_1, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_1", fm_depth_3_1, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_1", fm_depth_3_1, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_1", fm_depth_3_1, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_1", fm_depth_3_1, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_1", fm_depth_3_1, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_1", fm_depth_3_1, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_1", fm_depth_3_1, 7),
    OSC_PSCALAR("/hds/fm/depth/3_2", fm_depth_3_2),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_2", fm_depth_3_2, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_2", fm_depth_3_2, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_2", fm_depth_3_2, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_2", fm_depth_3_2, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_2", fm_depth_3_2, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_2", fm_depth_3_2, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_2", fm_depth_3_2, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_2", fm_depth_3_2, 7),
    OSC_PSCALAR("/hds/fm/depth/4_1", fm_depth_4_1),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/4_1", fm_depth_4_1, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/4_1", fm_depth_4_1, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/4_1", fm_depth_4_1, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/4_1", fm_depth_4_1, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/4_1", fm_depth_4_1, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/4_1", fm_depth_4_1, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/4_1", fm_depth_4_1, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/4_1", fm_depth_4_1, 7),
    OSC_PSCALAR("/hds/fm/depth/4_3", fm_depth_4_3),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/4_3", fm_depth_4_3, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/4_3", fm_depth_4_3, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/4_3", fm_depth_4_3, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/4_3", fm_depth_4_3, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/4_3", fm_depth_4_3, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/4_3", fm_depth_4_3, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/4_3", fm_depth_4_3, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/4_3", fm_depth_4_3, 7),
    OSC_PSCALAR("/hds/fm/depth/1_2", fm_depth_1_2),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/1_2", fm_depth_1_2, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/1_2", fm_depth_1_2, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/1_2", fm_depth_1_2, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/1_2", fm_depth_1_2, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/1_2", fm_depth_1_2, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/1_2", fm_depth_1_2, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/1_2", fm_depth_1_2, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/1_2", fm_depth_1_2, 7),
    OSC_PSCALAR("/hds/fm/depth/3_4", fm_depth_3_4),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_4", fm_depth_3_4, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_4", fm_depth_3_4, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_4", fm_depth_3_4, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_4", fm_depth_3_4, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_4", fm_depth_3_4, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_4", fm_depth_3_4, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_4", fm_depth_3_4, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_4", fm_depth_3_4, 7),
    OSC_PSCALAR("/hds/fm/depth/1_3", fm_depth_1_3),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/1_3", fm_depth_1_3, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/1_3", fm_depth_1_3, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/1_3", fm_depth_1_3, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/1_3", fm_depth_1_3, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/1_3", fm_depth_1_3, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/1_3", fm_depth_1_3, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/1_3", fm_depth_1_3, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/1_3", fm_depth_1_3, 7),

    OSC_PSCALAR("/hds/vco1/pitch", vco1_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pitch", vco1_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pitch", vco1_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pitch", vco1_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pitch", vco1_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pitch", vco1_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pitch", vco1_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pitch", vco1_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pitch", vco1_pitch, 7),
    OSC_PSCALAR("/hds/vco2/pitch", vco2_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pitch", vco2_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pitch", vco2_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pitch", vco2_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pitch", vco2_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pitch", vco2_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pitch", vco2_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pitch", vco2_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pitch", vco2_pitch, 7),
    OSC_PSCALAR("/hds/vco3/pitch", vco3_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pitch", vco3_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pitch", vco3_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pitch", vco3_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pitch", vco3_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pitch", vco3_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pitch", vco3_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pitch", vco3_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pitch", vco3_pitch, 7),
    OSC_PSCALAR("/hds/vco4/pitch", vco4_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pitch", vco4_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pitch", vco4_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pitch", vco4_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pitch", vco4_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pitch", vco4_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pitch", vco4_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pitch", vco4_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pitch", vco4_pitch, 7),

    // Monophonic Noise (Noise shape only -- see ProgramState.vco1_mono_noise's own comment).
    OSC_PSCALAR("/hds/vco1/mono_noise", vco1_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/mono_noise", vco1_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/mono_noise", vco1_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/mono_noise", vco1_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/mono_noise", vco1_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/mono_noise", vco1_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/mono_noise", vco1_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/mono_noise", vco1_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/mono_noise", vco1_mono_noise, 7),
    OSC_PSCALAR("/hds/vco2/mono_noise", vco2_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/mono_noise", vco2_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/mono_noise", vco2_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/mono_noise", vco2_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/mono_noise", vco2_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/mono_noise", vco2_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/mono_noise", vco2_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/mono_noise", vco2_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/mono_noise", vco2_mono_noise, 7),
    OSC_PSCALAR("/hds/vco3/mono_noise", vco3_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/mono_noise", vco3_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/mono_noise", vco3_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/mono_noise", vco3_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/mono_noise", vco3_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/mono_noise", vco3_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/mono_noise", vco3_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/mono_noise", vco3_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/mono_noise", vco3_mono_noise, 7),
    OSC_PSCALAR("/hds/vco4/mono_noise", vco4_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/mono_noise", vco4_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/mono_noise", vco4_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/mono_noise", vco4_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/mono_noise", vco4_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/mono_noise", vco4_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/mono_noise", vco4_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/mono_noise", vco4_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/mono_noise", vco4_mono_noise, 7),

    // --- Velocity -> Amp, per oscillator (see ProgramState.vco{1-4}_vel_to_amp's own comment) ---
    OSC_PSCALAR("/hds/vco1/vel_to_amp", vco1_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/vel_to_amp", vco1_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/vel_to_amp", vco1_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/vel_to_amp", vco1_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/vel_to_amp", vco1_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/vel_to_amp", vco1_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/vel_to_amp", vco1_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/vel_to_amp", vco1_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/vel_to_amp", vco1_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco2/vel_to_amp", vco2_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/vel_to_amp", vco2_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/vel_to_amp", vco2_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/vel_to_amp", vco2_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/vel_to_amp", vco2_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/vel_to_amp", vco2_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/vel_to_amp", vco2_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/vel_to_amp", vco2_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/vel_to_amp", vco2_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco3/vel_to_amp", vco3_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/vel_to_amp", vco3_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/vel_to_amp", vco3_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/vel_to_amp", vco3_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/vel_to_amp", vco3_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/vel_to_amp", vco3_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/vel_to_amp", vco3_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/vel_to_amp", vco3_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/vel_to_amp", vco3_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco4/vel_to_amp", vco4_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/vel_to_amp", vco4_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/vel_to_amp", vco4_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/vel_to_amp", vco4_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/vel_to_amp", vco4_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/vel_to_amp", vco4_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/vel_to_amp", vco4_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/vel_to_amp", vco4_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/vel_to_amp", vco4_vel_to_amp, 7),

    // --- Curve/Drive, per oscillator (see ProgramState.vco{1-4}_curve_drive's own comment) ---
    OSC_PSCALAR("/hds/vco1/curve_drive", vco1_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/curve_drive", vco1_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/curve_drive", vco1_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/curve_drive", vco1_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/curve_drive", vco1_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/curve_drive", vco1_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/curve_drive", vco1_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/curve_drive", vco1_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/curve_drive", vco1_curve_drive, 7),
    OSC_PSCALAR("/hds/vco2/curve_drive", vco2_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/curve_drive", vco2_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/curve_drive", vco2_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/curve_drive", vco2_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/curve_drive", vco2_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/curve_drive", vco2_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/curve_drive", vco2_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/curve_drive", vco2_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/curve_drive", vco2_curve_drive, 7),
    OSC_PSCALAR("/hds/vco3/curve_drive", vco3_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/curve_drive", vco3_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/curve_drive", vco3_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/curve_drive", vco3_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/curve_drive", vco3_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/curve_drive", vco3_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/curve_drive", vco3_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/curve_drive", vco3_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/curve_drive", vco3_curve_drive, 7),
    OSC_PSCALAR("/hds/vco4/curve_drive", vco4_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/curve_drive", vco4_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/curve_drive", vco4_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/curve_drive", vco4_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/curve_drive", vco4_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/curve_drive", vco4_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/curve_drive", vco4_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/curve_drive", vco4_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/curve_drive", vco4_curve_drive, 7),

    // --- Curve/Drive, per LFO (see PackLfoParams.curve_drive's own comment) ---
    OSC_PSUBARRAY("/hds/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 7),

    // --- Combi/Sequencer scaffolding (see SEQ_LANE_COUNT's own comment) ---
    OSC_SCALAR("/hds/performance_mode", performance_mode),
    OSC_SUBARRAY("/hds/seq/%d/lane_type", seq_lanes, SeqLaneState, lane_type, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/volume", seq_lanes, SeqLaneState, volume, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/pan", seq_lanes, SeqLaneState, pan, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/midi_channel", seq_lanes, SeqLaneState, midi_channel, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/mode", seq_lanes, SeqLaneState, seq_mode, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/mute", seq_lanes, SeqLaneState, mute, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/rec_armed", seq_lanes, SeqLaneState, rec_armed, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/rec_overdub", seq_lanes, SeqLaneState, rec_overdub, SEQ_LANE_COUNT),
    OSC_STRING_SUBARRAY("/hds/seq/%d/audio_path", seq_lanes, SeqLaneState, audio_path, SEQ_LANE_COUNT, SEQ_AUDIO_PATH_LEN),
    OSC_SUBARRAY("/hds/seq/%d/audio_offset_sec", seq_lanes, SeqLaneState, audio_offset_sec, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/audio_preview_trigger", seq_lanes, SeqLaneState, audio_preview_trigger, SEQ_LANE_COUNT),
    OSC_SCALAR("/hds/seq/transport/play", seq_transport_play_trigger),
    OSC_SCALAR("/hds/seq/transport/pause", seq_transport_pause_trigger),
    OSC_SCALAR("/hds/seq/transport/stop", seq_transport_stop_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_start", seq_transport_seek_start_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_back", seq_transport_seek_back_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_fwd", seq_transport_seek_fwd_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_to_target_sec", seq_transport_seek_to_target_sec),
    OSC_SCALAR("/hds/seq/transport/seek_to", seq_transport_seek_to_trigger),
    OSC_SCALAR("/hds/seq/transport/metronome", metronome_enable),
    OSC_SCALAR("/hds/seq/quantize/enable", seq_quantize_enable),
    OSC_SCALAR("/hds/seq/quantize/resolution", seq_quantize_resolution),
    OSC_SCALAR("/hds/seq/sync_source", seq_sync_source),

    OSC_ARRAY("/hds/combi/%d/midi_channel", combi_lane_midi_channel, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/key_lo", combi_lane_key_lo, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/key_hi", combi_lane_key_hi, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/vel_lo", combi_lane_vel_lo, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/vel_hi", combi_lane_vel_hi, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/transpose", combi_lane_transpose, COMBI_PROGRAM_COUNT),

    OSC_STRING("/hds/device/output", output_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/input", input_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/midi", midi_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/instance_name", instance_name, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/record/path", record_path, RECORD_PATH_LEN),
};
#define OSC_PARAMS_COUNT (sizeof(OSC_PARAMS) / sizeof(OSC_PARAMS[0]))


// Dispatch table: a flat, open-addressed hash map from every CONCRETE OSC address (i.e. OSC_PARAMS
// fully expanded across every array index -- ~4100 entries, not the ~1050 template rows in
// OSC_PARAMS itself) straight to its resolved (offset, kind, str_len). Built once at startup by
// build_osc_dispatch_table() (see its own call site in main(), before the OSC server starts
// accepting messages) and read-only from then on, so it's safe to probe from generic_osc_handler
// with no locking.
//
// This replaces an earlier linear-scan-over-OSC_PARAMS design that measured fine in isolation
// (control messages "at UI-interaction rates, dozens/sec at most") but broke down against how
// transmit_knob_packet() actually behaves: it resends the ENTIRE knob state -- all ~4100 concrete
// addresses -- as one OSC bundle on EVERY single knob change, and that whole bundle is drained
// synchronously on the audio thread (see main()'s "while (lo_server_recv_noblock(...))" loop)
// before that block's audio is rendered. A probe harness driving the old linear-scan handler
// through one such burst measured ~46ms wall time against a ~10.7ms real-time budget for a
// 512-frame block -- a guaranteed audible click/pop synced to the very next UI change, and worse
// the more OSC_PARAMS grows (which it has, repeatedly, as this synth's feature surface grew).
// The hash lookup below is O(1) average per message, so the same 4100-message burst finishes in
// well under a millisecond -- see test_osc_dispatch_no_click.c for the regression test asserting
// this stays true.
#define OSC_DISPATCH_TABLE_SIZE 16384 // power of two; ~4100 concrete addresses -> ~25% load factor
typedef struct {
    char address[128];
    size_t offset;
    OscParamKind kind;
    size_t str_len;
    bool occupied;
} OscDispatchSlot;
static OscDispatchSlot osc_dispatch_table[OSC_DISPATCH_TABLE_SIZE];

// FNV-1a: fast, simple, and more than good enough for a fixed, known-at-startup key set with no
// adversarial-input concern (OSC addresses are our own compile-time-generated strings).
static uint32_t osc_hash_address(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static void osc_dispatch_insert(const char *address, size_t offset, OscParamKind kind, size_t str_len) {
    uint32_t h = osc_hash_address(address) & (OSC_DISPATCH_TABLE_SIZE - 1);
    for (int probes = 0; probes < OSC_DISPATCH_TABLE_SIZE; probes++) {
        OscDispatchSlot *slot = &osc_dispatch_table[h];
        if (!slot->occupied) {
            snprintf(slot->address, sizeof(slot->address), "%s", address);
            slot->offset = offset; slot->kind = kind; slot->str_len = str_len; slot->occupied = true;
            return;
        }
        // Every concrete address OSC_PARAMS expands to must be unique (the whole point of the
        // table) -- a collision on the STRING itself (not just the hash) means two OSC_PARAMS
        // rows generate the same wire address, an authoring bug in the table that must be caught
        // at startup, never silently overwritten.
        if (strcmp(slot->address, address) == 0) {
            fprintf(stderr, "[osc] FATAL: duplicate OSC_PARAMS address '%s' -- two table rows generate the same concrete address\n", address);
            exit(1);
        }
        h = (h + 1) & (OSC_DISPATCH_TABLE_SIZE - 1);
    }
    fprintf(stderr, "[osc] FATAL: dispatch table full inserting '%s' -- raise OSC_DISPATCH_TABLE_SIZE\n", address);
    exit(1);
}

// Mirrors dump_osc_params_addresses.c's own expansion loop exactly (same nested-index shape),
// since that's the same enumeration this table's correctness depends on matching.
static void build_osc_dispatch_table(void) {
    for (size_t i = 0; i < OSC_PARAMS_COUNT; i++) {
        const OscParamEntry *e = &OSC_PARAMS[i];
        int n1 = (e->num_indices >= 1) ? e->count : 1;
        int n2 = (e->num_indices == 2) ? e->count2 : 1;
        for (int idx1 = 0; idx1 < n1; idx1++) {
            for (int idx2 = 0; idx2 < n2; idx2++) {
                char addr[128];
                size_t offset = e->base_offset;
                if (e->num_indices == 0) {
                    snprintf(addr, sizeof(addr), "%s", e->address);
                } else if (e->num_indices == 1) {
                    snprintf(addr, sizeof(addr), e->address, idx1);
                    offset += (size_t)idx1 * e->stride;
                } else {
                    snprintf(addr, sizeof(addr), e->address, idx1, idx2);
                    offset += (size_t)idx1 * e->stride + (size_t)idx2 * e->stride2;
                }
                osc_dispatch_insert(addr, offset, e->kind, e->str_len);
            }
        }
    }
}

static const OscDispatchSlot *osc_dispatch_lookup(const char *address) {
    uint32_t h = osc_hash_address(address) & (OSC_DISPATCH_TABLE_SIZE - 1);
    for (int probes = 0; probes < OSC_DISPATCH_TABLE_SIZE; probes++) {
        const OscDispatchSlot *slot = &osc_dispatch_table[h];
        if (!slot->occupied) return NULL;
        if (strcmp(slot->address, address) == 0) return slot;
        h = (h + 1) & (OSC_DISPATCH_TABLE_SIZE - 1);
    }
    return NULL;
}

// The single generic OSC method (registered with path=NULL, type=NULL -- liblo calls this for
// every incoming message that no more specific method claims). O(1) average-case hash lookup via
// osc_dispatch_table (see above) -- an out-of-range index (e.g. "/hds/lfo/99/rate") simply was
// never inserted into the table, so it correctly falls through to "unrecognized address" below
// rather than needing its own explicit bounds check.
static int generic_osc_handler(const char *path, const char *types, lo_arg **argv, int argc,
                                lo_message msg, void *user_data) {
    (void)msg;
    SynthSharedControls *knobs = (SynthSharedControls *)user_data;
    const OscDispatchSlot *slot = osc_dispatch_lookup(path);
    if (!slot) {
        fprintf(stderr, "[osc] unrecognized address '%s' (types='%s') ignored\n", path, types);
        return 0;
    }
    if (slot->kind == OSC_KIND_FLOAT) {
        if (argc != 1 || types[0] != 'f') { fprintf(stderr, "[osc] '%s' expects one float arg, got types='%s'\n", path, types); return 0; }
        *(float *)((char *)knobs + slot->offset) = argv[0]->f;
    } else { // OSC_KIND_STRING
        if (argc != 1 || types[0] != 's') { fprintf(stderr, "[osc] '%s' expects one string arg, got types='%s'\n", path, types); return 0; }
        snprintf((char *)knobs + slot->offset, slot->str_len, "%s", (const char *)&argv[0]->s);
    }
    return 0;
}

static void osc_error_handler(int num, const char *msg, const char *path) {
    fprintf(stderr, "[osc] liblo error %d (%s): %s\n", num, path ? path : "?", msg);
}

// --- "/hds/request_state": lets a frontend that just connected pull the CURRENT live state
// instead of blindly pushing its own GUI defaults (which used to silently overwrite, say, a DAW-
// restored CLAP patch the instant frontend.py's old boot-time transmit_knob_packet() broadcast
// landed -- see the fix that added this whole request/reply pair). Reuses osc_dispatch_table
// (the exact same address->offset map generic_osc_handler() itself reads), so this can never
// name-drift from what the live knob addresses actually mean. ---

// Registered explicitly (not via the generic catch-all method) so it can carry its own per-owner
// context -- which knobs to dump and where to reply -- mirrors zone_upsert_handler/
// zone_remove_handler's own registration style with &g_sample_engine/&p->sample_engine.
typedef struct {
    const SynthSharedControls *knobs;
} HdsStateRequestContext;

// osc_dispatch_table holds ~4100 concrete addresses -- sent as one giant bundle that would
// exceed UDP's ~65507-byte max datagram payload (measured well over 100KB for the full set,
// vs. transmit_knob_packet()'s own much smaller ~40KB bundle, which only covers the ~1150
// addresses this frontend build actually has live widgets for). Chunked into many small bundles
// instead -- the same "many small sends, not one unbounded one" shape the Sample Bank zone
// catalog protocol already uses for its own unbounded-size data.
#define HDS_STATE_DUMP_CHUNK_SIZE 150

// Sends every currently-occupied osc_dispatch_table address's live value to reply_addr, then one
// terminating sentinel message so the receiver knows the dump is complete (rather than guessing
// via a timeout) -- see README.md's OSC protocol section for the full request/reply shape.
static void send_full_state_dump(lo_address reply_addr, const SynthSharedControls *knobs) {
    if (!reply_addr) return;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    int in_bundle = 0;
    for (int i = 0; i < OSC_DISPATCH_TABLE_SIZE; i++) {
        const OscDispatchSlot *slot = &osc_dispatch_table[i];
        if (!slot->occupied) continue;
        lo_message m = lo_message_new();
        if (slot->kind == OSC_KIND_FLOAT) {
            lo_message_add_float(m, *(const float *)((const char *)knobs + slot->offset));
        } else {
            lo_message_add_string(m, (const char *)knobs + slot->offset);
        }
        lo_bundle_add_message(b, slot->address, m);
        if (++in_bundle >= HDS_STATE_DUMP_CHUNK_SIZE) {
            lo_send_bundle(reply_addr, b);
            lo_bundle_free_recursive(b);
            b = lo_bundle_new(LO_TT_IMMEDIATE);
            in_bundle = 0;
        }
    }
    if (in_bundle > 0) lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    lo_send(reply_addr, "/hds/status/state_dump_complete", "");
}

static int request_state_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    HdsStateRequestContext *ctx = (HdsStateRequestContext *)user_data;
    // Reply to whoever actually sent this request (lo_message_get_source), not ctx->reply_addr's
    // own fixed, startup-configured target -- this is what lets a frontend pull an instance's full
    // state regardless of whether it's the one --osc-reply-host/--osc-reply-port (or CLAP's own
    // paired port) originally pointed at, e.g. the Instance Switcher scanning/connecting to a
    // DIFFERENT already-running instance than whichever frontend it was launched alongside. The
    // source address liblo hands back here is exactly the socket the request arrived FROM -- see
    // frontend.py's own comment on sending this one message from status_sock directly, not the
    // general osc_client, so that source is its real listening address, not an unrelated ephemeral
    // send-only port nothing is listening on.
    lo_address source = lo_message_get_source(msg);
    send_full_state_dump(source, ctx->knobs);
    return 0;
}

// --- Instance Switcher support: "/hds/ping" (discovery) and "/hds/subscribe" (redirect this
// instance's own ongoing status pushes) --- see README's own "Instance Switcher" section for the
// full design. Both are registered per-instance (not via the generic catch-all), same reasoning as
// request_state above.

// A frontend scanning for reachable instances blasts this at a candidate host:port list and
// collects whoever answers within a short timeout -- "replied just now" IS the liveness proof,
// no separate registry/heartbeat bookkeeping needed. Reply-to-sender (see request_state_handler's
// own comment) -- frontend.py sends this from its own status_sock so the reply lands back at its
// actual listening port, not an unrelated scan-only socket.
static int ping_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    const SynthSharedControls *knobs = (const SynthSharedControls *)user_data;
    lo_address source = lo_message_get_source(msg);
    if (!source) return 0;
    lo_send(source, "/hds/pong", "s", knobs->instance_name);
    return 0;
}

// Redirects this instance's own ONGOING, unprompted status pushes (VU meter, tempo, recording
// state -- send_osc_status()/send_seq_audio_status(), called every block from a fixed target) to
// whoever most recently subscribed, instead of only ever the --osc-reply-host/--osc-reply-port (or
// CLAP's own paired port) configured at startup. Unlike request_state/SMF import above, there's no
// per-push incoming message to reply to for an unprompted push, so this one genuinely needs to
// remember a target rather than resolve it fresh each time -- *target is repointed at a freshly
// allocated, independent lo_address (lo_message_get_source()'s own return value is explicitly
// documented as "do not free" and owned by msg, not safe to store past this handler returning) built
// from the sender's own host/port, replacing (and freeing) whatever was targeted before. The LAST
// instance to subscribe wins -- this engine only ever had one live status target to begin with, so
// "switching which instance a frontend controls" and "taking over as the new status target" are the
// same action, not two.
typedef struct {
    lo_address *target; // points at the actual variable send_osc_status()/send_seq_audio_status()
                         // read from at their own call sites -- backend.c's main() passes &(a local
                         // variable already in scope there); clap_plugin.c passes &p->osc_reply_addr
                         // directly, its own per-instance field.
} HdsSubscribeContext;

static int subscribe_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    HdsSubscribeContext *ctx = (HdsSubscribeContext *)user_data;
    lo_address source = lo_message_get_source(msg);
    if (!source) return 0;
    const char *host = lo_address_get_hostname(source);
    const char *port = lo_address_get_port(source);
    if (!host || !port) return 0;
    lo_address new_target = lo_address_new(host, port);
    if (!new_target) return 0;
    if (*ctx->target) lo_address_free(*ctx->target);
    *ctx->target = new_target;
    return 0;
}

// FIXED: Restored 'init_default_controls' macro mapping setup routine missing from compilation stack
static void init_default_controls(SynthSharedControls *knobs) {
    knobs->programs[0].is_polyphonic = 1.0f; knobs->programs[0].vco1_shape = 0.0f; knobs->programs[0].vco2_shape = (float)WAVE_OFF;
    knobs->programs[0].fm_intensity = 0.0f; knobs->programs[0].vco1_vol = 0.8f; knobs->programs[0].vco2_vol = 0.5f;
    knobs->programs[0].portamento_time_ms = 150.0f; knobs->master_volume = 0.50f;
    knobs->transpose_semitones = 0.0f; // centered, no transposition
    knobs->tuning_enable = 0.0f; // standard 12-TET
    knobs->tuning_reference_note = 60.0f;   // Middle C
    knobs->tuning_reference_freq = 261.6256f; // Middle C's own standard 12-TET frequency
    knobs->programs[0].vco1_pan = 0.0f; knobs->programs[0].vco2_pan = 0.0f;
    knobs->programs[0].vco1_fine_tune = 0.0f; knobs->programs[0].vco2_pitch_offset = 0.0f; knobs->programs[0].vco2_fine_tune = 0.0f;
    // Filters: 5 flat slots, Series by default, every slot Off -- a fresh patch pays no filter
    // cost at all until a player actually reaches for one (same "Off is a true bypass" posture
    // as the post-filter EQ bands below). Filter 0's cutoff/res still default to a sensible
    // 800Hz/Q20 starting point for whenever it IS switched on, just not engaged automatically.
    knobs->programs[0].filter_chain_mode = 0.0f; // Series
    knobs->programs[0].filters[0].cutoff = 800.0f; knobs->programs[0].filters[0].res_q = 20.0f;
    knobs->programs[0].filters[0].slope_poles = 0.0f; knobs->programs[0].filters[0].filter_type = 0.0f; // Off -- every filter slot off by default
    knobs->programs[0].filters[0].key_track = 0.0f; knobs->programs[0].filters[0].cutoff_mod_source_cc = -1.0f;
    for (int f = 1; f < 5; f++) {
        knobs->programs[0].filters[f].cutoff = 2000.0f; knobs->programs[0].filters[f].res_q = 20.0f;
        knobs->programs[0].filters[f].slope_poles = 0.0f; knobs->programs[0].filters[f].filter_type = 0.0f; // Off
        knobs->programs[0].filters[f].key_track = 0.0f; knobs->programs[0].filters[f].cutoff_mod_source_cc = -1.0f;
    }
    // Post-filter 3-band EQ: flat (0dB) by default on every band -- see compute_eq_band_coeffs'
    // own "gain_db==0 is a true bypass" comment, so a fresh patch pays no EQ cost at all until a
    // player actually reaches for it. Freq defaults are just sensible starting points for each
    // band's own useful range (Low Shelf/Mid Peak/High Shelf).
    knobs->programs[0].eq_bands[0].freq = 100.0f;  knobs->programs[0].eq_bands[0].gain_db = 0.0f; knobs->programs[0].eq_bands[0].q = 0.707f;
    knobs->programs[0].eq_bands[1].freq = 1000.0f; knobs->programs[0].eq_bands[1].gain_db = 0.0f; knobs->programs[0].eq_bands[1].q = 0.707f;
    knobs->programs[0].eq_bands[2].freq = 5000.0f; knobs->programs[0].eq_bands[2].gain_db = 0.0f; knobs->programs[0].eq_bands[2].q = 0.707f;
    for(int l=0; l<LFO_COUNT; l++) { knobs->programs[0].lfo[l].wave_type = 0.0f; knobs->programs[0].lfo[l].rate_hz = 5.0f; knobs->programs[0].lfo[l].mod_depth = 0.0f; knobs->programs[0].lfo[l].key_sync = 1.0f; knobs->programs[0].lfo[l].dest_target = 0.0f; knobs->programs[0].lfo[l].phase_offset = 0.0f; knobs->programs[0].lfo[l].mod_source_cc = -1.0f; knobs->programs[0].lfo[l].per_key_mode = 0.0f; knobs->programs[0].lfo[l].key_track = 0.0f; knobs->programs[0].lfo[l].vel_track = 0.0f; knobs->programs[0].lfo[l].tempo_sync_division = 0.0f; knobs->programs[0].lfo[l].curve_drive = 0.0f; }
    for(int e=0; e<ENV_COUNT; e++) { knobs->programs[0].eg[e].d = 0.0f; knobs->programs[0].eg[e].h = 0.0f; knobs->programs[0].eg[e].a = 10.0f; knobs->programs[0].eg[e].dec = 150.0f; knobs->programs[0].eg[e].s = 80.0f; knobs->programs[0].eg[e].r = 300.0f; knobs->programs[0].eg[e].al = 0.0f; knobs->programs[0].eg[e].pk = 100.0f; knobs->programs[0].eg_mode[e] = 0.0f; }
    for(int i=0; i<ASSIGNABLE_EG_COUNT; i++) { knobs->programs[0].eg_intensity[i] = 1.0f; knobs->programs[0].eg_dest_target[i] = 0.0f; }
    // Subtle default detune (matches the old fixed +/-1.5% spread) and no width until the user asks for it
    knobs->programs[0].vco1_unison_saw_detune = 25.0f; knobs->programs[0].vco1_unison_saw_width = 50.0f;
    knobs->programs[0].vco2_unison_saw_detune = 25.0f; knobs->programs[0].vco2_unison_saw_width = 50.0f;
    knobs->programs[0].mono_glide_random = 0.0f;
    knobs->programs[0].vco1_mute = 0.0f; knobs->programs[0].vco2_mute = 0.0f;
    knobs->programs[0].hard_sync_mode = 0.0f;
    // Arpeggiator: off by default (byte-identical to today's behavior for every existing
    // patch), Up/1 octave/1-8/70% gate/no Hold when a user does turn it on.
    knobs->programs[0].arp_enabled = 0.0f;
    knobs->programs[0].arp_style = 0.0f;       // Up
    knobs->programs[0].arp_octave_range = 1.0f;
    knobs->programs[0].arp_rate = 1.0f;        // 1/8
    knobs->programs[0].arp_gate_pct = 70.0f;
    knobs->programs[0].arp_hold = 0.0f;
    knobs->programs[0].arp_velocity = 0.0f; // As Played -- reproduces today's exact behavior
    knobs->programs[0].vco1_phase_offset = 0.0f; knobs->programs[0].vco2_phase_offset = 0.0f;

    knobs->programs[0].vco1_pulse_width = 50.0f; knobs->programs[0].vco2_pulse_width = 50.0f;
    knobs->programs[0].vco3_vol = 0.0f; knobs->programs[0].vco3_pan = 0.0f; knobs->programs[0].vco3_mute = 0.0f; knobs->programs[0].vco3_pulse_width = 0.0f;
    knobs->programs[0].vco3_shape = (float)WAVE_OFF; knobs->programs[0].vco3_fine_tune = 0.0f; knobs->programs[0].vco3_pitch_offset = 0.0f;
    knobs->programs[0].vco3_phase_offset = 0.0f; knobs->programs[0].vco3_unison_saw_detune = 25.0f; knobs->programs[0].vco3_unison_saw_width = 50.0f;
    knobs->programs[0].vco4_vol = 0.0f; knobs->programs[0].vco4_pan = 0.0f; knobs->programs[0].vco4_mute = 0.0f; knobs->programs[0].vco4_pulse_width = 50.0f;
    knobs->programs[0].vco4_shape = (float)WAVE_OFF; knobs->programs[0].vco4_fine_tune = 0.0f; knobs->programs[0].vco4_pitch_offset = 0.0f;
    knobs->programs[0].vco4_phase_offset = 0.0f; knobs->programs[0].vco4_unison_saw_detune = 25.0f; knobs->programs[0].vco4_unison_saw_width = 50.0f;
    knobs->programs[0].fm_algorithm = 0.0f;
    knobs->programs[0].fm_depth_3_1 = 0.0f; knobs->programs[0].fm_depth_3_2 = 0.0f;
    knobs->programs[0].fm_depth_4_1 = 0.0f; knobs->programs[0].fm_depth_4_3 = 0.0f;
    knobs->programs[0].fm_depth_1_2 = 0.0f; knobs->programs[0].fm_depth_3_4 = 0.0f; knobs->programs[0].fm_depth_1_3 = 0.0f;
    knobs->programs[0].vco1_pitch = 0.0f; knobs->programs[0].vco2_pitch = 0.0f; knobs->programs[0].vco3_pitch = 0.0f; knobs->programs[0].vco4_pitch = 0.0f;
    knobs->programs[0].vco1_mono_noise = 0.0f; knobs->programs[0].vco2_mono_noise = 0.0f;
    knobs->programs[0].vco3_mono_noise = 0.0f; knobs->programs[0].vco4_mono_noise = 0.0f;
    // Velocity -> Amp: 0 reproduces today's exact velocity-insensitive output level regardless of
    // what velocity value a note-on carries -- see ProgramState.vco{1-4}_vel_to_amp's own comment.
    knobs->programs[0].vco1_vel_to_amp = 0.0f; knobs->programs[0].vco2_vel_to_amp = 0.0f;
    knobs->programs[0].vco3_vel_to_amp = 0.0f; knobs->programs[0].vco4_vel_to_amp = 0.0f;
    // Curve/Drive: 0.0f = linear ramp (Saw/Tri) / clean sine, reproducing today's exact output on
    // every shape -- see ProgramState.vco{1-4}_curve_drive's own comment.
    knobs->programs[0].vco1_curve_drive = 0.0f; knobs->programs[0].vco2_curve_drive = 0.0f;
    knobs->programs[0].vco3_curve_drive = 0.0f; knobs->programs[0].vco4_curve_drive = 0.0f;
    // Osc 3/4 Amp envelopes live at eg[2]/eg[3] (and eg_mode[2]/eg_mode[3]) and were covered by
    // the generic `for(int e=0; e<ENV_COUNT; e++)` default-fill loop above -- nothing extra
    // needed here.

    // Programs 1-7 (Combi mode's remaining lanes, inert until a real per-lane Program picker
    // loads something onto them -- see the "Real Combi mode" plan's Phase 5) and
    // programs[SINGLE_PROGRAM_SLOT] (Single Program mode's own dedicated ProgramState -- see that
    // slot's own comment on the mutual-exclusivity fix) all start from the exact same sane,
    // musical defaults as programs[0] above, not raw zero-init -- a plain struct copy is safe
    // here since ProgramState holds no pointers. Whatever gets loaded/edited onto a slot later
    // simply overwrites this, independently of every other slot.
    for (int p = 1; p <= COMBI_PROGRAM_COUNT; p++) knobs->programs[p] = knobs->programs[0];

    // ext_in_gain defaults to 0.0 (silence/-inf), not unity: an upstream mic pre or interface
    // could already be set hot, and External Input must never be able to burst into the FX bus
    // the moment ext_in_enable goes live (e.g. right after this engine (re)starts, before the
    // frontend's own dial -- which also always inits to 0 -- has sent its first packet). The
    // user always has to bring this up manually.
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) { knobs->ext_in_enable[p] = 0.0f; knobs->ext_in_gain[p] = 0.0f; }
    knobs->gui_routing_tab_visible = 0.0f;
    for (int f = 0; f < FX_SLOT_COUNT; f++) {
        knobs->fx[f].fx_type = FX_TYPE_OFF;
        for (int p = 0; p < FX_SLOT_PARAM_COUNT; p++) knobs->fx[f].param[p] = 0.0f;
        knobs->fx[f].mix = 100.0f; // fully wet once a real effect is assigned to this slot
        knobs->fx[f].bypass = 0.0f;
        knobs->fx[f].fx_source = 0.0f; // Program -- reproduces today's exact behavior by default
        knobs->fx[f].chain_to_next = 0.0f; // every slot its own one-slot branch in Parallel mode -- today's exact behavior
        knobs->fx[f].nam_model_path[0] = '\0';
        knobs->fx[f].nam_ir_path[0] = '\0';
    }
    // Routing defaults: only the Program is routed anywhere (to output pair 0, i.e. today's only
    // output pair), every ext-in pair unrouted, every source's own FX chain in Series -- this
    // combination reproduces today's exact single-output-pair, Program-only behavior bit-for-bit
    // in Single Program mode (Programs 1-7 never claim a voice outside Combi mode, so routing
    // them anywhere is a harmless no-op there).
    for (int s = 0; s < SOURCE_COUNT; s++) {
        knobs->fx_source_routing_mode[s] = 0.0f; // Series
        for (int o = 0; o < OUTPUT_PAIR_MAX; o++) knobs->source_routing[s].enable[o] = 0.0f;
    }
    // Every Combi Program (source ids 0..COMBI_PROGRAM_COUNT-1) defaults to output pair 0, not
    // just Program 1 -- otherwise switching to Combi mode and loading a Program onto lane 2+
    // produces total silence for that lane until the user separately visits the Routing tab,
    // which nothing prompts them to do. Combi Mix (source id COMBI_PROGRAM_COUNT) stays
    // unrouted by default -- routing it too would double-count every individually-routed
    // Program's output. Ext-in pairs stay unrouted by default, unchanged.
    for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) knobs->source_routing[p].enable[0] = 1.0f;
    knobs->programs[0].pb_range_up = 2.0f; knobs->programs[0].pb_range_down = 2.0f; // standard +/-2 semitone default
    knobs->programs[0].sustain_pedal_polarity = 0.0f; // Normal
    knobs->audio_latency_ms = 25.0f; // matches the old hardcoded 25ms target -- no behavior change by default
    knobs->active_voice_limit = 32.0f; // matches the last-measured-safe default -- live-adjustable from here
    knobs->block_size_frames = 128.0f; // matches the old hardcoded BUFFER_SIZE -- no behavior change by default
    knobs->record_enable = 0.0f;
    knobs->record_include_synth_output = 1.0f;
    knobs->record_downmix_stereo = 0.0f;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) knobs->record_input_channel_enable[c] = 0.0f;
    knobs->record_path[0] = '\0';
    knobs->record_format = 0.0f; // WAV
    knobs->programs[0].tempo_bpm = 120.0f; // a common, neutral default tempo
    knobs->programs[0].tempo_marker_count = 0.0f; // no tempo map programmed -- tempo_bpm alone applies
    for (int t = 0; t < TEMPO_MARKER_MAX; t++) { knobs->programs[0].tempo_marker_time[t] = 0.0f; knobs->programs[0].tempo_marker_bpm[t] = 0.0f; }
    // No sample-bank defaults to fill here -- the Sample Bank isn't part of this struct anymore
    // (see its own dedicated socket/g_sample_slots[]).
    // Combi/Sequencer scaffolding: Single Program (today's only real mode) by default, every
    // lane inert -- see SeqLaneState's own comment for why none of this affects the DSP yet.
    knobs->performance_mode = 0.0f; // Single Program
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        knobs->seq_lanes[l].lane_type = 0.0f;    // Program/MIDI
        knobs->seq_lanes[l].volume = 80.0f;
        knobs->seq_lanes[l].pan = 0.0f;
        knobs->seq_lanes[l].midi_channel = 0.0f; // Omni
        knobs->seq_lanes[l].seq_mode = 0.0f;     // Seq
        knobs->seq_lanes[l].mute = 0.0f;
        knobs->seq_lanes[l].rec_armed = 0.0f;
        knobs->seq_lanes[l].rec_overdub = 0.0f;  // Overwrite
        knobs->seq_lanes[l].audio_path[0] = '\0';
        knobs->seq_lanes[l].audio_offset_sec = 0.0f;
        knobs->seq_lanes[l].audio_preview_trigger = 0.0f;
    }
    knobs->seq_transport_play_trigger = 0.0f;
    knobs->seq_transport_pause_trigger = 0.0f;
    knobs->seq_transport_stop_trigger = 0.0f;
    knobs->seq_transport_seek_start_trigger = 0.0f;
    knobs->seq_transport_seek_back_trigger = 0.0f;
    knobs->seq_transport_seek_fwd_trigger = 0.0f;
    knobs->seq_transport_seek_to_target_sec = 0.0f;
    knobs->seq_transport_seek_to_trigger = 0.0f;
    knobs->seq_quantize_enable = 0.0f;
    knobs->seq_quantize_resolution = 4.0f; // sixteenth notes
    knobs->seq_sync_source = 0.0f; // Internal
    knobs->metronome_enable = 0.0f;
    for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
        knobs->combi_lane_midi_channel[p] = 0.0f; // Omni
        knobs->combi_lane_key_lo[p] = 0.0f; knobs->combi_lane_key_hi[p] = 127.0f; // full range
        knobs->combi_lane_vel_lo[p] = 1.0f; knobs->combi_lane_vel_hi[p] = 127.0f; // full range
        knobs->combi_lane_transpose[p] = 0.0f; // centered, no transposition
    }
    snprintf(knobs->output_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->input_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->midi_device, AUDIO_DEVICE_NAME_LEN, "auto");
}

static void init_voice(SynthVoice *voice) {
    voice->midi_note = -1;
    voice->midi_velocity = 100;
    voice->program_index = 0;
    voice->vco1_phase = 0.0f; voice->vco1_target_freq = 0.0f; voice->vco1_current_freq = 0.0f; voice->vco1_tri_state = 0.0f; voice->vco1_prev_fm_phase = 0.0f;
    voice->vco2_phase = 0.0f; voice->vco2_target_freq = 0.0f; voice->vco2_current_freq = 0.0f; voice->vco2_tri_state = 0.0f; voice->vco2_prev_fm_phase = 0.0f; voice->vco2_pending_sync_correction = 0.0f;
    voice->vco3_phase = 0.0f; voice->vco3_target_freq = 0.0f; voice->vco3_current_freq = 0.0f; voice->vco3_tri_state = 0.0f; voice->vco3_prev_fm_phase = 0.0f; voice->vco3_pending_sync_correction = 0.0f;
    voice->vco4_phase = 0.0f; voice->vco4_target_freq = 0.0f; voice->vco4_current_freq = 0.0f; voice->vco4_tri_state = 0.0f; voice->vco4_prev_fm_phase = 0.0f; voice->vco4_pending_sync_correction = 0.0f;
    for(int u=0; u<UNISON_SAW_VOICES; u++) { voice->vco1_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco2_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco3_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco4_unison_saw_phases[u] = (float)rand()/RAND_MAX; }
    for(int c=0; c<2; c++) { for(int f=0; f<5; f++) { voice->f_low[c][f][0]=0.0f; voice->f_low[c][f][1]=0.0f; voice->f_band[c][f][0]=0.0f; voice->f_band[c][f][1]=0.0f; } }
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
    for (int l = 0; l < LFO_COUNT; l++) {
        voice->lfo_phase[l] = 0.0f;
        voice->lfo_decim_counter[l] = 0;
        voice->lfo_decim_last_value[l] = 0.0f;
        voice->lfo_decim_target_value[l] = 0.0f;
    }
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
                            // zone-matching slot sharing this same group id (scoped per-oscillator
                            // -- see round_robin_hit_count's own comment) -- see
                            // find_matching_sample_slot()/g_round_robin_hit_count[].
    int assigned_osc; // 0-3 (vco1..vco4) -- which oscillator this zone belongs to. Set once at
                       // creation (a frontend upsert, a CLI --sfz-vcoN flag, or an
                       // /hds/zone/load_sfz message) and never changed by an in-place
                       // metadata-only upsert (same posture as zone_id, just below). See
                       // find_matching_sample_slot()'s oscillator filter -- every WAVE_SAMPLE
                       // oscillator used to share one unpartitioned note/velocity-keyed catalog;
                       // this is what actually splits it into 4 independent ones.
    int owner_program; // 0-7 = that Combi lane (programs[0..7]), SINGLE_PROGRAM_SLOT (8) = Single
                       // Program mode's own slot. Set once at creation, same immutable posture as
                       // assigned_osc/zone_id just above. Splits the catalog a second way, on top
                       // of assigned_osc: two Programs can each have their OWN VCO1 sample set
                       // rather than being forced to share one unpartitioned list just because
                       // they use the same oscillator -- see find_matching_sample_slot()'s own
                       // owner_program filter. This is also what makes restoring/loading a
                       // Program's own saved state only ever touch that Program's own zones,
                       // instead of every zone anyone ever created across every Program sharing
                       // the same oscillator index.
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

// Part of the multi-instance refactor's HdsEngineState consolidation (see its main definition,
// much further down, for the full explanation) -- split into several separately-named,
// separately-positioned struct "tiers" purely because C requires each field's type to be
// complete at the point of use: SeqMidiLane (needed by the "main" HdsEngineState) isn't declared
// until much later in this file, well after several of these globals are already read/written by
// functions declared right here. Each tier is declared at the earliest point its own member
// type(s) are available, and each is a real, separate per-instance struct/global -- in Phase 6
// (clap_plugin.c) HdsClapPlugin holds one instance of each tier as a named field, which is
// functionally identical "per-instance state," just spread across a few struct types instead of
// one, for this ordering reason alone.
//
// This tier: the Sample Bank catalog. Its base address never moves for the life of ITS OWNING
// INSTANCE (same guarantee sample_disk_thread_main() depends on, now scoped per-instance instead
// of per-process). sample_slot_count tracks the next append position; entries are append-only and
// never compacted/reused after a ZONE_OP_REMOVE (see the OSC zone handlers) -- an inactive
// (.active == false) entry is just permanently skipped everywhere.
typedef struct {
    SampleSlot sample_slots[SAMPLE_SLOT_CAPACITY];
    int        sample_slot_count;
} HdsSampleSlotState;
static HdsSampleSlotState g_sample_slot_state;
#define g_sample_slots g_sample_slot_state.sample_slots
#define g_sample_slot_count g_sample_slot_state.sample_slot_count

// Multi-instance refactor, Phase 3: bundles the three sample-streaming tiers (each already a
// separate global for pure struct-ordering reasons -- see this struct's own comment above) into
// one handle so the sample-streaming functions below only need ONE new parameter each, not up to
// three. Forward-declares HdsStreamsState/HdsPendingLoadState (defined further down, once their
// own member types are complete) as bare struct tags -- legal here because HdsSampleEngine only
// holds POINTERS to them, which are a complete type regardless of whether the pointed-to struct
// itself is complete yet. Can't bundle all four HdsEngineState tiers together (HdsEngineState
// itself isn't declared until much later, after SeqMidiLane) -- this bundle covers exactly the
// tiers load_sample_slot() and its neighbors, declared right after this, need. The actual
// g_sample_engine global instance is declared much further down (see its own comment), once
// HdsStreamsState/HdsPendingLoadState's own globals actually exist to point at -- still just one
// shared instance for now (identical to before this phase, same as HdsEngineState itself); real
// per-instance allocation is Phase 6's job.
typedef struct HdsStreamsState HdsStreamsState;
typedef struct HdsPendingLoadState HdsPendingLoadState;
typedef struct {
    HdsSampleSlotState *slots;
    HdsStreamsState *streams;
    HdsPendingLoadState *pending;
} HdsSampleEngine;

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
static bool load_sample_slot(HdsSampleEngine *se, int index, const char *path, int root_note) {
    if (index < 0 || index >= SAMPLE_SLOT_CAPACITY) return false;
    SampleSlot *slot = &se->slots->sample_slots[index];

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

// Header-only counterpart to load_sample_slot()'s own RIFF chunk walk (same chunk-skip shape,
// stopping right after reading data_size, with none of the pre-roll PCM decode that follows) --
// the C-side SFZ parser's own equivalent of frontend.py's _wav_total_frames, needed to convert
// loop_start=/loop_end=/offset=/end= (given in the SFZ file as absolute sample frames) into the
// 0-1 fractions ZoneRecordWire actually carries. Deliberately a small, independent duplicate of
// load_sample_slot()'s chunk-walk rather than a shared sub-helper both call -- load_sample_slot()
// is a heavily-exercised, already-correct hot path; this keeps that function untouched.
static bool read_wav_frame_count(const char *path, unsigned int *out_total_frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
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
    (void)audio_format; (void)sample_rate; (void)byte_rate; (void)block_align;
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
    fclose(f);
    int bytes_per_sample = bits_per_sample / 8;
    if (num_channels == 0 || bytes_per_sample == 0) return false;
    *out_total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);
    return true;
}

// ============================================================================
// Native SFZ file parser -- lets the backend load a multisample instrument directly from a local
// .sfz file, without any frontend ever having been connected (see the CLI --sfz-vcoN flags and
// the /hds/zone/load_sfz OSC method, both further down, which are the two things that actually
// call sfz_parse_file()/hds_load_sfz_for_osc()). Mirrors frontend.py's own _parse_sfz()/
// _build_zones_from_sfz() opcode table and inheritance rules exactly (see each function's own
// comment for the specific correspondence) so the two loading paths -- a frontend's rich,
// warning-reporting Python import, and this backend-native path -- produce equivalent zones.
// #define/$variable macros are NOT supported here either, matching the Python parser's own
// documented limitation: a region using one is skipped (its sample= fails to resolve), never
// crashed on or silently mis-mapped.
// ============================================================================

// One <region>'s recognized opcodes, built the same way the Python importer's per-region dict is
// (SFZ inheritance: a region starts as a copy of its enclosing <group>/<global> and each opcode
// line overwrites its own field, last-value-wins). A bounded struct-of-optional-fields instead of
// a generic string map, since HDS's supported opcode set is fixed and small. Text-valued fields
// are used wherever the raw string needs deferred numeric interpretation (lokey/hikey/
// pitch_keycenter as note-name-or-int; loop_start/loop_end/offset/end as frame counts, which need
// total_frames -- only known once sample= resolves -- before they can become a 0-1 fraction).
#define SFZ_TEXT_FIELD_LEN 32
typedef struct {
    bool has_sample;              char sample[SAMPLE_PATH_LEN];
    bool has_key;                  char key[SFZ_TEXT_FIELD_LEN];
    bool has_lokey;                char lokey[SFZ_TEXT_FIELD_LEN];
    bool has_hikey;                char hikey[SFZ_TEXT_FIELD_LEN];
    bool has_pitch_keycenter;      char pitch_keycenter[SFZ_TEXT_FIELD_LEN];
    bool has_pitch_keytrack;       float pitch_keytrack;
    bool has_lovel;                int lovel;
    bool has_hivel;                int hivel;
    bool has_loop_mode;            char loop_mode[24];
    bool has_loop_start;           char loop_start[SFZ_TEXT_FIELD_LEN];
    bool has_loop_end;             char loop_end[SFZ_TEXT_FIELD_LEN];
    bool has_offset;               char offset[SFZ_TEXT_FIELD_LEN];
    bool has_end;                  char end[SFZ_TEXT_FIELD_LEN];
    bool has_tune;                 float tune;
    bool has_pitch;                float pitch;
    bool has_volume;               float volume;
    bool has_pan;                  float pan;
    bool has_direction;            char direction[16];
    bool has_group;                char group[SFZ_TEXT_FIELD_LEN];
    bool has_seq_length;           // presence only, matches "group= AND seq_length=" round-robin gate
} SfzRegion;

#define SFZ_MAX_REGIONS_PER_FILE 65536
// File-scope static scratch buffer (used by hds_load_sfz_for_osc(), further down, as the
// out_regions argument to sfz_parse_file()) rather than a stack array or per-call malloc -- reused
// across calls, not reentrant/thread-safe, which is correct because every caller (the CLI
// --sfz-vcoN flags at startup, and /hds/zone/load_sfz's handler) runs on the same single thread
// that already owns g_sample_slot_state/g_pending_load_state (main()'s own OSC-poll/audio
// thread) -- same "audio-thread-only, no cross-thread sync needed" posture those already have.
static SfzRegion g_sfz_parse_scratch[SFZ_MAX_REGIONS_PER_FILE];

// Mirrors _sfz_note_to_number(): bare MIDI int, or a note name like "C4"/"F#3"/"Ab-1" (C4=60,
// 's' accepted as an alternate sharp spelling same as the Python side). Returns -1 (the C
// equivalent of Python's None here) on unparseable/out-of-range input.
static int sfz_note_to_number(const char *value) {
    if (!value || !*value) return -1;
    // Bare integer (allowing a leading '-' for the theoretical negative case, though notes are
    // always 0-127 in practice).
    char *endptr = NULL;
    long as_int = strtol(value, &endptr, 10);
    if (endptr && *endptr == '\0' && endptr != value) {
        if (as_int < 0 || as_int > 127) return -1;
        return (int)as_int;
    }
    // Note name: letter (a-g/A-G), optional accidental (#, s, S, b, B), signed octave integer.
    const char *p = value;
    if (!isalpha((unsigned char)*p)) return -1;
    char letter = (char)tolower((unsigned char)*p); p++;
    static const int PITCH_CLASS[7] = {9, 11, 0, 2, 4, 5, 7}; // a,b,c,d,e,f,g
    if (letter < 'a' || letter > 'g') return -1;
    int pitch_class = PITCH_CLASS[letter - 'a'];
    if (*p == '#' || *p == 's' || *p == 'S') { pitch_class += 1; p++; }
    else if (*p == 'b' || *p == 'B') { pitch_class -= 1; p++; }
    if (!*p) return -1;
    endptr = NULL;
    long octave = strtol(p, &endptr, 10);
    if (!endptr || *endptr != '\0' || endptr == p) return -1;
    long note = (octave + 1) * 12 + pitch_class;
    if (note < 0 || note > 127) return -1;
    return (int)note;
}

// Mirrors _sfz_frame_to_frac(): an absolute frame-count string + total_frames -> a 0-1 fraction.
// Returns false (out_frac left untouched) if frame_str isn't a valid number.
static bool sfz_frame_to_frac(const char *frame_str, unsigned int total_frames, float *out_frac) {
    if (!frame_str || !*frame_str || total_frames <= 1) return false;
    char *endptr = NULL;
    double frame = strtod(frame_str, &endptr);
    if (!endptr || endptr == frame_str) return false;
    double frac = frame / (double)(total_frames - 1);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    *out_frac = (float)frac;
    return true;
}

// Mirrors _resolve_sfz_sample_path(): backslashes normalized to '/' first (Windows-authored
// libraries), then tried in order: (1) as-is if absolute and it exists; (2) joined with
// default_path (the <control> header's own default_path=, itself already resolved against
// sfz_dir by the caller); (3) joined with sfz_dir; (4) a case-insensitive filename match via
// opendir()/readdir() within whichever of (2)/(3) resolved to a real, existing directory.
// Bounded "dir/name" join with an explicit precision on each "%s", for the same
// -Wformat-truncation reason sfz_set_text_field exists -- dir can come from a PATH_MAX-sized
// directory buffer several frames up (sfz_parse_file_recursive's sfz_dir), but the destination is
// the SAMPLE_PATH_LEN(256) bound every actual sample path in this file already lives within; a
// join that would overflow simply truncates, and the resulting fopen() attempt then just fails
// like any other unresolvable candidate path (skip, don't crash).
static void sfz_join_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    int half = (int)(dst_len / 2) - 1;
    if (half < 1) half = 1;
    snprintf(dst, dst_len, "%.*s/%.*s", half, dir, half, name);
}

static bool sfz_resolve_sample_path(const char *sample_value, const char *sfz_dir, const char *default_path, char *out_resolved, size_t out_len) {
    char normalized[SAMPLE_PATH_LEN];
    snprintf(normalized, sizeof(normalized), "%s", sample_value);
    for (char *c = normalized; *c; c++) if (*c == '\\') *c = '/';

    if (normalized[0] == '/') {
        FILE *f = fopen(normalized, "rb");
        if (f) { fclose(f); snprintf(out_resolved, out_len, "%s", normalized); return true; }
    }

    const char *candidate_dirs[2]; int candidate_dir_count = 0;
    if (default_path && default_path[0]) candidate_dirs[candidate_dir_count++] = default_path;
    candidate_dirs[candidate_dir_count++] = sfz_dir;

    for (int d = 0; d < candidate_dir_count; d++) {
        char joined[SAMPLE_PATH_LEN];
        sfz_join_path(joined, sizeof(joined), candidate_dirs[d], normalized);
        FILE *f = fopen(joined, "rb");
        if (f) { fclose(f); snprintf(out_resolved, out_len, "%s", joined); return true; }
    }

    // Case-insensitive filename fallback: only the basename needs matching (a sample= value with
    // its own subdirectory prefix that merely differs in case is rare enough not to be worth
    // walking -- same practical scope the Python importer's own fallback covers).
    const char *base = strrchr(normalized, '/');
    base = base ? base + 1 : normalized;
    for (int d = 0; d < candidate_dir_count; d++) {
        DIR *dir = opendir(candidate_dirs[d]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcasecmp(entry->d_name, base) == 0) {
                char joined[SAMPLE_PATH_LEN];
                sfz_join_path(joined, sizeof(joined), candidate_dirs[d], entry->d_name);
                FILE *f = fopen(joined, "rb");
                if (f) { fclose(f); closedir(dir); snprintf(out_resolved, out_len, "%s", joined); return true; }
            }
        }
        closedir(dir);
    }
    return false;
}

// Finds the next SFZ token (a "<name>" header or a "name=" opcode key) at or after `pos`, mirroring
// _SFZ_TOKEN_RE's own "<(\w+)>|([a-zA-Z_][a-zA-Z0-9_]*)=" alternation -- a header always starts
// with a literal '<', an opcode key always starts with a letter/underscore, so there's no ambiguity
// in which pattern to try at a given scan position. Returns the token's own start position (its
// end position is written to *out_token_end), or SFZ_TOKEN_NOT_FOUND if nothing matched before len.
#define SFZ_TOKEN_NOT_FOUND (-1)
static long sfz_find_next_token(const char *text, size_t pos, size_t len, size_t *out_token_end, bool *out_is_header, char *out_name, size_t out_name_cap) {
    for (size_t i = pos; i < len; i++) {
        if (text[i] == '<') {
            size_t j = i + 1, n = 0;
            while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_')) { if (n + 1 < out_name_cap) out_name[n++] = text[j]; j++; }
            if (n > 0 && j < len && text[j] == '>') {
                out_name[n] = '\0';
                *out_is_header = true;
                *out_token_end = j + 1;
                return (long)i;
            }
            continue;
        }
        if (isalpha((unsigned char)text[i]) || text[i] == '_') {
            size_t j = i, n = 0;
            while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_')) { if (n + 1 < out_name_cap) out_name[n++] = text[j]; j++; }
            if (n > 0 && j < len && text[j] == '=') {
                out_name[n] = '\0';
                *out_is_header = false;
                *out_token_end = j + 1;
                return (long)i;
            }
            // Not immediately followed by '=' -- not an opcode token; resume scanning right after
            // this identifier run (not i+1) to avoid re-testing every character inside it.
            i = j - 1;
            continue;
        }
    }
    return SFZ_TOKEN_NOT_FOUND;
}

// Bounded copy into a fixed-size SfzRegion text field, with an explicit precision on the "%s" so
// -Wformat-truncation can prove the write can never overflow regardless of value's actual length
// -- value always comes from a much larger scratch buffer (see sfz_tokenize_and_apply's own
// value[SAMPLE_PATH_LEN]), which is what triggers the warning on a plain "%s" here. Truncation
// itself is harmless for every field this is used on (note names/loop-mode keywords/group ids are
// always short in real SFZ files).
static void sfz_set_text_field(char *dst, size_t dst_len, const char *value) {
    snprintf(dst, dst_len, "%.*s", (int)(dst_len - 1), value);
}

// Applies one opcode (already-trimmed key/value strings) onto a region-shaped accumulator --
// shared by every header level (<global>/<group>/<region>/<control>/anything-else-thrown-away).
static void sfz_apply_opcode(SfzRegion *r, const char *key, const char *value) {
    if (strcasecmp(key, "sample") == 0) { r->has_sample = true; sfz_set_text_field(r->sample, sizeof(r->sample), value); }
    else if (strcasecmp(key, "key") == 0) { r->has_key = true; sfz_set_text_field(r->key, sizeof(r->key), value); }
    else if (strcasecmp(key, "lokey") == 0) { r->has_lokey = true; sfz_set_text_field(r->lokey, sizeof(r->lokey), value); }
    else if (strcasecmp(key, "hikey") == 0) { r->has_hikey = true; sfz_set_text_field(r->hikey, sizeof(r->hikey), value); }
    else if (strcasecmp(key, "pitch_keycenter") == 0) { r->has_pitch_keycenter = true; sfz_set_text_field(r->pitch_keycenter, sizeof(r->pitch_keycenter), value); }
    else if (strcasecmp(key, "pitch_keytrack") == 0) { r->has_pitch_keytrack = true; r->pitch_keytrack = (float)atof(value); }
    else if (strcasecmp(key, "lovel") == 0) { r->has_lovel = true; r->lovel = atoi(value); }
    else if (strcasecmp(key, "hivel") == 0) { r->has_hivel = true; r->hivel = atoi(value); }
    else if (strcasecmp(key, "loop_mode") == 0) { r->has_loop_mode = true; sfz_set_text_field(r->loop_mode, sizeof(r->loop_mode), value); }
    else if (strcasecmp(key, "loop_start") == 0) { r->has_loop_start = true; sfz_set_text_field(r->loop_start, sizeof(r->loop_start), value); }
    else if (strcasecmp(key, "loop_end") == 0) { r->has_loop_end = true; sfz_set_text_field(r->loop_end, sizeof(r->loop_end), value); }
    else if (strcasecmp(key, "offset") == 0) { r->has_offset = true; sfz_set_text_field(r->offset, sizeof(r->offset), value); }
    else if (strcasecmp(key, "end") == 0) { r->has_end = true; sfz_set_text_field(r->end, sizeof(r->end), value); }
    else if (strcasecmp(key, "tune") == 0 || strcasecmp(key, "pitch") == 0) {
        // "pitch=" is the fallback spelling for "tune=" -- both write the same field (matches
        // _build_zones_from_sfz's own "tune= (fallback pitch=)" note).
        r->has_tune = true; r->tune = (float)atof(value);
    }
    else if (strcasecmp(key, "volume") == 0) { r->has_volume = true; r->volume = (float)atof(value); }
    else if (strcasecmp(key, "pan") == 0) { r->has_pan = true; r->pan = (float)atof(value); }
    else if (strcasecmp(key, "direction") == 0) { r->has_direction = true; sfz_set_text_field(r->direction, sizeof(r->direction), value); }
    else if (strcasecmp(key, "group") == 0) { r->has_group = true; sfz_set_text_field(r->group, sizeof(r->group), value); }
    else if (strcasecmp(key, "seq_length") == 0) { r->has_seq_length = true; }
    // Any other opcode (ADSR, filter, curve, etc.) is silently ignored -- out of scope for HDS's
    // own zone model, same posture _build_zones_from_sfz's own opcode table already has.
}

// Tokenizes one already-comment-stripped, #include-blanked text buffer into <global>/<group>/
// <master>/<region>/<control> opcode sets, applying inheritance exactly like _parse_sfz's own
// current_dict swap: <global> resets the global accumulator; <group>/<master> both copy from the
// current global accumulator into a fresh group accumulator; <region> copies from the current
// group accumulator into a fresh region, appended to out_regions once the NEXT header (or EOF) is
// reached; <control> opcodes go into a separate accumulator (only default_path= is used, resolved
// against base_dir); any other header (<curve>, <effect>, ...) tokenizes into a throwaway
// accumulator and is otherwise ignored.
static void sfz_tokenize_and_apply(const char *text, const char *base_dir, SfzRegion *out_regions, int max_regions, int *io_region_count, char *out_default_path, size_t out_default_path_len) {
    size_t len = strlen(text);
    SfzRegion global_opcodes; memset(&global_opcodes, 0, sizeof(global_opcodes));
    SfzRegion group_opcodes; memset(&group_opcodes, 0, sizeof(group_opcodes));
    SfzRegion control_opcodes; memset(&control_opcodes, 0, sizeof(control_opcodes));
    SfzRegion throwaway; memset(&throwaway, 0, sizeof(throwaway));
    SfzRegion current_region; memset(&current_region, 0, sizeof(current_region));
    bool region_open = false;
    SfzRegion *active = &throwaway; // where opcodes seen before ANY header go -- discarded, matches SFZ having no meaning for stray top-level opcodes

    size_t pos = 0;
    for (;;) {
        size_t token_end; bool is_header; char name[64];
        long token_start = sfz_find_next_token(text, pos, len, &token_end, &is_header, name, sizeof(name));

        if (is_header || token_start == SFZ_TOKEN_NOT_FOUND) {
            // Close out whatever region was open before switching context (a new header, or EOF).
            if (region_open && *io_region_count < max_regions) {
                out_regions[(*io_region_count)++] = current_region;
                region_open = false;
            }
        }
        if (token_start == SFZ_TOKEN_NOT_FOUND) break;

        if (is_header) {
            if (strcasecmp(name, "global") == 0) {
                memset(&global_opcodes, 0, sizeof(global_opcodes));
                active = &global_opcodes;
            } else if (strcasecmp(name, "group") == 0 || strcasecmp(name, "master") == 0) {
                group_opcodes = global_opcodes;
                active = &group_opcodes;
            } else if (strcasecmp(name, "region") == 0) {
                current_region = group_opcodes;
                region_open = true;
                active = &current_region;
            } else if (strcasecmp(name, "control") == 0) {
                memset(&control_opcodes, 0, sizeof(control_opcodes));
                active = &control_opcodes;
            } else {
                memset(&throwaway, 0, sizeof(throwaway));
                active = &throwaway;
            }
            pos = token_end;
            continue;
        }

        // Opcode token: name[0..) is the key, token_end is right after '='. The value runs until
        // the NEXT token's own start (or EOF), trimmed of surrounding whitespace.
        size_t value_start = token_end;
        size_t next_end; bool next_is_header; char next_name[64];
        long next_start = sfz_find_next_token(text, token_end, len, &next_end, &next_is_header, next_name, sizeof(next_name));
        size_t value_end = (next_start == SFZ_TOKEN_NOT_FOUND) ? len : (size_t)next_start;
        while (value_start < value_end && isspace((unsigned char)text[value_start])) value_start++;
        size_t trimmed_end = value_end;
        while (trimmed_end > value_start && isspace((unsigned char)text[trimmed_end - 1])) trimmed_end--;
        char value[SAMPLE_PATH_LEN];
        size_t vlen = trimmed_end - value_start; if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
        memcpy(value, text + value_start, vlen); value[vlen] = '\0';

        if (active == &control_opcodes) {
            if (strcasecmp(name, "default_path") == 0) {
                char normalized[SAMPLE_PATH_LEN]; snprintf(normalized, sizeof(normalized), "%s", value);
                for (char *c = normalized; *c; c++) if (*c == '\\') *c = '/';
                if (normalized[0] == '/') snprintf(out_default_path, out_default_path_len, "%s", normalized);
                else sfz_join_path(out_default_path, out_default_path_len, base_dir, normalized);
            }
        } else {
            sfz_apply_opcode(active, name, value);
            if (strcasecmp(name, "sample") == 0) {
                // Resolved EAGERLY, right here, using THIS file's own base_dir/default_path --
                // not deferred to the caller -- because a region's sample= is usually set at
                // <group>/<global> level and inherited by many regions via the struct-copy
                // above; resolving once here (and inheriting the already-resolved absolute path)
                // is both correct for multi-file libraries (each #include'd file's own regions
                // resolve against ITS OWN directory, not the top-level file's) and cheaper than
                // re-resolving per region. Left as the raw, unresolved value if resolution fails
                // -- that zone's own load simply fails later (logged, slot deactivated), the same
                // "skip, don't crash" outcome the Python importer's own warning-and-skip gives.
                char resolved[SAMPLE_PATH_LEN];
                if (sfz_resolve_sample_path(value, base_dir, out_default_path[0] ? out_default_path : NULL, resolved, sizeof(resolved))) {
                    snprintf(active->sample, sizeof(active->sample), "%s", resolved);
                }
            }
        }
        pos = token_end; // resume scanning right after '=' -- the value text itself is skipped
                          // over naturally since the next token search always starts past it
    }
}

// Reads `path`, strips "//" comments and blanks "#include \"...\"" lines (mirrors _parse_sfz's
// own two-pass shape: strip/tokenize first, separately re-scan the raw text for #include lines
// and recurse into each). Cycle guard: `visited_realpaths` is a caller-owned array of
// realpath()-resolved strings; this file is skipped (no new regions) if already present, mirroring
// _parse_sfz's own `_visited` set exactly. `io_region_count` is threaded through every recursive
// call (not a global) so regions from every #include'd file land in the SAME out_regions array,
// continuing to append rather than each recursion starting its own separate count.
static void sfz_parse_file_recursive(const char *path, SfzRegion *out_regions, int max_regions, int *io_region_count, char *out_default_path, size_t out_default_path_len, char (*visited_realpaths)[PATH_MAX], int *visited_count, int max_visited) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) { fprintf(stderr, "[sfz] failed to resolve '%s'\n", path); return; }
    for (int i = 0; i < *visited_count; i++) if (strcmp(visited_realpaths[i], resolved) == 0) return; // cycle
    if (*visited_count >= max_visited) return;
    strcpy(visited_realpaths[*visited_count], resolved);
    (*visited_count)++;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[sfz] failed to open '%s': %s\n", path, strerror(errno)); return; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return; }
    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return; }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    char sfz_dir[PATH_MAX];
    snprintf(sfz_dir, sizeof(sfz_dir), "%s", resolved);
    char *last_slash = strrchr(sfz_dir, '/');
    if (last_slash) *last_slash = '\0'; else snprintf(sfz_dir, sizeof(sfz_dir), ".");

    // Strip "//" comments (blank to end of line) and collect+blank "#include \"...\"" lines, in
    // one forward pass over the mutable buffer.
    #define SFZ_MAX_INCLUDES_PER_FILE 64
    char include_paths[SFZ_MAX_INCLUDES_PER_FILE][SAMPLE_PATH_LEN];
    int include_count = 0;
    for (size_t i = 0; i < read_n; i++) {
        if (buf[i] == '/' && i + 1 < read_n && buf[i + 1] == '/') {
            while (i < read_n && buf[i] != '\n') buf[i++] = ' ';
        } else if (buf[i] == '#' && strncmp(buf + i, "#include", 8) == 0) {
            size_t j = i + 8;
            while (j < read_n && isspace((unsigned char)buf[j])) j++;
            if (j < read_n && buf[j] == '"') {
                size_t start = j + 1, k = start;
                while (k < read_n && buf[k] != '"' && buf[k] != '\n') k++;
                if (k < read_n && buf[k] == '"' && include_count < SFZ_MAX_INCLUDES_PER_FILE) {
                    size_t plen = k - start; if (plen >= SAMPLE_PATH_LEN) plen = SAMPLE_PATH_LEN - 1;
                    memcpy(include_paths[include_count], buf + start, plen);
                    include_paths[include_count][plen] = '\0';
                    include_count++;
                    for (size_t z = i; z <= k && z < read_n; z++) buf[z] = ' '; // blank the whole directive
                    i = k;
                }
            }
        }
    }

    sfz_tokenize_and_apply(buf, sfz_dir, out_regions, max_regions, io_region_count, out_default_path, out_default_path_len);
    free(buf);

    for (int inc = 0; inc < include_count; inc++) {
        char inc_path[SAMPLE_PATH_LEN];
        if (include_paths[inc][0] == '/') sfz_set_text_field(inc_path, sizeof(inc_path), include_paths[inc]);
        else sfz_join_path(inc_path, sizeof(inc_path), sfz_dir, include_paths[inc]);
        char inc_default_path[SAMPLE_PATH_LEN]; inc_default_path[0] = '\0';
        sfz_parse_file_recursive(inc_path, out_regions, max_regions, io_region_count, inc_default_path, sizeof(inc_default_path), visited_realpaths, visited_count, max_visited);
        // A sub-file's own default_path= (if any) only applies to ITS OWN regions -- already
        // baked into them via the recursive call above, which passes sfz_dir derived from
        // inc_path's own directory. The outer file's out_default_path is left as whatever the
        // outer file's own <control> block set (or empty), matching real SFZ scoping.
    }
}

// Public entry point: owns/frees the visited-path cycle-guard list itself. Returns the number of
// regions parsed into out_regions (0 on total failure, e.g. the file doesn't open).
static int sfz_parse_file(const char *path, SfzRegion *out_regions, int max_regions, char *out_default_path, size_t out_default_path_len) {
    out_default_path[0] = '\0';
    int region_count = 0;
    #define SFZ_MAX_VISITED_FILES 256
    char (*visited)[PATH_MAX] = (char (*)[PATH_MAX])malloc((size_t)SFZ_MAX_VISITED_FILES * PATH_MAX);
    if (!visited) return 0;
    int visited_count = 0;
    sfz_parse_file_recursive(path, out_regions, max_regions, &region_count, out_default_path, out_default_path_len, visited, &visited_count, SFZ_MAX_VISITED_FILES);
    free(visited);
    return region_count;
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

// g_sample_streams[]/g_sample_disk_thread/g_sample_disk_thread_shutdown are declared as part of
// HdsStreamsState, just below SeqAudioLane (see that struct's own comment for why the combined
// declaration point had to move this far down).

// ============================================================================
// Sequencer audio-lane RAM loading ("Load Audio", SeqLaneState.audio_path)
// ============================================================================
// Audio lanes are meant to be short, RAM-cached clips (per the plan's own "Future Full-Build
// Notes" -- NOT the DFD disk-streaming pool above, which exists for large multisample
// libraries). Reading a whole file into RAM is, unlike a sample zone's small header-only parse,
// an unbounded blocking operation -- it must never run on the realtime audio thread. Rather than
// invent a new concurrency primitive, this reuses the exact pattern already validated by the
// sample-streaming engine above: a dedicated background pthread does the blocking I/O, and hands
// the finished result to the audio thread through a single atomic synchronization point
// (load_state), with every other field a plain write-then-release/read-after-acquire, per the
// C11 memory model. One thread is spawned per load request (not a persistent thread, since loads
// are rare user actions, not a continuous stream) and immediately detached; loads for the same
// lane are always serialized by the main-loop gate below (never spawns a new one while the
// previous is still SEQ_AUDIO_LOADING), so exactly one thread ever touches a given lane's `pcm`
// at a time.
typedef enum { SEQ_AUDIO_EMPTY = 0, SEQ_AUDIO_LOADING = 1, SEQ_AUDIO_READY = 2, SEQ_AUDIO_ERROR = 3 } SeqAudioLoadState;

typedef struct {
    float *pcm;           // interleaved float samples, valid once load_state == SEQ_AUDIO_READY.
                            // Not itself atomic -- see the struct comment above for why that's
                            // safe (its writer-thread's writes happen-before the load_state
                            // release store, which happens-before any reader's acquire load).
    int frame_count;
    int channels;
    int sample_rate; // the FILE's own native rate (may differ from the engine's SAMPLE_RATE) --
                       // needed for a correct duration display now, and for correct playback
                       // rate once a real playback engine exists.
    _Atomic int load_state; // SeqAudioLoadState -- the one synchronization point for this lane.
    char last_requested_path[SEQ_AUDIO_PATH_LEN]; // main-thread-only; never touched by a loader
                                                     // thread, so needs no atomics of its own.
    // "Preview" one-shot playback (see poll_seq_audio_preview_triggers/the mix step in
    // render_audio_block). Plain fields, no atomics needed -- unlike pcm/load_state above (which
    // cross from a background loader thread to the audio thread), these are written AND read
    // exclusively by the single realtime thread (main()'s loop calls both the trigger-poll and
    // render_audio_block itself -- see the "single non-threaded audio loop" invariant documented
    // at this file's OSC section).
    float last_seen_trigger;
    bool is_playing;
    long play_frame; // next frame index to read from pcm; advances by 1 per rendered sample
    // true if the TRANSPORT started this lane playing (transport_start_fresh, on STOPPED ->
    // PLAYING); false if the manual Preview button started it. Gates the per-sample mix loop:
    // a transport-driven lane pauses/stops with the transport, a Preview-driven lane always
    // plays through regardless of transport state, exactly like before the transport existed.
    bool driven_by_transport;
    // Set by transport_start_fresh() for every Seq-mode audio lane, even one whose load hasn't
    // finished yet (SEQ_AUDIO_READY not reached) -- without this, hitting Play on a lane that's
    // still loading (a real, easy-to-hit race: click Load Audio, then immediately click Play)
    // permanently skips that lane for the rest of this Play session, since nothing else ever
    // revisits it. poll_seq_audio_transport_catchup(), called every block, starts any armed lane
    // the moment it becomes ready while the transport is still playing, and clears the flag once
    // started (or on Stop). A no-op for a lane that was already ready when Play was pressed --
    // transport_start_fresh starts it immediately and this never needs to trigger.
    bool armed_for_transport;
} SeqAudioLane;

// This tier (see HdsSampleSlotState's own comment for why this is split into several structs):
// the sample-streaming disk-read pool (SampleStream, ready just above SeqAudioLane) plus the
// Sequencer audio lanes (SeqAudioLane, just declared) -- grouped together because both need to
// be visible before the functions right after this point start using them, and SampleStream was
// already ready by the time SeqAudioLane became available, so one combined declaration here
// covers both with no further splitting needed.
typedef struct HdsStreamsState {
    SampleStream sample_streams[MAX_SAMPLE_STREAMS];
    pthread_t    sample_disk_thread;
    _Atomic bool sample_disk_thread_shutdown;
    bool         sample_disk_thread_running; // lets deactivate()/destroy() know whether THIS
                                              // instance actually has a thread to join
    SeqAudioLane seq_audio[SEQ_LANE_COUNT];
    // Tracks in-flight per-lane audio loader threads so deactivate() can join them before this
    // struct can be freed -- see poll_seq_audio_load_requests's own comment on the use-after-
    // free hazard this fixes (a detached thread was safe only while seq_audio[] was process-
    // lifetime BSS that never got freed; once it's a field of a free()-able struct, a still-in-
    // flight detached load completing after destroy() would write into freed memory).
    pthread_t    seq_audio_loader_thread[SEQ_LANE_COUNT];
    bool         seq_audio_loader_thread_active[SEQ_LANE_COUNT];
} HdsStreamsState;
static HdsStreamsState g_streams_state;
#define g_sample_streams g_streams_state.sample_streams
#define g_sample_disk_thread g_streams_state.sample_disk_thread
#define g_sample_disk_thread_shutdown g_streams_state.sample_disk_thread_shutdown
#define g_seq_audio g_streams_state.seq_audio

typedef struct {
    int lane_index;
    char path[SEQ_AUDIO_PATH_LEN];
    // Multi-instance refactor: which instance's HdsStreamsState (seq_audio[]/loader-thread-
    // tracking) this in-flight load belongs to -- lets the thread write into the right instance's
    // seq_audio[] instead of an implicit global, and lets deactivate() know which instance's
    // tracking table to update once this thread exits (see seq_audio_loader_thread_active's own
    // comment for the use-after-free hazard this fixes).
    HdsStreamsState *owner;
} SeqAudioLoadRequest;

// Reads an entire WAV file into a newly malloc'd interleaved float buffer. Deliberately separate
// from load_sample_slot() above (which only caches a short pre-roll and hands the rest to
// on-demand disk streaming) -- an audio lane's whole clip lives in RAM at once, so this is a
// plain one-shot full-file decode, run only on seq_audio_loader_thread_main's own thread. Keeps
// every channel present in the file (unlike the sampler's mono/stereo-only pre-roll) since a
// lane's own future mixdown, not this loader, should decide how to handle >2 channels. Returns
// NULL on any failure (out_frames/out_channels/out_sample_rate left at 0).
static float *seq_audio_load_wav_from_disk(const char *path, int *out_frames, int *out_channels, int *out_sample_rate) {
    *out_frames = 0; *out_channels = 0; *out_sample_rate = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[seq-audio] failed to open '%s': %s\n", path, strerror(errno)); return NULL; }

    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }

    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    if (fread(&subchunk1_size, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&audio_format, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&num_channels, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&sample_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&byte_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&block_align, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&bits_per_sample, 2, 1, f) != 1) { fclose(f); return NULL; }
    (void)byte_rate; (void)block_align;

    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "[seq-audio] '%s': unsupported WAV format tag %u (need 1=PCM or 3=float)\n", path, audio_format);
        fclose(f); return NULL;
    }
    if (audio_format == 1 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': unsupported %u-bit PCM\n", path, bits_per_sample);
        fclose(f); return NULL;
    }
    if (audio_format == 3 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': float WAV must be 32-bit\n", path);
        fclose(f); return NULL;
    }
    if (num_channels == 0) { fclose(f); return NULL; }
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);

    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }
    unsigned int data_size;
    if (fread(&data_size, 4, 1, f) != 1) { fclose(f); return NULL; }

    int bytes_per_sample = bits_per_sample / 8;
    unsigned int total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);
    if (total_frames == 0) { fclose(f); return NULL; }

    float *pcm = malloc((size_t)total_frames * num_channels * sizeof(float));
    if (!pcm) { fprintf(stderr, "[seq-audio] '%s': out of memory (%u frames x %u ch)\n", path, total_frames, num_channels); fclose(f); return NULL; }

    for (unsigned int i = 0; i < total_frames * num_channels; i++) {
        float sample = 0.0f;
        bool ok = true;
        if (audio_format == 1) {
            if (bits_per_sample == 16) {
                short raw16;
                if (fread(&raw16, 2, 1, f) != 1) ok = false; else sample = (float)raw16 / 32768.0f;
            } else if (bits_per_sample == 24) {
                unsigned char b24[3];
                if (fread(b24, 1, 3, f) != 3) ok = false;
                else { int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); sample = (float)v / 2147483648.0f; }
            } else {
                int raw32;
                if (fread(&raw32, 4, 1, f) != 1) ok = false; else sample = (float)raw32 / 2147483648.0f;
            }
        } else {
            if (fread(&sample, 4, 1, f) != 1) ok = false;
        }
        if (!ok) { fprintf(stderr, "[seq-audio] '%s': truncated read at sample %u\n", path, i); free(pcm); fclose(f); return NULL; }
        pcm[i] = sample;
    }
    fclose(f);
    *out_frames = (int)total_frames;
    *out_channels = (int)num_channels;
    *out_sample_rate = (int)sample_rate;
    return pcm;
}

// Linear-interpolation sample-rate conversion. Runs once, only on this (non-realtime) loader
// thread, so a plain single-pass linear resample is more than adequate -- nothing here needs to
// be low-latency or audio-thread-safe. Returns a newly malloc'd interleaved buffer at to_rate and
// writes its frame count to *out_frames; returns NULL (leaving *out_frames untouched) if nothing
// needs doing or on allocation failure, in which case the caller keeps the original buffer as-is.
static float *seq_audio_resample_linear(const float *pcm, int frames, int channels, int from_rate, int to_rate, int *out_frames) {
    if (from_rate <= 0 || to_rate <= 0 || from_rate == to_rate || frames <= 0) return NULL;
    double ratio = (double)to_rate / (double)from_rate;
    int new_frames = (int)((double)frames * ratio + 0.5);
    if (new_frames < 1) new_frames = 1;
    float *out = malloc((size_t)new_frames * (size_t)channels * sizeof(float));
    if (!out) return NULL;
    double step = (double)from_rate / (double)to_rate;
    for (int i = 0; i < new_frames; i++) {
        double src_pos = (double)i * step;
        long idx0 = (long)src_pos;
        if (idx0 >= frames) idx0 = frames - 1;
        long idx1 = idx0 + 1;
        if (idx1 >= frames) idx1 = frames - 1;
        double frac = src_pos - (double)idx0;
        for (int c = 0; c < channels; c++) {
            float s0 = pcm[idx0 * channels + c];
            float s1 = pcm[idx1 * channels + c];
            out[i * channels + c] = (float)((double)s0 + ((double)s1 - (double)s0) * frac);
        }
    }
    *out_frames = new_frames;
    return out;
}

static void *seq_audio_loader_thread_main(void *arg) {
    SeqAudioLoadRequest *req = (SeqAudioLoadRequest *)arg;
    SeqAudioLane *lane = &req->owner->seq_audio[req->lane_index];
    int frames = 0, channels = 0, sample_rate = 0;
    float *pcm = seq_audio_load_wav_from_disk(req->path, &frames, &channels, &sample_rate);
    // Safe to read/free the previous buffer here without its own atomic: loads for this lane are
    // serialized by the main-loop gate (see below), and its write happens-before this thread's
    // own start via the acquire/release pair on load_state plus pthread_create's own
    // synchronizes-with guarantee -- see this file's SeqAudioLane comment.
    float *old_pcm = lane->pcm;
    if (pcm && sample_rate > 0 && sample_rate != SAMPLE_RATE) {
        // The playback loop (render_seq_audio_preview_sample) reads one PCM frame per rendered
        // sample with no rate conversion of its own -- a file whose native rate differs from the
        // engine's would otherwise play back at the wrong pitch/speed. Resample here, once, on
        // this background thread, so every loaded lane always plays back correctly regardless of
        // its source file's native rate.
        int resampled_frames = 0;
        float *resampled = seq_audio_resample_linear(pcm, frames, channels, sample_rate, SAMPLE_RATE, &resampled_frames);
        if (resampled) {
            free(pcm);
            pcm = resampled;
            frames = resampled_frames;
            sample_rate = SAMPLE_RATE;
        }
        // resampled == NULL (allocation failure): fall through and keep the original-rate
        // buffer -- still playable, just at the wrong pitch/speed, same as before this feature
        // existed, rather than losing the load entirely over a resample-only OOM.
    }
    if (pcm) {
        lane->frame_count = frames;
        lane->channels = channels;
        lane->sample_rate = sample_rate;
        lane->pcm = pcm;
        atomic_store_explicit(&lane->load_state, SEQ_AUDIO_READY, memory_order_release);
    } else {
        lane->pcm = NULL;
        lane->frame_count = 0;
        lane->channels = 0;
        lane->sample_rate = 0;
        atomic_store_explicit(&lane->load_state, SEQ_AUDIO_ERROR, memory_order_release);
    }
    free(old_pcm);
    free(req);
    return NULL;
}

// Once-per-block change detection (mirrors the existing output_device/input_device strncmp
// pattern in main()'s loop): notices when a lane's requested audio_path differs from what's
// already loading/loaded, and spawns a loader thread for it -- never more than one in flight per
// lane at a time. Call this from main()'s loop, same cadence as every other live-knob check.
static void poll_seq_audio_load_requests(HdsStreamsState *streams, const SynthSharedControls *knobs) {
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        SeqAudioLane *lane = &streams->seq_audio[l];
        const char *want_path = knobs->seq_lanes[l].audio_path;
        if (want_path[0] == '\0') continue;
        if (strncmp(want_path, lane->last_requested_path, SEQ_AUDIO_PATH_LEN) == 0) continue;
        int state = atomic_load_explicit(&lane->load_state, memory_order_acquire);
        if (state == SEQ_AUDIO_LOADING) continue; // catches up once the in-flight load finishes
        snprintf(lane->last_requested_path, SEQ_AUDIO_PATH_LEN, "%s", want_path);
        // A previous load for this lane may still be un-joined (this is a later reload of the
        // same lane) -- join it now before reusing the tracking slot below, so this instance
        // never leaks a thread handle across repeated reloads. Cheap in practice: gated by
        // load_state != LOADING just above, so any earlier thread already finished its work by
        // the time control reaches here -- see SeqAudioLoadRequest.owner's own comment on why
        // this join (not a detach) is now required at all.
        if (streams->seq_audio_loader_thread_active[l]) {
            pthread_join(streams->seq_audio_loader_thread[l], NULL);
            streams->seq_audio_loader_thread_active[l] = false;
        }
        SeqAudioLoadRequest *req = malloc(sizeof(SeqAudioLoadRequest));
        if (!req) continue;
        req->lane_index = l;
        req->owner = streams;
        snprintf(req->path, SEQ_AUDIO_PATH_LEN, "%s", want_path);
        atomic_store_explicit(&lane->load_state, SEQ_AUDIO_LOADING, memory_order_release);
        if (pthread_create(&streams->seq_audio_loader_thread[l], NULL, seq_audio_loader_thread_main, req) == 0) {
            streams->seq_audio_loader_thread_active[l] = true;
        } else {
            fprintf(stderr, "[seq-audio] failed to spawn loader thread for lane %d\n", l);
            free(req);
            atomic_store_explicit(&lane->load_state, SEQ_AUDIO_ERROR, memory_order_release);
        }
    }
}

#if HDS_NAM_ENABLED
// Same "once-per-block change detection" shape as poll_seq_audio_load_requests() just above --
// notices when an FX slot's nam_model_path/nam_ir_path differs from what that slot's own state
// already has loaded, and (re)loads it. Unlike Sequencer audio lanes, this runs the load
// SYNCHRONOUSLY on this thread (the same one OSC dispatch and audio rendering both run on -- see
// this file's own single-threaded-main-loop design) rather than spawning a loader thread: a .nam
// model file is a small, bounded JSON+weights blob (nothing like a whole song-length audio
// recording), and a cab IR is capped at FX_NAM_IR_MAX_TAPS frames -- both comfortably finish within
// one block's worth of stall, the same bet load_sample_slot() already makes for a sample zone's WAV
// header + pre-roll. A real streaming thread would be the right call for something unboundedly
// large; neither of these is.
//
// Only meaningful once a slot's own engine state actually exists as an FxNamState -- gated on
// active_type == FX_TYPE_NAM, which sync_fx_engine_types() (called once per block from
// render_audio_block, before this in the same iteration -- see main()'s own loop) guarantees is
// current for any type change already applied by the time this runs; a slot switched to
// FX_TYPE_NAM within the SAME block this runs simply picks up its load on the next block instead,
// same one-block grace every other live-knob check in this loop already has.
static void poll_fx_nam_requests(FXSlotEngine fx_engine[FX_SLOT_COUNT], const SynthSharedControls *knobs) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        if (fx_engine[s].active_type != FX_TYPE_NAM || !fx_engine[s].state) continue;
        FxNamState *st = (FxNamState *)fx_engine[s].state;
        const char *want_model = knobs->fx[s].nam_model_path;
        if (strncmp(want_model, st->loaded_model_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_model(st, want_model);
        }
        const char *want_ir = knobs->fx[s].nam_ir_path;
        if (strncmp(want_ir, st->loaded_ir_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_ir(st, want_ir);
        }
    }
}
#endif // HDS_NAM_ENABLED

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

// "Preview" one-shot playback trigger detection (see SeqLaneState.audio_preview_trigger's own
// comment). Edge-triggered: the frontend increments the trigger value by 1 on every click, so
// any change (not just "> last") starts (or restarts) that lane's playback from the beginning --
// or audio_offset_sec seconds in, if positive. Deliberately independent of the Sequencer
// transport (see poll_transport_triggers/transport_start_fresh, added alongside this): a
// Preview-triggered lane always plays through to completion regardless of Play/Pause/Stop state,
// unlike a transport-driven lane -- auditioning a sound must never depend on whether the
// Sequencer happens to be running. Call once per block from main()'s loop, same as
// poll_seq_audio_load_requests.
static void poll_seq_audio_preview_triggers(HdsStreamsState *streams, const SynthSharedControls *knobs) {
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        SeqAudioLane *lane = &streams->seq_audio[l];
        float want_trigger = knobs->seq_lanes[l].audio_preview_trigger;
        if (want_trigger == lane->last_seen_trigger) continue;
        lane->last_seen_trigger = want_trigger;
        if (atomic_load_explicit(&lane->load_state, memory_order_acquire) != SEQ_AUDIO_READY || !lane->pcm || lane->frame_count <= 0) {
            continue; // nothing loaded (yet) for this lane -- silently ignore the trigger
        }
        float offset_sec = knobs->seq_lanes[l].audio_offset_sec;
        long start_frame = 0;
        if (offset_sec > 0.0f && lane->sample_rate > 0) {
            start_frame = (long)(offset_sec * (float)lane->sample_rate);
            if (start_frame >= lane->frame_count) start_frame = lane->frame_count - 1;
        }
        lane->play_frame = start_frame;
        lane->is_playing = true;
        lane->driven_by_transport = false;
    }
}

// Claims a free pool slot for a NEW stream session (audio-thread only, called from
// handle_note_on). Returns -1 if the pool is exhausted -- a real possibility if someone tries to
// play more simultaneous WAVE_SAMPLE oscillators than MAX_SAMPLE_STREAMS; the caller treats -1
// as "this oscillator stays silent until a slot frees up," never a crash.
static int claim_sample_stream(HdsSampleEngine *se, int slot_index, unsigned int start_frame) {
    SampleStream *streams = se->streams->sample_streams;
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        if (!streams[i].in_use) {
            streams[i].in_use = true;
            atomic_store_explicit(&streams[i].slot_index, slot_index, memory_order_relaxed);
            atomic_store_explicit(&streams[i].start_frame, start_frame, memory_order_relaxed);
            // Fresh note, fresh session -- a reclaimed pool entry must not inherit a stale `true`
            // left over from whatever earlier note last released this same slot.
            atomic_store_explicit(&streams[i].release_requested, false, memory_order_relaxed);
            atomic_store_explicit(&streams[i].command, SAMPLE_CMD_START, memory_order_release);
            return i;
        }
    }
    return -1;
}

// Releases a claimed stream back to the pool (audio-thread only) and tells the disk thread to
// close its file. Safe to call even if a STOP is already pending/being processed.
static void release_sample_stream(HdsSampleEngine *se, int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&se->streams->sample_streams[stream_index].command, SAMPLE_CMD_STOP, memory_order_release);
    se->streams->sample_streams[stream_index].in_use = false;
}

// Marks a claimed stream's note as having entered RELEASE (audio-thread only, called once from
// trigger_voice_release() per oscillator). Read by sample_disk_thread_main() to decide whether a
// loop_release_exit zone should keep honoring its loop or fall through to its natural end -- see
// SampleSlot.loop_release_exit's own comment. A no-op on an already-released/invalid stream index,
// same defensive shape as release_sample_stream() above.
static void request_sample_stream_release(HdsSampleEngine *se, int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&se->streams->sample_streams[stream_index].release_requested, true, memory_order_release);
}

static void *sample_disk_thread_main(void *arg) {
    HdsSampleEngine *se = (HdsSampleEngine *)arg;
    for (;;) {
        bool work_done = false;
        for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
            SampleStream *st = &se->streams->sample_streams[i];

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
                SampleSlot *slot = (st->active_slot_index >= 0 && st->active_slot_index < SAMPLE_SLOT_CAPACITY) ? &se->slots->sample_slots[st->active_slot_index] : NULL;
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
            SampleSlot *slot = &se->slots->sample_slots[st->active_slot_index];
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

        if (atomic_load_explicit(&se->streams->sample_disk_thread_shutdown, memory_order_acquire)) break;
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
//   "/hds/zone/upsert" (types "i" + "f"*18 + "s") -- add-or-update one zone by zone_id (on
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
    // Which oscillator (0-3) this zone belongs to -- appended (18th float), never renumbering the
    // 17 above, matching this file's own established convention. See SampleSlot.assigned_osc.
    float assigned_osc;
    // Which Program owns this zone (0-7 a Combi lane, SINGLE_PROGRAM_SLOT a Single Program) --
    // appended (19th float), same convention. See SampleSlot.owner_program.
    float owner_program;
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
    int assigned_osc; // 0-3 -- see SampleSlot.assigned_osc
} PendingSampleLoad;
#define PENDING_SAMPLE_LOAD_CAPACITY 4096
// How many queued sample loads (each a bounded, one-file blocking file-open+pre-roll-decode) the
// audio thread will perform per block -- the throttle that keeps a large bulk-add from stalling
// real-time audio in one long burst; see SAMPLE_SLOT_CAPACITY's comment for the accepted
// trade-off (one genuinely slow file, e.g. a cold network mount, still stalls the whole audio
// thread for its own load duration -- this bounds how many such stalls can pile up per block, not
// how long any single one can take).
#define SAMPLE_LOADS_PER_BLOCK 1

// This tier (see HdsSampleSlotState's own comment for why this is split into several structs):
// the throttled sample-load queue plus round-robin cycling state, grouped together since both
// need PendingSampleLoad (just declared) and nothing later.
typedef struct HdsPendingLoadState {
    PendingSampleLoad pending_loads[PENDING_SAMPLE_LOAD_CAPACITY];
    int          pending_load_head, pending_load_tail, pending_load_count;
    // [assigned_osc][group_idx] -- scoped per-oscillator so two unrelated zone sets on different
    // oscillators that happen to both use round_robin_group=1 cycle independently instead of
    // sharing one counter. Index 0 of the inner dimension is unused (group 0 = "not grouped"),
    // same convention as before this gained the outer oscillator dimension. Outermost dimension is
    // owner_program (0..SINGLE_PROGRAM_SLOT) -- added alongside owner_program itself, so two
    // Programs sharing the same oscillator + round_robin_group number cycle independently too,
    // same reasoning as the oscillator dimension's own comment.
    unsigned int round_robin_hit_count[SINGLE_PROGRAM_SLOT + 1][4][MAX_ROUND_ROBIN_GROUPS + 1];
} HdsPendingLoadState;
static HdsPendingLoadState g_pending_load_state;
#define g_pending_loads g_pending_load_state.pending_loads
#define g_pending_load_head g_pending_load_state.pending_load_head
#define g_pending_load_tail g_pending_load_state.pending_load_tail
#define g_pending_load_count g_pending_load_state.pending_load_count
#define g_round_robin_hit_count g_pending_load_state.round_robin_hit_count

// The actual HdsSampleEngine global instance -- the type itself is forward-declared much earlier
// (right after HdsSampleSlotState, see that comment for why) so load_sample_slot() and friends
// can take it as a parameter; the instance is only declared here, now that all three tiers'
// globals it points at actually exist.
static HdsSampleEngine g_sample_engine = { &g_sample_slot_state, &g_streams_state, &g_pending_load_state };

// Clamps a wire-carried oscillator index (a plain float, like every other OSC-carried enum in
// this file) into the valid 0-3 range -- shared by every site that reads ZoneRecordWire.assigned_osc.
static inline int clamp_assigned_osc(float raw) {
    int v = (int)raw;
    if (v < 0) return 0;
    if (v > 3) return 3;
    return v;
}

// Same idea as clamp_assigned_osc() above, for ZoneRecordWire.owner_program's 0..SINGLE_PROGRAM_SLOT range.
static inline int clamp_owner_program(float raw) {
    int v = (int)raw;
    if (v < 0) return 0;
    if (v > SINGLE_PROGRAM_SLOT) return SINGLE_PROGRAM_SLOT;
    return v;
}

static void enqueue_pending_sample_load(HdsSampleEngine *se, int slot_index, const ZoneRecordWire *rec) {
    if (se->pending->pending_load_count >= PENDING_SAMPLE_LOAD_CAPACITY) {
        fprintf(stderr, "[samples] pending-load queue full -- dropping load for zone_id %d (re-send to retry)\n", rec->zone_id);
        return;
    }
    PendingSampleLoad *p = &se->pending->pending_loads[se->pending->pending_load_tail];
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
    p->assigned_osc = clamp_assigned_osc(rec->assigned_osc);
    se->pending->pending_load_tail = (se->pending->pending_load_tail + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
    se->pending->pending_load_count++;
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
static void drain_pending_sample_loads(HdsSampleEngine *se) {
    for (int i = 0; i < SAMPLE_LOADS_PER_BLOCK && se->pending->pending_load_count > 0; i++) {
        PendingSampleLoad *p = &se->pending->pending_loads[se->pending->pending_load_head];
        se->pending->pending_load_head = (se->pending->pending_load_head + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
        se->pending->pending_load_count--;

        // A ZONE_OP_REMOVE can tombstone this slot_index while its load is still sitting in the
        // queue (added, then removed again before its throttled turn comes up) -- the removal
        // wins: skip the load rather than have load_sample_slot() resurrect a tombstoned slot by
        // setting active back to true. Safe under the append-only invariant (a slot_index is
        // never reused for a different zone_id), so this can only be the same zone this load was
        // originally queued for.
        if (!se->slots->sample_slots[p->slot_index].active) continue;

        if (load_sample_slot(se, p->slot_index, p->path, p->root_note)) {
            SampleSlot *slot = &se->slots->sample_slots[p->slot_index];
            slot->min_note = p->min_note; slot->max_note = p->max_note;
            slot->min_velocity = p->min_velocity; slot->max_velocity = p->max_velocity;
            slot->unpitched = p->unpitched;
            slot->round_robin_group = p->round_robin_group;
            slot->assigned_osc = p->assigned_osc;
            slot->loop_enabled = p->loop_enabled;
            apply_loop_points(slot, p->loop_start_frac, p->loop_end_frac);
            slot->tune_cents = p->tune_cents; slot->gain_db = p->gain_db; slot->pan = p->pan;
            slot->start_offset_frac = p->start_offset_frac;
            slot->reverse = p->reverse;
            slot->loop_release_exit = p->loop_release_exit;
            apply_playback_end(slot, p->end_frac);
        } else {
            fprintf(stderr, "[samples] failed to load queued sample '%s' (zone_id %d)\n", p->path, p->zone_id);
            se->slots->sample_slots[p->slot_index].active = false;
        }
    }
}

// Applies one zone upsert -- unchanged logic from the old wire-parsed version, just fed an
// already-populated ZoneRecordWire (built from OSC args in zone_upsert_handler() below) instead
// of parsing one out of a raw byte buffer.
//
// BUG FIX (the actual one, not an earlier mode-gated attempt that was tried and reverted -- see
// git history/README): every zone used to live in one flat, unpartitioned-by-Program catalog,
// split only by oscillator. Loading/restoring ANY Program's own saved samples meant every OTHER
// Program that happened to share an oscillator index saw its zones silently dragged along too --
// "patches existing" (in the shared catalog) was being treated as a reason to load their samples,
// even for a Program nothing currently referenced. Fixed by owner_program (see SampleSlot's own
// comment): each zone now genuinely belongs to ONE Program, so restoring/loading a Program's own
// saved state only ever touches that Program's own zones. Loading itself stays exactly as
// immediate as it always was -- there's no separate "is this relevant right now" gate here at
// all; a zone loads because its owning Program actually has it assigned, full stop.
static void apply_zone_upsert(HdsSampleEngine *se, const ZoneRecordWire *rec) {
    int found_index = -1;
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (se->slots->sample_slots[s].active && se->slots->sample_slots[s].zone_id == rec->zone_id) { found_index = s; break; }
    }

    if (found_index < 0) {
        // Brand new zone -- append. sample_slots is a plain static array (zero-initialized
        // BSS), so a never-before-used position already has loaded=false, pre_roll_l/r=NULL,
        // etc.; only zone_id/active/owner_program need setting explicitly here.
        if (se->slots->sample_slot_count >= SAMPLE_SLOT_CAPACITY) {
            fprintf(stderr, "[samples] Sample Bank at its %d-zone capacity -- ignoring zone_id %d\n", SAMPLE_SLOT_CAPACITY, rec->zone_id);
            return;
        }
        found_index = se->slots->sample_slot_count++;
        se->slots->sample_slots[found_index].zone_id = rec->zone_id;
        se->slots->sample_slots[found_index].active = true;
        se->slots->sample_slots[found_index].owner_program = clamp_owner_program(rec->owner_program);
        enqueue_pending_sample_load(se, found_index, rec);
        return;
    }

    SampleSlot *slot = &se->slots->sample_slots[found_index];
    if (strncmp(slot->file_path, rec->path, SAMPLE_PATH_LEN) != 0 || !slot->loaded) {
        // Path changed (or a previous load of this same slot never finished/failed) -- (re)queue
        // rather than apply cheap fields in place.
        enqueue_pending_sample_load(se, found_index, rec);
        return;
    }

    // Path unchanged and already loaded: apply the cheap fields immediately, no reload.
    slot->root_note = (int)rec->root_note;
    slot->min_note = (int)rec->min_note; slot->max_note = (int)rec->max_note;
    slot->min_velocity = (int)rec->min_velocity; slot->max_velocity = (int)rec->max_velocity;
    slot->unpitched = rec->unpitched != 0.0f;
    slot->round_robin_group = (int)rec->round_robin_group;
    slot->assigned_osc = clamp_assigned_osc(rec->assigned_osc);
    slot->loop_enabled = rec->loop_enabled != 0.0f;
    apply_loop_points(slot, rec->loop_start, rec->loop_end);
    slot->tune_cents = rec->tune_cents; slot->gain_db = rec->gain_db; slot->pan = rec->pan;
    slot->start_offset_frac = rec->start_offset_frac;
    slot->reverse = rec->reverse != 0.0f;
    slot->loop_release_exit = rec->loop_release_exit != 0.0f;
    apply_playback_end(slot, rec->end_frac);
}

static void apply_zone_remove(HdsSampleEngine *se, int32_t zone_id) {
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (se->slots->sample_slots[s].active && se->slots->sample_slots[s].zone_id == zone_id) {
            se->slots->sample_slots[s].active = false;
            se->slots->sample_slots[s].loaded = false; // matches the pre-existing "empty path" unload behavior
            free(se->slots->sample_slots[s].pre_roll_l); se->slots->sample_slots[s].pre_roll_l = NULL;
            free(se->slots->sample_slots[s].pre_roll_r); se->slots->sample_slots[s].pre_roll_r = NULL;
            break;
        }
    }
}

// "/hds/zone/upsert" i + 19 f's + s -- zone_id + 19 floats in ZoneRecordWire's own field order
// (root_note, min_note, max_note, min_velocity, max_velocity, unpitched, loop_start, loop_end,
// loop_enabled, round_robin_group, tune_cents, gain_db, pan, start_offset_frac, reverse,
// loop_release_exit, end_frac, assigned_osc, owner_program) + path. liblo only dispatches here
// when the incoming type string matches exactly, so argc/types are already guaranteed correct --
// no manual validation needed, unlike generic_osc_handler's table.
static int zone_upsert_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    ZoneRecordWire rec;
    rec.zone_id = argv[0]->i;
    float *f = &rec.root_note; // root_note..owner_program are 19 consecutive floats
    for (int i = 0; i < 19; i++) f[i] = argv[1 + i]->f;
    snprintf(rec.path, SAMPLE_PATH_LEN, "%s", (const char *)&argv[20]->s);
    apply_zone_upsert(se, &rec);
    return 0;
}
// "/hds/zone/remove" i -- a single zone_id.
static int zone_remove_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    apply_zone_remove(se, argv[0]->i);
    return 0;
}

// Backend-native SFZ loading -- lets a whole multisample instrument be loaded directly from a
// local .sfz file, for one oscillator (0-3), without any frontend ever deriving/pushing zone
// data itself. The shared core both the CLI --sfz-vcoN startup flags and the /hds/zone/load_sfz
// OSC method call (see main() and zone_load_sfz_handler, further down) -- they differ only in
// WHEN/HOW this gets triggered, not in what it does once triggered. Reuses apply_zone_upsert()
// (the exact same function an OSC-driven zone upsert already calls) for every parsed region, so
// DFD streaming/pre-roll setup happens through the identical, already-proven throttled-load path
// either way -- one code path, not two to keep in sync.
static int32_t g_next_native_zone_id = -1; // decrements per native-loaded zone; frontend-assigned
    // zone_ids are always >= 1 (SamplerMappingGrid._next_zone_id only ever increments from 1), so
    // this negative space can never collide with one.

static int hds_load_sfz_for_osc(HdsSampleEngine *se, int osc_index, int owner_program, const char *sfz_path) {
    if (osc_index < 0 || osc_index > 3) return 0;
    owner_program = clamp_owner_program((float)owner_program);

    // Always "replace THIS PROGRAM's own zone set on this oscillator" -- a no-op at empty-catalog
    // startup time, the real live-reload behavior for /hds/zone/load_sfz. Scoped by owner_program
    // too (not just osc_index) so loading a new SFZ for one Program's own VCO1 doesn't remove a
    // DIFFERENT Program's own VCO1 zones that happen to share the oscillator index -- see
    // SampleSlot.owner_program's own comment. Collect ids first (a plain forward scan while
    // tombstoning in place is safe here too, same as apply_zone_remove's own loop, but collecting
    // first keeps this obviously correct regardless of how apply_zone_remove's own loop shape
    // might change later).
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        SampleSlot *slot = &se->slots->sample_slots[s];
        if (slot->active && slot->assigned_osc == osc_index && slot->owner_program == owner_program) apply_zone_remove(se, slot->zone_id);
    }

    char default_path[SAMPLE_PATH_LEN];
    int region_count = sfz_parse_file(sfz_path, g_sfz_parse_scratch, SFZ_MAX_REGIONS_PER_FILE, default_path, sizeof(default_path));
    if (region_count == 0) {
        fprintf(stderr, "[sfz] '%s': no regions parsed (osc %d)\n", sfz_path, osc_index + 1);
        return 0;
    }

    // group=+seq_length= round-robin compaction, local to this one load -- mirrors the Python
    // importer's own per-file round_robin_ids dict: each distinct qualifying group= value seen in
    // THIS file gets its own small sequential id (1, 2, 3, ...), since SFZ's own group numbers can
    // collide across unrelated articulations/files.
    #define SFZ_MAX_SEEN_GROUPS 64
    char seen_group_names[SFZ_MAX_SEEN_GROUPS][SFZ_TEXT_FIELD_LEN];
    int seen_group_ids[SFZ_MAX_SEEN_GROUPS];
    int seen_group_count = 0;

    int queued = 0;
    for (int i = 0; i < region_count; i++) {
        SfzRegion *r = &g_sfz_parse_scratch[i];
        if (!r->has_sample || !r->sample[0]) continue; // no sample= at all -- nothing to play

        const char *lokey_str = r->has_lokey ? r->lokey : (r->has_key ? r->key : NULL);
        const char *hikey_str = r->has_hikey ? r->hikey : (r->has_key ? r->key : NULL);
        int lokey = lokey_str ? sfz_note_to_number(lokey_str) : 0; if (lokey < 0) lokey = 0;
        int hikey = hikey_str ? sfz_note_to_number(hikey_str) : 127; if (hikey < 0) hikey = 127;

        bool is_drum = r->has_pitch_keytrack && r->pitch_keytrack == 0.0f;
        int key_center = lokey;
        if (!is_drum) {
            const char *keycenter_str = r->has_pitch_keycenter ? r->pitch_keycenter : lokey_str;
            if (keycenter_str) { int kc = sfz_note_to_number(keycenter_str); if (kc >= 0) key_center = kc; }
        }

        int min_vel = r->has_lovel ? r->lovel : 0; if (min_vel < 0) min_vel = 0; if (min_vel > 127) min_vel = 127;
        int max_vel = r->has_hivel ? r->hivel : 127; if (max_vel < 0) max_vel = 0; if (max_vel > 127) max_vel = 127;

        bool loop_enabled = false, loop_release_exit = false;
        if (r->has_loop_mode) {
            if (strcasecmp(r->loop_mode, "loop_sustain") == 0) { loop_enabled = true; loop_release_exit = true; }
            else if (strcasecmp(r->loop_mode, "loop_continuous") == 0) { loop_enabled = true; }
            // "no_loop"/"one_shot"/anything else: loop_enabled stays false.
        }

        unsigned int total_frames = 0;
        read_wav_frame_count(r->sample, &total_frames); // best-effort -- 0 leaves every frac below at its safe default

        float loop_start_frac = 0.0f, loop_end_frac = 1.0f, offset_frac = 0.0f, end_frac = 1.0f;
        if (r->has_loop_start) sfz_frame_to_frac(r->loop_start, total_frames, &loop_start_frac);
        if (r->has_loop_end) sfz_frame_to_frac(r->loop_end, total_frames, &loop_end_frac);
        if (r->has_offset) sfz_frame_to_frac(r->offset, total_frames, &offset_frac);
        if (r->has_end) sfz_frame_to_frac(r->end, total_frames, &end_frac);

        int round_robin_group = 0;
        if (r->has_group && r->has_seq_length) {
            int found = -1;
            for (int g = 0; g < seen_group_count; g++) if (strcmp(seen_group_names[g], r->group) == 0) { found = g; break; }
            if (found < 0 && seen_group_count < SFZ_MAX_SEEN_GROUPS) {
                found = seen_group_count;
                snprintf(seen_group_names[found], sizeof(seen_group_names[found]), "%s", r->group);
                seen_group_ids[found] = seen_group_count + 1;
                seen_group_count++;
            }
            if (found >= 0) round_robin_group = seen_group_ids[found];
        }

        ZoneRecordWire rec;
        rec.zone_id = g_next_native_zone_id--;
        rec.root_note = is_drum ? 60.0f : (float)key_center;
        rec.min_note = (float)lokey; rec.max_note = (float)hikey;
        rec.min_velocity = (float)min_vel; rec.max_velocity = (float)max_vel;
        rec.unpitched = is_drum ? 1.0f : 0.0f;
        rec.loop_start = loop_start_frac; rec.loop_end = loop_end_frac;
        rec.loop_enabled = loop_enabled ? 1.0f : 0.0f;
        rec.round_robin_group = (float)round_robin_group;
        rec.tune_cents = r->has_tune ? r->tune : 0.0f;
        rec.gain_db = r->has_volume ? r->volume : 0.0f;
        rec.pan = (r->has_pan ? r->pan : 0.0f) / 100.0f;
        rec.start_offset_frac = offset_frac;
        rec.reverse = (r->has_direction && strcasecmp(r->direction, "reverse") == 0) ? 1.0f : 0.0f;
        rec.loop_release_exit = loop_release_exit ? 1.0f : 0.0f;
        rec.end_frac = end_frac;
        rec.assigned_osc = (float)osc_index;
        rec.owner_program = (float)owner_program;
        snprintf(rec.path, SAMPLE_PATH_LEN, "%s", r->sample);

        apply_zone_upsert(se, &rec);
        queued++;
    }
    return queued;
}

// "/hds/zone/load_sfz" i i s -- oscillator index (0-3), owner Program index (0-7 a Combi lane,
// SINGLE_PROGRAM_SLOT a Single Program), and a path, valid on the BACKEND's own filesystem (never
// a payload the frontend decoded itself -- see this pass's own design rationale: a backend on
// another system has its samples with it locally, so only a path needs to cross the wire). Reuses
// the exact same hds_load_sfz_for_osc() the CLI --sfz-vcoN flags call.
static int zone_load_sfz_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    int osc_index = argv[0]->i;
    int owner_program = argv[1]->i;
    const char *sfz_path = (const char *)&argv[2]->s;
    int n = hds_load_sfz_for_osc(se, osc_index, owner_program, sfz_path);
    fprintf(stderr, "[sfz] /hds/zone/load_sfz osc=%d program=%d '%s': %d zone(s) queued\n", osc_index + 1, owner_program, sfz_path, n);
    return 0;
}

// Round-robin hit counters, one per possible group id (index 0 unused -- group 0 means "not
// grouped", see SampleSlot.round_robin_group). Persists across calls the same way limiter_gain/
// lfo_phase_mod_state do elsewhere in this file (one synth instance, one audio thread -- no
// atomics needed, this is audio-thread-only state).

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
static int find_matching_sample_slot(HdsSampleEngine *se, int assigned_osc, int owner_program, int note, int velocity) {
    long best_area = -1; // -1 sentinel: "no candidate yet", distinct from a real zero-area zone
    int first_best_idx = -1;
    int tie_count = 0;
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc) continue;
        if (slot->owner_program != owner_program) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (best_area < 0 || area < best_area) { best_area = area; first_best_idx = i; tie_count = 1; }
        else if (area == best_area) { tie_count++; }
    }
    if (tie_count == 0) return -1;
    if (tie_count == 1) return first_best_idx;

    int group = se->slots->sample_slots[first_best_idx].round_robin_group;
    if (group <= 0) return first_best_idx;

    int member_count = 0;
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc) continue;
        if (slot->owner_program != owner_program) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (area == best_area && slot->round_robin_group == group) member_count++;
    }
    if (member_count <= 1) return first_best_idx;

    int group_idx = group;
    if (group_idx < 1) group_idx = 1; else if (group_idx > MAX_ROUND_ROBIN_GROUPS) group_idx = MAX_ROUND_ROBIN_GROUPS;
    unsigned int hit = se->pending->round_robin_hit_count[owner_program][assigned_osc][group_idx]++;
    unsigned int target_ordinal = hit % (unsigned int)member_count;
    unsigned int ordinal = 0;
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc) continue;
        if (slot->owner_program != owner_program) continue;
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
static void start_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr, bool *preroll, int assigned_osc, int owner_program, int note, int velocity) {
    if (*stream_idx >= 0) { release_sample_stream(se, *stream_idx); *stream_idx = -1; }
    int slot_idx = find_matching_sample_slot(se, assigned_osc, owner_program, note, velocity);
    unsigned int start_frame = 0;
    if (slot_idx >= 0) {
        SampleSlot *slot = &se->slots->sample_slots[slot_idx];
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
    *stream_idx = (slot_idx >= 0) ? claim_sample_stream(se, slot_idx, start_frame) : -1;
}

// Called once per active voice per sample from render_audio_block, right after the EG tick loop
// -- if an oscillator's amp envelope has gone idle but it's still holding a claimed sample
// stream, releases it. Idempotent (stream_index stays -1 afterward, so this is a cheap no-op on
// every subsequent call) and cheap enough to run unconditionally.
static void release_idle_sample_streams(HdsSampleEngine *se, SynthVoice *voice) {
    if (voice->vco1_eg.state == STATE_IDLE && voice->vco1_sample_stream >= 0) {
        release_sample_stream(se, voice->vco1_sample_stream);
        voice->vco1_sample_stream = -1;
    }
    if (voice->vco2_eg.state == STATE_IDLE && voice->vco2_sample_stream >= 0) {
        release_sample_stream(se, voice->vco2_sample_stream);
        voice->vco2_sample_stream = -1;
    }
    if (voice->vco3_eg.state == STATE_IDLE && voice->vco3_sample_stream >= 0) {
        release_sample_stream(se, voice->vco3_sample_stream);
        voice->vco3_sample_stream = -1;
    }
    if (voice->vco4_eg.state == STATE_IDLE && voice->vco4_sample_stream >= 0) {
        release_sample_stream(se, voice->vco4_sample_stream);
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
static float render_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr_inout, bool *preroll_inout, float hz, float *out_l_ptr, float *out_r_ptr) {
    if (*stream_idx < 0) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }
    SampleStream *stream = &se->streams->sample_streams[*stream_idx];
    if (!stream->in_use) { *stream_idx = -1; if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; } // reclaimed elsewhere -- defensive, shouldn't happen
    SampleSlot *slot = &se->slots->sample_slots[atomic_load_explicit(&stream->slot_index, memory_order_relaxed)];
    if (!slot->loaded) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }

    float ratio;
    if (slot->unpitched) {
        ratio = 1.0f; // DRUM (Unpitched) -- always plays at its native recorded rate
    } else {
        // Deliberately always standard 12-TET here, never hds_note_to_hz() -- see that function's
        // own comment for why WAVE_SAMPLE's root-note reference is a documented microtuning scope
        // boundary rather than an oversight.
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
                release_sample_stream(se, *stream_idx);
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
static float sample_osc_zone_pan_offset(HdsSampleEngine *se, int stream_idx) {
    if (stream_idx < 0) return 0.0f;
    SampleStream *stream = &se->streams->sample_streams[stream_idx];
    if (!stream->in_use) return 0.0f;
    int slot_idx = atomic_load_explicit(&stream->slot_index, memory_order_relaxed);
    if (slot_idx < 0 || slot_idx >= SAMPLE_SLOT_CAPACITY) return 0.0f;
    SampleSlot *slot = &se->slots->sample_slots[slot_idx];
    return slot->loaded ? slot->pan : 0.0f;
}

// target_program: which of knobs->programs[COMBI_PROGRAM_COUNT] this note belongs to (0 in
// --- Microtuning (Scala .scl import -- see SynthSharedControls.tuning_enable's own comment) ---
//
// Off by default (tuning_enable=0), reproducing today's exact 12-tone equal temperament (440Hz
// A4) everywhere -- see hds_note_to_hz()'s own early-out. Applies to the primary oscillator pitch
// only (handle_note_on/handle_note_off's target_hz below, i.e. what frequency a played MIDI note
// actually sounds at) -- WAVE_SAMPLE playback's own root-note reference frequency
// (render_sample_osc) deliberately stays 12-TET regardless, a documented scope boundary rather
// than an oversight: retuning a sample's own playback-RATE reference would need threading knobs/
// tuning state through the sample-streaming engine's own call chain, for a comparatively obscure
// interaction (how fast a recorded sample plays back relative to its own root note) most players
// retuning their oscillators would never even notice.
//
// A Scala .scl file lists N pitch ratios (each either a fraction like "3/2", a bare integer like
// "2" meaning 2/1, or a cents value like "701.955" -- distinguished by containing a '.') relative
// to an implicit unison (1/1) -- the LAST of the N ratios is the scale's own "period" (almost
// always an octave, 2/1, but Scala doesn't require that). MIDI notes map onto scale degrees
// chromatically from a chosen reference note (tuning_reference_note, default 60 = Middle C) at a
// chosen reference frequency (tuning_reference_freq, default 261.6256Hz -- Middle C's own standard
// 12-TET frequency), wrapping (and multiplying in another period) every N notes.
#define HDS_TUNING_MAX_DEGREES 128
typedef struct {
    bool loaded;
    int degree_count; // N -- pitch lines actually read from the file (NOT counting the implicit 1/1)
    float degree_ratio[HDS_TUNING_MAX_DEGREES]; // degree_ratio[0..N-1]; degree_ratio[N-1] is the period (e.g. 2.0 for an octave-repeating scale)
    char description[128];
} HdsTuningTable;
static HdsTuningTable g_tuning_table;

// Parses one .scl pitch line into a ratio. Returns false (leaving *out_ratio untouched) if the
// line has no parseable number at all -- a blank/malformed line the caller should skip rather
// than insert a garbage degree.
static bool hds_parse_scl_pitch_line(const char *line, float *out_ratio) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '!') return false; // blank or comment
    if (strchr(line, '.')) {
        *out_ratio = powf(2.0f, (float)(atof(line) / 1200.0)); // cents
        return true;
    }
    long numerator = 0, denominator = 1; // denominator's own default covers a bare integer line ("2" means 2/1)
    if (sscanf(line, "%ld / %ld", &numerator, &denominator) >= 1) {
        if (denominator == 0) return false;
        *out_ratio = (float)numerator / (float)denominator;
        return true;
    }
    return false;
}

// Reads a Scala .scl file directly (backend-native, matching --sfz-vcoN/zone_load_sfz's own "OSC
// carries just a path, the backend does the file I/O" shape) -- replaces the currently-loaded
// scale wholesale on success; a malformed/truncated file is rejected with NOTHING changed (same
// "no partial application" posture as hds_song_load_midi).
static bool hds_tuning_load_scl(const char *file_path) {
    FILE *f = fopen(file_path, "r");
    if (!f) return false;
    char line[256];
    char description[256] = {0}; // matches line's own size (only the first 127 chars actually survive into g_tuning_table.description's own smaller buffer at the end)
    int degree_count = -1; // -1 = "count line not seen yet"
    float ratios[HDS_TUNING_MAX_DEGREES];
    int parsed = 0;
    bool have_description = false;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0'; // tolerate CRLF files
        char *trimmed = line; while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '!' || *trimmed == '\0') continue; // comment or blank -- never counts toward description/count/pitch lines
        if (!have_description) {
            snprintf(description, sizeof(description), "%s", trimmed);
            have_description = true;
        } else if (degree_count < 0) {
            degree_count = atoi(trimmed);
            if (degree_count <= 0 || degree_count > HDS_TUNING_MAX_DEGREES) { fclose(f); return false; }
        } else if (parsed < degree_count) {
            float ratio;
            if (!hds_parse_scl_pitch_line(trimmed, &ratio) || ratio <= 0.0f) { fclose(f); return false; }
            ratios[parsed++] = ratio;
        }
    }
    fclose(f);
    if (degree_count < 0 || parsed != degree_count) return false; // truncated -- fewer pitch lines than the count line promised
    g_tuning_table.degree_count = degree_count;
    memcpy(g_tuning_table.degree_ratio, ratios, sizeof(float) * (size_t)degree_count);
    // %.127s (not a bare %s): description's own local buffer is 256 bytes (matching line[]) while
    // g_tuning_table.description is a smaller, deliberately compact 128 -- an explicit precision
    // matching that destination size lets the compiler verify no truncation risk regardless of
    // optimization level, rather than relying on it proving that some other way.
    snprintf(g_tuning_table.description, sizeof(g_tuning_table.description), "%.127s", description);
    g_tuning_table.loaded = true;
    return true;
}

// The one lookup every note-on/note-off pitch calculation goes through -- see this section's own
// header comment for the mapping. Falls back to standard 12-TET (identical to this file's
// original hardcoded formula) whenever tuning is off, nothing has been loaded yet, or the loaded
// table is somehow empty, so a build/session that never touches tuning behaves byte-identically
// to before this feature existed.
static float hds_note_to_hz(const SynthSharedControls *knobs, int note) {
    float standard_hz = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    if (knobs->tuning_enable == 0.0f || !g_tuning_table.loaded || g_tuning_table.degree_count <= 0) return standard_hz;
    int n = g_tuning_table.degree_count;
    int reference_note = (int)knobs->tuning_reference_note;
    int offset = note - reference_note;
    int period_count = (offset >= 0) ? (offset / n) : -(((-offset) + n - 1) / n); // floor division, correct for negative offset too
    int degree = offset - period_count * n; // always in [0, n-1] regardless of offset's sign
    float degree_ratio = (degree == 0) ? 1.0f : g_tuning_table.degree_ratio[degree - 1];
    float period_ratio = g_tuning_table.degree_ratio[n - 1];
    return knobs->tuning_reference_freq * powf(period_ratio, (float)period_count) * degree_ratio;
}

// Single Program/Sequencer modes; 0-7 in Combi mode). Poly-mode voice claiming stays fully
// program-agnostic (searches/steals across the whole shared pool, per the user's explicit "one
// shared budget" requirement) -- only the claimed voice's program_index stamp and which
// ProgramState's fields/EGs/LFO-key-sync get read are program-specific. Mono mode reserves voice
// slot `target_program` itself (0-7) as that Program's own dedicated single voice, so multiple
// simultaneously-mono-mode Combi Programs never collide on voice 0 -- a direct generalization of
// today's hardcoded "mono always uses voice 0", byte-identical when target_program is always 0
// (Single Program/Sequencer modes).
static void handle_note_on(HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], int target_program, int note, int velocity, bool poly_mode_active, MonoNoteStack *mono_stack) {
    const ProgramState *prog = &knobs->programs[target_program];
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
    } else { slot = target_program; }

    float target_hz = hds_note_to_hz(knobs, note);
    voices[slot].program_index = target_program;
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
        if ((WaveType)((int)prog->vco1_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco1_sample_stream, &voices[slot].vco1_sample_ptr, &voices[slot].vco1_sample_preroll, 0, target_program, note, velocity);
        } else if (voices[slot].vco1_sample_stream >= 0) {
            release_sample_stream(se, voices[slot].vco1_sample_stream);
            voices[slot].vco1_sample_stream = -1;
        }
        if ((WaveType)((int)prog->vco2_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco2_sample_stream, &voices[slot].vco2_sample_ptr, &voices[slot].vco2_sample_preroll, 1, target_program, note, velocity);
        } else if (voices[slot].vco2_sample_stream >= 0) {
            release_sample_stream(se, voices[slot].vco2_sample_stream);
            voices[slot].vco2_sample_stream = -1;
        }
        if ((WaveType)((int)prog->vco3_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco3_sample_stream, &voices[slot].vco3_sample_ptr, &voices[slot].vco3_sample_preroll, 2, target_program, note, velocity);
        } else if (voices[slot].vco3_sample_stream >= 0) {
            release_sample_stream(se, voices[slot].vco3_sample_stream);
            voices[slot].vco3_sample_stream = -1;
        }
        if ((WaveType)((int)prog->vco4_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco4_sample_stream, &voices[slot].vco4_sample_ptr, &voices[slot].vco4_sample_preroll, 3, target_program, note, velocity);
        } else if (voices[slot].vco4_sample_stream >= 0) {
            release_sample_stream(se, voices[slot].vco4_sample_stream);
            voices[slot].vco4_sample_stream = -1;
        }
        for(int c=0; c<2; c++) { for(int f=0; f<5; f++) { voices[slot].f_low[c][f][0]=0.0f; voices[slot].f_low[c][f][1]=0.0f; voices[slot].f_band[c][f][0]=0.0f; voices[slot].f_band[c][f][1]=0.0f; } }
        for (int l = 0; l < LFO_COUNT; l++) {
            if (prog->lfo[l].key_sync == 0.0f) continue;
            // Per-Key mode: reset only THIS voice's own phase (every other voice's own per-key
            // cycle is untouched) -- the whole point of Per-Key mode. Global mode: reset the one
            // shared phase every voice of this Program reads, exactly as today.
            if (prog->lfo[l].per_key_mode != 0.0f) {
                voices[slot].lfo_phase[l] = 0.0f;
                voices[slot].lfo_decim_counter[l] = 0;
                voices[slot].lfo_decim_last_value[l] = 0.0f;
                voices[slot].lfo_decim_target_value[l] = 0.0f;
            } else {
                global_lfo_phases[target_program][l] = 0.0f;
            }
        }

        int m0 = (int)prog->eg_mode[0]; voices[slot].vco1_eg.state = (m0 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco1_eg.current_level = (m0 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[0].al / 100.0f));
        int m1 = (int)prog->eg_mode[1]; voices[slot].vco2_eg.state = (m1 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco2_eg.current_level = (m1 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[1].al / 100.0f));
        // Osc 3/4 Amp envelopes: eg[2]/eg_mode[2] and eg[3]/eg_mode[3].
        int mn = (int)prog->eg_mode[2]; voices[slot].vco3_eg.state = (mn == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco3_eg.current_level = (mn == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[2].al / 100.0f));
        int m3 = (int)prog->eg_mode[3]; voices[slot].vco4_eg.state = (m3 == 5) ? STATE_DELAY : STATE_ATTACK;
        voices[slot].vco4_eg.current_level = (m3 == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[3].al / 100.0f));
        for(int e=0; e<ASSIGNABLE_EG_COUNT; e++) {
            int me = (int)prog->eg_mode[4 + e]; voices[slot].assign_eg[e].state = (me == 5) ? STATE_DELAY : STATE_ATTACK;
            voices[slot].assign_eg[e].current_level = (me == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[4 + e].al / 100.0f));
        }
    } else {
        // An overlapping legato note: this is an actual audible glide, so reroll its time.
        voices[slot].glide_time_multiplier = roll_glide_multiplier(prog->mono_glide_random);
    }
    if (!poly_mode_active) mono_stack_push(mono_stack, note, velocity);
}

// Puts a voice's envelopes into RELEASE from wherever they currently are. Factored out since
// it's needed from three places: a normal note-off (poly and mono), and releasing every voice
// that was deferred by the sustain pedal once it lifts (see handle_sustain_release).
static void trigger_voice_release(HdsSampleEngine *se, SynthVoice *voice) {
    voice->vco1_eg.release_level = voice->vco1_eg.current_level; voice->vco1_eg.state = STATE_RELEASE;
    voice->vco2_eg.release_level = voice->vco2_eg.current_level; voice->vco2_eg.state = STATE_RELEASE;
    voice->vco3_eg.release_level = voice->vco3_eg.current_level; voice->vco3_eg.state = STATE_RELEASE;
    voice->vco4_eg.release_level = voice->vco4_eg.current_level; voice->vco4_eg.state = STATE_RELEASE;
    for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) { voice->assign_eg[e].release_level = voice->assign_eg[e].current_level; voice->assign_eg[e].state = STATE_RELEASE; }
    // SFZ loop_sustain support: tell each oscillator's claimed sample stream (if any) that this
    // voice just entered release -- sample_disk_thread_main reads this to decide whether a
    // loop_release_exit zone should keep looping. A no-op for any oscillator not currently
    // playing a WAVE_SAMPLE zone (request_sample_stream_release ignores stream_index -1).
    request_sample_stream_release(se, voice->vco1_sample_stream);
    request_sample_stream_release(se, voice->vco2_sample_stream);
    request_sample_stream_release(se, voice->vco3_sample_stream);
    request_sample_stream_release(se, voice->vco4_sample_stream);
}

static void handle_note_off(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs, const MidiLiveState *midi_live, int target_program, int note, bool poly_mode_active, MonoNoteStack *mono_stack) {
    if (poly_mode_active) {
        int voice_limit = effective_voice_limit(knobs);
        for (int v = 0; v < voice_limit; v++) { if (voices[v].midi_note == note && voices[v].program_index == target_program) {
            // Sustain pedal held: defer the actual release until it lifts (see
            // handle_sustain_release) instead of releasing right now.
            if (midi_live->sustain_held) voices[v].sustain_pending = true;
            else trigger_voice_release(se, &voices[v]);
        } }
        return;
    }

    // Mono mode: this Program's dedicated voice is always voices[target_program] -- see
    // handle_note_on's own comment on why (matches its slot = target_program assignment).
    SynthVoice *mono_voice = &voices[target_program];
    mono_stack_remove(mono_stack, note);
    if (mono_stack->count > 0) {
        // FIXED: another key is still physically held -- fall back to it (the note most
        // recently pressed among those still down) instead of cutting the sound. This is the
        // core mono-mode bug: releasing whichever key came down last used to kill the note
        // even when an earlier key was still held.
        int fallback_note = mono_stack->notes[mono_stack->count - 1];
        float target_hz = hds_note_to_hz(knobs, fallback_note);
        mono_voice->midi_note = fallback_note;
        mono_voice->midi_velocity = mono_stack->velocities[mono_stack->count - 1];
        mono_voice->vco1_target_freq = target_hz;
        mono_voice->vco2_target_freq = target_hz;
        mono_voice->vco3_target_freq = target_hz;
        mono_voice->vco4_target_freq = target_hz;
        // Falling back to a still-held note is also a real audible glide -- reroll its time too.
        mono_voice->glide_time_multiplier = roll_glide_multiplier(knobs->programs[target_program].mono_glide_random);
        // Falling back to a still-held note never retriggers or releases -- the sound never stopped.
    } else if (midi_live->sustain_held) {
        mono_voice->sustain_pending = true;
    } else {
        trigger_voice_release(se, mono_voice);
    }
}

// Called when the sustain pedal transitions held -> released: actually releases every voice
// that had a note-off deferred while it was held.
static void handle_sustain_release(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs) {
    int voice_limit = effective_voice_limit(knobs);
    for (int v = 0; v < voice_limit; v++) {
        if (voices[v].sustain_pending) {
            voices[v].sustain_pending = false;
            trigger_voice_release(se, &voices[v]);
        }
    }
}

// ============================================================================
// Sequencer transport (Play/Pause/Stop) + per-lane MIDI record/playback
// ============================================================================
// A real playhead, decoupled from engine uptime (unlike compute_current_tempo_bpm's own
// engine_elapsed_seconds axis) -- advances only while PLAYING, freezes exactly in place on
// PAUSE, resets to 0 on STOP. Single realtime thread owns all of this (main()'s loop), same as
// every other piece of Sequencer state -- no atomics needed here at all, unlike g_seq_audio's
// pcm/load_state (which cross from a background loader thread).
//
// Scope, stated plainly: this plays recorded MIDI back through the SAME single Program engine
// live MIDI already drives (handle_note_on/handle_note_off, reused verbatim) -- NOT N separate
// per-lane synthesis instances, which don't exist (see the Combi/Sequencer scaffolding plan's own
// "Future Full-Build Notes"). Several lanes in Seq mode play back simultaneously (their recorded
// notes interleaved by time into one instrument, like a classic hardware step-sequencer's single
// sound module), and a lane in Rec mode can record a new take while others play back -- real,
// useful layered-recording behavior, just not independent per-lane instruments.
typedef enum { TRANSPORT_STOPPED = 0, TRANSPORT_PLAYING = 1, TRANSPORT_PAUSED = 2 } TransportState;

// RAM-only, mirrors the audio lanes' own RAM-caching decision -- a short recorded phrase's worth
// of note events is tiny even at this capacity, no disk streaming needed. Events are stored in
// arrival order, so time_sec is monotonically non-decreasing within one lane by construction
// (real-time recording can't record out of order) -- playback just walks forward, no sort needed.
#define SEQ_MIDI_EVENT_CAPACITY 8192
typedef struct {
    float time_sec;     // playhead-relative timestamp when this event was recorded
    unsigned char type;  // 0 = note_on, 1 = note_off
    unsigned char note;
    unsigned char velocity; // unused for note_off, kept for a uniform record shape
} SeqMidiEvent;
typedef struct {
    SeqMidiEvent events[SEQ_MIDI_EVENT_CAPACITY];
    int event_count;
    int playback_cursor; // next event index to fire during Seq-mode playback
} SeqMidiLane;

// ============================================================================
// HdsEngineState -- the last "tier" of the per-synth-instance state consolidation started at
// HdsSampleSlotState (see that struct's own comment for the full explanation of why this is
// split into several separately-declared structs, and for the overall multi-instance-refactor
// rationale). This tier holds the pieces that only became visible this late in the file
// (SeqMidiLane, TransportState) plus render_audio_block's own former function-local statics
// (FXSlotEngine, the LFO mod-state arrays, the limiter) -- everything that didn't need to move
// to an earlier tier.
//
// Deliberately NOT a bounded MAX_HDS_INSTANCES array of these -- exactly one HdsEngineState (and
// one of each earlier tier) is calloc'd/BSS-allocated per synth "session" (the standalone app's
// own single static instance below; one embedded per CLAP plugin instance), matching CLAP's own
// create_plugin/destroy lifecycle with no artificial instance cap.
//
// This phase (multi-instance Phase 2, "pure consolidation") only regroups this state into one
// struct reached through a single new global, g_engine -- every existing global name below is
// kept alive as a macro expanding to the matching g_engine field, so every one of this file's
// existing read/write sites keeps compiling completely unchanged. Later phases replace these
// macros, one subsystem at a time, with a real `engine` parameter threaded explicitly through
// every function that needs it (see /root/.claude/plans/wobbly-marinating-prism.md, "Multi-
// instance CLAP support").
// ============================================================================
typedef struct {
    // --- Sequencer MIDI lanes (the Sequencer audio lanes / sample-streaming / pending-load
    // pieces of this same "per-instance state" concept live on HdsSampleSlotState/
    // HdsStreamsState/HdsPendingLoadState instead, declared earlier in this file -- see
    // HdsSampleSlotState's own comment for why: SeqMidiLane isn't a complete type until this
    // point, well after those subsystems' own functions already needed their globals) ---
    SeqMidiLane   seq_midi[SEQ_LANE_COUNT];

    // --- Transport ---
    TransportState transport_state;
    double        transport_playhead_sec;
    float         transport_last_play_trigger, transport_last_pause_trigger, transport_last_stop_trigger;
    float         transport_last_seek_start_trigger, transport_last_seek_back_trigger, transport_last_seek_fwd_trigger;
    float         transport_last_seek_to_trigger;

    // --- External MIDI Clock sync (see SynthSharedControls.seq_sync_source) ---
    bool   midi_clock_valid;                  // true once enough consecutive ticks have arrived to trust midi_clock_bpm
    float  midi_clock_bpm;                    // derived tempo, smoothed over a handful of ticks
    float  midi_clock_tick_interval_smoothed; // seconds/tick, exponential moving average
    double midi_clock_last_tick_sec;          // hds_monotonic_seconds() timestamp of the last received tick
    int    midi_clock_tick_count;             // consecutive ticks seen since the last Start/long-gap reset

    // --- Metronome ---
    double        metronome_next_click_sec;
    int           metronome_click_samples_left;
    int           metronome_click_phase;

    // --- render_audio_block's former function-local statics (see its own restructuring in
    // multi-instance Phase 5) ---
    float         lfo_phase_mod_state[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    float         lfo_rate_mod_state[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    float         lfo_depth_mod_state[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    int           lfo_decim_counter[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    float         lfo_decim_last_value[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    float         lfo_decim_target_value[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    float         mono_noise_tilt_lp[COMBI_PROGRAM_COUNT + 1][4];
    // Post-filter 3-band EQ biquad state (Direct Form I z^-1/z^-2 delay elements) -- one pair per
    // Program slot per band per channel (0=L, 1=R), mirroring mono_noise_tilt_lp's own per-Program
    // placement just above. DSP STATE, not a knob value, so it lives here on HdsEngineState rather
    // than on ProgramState/SynthSharedControls -- same reasoning as filters[]'s own per-voice
    // f_low/f_band state (SynthVoice), just per-Program instead of per-voice since the EQ runs
    // once per Program per sample, not once per voice (see EqBandParams' own comment).
    float         eq_z1[COMBI_PROGRAM_COUNT + 1][3][2];
    float         eq_z2[COMBI_PROGRAM_COUNT + 1][3][2];
    FXSlotEngine  fx_engine[FX_SLOT_COUNT];
    bool          fx_engine_initialized;
    float         limiter_gain[OUTPUT_PAIR_MAX];
    bool          limiter_gain_initialized;
    float         master_volume_smoothed; // chases knobs->master_volume over a short time
                                            // constant instead of applying it instantly -- see
                                            // its own comment at the point of use for the click
                                            // this fixes.
    bool          master_volume_smoothed_initialized;

    // Arpeggiator: one independent instance per Program slot (0-7 Combi lanes, 8 =
    // SINGLE_PROGRAM_SLOT) -- see poll_arpeggiator()/arp_note_on()/arp_note_off(). Reuses
    // MonoNoteStack (already a plain insertion-ordered, evict-oldest-when-full held-notes list --
    // see mono_stack_push/mono_stack_remove) rather than inventing a near-identical struct.
    MonoNoteStack arp_held_notes[COMBI_PROGRAM_COUNT + 1];
    int           arp_keys_down[COMBI_PROGRAM_COUNT + 1];      // physically-held key count, kept
                                                                 // even in Hold/Latch mode (where
                                                                 // arp_held_notes itself stops
                                                                 // tracking releases) so the NEXT
                                                                 // fresh note-on after a full
                                                                 // release knows to replace the
                                                                 // latched chord instead of adding
                                                                 // to it.
    int           arp_step_samples_left[COMBI_PROGRAM_COUNT + 1]; // sample countdown to the next step
    int           arp_gate_samples_left[COMBI_PROGRAM_COUNT + 1]; // sample countdown to the current step's own note-off
    bool          arp_note_active[COMBI_PROGRAM_COUNT + 1];       // true while a synthesized arp note is currently sounding
    int           arp_playing_note[COMBI_PROGRAM_COUNT + 1];      // only meaningful when arp_note_active is true
    int           arp_step_cursor[COMBI_PROGRAM_COUNT + 1];       // 0..(held_count*octave_range - 1), advances every step
    unsigned int  arp_rng_state[COMBI_PROGRAM_COUNT + 1];         // xorshift32 state for Style=Random; 0 is a degenerate seed, guarded at first use
    float         arp_tempo_smoothed;        // chases compute_current_tempo_bpm()'s live result
                                               // over a short time constant -- see poll_arpeggiator's
                                               // own comment for the glitch this fixes. One shared
                                               // value, not per-Program: compute_current_tempo_bpm()
                                               // itself always resolves to a single "current tempo"
                                               // slot regardless of which Program's arp is asking.
    bool          arp_tempo_smoothed_initialized;
    float         lfo_tempo_smoothed;        // chases compute_current_tempo_bpm()'s live result,
                                               // same shape and reasoning as arp_tempo_smoothed
                                               // just above -- see render_audio_block's own
                                               // comment on it. One shared value, not per-Program,
                                               // for the same reason arp_tempo_smoothed is.
    bool          lfo_tempo_smoothed_initialized;
} HdsEngineState;

// The standalone app's own single instance -- same storage duration (process lifetime) and
// zero-initialization (BSS) as every global/static it replaces, and main() remains the only
// caller in the whole program, reaching every value through this one unchanging pointer instead
// of implicitly. Provably behavior-identical by construction; verified empirically per-phase via
// the determinism-check harness (see the plan doc) rather than trusted on this reasoning alone.
static HdsEngineState g_engine;

// Temporary bridge macros (multi-instance Phase 2 only): every pre-existing g_* name below now
// expands to the matching g_engine field, so this phase is a pure rename/regroup with zero
// control-flow change. Removed incrementally in later phases as each subsystem's functions gain
// a real `engine` parameter (see the plan doc's phase breakdown).
#define g_seq_midi g_engine.seq_midi
#define g_transport_state g_engine.transport_state
#define g_transport_playhead_sec g_engine.transport_playhead_sec
#define g_transport_last_play_trigger g_engine.transport_last_play_trigger
#define g_transport_last_pause_trigger g_engine.transport_last_pause_trigger
#define g_transport_last_stop_trigger g_engine.transport_last_stop_trigger
#define g_transport_last_seek_start_trigger g_engine.transport_last_seek_start_trigger
#define g_transport_last_seek_back_trigger g_engine.transport_last_seek_back_trigger
#define g_transport_last_seek_fwd_trigger g_engine.transport_last_seek_fwd_trigger
#define g_metronome_next_click_sec g_engine.metronome_next_click_sec
#define g_metronome_click_samples_left g_engine.metronome_click_samples_left
#define g_metronome_click_phase g_engine.metronome_click_phase

// Actually starts (or repositions) one Seq-mode audio lane's transport-driven playback: its file
// position is audio_offset_sec (the lane's own knob, "start reading the file from here") PLUS
// the transport's own current playhead (g_transport_playhead_sec) -- so a lane repositions
// correctly whether this is a fresh Play from Stop (playhead is 0 there), a late-loading lane
// catching up mid-song (poll_seq_audio_transport_catchup), or a Seek (transport_reposition_lanes,
// playhead already updated by the caller before this runs). Shared by all three callers. Caller
// must have already confirmed load_state == SEQ_AUDIO_READY.
static void seq_audio_start_transport_playback(HdsEngineState *engine, SeqAudioLane *al, float audio_offset_sec) {
    long start_frame = 0;
    float target_sec = audio_offset_sec + (float)engine->transport_playhead_sec;
    if (target_sec > 0.0f && al->sample_rate > 0) {
        start_frame = (long)(target_sec * (float)al->sample_rate);
        if (start_frame >= al->frame_count) start_frame = al->frame_count - 1;
    }
    al->play_frame = start_frame;
    al->is_playing = true;
    al->driven_by_transport = true;
    al->armed_for_transport = false;
}

// Repositions every playback-eligible lane's own playback state (Audio lanes' play_frame, MIDI
// lanes' playback_cursor) to match wherever g_transport_playhead_sec CURRENTLY is -- shared by
// transport_start_fresh (a fresh Play from Stop, playhead already reset to the target by the
// caller) and the Seek triggers below (Reset/Back/Forward, playhead already moved by the caller
// before this runs). Off lanes and Rec-armed lanes are both deliberately left untouched -- an Off
// lane does nothing at all (see SeqLaneState's own precedence comment), and a Rec-armed lane has
// no "position" to seek to (repositioning would either discard an in-progress take or make no
// sense for one that hasn't started), so a seek simply doesn't affect either.
static void transport_reposition_lanes(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        if ((int)knobs->seq_lanes[l].seq_mode == 1) continue; // Off -- a real bypass, the transport never touches this lane
        bool is_audio_lane = (knobs->seq_lanes[l].lane_type != 0.0f);
        if (is_audio_lane) {
            // Audio lanes never record -- rec_armed is meaningless for them.
            SeqAudioLane *al = &streams->seq_audio[l];
            if (atomic_load_explicit(&al->load_state, memory_order_acquire) != SEQ_AUDIO_READY || !al->pcm || al->frame_count <= 0) {
                // Not ready yet -- arm it so poll_seq_audio_transport_catchup starts it at the
                // right (already-updated) position the moment it becomes ready.
                al->armed_for_transport = true;
                continue;
            }
            seq_audio_start_transport_playback(engine, al, knobs->seq_lanes[l].audio_offset_sec);
        } else {
            if (knobs->seq_lanes[l].rec_armed != 0.0f) continue; // Rec-armed lanes have no playback position to reposition
            SeqMidiLane *ml = &engine->seq_midi[l];
            // Events are stored in non-decreasing time order (real-time recording can't record
            // out of order -- see SeqMidiLane's own comment), so the new cursor is just "how many
            // events have a timestamp at or before the new playhead," found by a plain forward
            // scan from 0. Cheap even for a full 8192-event lane; only runs on an actual seek
            // (button click), never per-block.
            int cursor = 0;
            while (cursor < ml->event_count && ml->events[cursor].time_sec <= (float)engine->transport_playhead_sec) cursor++;
            ml->playback_cursor = cursor;
        }
    }
}

// Called once, only on a STOPPED -> PLAYING transition (never on PAUSED -> PLAYING/resume, which
// must leave every lane's position exactly where it was): repositions every playback-eligible
// lane to match the transport's current playhead (transport_reposition_lanes -- ordinarily 0,
// unless the transport was seeked while Stopped, in which case Play correctly starts from there
// instead of silently snapping back to the top). A lane armed to Rec starts a genuinely fresh
// take (clears its previously recorded events) UNLESS rec_overdub is set, in which case its
// existing events are deliberately left in place -- newly recorded notes get merged into them in
// time order instead (see seq_record_midi_event).
static void transport_start_fresh(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        if (knobs->seq_lanes[l].lane_type != 0.0f) continue; // Audio lanes never record
        if ((int)knobs->seq_lanes[l].seq_mode == 1) continue; // Off -- untouched
        if (knobs->seq_lanes[l].rec_armed == 0.0f) continue; // not armed to Rec
        if (knobs->seq_lanes[l].rec_overdub != 0.0f) continue; // Overdub -- keep existing events
        engine->seq_midi[l].event_count = 0; // Overwrite (default) -- fresh take
    }
    transport_reposition_lanes(streams, engine, knobs);
}

// --- External MIDI Clock sync (see SynthSharedControls.seq_sync_source) ---
//
// MIDI Clock (0xF8) ticks 24 times per quarter note, with no tempo field of its own -- the
// receiving device is expected to derive BPM purely from how far apart consecutive ticks land in
// real time. Measured here via CLOCK_MONOTONIC wall-clock timestamps at the point each tick is
// polled (this engine's MIDI input is polled once per audio block, not on a dedicated MIDI
// thread, so tick timing carries up to one block's worth of jitter -- a few milliseconds at
// typical block sizes, negligible for tempo derivation and the same polling-granularity tradeoff
// every other block-polled input here already accepts).
#define HDS_MIDI_CLOCK_TICKS_PER_QUARTER 24
#define HDS_MIDI_CLOCK_TIMEOUT_SEC 2.0 // no tick for this long -> treat the external clock as gone

static double hds_monotonic_seconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Hooked directly into process_midi_input's SND_SEQ_EVENT_CLOCK case -- runs unconditionally
// regardless of seq_sync_source (cheap bookkeeping either way), so switching sync source live
// doesn't need to "catch up" on tempo -- it's already tracked in the background.
static void hds_handle_midi_clock_tick(HdsEngineState *engine) {
    double now = hds_monotonic_seconds();
    if (engine->midi_clock_tick_count > 0) {
        double delta = now - engine->midi_clock_last_tick_sec;
        if (delta > 0.0 && delta < HDS_MIDI_CLOCK_TIMEOUT_SEC) {
            if (!engine->midi_clock_valid) {
                engine->midi_clock_tick_interval_smoothed = (float)delta; // first real sample -- nothing to smooth against yet
            } else {
                // ~5-tick time constant: fast enough to track a live tempo ramp (a performer
                // riding a hardware sequencer's tempo knob) without jittering the derived BPM on
                // every single tick's own small timing noise.
                float smoothing = 0.2f;
                engine->midi_clock_tick_interval_smoothed += ((float)delta - engine->midi_clock_tick_interval_smoothed) * smoothing;
            }
            engine->midi_clock_bpm = 60.0f / (engine->midi_clock_tick_interval_smoothed * (float)HDS_MIDI_CLOCK_TICKS_PER_QUARTER);
            if (engine->midi_clock_tick_count >= 4) engine->midi_clock_valid = true; // a few ticks in before trusting it
        } else {
            engine->midi_clock_tick_count = 0; // long gap (e.g. was stopped) -- restart clean rather than poisoning the average with one huge interval
        }
    }
    engine->midi_clock_last_tick_sec = now;
    engine->midi_clock_tick_count++;
}

// MIDI Start (0xFA): always begins from the top, exactly like the GUI's own Stop->Play transition
// -- see transport_start_fresh()'s own comment for what "fresh" means for a Rec-armed lane.
static void hds_handle_midi_transport_start(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
    engine->transport_playhead_sec = 0.0;
    engine->transport_state = TRANSPORT_PLAYING;
    transport_start_fresh(streams, engine, knobs);
}

// MIDI Continue (0xFB): resumes from exactly wherever the transport currently sits -- no
// repositioning, same as the GUI's own Paused->Playing transition.
static void hds_handle_midi_transport_continue(HdsEngineState *engine) {
    engine->transport_state = TRANSPORT_PLAYING;
}

// MIDI Stop (0xFC): PAUSES rather than resets to zero -- matches General MIDI/MIDI Machine
// Control's own Stop/Continue semantics (a subsequent Continue must resume from here), the same
// distinction the GUI's own separate Pause and Stop buttons already make. A performer who wants
// to rewind to the top sends Start, not Stop, exactly as most DAWs already behave.
static void hds_handle_midi_transport_stop(HdsEngineState *engine) {
    if (engine->transport_state == TRANSPORT_PLAYING) engine->transport_state = TRANSPORT_PAUSED;
}

// --- Metronome state (declared here, ahead of poll_transport_triggers below, so a Seek can
// resync it -- see the `seeked` block's own comment further down). A cheap, deliberately
// hardcoded 400Hz/8ms square-wave click on each beat while PLAYING -- not a Program voice, so it
// never touches the oscillator/filter/envelope machinery (per the ask: "low quality, efficient").
// Scheduled at block granularity (matching how the transport playhead itself only updates once
// per block -- see advance_transport_playhead), which is more than adequate for a monitoring
// click. ---
#define METRONOME_CLICK_HZ 400.0f
#define METRONOME_CLICK_DURATION_SEC 0.008f
#define METRONOME_CLICK_GAIN 0.25f

// Edge-triggered, same idiom as poll_seq_audio_preview_triggers: each button increments its own
// counter on every click and resends the whole packet, so a genuine change (not just "button
// held") is what starts/pauses/stops/seeks the transport -- immune to a stale resend
// re-triggering anything. Call once per block from main()'s loop. current_tempo_bpm is this
// block's already-resolved tempo (see the Seek Back/Forward triggers' own comment on why "right
// now" rather than "at the seek target").
static void poll_transport_triggers(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm) {
    if (knobs->seq_transport_play_trigger != engine->transport_last_play_trigger) {
        engine->transport_last_play_trigger = knobs->seq_transport_play_trigger;
        if (engine->transport_state != TRANSPORT_PLAYING) {
            bool was_stopped = (engine->transport_state == TRANSPORT_STOPPED);
            engine->transport_state = TRANSPORT_PLAYING;
            if (was_stopped) transport_start_fresh(streams, engine, knobs);
            // PAUSED -> PLAYING: deliberately does nothing else -- every lane's play_frame/
            // playback_cursor already holds exactly where it was paused.
        }
    }
    if (knobs->seq_transport_pause_trigger != engine->transport_last_pause_trigger) {
        engine->transport_last_pause_trigger = knobs->seq_transport_pause_trigger;
        if (engine->transport_state == TRANSPORT_PLAYING) engine->transport_state = TRANSPORT_PAUSED;
    }
    if (knobs->seq_transport_stop_trigger != engine->transport_last_stop_trigger) {
        engine->transport_last_stop_trigger = knobs->seq_transport_stop_trigger;
        engine->transport_state = TRANSPORT_STOPPED;
        engine->transport_playhead_sec = 0.0;
        for (int l = 0; l < SEQ_LANE_COUNT; l++) {
            // Only stop lanes the TRANSPORT itself started -- a manually Preview-triggered lane
            // keeps playing through Stop exactly as it already does today (Preview is fully
            // independent of transport state, per its own existing contract).
            if (streams->seq_audio[l].driven_by_transport) {
                streams->seq_audio[l].is_playing = false;
                streams->seq_audio[l].driven_by_transport = false;
            }
            streams->seq_audio[l].armed_for_transport = false;
            engine->seq_midi[l].playback_cursor = 0;
        }
    }

    // Seek: "very beginning" reset, and back/forward by one whole measure. Works in every
    // transport state (Stopped/Paused/Playing) -- none of these three requires PLAYING first,
    // unlike Play/Pause/Stop's own state-transition guards above. A measure is 4 beats (this
    // engine has no time-signature concept yet, so 4/4 is assumed -- matching the Sequencer tab's
    // own bars:beats readout, which already makes the same assumption) at current_tempo_bpm;
    // clamped to a sane floor so a pathologically low/zero tempo can't produce an infinite or
    // negative-duration measure.
    float measure_sec = 4.0f * 60.0f / fmaxf(1.0f, current_tempo_bpm);
    bool seeked = false;
    if (knobs->seq_transport_seek_start_trigger != engine->transport_last_seek_start_trigger) {
        engine->transport_last_seek_start_trigger = knobs->seq_transport_seek_start_trigger;
        engine->transport_playhead_sec = 0.0;
        seeked = true;
    }
    if (knobs->seq_transport_seek_back_trigger != engine->transport_last_seek_back_trigger) {
        engine->transport_last_seek_back_trigger = knobs->seq_transport_seek_back_trigger;
        engine->transport_playhead_sec = fmax(0.0, engine->transport_playhead_sec - (double)measure_sec);
        seeked = true;
    }
    if (knobs->seq_transport_seek_fwd_trigger != engine->transport_last_seek_fwd_trigger) {
        engine->transport_last_seek_fwd_trigger = knobs->seq_transport_seek_fwd_trigger;
        engine->transport_playhead_sec += (double)measure_sec;
        seeked = true;
    }
    // Seek-to-arbitrary-position ("jump to marker" -- see SynthSharedControls' own comment):
    // jumps straight to the requested absolute position rather than a relative one-measure step.
    if (knobs->seq_transport_seek_to_trigger != engine->transport_last_seek_to_trigger) {
        engine->transport_last_seek_to_trigger = knobs->seq_transport_seek_to_trigger;
        engine->transport_playhead_sec = fmax(0.0, (double)knobs->seq_transport_seek_to_target_sec);
        seeked = true;
    }
    if (seeked) {
        transport_reposition_lanes(streams, engine, knobs);
        // Resync the metronome's own schedule to the NEW playhead position -- without this, a
        // seek backward (e.g. "to beginning") left it pinned to wherever it was scheduled BEFORE
        // the jump, now far ahead of the playhead, so it would stay silent until the playhead
        // caught back up to that stale point (looking "broken"); a seek forward past one or more
        // beat boundaries would instead have poll_metronome's own while-loop fire a whole backlog
        // of missed clicks at once on the very next block. Same "pin to right where the playhead
        // currently is" fix poll_metronome already applies when disabled/not-PLAYING (see its own
        // comment) -- a seek is exactly the same kind of playhead discontinuity.
        engine->metronome_next_click_sec = engine->transport_playhead_sec;
        engine->metronome_click_samples_left = 0;
    }
}

// Called once per block, right after poll_transport_triggers (so it sees this block's already-
// resolved tempo/playhead). Schedules (but does not itself render) upcoming clicks: crossing one
// or more beat boundaries this block arms/re-arms the click-sample countdown that
// render_audio_block's per-sample loop actually renders from.
static void poll_metronome(HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm) {
    if (knobs->metronome_enable == 0.0f || engine->transport_state != TRANSPORT_PLAYING) {
        // Not engaged, or transport isn't running -- keep the next-click schedule pinned to
        // "right after wherever the playhead currently is" so re-enabling or resuming Play
        // starts cleanly on the very next beat instead of firing a backlog of missed clicks.
        engine->metronome_next_click_sec = engine->transport_playhead_sec;
        engine->metronome_click_samples_left = 0;
        return;
    }
    float beat_sec = 60.0f / fmaxf(1.0f, current_tempo_bpm);
    while (engine->transport_playhead_sec >= engine->metronome_next_click_sec) {
        engine->metronome_next_click_sec += (double)beat_sec;
        engine->metronome_click_samples_left = (int)(METRONOME_CLICK_DURATION_SEC * (float)SAMPLE_RATE);
        engine->metronome_click_phase = 0;
    }
}

// Called every block from main()'s loop (see seq_audio_start_transport_playback's own comment
// on why this exists): starts any lane transport_start_fresh had to skip because its load hadn't
// finished yet, the moment SEQ_AUDIO_READY actually lands, as long as the transport is still
// PLAYING when that happens. A cheap, unconditional scan -- armed_for_transport is false for
// every lane in the overwhelmingly common case (load already finished before Play was pressed),
// so this is a no-op most of the time.
static void poll_seq_audio_transport_catchup(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
    if (engine->transport_state != TRANSPORT_PLAYING) return;
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        SeqAudioLane *al = &streams->seq_audio[l];
        if (!al->armed_for_transport) continue;
        if (atomic_load_explicit(&al->load_state, memory_order_acquire) != SEQ_AUDIO_READY || !al->pcm || al->frame_count <= 0) continue;
        seq_audio_start_transport_playback(engine, al, knobs->seq_lanes[l].audio_offset_sec);
    }
}

// Advances the real playhead by exactly one block's worth of time, only while PLAYING. Call once
// per block from main()'s loop, after block_size is known for this iteration.
static void advance_transport_playhead(HdsEngineState *engine, int block_size) {
    if (engine->transport_state == TRANSPORT_PLAYING) {
        engine->transport_playhead_sec += (double)block_size / (double)SAMPLE_RATE;
    }
}

// Hooked directly into process_midi_input's existing NOTEON/NOTEOFF dispatch (right after the
// live handle_note_on/handle_note_off call, never instead of it -- recording must never affect
// what's actually heard live, and is never itself filtered by channel -- only which Rec-armed
// lane(s) a note gets recorded INTO is). A no-op unless the transport is genuinely PLAYING and at
// least one lane is armed to Rec, so an idle Sequencer costs nothing extra on the live MIDI path.
// midi_channel_1_16 is the incoming event's own MIDI channel, already converted from ALSA's
// 0-indexed data.note.channel to this file's 1-16 convention (matching SeqLaneState.midi_channel)
// by the caller.
static void seq_record_midi_event(HdsEngineState *engine, const SynthSharedControls *knobs, unsigned char type, unsigned char note, unsigned char velocity, int midi_channel_1_16, float current_tempo_bpm) {
    if (engine->transport_state != TRANSPORT_PLAYING) return;
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        if (knobs->seq_lanes[l].lane_type != 0.0f) continue; // Audio lanes don't record MIDI
        if ((int)knobs->seq_lanes[l].seq_mode == 1) continue; // Off -- a real bypass, see SeqLaneState's own precedence comment
        if (knobs->seq_lanes[l].rec_armed == 0.0f) continue; // not armed to Rec
        float lane_channel = knobs->seq_lanes[l].midi_channel;
        if (lane_channel != 0.0f && (int)lane_channel != midi_channel_1_16) continue; // 0 = Omni, else must match exactly
        SeqMidiLane *ml = &engine->seq_midi[l];
        if (ml->event_count >= SEQ_MIDI_EVENT_CAPACITY) continue; // full -- silently drop further events past this documented cap
        // Insert in time order rather than a blind append: Overdub mode deliberately leaves this
        // lane's previously-recorded events in place (see transport_start_fresh), so a new note's
        // own timestamp can land anywhere relative to them, not just at the end. (This also
        // quietly closes a latent Overwrite-mode edge case: seeking the transport backward
        // mid-take already made engine->transport_playhead_sec non-monotonic relative to a blind
        // append, which would have stored that event out of order.)
        float t = (float)engine->transport_playhead_sec;
        // Quantize: note-ON only (see SynthSharedControls.seq_quantize_enable's own comment for
        // why note-OFF is deliberately left untouched), snapped to the nearest grid line at
        // whichever divisions-per-beat the Sequencer page's Resolution control is set to.
        if (type == 0 && knobs->seq_quantize_enable != 0.0f) {
            float beat_sec = 60.0f / fmaxf(1.0f, current_tempo_bpm);
            float grid_sec = beat_sec / fmaxf(1.0f, knobs->seq_quantize_resolution);
            t = roundf(t / grid_sec) * grid_sec;
        }
        int idx = ml->event_count;
        while (idx > 0 && ml->events[idx - 1].time_sec > t) idx--;
        for (int j = ml->event_count; j > idx; j--) ml->events[j] = ml->events[j - 1];
        ml->events[idx] = (SeqMidiEvent){ t, type, note, velocity };
        ml->event_count++;
    }
}

// Called once per block, right after process_midi_input -- walks every Seq-mode Program/MIDI
// lane's recorded events and fires any whose timestamp has now been reached, via the exact same
// handle_note_on/handle_note_off functions live MIDI already calls (see this section's own scope
// comment for why that's a deliberate choice, not a shortcut).
static void seq_playback_midi_events(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack *mono_stack, const MidiLiveState *midi_live) {
    if (engine->transport_state != TRANSPORT_PLAYING) return;
    // Sequencer's recorded MIDI always targets Single Program mode's own dedicated ProgramState
    // (SINGLE_PROGRAM_SLOT, not any Combi lane -- see this section's scope comment and
    // ProgramState's own comment on the mutual-exclusivity fix) -- so its own poly/mono mode is
    // always that slot's is_polyphonic, computed once here.
    bool poly_mode_active = (knobs->programs[SINGLE_PROGRAM_SLOT].is_polyphonic != 0.0f);
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        if (knobs->seq_lanes[l].lane_type != 0.0f) continue;
        if ((int)knobs->seq_lanes[l].seq_mode == 1) continue; // Off -- a real bypass, see SeqLaneState's own precedence comment
        if (knobs->seq_lanes[l].rec_armed != 0.0f) continue; // Rec-armed lanes don't play back at all
        bool muted = (knobs->seq_lanes[l].mute != 0.0f);
        SeqMidiLane *ml = &engine->seq_midi[l];
        while (ml->playback_cursor < ml->event_count && ml->events[ml->playback_cursor].time_sec <= (float)engine->transport_playhead_sec) {
            SeqMidiEvent *e = &ml->events[ml->playback_cursor];
            // Mute still walks the cursor forward in step with the playhead -- just without ever
            // calling handle_note_on/off -- so un-muting resumes wherever the playhead currently
            // is instead of bursting out every note that was skipped while muted. Same "stay in
            // sync, just silence the output" shape as the audio-lane Mute fix right above this
            // function's own mix-loop call site.
            if (!muted) {
                // Sequencer's own recorded MIDI always targets SINGLE_PROGRAM_SLOT -- see this
                // section's scope comment (a separate, parallel concern from Combi's own live
                // dispatch) and ProgramState's own comment on why this is no longer Program 0.
                if (e->type == 0) handle_note_on(se, voices, knobs, global_lfo_phases, SINGLE_PROGRAM_SLOT, e->note, e->velocity, poly_mode_active, mono_stack);
                else handle_note_off(se, voices, knobs, midi_live, SINGLE_PROGRAM_SLOT, e->note, poly_mode_active, mono_stack);
            }
            ml->playback_cursor++;
        }
    }
}

// --- Song save/load: recorded Sequencer MIDI data (SeqMidiLane.events[]) ---
//
// Everything else a "song" needs (each lane's own type/volume/pan/midi_channel/mute/rec state,
// and an Audio lane's own audio_path/offset) already lives on SynthSharedControls, addressed by
// the existing /hds/seq/%d/... OSC entries -- the frontend saves/restores all of that itself the
// exact same way it already saves/restores a Program's fields, no new backend involvement needed.
// The ONE piece that can't work that way is a Rec-armed lane's actual recorded performance
// (SeqMidiLane.events[], up to SEQ_MIDI_EVENT_CAPACITY=8192 per lane): pure runtime state, never
// exposed as an OSC-addressable knob at all (there's no single "current value" to read/write one
// at a time the way a fader position is), and too large to safely shuttle over a UDP OSC bundle in
// the worst case (32 fully-packed lanes would be several times over a single UDP datagram's
// realistic safe size). Backend-native file I/O instead -- the same "OSC carries just a path
// string, the backend does the actual file work" shape as --sfz-vcoN/zone_load_sfz_handler's SFZ
// loading, applied here to this engine's own small binary dump instead of a third-party format.
//
// REALTIME NOTE: unlike WAV recording/sample streaming (continuous for a take's whole duration,
// so routed through a dedicated writer/loader thread specifically to keep file I/O off the audio
// thread), this runs synchronously on a Save/Load button click -- a rare, discrete, user-initiated
// action, not something that happens while notes are actively being processed. A worst-case
// fully-packed take (32 lanes x 8192 events, 8 bytes/event) is still only ~2MB, a few
// milliseconds of disk I/O even on a slow card -- an explicitly accepted tradeoff against building
// a whole second dedicated-thread pipeline for something this infrequent, not an oversight.
#define HDS_SONG_MIDI_MAGIC 0x48445351u // "HDSQ" as a little-endian u32
#define HDS_SONG_MIDI_VERSION 1u

static void hds_song_write_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static uint32_t hds_song_read_u32(FILE *f) {
    uint8_t b[4] = {0};
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void hds_song_write_f32(FILE *f, float v) {
    uint32_t bits; memcpy(&bits, &v, sizeof(bits));
    hds_song_write_u32(f, bits);
}
static float hds_song_read_f32(FILE *f) {
    uint32_t bits = hds_song_read_u32(f);
    float v; memcpy(&v, &bits, sizeof(v));
    return v;
}

// Writes every lane's recorded MIDI events to file_path (created/overwritten). Format: magic (u32)
// + version (u32) + lane count (u32), then per lane: event_count (u32) followed by that many
// (time_sec:f32, type:u8, note:u8, velocity:u8) records -- explicit byte-level fields throughout
// (not a raw struct dump), so the file is portable across this project's own x86_64/aarch64
// targets regardless of either platform's struct padding/alignment rules.
static bool hds_song_save_midi(const HdsEngineState *engine, const char *file_path) {
    FILE *f = fopen(file_path, "wb");
    if (!f) return false;
    hds_song_write_u32(f, HDS_SONG_MIDI_MAGIC);
    hds_song_write_u32(f, HDS_SONG_MIDI_VERSION);
    hds_song_write_u32(f, (uint32_t)SEQ_LANE_COUNT);
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        const SeqMidiLane *ml = &engine->seq_midi[l];
        hds_song_write_u32(f, (uint32_t)ml->event_count);
        for (int e = 0; e < ml->event_count; e++) {
            hds_song_write_f32(f, ml->events[e].time_sec);
            uint8_t rec[3] = { ml->events[e].type, ml->events[e].note, ml->events[e].velocity };
            fwrite(rec, 1, 3, f);
        }
    }
    bool ok = (ferror(f) == 0);
    fclose(f);
    return ok;
}

// Reads back a file written by hds_song_save_midi(), replacing every lane's current recorded
// events entirely (a real Load, not a merge) and resetting playback_cursor to 0 so Play starts
// from the top of whatever was just loaded. A truncated/corrupt/version-mismatched file is
// rejected wholesale, with NOTHING applied -- silently applying half a load would leave some
// lanes holding a stale previous take and others the new one, worse than just refusing outright.
static bool hds_song_load_midi(HdsEngineState *engine, const char *file_path) {
    FILE *f = fopen(file_path, "rb");
    if (!f) return false;
    uint32_t magic = hds_song_read_u32(f);
    uint32_t version = hds_song_read_u32(f);
    uint32_t lane_count = hds_song_read_u32(f);
    if (magic != HDS_SONG_MIDI_MAGIC || version != HDS_SONG_MIDI_VERSION || lane_count != (uint32_t)SEQ_LANE_COUNT || ferror(f)) {
        fclose(f);
        return false;
    }
    // Too large for main()'s stack (~2MB worst case, same reasoning as capture_buffer/RecordRing
    // elsewhere in this file) -- static is safe here since this is only ever reached synchronously
    // from the single OSC-dispatch thread, one call at a time (see this section's own REALTIME
    // NOTE), never concurrently.
    static SeqMidiLane loaded[SEQ_LANE_COUNT];
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        uint32_t count = hds_song_read_u32(f);
        if (count > (uint32_t)SEQ_MIDI_EVENT_CAPACITY || ferror(f)) { fclose(f); return false; }
        loaded[l].event_count = (int)count;
        loaded[l].playback_cursor = 0;
        for (uint32_t e = 0; e < count; e++) {
            float t = hds_song_read_f32(f);
            uint8_t rec[3] = {0};
            if (fread(rec, 1, 3, f) != 3) { fclose(f); return false; }
            loaded[l].events[e] = (SeqMidiEvent){ t, rec[0], rec[1], rec[2] };
        }
    }
    fclose(f);
    // Only commit into the real engine state once the whole file has parsed cleanly -- see this
    // function's own "nothing applied on failure" comment.
    for (int l = 0; l < SEQ_LANE_COUNT; l++) engine->seq_midi[l] = loaded[l];
    return true;
}

static int song_save_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)msg;
    HdsEngineState *engine = (HdsEngineState *)user_data;
    if (argc != 1) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    if (!hds_song_save_midi(engine, file_path)) {
        fprintf(stderr, "[song] failed to save MIDI data to '%s'\n", file_path);
    }
    return 0;
}

static int song_load_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)msg;
    HdsEngineState *engine = (HdsEngineState *)user_data;
    if (argc != 1) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    if (!hds_song_load_midi(engine, file_path)) {
        fprintf(stderr, "[song] failed to load MIDI data from '%s' (missing, unreadable, or wrong format/version)\n", file_path);
    }
    return 0;
}

// --- Standard MIDI File (.mid) import: a REAL SMF Type 0/1 reader (e.g. for a file exported from
// a DAW like Reaper) -- distinct from hds_song_save_midi/load_midi just above, which is this
// engine's OWN small internal round-trip format, not a real SMF parser. Populates Sequencer
// lanes from each note-carrying track, and always reports the file's own text markers (0xFF 0x06)
// back to the caller for the Sequencer tab's Markers list ("jump to marker") regardless of
// whether the caller also asked to import the file's tempo map.
//
// Ticks are shared across every track in one SMF file (each track advances the same underlying
// clock via its own independent delta-times), and Set Tempo (0xFF 0x51)/Marker (0xFF 0x06)
// meta-events conventionally live on a Type 1 file's own Track 0 -- but this scans EVERY track
// for them rather than assuming that, so a Type 0 file (tempo, markers, and notes all
// interleaved on one single track) is handled by the exact same code path with no special case.
#define HDS_SMF_MARKER_MAX 64
#define HDS_SMF_MARKER_LABEL_LEN 64
#define HDS_SMF_TEMPO_EVENT_MAX 256
// Generous ceiling on a plausible plain MIDI file -- rejects anything wildly oversized outright
// rather than fread()ing an arbitrarily large file into memory (a real SMF export, even a long,
// busy multi-track song, is almost always well under 1MB).
#define HDS_SMF_FILE_SIZE_MAX (16 * 1024 * 1024)

typedef struct { long tick; uint32_t usec_per_qn; } HdsSmfTempoEvent;
typedef struct { long tick; char label[HDS_SMF_MARKER_LABEL_LEN]; } HdsSmfMarkerEvent;
typedef struct { long tick; unsigned char type; unsigned char note; unsigned char velocity; } HdsSmfNoteEvent; // type: 0 = note_on, 1 = note_off -- pre-conversion twin of SeqMidiEvent, tick instead of time_sec

typedef struct {
    bool ok;
    char error[128]; // empty on success
    int lanes_used;
    bool tempo_applied;
    int marker_count;
    float marker_time[HDS_SMF_MARKER_MAX];
    char marker_label[HDS_SMF_MARKER_MAX][HDS_SMF_MARKER_LABEL_LEN];
} HdsSmfImportResult;

static bool hds_smf_read_u16be(const uint8_t *buf, size_t len, size_t *pos, uint16_t *out) {
    if (*pos + 2 > len) return false;
    *out = (uint16_t)(((uint16_t)buf[*pos] << 8) | buf[*pos + 1]);
    *pos += 2;
    return true;
}
static bool hds_smf_read_u32be(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    if (*pos + 4 > len) return false;
    *out = ((uint32_t)buf[*pos] << 24) | ((uint32_t)buf[*pos + 1] << 16) | ((uint32_t)buf[*pos + 2] << 8) | (uint32_t)buf[*pos + 3];
    *pos += 4;
    return true;
}
// A Variable-Length Quantity: 7 payload bits per byte, MSB set on every byte but the last. Used
// for both delta-times and meta-event lengths. Real SMF values never need more than 4 continuation
// bytes (28 bits) -- more than that is corrupt input, not a value this format can express.
static bool hds_smf_read_vlq(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= len) return false;
        uint8_t b = buf[*pos]; (*pos)++;
        value = (value << 7) | (uint32_t)(b & 0x7F);
        if (!(b & 0x80)) { *out = value; return true; }
    }
    return false;
}

// Parses one MTrk chunk's raw event bytes (delta-time + event, repeated until the chunk ends).
// Returns false only for genuine STRUCTURAL corruption (a truncated VLQ, or an event that runs
// off the end of the chunk) -- "no partial application" all the way up to hds_song_import_smf's
// own caller, same posture as hds_song_load_midi/hds_tuning_load_scl elsewhere in this file. A
// recognized-but-full capacity (more tempo/marker/note events than this engine reserves room for)
// is NOT such a failure -- it's silently capped, with a diagnostic, exactly like every other
// fixed-capacity import here (see hds_tuning_load_scl's HDS_TUNING_MAX_DEGREES, or
// seq_record_midi_event's own SEQ_MIDI_EVENT_CAPACITY cap). note_cap == 0 means "don't collect
// notes for this track at all" (this file already has SEQ_LANE_COUNT note tracks) -- silent,
// since the caller already reports that once per file, not once per dropped note.
static bool hds_smf_parse_track(const uint8_t *data, size_t len,
                                 HdsSmfTempoEvent *tempo_events, int *tempo_count, int tempo_cap,
                                 HdsSmfMarkerEvent *markers, int *marker_count, int marker_cap,
                                 HdsSmfNoteEvent *notes, int *note_count, int note_cap) {
    size_t pos = 0;
    long abs_tick = 0;
    uint8_t running_status = 0;
    while (pos < len) {
        uint32_t delta;
        if (!hds_smf_read_vlq(data, len, &pos, &delta)) return false;
        abs_tick += (long)delta;
        if (pos >= len) return false; // a delta-time with no event following it -- truncated
        uint8_t status_byte = data[pos];
        if (status_byte == 0xFF) {
            pos++;
            if (pos >= len) return false;
            uint8_t meta_type = data[pos]; pos++;
            uint32_t meta_len;
            if (!hds_smf_read_vlq(data, len, &pos, &meta_len)) return false;
            if (meta_len > len - pos) return false;
            if (meta_type == 0x51) { // Set Tempo: always exactly 3 bytes, microseconds per quarter note, big-endian
                if (meta_len != 3) return false;
                uint32_t usec = ((uint32_t)data[pos] << 16) | ((uint32_t)data[pos + 1] << 8) | (uint32_t)data[pos + 2];
                if (*tempo_count < tempo_cap) { tempo_events[*tempo_count] = (HdsSmfTempoEvent){ abs_tick, usec }; (*tempo_count)++; }
                else fprintf(stderr, "[smf] more than %d tempo changes in this file -- dropping the rest\n", tempo_cap);
            } else if (meta_type == 0x06) { // Marker: an ASCII label, exactly meta_len bytes, not NUL-terminated in the file
                if (*marker_count < marker_cap) {
                    size_t copy_len = meta_len < (uint32_t)(HDS_SMF_MARKER_LABEL_LEN - 1) ? meta_len : (uint32_t)(HDS_SMF_MARKER_LABEL_LEN - 1);
                    HdsSmfMarkerEvent *me = &markers[*marker_count];
                    memcpy(me->label, &data[pos], copy_len);
                    me->label[copy_len] = '\0';
                    me->tick = abs_tick;
                    (*marker_count)++;
                } else fprintf(stderr, "[smf] more than %d markers in this file -- dropping the rest\n", marker_cap);
            }
            // Every other meta type (Track Name, End of Track, Time/Key Signature, Sequencer-
            // Specific, ...) is simply skipped by its own length prefix -- a real SMF reader must
            // tolerate any meta type it doesn't specifically use, not just the two above.
            pos += meta_len;
        } else if (status_byte == 0xF0 || status_byte == 0xF7) { // Sysex -- skip by its own length prefix
            pos++;
            uint32_t sysex_len;
            if (!hds_smf_read_vlq(data, len, &pos, &sysex_len)) return false;
            if (sysex_len > len - pos) return false;
            pos += sysex_len;
        } else {
            // A channel voice/mode message, possibly via running status: a status byte < 0x80 is
            // NOT a fresh status -- it's the first DATA byte of an event reusing whichever real
            // status byte came before it (a standard MIDI file-size optimization for runs of the
            // same message type, e.g. a whole chord's worth of Note On events).
            uint8_t status;
            if (status_byte & 0x80) { status = status_byte; running_status = status_byte; pos++; }
            else { status = running_status; if (status == 0) return false; } // no running status yet to fall back on -- corrupt
            uint8_t hi = status & 0xF0;
            int data_bytes = (hi == 0xC0 || hi == 0xD0) ? 1 : 2; // Program Change/Channel Pressure: 1 data byte; everything else here: 2
            if ((size_t)data_bytes > len - pos) return false;
            uint8_t d1 = data[pos];
            uint8_t d2 = (data_bytes == 2) ? data[pos + 1] : 0;
            pos += (size_t)data_bytes;
            if (hi == 0x80 || hi == 0x90) { // Note Off / Note On
                bool is_on = (hi == 0x90 && d2 > 0); // velocity-0 Note On counts as Note Off, matching this engine's own live MIDI decode convention
                if (*note_count < note_cap) { notes[*note_count] = (HdsSmfNoteEvent){ abs_tick, is_on ? (unsigned char)0 : (unsigned char)1, d1, d2 }; (*note_count)++; }
                else if (note_cap > 0) fprintf(stderr, "[smf] more than %d note events on one track -- dropping the rest\n", note_cap);
            }
            // Every other channel message (CC, Program Change, Pitch Bend, Aftertouch) is simply
            // ignored for import purposes -- matches seq_record_midi_event's own scope (this
            // engine's recorded Sequencer lanes only ever hold note-on/off events).
        }
    }
    return true;
}

static int hds_smf_tempo_event_cmp(const void *a, const void *b) {
    long ta = ((const HdsSmfTempoEvent *)a)->tick, tb = ((const HdsSmfTempoEvent *)b)->tick;
    return (ta > tb) - (ta < tb);
}
// Used only by hds_song_import_smf_track() below, which -- unlike hds_song_import_smf() -- can
// merge more than one track's own note events into a single lane, so their combined order needs
// an explicit sort (each track's own slice is already tick-ordered on its own, but concatenating
// several tracks' slices back-to-back in file order is not the same as concatenating them in TIME
// order). qsort's lack of a stability guarantee is harmless here: two notes landing on the exact
// same tick from different tracks fire simultaneously either way.
static int hds_smf_note_event_cmp(const void *a, const void *b) {
    long ta = ((const HdsSmfNoteEvent *)a)->tick, tb = ((const HdsSmfNoteEvent *)b)->tick;
    return (ta > tb) - (ta < tb);
}

// Converts an absolute tick position into real seconds by walking the (already tick-sorted)
// tempo map and accumulating elapsed time segment by segment. Before the first explicit Set Tempo
// event, the MIDI spec's own default applies: 500000 usec/quarter note = 120 BPM.
static double hds_smf_ticks_to_seconds(long target_tick, const HdsSmfTempoEvent *tempo_events, int tempo_count, int ticks_per_quarter) {
    double seconds = 0.0;
    long prev_tick = 0;
    uint32_t usec_per_qn = 500000;
    for (int i = 0; i < tempo_count && tempo_events[i].tick <= target_tick; i++) {
        seconds += (double)(tempo_events[i].tick - prev_tick) * (double)usec_per_qn / 1e6 / (double)ticks_per_quarter;
        usec_per_qn = tempo_events[i].usec_per_qn;
        prev_tick = tempo_events[i].tick;
    }
    seconds += (double)(target_tick - prev_tick) * (double)usec_per_qn / 1e6 / (double)ticks_per_quarter;
    return seconds;
}

// Opens, reads, and validates file_path's own MThd header -- shared by both the whole-song
// importer (hds_song_import_smf) and the single-lane/single-track importer
// (hds_song_import_smf_track) below, which differ only in what they do with each track's own
// tempo/marker/note events, not in how the file itself is opened or its header validated. On
// success returns the malloc'd raw file buffer positioned to read from byte 14 onward (the
// header is always exactly 14 bytes: "MThd" + u32 length(6) + u16 format/ntrks/division) --
// caller owns it and must free() it. On failure returns NULL with *error already filled in.
static uint8_t *hds_smf_read_and_validate_header(const char *file_path, long *out_fsize, uint16_t *out_ntrks, int *out_ticks_per_quarter, char *error, size_t error_len) {
    FILE *f = fopen(file_path, "rb");
    if (!f) { snprintf(error, error_len, "could not open file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 14 || fsize > HDS_SMF_FILE_SIZE_MAX) {
        fclose(f);
        snprintf(error, error_len, "not a plausible Standard MIDI File (size)");
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fclose(f); snprintf(error, error_len, "out of memory"); return NULL; }
    size_t read_bytes = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (read_bytes != (size_t)fsize) { free(buf); snprintf(error, error_len, "short read"); return NULL; }

    if (memcmp(buf, "MThd", 4) != 0) { free(buf); snprintf(error, error_len, "missing MThd header"); return NULL; }
    size_t pos = 4;
    uint32_t header_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &header_len);
    if (header_len != 6) { free(buf); snprintf(error, error_len, "unexpected MThd length"); return NULL; }
    uint16_t format, ntrks, division;
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &format);
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &ntrks);
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &division);
    if (format == 2) { free(buf); snprintf(error, error_len, "Type 2 (independent unsynced tracks) has no single timeline -- not supported"); return NULL; }
    if (division & 0x8000) { free(buf); snprintf(error, error_len, "SMPTE timecode division is not supported (only ticks-per-quarter-note files)"); return NULL; }
    int ticks_per_quarter = (int)division;
    if (ticks_per_quarter <= 0) { free(buf); snprintf(error, error_len, "invalid ticks-per-quarter-note division"); return NULL; }

    *out_fsize = fsize;
    *out_ntrks = ntrks;
    *out_ticks_per_quarter = ticks_per_quarter;
    return buf;
}

// Imports file_path into engine->seq_midi[]/knobs, per this section's own header comment.
// import_tempo controls ONLY whether the file's Set Tempo events also get applied to Single
// Program mode's own tempo-map knobs (programs[SINGLE_PROGRAM_SLOT].tempo_marker_*) -- note
// timing itself always uses the file's own tempo map to convert ticks to seconds regardless of
// this flag, since that's simply what correct playback of the file requires.
static bool hds_song_import_smf(HdsEngineState *engine, SynthSharedControls *knobs, const char *file_path, bool import_tempo, HdsSmfImportResult *result) {
    *result = (HdsSmfImportResult){0};
    long fsize; uint16_t ntrks; int ticks_per_quarter;
    uint8_t *buf = hds_smf_read_and_validate_header(file_path, &fsize, &ntrks, &ticks_per_quarter, result->error, sizeof(result->error));
    if (!buf) return false;
    size_t pos = 14;

    static HdsSmfTempoEvent tempo_events[HDS_SMF_TEMPO_EVENT_MAX];
    int tempo_count = 0;
    static HdsSmfMarkerEvent marker_events[HDS_SMF_MARKER_MAX];
    int marker_count = 0;
    // Per-track note staging -- static, far too large for the stack (mirrors hds_song_load_midi's
    // own identical reasoning for its "loaded[]" staging array).
    static HdsSmfNoteEvent note_stage[SEQ_LANE_COUNT][SEQ_MIDI_EVENT_CAPACITY];
    static int note_stage_count[SEQ_LANE_COUNT];
    int lanes_used = 0;
    memset(note_stage_count, 0, sizeof(note_stage_count));

    for (int t = 0; t < (int)ntrks; t++) {
        if (pos + 8 > (size_t)fsize || memcmp(&buf[pos], "MTrk", 4) != 0) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "missing MTrk header for track %d", t);
            return false;
        }
        pos += 4;
        uint32_t track_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &track_len);
        if (track_len > (size_t)fsize - pos) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "track %d runs past end of file", t);
            return false;
        }
        int this_track_note_count = 0;
        HdsSmfNoteEvent *dest_notes = (lanes_used < SEQ_LANE_COUNT) ? note_stage[lanes_used] : NULL;
        int dest_cap = (dest_notes != NULL) ? SEQ_MIDI_EVENT_CAPACITY : 0;
        bool track_ok = hds_smf_parse_track(&buf[pos], track_len,
                                             tempo_events, &tempo_count, HDS_SMF_TEMPO_EVENT_MAX,
                                             marker_events, &marker_count, HDS_SMF_MARKER_MAX,
                                             dest_notes, &this_track_note_count, dest_cap);
        if (!track_ok) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "corrupt event data in track %d", t);
            return false;
        }
        if (this_track_note_count > 0) {
            if (lanes_used < SEQ_LANE_COUNT) {
                note_stage_count[lanes_used] = this_track_note_count;
                lanes_used++;
            } else {
                fprintf(stderr, "[smf] more than %d note-carrying tracks in this file -- dropping track %d entirely\n", SEQ_LANE_COUNT, t);
            }
        }
        pos += track_len;
    }
    free(buf);

    qsort(tempo_events, (size_t)tempo_count, sizeof(HdsSmfTempoEvent), hds_smf_tempo_event_cmp);

    // Everything parsed cleanly -- commit. Clears every lane first (a real Load, not a merge --
    // same "no stale leftovers" posture as hds_song_load_midi), then refills however many the
    // file actually used.
    for (int l = 0; l < SEQ_LANE_COUNT; l++) { engine->seq_midi[l].event_count = 0; engine->seq_midi[l].playback_cursor = 0; }
    for (int l = 0; l < lanes_used; l++) {
        int n = note_stage_count[l];
        engine->seq_midi[l].event_count = n;
        for (int e = 0; e < n; e++) {
            engine->seq_midi[l].events[e] = (SeqMidiEvent){
                (float)hds_smf_ticks_to_seconds(note_stage[l][e].tick, tempo_events, tempo_count, ticks_per_quarter),
                note_stage[l][e].type, note_stage[l][e].note, note_stage[l][e].velocity
            };
        }
    }

    if (import_tempo) {
        ProgramState *prog = &knobs->programs[SINGLE_PROGRAM_SLOT];
        if (tempo_count <= 1) {
            // The common case, a file with one flat, unchanging tempo: no map needed at all --
            // matches compute_current_tempo_bpm()'s own "no markers" fast path.
            prog->tempo_bpm = (tempo_count == 1) ? (60000000.0f / (float)tempo_events[0].usec_per_qn) : 120.0f;
            prog->tempo_marker_count = 0.0f;
        } else {
            int n = tempo_count < TEMPO_MARKER_MAX ? tempo_count : TEMPO_MARKER_MAX;
            if (tempo_count > TEMPO_MARKER_MAX) {
                fprintf(stderr, "[smf] file has %d tempo changes, more than this engine's %d-slot tempo map holds -- keeping the first %d\n", tempo_count, TEMPO_MARKER_MAX, TEMPO_MARKER_MAX);
            }
            for (int i = 0; i < n; i++) {
                prog->tempo_marker_time[i] = (float)hds_smf_ticks_to_seconds(tempo_events[i].tick, tempo_events, tempo_count, ticks_per_quarter);
                prog->tempo_marker_bpm[i] = 60000000.0f / (float)tempo_events[i].usec_per_qn;
            }
            prog->tempo_marker_count = (float)n;
        }
    }

    int n_markers = marker_count < HDS_SMF_MARKER_MAX ? marker_count : HDS_SMF_MARKER_MAX;
    for (int i = 0; i < n_markers; i++) {
        result->marker_time[i] = (float)hds_smf_ticks_to_seconds(marker_events[i].tick, tempo_events, tempo_count, ticks_per_quarter);
        snprintf(result->marker_label[i], HDS_SMF_MARKER_LABEL_LEN, "%s", marker_events[i].label);
    }
    // Sort markers by time -- almost always already in order (collected in increasing-tick order
    // per track, and markers conventionally live on just one track), but a stable guarantee costs
    // nothing here (marker_count is capped small) and the Sequencer tab's Markers list shouldn't
    // have to re-sort what should already read as a timeline. Plain insertion sort (small N).
    for (int i = 1; i < n_markers; i++) {
        float t = result->marker_time[i];
        char lbl[HDS_SMF_MARKER_LABEL_LEN]; snprintf(lbl, sizeof(lbl), "%s", result->marker_label[i]);
        int j = i - 1;
        while (j >= 0 && result->marker_time[j] > t) {
            result->marker_time[j + 1] = result->marker_time[j];
            memcpy(result->marker_label[j + 1], result->marker_label[j], HDS_SMF_MARKER_LABEL_LEN);
            j--;
        }
        result->marker_time[j + 1] = t;
        snprintf(result->marker_label[j + 1], HDS_SMF_MARKER_LABEL_LEN, "%s", lbl);
    }

    result->ok = true;
    result->lanes_used = lanes_used;
    result->tempo_applied = import_tempo;
    result->marker_count = n_markers;
    return true;
}

typedef struct {
    bool ok;
    char error[128];
    int lane;
    int note_count;
    int tracks_with_notes; // > 1 means more than one track in the file had notes -- they were all merged into this one lane, not silently dropped
} HdsSmfTrackImportResult;

// Per-lane "Load MIDI..." (Sequencer tab, each Program/MIDI lane's own strip): imports a single
// .mid/.midi file's note events straight into ONE lane, replacing whatever it currently holds --
// distinct from hds_song_import_smf's whole-song import just above, which repopulates EVERY lane
// and can also apply the file's tempo map. This never touches tempo_marker_*/the Markers list at
// all -- those are exclusively a whole-song concept; a single dropped-in track has no "song
// position" of its own to speak of. Every note-carrying track in the file is merged together,
// sorted by time, into the target lane -- most single-track exports only ever have one such
// track anyway, but a stray extra one (e.g. a leftover tempo-only conductor track alongside the
// real notes) merges in rather than getting silently dropped.
static bool hds_song_import_smf_track(HdsEngineState *engine, int lane_index, const char *file_path, HdsSmfTrackImportResult *result) {
    *result = (HdsSmfTrackImportResult){0};
    result->lane = lane_index;
    if (lane_index < 0 || lane_index >= SEQ_LANE_COUNT) { snprintf(result->error, sizeof(result->error), "invalid lane index"); return false; }

    long fsize; uint16_t ntrks; int ticks_per_quarter;
    uint8_t *buf = hds_smf_read_and_validate_header(file_path, &fsize, &ntrks, &ticks_per_quarter, result->error, sizeof(result->error));
    if (!buf) return false;
    size_t pos = 14;

    static HdsSmfTempoEvent tempo_events[HDS_SMF_TEMPO_EVENT_MAX];
    int tempo_count = 0;
    // Collected (Set Tempo meta-events are still needed to convert this file's OWN ticks to
    // correct real seconds) but never applied to any knob -- see this function's own comment.
    static HdsSmfMarkerEvent marker_events[HDS_SMF_MARKER_MAX]; // collected, then simply discarded -- markers are a whole-song concept only
    int marker_count = 0;
    static HdsSmfNoteEvent note_stage[SEQ_MIDI_EVENT_CAPACITY]; // every note-carrying track's events, concatenated (sorted by time below)
    int note_count = 0;
    int tracks_with_notes = 0;

    for (int t = 0; t < (int)ntrks; t++) {
        if (pos + 8 > (size_t)fsize || memcmp(&buf[pos], "MTrk", 4) != 0) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "missing MTrk header for track %d", t);
            return false;
        }
        pos += 4;
        uint32_t track_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &track_len);
        if (track_len > (size_t)fsize - pos) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "track %d runs past end of file", t);
            return false;
        }
        int this_track_note_count = 0;
        int remaining_cap = SEQ_MIDI_EVENT_CAPACITY - note_count;
        bool track_ok = hds_smf_parse_track(&buf[pos], track_len,
                                             tempo_events, &tempo_count, HDS_SMF_TEMPO_EVENT_MAX,
                                             marker_events, &marker_count, HDS_SMF_MARKER_MAX,
                                             &note_stage[note_count], &this_track_note_count, remaining_cap);
        if (!track_ok) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "corrupt event data in track %d", t);
            return false;
        }
        if (this_track_note_count > 0) { note_count += this_track_note_count; tracks_with_notes++; }
        pos += track_len;
    }
    free(buf);

    qsort(tempo_events, (size_t)tempo_count, sizeof(HdsSmfTempoEvent), hds_smf_tempo_event_cmp);
    qsort(note_stage, (size_t)note_count, sizeof(HdsSmfNoteEvent), hds_smf_note_event_cmp);

    engine->seq_midi[lane_index].event_count = note_count;
    engine->seq_midi[lane_index].playback_cursor = 0;
    for (int e = 0; e < note_count; e++) {
        engine->seq_midi[lane_index].events[e] = (SeqMidiEvent){
            (float)hds_smf_ticks_to_seconds(note_stage[e].tick, tempo_events, tempo_count, ticks_per_quarter),
            note_stage[e].type, note_stage[e].note, note_stage[e].velocity
        };
    }

    result->ok = true;
    result->note_count = note_count;
    result->tracks_with_notes = tracks_with_notes;
    return true;
}

typedef struct {
    HdsEngineState *engine;
} HdsSmfTrackImportContext;

// Fired once, synchronously -- same posture as song_import_smf_handler() just below, which this
// mirrors closely (a per-lane import instead of a whole-song one, no tempo/knobs argument since
// this never touches any).
static int song_import_smf_track_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types;
    HdsSmfTrackImportContext *ctx = (HdsSmfTrackImportContext *)user_data;
    if (argc != 2) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    int lane_index = (int)argv[1]->f;
    HdsSmfTrackImportResult result;
    if (!hds_song_import_smf_track(ctx->engine, lane_index, file_path, &result)) {
        fprintf(stderr, "[smf] track import of '%s' into lane %d failed: %s\n", file_path, lane_index, result.error);
    }
    // Reply to the actual sender (see request_state_handler's own comment on why) rather than
    // ctx->reply_addr's fixed startup target -- whichever frontend just asked for this import is
    // the one that should hear back, not necessarily whoever --osc-reply-host/--osc-reply-port
    // originally pointed at.
    lo_address reply_addr = lo_message_get_source(msg);
    if (!reply_addr) return 0;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, result.ok ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import_track/ok", m);
    m = lo_message_new(); lo_message_add_string(m, result.error); lo_bundle_add_message(b, "/hds/status/smf_import_track/error", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.lane); lo_bundle_add_message(b, "/hds/status/smf_import_track/lane", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.note_count); lo_bundle_add_message(b, "/hds/status/smf_import_track/note_count", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.tracks_with_notes); lo_bundle_add_message(b, "/hds/status/smf_import_track/tracks_with_notes", m);
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    return 0;
}

typedef struct {
    HdsEngineState *engine;
    SynthSharedControls *knobs;
} HdsSmfImportContext;

// Fired once, synchronously, right from this handler (same "rare, discrete, user-initiated
// action" posture as hds_song_save_midi/load_midi -- see that section's own REALTIME NOTE): parses
// the file, applies it, and immediately reports the result back to reply_addr as one small bundle
// -- there's no periodic per-block status poll for this the way send_seq_audio_status has, since
// an import is a one-shot event, not ongoing state.
static int song_import_smf_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types;
    HdsSmfImportContext *ctx = (HdsSmfImportContext *)user_data;
    if (argc != 2) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    bool import_tempo = argv[1]->f != 0.0f;
    HdsSmfImportResult result;
    if (!hds_song_import_smf(ctx->engine, ctx->knobs, file_path, import_tempo, &result)) {
        fprintf(stderr, "[smf] import of '%s' failed: %s\n", file_path, result.error);
    }
    // Reply to the actual sender -- see request_state_handler's own comment.
    lo_address reply_addr = lo_message_get_source(msg);
    if (!reply_addr) return 0;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, result.ok ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import/ok", m);
    m = lo_message_new(); lo_message_add_string(m, result.error); lo_bundle_add_message(b, "/hds/status/smf_import/error", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.lanes_used); lo_bundle_add_message(b, "/hds/status/smf_import/lanes_used", m);
    m = lo_message_new(); lo_message_add_float(m, result.tempo_applied ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_applied", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.marker_count); lo_bundle_add_message(b, "/hds/status/smf_import/marker_count", m);
    for (int i = 0; i < result.marker_count; i++) {
        char addr[64];
        snprintf(addr, sizeof(addr), "/hds/status/smf_import/marker/%d/time", i);
        m = lo_message_new(); lo_message_add_float(m, result.marker_time[i]); lo_bundle_add_message(b, addr, m);
        snprintf(addr, sizeof(addr), "/hds/status/smf_import/marker/%d/label", i);
        m = lo_message_new(); lo_message_add_string(m, result.marker_label[i]); lo_bundle_add_message(b, addr, m);
    }
    // Tempo map values, if applied -- so the frontend can cache them and keep echoing them back
    // on every future transmit_knob_packet() call (which resends every knob's CURRENT value on
    // any change, tempo markers included -- without its own cached copy, the very next unrelated
    // knob tweak would blast the just-imported tempo map back to flat/zero; see frontend.py's own
    // transmit_knob_packet() comment on this exact block).
    if (result.tempo_applied) {
        const ProgramState *prog = &ctx->knobs->programs[SINGLE_PROGRAM_SLOT];
        int n = (int)prog->tempo_marker_count;
        m = lo_message_new(); lo_message_add_float(m, prog->tempo_bpm); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_bpm", m);
        m = lo_message_new(); lo_message_add_float(m, (float)n); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_marker_count", m);
        for (int i = 0; i < n; i++) {
            char addr[64];
            snprintf(addr, sizeof(addr), "/hds/status/smf_import/tempo_marker/%d/time", i);
            m = lo_message_new(); lo_message_add_float(m, prog->tempo_marker_time[i]); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/smf_import/tempo_marker/%d/bpm", i);
            m = lo_message_new(); lo_message_add_float(m, prog->tempo_marker_bpm[i]); lo_bundle_add_message(b, addr, m);
        }
    }
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    return 0;
}

// No context needed beyond the path itself -- hds_tuning_load_scl() writes straight into the
// static g_tuning_table (see its own comment), the same "OSC carries just a path, the backend
// does the file I/O" shape as zone_load_sfz_handler/song_load_midi_handler just above.
static int tuning_load_scl_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)msg; (void)user_data;
    if (argc != 1) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    if (!hds_tuning_load_scl(file_path)) {
        fprintf(stderr, "[tuning] failed to load '%s' (missing, unreadable, or malformed .scl)\n", file_path);
    } else {
        fprintf(stderr, "[tuning] loaded '%s': %d degrees (\"%s\")\n", file_path, g_tuning_table.degree_count, g_tuning_table.description);
    }
    return 0;
}

// How many arp steps occur per quarter-note beat, indexed by ProgramState.arp_rate. Deliberately
// a plain, even note-division rate table rather than a full swing/groove pattern system (unlike
// Arpeggino/Midier's own 12-entry Rhythm module) -- a plain Rate is what "Rate" means on the vast
// majority of hardware/software arpeggiators, and is the design this pass's own from-scratch
// engine targets (see the Arpeggiator section on ProgramState for the full scope explanation).
static const float ARP_RATE_STEPS_PER_BEAT[6] = {
    1.0f, // 0 = 1/4
    2.0f, // 1 = 1/8
    4.0f, // 2 = 1/16
    3.0f, // 3 = 1/8 Triplet
    6.0f, // 4 = 1/16 Triplet
    8.0f, // 5 = 1/32
};

// Simple xorshift32 PRNG -- deterministic, no external entropy needed (a musical pattern
// generator, not cryptography) -- used only by Style=Random below.
static unsigned int arp_xorshift32(unsigned int *state) {
    unsigned int x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

// Arpeggiator engine: one independent musical clock per Program, called once per block (same
// placement/shape as seq_playback_midi_events just above, not from inside render_audio_block's
// own per-sample loop -- render_audio_block has no access to mono_stacks[], which
// handle_note_on/handle_note_off both require, so this stays a main()-loop-level poll like
// poll_metronome/poll_transport_triggers/seq_playback_midi_events already are).
//
// While a Program's arp_enabled is set, its real note-on/off events never reach
// handle_note_on/handle_note_off directly (see dispatch_midi_event's own arp_enabled branch) --
// they only update engine->arp_held_notes[p] via arp_note_on/arp_note_off. This function is the
// only place that actually triggers voices for such a Program, synthesizing its own note-on/off
// calls exactly like seq_playback_midi_events already does for recorded Sequencer events.
static void poll_arpeggiator(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], const MidiLiveState *midi_live, int block_size) {
    bool combi_mode_active = ((int)knobs->performance_mode == 1);
    int active_program_count = combi_mode_active ? COMBI_PROGRAM_COUNT : 1;

    // BUG FIX: each step used to call compute_current_tempo_bpm() directly and immediately bake
    // its raw, instantaneous result into that step's own samples_per_step -- so a fast Tempo knob
    // move (or any large single jump, e.g. a fast drag across the dial's 20-300 range) landed on
    // the very next step boundary as a hard, un-smoothed discontinuity in the arp's own clock
    // rate. Musically that reads as exactly the "glitchy"/"reset"-feeling jump reported: the
    // pattern doesn't glide to the new tempo, it snaps. Chased here instead, once per block (this
    // function's own natural granularity -- poll_arpeggiator runs once per audio block, not once
    // per sample), over a ~120ms time constant: fast enough that turning the knob still feels
    // live and responsive, slow enough that the step clock always arrives at the new tempo
    // smoothly rather than jumping straight to it. The chase-rate formula (1 - exp(-block_size /
    // (SAMPLE_RATE * tau))) is the block-granularity equivalent of the same per-sample one-pole
    // chase used elsewhere in this file (see tick_adsr's sustain-chase and master_volume_smoothed
    // above) -- applying the per-sample version block_size times in a row is mathematically
    // identical to one application of this formula, so this stays correct regardless of the
    // live-adjustable Block Size knob's own current value.
    float raw_tempo_bpm = compute_current_tempo_bpm(knobs, engine->transport_playhead_sec);
    if (!engine->arp_tempo_smoothed_initialized) {
        engine->arp_tempo_smoothed = raw_tempo_bpm;
        engine->arp_tempo_smoothed_initialized = true;
    } else {
        const float tempo_chase_rate = 1.0f - expf(-(float)block_size / (SAMPLE_RATE * 0.120f));
        engine->arp_tempo_smoothed += (raw_tempo_bpm - engine->arp_tempo_smoothed) * tempo_chase_rate;
    }

    for (int p_loop = 0; p_loop < active_program_count; p_loop++) {
        int p = combi_mode_active ? p_loop : SINGLE_PROGRAM_SLOT;
        const ProgramState *prog = &knobs->programs[p];
        MonoNoteStack *held = &engine->arp_held_notes[p];
        bool poly_mode_active = (prog->is_polyphonic != 0.0f);

        if (prog->arp_enabled == 0.0f) {
            // Off: release anything still sounding and reset transient state, so re-enabling
            // later always starts a fresh phrase rather than resuming mid-pattern.
            if (engine->arp_note_active[p]) {
                handle_note_off(se, voices, knobs, midi_live, p, engine->arp_playing_note[p], poly_mode_active, &mono_stacks[p]);
                engine->arp_note_active[p] = false;
            }
            held->count = 0;
            engine->arp_keys_down[p] = 0;
            engine->arp_step_samples_left[p] = 0;
            engine->arp_step_cursor[p] = 0;
            continue;
        }

        if (held->count == 0) {
            // Nothing held (and nothing latched) -- idle. Re-arming the step clock to 0 means the
            // next held note starts its own first step immediately, not partway through whatever
            // countdown happened to be left over from the previous phrase.
            if (engine->arp_note_active[p]) {
                handle_note_off(se, voices, knobs, midi_live, p, engine->arp_playing_note[p], poly_mode_active, &mono_stacks[p]);
                engine->arp_note_active[p] = false;
            }
            engine->arp_step_samples_left[p] = 0;
            engine->arp_step_cursor[p] = 0;
            continue;
        }

        // Gate: release the currently-sounding arp note once its own gate length elapses.
        if (engine->arp_note_active[p]) {
            engine->arp_gate_samples_left[p] -= block_size;
            if (engine->arp_gate_samples_left[p] <= 0) {
                handle_note_off(se, voices, knobs, midi_live, p, engine->arp_playing_note[p], poly_mode_active, &mono_stacks[p]);
                engine->arp_note_active[p] = false;
            }
        }

        // Step clock -- a `while`, not `if` (mirrors poll_metronome's own idiom), so an unusually
        // fast Rate relative to this block's own sample count still fires every step it owes
        // rather than falling behind; capped defensively so a near-zero step length can never
        // spin this loop indefinitely.
        engine->arp_step_samples_left[p] -= block_size;
        int guard = 0;
        // Strictly `< 0`, not `<= 0`: after firing, `+= samples_per_step` can land the
        // remainder on EXACTLY 0 whenever block_size happens to divide evenly into
        // samples_per_step (a real, unremarkable case -- e.g. block_size==samples_per_step
        // outright). That remainder means "the following step is due at the very start of the
        // NEXT call," not "still due within this one" -- `<= 0` would double-fire right here
        // whenever the schedule lands exactly on a block boundary.
        while (engine->arp_step_samples_left[p] < 0 && guard++ < 8) {
            if (engine->arp_note_active[p]) {
                // A still-sounding previous step's note (gate >= 100%, or simply not yet expired)
                // is cut now -- one note at a time, never overlapping, within one Program.
                handle_note_off(se, voices, knobs, midi_live, p, engine->arp_playing_note[p], poly_mode_active, &mono_stacks[p]);
                engine->arp_note_active[p] = false;
            }

            int n = held->count;
            int octave_range = (int)prog->arp_octave_range;
            if (octave_range < 1) octave_range = 1; else if (octave_range > 4) octave_range = 4;
            int total = n * octave_range;
            int style = (int)prog->arp_style;

            // Pitch-ascending permutation of held->notes[0..n-1]'s own indices -- Up/Down/UpDown/
            // DownUp/Random all walk PITCH order, not press order (the classic arpeggiator
            // meaning of "Up"); AsPlayed alone uses held->notes[] directly in MonoNoteStack's own
            // insertion order. n <= MONO_STACK_SIZE always, so this plain insertion sort (cheap,
            // and only run once per step fire, not per sample) never exceeds its stack array.
            int sorted[MONO_STACK_SIZE];
            for (int i = 0; i < n; i++) sorted[i] = i;
            for (int i = 1; i < n; i++) {
                int key = sorted[i], key_note = held->notes[key], j = i - 1;
                while (j >= 0 && held->notes[sorted[j]] > key_note) { sorted[j + 1] = sorted[j]; j--; }
                sorted[j + 1] = key;
            }

            int note_slot, octave_index;
            if (style == 4) {
                // Random: a fresh uniformly-random combined step every time -- no cursor to advance.
                if (engine->arp_rng_state[p] == 0) engine->arp_rng_state[p] = (unsigned int)p * 2654435761u + 1u; // guards xorshift32's degenerate all-zero seed
                unsigned int r = arp_xorshift32(&engine->arp_rng_state[p]);
                int combined = (int)(r % (unsigned int)total);
                note_slot = sorted[combined % n];
                octave_index = combined / n;
            } else if (style == 5) {
                // As Played: raw press order (MonoNoteStack's own insertion order), not pitch order.
                int combined = engine->arp_step_cursor[p] % total;
                note_slot = combined % n;
                octave_index = combined / n;
                engine->arp_step_cursor[p]++;
            } else {
                // Up (0) / Down (1) / UpDown (2) / DownUp (3): all built from one advancing
                // combined-step counter over the full [0, n*octave_range) span.
                int combined;
                if (style == 2 || style == 3) {
                    // Ping-pong fold: play the full ascending run then the full descending run,
                    // without repeating either end note -- the "steps*2-2, mirror past the
                    // midpoint" trick Arpeggino's own Midier engine uses for its own UpDown-style
                    // scale-degree permutations (src/layer/layer.cpp, Layer::click(), MIT
                    // licensed) -- the one concrete algorithmic idea this from-scratch engine
                    // borrows directly from it, generalized here from "scale degrees" to "held
                    // notes x octave layers".
                    int fold_len = (total <= 1) ? 1 : (total * 2 - 2);
                    int c = engine->arp_step_cursor[p] % fold_len;
                    combined = (c < total) ? c : (fold_len - c);
                    engine->arp_step_cursor[p]++;
                } else {
                    combined = engine->arp_step_cursor[p] % total;
                    engine->arp_step_cursor[p]++;
                }
                if (style == 1 || style == 3) combined = (total - 1) - combined; // Down/DownUp: reverse the whole mapping
                note_slot = sorted[combined % n];
                octave_index = combined / n;
            }

            int note = held->notes[note_slot] + 12 * octave_index;
            if (note < 0) note = 0; else if (note > 127) note = 127;
            int arp_vel_override = (int)prog->arp_velocity;
            int velocity = (arp_vel_override <= 0) ? held->velocities[note_slot]
                : (arp_vel_override > 127 ? 127 : arp_vel_override);

            handle_note_on(se, voices, knobs, global_lfo_phases, p, note, velocity, poly_mode_active, &mono_stacks[p]);
            engine->arp_note_active[p] = true;
            engine->arp_playing_note[p] = note;

            float bpm = engine->arp_tempo_smoothed; // smoothed, not raw -- see this function's own header comment
            int rate_idx = (int)prog->arp_rate;
            if (rate_idx < 0 || rate_idx > 5) rate_idx = 1;
            float steps_per_beat = ARP_RATE_STEPS_PER_BEAT[rate_idx];
            int samples_per_step = (int)((float)SAMPLE_RATE * 60.0f / fmaxf(1.0f, bpm) / steps_per_beat);
            if (samples_per_step < 1) samples_per_step = 1;
            float gate_frac = fmaxf(0.01f, fminf(1.0f, prog->arp_gate_pct / 100.0f));
            engine->arp_gate_samples_left[p] = (int)((float)samples_per_step * gate_frac);
            if (engine->arp_gate_samples_left[p] < 1) engine->arp_gate_samples_left[p] = 1;

            engine->arp_step_samples_left[p] += samples_per_step;
        }
    }
}

// --- MIDI auto-connect: no more manually running aconnect ---
//
// --- Shared MIDI event dispatch ---
// Everything about WHAT happens once a MIDI event is decoded -- Combi-lane channel fan-out,
// Single Program/Sequencer dispatch, sustain-pedal polarity, pitch-bend/channel-pressure scaling,
// Sequencer recording -- lives in this ONE function, called by process_midi_input below once it
// has decoded a raw ALSA sequencer event into this neutral shape.
// `value`'s meaning depends on `type`, matching the exact units the ALSA sequencer already
// delivered before this pass: NOTE_ON/NOTE_OFF -> velocity (0-127, data1=note 0-127); CC ->
// raw CC value (0-127, data1=CC number 0-127); PITCH_BEND -> signed -8192..8191, 0=centered
// (data1 unused); CHANNEL_PRESSURE -> raw pressure (0-127, data1 unused).
typedef enum { HDS_MIDI_NOTE_ON, HDS_MIDI_NOTE_OFF, HDS_MIDI_CC, HDS_MIDI_PITCH_BEND, HDS_MIDI_CHANNEL_PRESSURE } HdsMidiEventType;

// --- Arpeggiator note-tracking (see the Arpeggiator section on ProgramState and
// poll_arpeggiator()'s own header comment for the full design) ---
//
// When a Program has arp_enabled, a real note-on/off event must NOT trigger a voice directly --
// it should only update this Program's own held-notes list, and poll_arpeggiator() (its own
// independent musical clock) decides which note actually sounds and when, via its own synthetic
// handle_note_on/handle_note_off calls. These two functions are dispatch_midi_event's own
// note-on/off handling, redirected here instead of straight into handle_note_on/handle_note_off.
static void arp_note_on(HdsEngineState *engine, int target_program, int note, int velocity) {
    mono_stack_push(&engine->arp_held_notes[target_program], note, velocity);
    engine->arp_keys_down[target_program]++;
}

static void arp_note_off(HdsEngineState *engine, const ProgramState *prog, int target_program, int note) {
    if (engine->arp_keys_down[target_program] > 0) engine->arp_keys_down[target_program]--;
    if (prog->arp_hold == 0.0f) {
        // Not latched: track exactly which keys are physically held, same as a real arpeggiator.
        mono_stack_remove(&engine->arp_held_notes[target_program], note);
    }
    // Hold/Latch engaged: deliberately leave arp_held_notes untouched so the arpeggio keeps
    // cycling through the already-latched chord after the physical keys are released -- classic
    // hardware "Hold" behavior. arp_keys_down still gets decremented above regardless, so the
    // NEXT fresh note-on (arriving once every physically-held key has been released) can tell it
    // should replace the latched chord -- see dispatch_midi_event's own Hold-handling comment.
}

static void dispatch_midi_event(HdsEngineState *engine, HdsSampleEngine *se, HdsMidiEventType type, int midi_channel_1_16, int data1, int value,
                                 SynthVoice voices[], SynthSharedControls *knobs,
                                 float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT],
                                 MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], MidiLiveState *midi_live, float current_tempo_bpm) {
    if (type == HDS_MIDI_NOTE_ON || type == HDS_MIDI_NOTE_OFF) {
        int note = data1;
        bool is_note_on = (type == HDS_MIDI_NOTE_ON);
        if ((int)knobs->performance_mode == 1) {
            // Combi mode: live dispatch fans out to every Combi lane whose own MIDI channel
            // matches (0 = Omni) -- a separate, parallel mechanism from Sequencer's own
            // recorded-MIDI playback (seq_playback_midi_events), which always targets
            // SINGLE_PROGRAM_SLOT regardless of this mode (see that function's own scope
            // comment).
            for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
                float lane_channel = knobs->combi_lane_midi_channel[p];
                if (lane_channel != 0.0f && (int)lane_channel != midi_channel_1_16) continue;
                // Key range gates both note-on and note-off identically (a note's pitch is the
                // same for its own on/off pair). Velocity range gates note-on ONLY -- see
                // combi_lane_vel_lo/hi's own comment for why note-off must never be gated by it
                // (a note-off's "velocity" is release velocity, an unrelated value; gating by it
                // would risk a stuck note for a lane this note-on legitimately sounded on).
                if (note < (int)knobs->combi_lane_key_lo[p] || note > (int)knobs->combi_lane_key_hi[p]) continue;
                if (is_note_on && (value < (int)knobs->combi_lane_vel_lo[p] || value > (int)knobs->combi_lane_vel_hi[p])) continue;
                const ProgramState *lane_prog = &knobs->programs[p];
                if (lane_prog->arp_enabled != 0.0f) {
                    // Arp engaged for this lane: feed the held-note list, don't trigger a voice
                    // directly -- poll_arpeggiator() does that on its own musical clock.
                    if (is_note_on) {
                        // Hold/Latch: a fresh phrase (arriving once every previously-held key has
                        // been released) replaces the latched chord rather than piling onto it --
                        // the standard "press a new chord to change it" Hold convention.
                        if (lane_prog->arp_hold != 0.0f && engine->arp_keys_down[p] == 0) engine->arp_held_notes[p].count = 0;
                        arp_note_on(engine, p, note, value);
                    } else {
                        arp_note_off(engine, lane_prog, p, note);
                    }
                    continue;
                }
                bool prog_poly_active = (lane_prog->is_polyphonic != 0.0f);
                if (is_note_on) handle_note_on(se, voices, knobs, global_lfo_phases, p, note, value, prog_poly_active, &mono_stacks[p]);
                else handle_note_off(se, voices, knobs, midi_live, p, note, prog_poly_active, &mono_stacks[p]);
            }
        } else {
            // Single Program / Sequencer modes: always SINGLE_PROGRAM_SLOT (Single Program
            // mode's own dedicated ProgramState, never a Combi lane's -- see ProgramState's
            // own comment on the mutual-exclusivity fix), fully channel-agnostic, exactly as
            // today.
            const ProgramState *prog = &knobs->programs[SINGLE_PROGRAM_SLOT];
            if (prog->arp_enabled != 0.0f) {
                if (is_note_on) {
                    if (prog->arp_hold != 0.0f && engine->arp_keys_down[SINGLE_PROGRAM_SLOT] == 0) engine->arp_held_notes[SINGLE_PROGRAM_SLOT].count = 0;
                    arp_note_on(engine, SINGLE_PROGRAM_SLOT, note, value);
                } else {
                    arp_note_off(engine, prog, SINGLE_PROGRAM_SLOT, note);
                }
            } else {
                bool prog_poly_active = (prog->is_polyphonic != 0.0f);
                if (is_note_on) handle_note_on(se, voices, knobs, global_lfo_phases, SINGLE_PROGRAM_SLOT, note, value, prog_poly_active, &mono_stacks[SINGLE_PROGRAM_SLOT]);
                else handle_note_off(se, voices, knobs, midi_live, SINGLE_PROGRAM_SLOT, note, prog_poly_active, &mono_stacks[SINGLE_PROGRAM_SLOT]);
            }
        }
        // Deliberate scope boundary: this records the raw physical key event regardless of Arp
        // routing above -- Sequencer recording captures what the performer played, not what the
        // Arp subsequently generates from it. Making Sequencer-recorded/played-back events
        // themselves feed an Arp (via seq_playback_midi_events' own separate call site) is a
        // further extension not attempted here.
        if (is_note_on) seq_record_midi_event(engine, knobs, 0, (unsigned char)note, (unsigned char)value, midi_channel_1_16, current_tempo_bpm);
        else seq_record_midi_event(engine, knobs, 1, (unsigned char)note, 0, midi_channel_1_16, current_tempo_bpm);
    } else if (type == HDS_MIDI_CC) {
        int cc_num = data1;
        if (cc_num >= 0 && cc_num < 128) midi_live->cc[cc_num] = fmaxf(0.0f, fminf(1.0f, value / 127.0f));
        if (cc_num == 64) {
            // Sustain pedal. polarity: 0 = Normal (>=64 means held, essentially every real
            // pedal), 1 = Inverted (for the rare one wired backwards). Uses Single Program
            // mode's own polarity setting (the one physical Global-panel knob) even in Combi
            // mode -- the sustain pedal is one physical controller and this engine has no
            // per-Program polarity concept to fan out to; a documented scope boundary, not an
            // oversight.
            bool raw_held = value >= 64;
            bool now_held = (knobs->programs[SINGLE_PROGRAM_SLOT].sustain_pedal_polarity != 0.0f) ? !raw_held : raw_held;
            if (midi_live->sustain_held && !now_held) handle_sustain_release(se, voices, knobs); // pedal just lifted
            midi_live->sustain_held = now_held;
        }
    } else if (type == HDS_MIDI_PITCH_BEND) {
        midi_live->pitch_bend = fmaxf(-1.0f, fminf(1.0f, value / 8192.0f));
    } else if (type == HDS_MIDI_CHANNEL_PRESSURE) {
        midi_live->channel_pressure = fmaxf(0.0f, fminf(1.0f, value / 127.0f));
    }
}

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
// our port to the ALSA announce port at startup (see main()'s comment on that subscription). The
// actual musical response to each decoded event is dispatch_midi_event() above -- this function's
// own job is purely "decode an ALSA sequencer event."
static void process_midi_input(HdsStreamsState *streams, HdsEngineState *engine, HdsSampleEngine *se, snd_seq_t *seq_handle, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], int our_midi_port, MidiSourceList *midi_sources, MidiLiveState *midi_live, float current_tempo_bpm) {
    snd_seq_event_t *ev = NULL;
    while (snd_seq_event_input_pending(seq_handle, 1) > 0) {
        snd_seq_event_input(seq_handle, &ev); if (ev == NULL) continue;
        if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
            // ALSA's own sequencer channel is 0-indexed (0-15); SeqLaneState.midi_channel/
            // combi_lane_midi_channel use 1-16 (0 reserved for Omni), so +1 here converts once
            // for every call site inside dispatch_midi_event.
            int midi_channel_1_16 = (int)ev->data.note.channel + 1;
            bool is_note_on = (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0);
            dispatch_midi_event(engine, se, is_note_on ? HDS_MIDI_NOTE_ON : HDS_MIDI_NOTE_OFF, midi_channel_1_16, ev->data.note.note, ev->data.note.velocity, voices, knobs, global_lfo_phases, mono_stacks, midi_live, current_tempo_bpm);
        } else if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
            dispatch_midi_event(engine, se, HDS_MIDI_CC, (int)ev->data.control.channel + 1, ev->data.control.param, ev->data.control.value, voices, knobs, global_lfo_phases, mono_stacks, midi_live, current_tempo_bpm);
        } else if (ev->type == SND_SEQ_EVENT_PITCHBEND) {
            // ALSA's own sequencer convention: value is already signed, -8192..8191, 0 = centered.
            dispatch_midi_event(engine, se, HDS_MIDI_PITCH_BEND, (int)ev->data.control.channel + 1, 0, ev->data.control.value, voices, knobs, global_lfo_phases, mono_stacks, midi_live, current_tempo_bpm);
        } else if (ev->type == SND_SEQ_EVENT_CHANPRESS) {
            dispatch_midi_event(engine, se, HDS_MIDI_CHANNEL_PRESSURE, (int)ev->data.control.channel + 1, 0, ev->data.control.value, voices, knobs, global_lfo_phases, mono_stacks, midi_live, current_tempo_bpm);
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
        } else if (ev->type == SND_SEQ_EVENT_CLOCK) {
            // Tempo tracking runs unconditionally (see hds_handle_midi_clock_tick's own comment)
            // -- only ACTING on it (feeding current_tempo_bpm) is gated on seq_sync_source, done
            // once per block in main(), not here.
            hds_handle_midi_clock_tick(engine);
        } else if (ev->type == SND_SEQ_EVENT_START) {
            if (knobs->seq_sync_source != 0.0f) hds_handle_midi_transport_start(streams, engine, knobs);
        } else if (ev->type == SND_SEQ_EVENT_CONTINUE) {
            if (knobs->seq_sync_source != 0.0f) hds_handle_midi_transport_continue(engine);
        } else if (ev->type == SND_SEQ_EVENT_STOP) {
            if (knobs->seq_sync_source != 0.0f) hds_handle_midi_transport_stop(engine);
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
static float render_audio_block(HdsSampleEngine *se, HdsEngineState *engine, SynthVoice voices[], const SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], const float *ext_in_buffer, int ext_in_channels, int output_channels, const MidiLiveState *midi_live, float *out_buffer, float current_tempo_bpm, float ext_in_peak_out[EXT_IN_PAIR_COUNT], float *per_program_out) {
    float peak = 0.0f;
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
    if (!engine->fx_engine_initialized) { reset_fx_engine(fx_engine); engine->fx_engine_initialized = true; }

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
    sync_fx_engine_types(knobs->fx, fx_engine);
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

            for(int e=0; e<ENV_COUNT; e++) {
                if(e==0) tick_adsr(&voices[v].vco1_eg, &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==1) tick_adsr(&voices[v].vco2_eg, &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==2) tick_adsr(&voices[v].vco3_eg, &eg_rates[pidx][e], eg_sustain_chase_rate);
                else if(e==3) tick_adsr(&voices[v].vco4_eg, &eg_rates[pidx][e], eg_sustain_chase_rate);
                else tick_adsr(&voices[v].assign_eg[e-4], &eg_rates[pidx][e], eg_sustain_chase_rate);
            }
            release_idle_sample_streams(se, &voices[v]); // WAVE_SAMPLE: free any DFD stream this voice's oscillator(s) no longer need

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
            // transpose_cents: the global performance Transpose knob (-16..16 semitones,
            // session-level -- see SynthSharedControls.transpose_semitones), applied the same
            // way pitch bend is: an additive cents offset here, never touching voice->midi_note,
            // so it retunes already-held notes live without disturbing Key Track/note-off
            // matching (both of which read midi_note directly).
            float transpose_cents = knobs->transpose_semitones * 100.0f;
            // Combi per-lane transpose (octave layering): an additional additive-cents term,
            // exactly like transpose_cents above, but per-Combi-lane instead of global -- see
            // SynthSharedControls.combi_lane_transpose's own comment. Only meaningful for the 8
            // real Combi lanes (pidx 0-7); SINGLE_PROGRAM_SLOT (pidx==COMBI_PROGRAM_COUNT) has no
            // lane to look up, so it contributes nothing there, same as today.
            float combi_transpose_cents = (pidx < COMBI_PROGRAM_COUNT) ? knobs->combi_lane_transpose[pidx] * 100.0f : 0.0f;
            float mod_vco1_pitch = prog->vco1_fine_tune + prog->vco1_pitch + pb_bend_cents[pidx] + transpose_cents + combi_transpose_cents;
            float mod_vco2_pitch = prog->vco2_fine_tune + (prog->vco2_pitch_offset * 100.0f) + prog->vco2_pitch + pb_bend_cents[pidx] + transpose_cents + combi_transpose_cents;
            float mod_vco3_pitch = prog->vco3_fine_tune + (prog->vco3_pitch_offset * 100.0f) + prog->vco3_pitch + pb_bend_cents[pidx] + transpose_cents + combi_transpose_cents;
            float mod_vco4_pitch = prog->vco4_fine_tune + (prog->vco4_pitch_offset * 100.0f) + prog->vco4_pitch + pb_bend_cents[pidx] + transpose_cents + combi_transpose_cents;
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
            // Pan/Pulse Width/Noise/Sub as mod destinations (targets 32-48 -- see the
            // matrix_shared_routes comment in frontend.py for the full list). Pan mods are
            // applied directly (same convention as Phase); the rest get a scale factor sized so
            // a unity-depth LFO/EG sweeps the whole musically useful range.
            float mod_vco1_pan = 0.0f, mod_vco2_pan = 0.0f;
            float mod_vco1_pulse_width = 0.0f, mod_vco2_pulse_width = 0.0f;
            float mod_vco3_amp = 0.0f, mod_vco3_pan = 0.0f, mod_vco3_tilt = 0.0f;
            float mod_vco4_amp = 0.0f, mod_vco4_pan = 0.0f, mod_vco4_tilt = 0.0f;

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
            float curve_drive_arr[4] = { prog->vco1_curve_drive, prog->vco2_curve_drive, prog->vco3_curve_drive, prog->vco4_curve_drive };
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
                float modulator_sum = 0.0f;
                for (int e = 0; e < alg->edge_count; e++) {
                    if (alg->edges[e].to_osc != osc) continue;
                    int src = alg->edges[e].from_osc;
                    float depth = *(const float*)((const char*)knobs + alg->edges[e].depth_offset + (size_t)pidx * sizeof(ProgramState));
                    if (alg->edges[e].depth_offset == offsetof(SynthSharedControls, programs[0].fm_intensity)) depth += fm_mod;
                    depth = fmaxf(0.0f, depth);
                    modulator_sum += modulator_output[src] * depth;
                }
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
                shape_arr[0] == WAVE_UNISON_SAW || shape_arr[0] == WAVE_SAMPLE,
                shape_arr[1] == WAVE_UNISON_SAW || shape_arr[1] == WAVE_SAMPLE,
                shape_arr[2] == WAVE_UNISON_SAW || shape_arr[2] == WAVE_SAMPLE,
                shape_arr[3] == WAVE_UNISON_SAW || shape_arr[3] == WAVE_SAMPLE,
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
            // per-oscillator gain multiplier, 1.0 at vel_to_amp=0 for every velocity (byte-
            // identical to pre-existing behavior by construction) or at velocity=64 ("normal")
            // regardless of depth. Clamped to non-negative -- an extreme depth at a very low
            // velocity could otherwise go slightly negative and phase-invert the oscillator.
            float vel_to_amp_arr[4] = { prog->vco1_vel_to_amp, prog->vco2_vel_to_amp, prog->vco3_vel_to_amp, prog->vco4_vel_to_amp };
            float vel_amp_mult_arr[4];
            for (int osc = 0; osc < 4; osc++) {
                vel_amp_mult_arr[osc] = fmaxf(0.0f, 1.0f + (vel_to_amp_arr[osc] / 100.0f) * ((float)(voices[v].midi_velocity - 64) / 63.0f));
            }
            float osc_l[4], osc_r[4];
            for (int osc = 0; osc < 4; osc++) {
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
                // If this pair's second channel doesn't exist (e.g. a mono capture device, or an
                // odd channel count's last pair), duplicate the first into both L/R -- matches
                // this engine's existing mono-capture-device convention exactly (see the old
                // single-pair tap this generalizes, which did the same thing for channels 0/1).
                float l = ext_in_buffer[i * ext_in_channels + 2 * p];
                float r = (2 * p + 1 < ext_in_channels) ? ext_in_buffer[i * ext_in_channels + 2 * p + 1] : l;
                bus_l[src] = l * knobs->ext_in_gain[p];
                bus_r[src] = r * knobs->ext_in_gain[p];
                if (meter_ext_in) {
                    float pair_peak = fmaxf(fabsf(bus_l[src]), fabsf(bus_r[src]));
                    if (pair_peak > ext_in_peak_out[p]) ext_in_peak_out[p] = pair_peak;
                }
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
    return peak;
}
// --- Audio platform shim ---
// HdsPcmStream/HdsStreamDirection are the ONLY audio-I/O types anything outside this shim (main()
// included) ever names. HdsPcmStream used to be a bare type alias for snd_pcm_t; it's now a small
// tagged wrapper so the exact same four functions (open/write/read/close, plus the two channel-
// count probes just below) can transparently back onto either the ALSA device path that has
// always lived here, OR a native PipeWire client stream (see the PipeWire backend section right
// after open_pcm_stream()) -- chosen once at startup via --backend and never mixed within one
// process. Every call site elsewhere in this file (main()'s setup and its live device-switching
// loop) is completely unchanged by this -- it never touches the union's members directly.
typedef enum { HDS_BACKEND_ALSA, HDS_BACKEND_PIPEWIRE } HdsAudioBackend;
// Resolved once from --backend at the very top of main(), before any device is opened, and never
// changed afterward except by the interactive output-device recovery path falling back from a
// broken PipeWire connection to ALSA (see interactive_recover_output_device()).
static HdsAudioBackend g_audio_backend = HDS_BACKEND_ALSA;
// --pipewire-force-quantum: opt-in, default OFF. See hds_pw_stream_open()'s own comment on
// PW_KEY_NODE_FORCE_QUANTUM for what this actually does and its real tradeoff (forces every OTHER
// PipeWire client on the same system down to this stream's own small buffer too, for as long as
// this one is connected -- can make an unrelated app underrun/glitch). Deliberately a separate
// opt-in flag rather than baked into --backend pipewire unconditionally, specifically so it's
// trivially reversible at runtime (just don't pass the flag) rather than needing a code revert if
// it turns out to cause problems on some system.
static bool g_pipewire_force_quantum = false;

#if HDS_PIPEWIRE_ENABLED
// Bridges this engine's existing "blocking write / one-shot best-effort read" I/O contract (see
// write_audio_block()/read_capture_block() below, which is exactly the ALSA behavior this file
// has always had) onto PipeWire's own push-model client API: a pw_stream's process() callback
// runs on PipeWire's OWN realtime thread and pulls/pushes data on ITS schedule, not the main
// loop's -- unlike a blocking snd_pcm_writei()/readi() call, there's no way to just call into
// PipeWire and have it synchronously hand back "the next block". A small lock-free SPSC ring
// buffer per direction (same producer/consumer discipline as RecordRing elsewhere in this file)
// closes that gap, plus one semaphore used ONLY on the playback side as a wake signal so
// write_audio_block()'s pipewire branch can block until room frees up -- exactly mirroring
// snd_pcm_writei()'s own blocking behavior, which is what actually paces this engine's entire main
// loop today. The ring's atomics remain the actual source of truth for how much space/data is
// available; the semaphore only avoids busy-spinning between checks.
#define HDS_PW_RING_CAPACITY (1u << 16) // 65536 floats -- ~170ms at 48kHz/8ch, far more than one PipeWire quantum
typedef struct {
    float data[HDS_PW_RING_CAPACITY];
    atomic_uint_fast32_t write_pos;
    atomic_uint_fast32_t read_pos;
} HdsPwRing;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    bool is_capture;
    int channels;
    HdsPwRing ring;
    sem_t space_sem;     // posted by on_playback_process() whenever it frees ring space; unused on the capture side (read_capture_block() never blocks, matching the ALSA capture branch's own non-blocking, best-effort contract)
    atomic_bool connected; // set once the stream reaches PW_STREAM_STATE_STREAMING
    atomic_bool failed;    // set on PW_STREAM_STATE_ERROR/UNCONNECTED after having been open -- lets a blocked write_audio_block() bail out instead of hanging on a dead daemon/unplugged device
} HdsPwStream;
#endif

typedef struct HdsPcmStream {
    HdsAudioBackend backend;
    snd_pcm_t *alsa;
#if HDS_PIPEWIRE_ENABLED
    HdsPwStream *pw;
#endif
} HdsPcmStream;
typedef enum { HDS_STREAM_PLAYBACK, HDS_STREAM_CAPTURE } HdsStreamDirection;

#if HDS_PIPEWIRE_ENABLED
// Forward declaration: close_pcm_stream() (just below) needs to call this, but its real
// definition lives down in the "PipeWire backend" section right before open_pcm_stream(), which
// is the more natural place to read it alongside hds_pw_stream_open() and the process callbacks.
static void hds_pw_stream_close(HdsPwStream *pw);
#endif

// DIAGNOSTIC: active_voices_count and render_ms are purely informational (see their computation
// in main()'s loop) -- passed through so an underrun/xrun can be reported alongside what the
// engine was doing at that exact moment, instead of a bare device error with no way to tell
// whether it was a CPU-load problem (too many active voices / too expensive a patch to render in
// time) or something unrelated (OS scheduling jitter, another process, a flaky USB interface).
static void write_audio_block(HdsPcmStream *pcm_handle, const float *buffer, int channels, uint32_t frame_count, int active_voices_count, double render_ms) {
    // Guards the pathological case where even the fallback-to-last-known-good reopen in main()
    // failed (e.g. a USB audio interface was physically unplugged) -- silently drop this block
    // rather than crash; main() will keep retrying the device on the next control packet.
    if (!pcm_handle) return;
    if (pcm_handle->backend == HDS_BACKEND_ALSA) {
        snd_pcm_uframes_t written = 0;
        while (written < frame_count && g_synth_running) {
            snd_pcm_sframes_t res = snd_pcm_writei(pcm_handle->alsa, buffer + (written * (size_t)channels), frame_count - written);
            if (res < 0) {
                fprintf(stderr, "[audio] ALSA underrun/xrun (%s) -- %d active voices, last render_audio_block took %.3fms\n",
                        snd_strerror((int)res), active_voices_count, render_ms);
                res = snd_pcm_recover(pcm_handle->alsa, res, 0); if (res < 0) break;
            }
            else { written += res; }
        }
        return;
    }
#if HDS_PIPEWIRE_ENABLED
    // PipeWire side: push into the SPSC ring, blocking (via space_sem) exactly the way
    // snd_pcm_writei() above blocks until the device wants more -- this is what keeps this
    // engine's main loop paced at the real playback rate under PipeWire too, not just under ALSA.
    HdsPwStream *pw = pcm_handle->pw;
    uint32_t total_samples = frame_count * (uint32_t)channels;
    uint32_t written_samples = 0;
    while (written_samples < total_samples && g_synth_running) {
        if (atomic_load_explicit(&pw->failed, memory_order_acquire)) {
            fprintf(stderr, "[pipewire] output stream failed -- %d active voices, last render_audio_block took %.3fms\n", active_voices_count, render_ms);
            break;
        }
        uint32_t write_pos = atomic_load_explicit(&pw->ring.write_pos, memory_order_relaxed);
        uint32_t read_pos = atomic_load_explicit(&pw->ring.read_pos, memory_order_acquire);
        uint32_t space = HDS_PW_RING_CAPACITY - (write_pos - read_pos);
        if (space == 0) {
            // sem_timedwait (not sem_wait) so a missed wakeup -- a post landing between the space
            // check above and this wait -- can't block this thread, and therefore the whole main
            // loop, forever; a timeout just re-checks the ring's real atomics on the next pass.
            struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 1;
            sem_timedwait(&pw->space_sem, &deadline);
            continue;
        }
        uint32_t take = space < (total_samples - written_samples) ? space : (total_samples - written_samples);
        for (uint32_t i = 0; i < take; i++) pw->ring.data[(write_pos + i) & (HDS_PW_RING_CAPACITY - 1)] = buffer[written_samples + i];
        atomic_store_explicit(&pw->ring.write_pos, write_pos + take, memory_order_release);
        written_samples += take;
    }
#endif
}

// Reads up to block_size frames of capture audio, interleaved, `channels` per frame -- the exact
// shape main()'s loop already builds capture_buffer around. Returns the number of frames actually
// filled; the caller (main()) zero-fills the remainder on a short read, same as it always has.
// A NEW named function (replacing what used to be a 3-line inline snd_pcm_readi call in main())
// so device I/O stays isolated behind this shim rather than inlined into main()'s own loop.
static uint32_t read_capture_block(HdsPcmStream *pcm_capture_handle, float *capture_buffer, int channels, uint32_t block_size) {
    if (!pcm_capture_handle) return 0;
    if (pcm_capture_handle->backend == HDS_BACKEND_ALSA) {
        (void)channels; // ALSA already knows its own channel count from the params negotiated at open time
        snd_pcm_sframes_t got = snd_pcm_readi(pcm_capture_handle->alsa, capture_buffer, block_size);
        if (got < 0) { snd_pcm_recover(pcm_capture_handle->alsa, (int)got, 0); return 0; }
        return (uint32_t)got;
    }
#if HDS_PIPEWIRE_ENABLED
    // Same non-blocking, best-effort contract as the ALSA branch above: pop whatever's already in
    // the ring (filled by on_capture_process() on PipeWire's own thread) and return immediately --
    // the caller (main()) already zero-fills any shortfall, same as a short ALSA read.
    HdsPwStream *pw = pcm_capture_handle->pw;
    uint32_t want_samples = block_size * (uint32_t)channels;
    uint32_t write_pos = atomic_load_explicit(&pw->ring.write_pos, memory_order_acquire);
    uint32_t read_pos = atomic_load_explicit(&pw->ring.read_pos, memory_order_relaxed);
    uint32_t available = write_pos - read_pos;
    uint32_t take = available < want_samples ? available : want_samples;
    for (uint32_t i = 0; i < take; i++) capture_buffer[i] = pw->ring.data[(read_pos + i) & (HDS_PW_RING_CAPACITY - 1)];
    atomic_store_explicit(&pw->ring.read_pos, read_pos + take, memory_order_release);
    return take / (uint32_t)channels;
#else
    return 0; // unreachable: backend is always HDS_BACKEND_ALSA when HDS_PIPEWIRE_ENABLED==0 (see --backend validation in main())
#endif
}

// Closes and frees a stream opened by open_pcm_stream() -- a NEW named function replacing
// main()'s scattered direct snd_pcm_close(...) calls, so cleanup stays platform-agnostic too.
// Safe to call with NULL.
static void close_pcm_stream(HdsPcmStream *handle) {
    if (!handle) return;
    if (handle->backend == HDS_BACKEND_ALSA) {
        if (handle->alsa) snd_pcm_close(handle->alsa);
    }
#if HDS_PIPEWIRE_ENABLED
    else {
        hds_pw_stream_close(handle->pw);
    }
#endif
    free(handle);
}

// Queries how many capture channels a device actually offers (opens it briefly just to ask, then
// closes it again). Used to size the raw capture stream/buffer for the Recording tab's per-
// channel checkboxes: "checkboxes per channel detected" needs a real detected count, not a
// guess. Falls back to 2 (plain stereo) on any failure -- an unopenable device will fail again,
// loudly, when open_pcm_stream() itself tries.
static int query_device_channel_count(const char *device_name) {
    // PipeWire's own graph handles channel remixing between whatever this engine requests and
    // whatever the real target node/hardware actually has, so there's no cheap local hw_params-
    // style probe the way ALSA offers one -- this engine talks a fixed stereo capture stream to
    // PipeWire (see hds_pw_stream_open()); a multichannel PipeWire target still works, just
    // downmixed to stereo by the graph rather than reaching every raw channel individually the way
    // the ALSA backend's own multichannel routing can.
    if (g_audio_backend == HDS_BACKEND_PIPEWIRE) return 2;
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
    if (g_audio_backend == HDS_BACKEND_PIPEWIRE) return 2; // see query_device_channel_count()'s own comment above
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

// (Re)opens a float PCM stream by device name, closing *handle first if it was already open.
// Used both at startup and whenever the Audio tab's device fields change live (see main()'s
// loop) -- a single place so open/configure/error-reporting can't drift between the call sites.
// `channels` is whatever query_device_channel_count()/query_output_device_channel_count()
// detected for that stream.
// BUG FIX (Linux): the original startup code never checked snd_pcm_open()/snd_pcm_set_params()'s
// return values at all -- an unavailable "default" device (no sound card, no PulseAudio/ALSA
// routing) left pcm_handle uninitialized/unusable and the very first snd_pcm_writei() call
// would segfault or hang instead of failing with a clear message.
#if HDS_PIPEWIRE_ENABLED
// --- PipeWire backend (native libpipewire client -- --backend pipewire) ---
//
// This talks to the PipeWire daemon directly over its own client protocol (the same way
// `pw-play`/`pw-cat`/`pw-record` do), NOT through the ALSA "pipewire" plugin
// (libasound_module_pcm_pipewire.so) that snd_pcm_open("default", ...) would otherwise route
// through on a PipeWire-based system. That ALSA plugin is a separate, optional compatibility shim
// with its own load-time/ABI requirements independent of the PipeWire daemon itself -- exactly the
// gap this backend exists to route around: a real, observed failure mode is that plugin failing to
// load ("... cannot be opened or _snd_pcm_pipewire_open was not defined inside") while the
// PipeWire daemon it would have connected to is running fine. A native pw_stream client has no
// dependency on that plugin at all.
//
// Being a real PipeWire graph client (rather than an ALSA client of any kind) is also what makes
// "run while other programs are running" work: PipeWire's own session manager mixes every client
// stream in software, the same as it does for a browser tab and a media player sharing one output
// today -- there's no exclusive hardware lock to contend over, unlike a raw ALSA hw: device.
//
// device_name here names a PipeWire target node (as `wpctl status`/`pw-cli ls Node` print it) --
// "default"/"" leaves PW_KEY_TARGET_OBJECT unset, so PipeWire's own session manager
// (wireplumber/pipewire-media-session) picks the default sink/source, mirroring what ALSA's
// "default" PCM aims for.

static void hds_pw_on_playback_process(void *userdata) {
    HdsPwStream *pw = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(pw->stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(pw->stream, b); return; }
    uint32_t stride = sizeof(float) * (uint32_t)pw->channels;
    uint32_t max_frames = buf->datas[0].maxsize / stride;
    uint32_t want_frames = (b->requested > 0 && (uint32_t)b->requested < max_frames) ? (uint32_t)b->requested : max_frames;
    uint32_t want_samples = want_frames * (uint32_t)pw->channels;

    uint32_t write_pos = atomic_load_explicit(&pw->ring.write_pos, memory_order_acquire);
    uint32_t read_pos = atomic_load_explicit(&pw->ring.read_pos, memory_order_relaxed);
    uint32_t available = write_pos - read_pos;
    uint32_t take = available < want_samples ? available : want_samples;
    for (uint32_t i = 0; i < take; i++) dst[i] = pw->ring.data[(read_pos + i) & (HDS_PW_RING_CAPACITY - 1)];
    if (take < want_samples) memset(dst + take, 0, (want_samples - take) * sizeof(float)); // underrun -- write_audio_block() hasn't fed the ring fast enough; silence the rest rather than play stale/garbage data
    atomic_store_explicit(&pw->ring.read_pos, read_pos + take, memory_order_release);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int32_t)stride;
    buf->datas[0].chunk->size = want_samples * (uint32_t)sizeof(float);
    pw_stream_queue_buffer(pw->stream, b);
    sem_post(&pw->space_sem); // wake write_audio_block()'s pipewire branch -- more ring space just opened up
}

static void hds_pw_on_capture_process(void *userdata) {
    HdsPwStream *pw = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(pw->stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    float *src = buf->datas[0].data;
    uint32_t stride = sizeof(float) * (uint32_t)pw->channels;
    uint32_t n_samples = src ? (buf->datas[0].chunk->size / (uint32_t)sizeof(float)) : 0;
    if (src && n_samples > 0) {
        uint32_t write_pos = atomic_load_explicit(&pw->ring.write_pos, memory_order_relaxed);
        uint32_t read_pos = atomic_load_explicit(&pw->ring.read_pos, memory_order_acquire);
        uint32_t space = HDS_PW_RING_CAPACITY - (write_pos - read_pos);
        uint32_t take = space < n_samples ? space : n_samples; // drop the tail on overflow (main thread reading too slowly) -- input is already documented best-effort elsewhere in this file
        for (uint32_t i = 0; i < take; i++) pw->ring.data[(write_pos + i) & (HDS_PW_RING_CAPACITY - 1)] = src[i];
        atomic_store_explicit(&pw->ring.write_pos, write_pos + take, memory_order_release);
    }
    (void)stride;
    pw_stream_queue_buffer(pw->stream, b);
}

static void hds_pw_on_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)old;
    HdsPwStream *pw = userdata;
    if (state == PW_STREAM_STATE_STREAMING) {
        atomic_store_explicit(&pw->connected, true, memory_order_release);
    } else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        atomic_store_explicit(&pw->connected, false, memory_order_release);
        atomic_store_explicit(&pw->failed, true, memory_order_release);
        if (error) fprintf(stderr, "[pipewire] stream error: %s\n", error);
        sem_post(&pw->space_sem); // wake a blocked write_audio_block() rather than let it hang on a now-dead stream
    }
}

static const struct pw_stream_events hds_pw_playback_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = hds_pw_on_state_changed,
    .process = hds_pw_on_playback_process,
};
static const struct pw_stream_events hds_pw_capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = hds_pw_on_state_changed,
    .process = hds_pw_on_capture_process,
};

static void hds_pw_stream_close(HdsPwStream *pw) {
    if (!pw) return;
    if (pw->loop) {
        pw_thread_loop_stop(pw->loop);
        if (pw->stream) pw_stream_destroy(pw->stream);
        pw_thread_loop_destroy(pw->loop);
    }
    sem_destroy(&pw->space_sem);
    free(pw);
}

static HdsPwStream *hds_pw_stream_open(const char *device_name, bool is_capture, int channels, float latency_ms) {
    HdsPwStream *pw = calloc(1, sizeof(*pw));
    if (!pw) return NULL;
    pw->is_capture = is_capture;
    pw->channels = channels;
    sem_init(&pw->space_sem, 0, 0);

    pw->loop = pw_thread_loop_new(is_capture ? "hds-pw-capture" : "hds-pw-playback", NULL);
    if (!pw->loop) { sem_destroy(&pw->space_sem); free(pw); return NULL; }

    pw_thread_loop_lock(pw->loop);

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, is_capture ? "Capture" : "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        NULL);
    if (device_name && device_name[0] != '\0' && strcmp(device_name, "default") != 0) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device_name);
    }
    // Maps this engine's own audio_latency_ms control (Audio tab's Latency knob) onto PipeWire's
    // node latency hint, the same way open_pcm_stream()'s ALSA branch maps it onto the ALSA buffer
    // target -- kept consistent across both backends rather than PipeWire silently ignoring it.
    char latency_prop[32];
    unsigned int latency_frames = (unsigned int)fmaxf(64.0f, (latency_ms / 1000.0f) * (float)SAMPLE_RATE);
    snprintf(latency_prop, sizeof(latency_prop), "%u/%u", latency_frames, (unsigned int)SAMPLE_RATE);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_prop);
    // PW_KEY_NODE_LATENCY above is only a HINT: PipeWire negotiates one shared buffer size (the
    // graph "quantum") across every client currently connected to the daemon, so a lower request
    // from this one node can still be silently overridden upward if anything else sharing the same
    // daemon needs a bigger buffer -- the real, structural reason the PipeWire backend can measure
    // higher latency than the ALSA one, which gets an exclusive, tightly-sized hardware buffer
    // directly. PW_KEY_NODE_FORCE_QUANTUM instead actively forces the WHOLE graph's quantum to
    // this value for as long as this stream is connected -- a real lever, not just a request, and
    // exactly what a pro-audio PipeWire client is expected to do when the user wants low latency.
    // Real tradeoff, not a free lunch: every OTHER PipeWire client on the same system (a browser
    // tab, a video call, a media player) gets forced down to this same small buffer too for as
    // long as HDS is running, and can underrun/glitch if it can't keep up with it. Opt-in only
    // (--pipewire-force-quantum, default off) -- see g_pipewire_force_quantum's own comment for
    // why this is a separate flag rather than unconditional under --backend pipewire. Set to the
    // SAME value as the hint above so both stay consistent with whatever the Latency knob asks for.
    if (g_pipewire_force_quantum) {
        char force_quantum_prop[16];
        snprintf(force_quantum_prop, sizeof(force_quantum_prop), "%u", latency_frames);
        pw_properties_set(props, PW_KEY_NODE_FORCE_QUANTUM, force_quantum_prop);
    }

    pw->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(pw->loop),
        is_capture ? "HDS Synth Input" : "HDS Synth Output",
        props, // consumed by pw_stream_new_simple regardless of outcome -- never freed by us
        is_capture ? &hds_pw_capture_events : &hds_pw_playback_events,
        pw);
    if (!pw->stream) {
        pw_thread_loop_unlock(pw->loop);
        pw_thread_loop_destroy(pw->loop);
        sem_destroy(&pw->space_sem);
        free(pw);
        return NULL;
    }

    uint8_t pod_buffer[1024];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    struct spa_audio_info_raw info = { .format = SPA_AUDIO_FORMAT_F32, .channels = (uint32_t)channels, .rate = SAMPLE_RATE };
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(pw->stream, is_capture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    if (res < 0) {
        fprintf(stderr, "[pipewire] pw_stream_connect failed (%d)\n", res);
        pw_thread_loop_unlock(pw->loop);
        pw_stream_destroy(pw->stream);
        pw_thread_loop_destroy(pw->loop);
        sem_destroy(&pw->space_sem);
        free(pw);
        return NULL;
    }
    pw_thread_loop_unlock(pw->loop);

    if (pw_thread_loop_start(pw->loop) < 0) {
        pw_stream_destroy(pw->stream);
        pw_thread_loop_destroy(pw->loop);
        sem_destroy(&pw->space_sem);
        free(pw);
        return NULL;
    }

    // Give the daemon a real but bounded window to actually reach STREAMING -- a genuinely
    // unreachable PipeWire daemon (or a bad target/permission) should fail startup here rather
    // than the engine silently running with a dead audio stream, matching this shim's ALSA branch
    // "fail loudly, don't hang" posture for the equivalent open.
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (atomic_load_explicit(&pw->connected, memory_order_acquire)) break;
        if (atomic_load_explicit(&pw->failed, memory_order_acquire)) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20L * 1000L * 1000L };
        nanosleep(&ts, NULL);
    }
    if (!atomic_load_explicit(&pw->connected, memory_order_acquire)) {
        fprintf(stderr, "[pipewire] %s stream '%s' did not reach STREAMING state (daemon unreachable, or the target node/permission is wrong)\n",
                is_capture ? "Input" : "Output", (device_name && device_name[0]) ? device_name : "default");
        hds_pw_stream_close(pw);
        return NULL;
    }
    return pw;
}
#endif // HDS_PIPEWIRE_ENABLED

static bool open_pcm_stream(const char *device_name, HdsStreamDirection stream, HdsPcmStream **handle, float latency_ms, int channels) {
    if (*handle) { close_pcm_stream(*handle); *handle = NULL; }
#if HDS_PIPEWIRE_ENABLED
    if (g_audio_backend == HDS_BACKEND_PIPEWIRE) {
        HdsPwStream *pw = hds_pw_stream_open(device_name, stream == HDS_STREAM_CAPTURE, channels, latency_ms);
        if (!pw) return false;
        HdsPcmStream *s = calloc(1, sizeof(*s));
        if (!s) { hds_pw_stream_close(pw); return false; }
        s->backend = HDS_BACKEND_PIPEWIRE;
        s->pw = pw;
        *handle = s;
        return true;
    }
#endif
    snd_pcm_t *alsa = NULL;
    snd_pcm_stream_t alsa_stream = (stream == HDS_STREAM_PLAYBACK) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
    int err = snd_pcm_open(&alsa, device_name, alsa_stream, 0);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to open: %s\n", stream == HDS_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
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
    err = snd_pcm_set_params(alsa, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned int)channels, SAMPLE_RATE, 1, latency_us);
    if (err < 0) {
        fprintf(stderr, "%s device '%s' failed to configure: %s\n", stream == HDS_STREAM_PLAYBACK ? "Output" : "Input", device_name, snd_strerror(err));
        snd_pcm_close(alsa);
        return false;
    }
    HdsPcmStream *s = calloc(1, sizeof(*s));
    if (!s) { snd_pcm_close(alsa); return false; }
    s->backend = HDS_BACKEND_ALSA;
    s->alsa = alsa;
    *handle = s;
    return true;
}

// --- Interactive device recovery ---
//
// An output device that fails to open has always been FATAL at startup (see main()'s call to
// open_pcm_stream() for HDS_STREAM_PLAYBACK) -- there's no way to reach the OSC server's
// /hds/device/output handler if the process has already exited. That's still true for a
// non-interactive launch (systemd unit, script, CI, a headless SSH session with stdin redirected
// from /dev/null) -- it can never answer a prompt, so blocking on fgets() there would just hang
// forever instead of failing. But an interactive launch (a real terminal) doesn't have to just
// die on a bad/missing "default" device -- see interactive_recover_output_device() below and its
// isatty() guard at the one call site in main().
#define HDS_DEVICE_PROMPT_MAX 32

// Enumerates ALSA PCM device names (+ short descriptions) via snd_device_name_hint -- the same
// information `aplay -L`/`arecord -L` print (frontend.py's own device dropdown already shells out
// to those, see _list_alsa_devices() there), but through libasound directly, so this prompt needs
// no external alsa-utils binary to be installed.
static int enumerate_pcm_devices(HdsStreamDirection stream, char names[][AUDIO_DEVICE_NAME_LEN], char descs[][128], int max_devices) {
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return 0;
    const char *want_ioid = (stream == HDS_STREAM_PLAYBACK) ? "Output" : "Input";
    int count = 0;
    for (void **n = hints; *n != NULL && count < max_devices; n++) {
        char *name = snd_device_name_get_hint(*n, "NAME");
        char *ioid = snd_device_name_get_hint(*n, "IOID"); // NULL means "works for both directions"
        if (name && strcmp(name, "null") != 0 && (!ioid || strcmp(ioid, want_ioid) == 0)) {
            snprintf(names[count], AUDIO_DEVICE_NAME_LEN, "%s", name);
            char *desc = snd_device_name_get_hint(*n, "DESC");
            if (desc) {
                char *newline = strchr(desc, '\n'); // DESC can be two lines ("short\nlong") -- keep only the first for a compact prompt row
                if (newline) *newline = '\0';
            }
            snprintf(descs[count], 128, "%s", desc ? desc : "");
            free(desc);
            count++;
        }
        free(name);
        free(ioid);
    }
    snd_device_name_free_hint(hints);
    return count;
}

// Interactively prompts on stdin for one of the enumerated ALSA device names.
static bool prompt_for_pcm_device(HdsStreamDirection stream, char *out_device, size_t out_len) {
    char names[HDS_DEVICE_PROMPT_MAX][AUDIO_DEVICE_NAME_LEN];
    char descs[HDS_DEVICE_PROMPT_MAX][128];
    int count = enumerate_pcm_devices(stream, names, descs, HDS_DEVICE_PROMPT_MAX);
    if (count == 0) {
        fprintf(stderr, "No ALSA %s devices found to choose from.\n", stream == HDS_STREAM_PLAYBACK ? "output" : "input");
        return false;
    }
    for (;;) {
        fprintf(stderr, "\nAvailable ALSA %s devices:\n", stream == HDS_STREAM_PLAYBACK ? "output" : "input");
        for (int i = 0; i < count; i++) {
            fprintf(stderr, "  [%d] %s%s%s\n", i + 1, names[i], descs[i][0] ? " -- " : "", descs[i]);
        }
        fprintf(stderr, "Select a device by number (or 'q' to quit): ");
        fflush(stderr);
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) return false; // EOF/closed stdin -- can't prompt further
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == 'q' || line[0] == 'Q') return false;
        int choice = atoi(line);
        if (choice >= 1 && choice <= count) {
            snprintf(out_device, out_len, "%s", names[choice - 1]);
            return true;
        }
        fprintf(stderr, "Invalid selection.\n");
    }
}

#if HDS_PIPEWIRE_ENABLED
// PipeWire target nodes aren't cheap to enumerate the way ALSA's are (snd_device_name_hint is a
// synchronous, local query; PipeWire's graph lives behind an async registry round-trip), so this
// prompt asks for a target name directly rather than listing one -- `wpctl status`/`pw-cli ls
// Node` show the names to try. Also offers falling back to the ALSA backend entirely, for the
// case where the PipeWire daemon itself turns out to be unreachable.
static bool prompt_pipewire_retry(char *out_device, size_t out_len, bool *fall_back_to_alsa) {
    *fall_back_to_alsa = false;
    fprintf(stderr, "\nEnter a PipeWire target node name (see `wpctl status` or `pw-cli ls Node`),\n"
                     "leave blank to retry PipeWire's own default sink/source, type 'alsa' to fall\n"
                     "back to the ALSA backend instead, or 'q' to give up: ");
    fflush(stderr);
    char line[AUDIO_DEVICE_NAME_LEN]; // matches out_device's own real capacity (always AUDIO_DEVICE_NAME_LEN at the one call site) so a long name truncates once, consistently, rather than risking a second truncation in the snprintf below
    if (!fgets(line, sizeof(line), stdin)) return false;
    line[strcspn(line, "\n")] = '\0';
    if (line[0] == 'q' || line[0] == 'Q') return false;
    if (strcasecmp(line, "alsa") == 0) { *fall_back_to_alsa = true; return true; }
    snprintf(out_device, out_len, "%s", line);
    return true;
}
#endif

// The one place all of the above comes together: called only when the startup output device
// failed to open AND stdin is a real terminal (see main()'s isatty() guard). Loops until either a
// device opens successfully or the user gives up ('q'/EOF).
static bool interactive_recover_output_device(SynthSharedControls *knobs, HdsPcmStream **pcm_handle, int *detected_output_channels) {
    for (;;) {
        char picked[AUDIO_DEVICE_NAME_LEN] = {0};
        bool have_pick;
        if (g_audio_backend == HDS_BACKEND_ALSA) {
            have_pick = prompt_for_pcm_device(HDS_STREAM_PLAYBACK, picked, sizeof(picked));
        }
#if HDS_PIPEWIRE_ENABLED
        else {
            bool fall_back_to_alsa = false;
            have_pick = prompt_pipewire_retry(picked, sizeof(picked), &fall_back_to_alsa);
            if (have_pick && fall_back_to_alsa) {
                g_audio_backend = HDS_BACKEND_ALSA;
                snprintf(picked, sizeof(picked), "default");
                fprintf(stderr, "Falling back to the ALSA backend.\n");
            }
        }
#else
        else { have_pick = false; }
#endif
        if (!have_pick) return false; // user gave up ('q'/EOF)
        snprintf(knobs->output_device, AUDIO_DEVICE_NAME_LEN, "%s", picked[0] ? picked : "default");
        *detected_output_channels = query_output_device_channel_count(knobs->output_device);
        if (open_pcm_stream(knobs->output_device, HDS_STREAM_PLAYBACK, pcm_handle, knobs->audio_latency_ms, *detected_output_channels)) return true;
        fprintf(stderr, "That device also failed to open. Try another, or 'q' to give up.\n");
    }
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
// backpatch step, so the encoder is simply flushed and closed on stop.
//
// DISABLED for now (HDS_WAVPACK_ENABLED, defined near the top of this file, is 0) -- kept intact
// below rather than deleted so it can come back with a one-line flip. Below the real
// implementation is a small stub with the identical signatures/struct shape, so every call site
// (record_writer_drain, record_writer_thread_main) needs zero changes either way. ---
#if HDS_WAVPACK_ENABLED
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
#else // !HDS_WAVPACK_ENABLED -- stub with the same shape, never reached from the frontend since
      // its format radio button is disabled, but wavpack_recorder_start() fails loudly (not
      // silently) if ever reached directly via raw OSC.
typedef struct {
    int channels;
    uint32_t frames_written;
    bool active;
} WavPackRecorder;
static bool wavpack_recorder_start(WavPackRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    (void)rec; (void)path; (void)include_synth_output; (void)downmix_stereo; (void)input_channel_enable; (void)input_channel_count;
    fprintf(stderr, "[record] WavPack support is currently disabled (HDS_WAVPACK_ENABLED=0) -- recording not started\n");
    return false;
}
static void wavpack_recorder_write(WavPackRecorder *rec, const float *interleaved, int num_frames) { (void)rec; (void)interleaved; (void)num_frames; }
static void wavpack_recorder_stop(WavPackRecorder *rec) { (void)rec; }
#endif

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
    // Sequencer tab's transport readout. Reports the REAL transport playhead now (see
    // g_transport_playhead_sec/poll_transport_triggers) -- this field used to reuse engine
    // uptime as a placeholder before a real transport existed; now that one does, this is the
    // genuine, pausable/resettable position, a real behavior change from that placeholder.
    // Deliberately still a SEPARATE field from elapsed_seconds above, which remains the WAV
    // recorder's own unrelated recording-duration counter.
    float seq_elapsed_seconds;
    float seq_transport_state; // 0 = stopped, 1 = playing, 2 = paused -- see TransportState
    // Routing tab's per-ext-in-pair VU meters -- only ever non-zero while gui_routing_tab_visible
    // is set (see render_audio_block's own ext_in_peak_out comment); zero the rest of the time,
    // same "computed and sent every status tick regardless, just usually zero" shape as peak
    // itself rather than a separate conditional message.
    float ext_in_peak[EXT_IN_PAIR_COUNT];
    // 1.0 once External MIDI Clock sync is both selected AND actually locked onto real incoming
    // ticks (see main()'s own use_external_clock resolution) -- lets the GUI show "Ext: Locked"
    // vs "Ext: Waiting..." instead of leaving a player guessing whether a cable/routing problem is
    // why tempo isn't moving.
    float midi_clock_locked;
} EngineStatus;

// Sends the GUI-status snapshot as one OSC bundle (one UDP datagram, 8 messages) to the reply
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
    m = lo_message_new(); lo_message_add_float(m, status->seq_elapsed_seconds); lo_bundle_add_message(b, "/hds/status/seq_elapsed_seconds", m);
    m = lo_message_new(); lo_message_add_float(m, status->seq_transport_state); lo_bundle_add_message(b, "/hds/status/seq_transport_state", m);
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) {
        char addr[40];
        snprintf(addr, sizeof(addr), "/hds/status/ext_in_peak/%d", p);
        m = lo_message_new(); lo_message_add_float(m, status->ext_in_peak[p]); lo_bundle_add_message(b, addr, m);
    }
    m = lo_message_new(); lo_message_add_float(m, status->midi_clock_locked); lo_bundle_add_message(b, "/hds/status/midi_clock_locked", m);
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b); // frees the bundle AND every message added to it
}

// Per-lane "Load Audio" feedback (see g_seq_audio/poll_seq_audio_load_requests). Separate from
// send_osc_status()'s fixed-shape bundle above since this is a variable-length, mostly-empty
// set -- only lanes with a non-idle load_state (at most SEQ_LANE_COUNT, in practice usually 0)
// get a message, so an unused Sequencer session costs nothing extra on this reply port. Sent at
// the same vu_decimator cadence as send_osc_status(), from the same call site.
static void send_seq_audio_status(HdsStreamsState *streams, lo_address reply_addr) {
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    bool any = false;
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        int state = atomic_load_explicit(&streams->seq_audio[l].load_state, memory_order_acquire);
        if (state == SEQ_AUDIO_EMPTY) continue;
        any = true;
        char addr[64];
        snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/state", l);
        lo_message m = lo_message_new(); lo_message_add_float(m, (float)state); lo_bundle_add_message(b, addr, m);
        if (state == SEQ_AUDIO_READY) {
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/frames", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].frame_count); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/channels", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].channels); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/sample_rate", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].sample_rate); lo_bundle_add_message(b, addr, m);
        }
    }
    if (any) lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
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
    // --output-device/--input-device: override the "default" ALSA device name
    // init_default_controls() otherwise starts with. Exists specifically because an output
    // device that fails to open is FATAL at startup (see the open_pcm_stream() call below --
    // the process exits before the OSC server has a chance to receive any /hds/device/output
    // message that could fix it live) -- without this flag, a machine whose "default" device
    // doesn't work at all (e.g. a fresh headless Pi with a USB audio interface that ALSA's
    // dmix/"default" plugin chain can't open cleanly) has no way to even get the process to
    // start long enough to be pointed at a working device name from a connected frontend.
    // `aplay -l`/`arecord -l` on the target machine lists the real card/device numbers to try
    // here (e.g. `--output-device hw:1,0` or `--output-device plughw:1,0`).
    const char *cli_output_device = NULL;
    const char *cli_input_device = NULL;
    // --backend alsa|pipewire: chooses the audio platform shim's backend (see the "Audio platform
    // shim" section above) once at startup -- ALSA remains the default, unchanged. "pipewire"
    // opens a native libpipewire client stream instead (only available in a build where the
    // Makefile's pkg-config check found libpipewire-0.3 -- see HDS_PIPEWIRE_ENABLED), talking to
    // the PipeWire daemon directly rather than through the ALSA "pipewire" plugin, so it works
    // even when that plugin itself fails to load. Device names mean different things per backend:
    // an ALSA device string (`aplay -l`-style, e.g. "hw:1,0") under --backend alsa, or a PipeWire
    // target node name (`wpctl status`-style) -- or "default" for PipeWire's own session-manager
    // default -- under --backend pipewire.
    const char *cli_backend = "alsa";
    // --sfz-vco1 <path> .. --sfz-vco4 <path>: load a multisample instrument onto that oscillator
    // at startup, directly from the backend's own local filesystem -- no frontend needs to ever
    // connect for this synth to have playable sample content (see hds_load_sfz_for_osc() above
    // for the shared loading core this and /hds/zone/load_sfz both call).
    const char *cli_sfz_path[4] = { NULL, NULL, NULL, NULL };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--osc-listen-port") == 0 && i + 1 < argc) osc_listen_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--osc-reply-host") == 0 && i + 1 < argc) osc_reply_host = argv[++i];
        else if (strcmp(argv[i], "--osc-reply-port") == 0 && i + 1 < argc) osc_reply_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--output-device") == 0 && i + 1 < argc) cli_output_device = argv[++i];
        else if (strcmp(argv[i], "--input-device") == 0 && i + 1 < argc) cli_input_device = argv[++i];
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) cli_backend = argv[++i];
        else if (strcmp(argv[i], "--pipewire-force-quantum") == 0) g_pipewire_force_quantum = true;
        else if (strcmp(argv[i], "--sfz-vco1") == 0 && i + 1 < argc) cli_sfz_path[0] = argv[++i];
        else if (strcmp(argv[i], "--sfz-vco2") == 0 && i + 1 < argc) cli_sfz_path[1] = argv[++i];
        else if (strcmp(argv[i], "--sfz-vco3") == 0 && i + 1 < argc) cli_sfz_path[2] = argv[++i];
        else if (strcmp(argv[i], "--sfz-vco4") == 0 && i + 1 < argc) cli_sfz_path[3] = argv[++i];
    }

    if (strcmp(cli_backend, "pipewire") == 0) {
#if HDS_PIPEWIRE_ENABLED
        g_audio_backend = HDS_BACKEND_PIPEWIRE;
        pw_init(&argc, &argv);
#else
        fprintf(stderr, "This build has no PipeWire support (the Makefile's pkg-config check didn't find "
                         "libpipewire-0.3 at build time) -- rebuild inside nix-shell (see shell.nix), install "
                         "libpipewire-0.3-dev and rebuild, or drop --backend pipewire to use ALSA.\n");
        return 1;
#endif
    } else if (strcmp(cli_backend, "alsa") != 0) {
        fprintf(stderr, "Unknown --backend '%s' -- expected 'alsa' or 'pipewire'.\n", cli_backend);
        return 1;
    }

    SynthSharedControls active_knobs; init_default_controls(&active_knobs);
    if (cli_output_device) snprintf(active_knobs.output_device, AUDIO_DEVICE_NAME_LEN, "%s", cli_output_device);
    if (cli_input_device) snprintf(active_knobs.input_device, AUDIO_DEVICE_NAME_LEN, "%s", cli_input_device);

    // Non-threaded lo_server, polled once per audio block from the main loop below (see the
    // "while (lo_server_recv_noblock(...))" call site) -- deliberately NOT lo_server_thread, so
    // only this thread ever touches active_knobs/g_sample_slots, exactly like the poll_*_socket()
    // functions this replaces. The zone methods are registered with explicit type strings (liblo
    // tries the most specific registration first), the knob table's generic method last with
    // path=NULL/type=NULL as the catch-all for everything else.
    build_osc_dispatch_table(); // once, before any message can arrive -- see its own comment for why
    char osc_listen_port_str[16]; snprintf(osc_listen_port_str, sizeof(osc_listen_port_str), "%d", osc_listen_port);
    lo_server osc_server = lo_server_new_with_proto(osc_listen_port_str, LO_UDP, osc_error_handler);
    if (!osc_server) { fprintf(stderr, "Cannot continue without an OSC listen socket on port %d.\n", osc_listen_port); return 1; }
    lo_server_add_method(osc_server, "/hds/zone/upsert", "i" "fffffffffffffffffff" "s", zone_upsert_handler, &g_sample_engine);
    lo_server_add_method(osc_server, "/hds/zone/remove", "i", zone_remove_handler, &g_sample_engine);
    lo_server_add_method(osc_server, "/hds/zone/load_sfz", "iis", zone_load_sfz_handler, &g_sample_engine);
    lo_server_add_method(osc_server, "/hds/song/save_midi", "s", song_save_midi_handler, &g_engine);
    lo_server_add_method(osc_server, "/hds/song/load_midi", "s", song_load_midi_handler, &g_engine);
    lo_server_add_method(osc_server, "/hds/tuning/load_scl", "s", tuning_load_scl_handler, NULL);
    // generic_osc_handler (registered below, once every specific-path method above AND below is
    // registered) is a real catch-all: NULL path, NULL types, matching literally everything. Bug
    // fix: liblo tries a message's matching methods in REGISTRATION ORDER and stops at the first
    // one that returns 0 ("fully handled") -- it does NOT prefer a more specific path match over
    // a less specific one registered earlier. This catch-all used to be registered HERE, before
    // /hds/request_state and the two /hds/song/import_smf* handlers below -- meaning every single
    // message to any of those three addresses matched THIS handler first, which (correctly, from
    // its own perspective) treats an address absent from osc_dispatch_table as "unrecognized,"
    // logs it, and returns 0 -- permanently preventing liblo from EVER trying the real, specific
    // handler for that address. /hds/request_state -- the entire "pull this instance's live state
    // on connect" mechanism a DAW-restored CLAP patch depends on -- was silently, deterministically
    // dead code from the moment it was added: not a race, not host-dependent, never worked once.
    // Registering this catch-all LAST (after every specific method, including the three below)
    // fixes it: liblo now reaches the specific handler for any of those exact addresses first,
    // and only truly falls through to this catch-all for the ~1150 plain knob addresses that have
    // no specific method of their own -- exactly its original intent.

    char osc_reply_port_str[16]; snprintf(osc_reply_port_str, sizeof(osc_reply_port_str), "%d", osc_reply_port);
    lo_address osc_reply_addr = lo_address_new(osc_reply_host, osc_reply_port_str);
    fprintf(stderr, "[osc] listening on UDP port %d, status replies to %s:%d\n", osc_listen_port, osc_reply_host, osc_reply_port);

    // "/hds/request_state" -- registered here (not alongside the other methods above) purely by
    // convention grouping, not because it still needs osc_reply_addr: it (and the two SMF import
    // handlers below) now reply directly to whoever sent the request (lo_message_get_source), not
    // a fixed configured target -- see request_state_handler's own comment on why. state_request_ctx
    // has this function's own full lifetime (never returns before shutdown), so a pointer to it
    // stays valid for as long as osc_server itself does.
    HdsStateRequestContext state_request_ctx = { &active_knobs };
    lo_server_add_method(osc_server, "/hds/request_state", "", request_state_handler, &state_request_ctx);

    // "/hds/song/import_smf" -- see request_state's own comment just above on reply-to-sender.
    HdsSmfImportContext smf_import_ctx = { &g_engine, &active_knobs };
    lo_server_add_method(osc_server, "/hds/song/import_smf", "sf", song_import_smf_handler, &smf_import_ctx);

    // "/hds/song/import_smf_track" -- per-lane "Load MIDI..." (Sequencer tab), same reply-to-sender
    // reasoning as the two contexts just above.
    HdsSmfTrackImportContext smf_import_track_ctx = { &g_engine };
    lo_server_add_method(osc_server, "/hds/song/import_smf_track", "sf", song_import_smf_track_handler, &smf_import_track_ctx);

    // "/hds/ping" and "/hds/subscribe" -- Instance Switcher support, see ping_handler()/
    // subscribe_handler()'s own comments. subscribe_ctx.target points directly at osc_reply_addr
    // itself (declared just above, same function scope, same lifetime as osc_server) -- the exact
    // variable send_osc_status()/send_seq_audio_status() already read from at their own call sites
    // further down this loop, so subscribing needs no changes there at all.
    lo_server_add_method(osc_server, "/hds/ping", "", ping_handler, &active_knobs);
    HdsSubscribeContext subscribe_ctx = { &osc_reply_addr };
    lo_server_add_method(osc_server, "/hds/subscribe", "", subscribe_handler, &subscribe_ctx);

    // The real catch-all, registered LAST -- see its own comment above (right after the
    // /hds/tuning/load_scl registration) for why registration order here is load-bearing.
    lo_server_add_method(osc_server, NULL, NULL, generic_osc_handler, &active_knobs);

    // --sfz-vcoN startup loading: right here, after the OSC methods are registered (so a
    // subsequent /hds/zone/load_sfz can still layer on top live) but before open_pcm_stream()
    // opens the real-time audio device -- hds_load_sfz_for_osc() only does text parsing + path/
    // stat() resolution + one header-only WAV read per region (no PCM decode, which stays
    // throttled through drain_pending_sample_loads() at SAMPLE_LOADS_PER_BLOCK per block, same as
    // any other zone load), so even a large multisample library enqueues fast and never risks
    // blocking a thread that has real-time constraints. A bad/missing path logs and continues
    // starting -- never fatal, matching load_sample_slot()'s own established failure posture.
    for (int i = 0; i < 4; i++) {
        if (!cli_sfz_path[i]) continue;
        int n = hds_load_sfz_for_osc(&g_sample_engine, i, SINGLE_PROGRAM_SLOT, cli_sfz_path[i]);
        fprintf(stderr, "[sfz] --sfz-vco%d '%s': %d zone(s) queued\n", i + 1, cli_sfz_path[i], n);
    }

    // Output: opened with however many channels query_output_device_channel_count() detects --
    // see the multi-source routing model's output-pair grid (SynthSharedControls.source_routing)
    // -- not hardcoded stereo, so a class-compliant multichannel interface's extra outputs are
    // actually reachable, not just its first pair. Symmetric with the input side just below.
    int detected_output_channels = query_output_device_channel_count(active_knobs.output_device);
    HdsPcmStream *pcm_handle = NULL;
    if (!open_pcm_stream(active_knobs.output_device, HDS_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels)) {
        // Rather than exiting immediately, offer an interactive way out -- but only when stdin is
        // actually a terminal (a non-interactive launch, e.g. a systemd unit or CI, can never
        // answer a prompt and would otherwise hang forever on fgets() waiting for input that will
        // never come). See interactive_recover_output_device()'s own comment for the full story.
        bool opened = false;
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "Could not open the configured output device ('%s').\n", active_knobs.output_device);
            opened = interactive_recover_output_device(&active_knobs, &pcm_handle, &detected_output_channels);
        }
        if (!opened) {
            fprintf(stderr, "Cannot continue without an output device.\n");
            lo_address_free(osc_reply_addr); lo_server_free(osc_server); return 1;
        }
    }
    char applied_output_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);

    // External Audio Input: best-effort. Unlike output, a capture device failing to open isn't
    // fatal -- external input just stays silent (ext_in_enable has nothing to mix in) until a
    // working device name is picked from the Audio tab. Opened with however many channels
    // query_device_channel_count() detects -- see the Recording tab's per-channel checkboxes --
    // not hardcoded stereo, so a class-compliant multichannel interface's extra inputs are
    // actually reachable, not just its first two.
    HdsPcmStream *pcm_capture_handle = NULL;
    int detected_input_channels = query_device_channel_count(active_knobs.input_device);
    open_pcm_stream(active_knobs.input_device, HDS_STREAM_CAPTURE, &pcm_capture_handle, active_knobs.audio_latency_ms, detected_input_channels);
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
    bool sample_disk_thread_ok = (pthread_create(&g_sample_disk_thread, NULL, sample_disk_thread_main, &g_sample_engine) == 0);
    if (!sample_disk_thread_ok) {
        fprintf(stderr, "[sample] failed to start disk streaming thread -- WAVE_SAMPLE playback will be unavailable this session\n");
    }

    snd_seq_t *seq_handle;
    int seq_err = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_INPUT, 0);
    if (seq_err < 0) {
        fprintf(stderr, "MIDI sequencer failed to open: %s\n", snd_strerror(seq_err));
        close_pcm_stream(pcm_handle); if (pcm_capture_handle) close_pcm_stream(pcm_capture_handle);
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
    midi_autoconnect_scan(seq_handle, our_midi_port, active_knobs.midi_device, &midi_sources);
    MidiLiveState midi_live = {0}; // pitch_bend/channel_pressure/cc[] all start at their "centered/off" 0.0
    char applied_midi_device[AUDIO_DEVICE_NAME_LEN];
    snprintf(applied_midi_device, sizeof(applied_midi_device), "%s", active_knobs.midi_device);

    float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT] = {{0.0f}}; int vu_decimator = 0;

    SynthVoice voices[MAX_VOICES]; for (int i = 0; i < MAX_VOICES; i++) init_voice(&voices[i]);
    MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1]; for (int mp = 0; mp <= COMBI_PROGRAM_COUNT; mp++) mono_stacks[mp] = (MonoNoteStack){0}; // index COMBI_PROGRAM_COUNT = SINGLE_PROGRAM_SLOT

    while (g_synth_running) {
        // Drain every pending OSC datagram this block -- generic_osc_handler/zone_upsert_handler/
        // zone_remove_handler write directly into active_knobs/g_sample_slots as each message is
        // dispatched, so nothing further is needed here once this returns 0 (no more pending).
        while (lo_server_recv_noblock(osc_server, 0) > 0) {}
        poll_seq_audio_load_requests(&g_streams_state, &active_knobs); // spawns a loader thread if a lane's audio_path changed
#if HDS_NAM_ENABLED
        poll_fx_nam_requests(g_engine.fx_engine, &active_knobs); // (re)loads a slot's .nam model/cab IR if its path changed
#endif
        poll_seq_audio_preview_triggers(&g_streams_state, &active_knobs); // starts one-shot playback if a lane's Preview button was clicked

        // Programmable tempo: resolved once per block from the Tempo knob/tempo-map (see
        // compute_current_tempo_bpm()) against the Sequencer's own playhead position
        // (g_transport_playhead_sec), matching poll_arpeggiator()'s own independent resolution of
        // the same thing. FIXED: this used to interpolate against the engine's running uptime
        // instead -- harmless as long as tempo_marker_count was always 0 (the only case that ever
        // existed before a real tempo MAP could be populated, e.g. by importing a Standard MIDI
        // File's own tempo track -- see hds_song_import_smf()), but wrong the moment it isn't: a
        // song's tempo map is inherently relative to SONG position, not to how long this process
        // happens to have been running. Keyed to uptime, the map would read a different tempo
        // every time depending purely on when Play was pressed relative to launch, and permanently
        // drift out of step with the actual performance on every Stop/Play restart or Seek (the
        // playhead resets/jumps; uptime doesn't). Computed here, before poll_transport_triggers,
        // so the Seek Back/Forward-one-measure triggers can use it too (a measure = 4 beats at
        // whatever tempo is "current" right now).
        // External MIDI Clock sync (see SynthSharedControls.seq_sync_source): substitutes the
        // clock-derived BPM for the Tempo knob/tempo-map's own result at this single resolution
        // point -- every downstream consumer (Arp, LFO Tempo Sync, Quantize, the metronome, the
        // Seek Back/Forward measure calc) already just takes whatever current_tempo_bpm resolves
        // to here, so nothing else needs to know or care which source it came from. Falls back to
        // Internal automatically if no clock tick has arrived recently (a disconnected/unplugged
        // external source degrades to the Tempo knob rather than freezing at a stale BPM).
        bool use_external_clock = (active_knobs.seq_sync_source != 0.0f) && g_engine.midi_clock_valid
            && (hds_monotonic_seconds() - g_engine.midi_clock_last_tick_sec) < HDS_MIDI_CLOCK_TIMEOUT_SEC;
        float current_tempo_bpm = use_external_clock ? g_engine.midi_clock_bpm : compute_current_tempo_bpm(&active_knobs, g_transport_playhead_sec);

        poll_transport_triggers(&g_streams_state, &g_engine, &active_knobs, current_tempo_bpm); // Play/Pause/Stop/Seek -- starts/freezes/resets/repositions the real Sequencer playhead
        poll_metronome(&g_engine, &active_knobs, current_tempo_bpm); // schedules the next click, if engaged and PLAYING
        poll_seq_audio_transport_catchup(&g_streams_state, &g_engine, &active_knobs); // starts any lane whose load finished after Play was already pressed

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
            if (open_pcm_stream(active_knobs.output_device, HDS_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, candidate_output_channels)) {
                detected_output_channels = candidate_output_channels;
                snprintf(applied_output_device, sizeof(applied_output_device), "%s", active_knobs.output_device);
            } else {
                // Failed -- fall back to the last device that was actually working rather than
                // leaving pcm_handle NULL, and stop retrying this same bad name every block.
                open_pcm_stream(applied_output_device, HDS_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels);
                snprintf(active_knobs.output_device, AUDIO_DEVICE_NAME_LEN, "%s", applied_output_device);
            }
        } else if (latency_changed) {
            // Latency-only change with no valid device name yet -- still worth reapplying to
            // whatever's already open rather than silently doing nothing.
            open_pcm_stream(applied_output_device, HDS_STREAM_PLAYBACK, &pcm_handle, active_knobs.audio_latency_ms, detected_output_channels);
        }
        bool input_device_valid = active_knobs.input_device[0] != '\0';
        if (input_device_valid && (latency_changed || strncmp(active_knobs.input_device, applied_input_device, AUDIO_DEVICE_NAME_LEN) != 0)) {
            // Re-detect channel count too -- a device switch can mean a completely different
            // interface with a different channel count, not just a renamed same one.
            detected_input_channels = query_device_channel_count(active_knobs.input_device);
            open_pcm_stream(active_knobs.input_device, HDS_STREAM_CAPTURE, &pcm_capture_handle, active_knobs.audio_latency_ms, detected_input_channels); // best-effort
            snprintf(applied_input_device, sizeof(applied_input_device), "%s", active_knobs.input_device);
        }
        if (latency_changed) { applied_audio_latency_ms = active_knobs.audio_latency_ms; }
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
        drain_pending_sample_loads(&g_sample_engine);

        process_midi_input(&g_streams_state, &g_engine, &g_sample_engine, seq_handle, voices, &active_knobs, global_lfo_phases, mono_stacks, our_midi_port, &midi_sources, &midi_live, current_tempo_bpm);
        seq_playback_midi_events(&g_engine, &g_sample_engine, voices, &active_knobs, global_lfo_phases, &mono_stacks[SINGLE_PROGRAM_SLOT], &midi_live); // fires any Seq-mode lane's recorded events reached by the playhead (always targets SINGLE_PROGRAM_SLOT)

        // block_size/voice_limit are computed fresh every iteration since both are live,
        // user-adjustable controls (Audio tab: Block Size, Polyphony) -- see
        // effective_block_size()/effective_voice_limit(). capture_buffer/buffer stay declared
        // at the hard BUFFER_SIZE ceiling; only the first block_size frames of each are
        // actually read/written/rendered this iteration.
        int block_size = effective_block_size(&active_knobs);
        advance_transport_playhead(&g_engine, block_size); // advances the real Sequencer playhead by exactly this block's worth of time, only while PLAYING
        poll_arpeggiator(&g_engine, &g_sample_engine, voices, &active_knobs, global_lfo_phases, mono_stacks, &midi_live, block_size); // fires the next arp step(s) for every Program with arp_enabled, independent of Sequencer transport state

        // Read one block of external input, ALL detected_input_channels of it -- interleaved,
        // stride detected_input_channels per frame (not always 2 anymore; see
        // query_device_channel_count()). Silence (the zero-init) if there's no working capture
        // device, or a read comes back short/broken -- external input degrading to silence
        // should never be able to stall or crash the synth's own audio output.
        static float capture_buffer[BUFFER_SIZE * RECORD_MAX_CHANNELS];
        memset(capture_buffer, 0, sizeof(float) * (size_t)block_size * (size_t)detected_input_channels);
        if (pcm_capture_handle) {
            uint32_t got = read_capture_block(pcm_capture_handle, capture_buffer, detected_input_channels, (uint32_t)block_size);
            if (got < (uint32_t)block_size) {
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
        float ext_in_peak[EXT_IN_PAIR_COUNT];
        float block_peak = render_audio_block(&g_sample_engine, &g_engine, voices, &active_knobs, global_lfo_phases, capture_buffer, detected_input_channels, detected_output_channels, &midi_live, buffer, current_tempo_bpm, ext_in_peak, NULL);
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
                .seq_elapsed_seconds = (float)g_transport_playhead_sec,
                .seq_transport_state = (float)g_transport_state,
            };
            for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) status.ext_in_peak[p] = ext_in_peak[p];
            status.midi_clock_locked = use_external_clock ? 1.0f : 0.0f;
            send_osc_status(osc_reply_addr, &status);
            send_seq_audio_status(&g_streams_state, osc_reply_addr);
            vu_decimator = 0;
        }
        write_audio_block(pcm_handle, buffer, detected_output_channels, block_size, active_voices_count, render_ms);
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
    // Best-effort: seq-audio loader threads are one-shot/detached (see poll_seq_audio_load_
    // requests), not joined here -- the process is exiting either way, so this is tidiness for a
    // leak checker, not a safety requirement.
    for (int l = 0; l < SEQ_LANE_COUNT; l++) free(g_seq_audio[l].pcm);
    lo_server_free(osc_server);
    lo_address_free(osc_reply_addr);
    snd_seq_close(seq_handle);
    close_pcm_stream(pcm_handle);
    if (pcm_capture_handle) close_pcm_stream(pcm_capture_handle);
#if HDS_PIPEWIRE_ENABLED
    if (g_audio_backend == HDS_BACKEND_PIPEWIRE) pw_deinit();
#endif
    return 0;
}
