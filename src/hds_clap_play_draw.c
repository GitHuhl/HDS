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
          *pitch, *mono_noise, *vel_to_amp, *curve_drive;
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
            break;
        case 1:
            f->shape = &prog->vco2_shape; f->vol = &prog->vco2_vol; f->pan = &prog->vco2_pan;
            f->mute = &prog->vco2_mute; f->fine_tune = &prog->vco2_fine_tune; f->pitch_offset = &prog->vco2_pitch_offset;
            f->phase_offset = &prog->vco2_phase_offset; f->pulse_width = &prog->vco2_pulse_width;
            f->unison_detune = &prog->vco2_unison_saw_detune; f->unison_width = &prog->vco2_unison_saw_width;
            f->pitch = &prog->vco2_pitch; f->mono_noise = NULL;
            f->vel_to_amp = &prog->vco2_vel_to_amp; f->curve_drive = &prog->vco2_curve_drive;
            break;
        case 2:
            f->shape = &prog->vco3_shape; f->vol = &prog->vco3_vol; f->pan = &prog->vco3_pan;
            f->mute = &prog->vco3_mute; f->fine_tune = &prog->vco3_fine_tune; f->pitch_offset = &prog->vco3_pitch_offset;
            f->phase_offset = &prog->vco3_phase_offset; f->pulse_width = &prog->vco3_pulse_width;
            f->unison_detune = &prog->vco3_unison_saw_detune; f->unison_width = &prog->vco3_unison_saw_width;
            f->pitch = &prog->vco3_pitch; f->mono_noise = &prog->vco3_mono_noise;
            f->vel_to_amp = &prog->vco3_vel_to_amp; f->curve_drive = &prog->vco3_curve_drive;
            break;
        default: // case 3
            f->shape = &prog->vco4_shape; f->vol = &prog->vco4_vol; f->pan = &prog->vco4_pan;
            f->mute = &prog->vco4_mute; f->fine_tune = &prog->vco4_fine_tune; f->pitch_offset = &prog->vco4_pitch_offset;
            f->phase_offset = &prog->vco4_phase_offset; f->pulse_width = &prog->vco4_pulse_width;
            f->unison_detune = &prog->vco4_unison_saw_detune; f->unison_width = &prog->vco4_unison_saw_width;
            f->pitch = &prog->vco4_pitch; f->mono_noise = &prog->vco4_mono_noise;
            f->vel_to_amp = &prog->vco4_vel_to_amp; f->curve_drive = &prog->vco4_curve_drive;
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
    "Saw", "Square", "Triangle", "Sine", "Unison Saw", "Noise", "Pulse", "Sample", "Off", "Rev Saw",
};
#define HDS_PLAY_EGMODE_COUNT 7
static const char *const HDS_PLAY_EGMODE_NAMES[HDS_PLAY_EGMODE_COUNT] = {
    "ADSR", "AR", "AD", "ASR", "AHD", "DADSR", "AHDSR",
};

#define HDS_PLAY_DROPDOWN_NONE 0
#define HDS_PLAY_DROPDOWN_SHAPE 1
#define HDS_PLAY_DROPDOWN_EGMODE 2

typedef struct {
    float *target;      // knob currently being dragged, NULL = none
    double lo, hi;       // that knob's own real backend-unit range
    double start_y, start_val;
} HdsPlayKnobDrag;

typedef struct {
    int dropdown_kind;         // HDS_PLAY_DROPDOWN_* -- one at a time, same convention as the FX tab's own
    int dropdown_index;        // which oscillator (0-3, SHAPE) or which Amp EG (0-3, EGMODE)
    HdsFxRect dropdown_anchor;

    HdsPlayKnobDrag knob_drag;

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
#define HDS_PLAY_MARGIN 8.0
// 130, not the original 92: each Amp EG panel below shares this same column width (see
// hds_play_osc_col_x()/hds_play_render_eg_panel()'s own call site), and the widest topologies
// (DADSR/AHDSR) show 5 dials at once -- 92px left them scrunched together with overlapping labels.
#define HDS_PLAY_OSC_COL_W 130.0
#define HDS_PLAY_KNOB_R 9.0
#define HDS_PLAY_KNOB_STRIDE 40.0 // vertical space one knob (label+arc+value) reserves
#define HDS_PLAY_PILL_H 16.0
#define HDS_PLAY_CHECK_H 13.0
// 46.0, not 30.0: leaves an 8px-margin, 16px-tall strip at the very top of the tab for the Mode
// toggle (Single/Combi, see hds_clap_play_mode_toggle_rects()/hds_clap_gui_draw.c's own
// HDS_CLAP_TAB_PLAY dispatch, which draws/hit-tests it and switches to hds_clap_combi_draw.c's own
// 8-lane mixer view when Combi is selected) above every oscillator/EG section header.
#define HDS_PLAY_CONTENT_TOP 46.0 // below this file's own "Oscillators"/"Amp EGs" section labels

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

#define HDS_PLAY_EG_CURVE_H 110.0

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
    out->title = (HdsFxRect){col_x, top_y, col_w, 18.0};
    if (eg_index == 0) {
        out->sync_all_btn = (HdsFxRect){col_x + col_w - 66.0, top_y + 1.0, 30.0, 16.0};
        out->sync_live_btn = (HdsFxRect){col_x + col_w - 33.0, top_y + 1.0, 30.0, 16.0};
    } else {
        out->vel_to_amp_knob = (HdsFxRect){col_x + col_w - 16.0, top_y + HDS_PLAY_KNOB_R + 1.0, HDS_PLAY_KNOB_R, HDS_PLAY_KNOB_R};
    }
    double y = top_y + 20.0;
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
        cairo_set_font_size(cr, 7.0);
        hds_fx_rgb(cr, disabled ? 0.38 : 0.70, disabled ? 0.38 : 0.72, disabled ? 0.40 : 0.74);
        hds_fx_centered_text(cr, cx, cy - radius - 6.0, label);
    }
    hds_fx_draw_knob_arc(cr, cx, cy, radius, value01, disabled);
    if (value_text && value_text[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7.0);
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
static void hds_play_start_knob_drag(HdsPlayKnobDrag *drag, float *target, double lo, double hi, double y) {
    if (!target) return;
    drag->target = target;
    drag->lo = lo; drag->hi = hi;
    drag->start_y = y;
    drag->start_val = *target;
}

static bool hds_play_try_osc_knob_click(HdsClapPlayViewState *vs, const HdsFxRect *r, float *target, HdsPlayOscKnobId id, int shape, double x, double y) {
    if (!target || !hds_play_knob_hit_test(r, x, y)) return false;
    double lo, hi; const char *label; (void)label;
    hds_play_osc_knob_info(id, shape, &lo, &hi, &label);
    hds_play_start_knob_drag(&vs->knob_drag, target, lo, hi, y);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Dropdown lists -- Shape (10 options) and Amp EG Mode (7 options). Same "open list positioned
// off the anchor pill, one at a time" idiom hds_fx_gui_draw.c's own dropdown already uses, kept as
// this file's own small version rather than sharing that one directly: the option sets, and what
// dropdown_index means (an oscillator vs. an Amp EG), are entirely different domains here.
// ---------------------------------------------------------------------------------------------
#define HDS_PLAY_DROPDOWN_ROW_H 16.0

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
    cairo_set_font_size(cr, 9.0);
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
#define HDS_PLAY_OSC_GAP 14.0 // widened from the original 4 -- clearer separation between strips
#define HDS_PLAY_REGION_GAP 16.0 // vertical gap between the oscillator region and the EG region below it

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
    cairo_set_font_size(cr, 9.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    char hdr[16]; snprintf(hdr, sizeof(hdr), "Osc %s", num);
    hds_fx_left_text(cr, col_x, HDS_PLAY_CONTENT_TOP - 8.0, hdr);

    bool shape_dd_open = (vs->dropdown_kind == HDS_PLAY_DROPDOWN_SHAPE && vs->dropdown_index == osc);
    hds_fx_draw_pill(cr, L.shape_pill.x, L.shape_pill.y, L.shape_pill.w, L.shape_pill.h,
                      HDS_PLAY_SHAPE_NAMES[shape >= 0 && shape < HDS_PLAY_SHAPE_COUNT ? shape : 0],
                      shape_dd_open, false);

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
    hds_fx_draw_pill(cr, L.mute_btn.x, L.mute_btn.y, L.mute_btn.w, L.mute_btn.h, "M", muted, false);
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
    cairo_set_font_size(cr, 9.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    char title[24]; snprintf(title, sizeof(title), "Osc %d Amp EG", eg_index + 1);
    hds_fx_left_text(cr, L.title.x, L.title.y + 12.0, title);

    bool disabled = (eg_index > 0 && vs->amp_eg_live_sync);
    if (eg_index == 0) {
        hds_fx_draw_pill(cr, L.sync_all_btn.x, L.sync_all_btn.y, L.sync_all_btn.w, L.sync_all_btn.h, "1->All", false, false); // plain ASCII, not the UTF-8 arrow -- see hds_clap_global_draw.c's own comment on why
        hds_fx_draw_pill(cr, L.sync_live_btn.x, L.sync_live_btn.y, L.sync_live_btn.w, L.sync_live_btn.h, "Sync", vs->amp_eg_live_sync, false);
    } else {
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

static void hds_clap_play_render(cairo_t *cr, HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double window_h) {
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
}

// ---------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------

// "1->All"/"Sync": one-shot and live copy of Amp EG 0 (Osc1)'s full state onto Amp EGs 1-3 --
// mirrors frontend.py's own _copy_amp_eg_to_all()/_mirror_amp_eg_live() exactly (topology mode +
// all 8 PackEGParams fields, nothing else).
static void hds_play_copy_eg0_to_all(ProgramState *prog) {
    for (int i = 1; i < 4; i++) {
        prog->eg[i] = prog->eg[0];
        prog->eg_mode[i] = prog->eg_mode[0];
    }
}

static bool hds_clap_play_on_button_press(HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);

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
        if (L.mono_chk.w > 0 && hds_play_rect_contains(&L.mono_chk, x, y)) {
            if (button == 1 && f.mono_noise) *f.mono_noise = (*f.mono_noise != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_rect_contains(&L.mute_btn, x, y)) {
            if (button == 1) *f.mute = (*f.mute != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_try_osc_knob_click(vs, &L.pulse_knob, f.pulse_width, HDS_PLAY_KNOB_PULSE, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.curve_knob, f.curve_drive, HDS_PLAY_KNOB_CURVE, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pitch_off_knob, f.pitch_offset, HDS_PLAY_KNOB_PITCH_OFFSET, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.fine_knob, f.fine_tune, HDS_PLAY_KNOB_FINE, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.phase_knob, f.phase_offset, HDS_PLAY_KNOB_PHASE, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pitch_knob, f.pitch, HDS_PLAY_KNOB_PITCH, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.uni_width_knob, f.unison_width, HDS_PLAY_KNOB_UNI_WIDTH, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.uni_detune_knob, f.unison_detune, HDS_PLAY_KNOB_UNI_DETUNE, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.vol_knob, f.vol, HDS_PLAY_KNOB_VOL, shape, x, y)) return true;
        if (hds_play_try_osc_knob_click(vs, &L.pan_knob, f.pan, HDS_PLAY_KNOB_PAN, shape, x, y)) return true;
    }

    (void)window_w;
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
        } else if (!disabled) {
            HdsPlayOscFields f; hds_play_osc_fields(prog, i, &f);
            if (hds_play_try_osc_knob_click(vs, &L.vel_to_amp_knob, f.vel_to_amp, HDS_PLAY_KNOB_VEL_TO_AMP, 0, x, y)) return true;
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
                    hds_play_start_knob_drag(&vs->knob_drag, target, HDS_PLAY_EG_LO[field], HDS_PLAY_EG_HI[field], y);
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
        double delta = vs->knob_drag.start_y - y; // up = increase, matching every knob in this GUI
        double v = vs->knob_drag.start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (vs->knob_drag.hi - vs->knob_drag.lo);
        if (v < vs->knob_drag.lo) v = vs->knob_drag.lo; else if (v > vs->knob_drag.hi) v = vs->knob_drag.hi;
        *vs->knob_drag.target = (float)v;
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
    vs->knob_drag.target = NULL;
    vs->adsr_drag_eg = -1;
}
