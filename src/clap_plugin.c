// HDS as a Linux CLAP instrument plugin.
//
// Reuses the entire engine unchanged via the same "#define main .../#include backend.c" pattern
// this project's own scratchpad test files already use to pull in backend.c's ~400 static
// functions/globals with zero header-extraction refactor -- backend.c itself is not modified at
// all for this (see /root/.claude/plans/wobbly-marinating-prism.md, "Linux CLAP plugin support",
// Phase 1: every function main() calls to drive the engine one block at a time --
// poll_seq_audio_load_requests, drain_pending_sample_loads, dispatch_midi_event,
// render_audio_block, etc. -- is already a standalone function with explicit parameters, so this
// file just calls those same functions directly instead of main()'s own ALSA device-I/O loop.
//
// As a plugin, HDS stops owning audio device I/O and MIDI device I/O entirely -- the host
// supplies both via CLAP's audio-ports/note-ports extensions (see the multi-out section below for
// audio-ports specifically) -- but keeps its own control surface unchanged: this file starts its
// own embedded OSC server (default ports 9010/9011, distinct from the standalone app's 9000/9001)
// so frontend.py can drive it exactly as it drives the standalone app, just pointed at a different
// port. The ~1150 knobs stay 100% OSC-driven; a CURATED subset of ~60 (see HDS_CLAP_PARAMS) is
// ALSO exposed via a real `params` extension for host automation -- see that table's own comment
// for why exhaustive (all ~1150) was deliberately not attempted. `state` (DAW project save/load)
// is implemented too -- see its own section below. This project targets Linux only (see
// shell.nix's own header comment), so there is no Windows build of this plugin, now or planned.

#include <stdarg.h>
#include "../third_party/clap/include/clap/clap.h"

#define main backend_main_unused
#include "backend.c"
#undef main

// ---------------------------------------------------------------------------------------------
// build_osc_dispatch_table() builds a process-wide static table and calls exit(1) on a duplicate
// insert -- calling it more than once per process would hit that path. It only needs to run once
// ever, regardless of how many plugin instances get created/destroyed in this process (the table
// maps OSC address strings to struct-member byte offsets, which is identical for every instance,
// since SynthSharedControls's own layout doesn't vary per instance), so it's built once here
// rather than per-instance.
// ---------------------------------------------------------------------------------------------
static bool g_osc_dispatch_table_ready = false;

// Default embedded OSC ports -- distinct from the standalone app's 9000/9001 (see this file's own
// header comment). A plugin has no argv (hosts don't pass command-line flags), so these are the
// starting point for the sequential bind-retry every instance performs at activate() time (see
// clap_plugin_activate()'s own comment) -- instance 0 gets 9010/9011 exactly as before this pass,
// a second concurrent instance lands on 9012/9013, and so on, rather than colliding.
#define CLAP_OSC_LISTEN_PORT_BASE 9010
#define CLAP_OSC_REPLY_HOST "127.0.0.1"
#define CLAP_OSC_REPLY_PORT_BASE 9011
#define CLAP_OSC_PORT_RETRY_COUNT 32 // how many (listen, reply) port pairs to try before giving up

// ---------------------------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------------------------
// Multi-instance refactor: every piece of process-wide state the engine's call graph reaches
// (the sample-streaming tiers, the Sequencer/transport tier) is now a field of this struct instead
// of a file-scope global -- see HdsSampleSlotState/HdsStreamsState/HdsPendingLoadState/
// HdsEngineState's own comments in backend.c for why each is its own separately-declared tier.
// sample_engine is the small bundle of pointers into the three sample-streaming tiers that the
// engine's own functions take as a single parameter (see HdsSampleEngine's comment) -- it's set up
// once in clap_factory_create_plugin() right after this struct is allocated, since its pointer
// members depend on this instance's own address.
// ---------------------------------------------------------------------------------------------

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;

    SynthSharedControls knobs;
    SynthVoice voices[MAX_VOICES];
    MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1];
    float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    MidiLiveState midi_live;

    HdsSampleSlotState sample_slot_state;
    HdsStreamsState streams_state;
    HdsPendingLoadState pending_load_state;
    HdsEngineState engine;
    HdsSampleEngine sample_engine;

    // CLAP multi-out: one extra stereo audio-port per Combi Program (see clap_audio_ports_get()),
    // alongside the existing main stereo mix. A per-instance field, not a process()-local stack
    // array (BUFFER_SIZE * COMBI_PROGRAM_COUNT * 2 floats = 64KB) -- a real-time audio callback's
    // own stack is a scarcer, less predictable resource than this struct's already-heap/static-
    // lifetime allocation, and this buffer needs no more per-call isolation than chunk_buffer's
    // own comment already requires (per-instance is enough; concurrent instances already don't
    // share this struct).
    float per_program_render_buf[BUFFER_SIZE * COMBI_PROGRAM_COUNT * 2];

    // CLAP params extension (see HDS_CLAP_PARAMS/hds_clap_push_changed_params further down) --
    // one shadow value per exposed param, used purely to detect an OSC/GUI-driven change so it
    // can be echoed to the host as an output event. Sized/initialized once the table itself is
    // known to exist (clap_factory_create_plugin sets every entry to that param's default_value).
    double clap_param_shadow[64]; // >= HDS_CLAP_PARAM_COUNT; a plain fixed size since the table is a compile-time constant and this is just a shadow cache, not wire-visible

    int vu_decimator;

    lo_server osc_server;
    lo_address osc_reply_addr;
    bool osc_ready; // true once activate() successfully opened the OSC server this session
    int osc_listen_port, osc_reply_port; // the actual bound pair, once osc_ready (see the bind-retry in activate())

    // "/hds/request_state" support -- see HdsStateRequestContext's own comment in backend.c. Lets
    // a frontend that just connected to THIS instance's OSC port pull its current live state
    // instead of blindly pushing GUI defaults over a DAW-restored patch.
    HdsStateRequestContext state_request_ctx;
    // "/hds/song/import_smf" support -- see HdsSmfImportContext's own comment in backend.c. Same
    // "needs this instance's own osc_reply_addr" reason as state_request_ctx just above.
    HdsSmfImportContext smf_import_ctx;
    // "/hds/song/import_smf_track" support (per-lane "Load MIDI...") -- same reason.
    HdsSmfTrackImportContext smf_import_track_ctx;
} HdsClapPlugin;

// ---------------------------------------------------------------------------------------------
// audio-ports extension: 0 inputs (external audio input is a standalone-only feature for now --
// matches the existing no-capture-device code path render_audio_block already exercises when
// ext_in_channels=0/a NULL buffer). Outputs: port 0 is the main stereo mix (today's only output,
// unchanged -- IS_MAIN, so a host that doesn't care about multi-out just uses this one and never
// notices the rest exist), plus one more stereo port per Combi Program (ports 1-8) carrying that
// Program's own fully-processed (post-EQ, post-FX) signal -- the same multi-source richness the
// standalone app already exposes via its own Routing tab's hardware-output-pair grid, made
// reachable to a DAW host as separate tracks/busses instead. A host that connects only port 0
// hears the same identical mix as before this existed; per_program_render_buf (see this struct's
// own field comment) is only ever populated for a port the host actually connected.
// ---------------------------------------------------------------------------------------------
#define CLAP_OUTPUT_PORT_COUNT (1 + COMBI_PROGRAM_COUNT)

static uint32_t clap_audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? 0 : (uint32_t)CLAP_OUTPUT_PORT_COUNT;
}

static bool clap_audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void)plugin;
    if (is_input || index >= (uint32_t)CLAP_OUTPUT_PORT_COUNT) return false;
    info->id = index;
    if (index == 0) {
        snprintf(info->name, sizeof(info->name), "%s", "Main Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    } else {
        snprintf(info->name, sizeof(info->name), "Program %u", (unsigned int)index);
        info->flags = 0;
    }
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
// params extension: host automation for a CURATED set of the knobs a player would actually want
// to automate/ride live, not all ~1150 -- see this table's own field comments. Deliberately
// scoped rather than exhaustive: backend.c's OSC_PARAMS[] (the single source of truth for every
// knob's address/offset -- see its own comment) carries NO min/max/default metadata at all, since
// nothing needed it before this -- every real range today lives only in frontend.py's widget
// construction (e.g. PotentiometerKnob.setRange(...)). Exposing all ~1150 as CLAP params would
// mean hand-authoring ~1150 rows of range metadata from scratch (a huge, error-prone undertaking
// with no way to keep it in sync with frontend.py automatically), and would hand the host's own
// automation-lane picker an unusably long flat list -- the same reason most commercial synth
// plugins expose a curated "macro" parameter page rather than every internal register. This list
// (~60 params, each range hand-verified against frontend.py's own widget construction) covers:
// all 5 filters' Cutoff/Resonance, the post-filter 3-band EQ, each oscillator's Volume/Pan, each
// oscillator's own Amp EG (Attack/Decay/Sustain/Release), all 12 FX slots' Mix, Master Volume, and
// Transpose. Every entry addresses SINGLE_PROGRAM_SLOT (or a session-level field) exactly like an
// unprefixed "/hds/..." OSC address does -- Combi lanes and the Sequencer aren't exposed as CLAP
// params in this pass (a plugin instance is used as one voice/patch from a host's perspective; see
// this file's own header comment on the Combi/Sequencer machinery still being fully OSC-reachable
// regardless).
//
// THREAD SAFETY: get_value()/get_info() run on the host's main thread while process() (which
// writes these same SynthSharedControls fields, from OSC and from incoming param-value events
// alike) runs on the audio thread -- a plain, unsynchronized float read/write race, same as every
// other field in this ~1150-field struct already has with respect to OSC (this plugin drains its
// OSC socket from inside process() itself, so that side was never actually concurrent -- CLAP's
// own main-thread parameter calls are the first genuinely concurrent access here). Consistent
// with this struct's existing floats-everywhere design (nothing in it is atomic), and a torn read
// of a scalar float degrades to "the host's UI briefly shows a slightly stale value," never a
// crash -- judged an acceptable, explicitly-noted simplification rather than introducing a second
// synchronization discipline for just these 60 fields.
typedef struct {
    clap_id id;              // stable identifier -- hand-assigned below, never derived from array position (see this table's own comment on why)
    const char *name;        // host-facing display name
    size_t offset;           // offsetof(SynthSharedControls, ...) -- same offset-into-the-struct idea OSC_PARAMS itself uses
    double min_value, max_value, default_value;
    const char *unit;        // suffix for value_to_text(), e.g. " Hz" -- "" for a bare number
} HdsClapParamInfo;

#define HDS_CLAP_PPARAM(pid, nm, field, mn, mx, dflt, u) \
    { (pid), (nm), offsetof(SynthSharedControls, programs[SINGLE_PROGRAM_SLOT].field), (mn), (mx), (dflt), (u) }
#define HDS_CLAP_TPARAM(pid, nm, field, mn, mx, dflt, u) \
    { (pid), (nm), offsetof(SynthSharedControls, field), (mn), (mx), (dflt), (u) }

static const HdsClapParamInfo HDS_CLAP_PARAMS[] = {
    HDS_CLAP_PPARAM(0,  "Filter 1 Cutoff",    filters[0].cutoff, 40.0, 18000.0, 800.0,  " Hz"),
    HDS_CLAP_PPARAM(1,  "Filter 2 Cutoff",    filters[1].cutoff, 40.0, 18000.0, 2000.0, " Hz"),
    HDS_CLAP_PPARAM(2,  "Filter 3 Cutoff",    filters[2].cutoff, 40.0, 18000.0, 2000.0, " Hz"),
    HDS_CLAP_PPARAM(3,  "Filter 4 Cutoff",    filters[3].cutoff, 40.0, 18000.0, 2000.0, " Hz"),
    HDS_CLAP_PPARAM(4,  "Filter 5 Cutoff",    filters[4].cutoff, 40.0, 18000.0, 2000.0, " Hz"),
    HDS_CLAP_PPARAM(5,  "Filter 1 Resonance", filters[0].res_q,  0.0, 100.0, 20.0, ""),
    HDS_CLAP_PPARAM(6,  "Filter 2 Resonance", filters[1].res_q,  0.0, 100.0, 20.0, ""),
    HDS_CLAP_PPARAM(7,  "Filter 3 Resonance", filters[2].res_q,  0.0, 100.0, 20.0, ""),
    HDS_CLAP_PPARAM(8,  "Filter 4 Resonance", filters[3].res_q,  0.0, 100.0, 20.0, ""),
    HDS_CLAP_PPARAM(9,  "Filter 5 Resonance", filters[4].res_q,  0.0, 100.0, 20.0, ""),
    HDS_CLAP_PPARAM(10, "EQ Low Freq",  eq_bands[0].freq,    20.0, 20000.0, 100.0,  " Hz"),
    HDS_CLAP_PPARAM(11, "EQ Low Gain",  eq_bands[0].gain_db, -18.0, 18.0, 0.0, " dB"),
    HDS_CLAP_PPARAM(12, "EQ Mid Freq",  eq_bands[1].freq,    20.0, 20000.0, 1000.0, " Hz"),
    HDS_CLAP_PPARAM(13, "EQ Mid Gain",  eq_bands[1].gain_db, -18.0, 18.0, 0.0, " dB"),
    HDS_CLAP_PPARAM(14, "EQ Mid Q",     eq_bands[1].q,       0.1, 10.0, 0.707, ""),
    HDS_CLAP_PPARAM(15, "EQ High Freq", eq_bands[2].freq,    20.0, 20000.0, 5000.0, " Hz"),
    HDS_CLAP_PPARAM(16, "EQ High Gain", eq_bands[2].gain_db, -18.0, 18.0, 0.0, " dB"),
    HDS_CLAP_PPARAM(17, "Osc 1 Volume", vco1_vol, 0.0, 1.0, 0.8, ""),
    HDS_CLAP_PPARAM(18, "Osc 2 Volume", vco2_vol, 0.0, 1.0, 0.5, ""),
    HDS_CLAP_PPARAM(19, "Osc 3 Volume", vco3_vol, 0.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(20, "Osc 4 Volume", vco4_vol, 0.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(21, "Osc 1 Pan", vco1_pan, -1.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(22, "Osc 2 Pan", vco2_pan, -1.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(23, "Osc 3 Pan", vco3_pan, -1.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(24, "Osc 4 Pan", vco4_pan, -1.0, 1.0, 0.0, ""),
    HDS_CLAP_PPARAM(25, "Osc 1 Amp Attack",  eg[0].a,   1.0, 1000.0, 10.0,  " ms"),
    HDS_CLAP_PPARAM(26, "Osc 1 Amp Decay",   eg[0].dec, 1.0, 2000.0, 150.0, " ms"),
    HDS_CLAP_PPARAM(27, "Osc 1 Amp Sustain", eg[0].s,   0.0, 100.0, 80.0,  "%"),
    HDS_CLAP_PPARAM(28, "Osc 1 Amp Release", eg[0].r,   1.0, 2000.0, 300.0, " ms"),
    HDS_CLAP_PPARAM(29, "Osc 2 Amp Attack",  eg[1].a,   1.0, 1000.0, 10.0,  " ms"),
    HDS_CLAP_PPARAM(30, "Osc 2 Amp Decay",   eg[1].dec, 1.0, 2000.0, 150.0, " ms"),
    HDS_CLAP_PPARAM(31, "Osc 2 Amp Sustain", eg[1].s,   0.0, 100.0, 80.0,  "%"),
    HDS_CLAP_PPARAM(32, "Osc 2 Amp Release", eg[1].r,   1.0, 2000.0, 300.0, " ms"),
    HDS_CLAP_PPARAM(33, "Osc 3 Amp Attack",  eg[2].a,   1.0, 1000.0, 10.0,  " ms"),
    HDS_CLAP_PPARAM(34, "Osc 3 Amp Decay",   eg[2].dec, 1.0, 2000.0, 150.0, " ms"),
    HDS_CLAP_PPARAM(35, "Osc 3 Amp Sustain", eg[2].s,   0.0, 100.0, 80.0,  "%"),
    HDS_CLAP_PPARAM(36, "Osc 3 Amp Release", eg[2].r,   1.0, 2000.0, 300.0, " ms"),
    HDS_CLAP_PPARAM(37, "Osc 4 Amp Attack",  eg[3].a,   1.0, 1000.0, 10.0,  " ms"),
    HDS_CLAP_PPARAM(38, "Osc 4 Amp Decay",   eg[3].dec, 1.0, 2000.0, 150.0, " ms"),
    HDS_CLAP_PPARAM(39, "Osc 4 Amp Sustain", eg[3].s,   0.0, 100.0, 80.0,  "%"),
    HDS_CLAP_PPARAM(40, "Osc 4 Amp Release", eg[3].r,   1.0, 2000.0, 300.0, " ms"),
    HDS_CLAP_TPARAM(41, "FX 1 Mix",  fx[0].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(42, "FX 2 Mix",  fx[1].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(43, "FX 3 Mix",  fx[2].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(44, "FX 4 Mix",  fx[3].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(45, "FX 5 Mix",  fx[4].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(46, "FX 6 Mix",  fx[5].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(47, "FX 7 Mix",  fx[6].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(48, "FX 8 Mix",  fx[7].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(49, "FX 9 Mix",  fx[8].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(50, "FX 10 Mix", fx[9].mix,  0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(51, "FX 11 Mix", fx[10].mix, 0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(52, "FX 12 Mix", fx[11].mix, 0.0, 100.0, 100.0, "%"),
    HDS_CLAP_TPARAM(53, "Master Volume", master_volume, 0.0, 1.0, 0.5, ""),
    HDS_CLAP_TPARAM(54, "Transpose", transpose_semitones, -16.0, 16.0, 0.0, " st"),
};
#define HDS_CLAP_PARAM_COUNT ((uint32_t)(sizeof(HDS_CLAP_PARAMS) / sizeof(HDS_CLAP_PARAMS[0])))
// HdsClapPlugin.clap_param_shadow is a fixed size (64), declared long before this table exists in
// the file -- this catches a future table growth past that size at compile time instead of a
// silent out-of-bounds write.
_Static_assert(HDS_CLAP_PARAM_COUNT <= 64, "HDS_CLAP_PARAMS grew past HdsClapPlugin.clap_param_shadow's fixed size -- grow that array too");

static const HdsClapParamInfo *hds_clap_param_by_id(clap_id id) {
    for (uint32_t i = 0; i < HDS_CLAP_PARAM_COUNT; i++) {
        if (HDS_CLAP_PARAMS[i].id == id) return &HDS_CLAP_PARAMS[i];
    }
    return NULL;
}

static double hds_clap_param_read(const HdsClapPlugin *p, const HdsClapParamInfo *info) {
    return (double)(*(const float *)((const char *)&p->knobs + info->offset));
}
static void hds_clap_param_write(HdsClapPlugin *p, const HdsClapParamInfo *info, double value) {
    if (value < info->min_value) value = info->min_value;
    if (value > info->max_value) value = info->max_value;
    *(float *)((char *)&p->knobs + info->offset) = (float)value;
}

static uint32_t clap_params_count(const clap_plugin_t *plugin) {
    (void)plugin;
    return HDS_CLAP_PARAM_COUNT;
}

static bool clap_params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void)plugin;
    if (index >= HDS_CLAP_PARAM_COUNT) return false;
    const HdsClapParamInfo *src = &HDS_CLAP_PARAMS[index];
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->min_value = src->min_value;
    info->max_value = src->max_value;
    info->default_value = src->default_value;
    return true;
}

static bool clap_params_get_value(const clap_plugin_t *plugin, clap_id param_id, double *out_value) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    const HdsClapParamInfo *info = hds_clap_param_by_id(param_id);
    if (!info) return false;
    *out_value = hds_clap_param_read(p, info);
    return true;
}

static bool clap_params_value_to_text(const clap_plugin_t *plugin, clap_id param_id, double value, char *out_buffer, uint32_t out_buffer_capacity) {
    (void)plugin;
    const HdsClapParamInfo *info = hds_clap_param_by_id(param_id);
    if (!info) return false;
    snprintf(out_buffer, out_buffer_capacity, "%.2f%s", value, info->unit);
    return true;
}

static bool clap_params_text_to_value(const clap_plugin_t *plugin, clap_id param_id, const char *param_value_text, double *out_value) {
    (void)plugin;
    const HdsClapParamInfo *info = hds_clap_param_by_id(param_id);
    if (!info) return false;
    char *end = NULL;
    double v = strtod(param_value_text, &end);
    if (end == param_value_text) return false; // no number found at all
    *out_value = v;
    return true;
}

// Shared by process()'s own in_events loop and flush() below -- applies one host->plugin
// parameter change and immediately mirrors it into the shadow table so the diff-and-echo pass
// (hds_clap_push_changed_params) doesn't needlessly bounce the exact same value straight back to
// the host as if it had originated from the GUI/OSC side instead.
static void hds_clap_apply_param_event(HdsClapPlugin *p, const clap_event_param_value_t *ev) {
    const HdsClapParamInfo *info = hds_clap_param_by_id(ev->param_id);
    if (!info) return;
    hds_clap_param_write(p, info, ev->value);
    // Mirror into the shadow immediately -- see hds_clap_push_changed_params()'s own comment for
    // why (without this, the very next diff pass would wrongly treat the host's own automation as
    // an OSC/GUI-driven change and echo it straight back).
    size_t idx = (size_t)(info - HDS_CLAP_PARAMS);
    p->clap_param_shadow[idx] = hds_clap_param_read(p, info);
}

// Bi-directional sync's other half: a value changed some OTHER way (the frontend's own GUI, over
// OSC -- the two run concurrently, see this file's own header comment) needs to reach the host's
// automation lane/UI too, not just the reverse. Diffs every exposed param's live value against
// clap_param_shadow (updated here AND by hds_clap_apply_param_event above) and emits a
// CLAP_EVENT_PARAM_VALUE output event for whatever actually changed. Cheap enough (60 float
// comparisons) to just run once per process()/flush() call rather than tracking dirty bits.
static void hds_clap_push_changed_params(HdsClapPlugin *p, const clap_output_events_t *out_events) {
    if (!out_events) return;
    for (uint32_t i = 0; i < HDS_CLAP_PARAM_COUNT; i++) {
        const HdsClapParamInfo *info = &HDS_CLAP_PARAMS[i];
        double current = hds_clap_param_read(p, info);
        if (current == p->clap_param_shadow[i]) continue;
        p->clap_param_shadow[i] = current;
        clap_event_param_value_t ev = {0};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = info->id;
        ev.cookie = NULL;
        ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
        ev.value = current;
        out_events->try_push(out_events, &ev.header);
    }
}

static void clap_params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    if (in) {
        uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; i++) {
            const clap_event_header_t *hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE) {
                hds_clap_apply_param_event(p, (const clap_event_param_value_t *)hdr);
            }
        }
    }
    hds_clap_push_changed_params(p, out);
}

static const clap_plugin_params_t k_params_ext = {
    .count = clap_params_count,
    .get_info = clap_params_get_info,
    .get_value = clap_params_get_value,
    .value_to_text = clap_params_value_to_text,
    .text_to_value = clap_params_text_to_value,
    .flush = clap_params_flush,
};

// ---------------------------------------------------------------------------------------------
// state extension: real save/load. Round-trips every knob in `p->knobs` PLUS the Sample Bank
// (p->sample_slot_state) so a DAW project reopens with the patch -- oscillators, filters, EGs, LFOs,
// FX, Combi/Sequencer state, WAVE_SAMPLE zone mappings -- intact, not the default patch.
//
// Format is text, one line per record, addressed by NAME (the same OSC address strings
// OSC_PARAMS[]/osc_dispatch_table already use) rather than a raw positional struct dump. This is
// a deliberate reuse of this project's own hard-won lesson: the backend used to speak a raw
// positional wire protocol (a straight memcpy onto SynthSharedControls) and that was replaced
// with OSC specifically because a struct-layout change silently corrupted the wire contract with
// no compiler error -- see this repo's own "Replace the control-socket protocols with OSC" pass.
// A name-addressed save is immune to the same failure mode: a field the CURRENT binary doesn't
// recognize (e.g. state saved by a newer/older build) is just skipped, and a field the saved
// file doesn't mention simply keeps its already-applied default -- forward AND backward
// compatible by construction, no migration scheme needed.
//
//   HDS1                                 -- version line, first
//   K <address> <float>                  -- one per concrete OSC_KIND_FLOAT address
//   T <address> <string>                 -- one per concrete OSC_KIND_STRING address
//   Z <zone_id> <18 ints/floats> <path>  -- one per loaded Sample Bank zone, ZoneRecordWire's
//                                           own field order (see apply_zone_upsert)
// ---------------------------------------------------------------------------------------------

#define HDS_CLAP_STATE_VERSION 1

// Small growable buffer for building the save payload in memory before writing it out (the CLAP
// stream API only guarantees partial writes per call, not "write this whole blob atomically") --
// and for reading the whole load payload in before parsing it line-by-line (simpler and more
// robust than trying to reassemble lines split across arbitrary stream->read() chunk boundaries).
// Total size is bounded and small in practice (~4100 knob lines + a few hundred zone lines at
// most, well under 1MB) -- not a concern to hold in memory whole.
typedef struct { char *buf; size_t len; size_t cap; } HdsStateBuf;

static void state_buf_reserve(HdsStateBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t new_cap = b->cap ? b->cap * 2 : 4096;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    b->buf = (char *)realloc(b->buf, new_cap);
    b->cap = new_cap;
}

static void state_buf_appendf(HdsStateBuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return; }
    state_buf_reserve(b, (size_t)needed);
    vsnprintf(b->buf + b->len, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)needed;
}

static bool clap_state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsStateBuf b = {0};
    state_buf_appendf(&b, "HDS%d\n", HDS_CLAP_STATE_VERSION);

    // Every concrete knob address, straight from the already-built dispatch table (built once at
    // plugin.init(), guaranteed populated -- see g_osc_dispatch_table_ready) -- avoids
    // re-deriving the same OSC_PARAMS[] index-expansion logic build_osc_dispatch_table() already
    // owns, and guarantees this walks the EXACT same address space generic_osc_handler() does.
    for (int i = 0; i < OSC_DISPATCH_TABLE_SIZE; i++) {
        const OscDispatchSlot *slot = &osc_dispatch_table[i];
        if (!slot->occupied) continue;
        if (slot->kind == OSC_KIND_FLOAT) {
            float v = *(const float *)((const char *)&p->knobs + slot->offset);
            state_buf_appendf(&b, "K %s %.9g\n", slot->address, (double)v);
        } else {
            const char *s = (const char *)&p->knobs + slot->offset;
            state_buf_appendf(&b, "T %s %s\n", slot->address, s);
        }
    }

    // Sample Bank: every active, loaded zone, mirroring ZoneRecordWire's own field order/units
    // exactly. loop_start/loop_end/playback_end_frame are stored on SampleSlot as frame indices
    // (only meaningful once total_frames is known) -- converted back to the 0-1 fractions the
    // wire format uses, the direct inverse of apply_loop_points()/apply_playback_end().
    for (int s = 0; s < p->sample_slot_state.sample_slot_count; s++) {
        SampleSlot *slot = &p->sample_slot_state.sample_slots[s];
        if (!slot->active || !slot->loaded) continue;
        float loop_start_frac = (slot->total_frames > 1) ? (float)slot->loop_start / (float)(slot->total_frames - 1) : 0.0f;
        float loop_end_frac   = (slot->total_frames > 1) ? (float)slot->loop_end   / (float)(slot->total_frames - 1) : 0.0f;
        float end_frac = (slot->total_frames > 0) ? (float)slot->playback_end_frame / (float)slot->total_frames : 1.0f;
        state_buf_appendf(&b, "Z %d %d %d %d %d %d %d %.9g %.9g %d %d %.9g %.9g %.9g %.9g %d %d %.9g %d %s\n",
            slot->zone_id, slot->root_note, slot->min_note, slot->max_note, slot->min_velocity, slot->max_velocity,
            slot->unpitched ? 1 : 0, (double)loop_start_frac, (double)loop_end_frac, slot->loop_enabled ? 1 : 0,
            slot->round_robin_group, (double)slot->tune_cents, (double)slot->gain_db, (double)slot->pan,
            (double)slot->start_offset_frac, slot->reverse ? 1 : 0, slot->loop_release_exit ? 1 : 0, (double)end_frac,
            slot->assigned_osc, slot->file_path);
    }

    bool ok = true;
    size_t written = 0;
    while (written < b.len) {
        int64_t n = stream->write(stream, b.buf + written, b.len - written);
        if (n <= 0) { ok = false; break; }
        written += (size_t)n;
    }
    free(b.buf);
    return ok;
}

static bool clap_state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;

    HdsStateBuf b = {0};
    char chunk[4096];
    for (;;) {
        int64_t n = stream->read(stream, chunk, sizeof(chunk));
        if (n < 0) { free(b.buf); return false; }
        if (n == 0) break;
        state_buf_reserve(&b, (size_t)n);
        memcpy(b.buf + b.len, chunk, (size_t)n);
        b.len += (size_t)n;
    }
    if (b.len == 0) { free(b.buf); return false; } // an empty state is invalid, not "nothing to do"
    state_buf_reserve(&b, 1); b.buf[b.len] = '\0';

    char *cursor = b.buf;
    char *first_newline = strchr(cursor, '\n');
    if (!first_newline) { free(b.buf); return false; }
    *first_newline = '\0';
    int version = 0;
    if (sscanf(cursor, "HDS%d", &version) != 1 || version < 1 || version > HDS_CLAP_STATE_VERSION) { free(b.buf); return false; }
    cursor = first_newline + 1;

    // Clear whatever's currently loaded before applying the saved zones -- loading a project
    // means "this project's own samples," not "add these on top of whatever was already there"
    // (same posture frontend.py's own Program-load-with-SFZ path already established).
    for (int s = 0; s < p->sample_slot_state.sample_slot_count; s++) {
        if (p->sample_slot_state.sample_slots[s].active) apply_zone_remove(&p->sample_engine, p->sample_slot_state.sample_slots[s].zone_id);
    }

    int applied_knobs = 0, skipped_knobs = 0, applied_zones = 0;
    while (*cursor) {
        char *line_end = strchr(cursor, '\n');
        if (line_end) *line_end = '\0';

        if (cursor[0] == 'K' && cursor[1] == ' ') {
            char addr[128]; double value;
            if (sscanf(cursor + 2, "%127s %lf", addr, &value) == 2) {
                const OscDispatchSlot *slot = osc_dispatch_lookup(addr);
                if (slot && slot->kind == OSC_KIND_FLOAT) { *(float *)((char *)&p->knobs + slot->offset) = (float)value; applied_knobs++; }
                else skipped_knobs++;
            }
        } else if (cursor[0] == 'T' && cursor[1] == ' ') {
            char addr[128]; int addr_chars = 0;
            if (sscanf(cursor + 2, "%127s%n", addr, &addr_chars) == 1) {
                const char *value = cursor + 2 + addr_chars;
                while (*value == ' ') value++;
                const OscDispatchSlot *slot = osc_dispatch_lookup(addr);
                if (slot && slot->kind == OSC_KIND_STRING) { snprintf((char *)&p->knobs + slot->offset, slot->str_len, "%s", value); applied_knobs++; }
                else skipped_knobs++;
            }
        } else if (cursor[0] == 'Z' && cursor[1] == ' ') {
            ZoneRecordWire rec;
            int unpitched_i, loop_enabled_i, reverse_i, loop_release_exit_i;
            int root_note, min_note, max_note, min_velocity, max_velocity, round_robin_group;
            int assigned_osc;
            double loop_start, loop_end, tune_cents, gain_db, pan, start_offset_frac, end_frac;
            char path[SAMPLE_PATH_LEN];
            int n = sscanf(cursor + 2, "%d %d %d %d %d %d %d %lf %lf %d %d %lf %lf %lf %lf %d %d %lf %d %[^\n]",
                &rec.zone_id, &root_note, &min_note, &max_note, &min_velocity, &max_velocity,
                &unpitched_i, &loop_start, &loop_end, &loop_enabled_i, &round_robin_group,
                &tune_cents, &gain_db, &pan, &start_offset_frac, &reverse_i, &loop_release_exit_i, &end_frac,
                &assigned_osc, path);
            if (n == 20) {
                rec.root_note = (float)root_note; rec.min_note = (float)min_note; rec.max_note = (float)max_note;
                rec.min_velocity = (float)min_velocity; rec.max_velocity = (float)max_velocity;
                rec.unpitched = unpitched_i ? 1.0f : 0.0f;
                rec.loop_start = (float)loop_start; rec.loop_end = (float)loop_end;
                rec.loop_enabled = loop_enabled_i ? 1.0f : 0.0f;
                rec.round_robin_group = (float)round_robin_group;
                rec.tune_cents = (float)tune_cents; rec.gain_db = (float)gain_db; rec.pan = (float)pan;
                rec.start_offset_frac = (float)start_offset_frac;
                rec.reverse = reverse_i ? 1.0f : 0.0f;
                rec.loop_release_exit = loop_release_exit_i ? 1.0f : 0.0f;
                rec.end_frac = (float)end_frac;
                rec.assigned_osc = (float)assigned_osc;
                snprintf(rec.path, sizeof(rec.path), "%s", path);
                apply_zone_upsert(&p->sample_engine, &rec); // brand-new zone -> queued via enqueue_pending_sample_load, picked up by this instance's own drain_pending_sample_loads() poll in process()
                applied_zones++;
            }
        }
        cursor = line_end ? line_end + 1 : cursor + strlen(cursor);
    }

    fprintf(stderr, "[hds-clap] state load: %d knob(s) applied, %d skipped (unrecognized address), %d zone(s) queued\n",
            applied_knobs, skipped_knobs, applied_zones);
    free(b.buf);
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

    // Embedded OSC server -- same non-threaded, drained-once-per-block-from-the-audio-thread
    // design main() uses (see main()'s own comment on why: only one thread ever touches its own
    // active_knobs/sample slots, no locking needed -- now true per-instance rather than
    // process-wide). Bind failure is logged but not fatal to activation: the synth can still make
    // sound (default patch, MIDI-driven) with no external control surface, which is strictly
    // better than refusing to activate at all.
    // build_osc_dispatch_table() is NOT called here: it's a call-once-ever-per-process function
    // (a second call hits its own internal "duplicate address" fatal check and calls exit(1) --
    // fatal inside a plugin loaded into a host, unlike the standalone app where it's genuinely
    // fatal-and-correct at startup). plugin.init() already guarantees it ran once before any
    // activate() call can happen, per the CLAP spec's own init-before-activate ordering. The table
    // itself only maps OSC address strings to struct-member byte OFFSETS (not instance-specific
    // addresses), so sharing it across instances is correct -- generic_osc_handler resolves an
    // offset against whichever &p->knobs it was registered with below.
    //
    // Multi-instance OSC port allocation: sequential bind-retry starting at
    // CLAP_OSC_LISTEN_PORT_BASE/CLAP_OSC_REPLY_PORT_BASE (9010/9011) -- the single-instance guard
    // this file used to have is gone, so a second concurrent instance must land on a genuinely
    // different port pair rather than silently losing its control surface to the first instance
    // that grabbed 9010. Each candidate pair is tried together (listen bound only if paired with a
    // free reply port too) so a host's several tracks end up on predictable, sequential pairs
    // (9010/9011, 9012/9013, 9014/9015, ...) rather than mismatched listen/reply numbers.
    for (int i = 0; i < CLAP_OSC_PORT_RETRY_COUNT; i++) {
        int listen_port = CLAP_OSC_LISTEN_PORT_BASE + 2 * i;
        int reply_port = CLAP_OSC_REPLY_PORT_BASE + 2 * i;
        char listen_port_str[16]; snprintf(listen_port_str, sizeof(listen_port_str), "%d", listen_port);
        lo_server candidate = lo_server_new_with_proto(listen_port_str, LO_UDP, osc_error_handler);
        if (candidate) {
            p->osc_server = candidate;
            p->osc_listen_port = listen_port;
            p->osc_reply_port = reply_port;
            break;
        }
    }
    if (p->osc_server) {
        lo_server_add_method(p->osc_server, "/hds/zone/upsert", "i" "ffffffffffffffffff" "s", zone_upsert_handler, &p->sample_engine);
        lo_server_add_method(p->osc_server, "/hds/zone/remove", "i", zone_remove_handler, &p->sample_engine);
        lo_server_add_method(p->osc_server, "/hds/zone/load_sfz", "is", zone_load_sfz_handler, &p->sample_engine);
        char reply_port_str[16]; snprintf(reply_port_str, sizeof(reply_port_str), "%d", p->osc_reply_port);
        p->osc_reply_addr = lo_address_new(CLAP_OSC_REPLY_HOST, reply_port_str);
        // Registered here rather than in the block above -- needs p->osc_reply_addr, just resolved
        // on the previous line. state_request_ctx lives as long as this instance does, so the
        // pointer handed to liblo stays valid for the plugin's whole lifetime.
        p->state_request_ctx.knobs = &p->knobs;
        p->state_request_ctx.reply_addr = p->osc_reply_addr;
        lo_server_add_method(p->osc_server, "/hds/request_state", "", request_state_handler, &p->state_request_ctx);
        p->smf_import_ctx.engine = &p->engine;
        p->smf_import_ctx.knobs = &p->knobs;
        p->smf_import_ctx.reply_addr = p->osc_reply_addr;
        lo_server_add_method(p->osc_server, "/hds/song/import_smf", "sf", song_import_smf_handler, &p->smf_import_ctx);
        p->smf_import_track_ctx.engine = &p->engine;
        p->smf_import_track_ctx.reply_addr = p->osc_reply_addr;
        lo_server_add_method(p->osc_server, "/hds/song/import_smf_track", "sf", song_import_smf_track_handler, &p->smf_import_track_ctx);
        // Bug fix: this catch-all (NULL path, NULL types -- matches every address) used to be
        // registered FIRST, right after the three /hds/zone/* methods above. liblo tries a
        // message's matching methods in REGISTRATION ORDER and stops at the first one that
        // returns 0 ("fully handled") -- it does NOT prefer a more specific path match over a
        // less specific one registered earlier. So every message to /hds/request_state or either
        // /hds/song/import_smf* address matched THIS handler first, which (correctly, from its
        // own perspective) treats an address absent from osc_dispatch_table as "unrecognized,"
        // logs it, and returns 0 -- permanently preventing liblo from EVER trying the real,
        // specific handler for that address. /hds/request_state -- the entire "pull this
        // instance's live state on connect" mechanism a DAW-restored patch depends on -- was
        // silently, deterministically dead code from the moment it was added: not a race, not
        // host-dependent, never worked once, regardless of any frontend.py-side fix. Registering
        // this catch-all LAST (after every specific method above) fixes it: liblo now reaches the
        // specific handler for any of those exact addresses first, and only truly falls through
        // to this catch-all for the ~1150 plain knob addresses that have no specific method of
        // their own -- exactly its original intent.
        lo_server_add_method(p->osc_server, NULL, NULL, generic_osc_handler, &p->knobs);
        p->osc_ready = true;
        fprintf(stderr, "[hds-clap] OSC listening on UDP port %d, status replies to %s:%d\n", p->osc_listen_port, CLAP_OSC_REPLY_HOST, p->osc_reply_port);
        if (p->host && p->host->get_extension) {
            const clap_host_log_t *log = (const clap_host_log_t *)p->host->get_extension(p->host, CLAP_EXT_LOG);
            if (log && log->log) {
                char msg[128]; snprintf(msg, sizeof(msg), "HDS OSC control surface on UDP port %d (status replies to port %d)", p->osc_listen_port, p->osc_reply_port);
                log->log(p->host, CLAP_LOG_INFO, msg);
            }
        }
    } else {
        fprintf(stderr, "[hds-clap] failed to open an OSC listen socket in the %d..%d range -- continuing with no external control surface\n",
                CLAP_OSC_LISTEN_PORT_BASE, CLAP_OSC_LISTEN_PORT_BASE + 2 * (CLAP_OSC_PORT_RETRY_COUNT - 1));
        p->osc_ready = false;
    }

    // Sample-streaming disk thread (DFD/WAVE_SAMPLE) -- same "started once, talked to only via
    // atomics" pattern main() uses, now started against this instance's own HdsStreamsState
    // instead of a process-wide global. sample_disk_thread_shutdown may still be `true` from a
    // PREVIOUS activate()/deactivate() cycle of this same instance -- reset it before starting, or
    // the thread would see shutdown already requested and exit immediately. Safe: deactivate()
    // only sets it after pthread_join() confirms the old thread has fully exited.
    atomic_store_explicit(&p->streams_state.sample_disk_thread_shutdown, false, memory_order_release);
    bool disk_thread_ok = (pthread_create(&p->streams_state.sample_disk_thread, NULL, sample_disk_thread_main, &p->sample_engine) == 0);
    p->streams_state.sample_disk_thread_running = disk_thread_ok;
    if (!disk_thread_ok) {
        fprintf(stderr, "[hds-clap] failed to start sample disk streaming thread -- WAVE_SAMPLE playback unavailable this session\n");
    }

    p->vu_decimator = 0;
    return true;
}

static void clap_plugin_deactivate(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;

    if (p->streams_state.sample_disk_thread_running) {
        atomic_store_explicit(&p->streams_state.sample_disk_thread_shutdown, true, memory_order_release);
        pthread_join(p->streams_state.sample_disk_thread, NULL);
        atomic_store_explicit(&p->streams_state.sample_disk_thread_shutdown, false, memory_order_release); // see activate()'s own comment on why this must be reset for a future reactivation
        p->streams_state.sample_disk_thread_running = false;
    }

    // Join any still-in-flight Sequencer audio-lane loader thread before this instance's own
    // HdsStreamsState can be freed (destroy() calls free(p) right after a final deactivate()) --
    // the fix for the use-after-free hazard flagged when detach was replaced with tracking in
    // poll_seq_audio_load_requests() (see SeqAudioLoadRequest.owner's own comment): a still-
    // running detached thread writing into req->owner->seq_audio[] after this instance was freed
    // would corrupt whatever memory now occupies that address. Safe to join unconditionally here
    // even for a lane that finished long ago (pthread_join on an already-exited thread returns
    // immediately).
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        if (p->streams_state.seq_audio_loader_thread_active[l]) {
            pthread_join(p->streams_state.seq_audio_loader_thread[l], NULL);
            p->streams_state.seq_audio_loader_thread_active[l] = false;
        }
    }

    if (p->osc_ready) {
        if (p->osc_reply_addr) { lo_address_free(p->osc_reply_addr); p->osc_reply_addr = NULL; }
        lo_server_free(p->osc_server); p->osc_server = NULL;
        p->osc_ready = false;
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
    poll_seq_audio_load_requests(&p->streams_state, &p->knobs);
    poll_seq_audio_preview_triggers(&p->streams_state, &p->knobs);
    // Resolved against the Sequencer's own playhead position, not this instance's uptime -- see
    // backend.c main()'s own identical fix and comment (a song's tempo map is relative to song
    // position, not to how long this plugin instance has been running).
    float current_tempo_bpm = compute_current_tempo_bpm(&p->knobs, p->engine.transport_playhead_sec);
    // Host tempo sync (SynthSharedControls.seq_sync_source == 1, "External"): a CLAP host already
    // hands its own tempo directly via process->transport -- no raw MIDI Clock bytes to parse at
    // all here, unlike the standalone app's ALSA-sequencer path (see
    // hds_handle_midi_clock_tick()/SND_SEQ_EVENT_CLOCK in backend.c's process_midi_input, the
    // equivalent for a real external MIDI cable/hardware sequencer). Deliberately tempo-only: the
    // host's own timeline position doesn't necessarily correspond to "the start of HDS's own
    // recorded Sequencer take" the way a real MIDI Start message does, so the host's play/stop
    // state is NOT wired to the Sequencer's own transport here -- that stays driven by its own
    // Play/Pause/Stop buttons (or OSC), exactly as before.
    if (p->knobs.seq_sync_source != 0.0f && process->transport && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) {
        current_tempo_bpm = (float)process->transport->tempo;
    }
    poll_transport_triggers(&p->streams_state, &p->engine, &p->knobs, current_tempo_bpm);
    poll_metronome(&p->engine, &p->knobs, current_tempo_bpm);
    poll_seq_audio_transport_catchup(&p->streams_state, &p->engine, &p->knobs);
    drain_pending_sample_loads(&p->sample_engine);

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
                dispatch_midi_event(&p->engine, &p->sample_engine, hdr->type == CLAP_EVENT_NOTE_ON ? HDS_MIDI_NOTE_ON : HDS_MIDI_NOTE_OFF,
                                     (int)ev->channel + 1, (int)ev->key, velocity_127,
                                     p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, current_tempo_bpm);
            } else if (hdr->type == CLAP_EVENT_MIDI) {
                const clap_event_midi_t *ev = (const clap_event_midi_t *)hdr;
                unsigned char status = ev->data[0] & 0xF0;
                int midi_channel_1_16 = (ev->data[0] & 0x0F) + 1;
                // Same raw-byte decode convention backend.c's own ALSA reader already uses (see
                // process_midi_input) -- a Note On with velocity 0 counts as Note Off.
                if (status == 0x90 || status == 0x80) {
                    bool is_note_on = (status == 0x90 && ev->data[2] > 0);
                    dispatch_midi_event(&p->engine, &p->sample_engine, is_note_on ? HDS_MIDI_NOTE_ON : HDS_MIDI_NOTE_OFF, midi_channel_1_16, ev->data[1], ev->data[2],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, current_tempo_bpm);
                } else if (status == 0xB0) {
                    dispatch_midi_event(&p->engine, &p->sample_engine, HDS_MIDI_CC, midi_channel_1_16, ev->data[1], ev->data[2],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, current_tempo_bpm);
                } else if (status == 0xE0) {
                    int bend14 = (int)ev->data[1] | ((int)ev->data[2] << 7);
                    dispatch_midi_event(&p->engine, &p->sample_engine, HDS_MIDI_PITCH_BEND, midi_channel_1_16, 0, bend14 - 8192,
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, current_tempo_bpm);
                } else if (status == 0xD0) {
                    dispatch_midi_event(&p->engine, &p->sample_engine, HDS_MIDI_CHANNEL_PRESSURE, midi_channel_1_16, 0, ev->data[1],
                                         p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, current_tempo_bpm);
                }
            } else if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                hds_clap_apply_param_event(p, (const clap_event_param_value_t *)hdr);
            }
        }
    }

    seq_playback_midi_events(&p->engine, &p->sample_engine, p->voices, &p->knobs, p->global_lfo_phases, &p->mono_stacks[SINGLE_PROGRAM_SLOT], &p->midi_live);
    // Coarser than the standalone app's own per-block call (main() polls once per its own,
    // smaller render block; this fires once per this entire process() call, using the host's
    // full frames_count as one lump elapsed-sample count) -- acceptable given a host's own
    // process() call is typically already close to one audio-thread callback's worth of frames,
    // same granularity this plugin already accepts elsewhere (see the chunking loop below).
    poll_arpeggiator(&p->engine, &p->sample_engine, p->voices, &p->knobs, p->global_lfo_phases, p->mono_stacks, &p->midi_live, (int)process->frames_count);

    float **out = NULL;
    if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32 &&
        process->audio_outputs[0].channel_count >= 2 && process->audio_outputs[0].data32[0] && process->audio_outputs[0].data32[1]) {
        out = process->audio_outputs[0].data32;
        process->audio_outputs[0].constant_mask = 0;
    }
    // CLAP multi-out (see clap_audio_ports_get()): a per-Program port the host didn't connect
    // (common -- most hosts default to just the main port) simply stays NULL here and is never
    // written to, so render_audio_block's own per_program_out capture is the only cost paid
    // regardless of how many of these are actually wired up in the host.
    float *prog_out[COMBI_PROGRAM_COUNT][2] = {{0}};
    bool any_prog_out_connected = false;
    for (int prog = 0; prog < COMBI_PROGRAM_COUNT; prog++) {
        uint32_t port_idx = (uint32_t)(1 + prog);
        if (port_idx < process->audio_outputs_count && process->audio_outputs[port_idx].data32 &&
            process->audio_outputs[port_idx].channel_count >= 2 &&
            process->audio_outputs[port_idx].data32[0] && process->audio_outputs[port_idx].data32[1]) {
            prog_out[prog][0] = process->audio_outputs[port_idx].data32[0];
            prog_out[prog][1] = process->audio_outputs[port_idx].data32[1];
            process->audio_outputs[port_idx].constant_mask = 0;
            any_prog_out_connected = true;
        }
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
        advance_transport_playhead(&p->engine, block_size);

        // Not `static`: this buffer must be per-instance (and per-call), since two HdsClapPlugin
        // instances can now genuinely render concurrently -- a `static` buffer here would let one
        // instance's render clobber another's mid-flight, a real cross-instance data race this
        // multi-instance pass exists to eliminate, not reintroduce.
        float chunk_buffer[BUFFER_SIZE * 2];
        // No Routing tab exists for a CLAP plugin instance's own OSC control surface to report
        // visibility for (see gui_routing_tab_visible's own comment) -- ext_in_peak_scratch is
        // therefore always all-zero (gui_routing_tab_visible defaults to 0) and simply discarded.
        float ext_in_peak_scratch[EXT_IN_PAIR_COUNT];
        float peak = render_audio_block(&p->sample_engine, &p->engine, p->voices, &p->knobs, p->global_lfo_phases, NULL, 0, 2, &p->midi_live, chunk_buffer, current_tempo_bpm, ext_in_peak_scratch,
                                         any_prog_out_connected ? p->per_program_render_buf : NULL);
        if (peak > block_peak) block_peak = peak;

        if (out) {
            for (int i = 0; i < block_size; i++) {
                out[0][frames_done + (uint32_t)i] = chunk_buffer[i * 2 + 0];
                out[1][frames_done + (uint32_t)i] = chunk_buffer[i * 2 + 1];
            }
        }
        if (any_prog_out_connected) {
            for (int prog = 0; prog < COMBI_PROGRAM_COUNT; prog++) {
                if (!prog_out[prog][0]) continue; // this particular port wasn't connected -- skip it individually
                for (int i = 0; i < block_size; i++) {
                    prog_out[prog][0][frames_done + (uint32_t)i] = p->per_program_render_buf[(size_t)i * COMBI_PROGRAM_COUNT * 2 + (size_t)prog * 2 + 0];
                    prog_out[prog][1][frames_done + (uint32_t)i] = p->per_program_render_buf[(size_t)i * COMBI_PROGRAM_COUNT * 2 + (size_t)prog * 2 + 1];
                }
            }
        }

        frames_done += (uint32_t)block_size;
    }

    if (p->osc_ready) {
        p->vu_decimator++;
        if (p->vu_decimator >= 12) {
            EngineStatus status = {0};
            status.peak = block_peak;
            status.detected_output_channels = 2.0f;
            status.tempo_bpm_live = current_tempo_bpm;
            status.seq_elapsed_seconds = (float)p->engine.transport_playhead_sec;
            status.seq_transport_state = (float)p->engine.transport_state;
            send_osc_status(p->osc_reply_addr, &status);
            send_seq_audio_status(&p->streams_state, p->osc_reply_addr);
            p->vu_decimator = 0;
        }
    }

    // Bi-directional param sync's OSC/GUI->host half -- see hds_clap_push_changed_params()'s own
    // comment. Run every process() call (not just on the vu_decimator's slower cadence) so a
    // knob turned in the GUI reaches the host's automation lane/UI promptly, not up to ~12 blocks
    // late.
    hds_clap_push_changed_params(p, process->out_events);

    return CLAP_PROCESS_CONTINUE;
}

static const void *clap_plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &k_audio_ports_ext;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &k_note_ports_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &k_state_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &k_params_ext;
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
    // Seed the CLAP params shadow from the REAL live value init_default_controls() just set, not
    // a separately-hardcoded copy -- if the two ever drift, this avoids a spurious "changed"
    // event firing on this instance's very first process() call (see
    // hds_clap_push_changed_params()'s own comment).
    for (uint32_t i = 0; i < HDS_CLAP_PARAM_COUNT; i++) p->clap_param_shadow[i] = hds_clap_param_read(p, &HDS_CLAP_PARAMS[i]);
    for (int i = 0; i < MAX_VOICES; i++) init_voice(&p->voices[i]);
    for (int i = 0; i <= COMBI_PROGRAM_COUNT; i++) p->mono_stacks[i] = (MonoNoteStack){0};

    // Points this instance's HdsSampleEngine bundle at its OWN three sample-streaming tiers --
    // can't be a static initializer (the plan's original design for the process-wide
    // g_sample_engine) since these addresses depend on where THIS calloc landed.
    p->sample_engine.slots = &p->sample_slot_state;
    p->sample_engine.streams = &p->streams_state;
    p->sample_engine.pending = &p->pending_load_state;

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
