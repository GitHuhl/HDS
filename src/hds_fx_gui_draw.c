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
// feeds its own chain, whether that chain treats the port as Mono or Stereo, or Off -- silence in,
// independent of whatever port is otherwise remembered) -- shown/editable only on a chain HEAD slot
// (hds_fx_slot_is_chain_head()) -- and, once its own Chain-to-Next is off (a chain TAIL), an Output
// (which port the result lands on, independent of the Input, same Mono/Stereo/Off choice).
// Reordering (Up/Down) is disabled outright on any slot touching a chain link, in either direction
// -- see hds_fx_slot_participates_in_chain()'s own comment.
//
// Type/Input/Sidechain/Output each have too many possible values (up to 21) for click-to-cycle to
// be usable -- clicking one of these pills opens a real dropdown option list instead (see the
// "Dropdowns" section further down); Bypass/Chain-to-Next stay plain click-to-toggle buttons (only
// two states each), and Mix/param knobs stay click-and-drag.
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

// Selectable types for the GUI's own Type dropdown -- every real FX_TYPE_*. FX_TYPE_NAM is now
// included (it used to be excluded here specifically because HDS_NAM_ENABLED was 0 in
// hds_fx_plugin.c, which would have let the dropdown land on a type fx_chain.c's own dispatch
// switch couldn't actually run -- see that file's own header comment for why it's wired up for
// real now, model/cab-IR loading included).
static const int HDS_FX_SELECTABLE_TYPES[] = {
    FX_TYPE_OFF, FX_TYPE_ENSEMBLE, FX_TYPE_CREAMCOAT, FX_TYPE_DENSITY3, FX_TYPE_STEREOCHORUS,
    FX_TYPE_STEREODOUBLER, FX_TYPE_TAPEDELAY2, FX_TYPE_POINTYGUITAR, FX_TYPE_GUITARCONDITIONER,
    FX_TYPE_CABS2, FX_TYPE_NAM, FX_TYPE_GATE, FX_TYPE_EQ3, FX_TYPE_LIMITER, FX_TYPE_FETCOMP,
    FX_TYPE_FAIRKID67, FX_TYPE_PEAKMAX, FX_TYPE_MULTIFILTER, FX_TYPE_OPTOCOMP, FX_TYPE_DEESSER,
    FX_TYPE_HARDTUNE, FX_TYPE_FDNVERB,
};
#define HDS_FX_SELECTABLE_TYPE_COUNT (int)(sizeof(HDS_FX_SELECTABLE_TYPES) / sizeof(HDS_FX_SELECTABLE_TYPES[0]))

static int hds_fx_selectable_type_index(int fx_type) {
    for (int i = 0; i < HDS_FX_SELECTABLE_TYPE_COUNT; i++) if (HDS_FX_SELECTABLE_TYPES[i] == fx_type) return i;
    return 0;
}

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
        case FX_TYPE_NAM: return "NAM";
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
    static const char *const nam[] = {"InGain", "OutGan", "Cab"};
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
        case FX_TYPE_NAM: table = nam; count = 3; break;
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

// FX_TYPE_NAM only -- gates whether nam_model_pill/nam_ir_pill (see HdsFxRowLayout's own comment)
// are drawn/clickable on a given row, same "always-present rect, only sometimes meaningful"
// pattern hds_fx_type_uses_sidechain() already established.
static bool hds_fx_type_uses_nam_paths(int fx_type) { return fx_type == FX_TYPE_NAM; }

// The part of a path after the last '/', or the whole path if there's no '/' -- for a compact
// pill label; the full path still lives in FXSlotParams.nam_model_path/nam_ir_path and is what
// actually gets saved/loaded, this is purely a display truncation.
static const char *hds_fx_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// hds_fx_draw_pill() never clips its own text to its own rect (see that function's own body --
// every OTHER label in this GUI is already short enough by construction that this never mattered),
// but a real filename easily overflows nam_model_pill/nam_ir_pill's ~84px of usable width and would
// bleed into neighboring columns. Truncates from the FRONT with a leading ellipsis, keeping the
// END of the string -- for a loaded path that's usually the actual filename past its directory, and
// while actively typing (hds_fx_gui_on_key()) it's exactly where the cursor and newest keystrokes
// are, so the visible text scrolls the way any real text field's viewport would.
static void hds_fx_truncate_pill_text(const char *text, char *buf, size_t buflen, size_t max_visible) {
    size_t len = strlen(text);
    if (len <= max_visible) { snprintf(buf, buflen, "%s", text); return; }
    snprintf(buf, buflen, "\xe2\x80\xa6%s", text + (len - (max_visible - 1)));
}

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
// Which dropdown (if any) is currently open -- Type/Input/Sidechain/Output all have too many
// possible values (up to 21) for click-to-cycle to be usable, so each opens a real option list
// instead (see the "Dropdowns" section further down). Only one can ever be open at a time.
#define HDS_FX_DROPDOWN_NONE 0
#define HDS_FX_DROPDOWN_TYPE 1
#define HDS_FX_DROPDOWN_INPUT 2
#define HDS_FX_DROPDOWN_SIDECHAIN 3
#define HDS_FX_DROPDOWN_OUTPUT 4

typedef struct { double x, y, w, h; } HdsFxRect;

typedef struct {
    int kind;         // one of HDS_FX_DROPDOWN_* above; NONE when closed
    int row;          // which pool slot this dropdown belongs to
    HdsFxRect anchor; // the pill's own rect at the moment it was opened -- positions the list
} HdsFxDropdown;

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

    HdsFxDropdown dropdown;

    // Inline text-edit state -- FX_TYPE_NAM's own Model/IR path fields are the only free-text
    // inputs anywhere in this GUI (every other value is a knob, a toggle, or a dropdown option),
    // and there's no file-browser dialog here (this plugin stays a plain Cairo+X11 build, no GTK/Qt
    // toolkit dependency -- see this file's own header comment) -- so typing/pasting a path
    // directly is the mechanism. edit_row < 0 means nothing is being edited; edit_field 0 = Model,
    // 1 = IR. edit_buffer is pre-filled with the slot's CURRENT path when editing starts (see
    // hds_fx_gui_on_button_press()'s own NAM-pill handling), so backspacing an existing path works
    // exactly like editing any other text field, not just typing one from scratch. See
    // hds_fx_gui_on_key()'s own comment for how a real X11 keypress reaches this state.
    int edit_row;
    int edit_field;
    char edit_buffer[SAMPLE_PATH_LEN];
} HdsFxGuiViewState;

static void hds_fx_gui_view_state_init(HdsFxGuiViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->window_w = HDS_FX_GUI_DEFAULT_W;
    vs->window_h = HDS_FX_GUI_DEFAULT_H;
    vs->hover_row = -1;
    vs->drag_kind = 0;
    vs->edit_row = -1;
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
typedef struct {
    HdsFxRect row;
    HdsFxRect type_pill;
    HdsFxRect input_pill;
    HdsFxRect sidechain_pill;
    HdsFxRect param_knob[FX_SLOT_PARAM_COUNT];
    // FX_TYPE_NAM only ever uses param_knob[0..2] (Input/Output Gain, Cab Enable -- see
    // hds_fx_type_param_count()) -- columns 3-8 are otherwise blank real estate for that type, so
    // NAM's own Model/IR path fields reuse them as two wide clickable pills rather than adding any
    // new row width. Computed directly alongside param_knob[] in hds_fx_gui_compute_row_layout()
    // from the same running `x`, not back-derived from already-computed knob centers, so draw and
    // hit-test share one single source of truth the same way every other control here does. Rects
    // exist unconditionally on every row (same "always present, only sometimes meaningful" pattern
    // sidechain_pill already uses) -- only relevant when hds_fx_type_uses_nam_paths() is true.
    HdsFxRect nam_model_pill;
    HdsFxRect nam_ir_pill;
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
    // Columns 3,4,5 and 6,7,8 -- see this struct's own comment on nam_model_pill/nam_ir_pill for
    // why these specific columns (NAM's own 3 real knobs stop at column 2; column 9 is left as
    // ordinary trailing gap, same spacing every other type already has before the Output pill).
    out->nam_model_pill = (HdsFxRect){x + 3 * HDS_FX_PARAM_STRIDE, y + 4, 3 * HDS_FX_PARAM_STRIDE - HDS_FX_GAP, HDS_FX_GUI_ROW_H - 8};
    out->nam_ir_pill = (HdsFxRect){x + 6 * HDS_FX_PARAM_STRIDE, y + 4, 3 * HDS_FX_PARAM_STRIDE - HDS_FX_GAP, HDS_FX_GUI_ROW_H - 8};
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

// "In 3" (mono) or "In 3 St" (stereo) -- Input only. There is no port-COMBINING happening here or
// anywhere else in this file: each of the HDS_FX_PORT_COUNT CLAP ports is already a genuine 2-
// channel (L+R) stereo pair on its own (see hds_fx_audio_ports_get() in hds_fx_plugin.c), so Mono
// vs Stereo is purely "read this ONE port's L only, centered" vs "read this one port's L and R for
// real" -- never "combine two separate ports into one signal". An earlier revision of this GUI
// formatted the stereo case as "In 3+4", which was flat wrong (it implied combining ports 3 and 4,
// something no code anywhere ever did) and confirmed as a real, confusing bug from live testing:
// selecting a labeled "N+N+1" option changed nothing about which second port was touched, because
// there never was one. Output has no Mono/Stereo choice at all any more (see this file's own
// "Dropdowns" section for why) so this formatter is Input-only now.
static void hds_fx_format_input_pill(char *buf, size_t buflen, int port, bool stereo) {
    if (stereo) snprintf(buf, buflen, "In %d St", port + 1);
    else snprintf(buf, buflen, "In %d", port + 1);
}

// ---------------------------------------------------------------------------------------------
// Dropdowns -- Type (21 options), Input (Off + 8 mono + 8 stereo = 17), Output (Off + 8 = 9), and
// Sidechain (Same as Source + 8 = 9) all have too many possible values for click-to-cycle to be
// usable. Clicking one of these pills opens a real option list instead (see
// HdsFxGuiViewState.dropdown); clicking an option applies it and closes the list, clicking
// anywhere else just closes it.
//
// Output has NO Mono/Stereo choice, unlike Input -- a real, confirmed bug from live testing, not a
// design nuance: every one of the HDS_FX_PORT_COUNT CLAP ports is ALREADY a fixed 2-channel (L+R)
// stereo pair (hds_fx_audio_ports_get() declares channel_count=2 for all of them), so there was
// never any way to make an OUTPUT selection land on a single independent physical channel without
// redeclaring the whole port layout as 16 mono ports instead of 8 stereo ones -- a much bigger,
// riskier change than this plugin's first real release should take on. The earlier "Mono output"
// choice didn't deliver that anyway: it just wrote the SAME centered value to BOTH channels of the
// chosen port, which is not "one specified channel" by any reasonable reading, and its "N+N+1"
// label falsely implied combining two separate ports that never actually happened in the DSP.
// Given the choice between building real 16-channel mono addressability (a real, separate,
// scoped follow-up) and a confusing half-measure, Output is simply always the chosen port's own
// real stereo pair now: pick a port, get exactly that port's L and R, nothing else, ever. Input
// KEEPS its Mono/Stereo choice (reading one port's L only, centered, vs its real L+R) -- that half
// of the design was never in question, only its label was wrong (fixed below, see
// hds_fx_format_input_pill()'s own comment) and its option count had a real off-by-one (previously
// only 7 stereo options, silently missing port 8 entirely).
// ---------------------------------------------------------------------------------------------

static int hds_fx_dropdown_option_count(int kind) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE: return HDS_FX_SELECTABLE_TYPE_COUNT;
        case HDS_FX_DROPDOWN_INPUT: return 1 + HDS_FX_PORT_COUNT + HDS_FX_PORT_COUNT; // Off + 8 mono + 8 stereo
        case HDS_FX_DROPDOWN_OUTPUT: return 1 + HDS_FX_PORT_COUNT; // Off + 8 (always that port's real stereo pair)
        case HDS_FX_DROPDOWN_SIDECHAIN: return 1 + HDS_FX_PORT_COUNT; // "Same as Source" + 8
        default: return 0;
    }
}

// Pure text for option `idx` -- doesn't need plugin state, just which list and which row within it.
static void hds_fx_dropdown_option_label(int kind, int idx, char *buf, size_t buflen) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE:
            snprintf(buf, buflen, "%s", hds_fx_type_name(HDS_FX_SELECTABLE_TYPES[idx]));
            return;
        case HDS_FX_DROPDOWN_INPUT: {
            if (idx == 0) { snprintf(buf, buflen, "Off"); return; }
            int j = idx - 1;
            bool stereo = j >= HDS_FX_PORT_COUNT;
            int port = stereo ? (j - HDS_FX_PORT_COUNT) : j;
            hds_fx_format_input_pill(buf, buflen, port, stereo);
            return;
        }
        case HDS_FX_DROPDOWN_OUTPUT:
            if (idx == 0) snprintf(buf, buflen, "Off");
            else snprintf(buf, buflen, "Out %d", idx); // idx 1..8 -> port idx-1, 1-based display
            return;
        case HDS_FX_DROPDOWN_SIDECHAIN:
            if (idx == 0) snprintf(buf, buflen, "Same as Source");
            else snprintf(buf, buflen, "SC In %d", idx);
            return;
        default: buf[0] = 0; return;
    }
}

// Which option index is this slot's CURRENT value -- drives the highlighted row in the open list.
static int hds_fx_dropdown_current_index(const HdsFxPlugin *p, int kind, int row) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE:
            return hds_fx_selectable_type_index((int)p->fx[row].fx_type);
        case HDS_FX_DROPDOWN_INPUT: {
            int port = (int)p->fx[row].fx_source;
            if (port < 0 || port >= HDS_FX_PORT_COUNT) port = 0;
            if (p->port_input_off[port] != 0.0f) return 0;
            bool stereo = p->port_input_stereo[port] != 0.0f;
            return 1 + (stereo ? HDS_FX_PORT_COUNT + port : port);
        }
        case HDS_FX_DROPDOWN_OUTPUT: {
            int g = (int)p->fx[row].fx_source;
            if (g < 0 || g >= HDS_FX_PORT_COUNT) g = 0;
            if (p->port_output_off[g] != 0.0f) return 0;
            int ch = (int)p->port_output_channel[g];
            if (ch < 0 || ch >= HDS_FX_PORT_COUNT) ch = g;
            return 1 + ch;
        }
        case HDS_FX_DROPDOWN_SIDECHAIN: {
            int sc = (int)p->fx[row].sidechain_source;
            return (sc < 0 || sc >= HDS_FX_PORT_COUNT) ? 0 : sc + 1;
        }
        default: return -1;
    }
}

// Applies option `idx` to `row`. Input/Output's "Off" (idx 0) deliberately leaves fx_source/
// port_output_channel untouched -- Off is "this chain reads/writes silence right now", not a
// different port choice, so switching away from Off later returns to whatever port was last set.
static void hds_fx_dropdown_apply(HdsFxPlugin *p, int kind, int row, int idx) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE:
            p->fx[row].fx_type = (float)HDS_FX_SELECTABLE_TYPES[idx];
            return;
        case HDS_FX_DROPDOWN_INPUT: {
            int port = (int)p->fx[row].fx_source;
            if (port < 0 || port >= HDS_FX_PORT_COUNT) port = 0;
            if (idx == 0) { p->port_input_off[port] = 1.0f; return; }
            int j = idx - 1;
            bool new_stereo = j >= HDS_FX_PORT_COUNT;
            int new_port = new_stereo ? (j - HDS_FX_PORT_COUNT) : j;
            p->fx[row].fx_source = (float)new_port;
            p->port_input_stereo[new_port] = new_stereo ? 1.0f : 0.0f;
            p->port_input_off[new_port] = 0.0f;
            return;
        }
        case HDS_FX_DROPDOWN_OUTPUT: {
            int g = (int)p->fx[row].fx_source;
            if (g < 0 || g >= HDS_FX_PORT_COUNT) g = 0;
            if (idx == 0) { p->port_output_off[g] = 1.0f; return; }
            p->port_output_channel[g] = (float)(idx - 1); // always that port's own real stereo pair
            p->port_output_off[g] = 0.0f;
            return;
        }
        case HDS_FX_DROPDOWN_SIDECHAIN:
            p->fx[row].sidechain_source = (idx == 0) ? -1.0f : (float)(idx - 1);
            return;
        default: return;
    }
}

#define HDS_FX_DROPDOWN_ROW_H 20.0

static double hds_fx_dropdown_width(int kind) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE: return 130.0;
        case HDS_FX_DROPDOWN_SIDECHAIN: return 110.0;
        default: return 90.0; // Input/Output
    }
}

// The open list's own background rect -- flips to open ABOVE the anchor pill instead of below if
// the list would otherwise run off the window's own bottom edge. Same geometry
// hds_fx_dropdown_option_rect() below builds each individual option row from, so draw and
// hit-test can never disagree about where anything is.
static void hds_fx_dropdown_bg_rect(const HdsFxDropdown *dd, int window_h, HdsFxRect *out) {
    int count = hds_fx_dropdown_option_count(dd->kind);
    double w = hds_fx_dropdown_width(dd->kind);
    double list_h = count * HDS_FX_DROPDOWN_ROW_H;
    double below_y = dd->anchor.y + dd->anchor.h + 2.0;
    double y0 = (below_y + list_h > (double)window_h) ? (dd->anchor.y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    *out = (HdsFxRect){dd->anchor.x, y0, w, list_h};
}

static void hds_fx_dropdown_option_rect(const HdsFxDropdown *dd, int window_h, int idx, HdsFxRect *out) {
    HdsFxRect bg;
    hds_fx_dropdown_bg_rect(dd, window_h, &bg);
    *out = (HdsFxRect){bg.x, bg.y + idx * HDS_FX_DROPDOWN_ROW_H, bg.w, HDS_FX_DROPDOWN_ROW_H};
}

static void hds_fx_gui_render_dropdown(cairo_t *cr, HdsFxPlugin *p, const HdsFxGuiViewState *vs) {
    if (vs->dropdown.kind == HDS_FX_DROPDOWN_NONE) return;
    const HdsFxDropdown *dd = &vs->dropdown;
    int count = hds_fx_dropdown_option_count(dd->kind);
    int current = hds_fx_dropdown_current_index(p, dd->kind, dd->row);

    HdsFxRect bg;
    hds_fx_dropdown_bg_rect(dd, vs->window_h, &bg);
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr);
    cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h);
    cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    for (int i = 0; i < count; i++) {
        HdsFxRect r;
        hds_fx_dropdown_option_rect(dd, vs->window_h, i, &r);
        if (i == current) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_fill(cr);
        }
        char label[24];
        hds_fx_dropdown_option_label(dd->kind, i, label, sizeof(label));
        hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
        hds_fx_left_text(cr, r.x + 6, r.y + r.h / 2 + 3.5, label);
    }
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
        // just inherits the running signal from whatever chains into it. "Off" (silence in,
        // instead of any physical port) is its own selectable state, independent of which port
        // number is stored -- see hds_fx_dropdown_apply()'s own comment.
        {
            char inbuf[16];
            bool input_off = p->port_input_off[port] != 0.0f;
            if (input_off) snprintf(inbuf, sizeof(inbuf), "Off");
            else hds_fx_format_input_pill(inbuf, sizeof(inbuf), port, p->port_input_stereo[port] != 0.0f);
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, inbuf, is_head && !input_off, !is_head);
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

        // NAM's own Model/IR path pills -- see HdsFxRowLayout's own comment on nam_model_pill/
        // nam_ir_pill for why these reuse otherwise-blank knob columns. Shows the live edit buffer
        // (with a text cursor) while that specific field is being typed into, otherwise the
        // loaded path's basename or "(none)" -- clicking either pill starts editing (see
        // hds_fx_gui_on_button_press()'s own NAM handling).
        if (hds_fx_type_uses_nam_paths(fx_type)) {
            bool editing_model = (vs->edit_row == s && vs->edit_field == 0);
            bool editing_ir = (vs->edit_row == s && vs->edit_field == 1);
            char trunc[24], model_buf[32], ir_buf[32];
            if (editing_model) {
                hds_fx_truncate_pill_text(vs->edit_buffer, trunc, sizeof(trunc), 15);
                snprintf(model_buf, sizeof(model_buf), "%s_", trunc);
            } else {
                hds_fx_truncate_pill_text(slot->nam_model_path[0] ? hds_fx_basename(slot->nam_model_path) : "(none)", trunc, sizeof(trunc), 15);
                snprintf(model_buf, sizeof(model_buf), "M: %s", trunc);
            }
            if (editing_ir) {
                hds_fx_truncate_pill_text(vs->edit_buffer, trunc, sizeof(trunc), 15);
                snprintf(ir_buf, sizeof(ir_buf), "%s_", trunc);
            } else {
                hds_fx_truncate_pill_text(slot->nam_ir_path[0] ? hds_fx_basename(slot->nam_ir_path) : "(none)", trunc, sizeof(trunc), 15);
                snprintf(ir_buf, sizeof(ir_buf), "IR: %s", trunc);
            }
            hds_fx_draw_pill(cr, L.nam_model_pill.x, L.nam_model_pill.y, L.nam_model_pill.w, L.nam_model_pill.h, model_buf, editing_model || slot->nam_model_path[0], false);
            hds_fx_draw_pill(cr, L.nam_ir_pill.x, L.nam_ir_pill.y, L.nam_ir_pill.w, L.nam_ir_pill.h, ir_buf, editing_ir || slot->nam_ir_path[0], false);
        }

        // Output pill: only live on a chain TAIL -- a slot still chaining forward doesn't decide
        // where anything lands yet, the tail of its own chain does. "Off" (result goes nowhere)
        // is its own selectable state, independent of which port number is stored.
        {
            bool output_off = p->port_output_off[port] != 0.0f;
            char outbuf[16];
            if (output_off) {
                snprintf(outbuf, sizeof(outbuf), "Off");
            } else {
                int out_ch = (int)p->port_output_channel[port];
                if (out_ch < 0 || out_ch >= HDS_FX_PORT_COUNT) out_ch = port;
                snprintf(outbuf, sizeof(outbuf), "Out %d", out_ch + 1); // always that port's own real stereo pair -- no Mono/Stereo choice, see the "Dropdowns" section's own comment
            }
            hds_fx_draw_pill(cr, L.output_pill.x, L.output_pill.y, L.output_pill.w, L.output_pill.h, outbuf, is_tail && !output_off, !is_tail);
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

    hds_fx_gui_render_dropdown(cr, p, vs); // last -- on top of everything else, unclipped by the list's own scroll region
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


// Returns true if the click was consumed. If a dropdown is currently open, this ONLY interacts
// with it (selecting an option if the click landed inside it, closing it either way) -- a click
// can never both close one dropdown and immediately act on whatever's underneath it. Otherwise:
// Type/Input/Sidechain/Output pills OPEN their own dropdown (see the "Dropdowns" section above)
// rather than mutating anything directly; bypass/chain/up/down toggle immediately; a knob starts a
// drag tracked in *vs instead.
// Commits whatever NAM Model/IR field is currently being typed into (if any) -- writes
// edit_buffer into the real FXSlotParams field and closes the editor. Called unconditionally at
// the top of every hds_fx_gui_on_button_press(), same "any click ends the current transient UI
// state" posture the dropdown-close-on-outside-click logic just below already has: clicking
// elsewhere while editing SAVES what was typed (like tabbing out of any normal text field) rather
// than silently discarding it, then the click's own normal hit-test proceeds -- so clicking a
// DIFFERENT NAM pill commits the old one and immediately opens the new one, in one motion.
static void hds_fx_gui_commit_edit(HdsFxPlugin *p, HdsFxGuiViewState *vs) {
    if (vs->edit_row < 0) return;
    char *dst = (vs->edit_field == 0) ? p->fx[vs->edit_row].nam_model_path : p->fx[vs->edit_row].nam_ir_path;
    snprintf(dst, SAMPLE_PATH_LEN, "%s", vs->edit_buffer);
    vs->edit_row = -1;
}

static bool hds_fx_gui_on_button_press(HdsFxPlugin *p, HdsFxGuiViewState *vs, double x, double y, int button) {
    int width = vs->window_w;
    hds_fx_gui_commit_edit(p, vs);

    if (vs->dropdown.kind != HDS_FX_DROPDOWN_NONE) {
        int count = hds_fx_dropdown_option_count(vs->dropdown.kind);
        for (int i = 0; i < count; i++) {
            HdsFxRect r;
            hds_fx_dropdown_option_rect(&vs->dropdown, vs->window_h, i, &r);
            if (hds_fx_rect_contains(&r, x, y)) {
                if (button == 1) hds_fx_dropdown_apply(p, vs->dropdown.kind, vs->dropdown.row, i);
                vs->dropdown.kind = HDS_FX_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown.kind = HDS_FX_DROPDOWN_NONE; // clicked outside the open list -- just close it
        return true;
    }

    if (y < HDS_FX_GUI_HEADER_H) return false;

    int row = (int)((y - HDS_FX_GUI_HEADER_H + vs->scroll_offset_px) / HDS_FX_GUI_ROW_H);
    if (row < 0 || row >= FX_SLOT_COUNT) return false;
    double row_y = HDS_FX_GUI_HEADER_H + row * HDS_FX_GUI_ROW_H - vs->scroll_offset_px;
    HdsFxRowLayout L;
    hds_fx_gui_compute_row_layout(width, row_y, &L);

    if (hds_fx_rect_contains(&L.type_pill, x, y)) {
        vs->dropdown.kind = HDS_FX_DROPDOWN_TYPE; vs->dropdown.row = row; vs->dropdown.anchor = L.type_pill;
        return true;
    }
    if (hds_fx_slot_is_chain_head(p, row) && hds_fx_rect_contains(&L.input_pill, x, y)) {
        vs->dropdown.kind = HDS_FX_DROPDOWN_INPUT; vs->dropdown.row = row; vs->dropdown.anchor = L.input_pill;
        return true;
    }
    if (hds_fx_type_uses_sidechain((int)p->fx[row].fx_type) && hds_fx_rect_contains(&L.sidechain_pill, x, y)) {
        vs->dropdown.kind = HDS_FX_DROPDOWN_SIDECHAIN; vs->dropdown.row = row; vs->dropdown.anchor = L.sidechain_pill;
        return true;
    }
    if (hds_fx_type_uses_nam_paths((int)p->fx[row].fx_type)) {
        if (hds_fx_rect_contains(&L.nam_model_pill, x, y)) {
            vs->edit_row = row; vs->edit_field = 0;
            snprintf(vs->edit_buffer, sizeof(vs->edit_buffer), "%s", p->fx[row].nam_model_path);
            return true;
        }
        if (hds_fx_rect_contains(&L.nam_ir_pill, x, y)) {
            vs->edit_row = row; vs->edit_field = 1;
            snprintf(vs->edit_buffer, sizeof(vs->edit_buffer), "%s", p->fx[row].nam_ir_path);
            return true;
        }
    }
    if (hds_fx_slot_is_chain_tail(p, row) && hds_fx_rect_contains(&L.output_pill, x, y)) {
        vs->dropdown.kind = HDS_FX_DROPDOWN_OUTPUT; vs->dropdown.row = row; vs->dropdown.anchor = L.output_pill;
        return true;
    }
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

// Named key actions the platform shim (hds_fx_gui_x11.c) translates a real X11 KeyPress into --
// keeps this file free of any X11 type, the same "zero windowing references" discipline this
// file's own header comment already holds every other piece of drawing/hit-test logic to. A no-op
// whenever nothing is being edited (vs->edit_row < 0) -- this GUI has exactly one thing keyboard
// input can ever reach, FX_TYPE_NAM's own Model/IR path fields (see hds_fx_gui_on_button_press()'s
// own NAM handling for how editing starts), so there's no focus model to manage beyond that.
#define HDS_FX_KEY_CHAR 0      // a printable character in `ch`
#define HDS_FX_KEY_BACKSPACE 1
#define HDS_FX_KEY_ENTER 2     // commits edit_buffer into the real field, same as hds_fx_gui_commit_edit()
#define HDS_FX_KEY_ESCAPE 3    // discards edit_buffer, leaving the field's real value untouched

static void hds_fx_gui_on_key(HdsFxPlugin *p, HdsFxGuiViewState *vs, int action, char ch) {
    if (vs->edit_row < 0) return;
    size_t len = strlen(vs->edit_buffer);
    switch (action) {
        case HDS_FX_KEY_CHAR:
            // Printable ASCII only (0x20..0x7e) -- a file path never needs anything outside that
            // range, and this sidesteps ever having to reason about multi-byte UTF-8 XLookupString
            // can hand back for a real IME/dead-key sequence.
            if (ch >= 0x20 && ch < 0x7f && len + 1 < sizeof(vs->edit_buffer)) {
                vs->edit_buffer[len] = ch;
                vs->edit_buffer[len + 1] = '\0';
            }
            return;
        case HDS_FX_KEY_BACKSPACE:
            if (len > 0) vs->edit_buffer[len - 1] = '\0';
            return;
        case HDS_FX_KEY_ENTER:
            hds_fx_gui_commit_edit(p, vs);
            return;
        case HDS_FX_KEY_ESCAPE:
            vs->edit_row = -1; // discard -- do NOT call hds_fx_gui_commit_edit()
            return;
        default:
            return;
    }
}

// delta: +1 (wheel up / X11 Button4) or -1 (wheel down / X11 Button5).
static void hds_fx_gui_on_scroll(HdsFxGuiViewState *vs, int delta) {
    vs->scroll_offset_px -= delta * (HDS_FX_GUI_ROW_H * 3.0);
    double max_scroll = hds_fx_gui_max_scroll(vs);
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
    // A dropdown's own anchor rect is captured in absolute window coordinates at open time --
    // scrolling the list would leave it floating, visually detached from the row it belongs to.
    vs->dropdown.kind = HDS_FX_DROPDOWN_NONE;
}

static void hds_fx_gui_on_resize(HdsFxGuiViewState *vs, int w, int h) {
    if (w < HDS_FX_GUI_MIN_W) w = HDS_FX_GUI_MIN_W;
    if (h < HDS_FX_GUI_MIN_H) h = HDS_FX_GUI_MIN_H;
    vs->window_w = w; vs->window_h = h;
    double max_scroll = hds_fx_gui_max_scroll(vs);
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
    vs->dropdown.kind = HDS_FX_DROPDOWN_NONE; // same reasoning as hds_fx_gui_on_scroll's own comment
}
