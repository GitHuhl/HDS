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
// This is just a multi-FX: a flat, ordered pool of slots, freely wired to whatever the DAW routes
// in. No "bus" concept anywhere (Reaper doesn't have one, and neither does this plugin) -- 8 real,
// full stereo CLAP ports each way (HDS_FX_PORT_COUNT), the same physical shape as any normal stereo
// insert effect's ports. What's NOT assumed anymore is that a port is always used in stereo: each
// slot's own Input selector picks which one of the 8 ports feeds it AND whether this chain treats
// that port as Mono (only the port's L channel is read, centered to dry_l=dry_r before any FX runs)
// or Stereo (L and R both read as a genuine stereo signal) -- only meaningful for a slot that's the
// HEAD of its own chain (see hds_fx_slot_is_chain_head()); a slot fed by the previous slot's own
// Chain-to-Next just inherits that running signal instead, no Input of its own. Once a slot's own
// Chain-to-Next is off (it's the TAIL of its chain, or a standalone single-slot chain), its own
// Output selector -- independent of its Input, same Mono/Stereo choice, either of which is a REAL
// stereo port either way -- says which port the result actually lands on and how: Mono writes the
// identical value to BOTH L and R of the chosen port (a proper centered mono-to-stereo placement,
// never just one ear), Stereo writes l to L and r to R. A chain reading Port 3 can perfectly well
// come back out on Port 7. Chain-to-Next always works, full stop, on every slot, unconditionally --
// there is no separate Series/Parallel mode to gate it: Parallel mode's own existing semantics
// (every owned slot starts a fresh branch UNLESS the previous owned slot's own Chain-to-Next says
// otherwise) ALREADY reproduce plain Series exactly when every slot chains to the next (the branch
// divisor becomes 1, i.e. no attenuation) -- so this plugin just always calls process_fx_chain() in
// Parallel mode and the redundant mode switch is gone entirely (an earlier draft copied it from the
// synth's own per-source grid and it never made sense here).
//
// A slot's own fx_source is which of the 8 ports its chain's Input reads from -- freely
// reassignable, not pinned at creation, see HdsFxPlugin's own comment for why this needs ONE shared
// pool of slots (matching the synth's real architecture) rather than per-port pinned arrays.
// Reordering (Up/Down) is disabled outright on any slot that's part of a chain link (either end) --
// a value-swap would silently rewire which chain a slot's params actually belong to, since a chain
// link is defined by ARRAY POSITION + matching fx_source, not by an ID that travels with the slot.
//
// The synth's own ext-in pairs don't have this Mono/Stereo choice yet (they always assume full
// stereo) -- a real, separate, cross-cutting follow-up (OSC wire + state save/load + frontend UI),
// not bundled into this file.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "../third_party/clap/include/clap/clap.h" // already pulls in ext/gui.h and ext/timer-support.h

// The handful of plain constants fx_types.h/fx_chain.c need defined before they're #include'd --
// backend.c normally supplies these (see its own header comment on each); this file supplies its
// own copies instead of pulling in backend.c wholesale, since none of backend.c's synth machinery
// is wanted here at all.
#define SAMPLE_RATE 48000
#define BUFFER_SIZE 1024
#define SAMPLE_PATH_LEN 256
// The synth's own FX bus uses 12 (SynthSharedControls.fx[FX_SLOT_COUNT]) because it's a shared
// budget with everything else a Combi Program needs; HDS FX has no synth competing for that budget
// at all, so this is sized generously -- 32 slots shared across all 8 ports, plenty of headroom for
// several chains each running a real multi-slot chain at once. FX_SLOT_COUNT is the exact macro name
// process_fx_chain()/sync_fx_engine_types()/reset_fx_engine() (fx_chain.c) size their own array
// parameters by -- this file is its own separate translation unit (never linked with backend.c's
// copy of fx_chain.c), so redefining it here to a different value than backend.c's own 12 is safe
// and gives the exact same shared-pool bucketing model fx_chain.c already implements, for free, at
// whatever pool size this plugin wants.
#define FX_SLOT_COUNT 32
// 8 real stereo CLAP ports each way -- see this file's own header comment for why a port's own
// Mono/Stereo USE is a per-slot-group choice, not baked into the port count/shape itself. A slot's
// fx_source (0..HDS_FX_PORT_COUNT-1) picks which one of these 8 ports its chain's Input reads from;
// that same 0-7 id is also process_fx_chain()'s own "source" concept (SOURCE_COUNT below) and
// sidechain_source's own numbering (see fx_types.h) -- one flat id space throughout.
#define HDS_FX_PORT_COUNT 8
#define SOURCE_COUNT HDS_FX_PORT_COUNT
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
// fx/fx_engine: ONE shared pool of FX_SLOT_COUNT slots across ALL 8 ports -- physically identical
// model to backend.c's own SynthSharedControls.fx[FX_SLOT_COUNT]/FXSlotEngine
// fx_engine[FX_SLOT_COUNT], just sized bigger (see FX_SLOT_COUNT's own comment) since there's no
// synth sharing the budget here. Each slot's own fx_source field (0..HDS_FX_PORT_COUNT-1) says which
// ONE port its chain's Input reads from -- process_fx_chain()'s own bucketing
// (`fx_params[s].fx_source == source_id`) is what makes this genuinely reassignable: a slot can move
// from Port 3 to Port 5 just by writing a new fx_source, no engine-state shuffling needed.
// fx_engine[] stays indexed by physical POOL slot regardless of which port owns it this block, so a
// slot's lazily-allocated DSP state (e.g. a delay's buffered tail) survives a live port reassignment
// exactly like it already survives a live bypass toggle -- see process_fx_chain()'s own comment.
// Every slot defaults to Port 1 (fx_source = 0) at creation, matching the Standalone FX tab's own
// "every slot defaults to Program" convention -- see hds_fx_init_pool_defaults().
typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;

    FXSlotParams fx[FX_SLOT_COUNT];
    FXSlotEngine fx_engine[FX_SLOT_COUNT];

    // Per-port Input routing: whether a chain reading this port treats it as Mono (only L is read,
    // centered to dry_l=dry_r) or Stereo (L and R both read as a genuine stereo pair) -- see this
    // file's own header comment. Indexed by the SAME 0..HDS_FX_PORT_COUNT-1 id every chain-head
    // slot's own fx_source already uses, since this is a property of the PORT/chain, not of any one
    // slot (every slot sharing a chain necessarily shares its own chain's Input).
    float port_input_stereo[HDS_FX_PORT_COUNT];
    // Per-port Output routing (only consulted for a chain TAIL slot, chain_to_next == 0): which
    // port (0..HDS_FX_PORT_COUNT-1) the result is written to -- independent of the chain's own
    // Input port -- and Mono (identical value written to both L and R of that port, a proper
    // centered placement) or Stereo (l -> L, r -> R). Indexed by fx_source (which chain/group this
    // is), same as port_input_stereo above. Defaults to output_channel==the port's own index and
    // Stereo, i.e. today's obvious behavior (read Port N stereo, write back to Port N stereo)
    // reproduced exactly until a chain's tail slot changes it.
    float port_output_channel[HDS_FX_PORT_COUNT];
    float port_output_stereo[HDS_FX_PORT_COUNT];

    float gr_db_scratch[FX_SLOT_COUNT]; // required non-NULL out-param, not yet exposed to the host/GUI -- shared across all 8 ports' own calls, same convention as backend.c's own gr_db_out (see render_audio_block)

    // CLAP params extension shadow (see hds_fx_push_changed_params's own comment, same pattern
    // clap_plugin.c already uses for HDS_CLAP_PARAMS) -- one entry per exposed param.
    double param_shadow[FX_SLOT_COUNT];

    // Opaque HdsFxGuiWindow* (defined in hds_fx_gui_x11.c, #include'd below) -- NULL whenever no GUI
    // is open. Deliberately typed void* here rather than forward-declaring the real struct: this
    // file's own HdsFxPlugin definition has no business knowing X11/Cairo types at all, only the
    // platform-specific GUI file does -- see hds_fx_gui_draw.c's own header comment on why the
    // draw/layout code and the platform window code are two separate files in the first place.
    void *gui_window;
} HdsFxPlugin;

// hds_fx_gui_draw.c: pure Cairo, platform-agnostic layout/drawing/hit-testing (needs HdsFxPlugin,
// just defined above, plus FXSlotParams/FX_TYPE_*/FX_SLOT_COUNT, all already in scope from
// fx_types.h/this file's own defines). hds_fx_gui_x11.c: the X11/XEmbed/event-pump shim, needs
// everything hds_fx_gui_draw.c defines (HdsFxGuiViewState, hds_fx_gui_render, hds_fx_gui_on_*).
#include "hds_fx_gui_draw.c"
#include "hds_fx_gui_x11.c"

// ---------------------------------------------------------------------------------------------
// audio-ports extension: HDS_FX_PORT_COUNT real stereo inputs, HDS_FX_PORT_COUNT real stereo
// outputs, named to match each other 1:1 ("Input N" -> "Output N") -- a host inserting this on a
// single track just uses pair 0 (IS_MAIN on both sides) and never notices the other 7 exist; a host
// that wants HDS FX processing several ports at once (e.g. several tracks' sends summed into one
// plugin instance rather than one instance per track) wires up as many of the rest as it wants.
// Whether a given port is actually USED as mono or stereo is a per-chain choice made in the GUI
// (port_input_stereo/port_output_stereo), never baked into the port declaration itself -- see this
// file's own header comment. An unconnected input port is silence in (see process()'s own null
// check) -- harmless, exactly the ext-in pattern backend.c's own audio-ports extension established.
// ---------------------------------------------------------------------------------------------

static uint32_t hds_fx_audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin; (void)is_input;
    return (uint32_t)HDS_FX_PORT_COUNT;
}

static bool hds_fx_audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void)plugin;
    if (index >= (uint32_t)HDS_FX_PORT_COUNT) return false;
    info->id = index;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = index; // this port's input can safely alias its own output buffer -- process() never reads input[port] after writing output[port]
    snprintf(info->name, sizeof(info->name), "%s %u", is_input ? "Input" : "Output", (unsigned int)(index + 1));
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    return true;
}

static const clap_plugin_audio_ports_t k_audio_ports_ext = {
    .count = hds_fx_audio_ports_count,
    .get = hds_fx_audio_ports_get,
};

// ---------------------------------------------------------------------------------------------
// params extension: per-slot Mix -- same curated "expose Mix only, drive everything else (Type,
// Bypass, Source, routing) from the plugin's own GUI" choice clap_plugin.c's own HDS_CLAP_PARAMS
// already made for the synth's 12 FX slots (see that table's own comment for the full reasoning:
// no min/max/default metadata exists for the other ~9 fields per slot without hand-authoring it,
// and a host's automation-lane picker gets unusable if handed hundreds of raw entries). One param
// per POOL slot (not per bus*slot -- see HdsFxPlugin's own comment, a slot's bus is now dynamic,
// not part of its identity). Every other field (fx_type, bypass, fx_source, chain_to_next,
// sidechain_source) is written directly by this plugin's own GUI -- see hds_fx_gui.c -- the same
// plain unsynchronized main-thread-writes/audio-thread-reads float race clap_plugin.c's own
// p->knobs already accepts (a torn read degrades to "the GUI briefly shows a stale value," never a
// crash).
#define HDS_FX_PARAM_COUNT FX_SLOT_COUNT

static uint32_t hds_fx_params_count(const clap_plugin_t *plugin) { (void)plugin; return HDS_FX_PARAM_COUNT; }

static bool hds_fx_params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void)plugin;
    if (index >= HDS_FX_PARAM_COUNT) return false;
    memset(info, 0, sizeof(*info));
    info->id = (clap_id)index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "Slot %u Mix", (unsigned int)(index + 1));
    info->min_value = 0.0;
    info->max_value = 100.0;
    info->default_value = 100.0;
    return true;
}

static bool hds_fx_params_get_value(const clap_plugin_t *plugin, clap_id param_id, double *out_value) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    if (param_id >= HDS_FX_PARAM_COUNT) return false;
    *out_value = (double)p->fx[param_id].mix;
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
    double v = ev->value;
    if (v < 0.0) v = 0.0; else if (v > 100.0) v = 100.0;
    p->fx[ev->param_id].mix = (float)v;
    p->param_shadow[ev->param_id] = v;
}

// Bi-directional sync's other half (see clap_plugin.c's own hds_clap_push_changed_params for the
// full reasoning this mirrors exactly): a value the GUI changed needs to reach the host's own
// automation lane too, not just the reverse.
static void hds_fx_push_changed_params(HdsFxPlugin *p, const clap_output_events_t *out_events) {
    if (!out_events) return;
    for (uint32_t i = 0; i < HDS_FX_PARAM_COUNT; i++) {
        double current = (double)p->fx[i].mix;
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

static void hds_fx_init_pool_defaults(HdsFxPlugin *p) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        p->fx[s] = (FXSlotParams){0};
        p->fx[s].fx_type = FX_TYPE_OFF;
        p->fx[s].mix = 100.0f;
        p->fx[s].bypass = 0.0f;
        p->fx[s].fx_source = 0.0f; // every slot defaults to Port 1 -- freely reassignable via the GUI's own Input pill, see HdsFxPlugin's own comment
        p->fx[s].chain_to_next = 0.0f;
        p->fx[s].sidechain_source = -1.0f; // "Same as Source" -- see FXSlotParams' own comment
    }
    reset_fx_engine(p->fx_engine);
    for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
        p->port_input_stereo[port] = 1.0f;  // default: read the port as real stereo, today's obvious behavior
        p->port_output_channel[port] = (float)port; // default: write back to the SAME port
        p->port_output_stereo[port] = 1.0f;
    }
}

static bool hds_fx_plugin_init(const clap_plugin_t *plugin) { (void)plugin; return true; }

static void hds_fx_plugin_destroy(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    // Defensive: a well-behaved host always calls clap_plugin_gui->destroy() before this, but
    // clean up an open X11 window/Display here too rather than leak it if one doesn't.
    hds_fx_gui_destroy_internal(p);
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
    // Full DSP state reset (delay lines, filter memories, envelope followers) across the whole
    // pool, parameter values (fx[].mix/fx_type/etc.) unchanged -- matches CLAP's own contract for
    // reset(), same as clap_plugin_reset()'s own posture in clap_plugin.c.
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        free(p->fx_engine[s].state);
        p->fx_engine[s].state = NULL;
        p->fx_engine[s].active_type = FX_TYPE_OFF;
    }
}

// ---------------------------------------------------------------------------------------------
// process(): per-sample, per-port -- process_fx_chain() itself already operates one sample at a
// time (see its own signature), so there is no BUFFER_SIZE-chunking loop to manage here the way
// clap_plugin.c's own render_audio_block-driven process() needs; this just walks frames_count
// directly, reading/writing straight from/to the host's own port buffers. Always calls
// process_fx_chain() in Parallel mode (routing_mode==1) -- see this file's own header comment for
// why that alone, combined with chain_to_next, already reproduces plain Series exactly when every
// slot in a chain chains to the next, so there's no separate mode to thread through here at all.
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
    // synced to its current fx_type once per block, before any per-sample dispatch. One call now
    // (not once per port) -- there's only one pool.
    sync_fx_engine_types(p->fx, p->fx_engine);
    // gr_db_scratch zeroed once per block, then refreshed by whichever port's call owns each slot
    // this sample -- same convention as backend.c's own gr_db_out (see render_audio_block).
    for (int s = 0; s < FX_SLOT_COUNT; s++) p->gr_db_scratch[s] = 0.0f;

    float *in_l[HDS_FX_PORT_COUNT], *in_r[HDS_FX_PORT_COUNT], *out_l[HDS_FX_PORT_COUNT], *out_r[HDS_FX_PORT_COUNT];
    for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
        bool has_in = port < (int)process->audio_inputs_count && process->audio_inputs[port].data32 &&
                      process->audio_inputs[port].channel_count >= 2 && process->audio_inputs[port].data32[0] && process->audio_inputs[port].data32[1];
        in_l[port] = has_in ? process->audio_inputs[port].data32[0] : NULL;
        in_r[port] = has_in ? process->audio_inputs[port].data32[1] : NULL;
        bool has_out = port < (int)process->audio_outputs_count && process->audio_outputs[port].data32 &&
                       process->audio_outputs[port].channel_count >= 2 && process->audio_outputs[port].data32[0] && process->audio_outputs[port].data32[1];
        out_l[port] = has_out ? process->audio_outputs[port].data32[0] : NULL;
        out_r[port] = has_out ? process->audio_outputs[port].data32[1] : NULL;
        if (has_out) {
            process->audio_outputs[port].constant_mask = 0;
            // Zeroed up front, once per block, then ACCUMULATED into below (+=) -- an Output
            // selector can point several different chains at the SAME physical port (see this
            // file's own header comment), so this can't just be overwritten per chain the way a
            // fixed 1:1 input->output mapping could.
            memset(out_l[port], 0, sizeof(float) * process->frames_count);
            memset(out_r[port], 0, sizeof(float) * process->frames_count);
        }
    }

    float sc_snapshot_l[HDS_FX_PORT_COUNT], sc_snapshot_r[HDS_FX_PORT_COUNT];
    for (uint32_t i = 0; i < process->frames_count; i++) {
        // Freeze every port's own pre-FX dry sample first (see process_fx_chain()'s own
        // sc_snapshot_l/r comment for why this must happen before ANY chain runs this sample -- an
        // order-dependent live read would give a slot a sometimes-pre-FX, sometimes-post-FX view of
        // another port depending purely on which port number is smaller). A Mono-Input port reads
        // ONLY its own L channel, centered to both dry_l/dry_r -- see this file's own header
        // comment on why that's the correct "proper M->S" treatment, not an average or an ignored R.
        for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
            float l = in_l[port] ? in_l[port][i] : 0.0f;
            if (p->port_input_stereo[port] != 0.0f) {
                sc_snapshot_l[port] = l;
                sc_snapshot_r[port] = in_r[port] ? in_r[port][i] : 0.0f;
            } else {
                sc_snapshot_l[port] = l;
                sc_snapshot_r[port] = l; // centered mono
            }
        }
        for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
            float l = sc_snapshot_l[port], r = sc_snapshot_r[port];
            process_fx_chain(p->fx, p->fx_engine, /* always Parallel */ 1, port, sc_snapshot_l, sc_snapshot_r, &l, &r, p->gr_db_scratch);

            // Output routing is per-CHAIN (keyed by fx_source == port, same id process_fx_chain()
            // just bucketed slots by), not fixed to this same port -- see this file's own header
            // comment. A port that owns zero slots is a no-op passthrough from process_fx_chain()
            // itself (its own zero-slot-count guard), so this still correctly writes that port's
            // own dry signal out via ITS OWN Output selector (today's obvious default: right back
            // to the same port), rather than silently dropping unconfigured ports.
            int out_ch = (int)p->port_output_channel[port];
            if (out_ch < 0 || out_ch >= HDS_FX_PORT_COUNT) out_ch = port;
            if (p->port_output_stereo[port] != 0.0f) {
                if (out_l[out_ch]) out_l[out_ch][i] += l;
                if (out_r[out_ch]) out_r[out_ch][i] += r;
            } else {
                float centered = 0.5f * (l + r); // proper centered mono-to-stereo placement, never just one ear
                if (out_l[out_ch]) out_l[out_ch][i] += centered;
                if (out_r[out_ch]) out_r[out_ch][i] += centered;
            }
        }
    }

    hds_fx_push_changed_params(p, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

static const void *hds_fx_plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &k_audio_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &k_params_ext;
    if (strcmp(id, CLAP_EXT_GUI) == 0) return &k_gui_ext;
    // Our own clap_plugin_timer_support_t -- lets the HOST call our on_timer(). Separately, this
    // plugin also calls the host's OWN clap_host_timer_support_t (register_timer/unregister_timer,
    // via p->host->get_extension in hds_fx_gui_x11.c's show()/hide()) to actually request one.
    if (strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &k_timer_support_ext;
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
                    "synth-free Linux CLAP multi-FX plugin. 8 real stereo ports each way, each "
                    "independently Mono- or Stereo-capable, sharing a pool of 32 freely reassignable, "
                    "freely chained FX slots with independent per-chain Input/Output routing -- "
                    "host-automatable/UI-driven only, no networking.",
    .features = k_features,
};

static const clap_plugin_t *hds_fx_factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void)factory;
    if (strcmp(plugin_id, k_descriptor.id) != 0) return NULL;

    HdsFxPlugin *p = (HdsFxPlugin *)calloc(1, sizeof(HdsFxPlugin));
    if (!p) return NULL;

    p->host = host;
    hds_fx_init_pool_defaults(p);
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
