// hds_clap_routing_draw.c: hds.clap's own Routing tab -- one row per real Source (the same 17,
// Program 1-8/Combi Mix/Ext In 1-8, frontend.py's own SOURCE_NAMES list the FX tab's Source combo
// already ports -- see hds_clap_source_label() in clap_plugin.c, reused here directly), each with
// a Main Out toggle (source_routing[src].enable[0]) plus, for the 8 Ext In rows only, Enable/Gain/
// Stereo controls and a live VU meter.
//
// SCOPE DECISION (confirmed with the user before writing this file -- see the session's own
// research on this): frontend.py's own Routing tab is an 8-column matrix (source_routing[src].
// enable[0..7], one column per hardware output pair). Inside hds.clap specifically,
// clap_plugin_process() always calls render_audio_block() with output_channels hardcoded to 2 --
// confirmed by reading that call site -- so only enable[0] ("Out 1"/Main Out) ever reaches this
// plugin's actual audio output; enable[1..7] compute internally (the routing-matrix summing loop
// in render_audio_block() runs unconditionally over the full OUTPUT_PAIR_MAX) but their summed
// result is discarded before ever reaching out_buffer or any CLAP port. Porting the full 8-column
// grid would ship 7 columns that visibly do nothing in this host. The user chose: show only the
// one column that's actually audible (Main Out / enable[0]) -- columns 1-7 are left exactly as
// whatever state they already hold (never zeroed, never touched by this GUI), so a save file or a
// future architecture change that actually wires up more outputs loses nothing.
//
// Also revives the ext-in VU meters, which are otherwise fully dead in hds.clap (backend.c's own
// gui_routing_tab_visible is never set from a CLAP instance's control surface, since it has no
// OSC-driven Routing tab of its own to report from) -- see HdsClapPlugin.ext_in_peak's own comment
// in clap_plugin.c and hds_clap_gui_render()'s own comment on setting gui_routing_tab_visible.

typedef struct {
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
} HdsClapRoutingViewState;

static void hds_clap_routing_view_state_init(HdsClapRoutingViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_ROUTING_MARGIN 8.0
#define HDS_ROUTING_HEADER_H 24.0
#define HDS_ROUTING_ROW_H 34.0
#define HDS_ROUTING_KNOB_R 9.0

typedef struct {
    HdsFxRect name_area, enable_pill, gain_knob, stereo_pill, mainout_pill, vu_bar;
} HdsRoutingRowLayout;

static void hds_clap_routing_compute_row_layout(int src, HdsRoutingRowLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = HDS_ROUTING_HEADER_H + src * HDS_ROUTING_ROW_H;
    out->name_area = (HdsFxRect){HDS_ROUTING_MARGIN, y, 110.0, HDS_ROUTING_ROW_H};
    out->enable_pill = (HdsFxRect){126.0, y + 8.0, 46.0, 18.0};
    out->gain_knob = (HdsFxRect){195.0, y + HDS_ROUTING_KNOB_R + 6.0, HDS_ROUTING_KNOB_R, HDS_ROUTING_KNOB_R};
    out->stereo_pill = (HdsFxRect){225.0, y + 8.0, 54.0, 18.0};
    out->mainout_pill = (HdsFxRect){290.0, y + 8.0, 80.0, 18.0};
    out->vu_bar = (HdsFxRect){380.0, y + 11.0, 160.0, 12.0};
}

static bool hds_routing_is_ext_in(int src) { return src >= COMBI_PROGRAM_COUNT + 1; }
static int hds_routing_ext_in_pair(int src) { return src - COMBI_PROGRAM_COUNT - 1; }

static void hds_clap_routing_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRoutingViewState *vs, double window_w, double window_h) {
    (void)vs;
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 8.0);
    hds_fx_rgb(cr, 0.5, 0.5, 0.53);
    hds_fx_left_text(cr, HDS_ROUTING_MARGIN, HDS_ROUTING_HEADER_H - 10.0, "Source");
    hds_fx_left_text(cr, 126.0, HDS_ROUTING_HEADER_H - 10.0, "Enable");
    hds_fx_left_text(cr, 190.0, HDS_ROUTING_HEADER_H - 10.0, "Gain");
    hds_fx_left_text(cr, 225.0, HDS_ROUTING_HEADER_H - 10.0, "Stereo");
    hds_fx_left_text(cr, 290.0, HDS_ROUTING_HEADER_H - 10.0, "Main Out");
    hds_fx_left_text(cr, 380.0, HDS_ROUTING_HEADER_H - 10.0, "VU (Ext In)");

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.0);
    for (int src = 0; src < SOURCE_COUNT; src++) {
        HdsRoutingRowLayout L;
        hds_clap_routing_compute_row_layout(src, &L);
        bool ext_in = hds_routing_is_ext_in(src);

        char name[16]; hds_clap_source_label(src, name, sizeof(name));
        hds_fx_rgb(cr, 0.75, 0.75, 0.78);
        hds_fx_left_text(cr, L.name_area.x, L.name_area.y + HDS_ROUTING_ROW_H / 2 + 3.0, name);

        bool main_out = p->knobs.source_routing[src].enable[0] != 0.0f;
        hds_fx_draw_pill(cr, L.mainout_pill.x, L.mainout_pill.y, L.mainout_pill.w, L.mainout_pill.h, main_out ? "On" : "Off", main_out, false);

        if (!ext_in) continue;
        int pair = hds_routing_ext_in_pair(src);
        bool enabled = p->knobs.ext_in_enable[pair] != 0.0f;
        hds_fx_draw_pill(cr, L.enable_pill.x, L.enable_pill.y, L.enable_pill.w, L.enable_pill.h, enabled ? "On" : "Off", enabled, false);

        float gain = p->knobs.ext_in_gain[pair];
        char gtxt[16]; snprintf(gtxt, sizeof(gtxt), "%d%%", (int)(gain * 100.0f + 0.5f));
        hds_play_draw_knob(cr, L.gain_knob.x, L.gain_knob.y, L.gain_knob.w, gain / 2.0f, false, NULL, gtxt);

        bool stereo = p->knobs.ext_in_stereo[pair] != 0.0f;
        hds_fx_draw_pill(cr, L.stereo_pill.x, L.stereo_pill.y, L.stereo_pill.w, L.stereo_pill.h, stereo ? "Stereo" : "Mono", stereo, false);

        float peak = p->ext_in_peak[pair];
        if (peak < 0.0f) peak = 0.0f; else if (peak > 1.0f) peak = 1.0f;
        hds_fx_rgb(cr, 0.16, 0.16, 0.19);
        cairo_new_path(cr); cairo_rectangle(cr, L.vu_bar.x, L.vu_bar.y, L.vu_bar.w, L.vu_bar.h); cairo_fill(cr);
        if (peak > 0.001f) {
            double fill_w = L.vu_bar.w * peak;
            if (peak < 0.7) hds_fx_rgb(cr, 0.25, 0.75, 0.35);
            else if (peak < 0.92) hds_fx_rgb(cr, 0.85, 0.75, 0.20);
            else hds_fx_rgb(cr, 0.85, 0.25, 0.25);
            cairo_new_path(cr); cairo_rectangle(cr, L.vu_bar.x, L.vu_bar.y, fill_w, L.vu_bar.h); cairo_fill(cr);
        }
    }
}

static bool hds_clap_routing_on_button_press(HdsClapPlugin *p, HdsClapRoutingViewState *vs, double x, double y, int button) {
    if (y < HDS_ROUTING_HEADER_H) return false;
    int src = (int)((y - HDS_ROUTING_HEADER_H) / HDS_ROUTING_ROW_H);
    if (src < 0 || src >= SOURCE_COUNT) return false;

    HdsRoutingRowLayout L;
    hds_clap_routing_compute_row_layout(src, &L);

    if (hds_play_rect_contains(&L.mainout_pill, x, y)) {
        if (button == 1) {
            float *e = &p->knobs.source_routing[src].enable[0];
            *e = (*e != 0.0f) ? 0.0f : 1.0f;
        }
        return true;
    }
    if (hds_routing_is_ext_in(src)) {
        int pair = hds_routing_ext_in_pair(src);
        if (hds_play_rect_contains(&L.enable_pill, x, y)) {
            if (button == 1) p->knobs.ext_in_enable[pair] = (p->knobs.ext_in_enable[pair] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_knob_hit_test(&L.gain_knob, x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.ext_in_gain[pair], 0.0, 2.0, y);
            return true;
        }
        if (hds_play_rect_contains(&L.stereo_pill, x, y)) {
            if (button == 1) p->knobs.ext_in_stereo[pair] = (p->knobs.ext_in_stereo[pair] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
    }

    (void)button;
    return true;
}

static void hds_clap_routing_on_motion(HdsClapRoutingViewState *vs, double x, double y) {
    (void)x;
    if (!vs->knob_drag.target) return;
    double delta = vs->knob_drag.start_y - y;
    double v = vs->knob_drag.start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (vs->knob_drag.hi - vs->knob_drag.lo);
    if (v < vs->knob_drag.lo) v = vs->knob_drag.lo; else if (v > vs->knob_drag.hi) v = vs->knob_drag.hi;
    *vs->knob_drag.target = (float)v;
}

static void hds_clap_routing_on_button_release(HdsClapRoutingViewState *vs) { vs->knob_drag.target = NULL; }
