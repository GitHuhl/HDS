// hds_clap_lfos_draw.c: hds.clap's own LFOs tab -- all 16 PackLfoParams rows (Waveform/Curve-
// Drive/Rate/Tempo-Sync/Depth/Phase/Key-Sync/Per-Key+Key-Track+Vel-Track/Destination), one
// scrollable list of 16 identical rows. Platform-agnostic like every
// other tab file -- #include'd by clap_plugin.c after hds_clap_filters_draw.c, reusing the same
// generic knob primitives (HdsPlayKnobDrag/hds_play_draw_knob/hds_play_knob_hit_test/
// hds_play_start_knob_drag) that file already reuses from hds_clap_play_draw.c, rather than
// duplicating them a third time.
//
// Rate and Phase each have their own right-click "Modulate From" menu, and Depth its own
// right-click "Modulation Source (CC)" menu (HDS_CTXMENU_KIND_LFO_DEPTH -- mod_source_cc, a
// live-MIDI multiplicative scale, unrelated to the additive "Modulate From" routing) -- all three
// using hds_clap_play_draw.c's generic HdsCtxMenu machinery, the same way Play's oscillator
// Pitch/Phase/Pulse-Width and Filters' Cutoff/Resonance already do. The Destination combo itself
// (100 real entries) is always-visible, not a right-click extra -- unlike Filter/Osc knobs, an
// LFO's whole reason to exist is routing its own output somewhere.
//
// hds_lfo_dest_label()/HDS_LFO_DEST_COUNT and the Destination dropdown's own grouped display order
// (hds_dest_list_build/hds_dest_list_render/hds_dest_list_hit_test) now live in
// hds_clap_play_draw.c (an earlier-included file) instead of here: the "Modulate From" menu content
// built there needs the label/count too, and that file is #include'd before this one in the fixed
// single-TU chain clap_plugin.c/hds_standalone.c both use.
//
// The LFO waveform enum (Tri/Sine/RevSaw/Sq/Off/Saw = indices 0-5) is a SEPARATE, LFO-only enum
// from the oscillators' own WaveType (WAVE_SAW=0..WAVE_REVSAW=9, see hds_clap_play_draw.c) --
// lookup_lfo_waveform() in backend.c consumes this file's own HDS_LFO_WAVE_NAMES ordering, not
// WaveType at all. Never conflate the two.

#include "hds_clap_gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDS_LFO_DROPDOWN_NONE 0
#define HDS_LFO_DROPDOWN_WAVE 1
#define HDS_LFO_DROPDOWN_TEMPOSYNC 2
#define HDS_LFO_DROPDOWN_KEYSYNC 3
#define HDS_LFO_DROPDOWN_DEST 4

#define HDS_LFO_WAVE_COUNT 6
static const char *const HDS_LFO_WAVE_NAMES[HDS_LFO_WAVE_COUNT] = {"Tri", "Sine", "RevSaw", "Sq", "Off", "Saw"};
// Curve/Drive is meaningful only for these 4 shapes (Tri/Sine/RevSaw/Saw) -- indices 3=Sq/4=Off
// hide it.
static bool hds_lfo_wave_has_curve(int wave) { return wave == 0 || wave == 1 || wave == 2 || wave == 5; }

#define HDS_LFO_KEYSYNC_COUNT 3
static const char *const HDS_LFO_KEYSYNC_NAMES[HDS_LFO_KEYSYNC_COUNT] = {"Free", "Sync", "Whl"};

#define HDS_LFO_TEMPOSYNC_COUNT 25
static const char *const HDS_LFO_TEMPOSYNC_NAMES[HDS_LFO_TEMPOSYNC_COUNT] = {
    "Free",
    "2 Bars", "2 Bars D", "2 Bars T",
    "1 Bar", "1 Bar D", "1 Bar T",
    "1/2", "1/2 D", "1/2 T",
    "1/4", "1/4 D", "1/4 T",
    "1/8", "1/8 D", "1/8 T",
    "1/16", "1/16 D", "1/16 T",
    "1/32", "1/32 D", "1/32 T",
    "1/64", "1/64 D", "1/64 T",
};

void hds_clap_lfos_view_state_init(HdsClapLfosViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->dropdown_kind = HDS_LFO_DROPDOWN_NONE;
}

// ---------------------------------------------------------------------------------------------
// Layout -- one row, all controls left to right. Reused verbatim by render and hit-testing.
// ---------------------------------------------------------------------------------------------
#define HDS_LFO_MARGIN 10.0
#define HDS_LFO_HEADER_H 28.0
#define HDS_LFO_ROW_H 58.0
#define HDS_LFO_KNOB_R 10.0
#define HDS_LFO_GAP 8.0

typedef struct {
    HdsFxRect row;
    HdsFxRect wave_pill, curve_knob, rate_knob, temposync_pill, depth_knob, phase_knob,
              keysync_pill, pk_btn, keytrack_knob, veltrack_knob, dest_pill;
} HdsLfoRowLayout;

static void hds_clap_lfos_compute_row_layout(int idx, double scroll_offset_px, HdsLfoRowLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = HDS_LFO_HEADER_H + idx * HDS_LFO_ROW_H - scroll_offset_px;
    out->row = (HdsFxRect){0, y, 1080.0, HDS_LFO_ROW_H};
    double x = HDS_LFO_MARGIN + 40.0; // room for the "LFO N" label to its left
    double cy = y + HDS_LFO_KNOB_R + 18.0;

    out->wave_pill = (HdsFxRect){x, y + 16.0, 46.0, 18.0}; x += 46.0 + HDS_LFO_GAP;
    out->curve_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->rate_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->temposync_pill = (HdsFxRect){x, y + 16.0, 62.0, 18.0}; x += 62.0 + HDS_LFO_GAP;
    out->depth_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->phase_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->keysync_pill = (HdsFxRect){x, y + 16.0, 46.0, 18.0}; x += 46.0 + HDS_LFO_GAP;
    out->pk_btn = (HdsFxRect){x, y + 16.0, 22.0, 18.0}; x += 22.0 + HDS_LFO_GAP;
    out->keytrack_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->veltrack_knob = (HdsFxRect){x + HDS_LFO_KNOB_R, cy, HDS_LFO_KNOB_R, HDS_LFO_KNOB_R}; x += 2.0 * HDS_LFO_KNOB_R + HDS_LFO_GAP;
    out->dest_pill = (HdsFxRect){x, y + 16.0, 112.0, 18.0};
}

// ---------------------------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------------------------
static void hds_clap_lfos_render_short_dropdown(cairo_t *cr, const HdsFxRect *anchor, double window_h,
                                                  const char *const *names, int count, int current) {
    double list_h = count * 16.0;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    HdsFxRect bg = {anchor->x, y0, 90.0, list_h};
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
        hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, names[i]);
    }
}

static void hds_clap_lfos_dest_list_rect(const HdsFxRect *anchor, double window_h, HdsFxRect *out) {
    double list_h = HDS_DEST_LIST_MAX_H;
    if (list_h > window_h - 8.0) list_h = window_h - 8.0;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    *out = (HdsFxRect){anchor->x, y0, 140.0, list_h};
}

static void hds_clap_lfos_render_dest_dropdown(cairo_t *cr, HdsClapLfosViewState *vs, double window_h, int current) {
    HdsFxRect bg;
    hds_clap_lfos_dest_list_rect(&vs->dropdown_anchor, window_h, &bg);
    hds_dest_list_render(cr, &bg, &vs->dest_scroll_px, current);
}

void hds_clap_lfos_render(cairo_t *cr, HdsClapPlugin *p, HdsClapLfosViewState *vs, double window_w, double window_h) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    ProgramState *prog = hds_play_program(p);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_LFO_MARGIN, HDS_LFO_HEADER_H - 8.0, "Assignable LFOs");

    cairo_save(cr);
    cairo_rectangle(cr, 0, HDS_LFO_HEADER_H, window_w, window_h - HDS_LFO_HEADER_H);
    cairo_clip(cr);

    for (int i = 0; i < LFO_COUNT; i++) {
        PackLfoParams *lfo = &prog->lfo[i];
        HdsLfoRowLayout L;
        hds_clap_lfos_compute_row_layout(i, vs->scroll_offset_px, &L);
        if (L.row.y + L.row.h < HDS_LFO_HEADER_H || L.row.y > window_h) continue;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5);
        hds_fx_rgb(cr, 0.5, 0.5, 0.53);
        char lbl[10]; snprintf(lbl, sizeof(lbl), "LFO %d", i + 1);
        hds_fx_left_text(cr, HDS_LFO_MARGIN, L.row.y + HDS_LFO_ROW_H / 2 + 3.0, lbl);

        int wave = (int)lfo->wave_type;
        if (wave < 0 || wave >= HDS_LFO_WAVE_COUNT) wave = 4;
        bool wave_dd_open = (vs->dropdown_kind == HDS_LFO_DROPDOWN_WAVE && vs->dropdown_row == i);
        hds_fx_draw_pill(cr, L.wave_pill.x, L.wave_pill.y, L.wave_pill.w, L.wave_pill.h, HDS_LFO_WAVE_NAMES[wave], wave_dd_open || wave != 4, false);

        if (hds_lfo_wave_has_curve(wave)) {
            char ctxt[16]; snprintf(ctxt, sizeof(ctxt), "%d%%", (int)(lfo->curve_drive * 100.0f));
            const char *clabel = (wave == 1) ? "Drive" : "Curve";
            float cfrac = (lfo->curve_drive + 1.0f) / 2.0f;
            hds_play_draw_knob(cr, L.curve_knob.x, L.curve_knob.y, L.curve_knob.w, cfrac, false, clabel, ctxt);
        }

        bool synced = (int)lfo->tempo_sync_division != 0;
        char rtxt[16];
        float rfrac;
        if (synced) { snprintf(rtxt, sizeof(rtxt), "%s", HDS_LFO_TEMPOSYNC_NAMES[(int)lfo->tempo_sync_division]); rfrac = 0.0f; }
        else { snprintf(rtxt, sizeof(rtxt), "%.1fHz", lfo->rate_hz); rfrac = (float)((lfo->rate_hz - 0.1) / (200.0 - 0.1)); }
        hds_play_draw_knob(cr, L.rate_knob.x, L.rate_knob.y, L.rate_knob.w, rfrac, synced, "Rate", rtxt);

        bool tempo_dd_open = (vs->dropdown_kind == HDS_LFO_DROPDOWN_TEMPOSYNC && vs->dropdown_row == i);
        hds_fx_draw_pill(cr, L.temposync_pill.x, L.temposync_pill.y, L.temposync_pill.w, L.temposync_pill.h,
                          HDS_LFO_TEMPOSYNC_NAMES[(int)lfo->tempo_sync_division], tempo_dd_open || synced, false);

        char dtxt[16]; snprintf(dtxt, sizeof(dtxt), "%d%%", (int)(lfo->mod_depth * 100.0f + 0.5f));
        hds_play_draw_knob(cr, L.depth_knob.x, L.depth_knob.y, L.depth_knob.w, lfo->mod_depth, false, "Dp", dtxt);

        char ptxt[16]; snprintf(ptxt, sizeof(ptxt), "%d\xc2\xb0", (int)lfo->phase_offset);
        hds_play_draw_knob(cr, L.phase_knob.x, L.phase_knob.y, L.phase_knob.w, (float)(lfo->phase_offset / 360.0), false, "Ph", ptxt);

        int keysync = (int)lfo->key_sync;
        if (keysync < 0 || keysync >= HDS_LFO_KEYSYNC_COUNT) keysync = 0;
        bool keysync_dd_open = (vs->dropdown_kind == HDS_LFO_DROPDOWN_KEYSYNC && vs->dropdown_row == i);
        hds_fx_draw_pill(cr, L.keysync_pill.x, L.keysync_pill.y, L.keysync_pill.w, L.keysync_pill.h, HDS_LFO_KEYSYNC_NAMES[keysync], keysync_dd_open || keysync != 0, false);

        bool pk = lfo->per_key_mode != 0.0f;
        hds_fx_draw_pill(cr, L.pk_btn.x, L.pk_btn.y, L.pk_btn.w, L.pk_btn.h, "PK", pk, false);
        if (pk) {
            char ktxt[16]; snprintf(ktxt, sizeof(ktxt), "%d", (int)lfo->key_track);
            hds_play_draw_knob(cr, L.keytrack_knob.x, L.keytrack_knob.y, L.keytrack_knob.w, (float)((lfo->key_track + 100.0) / 200.0), false, "Key", ktxt);
            char vtxt[16]; snprintf(vtxt, sizeof(vtxt), "%d", (int)lfo->vel_track);
            hds_play_draw_knob(cr, L.veltrack_knob.x, L.veltrack_knob.y, L.veltrack_knob.w, (float)((lfo->vel_track + 100.0) / 200.0), false, "Vel", vtxt);
        }

        int dest = (int)lfo->dest_target;
        if (dest < 0 || dest >= HDS_LFO_DEST_COUNT) dest = 0;
        char dest_label[24]; hds_lfo_dest_label(dest, dest_label, sizeof(dest_label));
        bool dest_dd_open = (vs->dropdown_kind == HDS_LFO_DROPDOWN_DEST && vs->dropdown_row == i);
        hds_fx_draw_pill(cr, L.dest_pill.x, L.dest_pill.y, L.dest_pill.w, L.dest_pill.h, dest_label, dest_dd_open || dest != 0, false);
    }
    cairo_restore(cr);

    if (vs->dropdown_kind != HDS_LFO_DROPDOWN_NONE) {
        PackLfoParams *lfo = &prog->lfo[vs->dropdown_row];
        switch (vs->dropdown_kind) {
            case HDS_LFO_DROPDOWN_WAVE:
                hds_clap_lfos_render_short_dropdown(cr, &vs->dropdown_anchor, window_h, HDS_LFO_WAVE_NAMES, HDS_LFO_WAVE_COUNT, (int)lfo->wave_type);
                break;
            case HDS_LFO_DROPDOWN_TEMPOSYNC:
                hds_clap_lfos_render_short_dropdown(cr, &vs->dropdown_anchor, window_h, HDS_LFO_TEMPOSYNC_NAMES, HDS_LFO_TEMPOSYNC_COUNT, (int)lfo->tempo_sync_division);
                break;
            case HDS_LFO_DROPDOWN_KEYSYNC:
                hds_clap_lfos_render_short_dropdown(cr, &vs->dropdown_anchor, window_h, HDS_LFO_KEYSYNC_NAMES, HDS_LFO_KEYSYNC_COUNT, (int)lfo->key_sync);
                break;
            case HDS_LFO_DROPDOWN_DEST:
                hds_clap_lfos_render_dest_dropdown(cr, vs, window_h, (int)lfo->dest_target);
                break;
        }
    }

    if (vs->ctx_menu.open) {
        HdsCtxMenuRow rows[HDS_CTXMENU_MAX_ROWS]; int count = 0;
        hds_play_build_ctxmenu_rows(p, &vs->ctx_menu, rows, &count);
        hds_ctxmenu_render(cr, &vs->ctx_menu, rows, count, window_w, window_h);
    }
}

// ---------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------
bool hds_clap_lfos_on_button_press(HdsClapPlugin *p, HdsClapLfosViewState *vs, double window_w, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);

    if (vs->ctx_menu.open) return hds_play_ctxmenu_handle_click(p, &vs->ctx_menu, window_w, window_h, x, y);

    if (vs->dropdown_kind != HDS_LFO_DROPDOWN_NONE) {
        PackLfoParams *lfo = &prog->lfo[vs->dropdown_row];
        if (vs->dropdown_kind == HDS_LFO_DROPDOWN_DEST) {
            HdsFxRect bg;
            hds_clap_lfos_dest_list_rect(&vs->dropdown_anchor, window_h, &bg);
            if (hds_play_rect_contains(&bg, x, y)) {
                int target;
                if (hds_dest_list_hit_test(&bg, vs->dest_scroll_px, x, y, &target)) {
                    if (button == 1) lfo->dest_target = (float)target;
                    vs->dropdown_kind = HDS_LFO_DROPDOWN_NONE;
                } // a section-header hit is a no-op -- keeps the list open, same as before
                return true;
            }
            vs->dropdown_kind = HDS_LFO_DROPDOWN_NONE;
            return true;
        }
        const char *const *names; int count; float *target_f = NULL; int *target_i = NULL; (void)target_i;
        switch (vs->dropdown_kind) {
            case HDS_LFO_DROPDOWN_WAVE: names = HDS_LFO_WAVE_NAMES; count = HDS_LFO_WAVE_COUNT; target_f = &lfo->wave_type; break;
            case HDS_LFO_DROPDOWN_TEMPOSYNC: names = HDS_LFO_TEMPOSYNC_NAMES; count = HDS_LFO_TEMPOSYNC_COUNT; target_f = &lfo->tempo_sync_division; break;
            case HDS_LFO_DROPDOWN_KEYSYNC: names = HDS_LFO_KEYSYNC_NAMES; count = HDS_LFO_KEYSYNC_COUNT; target_f = &lfo->key_sync; break;
            default: names = NULL; count = 0; break;
        }
        double list_h = count * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        for (int i = 0; i < count; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, 90.0, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1 && target_f) *target_f = (float)i;
                vs->dropdown_kind = HDS_LFO_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_LFO_DROPDOWN_NONE;
        (void)names;
        return true;
    }

    if (y < HDS_LFO_HEADER_H) return false;
    int row = (int)((y - HDS_LFO_HEADER_H + vs->scroll_offset_px) / HDS_LFO_ROW_H);
    if (row < 0 || row >= LFO_COUNT) return false;

    PackLfoParams *lfo = &prog->lfo[row];
    HdsLfoRowLayout L;
    hds_clap_lfos_compute_row_layout(row, vs->scroll_offset_px, &L);

    if (hds_play_rect_contains(&L.wave_pill, x, y)) {
        vs->dropdown_kind = HDS_LFO_DROPDOWN_WAVE; vs->dropdown_row = row; vs->dropdown_anchor = L.wave_pill;
        return true;
    }
    int wave = (int)lfo->wave_type;
    if (hds_lfo_wave_has_curve(wave) && hds_play_knob_hit_test(&L.curve_knob, x, y)) {
        hds_play_start_knob_drag(&vs->knob_drag, &lfo->curve_drive, -1.0, 1.0, x, y, button, HDS_PLAY_NO_MENU);
        return true;
    }
    bool synced = (int)lfo->tempo_sync_division != 0;
    HdsPlayKnobMenu rate_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, hds_lfo_rate_dest_target(row)};
    HdsPlayKnobMenu depth_menu = {HDS_CTXMENU_KIND_LFO_DEPTH, row};
    HdsPlayKnobMenu phase_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, hds_lfo_phase_dest_target(row)};
    if (!synced && hds_play_knob_hit_test(&L.rate_knob, x, y)) {
        hds_play_start_knob_drag(&vs->knob_drag, &lfo->rate_hz, 0.1, 200.0, x, y, button, rate_menu);
        return true;
    }
    if (hds_play_rect_contains(&L.temposync_pill, x, y)) {
        vs->dropdown_kind = HDS_LFO_DROPDOWN_TEMPOSYNC; vs->dropdown_row = row; vs->dropdown_anchor = L.temposync_pill;
        return true;
    }
    if (hds_play_knob_hit_test(&L.depth_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &lfo->mod_depth, 0.0, 1.0, x, y, button, depth_menu); return true; }
    if (hds_play_knob_hit_test(&L.phase_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &lfo->phase_offset, 0.0, 360.0, x, y, button, phase_menu); return true; }
    if (hds_play_rect_contains(&L.keysync_pill, x, y)) {
        vs->dropdown_kind = HDS_LFO_DROPDOWN_KEYSYNC; vs->dropdown_row = row; vs->dropdown_anchor = L.keysync_pill;
        return true;
    }
    if (hds_play_rect_contains(&L.pk_btn, x, y)) {
        if (button == 1) lfo->per_key_mode = (lfo->per_key_mode != 0.0f) ? 0.0f : 1.0f;
        return true;
    }
    if (lfo->per_key_mode != 0.0f) {
        if (hds_play_knob_hit_test(&L.keytrack_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &lfo->key_track, -100.0, 100.0, x, y, button, HDS_PLAY_NO_MENU); return true; }
        if (hds_play_knob_hit_test(&L.veltrack_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &lfo->vel_track, -100.0, 100.0, x, y, button, HDS_PLAY_NO_MENU); return true; }
    }
    if (hds_play_rect_contains(&L.dest_pill, x, y)) {
        vs->dropdown_kind = HDS_LFO_DROPDOWN_DEST; vs->dropdown_row = row; vs->dropdown_anchor = L.dest_pill; vs->dest_scroll_px = 0.0;
        return true;
    }

    (void)button;
    return true; // clicking empty row space is a no-op, not a click-through (matches this GUI's own tab-bar convention)
}

void hds_clap_lfos_on_motion(HdsClapLfosViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

void hds_clap_lfos_on_button_release(HdsClapLfosViewState *vs) {
    double anchor_x = 0.0, anchor_y = 0.0;
    HdsPlayKnobMenu menu = hds_play_release_knob_drag(&vs->knob_drag, &anchor_x, &anchor_y);
    if (menu.kind != 0) vs->ctx_menu = (HdsCtxMenu){true, anchor_x, anchor_y, menu.kind, menu.arg};
}

void hds_clap_lfos_on_scroll(HdsClapLfosViewState *vs, double window_h, int delta) {
    if (vs->dropdown_kind == HDS_LFO_DROPDOWN_DEST) {
        vs->dest_scroll_px -= delta * HDS_DEST_LIST_ROW_H * 3.0;
        if (vs->dest_scroll_px < 0.0) vs->dest_scroll_px = 0.0;
        return;
    }
    double content_h = HDS_LFO_HEADER_H + LFO_COUNT * HDS_LFO_ROW_H;
    double max_scroll = content_h - window_h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    vs->scroll_offset_px -= delta * HDS_LFO_ROW_H;
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}
