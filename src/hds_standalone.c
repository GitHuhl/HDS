// hds_standalone: the ALSA/PipeWire-backed standalone synth_engine executable -- everything that
// touches a real audio device, a real MIDI sequencer client, the local filesystem for recording/
// program files, or an interactive terminal, none of which a host-driven target like a CLAP
// plugin needs (the host owns the device and the plugin format has its own state save/load) --
// see backend.c's own header comment for the shared DSP/OSC/state core this #includes.
//
// This file (not backend.c) is what $(BIN)/$(ARM64_BIN) actually compile (see the Makefile) --
// backend.c itself is never compiled standalone, exactly like clap_plugin.c already treats it.
// Moved here, verbatim, from backend.c: the ALSA PCM + PipeWire audio shim, the ALSA MIDI
// sequencer client, interactive device-recovery prompts, WAV/WavPack recording, the --program
// JSON file loader, the interactive arrow-key CLI, and main() itself. Verified before the move
// (grep, not assumed) that nothing in the shared core references any of it -- the only two
// exceptions, send_osc_status()/send_seq_audio_status(), stayed in backend.c because
// clap_plugin.c calls them too. This is a pure code-motion refactor: hds_standalone.c #includes
// backend.c, so both targets still compile as ONE translation unit each, exactly as before --
// there is no new cross-TU call, no inlining lost, and no behavior change of any kind.
//
// NOT moved, on purpose, despite also only being reachable from main()'s own code (confirmed by a
// real, if harmless, cascade of -Wunused-function/-Wunused-variable warnings the CLAP build shows
// for them): g_sample_engine (and its 3 constituent globals), poll_fx_nam_requests, the MIDI Clock/
// Start/Continue/Stop transport handlers, and the Scala tuning / song-as-MIDI OSC handlers.
// render_audio_block's own Sequencer-audio-lane playback block reads g_streams_state (via the
// g_seq_audio macro) DIRECTLY BY NAME rather than through a parameter, even though every other
// piece of state it touches is passed in -- so g_streams_state has to stay declared before
// backend.c's own content, which only this file's OWN prior position (not after an #include of
// it) can satisfy. This is a genuine pre-existing wrinkle, not something this split created: it
// means a CLAP plugin instance's Sequencer "Load Audio" feature loads into that instance's own
// p->streams_state but render_audio_block's playback code reads the standalone's global instead
// -- i.e. Sequencer audio-lane playback likely does not work in the CLAP plugin today, independent
// of this refactor entirely. Left exactly as found; fixing it (parametrizing render_audio_block
// to take a streams pointer) is a real, separate, scoped follow-up, not bundled into this split.
#include <alsa/asoundlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h> // raw-mode stdin for the interactive arrow-key CLI controls (see poll_stdin_control)

// WavPack recording is disabled for now (commented out via this single guard, not deleted --
// flip to 1, re-add -lwavpack to the Makefile's LDLIBS, and re-enable the frontend.py format
// radio button to bring it back). See WavPackRecorder further down for the real implementation
// this guards, kept intact behind #if HDS_WAVPACK_ENABLED rather than removed.
#define HDS_WAVPACK_ENABLED 0
#if HDS_WAVPACK_ENABLED
#include <wavpack/wavpack.h>
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

#include "backend.c"

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
// 65536 floats -- purely a physical safety cap on the ring array's own size (so a slow/stalled
// PipeWire graph can never write out of bounds), NOT the target latency -- see
// HdsPwStream.high_water_samples/write_audio_block()'s own comment for what actually paces this
// buffer's real steady-state depth (and therefore round-trip latency) down near the Latency knob.
#define HDS_PW_RING_CAPACITY (1u << 16)
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
    // Playback-only real latency throttle -- see write_audio_block()'s own comment on why this,
    // not HDS_PW_RING_CAPACITY, is what actually bounds round-trip latency. Set once in
    // hds_pw_stream_open() from the same latency_ms the Latency knob already maps onto
    // PW_KEY_NODE_LATENCY, so both halves of "how much this backend buffers" stay in sync instead
    // of the ring silently overriding the knob with its own much larger fixed capacity. Plain
    // uint32_t, not atomic: written once before pw_thread_loop_start(), read-only from then on.
    uint32_t high_water_samples;
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
    //
    // FIXED (real round-trip-latency bug, not an environment/PipeWire-daemon issue): this used to
    // only block when the ring was COMPLETELY FULL (space == 0), i.e. up to HDS_PW_RING_CAPACITY's
    // own ~170ms(8ch)-680ms(2ch) physical size -- regardless of how small the negotiated PipeWire
    // quantum was. A tool like `pw-top` only shows PipeWire's OWN graph-level buffering; it has no
    // visibility into this client's own software ring sitting upstream of that, so lowering the
    // quantum (even confirmed low via pw-top) never touched the real bottleneck. Now throttled
    // against pw->high_water_samples (2x the Latency knob's own target -- see its own comment)
    // instead, so the ring's actual steady-state depth tracks the knob, not its own much larger
    // physical capacity.
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
        uint32_t queued = write_pos - read_pos;
        if (queued >= pw->high_water_samples) {
            // sem_timedwait (not sem_wait) so a missed wakeup -- a post landing between the queued
            // check above and this wait -- can't block this thread, and therefore the whole main
            // loop, forever; a timeout just re-checks the ring's real atomics on the next pass.
            struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 1;
            sem_timedwait(&pw->space_sem, &deadline);
            continue;
        }
        uint32_t space = HDS_PW_RING_CAPACITY - queued;              // physical headroom -- never overflow the real array
        uint32_t room_to_target = pw->high_water_samples - queued;   // the actual throttle: don't overshoot the latency target either
        uint32_t allowed = space < room_to_target ? space : room_to_target;
        uint32_t take = allowed < (total_samples - written_samples) ? allowed : (total_samples - written_samples);
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
    // THE actual round-trip-latency fix (see this struct field's own comment): double-buffer worth
    // of headroom above the requested latency (standard low-latency-audio practice -- enough slack
    // to absorb this thread's own normal scheduling jitter between successive write_audio_block()
    // calls without a real-time xrun, while staying close to what the Latency knob actually asked
    // for) rather than the ~170-680ms (depending on channel count) the ring's own full physical
    // capacity previously silently allowed to build up before write_audio_block() ever blocked.
    pw->high_water_samples = 2u * latency_frames * (uint32_t)channels;
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

// --- Minimal JSON reader, for --program (see hds_load_program_file() below) ---------------------
// A real (if small) recursive-descent parser -- objects, arrays, strings with escapes, numbers,
// true/false/null -- not a hand-tuned scanner for this file's own specific shape. A .hdsprog is
// user-editable (opened in any text editor) and can carry arbitrary nested metadata (category,
// sfz_file, future fields) around the two objects this file actually reads, so genuinely skipping
// an unrecognized value correctly (nested braces, escaped quotes and all) needs real JSON grammar,
// not a flat key:value scan. Deliberately NOT the vendored third_party/json/json.hpp (nlohmann/
// json): that is a C++ template header, and pulling it in here would mean a second C++ bridge
// translation unit purely to parse a handful of flat key/value pairs -- exactly the kind of
// framework-for-a-simple-job this whole codebase otherwise avoids (see every FX_TYPE_* port's own
// "stripped off JUCE" comment). This parser is deliberately small and single-purpose instead: it
// builds a plain tagged-value tree, no schema validation, no writer half (--program only ever
// reads).
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

static void hds_json_free(HdsJsonValue *v) {
    if (!v) return;
    for (HdsJsonMember *m = v->first_member; m; ) {
        HdsJsonMember *next = m->next;
        free(m->key);
        hds_json_free(m->value);
        free(m);
        m = next;
    }
    free(v->string);
    free(v);
}

static void hds_json_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

// Parses one JSON string literal (the opening quote must already be consumed by the caller) into a
// freshly malloc'd, NUL-terminated C string, decoding the standard backslash escapes (\uXXXX only
// as far as a plain UTF-8 passthrough of the code point below U+0800 -- every address/value this
// file actually reads is plain ASCII, e.g. a file path or an OSC address, so this stays deliberately
// simple rather than a full UTF-16-surrogate-pair-aware decoder). Returns NULL on a malformed/
// unterminated string.
static char *hds_json_parse_string_body(const char **p) {
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (**p != '"') {
        if (**p == '\0') { free(out); return NULL; }
        char c = **p;
        if (c == '\\') {
            (*p)++;
            switch (**p) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (!isxdigit((unsigned char)(*p)[1]) || !isxdigit((unsigned char)(*p)[2]) ||
                        !isxdigit((unsigned char)(*p)[3]) || !isxdigit((unsigned char)(*p)[4])) { free(out); return NULL; }
                    char hex[5] = { (*p)[1], (*p)[2], (*p)[3], (*p)[4], '\0' };
                    long code = strtol(hex, NULL, 16);
                    *p += 4;
                    c = (code >= 0x20 && code < 0x7f) ? (char)code : '?'; // ASCII passthrough, see comment above
                    break;
                }
                default: free(out); return NULL;
            }
        }
        if (len + 1 >= cap) { cap *= 2; char *grown = realloc(out, cap); if (!grown) { free(out); return NULL; } out = grown; }
        out[len++] = c;
        (*p)++;
    }
    (*p)++; // closing quote
    out[len] = '\0';
    return out;
}

static HdsJsonValue *hds_json_parse_value(const char **p);

static HdsJsonValue *hds_json_new_value(HdsJsonType type) {
    HdsJsonValue *v = calloc(1, sizeof(HdsJsonValue));
    if (v) v->type = type;
    return v;
}

static HdsJsonValue *hds_json_parse_object(const char **p) {
    HdsJsonValue *obj = hds_json_new_value(HDS_JSON_OBJECT);
    if (!obj) return NULL;
    (*p)++; // '{'
    hds_json_skip_ws(p);
    if (**p == '}') { (*p)++; return obj; }
    HdsJsonMember **next_slot = &obj->first_member;
    for (;;) {
        hds_json_skip_ws(p);
        if (**p != '"') { hds_json_free(obj); return NULL; }
        (*p)++;
        char *key = hds_json_parse_string_body(p);
        if (!key) { hds_json_free(obj); return NULL; }
        hds_json_skip_ws(p);
        if (**p != ':') { free(key); hds_json_free(obj); return NULL; }
        (*p)++;
        hds_json_skip_ws(p);
        HdsJsonValue *val = hds_json_parse_value(p);
        if (!val) { free(key); hds_json_free(obj); return NULL; }
        HdsJsonMember *member = calloc(1, sizeof(HdsJsonMember));
        if (!member) { free(key); hds_json_free(val); hds_json_free(obj); return NULL; }
        member->key = key;
        member->value = val;
        *next_slot = member;
        next_slot = &member->next;
        hds_json_skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; break; }
        hds_json_free(obj);
        return NULL;
    }
    return obj;
}

static HdsJsonValue *hds_json_parse_array(const char **p) {
    HdsJsonValue *arr = hds_json_new_value(HDS_JSON_ARRAY);
    if (!arr) return NULL;
    (*p)++; // '['
    hds_json_skip_ws(p);
    if (**p == ']') { (*p)++; return arr; }
    HdsJsonMember **next_slot = &arr->first_member;
    for (;;) {
        hds_json_skip_ws(p);
        HdsJsonValue *val = hds_json_parse_value(p);
        if (!val) { hds_json_free(arr); return NULL; }
        HdsJsonMember *member = calloc(1, sizeof(HdsJsonMember));
        if (!member) { hds_json_free(val); hds_json_free(arr); return NULL; }
        member->value = val;
        *next_slot = member;
        next_slot = &member->next;
        hds_json_skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; break; }
        hds_json_free(arr);
        return NULL;
    }
    return arr;
}

static HdsJsonValue *hds_json_parse_value(const char **p) {
    hds_json_skip_ws(p);
    switch (**p) {
        case '{': return hds_json_parse_object(p);
        case '[': return hds_json_parse_array(p);
        case '"': {
            (*p)++;
            char *s = hds_json_parse_string_body(p);
            if (!s) return NULL;
            HdsJsonValue *v = hds_json_new_value(HDS_JSON_STRING);
            if (!v) { free(s); return NULL; }
            v->string = s;
            return v;
        }
        case 't':
            if (strncmp(*p, "true", 4) != 0) return NULL;
            *p += 4;
            { HdsJsonValue *v = hds_json_new_value(HDS_JSON_BOOL); if (v) v->boolean = true; return v; }
        case 'f':
            if (strncmp(*p, "false", 5) != 0) return NULL;
            *p += 5;
            return hds_json_new_value(HDS_JSON_BOOL);
        case 'n':
            if (strncmp(*p, "null", 4) != 0) return NULL;
            *p += 4;
            return hds_json_new_value(HDS_JSON_NULL);
        default: {
            if (**p != '-' && !isdigit((unsigned char)**p)) return NULL;
            char *end = NULL;
            double num = strtod(*p, &end);
            if (end == *p) return NULL;
            *p = end;
            HdsJsonValue *v = hds_json_new_value(HDS_JSON_NUMBER);
            if (v) v->number = num;
            return v;
        }
    }
}

// Top-level entry point: parses a whole JSON document, requiring the entire (whitespace-trimmed)
// input to be consumed -- trailing garbage after a syntactically valid value is still a malformed
// document, not a value to silently truncate to. Returns NULL on any parse failure.
static HdsJsonValue *hds_json_parse(const char *text) {
    const char *p = text;
    HdsJsonValue *root = hds_json_parse_value(&p);
    if (!root) return NULL;
    hds_json_skip_ws(&p);
    if (*p != '\0') { hds_json_free(root); return NULL; }
    return root;
}

// Linear scan -- these are small, hand-edited-file-sized objects (a Program's own top-level keys,
// or its "fields"/"sfz_file" sub-objects), never a hot path, so a hash lookup would be pure
// overkill (unlike osc_dispatch_table, which really does need O(1) for every incoming OSC message
// during real-time playback).
static HdsJsonValue *hds_json_object_get(const HdsJsonValue *obj, const char *key) {
    if (!obj || obj->type != HDS_JSON_OBJECT) return NULL;
    for (HdsJsonMember *m = obj->first_member; m; m = m->next) {
        if (m->key && strcmp(m->key, key) == 0) return m->value;
    }
    return NULL;
}

// --- --program <path.hdsprog>: load a full Program (every field frontend.py's own Program Save
// writes into "fields", scoped by _is_program_scope_address()) directly from the backend's own
// local filesystem -- no frontend needs to ever connect, matching --sfz-vcoN's own posture. Reuses
// osc_dispatch_table (the exact same address->offset map generic_osc_handler() itself reads), so a
// Program field can never mean something different here than it does live over OSC -- and reuses
// hds_load_sfz_for_osc() for the "sfz_file" map, the exact same core --sfz-vcoN and
// /hds/zone/load_sfz both call. A bad/missing/malformed file logs and continues starting, same
// failure posture as a bad --sfz-vcoN path -- never fatal.
static void hds_apply_program_fields(SynthSharedControls *knobs, const HdsJsonValue *fields_obj) {
    int applied = 0, skipped = 0;
    for (const HdsJsonMember *m = fields_obj->first_member; m; m = m->next) {
        if (!m->key) continue;
        const OscDispatchSlot *slot = osc_dispatch_lookup(m->key);
        if (!slot) {
            fprintf(stderr, "[program] unrecognized address '%s' ignored\n", m->key);
            skipped++;
            continue;
        }
        if (slot->kind == OSC_KIND_FLOAT) {
            if (m->value->type != HDS_JSON_NUMBER) {
                fprintf(stderr, "[program] '%s' expected a number, skipped\n", m->key);
                skipped++;
                continue;
            }
            *(float *)((char *)knobs + slot->offset) = (float)m->value->number;
        } else { // OSC_KIND_STRING
            if (m->value->type != HDS_JSON_STRING) {
                fprintf(stderr, "[program] '%s' expected a string, skipped\n", m->key);
                skipped++;
                continue;
            }
            snprintf((char *)knobs + slot->offset, slot->str_len, "%s", m->value->string);
        }
        applied++;
    }
    fprintf(stderr, "[program] %d field(s) applied, %d skipped\n", applied, skipped);
}

// sfz_file is {"<osc_index 0-3>": "<filename>"} -- filenames are relative to the .hdsprog's own
// directory (exactly where Program Save wrote them, alongside the .hdsprog itself -- see
// _pick_and_save_program()'s own sfz_map comment in frontend.py), so program_dir (the --program
// path's own directory, resolved once by the caller) is joined on here.
static void hds_apply_program_sfz_files(HdsSampleEngine *se, const HdsJsonValue *sfz_obj, const char *program_dir) {
    for (const HdsJsonMember *m = sfz_obj->first_member; m; m = m->next) {
        if (!m->key || m->value->type != HDS_JSON_STRING) continue;
        int osc_index = atoi(m->key);
        if (osc_index < 0 || osc_index > 3) {
            fprintf(stderr, "[program] sfz_file key '%s' is not a valid oscillator index (0-3), skipped\n", m->key);
            continue;
        }
        // program_dir and m->value->string can each be up to SAMPLE_PATH_LEN on their own --
        // sized for both plus the joining '/' with room to spare, not SAMPLE_PATH_LEN itself
        // (which the compiler correctly flags as too small to guarantee no truncation).
        char full_path[SAMPLE_PATH_LEN * 2];
        snprintf(full_path, sizeof(full_path), "%s/%s", program_dir, m->value->string);
        int n = hds_load_sfz_for_osc(se, osc_index, SINGLE_PROGRAM_SLOT, full_path);
        fprintf(stderr, "[program] sfz_file[%d] '%s': %d zone(s) queued\n", osc_index, full_path, n);
    }
}

static void hds_load_program_file(SynthSharedControls *knobs, HdsSampleEngine *se, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[program] --program '%s': %s\n", path, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        fprintf(stderr, "[program] --program '%s': empty or unreadable, skipping\n", path);
        return;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';

    HdsJsonValue *root = hds_json_parse(buf);
    free(buf);
    if (!root || root->type != HDS_JSON_OBJECT) {
        fprintf(stderr, "[program] --program '%s': not valid JSON, skipping\n", path);
        hds_json_free(root);
        return;
    }

    char program_dir[SAMPLE_PATH_LEN];
    snprintf(program_dir, sizeof(program_dir), "%s", path);
    char *slash = strrchr(program_dir, '/');
    if (slash) *slash = '\0'; else snprintf(program_dir, sizeof(program_dir), ".");

    const HdsJsonValue *fields = hds_json_object_get(root, "fields");
    if (fields && fields->type == HDS_JSON_OBJECT) hds_apply_program_fields(knobs, fields);

    const HdsJsonValue *sfz_file = hds_json_object_get(root, "sfz_file");
    if (sfz_file && sfz_file->type == HDS_JSON_OBJECT) hds_apply_program_sfz_files(se, sfz_file, program_dir);

    hds_json_free(root);
    fprintf(stderr, "[program] --program '%s': loaded\n", path);
}


// --- Interactive arrow-key CLI control (Up/Down = master volume, Left/Right = cycle saved
// Programs) --- entirely optional, zero-risk, best-effort: guarded by isatty() so a headless/
// systemd/piped launch (stdin not a real terminal) is completely unaffected -- poll_stdin_control()
// below is a no-op unless hds_stdin_raw_mode_enable() actually managed to put a real terminal into
// raw mode. Every load this triggers (arrow-key Program cycling) reuses hds_load_program_file()
// itself, so it's the exact same bounded, small-text-file-only work --program already does at
// startup, just triggered live instead -- and per the existing live /hds/zone/load_sfz OSC handler,
// this file's PCM sample loading is already deferred/throttled (SAMPLE_LOADS_PER_BLOCK), so nothing
// here can block the audio thread on real disk I/O.
static struct termios g_stdin_orig_termios;
static bool g_stdin_raw_mode_active = false;

static void hds_stdin_raw_mode_enable(void) {
    if (!isatty(STDIN_FILENO)) return; // piped/redirected/headless stdin -- leave it alone entirely
    if (tcgetattr(STDIN_FILENO, &g_stdin_orig_termios) != 0) return;
    struct termios raw = g_stdin_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); // no line buffering, no local echo of the raw escape bytes
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags == -1 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_orig_termios); // couldn't make it non-blocking -- don't leave the terminal half-configured
        return;
    }
    g_stdin_raw_mode_active = true;
    fprintf(stderr, "[cli] interactive terminal detected -- Up/Down: master volume, Left/Right: cycle saved Programs\n");
}

static void hds_stdin_raw_mode_restore(void) {
    if (g_stdin_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_orig_termios);
        g_stdin_raw_mode_active = false;
    }
}

static int hds_qsort_strcmp(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

// Sorted list of saved Program names -- one per ~/HDS_Programs/<name>/<name>.hdsprog subfolder,
// mirroring _scan_programs_folder() in frontend.py exactly (same folder convention, same
// alphabetical order) so Left/Right cycles through the identical list the frontend's own "<>"
// buttons would. Caller owns the returned array (and each of its strings) via free(); returns NULL
// (out_count 0) if ~/HDS_Programs doesn't exist or has no saved Programs.
static char **hds_scan_programs_folder(int *out_count) {
    *out_count = 0;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char root[SAMPLE_PATH_LEN];
    snprintf(root, sizeof(root), "%s/HDS_Programs", home);
    DIR *d = opendir(root);
    if (!d) return NULL;
    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // skip "."/".."/hidden
        char check_path[SAMPLE_PATH_LEN + 2 * (NAME_MAX + 1) + 16]; // root + "/" + d_name + "/" + d_name + ".hdsprog"
        snprintf(check_path, sizeof(check_path), "%s/%s/%s.hdsprog", root, ent->d_name, ent->d_name);
        FILE *f = fopen(check_path, "r");
        if (!f) continue; // not a valid Program subfolder -- same test frontend.py's own scan uses
        fclose(f);
        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            char **grown = realloc(names, sizeof(char *) * (size_t)cap);
            if (!grown) break; // allocation failure -- keep whatever's already collected
            names = grown;
        }
        names[count++] = strdup(ent->d_name);
    }
    closedir(d);
    if (count == 0) { free(names); return NULL; }
    qsort(names, (size_t)count, sizeof(char *), hds_qsort_strcmp);
    *out_count = count;
    return names;
}

// Remembers the currently-loaded Program's name (best-effort -- seeded from --program at startup,
// updated on every arrow-key cycle) so Left/Right can start cycling RELATIVE to it, matching
// _cycle_program()'s own "if the current text matches a saved Program, cycle relative to it"
// behavior in frontend.py -- an unrecognized/no current Program just starts at the first (Right) or
// last (Left) saved Program instead, same as there.
static char g_stdin_current_program_name[SAMPLE_PATH_LEN] = {0};

static void hds_cycle_program(SynthSharedControls *knobs, HdsSampleEngine *se, int delta) {
    int count = 0;
    char **names = hds_scan_programs_folder(&count);
    if (!names) {
        fprintf(stderr, "[cli] no saved Programs in ~/HDS_Programs -- nothing to cycle to\n");
        return;
    }
    int current_idx = -1;
    if (g_stdin_current_program_name[0]) {
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], g_stdin_current_program_name) == 0) { current_idx = i; break; }
        }
    }
    int next_idx = (current_idx >= 0) ? (((current_idx + delta) % count + count) % count)
                                       : (delta > 0 ? 0 : count - 1);
    const char *home = getenv("HOME"); // already validated non-NULL by hds_scan_programs_folder finding a folder under it
    char path[SAMPLE_PATH_LEN + 2 * (NAME_MAX + 1) + 32];
    snprintf(path, sizeof(path), "%s/HDS_Programs/%s/%s.hdsprog", home, names[next_idx], names[next_idx]);
    fprintf(stderr, "[cli] %s: loading Program '%s'\n", delta > 0 ? "Right" : "Left", names[next_idx]);
    hds_load_program_file(knobs, se, path);
    snprintf(g_stdin_current_program_name, sizeof(g_stdin_current_program_name), "%s", names[next_idx]);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

// Called once per main-loop iteration, alongside the OSC/MIDI polls -- a true no-op (one isatty-
// gated bool check) unless hds_stdin_raw_mode_enable() actually put a real terminal into raw mode.
// Parses ANSI cursor-key escape sequences (ESC '[' 'A'/'B'/'C'/'D') byte-by-byte via a small state
// machine that persists across calls, since a slow terminal/pty could in principle deliver an
// escape sequence's 3 bytes across more than one non-blocking read.
static void poll_stdin_control(SynthSharedControls *knobs, HdsSampleEngine *se) {
    if (!g_stdin_raw_mode_active) return;
    static int esc_state = 0; // 0 = idle, 1 = saw ESC, 2 = saw ESC '['
    unsigned char buf[64];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (esc_state == 0) {
                if (c == 0x1b) esc_state = 1;
                continue;
            }
            if (esc_state == 1) {
                esc_state = (c == '[') ? 2 : 0;
                continue;
            }
            esc_state = 0; // esc_state == 2: this byte is the final one, recognized or not
            if (c == 'A') { // Up
                knobs->master_volume = fmaxf(0.0f, fminf(1.0f, knobs->master_volume + 0.02f));
                fprintf(stderr, "[cli] Up: master volume -> %.3f\n", (double)knobs->master_volume);
            } else if (c == 'B') { // Down
                knobs->master_volume = fmaxf(0.0f, fminf(1.0f, knobs->master_volume - 0.02f));
                fprintf(stderr, "[cli] Down: master volume -> %.3f\n", (double)knobs->master_volume);
            } else if (c == 'C') { // Right
                hds_cycle_program(knobs, se, 1);
            } else if (c == 'D') { // Left
                hds_cycle_program(knobs, se, -1);
            }
        }
    }
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
    // --program <path.hdsprog>: load a full Program (see hds_load_program_file()'s own comment)
    // directly from the backend's own local filesystem at startup -- same "no frontend needs to
    // ever connect" posture as --sfz-vcoN above.
    const char *cli_program_path = NULL;
    // --master-volume <0.0-1.0>: set the master volume once at startup -- independent of
    // --program (a Program's own saved fields never include master_volume at all, see
    // _is_program_scope_address()'s own comment in frontend.py, so there is no ordering to get
    // right between the two flags: nothing about loading a Program can ever touch this value).
    float cli_master_volume = 0.0f;
    bool cli_master_volume_set = false;
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
        else if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) cli_program_path = argv[++i];
        else if (strcmp(argv[i], "--master-volume") == 0 && i + 1 < argc) { cli_master_volume = (float)atof(argv[++i]); cli_master_volume_set = true; }
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

#if HDS_NAM_ENABLED
    // FX slot NAM model/cab IR upload -- see fx_upload_begin_handler's own comment. Gated the same
    // as the rest of NAM support (HDS_NAM_ENABLED) since it exists purely to feed that FX type.
    HdsFxUploadContext fx_upload_ctx = { &g_engine, &active_knobs };
    lo_server_add_method(osc_server, "/hds/fx/upload_begin", "iiii", fx_upload_begin_handler, &fx_upload_ctx);
    lo_server_add_method(osc_server, "/hds/fx/upload_chunk", "ib", fx_upload_chunk_handler, &fx_upload_ctx);
    lo_server_add_method(osc_server, "/hds/fx/upload_end", "", fx_upload_end_handler, &fx_upload_ctx);
#endif

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

    if (cli_program_path) {
        hds_load_program_file(&active_knobs, &g_sample_engine, cli_program_path);
        // Best-effort seed for the arrow-key Left/Right cycle's "start relative to the current
        // Program" behavior (see hds_cycle_program's own comment) -- just the basename minus its
        // extension, regardless of whether this path actually lives under ~/HDS_Programs. If it
        // doesn't match any name hds_scan_programs_folder() later finds, cycling gracefully falls
        // back to starting at the first/last saved Program instead, same as an unrecognized name in
        // frontend.py's own _cycle_program().
        const char *base = strrchr(cli_program_path, '/');
        base = base ? base + 1 : cli_program_path;
        snprintf(g_stdin_current_program_name, sizeof(g_stdin_current_program_name), "%s", base);
        char *dot = strrchr(g_stdin_current_program_name, '.');
        if (dot) *dot = '\0';
    }
    if (cli_master_volume_set) {
        active_knobs.master_volume = cli_master_volume;
        fprintf(stderr, "[cli] --master-volume %.3f\n", (double)cli_master_volume);
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

    hds_stdin_raw_mode_enable(); // no-op (prints nothing) unless stdin is a real interactive terminal

    while (g_synth_running) {
        // Drain every pending OSC datagram this block -- generic_osc_handler/zone_upsert_handler/
        // zone_remove_handler write directly into active_knobs/g_sample_slots as each message is
        // dispatched, so nothing further is needed here once this returns 0 (no more pending).
        while (lo_server_recv_noblock(osc_server, 0) > 0) {}
        poll_stdin_control(&active_knobs, &g_sample_engine); // arrow keys: Up/Down = master volume, Left/Right = Program cycle -- see its own comment
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
        float gr_db[FX_SLOT_COUNT];
        float block_peak = render_audio_block(&g_sample_engine, &g_engine, voices, &active_knobs, global_lfo_phases, capture_buffer, detected_input_channels, detected_output_channels, &midi_live, buffer, current_tempo_bpm, ext_in_peak, NULL, gr_db);
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
            for (int s = 0; s < FX_SLOT_COUNT; s++) status.fx_gain_reduction_db[s] = gr_db[s];
            send_osc_status(osc_reply_addr, &status);
            send_seq_audio_status(&g_streams_state, osc_reply_addr);
            vu_decimator = 0;
        }
        write_audio_block(pcm_handle, buffer, detected_output_channels, block_size, active_voices_count, render_ms);
    }
    hds_stdin_raw_mode_restore(); // no-op unless hds_stdin_raw_mode_enable() actually changed the terminal
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
