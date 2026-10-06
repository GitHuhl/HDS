// hds_fx_gui_draw.c: the ENTIRE visual/interaction model for HDS FX's GUI, deliberately platform-
// agnostic -- every function here takes a plain `cairo_t*` and touches no windowing API of any kind
// (no Xlib, no Win32, nothing). This is the file a future Windows or macOS port reuses unchanged:
// only the SURFACE that `cairo_t*` draws into differs per platform (Xlib here, GDI on Windows via
// cairo_win32_surface_create(), Quartz on macOS), and Cairo's own drawing API is identical across
// all three -- see hds_fx_gui_x11.c's own header comment for the platform shim that owns the
// window/event pump and calls into this file every frame.
//
// #include'd by any host with an FX_SLOT_COUNT pool AFTER fx_types.h/fx_chain.c (needs
// FXSlotParams/FX_TYPE_*) and hds_fx_gui_host.h (needs HdsFxGuiHost -- see that header's own
// comment). Every function here reads/writes a HdsFxGuiHost directly, no separate "view model"
// copy -- but HdsFxGuiHost is itself just a handful of pointers INTO whatever real host struct
// (HdsFxPlugin for hds_fx.clap, or hds.clap's own plugin struct) actually owns the arrays, so this
// same engine serves any such host unchanged.
//
// Layout model: ONE flat,
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
// Font selection -- a drop-in stand-in for cairo_select_font_face(), needed because Android has
// no Fontconfig, and Cairo's own "toy font" backend (the thing cairo_select_font_face/
// cairo_show_text use to turn a family NAME like "sans-serif" into real glyphs) is wired to a
// null function pointer entirely when Cairo is built without Fontconfig -- confirmed directly in
// Cairo's own source (src/cairo-ft-font.c's _cairo_ft_font_face_backend.create_for_toy), not
// assumed. This is the ONE thing every draw file in this whole native GUI (hds_clap_gui_draw.c
// and every hds_clap_*_draw.c it composes, all #include'd after this file) needed to change for
// Android -- and thanks to this #define, none of them actually did: every existing
// `cairo_select_font_face(cr, "sans-serif", slant, weight)` call site, in every one of those
// files, is rewritten to call hds_font_select_face() instead, automatically, by the preprocessor,
// with the exact same arguments and zero other changes anywhere.
//
// hds_font_select_face() must be defined (and must call the REAL cairo_select_font_face, by its
// real name) BEFORE the #define below takes effect -- that's why the definition and the #define
// are in this exact order, in this exact file: every other draw file in this codebase is
// #include'd strictly after this one (see this file's own header comment + every host's own
// #include order), so this is the one place in the whole codebase where "cairo_select_font_face"
// still means the real library function.
static void hds_font_select_face(cairo_t *cr, const char *family, cairo_font_slant_t slant, cairo_font_weight_t weight) {
#if defined(HDS_ANDROID_GUI_ENABLED)
    // family is always "sans-serif" at every real call site (checked) -- nothing to switch on.
    // slant is dropped (Android ships two bundled font FACES -- regular and bold, see
    // hds_android_gui.c's own comment on hds_android_font_regular/bold -- not four, so the one
    // real italic use in this codebase, src/hds_clap_devices_draw.c's "Ultra/Extra Snappy" latency
    // warning label, renders upright on Android instead of slanted; a real, accepted, minimal
    // visual deviation, not an oversight).
    (void)family; (void)slant;
    extern cairo_font_face_t *hds_android_font_regular, *hds_android_font_bold;
    cairo_set_font_face(cr, weight == CAIRO_FONT_WEIGHT_BOLD ? hds_android_font_bold : hds_android_font_regular);
#else
    cairo_select_font_face(cr, family, slant, weight);
#endif
}
#define cairo_select_font_face hds_font_select_face

// ---------------------------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------------------------
#define HDS_FX_GUI_DEFAULT_W 1150
#define HDS_FX_GUI_DEFAULT_H 760
#define HDS_FX_GUI_MIN_W 740
#define HDS_FX_GUI_MIN_H 380
// Tall enough to fit, all strictly inside this one row's own bounds: a short label naming what the
// knob actually does, the knob itself, and its current value -- see hds_fx_draw_knob's own comment
// for why a bare unlabeled pot was wrong twice over (no name AND no visible value).
#define HDS_FX_GUI_ROW_H 62.0
// Title row + a column-header strip naming every column (see hds_fx_gui_render's own header-row
// drawing) -- a flat list with no per-group-box titles like the Standalone's own QGroupBoxes needs
// this or a column is just an unlabeled row of controls.
#define HDS_FX_GUI_HEADER_H 62.0
#define HDS_FX_GUI_KNOB_R 10.0
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
    FX_TYPE_HARDTUNE, FX_TYPE_FDNVERB, FX_TYPE_SSL4K, FX_TYPE_DBX160, FX_TYPE_BROADCAST_DELAY,
    FX_TYPE_DBX120X,
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
        case FX_TYPE_SSL4K: return "SSL4K";
        case FX_TYPE_DBX160: return "DBX160";
        case FX_TYPE_BROADCAST_DELAY: return "BcastDly";
        case FX_TYPE_DBX120X: return "DBX120X";
        default: return "?";
    }
}

// Real per-type param counts (see each FX_TYPE_*'s own doc comment in fx_types.h for exactly what
// each param does). Indexed directly by fx_type.
static int hds_fx_type_param_count(int fx_type) {
    static const int counts[] = {
        /* OFF */ 0, /* ENSEMBLE */ 2, /* CREAMCOAT */ 5, /* DENSITY3 */ 3, /* STEREOCHORUS */ 2,
        /* STEREODOUBLER */ 2, /* TAPEDELAY2 */ 7, /* POINTYGUITAR */ 10, /* GUITARCONDITIONER */ 0,
        /* CABS2 */ 2, /* NAM */ 3, /* GATE */ 6, /* EQ3 */ 7, /* LIMITER */ 3, /* FETCOMP */ 8,
        /* FAIRKID67 */ 8, /* PEAKMAX */ 7, /* MULTIFILTER */ 4, /* OPTOCOMP */ 5, /* DEESSER */ 6,
        /* HARDTUNE */ 10, /* FDNVERB */ 10, /* SSL4K */ 11, /* DBX160 */ 5, /* BROADCAST_DELAY */ 7,
        /* DBX120X */ 6,
    };
    if (fx_type < 0 || fx_type >= (int)(sizeof(counts) / sizeof(counts[0]))) return 0;
    return counts[fx_type];
}

// Real, short per-param labels, ported directly from each FX_TYPE_*'s own doc comment in
// fx_types.h (the authoritative description of what param[idx] actually does for that effect) --
// abbreviated to fit this GUI's own compact knob column, never invented. Every knob is drawn with
// its own label directly above it -- see hds_fx_draw_knob's own comment -- so nothing is ever a
// bare numbered pot.
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
    static const char *const gate[] = {"Thresh", "Attack", "Releas", "Range", "Hyst", "Hold"};
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
    static const char *const ssl4k[] = {"HPFrq", "LFFrq", "LFGan", "LMFrq", "LMGan", "LMQ", "HMFrq", "HMGan", "HFFrq", "HFGan", "E/G"};
    static const char *const dbx160[] = {"Thresh", "Ratio", "Makeup", "Sat", "SC-HP"};
    static const char *const bcastdly[] = {"Delay", "Dump7s", "Dmp15s", "Dmp30s", "Rstock", "OnAir", "Vol"};
    static const char *const dbx120x[] = {"TrkFrq", "Thresh", "Releas", "SynLvl", "Octave", "Glide"};

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
        case FX_TYPE_GATE: table = gate; count = 6; break;
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
        case FX_TYPE_SSL4K: table = ssl4k; count = 11; break;
        case FX_TYPE_DBX160: table = dbx160; count = 5; break;
        case FX_TYPE_BROADCAST_DELAY: table = bcastdly; count = 7; break;
        case FX_TYPE_DBX120X: table = dbx120x; count = 6; break;
        default: return "";
    }
    if (idx < 0 || idx >= count) return "";
    return table[idx];
}

// Real per-param VALUE ranges/units, extracted directly from each run_fx_*()'s own formula in
// fx_chain.c (not guessed) -- a knob's raw `slot->param[i]` was blanket-displayed as "NN%" and
// blanket-dragged as a plain 0.0-1.0 fraction regardless of what the DSP actually did with it,
// which was wrong two different ways at once, confirmed from a live DAW test:
//   1. Cosmetic: half these params are really dB/ms/Hz/a named discrete choice (e.g. FetComp's
//      Ratio is a genuine 5-way switch -- 4:1/8:1/12:1/20:1/All -- ratio_idx = (int)(p*4+0.5f),
//      never anything in between), not a continuous percentage -- see hds_fx_type_param_value_text()
//      just below for the full, per-effect breakdown.
//   2. Functional, and worse: FX_TYPE_MULTIFILTER's own Reso param (idx 2) is read by the DSP as
//      `clamp(param[2], 0.0f, 100.0f)` -- ALREADY a 0-100 value, the one param in this entire file
//      that isn't a plain 0.0-1.0 fraction -- while this GUI's drag code (hds_fx_gui_on_motion())
//      clamped every param uniformly to [0,1]. That capped the Reso knob at 1% of its own real
//      range, functionally inert (the DSP's own resonance formula, `2*(1 - reso/100)`, barely moves
//      off its minimum for any value this GUI could ever have produced). This function is the one
//      source of truth for a param's real STORAGE range now -- hds_fx_gui_on_motion()'s own drag
//      clamp and hds_fx_gui_render()'s own knob-arc normalization both call it, so the two can never
//      disagree about what "full turn" means for this one param again.
static void hds_fx_type_param_range(int fx_type, int idx, float *lo, float *hi) {
    *lo = 0.0f; *hi = 1.0f;
    if (fx_type == FX_TYPE_MULTIFILTER && idx == 2) { *lo = 0.0f; *hi = 100.0f; }
}

// ---------------------------------------------------------------------------------------------
// Real per-param VALUE TEXT -- replaces the old blanket "NN%" every knob used to show regardless
// of what it actually controlled. `frac` is always a plain 0.0-1.0 fraction of the param's own
// full travel (already normalized against hds_fx_type_param_range() by the caller -- see
// hds_fx_gui_render()'s own knob-drawing loop), matching exactly what each run_fx_*() function's
// own formula computes from its raw param[idx] (or, for MULTIFILTER's Reso, from param[idx]/100 --
// same fraction-of-travel either way). Every formula below is copied verbatim from its own
// run_fx_*() -- see fx_chain.c for the authoritative source; anything without a clean physical unit
// in the DSP itself (a proprietary internal coefficient/curve with no dB/ms/Hz conversion anywhere
// in the code -- Ensemble, CreamCoat's Regen/DeRez/PreDly, StereoChorus, StereoDoubler's Detune,
// TapeDelay2's Time/Freq/Reso/Flutter, PointyGuitar's Drive/tone knobs/HSpkr/LSpkr/Gate/Outpt,
// Cabs2, Density3's Densty/HiPass) is left as a plain percent -- inventing a unit conversion the
// DSP itself never computes would show a confident-looking but FALSE number, worse than "NN%".
// ---------------------------------------------------------------------------------------------

static void hds_fx_fmt_percent(float frac, char *buf, size_t n) {
    snprintf(buf, n, "%d%%", (int)(frac * 100.0f + 0.5f));
}
static void hds_fx_fmt_db(float frac, float lo, float hi, char *buf, size_t n) {
    snprintf(buf, n, "%+.1fdB", lo + frac * (hi - lo));
}
static void hds_fx_fmt_ms(float frac, float lo, float hi, char *buf, size_t n) {
    float ms = lo + frac * (hi - lo);
    if (ms < 10.0f) snprintf(buf, n, "%.2fms", ms); else snprintf(buf, n, "%.0fms", ms);
}
static void hds_fx_fmt_us(float frac, float lo, float hi, char *buf, size_t n) {
    snprintf(buf, n, "%.0f\xc2\xb5s", lo + frac * (hi - lo)); // "us" -- UTF-8 micro sign, µs
}
// Log-taper Hz (matches fx_eq3_fraction_to_hz()/DeEsser's own `lo * powf(hi/lo, frac)` shape
// exactly -- both EQ3/MultiFilter's 20Hz-20kHz mapping and DeEsser's 1kHz-16kHz one use this same
// formula, just different lo/hi).
static void hds_fx_fmt_hz_log(float frac, float lo_hz, float hi_hz, char *buf, size_t n) {
    float hz = lo_hz * powf(hi_hz / lo_hz, frac);
    if (hz >= 1000.0f) snprintf(buf, n, "%.2fkHz", hz / 1000.0f); else snprintf(buf, n, "%.0fHz", hz);
}
static void hds_fx_fmt_hz_linear(float frac, float lo_hz, float hi_hz, char *buf, size_t n) {
    float hz = lo_hz + frac * (hi_hz - lo_hz);
    snprintf(buf, n, "%.1fHz", hz);
}
static void hds_fx_fmt_sec_log(float frac, float lo_s, float hi_s, char *buf, size_t n) {
    snprintf(buf, n, "%.2fs", lo_s * powf(hi_s / lo_s, frac));
}
static void hds_fx_fmt_sec_linear(float frac, float lo_s, float hi_s, char *buf, size_t n) {
    snprintf(buf, n, "%.1fs", lo_s + frac * (hi_s - lo_s));
}
static void hds_fx_fmt_semitones(float frac, float lo, float hi, char *buf, size_t n) {
    snprintf(buf, n, "%.2fst", lo + frac * (hi - lo));
}
static void hds_fx_fmt_ratio(float frac, float lo, float hi, char *buf, size_t n) {
    snprintf(buf, n, "%.1f:1", lo + frac * (hi - lo));
}
static void hds_fx_fmt_plain(float frac, float lo, float hi, const char *suffix, char *buf, size_t n) {
    snprintf(buf, n, "%.2f%s", lo + frac * (hi - lo), suffix);
}
// Reproduces `(int)(frac * (count-1) + 0.5f)` clamped 0..count-1 -- the EXACT stepped-index
// formula every ratio-stepped param in fx_chain.c uses (FetComp's Ratio, PeakMax's Release/
// Quantize/Dither/Shaping, HardTune's Key/Scale/Voice/VibSrc, MultiFilter's Type/Slope, FairKid67's
// TimeCn/Link) -- so the label shown here is always the exact option that slot's own DSP is
// currently running, never a different one a slightly different rounding rule might imply.
static void hds_fx_fmt_stepped(float frac, const char *const *labels, int count, char *buf, size_t n) {
    int idx = (int)(frac * (float)(count - 1) + 0.5f);
    if (idx < 0) idx = 0; else if (idx >= count) idx = count - 1;
    snprintf(buf, n, "%s", labels[idx]);
}
static void hds_fx_fmt_onoff(float frac, const char *off_label, const char *on_label, bool on_when_low, char *buf, size_t n) {
    bool is_on = on_when_low ? (frac < 0.5f) : (frac >= 0.5f);
    snprintf(buf, n, "%s", is_on ? on_label : off_label);
}

static void hds_fx_type_param_value_text(int fx_type, int idx, float frac, char *buf, size_t buflen) {
    static const char *const creamcoat_select[] = {
        "P1","P2","P3","P4","P5","P6","P7","P8","P9","P10","P11","P12","P13","P14","P15","P16","P17"
    };
    static const char *const pingpong[] = {"Off", "Ping-Pong"};
    static const char *const fetcomp_ratio[] = {"4:1", "8:1", "12:1", "20:1", "All"};
    static const char *const fetcomp_limit[] = {"ChrOnly", "Limit"}; // on_when_low -- see call site
    static const char *const fk67_timing[] = {"P1", "P2", "P3", "P4", "P5", "P6"};
    static const char *const fk67_link[] = {"Indep", "LnkMax", "LnkAvg", "Mid-Sd"};
    static const char *const peakmax_release[] = {"0.01ms", "0.1ms", "1ms", "10ms", "100ms", "1000ms"};
    static const char *const peakmax_quant[] = {"16-bit", "18-bit", "20-bit", "22-bit", "24-bit"};
    static const char *const peakmax_dither[] = {"None", "Type 1", "Type 2"};
    static const char *const peakmax_shape[] = {"None", "Modrt", "Normal", "Ultra"};
    static const char *const multifilter_type[] = {"LoPass", "HiPass", "BndPas", "Notch"};
    static const char *const multifilter_slope[] = {"6", "12", "18", "24", "30", "36", "42", "48", "60", "72", "84", "96"};
    static const char *const hardtune_key[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char *const hardtune_scale[] = {"Chroma", "Major", "Minor", "MajPnt", "MinPnt"};
    static const char *const hardtune_target[] = {"SclKey", "MIDI"};
    static const char *const hardtune_voice[] = {"Bass", "Tenor", "Alto", "Soprano", "Wide"};
    static const char *const hardtune_vibsrc[] = {"None", "ModWhl", "AfterT"};
    static const char *const dbx120x_octave[] = {"-1 Oct", "-2 Oct"};

    switch (fx_type) {
        case FX_TYPE_NAM:
            if (idx == 0) { hds_fx_fmt_db(frac, -20.0f, 20.0f, buf, buflen); return; } // Input Gain
            if (idx == 1) { hds_fx_fmt_db(frac, -20.0f, 20.0f, buf, buflen); return; } // Output Gain
            if (idx == 2) { hds_fx_fmt_onoff(frac, "Off", "On", false, buf, buflen); return; } // Cab Enable
            break;
        case FX_TYPE_GATE:
            if (idx == 0) { hds_fx_fmt_db(frac, -60.0f, 0.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_ms(frac, 0.1f, 20.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ms(frac, 5.0f, 500.0f, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_db(frac, -80.0f, 0.0f, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_db(frac, 0.0f, 12.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_ms(frac, 0.0f, 500.0f, buf, buflen); return; }
            break;
        case FX_TYPE_EQ3:
            if (idx == 0 || idx == 2 || idx == 5) { hds_fx_fmt_hz_log(frac, 20.0f, 20000.0f, buf, buflen); return; }
            if (idx == 1 || idx == 3 || idx == 6) { hds_fx_fmt_db(frac, -18.0f, 18.0f, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_plain(frac, 0.1f, 10.0f, " Q", buf, buflen); return; }
            break;
        case FX_TYPE_LIMITER:
            if (idx == 0) { hds_fx_fmt_db(frac, -20.0f, 0.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, 0.0f, 24.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ms(frac, 10.0f, 500.0f, buf, buflen); return; }
            break;
        case FX_TYPE_FETCOMP:
            if (idx == 0) { hds_fx_fmt_db(frac, -20.0f, 40.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, -20.0f, 20.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_stepped(frac, fetcomp_ratio, 5, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_us(frac, 20.0f, 800.0f, buf, buflen); return; } // microseconds, NOT ms -- see this section's own header comment
            if (idx == 4) { hds_fx_fmt_ms(frac, 50.0f, 1100.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_onoff(frac, fetcomp_limit[0], fetcomp_limit[1], true, buf, buflen); return; } // on("Limit") when frac<0.5
            if (idx == 6) { hds_fx_fmt_plain(frac, 0.0f, 0.5f, "", buf, buflen); return; }
            if (idx == 7) { hds_fx_fmt_percent(frac, buf, buflen); return; }
            break;
        case FX_TYPE_FAIRKID67:
            if (idx == 0) { hds_fx_fmt_db(frac, -20.0f, 40.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, -20.0f, 20.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_plain(frac, 0.0f, 5.0f, "V", buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_stepped(frac, fk67_timing, 6, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_stepped(frac, fk67_link, 4, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_plain(frac, 0.0f, 2.0f, "V", buf, buflen); return; }
            if (idx == 6) { hds_fx_fmt_plain(frac, 0.5f, 3.0f, "x", buf, buflen); return; }
            if (idx == 7) { hds_fx_fmt_plain(frac, 0.0f, 2.0f, "x", buf, buflen); return; }
            break;
        case FX_TYPE_PEAKMAX:
            if (idx == 0) { hds_fx_fmt_db(frac, -30.0f, 0.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, -30.0f, 0.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_onoff(frac, "Off", "On", false, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_stepped(frac, peakmax_release, 6, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_stepped(frac, peakmax_quant, 5, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_stepped(frac, peakmax_dither, 3, buf, buflen); return; }
            if (idx == 6) { hds_fx_fmt_stepped(frac, peakmax_shape, 4, buf, buflen); return; }
            break;
        case FX_TYPE_MULTIFILTER:
            if (idx == 0) { hds_fx_fmt_stepped(frac, multifilter_type, 4, buf, buflen); return; }
            if (idx == 1) {
                float hz = 20.0f * powf(1000.0f, frac);
                if (hz < 40.0f) hz = 40.0f; else if (hz > 18000.0f) hz = 18000.0f;
                if (hz >= 1000.0f) snprintf(buf, buflen, "%.2fkHz", hz / 1000.0f); else snprintf(buf, buflen, "%.0fHz", hz);
                return;
            }
            if (idx == 2) { hds_fx_fmt_percent(frac, buf, buflen); return; } // already normalized against hds_fx_type_param_range()'s own 0-100 range by the caller
            if (idx == 3) { char lbl[8]; hds_fx_fmt_stepped(frac, multifilter_slope, 12, lbl, sizeof(lbl)); snprintf(buf, buflen, "%sdB/o", lbl); return; }
            break;
        case FX_TYPE_OPTOCOMP:
            if (idx == 0) { hds_fx_fmt_db(frac, -60.0f, 0.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_ratio(frac, 1.0f, 20.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ms(frac, 0.1f, 100.0f, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_ms(frac, 100.0f, 1000.0f, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_db(frac, -12.0f, 12.0f, buf, buflen); return; }
            break;
        case FX_TYPE_DEESSER:
            if (idx == 0) { hds_fx_fmt_hz_log(frac, 1000.0f, 16000.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, -40.0f, 0.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ratio(frac, 1.0f, 20.0f, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_ms(frac, 0.1f, 20.0f, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_ms(frac, 5.0f, 300.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_onoff(frac, "Off", "On", false, buf, buflen); return; }
            break;
        case FX_TYPE_HARDTUNE:
            if (idx == 0) { hds_fx_fmt_stepped(frac, hardtune_key, 12, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_stepped(frac, hardtune_scale, 5, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_stepped(frac, hardtune_target, 2, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_percent(frac, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_ms(frac, 0.1f, 200.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_ms(frac, 0.1f, 500.0f, buf, buflen); return; }
            if (idx == 6) { hds_fx_fmt_stepped(frac, hardtune_voice, 5, buf, buflen); return; }
            if (idx == 7) { hds_fx_fmt_hz_linear(frac, 0.1f, 10.0f, buf, buflen); return; }
            if (idx == 8) { hds_fx_fmt_semitones(frac, 0.0f, 1.5f, buf, buflen); return; }
            if (idx == 9) { hds_fx_fmt_stepped(frac, hardtune_vibsrc, 3, buf, buflen); return; }
            break;
        case FX_TYPE_FDNVERB:
            if (idx == 0) { hds_fx_fmt_sec_log(frac, 0.3f, 12.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ms(frac, 0.0f, 250.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_hz_linear(frac, 0.1f, 10.0f, buf, buflen); return; }
            if (idx == 9) { hds_fx_fmt_onoff(frac, "Off", "On", false, buf, buflen); return; }
            // idx 1,3,4,6,7,8 (Size/Damp/Diffus/ModDp/ERLvl/ERDcy) are all genuine 0-100% fractions
            // with no further unit conversion in run_fx_fdnverb() -- fall through to plain percent.
            break;
        case FX_TYPE_TAPEDELAY2:
            if (idx == 6) { hds_fx_fmt_stepped(frac, pingpong, 2, buf, buflen); return; }
            break; // idx 0-5 are all proprietary internal coefficients -- see this section's own header comment
        case FX_TYPE_CREAMCOAT:
            if (idx == 0) { hds_fx_fmt_stepped(frac, creamcoat_select, 17, buf, buflen); return; }
            break; // idx 1-4 are all proprietary internal coefficients
        case FX_TYPE_SSL4K:
            if (idx == 0) { hds_fx_fmt_hz_log(frac, 20.0f, 500.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_hz_log(frac, 30.0f, 450.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_db(frac, -16.0f, 16.0f, buf, buflen); return; }
            // idx 3 (LMF Freq) falls through to plain percent -- its real Hz depends on the Console
            // switch (param[10]), which this function has no access to (only its own `frac`); a
            // guessed-console Hz reading would be confidently wrong half the time, worse than "NN%"
            // (see this section's own header comment on why a false unit beats no unit).
            if (idx == 4) { hds_fx_fmt_db(frac, -16.0f, 16.0f, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_plain(frac, 0.3f, 3.0f, " Q", buf, buflen); return; }
            // idx 6 (HMF Freq): same Console-dependent caveat as idx 3.
            if (idx == 7) { hds_fx_fmt_db(frac, -16.0f, 16.0f, buf, buflen); return; }
            if (idx == 8) { hds_fx_fmt_hz_log(frac, 1500.0f, 16000.0f, buf, buflen); return; }
            if (idx == 9) { hds_fx_fmt_db(frac, -16.0f, 16.0f, buf, buflen); return; }
            if (idx == 10) { hds_fx_fmt_onoff(frac, "E", "G", false, buf, buflen); return; }
            break;
        case FX_TYPE_DBX160:
            if (idx == 0) { hds_fx_fmt_db(frac, -60.0f, 0.0f, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_ratio(frac, 1.0f, 20.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_db(frac, -12.0f, 12.0f, buf, buflen); return; }
            if (idx == 3) { hds_fx_fmt_percent(frac, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_hz_linear(frac, 20.0f, 300.0f, buf, buflen); return; }
            break;
        case FX_TYPE_BROADCAST_DELAY:
            if (idx == 0) { hds_fx_fmt_sec_linear(frac, 0.0f, (float)HDS_BCASTDLY_MAX_SEC, buf, buflen); return; } // Delay
            if (idx == 4) { hds_fx_fmt_plain(frac, 1.0f, 25.0f, "%", buf, buflen); return; } // Restock Speed
            break; // idx 1-3 (Dump) and idx 5 (On-Air Override) are custom-drawn buttons, never reach this function; idx 6 (Volume) falls through to the plain percent default below
        case FX_TYPE_DBX120X:
            if (idx == 0) { hds_fx_fmt_hz_log(frac, FX_DBX120X_TRACK_MIN_HZ, FX_DBX120X_TRACK_MAX_HZ, buf, buflen); return; }
            if (idx == 1) { hds_fx_fmt_db(frac, -60.0f, 0.0f, buf, buflen); return; }
            if (idx == 2) { hds_fx_fmt_ms(frac, 10.0f, 1000.0f, buf, buflen); return; }
            if (idx == 4) { hds_fx_fmt_stepped(frac, dbx120x_octave, 2, buf, buflen); return; }
            if (idx == 5) { hds_fx_fmt_ms(frac, 10.0f, 500.0f, buf, buflen); return; }
            break; // idx 3 (Synthesis Level) is a genuine 0-100% fraction -- falls through to plain percent
        default: break;
    }
    hds_fx_fmt_percent(frac, buf, buflen);
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

// The 8 FX types that report a live gain-reduction reading (see process_fx_chain()'s own switch
// in fx_chain.c, the only place that fills gr_db_out[s] with anything other than the 0.0f
// default). Gates whether the GR
// dB readout (see HdsFxRowLayout's own gr_label comment) is drawn on a given row at all: on every
// other type host->fx_gr_db[s] is simply always 0.0f (process_fx_chain() zeroes it for them), so
// this isn't strictly needed for correctness, but showing a permanent "0.0 dB" next to, say, a
// reverb would misleadingly imply it's a dynamics processor.
static bool hds_fx_type_is_gr_capable(int fx_type) {
    switch (fx_type) {
        case FX_TYPE_GATE: case FX_TYPE_LIMITER: case FX_TYPE_FETCOMP: case FX_TYPE_FAIRKID67:
        case FX_TYPE_PEAKMAX: case FX_TYPE_OPTOCOMP: case FX_TYPE_DEESSER: case FX_TYPE_DBX160:
            return true;
        default:
            return false;
    }
}

// Gates FX_TYPE_BROADCAST_DELAY's own special-cased row furniture: param columns 1-3 (the Dump
// buttons) render as big red momentary buttons instead of knobs, and the GR-label column (unused
// by this non-GR-capable type, so free real estate -- see HdsFxRowLayout's own gr_label comment)
// is repurposed as a color-coded "available dump time" readout. Kept as its own named predicate,
// same convention as hds_fx_type_is_gr_capable/hds_fx_type_uses_sidechain above, rather than
// inlining the fx_type comparison at each of its several call sites.
static bool hds_fx_type_is_broadcast_delay(int fx_type) { return fx_type == FX_TYPE_BROADCAST_DELAY; }

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
// Builds "<prefix><value>" (e.g. "M: some_amp.nam"), truncating VALUE from the front (keeping the
// tail, prefixed with a leading ellipsis once truncated) until the
// REAL, MEASURED width of the whole label actually fits max_width_px at hds_fx_draw_pill()'s own
// font (9.5pt sans-serif) -- a real, reported bug: a fixed character-count guess (this function's
// own earlier revision) was nowhere near tight enough for a real, long, descriptive .nam filename,
// which rendered wider than the pill and (since hds_fx_draw_pill() didn't clip either, now fixed
// separately) visibly bled into neighboring columns. Measuring the ACTUAL string cairo will draw,
// including the prefix, is the only way to guarantee this fits regardless of filename or font
// metrics -- called once per pill per frame, cheap enough at this GUI's own row counts.
static void hds_fx_pill_label_fit(cairo_t *cr, const char *prefix, const char *value, char *buf, size_t buflen, double max_width_px) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    size_t len = strlen(value);
    snprintf(buf, buflen, "%s%s", prefix, value);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    if (ext.width <= max_width_px || len == 0) return;
    for (size_t keep = len - 1; keep > 0; keep--) {
        snprintf(buf, buflen, "%s\xe2\x80\xa6%s", prefix, value + (len - keep));
        cairo_text_extents(cr, buf, &ext);
        if (ext.width <= max_width_px) return;
    }
    snprintf(buf, buflen, "%s\xe2\x80\xa6", prefix); // not even one tail character fits -- prefix + ellipsis only
}

// The directory part of a path (everything before the last '/', not including it), or "" if there
// is no '/' at all -- used to seed the file browser's own starting directory from whatever's
// already stored in nam_model_path/nam_ir_path (see hds_fx_gui_browser_open()). Pure string math,
// no filesystem access -- this file stays free of any OS/filesystem call (see this file's own
// header comment on staying portable to a future Windows GUI); the platform shim
// (hds_fx_gui_x11.c) is what actually opendir()s whatever this produces, and falls back to a
// sane default (the user's HOME, or "/") if it doesn't exist or this returns "".
// A plain bounded strcpy -- used in place of snprintf(dst, n, "%s", src) wherever src's own
// declared buffer is bigger than dst's (e.g. a joined "dir/name" that could in principle exceed
// SAMPLE_PATH_LEN for a sufficiently deep/long real path): correctly truncating and NUL-terminating
// either way, just without tripping GCC's -Wformat-truncation, which can't see that this is an
// intentional, already-safe cap rather than an overlooked bug.
static void hds_fx_safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void hds_fx_dirname(const char *path, char *out, size_t out_len) {
    out[0] = '\0';
    if (!path || !path[0]) return;
    const char *slash = strrchr(path, '/');
    if (!slash) return; // no '/' at all -- leave empty, the platform shim's own fallback takes over
    size_t len = (size_t)(slash - path);
    if (len == 0) len = 1; // "/foo.nam" -> dirname "/", not ""
    if (len >= out_len) len = out_len - 1;
    memcpy(out, path, len);
    out[len] = '\0';
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
static int hds_fx_prev_same_source_slot(const HdsFxGuiHost *p, int slot) {
    int src = (int)p->fx[slot].fx_source;
    for (int i = slot - 1; i >= 0; i--) if ((int)p->fx[i].fx_source == src) return i;
    return -1;
}

// The nearest LATER slot sharing this slot's own fx_source (the slot chain_to_next would chain
// INTO, if this slot has it set), or -1 if there is none -- see hds_fx_slot_is_chain_tail()'s own
// comment for why this existence check matters.
static int hds_fx_next_same_source_slot(const HdsFxGuiHost *p, int slot) {
    int src = (int)p->fx[slot].fx_source;
    for (int i = slot + 1; i < FX_SLOT_COUNT; i++) if ((int)p->fx[i].fx_source == src) return i;
    return -1;
}

// True if this slot starts its own chain (nothing chains INTO it) -- its own Input pill is live.
// An explicit cross-slot Input override (input_from_slot) always wins: it can never ALSO be the
// live target of some earlier slot's own Chain-to-Next (the GUI only lets input_from_slot be set
// while a slot's Input pill is open, which is gated on being a chain head in the first place -- see
// hds_fx_gui_on_button_press()'s own Input-pill handling), so this is never ambiguous in practice.
static bool hds_fx_slot_is_chain_head(const HdsFxGuiHost *p, int slot) {
    if (p->fx[slot].input_from_slot >= 0.0f) return true;
    int prev = hds_fx_prev_same_source_slot(p, slot);
    return prev < 0 || p->fx[prev].chain_to_next == 0.0f;
}

// True if this slot is the TAIL of its own chain (chains nowhere further) -- its own Output pill is
// live. NOT simply "chain_to_next is off": a slot can have chain_to_next SET with no real successor
// to receive it (its own fx_source's last slot in the pool, or a stale flag left over from before a
// LATER same-fx_source slot got moved/reassigned away) -- process_fx_chain() itself still treats
// that as an ordinary, fully-live branch (the dangling continue_branch just gets folded into the
// parallel average at the end, same as any other closed branch), so the Output picker must stay
// live for it too. Without this check, such a slot's Output pill silently vanished with no other
// slot ever gaining one in its place -- a real, reported "the chain UI doesn't always do what it
// should" bug, not merely a cosmetic one (the ALREADY-SET output channel kept working under the
// hood; only the ability to see or change it disappeared).
static bool hds_fx_slot_is_chain_tail(const HdsFxGuiHost *p, int slot) {
    if (p->fx[slot].chain_to_next == 0.0f) return true;
    return hds_fx_next_same_source_slot(p, slot) < 0;
}

// True if some OTHER slot's input_from_slot currently points at this one -- an explicit cross-slot
// link, same "protect both ends of a link from a silent Up/Down rewire" reasoning as chain_to_next's
// own array-position link below.
static bool hds_fx_slot_is_referenced_as_input(const HdsFxGuiHost *p, int slot) {
    for (int i = 0; i < FX_SLOT_COUNT; i++) {
        if (i == slot || p->fx[i].input_from_slot < 0.0f) continue;
        if ((int)(p->fx[i].input_from_slot + 0.5f) == slot) return true;
    }
    return false;
}

// True if a value-swap (Up/Down reorder) touching this slot could silently rewire a chain link --
// either this slot receives from a previous slot's Chain-to-Next, or this slot itself chains
// forward, or this slot is either end of an explicit input_from_slot cross-reference. A chain link
// is defined by ARRAY POSITION + matching fx_source (or, for input_from_slot, by ARRAY POSITION
// alone), not by an ID that travels with a slot's own params, so swapping either end of a link
// would move the wrong slot's configuration into the position the link actually depends on --
// disallowed outright rather than attempting a more surgical (and more bug-prone) partial
// allowance. See hds_fx_plugin.c's own header comment.
static bool hds_fx_slot_participates_in_chain(const HdsFxGuiHost *p, int slot) {
    return !hds_fx_slot_is_chain_head(p, slot) || !hds_fx_slot_is_chain_tail(p, slot)
        || p->fx[slot].input_from_slot >= 0.0f || hds_fx_slot_is_referenced_as_input(p, slot);
}

// Maps a 0-based position within the "FX Slot Output" section of the Input dropdown (which always
// excludes THIS row's own slot number -- self-reference is nonsensical and explicitly disallowed)
// to the real target slot index (0..FX_SLOT_COUNT-1).
static int hds_fx_slot_output_target(int row, int list_pos) {
    return (list_pos < row) ? list_pos : list_pos + 1;
}

// ---------------------------------------------------------------------------------------------
// Interaction/view state -- purely which row is hovered/dragged and how far the list is scrolled;
// nothing here is audio state (that's HdsFxGuiHost's own fx[], viewing whatever real host owns it)
// and nothing here is
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
#define HDS_FX_DROPDOWN_SOURCE 5 // the Source-combo Input header for a host with no port model -- see HdsFxGuiHost's own comment
#define HDS_FX_DROPDOWN_OUT_PAIR 6 // the Source-combo Output pill's own single-pick routing choice -- see HdsFxGuiHost's own source_routing_flat comment
// FX_TYPE_BROADCAST_DELAY's On-Air Override options -- opened by a RIGHT-click on the button itself
// (every other dropdown here opens on any click; this is the one exception, since a plain click on
// that button means something else entirely -- see hds_fx_gui_on_button_press()'s own comment) --
// not anchored to a knob/pill rect the generic layout already computes, but to the button's own
// rect built the same way hds_fx_bcastdly_dump_btn_rect() always has. Unlike every other dropdown
// here, this isn't a mutually-exclusive single pick: its two rows (MIDI Trigger Enabled/param[8],
// Disabled/param[7]) are independent checkable toggles, each showing its own current on/off state
// as a checkmark prefix baked right into the label text (see hds_fx_dropdown_option_label()'s own
// ONAIR_MODE case) rather than the single highlighted-row convention every other kind here uses --
// current_index() returns -1 for this kind specifically so nothing gets that highlight treatment.
// See fx_types.h's own param[5]/param[7]/param[8] comments for what each toggle actually does.
#define HDS_FX_DROPDOWN_ONAIR_MODE 7

typedef struct { double x, y, w, h; } HdsFxRect;

typedef struct {
    int kind;         // one of HDS_FX_DROPDOWN_* above; NONE when closed
    int row;          // which pool slot this dropdown belongs to
    HdsFxRect anchor; // the pill's own rect at the moment it was opened -- positions the list
    int scroll;       // index of the first VISIBLE option -- see hds_fx_dropdown_visible_rows()'s
                        // own comment. Always reset to 0 by hds_fx_dropdown_open() below; nothing
                        // else should ever need to touch this field directly.
} HdsFxDropdown;

// The one place a dropdown is ever opened -- see every hds_fx_gui_on_button_press() call site
// below. Exists specifically so scroll always starts at 0 on a fresh open; forgetting that reset
// at any one of those call sites (there are several, one per pill/button kind) would otherwise
// leave a stale scroll position left over from whichever OTHER dropdown happened to be open last.
static void hds_fx_dropdown_open(HdsFxDropdown *dd, int kind, int row, HdsFxRect anchor) {
    dd->kind = kind;
    dd->row = row;
    dd->anchor = anchor;
    dd->scroll = 0;
}

// In-house file browser for FX_TYPE_NAM's Model/IR pills -- a real directory listing drawn with
// this file's own Cairo primitives, not a subprocess (zenity/kdialog) and not a GTK/Qt widget. This
// replaced an earlier revision that shelled out to zenity/kdialog: that approach depended on
// whichever dialog tool (if any) happened to be installed and reachable from inside the HOST's own
// process, and real DAW testing found it silently unresponsive on more than one real system (a
// tool that's on PATH but can't actually show a picker there for reasons this plugin has no way to
// diagnose remotely -- sandboxing, a missing display/DBus portal, etc.) even after hardening the
// failure-recovery heuristics twice. An in-house browser has no external dependency to fail: it's
// exactly the same opendir()/readdir() any other program on the SAME machine already needs to list
// a directory, drawn with the exact same Cairo calls every other pixel in this GUI already uses --
// works identically on every Linux desktop regardless of what's installed, and (unlike zenity/
// kdialog, which don't exist on Windows/macOS at all) is the genuinely portable path to this file's
// own already-stated future-Windows-port goal: a win32 platform shim would only need its own
// FindFirstFile/FindNextFile-based refresh function, this entire struct and every render/hit-test
// function around it stay untouched.
//
// This file (the portable draw file) owns the STATE and all rendering/hit-testing; it never touches
// the filesystem directly (see this file's own header comment on staying free of any OS call, the
// same discipline that already kept Xlib/Win32 types out). hds_fx_gui_x11.c's own
// hds_fx_gui_browser_refresh() is the one place that actually calls opendir()/readdir()/stat() --
// triggered whenever needs_refresh is set here (opening the browser, navigating into a directory,
// pressing Up, or committing a typed path) and polled once per timer tick, same "never block the
// UI thread on a filesystem call from inside a click handler" discipline the old subprocess
// approach already established (a network filesystem's own realistic latency is exactly the kind
// of stall a single click shouldn't have to wait through synchronously).
#define HDS_FX_BROWSER_MAX_ENTRIES 256
#define HDS_FX_BROWSER_NAME_LEN 256

typedef struct {
    bool open;
    // Generic: which extension to filter/match (e.g. ".nam", ".wav", ".sfz", ".scl") and the
    // caller-owned destination buffer the picked path gets written into on commit -- set at open
    // time (hds_fx_gui_browser_open()), so this ONE modal instance can serve every "pick an
    // existing file" need in the whole GUI (NAM Model/IR, a Play-tab oscillator's own .sfz, the
    // Global sidebar's own .scl, ...) rather than each caller needing its own copy of this widget.
    // `dest` must outlive this browser's own open state -- true for every real caller, since it's
    // always a field on some persistent struct (FXSlotParams, HdsClapPlugin, ...), never a stack
    // buffer.
    char ext[8];
    char title[64]; // header text, e.g. "Select NAM Model (*.nam)" -- set at open time too
    char *dest;
    size_t dest_len;
    // Folder-picking mode (hds_fx_gui_browser_open_folder()): lists only subdirectories (no
    // extension filtering at all) and adds a "Choose This Folder" button that commits the CURRENTLY
    // listed directory (b->dir itself) rather than any one entry -- the Recording tab's own output
    // folder is the one thing in this GUI that needs an existing DIRECTORY picked, not a file.
    bool folder_mode;

    // dir is the authoritative current directory while NOT editing the path bar; WHILE editing
    // (path_bar_editing) it doubles as the live typed buffer instead (same "one buffer serves both
    // display and edit" the old edit_buffer used) -- last_valid_dir is what Escape reverts to.
    char dir[SAMPLE_PATH_LEN];
    char last_valid_dir[SAMPLE_PATH_LEN];
    bool path_bar_editing;
    // Set by Enter on the path bar: the platform shim tries the typed text as an exact FILE path
    // first (stat()+S_ISREG -- committing it directly, same as clicking a file entry), then falls
    // back to trying it as a directory (same as clicking a directory entry) if that fails.
    bool path_bar_commit_attempt;
    bool needs_refresh; // platform shim sees this, re-lists `dir`, fills entries below, clears it

    char entry_name[HDS_FX_BROWSER_MAX_ENTRIES][HDS_FX_BROWSER_NAME_LEN];
    bool entry_is_dir[HDS_FX_BROWSER_MAX_ENTRIES];
    int entry_count;
    int scroll; // index of the first visible entry row
} HdsFxBrowser;

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

    // FX_TYPE_BROADCAST_DELAY's On-Air Override button (param[5]) is HELD, not clicked -- live for
    // exactly as long as the mouse button stays down, same real-world behavior as a physical panic
    // button. -1 when nothing is held; otherwise the row whose param[5] this view press set to 1.0,
    // so hds_fx_gui_on_button_release() (which gets no x/y of its own -- see its own comment) knows
    // which row's param[5] to clear back to 0.0 on release.
    int tone_override_row;

    HdsFxDropdown dropdown;

    // FX_TYPE_NAM's own Model/IR path fields are the only free-text-adjacent inputs anywhere in
    // this GUI (every other value is a knob, a toggle, or a dropdown option) -- see HdsFxBrowser's
    // own comment above for how clicking either pill opens this in-house file browser.
    HdsFxBrowser browser;
} HdsFxGuiViewState;

static void hds_fx_gui_view_state_init(HdsFxGuiViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->window_w = HDS_FX_GUI_DEFAULT_W;
    vs->window_h = HDS_FX_GUI_DEFAULT_H;
    vs->hover_row = -1;
    vs->drag_kind = 0;
    vs->tone_override_row = -1;
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

// color_kind: 0 = normal (active = green), 1 = mute (active = red) -- hds_fx_draw_pill() below is
// the normal case; hds_fx_draw_mute_pill() is the only other caller.
static void hds_fx_draw_pill_kind(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled, int color_kind) {
    hds_fx_rounded_rect(cr, x, y, w, h, 4);
    if (disabled) hds_fx_rgb(cr, 0.16, 0.16, 0.18);
    else if (active) { if (color_kind == 1) hds_fx_rgb(cr, 0.55, 0.16, 0.16); else hds_fx_rgb(cr, 0.18, 0.42, 0.22); }
    else hds_fx_rgb(cr, 0.19, 0.19, 0.22);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    if (disabled) hds_fx_rgb(cr, 0.42, 0.42, 0.45);
    else hds_fx_rgb(cr, 0.85, 0.90, 0.90);
    // Clipped to the pill's own rect: every caller is expected to pass text short enough to fit
    // (see e.g. hds_fx_truncate_pill_text_to_width()'s own comment for the NAM Model/IR pills,
    // whose real filenames are the one label in this GUI long enough to actually need that care),
    // but this is a cheap, universal safety net so a label that's STILL too wide (an unusually
    // long word this GUI's own truncation didn't anticipate, a future caller that forgets to
    // truncate at all, ...) bleeds nowhere -- it's clipped away, never smeared over a neighboring
    // column the way an uncapped `cairo_show_text()` already did in a real, reported case.
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    hds_fx_centered_text(cr, x + w / 2, y + h / 2 + 3.5, text);
    cairo_restore(cr);
}

static void hds_fx_draw_pill(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled) {
    hds_fx_draw_pill_kind(cr, x, y, w, h, text, active, disabled, 0);
}

__attribute__((unused)) static void hds_fx_draw_mute_pill(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled) {
    hds_fx_draw_pill_kind(cr, x, y, w, h, text, active, disabled, 1);
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
// value_text_override: NULL means fall back to the old blanket "NN%" (used by the Mix knob, which
// genuinely IS a plain percent -- see hds_fx_gui_render()'s own call). Every param knob instead
// passes the real unit text from hds_fx_type_param_value_text() -- see that function's own comment.
// Just the knob's own arc/track/tick -- no label, no value text, since those two are anchored
// differently by each of this arc's two callers (hds_fx_draw_knob below anchors them at fixed
// offsets from a ROW's own top; hds_play_draw_knob in hds_clap_play_draw.c anchors them relative
// to the knob's own cy instead, since Play tab's controls aren't laid out in fixed-height rows).
// Extracted rather than duplicated so both callers' knobs are pixel-identical and any future
// visual tweak (colors, sweep angle, tick length) never has to be made twice.
static void hds_fx_draw_knob_arc(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled) {
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
}

static void hds_fx_draw_knob(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled, double row_y, const char *name_label, const char *value_text_override) {
    if (name_label && name_label[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.5);
        hds_fx_rgb(cr, disabled ? 0.38 : 0.70, disabled ? 0.38 : 0.72, disabled ? 0.40 : 0.74);
        hds_fx_centered_text(cr, cx, row_y + HDS_FX_KNOB_TOP_LABEL_Y_OFFSET, name_label);
    }
    if (value01 < 0.0f) value01 = 0.0f; else if (value01 > 1.0f) value01 = 1.0f;
    hds_fx_draw_knob_arc(cr, cx, cy, radius, value01, disabled);

    char value_text[24];
    if (value_text_override) snprintf(value_text, sizeof(value_text), "%s", value_text_override);
    else snprintf(value_text, sizeof(value_text), "%d%%", (int)(value01 * 100.0f + 0.5f));
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8.5);
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
    // Only meaningful (and only drawn) when hds_fx_type_is_gr_capable() is true for this row's own
    // fx_type -- same "always-present rect, only sometimes meaningful" pattern as sidechain_pill/
    // nam_model_pill above. Placed right after Mix, before Bypass.
    HdsFxRect gr_label;
    HdsFxRect bypass_btn;
    HdsFxRect chain_btn;
    HdsFxRect up_btn;
    HdsFxRect down_btn;
} HdsFxRowLayout;

#define HDS_FX_ROW_MARGIN 10.0
#define HDS_FX_NUM_W 22.0
#define HDS_FX_TYPE_W 103.0
#define HDS_FX_IO_PILL_W 78.0
#define HDS_FX_SIDECHAIN_W 68.0
#define HDS_FX_PARAM_STRIDE 34.0
#define HDS_FX_MIX_W 34.0
#define HDS_FX_GR_W 34.0
#define HDS_FX_BYPASS_W 24.0
#define HDS_FX_CHAIN_W 20.0
#define HDS_FX_UPDOWN_W 18.0
#define HDS_FX_GAP 8.0

// FX_TYPE_BROADCAST_DELAY's three Dump buttons (param columns 1-3) reuse the same column x-centers
// the generic knob loop would otherwise put a tiny knob at, but are drawn/hit-tested as a single
// big rectangle spanning nearly the whole row height instead -- deliberately as loud/obvious as
// this GUI's compact row layout allows, since a dump button is a "hit this NOW" control, not
// something to be found by squinting at a 10px knob. One shared helper computes the rect so draw
// and hit-test can never disagree, same discipline every other control in this file already holds
// itself to. `knob_cx` is that column's own knob-center x (HdsFxRowLayout.param_knob[idx].x),
// passed in rather than re-deriving the stride math here.
static void hds_fx_bcastdly_dump_btn_rect(double knob_cx, double row_y, HdsFxRect *out) {
    double w = (HDS_FX_PARAM_STRIDE - 6.0) * 1.5; // 1.5x this column's own default knob-slot width -- see hds_fx_bcastdly_col_x()'s own comment for how the columns themselves were spaced to give this width real room instead of overlapping
    *out = (HdsFxRect){knob_cx - w / 2.0, row_y + 6.0, w, HDS_FX_GUI_ROW_H - 12.0};
}

#define HDS_FX_BCASTDLY_BTN_PITCH 48.0 // width (42, see hds_fx_bcastdly_dump_btn_rect) plus a plain 6px gap -- same gap convention HDS_FX_PARAM_STRIDE-6 already uses everywhere else, just applied to this row's own wider pitch instead of the shared 34px column grid

// FX_TYPE_BROADCAST_DELAY's widened (1.5x) Dump buttons don't fit in the shared, 34px-wide knob
// columns every other FX type's params share (adjacent columns overlap by 8px) -- but doubling the
// column pitch to spread them out (an earlier attempt) left visibly too much dead air between them.
// This type only uses 6 of the 11 param columns anyway (Delay, Dump7s, Dmp15s, Dmp30s, Rstock,
// On-Air Override; columns 6-10 sit unused, same as when it doesn't touch the NAM-path pills that
// reuse them for other types), so instead of snapping to that grid at all, Dump7s/Dmp15s/Dmp30s/
// Rstock/On-Air are laid out at their own fixed HDS_FX_BCASTDLY_BTN_PITCH from Delay's own column --
// just wide enough for a real gap, not a whole spare column's worth of one. `i` itself still indexes
// the real param[] storage everywhere else, so this has zero effect on the wire protocol or saved
// state.
static double hds_fx_bcastdly_col_x(double delay_col_x, int i) {
    return delay_col_x + i * HDS_FX_BCASTDLY_BTN_PITCH; // i==0 (Delay) -> itself, unchanged
}

// `disabled` grays this out (same fill/text palette hds_fx_draw_pill_kind() already uses for a
// disabled pill, so this reads as "off" the same consistent way every other disabled control in
// this GUI does) -- for FX_TYPE_BROADCAST_DELAY's own Dump buttons, true whenever the Delay knob
// (param[0]) is at 0: with no nominal delay to speak of, there's nothing buffered for a dump to
// ever remove, so pressing one would be a pure no-op. Gates the click, not just the paint -- see
// this file's own call site in hds_fx_gui_on_button_press().
static void hds_fx_draw_dump_button(cairo_t *cr, const HdsFxRect *r, const char *line1, const char *line2, bool disabled) {
    cairo_new_path(cr);
    cairo_rectangle(cr, r->x, r->y, r->w, r->h);
    if (disabled) hds_fx_rgb(cr, 0.16, 0.16, 0.18);
    else hds_fx_rgb(cr, 0.72, 0.10, 0.10);
    cairo_fill_preserve(cr);
    if (disabled) hds_fx_rgb(cr, 0.32, 0.32, 0.35);
    else hds_fx_rgb(cr, 0.97, 0.35, 0.32);
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.5);
    if (disabled) hds_fx_rgb(cr, 0.42, 0.42, 0.45);
    else hds_fx_rgb(cr, 1.0, 1.0, 1.0);
    hds_fx_centered_text(cr, r->x + r->w / 2.0, r->y + r->h / 2.0 - 1.0, line1);
    hds_fx_centered_text(cr, r->x + r->w / 2.0, r->y + r->h / 2.0 + 12.0, line2);
}

// On-Air Override (param[5]) -- same big-rectangle treatment as the Dump buttons above (a live
// momentary switch is exactly the kind of "hit this NOW" control that pattern was built for), but
// orange rather than red: this one doesn't discard anything or need the Dump buttons' own
// press-confirmation weight -- it's live audio replacement for as long as it's held, and releasing
// it is instant and lossless (see run_fx_broadcastdelay()'s own comment). Brighter while actually
// held (`active`) for the same reason a real illuminated panic switch lights up under your thumb --
// reflects the plain mouse-hold gate (param[5]) only; a MIDI Trigger (param[8]) engagement
// deliberately gets no live "currently sounding" glow of its own (nothing here can safely read the
// audio thread's own gate state -- see g_bcastdly_notes_held's own comment for why that stays a
// plain audio-side global, not something round-tripped back into a param a GUI thread also writes).
// `disabled` grays it out (param[7]) same as hds_fx_draw_dump_button()'s own palette.
static void hds_fx_draw_onair_button(cairo_t *cr, const HdsFxRect *r, bool active, bool disabled) {
    cairo_new_path(cr);
    cairo_rectangle(cr, r->x, r->y, r->w, r->h);
    if (disabled) hds_fx_rgb(cr, 0.16, 0.16, 0.18);
    else if (active) hds_fx_rgb(cr, 1.0, 0.55, 0.0);
    else hds_fx_rgb(cr, 0.68, 0.40, 0.05);
    cairo_fill_preserve(cr);
    if (disabled) hds_fx_rgb(cr, 0.32, 0.32, 0.35);
    else hds_fx_rgb(cr, 1.0, 0.78, 0.35);
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.5);
    if (disabled) hds_fx_rgb(cr, 0.42, 0.42, 0.45);
    else hds_fx_rgb(cr, 1.0, 1.0, 1.0);
    hds_fx_centered_text(cr, r->x + r->w / 2.0, r->y + r->h / 2.0 - 1.0, "1000");
    hds_fx_centered_text(cr, r->x + r->w / 2.0, r->y + r->h / 2.0 + 12.0, "Hz");
}

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
    out->gr_label = (HdsFxRect){x, y + 4, HDS_FX_GR_W, HDS_FX_GUI_ROW_H - 8};
    x += HDS_FX_GR_W + HDS_FX_GAP;
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

static int hds_fx_dropdown_option_count(const HdsFxGuiHost *p, int kind) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE: return HDS_FX_SELECTABLE_TYPE_COUNT;
        // + (FX_SLOT_COUNT - 1): the "FX Slot Output" section, one entry per OTHER slot in the pool
        // (see hds_fx_slot_output_target()'s own comment) -- always exactly FX_SLOT_COUNT-1 long
        // regardless of which row is open, since every row excludes exactly one slot: itself.
        case HDS_FX_DROPDOWN_INPUT: return 1 + HDS_FX_PORT_COUNT + HDS_FX_PORT_COUNT + (FX_SLOT_COUNT - 1); // Off + 8 mono + 8 stereo + slot outputs
        case HDS_FX_DROPDOWN_OUTPUT: return 1 + HDS_FX_PORT_COUNT; // Off + 8 (always that port's real stereo pair)
        case HDS_FX_DROPDOWN_SIDECHAIN: return 1 + HDS_FX_PORT_COUNT; // "Same as Source" + 8
        case HDS_FX_DROPDOWN_SOURCE: return p->source_count + (FX_SLOT_COUNT - 1); // no Off/Mono/Stereo suboptions -- a plain Source combo, plus the same slot-output section INPUT gets
        case HDS_FX_DROPDOWN_OUT_PAIR: return 1 + p->output_pair_count; // Off (clears every pair) + one entry per real output pair
        case HDS_FX_DROPDOWN_ONAIR_MODE: return 2; // MIDI Trigger Enabled, Disabled -- two independent checkable toggles, see this kind's own comment
        default: return 0;
    }
}

// Pure text for option `idx` -- doesn't need plugin state (except SOURCE, which is host-defined
// and has no other concept to fall back on), just which list and which row within it.
static void hds_fx_dropdown_option_label(const HdsFxGuiHost *p, int kind, int row, int idx, char *buf, size_t buflen) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE:
            snprintf(buf, buflen, "%s", hds_fx_type_name(HDS_FX_SELECTABLE_TYPES[idx]));
            return;
        case HDS_FX_DROPDOWN_INPUT: {
            int base = 1 + HDS_FX_PORT_COUNT + HDS_FX_PORT_COUNT;
            if (idx < base) {
                if (idx == 0) { snprintf(buf, buflen, "Off"); return; }
                int j = idx - 1;
                bool stereo = j >= HDS_FX_PORT_COUNT;
                int port = stereo ? (j - HDS_FX_PORT_COUNT) : j;
                hds_fx_format_input_pill(buf, buflen, port, stereo);
                return;
            }
            int target = hds_fx_slot_output_target(row, idx - base);
            snprintf(buf, buflen, "Slot %d Out", target + 1);
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
        case HDS_FX_DROPDOWN_SOURCE: {
            if (idx < p->source_count) { p->source_label(idx, buf, buflen); return; }
            int target = hds_fx_slot_output_target(row, idx - p->source_count);
            snprintf(buf, buflen, "Slot %d Out", target + 1);
            return;
        }
        case HDS_FX_DROPDOWN_OUT_PAIR:
            if (idx == 0) snprintf(buf, buflen, "Off");
            else snprintf(buf, buflen, "Out %d", idx); // idx 1..N -> pair idx-1, 1-based display
            return;
        case HDS_FX_DROPDOWN_ONAIR_MODE: {
            // Two independent checkable toggles, not a mutually-exclusive pick -- each row's own
            // current on/off state is baked into the label itself as a checkmark prefix (this
            // dropdown's own generic highlight is suppressed entirely, see
            // hds_fx_dropdown_current_index()'s own ONAIR_MODE case) since the shared single-row
            // highlight this file's every OTHER dropdown uses can only ever mark one row at a time.
            bool on = (idx == 0) ? (p->fx[row].param[8] != 0.0f) : (p->fx[row].param[7] != 0.0f);
            const char *label = (idx == 0) ? "Trigger via MIDI Key" : "Disable";
            snprintf(buf, buflen, on ? "\xe2\x9c\x93 %s" : "%s", label);
            return;
        }
        default: buf[0] = 0; return;
    }
}

// Which option index is this slot's CURRENT value -- drives the highlighted row in the open list.
static int hds_fx_dropdown_current_index(const HdsFxGuiHost *p, int kind, int row) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE:
            return hds_fx_selectable_type_index((int)p->fx[row].fx_type);
        case HDS_FX_DROPDOWN_INPUT: {
            if (p->fx[row].input_from_slot >= 0.0f) {
                int target = (int)(p->fx[row].input_from_slot + 0.5f);
                int base = 1 + HDS_FX_PORT_COUNT + HDS_FX_PORT_COUNT;
                return base + (target < row ? target : target - 1);
            }
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
        case HDS_FX_DROPDOWN_SOURCE: {
            if (p->fx[row].input_from_slot >= 0.0f) {
                int target = (int)(p->fx[row].input_from_slot + 0.5f);
                return p->source_count + (target < row ? target : target - 1);
            }
            int s = (int)p->fx[row].fx_source;
            return (s < 0 || s >= p->source_count) ? 0 : s;
        }
        case HDS_FX_DROPDOWN_OUT_PAIR: {
            int src = (int)p->fx[row].fx_source;
            if (src < 0 || src >= p->source_count) src = 0;
            for (int op = 0; op < p->output_pair_count; op++) {
                if (p->source_routing_flat[src * p->output_pair_count + op] != 0.0f) return op + 1;
            }
            return 0; // nothing enabled for this source -- "Off"
        }
        // No single "current" row -- both are independent toggles, shown via their own checkmark
        // prefix in the label text instead (see hds_fx_dropdown_option_label()'s own ONAIR_MODE
        // case). -1 never matches any real idx (0..count-1), so the renderer's own
        // `if (i == current)` highlight check simply never fires for this kind.
        case HDS_FX_DROPDOWN_ONAIR_MODE: return -1;
        default: return -1;
    }
}

// Applies option `idx` to `row`. Input/Output's "Off" (idx 0) deliberately leaves fx_source/
// port_output_channel untouched -- Off is "this chain reads/writes silence right now", not a
// different port choice, so switching away from Off later returns to whatever port was last set.
static void hds_fx_dropdown_apply(HdsFxGuiHost *p, int kind, int row, int idx) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE: {
            int new_type = HDS_FX_SELECTABLE_TYPES[idx];
            p->fx[row].fx_type = (float)new_type;
            // Every param otherwise starts at its own honest zero -- but Gate's Hysteresis (param[4],
            // 0..12dB) has no useful "off" reading for a freshly-loaded gate the way Range's 0 does, so
            // seed it to a typical hardware-gate value (~3dB) the moment a slot is switched to Gate,
            // matching what a real gate pedal/rack unit ships with rather than a chattery 0dB default.
            if (new_type == FX_TYPE_GATE) p->fx[row].param[4] = 0.25f;
            // FX_TYPE_BROADCAST_DELAY's Delay (param[0]) has no useful "off" reading at 0s either --
            // seed it to a real, usable 30s (half of the 0-60s range) so all three dump buttons have
            // something to work with the moment this slot is switched on, seed Restock Speed
            // (param[4]) to a moderate, clearly-audible-if-you-listen-for-it default rather than its
            // own honest-but-glacial 1% minimum, and seed On-Air Override Volume (param[6]) to the
            // 50% the button's own spec asks for rather than its own honest-but-silent 0%.
            if (new_type == FX_TYPE_BROADCAST_DELAY) { p->fx[row].param[0] = 0.5f; p->fx[row].param[4] = 0.4f; p->fx[row].param[6] = 0.5f; }
            return;
        }
        case HDS_FX_DROPDOWN_INPUT: {
            int base = 1 + HDS_FX_PORT_COUNT + HDS_FX_PORT_COUNT;
            if (idx >= base) {
                p->fx[row].input_from_slot = (float)hds_fx_slot_output_target(row, idx - base);
                return;
            }
            p->fx[row].input_from_slot = -1.0f; // picking a normal port Input clears any slot-output override
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
        case HDS_FX_DROPDOWN_SOURCE:
            if (idx >= p->source_count) {
                p->fx[row].input_from_slot = (float)hds_fx_slot_output_target(row, idx - p->source_count);
                return;
            }
            p->fx[row].input_from_slot = -1.0f; // picking a normal Source clears any slot-output override
            p->fx[row].fx_source = (float)idx;
            return;
        case HDS_FX_DROPDOWN_OUT_PAIR: {
            // Single-pick: clears every OTHER pair this source was routed to and sets exactly the
            // one chosen here (or none, for "Off") -- see HdsFxGuiHost's own source_routing_flat
            // comment for why this mirrors a port-model host's own single-choice Output picker
            // rather than trying to be a second, cut-down copy of the Routing tab's real many-to-
            // many grid. A user who wants that keeps using the Routing tab; this pill never runs
            // unless explicitly clicked, so an existing multi-pair setup is never silently narrowed.
            int src = (int)p->fx[row].fx_source;
            if (src < 0 || src >= p->source_count) src = 0;
            for (int op = 0; op < p->output_pair_count; op++) {
                p->source_routing_flat[src * p->output_pair_count + op] = (idx == op + 1) ? 1.0f : 0.0f;
            }
            return;
        }
        case HDS_FX_DROPDOWN_ONAIR_MODE:
            // Each row is an independent checkable toggle (idx 0 = MIDI Trigger Enabled/param[8],
            // idx 1 = Disabled/param[7]) -- clicking one flips ONLY that flag, never sets a
            // mutually-exclusive mode the way every other dropdown's apply() here does.
            if (idx == 0) {
                p->fx[row].param[8] = (p->fx[row].param[8] != 0.0f) ? 0.0f : 1.0f;
            } else {
                bool turning_on = p->fx[row].param[7] == 0.0f;
                p->fx[row].param[7] = turning_on ? 1.0f : 0.0f;
                // Turning Disabled ON kills any override already sounding via the plain mouse-hold
                // gate right away, rather than leaving param[5] at 1.0 until some future release
                // event that (now that Disabled forces the gate off regardless) may never come.
                if (turning_on) p->fx[row].param[5] = 0.0f;
            }
            return;
        default: return;
    }
}

#define HDS_FX_DROPDOWN_ROW_H 22.0

static double hds_fx_dropdown_width(int kind) {
    switch (kind) {
        case HDS_FX_DROPDOWN_TYPE: return 130.0;
        case HDS_FX_DROPDOWN_SIDECHAIN: return 110.0;
        case HDS_FX_DROPDOWN_SOURCE: return 120.0; // "Program 8"/"Ext In 8"/"Combi Mix"/"Slot 32 Out" -- widest names in the list
        case HDS_FX_DROPDOWN_INPUT: return 120.0; // wide enough for "Slot 32 Out" alongside the plain port list -- see hds_fx_slot_output_target()'s own comment
        case HDS_FX_DROPDOWN_ONAIR_MODE: return 180.0; // "<checkmark> Trigger via MIDI Key" is the widest label in this list
        default: return 90.0; // Output
    }
}

// How many option rows an open list can show at once before it needs to scroll -- fit to
// whatever vertical space is actually available in THIS window (same "fit to available space"
// posture HdsFxBrowserLayout's own visible_rows already uses for the file browser's list, not a
// hardcoded constant), so a taller host window naturally shows more rows and a short one still
// shows at least a few. Several of this GUI's own lists are genuinely long now -- the Input/
// Source dropdowns' "Slot N Out" section alone is FX_SLOT_COUNT-1 (31) entries wide, on top of
// whatever the rest of that list already has, and the Type dropdown has grown to 26+ entries as
// more effects get ported in -- so letting every open list simply render every option, however
// many there are, used to produce a list taller than the window itself: entries past the bottom
// edge were simply unreachable, with no indication more existed at all. Capped at whichever is
// smaller, the real option count or the space available; never less than 1 row even on a tiny
// window.
#define HDS_FX_DROPDOWN_MARGIN_PX 20.0 // breathing room off the window's own top/bottom edges
static int hds_fx_dropdown_visible_rows(int count, int window_h) {
    int fit = (int)(((double)window_h - HDS_FX_DROPDOWN_MARGIN_PX) / HDS_FX_DROPDOWN_ROW_H);
    if (fit < 1) fit = 1;
    return (count < fit) ? count : fit;
}

// The open list's own background rect -- flips to open ABOVE the anchor pill instead of below if
// the list would otherwise run off the window's own bottom edge. Same geometry
// hds_fx_dropdown_option_rect() below builds each individual option row from, so draw and
// hit-test can never disagree about where anything is. Height is capped to however many rows
// hds_fx_dropdown_visible_rows() says actually fit -- NOT a flat `count * ROW_H` the way this used
// to work, which is exactly what let a long list run off the bottom of the window unscrollable.
static void hds_fx_dropdown_bg_rect(const HdsFxGuiHost *p, const HdsFxDropdown *dd, int window_h, HdsFxRect *out) {
    int count = hds_fx_dropdown_option_count(p, dd->kind);
    int visible_rows = hds_fx_dropdown_visible_rows(count, window_h);
    double w = hds_fx_dropdown_width(dd->kind);
    double list_h = visible_rows * HDS_FX_DROPDOWN_ROW_H;
    double below_y = dd->anchor.y + dd->anchor.h + 2.0;
    double y0 = (below_y + list_h > (double)window_h) ? (dd->anchor.y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    *out = (HdsFxRect){dd->anchor.x, y0, w, list_h};
}

// idx is a real option index (0..count-1), not a row position within the visible window -- the
// caller is responsible for only asking about indices actually in [dd->scroll, dd->scroll+
// visible_rows) (see hds_fx_dropdown_visible_range() below); this function just maps whichever
// index it's given to a Y position relative to dd->scroll, same as it always positioned rows
// relative to 0 before scrolling existed.
static void hds_fx_dropdown_option_rect(const HdsFxGuiHost *p, const HdsFxDropdown *dd, int window_h, int idx, HdsFxRect *out) {
    HdsFxRect bg;
    hds_fx_dropdown_bg_rect(p, dd, window_h, &bg);
    *out = (HdsFxRect){bg.x, bg.y + (idx - dd->scroll) * HDS_FX_DROPDOWN_ROW_H, bg.w, HDS_FX_DROPDOWN_ROW_H};
}

// The [first, last) range of option indices actually visible right now, given dd->scroll -- the
// one place both the render loop and the click hit-test loop get this range from, so they can
// never disagree about which rows are currently on screen.
static void hds_fx_dropdown_visible_range(const HdsFxGuiHost *p, const HdsFxDropdown *dd, int window_h, int *first, int *last) {
    int count = hds_fx_dropdown_option_count(p, dd->kind);
    int visible_rows = hds_fx_dropdown_visible_rows(count, window_h);
    *first = dd->scroll;
    if (*first < 0) *first = 0;
    if (*first > count - visible_rows) *first = (count > visible_rows) ? count - visible_rows : 0;
    *last = *first + visible_rows;
    if (*last > count) *last = count;
}

static void hds_fx_gui_render_dropdown(cairo_t *cr, HdsFxGuiHost *p, const HdsFxGuiViewState *vs) {
    if (vs->dropdown.kind == HDS_FX_DROPDOWN_NONE) return;
    const HdsFxDropdown *dd = &vs->dropdown;
    int count = hds_fx_dropdown_option_count(p, dd->kind);
    int current = hds_fx_dropdown_current_index(p, dd->kind, dd->row);
    int first, last;
    hds_fx_dropdown_visible_range(p, dd, vs->window_h, &first, &last);

    HdsFxRect bg;
    hds_fx_dropdown_bg_rect(p, dd, vs->window_h, &bg);
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
    cairo_set_font_size(cr, 10.5);
    for (int i = first; i < last; i++) {
        HdsFxRect r;
        hds_fx_dropdown_option_rect(p, dd, vs->window_h, i, &r);
        if (i == current) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_fill(cr);
        }
        char label[24];
        hds_fx_dropdown_option_label(p, dd->kind, dd->row, i, label, sizeof(label));
        hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
        hds_fx_left_text(cr, r.x + 6, r.y + r.h / 2 + 3.5, label);
    }

    // Scrollbar thumb, same visual idiom as the main slot list's own (see hds_fx_gui_render's own
    // thumb a bit further down) -- only drawn at all when there's genuinely more than one
    // screenful of options, so a short list looks exactly like it always did.
    if (count > (last - first)) {
        double track_x = bg.x + bg.w - 4.0;
        double visible_frac = (double)(last - first) / (double)count;
        double thumb_h = bg.h * visible_frac;
        if (thumb_h < 12.0) thumb_h = 12.0;
        double max_scroll_rows = (double)(count - (last - first));
        double thumb_y = bg.y + (bg.h - thumb_h) * ((double)first / max_scroll_rows);
        hds_fx_rgba(cr, 1, 1, 1, 0.35);
        cairo_new_path(cr);
        cairo_rectangle(cr, track_x, thumb_y, 3.0, thumb_h);
        cairo_fill(cr);
    }
}

// ---------------------------------------------------------------------------------------------
// In-house file browser -- see HdsFxBrowser's own comment for why this replaced the earlier
// zenity/kdialog subprocess approach. A centered modal panel, same "click outside to close"
// posture as the dropdowns above, computed fresh from the CURRENT window size every time (not
// anchored at open time like a dropdown's own small list is), so a window resize while the
// browser is open never leaves it mis-positioned.
// ---------------------------------------------------------------------------------------------
#define HDS_FX_BROWSER_ROW_H 22.0
#define HDS_FX_BROWSER_TITLE_H 34.0
#define HDS_FX_BROWSER_PATHBAR_H 24.0
#define HDS_FX_BROWSER_UP_ROW_H 22.0
#define HDS_FX_BROWSER_MARGIN 12.0

typedef struct {
    HdsFxRect panel;
    HdsFxRect close_btn;
    HdsFxRect choose_btn; // only meaningful in folder_mode -- "Choose This Folder"
    HdsFxRect path_bar;
    HdsFxRect up_row; // only meaningful when show_up
    bool show_up;
    HdsFxRect list_area;
    int visible_rows;
} HdsFxBrowserLayout;

static void hds_fx_browser_compute_layout(const HdsFxGuiViewState *vs, HdsFxBrowserLayout *out) {
    double pw = (double)vs->window_w - 80.0;
    if (pw > 620.0) pw = 620.0;
    if (pw < 280.0) pw = ((double)vs->window_w > 300.0) ? (double)vs->window_w - 20.0 : 280.0;
    double ph = (double)vs->window_h - 70.0;
    if (ph > 520.0) ph = 520.0;
    if (ph < 220.0) ph = ((double)vs->window_h > 240.0) ? (double)vs->window_h - 20.0 : 220.0;
    out->panel.x = ((double)vs->window_w - pw) / 2.0;
    out->panel.y = ((double)vs->window_h - ph) / 2.0;
    out->panel.w = pw;
    out->panel.h = ph;

    out->close_btn = (HdsFxRect){out->panel.x + out->panel.w - 26, out->panel.y + 6, 20, 20};
    out->choose_btn = (HdsFxRect){out->panel.x + out->panel.w - 26 - 130, out->panel.y + 4, 124, 22};
    out->path_bar = (HdsFxRect){out->panel.x + HDS_FX_BROWSER_MARGIN, out->panel.y + HDS_FX_BROWSER_TITLE_H,
                                 out->panel.w - 2 * HDS_FX_BROWSER_MARGIN, HDS_FX_BROWSER_PATHBAR_H};
    double list_top = out->path_bar.y + out->path_bar.h + 4.0;
    out->show_up = (strcmp(vs->browser.dir, "/") != 0);
    if (out->show_up) {
        out->up_row = (HdsFxRect){out->panel.x + HDS_FX_BROWSER_MARGIN, list_top,
                                   out->panel.w - 2 * HDS_FX_BROWSER_MARGIN, HDS_FX_BROWSER_UP_ROW_H};
        list_top += HDS_FX_BROWSER_UP_ROW_H;
    } else {
        out->up_row = (HdsFxRect){0, 0, 0, 0};
    }
    double list_bottom = out->panel.y + out->panel.h - HDS_FX_BROWSER_MARGIN;
    out->list_area = (HdsFxRect){out->panel.x + HDS_FX_BROWSER_MARGIN, list_top,
                                  out->panel.w - 2 * HDS_FX_BROWSER_MARGIN, list_bottom - list_top};
    out->visible_rows = (int)(out->list_area.h / HDS_FX_BROWSER_ROW_H);
    if (out->visible_rows < 1) out->visible_rows = 1;
}

// Opens the browser to fill `dest` (a caller-owned buffer of `dest_len`, still holding whatever
// path -- if any -- is already stored there) with an existing file matching `ext`, labeled `title`
// -- called from wherever a "pick a file" need arises (FX_TYPE_NAM's own Model/IR pills here, an
// oscillator's own .sfz Multisample loader, the Global sidebar's own .scl Scale loader, ...): this
// ONE modal instance serves every one of them, never a per-caller copy. Seeds the starting directory
// from whatever path is already stored (hds_fx_dirname()); needs_refresh tells the platform shim
// (hds_fx_gui_x11.c) to actually list it on the very next poll, which happens before this same
// tick's own redraw (see hds_fx_gui_on_timer()'s own ordering) -- so the very click that opens the
// browser also sees a populated listing immediately, never one empty frame first.
static void hds_fx_gui_browser_open(HdsFxGuiViewState *vs, const char *ext, const char *title, char *dest, size_t dest_len) {
    HdsFxBrowser *b = &vs->browser;
    memset(b, 0, sizeof(*b));
    b->open = true;
    snprintf(b->ext, sizeof(b->ext), "%s", ext);
    snprintf(b->title, sizeof(b->title), "%s", title);
    b->dest = dest;
    b->dest_len = dest_len;
    hds_fx_dirname(dest, b->dir, sizeof(b->dir));
    b->needs_refresh = true;
}

// Same idea as hds_fx_gui_browser_open() but for picking an existing DIRECTORY instead of a file
// matching one extension -- see HdsFxBrowser's own folder_mode comment. `dest` already holds a
// folder path (never a file path), so it seeds the starting directory directly rather than through
// hds_fx_dirname() (which would incorrectly strip its last path component).
// __attribute__((unused)): only ever called from the Recording tab's own folder picker
// (hds_clap_recording_draw.c), which is standalone-only -- unused (and correctly so, same posture
// as hds_fx_draw_mute_pill() above) in both CLAP plugin builds.
__attribute__((unused)) static void hds_fx_gui_browser_open_folder(HdsFxGuiViewState *vs, const char *title, char *dest, size_t dest_len) {
    HdsFxBrowser *b = &vs->browser;
    memset(b, 0, sizeof(*b));
    b->open = true;
    b->folder_mode = true;
    snprintf(b->title, sizeof(b->title), "%s", title);
    b->dest = dest;
    b->dest_len = dest_len;
    hds_fx_safe_copy(b->dir, sizeof(b->dir), dest);
    b->needs_refresh = true;
}

static void hds_fx_gui_browser_close(HdsFxGuiViewState *vs) { vs->browser.open = false; }

// Handles every click while the browser is open -- called EXCLUSIVELY (see
// hds_fx_gui_on_button_press()'s own ordering, same "one modal owns every click" posture the
// dropdown check above already has). Always returns true (consumed): a click anywhere while the
// browser is open is never "let it fall through to whatever's underneath."
static bool hds_fx_gui_browser_on_click(HdsFxGuiViewState *vs, double x, double y) {
    HdsFxBrowser *b = &vs->browser;
    HdsFxBrowserLayout L;
    hds_fx_browser_compute_layout(vs, &L);

    if (!hds_fx_rect_contains(&L.panel, x, y)) { hds_fx_gui_browser_close(vs); return true; }
    if (hds_fx_rect_contains(&L.close_btn, x, y)) { hds_fx_gui_browser_close(vs); return true; }
    if (b->folder_mode && hds_fx_rect_contains(&L.choose_btn, x, y)) {
        hds_fx_safe_copy(b->dest, b->dest_len, b->dir);
        hds_fx_gui_browser_close(vs);
        return true;
    }
    if (hds_fx_rect_contains(&L.path_bar, x, y)) { b->path_bar_editing = true; return true; }

    // Any other click while the path bar was mid-edit abandons that edit (reverts to the last
    // directory that actually listed successfully) -- same "a click elsewhere ends the current
    // transient text edit" posture the old inline editor had, just without committing it (only
    // Enter commits a typed path here -- see hds_fx_gui_on_key()).
    if (b->path_bar_editing) {
        b->path_bar_editing = false;
        snprintf(b->dir, sizeof(b->dir), "%s", b->last_valid_dir);
    }

    if (L.show_up && hds_fx_rect_contains(&L.up_row, x, y)) {
        char parent[SAMPLE_PATH_LEN];
        hds_fx_dirname(b->dir, parent, sizeof(parent));
        snprintf(b->dir, sizeof(b->dir), "%s", parent[0] ? parent : "/");
        b->needs_refresh = true;
        return true;
    }

    if (hds_fx_rect_contains(&L.list_area, x, y)) {
        int visible_idx = (int)((y - L.list_area.y) / HDS_FX_BROWSER_ROW_H);
        int idx = b->scroll + visible_idx;
        if (idx >= 0 && idx < b->entry_count) {
            char full[SAMPLE_PATH_LEN + HDS_FX_BROWSER_NAME_LEN + 2];
            if (strcmp(b->dir, "/") == 0) snprintf(full, sizeof(full), "/%s", b->entry_name[idx]);
            else snprintf(full, sizeof(full), "%s/%s", b->dir, b->entry_name[idx]);
            if (b->entry_is_dir[idx]) {
                hds_fx_safe_copy(b->dir, sizeof(b->dir), full);
                b->needs_refresh = true;
            } else {
                hds_fx_safe_copy(b->dest, b->dest_len, full);
                hds_fx_gui_browser_close(vs);
            }
        }
        return true;
    }
    return true; // clicked somewhere inside the panel but not on any live control -- still consumed
}

static void hds_fx_gui_render_browser(cairo_t *cr, const HdsFxGuiViewState *vs) {
    const HdsFxBrowser *b = &vs->browser;
    if (!b->open) return;
    HdsFxBrowserLayout L;
    hds_fx_browser_compute_layout(vs, &L);

    hds_fx_rgba(cr, 0, 0, 0, 0.55); // dim backdrop -- click on it closes the browser
    cairo_new_path(cr);
    cairo_rectangle(cr, 0, 0, vs->window_w, vs->window_h);
    cairo_fill(cr);

    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    hds_fx_rounded_rect(cr, L.panel.x, L.panel.y, L.panel.w, L.panel.h, 6);
    cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.18);
    cairo_new_path(cr);
    cairo_set_line_width(cr, 1.0);
    hds_fx_rounded_rect(cr, L.panel.x + 0.5, L.panel.y + 0.5, L.panel.w - 1, L.panel.h - 1, 6);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    hds_fx_rgb(cr, 0.9, 0.9, 0.92);
    hds_fx_left_text(cr, L.panel.x + HDS_FX_BROWSER_MARGIN, L.panel.y + 20, b->title);

    hds_fx_rgb(cr, 0.22, 0.22, 0.26);
    hds_fx_rounded_rect(cr, L.close_btn.x, L.close_btn.y, L.close_btn.w, L.close_btn.h, 3);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    hds_fx_rgb(cr, 0.75, 0.75, 0.78);
    hds_fx_centered_text(cr, L.close_btn.x + L.close_btn.w / 2, L.close_btn.y + L.close_btn.h / 2 + 4, "X");

    if (b->folder_mode) {
        hds_fx_rgb(cr, 0.18, 0.42, 0.22);
        hds_fx_rounded_rect(cr, L.choose_btn.x, L.choose_btn.y, L.choose_btn.w, L.choose_btn.h, 3);
        cairo_fill(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        hds_fx_rgb(cr, 0.9, 0.98, 0.92);
        hds_fx_centered_text(cr, L.choose_btn.x + L.choose_btn.w / 2, L.choose_btn.y + L.choose_btn.h / 2 + 4, "Choose This Folder");
    }

    // Path bar -- `dir` doubles as the live typed buffer while path_bar_editing (see HdsFxBrowser's
    // own comment), with a trailing cursor "_" exactly like the old inline path editor drew.
    hds_fx_rgb(cr, b->path_bar_editing ? 0.16 : 0.10, b->path_bar_editing ? 0.20 : 0.10, b->path_bar_editing ? 0.22 : 0.12);
    cairo_new_path(cr);
    cairo_rectangle(cr, L.path_bar.x, L.path_bar.y, L.path_bar.w, L.path_bar.h);
    cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.15);
    cairo_new_path(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, L.path_bar.x + 0.5, L.path_bar.y + 0.5, L.path_bar.w - 1, L.path_bar.h - 1);
    cairo_stroke(cr);
    char path_disp[SAMPLE_PATH_LEN + 2];
    snprintf(path_disp, sizeof(path_disp), b->path_bar_editing ? "%s_" : "%s", b->dir);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    hds_fx_rgb(cr, 0.85, 0.88, 0.90);
    hds_fx_left_text(cr, L.path_bar.x + 6, L.path_bar.y + L.path_bar.h / 2 + 3.5, path_disp);

    if (L.show_up) {
        hds_fx_rgb(cr, 0.17, 0.17, 0.20);
        cairo_new_path(cr);
        cairo_rectangle(cr, L.up_row.x, L.up_row.y, L.up_row.w, L.up_row.h);
        cairo_fill(cr);
        hds_fx_rgb(cr, 0.78, 0.80, 0.82);
        hds_fx_left_text(cr, L.up_row.x + 6, L.up_row.y + L.up_row.h / 2 + 3.5, ".. (up one level)");
    }

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, L.list_area.x, L.list_area.y, L.list_area.w, L.list_area.h);
    cairo_clip(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    if (b->entry_count == 0) {
        hds_fx_rgb(cr, 0.5, 0.5, 0.53);
        hds_fx_left_text(cr, L.list_area.x + 4, L.list_area.y + 14, "(empty, or this directory can't be read)");
    }
    for (int i = 0; i < b->entry_count; i++) {
        int visible_idx = i - b->scroll;
        if (visible_idx < 0 || visible_idx >= L.visible_rows) continue;
        double ry = L.list_area.y + visible_idx * HDS_FX_BROWSER_ROW_H;
        if (b->entry_is_dir[i]) {
            // Directories shown as "name/" in a distinct color -- a trailing slash rather than a
            // glyph/icon prefix, since a plain ASCII "/" is guaranteed to render on every font this
            // GUI could ever be running under (an icon glyph is not).
            char buf[HDS_FX_BROWSER_NAME_LEN + 2];
            snprintf(buf, sizeof(buf), "%s/", b->entry_name[i]);
            hds_fx_rgb(cr, 0.55, 0.75, 0.95);
            hds_fx_left_text(cr, L.list_area.x + 4, ry + HDS_FX_BROWSER_ROW_H / 2 + 3.5, buf);
        } else {
            hds_fx_rgb(cr, 0.82, 0.84, 0.86);
            hds_fx_left_text(cr, L.list_area.x + 4, ry + HDS_FX_BROWSER_ROW_H / 2 + 3.5, b->entry_name[i]);
        }
    }
    cairo_restore(cr);
}

static void hds_fx_gui_render(cairo_t *cr, HdsFxGuiHost *p, const HdsFxGuiViewState *vs) {
    int width = vs->window_w, height = vs->window_h;

    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    // Title
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18);
    hds_fx_rgb(cr, 0.95, 0.55, 0.15);
    hds_fx_left_text(cr, HDS_FX_ROW_MARGIN, 22, "HDS FX");
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    hds_fx_rgb(cr, 0.5, 0.5, 0.53);
    hds_fx_left_text(cr, 90, 21, "8 stereo ports \xc2\xb7 32 shared FX slots \xc2\xb7 each chain picks its own Input/Output, Mono or Stereo");

    // Column headers -- reads the SAME x-positions every row itself uses (hds_fx_gui_compute_row_layout
    // at y=0, ignoring its y-derived fields entirely) so a label can never drift out of alignment
    // with its own column.
    {
        HdsFxRowLayout ref;
        hds_fx_gui_compute_row_layout(width, 0, &ref);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9);
        hds_fx_rgb(cr, 0.5, 0.5, 0.53);
        hds_fx_centered_text(cr, ref.type_pill.x + ref.type_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "TYPE");
        hds_fx_centered_text(cr, ref.input_pill.x + ref.input_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "IN");
        hds_fx_centered_text(cr, ref.sidechain_pill.x + ref.sidechain_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "SC");
        double params_cx = (ref.param_knob[0].x + ref.param_knob[FX_SLOT_PARAM_COUNT - 1].x) / 2.0;
        hds_fx_centered_text(cr, params_cx, HDS_FX_COLUMN_HEADER_Y, "PARAMS");
        hds_fx_centered_text(cr, ref.output_pill.x + ref.output_pill.w / 2, HDS_FX_COLUMN_HEADER_Y, "OUT");
        hds_fx_centered_text(cr, ref.mix_knob.x, HDS_FX_COLUMN_HEADER_Y, "MIX");
        hds_fx_centered_text(cr, ref.gr_label.x + ref.gr_label.w / 2, HDS_FX_COLUMN_HEADER_Y, "GR");
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
        cairo_set_font_size(cr, 10);
        hds_fx_rgb(cr, 0.42, 0.42, 0.45);
        char numbuf[8]; snprintf(numbuf, sizeof(numbuf), "%02d", s + 1);
        hds_fx_left_text(cr, HDS_FX_ROW_MARGIN, y + HDS_FX_GUI_ROW_H / 2 + 3, numbuf);

        hds_fx_draw_pill(cr, L.type_pill.x, L.type_pill.y, L.type_pill.w, L.type_pill.h, hds_fx_type_name(fx_type), fx_type != FX_TYPE_OFF, false);

        // Input pill: only live on a chain HEAD -- a continuation slot has no Input of its own, it
        // just inherits the running signal from whatever chains into it. "Off" (silence in,
        // instead of any physical port) is its own selectable state, independent of which port
        // number is stored -- see hds_fx_dropdown_apply()'s own comment.
        if (slot->input_from_slot >= 0.0f) {
            // Explicit cross-slot Input override (see FXSlotParams' own comment) -- always shown as
            // a live, active pill: it always wins over whatever fx_source/port state is left
            // underneath it, so it's never "disabled" the way an ordinary non-head slot's Input is.
            char inbuf[24];
            snprintf(inbuf, sizeof(inbuf), "Slot %d Out", (int)(slot->input_from_slot + 0.5f) + 1);
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, inbuf, true, false);
        } else if (p->port_input_stereo) {
            char inbuf[16];
            bool input_off = p->port_input_off[port] != 0.0f;
            if (input_off) snprintf(inbuf, sizeof(inbuf), "Off");
            else hds_fx_format_input_pill(inbuf, sizeof(inbuf), port, p->port_input_stereo[port] != 0.0f);
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, inbuf, is_head && !input_off, !is_head);
        } else if (p->source_count > 0) {
            // The OTHER Input header shape: a Source combo (Program/Combi Mix/Ext In), reusing the
            // exact same fx_source field and the exact same chain-head gating a port-model host
            // uses for its own Input pill -- see HdsFxGuiHost's own comment on source_count/
            // source_label. `port` here is already fx_source clamped against HDS_FX_PORT_COUNT,
            // which a Source-combo host #defines to its own real source count before including
            // this file (see clap_plugin.c's own comment), so it's already the right index.
            char srcbuf[24];
            p->source_label(port, srcbuf, sizeof(srcbuf));
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, srcbuf, is_head, !is_head);
        } else {
            // This host has neither an N-port Input concept nor a Source combo -- leaves the space
            // blank rather than assume a model that doesn't apply (currently unreached: both real
            // hosts set one or the other).
            hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, "\xe2\x80\x94", false, true);
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

        bool is_bcastdly = hds_fx_type_is_broadcast_delay(fx_type);
        for (int i = 0; i < param_count; i++) {
            double col_x = is_bcastdly ? hds_fx_bcastdly_col_x(L.param_knob[0].x, i) : L.param_knob[i].x; // see hds_fx_bcastdly_col_x()'s own comment
            // FX_TYPE_BROADCAST_DELAY's Dump 7s/15s/30s (param columns 1-3): a big red momentary
            // button, not a knob -- see hds_fx_draw_dump_button()'s own comment.
            if (is_bcastdly && (i == 1 || i == 2 || i == 3)) {
                HdsFxRect btn;
                hds_fx_bcastdly_dump_btn_rect(col_x, y, &btn);
                const char *line2 = (i == 1) ? "7s" : (i == 2) ? "15s" : "30s";
                hds_fx_draw_dump_button(cr, &btn, "DUMP", line2, slot->param[0] <= 0.0f);
                continue;
            }
            // On-Air Override (param[5]): same big momentary-button treatment, orange instead of
            // red -- see hds_fx_draw_onair_button()'s own comment.
            if (is_bcastdly && i == 5) {
                HdsFxRect btn;
                hds_fx_bcastdly_dump_btn_rect(col_x, y, &btn);
                hds_fx_draw_onair_button(cr, &btn, slot->param[5] != 0.0f, slot->param[7] != 0.0f);
                continue;
            }
            // Normalize against this param's own real STORAGE range (see
            // hds_fx_type_param_range()'s own comment -- every param is 0.0-1.0 except
            // FX_TYPE_MULTIFILTER's Reso, which is 0-100) before either drawing the knob's arc or
            // computing its value text, so both always agree on what "full turn" means.
            float lo, hi; hds_fx_type_param_range(fx_type, i, &lo, &hi);
            float frac = (hi > lo) ? (slot->param[i] - lo) / (hi - lo) : 0.0f;
            if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
            char value_text[24];
            hds_fx_type_param_value_text(fx_type, i, frac, value_text, sizeof(value_text));
            hds_fx_draw_knob(cr, col_x, L.param_knob[i].y, L.param_knob[i].w, frac, false, y, hds_fx_type_param_label(fx_type, i), value_text);
        }

        // NAM's own Model/IR path pills -- see HdsFxRowLayout's own comment on nam_model_pill/
        // nam_ir_pill for why these reuse otherwise-blank knob columns. Shows the loaded path's
        // basename or "(none)" -- clicking either pill opens the in-house file browser (see
        // hds_fx_gui_on_button_press()'s own NAM handling and HdsFxBrowser's own comment).
        if (hds_fx_type_uses_nam_paths(fx_type)) {
            bool browsing_model = (vs->browser.open && vs->browser.dest == slot->nam_model_path);
            bool browsing_ir = (vs->browser.open && vs->browser.dest == slot->nam_ir_path);
            char model_buf[64], ir_buf[64];
            // Just the filename (hds_fx_basename()), never the full path -- the pill has room for
            // a name, not a path, and the full path still lives in nam_model_path/nam_ir_path
            // (visible in the file browser's own path bar, or a save file) for anyone who needs it.
            hds_fx_pill_label_fit(cr, "M: ", slot->nam_model_path[0] ? hds_fx_basename(slot->nam_model_path) : "(none)",
                                   model_buf, sizeof(model_buf), L.nam_model_pill.w - 8.0);
            hds_fx_pill_label_fit(cr, "IR: ", slot->nam_ir_path[0] ? hds_fx_basename(slot->nam_ir_path) : "(none)",
                                   ir_buf, sizeof(ir_buf), L.nam_ir_pill.w - 8.0);
            hds_fx_draw_pill(cr, L.nam_model_pill.x, L.nam_model_pill.y, L.nam_model_pill.w, L.nam_model_pill.h, model_buf, browsing_model || slot->nam_model_path[0], false);
            hds_fx_draw_pill(cr, L.nam_ir_pill.x, L.nam_ir_pill.y, L.nam_ir_pill.w, L.nam_ir_pill.h, ir_buf, browsing_ir || slot->nam_ir_path[0], false);
        }

        // Output pill: only live on a chain TAIL -- a slot still chaining forward doesn't decide
        // where anything lands yet, the tail of its own chain does. "Off" (result goes nowhere)
        // is its own selectable state, independent of which port number is stored.
        if (p->port_output_channel) {
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
        } else if (p->source_routing_flat && p->output_pair_count > 0) {
            // Source-combo host's own equivalent -- see HdsFxGuiHost's own source_routing_flat
            // comment. Reads the REAL Routing-tab data directly (no separate copy): summarizes
            // however many pairs this row's own source is currently routed to, since a source can
            // legitimately be routed to several pairs at once (many-to-many, set from the Routing
            // tab) even though this pill's own dropdown can only pick a single one at a time.
            int src = (int)slot->fx_source;
            if (src < 0 || src >= p->source_count) src = 0;
            int enabled_count = 0, first_enabled = -1;
            for (int op = 0; op < p->output_pair_count; op++) {
                if (p->source_routing_flat[src * p->output_pair_count + op] != 0.0f) {
                    enabled_count++;
                    if (first_enabled < 0) first_enabled = op;
                }
            }
            char outbuf[16];
            if (enabled_count == 0) snprintf(outbuf, sizeof(outbuf), "Off");
            else if (enabled_count == 1) snprintf(outbuf, sizeof(outbuf), "Out %d", first_enabled + 1);
            else snprintf(outbuf, sizeof(outbuf), "Out %d+", first_enabled + 1); // multi-pair, set from the Routing tab
            hds_fx_draw_pill(cr, L.output_pill.x, L.output_pill.y, L.output_pill.w, L.output_pill.h, outbuf, is_tail && enabled_count > 0, !is_tail);
        } else {
            // No flat N-port Output concept for this host either -- see the Input pill's own
            // comment just above (same reasoning, mirrored).
            hds_fx_draw_pill(cr, L.output_pill.x, L.output_pill.y, L.output_pill.w, L.output_pill.h, "\xe2\x80\x94", false, true);
        }

        hds_fx_draw_knob(cr, L.mix_knob.x, L.mix_knob.y, L.mix_knob.w, slot->mix / 100.0f, false, y, NULL, NULL);

        // Per-slot GR (gain reduction) dB readout -- see hds_fx_type_is_gr_capable()'s own comment
        // and HdsFxGuiHost.fx_gr_db's own comment. Only drawn for a GR-capable type on a host that
        // actually supplied live data: a tiny "GR" caption above a bold amber value, "%.1f" with no
        // unit.
        if (p->fx_gr_db && hds_fx_type_is_gr_capable(fx_type)) {
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 8);
            hds_fx_rgb(cr, 0.45, 0.45, 0.48);
            hds_fx_centered_text(cr, L.gr_label.x + L.gr_label.w / 2, L.gr_label.y + 8, "GR");
            char gr_buf[16];
            snprintf(gr_buf, sizeof(gr_buf), "%.1f", (double)p->fx_gr_db[s]);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 10);
            hds_fx_rgb(cr, 0.71, 0.54, 0.0);
            hds_fx_centered_text(cr, L.gr_label.x + L.gr_label.w / 2, L.gr_label.y + L.gr_label.h - 3, gr_buf);
        }

        // FX_TYPE_BROADCAST_DELAY's own "available dump time" readout -- reuses the same gr_label
        // rect the GR meter above uses (this type isn't GR-capable, so that column is otherwise
        // blank on this row, same "always-present rect, only sometimes meaningful" reuse pattern
        // NAM's Model/IR pills already established for unused knob columns). Color runs green (the
        // buffer is at or near its full nominal Delay -- every dump button is safe to press) down
        // through amber to red (recently dumped and still restocking -- see fx_chain.c's own
        // run_fx_broadcastdelay() comment) as the fraction of nominal actually buffered right now
        // drops, giving an at-a-glance answer to "is it safe to hit dump again yet" without having
        // to read the number at all.
        if (p->fx_dump_avail_sec && hds_fx_type_is_broadcast_delay(fx_type)) {
            float nominal_sec = fmaxf(0.0f, fminf(1.0f, slot->param[0])) * (float)HDS_BCASTDLY_MAX_SEC;
            float avail_sec = p->fx_dump_avail_sec[s];
            float full_frac = (nominal_sec > 0.01f) ? fminf(1.0f, avail_sec / nominal_sec) : 1.0f;
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 8);
            hds_fx_rgb(cr, 0.45, 0.45, 0.48);
            hds_fx_centered_text(cr, L.gr_label.x + L.gr_label.w / 2, L.gr_label.y + 8, "AVAIL");
            char avail_buf[16];
            snprintf(avail_buf, sizeof(avail_buf), "%.1fs", (double)avail_sec);
            // Red (depleted) -> amber -> green (full), a plain two-segment lerp through amber at
            // the midpoint -- simple, and the exact hue barely matters once the two clear endpoints
            // (full = green = safe, empty = red = still restocking) read correctly at a glance.
            double rr, gg, bb;
            if (full_frac < 0.5f) { double t = (double)full_frac / 0.5; rr = 0.85; gg = 0.25 + t * (0.55 - 0.25); bb = 0.15; }
            else { double t = ((double)full_frac - 0.5) / 0.5; rr = 0.85 - t * (0.85 - 0.15); gg = 0.55 + t * (0.80 - 0.55); bb = 0.15 + t * (0.20 - 0.15); }
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 10);
            hds_fx_rgb(cr, rr, gg, bb);
            hds_fx_centered_text(cr, L.gr_label.x + L.gr_label.w / 2, L.gr_label.y + L.gr_label.h - 3, avail_buf);
        }

        bool bypassed = slot->bypass != 0.0f;
        hds_fx_rounded_rect(cr, L.bypass_btn.x, L.bypass_btn.y, L.bypass_btn.w, L.bypass_btn.h, 3);
        hds_fx_rgb(cr, bypassed ? 0.72 : 0.20, bypassed ? 0.45 : 0.20, bypassed ? 0.18 : 0.23);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_set_font_size(cr, 9);
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
        cairo_set_font_size(cr, 10);
        hds_fx_rgb(cr, chain_on ? 0.9 : (chain_available ? 0.5 : 0.3), chain_on ? 0.95 : (chain_available ? 0.5 : 0.3), chain_on ? 0.9 : (chain_available ? 0.53 : 0.32));
        // Plain ASCII, not the UTF-8 arrow/triangle glyphs this used to draw: cairo's "toy" text API
        // (cairo_show_text via hds_fx_centered_text) picks ONE font via fontconfig and does no
        // per-glyph fallback the way a real text-layout engine (Pango, HarfBuzz) would -- if that
        // resolved "sans-serif" font happens to lack a given Unicode glyph on some system, it simply
        // doesn't render anything for it, with no substitution. ASCII always has a glyph in any font
        // this API could plausibly resolve to.
        hds_fx_centered_text(cr, L.chain_btn.x + L.chain_btn.w / 2, L.chain_btn.y + L.chain_btn.h / 2 + 3, "->");

        // Up/Down: disabled at the pool's own edges AND on any slot touching a chain link (either
        // end) -- see hds_fx_slot_participates_in_chain()'s own comment for why swapping a chained
        // slot is unsafe, not just inconvenient.
        bool chained = hds_fx_slot_participates_in_chain(p, s);
        bool can_up = s > 0 && !chained, can_down = s < FX_SLOT_COUNT - 1 && !chained;
        hds_fx_rgb(cr, can_up ? 0.65 : 0.28, can_up ? 0.65 : 0.28, can_up ? 0.68 : 0.30);
        cairo_set_font_size(cr, 9);
        hds_fx_centered_text(cr, L.up_btn.x + L.up_btn.w / 2, L.up_btn.y + L.up_btn.h / 2 + 3, "^"); // was the UTF-8 triangle glyph -- see the chain-arrow's own comment just above on why
        hds_fx_rgb(cr, can_down ? 0.65 : 0.28, can_down ? 0.65 : 0.28, can_down ? 0.68 : 0.30);
        hds_fx_centered_text(cr, L.down_btn.x + L.down_btn.w / 2, L.down_btn.y + L.down_btn.h / 2 + 3, "v");
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

    hds_fx_gui_render_dropdown(cr, p, vs); // on top of everything else, unclipped by the list's own scroll region
    hds_fx_gui_render_browser(cr, vs); // last of all -- the browser is modal, so it draws over a dropdown too (never both open at once in practice, but this ordering is correct either way)
}

// ---------------------------------------------------------------------------------------------
// Interaction -- mutates HdsFxGuiHost/HdsFxGuiViewState directly; no CLAP types referenced
// anywhere in this file. hds_fx_swap_slots (Up/Down reorder) lives here since it's this file's own
// buttons that call it -- see its own comment for why it only swaps FXSlotParams and never touches
// FXSlotEngine.
// ---------------------------------------------------------------------------------------------

// Slot reorder (Up/Down arrows): swaps two neighboring pool slots' whole FXSlotParams value --
// type/params/mix/bypass/source/chain/sidechain all move together. FXSlotEngine is deliberately
// NOT touched: sync_fx_engine_types() already
// detects a type change at each index next block and reallocates accordingly, the same natural
// reset an ordinary Type change already goes through -- there is nothing reorder-specific to do.
// The caller (hds_fx_gui_on_button_press) is responsible for checking
// hds_fx_slot_participates_in_chain() first -- this function itself has no chain awareness.
static void hds_fx_swap_slots(HdsFxGuiHost *p, int a, int b) {
    if (a < 0 || b < 0 || a >= FX_SLOT_COUNT || b >= FX_SLOT_COUNT || a == b) return;
    FXSlotParams tmp = p->fx[a];
    p->fx[a] = p->fx[b];
    p->fx[b] = tmp;
}


// Returns true if the click was consumed. If the file browser is open, this ONLY interacts with
// it (see hds_fx_gui_browser_on_click()'s own comment) -- checked before the dropdown state below,
// since the browser is the more modal of the two (though in practice they can never both be open
// at once: nothing that opens one is reachable while the other is showing). Otherwise, if a
// dropdown is open, this ONLY interacts with it (selecting an option if the click landed inside
// it, closing it either way) -- a click can never both close one dropdown and immediately act on
// whatever's underneath it. Otherwise: Type/Input/Sidechain/Output pills OPEN their own dropdown
// (see the "Dropdowns" section above) rather than mutating anything directly; a NAM Model/IR pill
// opens the file browser (see HdsFxBrowser's own comment); bypass/chain/up/down toggle
// immediately; a knob starts a drag tracked in *vs instead.
static bool hds_fx_gui_on_button_press(HdsFxGuiHost *p, HdsFxGuiViewState *vs, double x, double y, int button) {
    int width = vs->window_w;

    if (vs->browser.open) {
        if (button == 1) return hds_fx_gui_browser_on_click(vs, x, y);
        return true; // right-click etc. while the browser is open -- still fully consumed
    }

    if (vs->dropdown.kind != HDS_FX_DROPDOWN_NONE) {
        int first, last;
        hds_fx_dropdown_visible_range(p, &vs->dropdown, vs->window_h, &first, &last);
        for (int i = first; i < last; i++) {
            HdsFxRect r;
            hds_fx_dropdown_option_rect(p, &vs->dropdown, vs->window_h, i, &r);
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
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_TYPE, row, L.type_pill);
        return true;
    }
    if (p->port_input_stereo && hds_fx_slot_is_chain_head(p, row) && hds_fx_rect_contains(&L.input_pill, x, y)) {
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_INPUT, row, L.input_pill);
        return true;
    }
    if (!p->port_input_stereo && p->source_count > 0 && hds_fx_slot_is_chain_head(p, row) && hds_fx_rect_contains(&L.input_pill, x, y)) {
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_SOURCE, row, L.input_pill);
        return true;
    }
    if (hds_fx_type_uses_sidechain((int)p->fx[row].fx_type) && hds_fx_rect_contains(&L.sidechain_pill, x, y)) {
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_SIDECHAIN, row, L.sidechain_pill);
        return true;
    }
    // On-Air Override's own menu -- RIGHT-click only (button==1 on this same rect means something
    // else entirely: hold-to-engage -- see the per-param loop's own i==5 handling just below).
    // Reachable regardless of the button's own current Disabled state, since this is the only way
    // to turn Disabled back off.
    if (button == 3 && hds_fx_type_is_broadcast_delay((int)p->fx[row].fx_type)) {
        HdsFxRect onair_btn;
        hds_fx_bcastdly_dump_btn_rect(hds_fx_bcastdly_col_x(L.param_knob[0].x, 5), row_y, &onair_btn);
        if (hds_fx_rect_contains(&onair_btn, x, y)) {
            hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_ONAIR_MODE, row, onair_btn);
            return true;
        }
    }
    if (hds_fx_type_uses_nam_paths((int)p->fx[row].fx_type)) {
        if (hds_fx_rect_contains(&L.nam_model_pill, x, y)) { hds_fx_gui_browser_open(vs, ".nam", "Select NAM Model (*.nam)", p->fx[row].nam_model_path, SAMPLE_PATH_LEN); return true; }
        if (hds_fx_rect_contains(&L.nam_ir_pill, x, y)) { hds_fx_gui_browser_open(vs, ".wav", "Select Cab IR (*.wav)", p->fx[row].nam_ir_path, SAMPLE_PATH_LEN); return true; }
    }
    if (p->port_output_channel && hds_fx_slot_is_chain_tail(p, row) && hds_fx_rect_contains(&L.output_pill, x, y)) {
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_OUTPUT, row, L.output_pill);
        return true;
    }
    if (!p->port_output_channel && p->source_routing_flat && hds_fx_slot_is_chain_tail(p, row) && hds_fx_rect_contains(&L.output_pill, x, y)) {
        hds_fx_dropdown_open(&vs->dropdown, HDS_FX_DROPDOWN_OUT_PAIR, row, L.output_pill);
        return true;
    }
    if (hds_fx_rect_contains(&L.bypass_btn, x, y)) { if (button == 1) p->fx[row].bypass = (p->fx[row].bypass != 0.0f) ? 0.0f : 1.0f; return true; }
    if (hds_fx_rect_contains(&L.chain_btn, x, y)) {
        if (button == 1 && row < FX_SLOT_COUNT - 1) {
            bool turning_on = p->fx[row].chain_to_next == 0.0f;
            p->fx[row].chain_to_next = turning_on ? 1.0f : 0.0f;
            if (turning_on) {
                // A chain link only actually takes effect once the very next slot shares this
                // slot's own fx_source (see hds_fx_next_same_source_slot()'s own comment -- the
                // link is defined by ARRAY POSITION + matching fx_source, not by chain_to_next
                // alone) -- so without this, turning Chain-to-Next on here would silently do
                // nothing until the next row's own Input pill was ALSO clicked and set to match by
                // hand. Auto-match it here instead, so one click is enough. Clears any explicit
                // input_from_slot override on that row first -- it's about to become an ordinary
                // chain continuation, not a cross-slot link, and the two are mutually exclusive
                // (see hds_fx_slot_is_chain_head()'s own comment).
                p->fx[row + 1].input_from_slot = -1.0f;
                p->fx[row + 1].fx_source = p->fx[row].fx_source;
            }
        }
        return true;
    }
    if (hds_fx_rect_contains(&L.up_btn, x, y)) { if (button == 1 && !hds_fx_slot_participates_in_chain(p, row)) hds_fx_swap_slots(p, row, row - 1); return true; }
    if (hds_fx_rect_contains(&L.down_btn, x, y)) { if (button == 1 && !hds_fx_slot_participates_in_chain(p, row)) hds_fx_swap_slots(p, row, row + 1); return true; }

    if (button == 1) {
        int row_type = (int)p->fx[row].fx_type;
        int param_count = hds_fx_type_param_count(row_type);
        bool is_bcastdly = hds_fx_type_is_broadcast_delay(row_type);
        for (int i = 0; i < param_count; i++) {
            double col_x = is_bcastdly ? hds_fx_bcastdly_col_x(L.param_knob[0].x, i) : L.param_knob[i].x; // must mirror the draw side exactly -- see hds_fx_bcastdly_col_x()'s own comment
            // FX_TYPE_BROADCAST_DELAY's Dump 7s/15s/30s buttons: a plain click-to-fire, same
            // one-shot-counter idiom as SeqLaneState.audio_preview_trigger (see fx_types.h's own
            // comment on param[1..3]) -- increments the param, never starts a knob drag. Grayed out
            // (see hds_fx_draw_dump_button()'s own comment) AND inert whenever Delay (param[0]) is
            // 0 -- still consumes the click (so it can never fall through to whatever knob would
            // otherwise sit at this column), just doesn't fire.
            if (is_bcastdly && (i == 1 || i == 2 || i == 3)) {
                HdsFxRect btn;
                hds_fx_bcastdly_dump_btn_rect(col_x, row_y, &btn);
                if (hds_fx_rect_contains(&btn, x, y)) {
                    if (p->fx[row].param[0] > 0.0f) p->fx[row].param[i] += 1.0f;
                    return true;
                }
                continue;
            }
            // On-Air Override (param[5]): HELD, not clicked -- set to 1.0 for as long as the mouse
            // button stays down, same idiom hds_fx_gui_on_motion()'s own knob-drag uses (start state
            // here, real per-frame state lives in vs->tone_override_row so
            // hds_fx_gui_on_button_release() -- which gets no x/y of its own -- knows which row to
            // clear back to 0.0 on release). See run_fx_broadcastdelay()'s own comment for why this
            // has to be a live hold, not an edge-triggered one-shot like the Dump buttons above.
            // Mouse-hold works regardless of whether MIDI Trigger (param[8]) is also enabled --
            // the two are independent, additive gates, neither one takes the other away. Only
            // Disabled (param[7]) grays this out and makes it inert, same "still consumes the
            // click, just doesn't fire" convention the Dump buttons use at Delay=0.
            if (is_bcastdly && i == 5) {
                HdsFxRect btn;
                hds_fx_bcastdly_dump_btn_rect(col_x, row_y, &btn);
                if (hds_fx_rect_contains(&btn, x, y)) {
                    if (p->fx[row].param[7] == 0.0f) { p->fx[row].param[5] = 1.0f; vs->tone_override_row = row; }
                    return true;
                }
                continue;
            }
            HdsFxRect knob_rect = L.param_knob[i]; knob_rect.x = col_x;
            if (hds_fx_knob_contains(&knob_rect, x, y)) {
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

static void hds_fx_gui_on_motion(HdsFxGuiHost *p, HdsFxGuiViewState *vs, double x, double y) {
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
        // Range-aware: FX_TYPE_MULTIFILTER's Reso (idx 2) is stored 0-100, not the plain 0.0-1.0
        // every other param uses -- see hds_fx_type_param_range()'s own comment on the real bug
        // this fixes (that knob used to be clampable to only 1% of its own real range). A full
        // HDS_FX_DRAG_PIXELS_FULL_RANGE drag always spans the param's own full lo..hi either way.
        float lo, hi; hds_fx_type_param_range((int)p->fx[vs->drag_row].fx_type, vs->drag_param_index, &lo, &hi);
        float v = vs->drag_start_value + (float)(delta / HDS_FX_DRAG_PIXELS_FULL_RANGE) * (hi - lo);
        if (v < lo) v = lo; else if (v > hi) v = hi;
        p->fx[vs->drag_row].param[vs->drag_param_index] = v;
    }
}

static void hds_fx_gui_on_button_release(HdsFxGuiHost *p, HdsFxGuiViewState *vs) {
    vs->drag_kind = 0;
    // On-Air Override (param[5]) is HELD -- see hds_fx_gui_on_button_press()'s own comment on this
    // button. Unconditional and independent of where the mouse actually is now (it may have moved
    // off the row, or even off the window, while still held) -- the button is released the instant
    // the mouse button comes up, full stop, same as a real momentary switch.
    if (vs->tone_override_row >= 0) {
        p->fx[vs->tone_override_row].param[5] = 0.0f;
        vs->tone_override_row = -1;
    }
}

// Named key actions the platform shim (hds_fx_gui_x11.c) translates a real X11 KeyPress into --
// keeps this file free of any X11 type, the same "zero windowing references" discipline this
// file's own header comment already holds every other piece of drawing/hit-test logic to. A no-op
// unless the file browser's own path bar is being typed into (see HdsFxBrowser's own
// path_bar_editing field) -- this GUI has exactly one thing keyboard input can ever reach.
#define HDS_FX_KEY_CHAR 0      // a printable character in `ch`
#define HDS_FX_KEY_BACKSPACE 1
#define HDS_FX_KEY_ENTER 2     // tries the typed text as an exact file first, then as a directory
#define HDS_FX_KEY_ESCAPE 3    // discards the typed text, reverting to the last dir that listed OK

static void hds_fx_gui_on_key(HdsFxGuiHost *p, HdsFxGuiViewState *vs, int action, char ch) {
    (void)p; // no longer needed here -- a committed path bar entry is applied by the platform
             // shim's own hds_fx_gui_browser_refresh() (see path_bar_commit_attempt's own comment),
             // not directly from this keystroke handler.
    HdsFxBrowser *b = &vs->browser;
    if (!b->open || !b->path_bar_editing) return;
    size_t len = strlen(b->dir);
    switch (action) {
        case HDS_FX_KEY_CHAR:
            // Printable ASCII only (0x20..0x7e) -- a file path never needs anything outside that
            // range, and this sidesteps ever having to reason about multi-byte UTF-8 XLookupString
            // can hand back for a real IME/dead-key sequence.
            if (ch >= 0x20 && ch < 0x7f && len + 1 < sizeof(b->dir)) {
                b->dir[len] = ch;
                b->dir[len + 1] = '\0';
            }
            return;
        case HDS_FX_KEY_BACKSPACE:
            if (len > 0) b->dir[len - 1] = '\0';
            return;
        case HDS_FX_KEY_ENTER:
            b->path_bar_editing = false;
            b->path_bar_commit_attempt = true;
            b->needs_refresh = true;
            return;
        case HDS_FX_KEY_ESCAPE:
            b->path_bar_editing = false;
            snprintf(b->dir, sizeof(b->dir), "%s", b->last_valid_dir);
            return;
        default:
            return;
    }
}

// delta: +1 (wheel up / X11 Button4) or -1 (wheel down / X11 Button5).
static void hds_fx_gui_on_scroll(const HdsFxGuiHost *p, HdsFxGuiViewState *vs, int delta) {
    if (vs->browser.open) {
        HdsFxBrowserLayout L;
        hds_fx_browser_compute_layout(vs, &L);
        vs->browser.scroll -= delta;
        int max_start = vs->browser.entry_count - L.visible_rows;
        if (max_start < 0) max_start = 0;
        if (vs->browser.scroll < 0) vs->browser.scroll = 0;
        if (vs->browser.scroll > max_start) vs->browser.scroll = max_start;
        return;
    }
    // An open dropdown's own list scrolls instead of the row list behind it -- its anchor rect
    // stays exactly where it was opened (only the list CONTENT inside moves), so there's no longer
    // a "floating, detached from its row" problem the way there would be if scrolling instead
    // moved the anchor itself. Standard "a scroll while a modal list is open scrolls THAT list"
    // convention (an HTML <select>, a native combobox, ...) -- never falls through to scrolling
    // whatever's behind it.
    if (vs->dropdown.kind != HDS_FX_DROPDOWN_NONE) {
        int count = hds_fx_dropdown_option_count(p, vs->dropdown.kind);
        int visible_rows = hds_fx_dropdown_visible_rows(count, vs->window_h);
        int max_start = count - visible_rows;
        if (max_start < 0) max_start = 0;
        vs->dropdown.scroll -= delta;
        if (vs->dropdown.scroll < 0) vs->dropdown.scroll = 0;
        if (vs->dropdown.scroll > max_start) vs->dropdown.scroll = max_start;
        return;
    }
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
    vs->dropdown.kind = HDS_FX_DROPDOWN_NONE; // same reasoning as hds_fx_gui_on_scroll's own comment
}
