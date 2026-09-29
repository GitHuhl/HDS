// hds_clap_routing_draw.c: this native GUI's own Routing tab -- shared by hds.clap AND the
// standalone app (compiled into both TUs, like every other hds_clap_*_draw.c file), but its own
// output-routing section deliberately DIFFERS between the two hosts, gated by
// HDS_STANDALONE_GUI_ENABLED (same per-TU macro convention as hds_clap_devices_draw.c's own
// standalone-only sections): one row per real Source (the same 17, Program 1-8/Combi Mix/Ext In
// 1-8, frontend.py's own SOURCE_NAMES list the FX tab's Source combo already ports -- see
// hds_clap_source_label() in clap_plugin.c, reused here directly), each with, for the 8 Ext In rows
// only, Enable/Gain/Stereo controls and a live VU meter, PLUS:
//
//   - hds.clap: a single Main Out toggle (source_routing[src].enable[0]) only. SCOPE DECISION
//     (confirmed with the user when this file was first written): clap_plugin_process() always
//     calls render_audio_block() with output_channels hardcoded to 2 -- confirmed by reading that
//     call site -- so only enable[0] ever reaches this plugin's actual audio output; enable[1..7]
//     compute internally (the routing-matrix summing loop in render_audio_block() runs
//     unconditionally over the full OUTPUT_PAIR_MAX) but their summed result is discarded before
//     ever reaching out_buffer or any CLAP port. Showing the full 8-column grid here would ship 7
//     columns that visibly do nothing -- so only the one column that's actually audible is shown;
//     columns 1-7 are left exactly as whatever state they already hold (never zeroed, never touched
//     by this GUI), so a save file or a future architecture change that actually wires up more CLAP
//     outputs loses nothing.
//   - the standalone app: the REAL, full 8-column source-to-output-pair grid (one checkbox per
//     OUTPUT_PAIR_MAX column, exactly matching frontend.py's own Routing tab) -- its own
//     multichannel ALSA device genuinely makes every one of those 8 pairs audible (see
//     render_audio_block()'s own output_pairs loop, driven by the real detected_output_channels).
//     Not yet dynamically shown/hidden by the connected device's real channel count the way
//     frontend.py's own _rebuild_routing_output_columns() does -- routing to a pair the device
//     doesn't have is harmless (that same output_pairs clamp/zero-fill already discards it), so
//     showing all 8 unconditionally is a correct, if not yet capability-aware, port. Tracked as
//     real follow-up work, not attempted here.
//
// Also revives the ext-in VU meters, which are otherwise fully dead in hds.clap (backend.c's own
// gui_routing_tab_visible is never set from a CLAP instance's control surface, since it has no
// OSC-driven Routing tab of its own to report from) -- see HdsClapPlugin.ext_in_peak's own comment
// in clap_plugin.c and hds_clap_gui_render()'s own comment on setting gui_routing_tab_visible.

typedef struct {
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
} HdsClapRoutingViewState;

static void hds_clap_routing_view_state_init(HdsClapRoutingViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_ROUTING_MARGIN 10.0
#define HDS_ROUTING_HEADER_H 28.0
#define HDS_ROUTING_ROW_H 38.0
#define HDS_ROUTING_KNOB_R 10.0

// Standalone only: the real thing frontend.py's own Routing tab has always been (an 8-column,
// many-to-many source-to-output-pair grid, one checkbox per OUTPUT_PAIR_MAX column -- see
// SynthSharedControls.source_routing/OUTPUT_PAIR_MAX in backend.c) -- the standalone's own
// multichannel ALSA device genuinely makes every one of the 8 pairs audible (render_audio_block's
// own output_pairs = output_channels/2 loop), unlike hds.clap's hardcoded 2-channel process(). Not
// dynamically shown/hidden by the currently-connected device's real channel count the way
// frontend.py's own _rebuild_routing_output_columns() does (that needs a live detected-channel-count
// field threaded into this draw file, real follow-up work) -- routing to a pair the connected
// device doesn't actually have is harmless (render_audio_block's own output_pairs clamp/zero-fill
// already discards it), so showing all 8 unconditionally is a correct, if not yet
// capability-aware, port.
#define HDS_ROUTING_OUT_PILL_W 26.0
#define HDS_ROUTING_OUT_PILL_STRIDE 28.0

typedef struct {
    HdsFxRect name_area, enable_pill, gain_knob, stereo_pill, vu_bar;
#ifdef HDS_STANDALONE_GUI_ENABLED
    HdsFxRect out_pill[OUTPUT_PAIR_MAX];
#else
    HdsFxRect mainout_pill; // CLAP: only enable[0] is ever audible -- see this file's own header comment
#endif
} HdsRoutingRowLayout;

static void hds_clap_routing_compute_row_layout(int src, HdsRoutingRowLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = HDS_ROUTING_HEADER_H + src * HDS_ROUTING_ROW_H;
    out->name_area = (HdsFxRect){HDS_ROUTING_MARGIN, y, 110.0, HDS_ROUTING_ROW_H};
    out->enable_pill = (HdsFxRect){126.0, y + 8.0, 46.0, 18.0};
    out->gain_knob = (HdsFxRect){195.0, y + HDS_ROUTING_KNOB_R + 6.0, HDS_ROUTING_KNOB_R, HDS_ROUTING_KNOB_R};
    out->stereo_pill = (HdsFxRect){225.0, y + 8.0, 54.0, 18.0};
#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
        out->out_pill[p] = (HdsFxRect){290.0 + p * HDS_ROUTING_OUT_PILL_STRIDE, y + 8.0, HDS_ROUTING_OUT_PILL_W, 18.0};
    }
    out->vu_bar = (HdsFxRect){290.0 + OUTPUT_PAIR_MAX * HDS_ROUTING_OUT_PILL_STRIDE + 14.0, y + 11.0, 160.0, 12.0};
#else
    out->mainout_pill = (HdsFxRect){290.0, y + 8.0, 80.0, 18.0};
    out->vu_bar = (HdsFxRect){380.0, y + 11.0, 160.0, 12.0};
#endif
}

static bool hds_routing_is_ext_in(int src) { return src >= COMBI_PROGRAM_COUNT + 1; }
static int hds_routing_ext_in_pair(int src) { return src - COMBI_PROGRAM_COUNT - 1; }

static void hds_clap_routing_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRoutingViewState *vs, double window_w, double window_h) {
    (void)vs;
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.0);
    hds_fx_rgb(cr, 0.5, 0.5, 0.53);
    hds_fx_left_text(cr, HDS_ROUTING_MARGIN, HDS_ROUTING_HEADER_H - 10.0, "Source");
    hds_fx_left_text(cr, 126.0, HDS_ROUTING_HEADER_H - 10.0, "Enable");
    hds_fx_left_text(cr, 190.0, HDS_ROUTING_HEADER_H - 10.0, "Gain");
    hds_fx_left_text(cr, 225.0, HDS_ROUTING_HEADER_H - 10.0, "Stereo");
#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
        char hdr[8]; snprintf(hdr, sizeof(hdr), "%d", p + 1);
        hds_fx_centered_text(cr, 290.0 + p * HDS_ROUTING_OUT_PILL_STRIDE + HDS_ROUTING_OUT_PILL_W / 2.0, HDS_ROUTING_HEADER_H - 10.0, hdr);
    }
    hds_fx_left_text(cr, 290.0 + OUTPUT_PAIR_MAX * HDS_ROUTING_OUT_PILL_STRIDE + 14.0, HDS_ROUTING_HEADER_H - 10.0, "VU (Ext In)");
#else
    hds_fx_left_text(cr, 290.0, HDS_ROUTING_HEADER_H - 10.0, "Main Out");
    hds_fx_left_text(cr, 380.0, HDS_ROUTING_HEADER_H - 10.0, "VU (Ext In)");
#endif

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    for (int src = 0; src < SOURCE_COUNT; src++) {
        HdsRoutingRowLayout L;
        hds_clap_routing_compute_row_layout(src, &L);
        bool ext_in = hds_routing_is_ext_in(src);

        char name[16]; hds_clap_source_label(src, name, sizeof(name));
        hds_fx_rgb(cr, 0.75, 0.75, 0.78);
        hds_fx_left_text(cr, L.name_area.x, L.name_area.y + HDS_ROUTING_ROW_H / 2 + 3.0, name);

#ifdef HDS_STANDALONE_GUI_ENABLED
        for (int op = 0; op < OUTPUT_PAIR_MAX; op++) {
            bool on = p->knobs.source_routing[src].enable[op] != 0.0f;
            hds_fx_draw_pill(cr, L.out_pill[op].x, L.out_pill[op].y, L.out_pill[op].w, L.out_pill[op].h, on ? "\xe2\x9c\x93" : "", on, false);
        }
#else
        bool main_out = p->knobs.source_routing[src].enable[0] != 0.0f;
        hds_fx_draw_pill(cr, L.mainout_pill.x, L.mainout_pill.y, L.mainout_pill.w, L.mainout_pill.h, main_out ? "On" : "Off", main_out, false);
#endif

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

#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int op = 0; op < OUTPUT_PAIR_MAX; op++) {
        if (hds_play_rect_contains(&L.out_pill[op], x, y)) {
            if (button == 1) {
                float *e = &p->knobs.source_routing[src].enable[op];
                *e = (*e != 0.0f) ? 0.0f : 1.0f;
            }
            return true;
        }
    }
#else
    if (hds_play_rect_contains(&L.mainout_pill, x, y)) {
        if (button == 1) {
            float *e = &p->knobs.source_routing[src].enable[0];
            *e = (*e != 0.0f) ? 0.0f : 1.0f;
        }
        return true;
    }
#endif
    if (hds_routing_is_ext_in(src)) {
        int pair = hds_routing_ext_in_pair(src);
        if (hds_play_rect_contains(&L.enable_pill, x, y)) {
            if (button == 1) p->knobs.ext_in_enable[pair] = (p->knobs.ext_in_enable[pair] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_knob_hit_test(&L.gain_knob, x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.ext_in_gain[pair], 0.0, 2.0, x, y, button, HDS_PLAY_NO_MENU);
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
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

static void hds_clap_routing_on_button_release(HdsClapRoutingViewState *vs) { hds_play_release_knob_drag(&vs->knob_drag, NULL, NULL); }
