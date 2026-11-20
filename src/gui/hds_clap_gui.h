// hds_clap_gui.h -- interface of the synth's native GUI (the hds_clap_*_draw.c tab files plus the
// tab-bar dispatcher hds_clap_gui_draw.c), shared by hds.clap and synth_engine --gui. Each tab file
// exports its view-state type and init/render/input functions to hds_clap_gui_draw.c; the Play tab
// also exports the knob, context-menu and destination-list widgets the other tabs reuse; and
// hds_clap_gui_draw.c exports HdsClapGuiViewState and the hds_clap_gui_* entry points the platform
// window code (hds_clap_gui_x11.c/_win32.c, hds_standalone_gui_x11.c) calls.
//
// HDS_STANDALONE_GUI_ENABLED changes some view-state layouts (Devices/Routing/Recording tabs), so
// every file including this must be compiled with the same setting as the objects it links with --
// the Makefile builds those tab files once per host for exactly that reason.
#ifndef HDS_CLAP_GUI_H
#define HDS_CLAP_GUI_H

#include <cairo/cairo.h>
#include <stdbool.h>

#include "core/backend.h"
#include "gui/hds_engine_instance.h"
#include "gui/hds_fx_gui_draw.h"
#ifdef HDS_STANDALONE_GUI_ENABLED
#include "targets/standalone/hds_standalone_devices.h"
#endif

// --- hds_clap_play_draw.c ---
ProgramState *hds_play_program(HdsClapPlugin *p);
#define HDS_PLAY_EGMODE_COUNT 7
// A knob's own right-click menu, if it has one -- stashed at right-press time (see
// hds_play_start_knob_drag()) and consulted on release if the button never actually dragged.
// kind==0 means "no menu": most knobs don't have one -- it's only wired onto destination-capable
// knobs (oscillator Pitch/Phase/Pulse-Width/Fine/Curve/Volume/Pan, Filter Cutoff/Resonance, LFO
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

    // Right-click support: button is 0 (idle), 1 (left, dragging at normal
    // sensitivity) or 3 (right -- either still a "was this a click or a drag" pending gesture, or
    // promoted to a fine-sensitivity drag once the pointer moves past the threshold). press_x/y is
    // where the right button went down; right_moved flips true the first time the pointer clears
    // HDS_PLAY_RIGHT_CLICK_THRESHOLD_PX from there.
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
void hds_ctxmenu_render(cairo_t *cr, const HdsCtxMenu *m, const HdsCtxMenuRow *rows, int count, double win_w, double win_h);
// The 100-entry Destination list -- shared by every LFO row's
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
void hds_lfo_dest_label(int target, char *buf, size_t buflen);
int hds_lfo_rate_dest_target(int idx);
int hds_lfo_phase_dest_target(int idx);
#define HDS_DEST_LIST_ROW_H 17.0
#define HDS_DEST_LIST_MAX_H 340.0
void hds_dest_list_render(cairo_t *cr, const HdsFxRect *bg, double *scroll_px, int current);
bool hds_dest_list_hit_test(const HdsFxRect *bg, double scroll_px, double x, double y, int *out_target);
#define HDS_CTXMENU_KIND_MODULATE_FROM 1
#define HDS_CTXMENU_KIND_LFO_DEPTH 2
#define HDS_CTXMENU_KIND_FILTER_CUTOFF 3
void hds_play_build_ctxmenu_rows(HdsClapPlugin *p, const HdsCtxMenu *menu, HdsCtxMenuRow *rows, int *count);
bool hds_play_ctxmenu_handle_click(HdsClapPlugin *p, HdsCtxMenu *menu, double win_w, double win_h, double x, double y);
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

    // This tab's own vertical scroll -- stays permanently 0.0 (and so, by construction, changes
    // nothing at all) on any host tall enough to show hds_play_content_h()'s own full content at
    // once, which every desktop default window size already is; only needed once a host's own
    // window got shorter than that (Android, see hds_android_gui_scroll_by_px()'s own PLAY case),
    // which this file had never needed to support until then. Same convention every other
    // scrollable tab here already uses (subtract from the layout's own fixed top-of-content
    // constant, see every HDS_PLAY_CONTENT_TOP/hds_play_eg_top_y() call site in this file) rather
    // than a cairo_translate(), so render and every hit-test function agree on row positions by
    // construction, never by keeping two copies of the same arithmetic in sync.
    double scroll_offset_px;

    // Plucked String editor panel: which oscillator's string it's editing (-1 = closed). Opened
    // by that oscillator's "String Edit" button; drawn over the whole tab while open.
    int string_edit_osc;
} HdsClapPlayViewState;
void hds_clap_play_view_state_init(HdsClapPlayViewState *vs);
// 46.0, not 30.0: leaves an 8px-margin, 16px-tall strip at the very top of the tab for the Mode
// toggle (Single/Combi, see hds_clap_play_mode_toggle_rects()/hds_clap_gui_draw.c's own
// HDS_CLAP_TAB_PLAY dispatch, which draws/hit-tests it and switches to hds_clap_combi_draw.c's own
// 8-lane mixer view when Combi is selected) above every oscillator/EG section header.
#define HDS_PLAY_CONTENT_TOP 52.0 // below this file's own "Oscillators"/"Amp EGs" section labels
void hds_clap_play_mode_toggle_rects(HdsFxRect *single_r, HdsFxRect *combi_r);
#define HDS_PLAY_EG_FIELD_S   4
int hds_play_eg_dial_count(int env_type);
float *hds_play_eg_dial_target(PackEGParams *eg, int i);
void hds_play_draw_knob(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled, const char *label, const char *value_text);
bool hds_play_rect_contains(const HdsFxRect *r, double x, double y);
bool hds_play_knob_hit_test(const HdsFxRect *r, double x, double y);
// Generic across every tab that has a click-and-drag knob (not just Play's own oscillator/EG
// knobs) -- takes the HdsPlayKnobDrag descriptor directly rather than a whole tab's own view
// state, so a later tab file (e.g. hds_clap_filters_draw.c) can reuse it verbatim against its own
// view state's own drag field, the same way it already reuses hds_play_draw_knob()/
// hds_play_knob_hit_test().
#define HDS_PLAY_NO_MENU ((HdsPlayKnobMenu){0, 0})
void hds_play_start_knob_drag(HdsPlayKnobDrag *drag, float *target, double lo, double hi, double x, double y, int button, HdsPlayKnobMenu menu);
void hds_play_apply_knob_drag(HdsPlayKnobDrag *drag, double x, double y);
HdsPlayKnobMenu hds_play_release_knob_drag(HdsPlayKnobDrag *drag, double *out_anchor_x, double *out_anchor_y);
void hds_clap_play_render(cairo_t *cr, HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double window_h);
bool hds_clap_play_on_button_press(HdsClapPlugin *p, HdsClapPlayViewState *vs, HdsFxGuiViewState *fxvs, double window_w, double window_h, double x, double y, int button);
void hds_clap_play_on_motion(HdsClapPlugin *p, HdsClapPlayViewState *vs, double window_w, double x, double y);
void hds_clap_play_on_button_release(HdsClapPlayViewState *vs);
void hds_clap_play_on_scroll(HdsClapPlayViewState *vs, ProgramState *prog, double window_h, int delta);

// Envelope-generator tables shared by the Play, Envelopes and Global tabs (defined in hds_clap_play_draw.c).
extern const char *const HDS_PLAY_EGMODE_NAMES[HDS_PLAY_EGMODE_COUNT];
extern const int HDS_PLAY_EG_FIELD_ORDER[HDS_PLAY_EGMODE_COUNT][6];
extern const char *const HDS_PLAY_DIAL_LABELS[6];
extern const double HDS_PLAY_EG_LO[6];
extern const double HDS_PLAY_EG_HI[6];

// --- hds_clap_combi_draw.c ---
#define HDS_COMBI_PROGRAM_NAME_LEN 64
#define HDS_COMBI_PROGRAM_LIST_MAX 24 // see this file's own header comment on why no scroll
typedef struct {
    int dropdown_kind;   // HDS_COMBI_DROPDOWN_*
    int dropdown_lane;   // which lane (0..COMBI_PROGRAM_COUNT-1) the open dropdown belongs to
    HdsFxRect dropdown_anchor;

    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern

    char lane_program_label[COMBI_PROGRAM_COUNT][HDS_COMBI_PROGRAM_NAME_LEN]; // "" = "(none)"
    char program_list_cache[HDS_COMBI_PROGRAM_LIST_MAX][HDS_COMBI_PROGRAM_NAME_LEN];
    int program_list_count;
} HdsClapCombiViewState;
void hds_clap_combi_view_state_init(HdsClapCombiViewState *vs);
void hds_clap_combi_render(cairo_t *cr, HdsClapPlugin *p, HdsClapCombiViewState *vs, double window_w, double window_h);
bool hds_clap_combi_on_button_press(HdsClapPlugin *p, HdsClapCombiViewState *vs, double window_h, double x, double y, int button);
void hds_clap_combi_on_motion(HdsClapCombiViewState *vs, double x, double y);
void hds_clap_combi_on_button_release(HdsClapCombiViewState *vs);

// --- hds_clap_filters_draw.c ---
typedef struct {
    int dropdown_kind;   // HDS_FILT_DROPDOWN_* -- only Type ever opens a real option list (5 items);
                          // Chain Mode and Slope are plain binary click-to-toggle pills instead (see
                          // this file's own header comment on why a 2-option control doesn't need one)
    int dropdown_index;  // which filter slot (0-4) the open Type dropdown belongs to
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag; // reused verbatim from hds_clap_play_draw.c -- see this file's own header comment
    HdsCtxMenu ctx_menu;       // Cutoff's/Resonance's own "Modulate From" menu, if one is open
    double scroll_offset_px;  // this tab's own vertical scroll -- same convention as hds_clap_lfos_draw.c's own identical field; 5 filter rows + the 3-band EQ section stack taller than a short window
} HdsClapFiltersViewState;
void hds_clap_filters_view_state_init(HdsClapFiltersViewState *vs);
void hds_clap_filters_render(cairo_t *cr, HdsClapPlugin *p, HdsClapFiltersViewState *vs, double window_w, double window_h);
bool hds_clap_filters_on_button_press(HdsClapPlugin *p, HdsClapFiltersViewState *vs, double window_w, double window_h, double x, double y, int button);
void hds_clap_filters_on_motion(HdsClapFiltersViewState *vs, double x, double y);
void hds_clap_filters_on_scroll(HdsClapFiltersViewState *vs, double window_h, int delta);
void hds_clap_filters_on_button_release(HdsClapFiltersViewState *vs);

// --- hds_clap_lfos_draw.c ---
typedef struct {
    int dropdown_kind;   // HDS_LFO_DROPDOWN_*
    int dropdown_row;    // which LFO (0-15) the open dropdown belongs to
    HdsFxRect dropdown_anchor;
    double dest_scroll_px; // the open Destination list's own internal scroll (reset whenever a NEW one opens)
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see this file's own header comment
    HdsCtxMenu ctx_menu;       // Rate's/Depth's/Phase's own "Modulate From" menu, if one is open
    double scroll_offset_px;  // the whole 16-row list's own vertical scroll, same convention as HdsFxGuiViewState's own
} HdsClapLfosViewState;
void hds_clap_lfos_view_state_init(HdsClapLfosViewState *vs);
void hds_clap_lfos_render(cairo_t *cr, HdsClapPlugin *p, HdsClapLfosViewState *vs, double window_w, double window_h);
bool hds_clap_lfos_on_button_press(HdsClapPlugin *p, HdsClapLfosViewState *vs, double window_w, double window_h, double x, double y, int button);
void hds_clap_lfos_on_motion(HdsClapLfosViewState *vs, double x, double y);
void hds_clap_lfos_on_button_release(HdsClapLfosViewState *vs);
void hds_clap_lfos_on_scroll(HdsClapLfosViewState *vs, double window_h, int delta);

// --- hds_clap_envelopes_draw.c ---
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
void hds_clap_envelopes_view_state_init(HdsClapEnvelopesViewState *vs);
void hds_clap_envelopes_render(cairo_t *cr, HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double window_h);
bool hds_clap_envelopes_on_button_press(HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double window_h, double x, double y, int button);
void hds_clap_envelopes_on_motion(HdsClapPlugin *p, HdsClapEnvelopesViewState *vs, double window_w, double x, double y);
void hds_clap_envelopes_on_button_release(HdsClapEnvelopesViewState *vs);
void hds_clap_envelopes_on_scroll(HdsClapEnvelopesViewState *vs, double window_h, int delta);

// --- hds_clap_routing_draw.c ---
typedef struct {
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
    double scroll_offset_px;   // whole tab's own vertical scroll (source rows + the Drum Bus Key
                                // Routing section below them) -- same convention as
                                // hds_clap_lfos_draw.c's own scroll_offset_px. Needed now that
                                // SOURCE_COUNT (41, since Drum Buses were appended -- see
                                // DRUM_BUS_COUNT's own comment in backend.c) plus the new section
                                // below it overflows any reasonable window height.
    int drum_bus_selected;     // 1..DRUM_BUS_COUNT -- which Drum Bus the Key Routing grid below
                                // currently shows/edits (see its own comment).
} HdsClapRoutingViewState;
void hds_clap_routing_view_state_init(HdsClapRoutingViewState *vs);
void hds_clap_routing_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRoutingViewState *vs, double window_w, double window_h);
bool hds_clap_routing_on_button_press(HdsClapPlugin *p, HdsClapRoutingViewState *vs, double x, double y, int button);
void hds_clap_routing_on_motion(HdsClapRoutingViewState *vs, double x, double y);
void hds_clap_routing_on_button_release(HdsClapRoutingViewState *vs);
void hds_clap_routing_on_scroll(HdsClapRoutingViewState *vs, double window_h, int delta);

// --- hds_clap_devices_draw.c ---
typedef struct {
    int dropdown_kind; // HDS_DEV_DROPDOWN_*
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
#ifdef HDS_STANDALONE_GUI_ENABLED
    // Scanned once at GUI startup and again on "Rescan Devices" -- a real subprocess spawn+read
    // (see hds_scan_alsa_devices()/hds_scan_midi_devices() in hds_standalone.c), never per-frame.
    bool devices_scanned;
    HdsDeviceList output_devices, input_devices, midi_devices;
#endif
} HdsClapDevicesViewState;
void hds_clap_devices_view_state_init(HdsClapDevicesViewState *vs);
void hds_clap_devices_render(cairo_t *cr, HdsClapPlugin *p, HdsClapDevicesViewState *vs, double window_w, double window_h);
bool hds_clap_devices_on_button_press(HdsClapPlugin *p, HdsClapDevicesViewState *vs, double window_h, double x, double y, int button);
void hds_clap_devices_on_motion(HdsClapDevicesViewState *vs, double x, double y);
void hds_clap_devices_on_button_release(HdsClapDevicesViewState *vs);

// --- hds_clap_global_draw.c ---
// Fixed width, not resizable independently: a permanent, constant-
// width column, not a panel that grows/shrinks with the rest of the window. hds_clap_gui_draw.c
// reads this same constant to know where the tab bar/content region starts.
#define HDS_CLAP_GLOBAL_SIDEBAR_W 160.0 // a hardcoded pixel width (not a percentage of window_w -- see this file's own header comment); every sub-element below is laid out as a fraction of this one constant, so this is the only place a new width needs to change
typedef struct {
    int dropdown_kind; // HDS_GLOBAL_DROPDOWN_*
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag;
    double scroll_offset_px; // the whole sidebar's own vertical scroll -- see hds_clap_lfos_draw.c's own identical field/convention; the stacked content is taller than a typical window
} HdsClapGlobalViewState;
void hds_clap_global_view_state_init(HdsClapGlobalViewState *vs);
void hds_global_section_label(cairo_t *cr, double x, double y, const char *text);
void hds_clap_global_render(cairo_t *cr, HdsClapPlugin *p, HdsClapGlobalViewState *vs, double window_w, double window_h);
bool hds_clap_global_on_button_press(HdsClapPlugin *p, HdsClapGlobalViewState *vs, HdsFxGuiViewState *fxvs, double window_h, double x, double y, int button);
void hds_clap_global_on_motion(HdsClapGlobalViewState *vs, double x, double y);
void hds_clap_global_on_button_release(HdsClapGlobalViewState *vs);
void hds_clap_global_on_scroll(HdsClapPlugin *p, HdsClapGlobalViewState *vs, double window_h, int delta);

// --- hds_clap_recording_draw.c ---
#if defined(HDS_STANDALONE_GUI_ENABLED) || defined(HDS_ANDROID_GUI_ENABLED)
typedef struct {
    bool folder_initialized; // lazy-inits p->record_folder to "~/HDS_Recordings" on first render -- see this file's own render function
    double scroll_offset_px;
} HdsClapRecordingViewState;
void hds_clap_recording_view_state_init(HdsClapRecordingViewState *vs);
void hds_clap_recording_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRecordingViewState *vs, double window_w, double window_h);
bool hds_clap_recording_on_button_press(HdsClapPlugin *p, HdsClapRecordingViewState *vs, HdsFxGuiViewState *fxvs, double x, double y, int button);
void hds_clap_recording_on_button_release(HdsClapRecordingViewState *vs);
void hds_clap_recording_on_motion(HdsClapRecordingViewState *vs, double x, double y);
void hds_clap_recording_on_scroll(HdsClapPlugin *p, HdsClapRecordingViewState *vs, double window_h, int delta);
#endif

// --- hds_clap_gui_draw.c ---
// 1080 (every tab's own original effective width) + HDS_CLAP_GLOBAL_SIDEBAR_W (260, the sidebar
// added alongside it) -- so no existing tab's own layout math needed to change to make room for
// the sidebar; it's purely additional space to its left. See this file's own header comment.
// Irrelevant on Android (HDS_ANDROID_GUI_ENABLED): the actual window size there always comes from
// the real device screen, never this constant -- left untouched rather than special-cased, since
// nothing on that path ever reads it.
#define HDS_CLAP_GUI_DEFAULT_W (1150 + (int)HDS_CLAP_GLOBAL_SIDEBAR_W)
#define HDS_CLAP_GUI_DEFAULT_H 760
#define HDS_CLAP_GUI_MIN_W (740 + (int)HDS_CLAP_GLOBAL_SIDEBAR_W)
#define HDS_CLAP_GUI_MIN_H 420
typedef enum {
    HDS_CLAP_TAB_PLAY = 0,
    HDS_CLAP_TAB_FILTERS,
    HDS_CLAP_TAB_LFOS,
    HDS_CLAP_TAB_ENVELOPES,
    HDS_CLAP_TAB_FX,
    HDS_CLAP_TAB_ROUTING,
    HDS_CLAP_TAB_DEVICES,
#if defined(HDS_STANDALONE_GUI_ENABLED) || defined(HDS_ANDROID_GUI_ENABLED)
    HDS_CLAP_TAB_RECORDING,
#endif
    HDS_CLAP_TAB_COUNT
} HdsClapTab;
typedef struct {
    int window_w, window_h;
    HdsClapTab active_tab;

    // The FX tab's own view state -- a genuine, nested HdsFxGuiViewState (same type
    // hds_fx.clap's own single-tab window uses directly), kept in sync with a SHRUNK window_h
    // (real window_h minus the tab bar) by hds_clap_gui_on_resize() so hds_fx_gui_render()'s own
    // background fill/clip rect never draws over or under the tab bar -- see this file's own
    // header comment.
    HdsFxGuiViewState fx_view;

    // The Play tab's own view state (hds_clap_play_draw.c) -- unlike fx_view, it doesn't need its
    // own window_w/window_h copy: every Play-tab function takes this HdsClapGuiViewState's own
    // window_w/window_h directly as plain parameters (see hds_clap_gui_render()'s own dispatch),
    // since hds_clap_play_draw.c has no equivalent of hds_fx_gui_render()'s single-struct-in
    // calling convention to match.
    HdsClapPlayViewState play_view;
    HdsClapCombiViewState combi_view; // the Play tab's own Combi-mode sub-view -- see hds_clap_combi_draw.c's own header comment and this file's own HDS_CLAP_TAB_PLAY dispatch for why it's a sibling of play_view, not nested inside it
    HdsClapFiltersViewState filters_view; // same convention as play_view -- see hds_clap_filters_draw.c's own header comment
    HdsClapLfosViewState lfos_view;       // same convention -- see hds_clap_lfos_draw.c's own header comment
    HdsClapEnvelopesViewState envelopes_view; // same convention -- see hds_clap_envelopes_draw.c's own header comment
    HdsClapRoutingViewState routing_view;     // same convention -- see hds_clap_routing_draw.c's own header comment
    HdsClapDevicesViewState devices_view;      // same convention -- see hds_clap_devices_draw.c's own header comment
    HdsClapGlobalViewState global_view;        // same convention -- see hds_clap_global_draw.c's own header comment
#if defined(HDS_STANDALONE_GUI_ENABLED) || defined(HDS_ANDROID_GUI_ENABLED)
    HdsClapRecordingViewState recording_view;  // same convention -- see hds_clap_recording_draw.c's own header comment (standalone-only tab)
#endif
} HdsClapGuiViewState;
void hds_clap_gui_view_state_init(HdsClapGuiViewState *vs);
void hds_clap_gui_render(cairo_t *cr, HdsClapPlugin *p, HdsClapGuiViewState *vs);
bool hds_clap_gui_on_button_press(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y, int button);
void hds_clap_gui_on_motion(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y);
void hds_clap_gui_on_button_release(HdsClapPlugin *p, HdsClapGuiViewState *vs);
void hds_clap_gui_on_key(HdsClapPlugin *p, HdsClapGuiViewState *vs, int action, char ch);
void hds_clap_gui_on_scroll(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y, int delta);
void hds_clap_gui_on_resize(HdsClapPlugin *p, HdsClapGuiViewState *vs, int w, int h);

#endif // HDS_CLAP_GUI_H
