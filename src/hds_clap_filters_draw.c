// hds_clap_filters_draw.c: hds.clap's own Filters tab -- the Chain Mode toggle, 5 flat
// interchangeable filter slots (Freq/Res/Slope/Type), and the post-filter 3-band EQ (Low Shelf/
// Mid Peak/High Shelf), matching frontend.py's own Filters tab layout (chain_mode_row + 5 stacked
// "Filter N" group boxes + a "3-Band EQ (post-filter)" group box of 3 side-by-side bands, all in
// one vertical column). Platform-agnostic like every other tab file (plain cairo_t*, no Xlib) --
// #include'd by clap_plugin.c after hds_clap_play_draw.c, whose generic knob primitives
// (HdsPlayKnobDrag/hds_play_draw_knob/hds_play_knob_hit_test/hds_play_start_knob_drag) this file
// reuses directly rather than duplicating -- the exact same "generic primitive, reused verbatim by
// a later tab file" pattern hds_clap_play_draw.c itself already used for hds_fx_gui_draw.c's own
// hds_fx_draw_knob_arc()/hds_fx_rgb()/hds_fx_draw_pill().
//
// SCOPE for this pass: every control visible WITHOUT a right-click in frontend.py. Deliberately
// NOT here yet, for the same reason Play tab's own "Modulate From" menu isn't (see that file's own
// header comment -- it needs a real list of modulator sources, and the LFOs/Envelopes tabs that
// would supply that list don't exist in this native GUI yet either): each Cutoff knob's own right-
// click menu (Key Track preset + ext-MIDI CC source + "Modulate From"), and each Resonance knob's
// own right-click "Modulate From" menu. The two Filter/Resonance right-click-only fields
// (FilterParams.key_track/cutoff_mod_source_cc) keep whatever value they already have (0/-1 by
// default -- see init_default_controls()) since nothing in this pass's UI ever touches them; a
// user who set them via the Python GUI and then opens this one won't have them reset. EQ knobs
// have no right-click menu in frontend.py at all (a post-filter EQ band's own 2-3 knobs are
// constant for the whole block, never a mod-matrix destination -- see EqBandParams' own comment).

#define HDS_FILT_DROPDOWN_NONE 0
#define HDS_FILT_DROPDOWN_TYPE 1

#define HDS_FILT_TYPE_COUNT 5
static const char *const HDS_FILT_TYPE_NAMES[HDS_FILT_TYPE_COUNT] = {"Off", "LPF", "HPF", "BPF", "Notch"};

typedef struct {
    int dropdown_kind;   // HDS_FILT_DROPDOWN_* -- only Type ever opens a real option list (5 items);
                          // Chain Mode and Slope are plain binary click-to-toggle pills instead (see
                          // this file's own header comment on why a 2-option control doesn't need one)
    int dropdown_index;  // which filter slot (0-4) the open Type dropdown belongs to
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag; // reused verbatim from hds_clap_play_draw.c -- see this file's own header comment
    HdsCtxMenu ctx_menu;       // Cutoff's/Resonance's own "Modulate From" menu, if one is open
} HdsClapFiltersViewState;

static void hds_clap_filters_view_state_init(HdsClapFiltersViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->dropdown_kind = HDS_FILT_DROPDOWN_NONE;
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------
#define HDS_FILT_MARGIN 10.0
#define HDS_FILT_CONTENT_TOP 34.0
#define HDS_FILT_ROW_H 58.0
#define HDS_FILT_KNOB_R 10.0

typedef struct {
    HdsFxRect chain_mode_pill;
} HdsFiltChainModeLayout;

static void hds_clap_filters_compute_chain_mode_layout(HdsFiltChainModeLayout *out) {
    out->chain_mode_pill = (HdsFxRect){HDS_FILT_MARGIN + 90.0, HDS_FILT_CONTENT_TOP, 150.0, 18.0};
}

typedef struct {
    HdsFxRect freq_knob, res_knob, slope_pill, type_pill;
} HdsFiltRowLayout;

static void hds_clap_filters_compute_row_layout(int slot, double top_y, HdsFiltRowLayout *out) {
    double y = top_y + slot * HDS_FILT_ROW_H;
    double x = HDS_FILT_MARGIN + 70.0; // room for the "Filter N" label to its left
    out->freq_knob = (HdsFxRect){x + 18.0, y + HDS_FILT_KNOB_R + 9.0, HDS_FILT_KNOB_R, HDS_FILT_KNOB_R};
    out->res_knob = (HdsFxRect){x + 68.0, y + HDS_FILT_KNOB_R + 9.0, HDS_FILT_KNOB_R, HDS_FILT_KNOB_R};
    out->slope_pill = (HdsFxRect){x + 100.0, y + 14.0, 88.0, 18.0};
    out->type_pill = (HdsFxRect){x + 196.0, y + 14.0, 64.0, 18.0};
}

typedef struct {
    HdsFxRect freq_knob, gain_knob, q_knob; // q_knob is empty ({0,0,0,0}) for Low/High Shelf (band != 1)
} HdsEqBandLayout;

#define HDS_FILT_EQ_BAND_W 190.0

static void hds_clap_filters_compute_eq_layout(int band, double col_x, double top_y, HdsEqBandLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = top_y + 26.0; // room for the band's own name label above (bumped alongside this file's own font-size increase)
    out->freq_knob = (HdsFxRect){col_x + 26.0, y + HDS_FILT_KNOB_R + 9.0, HDS_FILT_KNOB_R, HDS_FILT_KNOB_R};
    out->gain_knob = (HdsFxRect){col_x + 78.0, y + HDS_FILT_KNOB_R + 9.0, HDS_FILT_KNOB_R, HDS_FILT_KNOB_R};
    if (band == 1) out->q_knob = (HdsFxRect){col_x + 130.0, y + HDS_FILT_KNOB_R + 9.0, HDS_FILT_KNOB_R, HDS_FILT_KNOB_R};
}

static double hds_clap_filters_eq_top_y(void) { return HDS_FILT_CONTENT_TOP + 30.0 + 5.0 * HDS_FILT_ROW_H + 20.0; }

// ---------------------------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------------------------
static void hds_clap_filters_render_dropdown(cairo_t *cr, HdsClapFiltersViewState *vs, double window_h, int current) {
    if (vs->dropdown_kind == HDS_FILT_DROPDOWN_NONE) return;
    int count = HDS_FILT_TYPE_COUNT;
    double list_h = count * 16.0;
    double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    HdsFxRect bg = {vs->dropdown_anchor.x, y0, 64.0, list_h};
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    for (int i = 0; i < count; i++) {
        HdsFxRect r = {bg.x, bg.y + i * 16.0, bg.w, 16.0};
        if (i == current) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
        }
        hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
        hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, HDS_FILT_TYPE_NAMES[i]);
    }
}

static void hds_clap_filters_render(cairo_t *cr, HdsClapPlugin *p, HdsClapFiltersViewState *vs, double window_w, double window_h) {
    (void)window_w;
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    ProgramState *prog = hds_play_program(p);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_FILT_MARGIN, HDS_FILT_CONTENT_TOP + 12.0, "Chain Mode:");

    HdsFiltChainModeLayout cm;
    hds_clap_filters_compute_chain_mode_layout(&cm);
    bool parallel = prog->filter_chain_mode != 0.0f;
    hds_fx_draw_pill(cr, cm.chain_mode_pill.x, cm.chain_mode_pill.y, cm.chain_mode_pill.w, cm.chain_mode_pill.h,
                      parallel ? "Parallel (Summed)" : "Series (1->2->3->4->5)", parallel, false);

    double rows_top = HDS_FILT_CONTENT_TOP + 30.0;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    for (int i = 0; i < 5; i++) {
        FilterParams *filt = &prog->filters[i];
        HdsFiltRowLayout L;
        hds_clap_filters_compute_row_layout(i, rows_top, &L);
        double row_y = rows_top + i * HDS_FILT_ROW_H;

        hds_fx_rgb(cr, 0.55, 0.55, 0.58);
        char lbl[12]; snprintf(lbl, sizeof(lbl), "Filter %d", i + 1);
        hds_fx_left_text(cr, HDS_FILT_MARGIN, row_y + HDS_FILT_ROW_H / 2 + 3.0, lbl);

        char freq_txt[16];
        if (filt->cutoff >= 1000.0f) snprintf(freq_txt, sizeof(freq_txt), "%.1fk", filt->cutoff / 1000.0f);
        else snprintf(freq_txt, sizeof(freq_txt), "%dHz", (int)filt->cutoff);
        float freq_frac = (float)((filt->cutoff - 40.0) / (18000.0 - 40.0));
        hds_play_draw_knob(cr, L.freq_knob.x, L.freq_knob.y, L.freq_knob.w, freq_frac, false, "Freq", freq_txt);

        char res_txt[16]; snprintf(res_txt, sizeof(res_txt), "%d%%", (int)(filt->res_q + 0.5f));
        hds_play_draw_knob(cr, L.res_knob.x, L.res_knob.y, L.res_knob.w, filt->res_q / 100.0f, false, "Res", res_txt);

        bool four_pole = filt->slope_poles != 0.0f;
        hds_fx_draw_pill(cr, L.slope_pill.x, L.slope_pill.y, L.slope_pill.w, L.slope_pill.h, four_pole ? "4-Pole" : "2-Pole", four_pole, false);

        int ftype = (int)filt->filter_type;
        if (ftype < 0 || ftype >= HDS_FILT_TYPE_COUNT) ftype = 0;
        bool type_dd_open = (vs->dropdown_kind == HDS_FILT_DROPDOWN_TYPE && vs->dropdown_index == i);
        hds_fx_draw_pill(cr, L.type_pill.x, L.type_pill.y, L.type_pill.w, L.type_pill.h, HDS_FILT_TYPE_NAMES[ftype], type_dd_open || ftype != 0, false);
    }

    double eq_top = hds_clap_filters_eq_top_y();
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_FILT_MARGIN, eq_top + 2.0, "3-Band EQ (post-filter)");

    static const char *const band_names[3] = {"Low Shelf", "Mid Peak", "High Shelf"};
    for (int b = 0; b < 3; b++) {
        EqBandParams *band = &prog->eq_bands[b];
        double col_x = HDS_FILT_MARGIN + b * HDS_FILT_EQ_BAND_W;
        HdsEqBandLayout L;
        hds_clap_filters_compute_eq_layout(b, col_x, eq_top, &L);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5);
        hds_fx_rgb(cr, 0.55, 0.55, 0.58);
        hds_fx_left_text(cr, col_x, eq_top + 16.0, band_names[b]);

        char freq_txt[16];
        if (band->freq >= 1000.0f) snprintf(freq_txt, sizeof(freq_txt), "%.1fk", band->freq / 1000.0f);
        else snprintf(freq_txt, sizeof(freq_txt), "%dHz", (int)band->freq);
        float freq_frac = (float)((band->freq - 20.0) / (20000.0 - 20.0));
        hds_play_draw_knob(cr, L.freq_knob.x, L.freq_knob.y, L.freq_knob.w, freq_frac, false, "Freq", freq_txt);

        char gain_txt[16]; snprintf(gain_txt, sizeof(gain_txt), "%+ddB", (int)(band->gain_db + (band->gain_db >= 0 ? 0.5f : -0.5f)));
        float gain_frac = (float)((band->gain_db + 18.0) / 36.0);
        hds_play_draw_knob(cr, L.gain_knob.x, L.gain_knob.y, L.gain_knob.w, gain_frac, false, "Gain", gain_txt);

        if (b == 1) {
            char q_txt[16]; snprintf(q_txt, sizeof(q_txt), "%.1f", band->q);
            float q_frac = (float)((band->q - 0.1) / (10.0 - 0.1));
            hds_play_draw_knob(cr, L.q_knob.x, L.q_knob.y, L.q_knob.w, q_frac, false, "Q", q_txt);
        }
    }

    if (vs->dropdown_kind == HDS_FILT_DROPDOWN_TYPE) {
        hds_clap_filters_render_dropdown(cr, vs, window_h, (int)prog->filters[vs->dropdown_index].filter_type);
    }

    if (vs->ctx_menu.open && vs->ctx_menu.kind == HDS_CTXMENU_KIND_MODULATE_FROM) {
        HdsCtxMenuRow rows[HDS_CTXMENU_MAX_ROWS]; int count = 0;
        hds_play_build_modulate_from_rows(p, vs->ctx_menu.arg, rows, &count);
        hds_ctxmenu_render(cr, &vs->ctx_menu, rows, count, window_w, window_h);
    }
}

// ---------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------
// Cutoff/Resonance's own right-click "Modulate From" menu target numbers, one pair per flat
// filter slot (0-4) -- inverse of hds_lfo_dest_label()'s own `target >= 90` decode (Freq on the
// even target, Res on the odd one right after it).
static int hds_filt_cutoff_dest_target(int idx) { return 90 + idx * 2; }
static int hds_filt_res_dest_target(int idx) { return 91 + idx * 2; }

static bool hds_clap_filters_on_button_press(HdsClapPlugin *p, HdsClapFiltersViewState *vs, double window_w, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);

    if (vs->ctx_menu.open) return hds_play_ctxmenu_handle_click(p, &vs->ctx_menu, window_w, window_h, x, y);

    if (vs->dropdown_kind != HDS_FILT_DROPDOWN_NONE) {
        for (int i = 0; i < HDS_FILT_TYPE_COUNT; i++) {
            double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
            double list_h = HDS_FILT_TYPE_COUNT * 16.0;
            double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
            if (y0 < 0.0) y0 = 0.0;
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, 64.0, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) prog->filters[vs->dropdown_index].filter_type = (float)i;
                vs->dropdown_kind = HDS_FILT_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_FILT_DROPDOWN_NONE;
        return true;
    }

    HdsFiltChainModeLayout cm;
    hds_clap_filters_compute_chain_mode_layout(&cm);
    if (hds_play_rect_contains(&cm.chain_mode_pill, x, y)) {
        if (button == 1) prog->filter_chain_mode = (prog->filter_chain_mode != 0.0f) ? 0.0f : 1.0f;
        return true;
    }

    double rows_top = HDS_FILT_CONTENT_TOP + 30.0;
    for (int i = 0; i < 5; i++) {
        FilterParams *filt = &prog->filters[i];
        HdsFiltRowLayout L;
        hds_clap_filters_compute_row_layout(i, rows_top, &L);

        HdsPlayKnobMenu cutoff_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, hds_filt_cutoff_dest_target(i)};
        HdsPlayKnobMenu res_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, hds_filt_res_dest_target(i)};
        if (hds_play_knob_hit_test(&L.freq_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &filt->cutoff, 40.0, 18000.0, x, y, button, cutoff_menu); return true; }
        if (hds_play_knob_hit_test(&L.res_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &filt->res_q, 0.0, 100.0, x, y, button, res_menu); return true; }
        if (hds_play_rect_contains(&L.slope_pill, x, y)) {
            if (button == 1) filt->slope_poles = (filt->slope_poles != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_rect_contains(&L.type_pill, x, y)) {
            vs->dropdown_kind = HDS_FILT_DROPDOWN_TYPE; vs->dropdown_index = i; vs->dropdown_anchor = L.type_pill;
            return true;
        }
    }

    double eq_top = hds_clap_filters_eq_top_y();
    for (int b = 0; b < 3; b++) {
        EqBandParams *band = &prog->eq_bands[b];
        double col_x = HDS_FILT_MARGIN + b * HDS_FILT_EQ_BAND_W;
        HdsEqBandLayout L;
        hds_clap_filters_compute_eq_layout(b, col_x, eq_top, &L);
        if (hds_play_knob_hit_test(&L.freq_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &band->freq, 20.0, 20000.0, x, y, button, HDS_PLAY_NO_MENU); return true; }
        if (hds_play_knob_hit_test(&L.gain_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &band->gain_db, -18.0, 18.0, x, y, button, HDS_PLAY_NO_MENU); return true; }
        if (b == 1 && hds_play_knob_hit_test(&L.q_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &band->q, 0.1, 10.0, x, y, button, HDS_PLAY_NO_MENU); return true; }
    }

    (void)button;
    return false;
}

static void hds_clap_filters_on_motion(HdsClapFiltersViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

static void hds_clap_filters_on_button_release(HdsClapFiltersViewState *vs) {
    double anchor_x = 0.0, anchor_y = 0.0;
    HdsPlayKnobMenu menu = hds_play_release_knob_drag(&vs->knob_drag, &anchor_x, &anchor_y);
    if (menu.kind != 0) vs->ctx_menu = (HdsCtxMenu){true, anchor_x, anchor_y, menu.kind, menu.arg};
}
