// hds_clap_play_draw.c: hds.clap's own Play tab -- the 4 oscillator strips (Shape/Pulse-Width-or-
// Tilt/Curve-Drive/Pitch-Offset/Fine/Phase/Pitch/Unison/Vol/Mute/Pan) plus their 4 dedicated Amp
// EGs, stacked BELOW the oscillator strips (one EG per column, directly under its own oscillator)
// rather than frontend.py's own side-by-side "Oscillators" group box + "Dedicated Amplitude
// Envelopes" group box -- a deliberate native-GUI layout departure, not a port of that arrangement
// (frontend.py's own two-group-box layout is still the reference for every CONTROL this file
// draws, just not for where the EG panels sit on screen). Platform-agnostic like hds_fx_gui_draw.c
// itself (plain cairo_t*, no Xlib) -- #include'd by clap_plugin.c after hds_clap_gui_draw.c and
// hds_clap_adsr.h.
//
// SCOPE for this first pass (task #99): Single-Program mode only -- every control here reads/
// writes p->knobs.programs[SINGLE_PROGRAM_SLOT] directly. Combi mode's own separate 8-lane mixer
// view (Level/Pan fader per lane, Program-file picker, MIDI channel, key/velocity zone, per-lane
// transpose -- an entirely different widget in frontend.py, `self.osc_combi_widget`, toggled by
// the Play tab's own Mode radio row) is real, substantial, separate work -- its own follow-up task
// -- so the Mode radio itself isn't built yet either (a toggle to a mode with no view built would
// be a dead control). Two more things frontend.py's Play tab has that are deliberately NOT here
// yet: the WaveformVisualizer preview (purely decorative, no backend field of its own) and the
// right-click "Modulate From" context menu on Pulse-Width/Phase/Pitch (assigns an LFO/assignable-EG
// as that knob's modulation source) -- the latter genuinely can't be built yet: it needs a list of
// real modulator sources, and the LFOs/Envelopes tabs that would supply that list don't exist in
// this native GUI yet either (see this repo's own task list -- they're next, in order).
//
// One deliberate widget-type simplification versus frontend.py, consistent with this whole native
// GUI's own established interaction language (see hds_fx_gui_draw.c's own header comment: "Mix/
// param knobs stay click-and-drag" -- there is no vertical-fader widget anywhere else in this GUI):
// every numeric control here (including Vol and the 6 Amp-EG time/level dials, which are Python
// QDial subclasses with their own log/cubic taper) is the exact same plain click-and-drag knob
// hds_fx_gui_draw.c's own Mix/param knobs already are, dragging LINEARLY over that field's own
// real backend-unit range -- same convention hds_fx_type_param_range()'s own knobs already use for
// every FX param, several of which are just as perceptually non-linear in real use (a filter
// cutoff, say) without a dedicated taper either. This changes how many drag-pixels one full sweep
// takes, never the real value range or type. Pitch Offset/Fine Tune (QSpinBoxes in frontend.py,
// not QDials) become knobs too, for the same one-interaction-model reason -- again the same real
// range, just dragged instead of typed/spun.

#include "hds_clap_adsr.h"

// ---------------------------------------------------------------------------------------------
// Per-oscillator field lookup -- ProgramState does NOT store its 4 oscillators as an array (see
// backend.c's own ProgramState: vco1_shape/vco2_shape/vco3_shape/vco4_shape are four independently
// -declared fields, not vco[4].shape), so every other piece of this file goes through this one
// switch instead of repeating it. Built once per render/interaction call, not cached -- it's just
// 14 pointer stores, and caching it would risk it going stale across a resize/mode change for no
// real benefit.
// ---------------------------------------------------------------------------------------------
typedef struct {
    float *shape, *vol, *pan, *mute, *fine_tune, *pitch_offset /* NULL for osc 0 */,
          *phase_offset, *pulse_width, *unison_detune, *unison_width,
          *pitch, *mono_noise, *vel_to_amp, *curve_drive, *force_own_amp_eg;
} HdsPlayOscFields;

static void hds_play_osc_fields(ProgramState *prog, int osc, HdsPlayOscFields *f) {
    memset(f, 0, sizeof(*f));
    switch (osc) {
        case 0:
            f->shape = &prog->vco1_shape; f->vol = &prog->vco1_vol; f->pan = &prog->vco1_pan;
            f->mute = &prog->vco1_mute; f->fine_tune = &prog->vco1_fine_tune;
            f->phase_offset = &prog->vco1_phase_offset; f->pulse_width = &prog->vco1_pulse_width;
            f->unison_detune = &prog->vco1_unison_saw_detune; f->unison_width = &prog->vco1_unison_saw_width;
            f->pitch = &prog->vco1_pitch; f->mono_noise = NULL; // Osc1 has no mono_noise field (see backend.c)
            f->vel_to_amp = &prog->vco1_vel_to_amp; f->curve_drive = &prog->vco1_curve_drive;
            f->force_own_amp_eg = &prog->vco1_force_own_amp_eg;
            break;
        case 1:
            f->shape = &prog->vco2_shape; f->vol = &prog->vco2_vol; f->pan = &prog->vco2_pan;
            f->mute = &prog->vco2_mute; f->fine_tune = &prog->vco2_fine_tune; f->pitch_offset = &prog->vco2_pitch_offset;
            f->phase_offset = &prog->vco2_phase_offset; f->pulse_width = &prog->vco2_pulse_width;
            f->unison_detune = &prog->vco2_unison_saw_detune; f->unison_width = &prog->vco2_unison_saw_width;
            f->pitch = &prog->vco2_pitch; f->mono_noise = NULL;
            f->vel_to_amp = &prog->vco2_vel_to_amp; f->curve_drive = &prog->vco2_curve_drive;
            f->force_own_amp_eg = &prog->vco2_force_own_amp_eg;
            break;
        case 2:
            f->shape = &prog->vco3_shape; f->vol = &prog->vco3_vol; f->pan = &prog->vco3_pan;
            f->mute = &prog->vco3_mute; f->fine_tune = &prog->vco3_fine_tune; f->pitch_offset = &prog->vco3_pitch_offset;
            f->phase_offset = &prog->vco3_phase_offset; f->pulse_width = &prog->vco3_pulse_width;
            f->unison_detune = &prog->vco3_unison_saw_detune; f->unison_width = &prog->vco3_unison_saw_width;
            f->pitch = &prog->vco3_pitch; f->mono_noise = &prog->vco3_mono_noise;
            f->vel_to_amp = &prog->vco3_vel_to_amp; f->curve_drive = &prog->vco3_curve_drive;
            f->force_own_amp_eg = &prog->vco3_force_own_amp_eg;
            break;
        default: // case 3
            f->shape = &prog->vco4_shape; f->vol = &prog->vco4_vol; f->pan = &prog->vco4_pan;
            f->mute = &prog->vco4_mute; f->fine_tune = &prog->vco4_fine_tune; f->pitch_offset = &prog->vco4_pitch_offset;
            f->phase_offset = &prog->vco4_phase_offset; f->pulse_width = &prog->vco4_pulse_width;
            f->unison_detune = &prog->vco4_unison_saw_detune; f->unison_width = &prog->vco4_unison_saw_width;
            f->pitch = &prog->vco4_pitch; f->mono_noise = &prog->vco4_mono_noise;
            f->vel_to_amp = &prog->vco4_vel_to_amp; f->curve_drive = &prog->vco4_curve_drive;
            f->force_own_amp_eg = &prog->vco4_force_own_amp_eg;
            break;
    }
}

static ProgramState *hds_play_program(HdsClapPlugin *p) { return &p->knobs.programs[SINGLE_PROGRAM_SLOT]; }

// ---------------------------------------------------------------------------------------------
// Shape combo -- OSC_SHAPE_NAMES from frontend.py, same indices as the WaveType enum (backend.c),
// load-bearing for saved patches so this order must never change.
// ---------------------------------------------------------------------------------------------
#define HDS_PLAY_SHAPE_COUNT 10
static const char *const HDS_PLAY_SHAPE_NAMES[HDS_PLAY_SHAPE_COUNT] = {
    "Saw", "Square", "Triangle", "Sine", "Unison Saw", "Noise", "Pulse", "Multisample", "Off", "Rev Saw",
};
#define HDS_PLAY_EGMODE_COUNT 7
static const char *const HDS_PLAY_EGMODE_NAMES[HDS_PLAY_EGMODE_COUNT] = {
    "ADSR", "AR", "AD", "ASR", "AHD", "DADSR", "AHDSR",
};

#define HDS_PLAY_DROPDOWN_NONE 0
#define HDS_PLAY_DROPDOWN_SHAPE 1
#define HDS_PLAY_DROPDOWN_EGMODE 2

// A knob's own right-click menu, if it has one -- stashed at right-press time (see
// hds_play_start_knob_drag()) and consulted on release if the button never actually dragged.
// kind==0 means "no menu": most knobs don't have one, exactly like frontend.py's own
// PotentiometerKnob, where setContextMenuCallback() is only ever wired onto ~17 specific knob
// instances (destination knobs: oscillator Pitch/Phase/Pulse-Width, Filter Cutoff/Resonance, LFO
// Rate/Depth/Phase) -- every other knob still gets double-click-reset and right-drag fine
// adjustment, just no menu. kind/arg are interpreted entirely by whichever *_draw.c file opened
// the menu (each file defines its own small kind-constant space); this struct itself stays
// generic so every tab can keep sharing ONE HdsPlayKnobDrag type.
typedef struct {
    int kind;
    int arg;
} HdsPlayKnobMenu;

typedef struct {
    float *target;      // knob currently being dragged, NULL = none
    double lo, hi;       // that knob's own real backend-unit range
    double start_y, start_val;

    // Right-click support (mirrors frontend.py's own PotentiometerKnob mousePressEvent/
    // mouseMoveEvent/mouseReleaseEvent exactly): button is 0 (idle), 1 (left, dragging at normal
    // sensitivity) or 3 (right -- either still a "was this a click or a drag" pending gesture, or
    // promoted to a fine-sensitivity drag once the pointer moves past the threshold). press_x/y is
    // where the right button went down; right_moved flips true the first time the pointer clears
    // HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX from there (frontend.py's own manhattanLength() > 3 check).
    // A right-release with right_moved still false fires `menu` (if it has one) instead of having
    // done anything to the value -- a real "was this a click or a drag" gesture, not a fixed
    // click-vs-hold timer.
    int button;
    double press_x, press_y;
    bool right_moved;
    HdsPlayKnobMenu menu;

    // Double-click-to-reset bookkeeping (mirrors PotentiometerKnob.mouseDoubleClickEvent, which
    // always resets to a literal 0.0 regardless of the knob's own range -- clamped into range same
    // as any other write). last_click_target is compared by POINTER IDENTITY (which knob), not
    // value, so two different knobs' clicks never combine into a false double-click.
    float *last_click_target;
    struct timespec last_click_time;
    double last_click_x, last_click_y;
} HdsPlayKnobDrag;

static bool hds_play_rect_contains(const HdsFxRect *r, double x, double y); // defined below

// Generic right-click popup menu (checkable rows, optional section headers) -- reused by every
// tab that ports one of frontend.py's "Modulate From"/Key Track/Mod Source menus. kind/arg are
// opaque here; each opening file defines its own kind constants and rebuilds the same row list on
// every render/hit-test call (like every other dropdown in this GUI) rather than caching it.
#define HDS_CTXMENU_ROW_H 16.0
#define HDS_CTXMENU_SECTION_H 14.0
#define HDS_CTXMENU_W 160.0
#define HDS_CTXMENU_MAX_ROWS 24

typedef struct {
    bool is_section, checkable, checked, disabled;
    char label[40];
    int group, index; // meaning is up to whichever file built this row
} HdsCtxMenuRow;

typedef struct {
    bool open;
    double x, y;
    int kind, arg;
} HdsCtxMenu;

static void hds_ctxmenu_add(HdsCtxMenuRow *rows, int *count, bool is_section, bool checkable, bool checked, bool disabled, int group, int index, const char *label) {
    if (*count >= HDS_CTXMENU_MAX_ROWS) return;
    HdsCtxMenuRow *r = &rows[(*count)++];
    r->is_section = is_section; r->checkable = checkable; r->checked = checked; r->disabled = disabled;
    r->group = group; r->index = index;
    snprintf(r->label, sizeof(r->label), "%s", label);
}

static double hds_ctxmenu_total_h(const HdsCtxMenuRow *rows, int count) {
    double h = 4.0;
    for (int i = 0; i < count; i++) h += rows[i].is_section ? HDS_CTXMENU_SECTION_H : HDS_CTXMENU_ROW_H;
    return h + 4.0;
}

static void hds_ctxmenu_origin(const HdsCtxMenu *m, double total_h, double win_w, double win_h, double *out_x, double *out_y) {
    double mx = m->x, my = m->y;
    if (mx + HDS_CTXMENU_W > win_w) mx = win_w - HDS_CTXMENU_W;
    if (my + total_h > win_h) my = win_h - total_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    *out_x = mx; *out_y = my;
}

static void hds_ctxmenu_row_rect(const HdsCtxMenu *m, const HdsCtxMenuRow *rows, int count, int i, double win_w, double win_h, HdsFxRect *out) {
    double total_h = hds_ctxmenu_total_h(rows, count);
    double mx, my; hds_ctxmenu_origin(m, total_h, win_w, win_h, &mx, &my);
    double y = my + 4.0;
    for (int k = 0; k < i; k++) y += rows[k].is_section ? HDS_CTXMENU_SECTION_H : HDS_CTXMENU_ROW_H;
    out->x = mx; out->w = HDS_CTXMENU_W;
    out->y = y; out->h = rows[i].is_section ? HDS_CTXMENU_SECTION_H : HDS_CTXMENU_ROW_H;
}

static void hds_ctxmenu_render(cairo_t *cr, const HdsCtxMenu *m, const HdsCtxMenuRow *rows, int count, double win_w, double win_h) {
    if (!m->open || count == 0) return;
    double total_h = hds_ctxmenu_total_h(rows, count);
    double mx, my; hds_ctxmenu_origin(m, total_h, win_w, win_h, &mx, &my);
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, mx, my, HDS_CTXMENU_W, total_h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, mx + 0.5, my + 0.5, HDS_CTXMENU_W - 1, total_h - 1); cairo_stroke(cr);
    for (int i = 0; i < count; i++) {
        HdsFxRect r; hds_ctxmenu_row_rect(m, rows, count, i, win_w, win_h, &r);
        const HdsCtxMenuRow *row = &rows[i];
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, row->is_section ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, row->is_section ? 8.0 : 9.0);
        if (row->is_section) { hds_fx_rgb(cr, 0.55, 0.55, 0.6); hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.0, row->label); continue; }
        if (row->checked) { hds_fx_rgb(cr, 0.18, 0.32, 0.36); cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr); }
        double c1 = row->disabled ? 0.4 : (row->checked ? 0.92 : 0.78), c3 = row->disabled ? 0.42 : (row->checked ? 0.92 : 0.81);
        hds_fx_rgb(cr, c1, c1 + (row->checked && !row->disabled ? 0.03 : 0.0), c3);
        char buf[48];
        if (row->checkable) snprintf(buf, sizeof(buf), "%s %s", row->checked ? "[x]" : "[ ]", row->label);
        else snprintf(buf, sizeof(buf), "%s", row->label);
        hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.0, buf);
    }
}

// -1 if no row (or a section header) was hit.
static int hds_ctxmenu_hit_row(const HdsCtxMenu *m, const HdsCtxMenuRow *rows, int count, double win_w, double win_h, double x, double y) {
    if (!m->open) return -1;
    for (int i = 0; i < count; i++) {
        if (rows[i].is_section) continue;
        HdsFxRect r; hds_ctxmenu_row_rect(m, rows, count, i, win_w, win_h, &r);
        if (hds_play_rect_contains(&r, x, y)) return i;
    }
    return -1;
}

// The 100-entry Destination list (matrix_shared_routes in frontend.py) -- shared by every LFO row's
// own Destination pill (hds_clap_lfos_draw.c) AND every assignable EG's own eg_dest_target
// (hds_clap_envelopes_draw.c), plus this file's own "Modulate From" menu content below (which shows
// an already-pointed-elsewhere source's current destination inline). Lives here, in an
// EARLIER-included file, rather than in hds_clap_lfos_draw.c where it used to live, purely because
// of the fixed single-TU #include order (see clap_plugin.c/hds_standalone.c): this file's own
// "Modulate From" builder needs it, and can't reach forward into a later-included file. Indices
// 84-89 are retired placeholders from a pre-simplification design (kept only so a target ID already
// saved in an old patch still resolves to SOME row of this same list) -- shown as "--" and, since
// nothing meaningful would happen selecting them, simply not offered for selection. Indices 8-13
// used to be retired the exact same way but were reclaimed for Osc 3/4 Fine and Osc 1-4 Curve (see
// HDS_PLAY_FINE_DEST_TARGET/HDS_PLAY_CURVE_DEST_TARGET's own comment) -- precisely the kind of reuse
// this "keep the number reserved, never delete it" convention exists to make safe.
#define HDS_LFO_DEST_COUNT 100
static void hds_lfo_dest_label(int target, char *buf, size_t buflen) {
    static const char *const fixed[48] = {
        "None", "Osc 1 Pitch", "Osc 2 Pitch", "Osc 1+2 Pitch", "O1 Fine", "O2 Fine", "Osc 1 Amp", "Osc 2 Amp",
        "O3 Fine", "O4 Fine", "Osc 1 Curve", "Osc 2 Curve", "Osc 3 Curve", "Osc 4 Curve", // 8-13: reclaimed from "retired"
        "LFO 1 Rate", "LFO 2 Rate", "LFO 3 Rate", "LFO 4 Rate",
        "LFO 1 Dp", "LFO 2 Dp", "LFO 3 Dp", "LFO 4 Dp",
        "FM Depth", "Mix Balance",
        "Osc 1 Phase", "Osc 2 Phase",
        "O1 Unison Detune", "O2 Unison Detune",
        "LFO 1 Phase", "LFO 2 Phase", "LFO 3 Phase", "LFO 4 Phase",
        "Osc 1 Pan", "Osc 2 Pan",
        "Osc 1 Pulse Width", "Osc 2 Pulse Width",
        "Osc 3 Amp", "Osc 3 Pan", "Osc 3 Tilt",
        "Osc 4 Amp", "Osc 4 Pan",
        "Osc 3 Pitch", "Osc 4 Pitch",
        "Osc 3 Phase", "Osc 4 Phase",
        "Osc 3 Unison Detune", "Osc 4 Unison Detune",
        "Osc 4 Tilt",
    };
    if (target < 0 || target >= HDS_LFO_DEST_COUNT) { snprintf(buf, buflen, "?"); return; }
    if (target < 48) { snprintf(buf, buflen, "%s", fixed[target]); return; }
    if (target < 60) { snprintf(buf, buflen, "LFO %d Rate", target - 48 + 5); return; }
    if (target < 72) { snprintf(buf, buflen, "LFO %d Dp", target - 60 + 5); return; }
    if (target < 84) { snprintf(buf, buflen, "LFO %d Phase", target - 72 + 5); return; }
    if (target < 90) { snprintf(buf, buflen, "--"); return; }
    int f = (target - 90) / 2;
    if ((target - 90) % 2 == 0) snprintf(buf, buflen, "Filter %d Freq", f + 1);
    else snprintf(buf, buflen, "Filter %d Res", f + 1);
}
// Rate/Depth/Phase's own right-click "Modulate From" menu target numbers (LFOs tab) and the same
// LFO's own Destination-dropdown target (below) both need these, mirroring frontend.py's own
// _lfo_rate_dest_target/_lfo_depth_dest_target/_lfo_phase_dest_target exactly (two ranges each:
// LFO 1-4's original slots, LFO 5-16 appended after target 47).
static int hds_lfo_rate_dest_target(int idx) { return idx < 4 ? 14 + idx : 44 + idx; }
static int hds_lfo_depth_dest_target(int idx) { return idx < 4 ? 18 + idx : 56 + idx; }
static int hds_lfo_phase_dest_target(int idx) { return idx < 4 ? 28 + idx : 68 + idx; }

// ---------------------------------------------------------------------------------------------
// The Destination dropdown's shared DISPLAY order -- grouped by what a target actually modulates
// (Oscillator Pitch/Fine-Detune/Phase/Pulse-Width/Amp-Pan-Tilt, Mix, Filters, LFO Rate/Depth/
// Phase), with a section header before each group, rather than frontend.py's own flat ascending
// target-number order (0, 1, 2, 3, ... 99) -- pure display reorganization: the STORED value
// (dest_target / eg_dest_target) is still the exact same target number hds_lfo_dest_label() above
// already returns a label for, so an existing saved patch resolves identically either way. The 6
// still-retired placeholder targets (84-89 -- shown as "--" and never selectable) are dropped from
// this list entirely instead of appearing as blank, unlabeled rows (8-13 no longer belong to this
// set -- see hds_lfo_dest_label()'s own comment on why they were reclaimed). Shared by the LFOs tab's own
// Destination pill and the Envelopes tab's own assignable-EG Destination pill (hds_dest_list_render/
// hds_dest_list_hit_test below) -- rebuilt fresh on every render/hit-test call, same "cheap, always
// consistent, never a stale cache" convention every other dropdown in this GUI already uses.
// ---------------------------------------------------------------------------------------------
#define HDS_DEST_LIST_MAX_ROWS 100
#define HDS_DEST_LIST_ROW_H 17.0
#define HDS_DEST_LIST_SECTION_H 14.0
#define HDS_DEST_LIST_MAX_H 340.0

typedef struct {
    bool is_section;
    int target;         // meaningless when is_section is true
    const char *label;  // section title when is_section; NULL otherwise (hds_lfo_dest_label(target) is used)
} HdsDestListRow;

static void hds_dest_list_add(HdsDestListRow *rows, int *count, bool is_section, int target, const char *label) {
    if (*count >= HDS_DEST_LIST_MAX_ROWS) return;
    rows[(*count)++] = (HdsDestListRow){is_section, target, label};
}

static void hds_dest_list_build(HdsDestListRow *rows, int *count) {
    *count = 0;
    hds_dest_list_add(rows, count, false, 0, NULL); // None -- always first, no section of its own

    hds_dest_list_add(rows, count, true, -1, "Oscillator Pitch");
    static const int pitch[] = {1, 2, 41, 42, 3};
    for (size_t i = 0; i < sizeof(pitch) / sizeof(pitch[0]); i++) hds_dest_list_add(rows, count, false, pitch[i], NULL);

    hds_dest_list_add(rows, count, true, -1, "Oscillator Fine / Detune");
    static const int fine[] = {4, 5, 8, 9, 26, 27, 45, 46};
    for (size_t i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) hds_dest_list_add(rows, count, false, fine[i], NULL);

    hds_dest_list_add(rows, count, true, -1, "Oscillator Curve / Drive");
    static const int curve[] = {10, 11, 12, 13};
    for (size_t i = 0; i < sizeof(curve) / sizeof(curve[0]); i++) hds_dest_list_add(rows, count, false, curve[i], NULL);

    hds_dest_list_add(rows, count, true, -1, "Oscillator Phase");
    static const int phase[] = {24, 25, 43, 44};
    for (size_t i = 0; i < sizeof(phase) / sizeof(phase[0]); i++) hds_dest_list_add(rows, count, false, phase[i], NULL);

    hds_dest_list_add(rows, count, true, -1, "Oscillator Pulse Width");
    hds_dest_list_add(rows, count, false, 34, NULL);
    hds_dest_list_add(rows, count, false, 35, NULL);

    hds_dest_list_add(rows, count, true, -1, "Oscillator Amp / Pan / Tilt");
    static const int amp[] = {6, 7, 36, 39, 32, 33, 37, 40, 38, 47};
    for (size_t i = 0; i < sizeof(amp) / sizeof(amp[0]); i++) hds_dest_list_add(rows, count, false, amp[i], NULL);

    hds_dest_list_add(rows, count, true, -1, "Mix");
    hds_dest_list_add(rows, count, false, 22, NULL);
    hds_dest_list_add(rows, count, false, 23, NULL);

    hds_dest_list_add(rows, count, true, -1, "Filters");
    for (int f = 0; f < 5; f++) {
        hds_dest_list_add(rows, count, false, 90 + f * 2, NULL);
        hds_dest_list_add(rows, count, false, 91 + f * 2, NULL);
    }

    hds_dest_list_add(rows, count, true, -1, "LFO Rate");
    for (int i = 0; i < LFO_COUNT; i++) hds_dest_list_add(rows, count, false, hds_lfo_rate_dest_target(i), NULL);

    hds_dest_list_add(rows, count, true, -1, "LFO Depth");
    for (int i = 0; i < LFO_COUNT; i++) hds_dest_list_add(rows, count, false, hds_lfo_depth_dest_target(i), NULL);

    hds_dest_list_add(rows, count, true, -1, "LFO Phase");
    for (int i = 0; i < LFO_COUNT; i++) hds_dest_list_add(rows, count, false, hds_lfo_phase_dest_target(i), NULL);
}

// Renders the grouped list into `bg` (already positioned/sized by the caller's own anchor logic),
// clamping *scroll_px into range first exactly like the old flat-list renderer did. `current` is
// the field's OWN CURRENTLY STORED target (for highlighting), never a display index.
static void hds_dest_list_render(cairo_t *cr, const HdsFxRect *bg, double *scroll_px, int current) {
    HdsDestListRow rows[HDS_DEST_LIST_MAX_ROWS]; int count = 0;
    hds_dest_list_build(rows, &count);

    double total_h = 0.0;
    for (int i = 0; i < count; i++) total_h += rows[i].is_section ? HDS_DEST_LIST_SECTION_H : HDS_DEST_LIST_ROW_H;
    double max_scroll = total_h - bg->h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    if (*scroll_px < 0.0) *scroll_px = 0.0;
    if (*scroll_px > max_scroll) *scroll_px = max_scroll;

    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, bg->x, bg->y, bg->w, bg->h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg->x + 0.5, bg->y + 0.5, bg->w - 1, bg->h - 1); cairo_stroke(cr);

    cairo_save(cr);
    cairo_rectangle(cr, bg->x, bg->y, bg->w, bg->h);
    cairo_clip(cr);
    double y = bg->y - *scroll_px;
    for (int i = 0; i < count; i++) {
        double row_h = rows[i].is_section ? HDS_DEST_LIST_SECTION_H : HDS_DEST_LIST_ROW_H;
        if (y + row_h < bg->y) { y += row_h; continue; }
        if (y > bg->y + bg->h) break;
        if (rows[i].is_section) {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 8.5);
            hds_fx_rgb(cr, 0.5, 0.5, 0.55);
            hds_fx_left_text(cr, bg->x + 4, y + row_h / 2 + 3.0, rows[i].label);
        } else {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 9.5);
            char label[24]; hds_lfo_dest_label(rows[i].target, label, sizeof(label));
            bool is_current = (rows[i].target == current);
            if (is_current) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, bg->x, y, bg->w, row_h); cairo_fill(cr);
            }
            hds_fx_rgb(cr, is_current ? 0.92 : 0.78, is_current ? 0.95 : 0.78, is_current ? 0.92 : 0.81);
            hds_fx_left_text(cr, bg->x + 4, y + row_h / 2 + 3.0, label);
        }
        y += row_h;
    }
    cairo_restore(cr);
}

// Returns true and writes *out_target if (x,y) landed on a real (non-section) row inside `bg`;
// false (no write) for a miss outside `bg` or a hit on a section header -- clicking a header is a
// no-op, same convention hds_ctxmenu_hit_row() already uses for its own section rows.
static bool hds_dest_list_hit_test(const HdsFxRect *bg, double scroll_px, double x, double y, int *out_target) {
    if (!hds_play_rect_contains(bg, x, y)) return false;
    HdsDestListRow rows[HDS_DEST_LIST_MAX_ROWS]; int count = 0;
    hds_dest_list_build(rows, &count);
    double ry = bg->y - scroll_px;
    for (int i = 0; i < count; i++) {
        double row_h = rows[i].is_section ? HDS_DEST_LIST_SECTION_H : HDS_DEST_LIST_ROW_H;
        if (y >= ry && y < ry + row_h) {
            if (rows[i].is_section) return false;
            *out_target = rows[i].target;
            return true;
        }
        ry += row_h;
    }
    return false;
}

// Generic "Modulate From" menu content -- mirrors frontend.py's own _populate_modulate_from_menu()
// exactly: every LFO (16) + every assignable EG (6), each toggling that source's own Destination
// field (PackLfoParams.dest_target / ProgramState.eg_dest_target[]) to `target`, or back to 0/None
// if it was already pointed there. group 0 = LFO index, group 1 = assignable-EG index (see
// hds_play_modsource_dest_ptr() below) -- opaque to HdsCtxMenuRow itself, meaningful only here.
// Self-modulation (an LFO/EG targeting its own Rate/Depth/Phase) is allowed, same as frontend.py.
static float *hds_play_modsource_dest_ptr(ProgramState *prog, int group, int index) {
    return (group == 0) ? &prog->lfo[index].dest_target : &prog->eg_dest_target[index];
}

// Appends (never resets *count -- see hds_play_build_modulate_from_rows() below for the
// reset-and-call-this wrapper every plain "Modulate From"-only knob uses) the same section every
// kind of "Modulate From" menu ends with.
static void hds_play_append_modulate_from_rows(HdsClapPlugin *p, int target, HdsCtxMenuRow *rows, int *count) {
    ProgramState *prog = hds_play_program(p);
    hds_ctxmenu_add(rows, count, true, false, false, false, 0, 0, "Modulate From");
    for (int i = 0; i < LFO_COUNT; i++) {
        int cur = (int)prog->lfo[i].dest_target;
        bool checked = (cur == target);
        char label[40];
        if (!checked && cur > 0 && cur < HDS_LFO_DEST_COUNT) {
            char dl[24]; hds_lfo_dest_label(cur, dl, sizeof(dl));
            snprintf(label, sizeof(label), "LFO %d  (-> %s)", i + 1, dl);
        } else snprintf(label, sizeof(label), "LFO %d", i + 1);
        hds_ctxmenu_add(rows, count, false, true, checked, false, 0, i, label);
    }
    for (int i = 0; i < ASSIGNABLE_EG_COUNT; i++) {
        int cur = (int)prog->eg_dest_target[i];
        bool checked = (cur == target);
        char label[40];
        if (!checked && cur > 0 && cur < HDS_LFO_DEST_COUNT) {
            char dl[24]; hds_lfo_dest_label(cur, dl, sizeof(dl));
            snprintf(label, sizeof(label), "EG %d  (-> %s)", i + 1, dl);
        } else snprintf(label, sizeof(label), "EG %d", i + 1);
        hds_ctxmenu_add(rows, count, false, true, checked, false, 1, i, label);
    }
}

static void hds_play_build_modulate_from_rows(HdsClapPlugin *p, int target, HdsCtxMenuRow *rows, int *count) {
    *count = 0;
    hds_play_append_modulate_from_rows(p, target, rows, count);
}

static void hds_play_toggle_modulate_from(HdsClapPlugin *p, int target, int group, int index, bool was_checked) {
    float *dest = hds_play_modsource_dest_ptr(hds_play_program(p), group, index);
    *dest = was_checked ? 0.0f : (float)target;
}

// Common "None/Mod Wheel/Breath/Foot/Expression/Channel Aftertouch" preset list -- mirrors
// frontend.py's own _MOD_SOURCE_PRESETS exactly, shared by LFO Depth's own "Modulation Source
// (CC)" section and Filter Cutoff's own "Modulation Source (ext MIDI)" section below. Both fields
// this sets (PackLfoParams.mod_source_cc / FilterParams.cutoff_mod_source_cc) share the same
// -1=none/0-127=CC/128=aftertouch convention. frontend.py also offers a free-text "Custom CC..."
// entry via a Qt input dialog -- this GUI has no text-entry widget anywhere (same disclosed-scope
// boundary as Program Load/Save, see hds_clap_global_draw.c's own header comment), so a CC already
// set that way from the Python GUI keeps whatever value it has, just without its own checked row
// here; real follow-up work if a non-preset CC picker is ever needed.
typedef struct { const char *label; float value; } HdsPresetOption;
#define HDS_MOD_SOURCE_PRESET_COUNT 6
static const HdsPresetOption HDS_MOD_SOURCE_PRESETS[HDS_MOD_SOURCE_PRESET_COUNT] = {
    {"None", -1.0f}, {"Mod Wheel (CC1)", 1.0f}, {"Breath Controller (CC2)", 2.0f},
    {"Foot Controller (CC4)", 4.0f}, {"Expression (CC11)", 11.0f}, {"Channel Aftertouch", 128.0f},
};

// Filter Cutoff's own "Key Track" preset list -- mirrors frontend.py's own _KEY_TRACK_PRESETS.
#define HDS_KEY_TRACK_PRESET_COUNT 4
static const HdsPresetOption HDS_KEY_TRACK_PRESETS[HDS_KEY_TRACK_PRESET_COUNT] = {
    {"Off", 0.0f}, {"50%", 50.0f}, {"100%", 100.0f}, {"150%", 150.0f},
};

// Filter Cutoff's own right-click target numbers (Resonance's own menu, plain "Modulate From"
// only, computes its own inline in hds_clap_filters_draw.c -- no shared section needs it there).
// Lives here (an earlier-included file), not in hds_clap_filters_draw.c, because
// hds_play_build_filter_cutoff_menu_rows() below needs it too -- same fixed single-TU #include-
// order constraint as hds_lfo_dest_label() above.
static int hds_filt_cutoff_dest_target(int idx) { return 90 + idx * 2; }

// group/index values these two menus' own preset rows use, distinct from Modulate From's own
// group 0 (LFO)/1 (EG) -- meaningful only to hds_play_ctxmenu_handle_click() below.
#define HDS_CTXROW_GROUP_LFO_MODSOURCE 2
#define HDS_CTXROW_GROUP_FILTER_KEYTRACK 3
#define HDS_CTXROW_GROUP_FILTER_MODSOURCE 4

// LFO Depth's own right-click menu: "Modulation Source (CC)" (mod_source_cc, a live-MIDI
// multiplicative scale on this LFO's own output) THEN the generic "Modulate From" section --
// mirrors frontend.py's own _show_lfo_row_menu's 'depth' case exactly (same two-section shape).
// `lfo_idx` doubles as both which LFO's own mod_source_cc to read/write AND (via
// hds_lfo_depth_dest_target()) the Modulate From section's own target number.
static void hds_play_build_lfo_depth_menu_rows(HdsClapPlugin *p, int lfo_idx, HdsCtxMenuRow *rows, int *count) {
    *count = 0;
    PackLfoParams *lfo = &hds_play_program(p)->lfo[lfo_idx];
    hds_ctxmenu_add(rows, count, true, false, false, false, 0, 0, "Modulation Source (CC)");
    for (int i = 0; i < HDS_MOD_SOURCE_PRESET_COUNT; i++) {
        bool checked = (lfo->mod_source_cc == HDS_MOD_SOURCE_PRESETS[i].value);
        hds_ctxmenu_add(rows, count, false, true, checked, false, HDS_CTXROW_GROUP_LFO_MODSOURCE, i, HDS_MOD_SOURCE_PRESETS[i].label);
    }
    hds_play_append_modulate_from_rows(p, hds_lfo_depth_dest_target(lfo_idx), rows, count);
}

// Filter Cutoff's own right-click menu: "Key Track" preset list, THEN "Modulation Source (ext
// MIDI)" (cutoff_mod_source_cc, an additive cutoff offset -- see FilterParams' own comment in
// backend.c), THEN the generic "Modulate From" section -- mirrors frontend.py's own
// _show_filter_cutoff_menu exactly (same three-section shape). `filt_idx` doubles as which
// filter's own key_track/cutoff_mod_source_cc to read/write AND (via
// hds_filt_cutoff_dest_target()) the Modulate From section's own target number.
static void hds_play_build_filter_cutoff_menu_rows(HdsClapPlugin *p, int filt_idx, HdsCtxMenuRow *rows, int *count) {
    *count = 0;
    FilterParams *filt = &hds_play_program(p)->filters[filt_idx];
    hds_ctxmenu_add(rows, count, true, false, false, false, 0, 0, "Key Track");
    for (int i = 0; i < HDS_KEY_TRACK_PRESET_COUNT; i++) {
        bool checked = (filt->key_track == HDS_KEY_TRACK_PRESETS[i].value);
        hds_ctxmenu_add(rows, count, false, true, checked, false, HDS_CTXROW_GROUP_FILTER_KEYTRACK, i, HDS_KEY_TRACK_PRESETS[i].label);
    }
    hds_ctxmenu_add(rows, count, true, false, false, false, 0, 0, "Modulation Source (ext MIDI)");
    for (int i = 0; i < HDS_MOD_SOURCE_PRESET_COUNT; i++) {
        bool checked = (filt->cutoff_mod_source_cc == HDS_MOD_SOURCE_PRESETS[i].value);
        hds_ctxmenu_add(rows, count, false, true, checked, false, HDS_CTXROW_GROUP_FILTER_MODSOURCE, i, HDS_MOD_SOURCE_PRESETS[i].label);
    }
    hds_play_append_modulate_from_rows(p, hds_filt_cutoff_dest_target(filt_idx), rows, count);
}

#define HDS_CTXMENU_KIND_MODULATE_FROM 1
#define HDS_CTXMENU_KIND_LFO_DEPTH 2
#define HDS_CTXMENU_KIND_FILTER_CUTOFF 3

// Builds whichever row list `menu->kind` needs -- the one place both rendering (draw whatever's
// open) and hit-testing (below) get their content from, so the two can never disagree.
static void hds_play_build_ctxmenu_rows(HdsClapPlugin *p, const HdsCtxMenu *menu, HdsCtxMenuRow *rows, int *count) {
    switch (menu->kind) {
        case HDS_CTXMENU_KIND_MODULATE_FROM: hds_play_build_modulate_from_rows(p, menu->arg, rows, count); break;
        case HDS_CTXMENU_KIND_LFO_DEPTH: hds_play_build_lfo_depth_menu_rows(p, menu->arg, rows, count); break;
        case HDS_CTXMENU_KIND_FILTER_CUTOFF: hds_play_build_filter_cutoff_menu_rows(p, menu->arg, rows, count); break;
        default: *count = 0; break;
    }
}

// Every tab file with a ctx_menu field calls this from its own on_button_press, right after its
// existing dropdown check (same "consume the click, open list wins" precedence) -- closes the menu
// unconditionally (a click anywhere, hit or miss, dismisses it, same as every dropdown in this GUI)
// and applies whatever the hit row means first if one was hit. A preset row (Key Track/either
// Modulation Source list) always SETS its own value on click, never toggles back off -- matching
// frontend.py's own plain (not checkable-toggle) QAction wiring for those two menus.
static bool hds_play_ctxmenu_handle_click(HdsClapPlugin *p, HdsCtxMenu *menu, double win_w, double win_h, double x, double y) {
    if (!menu->open) return false;
    HdsCtxMenuRow rows[HDS_CTXMENU_MAX_ROWS]; int count = 0;
    hds_play_build_ctxmenu_rows(p, menu, rows, &count);
    int hit = hds_ctxmenu_hit_row(menu, rows, count, win_w, win_h, x, y);
    if (hit >= 0) {
        HdsCtxMenuRow *r = &rows[hit];
        if (r->group == 0 || r->group == 1) {
            int target = (menu->kind == HDS_CTXMENU_KIND_LFO_DEPTH) ? hds_lfo_depth_dest_target(menu->arg)
                       : (menu->kind == HDS_CTXMENU_KIND_FILTER_CUTOFF) ? hds_filt_cutoff_dest_target(menu->arg)
                       : menu->arg;
            hds_play_toggle_modulate_from(p, target, r->group, r->index, r->checked);
        } else if (r->group == HDS_CTXROW_GROUP_LFO_MODSOURCE) {
            hds_play_program(p)->lfo[menu->arg].mod_source_cc = HDS_MOD_SOURCE_PRESETS[r->index].value;
        } else if (r->group == HDS_CTXROW_GROUP_FILTER_KEYTRACK) {
            hds_play_program(p)->filters[menu->arg].key_track = HDS_KEY_TRACK_PRESETS[r->index].value;
        } else if (r->group == HDS_CTXROW_GROUP_FILTER_MODSOURCE) {
            hds_play_program(p)->filters[menu->arg].cutoff_mod_source_cc = HDS_MOD_SOURCE_PRESETS[r->index].value;
        }
    }
    menu->open = false;
    return true;
}

typedef struct {
    int dropdown_kind;         // HDS_PLAY_DROPDOWN_* -- one at a time, same convention as the FX tab's own
    int dropdown_index;        // which oscillator (0-3, SHAPE) or which Amp EG (0-3, EGMODE)
    HdsFxRect dropdown_anchor;

    HdsPlayKnobDrag knob_drag;
    HdsCtxMenu ctx_menu;        // the open "Modulate From" menu (if any) from a right-click-without-
                                 // drag release on Pitch/Phase/Pulse-Width -- see knob_drag.menu

    int adsr_drag_eg;           // -1 = not dragging any Amp EG curve; else 0-3
    int adsr_active_node[4];    // per-EG "which node is highlighted/being dragged", -1 = none

    bool amp_eg_live_sync;      // "Sync" toggle -- while true, every change to Amp EG 0 (Osc1) is
                                 // immediately mirrored onto Amp EGs 1-3, which are drawn disabled
} HdsClapPlayViewState;

static void hds_clap_play_view_state_init(HdsClapPlayViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->dropdown_kind = HDS_PLAY_DROPDOWN_NONE;
    vs->adsr_drag_eg = -1;
    for (int i = 0; i < 4; i++) vs->adsr_active_node[i] = -1;
}

// ---------------------------------------------------------------------------------------------
// Layout -- one function per region computes every sub-control's rect, shared by render and
// interaction so they can never disagree, same discipline hds_fx_gui_draw.c's own
// hds_fx_gui_compute_row_layout() already established.
// ---------------------------------------------------------------------------------------------
#define HDS_PLAY_MARGIN 10.0
// 130, not the original 92: each Amp EG panel below shares this same column width (see
// hds_play_osc_col_x()/hds_play_render_eg_panel()'s own call site), and the widest topologies
// (DADSR/AHDSR) show 5 dials at once -- 92px left them scrunched together with overlapping labels.
#define HDS_PLAY_OSC_COL_W 145.0
#define HDS_PLAY_KNOB_R 10.0
#define HDS_PLAY_KNOB_STRIDE 46.0 // vertical space one knob (label+arc+value) reserves
#define HDS_PLAY_PILL_H 18.0
#define HDS_PLAY_CHECK_H 15.0
// 46.0, not 30.0: leaves an 8px-margin, 16px-tall strip at the very top of the tab for the Mode
// toggle (Single/Combi, see hds_clap_play_mode_toggle_rects()/hds_clap_gui_draw.c's own
// HDS_CLAP_TAB_PLAY dispatch, which draws/hit-tests it and switches to hds_clap_combi_draw.c's own
// 8-lane mixer view when Combi is selected) above every oscillator/EG section header.
#define HDS_PLAY_CONTENT_TOP 52.0 // below this file's own "Oscillators"/"Amp EGs" section labels

// Single/Combi Mode toggle -- drawn/hit-tested by hds_clap_gui_draw.c's own HDS_CLAP_TAB_PLAY
// dispatch (not this file's own render/press functions), since switching to Combi mode means
// dispatching to a COMPLETELY different tab file (hds_clap_combi_draw.c) instead of this one, and
// hds_clap_gui_draw.c is the one place both are visible (it's #include'd after both). Reads/writes
// p->knobs.performance_mode directly (0=Single, 1=Combi) -- Sequencer (2) is deliberately never
// reachable from this native GUI (see clap_plugin.c's own task #83 comment: this plugin's
// process() never runs Sequencer-mode transport/lane playback at all).
static void hds_clap_play_mode_toggle_rects(HdsFxRect *single_r, HdsFxRect *combi_r) {
    *single_r = (HdsFxRect){HDS_PLAY_MARGIN, 4.0, 70.0, 16.0};
    *combi_r  = (HdsFxRect){HDS_PLAY_MARGIN + 74.0, 4.0, 70.0, 16.0};
}

typedef struct {
    HdsFxRect shape_pill;
    HdsFxRect sfz_load_btn;  // Multisample shape only -- see hds_play_compute_osc_layout()'s own comment
    HdsFxRect force_own_amp_eg_chk; // Multisample shape only -- see ProgramState.vco1_force_own_amp_eg's own comment
    HdsFxRect pulse_knob;    // Pulse Width (Pulse shape) or Tilt (Noise shape)
    HdsFxRect mono_chk;      // Noise shape only
    HdsFxRect curve_knob;    // Curve (Saw/Triangle/RevSaw) or Drive (Sine)
    HdsFxRect pitch_off_knob;// osc > 0 only
    HdsFxRect fine_knob;
    HdsFxRect phase_knob;
    HdsFxRect pitch_knob;
    HdsFxRect uni_width_knob;  // Unison Saw shape only
    HdsFxRect uni_detune_knob; // Unison Saw shape only
    HdsFxRect vol_knob;
    HdsFxRect mute_btn;
    HdsFxRect pan_knob;
    double col_bottom_y; // where this column's own controls end (for a future scroll-height check)
} HdsPlayOscLayout;

static void hds_play_compute_osc_layout(int osc, int shape, double col_x, double top_y, HdsPlayOscLayout *out) {
    memset(out, 0, sizeof(*out));
    double cx = col_x + HDS_PLAY_OSC_COL_W / 2.0;
    double y = top_y;

    out->shape_pill = (HdsFxRect){col_x, y, HDS_PLAY_OSC_COL_W, HDS_PLAY_PILL_H};
    y += HDS_PLAY_PILL_H + 4.0;

    // Multisample's own .sfz loader -- a real button, not a knob, so it gets its own full-width row
    // right below the Shape pill exactly like Noise's Mono checkbox does below Pulse Width's own
    // knob. See hds_clap_play_on_button_press()'s own comment for what clicking it actually does.
    if (shape == WAVE_SAMPLE) {
        out->sfz_load_btn = (HdsFxRect){col_x, y, HDS_PLAY_OSC_COL_W, HDS_PLAY_CHECK_H};
        y += HDS_PLAY_CHECK_H + 4.0;
        // SFZ per-zone amp envelope override checkbox -- only meaningful (and only shown) while
        // this oscillator is actually Multisample, same posture as the .sfz loader button right
        // above it. See ProgramState.vco1_force_own_amp_eg's own comment for what it does.
        out->force_own_amp_eg_chk = (HdsFxRect){col_x, y, HDS_PLAY_OSC_COL_W, HDS_PLAY_CHECK_H};
        y += HDS_PLAY_CHECK_H + 4.0;
    }

    bool show_pulse = (shape == WAVE_PULSE || shape == WAVE_NOISE);
    if (show_pulse) {
        out->pulse_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
        y += HDS_PLAY_KNOB_STRIDE;
        if (shape == WAVE_NOISE) {
            out->mono_chk = (HdsFxRect){col_x, y, HDS_PLAY_OSC_COL_W, HDS_PLAY_CHECK_H};
            y += HDS_PLAY_CHECK_H + 4.0;
        }
    }
    bool show_curve = (shape == WAVE_SAW || shape == WAVE_TRIANGLE || shape == WAVE_SINE || shape == WAVE_REVSAW);
    if (show_curve) {
        out->curve_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
        y += HDS_PLAY_KNOB_STRIDE;
    }
    if (osc > 0) {
        out->pitch_off_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
        y += HDS_PLAY_KNOB_STRIDE;
    }
    out->fine_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    y += HDS_PLAY_KNOB_STRIDE;
    out->phase_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    y += HDS_PLAY_KNOB_STRIDE;
    out->pitch_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    y += HDS_PLAY_KNOB_STRIDE;

    if (shape == WAVE_UNISON_SAW) {
        out->uni_width_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
        y += HDS_PLAY_KNOB_STRIDE;
        out->uni_detune_knob = (HdsFxRect){cx, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
        y += HDS_PLAY_KNOB_STRIDE;
    }

    y += 6.0;
    out->vol_knob = (HdsFxRect){col_x + 18.0, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    out->mute_btn = (HdsFxRect){cx - 9.0, y + 10.0, 18.0, 14.0};
    out->pan_knob = (HdsFxRect){col_x + HDS_PLAY_OSC_COL_W - 18.0, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    y += HDS_PLAY_KNOB_STRIDE;

    out->col_bottom_y = y;
}

typedef struct {
    HdsFxRect title;
    HdsFxRect vel_to_amp_knob;
    HdsFxRect sync_all_btn;   // Amp EG 0 (Osc1) only
    HdsFxRect sync_live_btn;  // Amp EG 0 (Osc1) only
    HdsFxRect curve;          // the ADSR graph itself
    HdsFxRect mode_pill;
    HdsFxRect dial[6];        // Dly, Hld, A, Dec, S, R -- only the topology-relevant ones are ever drawn/hit
} HdsPlayEgLayout;

#define HDS_PLAY_EG_CURVE_H 125.0

// Field indices into HDS_PLAY_DIAL_LABELS/HDS_PLAY_EG_LO/HI/hds_play_eg_dial_target's own switch.
#define HDS_PLAY_EG_FIELD_D   0
#define HDS_PLAY_EG_FIELD_H   1
#define HDS_PLAY_EG_FIELD_A   2
#define HDS_PLAY_EG_FIELD_DEC 3
#define HDS_PLAY_EG_FIELD_S   4
#define HDS_PLAY_EG_FIELD_R   5

// Per envelope-type, the ORDERED list of which fields are visible and in what left-to-right
// on-screen order -- mirrors frontend.py's own ENV_TYPE_KNOB_ORDER exactly: its own ADSRPanel
// docstring is explicit that it "physically shows, hides, and REORDERS knob columns on the screen
// so the visible knobs always read left-to-right in the same order as the letters in the selected
// envelope type's name." This is NOT the same as a fixed physical slot arrangement with per-slot
// visibility toggling: that approach (this table's predecessor) silently broke ordering for AHD
// and AHDSR specifically -- both names start with "A" (Attack), so Attack's dial must be the
// LEFTMOST one visible, but Hold's own fixed field index (1) sits lower than Attack's (2), so
// toggling visibility over fixed slots rendered Hold before Attack for exactly those two types.
// -1 terminates a shorter row; each row has 6 entries (the max, DADSR/AHDSR), matching
// HDS_PLAY_EGMODE_NAMES's own row order (ADSR, AR, AD, ASR, AHD, DADSR, AHDSR).
static const int HDS_PLAY_EG_FIELD_ORDER[HDS_PLAY_EGMODE_COUNT][6] = {
    /* ADSR  */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_DEC, HDS_PLAY_EG_FIELD_S, HDS_PLAY_EG_FIELD_R, -1, -1},
    /* AR    */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_R, -1, -1, -1, -1},
    /* AD    */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_DEC, -1, -1, -1, -1},
    /* ASR   */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_S, HDS_PLAY_EG_FIELD_R, -1, -1, -1},
    /* AHD   */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_H, HDS_PLAY_EG_FIELD_DEC, -1, -1, -1},
    /* DADSR */ {HDS_PLAY_EG_FIELD_D, HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_DEC, HDS_PLAY_EG_FIELD_S, HDS_PLAY_EG_FIELD_R, -1},
    /* AHDSR */ {HDS_PLAY_EG_FIELD_A, HDS_PLAY_EG_FIELD_H, HDS_PLAY_EG_FIELD_DEC, HDS_PLAY_EG_FIELD_S, HDS_PLAY_EG_FIELD_R, -1},
};

static int hds_play_eg_dial_count(int env_type) {
    if (env_type < 0 || env_type >= HDS_PLAY_EGMODE_COUNT) env_type = 0;
    int n = 0;
    while (n < 6 && HDS_PLAY_EG_FIELD_ORDER[env_type][n] >= 0) n++;
    return n;
}

static void hds_play_compute_eg_layout(int eg_index, int env_type, double col_x, double col_w, double top_y, HdsPlayEgLayout *out) {
    memset(out, 0, sizeof(*out));
    out->title = (HdsFxRect){col_x, top_y, col_w, 20.0};
    // Sync-All/Sync-Live (Osc 1's EG only -- see this struct's own field comments) live on the
    // title row itself, flush right: two small pill buttons fit there comfortably. The Velocity
    // knob is a DIFFERENT thing every one of the 4 EGs has (this synth's real vco{1-4}_vel_to_amp
    // backend fields, all wired identically -- see frontend.py's own vco1_vel_to_amp, which is
    // never special-cased away for Osc 1 either), so it gets its own row below the title, on EVERY
    // column -- previously Osc 1's title row had no room left for both the two Sync buttons AND a
    // third knob, which silently dropped that knob (and, with it, any way to see or adjust Osc 1's
    // own velocity-to-amp amount) instead of finding it a row of its own. Giving all 4 columns the
    // same extra row keeps every column's curve/dial content aligned at the same y regardless of
    // eg_index, exactly like hds_play_eg_top_y() already keeps them aligned across all 4 at all.
    if (eg_index == 0) {
        out->sync_all_btn = (HdsFxRect){col_x + col_w - 72.0, top_y + 2.0, 36.0, 16.0}; // 36 wide, not 30 -- "1->All" was clipping to "1->Al" at this file's own bumped label font size
        out->sync_live_btn = (HdsFxRect){col_x + col_w - 35.0, top_y + 2.0, 35.0, 16.0};
    }
    double vel_row_y = top_y + 28.0; // clear of the sync buttons' own bottom edge (top_y+18) above, plus room for the Vel label's own ascender
    out->vel_to_amp_knob = (HdsFxRect){col_x + col_w - 16.0, vel_row_y + HDS_PLAY_KNOB_R + 7.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    double y = vel_row_y + HDS_PLAY_KNOB_STRIDE;
    out->curve = (HdsFxRect){col_x, y, col_w, HDS_PLAY_EG_CURVE_H};
    y += HDS_PLAY_EG_CURVE_H + 6.0;
    out->mode_pill = (HdsFxRect){col_x, y, 70.0, HDS_PLAY_PILL_H};
    y += HDS_PLAY_PILL_H + 10.0;
    // dial[i] is the i-th VISIBLE knob's rect, left to right -- which underlying field that is
    // comes from HDS_PLAY_EG_FIELD_ORDER[env_type][i], not from i itself (see that table's own
    // comment). Spread evenly over however many are actually visible for this topology (2-5),
    // not always over a fixed 6 slots, so a 2-knob topology (e.g. AR) doesn't leave 4 slots empty
    // and a 5-knob one (DADSR/AHDSR) gets its full share of col_w instead of being squeezed into
    // 5 of 6 equal slots.
    int count = hds_play_eg_dial_count(env_type);
    double dial_stride = col_w / (double)count;
    for (int i = 0; i < count; i++) {
        out->dial[i] = (HdsFxRect){col_x + i * dial_stride + dial_stride / 2.0, y + HDS_PLAY_KNOB_R + 9.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    }
}

static const char *const HDS_PLAY_DIAL_LABELS[6] = {"Dly", "Hld", "A", "Dec", "S", "R"};
// Real backend-unit range for each of the 6 PackEGParams fields (d,h,a,dec,s,r), same order as
// HDS_PLAY_DIAL_LABELS -- shared by render (the knob arc's own fraction) and interaction (drag
// range) so they can never disagree, same "one source of truth" rule this whole file follows.
static const double HDS_PLAY_EG_LO[6] = {0, 0, 1, 1, 0, 1};
static const double HDS_PLAY_EG_HI[6] = {1000, 1000, 1000, 2000, 100, 2000};

static float *hds_play_eg_dial_target(PackEGParams *eg, int i) {
    switch (i) {
        case 0: return &eg->d; case 1: return &eg->h; case 2: return &eg->a;
        case 3: return &eg->dec; case 4: return &eg->s; default: return &eg->r;
    }
}

// ---------------------------------------------------------------------------------------------
// Oscillator knob field identity -- which HdsPlayOscFields pointer a given osc-strip knob edits,
// its real backend-unit range (drag is linear over this range -- see this file's own header
// comment on why), its short name label, and how its own value is formatted for the readout under
// it. shape only matters for the Pulse/Curve knob's dual-duty label (see hds_play_osc_fields'/
// backend.c's own comment on vco{n}_pulse_width and vco{n}_curve_drive).
// ---------------------------------------------------------------------------------------------
typedef enum {
    HDS_PLAY_KNOB_PULSE, HDS_PLAY_KNOB_CURVE, HDS_PLAY_KNOB_PITCH_OFFSET, HDS_PLAY_KNOB_FINE,
    HDS_PLAY_KNOB_PHASE, HDS_PLAY_KNOB_PITCH, HDS_PLAY_KNOB_UNI_WIDTH, HDS_PLAY_KNOB_UNI_DETUNE,
    HDS_PLAY_KNOB_VOL, HDS_PLAY_KNOB_PAN, HDS_PLAY_KNOB_VEL_TO_AMP,
} HdsPlayOscKnobId;

static void hds_play_osc_knob_info(HdsPlayOscKnobId id, int shape, double *lo, double *hi, const char **label) {
    switch (id) {
        case HDS_PLAY_KNOB_PULSE: *lo = -100.0; *hi = 100.0; *label = (shape == WAVE_NOISE) ? "Tilt" : "Pls W"; break;
        case HDS_PLAY_KNOB_CURVE: *lo = -1.0; *hi = 1.0; *label = (shape == WAVE_SINE) ? "Drive" : "Curve"; break;
        case HDS_PLAY_KNOB_PITCH_OFFSET: *lo = -48.0; *hi = 48.0; *label = "Pch Of"; break;
        case HDS_PLAY_KNOB_FINE: *lo = -100.0; *hi = 100.0; *label = "Fine"; break;
        case HDS_PLAY_KNOB_PHASE: *lo = 0.0; *hi = 360.0; *label = "Phase"; break;
        case HDS_PLAY_KNOB_PITCH: *lo = -2400.0; *hi = 2400.0; *label = "Pitch"; break;
        case HDS_PLAY_KNOB_UNI_WIDTH: *lo = 0.0; *hi = 100.0; *label = "Width"; break;
        case HDS_PLAY_KNOB_UNI_DETUNE: *lo = 0.0; *hi = 4200.0; *label = "Detune"; break;
        case HDS_PLAY_KNOB_VOL: *lo = 0.0; *hi = 1.0; *label = "Vol"; break;
        case HDS_PLAY_KNOB_PAN: *lo = -1.0; *hi = 1.0; *label = "Pan"; break;
        case HDS_PLAY_KNOB_VEL_TO_AMP: *lo = -100.0; *hi = 100.0; *label = "Vel"; break;
        default: *lo = 0.0; *hi = 1.0; *label = ""; break;
    }
}

static void hds_play_osc_knob_value_text(HdsPlayOscKnobId id, double val, char *buf, size_t buflen) {
    switch (id) {
        case HDS_PLAY_KNOB_CURVE: case HDS_PLAY_KNOB_PAN:
            snprintf(buf, buflen, "%d%%", (int)(val * 100.0 + (val >= 0 ? 0.5 : -0.5))); break;
        case HDS_PLAY_KNOB_PITCH: snprintf(buf, buflen, "%+.1fst", val / 100.0); break;
        case HDS_PLAY_KNOB_FINE: snprintf(buf, buflen, "%+dc", (int)val); break;
        case HDS_PLAY_KNOB_PHASE: snprintf(buf, buflen, "%d\xc2\xb0", (int)val); break;
        case HDS_PLAY_KNOB_PITCH_OFFSET: snprintf(buf, buflen, "%+dst", (int)val); break;
        case HDS_PLAY_KNOB_VOL: snprintf(buf, buflen, "%d%%", (int)(val * 100.0 + 0.5)); break;
        default: snprintf(buf, buflen, "%d", (int)(val + (val >= 0 ? 0.5 : -0.5))); break;
    }
}

// Generic knob draw -- label above, arc at (cx,cy), value readout below, all relative to the
// knob's own center (unlike hds_fx_draw_knob's row-relative anchoring -- see hds_fx_draw_knob_arc's
// own comment on why the arc itself was extracted into a shared primitive for exactly this reason).
static void hds_play_draw_knob(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled, const char *label, const char *value_text) {
    if (label && label[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.0);
        hds_fx_rgb(cr, disabled ? 0.38 : 0.70, disabled ? 0.38 : 0.72, disabled ? 0.40 : 0.74);
        hds_fx_centered_text(cr, cx, cy - radius - 6.0, label);
    }
    hds_fx_draw_knob_arc(cr, cx, cy, radius, value01, disabled);
    if (value_text && value_text[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.0);
        hds_fx_rgb(cr, disabled ? 0.38 : 0.62, disabled ? 0.38 : 0.62, disabled ? 0.40 : 0.65);
        hds_fx_centered_text(cr, cx, cy + radius + 9.0, value_text);
    }
}

static bool hds_play_rect_contains(const HdsFxRect *r, double x, double y) {
    return r->w > 0 && r->h > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// Every knob rect in this file stores its CENTER in (x,y) and its radius in w (h == w) -- the
// convention hds_play_draw_knob()/hds_fx_draw_knob_arc() both need (they draw an arc around a
// center, not a rectangle) -- so its own hit test is a circle around that center, never
// hds_play_rect_contains()'s rectangle (which would test the wrong box entirely: a rect whose
// TOP-LEFT corner is this knob's own CENTER, not one centered on it). A generous radius (the
// visual radius plus a few px) rather than the bare drawn radius, matching how forgiving a real
// click target should be for a small 9px knob.
static bool hds_play_knob_hit_test(const HdsFxRect *r, double x, double y) {
    if (r->w <= 0) return false;
    double dx = x - r->x, dy = y - r->y;
    double hit_r = r->w + 4.0;
    return dx * dx + dy * dy <= hit_r * hit_r;
}

// One click-and-drag knob: draw + hit-test share this exact geometry always (same discipline as
// every other control in this GUI); (*target) may be NULL (e.g. mono_noise's own field on Osc1,
// which doesn't exist -- see hds_play_osc_fields' own comment), in which case the caller must have
// already skipped drawing/hit-testing this rect entirely (an empty {0,0,0,0} rect from
// hds_play_compute_osc_layout() never hit-tests true anyway, so this is a belt-and-suspenders
// assert-free NULL guard, not the only thing preventing it).
static void hds_play_draw_osc_knob(cairo_t *cr, const HdsFxRect *r, float *target, HdsPlayOscKnobId id, int shape) {
    if (!target || r->w <= 0) return;
    double lo, hi; const char *label;
    hds_play_osc_knob_info(id, shape, &lo, &hi, &label);
    float frac = (float)((*target - lo) / (hi - lo));
    char value_text[16];
    hds_play_osc_knob_value_text(id, *target, value_text, sizeof(value_text));
    hds_play_draw_knob(cr, r->x, r->y, r->w, frac, false, label, value_text);
}

#define HDS_PLAY_DRAG_PIXELS_FULL_RANGE 150.0

// Generic across every tab that has a click-and-drag knob (not just Play's own oscillator/EG
// knobs) -- takes the HdsPlayKnobDrag descriptor directly rather than a whole tab's own view
// state, so a later tab file (e.g. hds_clap_filters_draw.c) can reuse it verbatim against its own
// view state's own drag field, the same way it already reuses hds_play_draw_knob()/
// hds_play_knob_hit_test().
#define HDS_PLAY_NO_MENU ((HdsPlayKnobMenu){0, 0})
#define HDS_PLAY_DOUBLE_CLICK_MS 400.0
#define HDS_PLAY_DOUBLE_CLICK_PX 4.0

// button 1 (left): double-click (same knob, within HDS_PLAY_DOUBLE_CLICK_MS/PX of the last left
// click) resets to 0 -- mirrors frontend.py's PotentiometerKnob.mouseDoubleClickEvent, which
// always resets to a literal 0.0 regardless of range -- otherwise starts a normal drag. button 3
// (right): recorded as pending; hds_play_apply_knob_drag()/hds_play_release_knob_drag() below
// promote it to a fine-sensitivity drag once the pointer moves, or open `menu` on release if it
// never did (frontend.py's own manhattanLength() > 3 click-vs-drag gesture).
static void hds_play_start_knob_drag(HdsPlayKnobDrag *drag, float *target, double lo, double hi, double x, double y, int button, HdsPlayKnobMenu menu) {
    if (!target) return;
    if (button == 3) {
        drag->target = target; drag->lo = lo; drag->hi = hi;
        drag->start_y = y; drag->start_val = *target;
        drag->button = 3; drag->press_x = x; drag->press_y = y; drag->right_moved = false;
        drag->menu = menu;
        return;
    }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double dt_ms = (now.tv_sec - drag->last_click_time.tv_sec) * 1000.0 + (now.tv_nsec - drag->last_click_time.tv_nsec) / 1e6;
    double ddx = x - drag->last_click_x, ddy = y - drag->last_click_y;
    bool is_double = drag->last_click_target == target && dt_ms < HDS_PLAY_DOUBLE_CLICK_MS && (ddx * ddx + ddy * ddy) < (HDS_PLAY_DOUBLE_CLICK_PX * HDS_PLAY_DOUBLE_CLICK_PX);
    drag->last_click_time = now; drag->last_click_x = x; drag->last_click_y = y;
    if (is_double) {
        *target = (float)fmax(lo, fmin(hi, 0.0));
        drag->last_click_target = NULL;
        drag->button = 0; drag->target = NULL;
        return;
    }
    drag->last_click_target = target;
    drag->target = target; drag->lo = lo; drag->hi = hi;
    drag->start_y = y; drag->start_val = *target;
    drag->button = 1;
}

#define HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX 3.0
#define HDS_PLAY_RIGHT_DRAG_SENSITIVITY 0.1

// Called every motion event a knob_drag is in flight (button 1 or 3 -- idle button==0 means
// drag->target is NULL and this is never reached). button 1 is the plain full-sensitivity drag this
// file always had. button 3 stays a no-op (not yet a drag at all) until the pointer clears
// HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX of its own ORIGINAL press point -- frontend.py's own
// manhattanLength() > 3 promotion -- after which it applies the exact same linear mapping at 1/10th
// sensitivity (frontend.py's sensitivity_scale=0.1), always measured from that same press point
// (never a running last-position), matching PotentiometerKnob.mouseMoveEvent's own math exactly.
static void hds_play_apply_knob_drag(HdsPlayKnobDrag *drag, double x, double y) {
    if (!drag->target) return;
    if (drag->button == 3) {
        double mdx = x - drag->press_x, mdy = y - drag->press_y;
        if (!drag->right_moved) {
            if (fabs(mdx) + fabs(mdy) <= HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX) return;
            drag->right_moved = true;
        }
        double delta = (drag->press_y - y) * HDS_PLAY_RIGHT_DRAG_SENSITIVITY;
        double v = drag->start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (drag->hi - drag->lo);
        if (v < drag->lo) v = drag->lo; else if (v > drag->hi) v = drag->hi;
        *drag->target = (float)v;
        return;
    }
    double delta = drag->start_y - y;
    double v = drag->start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (drag->hi - drag->lo);
    if (v < drag->lo) v = drag->lo; else if (v > drag->hi) v = drag->hi;
    *drag->target = (float)v;
}

// Called from on_button_release. Returns the knob's own menu (HDS_PLAY_NO_MENU if it doesn't have
// one, or if this release ends an actual drag rather than a click) and, only when a real menu is
// returned, where to anchor it -- the ORIGINAL right-press point, which is always within
// HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX of wherever the button was released in that case (right_moved is
// still false), so there's no need to plumb the release event's own x/y through the ~8 on_button_
// release call sites in this codebase just for this.
static HdsPlayKnobMenu hds_play_release_knob_drag(HdsPlayKnobDrag *drag, double *out_anchor_x, double *out_anchor_y) {
    HdsPlayKnobMenu menu = HDS_PLAY_NO_MENU;
    if (drag->target && drag->button == 3 && !drag->right_moved && drag->menu.kind != 0) {
        menu = drag->menu;
        if (out_anchor_x) *out_anchor_x = drag->press_x;
        if (out_anchor_y) *out_anchor_y = drag->press_y;
    }
    drag->target = NULL;
    drag->button = 0;
    return menu;
}

static bool hds_play_try_osc_knob_click(HdsClapPlayViewState *vs, const HdsFxRect *r, float *target, HdsPlayOscKnobId id, int shape, double x, double y, int button, HdsPlayKnobMenu menu) {
    if (!target || !hds_play_knob_hit_test(r, x, y)) return false;
    double lo, hi; const char *label; (void)label;
    hds_play_osc_knob_info(id, shape, &lo, &hi, &label);
    hds_play_start_knob_drag(&vs->knob_drag, target, lo, hi, x, y, button, menu);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Dropdown lists -- Shape (10 options) and Amp EG Mode (7 options). Same "open list positioned
// off the anchor pill, one at a time" idiom hds_fx_gui_draw.c's own dropdown already uses, kept as
// this file's own small version rather than sharing that one directly: the option sets, and what
// dropdown_index means (an oscillator vs. an Amp EG), are entirely different domains here.
// ---------------------------------------------------------------------------------------------
#define HDS_PLAY_DROPDOWN_ROW_H 18.0

static int hds_play_dropdown_option_count(int kind) {
    if (kind == HDS_PLAY_DROPDOWN_SHAPE) return HDS_PLAY_SHAPE_COUNT;
    if (kind == HDS_PLAY_DROPDOWN_EGMODE) return HDS_PLAY_EGMODE_COUNT;
    return 0;
}

static const char *hds_play_dropdown_option_label(int kind, int idx) {
    if (kind == HDS_PLAY_DROPDOWN_SHAPE) return HDS_PLAY_SHAPE_NAMES[idx];
    if (kind == HDS_PLAY_DROPDOWN_EGMODE) return HDS_PLAY_EGMODE_NAMES[idx];
    return "";
}

static void hds_play_dropdown_bg_rect(const HdsFxRect *anchor, int kind, double window_h, HdsFxRect *out) {
    int count = hds_play_dropdown_option_count(kind);
    double list_h = count * HDS_PLAY_DROPDOWN_ROW_H;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    *out = (HdsFxRect){anchor->x, y0, 78.0, list_h};
}

static void hds_play_dropdown_option_rect(const HdsFxRect *anchor, int kind, double window_h, int idx, HdsFxRect *out) {
    HdsFxRect bg;
    hds_play_dropdown_bg_rect(anchor, kind, window_h, &bg);
    *out = (HdsFxRect){bg.x, bg.y + idx * HDS_PLAY_DROPDOWN_ROW_H, bg.w, HDS_PLAY_DROPDOWN_ROW_H};
}

static void hds_play_render_dropdown(cairo_t *cr, HdsClapPlayViewState *vs, double window_h, int current) {
    if (vs->dropdown_kind == HDS_PLAY_DROPDOWN_NONE) return;
    int count = hds_play_dropdown_option_count(vs->dropdown_kind);
    HdsFxRect bg;
    hds_play_dropdown_bg_rect(&vs->dropdown_anchor, vs->dropdown_kind, window_h, &bg);
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    for (int i = 0; i < count; i++) {
        HdsFxRect r;
        hds_play_dropdown_option_rect(&vs->dropdown_anchor, vs->dropdown_kind, window_h, i, &r);
        if (i == current) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
        }
        hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
        hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, hds_play_dropdown_option_label(vs->dropdown_kind, i));
    }
}

// ---------------------------------------------------------------------------------------------
// Render -- one oscillator strip's worth of controls, then one Amp EG panel's worth. Both loop
// over the same 4 indices; kept as two separate loops (not one interleaved loop) purely because
// the EG region's own top_y depends on the TALLEST oscillator column (varies by shape -- see
// hds_play_osc_region_bottom_y()), which isn't known until all 4 oscillator layouts have been
// computed. Each EG panel sits directly below its own same-numbered oscillator column (same x,
// same width), not off to the side.
// ---------------------------------------------------------------------------------------------
#define HDS_PLAY_OSC_GAP 18.0 // widened for breathing room between strips
#define HDS_PLAY_REGION_GAP 20.0 // vertical gap between the oscillator region and the EG region below it

static double hds_play_osc_col_x(int osc) { return HDS_PLAY_MARGIN + osc * (HDS_PLAY_OSC_COL_W + HDS_PLAY_OSC_GAP); }

// Tallest of the 4 oscillator columns' own col_bottom_y (varies by shape -- e.g. Unison Saw shows
// two extra knobs no other shape does) -- the EG region below must start at the SAME y for all 4
// columns regardless of which oscillator happens to be tallest, so this takes the max rather than
// each column's own bottom.
static double hds_play_osc_region_bottom_y(ProgramState *prog) {
    double max_bottom = 0.0;
    for (int osc = 0; osc < 4; osc++) {
        HdsPlayOscFields f; hds_play_osc_fields(prog, osc, &f);
        HdsPlayOscLayout L;
        hds_play_compute_osc_layout(osc, (int)*f.shape, hds_play_osc_col_x(osc), HDS_PLAY_CONTENT_TOP, &L);
        if (L.col_bottom_y > max_bottom) max_bottom = L.col_bottom_y;
    }
    return max_bottom;
}

static double hds_play_eg_top_y(ProgramState *prog) {
    return hds_play_osc_region_bottom_y(prog) + HDS_PLAY_REGION_GAP;
}

static void hds_play_render_osc_strip(cairo_t *cr, HdsClapPlugin *p, HdsClapPlayViewState *vs, int osc, double col_x) {
    ProgramState *prog = hds_play_program(p);
    HdsPlayOscFields f; hds_play_osc_fields(prog, osc, &f);
    int shape = (int)*f.shape;

    HdsPlayOscLayout L;
    hds_play_compute_osc_layout(osc, shape, col_x, HDS_PLAY_CONTENT_TOP, &L);

    char num[4]; snprintf(num, sizeof(num), "%d", osc + 1);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    char hdr[16]; snprintf(hdr, sizeof(hdr), "Osc %s", num);
    hds_fx_left_text(cr, col_x, HDS_PLAY_CONTENT_TOP - 8.0, hdr);

    bool shape_dd_open = (vs->dropdown_kind == HDS_PLAY_DROPDOWN_SHAPE && vs->dropdown_index == osc);
    hds_fx_draw_pill(cr, L.shape_pill.x, L.shape_pill.y, L.shape_pill.w, L.shape_pill.h,
                      HDS_PLAY_SHAPE_NAMES[shape >= 0 && shape < HDS_PLAY_SHAPE_COUNT ? shape : 0],
                      shape_dd_open, false);

    if (L.sfz_load_btn.w > 0) {
        hds_fx_draw_pill(cr, L.sfz_load_btn.x, L.sfz_load_btn.y, L.sfz_load_btn.w, L.sfz_load_btn.h, "Load .sfz\xe2\x80\xa6", false, false);
    }
    if (L.force_own_amp_eg_chk.w > 0) {
        bool force_own = f.force_own_amp_eg && *f.force_own_amp_eg != 0.0f;
        hds_fx_draw_pill(cr, L.force_own_amp_eg_chk.x, L.force_own_amp_eg_chk.y, L.force_own_amp_eg_chk.w, L.force_own_amp_eg_chk.h, "Use Amp EG", force_own, false);
    }

    hds_play_draw_osc_knob(cr, &L.pulse_knob, f.pulse_width, HDS_PLAY_KNOB_PULSE, shape);
    if (L.mono_chk.w > 0) {
        bool mono = f.mono_noise && *f.mono_noise != 0.0f;
        hds_fx_draw_pill(cr, L.mono_chk.x, L.mono_chk.y, L.mono_chk.w, L.mono_chk.h, "Mono", mono, false);
    }
    hds_play_draw_osc_knob(cr, &L.curve_knob, f.curve_drive, HDS_PLAY_KNOB_CURVE, shape);
    hds_play_draw_osc_knob(cr, &L.pitch_off_knob, f.pitch_offset, HDS_PLAY_KNOB_PITCH_OFFSET, shape);
    hds_play_draw_osc_knob(cr, &L.fine_knob, f.fine_tune, HDS_PLAY_KNOB_FINE, shape);
    hds_play_draw_osc_knob(cr, &L.phase_knob, f.phase_offset, HDS_PLAY_KNOB_PHASE, shape);
    hds_play_draw_osc_knob(cr, &L.pitch_knob, f.pitch, HDS_PLAY_KNOB_PITCH, shape);
    hds_play_draw_osc_knob(cr, &L.uni_width_knob, f.unison_width, HDS_PLAY_KNOB_UNI_WIDTH, shape);
    hds_play_draw_osc_knob(cr, &L.uni_detune_knob, f.unison_detune, HDS_PLAY_KNOB_UNI_DETUNE, shape);
    hds_play_draw_osc_knob(cr, &L.vol_knob, f.vol, HDS_PLAY_KNOB_VOL, shape);
    bool muted = *f.mute != 0.0f;
    hds_fx_draw_mute_pill(cr, L.mute_btn.x, L.mute_btn.y, L.mute_btn.w, L.mute_btn.h, "M", muted, false);
    hds_play_draw_osc_knob(cr, &L.pan_knob, f.pan, HDS_PLAY_KNOB_PAN, shape);
}

static void hds_play_render_eg_panel(cairo_t *cr, HdsClapPlugin *p, HdsClapPlayViewState *vs, int eg_index, double col_x, double col_w, double top_y) {
    ProgramState *prog = hds_play_program(p);
    PackEGParams *eg = &prog->eg[eg_index];
    int env_type = (int)prog->eg_mode[eg_index];
    if (env_type < 0 || env_type >= HDS_PLAY_EGMODE_COUNT) env_type = 0;

    HdsPlayEgLayout L;
    hds_play_compute_eg_layout(eg_index, env_type, col_x, col_w, top_y, &L);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    // "Amp EG N", not "Osc N Amp EG" -- this column already sits directly beneath that oscillator's
    // own "Osc N" strip, so repeating the number's own "Osc" prefix here just burns horizontal room
    // this header row also needs for the Sync buttons (eg_index 0) and every EG's own Vel knob row.
    char title[24]; snprintf(title, sizeof(title), "Amp EG %d", eg_index + 1);
    hds_fx_left_text(cr, L.title.x, L.title.y + 12.0, title);

    bool disabled = (eg_index > 0 && vs->amp_eg_live_sync);
    if (eg_index == 0) {
        hds_fx_draw_pill(cr, L.sync_all_btn.x, L.sync_all_btn.y, L.sync_all_btn.w, L.sync_all_btn.h, "1->All", false, false); // plain ASCII, not the UTF-8 arrow -- see hds_clap_global_draw.c's own comment on why
        hds_fx_draw_pill(cr, L.sync_live_btn.x, L.sync_live_btn.y, L.sync_live_btn.w, L.sync_live_btn.h, "Sync", vs->amp_eg_live_sync, false);
    }
    // Every one of the 4 EGs has its own real vco{1-4}_vel_to_amp backend field (see this file's
    // own hds_play_compute_eg_layout() comment on why Osc 1 is not special-cased out of this) --
    // drawn on its own row now for all 4, not just eg_index > 0.
    {
        ProgramState *prog2 = hds_play_program(p);
        HdsPlayOscFields f; hds_play_osc_fields(prog2, eg_index, &f);
        hds_play_draw_osc_knob(cr, &L.vel_to_amp_knob, f.vel_to_amp, HDS_PLAY_KNOB_VEL_TO_AMP, 0);
    }

    // The curve widget: translate so its own draw code stays (0,0)-relative, same idiom the FX
    // tab's own embedding inside the tab bar already uses (see hds_clap_gui_render()'s own comment).
    AdsrEnvelopeState adsr;
    hds_clap_adsr_load_from_backend(&adsr, eg, env_type);
    adsr.active_node = vs->adsr_active_node[eg_index];
    cairo_save(cr);
    cairo_rectangle(cr, L.curve.x, L.curve.y, L.curve.w, L.curve.h);
    cairo_clip(cr);
    cairo_translate(cr, L.curve.x, L.curve.y);
    adsr_draw(cr, &adsr, L.curve.w, L.curve.h);
    cairo_restore(cr);

    bool eg_dd_open = (vs->dropdown_kind == HDS_PLAY_DROPDOWN_EGMODE && vs->dropdown_index == eg_index);
    hds_fx_draw_pill(cr, L.mode_pill.x, L.mode_pill.y, L.mode_pill.w, L.mode_pill.h,
                      HDS_PLAY_EGMODE_NAMES[env_type], eg_dd_open, disabled);

    double vals[6] = {eg->d, eg->h, eg->a, eg->dec, eg->s, eg->r};
    int count = hds_play_eg_dial_count(env_type);
    for (int i = 0; i < count; i++) {
        int field = HDS_PLAY_EG_FIELD_ORDER[env_type][i];
        float frac = (float)((vals[field] - HDS_PLAY_EG_LO[field]) / (HDS_PLAY_EG_HI[field] - HDS_PLAY_EG_LO[field]));
        char vtxt[16];
        if (field == HDS_PLAY_EG_FIELD_S) snprintf(vtxt, sizeof(vtxt), "%d%%", (int)(vals[field] + 0.5));
        else snprintf(vtxt, sizeof(vtxt), "%dms", (int)(vals[field] + 0.5));
        hds_play_draw_knob(cr, L.dial[i].x, L.dial[i].y, L.dial[i].w, frac, disabled, HDS_PLAY_DIAL_LABELS[field], vtxt);
    }
}

// A non-empty p->sfz_browser_result means the shared file browser's own .sfz pick (see
// HdsClapPlugin's own comment on that field) completed since the last render -- fire the real load
// right now, then clear it so this only ever runs once per pick. Safe to poll here specifically:
// the browser is a true modal (hds_clap_gui_draw.c's own top-level click dispatch refuses every
// click, including a tab-bar switch, while it's open -- see that file's own comment), so the Play
// tab stays the active tab, and this render function keeps running every frame, for the browser's
// entire open-to-closed lifetime whenever it was THIS button that opened it.
static void hds_play_poll_sfz_pick(HdsClapPlugin *p) {
    if (!p->sfz_browser_result[0]) return;
    hds_load_sfz_for_osc(&p->sample_engine, p->sfz_browser_target_osc, SINGLE_PROGRAM_SLOT, p->sfz_browser_result);
    p->sfz_browser_result[0] = '\0';
}

static void hds_clap_play_render(cairo_t *cr, HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double window_h) {
    hds_play_poll_sfz_pick(p);
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    double col_x = HDS_PLAY_MARGIN;
    for (int i = 0; i < 4; i++) {
        hds_play_render_osc_strip(cr, p, vs, i, col_x);
        col_x += HDS_PLAY_OSC_COL_W + HDS_PLAY_OSC_GAP;
    }

    ProgramState *prog = hds_play_program(p);
    double eg_top_y = hds_play_eg_top_y(prog);
    for (int i = 0; i < 4; i++) {
        hds_play_render_eg_panel(cr, p, vs, i, hds_play_osc_col_x(i), HDS_PLAY_OSC_COL_W, eg_top_y);
    }

    // Whichever dropdown (if any) is open draws LAST, on top of everything else -- same "open
    // list always wins the top paint layer" rule the FX tab's own dropdown already follows.
    if (vs->dropdown_kind == HDS_PLAY_DROPDOWN_SHAPE) {
        HdsPlayOscFields f; hds_play_osc_fields(prog, vs->dropdown_index, &f);
        hds_play_render_dropdown(cr, vs, window_h, (int)*f.shape);
    } else if (vs->dropdown_kind == HDS_PLAY_DROPDOWN_EGMODE) {
        hds_play_render_dropdown(cr, vs, window_h, (int)prog->eg_mode[vs->dropdown_index]);
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

// Oscillator Pitch/Phase/Pulse-Width's own "Modulate From" target numbers, one per oscillator
// (0-3) -- the same 3 knob kinds frontend.py wires setContextMenuCallback() onto via
// _show_dest_assign_menu(), matching hds_lfo_dest_label()'s own numbering exactly.
static const int HDS_PLAY_PITCH_DEST_TARGET[4] = {1, 2, 41, 42};
static const int HDS_PLAY_PHASE_DEST_TARGET[4] = {24, 25, 43, 44};
static const int HDS_PLAY_PULSE_DEST_TARGET[4] = {34, 35, 38, 47};
// Fine Tune/Curve/Volume/Pan's own "Modulate From" targets, same convention. Volume and Pan reuse
// targets that already existed (Osc 1/2's own Amp/Pan slots from the original layout, Osc 3/4's own
// later-appended ones) -- only the right-click menu itself is new, nothing to wire in backend.c.
// Fine Tune reuses the existing smaller-range pitch-mod targets for Osc 1/2 (4, 5); Osc 3/4 never
// had an equivalent, so 8/9 (two of the twelve retired 8-13/84-89 placeholder numbers -- see
// hds_lfo_dest_label()'s own comment on why those exist to be reclaimed like this) now cover them.
// Curve/Drive had no destination at all for any oscillator until now -- 10-13 (the remaining four
// retired 8-13 slots) are newly assigned, one per oscillator. See render_audio_block()'s own LFO/EG
// mod-application switch in backend.c (both copies -- LFOs and assignable EGs each dispatch
// separately) for where 8-13 actually get consumed, and hds_dest_list_build()'s own comment for
// where they became selectable in the shared Destination dropdown too.
static const int HDS_PLAY_FINE_DEST_TARGET[4] = {4, 5, 8, 9};
static const int HDS_PLAY_CURVE_DEST_TARGET[4] = {10, 11, 12, 13};
static const int HDS_PLAY_VOL_DEST_TARGET[4] = {6, 7, 36, 39};
static const int HDS_PLAY_PAN_DEST_TARGET[4] = {32, 33, 37, 40};

// "1->All"/"Sync": one-shot and live copy of Amp EG 0 (Osc1)'s full state onto Amp EGs 1-3 --
// mirrors frontend.py's own _copy_amp_eg_to_all()/_mirror_amp_eg_live() exactly (topology mode +
// all 8 PackEGParams fields, nothing else).
static void hds_play_copy_eg0_to_all(ProgramState *prog) {
    for (int i = 1; i < 4; i++) {
        prog->eg[i] = prog->eg[0];
        prog->eg_mode[i] = prog->eg_mode[0];
    }
}

static bool hds_clap_play_on_button_press(HdsClapPlugin *p, HdsClapPlayViewState *vs, HdsFxGuiViewState *fxvs, double window_w, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);

    if (vs->ctx_menu.open) return hds_play_ctxmenu_handle_click(p, &vs->ctx_menu, window_w, window_h, x, y);

    if (vs->dropdown_kind != HDS_PLAY_DROPDOWN_NONE) {
        int count = hds_play_dropdown_option_count(vs->dropdown_kind);
        for (int i = 0; i < count; i++) {
            HdsFxRect r;
            hds_play_dropdown_option_rect(&vs->dropdown_anchor, vs->dropdown_kind, window_h, i, &r);
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) {
                    if (vs->dropdown_kind == HDS_PLAY_DROPDOWN_SHAPE) {
                        HdsPlayOscFields f; hds_play_osc_fields(prog, vs->dropdown_index, &f);
                        *f.shape = (float)i;
                    } else if (vs->dropdown_kind == HDS_PLAY_DROPDOWN_EGMODE) {
                        prog->eg_mode[vs->dropdown_index] = (float)i;
                        if (vs->dropdown_index == 0 && vs->amp_eg_live_sync) hds_play_copy_eg0_to_all(prog);
                    }
                }
                vs->dropdown_kind = HDS_PLAY_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_PLAY_DROPDOWN_NONE; // clicked outside the open list -- just close it
        return true;
    }

    for (int osc = 0; osc < 4; osc++) {
        double col_x = hds_play_osc_col_x(osc);
        HdsPlayOscFields f; hds_play_osc_fields(prog, osc, &f);
        int shape = (int)*f.shape;
        HdsPlayOscLayout L;
        hds_play_compute_osc_layout(osc, shape, col_x, HDS_PLAY_CONTENT_TOP, &L);

        if (hds_play_rect_contains(&L.shape_pill, x, y)) {
            vs->dropdown_kind = HDS_PLAY_DROPDOWN_SHAPE; vs->dropdown_index = osc; vs->dropdown_anchor = L.shape_pill;
            return true;
        }
        // Multisample's own .sfz loader -- opens the ONE shared file browser (see HdsFxBrowser's
        // own comment) with p->sfz_browser_result as its dest; hds_play_poll_sfz_pick() (called at
        // the top of every hds_clap_play_render()) fires the real hds_load_sfz_for_osc() call once
        // a pick actually lands there.
        if (L.sfz_load_btn.w > 0 && hds_play_rect_contains(&L.sfz_load_btn, x, y)) {
            if (button == 1) {
                p->sfz_browser_target_osc = osc;
                p->sfz_browser_result[0] = '\0';
                hds_fx_gui_browser_open(fxvs, ".sfz", "Select SFZ File (*.sfz)", p->sfz_browser_result, SAMPLE_PATH_LEN);
            }
            return true;
        }
        if (L.mono_chk.w > 0 && hds_play_rect_contains(&L.mono_chk, x, y)) {
            if (button == 1 && f.mono_noise) *f.mono_noise = (*f.mono_noise != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (L.force_own_amp_eg_chk.w > 0 && hds_play_rect_contains(&L.force_own_amp_eg_chk, x, y)) {
            if (button == 1 && f.force_own_amp_eg) *f.force_own_amp_eg = (*f.force_own_amp_eg != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_rect_contains(&L.mute_btn, x, y)) {
            if (button == 1) *f.mute = (*f.mute != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        HdsPlayKnobMenu pulse_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_PULSE_DEST_TARGET[osc]};
        HdsPlayKnobMenu phase_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_PHASE_DEST_TARGET[osc]};
        HdsPlayKnobMenu pitch_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_PITCH_DEST_TARGET[osc]};
        HdsPlayKnobMenu fine_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_FINE_DEST_TARGET[osc]};
        HdsPlayKnobMenu curve_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_CURVE_DEST_TARGET[osc]};
        HdsPlayKnobMenu vol_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_VOL_DEST_TARGET[osc]};
        HdsPlayKnobMenu pan_menu = {HDS_CTXMENU_KIND_MODULATE_FROM, HDS_PLAY_PAN_DEST_TARGET[osc]};
        if (hds_play_try_osc_knob_click(vs, &L.pulse_knob, f.pulse_width, HDS_PLAY_KNOB_PULSE, shape, x, y, button, pulse_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.curve_knob, f.curve_drive, HDS_PLAY_KNOB_CURVE, shape, x, y, button, curve_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pitch_off_knob, f.pitch_offset, HDS_PLAY_KNOB_PITCH_OFFSET, shape, x, y, button, HDS_PLAY_NO_MENU)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.fine_knob, f.fine_tune, HDS_PLAY_KNOB_FINE, shape, x, y, button, fine_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.phase_knob, f.phase_offset, HDS_PLAY_KNOB_PHASE, shape, x, y, button, phase_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pitch_knob, f.pitch, HDS_PLAY_KNOB_PITCH, shape, x, y, button, pitch_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.uni_width_knob, f.unison_width, HDS_PLAY_KNOB_UNI_WIDTH, shape, x, y, button, HDS_PLAY_NO_MENU)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.uni_detune_knob, f.unison_detune, HDS_PLAY_KNOB_UNI_DETUNE, shape, x, y, button, HDS_PLAY_NO_MENU)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.vol_knob, f.vol, HDS_PLAY_KNOB_VOL, shape, x, y, button, vol_menu)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pan_knob, f.pan, HDS_PLAY_KNOB_PAN, shape, x, y, button, pan_menu)) return true;
    }

    double eg_top_y = hds_play_eg_top_y(prog);
    for (int i = 0; i < 4; i++) {
        int env_type = (int)prog->eg_mode[i];
        HdsPlayEgLayout L;
        hds_play_compute_eg_layout(i, env_type, hds_play_osc_col_x(i), HDS_PLAY_OSC_COL_W, eg_top_y, &L);
        bool disabled = (i > 0 && vs->amp_eg_live_sync);

        if (i == 0) {
            if (hds_play_rect_contains(&L.sync_all_btn, x, y)) {
                if (button == 1) hds_play_copy_eg0_to_all(prog);
                return true;
            }
            if (hds_play_rect_contains(&L.sync_live_btn, x, y)) {
                if (button == 1) { vs->amp_eg_live_sync = !vs->amp_eg_live_sync; if (vs->amp_eg_live_sync) hds_play_copy_eg0_to_all(prog); }
                return true;
            }
        }
        // Every EG's own Velocity knob is clickable now, i == 0 included (disabled is always false
        // there -- see this loop's own `disabled` line just above) -- see hds_play_render_eg_panel()'s
        // own comment on why Osc 1 is not special-cased out of this control at all.
        if (!disabled) {
            HdsPlayOscFields f; hds_play_osc_fields(prog, i, &f);
            if (hds_play_try_osc_knob_click(vs, &L.vel_to_amp_knob, f.vel_to_amp, HDS_PLAY_KNOB_VEL_TO_AMP, 0, x, y, button, HDS_PLAY_NO_MENU)) return true;
        }

        if (!disabled && hds_play_rect_contains(&L.mode_pill, x, y)) {
            vs->dropdown_kind = HDS_PLAY_DROPDOWN_EGMODE; vs->dropdown_index = i; vs->dropdown_anchor = L.mode_pill;
            return true;
        }
        if (!disabled && hds_play_rect_contains(&L.curve, x, y)) {
            AdsrEnvelopeState adsr;
            hds_clap_adsr_load_from_backend(&adsr, &prog->eg[i], env_type);
            adsr_calculate_geometry(&adsr, L.curve.w, L.curve.h); // populates point_x/y/count -- hit-testing needs real geometry, not just the raw d/h/a/... values
            int hit = adsr_hit_test(&adsr, x - L.curve.x, y - L.curve.y);
            vs->adsr_active_node[i] = hit;
            if (hit >= 0) vs->adsr_drag_eg = i;
            return true;
        }
        if (!disabled) {
            int count = hds_play_eg_dial_count(env_type);
            for (int d = 0; d < count; d++) {
                if (hds_play_knob_hit_test(&L.dial[d], x, y)) {
                    int field = HDS_PLAY_EG_FIELD_ORDER[env_type][d];
                    float *target = hds_play_eg_dial_target(&prog->eg[i], field);
                    hds_play_start_knob_drag(&vs->knob_drag, target, HDS_PLAY_EG_LO[field], HDS_PLAY_EG_HI[field], x, y, button, HDS_PLAY_NO_MENU);
                    return true;
                }
            }
        }
    }

    (void)button;
    return false;
}

static void hds_clap_play_on_motion(HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double x, double y) {
    ProgramState *prog = hds_play_program(p);
    (void)window_w;

    if (vs->knob_drag.target) {
        hds_play_apply_knob_drag(&vs->knob_drag, x, y);
        if (vs->amp_eg_live_sync) {
            // Cheap and correct rather than clever: any drag currently in flight might be one of
            // Amp EG 0's own 6 dial fields (their pointers all point inside prog->eg[0]) -- there's
            // no cheaper reliable way to tell from a bare float* than to just re-run the mirror
            // unconditionally on every motion event while Sync is on, which is exactly as cheap as
            // every other per-frame GUI redraw already is.
            hds_play_copy_eg0_to_all(prog);
        }
        return;
    }
    if (vs->adsr_drag_eg >= 0) {
        int i = vs->adsr_drag_eg;
        int env_type = (int)prog->eg_mode[i];
        HdsPlayEgLayout L;
        hds_play_compute_eg_layout(i, env_type, hds_play_osc_col_x(i), HDS_PLAY_OSC_COL_W, hds_play_eg_top_y(prog), &L);
        AdsrEnvelopeState adsr;
        hds_clap_adsr_load_from_backend(&adsr, &prog->eg[i], env_type);
        adsr.active_node = vs->adsr_active_node[i];
        adsr_calculate_geometry(&adsr, L.curve.w, L.curve.h); // needed before adsr_drag_update reads scale_x
        adsr_drag_update(&adsr, L.curve.w, L.curve.h, x - L.curve.x, y - L.curve.y);
        hds_clap_adsr_store_to_backend(&adsr, &prog->eg[i]);
        if (i == 0 && vs->amp_eg_live_sync) hds_play_copy_eg0_to_all(prog);
    }
}

static void hds_clap_play_on_button_release(HdsClapPlayViewState *vs) {
    double anchor_x = 0.0, anchor_y = 0.0;
    HdsPlayKnobMenu menu = hds_play_release_knob_drag(&vs->knob_drag, &anchor_x, &anchor_y);
    if (menu.kind != 0) {
        vs->ctx_menu = (HdsCtxMenu){true, anchor_x, anchor_y, menu.kind, menu.arg};
    }
    vs->adsr_drag_eg = -1;
}
