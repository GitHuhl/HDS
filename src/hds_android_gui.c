// hds_android_gui.c: the Android port's own native GUI window -- the SAME Cairo-drawn tab bar
// plus persistent Global sidebar (hds_clap_gui_draw.c -- see that file's own header comment;
// not Android-specific at all, same shape as every other platform) that hds.clap already embeds
// in a DAW host and
// hds_standalone_gui_x11.c already opens as a top-level X11 window, now drawn straight into a
// SurfaceView's own ANativeWindow buffer instead -- see this file's own hds_android_gui_redraw()
// for exactly how "straight into" is literal here: no Java Bitmap, no intermediate surface of
// this file's own, no pixel copy across JNI. Mirrors hds_standalone_gui_x11.c's own structure
// (a dedicated pthread owning redraw/input, HdsClapPlugin as the shared context type -- see
// hds_engine_instance.h's own header comment) wherever Android's own APIs don't force a
// different shape; every real difference from that file is called out below.
//
// #include'd by hds_android.c, after hds_engine_instance.h and every hds_clap_*_draw.c/
// hds_fx_gui_draw.c/hds_clap_gui_draw.c tab file (needs HdsClapPlugin and HdsClapGuiViewState
// fully defined), itself gated end-to-end on HDS_ANDROID_GUI_ENABLED by that file's own #include.
//
// Differences from hds_standalone_gui_x11.c:
//   - No Xlib window of this file's own exists to draw into at all -- Android hands this file a
//     Surface (via SurfaceHolder.Callback, relayed through JNI as hds_android_gui_surface_created/
//     _changed/_destroyed below) whose lifetime this file does not control. The struct/fonts/view
//     state are allocated once, at engine-init time (hds_android_gui_init(), called from
//     hds_android_engine_init() in hds_android.c); the ANativeWindow itself, and this window's own
//     redraw thread, exist only BETWEEN a surface_created and the matching surface_destroyed --
//     exactly the lifetime SurfaceHolder.Callback already promises, and a real device can cycle
//     through that more than once per process (screen off/on, a multi-window resize) -- see
//     hds_android_gui_surface_destroyed()'s own comment on why it tears down only the window/
//     thread, never the struct itself.
//   - Pixel format: ANativeWindow's own RGBA_8888/RGBX_8888 buffer formats are R,G,B,A/X in
//     memory (confirmed: this is Android's own documented Bitmap.Config.ARGB_8888 byte order,
//     the same convention OpenGL's GL_RGBA/GL_UNSIGNED_BYTE uses), while Cairo's CAIRO_FORMAT_
//     ARGB32 is B,G,R,A in memory on a little-endian device (confirmed directly in this port's own
//     earlier research, via test_noFontconfig.c's own by-hand PPM writer, which has to permute B
//     and R explicitly for exactly this reason). There is no public ANativeWindow pixel format
//     matching Cairo's own byte order, so after Cairo draws directly into the SAME locked
//     ANativeWindow buffer (no separate surface, no copy -- the literal architecture asked for),
//     hds_android_gui_swap_rb_inplace() below does a single in-place R<->B byte swap over that one
//     buffer, in C, before ANativeWindow_unlockAndPost(). This is the one unavoidable translation
//     between two different pixel-format CONVENTIONS, not a second render pass, not a copy to new
//     memory, and not the Java-Bitmap "cobbling" this whole architecture exists to avoid.
//   - Touch input, not mouse+keyboard+wheel: Android's own touch callbacks
//     (hds_android_gui_touch_event, called via JNI from a SurfaceView.OnTouchListener on the UI
//     thread) and this window's own redraw thread are DIFFERENT threads (unlike X11, where one
//     thread already owned both event pump and redraw) -- so touch events are queued through a
//     lock-free SPSC ring, exactly the same proven shape hds_android.c's own MIDI ring already
//     uses for the analogous Binder-thread-vs-audio-thread handoff, and drained once per frame on
//     THIS window's own redraw thread, before hds_clap_gui_render(). This means every
//     hds_clap_gui_on_*() call below runs on one thread only (this window's own), same
//     "single-threaded against this struct" invariant hds_standalone_gui_x11.c's own thread
//     already has against g_standalone_gui_window.
//   - Scroll vs. parameter, by finger count AND dropdown state, not by heuristic: there is no
//     separate scroll-wheel input channel on a touchscreen the way X11's button 4/5 give
//     hds_standalone_gui_x11.c's own pump_events() for free, so a touch-move has to double as
//     both a drag channel. Explicit product direction (not a guess): ONE finger taps/drags a
//     parameter (hds_clap_gui_on_button_press/_on_motion/_on_button_release, exactly like a
//     mouse) UNLESS an open dropdown/popup list is directly under it, in which case that one
//     finger scrolls the list instead (there's no param to drag under an open dropdown anyway --
//     see hds_android_gui_dropdown_open()'s own comment); TWO OR MORE fingers always pan the
//     active tab's own scroll position regardless of what's underneath. Both paths write
//     straight into each tab's own existing float scroll field (scroll_offset_px/
//     dest_scroll_px -- see hds_android_gui_scroll_by_px()'s own comment), continuously and 1:1
//     with dragged pixels, not the discrete per-"click" hds_clap_gui_on_scroll(..., +-1) X11's
//     wheel drives (an earlier version of this file called that instead, which real on-device
//     testing showed feeling jumpy/stepped rather than smooth). See
//     hds_android_touch_ring_drain_and_dispatch()'s own comment for the sticky per-sequence
//     gesture_mode the finger-count half of this decision lives in. No per-tab hit-test math
//     changes at all -- same posture hds_clap_gui_sidebar_w()'s own comment in
//     hds_clap_gui_draw.c already takes
//     for "only WHERE/WHEN, never WHAT."
//   - Scale-to-fill width, UNIFORMLY, not stretch: every hds_clap_*_draw.c tab lays its own
//     content out at fixed, absolute pixel positions (confirmed on a real device: a correctly-
//     colored, correctly-drawn GUI confined to roughly the left third of the real screen, dead
//     space past its own right/bottom edge) -- rewriting every tab's own layout to be responsive
//     is a large, invasive change this port's own "minimal deviation" mandate argues against.
//     Instead the view's logical WIDTH is held fixed forever (HDS_ANDROID_GUI_LOGICAL_W) and
//     hds_android_gui_redraw() computes ONE scale factor, real buffer width / that fixed logical
//     width, applied identically on both axes (cairo_scale(cr, scale, scale)) -- a first version
//     of this scaled X and Y independently to also fill the real screen's own height, which
//     looked visibly wrong on a real device (circular knobs rendering as ellipses) and, by
//     definition, stretched less on X than a width-only scale would have, since a phone's
//     landscape aspect ratio is wider than this GUI's own ~1150x760 design ratio. The view's
//     logical HEIGHT is instead recomputed every frame as real buffer height / that SAME scale --
//     i.e. "however many logical pixels are actually visible at this scale," not a fixed value --
//     so a tab whose content needs more vertical room than that already scrolls via its own
//     existing on_scroll() machinery exactly as it would on a short desktop window, with nothing
//     tab-specific added for it. Touch coordinates are divided back down by this same single
//     scale factor before ever reaching hds_clap_gui_on_*(), so hit-testing stays correct at any
//     real screen size.
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cairo/cairo-ft.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <time.h>

#include "hds_fx_gui_browser.c" // hds_fx_gui_browser_refresh() -- reused verbatim, same as hds_standalone_gui_x11.c's own identical include

// -----------------------------------------------------------------------------------------------
// Fonts: the two DejaVu Sans faces embed_font.S bakes directly into this .so's own .rodata (see
// that file's own header comment) loaded here via plain in-memory FreeType, with ZERO Fontconfig
// involvement anywhere -- confirmed working against the real cairo_ft_font_face_create_for_ft_face
// API this exact way in this port's own earlier research (test_noFontconfig.c). hds_android_font_
// regular/bold are deliberately NOT `static`: hds_fx_gui_draw.c's own font-selection macro
// (hds_font_select_face(), #include'd into this same translation unit long before this file is)
// declares them `extern` at block scope with no prior file-scope declaration yet visible at that
// point -- which gives that extern reference EXTERNAL linkage (C11 6.2.2p4) -- so the real
// definitions here must also be external-linkage (plain, non-static) to bind to the same two
// objects, not two separate ones.
extern const unsigned char hds_font_dejavu_sans_regular_start[];
extern const unsigned char hds_font_dejavu_sans_regular_end[];
extern const unsigned char hds_font_dejavu_sans_bold_start[];
extern const unsigned char hds_font_dejavu_sans_bold_end[];

cairo_font_face_t *hds_android_font_regular = NULL;
cairo_font_face_t *hds_android_font_bold = NULL;

static FT_Library g_android_ft_library;
static FT_Face g_android_ft_face_regular;
static FT_Face g_android_ft_face_bold;

static bool hds_android_gui_fonts_init(void) {
    if (FT_Init_FreeType(&g_android_ft_library) != 0) {
        HDS_LOGW("gui: FT_Init_FreeType failed");
        return false;
    }
    FT_Long regular_size = (FT_Long)(hds_font_dejavu_sans_regular_end - hds_font_dejavu_sans_regular_start);
    if (FT_New_Memory_Face(g_android_ft_library, hds_font_dejavu_sans_regular_start, regular_size, 0, &g_android_ft_face_regular) != 0) {
        HDS_LOGW("gui: FT_New_Memory_Face (regular) failed");
        return false;
    }
    FT_Long bold_size = (FT_Long)(hds_font_dejavu_sans_bold_end - hds_font_dejavu_sans_bold_start);
    if (FT_New_Memory_Face(g_android_ft_library, hds_font_dejavu_sans_bold_start, bold_size, 0, &g_android_ft_face_bold) != 0) {
        HDS_LOGW("gui: FT_New_Memory_Face (bold) failed");
        return false;
    }
    hds_android_font_regular = cairo_ft_font_face_create_for_ft_face(g_android_ft_face_regular, 0);
    hds_android_font_bold = cairo_ft_font_face_create_for_ft_face(g_android_ft_face_bold, 0);
    if (cairo_font_face_status(hds_android_font_regular) != CAIRO_STATUS_SUCCESS ||
        cairo_font_face_status(hds_android_font_bold) != CAIRO_STATUS_SUCCESS) {
        HDS_LOGW("gui: cairo_ft_font_face_create_for_ft_face failed");
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------
// Touch input ring -- see this file's own header comment for why (UI thread producer, this
// window's own redraw thread consumer). Shape mirrors hds_android.c's own HdsMidiRingEvent/
// g_midi_ring exactly (same capacity reasoning: a real finger produces nowhere near 64 touch
// events between two ~33ms redraws).
//
// pointer_count carries one-finger-vs-multi-finger gesture separation (explicit product
// direction, not this port's own earlier guess): a SINGLE finger only ever taps/drags parameters
// (knobs, pills, tab switches) -- it never scrolls -- while TWO OR MORE fingers only ever pan the
// active tab's own scroll position, never touching a knob/param no matter where they land. x/y is
// the single pointer's own real-screen position when pointer_count==1, or the CENTROID (plain
// average) of every active pointer when pointer_count>=2 -- computed once in
// HdsSurfaceView.java's own onTouch() (far simpler there, with full MotionEvent access, than
// reconstructing per-pointer coordinates from a flattened ring event here).
typedef struct { int action; int pointer_count; float x, y; } HdsTouchRingEvent;
// action: 0=sequence start (first finger down, pointer_count always 1), 1=move, 2=sequence end
// (last finger up or ACTION_CANCEL), 3=pointer count changed mid-sequence (a finger added or
// removed while at least one other is still down -- MotionEvent's own ACTION_POINTER_DOWN/_UP).
#define HDS_TOUCH_RING_CAPACITY 64
static HdsTouchRingEvent g_touch_ring[HDS_TOUCH_RING_CAPACITY];
static atomic_uint g_touch_ring_head = 0;
static atomic_uint g_touch_ring_tail = 0;

static void hds_touch_ring_push(int action, int pointer_count, float x, float y) {
    unsigned int head = atomic_load_explicit(&g_touch_ring_head, memory_order_relaxed);
    unsigned int next = (head + 1) % HDS_TOUCH_RING_CAPACITY;
    if (next == atomic_load_explicit(&g_touch_ring_tail, memory_order_acquire)) return; // full -- drop, never block the UI thread
    g_touch_ring[head] = (HdsTouchRingEvent){ action, pointer_count, x, y };
    atomic_store_explicit(&g_touch_ring_head, next, memory_order_release);
}

// Every hds_clap_*_draw.c tab lays its own content out at fixed, absolute pixel positions (the
// SAME files hds.clap/the standalone app already share -- see hds_clap_gui_draw.c's own header
// comment) -- only the tab BAR itself divides evenly across whatever width it's given
// (hds_clap_gui_tab_rect()). On a real phone's own much-wider-than-1150px landscape screen that
// means most of the tab bar's real width-awareness buys nothing: the content below it stayed
// exactly the size it was designed at, with dead space past its own right/bottom edge -- a real,
// confirmed-on-device symptom (a screenshot showing a working, correctly-colored, correctly-drawn
// GUI confined to roughly the left third of the real screen), not a rendering bug.
//
// Rewriting every tab file's own layout math to be responsive would be a large, invasive change
// to files this port's own explicit "minimal deviation" mandate (and every other platform's own
// regression safety) argues hard against. Instead, this window's logical WIDTH is held fixed
// forever at HDS_ANDROID_GUI_LOGICAL_W (matching the effective width every tab's own layout was
// actually designed and already tested against on desktop, see HDS_CLAP_GUI_DEFAULT_W's own
// comment -- minus the sidebar-reserved width that constant still carries, since Android has
// none at all) and hds_android_gui_redraw() computes ONE scale factor (real buffer width / this
// constant) applied identically on both axes -- real on-device testing showed independent-per-
// axis scaling visibly distorting round knobs into ellipses, and scaling less on X than a
// width-only factor would (a phone's own landscape aspect ratio is wider than this GUI's ~3:2
// design ratio) -- so this single factor is the one that actually maximizes real screen use
// without distortion. The logical HEIGHT is NOT fixed: it is recomputed every frame as real
// buffer height / this same scale (see hds_android_gui_redraw()), so whatever doesn't fit
// vertically at this scale is already handled by each tab's own pre-existing scroll/clip
// machinery, exactly as a short desktop window would be -- see
// hds_android_gui_scroll_by_px()'s own comment for how that machinery is driven here.
#define HDS_ANDROID_GUI_LOGICAL_W 1150

// -----------------------------------------------------------------------------------------------
typedef struct {
    HdsClapPlugin *plugin;
    ANativeWindow *window; // NULL whenever no Surface currently exists (between destroyed/created)
    HdsClapGuiViewState view;
    pthread_t thread;
    volatile bool should_stop;

    // Real-screen-pixels-per-logical-pixel, recomputed in hds_android_gui_redraw() whenever the
    // locked buffer's own dimensions change -- the SAME factor cairo_scale() stretches rendering
    // by, so touch dispatch (which only ever receives real screen coordinates from Java) can
    // divide back down into the logical coordinate space every hds_clap_gui_on_*() call expects.
    // One value, applied to both axes -- see HDS_ANDROID_GUI_LOGICAL_W's own comment on why a
    // single uniform factor, not two independent ones. Read and written only from this window's
    // own redraw thread (recomputed during redraw, consumed by touch dispatch on the very next
    // loop iteration, same thread either way) -- no synchronization needed, same posture as every
    // other field below.
    double scale;

    // Gesture-mode state -- explicit product direction, not a heuristic: a SINGLE finger only
    // ever taps/drags a parameter (on_button_press/on_motion/on_button_release -- knobs, pills,
    // tab switches) UNLESS an open dropdown/popup list (or the shared file browser) is directly
    // under it, in which case that single finger scrolls THAT list instead -- there is no param
    // to drag underneath an open dropdown anyway, and requiring a second/third finger just to
    // scroll a small popup list is unnatural (explicit follow-up direction, not the original
    // design). TWO OR MORE fingers always pan the active tab's own scroll position regardless of
    // what's underneath, never touching a knob/param. gesture_mode itself only ever tracks the
    // PARAM-vs-SCROLL finger-count distinction, "sticky" for the lifetime of one continuous touch
    // sequence (first finger down to last finger up) -- see
    // hds_android_touch_ring_drain_and_dispatch()'s own comment for exactly how it's set and why
    // it never flips back from SCROLL to PARAM mid-sequence; the dropdown-open check is
    // evaluated fresh on every move instead, since dropdown state can only ever actually change
    // at a down event, never mid-drag.
    enum { HDS_ANDROID_TOUCH_NONE, HDS_ANDROID_TOUCH_PARAM, HDS_ANDROID_TOUCH_SCROLL } gesture_mode;
    // Baseline for this move's own scroll/drag delta -- set at the start of every touch sequence
    // and re-baselined whenever gesture_mode (re-)enters SCROLL (so a baseline jump at that
    // instant never itself produces a scroll delta). LOGICAL (post-un-scale) units.
    double touch_last_y;
    // Leftover fractional pixels for the two scrollable widgets indexed by a plain INTEGER row
    // position rather than a continuous pixel offset (the FX tab's own file browser and
    // dropdown lists, HdsFxBrowser.scroll/HdsFxDropdown.scroll) -- see
    // hds_android_gui_scroll_by_px()'s own comment for why those two can't be scrolled with true
    // 1:1 pixel smoothness the way every other scrollable list here now is. Reset alongside
    // touch_last_y.
    double row_accum_px;
} HdsAndroidGuiWindow;

static HdsAndroidGuiWindow *g_android_gui_window = NULL;

static void *hds_android_gui_thread_main(void *arg);

// Allocates this window's own persistent state (fonts, view-state) -- called once, from
// hds_android_engine_init() (hds_android.c), well before any Surface exists. Does NOT touch
// ANativeWindow or spawn the redraw thread -- see this file's own header comment on why those
// two are tied to the Surface's own lifetime instead.
static bool hds_android_gui_init(HdsClapPlugin *p) {
    if (g_android_gui_window) return true;
    if (!hds_android_gui_fonts_init()) return false;
    HdsAndroidGuiWindow *w = (HdsAndroidGuiWindow *)calloc(1, sizeof(HdsAndroidGuiWindow));
    if (!w) return false;
    w->plugin = p;
    hds_clap_gui_view_state_init(&w->view);
    // hds_clap_gui_view_state_init()'s own default (HDS_CLAP_GUI_DEFAULT_W/H, desktop's own
    // sidebar-inclusive default) is a harmless placeholder here -- hds_android_gui_redraw() sets
    // the view's real logical width/height (HDS_ANDROID_GUI_LOGICAL_W fixed, height recomputed
    // every frame -- see that constant's own comment) the moment the first frame has a real
    // buffer to measure, well before anything is ever actually drawn or hit-tested.
    //
    // scale: neutral default, not left at calloc's own 0.0 -- touch dispatch divides real-screen
    // coordinates by it (see hds_android_touch_ring_drain_and_dispatch()'s own comment), and a
    // touch event could in principle be queued and drained before this window's own redraw
    // thread has run even once to compute the real factor, which would otherwise divide by zero.
    // 1.0 (i.e. "not yet scaled") is overwritten by the real factor within one frame of the first
    // redraw either way.
    w->scale = 1.0;
    g_android_gui_window = w;
    return true;
}

// Cairo's ARGB32 is B,G,R,A in memory on a little-endian device; ANativeWindow's RGBA_8888/
// RGBX_8888 are R,G,B,A/X -- see this file's own header comment. One in-place pass over the exact
// buffer Cairo just drew into, right before posting it; stride_px (pixels, NOT bytes -- the NDK's
// own ANativeWindow_Buffer.stride convention) can exceed width (row padding), so this only
// touches the `width` real pixels per row, never the padding.
static void hds_android_gui_swap_rb_inplace(unsigned char *bits, int32_t width, int32_t height, int32_t stride_px) {
    for (int32_t y = 0; y < height; y++) {
        unsigned char *row = bits + (size_t)y * (size_t)stride_px * 4;
        for (int32_t x = 0; x < width; x++) {
            unsigned char *px = row + (size_t)x * 4;
            unsigned char tmp = px[0];
            px[0] = px[2];
            px[2] = tmp;
        }
    }
}

// Enters SCROLL mode (or re-baselines it if already in it): resets the row accumulator and sets
// touch_last_y to logical_y, so the very next move's delta is measured from THIS instant, never
// from wherever the pointer(s) happened to be before this baseline was set. If a single-finger
// PARAM gesture was in flight (a knob being dragged, say) when a second finger landed, releases
// it cleanly first -- exactly like lifting that one finger would have, so nothing is ever left
// stuck mid-drag.
static void hds_android_gui_enter_scroll_mode(HdsAndroidGuiWindow *w, double logical_y) {
    if (w->gesture_mode == HDS_ANDROID_TOUCH_PARAM) {
        hds_clap_gui_on_button_release(w->plugin, &w->view);
    }
    w->gesture_mode = HDS_ANDROID_TOUCH_SCROLL;
    w->touch_last_y = logical_y;
    w->row_accum_px = 0.0;
}

// Whether an open dropdown/popup list (or the shared file browser) currently owns input --
// see this file's own header comment on why a single finger scrolls it directly instead of
// needing a second/third finger the way scrolling a tab's own background content does. Reads
// each tab's own already-public `dropdown_kind` field directly (every hds_clap_*_draw.c tab that
// has one uses that exact name, `HDS_XXX_DROPDOWN_NONE` as its own "closed" sentinel -- see each
// file's own declaration), plus the FX tab's own differently-shaped `dropdown.kind`/
// `browser.open` -- no field added to any of those files for this. The Global sidebar is no
// longer a tab of its own (see hds_clap_gui_draw.c's own header comment -- it's a persistent
// sidebar shown alongside every tab except FX), so its own dropdown is checked independently,
// gated on the touch actually being over the sidebar's own region (hds_clap_gui_sidebar_w(),
// the exact same width hds_clap_gui_draw.c's own dispatch functions check against) rather than
// on active_tab.
static bool hds_android_gui_dropdown_open(HdsClapGuiViewState *vs, double x) {
    if (vs->fx_view.browser.open || vs->fx_view.dropdown.kind != HDS_FX_DROPDOWN_NONE) return true;
    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);
    if (sidebar_w > 0.0 && x < sidebar_w) return vs->global_view.dropdown_kind != HDS_GLOBAL_DROPDOWN_NONE;
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_PLAY:
            return vs->play_view.dropdown_kind != HDS_PLAY_DROPDOWN_NONE || vs->combi_view.dropdown_kind != HDS_COMBI_DROPDOWN_NONE;
        case HDS_CLAP_TAB_FILTERS:
            return vs->filters_view.dropdown_kind != HDS_FILT_DROPDOWN_NONE;
        case HDS_CLAP_TAB_LFOS:
            return vs->lfos_view.dropdown_kind != HDS_LFO_DROPDOWN_NONE;
        case HDS_CLAP_TAB_ENVELOPES:
            return vs->envelopes_view.dropdown_kind != HDS_ENV_DROPDOWN_NONE;
        case HDS_CLAP_TAB_DEVICES:
            return vs->devices_view.dropdown_kind != HDS_DEV_DROPDOWN_NONE;
        default:
            return false; // FX handled above; Routing has no dropdown at all
    }
}

// Smooth, continuous touch-scroll: writes each scrollable list's own existing float field
// (scroll_offset_px / dest_scroll_px) directly, 1:1 with dragged logical pixels -- no row-
// height-sized "clicks" the way a discrete mouse-wheel step is, which is exactly what made the
// earlier on_scroll(+-1)-per-threshold touch scrolling feel jumpy/stepped on a real device.
// Clamped with the SAME max-scroll formula each tab's own on_scroll() already uses, read
// directly from already-public constants/helper functions (HDS_LFO_ROW_H, hds_fx_gui_max_scroll(),
// hds_clap_routing_content_h(), ...) rather than re-derived -- this is purely an ADDITIONAL,
// continuous-delta sibling path Android alone drives; zero changes to any tab's own on_scroll().
//
// The FX tab's own file browser and dropdown lists are the one exception: both are indexed by a
// plain INTEGER row position (HdsFxBrowser.scroll/HdsFxDropdown.scroll), not a continuous pixel
// offset, so true 1:1 smoothness isn't possible there without changing those fields' own type (a
// shared-file change this port avoids) -- dragged pixels are instead accumulated (row_accum_px)
// against that widget's own REAL row height (HDS_FX_BROWSER_ROW_H/HDS_FX_DROPDOWN_ROW_H, not a
// generic guessed constant) before stepping the integer index by one, which is as smooth as an
// integer-indexed list can be made.
//
// The Global sidebar is no longer a tab (see hds_clap_gui_draw.c's own header comment -- it's a
// persistent sidebar shown alongside every tab except FX), so scrolling it is no longer keyed off
// active_tab at all: `x` (the touch's own logical x, same as every hds_clap_gui_on_*() coordinate)
// decides whether this drag is over the sidebar's own region (hds_clap_gui_sidebar_w(), the exact
// same width hds_clap_gui_draw.c's own dispatch functions check against) BEFORE active_tab is
// consulted for anything -- mirroring that file's own sidebar-first dispatch order exactly.
static void hds_android_gui_scroll_by_px(HdsAndroidGuiWindow *w, double x, double delta_px) {
    HdsClapGuiViewState *vs = &w->view;
    double window_h = vs->window_h - HDS_CLAP_TAB_BAR_H;
    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);
    if (sidebar_w > 0.0 && x < sidebar_w) {
        HdsClapGlobalViewState *gv = &vs->global_view;
        ProgramState *prog = hds_play_program(w->plugin);
        int algo = (int)prog->fm_algorithm;
        if (algo < 0 || algo >= HDS_GLOBAL_FMALGO_COUNT) algo = 0;
        HdsGlobalLayout L;
        hds_clap_global_compute_layout(algo, 0.0, &L);
        double max_scroll = L.content_h - vs->window_h;
        if (max_scroll < 0.0) max_scroll = 0.0;
        gv->scroll_offset_px -= delta_px;
        if (gv->scroll_offset_px < 0.0) gv->scroll_offset_px = 0.0;
        if (gv->scroll_offset_px > max_scroll) gv->scroll_offset_px = max_scroll;
        return;
    }
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_FX: {
            HdsFxGuiViewState *fv = &vs->fx_view;
            if (fv->browser.open) {
                w->row_accum_px -= delta_px;
                HdsFxBrowserLayout L;
                hds_fx_browser_compute_layout(fv, &L);
                int max_start = fv->browser.entry_count - L.visible_rows;
                if (max_start < 0) max_start = 0;
                while (w->row_accum_px >= HDS_FX_BROWSER_ROW_H) { fv->browser.scroll++; w->row_accum_px -= HDS_FX_BROWSER_ROW_H; }
                while (w->row_accum_px <= -HDS_FX_BROWSER_ROW_H) { fv->browser.scroll--; w->row_accum_px += HDS_FX_BROWSER_ROW_H; }
                if (fv->browser.scroll < 0) fv->browser.scroll = 0;
                if (fv->browser.scroll > max_start) fv->browser.scroll = max_start;
                return;
            }
            if (fv->dropdown.kind != HDS_FX_DROPDOWN_NONE) {
                w->row_accum_px -= delta_px;
                int count = hds_fx_dropdown_option_count(&w->plugin->gui_host, fv->dropdown.kind);
                int visible_rows = hds_fx_dropdown_visible_rows(count, fv->window_h);
                int max_start = count - visible_rows;
                if (max_start < 0) max_start = 0;
                while (w->row_accum_px >= HDS_FX_DROPDOWN_ROW_H) { fv->dropdown.scroll++; w->row_accum_px -= HDS_FX_DROPDOWN_ROW_H; }
                while (w->row_accum_px <= -HDS_FX_DROPDOWN_ROW_H) { fv->dropdown.scroll--; w->row_accum_px += HDS_FX_DROPDOWN_ROW_H; }
                if (fv->dropdown.scroll < 0) fv->dropdown.scroll = 0;
                if (fv->dropdown.scroll > max_start) fv->dropdown.scroll = max_start;
                return;
            }
            fv->scroll_offset_px -= delta_px;
            double max_scroll = hds_fx_gui_max_scroll(fv);
            if (fv->scroll_offset_px < 0.0) fv->scroll_offset_px = 0.0;
            if (fv->scroll_offset_px > max_scroll) fv->scroll_offset_px = max_scroll;
            return;
        }
        case HDS_CLAP_TAB_LFOS: {
            HdsClapLfosViewState *lv = &vs->lfos_view;
            if (lv->dropdown_kind == HDS_LFO_DROPDOWN_DEST) {
                lv->dest_scroll_px -= delta_px;
                if (lv->dest_scroll_px < 0.0) lv->dest_scroll_px = 0.0;
                return;
            }
            double content_h = HDS_LFO_HEADER_H + LFO_COUNT * HDS_LFO_ROW_H;
            double max_scroll = content_h - window_h;
            if (max_scroll < 0.0) max_scroll = 0.0;
            lv->scroll_offset_px -= delta_px;
            if (lv->scroll_offset_px < 0.0) lv->scroll_offset_px = 0.0;
            if (lv->scroll_offset_px > max_scroll) lv->scroll_offset_px = max_scroll;
            return;
        }
        case HDS_CLAP_TAB_ENVELOPES: {
            HdsClapEnvelopesViewState *ev = &vs->envelopes_view;
            if (ev->dropdown_kind == HDS_ENV_DROPDOWN_DEST) {
                ev->dest_scroll_px -= delta_px;
                if (ev->dest_scroll_px < 0.0) ev->dest_scroll_px = 0.0;
                return;
            }
            double content_h = HDS_ENV_HEADER_H + ASSIGNABLE_EG_COUNT * HDS_ENV_PANEL_H;
            double max_scroll = content_h - window_h;
            if (max_scroll < 0.0) max_scroll = 0.0;
            ev->scroll_offset_px -= delta_px;
            if (ev->scroll_offset_px < 0.0) ev->scroll_offset_px = 0.0;
            if (ev->scroll_offset_px > max_scroll) ev->scroll_offset_px = max_scroll;
            return;
        }
        case HDS_CLAP_TAB_ROUTING: {
            HdsClapRoutingViewState *rv = &vs->routing_view;
            double content_h = hds_clap_routing_content_h();
            double max_scroll = content_h - window_h;
            if (max_scroll < 0.0) max_scroll = 0.0;
            rv->scroll_offset_px -= delta_px;
            if (rv->scroll_offset_px < 0.0) rv->scroll_offset_px = 0.0;
            if (rv->scroll_offset_px > max_scroll) rv->scroll_offset_px = max_scroll;
            return;
        }
        default:
            return; // Play/Filters/Devices have no scrollable background content
    }
}

// Drains every touch event queued since the last frame (this window's own redraw thread only --
// see this file's own header comment) and dispatches it, un-scaling every real-screen coordinate
// into the view's own logical space first (divides by the one uniform scale factor
// hds_android_gui_redraw() computes -- see HDS_ANDROID_GUI_LOGICAL_W's own comment).
// gesture_mode keeps a single continuous touch sequence from ever driving BOTH a knob and a
// background-content scroll: it is decided once, then sticky for the rest of that sequence --
//   - action 0 (sequence start) always arrives with pointer_count==1 (MotionEvent's own
//     ACTION_DOWN contract) -- always enters PARAM mode and issues on_button_press (this is also
//     what opens/closes/selects-from a dropdown in the first place, and how a single-finger tap
//     switches tabs). Baselines touch_last_y/row_accum_px unconditionally, so the very first move
//     afterward -- whether it turns out to drag a param or scroll a just-opened dropdown -- has a
//     correct delta from this exact instant, not a stale one.
//   - action 3 (pointer count changed mid-sequence, a finger added or removed) moves to SCROLL
//     mode the instant two or more fingers are down, however the sequence started -- and STAYS
//     in SCROLL mode even if it drops back to one finger (lifting one of two scrolling fingers
//     must never suddenly start dragging a knob under the remaining finger).
//   - action 1 (move): hds_android_gui_dropdown_open() is checked FRESH on every move (unlike
//     gesture_mode, dropdown state only ever actually changes at a down event, so this can never
//     flicker mid-drag) -- SCROLL mode OR an open dropdown/browser both route into
//     hds_android_gui_scroll_by_px(), continuous and smooth; PARAM mode with nothing open routes
//     into on_motion(); never both for the same move, which is the whole point -- a scroll can
//     never touch a param, and a param drag can never also scroll the list underneath it.
//   - action 2 (sequence end) always calls on_button_release (itself a no-op if nothing was ever
//     captured, e.g. a SCROLL-mode sequence -- same convention hds_standalone_gui_x11.c's own
//     unconditional release call already relies on) and resets gesture_mode to NONE.
static void hds_android_touch_ring_drain_and_dispatch(HdsAndroidGuiWindow *w) {
    unsigned int tail = atomic_load_explicit(&g_touch_ring_tail, memory_order_relaxed);
    unsigned int head = atomic_load_explicit(&g_touch_ring_head, memory_order_acquire);
    while (tail != head) {
        HdsTouchRingEvent ev = g_touch_ring[tail];
        double x = (double)ev.x / w->scale;
        double y = (double)ev.y / w->scale;
        if (ev.action == 0) { // sequence start, always one finger
            w->gesture_mode = HDS_ANDROID_TOUCH_PARAM;
            w->touch_last_y = y;
            w->row_accum_px = 0.0;
            hds_clap_gui_on_button_press(w->plugin, &w->view, x, y, 1);
        } else if (ev.action == 3) { // pointer count changed mid-sequence
            if (ev.pointer_count >= 2) hds_android_gui_enter_scroll_mode(w, y);
            else if (w->gesture_mode == HDS_ANDROID_TOUCH_SCROLL) hds_android_gui_enter_scroll_mode(w, y); // still scrolling, just re-baseline
        } else if (ev.action == 1) { // move
            bool scroll_now = (w->gesture_mode == HDS_ANDROID_TOUCH_SCROLL) || hds_android_gui_dropdown_open(&w->view, x);
            if (scroll_now) {
                // Natural/"content follows finger" convention (standard on touchscreens):
                // dragging DOWN reveals content above (same direction as an X11 "scroll up"
                // wheel click, on_scroll(..., +1) per hds_standalone_gui_x11.c's own
                // pump_events() convention -- hds_android_gui_scroll_by_px() itself subtracts,
                // matching that same sign).
                hds_android_gui_scroll_by_px(w, x, y - w->touch_last_y);
            } else if (w->gesture_mode == HDS_ANDROID_TOUCH_PARAM) {
                hds_clap_gui_on_motion(w->plugin, &w->view, x, y);
            }
            w->touch_last_y = y;
        } else { // action 2: sequence end (last finger up, or ACTION_CANCEL)
            hds_clap_gui_on_button_release(w->plugin, &w->view);
            w->gesture_mode = HDS_ANDROID_TOUCH_NONE;
        }
        tail = (tail + 1) % HDS_TOUCH_RING_CAPACITY;
    }
    atomic_store_explicit(&g_touch_ring_tail, tail, memory_order_release);
}

// Draws directly into the locked ANativeWindow buffer's own memory -- a cairo_image_surface_t
// wrapping that exact pointer, no separate render target, no copy (see this file's own header
// comment). The view's logical WIDTH never changes (HDS_ANDROID_GUI_LOGICAL_W, fixed); its
// logical HEIGHT and `scale` are both recomputed here whenever the real buffer size changes (the
// one thing ANativeWindow itself can report that this app doesn't otherwise learn about until
// the next lock, there being no ConfigureNotify-equivalent pushed to this thread) -- scale from
// buffer width alone, height as whatever is actually visible at that scale -- then ONE uniform
// cairo_scale() stretches the logical canvas to fill the real screen's width exactly, with no
// axis distortion. See HDS_ANDROID_GUI_LOGICAL_W's own comment for why.
static void hds_android_gui_redraw(HdsAndroidGuiWindow *w) {
    if (!w->window) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(w->window, &buf, NULL) != 0) return;
    if (buf.width > 0 && buf.height > 0) {
        double new_scale = (double)buf.width / (double)HDS_ANDROID_GUI_LOGICAL_W;
        int new_logical_h = (int)((double)buf.height / new_scale);
        if (new_scale != w->scale || new_logical_h != w->view.window_h) {
            w->scale = new_scale;
            hds_clap_gui_on_resize(&w->view, HDS_ANDROID_GUI_LOGICAL_W, new_logical_h);
        }
    }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        (unsigned char *)buf.bits, CAIRO_FORMAT_ARGB32, buf.width, buf.height, buf.stride * 4);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, w->scale, w->scale);
    hds_clap_gui_render(cr, w->plugin, &w->view);
    cairo_surface_flush(surface);
    hds_android_gui_swap_rb_inplace((unsigned char *)buf.bits, buf.width, buf.height, buf.stride);
    cairo_destroy(cr);
    cairo_surface_destroy(surface); // wraps buf.bits -- does not free it (cairo_image_surface_create_for_data never takes ownership)
    ANativeWindow_unlockAndPost(w->window);
}

// This window's own redraw/input-apply loop, on its own dedicated thread -- see this file's own
// header comment for why it must be a separate thread from both the UI thread (touch callbacks)
// and the AAudio realtime callback thread (hds_android.c's own audio_callback). Same ~30Hz redraw
// pacing as hds_standalone_gui_x11.c's own thread_main, for the same reason (nothing in this GUI
// animates continuously, plenty for a knob/pill UI) -- touch is still drained every ~10ms poll,
// same as that file's own X11-event-pump cadence, so input itself still feels immediate even
// though the screen only actually repaints at ~30Hz.
static void *hds_android_gui_thread_main(void *arg) {
    HdsAndroidGuiWindow *w = (HdsAndroidGuiWindow *)arg;
    struct timespec last_redraw;
    clock_gettime(CLOCK_MONOTONIC, &last_redraw);
    while (!w->should_stop) {
        hds_android_touch_ring_drain_and_dispatch(w);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_ms = (now.tv_sec - last_redraw.tv_sec) * 1000.0 + (now.tv_nsec - last_redraw.tv_nsec) / 1e6;
        if (elapsed_ms >= 33.0) {
            hds_fx_gui_browser_refresh(&w->view.fx_view.browser); // reused verbatim -- see this file's own header comment
            hds_android_gui_redraw(w);
            last_redraw = now;
        }
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 10L * 1000L * 1000L };
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

// JNI-callable surface lifecycle -- see this file's own header comment. `surface` is a
// java.view.Surface (SurfaceHolder.getSurface(), handed across from
// SurfaceHolder.Callback.surfaceCreated() -- see hds_android.c's own JNI entry point).
static void hds_android_gui_surface_created(JNIEnv *env, jobject surface) {
    HdsAndroidGuiWindow *w = g_android_gui_window;
    if (!w) return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) { HDS_LOGW("gui: ANativeWindow_fromSurface failed"); return; }
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888); // 0,0 keeps the SurfaceView's own natural size
    w->window = window;
    w->should_stop = false;
    if (pthread_create(&w->thread, NULL, hds_android_gui_thread_main, w) != 0) {
        HDS_LOGW("gui: pthread_create failed");
        ANativeWindow_release(window);
        w->window = NULL;
    }
}

// Same window/thread teardown ordering hds_standalone_gui_destroy() already established: signal,
// JOIN, only then release the window -- so the redraw thread can never be mid-lock against a
// window this call is simultaneously releasing. Safe to call with no window currently open (e.g.
// a stray surfaceDestroyed with no matching surfaceCreated) -- a no-op, same as that file's own
// "already torn down" guard.
static void hds_android_gui_surface_destroyed(void) {
    HdsAndroidGuiWindow *w = g_android_gui_window;
    if (!w || !w->window) return;
    w->should_stop = true;
    pthread_join(w->thread, NULL);
    ANativeWindow_release(w->window);
    w->window = NULL;
}

// action: 0=sequence start, 1=move, 2=sequence end, 3=pointer count changed mid-sequence
// (hds_android.c's own JNI entry point relays MotionEvent's real ACTION_* constants down to this
// -- see that file's own comment). x/y are real, un-scaled screen coordinates (HdsSurfaceView.
// java's own single-pointer coordinates, or its already-averaged multi-pointer centroid) --
// hds_android_touch_ring_drain_and_dispatch() divides by the current uniform `scale` into this
// window's own logical space before ever calling into hds_clap_gui_on_*().
static void hds_android_gui_touch_event(int action, int pointer_count, float x, float y) {
    if (!g_android_gui_window) return;
    hds_touch_ring_push(action, pointer_count, x, y);
}
