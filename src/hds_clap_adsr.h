// hds_clap_adsr.h: the Amp EG curve widget -- a Cairo port of frontend.py's own
// InteractiveADSREnvelope (paintEvent/calculate_geometry/mousePressEvent/mouseMoveEvent). Every
// constant, formula, and per-node drag rule below is copied verbatim from a standalone prototype
// (adsr_widget.h) that was itself verified node-by-node against the Python source BEFORE being
// wired into the real plugin here -- see hds_clap_play_draw.c's own header comment on how it's
// actually used (the real source of truth is the backend's own PackEGParams/eg_mode fields; this
// widget's own AdsrEnvelopeState is just a transient, per-render-call VIEW of them, converted by
// hds_clap_adsr_load_from_backend()/store_to_backend() below -- nothing here is state that
// persists across a redraw except active_node, which the caller keeps in its own view state).
//
// Platform-agnostic like hds_fx_gui_draw.c itself (plain cairo_t*, no Xlib/CLAP type) -- included
// by hds_clap_play_draw.c only.
#pragma once
#include <cairo.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define ADSR_MAX_POINTS 8

typedef enum {
    ADSR_TAG_START, ADSR_TAG_DELAY, ADSR_TAG_ATTACK, ADSR_TAG_HOLD,
    ADSR_TAG_DECAY, ADSR_TAG_SUSTAIN, ADSR_TAG_RELEASE
} AdsrPointTag;

typedef struct {
    // env_type: 0=ADSR 1=AR 2=AD 3=ASR 4=AHD 5=DADSR 6=AHDSR -- same indices as frontend.py's
    // combo_type ("ADSR","AR","AD","ASR","AHD","DADSR","AHDSR"), and the same indices this
    // project's own backend.c stores directly in eg_mode[n].
    int env_type;
    double delay, hold, attack, decay, sustain, release; // seconds (delay/hold/attack/decay/release), sustain 0..1
    double attack_level, attack_peak; // 0..1 fractions -- the (0,0) node's and the attack node's own level
    int active_node; // -1 = none being dragged, matches Python's active_node = None

    // Geometry cache, rebuilt every adsr_calculate_geometry() call (mirrors self.points/
    // self.point_tags/self.point_anchor_x, rebuilt fresh in every paintEvent()).
    double point_x[ADSR_MAX_POINTS], point_y[ADSR_MAX_POINTS];
    AdsrPointTag point_tag[ADSR_MAX_POINTS];
    double point_anchor_x[ADSR_MAX_POINTS];
    int point_count;
    double scale_x; // cached like Python's self._scale_x
    double x0, y0;  // the graph's own (0,0) origin in widget pixels
} AdsrEnvelopeState;

static inline void adsr_init(AdsrEnvelopeState *e) {
    memset(e, 0, sizeof(*e));
    e->attack = 0.1; e->decay = 0.2; e->sustain = 0.5; e->release = 0.3;
    e->attack_level = 0.0; e->attack_peak = 1.0;
    e->active_node = -1;
}

// Real backend units (PackEGParams: milliseconds/0-100 percent) -> this widget's own units
// (seconds/0-1 fraction). active_node is NOT touched here -- it's the caller's own persistent view
// state (see this file's own header comment), not part of what a redraw recomputes.
static inline void hds_clap_adsr_load_from_backend(AdsrEnvelopeState *e, const PackEGParams *eg, int env_type) {
    e->env_type = env_type;
    e->delay = eg->d / 1000.0; e->hold = eg->h / 1000.0; e->attack = eg->a / 1000.0;
    e->decay = eg->dec / 1000.0; e->sustain = eg->s / 100.0; e->release = eg->r / 1000.0;
    e->attack_level = eg->al / 100.0; e->attack_peak = eg->pk / 100.0;
}

static inline void hds_clap_adsr_store_to_backend(const AdsrEnvelopeState *e, PackEGParams *eg) {
    eg->d = (float)(e->delay * 1000.0); eg->h = (float)(e->hold * 1000.0); eg->a = (float)(e->attack * 1000.0);
    eg->dec = (float)(e->decay * 1000.0); eg->s = (float)(e->sustain * 100.0); eg->r = (float)(e->release * 1000.0);
    eg->al = (float)(e->attack_level * 100.0); eg->pk = (float)(e->attack_peak * 100.0);
}

// Mirrors InteractiveADSREnvelope.calculate_geometry() exactly: same margin, same per-env_type
// segment-inclusion rules (t_dly/t_hld/t_dec/t_sus/t_rel), same fixed 0.3s pseudo-duration for the
// sustain segment (purely a layout width, never the real held-note duration), same point/tag/
// anchor-x sequence.
static inline void adsr_calculate_geometry(AdsrEnvelopeState *e, double w, double h) {
    const double margin = 10.0;
    double graph_w = w - margin * 2.0;
    double graph_h = h - margin * 2.0;

    double t_dly = (e->env_type == 5) ? fmax(0.01, e->delay) : 0.0;
    double t_att = fmax(0.01, e->attack);
    double t_hld = (e->env_type == 4 || e->env_type == 6) ? fmax(0.01, e->hold) : 0.0;
    double t_dec = (e->env_type == 0 || e->env_type == 2 || e->env_type == 4 || e->env_type == 5 || e->env_type == 6) ? fmax(0.01, e->decay) : 0.0;
    double t_sus = (e->env_type == 0 || e->env_type == 3 || e->env_type == 5 || e->env_type == 6) ? 0.3 : 0.0;
    double t_rel = (e->env_type == 0 || e->env_type == 1 || e->env_type == 3 || e->env_type == 5 || e->env_type == 6) ? fmax(0.01, e->release) : 0.0;

    double total_time = t_dly + t_att + t_hld + t_dec + t_sus + t_rel;
    double scale_x = graph_w / total_time;
    e->scale_x = scale_x;

    double x0 = margin, y0 = h - margin;
    e->x0 = x0; e->y0 = y0;
    double peak_y = margin + (graph_h * (1.0 - e->attack_peak));
    double start_y = margin + (graph_h * (1.0 - e->attack_level));

    e->point_count = 0;
    double cur_x = x0;

    if (e->env_type != 5) {
        e->point_x[e->point_count] = x0; e->point_y[e->point_count] = start_y;
        e->point_tag[e->point_count] = ADSR_TAG_START; e->point_anchor_x[e->point_count] = x0;
        e->point_count++;
    }
    if (t_dly > 0) {
        cur_x += t_dly * scale_x;
        e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = y0;
        e->point_tag[e->point_count] = ADSR_TAG_DELAY; e->point_anchor_x[e->point_count] = x0;
        e->point_count++;
    }
    double attack_start_x = cur_x;
    cur_x += t_att * scale_x;
    e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = peak_y;
    e->point_tag[e->point_count] = ADSR_TAG_ATTACK; e->point_anchor_x[e->point_count] = attack_start_x;
    e->point_count++;

    if (t_hld > 0) {
        double hold_start_x = cur_x;
        cur_x += t_hld * scale_x;
        e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = peak_y;
        e->point_tag[e->point_count] = ADSR_TAG_HOLD; e->point_anchor_x[e->point_count] = hold_start_x;
        e->point_count++;
    }
    if (t_dec > 0) {
        double decay_start_x = cur_x;
        cur_x += t_dec * scale_x;
        double sus_y = (e->env_type == 0 || e->env_type == 5 || e->env_type == 6)
                       ? margin + (graph_h * (1.0 - e->sustain)) : y0;
        e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = sus_y;
        e->point_tag[e->point_count] = ADSR_TAG_DECAY; e->point_anchor_x[e->point_count] = decay_start_x;
        e->point_count++;
    }
    if (t_sus > 0) {
        double sustain_start_x = cur_x;
        cur_x += t_sus * scale_x;
        double sus_y = margin + (graph_h * (1.0 - e->sustain));
        e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = sus_y;
        e->point_tag[e->point_count] = ADSR_TAG_SUSTAIN; e->point_anchor_x[e->point_count] = sustain_start_x;
        e->point_count++;
    }
    if (t_rel > 0) {
        double release_start_x = cur_x;
        cur_x += t_rel * scale_x;
        e->point_x[e->point_count] = cur_x; e->point_y[e->point_count] = y0;
        e->point_tag[e->point_count] = ADSR_TAG_RELEASE; e->point_anchor_x[e->point_count] = release_start_x;
        e->point_count++;
    }
}

// Mirrors InteractiveADSREnvelope.paintEvent() exactly: same background, same path (moveTo the
// "start" node if present else the raw x0,y0 origin, lineTo every remaining point), same curve
// width, same handle radius (white+accent border when active_node, accent+bg border otherwise).
// The curve/handle color itself does NOT mirror the old frontend's own teal -- it was swapped to
// match the same blue used for every knob's own value arc elsewhere in this GUI (see
// hds_fx_draw_knob_arc's own 0.45/0.68/0.95 in hds_fx_gui_draw.c), for visual consistency across
// the native GUI rather than carrying over the old UI's own accent color verbatim. Caller must
// have already translated the cairo context to this widget's own (0,0).
static inline void adsr_draw(cairo_t *cr, AdsrEnvelopeState *e, double w, double h) {
    adsr_calculate_geometry(e, w, h);

    cairo_set_source_rgb(cr, 0x1e / 255.0, 0x1e / 255.0, 0x24 / 255.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    int start_idx = 0;
    cairo_new_path(cr);
    if (e->point_count > 0 && e->point_tag[0] == ADSR_TAG_START) {
        cairo_move_to(cr, e->point_x[0], e->point_y[0]);
        start_idx = 1;
    } else {
        cairo_move_to(cr, e->x0, e->y0);
    }
    for (int i = start_idx; i < e->point_count; i++) {
        cairo_line_to(cr, e->point_x[i], e->point_y[i]);
    }
    cairo_set_source_rgb(cr, 0.45, 0.68, 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    const double handle_radius = 4.0;
    for (int i = 0; i < e->point_count; i++) {
        if (i == e->active_node) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.45, 0.68, 0.95);
        }
        cairo_arc(cr, e->point_x[i], e->point_y[i], handle_radius, 0, 2 * M_PI);
        cairo_fill_preserve(cr);
        if (i == e->active_node) {
            cairo_set_source_rgb(cr, 0.45, 0.68, 0.95);
            cairo_set_line_width(cr, 1.5);
        } else {
            cairo_set_source_rgb(cr, 0x1e / 255.0, 0x1e / 255.0, 0x24 / 255.0);
            cairo_set_line_width(cr, 1.0);
        }
        cairo_stroke(cr);
    }
}

// Mirrors InteractiveADSREnvelope.mousePressEvent() exactly: pass 1 -- any point within
// handle_radius*3 (Manhattan distance in Python; here Euclidean is close enough for a hit test of
// this radius and matches visually, Python's manhattanLength() is itself just an approximation of
// a circular hit zone). Pass 2 -- the invisible "hold"/"sustain" plateau drag band between its own
// anchor_x and its own point x, only when NOT already caught by pass 1. px/py are already relative
// to this widget's own (0,0) (caller subtracts its own translate offset first).
static inline int adsr_hit_test(AdsrEnvelopeState *e, double px, double py) {
    const double handle_radius = 4.0;
    for (int i = 0; i < e->point_count; i++) {
        double dx = px - e->point_x[i], dy = py - e->point_y[i];
        if (fabs(dx) + fabs(dy) < handle_radius * 3.0) return i;
    }
    for (int i = 0; i < e->point_count; i++) {
        AdsrPointTag t = e->point_tag[i];
        if (t != ADSR_TAG_HOLD && t != ADSR_TAG_SUSTAIN) continue;
        double left_x = e->point_anchor_x[i];
        if (left_x <= px && px <= e->point_x[i] && fabs(py - e->point_y[i]) < handle_radius * 3.0) return i;
    }
    return -1;
}

// Mirrors InteractiveADSREnvelope.mouseMoveEvent() exactly: same clamping, same level_frac
// formula, same per-tag update rules (including "release" anchored from the RIGHT edge inward,
// the one segment whose own math isn't anchor-relative-from-the-left like every other tag).
static inline void adsr_drag_update(AdsrEnvelopeState *e, double w, double h, double px, double py) {
    if (e->active_node < 0 || e->point_count == 0) return;
    const double margin = 10.0;
    double graph_h = h - margin * 2.0;
    double scale_x = e->scale_x > 0 ? e->scale_x : 100.0;

    double clamped_x = fmax(margin, fmin(px, w - margin));
    double clamped_y = fmax(margin, fmin(py, h - margin));
    double level_frac = fmax(0.0, fmin(1.0, 1.0 - ((clamped_y - margin) / graph_h)));

    AdsrPointTag tag = e->point_tag[e->active_node];
    double anchor_x = e->point_anchor_x[e->active_node];

    switch (tag) {
        case ADSR_TAG_START: e->attack_level = level_frac; break;
        case ADSR_TAG_DELAY: e->delay = fmax(0.001, (clamped_x - anchor_x) / scale_x); break;
        case ADSR_TAG_ATTACK: e->attack = fmax(0.001, (clamped_x - anchor_x) / scale_x); e->attack_peak = level_frac; break;
        case ADSR_TAG_HOLD: e->hold = fmax(0.001, (clamped_x - anchor_x) / scale_x); break;
        case ADSR_TAG_DECAY: e->decay = fmax(0.001, (clamped_x - anchor_x) / scale_x); e->sustain = level_frac; break;
        case ADSR_TAG_SUSTAIN: e->sustain = level_frac; break;
        case ADSR_TAG_RELEASE: e->release = fmax(0.001, (w - margin - clamped_x) / scale_x); break;
    }
}
