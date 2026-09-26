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
#include <unistd.h>   // fork/pipe/dup2/close/read/execlp -- see hds_fx_gui_try_open_file_dialog()
#include <sys/wait.h> // waitpid/WIFEXITED/WEXITSTATUS/WNOHANG -- see hds_fx_gui_poll_file_dialog()
#include <fcntl.h>    // open() for /dev/null (redirecting the dialog subprocess's own stderr)

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

    // A pending native file-dialog subprocess (zenity/kdialog) for FX_TYPE_NAM's Model/IR pills --
    // see hds_fx_gui_try_open_file_dialog()'s own comment for why this exists (a real OS file
    // browser, not just typed/pasted text) and hds_fx_gui_poll_file_dialog() for how it's reaped
    // without ever blocking this window's own event pump while the dialog is open.
    // file_dialog_pid == 0 means no dialog is currently pending.
    pid_t file_dialog_pid;
    int file_dialog_read_fd;  // pipe read end the child's stdout was redirected to; -1 when unused
    int file_dialog_row;
    int file_dialog_field;    // 0 = Model, 1 = IR -- matches HDS_FX_KEY_*'s own edit_field convention
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
// Native file dialog for FX_TYPE_NAM's Model/IR pills -- a real OS file browser, not just the
// typed/pasted path entry hds_fx_gui_on_key() provides (that stays as the fallback when neither
// tool below is installed, see hds_fx_gui_poll_file_dialog()'s own comment). This plugin links
// neither GTK nor Qt (see this file's own header comment on staying a plain Cairo+X11 build with
// zero toolkit dependency) -- zenity/kdialog are launched as ordinary SEPARATE executables via
// plain fork()/execlp(), the same relationship any shell script calling `git` or `ls` has with
// them. Whether either is actually installed is entirely up to the user's own distro/desktop
// environment (GNOME ships zenity, KDE ships kdialog) -- this plugin never bundles, links, or
// redistributes either one, so neither's own license terms (GTK's LGPL, Qt's LGPL/commercial dual
// license) ever attach to this plugin's own binary or distribution.
//
// Deliberately non-blocking: forking and then synchronously waitpid()-ing for the dialog to close
// would freeze this window's own event pump (and this timer callback, which the CLAP host expects
// to return promptly) for however long the user takes to pick a file -- seconds, or however long
// they leave the dialog open. Instead the child's pid and its own stdout (redirected to a pipe) are
// stashed on the HdsFxGuiWindow, and hds_fx_gui_poll_file_dialog() -- called every timer tick,
// right alongside the ordinary event pump -- reaps it via a non-blocking waitpid(..., WNOHANG) once
// it actually exits, exactly like Sequencer audio lanes' own loader-thread convention never blocks
// the audio/UI path for disk I/O either.
static void hds_fx_gui_try_open_file_dialog(HdsFxGuiWindow *w, int row, int field) {
    if (w->file_dialog_pid > 0) return; // one at a time -- a second click while one's already open is a no-op

    int pipefd[2];
    if (pipe(pipefd) != 0) return; // give up silently; the inline text-edit fallback stays active

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return; }

    if (pid == 0) {
        // Child: stdout -> the pipe (so the parent can read back whatever path was chosen), stderr
        // -> /dev/null (neither tool's own diagnostic chatter is anything this plugin's user needs
        // to see). Tries zenity first, then kdialog -- execlp() only returns if THAT SPECIFIC binary
        // couldn't be found/run, so falling through to the next line is exactly "try the next tool",
        // and reaching the final _exit(127) means neither exists on this system at all -- the parent
        // (see hds_fx_gui_poll_file_dialog()) recognizes that exact exit code as "no tool available"
        // and falls back to the inline text-edit field automatically.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        if (field == 0) {
            execlp("zenity", "zenity", "--file-selection", "--title=Select NAM Model", "--file-filter=*.nam", (char *)NULL);
            execlp("kdialog", "kdialog", "--getopenfilename", ".", "*.nam|NAM Model Files (*.nam)", (char *)NULL);
        } else {
            execlp("zenity", "zenity", "--file-selection", "--title=Select Cab IR (WAV)", "--file-filter=*.wav", (char *)NULL);
            execlp("kdialog", "kdialog", "--getopenfilename", ".", "*.wav|Cab IR Files (*.wav)", (char *)NULL);
        }
        _exit(127); // neither tool exists on this system
    }

    // Parent: stash the pid/pipe and let hds_fx_gui_poll_file_dialog() take it from here. Cancel
    // the inline text-edit mode hds_fx_gui_on_button_press() already entered for this same click --
    // see hds_fx_gui_poll_file_dialog()'s own comment for how it comes back automatically if the
    // dialog subprocess turns out not to exist at all.
    close(pipefd[1]);
    w->file_dialog_pid = pid;
    w->file_dialog_read_fd = pipefd[0];
    w->file_dialog_row = row;
    w->file_dialog_field = field;
    w->view.edit_row = -1;
}

// Reaps a pending dialog subprocess once it has actually exited -- a no-op (returns immediately)
// while it's still open, called every timer tick alongside the ordinary event pump so this never
// needs its own separate polling loop.
static void hds_fx_gui_poll_file_dialog(HdsFxGuiWindow *w) {
    if (w->file_dialog_pid <= 0) return;
    int status = 0;
    pid_t r = waitpid(w->file_dialog_pid, &status, WNOHANG);
    if (r == 0) return; // still open -- try again next tick

    char buf[SAMPLE_PATH_LEN];
    ssize_t n = read(w->file_dialog_read_fd, buf, sizeof(buf) - 1);
    close(w->file_dialog_read_fd);
    w->file_dialog_read_fd = -1;
    w->file_dialog_pid = 0;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        // Neither zenity nor kdialog exists on this system -- fall back to exactly the same inline
        // text-edit field a direct pill click opens, pre-filled from whatever's already there, so
        // the feature this replaces never actually regresses on a system without either tool.
        HdsFxPlugin *p = w->plugin;
        const char *cur = (w->file_dialog_field == 0) ? p->fx[w->file_dialog_row].nam_model_path : p->fx[w->file_dialog_row].nam_ir_path;
        w->view.edit_row = w->file_dialog_row;
        w->view.edit_field = w->file_dialog_field;
        snprintf(w->view.edit_buffer, sizeof(w->view.edit_buffer), "%s", cur);
        return;
    }

    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n'); if (nl) *nl = '\0'; // both tools print a trailing newline
        if (buf[0] != '\0') {
            char *dst = (w->file_dialog_field == 0) ? w->plugin->fx[w->file_dialog_row].nam_model_path : w->plugin->fx[w->file_dialog_row].nam_ir_path;
            snprintf(dst, SAMPLE_PATH_LEN, "%s", buf);
        }
        // n<=0 (Cancel was clicked, or the dialog was closed without choosing anything) leaves the
        // field exactly as it was -- same posture as Escape in the inline text-edit fallback.
    }
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
                    int edit_row_before = w->view.edit_row;
                    hds_fx_gui_on_button_press(w->plugin, &w->view, (double)ev.xbutton.x, (double)ev.xbutton.y, ev.xbutton.button == 3 ? 3 : 1);
                    // edit_row going from "not editing" to "editing" means THIS click just opened
                    // the inline text-edit fallback for a NAM Model/IR pill -- try to upgrade it to
                    // a real file dialog (see hds_fx_gui_try_open_file_dialog()'s own comment); if
                    // neither zenity nor kdialog exists, hds_fx_gui_poll_file_dialog() reopens this
                    // exact same inline fallback automatically on the next tick.
                    if (edit_row_before < 0 && w->view.edit_row >= 0) {
                        hds_fx_gui_try_open_file_dialog(w, w->view.edit_row, w->view.edit_field);
                    }
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
    hds_fx_gui_poll_file_dialog(w);
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
    w->file_dialog_pid = 0;
    w->file_dialog_read_fd = -1;

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
    if (w->file_dialog_pid > 0) {
        // The GUI is closing with a dialog subprocess still open (the user hasn't picked a file or
        // hit Cancel yet) -- we deliberately do NOT waitpid() here, since that would block destroy()
        // for however long the dialog stays open. Just close our own read end so we don't leak that
        // fd; the orphaned child is reparented to init and reaped normally on its own exit, same as
        // any other abandoned child process -- it becomes a harmless zombie entry at worst until
        // this plugin's own host process eventually exits, never a hang or a crash here.
        close(w->file_dialog_read_fd);
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
