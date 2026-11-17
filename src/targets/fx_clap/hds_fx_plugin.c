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
// NAM (FX_TYPE_NAM) is enabled (HDS_NAM_ENABLED 1 below) and genuinely works here: it needs the
// small C++ bridge (nam_bridge.h/.cpp over the vendored NeuralAmpModelerCore) clap_plugin.c also
// links against -- see the Makefile's own hds-fx target for how NAM_OBJS get pulled into this
// plugin's own link step. This plugin's first pass shipped with NAM compiled out entirely, deferred
// as a scoped follow-up -- turned out that follow-up needed more than just flipping the flag: model/
// cab-IR (re)loading (poll_fx_nam_requests()/fx_nam_reload_model()/fx_nam_reload_ir(), all in
// fx_chain.c) had been living in hds_standalone.c, never called from ANY CLAP plugin's process() at
// all, which meant hds.clap's own FX_TYPE_NAM slots couldn't load a model either -- a real,
// confirmed bug, not a hypothetical one. Fixed by moving those three functions back into fx_chain.c
// (genuinely shared now) and having every FX-bus-running translation unit call
// poll_fx_nam_requests() itself once per block -- see hds_fx_plugin_process()'s own call, and
// backend.c's render_audio_block for the equivalent shared fix on the synth's own side.
//
// HardTune's (FX_TYPE_HARDTUNE) MIDI Target Source mode and its live-MIDI vibrato control both
// read four small globals (g_hardtune_held_note/g_hardtune_bend_semitones/g_hardtune_modwheel/
// g_hardtune_channel_pressure) that backend.c's own MIDI dispatch normally feeds -- this plugin
// declares a real note-ports extension (k_note_ports_ext below, 1 input) and decodes
// CLAP_EVENT_NOTE_ON/OFF/CLAP_EVENT_MIDI in process() to feed those same four globals itself (see
// hds_fx_hardtune_note_on/off and the event-decode loop in hds_fx_plugin_process()), so both Target
// Source: MIDI and the CC/pitch-bend-driven vibrato work here exactly as they do in hds.clap/
// synth_engine. This used to be a real, confirmed gap (the globals existed only so fx_chain.c would
// compile, never written anywhere in this file) -- Target Source: MIDI silently applied zero
// correction, forever, with no indication anything was wrong. A host that never connects this
// plugin's note port just leaves HardTune in the same state as before: Scale/Key mode is
// unaffected either way, and MIDI-dependent modes simply see no note held / no live controller data
// until one is.
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
#include <stdarg.h>
#include <errno.h> // needed by fx_chain.c's own seq_audio_load_wav_from_disk() (strerror(errno))
#include <clap/clap.h> // already pulls in ext/gui.h and ext/timer-support.h

// SAMPLE_RATE/BUFFER_SIZE/SAMPLE_PATH_LEN come from hds_config.h (via fx_chain.h below).
#include "core/hds_config.h"
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
// NAM was deliberately disabled in this plugin's first pass (see this file's own header comment,
// now stale on that point) until fx_chain.c's own poll_fx_nam_requests()/fx_nam_reload_model()/
// fx_nam_reload_ir() actually worked from ANY CLAP plugin context -- they didn't (a real,
// confirmed gap: they lived in hds_standalone.c, never called from clap_plugin.c's process()
// either, so hds.clap's own FX_TYPE_NAM slots could never load a model). Now that they're back in
// the genuinely shared fx_chain.c and this plugin calls poll_fx_nam_requests() itself every block
// (see hds_fx_plugin_process()'s own call), NAM works here for real -- same nam_bridge.h C++ shim
// clap_plugin.c already links, the one piece of C++ in an otherwise pure-C build (see the
// Makefile's own hds-fx target for how NAM_OBJS get linked in).
#define HDS_NAM_ENABLED 1
#include "fx/nam_bridge.h" // extern "C" shim over NeuralAmpModelerCore -- see that header's own comment

// fx_chain.c's own g_hardtune_held_note/g_hardtune_bend_semitones/g_hardtune_modwheel/
// g_hardtune_channel_pressure globals (declared there, not here -- see this file's own header
// comment) ARE written to by this plugin, from its own note-ports extension -- see
// hds_fx_hardtune_note_on()/hds_fx_hardtune_note_off() and hds_fx_plugin_process()'s own event loop,
// further down in this file (both need FXSlotParams/fx_chain.c's globals already in scope, so they
// can't live any earlier than this #include).
#include "fx/fx_types.h"
#include "fx/fx_chain.h" // FX bus DSP, compiled separately (src/fx_chain.c)
#include "gui/hds_fx_gui_host.h" // HdsFxGuiHost -- see its own header comment; HdsFxPlugin embeds one below

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
    // Input port. Indexed by fx_source (which chain/group this is), same as port_input_stereo
    // above. ALWAYS writes l -> L, r -> R of that port -- no separate Mono/Stereo choice here (an
    // earlier revision had one, but every one of the HDS_FX_PORT_COUNT CLAP ports is already a
    // fixed 2-channel stereo pair, so "Mono" could only ever mean "write the same centered value to
    // BOTH channels of the chosen port," never "land on one independent physical channel and leave
    // the other untouched" -- confirmed a real, confirmed-live-tested source of confusion (a
    // misleadingly labeled "N+N+1" option that didn't combine any ports at all), not a real
    // per-channel routing capability. See hds_fx_gui_draw.c's own "Dropdowns" section for the full
    // reasoning. Defaults to output_channel==the port's own index, i.e. today's obvious behavior
    // (read Port N, write back to Port N) reproduced exactly until a chain's tail slot changes it.
    float port_output_channel[HDS_FX_PORT_COUNT];
    // Off: this chain's Input reads silence (port_input_off) / this chain's Output goes nowhere
    // (port_output_off), regardless of whatever port/stereo choice is otherwise remembered for it
    // -- a real, separate state from any port number, not a sentinel value squeezed into one. See
    // hds_fx_gui_draw.c's own "Dropdowns" section for how the GUI presents this as its own option.
    float port_input_off[HDS_FX_PORT_COUNT];
    float port_output_off[HDS_FX_PORT_COUNT];

    float gr_db_scratch[FX_SLOT_COUNT]; // this block's own RAW reading, zeroed and refilled fresh every process() call -- NOT what the GUI reads directly (see gr_db_smoothed just below)
    // Persistent, chased-toward-gr_db_scratch METER-BALLISTICS state -- exposed to the GUI via
    // gui_host.fx_gr_db, same fast-attack/slow-release idea and same reason as backend.c's own
    // gr_db_out (see render_audio_block's own comment on it): the raw per-sample reading changes
    // too fast for a human to read the dB number off a live meter for any fast compressor (Gate,
    // Limiter). Chased once per process() call, right after the per-sample port loop, using
    // process->frames_count as this call's own block_size.
    float gr_db_smoothed[FX_SLOT_COUNT];
    float dump_avail_scratch[FX_SLOT_COUNT]; // same convention as gr_db_scratch just above, for FX_TYPE_BROADCAST_DELAY's own dump-availability meter -- exposed via gui_host.fx_dump_avail_sec below

    // Every slot's own last-computed post-mix output, in POOL-SLOT order -- see process_fx_chain()'s
    // own slot_out_l/r comment. Persists across blocks like fx_engine[] itself, never reset to 0.
    float fx_slot_out_l[FX_SLOT_COUNT];
    float fx_slot_out_r[FX_SLOT_COUNT];

    // CLAP params extension shadow (see hds_fx_push_changed_params's own comment, same pattern
    // clap_plugin.c already uses for HDS_CLAP_PARAMS) -- one entry per exposed param.
    double param_shadow[FX_SLOT_COUNT];

    // The view hds_fx_gui_draw.c/hds_fx_gui_x11.c actually operate on -- see HdsFxGuiHost's own
    // header comment. Populated once, right after the arrays above exist (hds_fx_gui_host_of()'s
    // own call site), with pointers into THIS struct's own fx[]/port_*[] arrays -- never a copy.
    // gui_window (inside gui_host) replaces what used to be this struct's own separate `void
    // *gui_window` field -- one real, persistent HdsFxGuiHost per plugin instance, not two places
    // that could theoretically drift apart.
    HdsFxGuiHost gui_host;
} HdsFxPlugin;

// Every real host with an FX_SLOT_COUNT pool provides these two accessors before #include'ing
// hds_fx_gui_x11.c -- see that file's own header comment. Trivial for this one (a single flat
// HdsFxPlugin owns everything directly), but this is the one seam that lets the exact same
// hds_fx_gui_x11.c/hds_fx_gui_draw.c serve a structurally different host (e.g. hds.clap's own
// plugin struct, which nests its FX pool inside a much bigger SynthSharedControls) without
// touching either file.
static HdsFxGuiHost *hds_fx_gui_host_of(const clap_plugin_t *plugin) {
    return &((HdsFxPlugin *)plugin->plugin_data)->gui_host;
}
static const clap_host_t *hds_fx_gui_clap_host_of(const clap_plugin_t *plugin) {
    return ((HdsFxPlugin *)plugin->plugin_data)->host;
}

// hds_fx_gui_draw.c: pure Cairo, platform-agnostic layout/drawing/hit-testing (needs HdsFxPlugin,
// just defined above, plus FXSlotParams/FX_TYPE_*/FX_SLOT_COUNT, all already in scope from
// fx_types.h/this file's own defines). hds_fx_gui_x11.c: the X11/XEmbed/event-pump shim, needs
// everything hds_fx_gui_draw.c defines (HdsFxGuiViewState, hds_fx_gui_render, hds_fx_gui_on_*).
#include "gui/hds_fx_gui_draw.h" // FX pool GUI, compiled separately (src/hds_fx_gui_draw.c)
#ifdef _WIN32
#include "platform/win32/hds_fx_gui_win32.c"
#else
#include "platform/x11/hds_fx_gui_x11.c"
#endif

// ---------------------------------------------------------------------------------------------
// audio-ports extension: HDS_FX_PORT_COUNT real stereo inputs, HDS_FX_PORT_COUNT real stereo
// outputs, named to match each other 1:1 ("Input N" -> "Output N") -- a host inserting this on a
// single track just uses pair 0 (IS_MAIN on both sides) and never notices the other 7 exist; a host
// that wants HDS FX processing several ports at once (e.g. several tracks' sends summed into one
// plugin instance rather than one instance per track) wires up as many of the rest as it wants.
// Whether a given INPUT port is read as mono or stereo is a per-chain choice made in the GUI
// (port_input_stereo), never baked into the port declaration itself -- see this file's own header
// comment. Output has no such choice (always that port's own real stereo pair, see
// port_output_channel's own comment). An unconnected input port is silence in (see process()'s own
// null check) -- harmless, exactly the ext-in pattern backend.c's own audio-ports extension established.
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
// note-ports extension: 1 input, 0 outputs -- same shape as clap_plugin.c's own k_note_ports_ext.
// Exists purely so a host can route MIDI performance data to FX_TYPE_HARDTUNE's own Target Source:
// MIDI mode and its CC/pitch-bend-driven vibrato (see g_hardtune_held_note's own comment in
// fx_chain.c, and this file's own header comment on why that mode used to be a confirmed dead end
// here) -- every other FX type ignores note/MIDI events entirely, so leaving this port unconnected
// is exactly as harmless as leaving any other optional port unconnected.
// ---------------------------------------------------------------------------------------------

static uint32_t hds_fx_note_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? 1 : 0;
}

static bool hds_fx_note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    (void)plugin;
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(info->name, sizeof(info->name), "%s", "MIDI In (HardTune)");
    return true;
}

static const clap_plugin_note_ports_t k_note_ports_ext = {
    .count = hds_fx_note_ports_count,
    .get = hds_fx_note_ports_get,
};

// Minimal last-note-priority held-note tracker feeding FX_TYPE_HARDTUNE's own g_hardtune_held_note
// (fx_chain.c) -- the same semantics as backend.c's mono_stack_push/mono_stack_remove against its
// own MonoNoteStack/g_hardtune_note_stack, reimplemented locally since this file deliberately
// doesn't pull in backend.c at all (see this file's own header comment) and has no Programs/voices
// of its own to hang a bigger stack struct off of. -1 (via hds_fx_hardtune_note_off draining the
// stack to empty) means "nothing held", matching g_hardtune_held_note's own documented sentinel.
#define HDS_FX_HARDTUNE_STACK_SIZE 8
static struct { int notes[HDS_FX_HARDTUNE_STACK_SIZE]; int count; } g_hds_fx_hardtune_notes = {0};

static void hds_fx_hardtune_note_on(int note) {
    for (int i = 0; i < g_hds_fx_hardtune_notes.count; i++) {
        if (g_hds_fx_hardtune_notes.notes[i] == note) {
            for (int j = i; j < g_hds_fx_hardtune_notes.count - 1; j++) g_hds_fx_hardtune_notes.notes[j] = g_hds_fx_hardtune_notes.notes[j + 1];
            g_hds_fx_hardtune_notes.count--;
            break;
        }
    }
    if (g_hds_fx_hardtune_notes.count < HDS_FX_HARDTUNE_STACK_SIZE) {
        g_hds_fx_hardtune_notes.notes[g_hds_fx_hardtune_notes.count++] = note;
    } else {
        for (int j = 0; j < HDS_FX_HARDTUNE_STACK_SIZE - 1; j++) g_hds_fx_hardtune_notes.notes[j] = g_hds_fx_hardtune_notes.notes[j + 1];
        g_hds_fx_hardtune_notes.notes[HDS_FX_HARDTUNE_STACK_SIZE - 1] = note;
    }
    g_hardtune_held_note = (float)note;
}

static void hds_fx_hardtune_note_off(int note) {
    for (int i = 0; i < g_hds_fx_hardtune_notes.count; i++) {
        if (g_hds_fx_hardtune_notes.notes[i] == note) {
            for (int j = i; j < g_hds_fx_hardtune_notes.count - 1; j++) g_hds_fx_hardtune_notes.notes[j] = g_hds_fx_hardtune_notes.notes[j + 1];
            g_hds_fx_hardtune_notes.count--;
            break;
        }
    }
    g_hardtune_held_note = (g_hds_fx_hardtune_notes.count > 0) ? (float)g_hds_fx_hardtune_notes.notes[g_hds_fx_hardtune_notes.count - 1] : -1.0f;
}

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
        p->fx[s].input_from_slot = -1.0f; // not used -- see FXSlotParams' own comment
    }
    reset_fx_engine(p->fx_engine, FX_SLOT_COUNT);
    for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
        p->port_input_stereo[port] = 1.0f;  // default: read the port as real stereo, today's obvious behavior
        p->port_output_channel[port] = (float)port; // default: write back to the SAME port (always its own real stereo pair)
        p->port_input_off[port] = 0.0f;
        p->port_output_off[port] = 0.0f;
    }
}

// ---------------------------------------------------------------------------------------------
// State extension (DAW project save/reload recall) -- this plugin's very first release shipped
// with NO clap_plugin_state_t at all (a known, documented gap -- see the README's own "What this
// first pass deliberately leaves out"), which meant a whole patch silently vanished on every
// project reload: nothing here was ever a project's own concern, host-side, without one. Real bug,
// found the same way every other one in this file was -- a live DAW save/reload, not guessed.
//
// Same plain-text, versioned, name-free-but-field-ordered format clap_plugin.c's own
// clap_state_save/clap_state_load use (see that file's own header comment for the full "why text,
// why versioned" reasoning) -- one S line per pool slot's own numeric fields (sidechain_source on
// its OWN separate C line, not crammed onto S -- keeps S's own field count fixed regardless of
// what else a slot ever grows), one P line per port's own routing config, and MP/IP lines only
// when a slot's NAM path is actually set (a plain trailing %s can't safely hold an empty string,
// and most slots have no NAM path at all -- cheaper and just as simple to only ever emit the line
// when there's something to say). Output has no Mono/Stereo flag to save -- see
// port_output_channel's own comment for why there's no longer any such choice to persist:
//
//   HDSFX1                                          -- version line, first
//   S <slot> <fx_type> <p0>..<p9> <mix> <bypass> <fx_source> <chain_to_next>
//   C <slot> <sidechain_source>
//   P <port> <input_stereo> <output_channel> <input_off> <output_off>
//   MP <slot> <path>                                -- only when nam_model_path[0] != '\0'
//   IP <slot> <path>                                -- only when nam_ir_path[0] != '\0'
//
// MP/IP's own path field is the LAST thing on its line (%[^\n], reads to end-of-line) specifically
// so a path containing spaces round-trips correctly -- same trick clap_plugin.c's own Z-record
// file_path field already relies on, for the same reason.
// ---------------------------------------------------------------------------------------------

#define HDS_FX_STATE_VERSION 1

// Same small growable buffer clap_plugin.c's own HdsStateBuf is (this file is a separate
// translation unit, never linked with clap_plugin.c, so there's no existing symbol to reuse and no
// risk of colliding with one) -- see that file's own comment on why the whole payload is built/read
// in memory rather than streamed field-by-field against the CLAP stream API's own partial-write/
// partial-read contract.
typedef struct { char *buf; size_t len; size_t cap; } HdsFxStateBuf;

static void hds_fx_state_buf_reserve(HdsFxStateBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t new_cap = b->cap ? b->cap * 2 : 4096;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    b->buf = (char *)realloc(b->buf, new_cap);
    b->cap = new_cap;
}

static void hds_fx_state_buf_appendf(HdsFxStateBuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return; }
    hds_fx_state_buf_reserve(b, (size_t)needed);
    vsnprintf(b->buf + b->len, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)needed;
}

static bool hds_fx_state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    HdsFxStateBuf b = {0};
    hds_fx_state_buf_appendf(&b, "HDSFX%d\n", HDS_FX_STATE_VERSION);

    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        const FXSlotParams *fx = &p->fx[s];
        hds_fx_state_buf_appendf(&b, "S %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
            s, (double)fx->fx_type,
            (double)fx->param[0], (double)fx->param[1], (double)fx->param[2], (double)fx->param[3], (double)fx->param[4],
            (double)fx->param[5], (double)fx->param[6], (double)fx->param[7], (double)fx->param[8], (double)fx->param[9],
            (double)fx->mix, (double)fx->bypass, (double)fx->fx_source, (double)fx->chain_to_next);
        // sidechain_source on its own line kind (state_load()'s own "C" handler reads it) rather
        // than a 16th field crammed onto the S line above.
        hds_fx_state_buf_appendf(&b, "C %d %.9g\n", s, (double)fx->sidechain_source);
        // input_from_slot, on its own line kind too, purely additive (an older save simply has no F
        // line for a slot, which hds_fx_init_pool_defaults()'s own reset above already leaves at its
        // default of -1.0f/"not used" before any line gets applied) -- see FXSlotParams' own comment.
        hds_fx_state_buf_appendf(&b, "F %d %.9g\n", s, (double)fx->input_from_slot);
        if (fx->nam_model_path[0]) hds_fx_state_buf_appendf(&b, "MP %d %s\n", s, fx->nam_model_path);
        if (fx->nam_ir_path[0]) hds_fx_state_buf_appendf(&b, "IP %d %s\n", s, fx->nam_ir_path);
    }
    for (int port = 0; port < HDS_FX_PORT_COUNT; port++) {
        hds_fx_state_buf_appendf(&b, "P %d %.9g %.9g %.9g %.9g\n", port,
            (double)p->port_input_stereo[port], (double)p->port_output_channel[port],
            (double)p->port_input_off[port], (double)p->port_output_off[port]);
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

static bool hds_fx_state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;

    HdsFxStateBuf b = {0};
    char chunk[4096];
    for (;;) {
        int64_t n = stream->read(stream, chunk, sizeof(chunk));
        if (n < 0) { free(b.buf); return false; }
        if (n == 0) break;
        hds_fx_state_buf_reserve(&b, (size_t)n);
        memcpy(b.buf + b.len, chunk, (size_t)n);
        b.len += (size_t)n;
    }
    if (b.len == 0) { free(b.buf); return false; }
    hds_fx_state_buf_reserve(&b, 1); b.buf[b.len] = '\0';

    char *cursor = b.buf;
    char *first_newline = strchr(cursor, '\n');
    if (!first_newline) { free(b.buf); return false; }
    *first_newline = '\0';
    int version = 0;
    if (sscanf(cursor, "HDSFX%d", &version) != 1 || version < 1 || version > HDS_FX_STATE_VERSION) { free(b.buf); return false; }
    cursor = first_newline + 1;

    // A loaded project's own patch replaces whatever this instance currently has, same "load means
    // load, not merge" posture clap_state_load() takes for the synth's own Sample Bank zones --
    // resets every slot to Off/defaults (including freeing any live FXSlotEngine.state) before
    // applying what the save actually contains, so a field the save doesn't mention (e.g. loading
    // an older/shorter version) lands on a known default rather than whatever this instance
    // happened to have lying around already.
    hds_fx_init_pool_defaults(p);

    int applied = 0, skipped = 0;
    while (*cursor) {
        char *line_end = strchr(cursor, '\n');
        if (line_end) *line_end = '\0';

        if (cursor[0] == 'S' && cursor[1] == ' ') {
            int s;
            double fx_type, param[FX_SLOT_PARAM_COUNT], mix, bypass, fx_source, chain_to_next;
            int n = sscanf(cursor + 2, "%d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
                &s, &fx_type, &param[0], &param[1], &param[2], &param[3], &param[4],
                &param[5], &param[6], &param[7], &param[8], &param[9], &mix, &bypass, &fx_source, &chain_to_next);
            if (n == 16 && s >= 0 && s < FX_SLOT_COUNT) {
                FXSlotParams *fx = &p->fx[s];
                fx->fx_type = (float)fx_type;
                for (int i = 0; i < FX_SLOT_PARAM_COUNT; i++) fx->param[i] = (float)param[i];
                fx->mix = (float)mix; fx->bypass = (float)bypass;
                fx->fx_source = (float)fx_source; fx->chain_to_next = (float)chain_to_next;
                applied++;
            } else skipped++;
        } else if (cursor[0] == 'C' && cursor[1] == ' ') {
            // sidechain_source, on its own line kind rather than crammed onto the already-16-field
            // S line above -- see hds_fx_state_save()'s own comment for why that line stops short.
            int s; double sidechain_source;
            if (sscanf(cursor + 2, "%d %lf", &s, &sidechain_source) == 2 && s >= 0 && s < FX_SLOT_COUNT) {
                p->fx[s].sidechain_source = (float)sidechain_source; applied++;
            } else skipped++;
        } else if (cursor[0] == 'F' && cursor[1] == ' ') {
            int s; double input_from_slot;
            if (sscanf(cursor + 2, "%d %lf", &s, &input_from_slot) == 2 && s >= 0 && s < FX_SLOT_COUNT) {
                p->fx[s].input_from_slot = (float)input_from_slot; applied++;
            } else skipped++;
        } else if (cursor[0] == 'P' && cursor[1] == ' ') {
            int port; double input_stereo, output_channel, input_off, output_off;
            if (sscanf(cursor + 2, "%d %lf %lf %lf %lf", &port, &input_stereo, &output_channel, &input_off, &output_off) == 5
                && port >= 0 && port < HDS_FX_PORT_COUNT) {
                p->port_input_stereo[port] = (float)input_stereo;
                p->port_output_channel[port] = (float)output_channel;
                p->port_input_off[port] = (float)input_off;
                p->port_output_off[port] = (float)output_off;
                applied++;
            } else skipped++;
        } else if (cursor[0] == 'M' && cursor[1] == 'P' && cursor[2] == ' ') {
            int s; int chars = 0;
            if (sscanf(cursor + 3, "%d%n", &s, &chars) == 1 && s >= 0 && s < FX_SLOT_COUNT) {
                const char *path = cursor + 3 + chars; while (*path == ' ') path++;
                snprintf(p->fx[s].nam_model_path, SAMPLE_PATH_LEN, "%s", path);
                applied++;
            } else skipped++;
        } else if (cursor[0] == 'I' && cursor[1] == 'P' && cursor[2] == ' ') {
            int s; int chars = 0;
            if (sscanf(cursor + 3, "%d%n", &s, &chars) == 1 && s >= 0 && s < FX_SLOT_COUNT) {
                const char *path = cursor + 3 + chars; while (*path == ' ') path++;
                snprintf(p->fx[s].nam_ir_path, SAMPLE_PATH_LEN, "%s", path);
                applied++;
            } else skipped++;
        }
        cursor = line_end ? line_end + 1 : cursor + strlen(cursor);
    }

    fprintf(stderr, "[hds-fx] state load: %d field(s) applied, %d skipped\n", applied, skipped);
    free(b.buf);
    return true;
}

static const clap_plugin_state_t k_state_ext = {
    .save = hds_fx_state_save,
    .load = hds_fx_state_load,
};

static bool hds_fx_plugin_init(const clap_plugin_t *plugin) { (void)plugin; return true; }

static void hds_fx_plugin_destroy(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    // Defensive: a well-behaved host always calls clap_plugin_gui->destroy() before this, but
    // clean up an open X11 window/Display here too rather than leak it if one doesn't.
    hds_fx_gui_destroy_internal(plugin);
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
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                hds_fx_apply_param_event(p, (const clap_event_param_value_t *)hdr);
            } else if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_NOTE_OFF) {
                // FX_TYPE_HARDTUNE's own Target Source: MIDI -- see hds_fx_hardtune_note_on/off's
                // own comment. No Program/channel concept here to filter on, same "every note-on/
                // off updates this regardless of context" posture handle_note_on/handle_note_off
                // already use for g_hardtune_held_note in backend.c.
                const clap_event_note_t *ev = (const clap_event_note_t *)hdr;
                if (ev->key < 0 || ev->key > 127) continue; // wildcard note-on/off, not supported
                if (hdr->type == CLAP_EVENT_NOTE_ON) hds_fx_hardtune_note_on((int)ev->key);
                else hds_fx_hardtune_note_off((int)ev->key);
            } else if (hdr->type == CLAP_EVENT_MIDI) {
                const clap_event_midi_t *ev = (const clap_event_midi_t *)hdr;
                unsigned char status = ev->data[0] & 0xF0;
                // Same raw-byte decode convention backend.c's own dispatch_midi_event call sites
                // already use (see clap_plugin.c's own process()) -- a Note On with velocity 0
                // counts as Note Off.
                if (status == 0x90 || status == 0x80) {
                    bool is_note_on = (status == 0x90 && ev->data[2] > 0);
                    if (is_note_on) hds_fx_hardtune_note_on(ev->data[1]);
                    else hds_fx_hardtune_note_off(ev->data[1]);
                } else if (status == 0xB0 && ev->data[1] == 1) {
                    g_hardtune_modwheel = fmaxf(0.0f, fminf(1.0f, (float)ev->data[2] / 127.0f)); // CC1
                } else if (status == 0xE0) {
                    int bend14 = (int)ev->data[1] | ((int)ev->data[2] << 7);
                    float pitch_bend = fmaxf(-1.0f, fminf(1.0f, (float)(bend14 - 8192) / 8192.0f));
                    // Standard +/-2 semitone default (same default backend.c's own Program 0
                    // ships, see pb_range_up/pb_range_down's own init) -- this plugin has no
                    // Program/pb_range concept of its own to read a real range from.
                    g_hardtune_bend_semitones = pitch_bend * 2.0f;
                } else if (status == 0xD0) {
                    g_hardtune_channel_pressure = fmaxf(0.0f, fminf(1.0f, (float)ev->data[1] / 127.0f));
                }
            }
        }
    }

    // PRECONDITION for process_fx_chain() (see its own comment): a slot's active_type must be
    // synced to its current fx_type once per block, before any per-sample dispatch. One call now
    // (not once per port) -- there's only one pool.
    sync_fx_engine_types(p->fx, p->fx_engine, FX_SLOT_COUNT);
#if HDS_NAM_ENABLED
    // Same "once per block, right after sync_fx_engine_types" precondition as that call itself --
    // see poll_fx_nam_requests()'s own comment in fx_chain.c. This plugin has no separate
    // standalone-style main loop to poll from the way hds_standalone.c does, so process() itself is
    // the one place per block this can run -- the equivalent shared call for the synth's own side
    // lives in backend.c's render_audio_block instead.
    poll_fx_nam_requests(p->fx, p->fx_engine, FX_SLOT_COUNT);
#endif
    // gr_db_scratch zeroed once per block, then refreshed by whichever port's call owns each slot
    // this sample -- same convention as backend.c's own gr_db_out (see render_audio_block).
    for (int s = 0; s < FX_SLOT_COUNT; s++) p->gr_db_scratch[s] = 0.0f;
    for (int s = 0; s < FX_SLOT_COUNT; s++) p->dump_avail_scratch[s] = 0.0f;

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
            if (p->port_input_off[port] != 0.0f) {
                sc_snapshot_l[port] = 0.0f; // Off: this chain's Input reads silence, regardless of
                sc_snapshot_r[port] = 0.0f; // whatever port/stereo choice is otherwise remembered
                continue;
            }
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
            process_fx_chain(p->fx, p->fx_engine, FX_SLOT_COUNT, /* always Parallel */ 1, port, sc_snapshot_l, sc_snapshot_r, SOURCE_COUNT, &l, &r, p->gr_db_scratch, p->dump_avail_scratch, p->fx_slot_out_l, p->fx_slot_out_r);

            if (p->port_output_off[port] != 0.0f) continue; // Off: this chain's Output goes nowhere

            // Output routing is per-CHAIN (keyed by fx_source == port, same id process_fx_chain()
            // just bucketed slots by), not fixed to this same port -- see this file's own header
            // comment. A port that owns zero slots is a no-op passthrough from process_fx_chain()
            // itself (its own zero-slot-count guard), so this still correctly writes that port's
            // own dry signal out via ITS OWN Output selector (today's obvious default: right back
            // to the same port), rather than silently dropping unconfigured ports.
            int out_ch = (int)p->port_output_channel[port];
            if (out_ch < 0 || out_ch >= HDS_FX_PORT_COUNT) out_ch = port;
            // Always that port's own real stereo pair, l -> L, r -> R -- no Mono option here, see
            // port_output_channel's own comment for why.
            if (out_l[out_ch]) out_l[out_ch][i] += l;
            if (out_r[out_ch]) out_r[out_ch][i] += r;
        }
    }

    // GR meter ballistics -- see gr_db_smoothed's own comment. Same formula/time constants as
    // backend.c's own render_audio_block (~15ms attack, ~350ms release), just driven by this
    // call's own frames_count instead of block_size.
    {
        const float gr_attack_rate = 1.0f - expf(-(float)process->frames_count / (SAMPLE_RATE * 0.015f));
        const float gr_release_rate = 1.0f - expf(-(float)process->frames_count / (SAMPLE_RATE * 0.35f));
        for (int s = 0; s < FX_SLOT_COUNT; s++) {
            float rate = (p->gr_db_scratch[s] > p->gr_db_smoothed[s]) ? gr_attack_rate : gr_release_rate;
            p->gr_db_smoothed[s] += (p->gr_db_scratch[s] - p->gr_db_smoothed[s]) * rate;
        }
    }

    hds_fx_push_changed_params(p, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

static const void *hds_fx_plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &k_audio_ports_ext;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &k_note_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &k_params_ext;
    if (strcmp(id, CLAP_EXT_GUI) == 0) return &k_gui_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &k_state_ext;
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

    // Wire the HdsFxGuiHost view onto this instance's own real arrays -- see HdsFxGuiHost's own
    // header comment. gui_window starts NULL (calloc already guarantees this; explicit here only
    // for readability, matching how every other flag/pointer above is left to calloc's zero-init).
    p->gui_host.fx = p->fx;
    p->gui_host.slot_count = FX_SLOT_COUNT;
    p->gui_host.port_count = HDS_FX_PORT_COUNT;
    p->gui_host.port_input_stereo = p->port_input_stereo;
    p->gui_host.port_output_channel = p->port_output_channel;
    p->gui_host.port_input_off = p->port_input_off;
    p->gui_host.port_output_off = p->port_output_off;
    p->gui_host.fx_gr_db = p->gr_db_smoothed;
    p->gui_host.fx_dump_avail_sec = p->dump_avail_scratch;
    p->gui_host.gui_window = NULL;

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
