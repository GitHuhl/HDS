// hds_clap_gui_win32.c: the Windows counterpart of hds_clap_gui_x11.c -- same shape/posture as
// hds_fx_gui_win32.c (see that file's own header comment for the embedding/timer/cairo-surface
// design rationale, unchanged here), but calling hds_clap_gui_draw.c's own tab-dispatch functions
// instead of hds_fx_gui_draw.c's single-tab ones directly, and using HdsClapGuiViewState instead of
// HdsFxGuiViewState -- exactly the same relationship hds_clap_gui_x11.c already has to
// hds_fx_gui_x11.c on Linux. hds_fx_gui_browser_refresh() is still reused verbatim (dirent/stat are
// portable via mingw-w64's own headers, no platform-specific browser code needed).
//
// #include'd by clap_plugin.c right after hds_clap_gui_draw.c (needs HdsClapGuiViewState/
// hds_clap_gui_render/hds_clap_gui_on_*) and after HdsClapPlugin is fully defined, guarded by
// #ifdef _WIN32 so the exact same clap_plugin.c source builds hds.clap on both platforms.
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM/GET_WHEEL_DELTA_WPARAM
#include <cairo/cairo-win32.h>

#include "hds_fx_gui_browser.h" // hds_fx_gui_browser_refresh(), compiled separately (src/hds_fx_gui_browser.c)

typedef struct {
    HdsClapPlugin *plugin;
    HWND hwnd;
    bool parented;
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

static void hds_clap_gui_redraw(HdsClapGuiWindow *w) {
    if (!w->backbuffer_cr) return;
    hds_clap_gui_render(w->backbuffer_cr, w->plugin, &w->view);
    cairo_surface_flush(w->backbuffer);
    HDC hdc = GetDC(w->hwnd);
    if (!hdc) return;
    cairo_surface_t *win_surface = cairo_win32_surface_create(hdc);
    cairo_t *cr = cairo_create(win_surface);
    cairo_set_source_surface(cr, w->backbuffer, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(win_surface);
    ReleaseDC(w->hwnd, hdc);
}

static LRESULT CALLBACK hds_clap_gui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // we always repaint the whole window ourselves -- see hds_clap_gui_redraw()
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps); // just validates the update region -- the periodic timer tick
            EndPaint(hwnd, &ps);   // (not WM_PAINT) is what actually repaints
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (w) { SetFocus(hwnd); hds_clap_gui_on_button_press(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp), 1); }
            return 0;
        case WM_RBUTTONDOWN:
            if (w) hds_clap_gui_on_button_press(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp), 3);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (w) hds_clap_gui_on_button_release(w->plugin, &w->view);
            return 0;
        case WM_MOUSEMOVE:
            if (w) hds_clap_gui_on_motion(w->plugin, &w->view, (double)GET_X_LPARAM(lp), (double)GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEWHEEL:
            if (w) {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ScreenToClient(hwnd, &pt); // WM_MOUSEWHEEL's own lParam is in SCREEN coordinates, unlike every other mouse message here
                hds_clap_gui_on_scroll(w->plugin, &w->view, (double)pt.x, (double)pt.y, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            }
            return 0;
        case WM_SIZE:
            if (w) {
                int width = LOWORD(lp), height = HIWORD(lp);
                if (width > 0 && height > 0 && (width != w->view.window_w || height != w->view.window_h)) {
                    hds_clap_gui_on_resize(w->plugin, &w->view, width, height);
                    hds_clap_gui_resize_backbuffer(w);
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (w) {
                if (wp == VK_BACK) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_BACKSPACE, 0);
                else if (wp == VK_RETURN) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ENTER, 0);
                else if (wp == VK_ESCAPE) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_ESCAPE, 0);
            }
            return 0;
        case WM_CHAR:
            if (w && wp >= 0x20 && wp < 0x7f) hds_clap_gui_on_key(w->plugin, &w->view, HDS_FX_KEY_CHAR, (char)wp);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static const wchar_t *const HDS_CLAP_WNDCLASS_NAME = L"HdsClapGuiWindow";

static void hds_clap_gui_register_class_once(void) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = hds_clap_gui_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = HDS_CLAP_WNDCLASS_NAME;
    RegisterClassExW(&wc);
    registered = true;
}

static void hds_clap_gui_pump_events(HdsClapGuiWindow *w) {
    MSG msg;
    while (PeekMessageW(&msg, w->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void hds_clap_gui_on_timer(const clap_plugin_t *plugin, clap_id timer_id) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w || timer_id != w->timer_id) return;
    hds_clap_gui_pump_events(w);
    hds_fx_gui_browser_refresh(&w->view.fx_view.browser); // reused verbatim, see hds_clap_gui_x11.c's own comment
    hds_clap_gui_redraw(w);
}

static const clap_plugin_timer_support_t k_timer_support_ext = { .on_timer = hds_clap_gui_on_timer };

static bool hds_clap_gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void)plugin;
    return !is_floating && strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}

static bool hds_clap_gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void)plugin;
    *api = CLAP_WINDOW_API_WIN32;
    *is_floating = false;
    return true;
}

static bool hds_clap_gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    if (is_floating || strcmp(api, CLAP_WINDOW_API_WIN32) != 0) return false;
    if (p->gui_window) return true;

    HdsClapGuiWindow *w = (HdsClapGuiWindow *)calloc(1, sizeof(HdsClapGuiWindow));
    if (!w) return false;
    w->plugin = p;
    hds_clap_gui_view_state_init(&w->view);
    w->timer_id = 0;
    w->timer_registered = false;
    w->visible = false;
    w->parented = false;

    hds_clap_gui_register_class_once();
    w->hwnd = CreateWindowExW(0, HDS_CLAP_WNDCLASS_NAME, L"HDS", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, w->view.window_w, w->view.window_h,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!w->hwnd) { fprintf(stderr, "[hds-clap] gui create: CreateWindowExW failed\n"); free(w); return false; }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
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
    if (w->hwnd) DestroyWindow(w->hwnd);
    free(w);
    p->gui_window = NULL;
}

static void hds_clap_gui_destroy(const clap_plugin_t *plugin) {
    hds_clap_gui_destroy_internal(plugin);
}

static bool hds_clap_gui_set_scale(const clap_plugin_t *plugin, double scale) { (void)plugin; (void)scale; return false; }

static bool hds_clap_gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)((HdsClapPlugin *)plugin->plugin_data)->gui_window;
    if (!w) return false;
    *width = (uint32_t)w->view.window_w;
    *height = (uint32_t)w->view.window_h;
    return true;
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
    SetWindowPos(w->hwnd, NULL, 0, 0, w->view.window_w, w->view.window_h, SWP_NOMOVE | SWP_NOZORDER);
    hds_clap_gui_resize_backbuffer(w);
    return true;
}

static bool hds_clap_gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)((HdsClapPlugin *)plugin->plugin_data)->gui_window;
    if (!w || strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0) return false;
    SetWindowLongPtrW(w->hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetParent(w->hwnd, (HWND)window->win32);
    w->parented = true;
    return true;
}

static bool hds_clap_gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) { (void)plugin; (void)window; return false; }
static void hds_clap_gui_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }

static bool hds_clap_gui_show(const clap_plugin_t *plugin) {
    HdsClapPlugin *p = (HdsClapPlugin *)plugin->plugin_data;
    HdsClapGuiWindow *w = (HdsClapGuiWindow *)p->gui_window;
    if (!w) return false;
    ShowWindow(w->hwnd, SW_SHOW);
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
    ShowWindow(w->hwnd, SW_HIDE);
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
