// arpeggiator.c -- the arpeggiator (poll_arpeggiator) and its note tracking.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"

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
void poll_arpeggiator(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], const MidiLiveState *midi_live, int block_size) {
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

// --- Arpeggiator note-tracking (see the Arpeggiator section on ProgramState and
// poll_arpeggiator()'s own header comment for the full design) ---
//
// When a Program has arp_enabled, a real note-on/off event must NOT trigger a voice directly --
// it should only update this Program's own held-notes list, and poll_arpeggiator() (its own
// independent musical clock) decides which note actually sounds and when, via its own synthetic
// handle_note_on/handle_note_off calls. These two functions are dispatch_midi_event's own
// note-on/off handling, redirected here instead of straight into handle_note_on/handle_note_off.
void arp_note_on(HdsEngineState *engine, int target_program, int note, int velocity) {
    mono_stack_push(&engine->arp_held_notes[target_program], note, velocity);
    engine->arp_keys_down[target_program]++;
}

void arp_note_off(HdsEngineState *engine, const ProgramState *prog, int target_program, int note) {
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
