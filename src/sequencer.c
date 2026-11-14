// sequencer.c: the Sequencer's own transport, audio/MIDI-lane playback, external MIDI Clock sync,
// and song-as-MIDI save/load -- standalone-only (the ALSA/PipeWire-backed synth_engine executable
// is the only thing that ever drives any of this; a CLAP plugin instance never does, per the
// user's own explicit direction -- see clap_plugin_process()'s own header comment in
// clap_plugin.c). Compiled as its own object and linked into synth_engine only; sequencer.h is the
// interface hds_standalone.c's main loop and MIDI/OSC dispatch use.
//
// What stays behind, in backend.c, and why: the Sequencer's own DATA shapes (SeqLaneState,
// SeqAudioLane, SeqMidiLane, and the transport/seq_midi fields on HdsEngineState) are genuinely
// shared -- a CLAP plugin instance still carries its own per-instance copy of all of it (it just
// never gets driven), and the OSC control table indexes into SeqLaneState directly, same reasoning
// FXSlotParams/FXSlotEngine/FX_TYPE_* stayed in backend.c after the FX split. Two small rendering
// hooks stay too, since they're called from functions both binaries compile: seq_record_midi_event
// (called from dispatch_midi_event -- a no-op in CLAP today, since p->engine.transport_state can
// never leave TRANSPORT_STOPPED there, but the call site itself is core) and
// render_seq_audio_preview_sample (called from render_audio_block's own per-sample mix loop, same
// reasoning). poll_arpeggiator/poll_metronome are NOT part of this file either, despite living
// right next to the Sequencer conceptually -- both are explicitly kept alive in CLAP (arp and
// tempo sync stay fully functional there, and the metronome now runs its own independent clock --
// see poll_metronome's own comment in backend.c), so they belong with the shared core, not here.

#include "sequencer.h"

// --- Sequencer transport / audio-lane polling (moved here, verbatim, from backend.c): all of
// it was already only reachable from main()'s own loop (a CLAP plugin instance never calls
// any of it -- see clap_plugin_process()'s own header comment on why the Sequencer tab's
// transport/lane/MIDI-playback workflow is deliberately standalone-only), so leaving it in
// backend.c only cost the CLAP build a cascade of -Wunused-function warnings once
// clap_plugin.c stopped calling any of it. transport_start_fresh/transport_reposition_lanes/
// seq_audio_start_transport_playback come first since hds_handle_midi_transport_start just
// below needs transport_start_fresh already defined.

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


// seq_audio_load_wav_from_disk() itself moved to fx_chain.c (see that file's own comment on why:
// FX_TYPE_NAM's cab IR loader needs the exact same WAV decoder, and fx_chain.c is genuinely shared
// across every translation unit -- backend.c, hds_fx_plugin.c, and (via backend.c) this file's own
// includer hds_standalone.c -- while this file (sequencer.c) is standalone-only, never linked into
// either CLAP plugin. Still available here under the same name (fx_chain.c is #include'd by
// backend.c, itself #include'd by hds_standalone.c BEFORE this file, so it's in scope by the time
// this line runs) -- pure code motion, no behavior change, no new cross-file symbol.

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
void poll_seq_audio_load_requests(HdsStreamsState *streams, const SynthSharedControls *knobs) {
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


// "Preview" one-shot playback trigger detection (see SeqLaneState.audio_preview_trigger's own
// comment). Edge-triggered: the frontend increments the trigger value by 1 on every click, so
// any change (not just "> last") starts (or restarts) that lane's playback from the beginning --
// or audio_offset_sec seconds in, if positive. Deliberately independent of the Sequencer
// transport (see poll_transport_triggers/transport_start_fresh, added alongside this): a
// Preview-triggered lane always plays through to completion regardless of Play/Pause/Stop state,
// unlike a transport-driven lane -- auditioning a sound must never depend on whether the
// Sequencer happens to be running. Call once per block from main()'s loop, same as
// poll_seq_audio_load_requests.
void poll_seq_audio_preview_triggers(HdsStreamsState *streams, const SynthSharedControls *knobs) {
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


// Edge-triggered, same idiom as poll_seq_audio_preview_triggers: each button increments its own
// counter on every click and resends the whole packet, so a genuine change (not just "button
// held") is what starts/pauses/stops/seeks the transport -- immune to a stale resend
// re-triggering anything. Call once per block from main()'s loop. current_tempo_bpm is this
// block's already-resolved tempo (see the Seek Back/Forward triggers' own comment on why "right
// now" rather than "at the seek target").
void poll_transport_triggers(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm) {
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
        // Metronome no longer resyncs here -- it runs its own free-running clock now (see
        // poll_metronome()'s own comment), entirely independent of the Sequencer transport's
        // playhead, so a Sequencer seek has nothing to do with its schedule any more.
    }
}


// Called every block from main()'s loop (see seq_audio_start_transport_playback's own comment
// on why this exists): starts any lane transport_start_fresh had to skip because its load hadn't
// finished yet, the moment SEQ_AUDIO_READY actually lands, as long as the transport is still
// PLAYING when that happens. A cheap, unconditional scan -- armed_for_transport is false for
// every lane in the overwhelmingly common case (load already finished before Play was pressed),
// so this is a no-op most of the time.
void poll_seq_audio_transport_catchup(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
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
void advance_transport_playhead(HdsEngineState *engine, int block_size) {
    if (engine->transport_state == TRANSPORT_PLAYING) {
        engine->transport_playhead_sec += (double)block_size / (double)SAMPLE_RATE;
    }
}


// Called once per block, right after process_midi_input -- walks every Seq-mode Program/MIDI
// lane's recorded events and fires any whose timestamp has now been reached, via the exact same
// handle_note_on/handle_note_off functions live MIDI already calls (see this section's own scope
// comment for why that's a deliberate choice, not a shortcut).
void seq_playback_midi_events(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack *mono_stack, const MidiLiveState *midi_live) {
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
// HDS_MIDI_CLOCK_TIMEOUT_SEC is in sequencer.h (hds_standalone.c's main loop checks it too).

double hds_monotonic_seconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Hooked directly into process_midi_input's SND_SEQ_EVENT_CLOCK case -- runs unconditionally
// regardless of seq_sync_source (cheap bookkeeping either way), so switching sync source live
// doesn't need to "catch up" on tempo -- it's already tracked in the background.
void hds_handle_midi_clock_tick(HdsEngineState *engine) {
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
void hds_handle_midi_transport_start(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs) {
    engine->transport_playhead_sec = 0.0;
    engine->transport_state = TRANSPORT_PLAYING;
    transport_start_fresh(streams, engine, knobs);
}

// MIDI Continue (0xFB): resumes from exactly wherever the transport currently sits -- no
// repositioning, same as the GUI's own Paused->Playing transition.
void hds_handle_midi_transport_continue(HdsEngineState *engine) {
    engine->transport_state = TRANSPORT_PLAYING;
}

// MIDI Stop (0xFC): PAUSES rather than resets to zero -- matches General MIDI/MIDI Machine
// Control's own Stop/Continue semantics (a subsequent Continue must resume from here), the same
// distinction the GUI's own separate Pause and Stop buttons already make. A performer who wants
// to rewind to the top sends Start, not Stop, exactly as most DAWs already behave.
void hds_handle_midi_transport_stop(HdsEngineState *engine) {
    if (engine->transport_state == TRANSPORT_PLAYING) engine->transport_state = TRANSPORT_PAUSED;
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

int song_save_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)msg;
    HdsEngineState *engine = (HdsEngineState *)user_data;
    if (argc != 1) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    if (!hds_song_save_midi(engine, file_path)) {
        fprintf(stderr, "[song] failed to save MIDI data to '%s'\n", file_path);
    }
    return 0;
}

int song_load_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)msg;
    HdsEngineState *engine = (HdsEngineState *)user_data;
    if (argc != 1) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    if (!hds_song_load_midi(engine, file_path)) {
        fprintf(stderr, "[song] failed to load MIDI data from '%s' (missing, unreadable, or wrong format/version)\n", file_path);
    }
    return 0;
}


