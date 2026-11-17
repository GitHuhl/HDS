// midi_dispatch.c -- shared MIDI event dispatch (dispatch_midi_event) and Sequencer MIDI-lane recording.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"

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

void dispatch_midi_event(HdsEngineState *engine, HdsSampleEngine *se, HdsMidiEventType type, int midi_channel_1_16, int data1, int value,
                                 SynthVoice voices[], SynthSharedControls *knobs,
                                 float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT],
                                 MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], MidiLiveState *midi_live, float current_tempo_bpm) {
    if (type == HDS_MIDI_NOTE_ON || type == HDS_MIDI_NOTE_OFF) {
        // Global performance Transpose (see SynthSharedControls.transpose_semitones' own comment
        // for why this moved here from a live audio-rate cents offset): applied to the raw
        // incoming note number FIRST, before anything downstream -- Combi key-range gating, Arp,
        // handle_note_on/off (voice claiming AND Multisample sample-zone lookup), Sequencer
        // recording -- ever sees it, so the whole engine genuinely believes a different key was
        // pressed rather than pitch-shifting whatever the physical key would have selected.
        // lroundf, not a plain cast: the knob is float-typed but always an integer value in
        // practice (a real transpose is whole semitones -- see the knob's own -16..16 int-stepped
        // range in every frontend), and round-to-nearest is the correct way to treat a value that
        // could in principle carry float imprecision from OSC/state round-tripping as that intended
        // integer. Clamped to the valid MIDI note range so an extreme transpose at the very top/
        // bottom of the keyboard can't hand a negative or >127 note number to anything downstream.
        int note = data1 + (int)lroundf(knobs->transpose_semitones);
        if (note < 0) note = 0; else if (note > 127) note = 127;
        bool is_note_on = (type == HDS_MIDI_NOTE_ON);
        // Feeds FX_TYPE_BROADCAST_DELAY's On-Air Override MIDI Trigger mode (see
        // g_bcastdly_notes_held's own comment in fx_chain.c) -- deliberately the rawest possible
        // choke point (every note-on/off this function is ever called with, regardless of which
        // Combi lane or Arp routing it goes on to take below), so "any MIDI key press" means
        // literally any key, not just ones actually reaching a voice.
        if (is_note_on) g_bcastdly_notes_held++;
        else if (g_bcastdly_notes_held > 0) g_bcastdly_notes_held--;
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
        if (cc_num == 1) g_hardtune_modwheel = fmaxf(0.0f, fminf(1.0f, value / 127.0f)); // see its own comment
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
            g_bcastdly_sustain_held = now_held; // feeds On-Air Override MIDI Trigger mode -- see its own comment in fx_chain.c
        }
    } else if (type == HDS_MIDI_PITCH_BEND) {
        midi_live->pitch_bend = fmaxf(-1.0f, fminf(1.0f, value / 8192.0f));
        // FX_TYPE_HARDTUNE's own live pitch-bend feature (see its own comment) -- reuses Program
        // 0's pb_range_up/pb_range_down exactly like render_audio_block's own per-Program pitch
        // bend already does, since this global effect has no single Program of its own to read a
        // range from.
        g_hardtune_bend_semitones = (midi_live->pitch_bend >= 0.0f)
            ? midi_live->pitch_bend * knobs->programs[0].pb_range_up
            : midi_live->pitch_bend * knobs->programs[0].pb_range_down;
    } else if (type == HDS_MIDI_CHANNEL_PRESSURE) {
        midi_live->channel_pressure = fmaxf(0.0f, fminf(1.0f, value / 127.0f));
        g_hardtune_channel_pressure = midi_live->channel_pressure; // see its own comment
    }
}
