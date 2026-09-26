// hds_fx_gui_x11.c: the ONLY platform-specific file in HDS FX's GUI -- owns the actual X11 window,
// the XEmbed handshake, and the event pump. Every drawing/layout/hit-test decision lives in
// hds_fx_gui_draw.c instead (a plain `cairo_t*`-based file with zero Xlib references) -- this file's
// entire job is: turn X11 events into calls on that file's own hds_fx_gui_on_*() functions, and turn
// a CLAP host's periodic timer tick into a call to hds_fx_gui_render(). A future Windows port is a
// new file with this same shape (HWND + Win32 message pump + cairo_win32_surface_create(), see
// hds_fx_gui_draw.c's own header comment) calling the exact same draw file, untouched.
//
// #include'd by hds_fx_plugin.c right after hds_fx_gui_draw.c (needs HdsFxGuiViewState/
// hds_fx_gui_render/hds_fx_gui_on_*, all defined there) and after HdsFxPlugin itself (this file
// reads p->host to talk to the CLAP host's own timer-support extension).
//
// XEmbed: this is the minimal client-side handshake (github.com/free-audio/clap's own gui.h points
// at the spec, freedesktop's xembed-spec) that's sufficient for every mainstream Linux host tested
// against similar plugins in practice -- set _XEMBED_INFO (version 0, XEMBED_MAPPED) on our own
// window, then XReparentWindow() it into the host-supplied XID from set_parent(). We do NOT
// implement the full two-way XEmbed message protocol (focus-in/out, activate/deactivate forwarding)
// -- a real, scoped follow-up once basic embedding + rendering + input is proven, not attempted here
// so this stays a manageable, well-understood first real version rather than a from-scratch XEmbed
// stack.
//
// Event pump: driven by CLAP's own clap_host_timer_support_t (register_timer/unregister_timer) --
// the host calls our on_timer() at roughly 30Hz (see show()/hide() below), and each tick we drain
// every currently-queued X11 event with XPending()/XNextEvent() (non-blocking: never waits for a
// NEW event, just processes whatever already arrived) before repainting once. A future
// clap_host_posix_fd_support_t path (register our X11 connection's own fd, get woken only when
// there's genuinely something to read, rather than polling ~30 times a second even while idle) is a
// real efficiency follow-up, not required for this to be a real, correctly-behaving embedded GUI --
// timer-support alone is what a first working version needs, and it's broadly supported (REAPER,
// Bitwig, and most others all implement it).
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h> // XLookupString -- see hds_fx_gui_pump_events()'s own KeyPress case
#include <X11/keysym.h> // XK_BackSpace/XK_Return/XK_KP_Enter/XK_Escape
#include <cairo/cairo-xlib.h>
#include <dirent.h>   // opendir/readdir/closedir -- see hds_fx_gui_browser_refresh()
#include <sys/stat.h> // stat/S_ISDIR/S_ISREG -- see hds_fx_gui_browser_refresh()
#include <strings.h>  // strcasecmp -- case-insensitive *.nam/*.wav filtering and sorting
#include <stdlib.h>   // getenv/qsort

typedef struct {
    HdsFxPlugin *plugin;
    Display *display;
    int screen;
    Window win;
    bool parented;
    cairo_surface_t *surface; // the real, on-screen Xlib surface -- touched exactly once per frame
    cairo_t *cr;
    // Off-screen backbuffer (a plain in-memory image surface) every frame renders into first --
    // see hds_fx_gui_redraw()'s own comment for why drawing dozens of separate primitives directly
    // against the live X11 window surface (what this file did originally) is what caused visible
    // flicker: each fill/stroke/paint call was its own separate request the X server executed
    // against on-screen pixels immediately, so the window visibly redrew element-by-element, every
    // ~33ms. Rendering to this backbuffer instead and blitting it to the window in ONE paint call
    // per frame makes the on-screen update atomic from the server's point of view.
    cairo_surface_t *backbuffer;
    cairo_t *backbuffer_cr;
    HdsFxGuiViewState view;
    clap_id timer_id;
    bool timer_registered;
    bool visible;
} HdsFxGuiWindow;

// (Re)creates the backbuffer at the window's own current size -- called on create() and on every
// resize (set_size()/a ConfigureNotify that actually changed size), always immediately after the
// on-screen surface itself is resized, so the two are never out of sync.
static void hds_fx_gui_resize_backbuffer(HdsFxGuiWindow *w) {
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    w->backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w->view.window_w, w->view.window_h);
    w->backbuffer_cr = cairo_create(w->backbuffer);
}

static const clap_host_timer_support_t *hds_fx_gui_host_timers(HdsFxPlugin *p) {
    if (!p->host || !p->host->get_extension) return NULL;
    return (const clap_host_timer_support_t *)p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
}

// _XEMBED_INFO: {version, flags} -- version 0 (the spec's own baseline), flags bit 0 =
// XEMBED_MAPPED (we want to be mapped as soon as the embedder shows us). Setting this property is
// the one piece of XEmbed a client is expected to publish before being reparented -- see this file's
// own header comment for what's deliberately NOT implemented beyond this.
static void hds_fx_gui_set_xembed_info(Display *dpy, Window win) {
    Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    unsigned long data[2] = {0, 1}; // version 0, XEMBED_MAPPED
    XChangeProperty(dpy, win, xembed_info, xembed_info, 32, PropModeReplace, (unsigned char *)data, 2);
}

// ---------------------------------------------------------------------------------------------
// In-house file browser directory listing for FX_TYPE_NAM's Model/IR pills -- see HdsFxBrowser's
// own comment in hds_fx_gui_draw.c for why this replaced an earlier zenity/kdialog subprocess
// approach (real DAW testing found that approach unresponsive on more than one real host, even
// after two rounds of hardening its failure-recovery heuristics). This is the ONE place that
// actually touches the filesystem (opendir/readdir/stat) -- hds_fx_gui_draw.c only ever reads the
// plain-data results this fills in, keeping it free of any OS call, same discipline that already
// kept Xlib types out of that file. A future Windows port only needs its own FindFirstFile/
// FindNextFile-based version of this ONE function; every render/hit-test function around
// HdsFxBrowser stays untouched.
// ---------------------------------------------------------------------------------------------
typedef struct { char name[HDS_FX_BROWSER_NAME_LEN]; bool is_dir; } HdsFxBrowserSortEntry;

static int hds_fx_browser_sort_cmp(const void *a, const void *b) {
    const HdsFxBrowserSortEntry *ea = (const HdsFxBrowserSortEntry *)a;
    const HdsFxBrowserSortEntry *eb = (const HdsFxBrowserSortEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; // directories before files
    return strcasecmp(ea->name, eb->name);
}

// Case-insensitive "does this filename end in this extension" check -- the ONE place this
// decision is made, shared by the directory listing below AND the path bar's own commit-attempt
// (see hds_fx_gui_browser_refresh()'s own comment on path_bar_commit_attempt): without this, the
// listing was already filtered to only ever show a *.nam/*.wav file, but typing or pasting an
// exact path into the path bar and pressing Enter bypassed that filter entirely -- any existing
// regular file, any extension (or none), got accepted as if it had been a real pick from the
// list. Requiring the same extension match there too means the path bar can never be used to load
// something the browser itself would never have shown in the first place.
static bool hds_fx_path_has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    return nlen >= elen && strcasecmp(name + nlen - elen, ext) == 0;
}

// Called every timer tick (a no-op unless needs_refresh is set -- opening the browser, navigating
// into a directory or Up, or committing a typed path all set it, see hds_fx_gui_draw.c's own
// browser functions). Never blocks the UI thread on anything slow (a real directory read is fast
// on local disk, but this same non-blocking-poll shape is what would let a future version move the
// actual I/O to a worker thread for a slow/network path without touching any caller).
static void hds_fx_gui_browser_refresh(HdsFxGuiWindow *w) {
    HdsFxBrowser *b = &w->view.browser;
    if (!b->open || !b->needs_refresh) return;
    b->needs_refresh = false;

    // Enter was pressed on the path bar: try the typed text as an exact FILE path first (same
    // effect as clicking a file entry), before ever trying it as a directory to list. Requires the
    // same extension match the listing itself already enforces (hds_fx_path_has_ext()'s own
    // comment) -- an existing regular file with the WRONG extension (or none) is deliberately left
    // to fall through to the directory-listing attempt below, which will then simply fail to
    // opendir() it and fall back to home/"/", same as any other bad typed path.
    if (b->path_bar_commit_attempt) {
        b->path_bar_commit_attempt = false;
        const char *ext = (b->field == 0) ? ".nam" : ".wav";
        struct stat st;
        if (hds_fx_path_has_ext(b->dir, ext) && stat(b->dir, &st) == 0 && S_ISREG(st.st_mode)) {
            char *dst = (b->field == 0) ? w->plugin->fx[b->row].nam_model_path : w->plugin->fx[b->row].nam_ir_path;
            snprintf(dst, SAMPLE_PATH_LEN, "%s", b->dir);
            b->open = false;
            return;
        }
    }

    DIR *d = b->dir[0] ? opendir(b->dir) : NULL;
    if (!d) {
        // The requested directory doesn't exist/isn't readable (or was never set at all, e.g. the
        // very first open with no existing path to derive one from) -- fall back to a sane
        // default rather than showing a dead end with no way to navigate anywhere.
        const char *home = getenv("HOME");
        if (home && (d = opendir(home))) snprintf(b->dir, sizeof(b->dir), "%s", home);
        else if ((d = opendir("/"))) snprintf(b->dir, sizeof(b->dir), "/");
    }
    b->entry_count = 0;
    b->scroll = 0;
    if (!d) return; // nothing listable at all -- the path bar and Close are still available

    const char *ext = (b->field == 0) ? ".nam" : ".wav";
    HdsFxBrowserSortEntry sort_buf[HDS_FX_BROWSER_MAX_ENTRIES];
    int n = 0;
    struct dirent *de;
    while (n < HDS_FX_BROWSER_MAX_ENTRIES && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (de->d_name[0] == '.') continue; // skip hidden entries
        char full[SAMPLE_PATH_LEN + HDS_FX_BROWSER_NAME_LEN + 2];
        if (strcmp(b->dir, "/") == 0) snprintf(full, sizeof(full), "/%s", de->d_name);
        else snprintf(full, sizeof(full), "%s/%s", b->dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue; // e.g. a broken symlink -- just skip it
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !hds_fx_path_has_ext(de->d_name, ext)) continue; // filtered out
        snprintf(sort_buf[n].name, sizeof(sort_buf[n].name), "%s", de->d_name);
        sort_buf[n].is_dir = is_dir;
        n++;
    }
    closedir(d);
    qsort(sort_buf, (size_t)n, sizeof(sort_buf[0]), hds_fx_browser_sort_cmp);
    for (int i = 0; i < n; i++) {
        hds_fx_safe_copy(b->entry_name[i], sizeof(b->entry_name[i]), sort_buf[i].name);
        b->entry_is_dir[i] = sort_buf[i].is_dir;
    }
    b->entry_count = n;
    snprintf(b->last_valid_dir, sizeof(b->last_valid_dir), "%s", b->dir);
}

static void hds_fx_gui_redraw(HdsFxGuiWindow *w) {
    if (!w->cr || !w->backbuffer_cr) return;
    hds_fx_gui_render(w->backbuffer_cr, w->plugin, &w->view); // draw the whole frame off-screen first
    cairo_surface_flush(w->backbuffer);
    cairo_set_source_surface(w->cr, w->backbuffer, 0, 0);     // then blit it to the real window --
    cairo_paint(w->cr);                                       // ONE operation, no visible flicker
    cairo_surface_flush(w->surface);
    XFlush(w->display);
}

static void hds_fx_gui_pump_events(HdsFxGuiWindow *w) {
    while (XPending(w->display) > 0) {
        XEvent ev;
        XNextEvent(w->display, &ev);
        switch (ev.type) {
            case ButtonPress:
                if (ev.xbutton.button == 4) hds_fx_gui_on_scroll(&w->view, 1);
                else if (ev.xbutton.button == 5) hds_fx_gui_on_scroll(&w->view, -1);
                else {
                    // Claim keyboard focus on every real click, not just when a NAM path field is
                    // hit -- a reparented (XEmbed-lite) child window doesn't receive KeyPress events
                    // just because it selected for them; the X server delivers keys to whichever
                    // window currently HAS input focus, and a host embedding us has no reason to
                    // hand that over on its own (the full two-way XEmbed protocol, which would make
                    // this automatic, is this file's own documented, separate follow-up). Harmless
                    // to call unconditionally: this is the only window in the process that ever
                    // wants keyboard input in the first place.
                    XSetInputFocus(w->display, w->win, RevertToParent, CurrentTime);
                    hds_fx_gui_on_button_press(w->plugin, &w->view, (double)ev.xbutton.x, (double)ev.xbutton.y, ev.xbutton.button == 3 ? 3 : 1);
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button != 4 && ev.xbutton.button != 5) hds_fx_gui_on_button_release(&w->view);
                break;
            case MotionNotify:
                hds_fx_gui_on_motion(w->plugin, &w->view, (double)ev.xmotion.x, (double)ev.xmotion.y);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != w->view.window_w || ev.xconfigure.height != w->view.window_h) {
                    hds_fx_gui_on_resize(&w->view, ev.xconfigure.width, ev.xconfigure.height);
                    cairo_xlib_surface_set_size(w->surface, ev.xconfigure.width, ev.xconfigure.height);
                    hds_fx_gui_resize_backbuffer(w);
                }
                break;
            case KeyPress: {
                // The only thing keyboard input can ever reach in this GUI is FX_TYPE_NAM's own
                // Model/IR path text fields (see hds_fx_gui_on_key()'s own comment) -- no other
                // control here takes typed input at all, so there's no broader keyboard-focus/
                // navigation model to build, just this one translation from a real X11 KeyPress to
                // the platform-agnostic action hds_fx_gui_draw.c itself understands.
                char text[8];
                KeySym keysym = NoSymbol;
                int n = XLookupString(&ev.xkey, text, sizeof(text) - 1, &keysym, NULL);
                if (keysym == XK_BackSpace) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_BACKSPACE, 0);
                else if (keysym == XK_Return || keysym == XK_KP_Enter) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ENTER, 0);
                else if (keysym == XK_Escape) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ESCAPE, 0);
                else if (n > 0) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_CHAR, text[0]);
                break;
            }
            default: break;
        }
    }
}

static void hds_fx_gui_on_timer(const clap_plugin_t *plugin, clap_id timer_id) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)p->gui_window;
    if (!w || timer_id != w->timer_id) return;
    hds_fx_gui_pump_events(w);
    hds_fx_gui_browser_refresh(w);
    hds_fx_gui_redraw(w);
}

static const clap_plugin_timer_support_t k_timer_support_ext = { .on_timer = hds_fx_gui_on_timer };

static bool hds_fx_gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void)plugin;
    return !is_floating && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

static bool hds_fx_gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void)plugin;
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

static bool hds_fx_gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    if (is_floating || strcmp(api, CLAP_WINDOW_API_X11) != 0) return false;
    if (p->gui_window) return true; // already created -- idempotent, matches other plugins' own posture

    HdsFxGuiWindow *w = (HdsFxGuiWindow *)calloc(1, sizeof(HdsFxGuiWindow));
    if (!w) return false;
    w->plugin = p;
    w->display = XOpenDisplay(NULL);
    if (!w->display) { fprintf(stderr, "[hds-fx] gui create: XOpenDisplay failed (no X11 display available)\n"); free(w); return false; }
    w->screen = DefaultScreen(w->display);
    hds_fx_gui_view_state_init(&w->view);
    w->timer_id = 0;
    w->timer_registered = false;
    w->visible = false;
    w->parented = false;

    w->win = XCreateSimpleWindow(w->display, RootWindow(w->display, w->screen), 0, 0,
                                  (unsigned int)w->view.window_w, (unsigned int)w->view.window_h, 0,
                                  BlackPixel(w->display, w->screen), BlackPixel(w->display, w->screen));
    XSelectInput(w->display, w->win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask);
    // Without this, the X server auto-clears the window to its background pixel on its own (a
    // resize, an Expose from the window manager, ...) independently of our own ~30Hz redraw --
    // a second, compounding source of visible flicker beyond the multi-primitive-direct-to-window
    // drawing hds_fx_gui_redraw() itself already fixed. With None, only OUR OWN backbuffer blit
    // ever touches this window's pixels.
    XSetWindowBackgroundPixmap(w->display, w->win, None);
    hds_fx_gui_set_xembed_info(w->display, w->win);

    w->surface = cairo_xlib_surface_create(w->display, w->win, DefaultVisual(w->display, w->screen), w->view.window_w, w->view.window_h);
    w->cr = cairo_create(w->surface);
    hds_fx_gui_resize_backbuffer(w);

    p->gui_window = w;
    return true;
}

static void hds_fx_gui_destroy_internal(HdsFxPlugin *p) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)p->gui_window;
    if (!w) return;
    if (w->timer_registered) {
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(p);
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

static void hds_fx_gui_destroy(const clap_plugin_t *plugin) {
    hds_fx_gui_destroy_internal((HdsFxPlugin *)plugin->plugin_data);
}

static bool hds_fx_gui_set_scale(const clap_plugin_t *plugin, double scale) { (void)plugin; (void)scale; return false; }

static bool hds_fx_gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)((HdsFxPlugin *)plugin->plugin_data)->gui_window;
    if (!w) return false;
    *width = (uint32_t)w->view.window_w;
    *height = (uint32_t)w->view.window_h;
    return true;
}

static bool hds_fx_gui_can_resize(const clap_plugin_t *plugin) { (void)plugin; return true; }

static bool hds_fx_gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    (void)plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = 0;
    hints->aspect_ratio_height = 0;
    return true;
}

static bool hds_fx_gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void)plugin;
    if (*width < HDS_FX_GUI_MIN_W) *width = HDS_FX_GUI_MIN_W;
    if (*height < HDS_FX_GUI_MIN_H) *height = HDS_FX_GUI_MIN_H;
    return true;
}

static bool hds_fx_gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)((HdsFxPlugin *)plugin->plugin_data)->gui_window;
    if (!w) return false;
    hds_fx_gui_on_resize(&w->view, (int)width, (int)height);
    XResizeWindow(w->display, w->win, (unsigned int)w->view.window_w, (unsigned int)w->view.window_h);
    cairo_xlib_surface_set_size(w->surface, w->view.window_w, w->view.window_h);
    hds_fx_gui_resize_backbuffer(w);
    return true;
}

static bool hds_fx_gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)((HdsFxPlugin *)plugin->plugin_data)->gui_window;
    if (!w || strcmp(window->api, CLAP_WINDOW_API_X11) != 0) return false;
    XReparentWindow(w->display, w->win, (Window)window->x11, 0, 0);
    w->parented = true;
    XFlush(w->display);
    return true;
}

static bool hds_fx_gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) { (void)plugin; (void)window; return false; }
static void hds_fx_gui_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }

static bool hds_fx_gui_show(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)p->gui_window;
    if (!w) return false;
    XMapWindow(w->display, w->win);
    XFlush(w->display);
    w->visible = true;
    if (!w->timer_registered) {
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(p);
        if (timers && timers->register_timer(p->host, 33, &w->timer_id)) w->timer_registered = true;
        else fprintf(stderr, "[hds-fx] gui show: host has no timer-support extension -- GUI will not repaint or receive input\n");
    }
    hds_fx_gui_redraw(w);
    return true;
}

static bool hds_fx_gui_hide(const clap_plugin_t *plugin) {
    HdsFxPlugin *p = (HdsFxPlugin *)plugin->plugin_data;
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)p->gui_window;
    if (!w) return false;
    XUnmapWindow(w->display, w->win);
    XFlush(w->display);
    w->visible = false;
    if (w->timer_registered) {
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(p);
        if (timers) timers->unregister_timer(p->host, w->timer_id);
        w->timer_registered = false;
    }
    return true;
}

static const clap_plugin_gui_t k_gui_ext = {
    .is_api_supported = hds_fx_gui_is_api_supported,
    .get_preferred_api = hds_fx_gui_get_preferred_api,
    .create = hds_fx_gui_create,
    .destroy = hds_fx_gui_destroy,
    .set_scale = hds_fx_gui_set_scale,
    .get_size = hds_fx_gui_get_size,
    .can_resize = hds_fx_gui_can_resize,
    .get_resize_hints = hds_fx_gui_get_resize_hints,
    .adjust_size = hds_fx_gui_adjust_size,
    .set_size = hds_fx_gui_set_size,
    .set_parent = hds_fx_gui_set_parent,
    .set_transient = hds_fx_gui_set_transient,
    .suggest_title = hds_fx_gui_suggest_title,
    .show = hds_fx_gui_show,
    .hide = hds_fx_gui_hide,
};
