// hds_android_gui.c: the Android port's own native GUI window -- the SAME Cairo-drawn tab bar
// (hds_clap_gui_draw.c, with Global folded in as its own tab here -- see that file's own
// HDS_ANDROID_GUI_ENABLED comments) that hds.clap already embeds in a DAW host and
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
//   - Swipe-to-scroll: there is no separate scroll-wheel input channel on a touchscreen the way
//     X11's button 4/5 give hds_standalone_gui_x11.c's own pump_events() for free. A touch-move is
//     the one and only drag channel, used for BOTH an active knob-drag (hds_clap_gui_on_motion(),
//     called on every move exactly like a mouse-drag already is -- zero changes needed anywhere)
//     AND, when no knob-drag is in flight (see hds_android_gui_knob_drag_active() below -- a
//     small, tab-aware check reading each tab's own already-public `knob_drag.target`/`drag_kind`
//     field, not a new field added to any of them), accumulated vertical pixels fed into the
//     SAME hds_clap_gui_on_scroll(..., +-1) entry point X11's wheel already drives, at a fixed
//     per-"click" pixel threshold -- see HDS_ANDROID_SWIPE_PX_PER_CLICK. No per-tab scroll math
//     changes at all; this just decides WHEN to call an existing function, the same posture
//     HDS_CLAP_SIDEBAR_RESERVED_W's own comment in hds_clap_gui_draw.c already takes for "only
//     WHERE/WHEN, never WHAT."
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
typedef struct { int action; float x, y; } HdsTouchRingEvent; // action: 0=down, 1=move, 2=up/cancel
#define HDS_TOUCH_RING_CAPACITY 64
static HdsTouchRingEvent g_touch_ring[HDS_TOUCH_RING_CAPACITY];
static atomic_uint g_touch_ring_head = 0;
static atomic_uint g_touch_ring_tail = 0;

static void hds_touch_ring_push(int action, float x, float y) {
    unsigned int head = atomic_load_explicit(&g_touch_ring_head, memory_order_relaxed);
    unsigned int next = (head + 1) % HDS_TOUCH_RING_CAPACITY;
    if (next == atomic_load_explicit(&g_touch_ring_tail, memory_order_acquire)) return; // full -- drop, never block the UI thread
    g_touch_ring[head] = (HdsTouchRingEvent){ action, x, y };
    atomic_store_explicit(&g_touch_ring_head, next, memory_order_release);
}

// -----------------------------------------------------------------------------------------------
typedef struct {
    HdsClapPlugin *plugin;
    ANativeWindow *window; // NULL whenever no Surface currently exists (between destroyed/created)
    HdsClapGuiViewState view;
    pthread_t thread;
    volatile bool should_stop;

    // Swipe-to-scroll accumulator -- see this file's own header comment. Reset on every fresh
    // touch-down (a new gesture starts a new accumulation; it would be wrong for a slow drag's
    // leftover fractional pixels to carry into an unrelated later gesture).
    bool touch_down;
    float touch_last_y;
    double swipe_accum_px;
} HdsAndroidGuiWindow;

static HdsAndroidGuiWindow *g_android_gui_window = NULL;

#define HDS_ANDROID_SWIPE_PX_PER_CLICK 24.0 // one on_scroll() "click" per this many dragged pixels -- a
                                             // single reasonable constant across every tab (on_scroll
                                             // itself has no per-tab row-height parameter to scale
                                             // against), not tuned per-tab

// Whether the active tab currently has a knob-drag (or the FX tab's own equivalent) in flight --
// see this file's own header comment on why swipe-to-scroll checks this first. Reads each tab's
// own already-public view-state field directly (every hds_clap_*_draw.c tab embeds a
// `HdsPlayKnobDrag knob_drag;` with `.target` NULL when idle, except the FX tab, which uses its
// own `drag_kind` -- see hds_fx_gui_draw.c's own HdsFxGuiViewState comment); no field is added to
// any of those files for this.
static bool hds_android_gui_knob_drag_active(HdsClapGuiViewState *vs) {
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_PLAY:
            // Checks both sub-views rather than branching on performance_mode: harmless (the
            // inactive one's own knob_drag.target can never be non-NULL -- only the currently
            // rendered/hit-tested sub-view ever starts a drag), and avoids needing a HdsClapPlugin*
            // in this function's own signature just for this one check.
            return vs->play_view.knob_drag.target != NULL || vs->combi_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_FILTERS:
            return vs->filters_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_LFOS:
            return vs->lfos_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_ENVELOPES:
            return vs->envelopes_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_FX:
            return vs->fx_view.drag_kind != 0; // the FX tab's own drag state uses a different shape -- see hds_fx_gui_draw.c's own HdsFxGuiViewState comment
        case HDS_CLAP_TAB_ROUTING:
            return vs->routing_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_DEVICES:
            return vs->devices_view.knob_drag.target != NULL;
        case HDS_CLAP_TAB_GLOBAL:
            return vs->global_view.knob_drag.target != NULL;
        default:
            return false;
    }
}

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

// Drains every touch event queued since the last frame (this window's own redraw thread only --
// see this file's own header comment) and dispatches it. Always feeds on_motion unconditionally
// on a move (so an in-flight knob-drag tracks the finger exactly like a mouse-drag already does,
// zero per-tab changes needed) -- swipe-accumulation on top of that only when
// hds_android_gui_knob_drag_active() says nothing is currently being dragged.
static void hds_android_touch_ring_drain_and_dispatch(HdsAndroidGuiWindow *w) {
    unsigned int tail = atomic_load_explicit(&g_touch_ring_tail, memory_order_relaxed);
    unsigned int head = atomic_load_explicit(&g_touch_ring_head, memory_order_acquire);
    while (tail != head) {
        HdsTouchRingEvent ev = g_touch_ring[tail];
        if (ev.action == 0) { // down
            w->touch_down = true;
            w->touch_last_y = ev.y;
            w->swipe_accum_px = 0.0;
            hds_clap_gui_on_button_press(w->plugin, &w->view, (double)ev.x, (double)ev.y, 1);
        } else if (ev.action == 1) { // move
            hds_clap_gui_on_motion(w->plugin, &w->view, (double)ev.x, (double)ev.y);
            if (w->touch_down && !hds_android_gui_knob_drag_active(&w->view)) {
                // Natural/"content follows finger" convention (standard on touchscreens): dragging
                // DOWN reveals content above (same direction as an X11 "scroll up" wheel click,
                // on_scroll(..., +1) per hds_standalone_gui_x11.c's own pump_events() convention).
                w->swipe_accum_px += (double)(ev.y - w->touch_last_y);
                while (w->swipe_accum_px >= HDS_ANDROID_SWIPE_PX_PER_CLICK) {
                    hds_clap_gui_on_scroll(w->plugin, &w->view, (double)ev.x, (double)ev.y, 1);
                    w->swipe_accum_px -= HDS_ANDROID_SWIPE_PX_PER_CLICK;
                }
                while (w->swipe_accum_px <= -HDS_ANDROID_SWIPE_PX_PER_CLICK) {
                    hds_clap_gui_on_scroll(w->plugin, &w->view, (double)ev.x, (double)ev.y, -1);
                    w->swipe_accum_px += HDS_ANDROID_SWIPE_PX_PER_CLICK;
                }
            }
            w->touch_last_y = ev.y;
        } else { // up/cancel
            hds_clap_gui_on_button_release(w->plugin, &w->view);
            w->touch_down = false;
            w->swipe_accum_px = 0.0;
        }
        tail = (tail + 1) % HDS_TOUCH_RING_CAPACITY;
    }
    atomic_store_explicit(&g_touch_ring_tail, tail, memory_order_release);
}

// Draws directly into the locked ANativeWindow buffer's own memory -- a cairo_image_surface_t
// wrapping that exact pointer, no separate render target, no copy (see this file's own header
// comment). Resizes the view (hds_clap_gui_on_resize()) first if the buffer's own dimensions
// changed since the last frame -- the one thing ANativeWindow itself can report that this app
// doesn't otherwise learn about until the next lock (there is no ConfigureNotify-equivalent
// pushed to this thread).
static void hds_android_gui_redraw(HdsAndroidGuiWindow *w) {
    if (!w->window) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(w->window, &buf, NULL) != 0) return;
    if (buf.width != w->view.window_w || buf.height != w->view.window_h) {
        hds_clap_gui_on_resize(&w->view, buf.width, buf.height);
    }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        (unsigned char *)buf.bits, CAIRO_FORMAT_ARGB32, buf.width, buf.height, buf.stride * 4);
    cairo_t *cr = cairo_create(surface);
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

// action: 0=down, 1=move, 2=up/cancel (hds_android.c's own JNI entry point already collapses
// MotionEvent's real ACTION_* constants down to this -- see that file's own comment).
static void hds_android_gui_touch_event(int action, float x, float y) {
    if (!g_android_gui_window) return;
    hds_touch_ring_push(action, x, y);
}
