// hds_fx_gui_draw.c: the ENTIRE visual/interaction model for HDS FX's GUI, deliberately platform-
// agnostic -- every function here takes a plain `cairo_t*` and touches no windowing API of any kind
// (no Xlib, no Win32, nothing). This is the file a future Windows or macOS port reuses unchanged:
// only the SURFACE that `cairo_t*` draws into differs per platform (Xlib here, GDI on Windows via
// cairo_win32_surface_create(), Quartz on macOS), and Cairo's own drawing API is identical across
// all three -- see hds_fx_gui_x11.c's own header comment for the platform shim that owns the
// window/event pump and calls into this file every frame.
//
// #include'd by hds_fx_plugin.c AFTER HdsFxPlugin is defined (this file reads/writes it directly,
// no separate "view model" copy) and after fx_types.h/fx_chain.c (needs FXSlotParams/FX_TYPE_*).
//
// Layout model: modeled directly on the Standalone FX tab's own per-slot row (see frontend.py's own
// FX_TAB construction, ~line 4919 onward) rather than a from-scratch paradigm -- ONE flat,
// scrollable list of FX_SLOT_COUNT rows (32 -- see FX_SLOT_COUNT's own comment in hds_fx_plugin.c).
// This is just a multi-FX: no "bus" concept anywhere, no global Series/Parallel switch (Parallel
// mode's own existing chain_to_next semantics already reproduce plain Series exactly when every
// slot in a chain chains to the next one -- see hds_fx_plugin.c's own header comment), and no
// assumed stereo grouping across ports. Every slot has an Input (which of the 8 real stereo ports
// feeds its own chain, and whether that chain treats the port as Mono or Stereo) -- shown/editable
// only on a chain HEAD slot (hds_fx_slot_is_chain_head()) -- and, once its own Chain-to-Next is off
// (a chain TAIL), an Output (which port the result lands on, independent of the Input, same
// Mono/Stereo choice). Reordering (Up/Down) is disabled outright on any slot touching a chain link,
// in either direction -- see hds_fx_slot_participates_in_chain()'s own comment.
#include <cairo/cairo.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// ---------------------------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------------------------
#define HDS_FX_GUI_DEFAULT_W 1080
#define HDS_FX_GUI_DEFAULT_H 700
#define HDS_FX_GUI_MIN_W 700
#define HDS_FX_GUI_MIN_H 360
// Tall enough to fit, all strictly inside this one row's own bounds: a short label naming what the
// knob actually does, the knob itself, and its current value -- see hds_fx_draw_knob's own comment
// for why a bare unlabeled pot was wrong twice over (no name AND no visible value).
#define HDS_FX_GUI_ROW_H 56.0
// Title row + a column-header strip naming every column (see hds_fx_gui_render's own header-row
// drawing) -- a flat list with no per-group-box titles like the Standalone's own QGroupBoxes needs
// this or a column is just an unlabeled row of controls.
#define HDS_FX_GUI_HEADER_H 56.0
#define HDS_FX_GUI_KNOB_R 9.0
// Vertical offsets of a row's own knob label/center/value, all measured from the row's own top --
// fixed constants (not row_h/2-derived) so hds_fx_gui_compute_row_layout and the label/value draw
// calls in hds_fx_draw_knob can never drift apart. 47 + text descent still clears comfortably
// before HDS_FX_GUI_ROW_H (56)'s own bottom edge -- the whole label+knob+value group stays strictly
// inside this one row, never bleeding into the row above or below.
#define HDS_FX_KNOB_TOP_LABEL_Y_OFFSET 9.0
#define HDS_FX_KNOB_CY_OFFSET 27.0
#define HDS_FX_KNOB_VALUE_Y_OFFSET 47.0

// Selectable types for the GUI's own Type pill cycling -- every FX_TYPE_* except FX_TYPE_NAM, which
// this plugin compiles out entirely (HDS_NAM_ENABLED 0, see hds_fx_plugin.c's own header comment):
// letting the pill land on it would silently do nothing (fx_chain.c's own dispatch switch has no
// case for it here), which is worse than just not offering it until NAM is wired up for real.
static const int HDS_FX_SELECTABLE_TYPES[] = {
    FX_TYPE_OFF, FX_TYPE_ENSEMBLE, FX_TYPE_CREAMCOAT, FX_TYPE_DENSITY3, FX_TYPE_STEREOCHORUS,
    FX_TYPE_STEREODOUBLER, FX_TYPE_TAPEDELAY2, FX_TYPE_POINTYGUITAR, FX_TYPE_GUITARCONDITIONER,
    FX_TYPE_CABS2, FX_TYPE_GATE, FX_TYPE_EQ3, FX_TYPE_LIMITER, FX_TYPE_FETCOMP, FX_TYPE_FAIRKID67,
    FX_TYPE_PEAKMAX, FX_TYPE_MULTIFILTER, FX_TYPE_OPTOCOMP, FX_TYPE_DEESSER, FX_TYPE_HARDTUNE,
    FX_TYPE_FDNVERB,
};
#define HDS_FX_SELECTABLE_TYPE_COUNT (int)(sizeof(HDS_FX_SELECTABLE_TYPES) / sizeof(HDS_FX_SELECTABLE_TYPES[0]))

static const char *hds_fx_type_name(int fx_type) {
    switch (fx_type) {
        case FX_TYPE_OFF: return "Off";
        case FX_TYPE_ENSEMBLE: return "Ensemble";
        case FX_TYPE_CREAMCOAT: return "CreamCoat";
        case FX_TYPE_DENSITY3: return "Density3";
        case FX_TYPE_STEREOCHORUS: return "StereoChorus";
        case FX_TYPE_STEREODOUBLER: return "StereoDoubler";
        case FX_TYPE_TAPEDELAY2: return "TapeDelay2";
        case FX_TYPE_POINTYGUITAR: return "PointyGuitar";
        case FX_TYPE_GUITARCONDITIONER: return "GuitarCond";
        case FX_TYPE_CABS2: return "Cabs2";
        case FX_TYPE_NAM: return "NAM (disabled)";
        case FX_TYPE_GATE: return "Gate";
        case FX_TYPE_EQ3: return "EQ3";
        case FX_TYPE_LIMITER: return "Limiter";
        case FX_TYPE_FETCOMP: return "FetComp";
        case FX_TYPE_FAIRKID67: return "FairKid67";
        case FX_TYPE_PEAKMAX: return "PeakMax";
        case FX_TYPE_MULTIFILTER: return "MultiFilter";
        case FX_TYPE_OPTOCOMP: return "OptoComp";
        case FX_TYPE_DEESSER: return "DeEsser";
        case FX_TYPE_HARDTUNE: return "HardTune";
        case FX_TYPE_FDNVERB: return "FDNVerb";
        default: return "?";
    }
}

// Real per-type param counts (see each FX_TYPE_*'s own doc comment in fx_types.h for exactly what
// each param does). Indexed directly by fx_type.
static int hds_fx_type_param_count(int fx_type) {
    static const int counts[] = {
        /* OFF */ 0, /* ENSEMBLE */ 2, /* CREAMCOAT */ 5, /* DENSITY3 */ 3, /* STEREOCHORUS */ 2,
        /* STEREODOUBLER */ 2, /* TAPEDELAY2 */ 7, /* POINTYGUITAR */ 10, /* GUITARCONDITIONER */ 0,
        /* CABS2 */ 2, /* NAM */ 3, /* GATE */ 4, /* EQ3 */ 7, /* LIMITER */ 3, /* FETCOMP */ 8,
        /* FAIRKID67 */ 8, /* PEAKMAX */ 7, /* MULTIFILTER */ 4, /* OPTOCOMP */ 5, /* DEESSER */ 6,
        /* HARDTUNE */ 10, /* FDNVERB */ 10,
    };
    if (fx_type < 0 || fx_type >= (int)(sizeof(counts) / sizeof(counts[0]))) return 0;
    return counts[fx_type];
}

// Real, short per-param labels, ported directly from each FX_TYPE_*'s own doc comment in
// fx_types.h (the authoritative description of what param[idx] actually does for that effect) --
// abbreviated to fit this GUI's own compact knob column (frontend.py has room for a full combo-box
// label; this flat list doesn't), never invented. Every knob is drawn with its own label directly
// above it -- see hds_fx_draw_knob's own comment -- so nothing is ever a bare numbered pot.
static const char *hds_fx_type_param_label(int fx_type, int idx) {
    static const char *const ensemble[] = {"Depth", "Level"};
    static const char *const creamcoat[] = {"Select", "Regen", "DeRez", "PreDly", "Wet"};
    static const char *const density3[] = {"Densty", "HiPass", "Outpt"};
    static const char *const stereochorus[] = {"Speed", "Depth"};
    static const char *const stereodoubler[] = {"Detune", "Dry/Wet"};
    static const char *const tapedelay2[] = {"Time", "Regen", "Freq", "Reso", "Flutr", "Dry/Wet", "PingPn"};
    static const char *const pointyguitar[] = {"Drive", "Presnc", "High", "Mid", "Low", "Sub", "HSpkr", "LSpkr", "Gate", "Outpt"};
    static const char *const cabs2[] = {"Lowpas", "Hipass"};
    static const char *const gate[] = {"Thresh", "Attack", "Releas", "Range"};
    static const char *const eq3[] = {"LoFreq", "LoGain", "MidFrq", "MidGan", "MidQ", "HiFreq", "HiGain"};
    static const char *const limiter[] = {"Ceil", "InGain", "Releas"};
    static const char *const fetcomp[] = {"Input", "Outpt", "Ratio", "Attack", "Releas", "Limit", "Char", "Iron"};
    static const char *const fairkid67[] = {"Input", "Outpt", "Thresh", "TimeCn", "Link", "Bias", "Iron", "SCGain"};
    static const char *const peakmax[] = {"Thresh", "Ceil", "AutoRl", "Releas", "Quant", "Dither", "Shape"};
    static const char *const multifilter[] = {"Type", "Cutoff", "Reso", "Slope"};
    static const char *const optocomp[] = {"Thresh", "Ratio", "Attack", "Releas", "Makeup"};
    static const char *const deesser[] = {"Freq", "Thresh", "Ratio", "Attack", "Releas", "Listen"};
    static const char *const hardtune[] = {"Key", "Scale", "Target", "Amount", "Retune", "Track", "Voice", "VibRt", "VibDp", "VibSrc"};
    static const char *const fdnverb[] = {"Decay", "Size", "PreDly", "Damp", "Diffus", "ModRt", "ModDp", "ERLvl", "ERDcy", "Freeze"};

    const char *const *table = NULL;
    int count = 0;
    switch (fx_type) {
        case FX_TYPE_ENSEMBLE: table = ensemble; count = 2; break;
        case FX_TYPE_CREAMCOAT: table = creamcoat; count = 5; break;
        case FX_TYPE_DENSITY3: table = density3; count = 3; break;
        case FX_TYPE_STEREOCHORUS: table = stereochorus; count = 2; break;
        case FX_TYPE_STEREODOUBLER: table = stereodoubler; count = 2; break;
        case FX_TYPE_TAPEDELAY2: table = tapedelay2; count = 7; break;
        case FX_TYPE_POINTYGUITAR: table = pointyguitar; count = 10; break;
        case FX_TYPE_CABS2: table = cabs2; count = 2; break;
        case FX_TYPE_GATE: table = gate; count = 4; break;
        case FX_TYPE_EQ3: table = eq3; count = 7; break;
        case FX_TYPE_LIMITER: table = limiter; count = 3; break;
        case FX_TYPE_FETCOMP: table = fetcomp; count = 8; break;
        case FX_TYPE_FAIRKID67: table = fairkid67; count = 8; break;
        case FX_TYPE_PEAKMAX: table = peakmax; count = 7; break;
        case FX_TYPE_MULTIFILTER: table = multifilter; count = 4; break;
        case FX_TYPE_OPTOCOMP: table = optocomp; count = 5; break;
        case FX_TYPE_DEESSER: table = deesser; count = 6; break;
        case FX_TYPE_HARDTUNE: table = hardtune; count = 10; break;
        case FX_TYPE_FDNVERB: table = fdnverb; count = 10; break;
        default: return "";
    }
    if (idx < 0 || idx >= count) return "";
    return table[idx];
}

// Only FX_TYPE_FAIRKID67 actually reads sidechain_source (see its own comment in fx_types.h) -- the
// Sidechain pill stays visible on every row for layout consistency (a row's own x-positions never
// shift by type, keeping draw and hit-test trivially in sync) but is greyed out and inert on every
// other type, directly fixing "sidechain shouldn't be an option on stuff like reverb."
static bool hds_fx_type_uses_sidechain(int fx_type) { return fx_type == FX_TYPE_FAIRKID67; }

// ---------------------------------------------------------------------------------------------
// Chain-link helpers -- a "chain" is any run of consecutive-BY-SOURCE slots joined by
// Chain-to-Next (see process_fx_chain()'s own Parallel-mode comment in fx_chain.c: the next OWNED
// slot of the SAME fx_source, not necessarily the physically-next pool index). These two predicates
// are the single source of truth for which slot's Input/Output pill is live, and which slots may be
// reordered -- both the GUI and the hit-test logic below call these rather than re-deriving the
// same thing twice.
// ---------------------------------------------------------------------------------------------

// The nearest EARLIER slot sharing this slot's own fx_source (the slot chain_to_next chains FROM,
// if any), or -1 if this is that source's own first slot in the pool.
static int hds_fx_prev_same_source_slot(const HdsFxPlugin *p, int slot) {
    int src = (int)p->fx[slot].fx_source;
    for (int i = slot - 1; i >= 0; i--) if ((int)p->fx[i].fx_source == src) return i;
    return -1;
}

// True if this slot starts its own chain (nothing chains INTO it) -- its own Input pill is live.
static bool hds_fx_slot_is_chain_head(const HdsFxPlugin *p, int slot) {
    int prev = hds_fx_prev_same_source_slot(p, slot);
    return prev < 0 || p->fx[prev].chain_to_next == 0.0f;
}

// True if this slot is the TAIL of its own chain (chains nowhere further) -- its own Output pill is
// live. Simply "not chaining forward"; every slot is exactly one of {chains forward} or {is a tail}.
static bool hds_fx_slot_is_chain_tail(const HdsFxPlugin *p, int slot) {
    return p->fx[slot].chain_to_next == 0.0f;
}

// True if a value-swap (Up/Down reorder) touching this slot could silently rewire a chain link --
// either this slot receives from a previous slot's Chain-to-Next, or this slot itself chains
// forward. A chain link is defined by ARRAY POSITION + matching fx_source, not by an ID that
// travels with a slot's own params, so swapping either end of a link would move the wrong slot's
// configuration into the position the link actually depends on -- disallowed outright rather than
// attempting a more surgical (and more bug-prone) partial allowance. See hds_fx_plugin.c's own
// header comment.
static bool hds_fx_slot_participates_in_chain(const HdsFxPlugin *p, int slot) {
    return !hds_fx_slot_is_chain_head(p, slot) || !hds_fx_slot_is_chain_tail(p, slot);
}

// ---------------------------------------------------------------------------------------------
// Interaction/view state -- purely which row is hovered/dragged and how far the list is scrolled;
// nothing here is audio state (that's HdsFxPlugin's own fx[]/fx_engine[]) and nothing here is
// platform state (that's HdsFxGuiWindow in hds_fx_gui_x11.c). Owned by the platform shim, passed by
// pointer into every function below.
// ---------------------------------------------------------------------------------------------
typedef struct {
    int window_w, window_h;
    double scroll_offset_px;
    int hover_row; // -1 = none

    // Drag state: drag_kind 0 = none, 1 = Mix knob, 2 = a param knob (drag_param_index valid).
    int drag_kind;
    int drag_row;
    int drag_param_index;
    double drag_start_y;
    float drag_start_value;
} HdsFxGuiViewState;

static void hds_fx_gui_view_state_init(HdsFxGuiViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->window_w = HDS_FX_GUI_DEFAULT_W;
    vs->window_h = HDS_FX_GUI_DEFAULT_H;
    vs->hover_row = -1;
    vs->drag_kind = 0;
}

// ---------------------------------------------------------------------------------------------
// Drawing primitives (see this file's own header comment on the cairo_new_path() defensive calls --
// cairo_show_text() leaves a "current point" that the next cairo_arc() would otherwise implicitly
// connect to with a stray line before stroking; ported from the mockup that found this bug).
// ---------------------------------------------------------------------------------------------
static void hds_fx_rgb(cairo_t *cr, double r, double g, double b) { cairo_set_source_rgb(cr, r, g, b); }
static void hds_fx_rgba(cairo_t *cr, double r, double g, double b, double a) { cairo_set_source_rgba(cr, r, g, b, a); }

static void hds_fx_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_path(cr);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void hds_fx_centered_text(cairo_t *cr, double cx, double y, const char *text) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - ext.width / 2, y);
    cairo_show_text(cr, text);
    cairo_new_path(cr);
}

static void hds_fx_left_text(cairo_t *cr, double x, double y, const char *text) {
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    cairo_new_path(cr);
}

static void hds_fx_draw_pill(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled) {
    hds_fx_rounded_rect(cr, x, y, w, h, 4);
    if (disabled) hds_fx_rgb(cr, 0.16, 0.16, 0.18);
    else if (active) hds_fx_rgb(cr, 0.22, 0.32, 0.36);
    else hds_fx_rgb(cr, 0.19, 0.19, 0.22);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    if (disabled) hds_fx_rgb(cr, 0.42, 0.42, 0.45);
    else hds_fx_rgb(cr, 0.85, 0.90, 0.90);
    hds_fx_centered_text(cr, x + w / 2, y + h / 2 + 3.5, text);
}

// A small knob: an optional short NAME label above it (what this pot actually does -- e.g.
// "Thresh", "Attack" -- see hds_fx_type_param_label(); pass NULL/"" for a knob that's already
// identified some other way, like Mix under its own "MIX" column header), the arc/tick itself, and
// its own current value as a "NN%" readout directly below. All three pieces are drawn at FIXED
// offsets from the row's own top (HDS_FX_KNOB_TOP_LABEL_Y_OFFSET/HDS_FX_KNOB_CY_OFFSET/
// HDS_FX_KNOB_VALUE_Y_OFFSET) rather than derived from the knob's own cy/radius, so every knob in
// every row lines up identically regardless of position, and the whole label+knob+value group is
// guaranteed to land inside this one row's own bounds -- never bleeding into the row above or
// below. row_y is the row's own top (not the knob's cy) so the caller doesn't have to re-derive it.
static void hds_fx_draw_knob(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled, double row_y, const char *name_label) {
    if (name_label && name_label[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7.5);
        hds_fx_rgb(cr, disabled ? 0.38 : 0.70, disabled ? 0.38 : 0.72, disabled ? 0.40 : 0.74);
        hds_fx_centered_text(cr, cx, row_y + HDS_FX_KNOB_TOP_LABEL_Y_OFFSET, name_label);
    }
    if (value01 < 0.0f) value01 = 0.0f; else if (value01 > 1.0f) value01 = 1.0f;
    double start = M_PI * 0.75, sweep = M_PI * 1.5;
    cairo_new_path(cr);
    cairo_set_line_width(cr, 2.4);
    hds_fx_rgb(cr, 0.20, 0.20, 0.24);
    cairo_arc(cr, cx, cy, radius, start, start + sweep);
    cairo_stroke(cr);
    cairo_new_path(cr);
    if (!disabled) {
        hds_fx_rgb(cr, 0.45, 0.68, 0.95);
        cairo_arc(cr, cx, cy, radius, start, start + sweep * (double)value01);
        cairo_stroke(cr);
        cairo_new_path(cr);
    }
    hds_fx_rgb(cr, disabled ? 0.15 : 0.16, disabled ? 0.15 : 0.16, disabled ? 0.17 : 0.19);
    cairo_arc(cr, cx, cy, radius - 3.5, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_new_path(cr);
    if (!disabled) {
        double angle = start + sweep * (double)value01;
        hds_fx_rgb(cr, 0.9, 0.9, 0.92);
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, cx + cos(angle) * (radius - 8), cy + sin(angle) * (radius - 8));
        cairo_line_to(cr, cx + cos(angle) * (radius - 3.5), cy + sin(angle) * (radius - 3.5));
        cairo_stroke(cr);
        cairo_new_path(cr);
    }

    char value_text[8];
    snprintf(value_text, sizeof(value_text), "%d%%", (int)(value01 * 100.0f + 0.5f));
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 7.5);
    hds_fx_rgb(cr, disabled ? 0.38 : 0.62, disabled ? 0.38 : 0.62, disabled ? 0.40 : 0.65);
    hds_fx_centered_text(cr, cx, row_y + HDS_FX_KNOB_VALUE_Y_OFFSET, value_text);
}

// ---------------------------------------------------------------------------------------------
// Row layout -- ONE function computes every sub-control's rect for a given row's absolute Y
// position; both drawing and hit-testing call this so they can never disagree about where
// anything is. Positions are fixed regardless of the row's own fx_type (param rects are always
// computed for all FX_SLOT_PARAM_COUNT slots) -- the caller decides how many of the trailing ones
// are actually meaningful via hds_fx_type_param_count(), keeping this geometry dead simple.
// Column order: # | Type | In | SC | [params...] | Out | Mix | By | Chain | Up/Down.
// ---------------------------------------------------------------------------------------------
typedef struct { double x, y, w, h; } HdsFxRect;

typedef struct {
    HdsFxRect row;
    HdsFxRect type_pill;
    HdsFxRect input_pill;
    HdsFxRect sidechain_pill;
    HdsFxRect param_knob[FX_SLOT_PARAM_COUNT];
    HdsFxRect output_pill;
    HdsFxRect mix_knob;
    HdsFxRect bypass_btn;
    HdsFxRect chain_btn;
    HdsFxRect up_btn;
    HdsFxRect down_btn;
} HdsFxRowLayout;

#define HDS_FX_ROW_MARGIN 8.0
#define HDS_FX_NUM_W 20.0
#define HDS_FX_TYPE_W 116.0
#define HDS_FX_IO_PILL_W 70.0
#define HDS_FX_SIDECHAIN_W 62.0
#define HDS_FX_PARAM_STRIDE 30.0
#define HDS_FX_MIX_W 30.0
#define HDS_FX_BYPASS_W 22.0
#define HDS_FX_CHAIN_W 18.0
#define HDS_FX_UPDOWN_W 16.0
#define HDS_FX_GAP 6.0

static void hds_fx_gui_compute_row_layout(int width, double y, HdsFxRowLayout *out) {
    memset(out, 0, sizeof(*out));
    double x = HDS_FX_ROW_MARGIN;
    out->row = (HdsFxRect){0, y, (double)width, HDS_FX_GUI_ROW_H};
    x += HDS_FX_NUM_W + HDS_FX_GAP;
    out->type_pill = (HdsFxRect){x, y + 4, HDS_FX_TYPE_W, HDS_FX_GUI_ROW_H - 8};
    x += HDS_FX_TYPE_W + HDS_FX_GAP;
    out->input_pill = (HdsFxRect){x, y + 4, HDS_FX_IO_PILL_W, HDS_FX_GUI_ROW_H - 8};
    x += HDS_FX_IO_PILL_W + HDS_FX_GAP;
    out->sidechain_pill = (HdsFxRect){x, y + 4, HDS_FX_SIDECHAIN_W, HDS_FX_GUI_ROW_H - 8};
    x += HDS_FX_SIDECHAIN_W + HDS_FX_GAP;
    for (int i = 0; i < FX_SLOT_PARAM_COUNT; i++) {
        out->param_knob[i] = (HdsFxRect){x + i * HDS_FX_PARAM_STRIDE + HDS_FX_PARAM_STRIDE / 2, y + HDS_FX_KNOB_CY_OFFSET, HDS_FX_GUI_KNOB_R, HDS_FX_GUI_KNOB_R};
    }
    x += FX_SLOT_PARAM_COUNT * HDS_FX_PARAM_STRIDE + HDS_FX_GAP;
    out->output_pill = (HdsFxRect){x, y + 4, HDS_FX_IO_PILL_W, HDS_FX_GUI_ROW_H - 8};
    x += HDS_FX_IO_PILL_W + HDS_FX_GAP;
    out->mix_knob = (HdsFxRect){x + HDS_FX_MIX_W / 2, y + HDS_FX_KNOB_CY_OFFSET, HDS_FX_GUI_KNOB_R, HDS_FX_GUI_KNOB_R};
    x += HDS_FX_MIX_W + HDS_FX_GAP;
    out->bypass_btn = (HdsFxRect){x, y + (HDS_FX_GUI_ROW_H - HDS_FX_BYPASS_W) / 2, HDS_FX_BYPASS_W, HDS_FX_BYPASS_W};
    x += HDS_FX_BYPASS_W + HDS_FX_GAP;
    out->chain_btn = (HdsFxRect){x, y + (HDS_FX_GUI_ROW_H - HDS_FX_BYPASS_W) / 2, HDS_FX_CHAIN_W, HDS_FX_BYPASS_W};
    x += HDS_FX_CHAIN_W + HDS_FX_GAP;
    out->up_btn = (HdsFxRect){x, y + 2, HDS_FX_UPDOWN_W, HDS_FX_GUI_ROW_H / 2 - 3};
    out->down_btn = (HdsFxRect){x, y + HDS_FX_GUI_ROW_H / 2 + 1, HDS_FX_UPDOWN_W, HDS_FX_GUI_ROW_H / 2 - 3};
}

static bool hds_fx_rect_contains(const HdsFxRect *r, double px, double py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}
// Knob rects store {cx, cy, radius, radius} (see hds_fx_gui_compute_row_layout) -- a knob's own
// click target is a plain radius test, not the bounding-box test hds_fx_rect_contains does for
// every rectangular control.
static bool hds_fx_knob_contains(const HdsFxRect *r, double px, double py) {
    double dx = px - r->x, dy = py - r->y;
    return dx * dx + dy * dy <= r->w * r->w;
}

// Column header strip (just above the slot list) -- reuses hds_fx_gui_compute_row_layout at y=0
// purely to read off the same X positions every row itself uses, so a header label can never drift
// out of alignment with the column it names.
#define HDS_FX_COLUMN_HEADER_Y 50.0

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------
static double hds_fx_gui_content_height(void) { return FX_SLOT_COUNT * HDS_FX_GUI_ROW_H; }

static double hds_fx_gui_max_scroll(const HdsFxGuiViewState *vs) {
    double visible = (double)vs->window_h - HDS_FX_GUI_HEADER_H;
    double max_scroll = hds_fx_gui_content_height() - visible;
    return max_scroll > 0.0 ? max_scroll : 0.0;
}

// "In 3" (mono) or "In 3+4" (stereo) -- same formatter for both Input and Output pills, since both
// address a port + Mono/Stereo choice the same way (see hds_fx_plugin.c's own header comment).
static void hds_fx_format_io_pill(char *buf, size_t buflen, const char *prefix, int port, bool stereo) {
    if (stereo) snprintf(buf, buflen, "%s %d+%d", prefix, port + 1, port + 2);
    else snprintf(buf, buflen, "%s %d", prefix, port + 1);
}

static void hds_fx_gui_render(cairo_t *cr, HdsFxPlugin *p, const HdsFxGuiViewState *vs) {
    int width = vs->window_w, height = vs->window_h;

    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    // Title
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    hds_fx_rgb(cr, 0.95, 0.55, 0.15);
    hds_fx_left_text(cr, HDS_FX_ROW_MARGIN, 22, "HDS FX");
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    hds_fx_rgb(cr, 0.5, 0.5, 0.53);
    hds_fx_left_text(cr, 90, 21, "8 stereo ports \xc2\xb7 32 shared FX slots \xc2\xb7 each chain picks its own Input/Output, Mono or Stereo");

    // Column headers -- reads the SAME x-positions every row itself uses (hds_fx_gui_compute_row_layout
    // at y=0, ignoring its y-derived fields entirely) so a label can never drift out of alignment
    // with its own column.
    {
        HdsFxRowLayout ref;
        hds_fx_gui_compute_row_layout(width, 0, &ref);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 8);
        hds_fx_rgb(cr, 0.5, 0.5, 0.53);
        hds_fx_centered_text(cr, ref.type_pill.x + ref.type_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "TYPE");
        hds_fx_centered_text(cr, ref.input_pill.x + ref.input_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "IN");
        hds_fx_centered_text(cr, ref.sidechain_pill.x + ref.sidechain_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "SC");
        double params_cx = (ref.param_knob[0].x + ref.param_knob[FX_SLOT_PARAM_COUNT - 1].x) / 2.0;
        hds_fx_centered_text(cr, params_cx, HDS_FX_COLUMN_HEADER_Y, "PARAMS");
        hds_fx_centered_text(cr, ref.output_pill.x + ref.output_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "OUT");
        hds_fx_centered_text(cr, ref.mix_knob.x, HDS_FX_COLUMN_HEADER_Y, "MIX");
        hds_fx_centered_text(cr, ref.bypass_btn.x + ref.bypass_btn.w / 2, HDS_FX_COLUMN_HEADER_Y, "BY");
        hds_fx_centered_text(cr, ref.chain_btn.x + ref.chain_btn.w / 2, HDS_FX_COLUMN_HEADER_Y, "CH");
    }

    // Clip the slot list to the area below the header so scrolling never draws over it.
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, 0, HDS_FX_GUI_HEADER_H, width, height - HDS_FX_GUI_HEADER_H);
    cairo_clip(cr);

    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        double y = HDS_FX_GUI_HEADER_H + s * HDS_FX_GUI_ROW_H - vs->scroll_offset_px;
        if (y + HDS_FX_GUI_ROW_H < HDS_FX_GUI_HEADER_H || y > height) continue; // off-screen, skip

        HdsFxRowLayout L;
        hds_fx_gui_compute_row_layout(width, y, &L);
        const FXSlotParams *slot = &p->fx[s];
        int fx_type = (int)slot->fx_type;
        int port = (int)slot->fx_source;
        if (port < 0 || port >= HDS_FX_PORT_COUNT) port = 0;
        int param_count = hds_fx_type_param_count(fx_type);
        bool uses_sc = hds_fx_type_uses_sidechain(fx_type);
        bool is_head = hds_fx_slot_is_chain_head(p, s);
        bool is_tail = hds_fx_slot_is_chain_tail(p, s);

        if (s == vs->hover_row) { hds_fx_rgba(cr, 1, 1, 1, 0.03); cairo_new_path(cr); cairo_rectangle(cr, L.row.x, L.row.y, L.row.w, L.row.h); cairo_fill(cr); cairo_new_path(cr); }

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9);
        hds_fx_rgb(cr, 0.42, 0.42, 0.45);
        char numbuf[8]; snprintf(numbuf, sizeof(numbuf), "%02d", s + 1);
        hds_fx_left_text(cr, HDS_FX_ROW_MARGIN, y + HDS_FX_GUI_ROW_H / 2 + 3, numbuf);

        hds_fx_draw_pill(cr, L.type_pill.x, L.type_pill.y, L.type_pill.w, L.type_pill.h, hds_fx_type_name(fx_type), fx_type != FX_TYPE_OFF, false);

        // Input pill: only live on a chain HEAD -- a continuation slot has no Input of its own, it
        // just inherits the running signal from whatever chains into it.
        {
            char inbuf[16];
            hds_fx_format_io_pill(inbuf, sizeof(inbuf), "In", port, p->port_input_stereo[port] != 0.0f);
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, inbuf, is_head, !is_head);
        }

        if (uses_sc) {
            int sc = (int)slot->sidechain_source;
            char scbuf[16];
            if (sc < 0 || sc >= HDS_FX_PORT_COUNT) snprintf(scbuf, sizeof(scbuf), "= Src");
            else snprintf(scbuf, sizeof(scbuf), "SC In%d", sc + 1);
            hds_fx_draw_pill(cr, L.sidechain_pill.x, L.sidechain_pill.y, L.sidechain_pill.w, L.sidechain_pill.h, scbuf, sc >= 0, false);
        } else {
            hds_fx_draw_pill(cr, L.sidechain_pill.x, L.sidechain_pill.y, L.sidechain_pill.w, L.sidechain_pill.h, "\xe2\x80\x94", false, true);
        }

        for (int i = 0; i < param_count; i++) {
            hds_fx_draw_knob(cr, L.param_knob[i].x, L.param_knob[i].y, L.param_knob[i].w, slot->param[i], false, y, hds_fx_type_param_label(fx_type, i));
        }

        // Output pill: only live on a chain TAIL -- a slot still chaining forward doesn't decide
        // where anything lands yet, the tail of its own chain does.
        {
            int out_ch = (int)p->port_output_channel[port];
            if (out_ch < 0 || out_ch >= HDS_FX_PORT_COUNT) out_ch = port;
            char outbuf[16];
            hds_fx_format_io_pill(outbuf, sizeof(outbuf), "Out", out_ch, p->port_output_stereo[port] != 0.0f);
            hds_fx_draw_pill(cr, L.output_pill.x, L.output_pill.y, L.output_pill.w, L.output_pill.h, outbuf, is_tail, !is_tail);
        }

        hds_fx_draw_knob(cr, L.mix_knob.x, L.mix_knob.y, L.mix_knob.w, slot->mix / 100.0f, false, y, NULL);

        bool bypassed = slot->bypass != 0.0f;
        hds_fx_rounded_rect(cr, L.bypass_btn.x, L.bypass_btn.y, L.bypass_btn.w, L.bypass_btn.h, 3);
        hds_fx_rgb(cr, bypassed ? 0.72 : 0.20, bypassed ? 0.45 : 0.20, bypassed ? 0.18 : 0.23);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_set_font_size(cr, 8);
        hds_fx_rgb(cr, bypassed ? 0.05 : 0.6, bypassed ? 0.05 : 0.6, bypassed ? 0.05 : 0.63);
        hds_fx_centered_text(cr, L.bypass_btn.x + L.bypass_btn.w / 2, L.bypass_btn.y + L.bypass_btn.h / 2 + 3, "By");

        // Chain-to-Next: always live, unconditionally -- no mode to gate it (see this file's own
        // header comment). Disabled only on the very last pool slot, which has no "next" at all.
        bool chain_on = slot->chain_to_next != 0.0f;
        bool chain_available = s < FX_SLOT_COUNT - 1;
        hds_fx_rounded_rect(cr, L.chain_btn.x, L.chain_btn.y, L.chain_btn.w, L.chain_btn.h, 3);
        if (chain_on) hds_fx_rgb(cr, 0.18, 0.55, 0.35);
        else hds_fx_rgb(cr, 0.20, 0.20, 0.23);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_set_font_size(cr, 9);
        hds_fx_rgb(cr, chain_on ? 0.9 : (chain_available ? 0.5 : 0.3), chain_on ? 0.95 : (chain_available ? 0.5 : 0.3), chain_on ? 0.9 : (chain_available ? 0.53 : 0.32));
        hds_fx_centered_text(cr, L.chain_btn.x + L.chain_btn.w / 2, L.chain_btn.y + L.chain_btn.h / 2 + 3, "\xe2\x86\x92");

        // Up/Down: disabled at the pool's own edges AND on any slot touching a chain link (either
        // end) -- see hds_fx_slot_participates_in_chain()'s own comment for why swapping a chained
        // slot is unsafe, not just inconvenient.
        bool chained = hds_fx_slot_participates_in_chain(p, s);
        bool can_up = s > 0 && !chained, can_down = s < FX_SLOT_COUNT - 1 && !chained;
        hds_fx_rgb(cr, can_up ? 0.65 : 0.28, can_up ? 0.65 : 0.28, can_up ? 0.68 : 0.30);
        cairo_set_font_size(cr, 8);
        hds_fx_centered_text(cr, L.up_btn.x + L.up_btn.w / 2, L.up_btn.y + L.up_btn.h / 2 + 3, "\xe2\x96\xb2");
        hds_fx_rgb(cr, can_down ? 0.65 : 0.28, can_down ? 0.65 : 0.28, can_down ? 0.68 : 0.30);
        hds_fx_centered_text(cr, L.down_btn.x + L.down_btn.w / 2, L.down_btn.y + L.down_btn.h / 2 + 3, "\xe2\x96\xbc");
    }

    cairo_restore(cr);

    // Scrollbar hint (a thin track on the right edge -- not draggable in this first pass, just a
    // position indicator; the mouse wheel is the only way to scroll for now).
    double max_scroll = hds_fx_gui_max_scroll(vs);
    if (max_scroll > 0.0) {
        double visible_h = (double)height - HDS_FX_GUI_HEADER_H;
        double content_h = hds_fx_gui_content_height();
        double thumb_h = visible_h * (visible_h / content_h);
        double thumb_y = HDS_FX_GUI_HEADER_H + (visible_h - thumb_h) * (vs->scroll_offset_px / max_scroll);
        hds_fx_rgba(cr, 1, 1, 1, 0.15);
        cairo_new_path(cr);
        cairo_rectangle(cr, width - 4, thumb_y, 3, thumb_h);
        cairo_fill(cr);
        cairo_new_path(cr);
    }
}

// ---------------------------------------------------------------------------------------------
// Interaction -- mutates HdsFxPlugin/HdsFxGuiViewState directly; no host/CLAP types referenced
// anywhere in this file. hds_fx_swap_slots (Up/Down reorder) lives here since it's this file's own
// buttons that call it -- see its own comment for why it only swaps FXSlotParams and never touches
// FXSlotEngine.
// ---------------------------------------------------------------------------------------------

// Slot reorder (Up/Down arrows): swaps two neighboring pool slots' whole FXSlotParams value --
// type/params/mix/bypass/source/chain/sidechain all move together -- exactly matching frontend.py's
// own _swap_fx_slots. FXSlotEngine is deliberately NOT touched: sync_fx_engine_types() already
// detects a type change at each index next block and reallocates accordingly, the same natural
// reset an ordinary Type change already goes through -- there is nothing reorder-specific to do.
// The caller (hds_fx_gui_on_button_press) is responsible for checking
// hds_fx_slot_participates_in_chain() first -- this function itself has no chain awareness.
static void hds_fx_swap_slots(HdsFxPlugin *p, int a, int b) {
    if (a < 0 || b < 0 || a >= FX_SLOT_COUNT || b >= FX_SLOT_COUNT || a == b) return;
    FXSlotParams tmp = p->fx[a];
    p->fx[a] = p->fx[b];
    p->fx[b] = tmp;
}

static int hds_fx_selectable_type_index(int fx_type) {
    for (int i = 0; i < HDS_FX_SELECTABLE_TYPE_COUNT; i++) if (HDS_FX_SELECTABLE_TYPES[i] == fx_type) return i;
    return 0;
}

static void hds_fx_cycle_type(HdsFxPlugin *p, int slot, int dir) {
    int idx = hds_fx_selectable_type_index((int)p->fx[slot].fx_type);
    idx = (idx + dir + HDS_FX_SELECTABLE_TYPE_COUNT) % HDS_FX_SELECTABLE_TYPE_COUNT;
    p->fx[slot].fx_type = (float)HDS_FX_SELECTABLE_TYPES[idx];
}

// Input pill: unified Mono/Stereo + port cycle -- mono positions 0..HDS_FX_PORT_COUNT-1, then
// stereo positions 0..HDS_FX_PORT_COUNT-2 (a stereo pair can't start on the last port, nothing to
// pair it with), wrapping. Mutates p->port_input_stereo[] -- a property of the PORT/chain a slot's
// own fx_source names, not of the individual slot -- see HdsFxPlugin's own comment.
static void hds_fx_cycle_input(HdsFxPlugin *p, int slot, int dir) {
    int port = (int)p->fx[slot].fx_source;
    if (port < 0 || port >= HDS_FX_PORT_COUNT) port = 0;
    bool stereo = p->port_input_stereo[port] != 0.0f;
    int mono_count = HDS_FX_PORT_COUNT, stereo_count = HDS_FX_PORT_COUNT - 1;
    int total = mono_count + stereo_count;
    int pos = stereo ? (mono_count + port) : port;
    pos = ((pos + dir) % total + total) % total;
    int new_port = (pos < mono_count) ? pos : (pos - mono_count);
    bool new_stereo = pos >= mono_count;
    p->fx[slot].fx_source = (float)new_port;
    p->port_input_stereo[new_port] = new_stereo ? 1.0f : 0.0f;
}

// Output pill: same unified cycle as Input, but keyed by fx_source (which chain this is) and
// mutating port_output_channel/port_output_stereo instead -- see HdsFxPlugin's own comment on why
// Output is independent of Input.
static void hds_fx_cycle_output(HdsFxPlugin *p, int slot, int dir) {
    int g = (int)p->fx[slot].fx_source;
    if (g < 0 || g >= HDS_FX_PORT_COUNT) g = 0;
    int ch = (int)p->port_output_channel[g];
    if (ch < 0 || ch >= HDS_FX_PORT_COUNT) ch = g;
    bool stereo = p->port_output_stereo[g] != 0.0f;
    int mono_count = HDS_FX_PORT_COUNT, stereo_count = HDS_FX_PORT_COUNT - 1;
    int total = mono_count + stereo_count;
    int pos = stereo ? (mono_count + ch) : ch;
    pos = ((pos + dir) % total + total) % total;
    int new_ch = (pos < mono_count) ? pos : (pos - mono_count);
    bool new_stereo = pos >= mono_count;
    p->port_output_channel[g] = (float)new_ch;
    p->port_output_stereo[g] = new_stereo ? 1.0f : 0.0f;
}

// -1 ("Same as Source") .. HDS_FX_PORT_COUNT-1, cycling forward/back through that whole range.
static void hds_fx_cycle_sidechain(HdsFxPlugin *p, int slot, int dir) {
    int sc = (int)p->fx[slot].sidechain_source;
    sc = ((sc + 1 + dir) % (HDS_FX_PORT_COUNT + 1) + (HDS_FX_PORT_COUNT + 1)) % (HDS_FX_PORT_COUNT + 1) - 1;
    p->fx[slot].sidechain_source = (float)sc;
}

// Returns true if the click was consumed by a row control (bypass/chain/up/down toggled
// immediately; a knob starts a drag tracked in *vs instead). button: 1 = left (forward-cycle /
// primary action), 3 = right (backward-cycle).
static bool hds_fx_gui_on_button_press(HdsFxPlugin *p, HdsFxGuiViewState *vs, double x, double y, int button) {
    int width = vs->window_w;
    int dir = (button == 3) ? -1 : 1;

    if (y < HDS_FX_GUI_HEADER_H) return false;

    int row = (int)((y - HDS_FX_GUI_HEADER_H + vs->scroll_offset_px) / HDS_FX_GUI_ROW_H);
    if (row < 0 || row >= FX_SLOT_COUNT) return false;
    double row_y = HDS_FX_GUI_HEADER_H + row * HDS_FX_GUI_ROW_H - vs->scroll_offset_px;
    HdsFxRowLayout L;
    hds_fx_gui_compute_row_layout(width, row_y, &L);

    if (hds_fx_rect_contains(&L.type_pill, x, y)) { hds_fx_cycle_type(p, row, dir); return true; }
    if (hds_fx_slot_is_chain_head(p, row) && hds_fx_rect_contains(&L.input_pill, x, y)) { hds_fx_cycle_input(p, row, dir); return true; }
    if (hds_fx_type_uses_sidechain((int)p->fx[row].fx_type) && hds_fx_rect_contains(&L.sidechain_pill, x, y)) {
        hds_fx_cycle_sidechain(p, row, dir);
        return true;
    }
    if (hds_fx_slot_is_chain_tail(p, row) && hds_fx_rect_contains(&L.output_pill, x, y)) { hds_fx_cycle_output(p, row, dir); return true; }
    if (hds_fx_rect_contains(&L.bypass_btn, x, y)) { if (button == 1) p->fx[row].bypass = (p->fx[row].bypass != 0.0f) ? 0.0f : 1.0f; return true; }
    if (hds_fx_rect_contains(&L.chain_btn, x, y)) { if (button == 1 && row < FX_SLOT_COUNT - 1) p->fx[row].chain_to_next = (p->fx[row].chain_to_next != 0.0f) ? 0.0f : 1.0f; return true; }
    if (hds_fx_rect_contains(&L.up_btn, x, y)) { if (button == 1 && !hds_fx_slot_participates_in_chain(p, row)) hds_fx_swap_slots(p, row, row - 1); return true; }
    if (hds_fx_rect_contains(&L.down_btn, x, y)) { if (button == 1 && !hds_fx_slot_participates_in_chain(p, row)) hds_fx_swap_slots(p, row, row + 1); return true; }

    if (button == 1) {
        int param_count = hds_fx_type_param_count((int)p->fx[row].fx_type);
        for (int i = 0; i < param_count; i++) {
            if (hds_fx_knob_contains(&L.param_knob[i], x, y)) {
                vs->drag_kind = 2; vs->drag_row = row; vs->drag_param_index = i;
                vs->drag_start_y = y; vs->drag_start_value = p->fx[row].param[i];
                return true;
            }
        }
        if (hds_fx_knob_contains(&L.mix_knob, x, y)) {
            vs->drag_kind = 1; vs->drag_row = row;
            vs->drag_start_y = y; vs->drag_start_value = p->fx[row].mix;
            return true;
        }
    }
    return false;
}

#define HDS_FX_DRAG_PIXELS_FULL_RANGE 150.0

static void hds_fx_gui_on_motion(HdsFxPlugin *p, HdsFxGuiViewState *vs, double x, double y) {
    if (y >= HDS_FX_GUI_HEADER_H) {
        int row = (int)((y - HDS_FX_GUI_HEADER_H + vs->scroll_offset_px) / HDS_FX_GUI_ROW_H);
        vs->hover_row = (row >= 0 && row < FX_SLOT_COUNT) ? row : -1;
    } else {
        vs->hover_row = -1;
    }
    (void)x;

    if (vs->drag_kind == 0) return;
    double delta = vs->drag_start_y - y; // up = increase, matching every knob's own visual convention
    if (vs->drag_kind == 1) {
        float v = vs->drag_start_value + (float)(delta / HDS_FX_DRAG_PIXELS_FULL_RANGE * 100.0);
        if (v < 0.0f) v = 0.0f; else if (v > 100.0f) v = 100.0f;
        p->fx[vs->drag_row].mix = v;
    } else if (vs->drag_kind == 2) {
        float v = vs->drag_start_value + (float)(delta / HDS_FX_DRAG_PIXELS_FULL_RANGE);
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
        p->fx[vs->drag_row].param[vs->drag_param_index] = v;
    }
}

static void hds_fx_gui_on_button_release(HdsFxGuiViewState *vs) { vs->drag_kind = 0; }

// delta: +1 (wheel up / X11 Button4) or -1 (wheel down / X11 Button5).
static void hds_fx_gui_on_scroll(HdsFxGuiViewState *vs, int delta) {
    vs->scroll_offset_px -= delta * (HDS_FX_GUI_ROW_H * 3.0);
    double max_scroll = hds_fx_gui_max_scroll(vs);
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}

static void hds_fx_gui_on_resize(HdsFxGuiViewState *vs, int w, int h) {
    if (w < HDS_FX_GUI_MIN_W) w = HDS_FX_GUI_MIN_W;
    if (h < HDS_FX_GUI_MIN_H) h = HDS_FX_GUI_MIN_H;
    vs->window_w = w; vs->window_h = h;
    double max_scroll = hds_fx_gui_max_scroll(vs);
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}
