// HDS FX: a standalone, synth-free Linux CLAP effects plugin -- "the normal HDS FX section" (the
// same FX_TYPE_* DSP, the same multi-bus Series/Parallel routing + slot chaining + sidechain model
// that backend.c's own embedded FX bus already runs) as its own instrument-free plugin, for
// someone who wants HDS's effects on a DAW track without wanting a synth voice at all.
//
// Deliberately NOT built like clap_plugin.c: HDS FX will only ever be host-automated/UI-driven
// inside a normal DAW session, never controlled remotely or over a LAN, so there is no embedded
// OSC server, no liblo dependency, and no `backend.c` #include at all here -- this file only pulls
// in the two pieces it actually needs, src/fx_types.h (the FX slot data model: FXSlotParams/
// FXSlotEngine/FX_TYPE_*) and src/fx_chain.c (the actual run_fx_*/process_fx_chain DSP), each
// #include'd the same way backend.c itself includes them, so the exact same ported effects (HT-76,
// Fairchild 670, LA-2A, FDNVerb, ...) run byte-for-byte identically here as they do in the synth.
// A slot's param layout is the same FXSlotParams struct either way, so it's already
// interchangeable between the synth's embedded FX bus and this plugin.
//
// NAM (FX_TYPE_NAM) is deliberately disabled in this first pass (HDS_NAM_ENABLED 0 below, which
// compiles every NAM code path out of fx_chain.c entirely) -- it needs the small C++ bridge
// (nam_bridge.h/.cpp over the vendored NeuralAmpModelerCore) clap_plugin.c also links against, and
// pulling that in is a real, separate, scoped follow-up once the rest of this plugin is proven, not
// something to bundle into a from-scratch first version.
//
// HardTune's (FX_TYPE_HARDTUNE) live-MIDI vibrato control reads four small globals
// (g_hardtune_held_note/g_hardtune_bend_semitones/g_hardtune_modwheel/g_hardtune_channel_pressure)
// that backend.c's own MIDI dispatch normally feeds -- this plugin declares them too (see just
// below fx_chain.c needs them to compile) but never writes them, since this plugin has no note-port
// and never sees MIDI performance data. HardTune's pitch-correction itself works exactly as normal;
// only its OPTIONAL vibrato-from-CC/pitch-bend feature is inert here, same "harmless, not wired up
// yet" posture as NAM above -- a real MIDI input port (for CC/pitch-bend modulation of FX params
// generally, not just HardTune's vibrato) is a natural, separate future addition.
//
// Multi-instance safety comes for free: process_fx_chain()/sync_fx_engine_types()/reset_fx_engine()
// (see fx_chain.c) already take every piece of state as an explicit parameter and touch no globals
// of their own -- the same discipline clap_plugin.c's own HdsClapPlugin already relies on. Running
// several HDS FX instances in one DAW session is exactly as safe as running several HDS instrument
// instances already is today: each HdsFxPlugin below is its own independent calloc'd struct.
//
// Multi-input + chaining "as the normal HDS FX section": HDS_FX_BUS_COUNT (8) independent stereo
// input/output pairs, each its own SOURCE_COUNT-shaped bus -- the exact same fx_source/
// fx_source_routing_mode/chain_to_next model backend.c's own ext-in pairs already use, just with no
// "Program" bus (there is no synth here at all). See clap_audio_ports_get() below for the port
// layout and HdsFxPlugin's own comment for how a bus's FX_SLOT_COUNT slots are stored.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "../third_party/clap/include/clap/clap.h"

// The handful of plain constants fx_types.h/fx_chain.c need defined before they're #include'd --
// backend.c normally supplies these (see its own header comment on each); this file supplies its
// own copies instead of pulling in backend.c wholesale, since none of backend.c's synth machinery
// is wanted here at all.
#define SAMPLE_RATE 48000
#define BUFFER_SIZE 1024
#define SAMPLE_PATH_LEN 256
#define FX_SLOT_COUNT 12
#define HDS_FX_BUS_COUNT 8
// process_fx_chain()'s own sidechain model addresses a "source" by a plain 0..SOURCE_COUNT-1 index
// (see FXSlotParams.sidechain_source's own comment in fx_types.h) -- here that's just "which of
// this plugin's own 8 buses", with no separate synth "Program" source to account for.
#define SOURCE_COUNT HDS_FX_BUS_COUNT
#define HDS_NAM_ENABLED 0

// fx_chain.c's own g_hardtune_held_note/g_hardtune_bend_semitones/g_hardtune_modwheel/
// g_hardtune_channel_pressure globals (declared there, not here -- see this file's own header
// comment) are never written to by this plugin (no note-port, no MIDI performance data reaches
// it), so HardTune's own vibrato-from-CC/pitch-bend feature simply stays inert at its defaults;
// HardTune's actual pitch correction is unaffected.
#include "fx_types.h"
#include "fx_chain.c"

// ---------------------------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------------------------
// fx[bus]/fx_engine[bus]: HDS_FX_BUS_COUNT independent FX_SLOT_COUNT-slot chains, one per audio
// bus -- physically identical shape to backend.c's own SynthSharedControls.fx[FX_SLOT_COUNT]/
// FXSlotEngine fx_engine[FX_SLOT_COUNT], just duplicated per bus instead of shared by every
// source. fx[bus][s].fx_source is always == bus (see clap_factory_create_plugin's own
// initialization) -- a slot never crosses buses, unlike the synth's own model where any slot can
// be assigned to any source; that generality bought nothing here since there's no UI reason to let
// a slot on Input 3 process Input 5's signal instead; the field stays (process_fx_chain needs it),
// it's just pinned per-bus by construction.
typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;

    FXSlotParams fx[HDS_FX_BUS_COUNT][FX_SLOT_COUNT];
    FXSlotEngine fx_engine[HDS_FX_BUS_COUNT][FX_SLOT_COUNT];
    float bus_routing_mode[HDS_FX_BUS_COUNT]; // 0 = Series, 1 = Parallel -- see process_fx_chain()'s own routing_mode parameter
    float gr_db_scratch[HDS_FX_BUS_COUNT][FX_SLOT_COUNT]; // required non-NULL out-param, not yet exposed to the host/GUI

    // CLAP params extension shadow (see hds_fx_push_changed_params's own comment, same pattern
    // clap_plugin.c already uses for HDS_CLAP_PARAMS) -- one entry per exposed param.
    double param_shadow[HDS_FX_BUS_COUNT * FX_SLOT_COUNT];

    bool gui_open;
} HdsFxPlugin;

// ---------------------------------------------------------------------------------------------
// audio-ports extension: HDS_FX_BUS_COUNT stereo inputs, HDS_FX_BUS_COUNT stereo outputs, named
// to match each other 1:1 ("Input N" -> that same bus's own "Output N") -- a host inserting this on
// a single track just uses pair 0 (IS_MAIN on both sides) and never notices the other 7 exist; a
// host that wants HDS FX processing several buses at once (e.g. several tracks' sends summed into
// one plugin instance rather than one instance per track) wires up as many of the rest as it wants.
// An unconnected input bus is silence in (see process()'s own per-bus null check) -- harmless,
// exactly the ext-in pattern backend.c's own audio-ports extension already established.
// ---------------------------------------------------------------------------------------------

static uint32_t hds_fx_audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin; (void)is_input;
    return (uint32_t)HDS_FX_BUS_COUNT;
}

static bool hds_fx_audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void)plugin;
    if (index >= (uint32_t)HDS_FX_BUS_COUNT) return false;
    info->id = index;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = index; // this bus's input can safely alias its own output buffer -- process() never reads input[bus] after writing output[bus]
    snprintf(info->name, sizeof(info->name), "%s %u", is_input ? "Input" : "Output", (unsigned int)(index + 1));
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    return true;
}

static const clap_plugin_audio_ports_t k_audio_ports_ext = {
    .count = hds_fx_audio_ports_count,
    .get = hds_fx_audio_ports_get,
};

// ---------------------------------------------------------------------------------------------
// params extension: per-bus, per-slot Mix -- same curated "expose Mix only, drive everything else
// (Type, Bypass, routing) from the plugin's own GUI" choice clap_plugin.c's own HDS_CLAP_PARAMS
// already made for the synth's 12 FX slots (see that table's own comment for the full reasoning:
// no min/max/default metadata exists for the other ~9 fields per slot without hand-authoring it,
// and a host's automation-lane picker gets unusable if handed hundreds of raw entries). Every
// other field (fx_type, bypass, fx_source's fixed pin, chain_to_next, sidechain_source) is written
// directly by this plugin's own GUI -- see hds_fx_gui.c -- the same plain unsynchronized
// main-thread-writes/audio-thread-reads float race clap_plugin.c's own p->knobs already accepts
// (a torn read degrades to "the GUI briefly shows a stale value," never a crash).
#define HDS_FX_PARAM_COUNT (HDS_FX_BUS_COUNT * FX_SLOT_COUNT)

// A param's id IS bus*FX_SLOT_COUNT+slot directly -- no separate id<->(bus,slot) forward mapping
// needed, just this one inverse.
static void hds_fx_param_id_to_bus_slot(clap_id id, int *bus, int *slot) { *bus = (int)id / FX_SLOT_COUNT; *slot = (int)id % FX_SLOT_COUNT; }

static uint32_t hds_fx_params_count(const clap_plugin_t *plugin) { (void)plugin; return HDS_FX_PARAM_COUNT; }

static bool hds_fx_params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void)plugin;
    if (index >= HDS_FX_PARAM_COUNT) return false;
    int bus, slot;
    hds_fx_param_id_to_bus_slot((clap_id)index, &bus, &slot);
    memset(info, 0, sizeof(*info));
    info->id = (clap_id)index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "Bus %d Slot %d Mix", bus + 1, slot + 1);
    info->min_value = 0.0;
    info->max_value = 100.0;
    info->default_value = 100.0;
    return true;
}

static bool hds_fx_params_get_value(const clap_plugin_t *plugin, clap_id param_id, double *out_value) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    if (param_id >= HDS_FX_PARAM_COUNT) return false;
    int bus, slot;
    hds_fx_param_id_to_bus_slot(param_id, &bus, &slot);
    *out_value = (double)p->fx[bus][slot].mix;
    return true;
}

static bool hds_fx_params_value_to_text(const clap_plugin_t *plugin, clap_id param_id, double value, char *out_buffer, uint32_t out_buffer_capacity) {
    (void)plugin;
    if (param_id >= HDS_FX_PARAM_COUNT) return false;
    snprintf(out_buffer, out_buffer_capacity, "%.1f%%", value);
    return true;
}

static bool hds_fx_params_text_to_value(const clap_plugin_t *plugin, clap_id param_id, const char *param_value_text, double *out_value) {
    (void)plugin;
    if (param_id >= HDS_FX_PARAM_COUNT) return false;
    char *end = NULL;
    double v = strtod(param_value_text, &end);
    if (end == param_value_text) return false;
    *out_value = v;
    return true;
}

static void hds_fx_apply_param_event(HdsFxPlugin *p, const clap_event_param_value_t *ev) {
    if (ev->param_id >= HDS_FX_PARAM_COUNT) return;
    int bus, slot;
    hds_fx_param_id_to_bus_slot(ev->param_id, &bus, &slot);
    double v = ev->value;
    if (v < 0.0) v = 0.0; else if (v > 100.0) v = 100.0;
    p->fx[bus][slot].mix = (float)v;
    p->param_shadow[ev->param_id] = v;
}

// Bi-directional sync's other half (see clap_plugin.c's own hds_clap_push_changed_params for the
// full reasoning this mirrors exactly): a value the GUI changed needs to reach the host's own
// automation lane too, not just the reverse.
static void hds_fx_push_changed_params(HdsFxPlugin *p, const clap_output_events_t *out_events) {
    if (!out_events) return;
    for (uint32_t i = 0; i < HDS_FX_PARAM_COUNT; i++) {
        int bus, slot;
        hds_fx_param_id_to_bus_slot((clap_id)i, &bus, &slot);
        double current = (double)p->fx[bus][slot].mix;
        if (current == p->param_shadow[i]) continue;
        p->param_shadow[i] = current;
        clap_event_param_value_t ev = {0};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.param_id = (clap_id)i;
        ev.cookie = NULL;
        ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
        ev.value = current;
        out_events->try_push(out_events, &ev.header);
    }
}

static void hds_fx_params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    if (in) {
        uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; i++) {
            const clap_event_header_t *hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE) {
                hds_fx_apply_param_event(p, (const clap_event_param_value_t *)hdr);
            }
        }
    }
    hds_fx_push_changed_params(p, out);
}

static const clap_plugin_params_t k_params_ext = {
    .count = hds_fx_params_count,
    .get_info = hds_fx_params_get_info,
    .get_value = hds_fx_params_get_value,
    .value_to_text = hds_fx_params_value_to_text,
    .text_to_value = hds_fx_params_text_to_value,
    .flush = hds_fx_params_flush,
};

// ---------------------------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------------------------

static void hds_fx_init_bus_defaults(HdsFxPlugin *p, int bus) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        p->fx[bus][s] = (FXSlotParams){0};
        p->fx[bus][s].fx_type = FX_TYPE_OFF;
        p->fx[bus][s].mix = 100.0f;
        p->fx[bus][s].bypass = 0.0f;
        p->fx[bus][s].fx_source = (float)bus; // pinned -- see HdsFxPlugin's own comment
        p->fx[bus][s].chain_to_next = 0.0f;
        p->fx[bus][s].sidechain_source = -1.0f; // "Same as Source" -- see FXSlotParams' own comment
    }
    reset_fx_engine(p->fx_engine[bus]);
    p->bus_routing_mode[bus] = 0.0f; // Series
}

static bool hds_fx_plugin_init(const clap_plugin_t *plugin) { (void)plugin; return true; }

static void hds_fx_plugin_destroy(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    free(p);
}

static bool hds_fx_plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames_count, uint32_t max_frames_count) {
    (void)min_frames_count; (void)max_frames_count;
    (void)plugin;
    // SAMPLE_RATE is a hard compile-time constant throughout every ported effect (delay-line
    // sizing, filter coefficient math) -- same posture as clap_plugin.c's own activate().
    if ((int)(sample_rate + 0.5) != SAMPLE_RATE) {
        fprintf(stderr, "[hds-fx] refusing activate: host requested %.0fHz, HDS FX only supports %dHz\n", sample_rate, SAMPLE_RATE);
        return false;
    }
    return true;
}

static void hds_fx_plugin_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool hds_fx_plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void hds_fx_plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }

static void hds_fx_plugin_reset(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    // Full DSP state reset (delay lines, filter memories, envelope followers) per bus, parameter
    // values (fx[][].mix/fx_type/etc.) unchanged -- matches CLAP's own contract for reset(), same
    // as clap_plugin_reset()'s own posture in clap_plugin.c.
    for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) {
        for (int s = 0; s < FX_SLOT_COUNT; s++) {
            free(p->fx_engine[bus][s].state);
            p->fx_engine[bus][s].state = NULL;
            p->fx_engine[bus][s].active_type = FX_TYPE_OFF;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// process(): per-sample, per-bus -- process_fx_chain() itself already operates one sample at a
// time (see its own signature), so there is no BUFFER_SIZE-chunking loop to manage here the way
// clap_plugin.c's own render_audio_block-driven process() needs; this just walks frames_count
// directly, reading/writing straight from/to the host's own port buffers.
// ---------------------------------------------------------------------------------------------

static clap_process_status hds_fx_plugin_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;

    if (process->in_events) {
        uint32_t n = process->in_events->size(process->in_events);
        for (uint32_t i = 0; i < n; i++) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE) {
                hds_fx_apply_param_event(p, (const clap_event_param_value_t *)hdr);
            }
        }
    }

    // PRECONDITION for process_fx_chain() (see its own comment): a slot's active_type must be
    // synced to its current fx_type once per block, before any per-sample dispatch.
    for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) sync_fx_engine_types(p->fx[bus], p->fx_engine[bus]);

    float *in_l[HDS_FX_BUS_COUNT], *in_r[HDS_FX_BUS_COUNT], *out_l[HDS_FX_BUS_COUNT], *out_r[HDS_FX_BUS_COUNT];
    for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) {
        bool has_in = bus < (int)process->audio_inputs_count && process->audio_inputs[bus].data32 &&
                      process->audio_inputs[bus].channel_count >= 2 && process->audio_inputs[bus].data32[0] && process->audio_inputs[bus].data32[1];
        in_l[bus] = has_in ? process->audio_inputs[bus].data32[0] : NULL;
        in_r[bus] = has_in ? process->audio_inputs[bus].data32[1] : NULL;
        bool has_out = bus < (int)process->audio_outputs_count && process->audio_outputs[bus].data32 &&
                       process->audio_outputs[bus].channel_count >= 2 && process->audio_outputs[bus].data32[0] && process->audio_outputs[bus].data32[1];
        out_l[bus] = has_out ? process->audio_outputs[bus].data32[0] : NULL;
        out_r[bus] = has_out ? process->audio_outputs[bus].data32[1] : NULL;
        if (has_out) process->audio_outputs[bus].constant_mask = 0;
    }

    float sc_snapshot_l[HDS_FX_BUS_COUNT], sc_snapshot_r[HDS_FX_BUS_COUNT];
    for (uint32_t i = 0; i < process->frames_count; i++) {
        // Freeze every bus's own pre-FX dry sample first (see process_fx_chain()'s own
        // sc_snapshot_l/r comment for why this must happen before ANY bus's chain runs this
        // sample -- an order-dependent live read would give a slot a sometimes-pre-FX,
        // sometimes-post-FX view of another bus depending purely on which bus number is smaller).
        for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) {
            sc_snapshot_l[bus] = in_l[bus] ? in_l[bus][i] : 0.0f;
            sc_snapshot_r[bus] = in_r[bus] ? in_r[bus][i] : 0.0f;
        }
        for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) {
            float l = sc_snapshot_l[bus], r = sc_snapshot_r[bus];
            process_fx_chain(p->fx[bus], p->fx_engine[bus], (int)p->bus_routing_mode[bus], bus, sc_snapshot_l, sc_snapshot_r, &l, &r, p->gr_db_scratch[bus]);
            if (out_l[bus]) out_l[bus][i] = l;
            if (out_r[bus]) out_r[bus][i] = r;
        }
    }

    hds_fx_push_changed_params(p, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

static const void *hds_fx_plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &k_audio_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &k_params_ext;
    return NULL;
}

static void hds_fx_plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

// ---------------------------------------------------------------------------------------------
// Descriptor + factory
// ---------------------------------------------------------------------------------------------

static const char *const k_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MULTI_EFFECTS, NULL};

static const clap_plugin_descriptor_t k_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.hds-synth.hds-fx",
    .name = "HDS FX",
    .vendor = "",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0-experiment",
    .description = "HDS's FX chain (HT-76, Fairchild 670, LA-2A, FDNVerb, ...) as a standalone, "
                    "synth-free Linux CLAP effects plugin. 8 independent stereo buses, each with "
                    "up to 12 chained/parallel-routed FX slots -- host-automatable/UI-driven only, "
                    "no networking.",
    .features = k_features,
};

static const clap_plugin_t *hds_fx_factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void)factory;
    if (strcmp(plugin_id, k_descriptor.id) != 0) return NULL;

    HdsFxPlugin *p = (HdsFxPlugin *)calloc(1, sizeof(HdsFxPlugin));
    if (!p) return NULL;

    p->host = host;
    for (int bus = 0; bus < HDS_FX_BUS_COUNT; bus++) hds_fx_init_bus_defaults(p, bus);
    for (uint32_t i = 0; i < HDS_FX_PARAM_COUNT; i++) p->param_shadow[i] = 100.0; // matches mix's own default above

    p->plugin.desc = &k_descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = hds_fx_plugin_init;
    p->plugin.destroy = hds_fx_plugin_destroy;
    p->plugin.activate = hds_fx_plugin_activate;
    p->plugin.deactivate = hds_fx_plugin_deactivate;
    p->plugin.start_processing = hds_fx_plugin_start_processing;
    p->plugin.stop_processing = hds_fx_plugin_stop_processing;
    p->plugin.reset = hds_fx_plugin_reset;
    p->plugin.process = hds_fx_plugin_process;
    p->plugin.get_extension = hds_fx_plugin_get_extension;
    p->plugin.on_main_thread = hds_fx_plugin_on_main_thread;

    return &p->plugin;
}

static uint32_t hds_fx_factory_get_plugin_count(const clap_plugin_factory_t *factory) { (void)factory; return 1; }

static const clap_plugin_descriptor_t *hds_fx_factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void)factory;
    return index == 0 ? &k_descriptor : NULL;
}

static const clap_plugin_factory_t k_factory = {
    .get_plugin_count = hds_fx_factory_get_plugin_count,
    .get_plugin_descriptor = hds_fx_factory_get_plugin_descriptor,
    .create_plugin = hds_fx_factory_create_plugin,
};

// ---------------------------------------------------------------------------------------------
// DSO entry point
// ---------------------------------------------------------------------------------------------

static bool hds_fx_entry_init(const char *plugin_path) { (void)plugin_path; return true; }
static void hds_fx_entry_deinit(void) {}

static const void *hds_fx_entry_get_factory(const char *factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &k_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = hds_fx_entry_init,
    .deinit = hds_fx_entry_deinit,
    .get_factory = hds_fx_entry_get_factory,
};
