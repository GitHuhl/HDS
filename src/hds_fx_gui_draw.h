// hds_fx_gui_draw.h -- interface of the FX pool GUI (hds_fx_gui_draw.c): the Cairo drawing,
// hit-testing and input handling for an FX_SLOT pool, plus the small drawing helpers (text, pills,
// knob arcs, colors) that the synth GUI's tab files reuse. Platform-free: the X11/Win32 shims
// own the window and call into these.
#ifndef HDS_FX_GUI_DRAW_H
#define HDS_FX_GUI_DRAW_H

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "fx_chain.h"
#include "hds_fx_gui_host.h"
#include "hds_fx_gui_browser.h"

// Every draw file calls cairo_select_font_face(); on Android this routes to the bundled fonts
// (see hds_font_select_face's comment in hds_fx_gui_draw.c), elsewhere it calls straight through.
void hds_font_select_face(cairo_t *cr, const char *family, cairo_font_slant_t slant, cairo_font_weight_t weight);
#define cairo_select_font_face hds_font_select_face

#define HDS_FX_GUI_MIN_W 740
#define HDS_FX_GUI_MIN_H 380

typedef struct { double x, y, w, h; } HdsFxRect;

typedef struct {
    int kind;         // one of HDS_FX_DROPDOWN_* above; NONE when closed
    int row;          // which pool slot this dropdown belongs to
    HdsFxRect anchor; // the pill's own rect at the moment it was opened -- positions the list
    int scroll;       // index of the first VISIBLE option -- see hds_fx_dropdown_visible_rows()'s
                        // own comment. Always reset to 0 by hds_fx_dropdown_open() below; nothing
                        // else should ever need to touch this field directly.
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

// Named key actions the platform shim (hds_fx_gui_x11.c) translates a real X11 KeyPress into --
// keeps this file free of any X11 type, the same "zero windowing references" discipline this
// file's own header comment already holds every other piece of drawing/hit-test logic to. A no-op
// unless the file browser's own path bar is being typed into (see HdsFxBrowser's own
// path_bar_editing field) -- this GUI has exactly one thing keyboard input can ever reach.
#define HDS_FX_KEY_CHAR 0      // a printable character in `ch`
#define HDS_FX_KEY_BACKSPACE 1
#define HDS_FX_KEY_ENTER 2     // tries the typed text as an exact file first, then as a directory
#define HDS_FX_KEY_ESCAPE 3    // discards the typed text, reverting to the last dir that listed OK

void hds_fx_gui_view_state_init(HdsFxGuiViewState *vs);
void hds_fx_rgb(cairo_t *cr, double r, double g, double b);
void hds_fx_rgba(cairo_t *cr, double r, double g, double b, double a);
void hds_fx_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
void hds_fx_centered_text(cairo_t *cr, double cx, double y, const char *text);
void hds_fx_left_text(cairo_t *cr, double x, double y, const char *text);
void hds_fx_draw_pill(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled);
void hds_fx_draw_mute_pill(cairo_t *cr, double x, double y, double w, double h, const char *text, bool active, bool disabled);
void hds_fx_draw_knob_arc(cairo_t *cr, double cx, double cy, double radius, float value01, bool disabled);
void hds_fx_gui_browser_open(HdsFxGuiViewState *vs, const char *ext, const char *title, char *dest, size_t dest_len);
void hds_fx_gui_browser_open_folder(HdsFxGuiViewState *vs, const char *title, char *dest, size_t dest_len);
void hds_fx_gui_render_browser(cairo_t *cr, const HdsFxGuiViewState *vs);
void hds_fx_gui_render(cairo_t *cr, HdsFxGuiHost *p, const HdsFxGuiViewState *vs);
bool hds_fx_gui_on_button_press(HdsFxGuiHost *p, HdsFxGuiViewState *vs, double x, double y, int button);
void hds_fx_gui_on_motion(HdsFxGuiHost *p, HdsFxGuiViewState *vs, double x, double y);
void hds_fx_gui_on_button_release(HdsFxGuiHost *p, HdsFxGuiViewState *vs);
void hds_fx_gui_on_key(HdsFxGuiHost *p, HdsFxGuiViewState *vs, int action, char ch);
void hds_fx_gui_on_scroll(const HdsFxGuiHost *p, HdsFxGuiViewState *vs, int delta);
void hds_fx_gui_on_resize(const HdsFxGuiHost *p, HdsFxGuiViewState *vs, int w, int h);

#endif // HDS_FX_GUI_DRAW_H
