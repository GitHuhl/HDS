// test_clap_gui.c -- opens each plugin's native GUI on an X display (Xvfb in tests/run_tests.sh),
// clicks through its tabs with real X11 button events, and checks every view actually rendered:
// the capture isn't blank, and each tab looks different from every other. Each capture is also
// fingerprinted (tests/golden.txt); run_tests.sh pins fontconfig to the vendored DejaVu fonts so
// text renders identically on every machine. Captures are saved as PPM files in <outdir>.
// Usage: test_clap_gui <outdir> <hds.clap> <hds_fx.clap>
#include "test_common.h"
#include "clap_test_host.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>

#define MAX_TABS 8

typedef struct { unsigned w, h; unsigned char *rgb; } Capture;

static Capture capture(Display *d, Window win, unsigned w, unsigned h) {
    Capture c = { w, h, malloc((size_t)w * h * 3) };
    XImage *img = XGetImage(d, win, 0, 0, w, h, AllPlanes, ZPixmap);
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        unsigned long px = XGetPixel(img, (int)x, (int)y); unsigned char *o = &c.rgb[(y * w + x) * 3];
        o[0] = (px >> 16) & 255; o[1] = (px >> 8) & 255; o[2] = px & 255;
    }
    XDestroyImage(img);
    return c;
}

static void save_ppm(const Capture *c, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", c->w, c->h); fwrite(c->rgb, 1, (size_t)c->w * c->h * 3, f); fclose(f);
}

static int distinct_colors_at_least(const Capture *c, int want) {
    unsigned seen[64]; int n = 0;
    for (size_t i = 0; i < (size_t)c->w * c->h && n < want; i += 7) {
        unsigned v = c->rgb[i * 3] << 16 | c->rgb[i * 3 + 1] << 8 | c->rgb[i * 3 + 2]; int dup = 0;
        for (int k = 0; k < n; k++) if (seen[k] == v) { dup = 1; break; }
        if (!dup) seen[n++] = v;
    }
    return n >= want;
}

static void click(Display *d, Window w, const TestPlugin *tp, int x, int y) {
    for (int type = ButtonPress; type <= ButtonRelease; type++) {
        XEvent ev; memset(&ev, 0, sizeof ev);
        ev.xbutton.type = type; ev.xbutton.display = d; ev.xbutton.window = w; ev.xbutton.root = DefaultRootWindow(d);
        ev.xbutton.x = x; ev.xbutton.y = y; ev.xbutton.button = Button1; ev.xbutton.same_screen = True;
        ev.xbutton.state = type == ButtonRelease ? Button1Mask : 0;
        XSendEvent(d, w, True, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &ev); XFlush(d);
        test_plugin_pump_timer(tp, 3);
    }
}

// tabs == 0: no tab bar, capture the initial view only. Otherwise click `tabs` tabs laid out evenly
// across the window from x = tab_x0, centered on y = 17 (hds_clap_gui_draw.c's tab bar).
static void test_gui(Display *d, const char *outdir, const char *label, const char *path, int tabs, double tab_x0) {
    TestPlugin tp; if (test_plugin_load(&tp, path)) { g_test_failures++; return; }
    const clap_plugin_t *p = tp.plugin;
    p->activate(p, 48000, 32, 1024);
    const clap_plugin_gui_t *gui = (const clap_plugin_gui_t *)p->get_extension(p, CLAP_EXT_GUI);
    char name[128];
    snprintf(name, sizeof name, "clap_gui/%s/create", label);
    int created = gui && gui->is_api_supported(p, CLAP_WINDOW_API_X11, false) && gui->create(p, CLAP_WINDOW_API_X11, false);
    test_check(created, name, "gui->create(x11) failed");
    if (!created) { test_plugin_unload(&tp); return; }
    uint32_t w = 0, h = 0; gui->get_size(p, &w, &h);
    Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, w, h, 0, 0, 0);
    XMapWindow(d, parent); XFlush(d);
    clap_window_t cw = { .api = CLAP_WINDOW_API_X11, .x11 = parent };
    gui->set_parent(p, &cw); gui->show(p);
    test_plugin_pump_timer(&tp, 40);
    Window root_ret, parent_ret, *kids = NULL; unsigned nk = 0;
    XQueryTree(d, parent, &root_ret, &parent_ret, &kids, &nk);
    Window child = nk ? kids[0] : parent;
    if (kids) XFree(kids);

    Capture caps[MAX_TABS]; int ncap = tabs ? tabs : 1;
    for (int t = 0; t < ncap; t++) {
        if (tabs) { click(d, child, &tp, (int)(tab_x0 + (w - tab_x0) / tabs * (t + 0.5)), 17); test_plugin_pump_timer(&tp, 15); }
        caps[t] = capture(d, parent, w, h);
        char path_out[512]; snprintf(path_out, sizeof path_out, "%s/%s_tab%d.ppm", outdir, label, t); save_ppm(&caps[t], path_out);
        snprintf(name, sizeof name, "clap_gui/%s/tab%d/rendered", label, t);
        test_check(distinct_colors_at_least(&caps[t], 8), name, "capture looks blank");
        TestHash hh; test_hash_init(&hh); test_hash_bytes(&hh, caps[t].rgb, (size_t)w * h * 3);
        snprintf(name, sizeof name, "clap_gui/%s/tab%d", label, t); test_hash_print(&hh, name);
    }
    if (tabs) {
        int same = 0;
        for (int a = 0; a < ncap; a++) for (int b = a + 1; b < ncap; b++) if (!memcmp(caps[a].rgb, caps[b].rgb, (size_t)w * h * 3)) same++;
        snprintf(name, sizeof name, "clap_gui/%s/tabs_switch", label);
        test_check(same == 0, name, "%d pairs of tabs rendered identically (a tab click did nothing?)", same);
    }
    for (int t = 0; t < ncap; t++) free(caps[t].rgb);
    gui->hide(p); gui->destroy(p); p->deactivate(p);
    XDestroyWindow(d, parent); XFlush(d);
    test_plugin_unload(&tp);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s outdir hds.clap hds_fx.clap\n", argv[0]); return 2; }
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("FAIL clap_gui/display: cannot open X display '%s'\n", getenv("DISPLAY") ? getenv("DISPLAY") : ""); return 1; }
    // hds.clap: 7 tabs (Play, Filters, LFOs, Envelopes, FX, Routing, Devices) to the right of the
    // Global sidebar (HDS_CLAP_GLOBAL_SIDEBAR_W 160 x HDS_CLAP_GUI_CONTENT_BOOST 1.3 = 208 px). The
    // sidebar hides while FX is active, widening the tab bar, but the Routing/Devices click points
    // still land inside those tabs. hds_fx.clap: a single view, no tab bar.
    test_gui(d, argv[1], "hds", argv[2], 7, 208);
    test_gui(d, argv[1], "hds_fx", argv[3], 0, 0);
    XCloseDisplay(d);
    return test_exit_code();
}
