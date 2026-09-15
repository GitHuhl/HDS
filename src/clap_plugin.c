// HDS as a Linux CLAP instrument plugin.
//
// Reuses the entire engine unchanged via the same "#define main .../#include backend.c" pattern
// this project's own scratchpad test files already use to pull in backend.c's ~400 static
// functions/globals with zero header-extraction refactor -- backend.c itself is not modified at
// all for this (see /root/.claude/plans/wobbly-marinating-prism.md, "Linux CLAP plugin support",
// Phase 1: every function main() calls to drive the engine one block at a time --
// poll_seq_audio_load_requests, drain_pending_sample_loads, dispatch_midi_event,
// render_audio_block, etc. -- is already a standalone function with explicit parameters, so this
// file just calls those same functions directly instead of main()'s own ALSA/WASAPI device-I/O
// loop.
//
// As a plugin, HDS stops owning audio device I/O and MIDI device I/O entirely -- the host
// supplies both via CLAP's audio-ports/note-ports extensions -- but keeps its own control
// surface unchanged: this file starts its own embedded OSC server (default ports 9010/9011,
// distinct from the standalone app's 9000/9001) so frontend.py can drive it exactly as it drives
// the standalone app, just pointed at a different port. There is no CLAP `params` extension in
// this first version; the ~1150 knobs stay 100% OSC-driven.
//
// Windows CLAP support and a CLAP `params`/`state` round-trip are explicitly out of scope for
// this version -- see the plan doc's own stated scope boundaries.

#include "../third_party/clap/include/clap/clap.h"

#define main backend_main_unused
#include "backend.c"
#undef main

// ---------------------------------------------------------------------------------------------
// Single-instance-per-process guard
// ---------------------------------------------------------------------------------------------
// backend.c has substantial file-scope/process-wide global state that was never built for two
// independent synth "sessions" to coexist in one address space -- g_sample_slots[]/
// g_sample_streams[]/g_sample_disk_thread (the DFD sample-streaming engine), g_seq_audio[]/
// g_seq_midi[] (Sequencer lanes), g_transport_state/g_transport_playhead_sec, g_pending_loads[],
// the OSC dispatch table, etc. This is invisible in the standalone app (always exactly one
// process, one synth) but a CLAP host loading TWO plugin instances into the same process (e.g.
// two DAW tracks) would silently share/corrupt this state. Rather than allow that, a second
// concurrent instance is refused outright at activate() time -- loud, not silent -- matching the
// user's own stated test plan (one instance, in one host, tested by hand first). Real
// multi-instance support (threading an instance context through every global) is a much bigger
// follow-up, explicitly out of scope here.
static _Atomic int g_clap_instance_count = 0;

// build_osc_dispatch_table() builds a process-wide static table and calls exit(1) on a duplicate
// insert -- calling it more than once per process would hit that path. It only needs to run once
// ever, regardless of how many plugin instances get created/destroyed in this process (the table
// maps OSC address strings to struct-member byte offsets, which is identical for every instance),
// so it's built once here rather than per-instance.
static bool g_osc_dispatch_table_ready = false;

// Default embedded OSC ports -- distinct from the standalone app's 9000/9001 (see this file's own
// header comment) so both could in principle run side by side without colliding. A plugin has no
// argv (hosts don't pass command-line flags), so this is a fixed default, not configurable.
#define CLAP_OSC_LISTEN_PORT 9010
#define CLAP_OSC_REPLY_HOST "127.0.0.1"
#define CLAP_OSC_REPLY_PORT 9011

// ---------------------------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------------------------

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;

    SynthSharedControls knobs;
    SynthVoice voices[MAX_VOICES];
    MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1];
    float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    MidiLiveState midi_live;

    uint64_t total_frames_rendered;
    int vu_decimator;

    lo_server osc_server;
    lo_address osc_reply_addr;
    bool osc_ready; // true once activate() successfully opened the OSC server this session

    bool holds_instance_slot; // did this instance claim g_clap_instance_count? (only true instance may release it)
} HdsClapPlugin;

// ---------------------------------------------------------------------------------------------
// audio-ports extension: 1 stereo output, 0 inputs (external audio input is a standalone-only
// feature for now -- matches the existing no-capture-device code path render_audio_block already
// exercises when ext_in_channels=0/a NULL buffer).
// ---------------------------------------------------------------------------------------------

static uint32_t clap_audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? 0 : 1;
}

static bool clap_audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void)plugin;
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t k_audio_ports_ext = {
    .count = clap_audio_ports_count,
    .get = clap_audio_ports_get,
};

// ---------------------------------------------------------------------------------------------
// note-ports extension: 1 input port accepting either native CLAP note events or raw MIDI, 0
// outputs (HDS never sends CLAP_EVENT_NOTE_END -- this engine has no per-voice lifecycle tracking
// keyed to a host-issued note_id, matching the fact that the standalone app's own MIDI decoders
// never needed one either).
// ---------------------------------------------------------------------------------------------

static uint32_t clap_note_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? 1 : 0;
}

static bool clap_note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    (void)plugin;
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(info->name, sizeof(info->name), "%s", "MIDI In");
    return true;
}

static const clap_plugin_note_ports_t k_note_ports_ext = {
    .count = clap_note_ports_count,
    .get = clap_note_ports_get,
};

// ---------------------------------------------------------------------------------------------
// state extension: version-header-only stub. Does NOT round-trip `knobs` yet -- reopening a
// saved DAW project loads HDS at its default patch; the user reconnects frontend.py (OSC) and
// re-establishes sound design manually. SynthSharedControls is flat POD with no pointers, so a
// real fwrite/fread round-trip is mechanically easy to add later, but needs a real version/
// migration scheme to not be build-fragile against this synth's still-growing struct -- a stated
// follow-up, not attempted here.
// ---------------------------------------------------------------------------------------------

#define HDS_CLAP_STATE_VERSION 1

static bool clap_state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    (void)plugin;
    uint32_t version = HDS_CLAP_STATE_VERSION;
    return stream->write(stream, &version, sizeof(version)) == (int64_t)sizeof(version);
}

static bool clap_state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    (void)plugin;
    uint32_t version = 0;
    int64_t got = stream->read(stream, &version, sizeof(version));
    // Reject anything that isn't exactly this stub's own 4-byte version header -- an empty
    // stream, a short read, or unrecognized bytes (e.g. state saved by a future version of this
    // stub, or random/foreign data) are all real failures, not silently accepted successes.
    if (got != (int64_t)sizeof(version) || version != HDS_CLAP_STATE_VERSION) return false;
    return true;
}

static const clap_plugin_state_t k_state_ext = {
    .save = clap_state_save,
    .load = clap_state_load,
};

// ---------------------------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------------------------

static bool clap_plugin_init(const clap_plugin_t *plugin) {
    (void)plugin;
    // Built once ever per process -- see g_osc_dispatch_table_ready's own comment. Safe to call
    // from plugin.init() (main-thread, per the CLAP spec) even if a host creates several
    // instances (e.g. while scanning) before ever activating one.
    if (!g_osc_dispatch_table_ready) {
        assert_fm_algorithms_acyclic(); // same startup self-check main() runs before anything else
        build_osc_dispatch_table();
        g_osc_dispatch_table_ready = true;
    }
    return true;
}

static void clap_plugin_destroy(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    free(p);
}

static bool clap_plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames_count, uint32_t max_frames_count) {
    (void)min_frames_count; (void)max_frames_count;
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;

    // SAMPLE_RATE is a hard compile-time constant throughout this engine (oscillator phase
    // increments, envelope/LFO timing, the whole DSP core) -- not runtime-adjustable anywhere.
    // Reject rather than silently render at the wrong pitch/tempo.
    if ((int)(sample_rate + 0.5) != SAMPLE_RATE) {
        fprintf(stderr, "[hds-clap] refusing activate: host requested %.0fHz, HDS only supports %dHz\n", sample_rate, SAMPLE_RATE);
        return false;
    }

    // Single-instance-per-process guard -- see its own top-of-file comment.
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_clap_instance_count, &expected, 1)) {
        fprintf(stderr, "[hds-clap] refusing to activate a second concurrent HDS instance in this process "
                        "(v1 limitation -- backend.c's sample-streaming/Sequencer/transport state is process-wide, "
                        "not per-instance; use only one HDS instance per host process for now)\n");
        return false;
    }
    p->holds_instance_slot = true;

    // Embedded OSC server -- same non-threaded, drained-once-per-block-from-the-audio-thread
    // design main() uses (see main()'s own comment on why: only one thread ever touches
    // active_knobs/g_sample_slots, no locking needed). Bind failure is logged but not fatal to
    // activation: the synth can still make sound (default patch, MIDI-driven) with no external
    // control surface, which is strictly better than refusing to activate at all.
    build_osc_dispatch_table(); // no-op if already built (guarded internally by g_osc_dispatch_table_ready via plugin.init(), called first per the CLAP spec) -- called again here defensively in case a host ever calls activate() without a preceding init() on this exact instance is impossible per spec, so this is just belt-and-suspenders
    char listen_port_str[16]; snprintf(listen_port_str, sizeof(listen_port_str), "%d", CLAP_OSC_LISTEN_PORT);
    p->osc_server = lo_server_new_with_proto(listen_port_str, LO_UDP, osc_error_handler);
    if (p->osc_server) {
        lo_server_add_method(p->osc_server, "/hds/zone/upsert", "ifffffffffffffffffs", zone_upsert_handler, NULL);
        lo_server_add_method(p->osc_server, "/hds/zone/remove", "i", zone_remove_handler, NULL);
        lo_server_add_method(p->osc_server, NULL, NULL, generic_osc_handler, &p->knobs);
        char reply_port_str[16]; snprintf(reply_port_str, sizeof(reply_port_str), "%d", CLAP_OSC_REPLY_PORT);
        p->osc_reply_addr = lo_address_new(CLAP_OSC_REPLY_HOST, reply_port_str);
        p->osc_ready = true;
        fprintf(stderr, "[hds-clap] OSC listening on UDP port %d, status replies to %s:%d\n", CLAP_OSC_LISTEN_PORT, CLAP_OSC_REPLY_HOST, CLAP_OSC_REPLY_PORT);
    } else {
        fprintf(stderr, "[hds-clap] failed to open OSC listen socket on port %d (already in use by another HDS instance?) "
                        "-- continuing with no external control surface\n", CLAP_OSC_LISTEN_PORT);
        p->osc_ready = false;
    }

    // Sample-streaming disk thread (DFD/WAVE_SAMPLE) -- same "started once, talked to only via
    // atomics" pattern main() uses. g_sample_disk_thread_shutdown may still be `true` from a
    // PREVIOUS activate()/deactivate() cycle of this same instance (or an earlier instance in
    // this process) -- reset it before starting, or the thread would see shutdown already
    // requested and exit immediately. Safe: deactivate() only sets it after pthread_join()
    // confirms the old thread has fully exited.
    atomic_store_explicit(&g_sample_disk_thread_shutdown, false, memory_order_release);
    bool disk_thread_ok = (pthread_create(&g_sample_disk_thread, NULL, sample_disk_thread_main, NULL) == 0);
    if (!disk_thread_ok) {
        fprintf(stderr, "[hds-clap] failed to start sample disk streaming thread -- WAVE_SAMPLE playback unavailable this session\n");
    }

    p->total_frames_rendered = 0;
    p->vu_decimator = 0;
    return true;
}

static void clap_plugin_deactivate(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;

    atomic_store_explicit(&g_sample_disk_thread_shutdown, true, memory_order_release);
    pthread_join(g_sample_disk_thread, NULL);
    atomic_store_explicit(&g_sample_disk_thread_shutdown, false, memory_order_release); // see activate()'s own comment on why this must be reset for a future reactivation

    if (p->osc_ready) {
        if (p->osc_reply_addr) { lo_address_free(p->osc_reply_addr); p->osc_reply_addr = NULL; }
        lo_server_free(p->osc_server); p->osc_server = NULL;
        p->osc_ready = false;
    }

    if (p->holds_instance_slot) {
        atomic_store_explicit(&g_clap_instance_count, 0, memory_order_release);
        p->holds_instance_slot = false;
    }
}

static bool clap_plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void clap_plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }

static void clap_plugin_reset(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    // Full reset of processing state (voices, per-voice filter/oscillator/envelope state, held-
    // note stacks, LFO phases, live controller state) -- parameter values (p->knobs) unchanged,
    // matching CLAP's own contract for reset(). Does NOT rewind the Sequencer transport (OSC-
    // driven, not host-transport-driven, matching the standalone app's existing design). FX/
    // limiter internal state is a function-local `static` inside render_audio_block, not reset
    // here -- not reachable from outside that function without a backend.c change, a documented,
    // minor limitation rather than a silent gap.
    for (int i = 0; i < MAX_VOICES; i++) init_voice(&p->voices[i]);
    for (int i = 0; i <= COMBI_PROGRAM_COUNT; i++) p->mono_stacks[i] = (MonoNoteStack){0};
    memset(p->global_lfo_phases, 0, sizeof(p->global_lfo_phases));
    p->midi_live = (MidiLiveState){0};
}

// ---------------------------------------------------------------------------------------------
// process(): the audio thread's own per-callback entry point.
//
// Decodes note/MIDI events from process->in_events, drains the embedded OSC server, then renders
// in <=BUFFER_SIZE-frame chunks (the host may hand over more frames per call than BUFFER_SIZE),
// de-interleaving each chunk into the CLAP host's own planar stereo output buffer.
//
// Sample-accurate event timing is NOT preserved here: every input event for this whole process()
// call is applied before any audio is rendered, exactly matching the standalone app's own
// once-per-block (not once-per-sample) MIDI poll -- a deliberate, stated simplification, not an
// oversight.
// ---------------------------------------------------------------------------------------------

static clap_process_status clap_plugin_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;

    if (p->osc_ready) {
        while (lo_server_recv_noblock(p->osc_server, 0) > 0) {}
    }

    // Engine-driving polls that main() also runs once per block -- see this file's own header
    // comment. Sequencer/Combi lanes are fully reachable via OSC (performance_mode, seq_lanes[],
    // etc. are ordinary knobs), so this plugin supports the same architecture as the standalone
    // app, not just a stripped-down MIDI-only mode.
    poll_seq_audio_load_requests(&p->knobs);
    poll_seq_audio_preview_triggers(&p->knobs);
    double engine_elapsed_seconds = (double)p->total_frames_rendered / (double)SAMPLE_RATE;
    float current_tempo_bpm = compute_current_tempo_bpm(&p->knobs, engine_elapsed_seconds);
    poll_transport_triggers(&p->knobs, current_tempo_bpm);
    poll_metronome(&p->knobs, current_tempo_bpm);
    poll_seq_audio_transport_catchup(&p->knobs);
    drain_pending_sample_loads();

    if (process->in_events) {
        uint32_t ev_count = process->in_events->size(process->in_events);
        for (uint32_t i = 0; i < ev_count; i++) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, i);
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_NOTE_OFF) {
                const clap_event_note_t *ev = (const clap_event_note_t *)hdr;
                if (ev->key < 0 || ev->key > 127 || ev->channel < 0 || ev->channel > 15) continue; // wildcard note-on/off, not supported by this direct-mapping engine
                double vel = ev->velocity < 0.0 ? 0.0 : (ev->velocity > 1.0 ? 1.0 : ev->velocity);
                int velocity_127 = (int)(vel * 127.0 + 0.5);
                dispatch_midi_event(hdr->type == CLAP_EVENT_NOTE_ON ? HDS_MIDI_NOTE_ON : HDS_MIDI_NOTE_OFF,
                                     (int)ev->channel + 1, (int)ev->key, velocity_127,
                                     p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live);
            } else if (hdr->type == CLAP_EVENT_MIDI) {
                const clap_event_midi_t *ev = (const clap_event_midi_t *)hdr;
                unsigned char status = ev->data[0] & 0xF0;
                int midi_channel_1_16 = (ev->data[0] & 0x0F) + 1;
                // Same raw-byte decode convention backend.c's own ALSA/WinMM readers already use
                // (see process_midi_input) -- a Note On with velocity 0 counts as Note Off.
                if (status == 0x90 || status == 0x80) {
                    bool is_note_on = (status == 0x90 && ev->data[2] > 0);
                    dispatch_midi_event(is_note_on ? HDS_MIDI_NOTE_ON : HDS_MIDI_NOTE_OFF, midi_channel_1_16, ev->data[1], ev->data[2],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live);
                } else if (status == 0xB0) {
                    dispatch_midi_event(HDS_MIDI_CC, midi_channel_1_16, ev->data[1], ev->data[2],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live);
                } else if (status == 0xE0) {
                    int bend14 = (int)ev->data[1] | ((int)ev->data[2] << 7);
                    dispatch_midi_event(HDS_MIDI_PITCH_BEND, midi_channel_1_16, 0, bend14 - 8192,
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live);
                } else if (status == 0xD0) {
                    dispatch_midi_event(HDS_MIDI_CHANNEL_PRESSURE, midi_channel_1_16, 0, ev->data[1],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live);
                }
            }
        }
    }

    seq_playback_midi_events(p->voices, &p->knobs, p->global_lfo_phases, &p->mono_stacks[SINGLE_PROGRAM_SLOT], &p->midi_live);

    float **out = NULL;
    if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32 &&
        process->audio_outputs[0].channel_count >= 2 && process->audio_outputs[0].data32[0] && process->audio_outputs[0].data32[1]) {
        out = process->audio_outputs[0].data32;
        process->audio_outputs[0].constant_mask = 0;
    }

    float block_peak = 0.0f;
    uint32_t frames_done = 0;
    while (frames_done < process->frames_count) {
        uint32_t remaining = process->frames_count - frames_done;
        uint32_t chunk = remaining < (uint32_t)BUFFER_SIZE ? remaining : (uint32_t)BUFFER_SIZE;

        // Block-size subdivision: the host may hand over more frames per call than BUFFER_SIZE.
        // Setting block_size_frames per chunk lets effective_block_size()'s existing [1,
        // BUFFER_SIZE] clamp do the right thing regardless of whatever the user's own OSC Block
        // Size knob is separately set to -- effective size ends up as the min of the two,
        // matching the clamp's existing spirit. A smaller knob value just means more, smaller
        // chunks; frames_done always advances by the ACTUAL frames rendered, so this loop still
        // terminates correctly either way.
        p->knobs.block_size_frames = (float)chunk;
        int block_size = effective_block_size(&p->knobs);
        advance_transport_playhead(block_size);

        static float chunk_buffer[BUFFER_SIZE * 2];
        float peak = render_audio_block(p->voices, &p->knobs, p->global_lfo_phases, NULL, 0, 2, &p->midi_live, chunk_buffer);
        if (peak > block_peak) block_peak = peak;

        if (out) {
            for (int i = 0; i < block_size; i++) {
                out[0][frames_done + (uint32_t)i] = chunk_buffer[i * 2 + 0];
                out[1][frames_done + (uint32_t)i] = chunk_buffer[i * 2 + 1];
            }
        }

        p->total_frames_rendered += (uint64_t)block_size;
        frames_done += (uint32_t)block_size;
    }

    if (p->osc_ready) {
        p->vu_decimator++;
        if (p->vu_decimator >= 12) {
            EngineStatus status = {0};
            status.peak = block_peak;
            status.detected_output_channels = 2.0f;
            status.tempo_bpm_live = current_tempo_bpm;
            status.seq_elapsed_seconds = (float)g_transport_playhead_sec;
            status.seq_transport_state = (float)g_transport_state;
            send_osc_status(p->osc_reply_addr, &status);
            send_seq_audio_status(p->osc_reply_addr);
            p->vu_decimator = 0;
        }
    }

    return CLAP_PROCESS_CONTINUE;
}

static const void *clap_plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &k_audio_ports_ext;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &k_note_ports_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &k_state_ext;
    return NULL; // NULL for everything else (including params) is spec-compliant -- hosts must tolerate it
}

static void clap_plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

// ---------------------------------------------------------------------------------------------
// Descriptor + factory
// ---------------------------------------------------------------------------------------------

static const char *const k_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, NULL};

static const clap_plugin_descriptor_t k_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.hds-synth.hds",
    .name = "HDS",
    .vendor = "",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "HDS synthesizer engine as a Linux CLAP instrument. OSC-controlled (frontend.py, "
                    "default ports 9010/9011) -- no host-automatable parameters in this version.",
    .features = k_features,
};

static const clap_plugin_t *clap_factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void)factory;
    if (strcmp(plugin_id, k_descriptor.id) != 0) return NULL;

    HdsClapPlugin *p = (HdsClapPlugin *)calloc(1, sizeof(HdsClapPlugin));
    if (!p) return NULL;

    p->host = host;
    init_default_controls(&p->knobs);
    for (int i = 0; i < MAX_VOICES; i++) init_voice(&p->voices[i]);
    for (int i = 0; i <= COMBI_PROGRAM_COUNT; i++) p->mono_stacks[i] = (MonoNoteStack){0};

    p->plugin.desc = &k_descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = clap_plugin_init;
    p->plugin.destroy = clap_plugin_destroy;
    p->plugin.activate = clap_plugin_activate;
    p->plugin.deactivate = clap_plugin_deactivate;
    p->plugin.start_processing = clap_plugin_start_processing;
    p->plugin.stop_processing = clap_plugin_stop_processing;
    p->plugin.reset = clap_plugin_reset;
    p->plugin.process = clap_plugin_process;
    p->plugin.get_extension = clap_plugin_get_extension;
    p->plugin.on_main_thread = clap_plugin_on_main_thread;

    return &p->plugin;
}

static uint32_t clap_factory_get_plugin_count(const clap_plugin_factory_t *factory) { (void)factory; return 1; }

static const clap_plugin_descriptor_t *clap_factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void)factory;
    return index == 0 ? &k_descriptor : NULL;
}

static const clap_plugin_factory_t k_factory = {
    .get_plugin_count = clap_factory_get_plugin_count,
    .get_plugin_descriptor = clap_factory_get_plugin_descriptor,
    .create_plugin = clap_factory_create_plugin,
};

// ---------------------------------------------------------------------------------------------
// DSO entry point
// ---------------------------------------------------------------------------------------------

static bool clap_entry_init(const char *plugin_path) { (void)plugin_path; return true; }
static void clap_entry_deinit(void) {}

static const void *clap_entry_get_factory(const char *factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &k_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = clap_entry_init,
    .deinit = clap_entry_deinit,
    .get_factory = clap_entry_get_factory,
};
