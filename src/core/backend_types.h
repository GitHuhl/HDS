// backend_types.h -- the synth engine's core data model: build-wide limits (voice/LFO/EG/slot
// counts), ProgramState (one Program's patch), SynthSharedControls (every user-facing control the
// OSC table and GUIs read/write), SynthVoice/ADSR/EGDerivedRates (per-voice runtime state), and the
// small types they're built from. Moved verbatim out of backend.c as the first step toward giving
// backend.c a real header and compiling it as its own object; still only included via backend.c.
#ifndef HDS_BACKEND_TYPES_H
#define HDS_BACKEND_TYPES_H

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hds_config.h" // SAMPLE_RATE, BUFFER_SIZE
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
// Drum Buses: DRUM_BUS_COUNT independent, session-level audio buses a WAVE_SAMPLE zone can be
// routed DIRECTLY into (SampleSlot.drum_bus), bypassing that oscillator's own Volume/Mute/Pan
// knobs AND its Program's own Filter chain/post-filter EQ entirely -- only the zone's own gain_db/
// pan and its own amp EG still apply (see render_audio_block's own drum-bus mixing comment). The
// point: routing individual drum-kit hits (kick, snare, hats, ...) on ONE Multisample oscillator to
// independent FX destinations without needing a per-key filter/pan/EQ routing matrix -- a zone
// that never sets drum_bus (0, the default) is completely unaffected, playing through the normal
// oscillator pipeline exactly as before. Appended to SOURCE_COUNT (see its own updated comment)
// rather than built as a parallel system, so a Drum Bus is immediately usable as any FX slot's own
// fx_source and in the multi-output source_routing grid, exactly like a Program or ext-in pair --
// no new FX-side machinery needed at all.
#define DRUM_BUS_COUNT 24
// BREAKING CHANGE (real Combi mode): SOURCE_COUNT grows from 9 (1 Program + 8 ext-in) to 17, and
// existing source ids are RENUMBERED -- ext-in pairs move from ids 1-8 to 9-16, since ids 0-7 are
// now the 8 Combi Programs (0 = today's single Program, byte-identical) and id 8 is the new
// "Combi Mix" bus (sum of all 8 Programs' raw dry pre-FX output -- lets one FX slot serially
// process the whole stack while other slots still individually target a single Program,
// deliberately allowed to overlap, per "let them route"). Any existing fx_source/routing
// configuration referencing ext-in pairs by their old 1-8 ids now points at the wrong source and
// must be re-set -- the same category of break this file's own running BREAKING CHANGE log has
// documented for every prior wire-format-shape change.
// BREAKING CHANGE (Drum Buses): SOURCE_COUNT grows again, from 17 to 17+DRUM_BUS_COUNT (41) --
// DRUM_BUS_COUNT new source ids appended AFTER every ext-in pair (ids 17..40), never inserted
// earlier in the list, so no existing Program/Combi-Mix/ext-in id is renumbered by this change.
// See DRUM_BUS_SOURCE_BASE for the exact base id and DRUM_BUS_COUNT's own comment for what these
// new sources actually carry.
#define SOURCE_COUNT (COMBI_PROGRAM_COUNT + 1 + EXT_IN_PAIR_COUNT + DRUM_BUS_COUNT)
// The first Drum Bus's own source id (Drum Bus b, 0-based, is source id DRUM_BUS_SOURCE_BASE + b)
// -- shared by every site that needs to convert between a raw SampleSlot.drum_bus value (1-based,
// 0 = off) and its own source-array index.
#define DRUM_BUS_SOURCE_BASE (COMBI_PROGRAM_COUNT + 1 + EXT_IN_PAIR_COUNT)
#define OUTPUT_MAX_CHANNELS 16
#define OUTPUT_PAIR_MAX (OUTPUT_MAX_CHANNELS / 2)
#define AUDIO_DEVICE_NAME_LEN 64
#include "core/recording_engine.h" // RECORD_MAX_CHANNELS, RECORD_PATH_LEN, RecordEngine
// Programmable tempo: hard ceiling on how many (time, bpm) tempo markers the tempo-map can hold
// at once -- see the Tempo section on SynthSharedControls and compute_current_tempo_bpm().
#define TEMPO_MARKER_MAX 64 // 32 distinct tempos from an imported MIDI file (each change is a 2-marker step)
// DFD (direct-from-disk) sample playback: the Mapping tab's Sample Bank has no product-facing
// cap on how many zones can be loaded -- the catalog now travels over its own dedicated,
// event-driven socket (see zone_upsert_handler()/zone_remove_handler()), not the
// fixed SynthSharedControls struct, specifically so it can scale with a real multisample
// instrument's zone count instead of a compile-time constant. g_sample_slots[] is still a single
// preallocated-once array sized SAMPLE_SLOT_CAPACITY, though, rather than a realloc()'d/growable
// one: the GUI and the CLAP state code reach into it by index from other threads, which is only
// safe because that array's base address never moves for the life of the process (the disk
// thread itself no longer reads it -- see SampleStream.claim_path). SAMPLE_SLOT_CAPACITY is deliberately huge
// (~34MB total at ~340 bytes/slot) so it is never the real constraint in practice, while keeping
// the "never moves" safety property. See the Sample Bank section on SynthSharedControls and
// find_matching_sample_slot().
#define SAMPLE_SLOT_CAPACITY 100000
// SAMPLE_PATH_LEN lives in hds_config.h (shared with fx_types.h).
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
    WAVE_OFF, // Compute-bypass: skips pan trig + render + mix for this oscillator. Phase still
              // advances and its envelope still ticks (Hard Sync/FM and voice-idle logic depend on
              // both staying live), so nothing downstream needs to special-case an Off oscillator.
    WAVE_REVSAW // Reverse (descending-ramp) sawtooth -- appended after WAVE_OFF (not inserted next
                // to WAVE_SAW) for the same reason WAVE_PULSE/WAVE_SAMPLE were appended: existing
                // saved patches' numeric shape indices, WAVE_OFF's own included, must never shift.
                // A plain sawtooth and its reverse are spectrally identical (every harmonic has the
                // same magnitude, just phase-inverted) -- inaudibly different alone, on a single
                // oscillator, by design (human hearing is essentially deaf to a steady tone's inter-
                // harmonic phase). It matters the moment a SECOND thing enters the picture: (1) two
                // oscillators, one Saw and one Rev Saw, summed -- perfectly in phase they cancel to
                // silence (they're exact negatives of each other), but at any OTHER phase offset
                // between them their equal-and-opposite slopes make the sum's slope zero everywhere
                // except at the two (now separated) discontinuities, producing an exact two-level
                // rectangle/pulse wave whose duty cycle is set by that offset -- 180 degrees gives a
                // symmetric square, other offsets give asymmetric PWM, and detuning one against the
                // other continuously sweeps it (the mechanism a real subtractive synth's "two saws"
                // trick and analog PWM-via-two-ramps both actually rely on -- verified here by
                // direct algebra, not folklore); (2) hard sync, where the reset instant lands on a
                // different point of a reverse ramp than a forward one, giving a distinct sync-sweep
                // timbre (see this WaveType's own case in render_fm_oscillator's hard-sync block);
                // (3) as an LFO/mod source (see lookup_lfo_waveform's new case 5) driving something
                // like filter cutoff, where a rising vs. falling ramp gives an audibly OPPOSITE
                // contour (snap-then-rise vs. rise-then-snap) even though neither would sound
                // different as a lone audio tone -- the classic reason hardware like the Moog Model
                // D/Roland SE-02 offers a dedicated reverse-saw option on a modulation oscillator.
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
    // All ignored entirely in Combi mode -- performance_mode==1's own render path (see the "Combi
    // lane Level/Pan" comment near active_program_count) never reads seq_mode/rec_armed/mute at
    // all; only the Sequencer tab's own transport-driven playback (sequencer.c) does.
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

// FXSlotParams/FXSlotEngine/FX_TYPE_*/FX_SLOT_PARAM_COUNT -- the FX slot data model, shared with
// src/hds_fx_plugin.c (the standalone HDS FX CLAP plugin), so it now lives in its own header --
// see fx_types.h's own header comment.
#include "fx/fx_types.h"

// One source's assignment onto the hardware output pairs -- a many-to-many grid (a source can
// feed multiple output pairs at once, e.g. Program to both a main mix and a monitor/cue pair).
// Wrapped in its own struct (rather than a bare 2D array directly on SynthSharedControls) purely
// so the existing OSC_NESTED macro -- which addresses an array-of-structs' own array field via
// offsetof -- applies unmodified; see OSC_PARAMS[]'s "/hds/routing/%d/out/%d" entry.
typedef struct {
    float enable[OUTPUT_PAIR_MAX];
} SourceRouting;

// ProgramState's own field layout below grew incrementally over many passes (4-oscillator
// architecture, filter overhaul, FX bus, assignable EGs, Recording tab, Sample Bank zone mapping,
// Sampler loop/round-robin, uncapped Sample Bank, 4-osc+FM+HardSync+16LFO consolidation, ...) --
// see git history for the field-by-field record of each change, back when this struct's exact
// byte layout had to match a hand-maintained Python struct.pack/unpack format one field at a time.
// That constraint is gone now: every field is addressed by name over OSC (OSC_PARAMS[]), so
// nothing on either end needs to agree on byte order or position, only on the address string.
// One independently-synthesizing Program (oscillators, filters, EGs, LFOs, FM, tempo -- "the
// patch"): every field that belongs to a single patch's own Save/Load boundary lives here, in one
// place, so nothing that ever reads or writes a .hdsprog file needs to re-derive that boundary
// itself (see hds_load_program_file()'s own comment on the current reader side -- Save is still a
// pending native-GUI feature, see README). Wrapping these fields in a
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
// FX slots (fx[]/source_routing below), ext-in, and every other rig/
// session-level field deliberately stay flat on SynthSharedControls, NOT in this struct -- those
// are shared, physical-insert-like resources any Program (or the whole Combi stack) can be routed
// into via fx_source, not part of any one patch -- a deliberate refinement over an FX slot's
// type/mix/bypass/params ever being treated as Program-scoped, a holdover from when there was only
// ever one Program and the FX pool WAS that Program's own FX -- see the "Real Combi mode" plan.
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

    // SFZ per-zone amp envelope override, one checkbox per oscillator (shown in the native GUI
    // only while that oscillator's own Shape is Multisample/WAVE_SAMPLE -- irrelevant, and hidden,
    // otherwise). Strictly EITHER/OR, never a blend of the two (explicit user direction): OFF
    // (0, the default) means a currently-playing zone that sets ANY ampeg_* opcode of its own
    // (SampleSlot.has_own_ampeg) uses that envelope ENTIRELY, with SFZ's own per-stage defaults for
    // whatever it doesn't set -- this Program's own eg[0..3]/eg_mode[0..3] knobs are not consulted
    // at all for that note. ON (1) makes this oscillator ALWAYS use this Program's own Amp EG
    // (eg[N]/eg_mode[N]) for every note, exactly as if the zone had no ampeg_* opcodes at all,
    // regardless of what its own SFZ actually declares. A zone that sets NO ampeg_* opcodes at all
    // is completely unaffected by this switch either way -- the Program's own Amp EG is what plays
    // it, exactly as it always has. See compute_zone_owned_eg_rates()/handle_note_on's own
    // eg-init block and the tick_adsr call sites in render_audio_block for where this is actually
    // consulted.
    float vco1_force_own_amp_eg;
    float vco2_force_own_amp_eg;
    float vco3_force_own_amp_eg;
    float vco4_force_own_amp_eg;

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
    // own vel_track knob). 0 (default) means every velocity plays at this oscillator's full,
    // undiminished output level (as if velocity were always 127) -- a note's OWN velocity has zero
    // effect. Increasing the knob toward +100 progressively pulls the BOTTOM of the dynamic range
    // down (velocity 127 always stays at that same full level regardless of depth; only softer
    // hits get quieter, down to silence at velocity 0 and depth 100 -- an ordinary linear velocity
    // curve). Negative depth mirrors this around the OTHER end instead: velocity 0 always stays at
    // full level, and it's now the hardest hits that get pulled down toward silence as depth
    // approaches -100. Deliberately per-oscillator rather than one global depth: since all 4
    // oscillators already read the SAME note's velocity (MIDI gives one velocity per note-on, not
    // per-oscillator), independent depths let a patch velocity-crossfade BETWEEN oscillator layers
    // -- e.g. a sub layer staying constant while a brighter overtone layer fades in on harder hits
    // -- not just scale the whole voice uniformly. Applied as a per-voice gain multiplier alongside
    // each oscillator's own Amp EG in render_audio_block (see the vel_amp_mult_arr computation
    // there), not as a mod-matrix destination -- velocity is a one-shot value captured at note-on
    // (voice->midi_velocity), not a continuously-varying signal like an LFO/EG, so it doesn't fit
    // that per-sample-modulated system.
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
    // whatever the performer set it to.
    //
    // Applied by dispatch_midi_event() at the very top of its own NOTE_ON/NOTE_OFF handling --
    // added directly to the raw incoming MIDI note number, before ANY downstream use (Combi
    // key-range gating, Arp, handle_note_on/off, Sequencer recording) -- rather than as a live
    // audio-rate cents offset the way pitch bend is. This used to be a cents offset added
    // alongside pitch bend in mod_vco{1-4}_pitch below, which genuinely transposes a synthesized
    // oscillator, but for a Multisample oscillator it's a pure playback-RATE change: the ORIGINAL
    // note's sample zone stays selected and just gets sped up/slowed down (a chipmunk/formant
    // shift), not replaced with the correct zone for the new pitch the way a real transposed key
    // press would be. Moving the offset onto the note number itself fixes that -- the whole
    // engine, sample-zone lookup included, now genuinely believes a different key was pressed --
    // at the cost of no longer retuning already-held notes live (a real hardware Transpose button
    // doesn't either: it changes what note the NEXT key press sends, not notes already sounding).
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
    // Explicit per-pair Mono/Stereo choice (default: Stereo, 1.0), mirroring HDS FX's own
    // port_input_stereo fix (see hds_fx_plugin.c's header comment, "The synth's own ext-in pairs
    // don't have this Mono/Stereo choice yet"). Before this field existed, a pair's mono-ness was
    // only ever an IMPLICIT fallback of the capture device's own channel count (see
    // render_audio_block's own comment: "if this pair's second channel doesn't exist... duplicate
    // the first into both L/R") -- there was no way to explicitly treat a pair as Mono when the
    // device genuinely does provide both channels (e.g. channel 2p+1 carries some unrelated
    // second mic/line this pair should never sum in as "R"). Stereo (1.0, default) keeps today's
    // exact behavior (read L/R, degrading to mono-duplicate only if the device is short a
    // channel); Mono (0.0) always reads channel 2p alone and centers it to both L/R of this pair's
    // bus, regardless of whether channel 2p+1 exists or what it carries.
    float ext_in_stereo[EXT_IN_PAIR_COUNT];

    // Per-Drum-Bus output gain (0.0-2.0, i.e. 0-200%, same range/UI convention as ext_in_gain
    // above) -- applied AFTER master_volume, right where drum_bus_l/r get folded into bus_l/r (see
    // render_audio_block's own Drum Bus population loop). Unlike ext_in_gain, defaults to 1.0 (unity)
    // rather than 0.0: an ext-in pair is a live hardware input that could feed back/clip the moment
    // Enable is checked, so it always starts silent -- a Drum Bus only ever makes sound because the
    // user already explicitly assigned zones to it (Key Routing grid) and routed its output
    // somewhere (Routing tab), so starting it silent on top of that would just be a second,
    // redundant "why isn't this making sound" trap.
    float drum_bus_gain[DRUM_BUS_COUNT];

    // Routing tab visibility (written directly by the native GUI's own tab-switch dispatch, see
    // hds_clap_gui_draw.c) -- gates render_audio_block's per-ext-in-pair peak metering
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

    // No separate per-source Series/Parallel routing-mode field: process_fx_chain() is always
    // called in Parallel mode (see its own call site in render_audio_block), and a slot's own
    // chain_to_next fully expresses what Series used to mean -- chaining every slot in a source's
    // chain to the next collapses Parallel's branch-averaging to a single branch (divisor 1),
    // reproducing plain Series exactly. Mirrors HDS FX's own fix (see hds_fx_plugin.c's header
    // comment: "the redundant mode switch is gone entirely") -- the shared process_fx_chain() in
    // fx_chain.c never needed two real modes at this call site, just the one chain_to_next can
    // already express fully.

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
    // mid-take never desyncs an already-open file. See wavpack_recorder_*/WavPackRecorder. Its
    // position in this struct carries no meaning -- every field is addressed by name via OSC_PARAMS'
    // own offsetof(), never by wire position.
    float record_format;

    // WavPack-specific encoding options (Recording tab) -- meaningless when record_format is WAV,
    // harmless dead floats on that path (same posture as an FX slot's own param[] entries an
    // effect that doesn't use them simply ignores). See wavpack_recorder_start()'s own comment in
    // hds_standalone.c for exactly how each maps onto libwavpack's WavpackConfig.
    //
    // 0 = Lossless (default -- the same bit-exact guarantee WAV itself gives, just compressed).
    // 1 = Hybrid: a smaller, genuinely LOSSY .wv file targeting record_wavpack_hybrid_bitrate's own
    // rate -- WITH, if record_wavpack_correction_file is also on, a second .wvc file that together
    // with the .wv reconstructs the original losslessly (the .wv alone still plays back fine on
    // its own, just lossy). This is WavPack's own well-known "hybrid" design, not something this
    // port invented -- answers "lossy/hybrid vs. lossless" as a single 2-way choice, since in
    // WavPack those really are the same underlying switch.
    float record_wavpack_hybrid;
    // Only meaningful when record_wavpack_hybrid != 0 -- see its own comment above. Also sets
    // WavPack's own CONFIG_OPTIMIZE_WVC flag automatically when on (maximizes hybrid compression
    // quality), matching what WavPack's own reference CLI (-c) already does whenever a correction
    // file is requested -- not exposed as a separate knob since it's not a real, independent choice.
    float record_wavpack_correction_file;
    // Hybrid mode's own target bitrate, in bits per sample (WavPack's own native unit for this,
    // avoiding the separate CONFIG_BITRATE_KBPS flag) -- 2.0-24.0 is a typical usable "hybrid
    // lossy" range; only meaningful when record_wavpack_hybrid != 0.
    float record_wavpack_hybrid_bitrate;
    // Compression level: 0=Fast, 1=Normal (WavPack's own default -- no flag needed), 2=High,
    // 3=Very High -- WavPack's own CONFIG_FAST_FLAG/CONFIG_HIGH_FLAG/CONFIG_VERY_HIGH_FLAG, a
    // speed/ratio tradeoff independent of hybrid vs. lossless (applies to either). Deliberately NOT
    // exposing WavPack's separate "extra processing" (-x) modes -- explicitly not intended for
    // real-time encoding (see CONFIG_EXTRA_MODE's own header comment in wavpack.h), and this
    // recorder runs on a live writer thread, same "no real-time-hostile options" posture as
    // everything else in it.
    float record_wavpack_compression_level;

    // --- Sample Bank (DFD/WAVE_SAMPLE) + Mapping tab zones ---
    // The Sample Bank itself (which files are loaded, their key/velocity zones, loop points,
    // round-robin groups) is NOT part of this struct -- it travels over its own dedicated,
    // event-driven socket instead (see zone_upsert_handler()/zone_remove_handler()
    // and the ZONE_OP_UPSERT/ZONE_OP_REMOVE messages), sent only when the catalog itself actually
    // changes, not broadcast in full on every unrelated knob turn like the fields in this struct
    // are. See g_sample_slots[]/find_matching_sample_slot() for the backend-side catalog; zones are
    // populated via the /hds/zone/* OSC messages or by loading an .sfz file directly (the native
    // GUI's own per-oscillator "Load .sfz..." button, or --sfz-vcoN at startup).

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
    // octave that lane was set to play at. Applied as an additive cents offset in mod_vco{1-4}_pitch
    // (render_audio_block) -- unlike the global transpose_semitones knob (see its own comment for
    // why THAT one moved to dispatch_midi_event instead), this one deliberately stays a live
    // cents offset: it's meant as a live layering/detune effect across already-held notes, not a
    // stand-in for "the performer pressed a different key."
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

    // A user-settable label purely for an OSC controller's own instance-discovery scan results (see
    // ping_handler()/subscribe_handler() and README's own section on this) -- lets "port 9012" show
    // up as "Track 3 Guitar Amp" instead. Session/rig identity, not patch content, so it stays
    // outside any Program's own field boundary exactly like master_volume/transpose_semitones do --
    // loading a Program should never rename the instance it's loaded into. Empty by default; a
    // controller displaying this falls back to host:port when empty.
    char instance_name[AUDIO_DEVICE_NAME_LEN];

    // Full filesystem path (not an ALSA device name, hence the bigger buffer than
    // AUDIO_DEVICE_NAME_LEN) the recorder writes to on the next record_enable 0->1 transition.
    // The frontend regenerates this to a fresh timestamped filename in the chosen Recordings
    // folder after every stop, so consecutive takes never collide/overwrite.
    char record_path[RECORD_PATH_LEN];
} SynthSharedControls;

// Everything tick_adsr() needs BESIDES the envelope instance's own dynamic state -- moved ahead of
// its original position (just below compute_eg_derived_rates(), further down) so SynthVoice can
// hold one of these directly per oscillator for the SFZ per-zone amp envelope override (see
// ProgramState.vco1_force_own_amp_eg's own comment) -- a plain forward-reference would work too,
// but C doesn't have those for typedefs, so the type itself has to move.
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

    // SFZ first-class opcode support (appended) -- one crossfade-LAYER partner stream per
    // oscillator (see find_matching_sample_slot's own comment: a zone using xfin_*/xfout_* can
    // sound simultaneously with a second, paired zone, each independently mixed by its own static
    // xfade gain captured at this note's own start_sample_osc() call) plus a delayed-start
    // countdown (SFZ's delay=, in frames at SAMPLE_RATE -- render_audio_block holds this
    // oscillator's output at silence, without touching its already-claimed stream/ptr/preroll at
    // all, until this reaches 0). -1 layer stream / gain 1.0 primary / 0.0 layer / 0 delay
    // reproduces today's exact pre-crossfade-layering, pre-delay behavior byte-for-byte for every
    // zone that never sets xfin_*/xfout_*/delay= at all -- the vast majority, by construction.
    int vco1_layer_sample_stream;      double vco1_layer_sample_ptr;      bool vco1_layer_sample_preroll;
    float vco1_xfade_gain;             float vco1_layer_xfade_gain;       int vco1_sample_delay_remaining;
    int vco2_layer_sample_stream;      double vco2_layer_sample_ptr;      bool vco2_layer_sample_preroll;
    float vco2_xfade_gain;             float vco2_layer_xfade_gain;       int vco2_sample_delay_remaining;
    int vco3_layer_sample_stream;      double vco3_layer_sample_ptr;      bool vco3_layer_sample_preroll;
    float vco3_xfade_gain;             float vco3_layer_xfade_gain;       int vco3_sample_delay_remaining;
    int vco4_layer_sample_stream;      double vco4_layer_sample_ptr;      bool vco4_layer_sample_preroll;
    float vco4_xfade_gain;             float vco4_layer_xfade_gain;       int vco4_sample_delay_remaining;

    // Per-Key LFO state (see PackLfoParams.per_key_mode) -- one independent phase/decimation
    // state per LFO slot, PER VOICE, used only for LFOs actually in Per-Key mode (meaningless/
    // unread otherwise, exactly like vco*_sample_stream above when that oscillator isn't
    // WAVE_SAMPLE). Mirrors the Program-level lfo_decim_* function-local statics in
    // render_audio_block exactly, just duplicated per voice instead of shared per Program.
    float lfo_phase[LFO_COUNT];
    int lfo_decim_counter[LFO_COUNT];
    float lfo_decim_last_value[LFO_COUNT];
    float lfo_decim_target_value[LFO_COUNT];

    // SFZ per-zone amp envelope override (see ProgramState.vco1_force_own_amp_eg's own comment):
    // when the zone this oscillator's currently-claimed stream belongs to sets its own ampeg_*
    // opcodes AND the Program's own override checkbox is off, vco1_using_own_ampeg is true and
    // vco1_own_eg_rates (computed once at this note's own start_sample_osc() call, via
    // compute_zone_owned_eg_rates()) is what render_audio_block's tick_adsr() call actually
    // consults INSTEAD of the Program's own shared eg_rates[program][0] -- see that call site's own
    // comment. false/unused (this whole feature's ordinary, unaffected state) for every oscillator
    // whose zone sets no ampeg_* opcodes at all, or isn't even WAVE_SAMPLE.
    bool vco1_using_own_ampeg; EGDerivedRates vco1_own_eg_rates;
    bool vco2_using_own_ampeg; EGDerivedRates vco2_own_eg_rates;
    bool vco3_using_own_ampeg; EGDerivedRates vco3_own_eg_rates;
    bool vco4_using_own_ampeg; EGDerivedRates vco4_own_eg_rates;

    // Drum Bus routing (see DRUM_BUS_COUNT's own comment): resolved once at this note's own
    // handle_note_on() call, from the zone this oscillator's currently-claimed stream belongs to
    // (mirrors vco1_using_own_ampeg's own resolve-once-at-note-on posture exactly, for the same
    // reason -- the zone can't change mid-note). 0 (off, the default) for every oscillator not
    // currently playing a zone with drum_bus set, or not even WAVE_SAMPLE -- render_audio_block's
    // stereo-mixer stage is a complete no-op for this feature in that ordinary case.
    int vco1_drum_bus, vco2_drum_bus, vco3_drum_bus, vco4_drum_bus;
} SynthVoice;


// ---------------------------------------------------------------------------------------------
// Engine-runtime types (sample streaming, sequencer/transport, MIDI, OSC dispatch, JSON, ...) and
// the constants they are sized by -- moved verbatim, in their original order, out of backend.c.
// ---------------------------------------------------------------------------------------------

typedef enum { OSC_KIND_FLOAT, OSC_KIND_STRING } OscParamKind;
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
// hds.clap's native Combi-lane Program picker (hds_clap_combi_draw.c's own
// hds_apply_program_fields_to_combi_lane() call below) -- lives here, not in either caller's own
// file, so both TUs can #include this one file and get it (this project's usual
// single-translation-unit-per-binary convention -- see this file's own header comment). A real (if
// small) recursive-descent parser -- objects, arrays, strings with escapes, numbers, true/false/
// null -- not a hand-tuned scanner for this file's own specific shape. A .hdsprog is user-editable
// (opened in any text editor) and can carry arbitrary nested metadata (category, sfz_file, future
// fields) around the two objects this file actually reads, so genuinely skipping an unrecognized
// value correctly (nested braces, escaped quotes and all) needs real JSON grammar, not a flat
// key:value scan. Deliberately NOT the vendored third_party/json/json.hpp (nlohmann/json): that is
// a C++ template header, and pulling it in here would mean a second C++ bridge translation unit
// purely to parse a handful of flat key/value pairs -- exactly the kind of framework-for-a-simple-
// job this whole codebase otherwise avoids (see every FX_TYPE_* port's own "stripped off JUCE"
// comment). This parser is deliberately small and single-purpose instead: it builds a plain
// tagged-value tree, no schema validation, no writer half (a .hdsprog is only ever read here).
typedef enum { HDS_JSON_NULL, HDS_JSON_BOOL, HDS_JSON_NUMBER, HDS_JSON_STRING, HDS_JSON_ARRAY, HDS_JSON_OBJECT } HdsJsonType;
typedef struct HdsJsonMember {
    char *key; // NULL for array elements
    struct HdsJsonValue *value;
    struct HdsJsonMember *next;
} HdsJsonMember;
typedef struct HdsJsonValue {
    HdsJsonType type;
    double number;
    char *string; // HDS_JSON_STRING only, malloc'd
    bool boolean;
    HdsJsonMember *first_member; // HDS_JSON_OBJECT/HDS_JSON_ARRAY only
} HdsJsonValue;
// Registered explicitly (not via the generic catch-all method) so it can carry its own per-owner
// context -- which knobs to dump and where to reply -- mirrors zone_upsert_handler/
// zone_remove_handler's own registration style with &g_sample_engine/&p->sample_engine.
typedef struct {
    const SynthSharedControls *knobs;
} HdsStateRequestContext;
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
// SFZ_MAX_VELCURVE_POINTS: amp_velcurve_<N>=<value> sets one (velocity, gain) breakpoint on a
// custom velocity response curve -- the spec allows up to 128 (one per MIDI velocity value), but
// real-world libraries that use this opcode at all set a small handful of breakpoints (a soft
// "knee" or two), so a generous-but-bounded table trades an essentially unreachable ceiling for a
// fixed, small footprint. A file that genuinely sets more than this many distinct breakpoints on
// one region -- vanishingly rare -- just has its excess points ignored (logged), the same "skip,
// don't crash" posture unparseable opcodes elsewhere in this file already have. Needed by both
// SampleSlot (just below) and SfzRegion (further down) -- moved ahead of both rather than left by
// SfzRegion's own declaration alone, which used to compile only by accident (SampleSlot's own
// velcurve_point[]/velcurve_gain[] happened to be read before that #define was ever reached).
#define SFZ_MAX_VELCURVE_POINTS 16
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
    unsigned int sample_rate_hz; // this WAV's own fmt-chunk sample rate -- see render_sample_osc's
                                  // own comment for why this must scale the pre-roll/ring read-
                                  // pointer's advance rate: it is a COMPLETELY different thing from
                                  // musical pitch (a 44.1kHz file played back frame-for-frame at
                                  // this engine's fixed 48kHz output rate comes out ~1.47 semitones
                                  // sharp and finishes early, regardless of what MIDI note asked for
                                  // it -- a real-time-clock mismatch, not a tuning one).
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

    // --- SFZ first-class opcode support (appended) -- all pre-resolved by hds_load_sfz_for_osc()
    // (which already applies SFZ's own per-opcode defaults for anything a zone doesn't set) before
    // ever reaching this struct, same "no has_* flags stored past the parser" convention as
    // loop_enabled/loop_release_exit/playback_end_frame above. ---

    // loop_crossfade: -1 = not explicitly set by this zone -- keep the engine's own fixed
    // SAMPLE_XFADE_SIZE crossfade (today's exact behavior, a practical click-avoidance measure
    // that predates real loop_crossfade= support). >=0 = this zone's own explicit crossfade
    // length in frames, INCLUDING 0 (an explicit "hard cut, no crossfade" author choice) -- see
    // disk_fill_forward()'s loop-splice comment (the crossfade is mixed by the disk thread as it
    // writes the ring).
    int loop_crossfade_frames;

    // seq_length/seq_position: round-robin cycling, decoupled from `group` entirely now (a zone
    // needs only seq_length>0 to participate -- no group= required, matching real SFZ semantics;
    // see find_matching_sample_slot's own comment on why bucketing by group used to be wrong).
    // seq_length 0 = not part of any sequence. seq_position 0 = unspecified (falls back to
    // file/array-encounter order among tied siblings); >0 = this zone's own explicit 1-based
    // ordinal.
    int seq_length;
    int seq_position;

    // lorand/hirand: alternative/complementary random-variation matching -- a zone only matches
    // when a per-note-on random draw in [0,1) falls inside [lorand,hirand). Defaults to the full
    // range (0.0/1.0) when the zone doesn't set them, i.e. "always matches on this axis."
    float lorand, hirand;

    // choke_group/choke_off_by: SFZ's REAL group=/off_by= (e.g. a closed hi-hat's off_by=
    // matching an open hi-hat's own group= chokes every currently-sounding voice from that
    // group the instant the closed hat triggers) -- see apply_choke_group()'s own comment. 0 =
    // none for either. Completely independent of seq_length/seq_position above.
    int choke_group;
    int choke_off_by;

    // trigger_type: HDS_SFZ_TRIGGER_* -- attack (ordinary note-on, the default every pre-existing
    // zone already behaves as), release/release_key (fires on note-off instead -- see
    // fire_release_trigger_zones), first/legato (restricts an ordinary note-on match to only the
    // first key of a phrase / only an overlapping legato note).
    int trigger_type;

    // sw_lokey/sw_hikey/sw_last: keyswitching. -1 on any field = that bound is unset. A zone that
    // sets NONE of these three is never keyswitch-gated (matches on every ordinary note exactly as
    // before). A zone that sets any of them only matches while this (owner_program, assigned_osc)
    // pair's own tracked keyswitch_note (see HdsPendingLoadState.keyswitch_note) falls inside
    // [sw_lokey,sw_hikey], or equals sw_last exactly when that's the only one given. See
    // hds_sfz_zone_requires_keyswitch()/hds_sfz_keyswitch_matches().
    int sw_lokey, sw_hikey, sw_last;

    // rt_decay: extra release-stage decay (dB/second), meaningful only on a trigger=release zone
    // -- see fire_release_trigger_zones's own comment. 0 = none.
    float rt_decay;

    // delay_seconds: silence this long after note-on before this zone's own stream actually
    // starts (0 = immediate, today's exact default) -- see start_sample_osc's own delay-arming
    // comment.
    float delay_seconds;

    // ampeg_*: this zone's OWN complete amp envelope, in SFZ's own units and already defaulted
    // (see HdsAmpEgOverride's own comment for the full "zone's own envelope OR the Program's,
    // never blended" design) -- has_own_ampeg is false when the zone set NONE of the ampeg_*
    // opcodes at all (the ordinary, unaffected case: the Program's own Amp EG is used exactly as
    // it always has been, regardless of vco1_force_own_amp_eg).
    bool has_own_ampeg;
    float ampeg_delay, ampeg_attack, ampeg_hold, ampeg_decay, ampeg_sustain, ampeg_release, ampeg_start;

    // note_polyphony: caps how many voices may simultaneously hold a claimed stream against THIS
    // one zone_id -- see start_sample_osc's own gate. 0 = unlimited (today's exact default).
    int note_polyphony;

    // amp_velcurve_N: a custom, piecewise-linear velocity-to-gain response curve -- see
    // sfz_velcurve_gain()'s own comment. velcurve_count 0 = no custom curve at all (the ordinary,
    // unaffected case -- velocity affects level only via whatever the Program's own voice
    // amplitude already does, exactly as before).
    int velcurve_count;
    int velcurve_point[SFZ_MAX_VELCURVE_POINTS];
    float velcurve_gain[SFZ_MAX_VELCURVE_POINTS];

    // xfin_*/xfout_* (velocity, then key): crossfade LAYERING -- unlike an ordinary lovel/hivel
    // hard boundary (exactly one zone ever matches), a zone with these set can sound
    // SIMULTANEOUSLY with a neighboring zone across the shared boundary range, mixed by a linear
    // gain ramp -- see render_sample_osc's own crossfade-layer mixing comment. -1 on any field =
    // that crossfade edge is unset (no layering on that axis for this zone).
    int xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel;
    int xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey;

    // Drum Bus routing (HDS-specific, not a real SFZ opcode -- see DRUM_BUS_COUNT's own comment).
    // 0 (the default) = off, plays through this oscillator's normal Volume/Mute/Pan -> Filter/EQ
    // pipeline exactly as before. 1..DRUM_BUS_COUNT = this zone's rendered voice output bypasses
    // ALL of that (only this zone's own gain_db/pan and its own amp EG still apply) and sums
    // directly into Drum Bus (drum_bus - 1) instead -- see render_audio_block's own drum-bus mixing
    // block and DRUM_BUS_SOURCE_BASE for how that bus then becomes an ordinary FX-slot source.
    int drum_bus;
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
// of per-process). sample_slot_count tracks the high-water mark of slots ever used; an inactive
// (.active == false) entry left by a ZONE_OP_REMOVE is eligible for reuse by the next brand-new
// zone apply_zone_upsert() sees (see that function's own comment), so the catalog stays bounded by
// the PEAK number of simultaneously-active zones rather than growing with every zone ever loaded
// across the whole session -- e.g. repeatedly swapping a Multisample oscillator's SFZ file no
// longer leaves every previous load's own dead entry sitting in the catalog forever.
typedef struct {
    SampleSlot sample_slots[SAMPLE_SLOT_CAPACITY];
    int        sample_slot_count;
} HdsSampleSlotState;
// Multi-instance refactor, Phase 3: bundles the three sample-streaming tiers (each already a
// separate global for pure struct-ordering reasons -- see this struct's own comment above) into
// one handle so the sample-streaming functions below only need ONE new parameter each, not up to
// three. Forward-declares HdsStreamsState/HdsPendingLoadState (defined further down, once their
// own member types are complete) as bare struct tags -- legal here because HdsSampleEngine only
// holds POINTERS to them, which are a complete type regardless of whether the pointed-to struct
// itself is complete yet. Can't bundle all four HdsEngineState tiers together (HdsEngineState
// itself isn't declared until much later, after SeqMidiLane) -- this bundle covers exactly the
// tiers load_sample_slot() and its neighbors, declared right after this, need. The actual
// g_sample_engine global instance, along with g_sample_slot_state's and g_pending_load_state's
// own global instances, now live in hds_standalone.c (all three standalone-only -- a CLAP plugin
// instance has its own per-instance p->sample_engine/p->sample_slot_state/p->pending_load_state
// instead, see clap_plugin.c). Only g_streams_state's own global instance stays declared here,
// further down, since core code like render_audio_block references it directly (via the
// g_seq_audio macro) rather than through a parameter -- see hds_standalone.c's own header comment
// for the full story on that one exception.
typedef struct HdsStreamsState HdsStreamsState;
typedef struct HdsPendingLoadState HdsPendingLoadState;
typedef struct {
    HdsSampleSlotState *slots;
    HdsStreamsState *streams;
    HdsPendingLoadState *pending;
} HdsSampleEngine;
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
    // Session handshake. Every claim starts a new session: the audio thread bumps `session`
    // (release, after writing slot_index/start_frame) before publishing START. The disk thread,
    // once it has reset write_ptr/read_ptr/disk_reached_eof for the session it picked up, stores
    // that session number in `ready_session` (release). Until the two match, the ring and the EOF
    // flag still belong to an earlier session on this pool entry, so render_sample_osc holds the
    // voice silent and does not touch read_ptr. Without this, a note that reads the ring at once
    // (reverse, or a start offset past the pre-roll) played the previous session's leftover audio,
    // or was ended at once by that session's EOF flag, whenever the disk thread was late.
    _Atomic unsigned int session;       // written by the audio thread only
    _Atomic unsigned int ready_session; // written by the disk thread only
    // The disk thread never reads SampleSlot: the audio thread owns the catalog and may reload or
    // edit a slot at any time. Instead, claim_sample_stream() copies what a session needs into
    // these fields (before the session release-store, so the disk thread's acquire of `session`
    // makes them visible), and the loop/end settings are republished on every live edit of the
    // zone (publish_zone_edit_to_streams), so dragging a loop point still affects a held note.
    // claim_path holds the file path as relaxed atomic 8-byte words: a pool entry claimed again
    // while the disk thread is copying it can still mix two paths, but that copy then belongs to
    // a superseded session and is discarded (its ready_session never matches).
    _Atomic uint64_t claim_path[SAMPLE_PATH_LEN / 8];
    _Atomic bool claim_loaded, claim_reverse;
    _Atomic unsigned int claim_data_offset, claim_total_frames, claim_pre_roll_frames;
    _Atomic unsigned int claim_channels, claim_bits, claim_audio_format;
    _Atomic bool live_loop_enabled, live_loop_release_exit;
    _Atomic unsigned int live_loop_start, live_loop_end, live_playback_end;
    _Atomic int live_loop_crossfade;
    FILE *disk_file_handle;           // disk-thread-owned exclusively: borrowed from the disk
                                       // thread's open-file cache (see DiskFileCache in
                                       // sample_engine.c), never fclose()d through this pointer.
    int disk_file_entry;              // disk-thread-owned: that cache entry's index, -1 for none
    // Disk-thread-owned copies of the claim_* fields, taken at START.
    unsigned int disk_data_start_offset, disk_total_frames;
    unsigned short disk_num_channels, disk_bits_per_sample, disk_audio_format;
    bool disk_reverse;
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
    bool disk_exhausted;              // disk-thread-owned only: this session has nothing more
                                       // to read (a non-looping zone reached its end in either
                                       // direction, the file could not be opened, or a read
                                       // failed). Keeps the disk thread from picking the stream
                                       // again; reset false on SAMPLE_CMD_START.
    // Disk-thread-owned write, audio-thread-owned read (render_sample_osc) -- the ONLY reliable
    // way to tell "the ring is under-filled because this non-looping zone has genuinely finished"
    // apart from "the ring is under-filled because this freshly-claimed stream hasn't been
    // serviced by the disk thread yet" (or the disk thread is just behind -- e.g. busy servicing
    // many OTHER freshly-claimed streams from a burst of rapidly-retriggered notes). Before this
    // flag existed, render_sample_osc's own ring-starvation check (available < SAMPLE_XFADE_SIZE+2)
    // treated BOTH cases identically and released the stream on either one -- almost always
    // harmless (a real note-on gets a full pre_roll_frames head start before it ever needs the
    // ring, plenty of time for the disk thread to catch up under ordinary load), but under a
    // stress-test burst of many simultaneous/rapid retriggers across multiple Multisample
    // oscillators, the single shared disk thread can fall far enough behind that a brand new
    // stream's ring is STILL EMPTY by the time playback reaches it -- indistinguishable, before
    // this flag, from "reached the real end of the file," so the voice's sample playback got
    // silently killed the instant it should have started: a note that looks like it triggered
    // (envelope attacking normally) but is never actually audible. Set true by the disk thread at
    // the exact moment it determines there is truly nothing left (forward: disk_read_frame_pos
    // reaches slot->playback_end_frame; reverse: it reads frame 0 of a non-looping zone); reset
    // false on SAMPLE_CMD_START, same as disk_exhausted.
    _Atomic bool disk_reached_eof;
    _Atomic bool release_requested;  // audio-thread-owned write (request_sample_stream_release(),
                                       // called once from trigger_voice_release() at the owning
                                       // voice's STATE_RELEASE transition), disk-thread-owned read
                                       // (sample_disk_thread_main's loop-continuation checks) --
                                       // same simple atomic-flag handoff shape as the rest of this
                                       // struct. Reset false on SAMPLE_CMD_START (claim_sample_
                                       // stream) so a reclaimed pool entry never inherits a stale
                                       // true from whatever note last used it.
} SampleStream;
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

    // --- SFZ first-class opcode support (appended) -- deliberately in-memory-only, NOT part of
    // the OSC wire byte protocol above: zone_upsert_handler's own "19 consecutive floats" loop
    // never touches these, so every OSC-driven zone (the frontend Mapping tab, the Python SFZ
    // importer) always gets them from zone_record_wire_set_sfz_defaults() below, i.e. "every one
    // of these SFZ-only features is off for this zone" -- only hds_load_sfz_for_osc()'s own
    // native parser ever sets these fields to anything else. Field meanings mirror SampleSlot's
    // own fields of the same names exactly (this struct is just apply_zone_upsert()'s transient
    // input, copied field-for-field into the matching SampleSlot on a successful load). ---
    int loop_crossfade_frames; // -1 = unset (keep the engine's own fixed crossfade)
    int seq_length, seq_position;
    float lorand, hirand; // defaults 0.0/1.0 (the full, "always matches" range)
    int choke_group, choke_off_by;
    int trigger_type; // HDS_SFZ_TRIGGER_*
    int sw_lokey, sw_hikey, sw_last; // -1 = unset
    float rt_decay;
    float delay_seconds;
    int has_own_ampeg; // 0/1, same OSC-safe "plain int, not bool" convention as unpitched/reverse/... above
    float ampeg_delay, ampeg_attack, ampeg_hold, ampeg_decay, ampeg_sustain, ampeg_release, ampeg_start;
    int note_polyphony;
    int velcurve_count;
    int velcurve_point[SFZ_MAX_VELCURVE_POINTS];
    float velcurve_gain[SFZ_MAX_VELCURVE_POINTS];
    int xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel; // -1 = unset
    int xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey; // -1 = unset
    int drum_bus; // 0 = off (the OSC-safe default) -- see SampleSlot.drum_bus's own comment
} ZoneRecordWire;
// A zone whose file needs (re)loading, queued rather than loaded immediately -- see
// drain_pending_sample_loads()'s comment, which hands these to the sample loader thread. The
// queue itself is a plain ring buffer, audio-thread-only (both the enqueue and the drain run on
// the audio thread), so it needs no cross-thread synchronization.
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

    // --- SFZ first-class opcode support (appended) -- carried through the queue exactly like
    // every other per-zone field above, then copied onto the SampleSlot once the load actually
    // completes (see drain_pending_sample_loads()). Field meanings mirror SampleSlot's own fields
    // of the same names exactly; see ZoneRecordWire's matching block for the OSC-safe defaults an
    // ordinary (non-SFZ-native) zone always carries here. ---
    int loop_crossfade_frames;
    int seq_length, seq_position;
    float lorand, hirand;
    int choke_group, choke_off_by;
    int trigger_type;
    int sw_lokey, sw_hikey, sw_last;
    float rt_decay;
    float delay_seconds;
    bool has_own_ampeg;
    float ampeg_delay, ampeg_attack, ampeg_hold, ampeg_decay, ampeg_sustain, ampeg_release, ampeg_start;
    int note_polyphony;
    int velcurve_count;
    int velcurve_point[SFZ_MAX_VELCURVE_POINTS];
    float velcurve_gain[SFZ_MAX_VELCURVE_POINTS];
    int xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel;
    int xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey;
    int drum_bus;
} PendingSampleLoad;
#define PENDING_SAMPLE_LOAD_CAPACITY 4096
// trigger=release/release_key: a small fixed pool of one-shot "release tail" players, entirely
// independent of the ordinary voices[]/SynthVoice.vcoN_sample_stream playback path -- see
// fire_release_trigger_zones()'s own comment. 16 simultaneous release-triggered samples across the
// whole engine is generous for what this opcode is actually used for in practice (piano key-
// release noise, mechanical thumps, etc., usually a handful of overlapping notes' tails at once).
#define HDS_RELEASE_TAIL_POOL_SIZE 16
typedef struct {
    bool active;
    int stream_idx;       // claimed SampleStream pool index, or -1 (inactive)
    double ptr;
    bool preroll;
    int owner_program;    // which mixed_l[]/mixed_r[] bus this player's output sums into
    float play_hz;         // fixed at fire time -- see fire_release_trigger_zones's own comment
    float gain;             // current linear gain, decayed every sample by decay_per_sample below
    float decay_per_sample; // rt_decay=, converted from dB/sec to a per-sample linear multiplier;
                            // 1.0f (no decay at all) for a zone that doesn't set rt_decay=.
} HdsReleaseTailPlayer;
// This tier (see HdsSampleSlotState's own comment for why this is split into several structs):
// the throttled sample-load queue plus round-robin cycling state, grouped together since both
// need PendingSampleLoad (just declared) and nothing later.
// --- Sample loader thread (see sample_engine.c's "Sample loader thread" section) ---
//
// Every file read a zone or SFZ load needs (WAV header, RAM pre-roll decode, the .sfz text and
// each region's header) happens on this thread; the audio thread only hands jobs over and swaps
// finished results into the catalog. Three bounded lock-free queues connect them (HdsMpmcQueue
// cells; any thread may push, any may pop): requests (audio and GUI threads -> loader), results
// (loader -> audio thread) and garbage (audio thread -> loader, buffers the audio thread retired
// and must not free itself).

// A WAV file's layout plus its decoded RAM pre-roll, ready to install into a SampleSlot.
typedef struct {
    unsigned int total_frames, sample_rate_hz, data_start_offset, pre_roll_frames;
    unsigned short num_channels, bits_per_sample, audio_format;
    float *pre_roll_l, *pre_roll_r; // malloc'd; ownership moves to the slot on install
} HdsPreparedSample;

#define HDS_LOAD_JOB_SAMPLE 1 // load one zone's file (PendingSampleLoad)
#define HDS_LOAD_JOB_SFZ    2 // parse an .sfz and load every region's file

typedef struct {
    int kind;
    PendingSampleLoad sample;          // HDS_LOAD_JOB_SAMPLE
    int osc_index, owner_program;      // HDS_LOAD_JOB_SFZ
    char sfz_path[SAMPLE_PATH_LEN * 2];
} HdsLoadJob;

typedef struct {
    int kind;
    // HDS_LOAD_JOB_SAMPLE
    bool ok;
    PendingSampleLoad sample;
    HdsPreparedSample prepared;
    // HDS_LOAD_JOB_SFZ: `count` regions, each with its zone record and (if prepared_ok[i]) its
    // prepared file. All three arrays are malloc'd by the loader and retired by the audio thread.
    int osc_index, owner_program, count;
    int sw_default_note; // -1: the file set no sw_default=
    ZoneRecordWire *records;
    HdsPreparedSample *prepared_list;
    bool *prepared_ok;
} HdsLoadResult;

#define HDS_LOADER_QUEUE_CAPACITY   256  // requests and results; power of two
#define HDS_LOADER_GARBAGE_CAPACITY 8192 // retired buffers; power of two

typedef struct {
    _Atomic size_t *seq;    // one sequence number per cell
    unsigned char *data;    // cells' payloads, elem_size bytes each
    size_t elem_size, mask;
    _Atomic size_t enqueue_pos, dequeue_pos;
} HdsMpmcQueue;

typedef struct {
    bool initialized;            // queues set up (once, by hds_sample_loader_start)
    _Atomic bool running;        // the thread is up and taking jobs
    _Atomic bool shutdown;
    pthread_t thread;
    HdsMpmcQueue requests, results, garbage;
    _Atomic size_t request_seq[HDS_LOADER_QUEUE_CAPACITY];
    HdsLoadJob request_cells[HDS_LOADER_QUEUE_CAPACITY];
    _Atomic size_t result_seq[HDS_LOADER_QUEUE_CAPACITY];
    HdsLoadResult result_cells[HDS_LOADER_QUEUE_CAPACITY];
    _Atomic size_t garbage_seq[HDS_LOADER_GARBAGE_CAPACITY];
    void *garbage_cells[HDS_LOADER_GARBAGE_CAPACITY];
    // Audio-thread-owned: an SFZ result being applied a few regions per block.
    bool sfz_applying;
    int sfz_cursor;
    HdsLoadResult sfz_current;
    // Buffers freed on the audio thread because the garbage queue was full (should stay 0).
    _Atomic unsigned long inline_frees;
} HdsSampleLoaderState;

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

    // Keyswitching (sw_lokey/sw_hikey/sw_last/sw_default) state -- one tracked "last keyswitch
    // note pressed" per (owner_program, assigned_osc) pair, same scoping reasoning as
    // round_robin_hit_count just above. Stored as note+1 (0 = "no keyswitch pressed yet on this
    // (Program, oscillator), and no sw_default= was set by its SFZ either" -- the BSS/calloc zero
    // state every never-touched pair starts in) so a real keyswitch note 0 can still be
    // represented without needing a separate "is this valid" flag alongside it. See
    // hds_sfz_zone_requires_keyswitch()/hds_sfz_keyswitch_matches() and handle_note_on's own
    // keyswitch-interception block.
    int keyswitch_note_plus1[SINGLE_PROGRAM_SLOT + 1][4];

    // poll_vco_shape_unload()'s own "last observed Shape" per (owner_program, oscillator) pair,
    // same scoping/indexing convention as keyswitch_note_plus1 just above -- per-INSTANCE state
    // (this struct, not a bare file-scope static) since two separate plugin instances in the same
    // process (e.g. two hds.clap tracks) must never share or interfere with each other's own
    // tracked shape transitions. -1 (never a real WaveType) is BSS/calloc's own zero-adjacent "not
    // yet observed" sentinel would otherwise wrongly be -- 0 IS a real WaveType (WAVE_SAWTOOTH or
    // similar) here, unlike keyswitch_note_plus1's own +1 encoding trick, so this is explicitly
    // reset to -1 once by poll_vco_shape_unload() itself on first use instead, see its own comment.
    float vco_shape_last[SINGLE_PROGRAM_SLOT + 1][4];
    bool vco_shape_last_init;

    // trigger=release/release_key one-shot players (see fire_release_trigger_zones()'s own
    // comment) -- a small fixed pool, entirely independent of voices[]/MAX_VOICES (a release-
    // triggered sample plays out on its own, with no amp EG, pitch modulation, or filter routing
    // of its own, after the ordinary voice that triggered it has already moved on into its own
    // release phase or even been stolen/reused for something else entirely).
    HdsReleaseTailPlayer release_tails[HDS_RELEASE_TAIL_POOL_SIZE];

    // The sample loader thread and its queues -- see HdsSampleLoaderState.
    HdsSampleLoaderState loader;
} HdsPendingLoadState;
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

    // --- Metronome (own independent clock -- see poll_metronome()'s own comment on why this no
    // longer rides transport_playhead_sec/transport_state) ---
    double        metronome_elapsed_sec;
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
    // Every slot's own last-computed post-mix output, in POOL-SLOT order (not per-source) -- lets
    // any slot's FXSlotParams.input_from_slot pull a DIFFERENT slot's real output directly, even one
    // owned by a different source's own process_fx_chain() call. Written unconditionally for every
    // slot process_fx_chain() actually processes (see that function's own comment); a slot referenced
    // before its own source has run this sample simply reads last sample's value -- a one-sample-or-
    // less lag, the same tradeoff sc_snapshot_l/r already accepts for the sidechain case. Persists
    // across blocks like fx_engine[] itself (never reset to 0 on its own), so this stays correct
    // regardless of which order sources happen to be walked in.
    float         fx_slot_out_l[FX_SLOT_COUNT];
    float         fx_slot_out_r[FX_SLOT_COUNT];
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

    // FX slot NAM model/cab IR upload-over-OSC receive state (see fx_upload_begin_handler()'s own
    // comment) -- one shared buffer, not per-slot: only one upload is ever realistically in flight
    // at a time (a user clicking one Load button, waiting for it to finish, before another), so a
    // second /hds/fx/upload_begin simply abandons whatever the first one was doing rather than
    // needing FX_SLOT_COUNT*2 buffers most of which would sit unused forever.
    uint8_t      *fx_upload_buffer;
    size_t        fx_upload_total_size;
    size_t        fx_upload_received;
    uint32_t      fx_upload_expected_crc32;
    int           fx_upload_slot;   // -1 when no upload is in progress
    bool          fx_upload_is_ir;  // which of that slot's two fields this upload targets
} HdsEngineState;
#if HDS_NAM_ENABLED
typedef struct {
    HdsEngineState *engine;
    SynthSharedControls *knobs;
} HdsFxUploadContext;
#endif
typedef struct {
    HdsEngineState *engine;
} HdsSmfTrackImportContext;
typedef struct {
    HdsEngineState *engine;
    SynthSharedControls *knobs;
} HdsSmfImportContext;
// Everything about WHAT happens once a MIDI event is decoded -- Combi-lane channel fan-out,
// Single Program/Sequencer dispatch, sustain-pedal polarity, pitch-bend/channel-pressure scaling,
// Sequencer recording -- lives in this ONE function, called by process_midi_input below once it
// has decoded a raw ALSA sequencer event into this neutral shape.
// `value`'s meaning depends on `type`, matching the exact units the ALSA sequencer already
// delivered before this pass: NOTE_ON/NOTE_OFF -> velocity (0-127, data1=note 0-127); CC ->
// raw CC value (0-127, data1=CC number 0-127); PITCH_BEND -> signed -8192..8191, 0=centered
// (data1 unused); CHANNEL_PRESSURE -> raw pressure (0-127, data1 unused).
typedef enum { HDS_MIDI_NOTE_ON, HDS_MIDI_NOTE_OFF, HDS_MIDI_CC, HDS_MIDI_PITCH_BEND, HDS_MIDI_CHANNEL_PRESSURE } HdsMidiEventType;
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
    // Per-FX-slot gain reduction, in dB (0 = no reduction / bypassed / non-GR-capable type) --
    // see render_audio_block's own gr_db_out comment and process_fx_chain's per-slot GR writes.
    // Only Gate/Limiter/FetComp(HT-76)/FairKid67(Fairchild 670) ever report nonzero here.
    float fx_gain_reduction_db[FX_SLOT_COUNT];
    // Per-FX-slot currently-buffered delay, in seconds -- see render_audio_block's own
    // dump_avail_out comment. Only FX_TYPE_BROADCAST_DELAY ever reports nonzero here.
    float fx_dump_avail_sec[FX_SLOT_COUNT];
} EngineStatus;

#endif // HDS_BACKEND_TYPES_H
