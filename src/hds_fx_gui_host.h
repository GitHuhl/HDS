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

typedef struct {
    FXSlotParams *fx;               // [FX_SLOT_COUNT], real, mutable -- never NULL
    float *port_input_stereo;       // [N ports], or NULL -- see this file's own header comment
    float *port_output_channel;     // [N ports], or NULL
    float *port_input_off;          // [N ports], or NULL
    float *port_output_off;         // [N ports], or NULL
    void *gui_window;               // opaque HdsFxGuiWindow*, owned by hds_fx_gui_x11.c -- NULL until gui->create()
} HdsFxGuiHost;
