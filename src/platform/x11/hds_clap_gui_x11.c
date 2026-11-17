// hds_clap_gui_x11.c: hds.clap's own platform shim (X11 window, XEmbed handshake, event pump) --
// same shape/posture as hds_fx_gui_x11.c (see that file's own header comment for the XEmbed/timer
// design rationale, unchanged here), but calling hds_clap_gui_draw.c's own tab-dispatch functions
// instead of hds_fx_gui_draw.c's single-tab ones directly, and using HdsClapGuiViewState instead of
// HdsFxGuiViewState. A dedicated (small) copy rather than a further-genericized hds_fx_gui_x11.c:
// this plugin's own window fundamentally dispatches across tabs, hds_fx.clap's never will, and this
// project's own current scope is finishing what already exists, not building a deeper abstraction
// on spec. hds_fx_gui_browser_refresh() (the one piece of real, nontrivial logic -- an opendir/
// readdir/stat directory listing) is still reused verbatim, not duplicated -- see its own comment
// on why it now takes its host/browser explicitly.
//
// #include'd by clap_plugin.c right after hds_clap_gui_draw.c (needs HdsClapGuiViewState/
// hds_clap_gui_render/hds_clap_gui_on_*) and after HdsClapPlugin is fully defined.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>

#include "gui/hds_fx_gui_browser.h" // hds_fx_gui_browser_refresh(), compiled separately (src/hds_fx_gui_browser.c)

typedef struct {
    HdsClapPlugin *plugin;
    Display *display;
    int screen;
    Window win;
    bool parented;
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_surface_t *backbuffer;
    cairo_t *backbuffer_cr;
    HdsClapGuiViewState view;
    clap_id timer_id;
    bool timer_registered;
    bool visible;
} HdsClapGuiWindow;

static void hds_clap_gui_resize_backbuffer(HdsClapGuiWindow *w) {
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    w->backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w->view.window_w, w->view.window_h);
    w->backbuffer_cr = cairo_create(w->backbuffer);
}

static void hds_clap_gui_set_xembed_info(Display *dpy, Window win) {
    Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    unsigned long data[2] = {0, 1}; // version 0, XEMBED_MAPPED -- see hds_fx_gui_x11.c's own comment
    XChangeProperty(dpy, win, xembed_info, xembed_info, 32, PropModeReplace, (unsigned char *)data, 2);
}

static void hds_clap_gui_redraw(HdsClapGuiWindow *w) {
    if (!w->cr || !w->backbuffer_cr) return;
    hds_clap_gui_render(w->backbuffer_cr, w->plugin, &w->view);
    cairo_surface_flush(w->backbuffer);
    cairo_set_source_surface(w->cr, w->backbuffer, 0, 0);
    cairo_paint(w->cr);
    cairo_surface_flush(w->surface);
    XFlush(w->display);
}

static void hds_clap_gui_pump_events(HdsClapGuiWindow *w) {
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
                    hds_clap_gui_on_resize(w->plugin, &w->view, ev.xconfigure.width, ev.xconfigure.height);
                    cairo_xlib_surface_set_size(w->surface, ev.xconfigure.width, ev.xconfigure.height);
                    hds_clap_gui_resize_backbuffer(w);
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
            default: break;
        }
    }
}

static void hds_clap_gui_on_timer(const clap_plugin_t *plugin, clap_id timer_id) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w || timer_id != w->timer_id) return;
    hds_clap_gui_pump_events(w);
    hds_fx_gui_browser_refresh(&w->view.fx_view.browser); // reused verbatim, see this file's own header comment
    hds_clap_gui_redraw(w);
}

static const clap_plugin_timer_support_t k_timer_support_ext = { .on_timer = hds_clap_gui_on_timer };

static bool hds_clap_gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void)plugin;
    return !is_floating && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

static bool hds_clap_gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void)plugin;
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

static bool hds_clap_gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    if (is_floating || strcmp(api, CLAP_WINDOW_API_X11) != 0) return false;
    if (p->gui_window) return true;

    HdsClapGuiWindow *w = (HdsClapGuiWindow *)calloc(1, sizeof(HdsClapGuiWindow));
    if (!w) return false;
    w->plugin = p;
    w->display = XOpenDisplay(NULL);
    if (!w->display) { fprintf(stderr, "[hds-clap] gui create: XOpenDisplay failed (no X11 display available)\n"); free(w); return false; }
    w->screen = DefaultScreen(w->display);
    hds_clap_gui_view_state_init(&w->view);
    w->timer_id = 0;
    w->timer_registered = false;
    w->visible = false;
    w->parented = false;

    w->win = XCreateSimpleWindow(w->display, RootWindow(w->display, w->screen), 0, 0,
                                  (unsigned int)w->view.window_w, (unsigned int)w->view.window_h, 0,
                                  BlackPixel(w->display, w->screen), BlackPixel(w->display, w->screen));
    XSelectInput(w->display, w->win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask);
    XSetWindowBackgroundPixmap(w->display, w->win, None);
    hds_clap_gui_set_xembed_info(w->display, w->win);

    w->surface = cairo_xlib_surface_create(w->display, w->win, DefaultVisual(w->display, w->screen), w->view.window_w, w->view.window_h);
    w->cr = cairo_create(w->surface);
    hds_clap_gui_resize_backbuffer(w);

    p->gui_window = w;
    return true;
}

static void hds_clap_gui_destroy_internal(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w) return;
    if (w->timer_registered) {
        const clap_host_timer_support_t *timers = (const clap_host_timer_support_t *)
            (p->host && p->host->get_extension ? p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT) : NULL);
        if (timers) timers->unregister_timer(p->host, w->timer_id);
    }
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    if (w->cr) cairo_destroy(w->cr);
    if (w->surface) cairo_surface_destroy(w->surface);
    if (w->display) { XDestroyWindow(w->display, w->win); XCloseDisplay(w->display); }
    free(w);
    p->gui_window = NULL;
}

static void hds_clap_gui_destroy(const clap_plugin_t *plugin) {
    hds_clap_gui_destroy_internal(plugin);
}

static bool hds_clap_gui_set_scale(const clap_plugin_t *plugin, double scale) { (void)plugin; (void)scale; return false; }

static bool hds_clap_gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)((HdsClapPlugin *)plugin->plugin_data)->gui_window;
    // Always write a sane value, even before create() -- a host that queries get_size() ahead of
    // creating the window (to reserve layout space) and doesn't check the boolean return strictly
    // would otherwise read *width/*height uninitialized from its own stack.
    *width = (uint32_t)(w ? w->view.window_w : HDS_CLAP_GUI_DEFAULT_W);
    *height = (uint32_t)(w ? w->view.window_h : HDS_CLAP_GUI_DEFAULT_H);
    return w != NULL;
}

static bool hds_clap_gui_can_resize(const clap_plugin_t *plugin) { (void)plugin; return true; }

static bool hds_clap_gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    (void)plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = 0;
    hints->aspect_ratio_height = 0;
    return true;
}

static bool hds_clap_gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void)plugin;
    if (*width < HDS_CLAP_GUI_MIN_W) *width = HDS_CLAP_GUI_MIN_W;
    if (*height < HDS_CLAP_GUI_MIN_H) *height = HDS_CLAP_GUI_MIN_H;
    return true;
}

static bool hds_clap_gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)((HdsClapPlugin *)plugin->plugin_data)->gui_window;
    if (!w) return false;
    hds_clap_gui_on_resize(w->plugin, &w->view, (int)width, (int)height);
    XResizeWindow(w->display, w->win, (unsigned int)w->view.window_w, (unsigned int)w->view.window_h);
    cairo_xlib_surface_set_size(w->surface, w->view.window_w, w->view.window_h);
    hds_clap_gui_resize_backbuffer(w);
    return true;
}

static bool hds_clap_gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)((HdsClapPlugin *)plugin->plugin_data)->gui_window;
    if (!w || strcmp(window->api, CLAP_WINDOW_API_X11) != 0) return false;
    XReparentWindow(w->display, w->win, (Window)window->x11, 0, 0);
    w->parented = true;
    XFlush(w->display);
    return true;
}

static bool hds_clap_gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) { (void)plugin; (void)window; return false; }
static void hds_clap_gui_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }

static bool hds_clap_gui_show(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w) return false;
    XMapWindow(w->display, w->win);
    XFlush(w->display);
    w->visible = true;
    if (!w->timer_registered) {
        const clap_host_timer_support_t *timers = (const clap_host_timer_support_t *)
            (p->host && p->host->get_extension ? p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT) : NULL);
        if (timers && timers->register_timer(p->host, 33, &w->timer_id)) w->timer_registered = true;
        else fprintf(stderr, "[hds-clap] gui show: host has no timer-support extension -- GUI will not repaint or receive input\n");
    }
    hds_clap_gui_redraw(w);
    return true;
}

static bool hds_clap_gui_hide(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w) return false;
    XUnmapWindow(w->display, w->win);
    XFlush(w->display);
    w->visible = false;
    if (w->timer_registered) {
        const clap_host_timer_support_t *timers = (const clap_host_timer_support_t *)
            (p->host && p->host->get_extension ? p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT) : NULL);
        if (timers) timers->unregister_timer(p->host, w->timer_id);
        w->timer_registered = false;
    }
    return true;
}

static const clap_plugin_gui_t k_gui_ext = {
    .is_api_supported = hds_clap_gui_is_api_supported,
    .get_preferred_api = hds_clap_gui_get_preferred_api,
    .create = hds_clap_gui_create,
    .destroy = hds_clap_gui_destroy,
    .set_scale = hds_clap_gui_set_scale,
    .get_size = hds_clap_gui_get_size,
    .can_resize = hds_clap_gui_can_resize,
    .get_resize_hints = hds_clap_gui_get_resize_hints,
    .adjust_size = hds_clap_gui_adjust_size,
    .set_size = hds_clap_gui_set_size,
    .set_parent = hds_clap_gui_set_parent,
    .set_transient = hds_clap_gui_set_transient,
    .suggest_title = hds_clap_gui_suggest_title,
    .show = hds_clap_gui_show,
    .hide = hds_clap_gui_hide,
};
