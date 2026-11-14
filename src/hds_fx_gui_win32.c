// hds_fx_gui_win32.c: the Windows counterpart of hds_fx_gui_x11.c -- owns the actual Win32 window,
// the SetParent-based embedding into the host's own HWND, and the Win32 message pump. Every
// drawing/layout/hit-test decision still lives in hds_fx_gui_draw.c (a plain `cairo_t*`-based file
// with zero platform references) -- this file's entire job is: turn Win32 messages into calls on
// that file's own hds_fx_gui_on_*() functions, and turn a CLAP host's periodic timer tick into a
// call to hds_fx_gui_render(). Exactly the shape hds_fx_gui_x11.c's own header comment predicted
// ("a new file with this same shape... calling the exact same draw file, untouched") -- verified:
// this file never edits hds_fx_gui_draw.c.
//
// #include'd right after hds_fx_gui_draw.c (needs HdsFxGuiViewState/hds_fx_gui_render/
// hds_fx_gui_on_*, all defined there), guarded by #ifdef _WIN32 in hds_fx_plugin.c so the exact same
// source file builds hds_fx.clap on both platforms (hds_clap_gui_win32.c's own header comment has
// the parallel case for hds.clap). The includer must ALSO define the same two small accessors
// hds_fx_gui_x11.c already requires (hds_fx_gui_host_of/hds_fx_gui_clap_host_of) -- unchanged, this
// file is a drop-in platform-shim swap, nothing about that seam differs by platform.
//
// Embedding: unlike X11's XEmbed (a client-side property + XReparentWindow), Win32 embedding is
// simpler and needs no protocol handshake at all -- create an ordinary top-level window hidden
// (WS_OVERLAPPEDWINDOW, parent NULL) at create() time, matching X11's own "create as a root-window
// child, reparent later" two-step; when set_parent() hands us the host's own HWND, flip our style to
// WS_CHILD (dropping the caption/border/system-menu bits WS_OVERLAPPEDWINDOW implied) and call
// SetParent() -- one Win32 API call, no XEmbed-equivalent spec to partially implement.
//
// Cairo surface: no persistent on-screen surface is kept at all (X11 keeps one, wrapping the
// window's own XID for the whole window's lifetime) -- a cairo_win32_surface_create(HDC) is created
// fresh for each repaint, wrapping a GetDC()'d device context, and destroyed again after that one
// blit (see hds_fx_gui_redraw() below). This sidesteps needing any "resize the on-screen surface"
// call Win32's own GDI has no equivalent for anyway (unlike cairo_xlib_surface_set_size()) --
// the backbuffer image surface (recreated on resize, exactly like X11) is still what
// hds_fx_gui_render() actually draws into either way, so nothing about the rendering path itself
// differs.
//
// Event pump: driven by the exact same clap_host_timer_support_t contract X11 uses (register_timer/
// unregister_timer, ~30Hz) -- each tick drains this window's own queued messages with
// PeekMessageW(..., PM_REMOVE) (non-blocking, filtered to our own HWND) before repainting once,
// mirroring XPending()/XNextEvent()'s own "drain what's already arrived, never block" posture.
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM/GET_WHEEL_DELTA_WPARAM
#include <cairo/cairo-win32.h>
#include <stdlib.h> // calloc/free

#include "hds_fx_gui_browser.h" // hds_fx_gui_browser_refresh(), compiled separately (src/hds_fx_gui_browser.c)

typedef struct {
    HdsFxGuiHost *plugin; // the real, persistent view -- see hds_fx_gui_host_of()'s own comment
    HWND hwnd;
    bool parented;
    // Off-screen backbuffer (a plain in-memory image surface) every frame renders into first -- same
    // one-paint-per-frame anti-flicker discipline hds_fx_gui_x11.c's own comment explains; here it
    // also doubles as the thing that survives a WM_SIZE (the on-screen win32 surface itself is never
    // persisted at all, see this file's own header comment).
    cairo_surface_t *backbuffer;
    cairo_t *backbuffer_cr;
    HdsFxGuiViewState view;
    clap_id timer_id;
    bool timer_registered;
    bool visible;
} HdsFxGuiWindow;

static void hds_fx_gui_resize_backbuffer(HdsFxGuiWindow *w) {
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    w->backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w->view.window_w, w->view.window_h);
    w->backbuffer_cr = cairo_create(w->backbuffer);
}

static const clap_host_timer_support_t *hds_fx_gui_host_timers(const clap_host_t *host) {
    if (!host || !host->get_extension) return NULL;
    return (const clap_host_timer_support_t *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
}

// In-house file browser directory listing (opendir/readdir/stat) for FX_TYPE_NAM's Model/IR pills --
// hds_fx_gui_browser.c (#include'd above), reused verbatim -- see that file's own header comment.

static void hds_fx_gui_redraw(HdsFxGuiWindow *w) {
    if (!w->backbuffer_cr) return;
    hds_fx_gui_render(w->backbuffer_cr, w->plugin, &w->view); // draw the whole frame off-screen first
    cairo_surface_flush(w->backbuffer);
    HDC hdc = GetDC(w->hwnd);
    if (!hdc) return;
    cairo_surface_t *win_surface = cairo_win32_surface_create(hdc);
    cairo_t *cr = cairo_create(win_surface);
    cairo_set_source_surface(cr, w->backbuffer, 0, 0); // then blit it to the real window --
    cairo_paint(cr);                                   // ONE operation, no visible flicker
    cairo_destroy(cr);
    cairo_surface_destroy(win_surface);
    ReleaseDC(w->hwnd, hdc);
}

static LRESULT CALLBACK hds_fx_gui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // we always repaint the whole window ourselves -- see hds_fx_gui_redraw()
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps); // just validates the update region -- the periodic timer tick
            EndPaint(hwnd, &ps);   // (not WM_PAINT) is what actually repaints, matching X11's own
            return 0;              // "drain Expose, let the timer redraw" posture (see header comment)
        }
        case WM_LBUTTONDOWN:
            if (w) { SetFocus(hwnd); hds_fx_gui_on_button_press(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp), 1); }
            return 0;
        case WM_RBUTTONDOWN:
            if (w) hds_fx_gui_on_button_press(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp), 3);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (w) hds_fx_gui_on_button_release(w->plugin, &w->view);
            return 0;
        case WM_MOUSEMOVE:
            if (w) hds_fx_gui_on_motion(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEWHEEL:
            if (w) hds_fx_gui_on_scroll(w->plugin, &w->view, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            return 0;
        case WM_SIZE:
            if (w) {
                int width = LOWORD(lp), height = HIWORD(lp);
                if (width > 0 && height > 0 && (width != w->view.window_w || height != w->view.window_h)) {
                    hds_fx_gui_on_resize(&w->view, width, height);
                    hds_fx_gui_resize_backbuffer(w);
                }
            }
            return 0;
        case WM_KEYDOWN:
            // Same scope as hds_fx_gui_x11.c's own KeyPress case: the only thing keyboard input can
            // ever reach in this GUI is FX_TYPE_NAM's own Model/IR path text fields.
            if (w) {
                if (wp == VK_BACK) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_BACKSPACE, 0);
                else if (wp == VK_RETURN) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ENTER, 0);
                else if (wp == VK_ESCAPE) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ESCAPE, 0);
            }
            return 0;
        case WM_CHAR:
            if (w && wp >= 0x20 && wp < 0x7f) hds_fx_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_CHAR, (char)wp);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static const wchar_t *const HDS_FX_WNDCLASS_NAME = L"HdsFxClapGuiWindow";

static void hds_fx_gui_register_class_once(void) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = hds_fx_gui_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = HDS_FX_WNDCLASS_NAME;
    RegisterClassExW(&wc);
    registered = true;
}

static void hds_fx_gui_pump_events(HdsFxGuiWindow *w) {
    MSG msg;
    while (PeekMessageW(&msg, w->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void hds_fx_gui_on_timer(const clap_plugin_t *plugin, clap_id timer_id) {
    HdsFxGuiHost *host = hds_fx_gui_host_of(plugin);
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)host->gui_window;
    if (!w || timer_id != w->timer_id) return;
    hds_fx_gui_pump_events(w);
    hds_fx_gui_browser_refresh(&w->view.browser);
    hds_fx_gui_redraw(w);
}

static const clap_plugin_timer_support_t k_timer_support_ext = { .on_timer = hds_fx_gui_on_timer };

static bool hds_fx_gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void)plugin;
    return !is_floating && strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}

static bool hds_fx_gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void)plugin;
    *api = CLAP_WINDOW_API_WIN32;
    *is_floating = false;
    return true;
}

static bool hds_fx_gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    HdsFxGuiHost *host = hds_fx_gui_host_of(plugin);
    if (is_floating || strcmp(api, CLAP_WINDOW_API_WIN32) != 0) return false;
    if (host->gui_window) return true; // already created -- idempotent, matches other plugins' own posture

    HdsFxGuiWindow *w = (HdsFxGuiWindow *)calloc(1, sizeof(HdsFxGuiWindow));
    if (!w) return false;
    w->plugin = host;
    hds_fx_gui_view_state_init(&w->view);
    w->timer_id = 0;
    w->timer_registered = false;
    w->visible = false;
    w->parented = false;

    hds_fx_gui_register_class_once();
    // Created hidden, top-level, unparented -- mirrors XCreateSimpleWindow's own "root-window child,
    // reparented later" two-step (see this file's own header comment on embedding).
    w->hwnd = CreateWindowExW(0, HDS_FX_WNDCLASS_NAME, L"HDS FX", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, w->view.window_w, w->view.window_h,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!w->hwnd) { fprintf(stderr, "[hds-fx] gui create: CreateWindowExW failed\n"); free(w); return false; }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    hds_fx_gui_resize_backbuffer(w);

    host->gui_window = w;
    return true;
}

static void hds_fx_gui_destroy_internal(const clap_plugin_t *plugin) {
    HdsFxGuiHost *host = hds_fx_gui_host_of(plugin);
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)host->gui_window;
    if (!w) return;
    if (w->timer_registered) {
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(hds_fx_gui_clap_host_of(plugin));
        if (timers) timers->unregister_timer(hds_fx_gui_clap_host_of(plugin), w->timer_id);
    }
    if (w->backbuffer_cr) cairo_destroy(w->backbuffer_cr);
    if (w->backbuffer) cairo_surface_destroy(w->backbuffer);
    if (w->hwnd) DestroyWindow(w->hwnd);
    free(w);
    host->gui_window = NULL;
}

static void hds_fx_gui_destroy(const clap_plugin_t *plugin) {
    hds_fx_gui_destroy_internal(plugin);
}

static bool hds_fx_gui_set_scale(const clap_plugin_t *plugin, double scale) { (void)plugin; (void)scale; return false; }

static bool hds_fx_gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)hds_fx_gui_host_of(plugin)->gui_window;
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
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)hds_fx_gui_host_of(plugin)->gui_window;
    if (!w) return false;
    hds_fx_gui_on_resize(&w->view, (int)width, (int)height);
    SetWindowPos(w->hwnd, NULL, 0, 0, w->view.window_w, w->view.window_h, SWP_NOMOVE | SWP_NOZORDER);
    hds_fx_gui_resize_backbuffer(w);
    return true;
}

static bool hds_fx_gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)hds_fx_gui_host_of(plugin)->gui_window;
    if (!w || strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0) return false;
    // Drop WS_OVERLAPPEDWINDOW's own caption/border/system-menu bits and become a real child of the
    // host's own HWND -- see this file's own header comment for why Win32 needs no XEmbed-equivalent
    // handshake beyond this.
    SetWindowLongPtrW(w->hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetParent(w->hwnd, (HWND)window->win32);
    w->parented = true;
    return true;
}

static bool hds_fx_gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) { (void)plugin; (void)window; return false; }
static void hds_fx_gui_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }

static bool hds_fx_gui_show(const clap_plugin_t *plugin) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)hds_fx_gui_host_of(plugin)->gui_window;
    if (!w) return false;
    ShowWindow(w->hwnd, SW_SHOW);
    w->visible = true;
    if (!w->timer_registered) {
        const clap_host_t *chost = hds_fx_gui_clap_host_of(plugin);
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(chost);
        if (timers && timers->register_timer(chost, 33, &w->timer_id)) w->timer_registered = true;
        else fprintf(stderr, "[hds-fx] gui show: host has no timer-support extension -- GUI will not repaint or receive input\n");
    }
    hds_fx_gui_redraw(w);
    return true;
}

static bool hds_fx_gui_hide(const clap_plugin_t *plugin) {
    HdsFxGuiWindow *w = (HdsFxGuiWindow *)hds_fx_gui_host_of(plugin)->gui_window;
    if (!w) return false;
    ShowWindow(w->hwnd, SW_HIDE);
    w->visible = false;
    if (w->timer_registered) {
        const clap_host_t *chost = hds_fx_gui_clap_host_of(plugin);
        const clap_host_timer_support_t *timers = hds_fx_gui_host_timers(chost);
        if (timers) timers->unregister_timer(chost, w->timer_id);
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
