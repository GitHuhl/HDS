// hds_clap_envelopes_draw.c: hds.clap's own Envelopes tab -- the 6 "assignable EGs" (eg[4..9]/
// eg_mode[4..9], the exact same PackEGParams/topology system the Play tab's own 4 Amp EGs already
// use -- see hds_clap_adsr.h's own header comment) plus each one's own Intensity knob
// (eg_intensity[6], signed -1..1: unlike an Amp EG's hard-wired unipolar 0..1 amplitude multiply,
// an assignable EG can target pitch/pan/filter cutoff/anything else in matrix_shared_routes, so
// Intensity doubles as both depth AND polarity) and Destination combo (eg_dest_target[6], the
// exact same 100-entry matrix_shared_routes list the LFOs tab's own Destination combo already
// uses -- this file reuses hds_lfo_dest_label()/hds_lfo_dest_selectable()/HDS_LFO_DEST_COUNT
// verbatim from hds_clap_lfos_draw.c rather than re-transcribing the list a second time).
//
// Layout mirrors frontend.py's own Envelopes tab exactly: 6 full-width ADSRPanel-equivalents
// stacked vertically (NOT the Play tab's 4-across QHBoxLayout -- Amp EGs sit 4-up in their own
// group box; assignable EGs stack one per row in a taller, scrollable list, same "doesn't fit,
// needs scroll" situation the LFOs tab's own 16 rows already solved -- reuses that same whole-
// list-scroll convention). Each panel's title row carries "EG N" + the Intensity knob + the
// Destination pill (frontend.py's own `title_layout.addStretch()` then Int: label + dial + combo,
// all in the ADSRPanel's own title bar, not a separate row) -- distinct from the Play tab's Amp
// EG panels, whose title row instead carries a Vel->Amp knob (and, Osc1 only, the "1->All"/"Sync"
// buttons) -- assignable EGs have none of that, they're pure modulation sources with no
// oscillator of their own to scale and no live-mirror concept.
//
// SCOPE for this pass: same "everything visible without a right-click" cut as every prior tab.
// The Intensity knob has no right-click menu at all in frontend.py (confirmed by reading every
// setContextMenuCallback call site -- only Filter/Osc/LFO knobs get one), so nothing is deferred
// there. The only deferred piece is the same one deferred everywhere else: each Destination-
// targetable knob elsewhere (Filter Cutoff, LFO Rate/Depth/Phase, Osc Pitch/Phase/PulseWidth)
// having its own right-click "Modulate From" menu that could list "EG 1".."EG 6" as a source --
// that menu isn't wired up anywhere yet, on any tab, so an assignable EG's own Destination combo
// (the ordinary, always-visible way it reaches a target) is the only routing path this native GUI
// offers today, exactly like LFOs. The 🎲 "Randomize" button (frontend.py's own
// randomize_assignable_egs) is also not ported -- a pure GUI convenience with no backend field of
// its own, lowest priority of anything seen on any tab so far.

#define HDS_ENV_DROPDOWN_NONE 0
#define HDS_ENV_DROPDOWN_MODE 1
#define HDS_ENV_DROPDOWN_DEST 2

typedef struct {
    int dropdown_kind;   // HDS_ENV_DROPDOWN_*
    int dropdown_row;    // which assignable EG (0-5) the open dropdown belongs to
    HdsFxRect dropdown_anchor;
    double dest_scroll_px; // the open Destination list's own internal scroll -- see hds_clap_lfos_draw.c's own identical field
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
    int adsr_drag_eg;        // -1 = not dragging any curve; else 0-5
    int adsr_active_node[ASSIGNABLE_EG_COUNT];
    double scroll_offset_px; // the whole 6-panel list's own vertical scroll, same convention as hds_clap_lfos_draw.c's own
} HdsClapEnvelopesViewState;

static void hds_clap_envelopes_view_state_init(HdsClapEnvelopesViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->dropdown_kind = HDS_ENV_DROPDOWN_NONE;
    vs->adsr_drag_eg = -1;
    for (int i = 0; i < ASSIGNABLE_EG_COUNT; i++) vs->adsr_active_node[i] = -1;
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------
#define HDS_ENV_MARGIN 8.0
#define HDS_ENV_HEADER_H 24.0
#define HDS_ENV_TITLE_ROW_H 40.0
#define HDS_ENV_CURVE_H 130.0
#define HDS_ENV_KNOB_R 9.0
#define HDS_ENV_PANEL_H (HDS_ENV_TITLE_ROW_H + HDS_ENV_CURVE_H + 6.0 + 16.0 + 44.0 + 10.0)

typedef struct {
    HdsFxRect title, intensity_knob, dest_pill, curve, mode_pill, dial[6];
} HdsEnvPanelLayout;

static void hds_clap_env_compute_panel_layout(int e, int env_type, double scroll_offset_px, double panel_w, HdsEnvPanelLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = HDS_ENV_HEADER_H + e * HDS_ENV_PANEL_H - scroll_offset_px;
    out->title = (HdsFxRect){HDS_ENV_MARGIN, y, panel_w, HDS_ENV_TITLE_ROW_H};
    out->dest_pill = (HdsFxRect){HDS_ENV_MARGIN + panel_w - 120.0, y + 11.0, 120.0, 18.0};
    out->intensity_knob = (HdsFxRect){out->dest_pill.x - 30.0, y + HDS_ENV_KNOB_R + 12.0, HDS_ENV_KNOB_R, HDS_ENV_KNOB_R};

    double cy = y + HDS_ENV_TITLE_ROW_H;
    out->curve = (HdsFxRect){HDS_ENV_MARGIN, cy, panel_w, HDS_ENV_CURVE_H};
    cy += HDS_ENV_CURVE_H + 6.0;
    out->mode_pill = (HdsFxRect){HDS_ENV_MARGIN, cy, 70.0, 16.0};
    cy += 16.0 + 10.0;
    // dial[i] is the i-th VISIBLE knob's rect, left to right -- see HDS_PLAY_EG_FIELD_ORDER's own
    // comment (hds_clap_play_draw.c) for why this is a per-type field order, not a fixed 6-slot
    // arrangement with visibility toggling.
    int count = hds_play_eg_dial_count(env_type);
    double dial_stride = panel_w / (double)count;
    for (int i = 0; i < count; i++) {
        out->dial[i] = (HdsFxRect){HDS_ENV_MARGIN + i * dial_stride + dial_stride / 2.0, cy + HDS_ENV_KNOB_R + 9.0, HDS_ENV_KNOB_R, HDS_ENV_KNOB_R};
    }
}

// ---------------------------------------------------------------------------------------------
// The Destination dropdown -- same scrollable-100-entry-list mechanism as hds_clap_lfos_draw.c's
// own (see that file's own header comment on HDS_LFO_DEST_LIST_ROW_H/MAX_H, and
// hds_lfo_dest_label()/hds_lfo_dest_selectable()), duplicated here only for the small amount of
// geometry/state that's genuinely per-tab (this tab's own dropdown_anchor/dest_scroll_px live on
// HdsClapEnvelopesViewState, not HdsClapLfosViewState) -- the actual 100-entry list/labels/
// selectability rules are reused, never re-transcribed.
// ---------------------------------------------------------------------------------------------
static void hds_clap_env_dest_list_rect(const HdsFxRect *anchor, double window_h, HdsFxRect *out) {
    double list_h = HDS_LFO_DEST_LIST_MAX_H;
    if (list_h > window_h - 8.0) list_h = window_h - 8.0;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    *out = (HdsFxRect){anchor->x - 20.0, y0, 140.0, list_h};
}

static void hds_clap_env_render_dest_dropdown(cairo_t *cr, HdsClapEnvelopesViewState *vs, double window_h, int current) {
    HdsFxRect bg;
    hds_clap_env_dest_list_rect(&vs->dropdown_anchor, window_h, &bg);
    int visible_rows = (int)(bg.h / HDS_LFO_DEST_LIST_ROW_H);
    double max_scroll = (HDS_LFO_DEST_COUNT - visible_rows) * HDS_LFO_DEST_LIST_ROW_H;
    if (max_scroll < 0.0) max_scroll = 0.0;
    if (vs->dest_scroll_px < 0.0) vs->dest_scroll_px = 0.0;
    if (vs->dest_scroll_px > max_scroll) vs->dest_scroll_px = max_scroll;

    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);

    cairo_save(cr);
    cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h);
    cairo_clip(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8.5);
    int first = (int)(vs->dest_scroll_px / HDS_LFO_DEST_LIST_ROW_H);
    for (int i = first; i < HDS_LFO_DEST_COUNT; i++) {
        double ry = bg.y + i * HDS_LFO_DEST_LIST_ROW_H - vs->dest_scroll_px;
        if (ry > bg.y + bg.h) break;
        char label[24]; hds_lfo_dest_label(i, label, sizeof(label));
        bool selectable = hds_lfo_dest_selectable(i);
        if (i == current) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr); cairo_rectangle(cr, bg.x, ry, bg.w, HDS_LFO_DEST_LIST_ROW_H); cairo_fill(cr);
        }
        if (!selectable) hds_fx_rgb(cr, 0.35, 0.35, 0.38);
        else hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
        hds_fx_left_text(cr, bg.x + 4, ry + HDS_LFO_DEST_LIST_ROW_H / 2 + 3.0, label);
    }
    cairo_restore(cr);
}

// ---------------------------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------------------------
static void hds_clap_envelopes_render(cairo_t *cr, HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double window_h) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    ProgramState *prog = hds_play_program(p);
    double panel_w = window_w - 2.0 * HDS_ENV_MARGIN;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_ENV_MARGIN, HDS_ENV_HEADER_H - 8.0, "Assignable Envelopes");

    cairo_save(cr);
    cairo_rectangle(cr, 0, HDS_ENV_HEADER_H, window_w, window_h - HDS_ENV_HEADER_H);
    cairo_clip(cr);

    for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) {
        int eg_idx = 4 + e;
        PackEGParams *eg = &prog->eg[eg_idx];
        int env_type = (int)prog->eg_mode[eg_idx];
        if (env_type < 0 || env_type >= HDS_PLAY_EGMODE_COUNT) env_type = 0;

        HdsEnvPanelLayout L;
        hds_clap_env_compute_panel_layout(e, env_type, vs->scroll_offset_px, panel_w, &L);
        if (L.title.y + HDS_ENV_PANEL_H < HDS_ENV_HEADER_H || L.title.y > window_h) continue;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.0);
        hds_fx_rgb(cr, 0.55, 0.55, 0.58);
        char title[10]; snprintf(title, sizeof(title), "EG %d", e + 1);
        hds_fx_left_text(cr, L.title.x, L.title.y + 24.0, title);

        float *intensity = &prog->eg_intensity[e];
        char itxt[16]; snprintf(itxt, sizeof(itxt), "%d%%", (int)(*intensity * 100.0f + (*intensity >= 0 ? 0.5f : -0.5f)));
        float ifrac = (*intensity + 1.0f) / 2.0f;
        hds_play_draw_knob(cr, L.intensity_knob.x, L.intensity_knob.y, L.intensity_knob.w, ifrac, false, "Int", itxt);

        int dest = (int)prog->eg_dest_target[e];
        if (dest < 0 || dest >= HDS_LFO_DEST_COUNT) dest = 0;
        char dest_label[24]; hds_lfo_dest_label(dest, dest_label, sizeof(dest_label));
        bool dest_dd_open = (vs->dropdown_kind == HDS_ENV_DROPDOWN_DEST && vs->dropdown_row == e);
        hds_fx_draw_pill(cr, L.dest_pill.x, L.dest_pill.y, L.dest_pill.w, L.dest_pill.h, dest_label, dest_dd_open || dest != 0, false);

        AdsrEnvelopeState adsr;
        hds_clap_adsr_load_from_backend(&adsr, eg, env_type);
        adsr.active_node = vs->adsr_active_node[e];
        cairo_save(cr);
        cairo_rectangle(cr, L.curve.x, L.curve.y, L.curve.w, L.curve.h);
        cairo_clip(cr);
        cairo_translate(cr, L.curve.x, L.curve.y);
        adsr_draw(cr, &adsr, L.curve.w, L.curve.h);
        cairo_restore(cr);

        bool mode_dd_open = (vs->dropdown_kind == HDS_ENV_DROPDOWN_MODE && vs->dropdown_row == e);
        hds_fx_draw_pill(cr, L.mode_pill.x, L.mode_pill.y, L.mode_pill.w, L.mode_pill.h, HDS_PLAY_EGMODE_NAMES[env_type], mode_dd_open, false);

        double vals[6] = {eg->d, eg->h, eg->a, eg->dec, eg->s, eg->r};
        int dial_count = hds_play_eg_dial_count(env_type);
        for (int i = 0; i < dial_count; i++) {
            int field = HDS_PLAY_EG_FIELD_ORDER[env_type][i];
            float frac = (float)((vals[field] - HDS_PLAY_EG_LO[field]) / (HDS_PLAY_EG_HI[field] - HDS_PLAY_EG_LO[field]));
            char vtxt[16];
            if (field == HDS_PLAY_EG_FIELD_S) snprintf(vtxt, sizeof(vtxt), "%d%%", (int)(vals[field] + 0.5));
            else snprintf(vtxt, sizeof(vtxt), "%dms", (int)(vals[field] + 0.5));
            hds_play_draw_knob(cr, L.dial[i].x, L.dial[i].y, L.dial[i].w, frac, false, HDS_PLAY_DIAL_LABELS[field], vtxt);
        }
    }
    cairo_restore(cr);

    if (vs->dropdown_kind == HDS_ENV_DROPDOWN_NONE) return;
    int eg_idx = 4 + vs->dropdown_row;
    if (vs->dropdown_kind == HDS_ENV_DROPDOWN_MODE) {
        int count = HDS_PLAY_EGMODE_COUNT;
        double list_h = count * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        HdsFxRect bg = {vs->dropdown_anchor.x, y0, 70.0, list_h};
        hds_fx_rgb(cr, 0.13, 0.13, 0.16);
        cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
        hds_fx_rgba(cr, 1, 1, 1, 0.25);
        cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.0);
        int current = (int)prog->eg_mode[eg_idx];
        for (int i = 0; i < count; i++) {
            HdsFxRect r = {bg.x, bg.y + i * 16.0, bg.w, 16.0};
            if (i == current) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
            }
            hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
            hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, HDS_PLAY_EGMODE_NAMES[i]);
        }
    } else if (vs->dropdown_kind == HDS_ENV_DROPDOWN_DEST) {
        hds_clap_env_render_dest_dropdown(cr, vs, window_h, (int)prog->eg_dest_target[vs->dropdown_row]);
    }
}

// ---------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------
static bool hds_clap_envelopes_on_button_press(HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);
    double panel_w = window_w - 2.0 * HDS_ENV_MARGIN;

    if (vs->dropdown_kind != HDS_ENV_DROPDOWN_NONE) {
        int eg_idx = 4 + vs->dropdown_row;
        if (vs->dropdown_kind == HDS_ENV_DROPDOWN_DEST) {
            HdsFxRect bg;
            hds_clap_env_dest_list_rect(&vs->dropdown_anchor, window_h, &bg);
            if (hds_play_rect_contains(&bg, x, y)) {
                int i = (int)((y - bg.y + vs->dest_scroll_px) / HDS_LFO_DEST_LIST_ROW_H);
                if (i >= 0 && i < HDS_LFO_DEST_COUNT && hds_lfo_dest_selectable(i)) {
                    if (button == 1) prog->eg_dest_target[vs->dropdown_row] = (float)i;
                    vs->dropdown_kind = HDS_ENV_DROPDOWN_NONE;
                }
                return true;
            }
            vs->dropdown_kind = HDS_ENV_DROPDOWN_NONE;
            return true;
        }
        // HDS_ENV_DROPDOWN_MODE
        int count = HDS_PLAY_EGMODE_COUNT;
        double list_h = count * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        for (int i = 0; i < count; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, 70.0, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) prog->eg_mode[eg_idx] = (float)i;
                vs->dropdown_kind = HDS_ENV_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_ENV_DROPDOWN_NONE;
        return true;
    }

    if (y < HDS_ENV_HEADER_H) return false;
    int e = (int)((y - HDS_ENV_HEADER_H + vs->scroll_offset_px) / HDS_ENV_PANEL_H);
    if (e < 0 || e >= ASSIGNABLE_EG_COUNT) return false;
    int eg_idx = 4 + e;
    PackEGParams *eg = &prog->eg[eg_idx];
    int env_type = (int)prog->eg_mode[eg_idx];

    HdsEnvPanelLayout L;
    hds_clap_env_compute_panel_layout(e, env_type, vs->scroll_offset_px, panel_w, &L);

    if (hds_play_knob_hit_test(&L.intensity_knob, x, y)) {
        hds_play_start_knob_drag(&vs->knob_drag, &prog->eg_intensity[e], -1.0, 1.0, y);
        return true;
    }
    if (hds_play_rect_contains(&L.dest_pill, x, y)) {
        vs->dropdown_kind = HDS_ENV_DROPDOWN_DEST; vs->dropdown_row = e; vs->dropdown_anchor = L.dest_pill; vs->dest_scroll_px = 0.0;
        return true;
    }
    if (hds_play_rect_contains(&L.mode_pill, x, y)) {
        vs->dropdown_kind = HDS_ENV_DROPDOWN_MODE; vs->dropdown_row = e; vs->dropdown_anchor = L.mode_pill;
        return true;
    }
    if (hds_play_rect_contains(&L.curve, x, y)) {
        AdsrEnvelopeState adsr;
        hds_clap_adsr_load_from_backend(&adsr, eg, env_type);
        adsr_calculate_geometry(&adsr, L.curve.w, L.curve.h);
        int hit = adsr_hit_test(&adsr, x - L.curve.x, y - L.curve.y);
        vs->adsr_active_node[e] = hit;
        if (hit >= 0) vs->adsr_drag_eg = e;
        return true;
    }
    int dial_count = hds_play_eg_dial_count(env_type);
    for (int d = 0; d < dial_count; d++) {
        if (hds_play_knob_hit_test(&L.dial[d], x, y)) {
            int field = HDS_PLAY_EG_FIELD_ORDER[env_type][d];
            float *target = hds_play_eg_dial_target(eg, field);
            hds_play_start_knob_drag(&vs->knob_drag, target, HDS_PLAY_EG_LO[field], HDS_PLAY_EG_HI[field], y);
            return true;
        }
    }

    (void)button;
    return true;
}

static void hds_clap_envelopes_on_motion(HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double x, double y) {
    ProgramState *prog = hds_play_program(p);
    double panel_w = window_w - 2.0 * HDS_ENV_MARGIN;

    if (vs->knob_drag.target) {
        double delta = vs->knob_drag.start_y - y;
        double v = vs->knob_drag.start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (vs->knob_drag.hi - vs->knob_drag.lo);
        if (v < vs->knob_drag.lo) v = vs->knob_drag.lo; else if (v > vs->knob_drag.hi) v = vs->knob_drag.hi;
        *vs->knob_drag.target = (float)v;
        return;
    }
    if (vs->adsr_drag_eg >= 0) {
        int e = vs->adsr_drag_eg;
        int eg_idx = 4 + e;
        int env_type = (int)prog->eg_mode[eg_idx];
        HdsEnvPanelLayout L;
        hds_clap_env_compute_panel_layout(e, env_type, vs->scroll_offset_px, panel_w, &L);
        AdsrEnvelopeState adsr;
        hds_clap_adsr_load_from_backend(&adsr, &prog->eg[eg_idx], env_type);
        adsr.active_node = vs->adsr_active_node[e];
        adsr_calculate_geometry(&adsr, L.curve.w, L.curve.h);
        adsr_drag_update(&adsr, L.curve.w, L.curve.h, x - L.curve.x, y - L.curve.y);
        hds_clap_adsr_store_to_backend(&adsr, &prog->eg[eg_idx]);
    }
}

static void hds_clap_envelopes_on_button_release(HdsClapEnvelopesViewState *vs) {
    vs->knob_drag.target = NULL;
    vs->adsr_drag_eg = -1;
}

static void hds_clap_envelopes_on_scroll(HdsClapEnvelopesViewState *vs, double window_h, int delta) {
    if (vs->dropdown_kind == HDS_ENV_DROPDOWN_DEST) {
        vs->dest_scroll_px -= delta * HDS_LFO_DEST_LIST_ROW_H * 3.0;
        if (vs->dest_scroll_px < 0.0) vs->dest_scroll_px = 0.0;
        return;
    }
    double content_h = HDS_ENV_HEADER_H + ASSIGNABLE_EG_COUNT * HDS_ENV_PANEL_H;
    double max_scroll = content_h - window_h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    vs->scroll_offset_px -= delta * HDS_ENV_PANEL_H / 2.0;
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}
