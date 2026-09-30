// hds_standalone_gui_x11.c: the standalone app's own native GUI window -- the SAME Cairo-drawn
// tab bar/sidebar (hds_clap_gui_draw.c/hds_clap_global_draw.c/every hds_clap_*_draw.c tab, plus
// hds_fx_gui_draw.c for the FX tab) that hds.clap already embeds inside a DAW host, now opened as
// this process's OWN top-level window instead of an embedded child -- see hds_engine_instance.h's
// own header comment on why HdsClapPlugin (despite its name) is exactly the right type to pass
// these already-shared render/interaction functions: they only ever touch its .knobs/.gui_host/
// .ext_in_peak fields, all three of which this file's own hds_standalone_gui_create() populates
// from the standalone app's real, live, in-process engine state -- no OSC round-trip, no second
// process, the exact same "reads/writes engine state directly" design hds_clap_gui_draw.c's own
// header comment already established for the CLAP plugin.
//
// Differences from hds_clap_gui_x11.c (that file's own platform shim, CLAP-embedded case):
//   - This window is created and mapped immediately as a real top-level window (WM_NAME/
//     WM_DELETE_WINDOW/a real title bar from the window manager) -- there is no host to call
//     set_parent()/show() on this process's behalf, so create() does both steps itself.
//   - No CLAP host timer-support extension exists to drive repaints/event pumping (there's no
//     CLAP host at all) -- hds_standalone_gui_create() spawns this window's OWN dedicated pthread
//     (hds_standalone_gui_thread_main()) instead, polling X11 events at ~100Hz and redrawing at
//     ~30Hz via a monotonic clock, entirely independent of hds_standalone.c's own main loop.
//     DELIBERATE, and not merely a convenience: that main loop's audio-write call can legitimately
//     block for real stretches of time (PipeWire backpressure waiting out a graph reconfiguration,
//     e.g. another app's patchbay tool reconnecting nodes) -- see write_audio_block()'s own
//     comment. Before this window ran on its own thread, that same blocking call sat directly
//     between successive calls to this file's own pump function, so a multi-second PipeWire stall
//     froze the entire GUI (window stops redrawing, stops responding to input) for that whole
//     stretch, even though audio itself kept flowing from the ring's own already-buffered content.
//     Two independent standing priorities for this whole codebase, in order: (1) the realtime
//     audio-producing path must never be made to wait on anything GUI-related, and (2) the GUI
//     must never be at the mercy of that path's own occasional, unavoidable stalls -- a GUI-domain
//     hang and an audio-domain hang must never be able to cause each other. Cross-thread state
//     access (this thread and hds_standalone.c's own main/audio thread both touch g_gui_ctx.knobs)
//     is accepted, unsynchronized "benign race" territory on purpose -- the exact same convention
//     hds.clap's own host-UI-thread-vs-audio-thread split already relies on for plain control
//     parameters (a knob read a block late/early is inaudible; a mutex on the audio thread is not
//     acceptable at any cost, so no lock is used here either).
//   - Closing the window (the WM's own close button, via WM_DELETE_WINDOW) sets g_synth_running
//     to false -- the same clean-shutdown path SIGINT (Ctrl+C) already triggers (see
//     handle_sigint() in backend.c) -- rather than only hiding the window with the process still
//     running headless underneath; this is currently the ONLY control surface a --gui run has, so
//     closing it is the natural "I'm done" signal, same as closing any other GUI app's window.
//   - Single top-level window per process (this app has exactly one instance, unlike a CLAP host
//     that can load several) -- g_standalone_gui_window is a plain file-scope singleton, not a
//     per-instance void* field threaded through a plugin struct.
//
// #include'd by hds_standalone.c after hds_engine_instance.h and every hds_clap_*_draw.c/
// hds_fx_gui_draw.c tab file (needs HdsClapPlugin fully defined, HdsClapGuiViewState/
// hds_clap_gui_render/hds_clap_gui_on_*) -- the same GUI-file include order clap_plugin.c already
// uses, just ending here instead of at hds_clap_gui_x11.c.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <pthread.h>

#include "hds_fx_gui_browser.c" // hds_fx_gui_browser_refresh() -- reused verbatim, same as hds_clap_gui_x11.c's own identical include

typedef struct {
    HdsClapPlugin *plugin;
    Display *display;
    int screen;
    Window win;
    Atom wm_delete_window;
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_surface_t *backbuffer;
    cairo_t *backbuffer_cr;
    HdsClapGuiViewState view;
    struct timespec last_redraw;
    // This window's own dedicated event-pump/redraw thread -- see this file's own header comment
    // for why it must never share a thread with hds_standalone.c's own audio-producing main loop.
    // should_stop is written exactly once (by hds_standalone_gui_destroy(), from the main thread,
    // strictly before pthread_join()) and only ever read by the thread's own loop condition -- the
    // same single-writer/plain-flag convention g_synth_running itself already uses for its own
    // cross-context (signal handler) stop signal, not a new pattern introduced here.
    pthread_t thread;
    volatile bool should_stop;
} HdsStandaloneGuiWindow;

static HdsStandaloneGuiWindow *g_standalone_gui_window = NULL;

static void hds_standalone_gui_resize_backbuffer(HdsStandaloneGuiWindow *w) {
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    w->backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w->view.window_w, w->view.window_h);
    w->backbuffer_cr = cairo_create(w->backbuffer);
}

static void hds_standalone_gui_redraw(HdsStandaloneGuiWindow *w) {
    if (!w->cr || !w->backbuffer_cr) return;
    hds_clap_gui_render(w->backbuffer_cr, w->plugin, &w->view);
    cairo_surface_flush(w->backbuffer);
    cairo_set_source_surface(w->cr, w->backbuffer, 0, 0);
    cairo_paint(w->cr);
    cairo_surface_flush(w->surface);
    XFlush(w->display);
    clock_gettime(CLOCK_MONOTONIC, &w->last_redraw);
}

// The window's own close button (WM_DELETE_WINDOW) sets g_synth_running to false directly -- the
// main/audio thread's own `while (g_synth_running)` loop condition (hds_standalone.c) already polls
// that same flag every iteration for the SIGINT/Ctrl+C case, so this needs no separate signaling.
static void hds_standalone_gui_pump_events(HdsStandaloneGuiWindow *w) {
    while (XPending(w->display) > 0) {
        XEvent ev;
        XNextEvent(w->display, &ev);
        switch (ev.type) {
            case ButtonPress:
                if (ev.xbutton.button == 4) hds_clap_gui_on_scroll(w->plugin, &w->view, (double)ev.xbutton.x, (double)ev.xbutton.y, 1);
                else if (ev.xbutton.button == 5) hds_clap_gui_on_scroll(w->plugin, &w->view, (double)ev.xbutton.x, (double)ev.xbutton.y, -1);
                else {
                    XSetInputFocus(w->display, w->win, RevertToParent, CurrentTime);
                    hds_clap_gui_on_button_press(w->plugin, &w->view, (double)ev.xbutton.x, (double)ev.xbutton.y, ev.xbutton.button == 3 ? 3 : 1);
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button != 4 && ev.xbutton.button != 5) hds_clap_gui_on_button_release(w->plugin, &w->view);
                break;
            case MotionNotify:
                hds_clap_gui_on_motion(w->plugin, &w->view, (double)ev.xmotion.x, (double)ev.xmotion.y);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != w->view.window_w || ev.xconfigure.height != w->view.window_h) {
                    hds_clap_gui_on_resize(&w->view, ev.xconfigure.width, ev.xconfigure.height);
                    cairo_xlib_surface_set_size(w->surface, ev.xconfigure.width, ev.xconfigure.height);
                    hds_standalone_gui_resize_backbuffer(w);
                }
                break;
            case KeyPress: {
                char text[8];
                KeySym keysym = NoSymbol;
                int n = XLookupString(&ev.xkey, text, sizeof(text) - 1, &keysym, NULL);
                if (keysym == XK_BackSpace) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_BACKSPACE, 0);
                else if (keysym == XK_Return || keysym == XK_KP_Enter) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ENTER, 0);
                else if (keysym == XK_Escape) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ESCAPE, 0);
                else if (n > 0) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_CHAR, text[0]);
                break;
            }
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == w->wm_delete_window) g_synth_running = 0;
                break;
            default: break;
        }
    }
}

static void *hds_standalone_gui_thread_main(void *arg);
static void hds_standalone_gui_destroy_window(HdsStandaloneGuiWindow *w);

static bool hds_standalone_gui_create(HdsClapPlugin *p) {
    if (g_standalone_gui_window) return true;

    HdsStandaloneGuiWindow *w = (HdsStandaloneGuiWindow *)calloc(1, sizeof(HdsStandaloneGuiWindow));
    if (!w) return false;
    w->plugin = p;
    w->display = XOpenDisplay(NULL);
    if (!w->display) { fprintf(stderr, "[hds] gui create: XOpenDisplay failed (no X11 display available -- run with a display, or without --gui)\n"); free(w); return false; }
    w->screen = DefaultScreen(w->display);
    hds_clap_gui_view_state_init(&w->view);

    w->win = XCreateSimpleWindow(w->display, RootWindow(w->display, w->screen), 0, 0,
                                  (unsigned int)w->view.window_w, (unsigned int)w->view.window_h, 0,
                                  BlackPixel(w->display, w->screen), BlackPixel(w->display, w->screen));
    XSelectInput(w->display, w->win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XSetWindowBackgroundPixmap(w->display, w->win, None);

    // Real top-level window chrome -- no CLAP host to embed us in, so this process owns the whole
    // window lifecycle: title, WM_DELETE_WINDOW (so the WM's own close button is a clean shutdown
    // rather than the WM just killing the connection), and immediate mapping.
    XStoreName(w->display, w->win, "HDS");
    XClassHint class_hint = { .res_name = (char *)"hds", .res_class = (char *)"HDS" };
    XSetClassHint(w->display, w->win, &class_hint);
    w->wm_delete_window = XInternAtom(w->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->display, w->win, &w->wm_delete_window, 1);

    w->surface = cairo_xlib_surface_create(w->display, w->win, DefaultVisual(w->display, w->screen), w->view.window_w, w->view.window_h);
    w->cr = cairo_create(w->surface);
    hds_standalone_gui_resize_backbuffer(w);

    XMapWindow(w->display, w->win);
    XFlush(w->display);
    clock_gettime(CLOCK_MONOTONIC, &w->last_redraw);

    g_standalone_gui_window = w;

    if (pthread_create(&w->thread, NULL, hds_standalone_gui_thread_main, w) != 0) {
        fprintf(stderr, "[hds] gui create: pthread_create failed -- continuing headless.\n");
        hds_standalone_gui_destroy_window(w); // window/Cairo state only -- see its own comment
        g_standalone_gui_window = NULL;
        return false;
    }
    return true;
}

// This window's own event-pump/redraw loop, running on its own dedicated thread -- see this
// file's own header comment for why it must never run on hds_standalone.c's own main/audio thread.
// Polls X11 at ~100Hz (input feels responsive) but only actually redraws at ~30Hz (matching
// hds_clap_gui_x11.c's own CLAP-host timer interval -- plenty for a knob/pill GUI, nothing here
// animates continuously). The 10ms sleep between polls is this thread's own pacing, replacing what
// used to be free pacing borrowed from the main loop's own per-block cadence.
static void *hds_standalone_gui_thread_main(void *arg) {
    HdsStandaloneGuiWindow *w = (HdsStandaloneGuiWindow *)arg;
    while (!w->should_stop && g_synth_running) {
        hds_standalone_gui_pump_events(w);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_ms = (now.tv_sec - w->last_redraw.tv_sec) * 1000.0 + (now.tv_nsec - w->last_redraw.tv_nsec) / 1e6;
        if (elapsed_ms >= 33.0) {
            hds_fx_gui_browser_refresh(&w->view.fx_view.browser); // reused verbatim -- see this file's own header comment
            hds_standalone_gui_redraw(w);
        }
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 10L * 1000L * 1000L };
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

// The window/Cairo/X11 teardown itself, shared by both the normal shutdown path
// (hds_standalone_gui_destroy(), after joining the thread) and hds_standalone_gui_create()'s own
// failure path above (pthread_create failing after the window already exists, no thread to join).
static void hds_standalone_gui_destroy_window(HdsStandaloneGuiWindow *w) {
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    if (w->cr) cairo_destroy(w->cr);
    if (w->surface) cairo_surface_destroy(w->surface);
    if (w->display) { XDestroyWindow(w->display, w->win); XCloseDisplay(w->display); }
    free(w);
}

// Normal shutdown path (process exiting, g_synth_running already false, or about to be) -- signals
// the thread to stop and JOINS it before touching any X11/Cairo resource, so the thread can never
// be mid-XNextEvent/mid-redraw against a display connection this call is simultaneously tearing
// down. Safe to call with the thread already exited on its own (g_synth_running went false first,
// e.g. Ctrl+C) -- pthread_join on an already-finished thread returns immediately either way.
static void hds_standalone_gui_destroy(void) {
    HdsStandaloneGuiWindow *w = g_standalone_gui_window;
    if (!w) return;
    w->should_stop = true;
    pthread_join(w->thread, NULL);
    hds_standalone_gui_destroy_window(w);
    g_standalone_gui_window = NULL;
}
