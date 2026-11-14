// HdsFxGuiHost: the plugin-agnostic view hds_fx_gui_draw.c/hds_fx_gui_x11.c actually operate on,
// instead of a concrete host's own top-level plugin struct (HdsFxPlugin, or hds.clap's own plugin
// struct). Every field is a raw pointer INTO whatever real array the concrete host struct owns --
// never a copy, so every read or write through it is real, live plugin state -- the same "FX_SLOT_
// COUNT pool, shared verbatim" idea fx_types.h/fx_chain.c already apply to the DSP side, applied
// here to the GUI side: one engine, reused by every real host with such a pool, instead of one
// engine duplicated per host.
//
// A host with no per-port routing concept at all (hds.clap's own Input header is a Source combo --
// Program/Combi Mix/Ext In -- with nothing analogous to a flat N-port Mono/Stereo/Off choice)
// simply leaves those four pointers NULL; hds_fx_gui_draw.c's own per-host Input/Output header
// rendering is gated on which of these are non-NULL, never assumed present.
//
// Needs only FXSlotParams (fx_types.h) -- deliberately kept free of any Cairo/Xlib/CLAP type, same
// discipline hds_fx_gui_draw.c's own header comment already holds itself to, so this header can be
// included wherever a concrete host struct is defined, with nothing else dragged in.
#pragma once

#include <stddef.h>

#include "fx_types.h"

typedef struct {
    FXSlotParams *fx;               // [slot_count], real, mutable -- never NULL
    int slot_count;                 // how many slots fx[] (and fx_gr_db/fx_dump_avail_sec) hold
    int port_count;                 // port-model hosts: number of stereo I/O ports (also sizes the
                                    // port_* arrays below); Source-combo hosts: their SOURCE_COUNT
    float *port_input_stereo;       // [N ports], or NULL -- see this file's own header comment
    float *port_output_channel;     // [N ports], or NULL
    float *port_input_off;          // [N ports], or NULL
    float *port_output_off;         // [N ports], or NULL
    void *gui_window;               // opaque HdsFxGuiWindow*, owned by hds_fx_gui_x11.c -- NULL until gui->create()

    // The OTHER shape an Input header can take: a host with no flat N-port routing concept at all
    // (hds.clap) instead has a Source combo -- Program N/Combi Mix/Ext In N -- reusing the SAME
    // fx_source field a port-model host uses for its own port index (see fx_types.h's own comment
    // on fx_source), just interpreted as a source-bus index instead. source_count == 0 means this
    // host has neither concept (matches the port_input_stereo == NULL fallback -- draws the inert
    // "--" placeholder); a host that sets source_count > 0 must also set source_label.
    int source_count;
    void (*source_label)(int idx, char *buf, size_t buflen); // pure function of idx, no host state needed

    // [FX_SLOT_COUNT], or NULL -- each slot's most recent gain-reduction reading in dB (0 = no
    // reduction), refreshed every process()/audio-callback by whichever host owns this pool (see
    // clap_plugin.c's/hds_standalone.c's/hds_fx_plugin.c's own persistent fx_gr_db array, fed from
    // process_fx_chain()'s own gr_db_out out-param -- fx_chain.c's own comment on gr_db_out). Only
    // meaningful for the 7 GR-capable FX types (Gate/Limiter/FetComp/FairKid67/PeakMax/OptoComp/
    // DeEsser -- see fx_chain.c's own process_fx_chain() switch); hds_fx_gui_draw.c only ever reads
    // this, never writes it, same "raw pointer into real host state" convention as every other
    // field here. NULL means this host doesn't have live GR data (there is currently no such host,
    // but the same defensive NULL-check convention as source_count/port_input_stereo etc. applies).
    float *fx_gr_db;

    // Same convention as fx_gr_db just above, for FX_TYPE_BROADCAST_DELAY's own per-slot dump-
    // availability meter: each slot's currently-buffered delay, in seconds (0 for every other
    // type). See process_fx_chain()'s own dump_avail_out out-param in fx_chain.c.
    float *fx_dump_avail_sec;

    // A Source-combo host's (source_count > 0) own equivalent of a port-model host's Output picker
    // (port_output_channel above): a port-model host already has one CLAP port per port index, so
    // "where does this port's FX result go" is a single index into that same port space -- but a
    // Source-combo host's "source" (Program/Combi Mix/Ext In/Drum Bus) is a semantic bus, not a
    // physical output, and where a SOURCE's post-FX audio actually reaches hardware is a genuinely
    // many-to-many question (SynthSharedControls.source_routing's own comment in backend.c) the
    // standalone build's dedicated Routing tab already answers in full. This pointer/count pair (or
    // NULL/0, for hds_fx_plugin.c, which is a port-model host and has no use for this) lets the FX
    // tab surface and SET that same real routing data directly -- source_routing_flat points at
    // SOURCE_COUNT*output_pair_count contiguous floats (SynthSharedControls.source_routing is a
    // flat array of `float enable[output_pair_count]` structs, so &source_routing[0].enable[0]
    // already IS that contiguous layout; no copy, no separate model). Indexed here as
    // source_routing_flat[source * output_pair_count + pair]. Picking a single pair from the FX
    // tab's own Out pill (see hds_fx_gui_draw.c's own HDS_FX_DROPDOWN_OUT_PAIR) sets that source's
    // routing to EXACTLY that one pair, clearing any others -- the same single-target semantics a
    // port-model host's own port_output_channel already has; a user who wants true many-to-many
    // routing for a source still has the Routing tab, untouched by this pill unless they click it.
    float *source_routing_flat;
    int output_pair_count;
} HdsFxGuiHost;
