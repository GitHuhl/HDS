// hds_clap_gui_draw.c: hds.clap's own top-level GUI shell -- a persistent left sidebar (see
// hds_clap_global_draw.c's own header comment: frontend.py's always-visible "Global
// Configurations" panel, ported here as a fixed-width column beside the tab area rather than a
// tab of its own) plus a tab bar (mirroring frontend.py's QTabWidget order exactly: Play, Filters,
// LFOs, Envelopes, FX, Routing, Devices, Recording, Mapping, Sequencer) with per-tab dispatch.
// Platform-agnostic like hds_fx_gui_draw.c itself (plain cairo_t*, no Xlib) -- hds_clap_gui_x11.c
// is this file's own platform shim, same split for the same reason.
//
// Composition: the sidebar occupies x=[0, HDS_CLAP_GLOBAL_SIDEBAR_W), full window height, with NO
// tab bar over it (frontend.py's own sidebar sits beside its QTabWidget, not inside it). The tab
// bar and every tab's own content occupy x=[HDS_CLAP_GLOBAL_SIDEBAR_W, window_w) instead -- see
// hds_clap_gui_render()'s own cairo_translate() and every dispatch function's own x-offset
// subtraction. HDS_CLAP_GUI_DEFAULT_W below already includes the sidebar's own width added on top
// of every tab's original effective width (1080), so no existing tab's own layout math needed to
// change to make room for it.
//
// The FX tab is NOT a new implementation: it's hds_fx_gui_draw.c's own hds_fx_gui_render()/
// hds_fx_gui_on_*(), called verbatim against this instance's own HdsFxGuiHost (see clap_plugin.c's
// own comment on gui_host), just drawn/hit-tested through a vertical offset (below this file's own
// tab bar) via a small nested HdsFxGuiViewState -- see hds_clap_gui_render()'s own comment on why a
// plain cairo_translate() plus a shrunk vs.window_h is enough, with zero changes needed to that
// already-shared, already-tested engine.
//
// The Play tab is likewise NOT a new implementation of its own top-level dispatch shape: it's
// hds_clap_play_draw.c's own hds_clap_play_render()/hds_clap_play_on_*(), called against this
// instance's own HdsClapPlugin directly (it needs ProgramState/PackEGParams fields no generic
// HdsFxGuiHost view could expose, unlike the FX tab -- see hds_clap_play_draw.c's own header
// comment on why it isn't shared with hds_fx.clap at all), through the same cairo_translate()
// idiom as the FX tab. Single-Program mode only for now -- Combi mode's own separate 8-lane mixer
// view is real, substantial follow-up work, tracked as its own task.
//
// Every OTHER tab is a placeholder for now (a centered "not yet built" label) -- each becomes real
// content in its own later commit (see the task list this session tracks). Mapping is a
// PERMANENT placeholder for the time being: explicit product decision, not a stand-in for
// unfinished work -- the Sample Zone editor/mapping grid is deliberately out of scope until told
// otherwise.
//
// #include'd by clap_plugin.c after hds_fx_gui_draw.c (needs HdsFxGuiViewState/hds_fx_gui_render/
// hds_fx_gui_on_*) and hds_clap_play_draw.c (needs HdsClapPlayViewState), and after
// HdsFxGuiHost/HdsClapPlugin are both defined.

#define HDS_CLAP_TAB_BAR_H 36.0
// 1080 (every tab's own original effective width) + HDS_CLAP_GLOBAL_SIDEBAR_W (260, the sidebar
// added alongside it) -- so no existing tab's own layout math needed to change to make room for
// the sidebar; it's purely additional space to its left. See this file's own header comment.
#define HDS_CLAP_GUI_DEFAULT_W (1150 + (int)HDS_CLAP_GLOBAL_SIDEBAR_W)
#define HDS_CLAP_GUI_DEFAULT_H 760
#define HDS_CLAP_GUI_MIN_W (740 + (int)HDS_CLAP_GLOBAL_SIDEBAR_W)
#define HDS_CLAP_GUI_MIN_H 420

typedef enum {
    HDS_CLAP_TAB_PLAY = 0,
    HDS_CLAP_TAB_FILTERS,
    HDS_CLAP_TAB_LFOS,
    HDS_CLAP_TAB_ENVELOPES,
    HDS_CLAP_TAB_FX,
    HDS_CLAP_TAB_ROUTING,
    HDS_CLAP_TAB_DEVICES,
    HDS_CLAP_TAB_MAPPING,
    HDS_CLAP_TAB_COUNT
} HdsClapTab;

// Order and labels match frontend.py's own addTab() call sequence (self.tab_widget.addTab calls,
// ~line 5564 onward) -- see this file's own header comment -- MINUS Recording and Sequencer,
// which don't exist here at all (not even as a placeholder tab): both are 100% standalone-only
// (WavRecorder/the record_enable writer thread, and the Sequencer transport/lane/MIDI-playback
// workflow respectively -- see clap_plugin_process()'s own comment on the latter), per explicit
// product direction -- a DAW host already owns both recording and sequencing for its plugins.
// "Global" is NOT a tab here at all (an earlier version of this port made it one) -- it lives in
// the persistent left sidebar instead, alongside the tab bar/content, matching frontend.py's own
// placement exactly. See hds_clap_global_draw.c's own header comment and this file's own header
// comment on how the sidebar and this tab bar compose together.
static const char *const HDS_CLAP_TAB_LABELS[HDS_CLAP_TAB_COUNT] = {
    "Play", "Filters", "LFOs", "Envelopes", "FX", "Routing", "Devices", "Mapping",
};

typedef struct {
    int window_w, window_h;
    HdsClapTab active_tab;

    // The FX tab's own view state -- a genuine, nested HdsFxGuiViewState (same type
    // hds_fx.clap's own single-tab window uses directly), kept in sync with a SHRUNK window_h
    // (real window_h minus the tab bar) by hds_clap_gui_on_resize() so hds_fx_gui_render()'s own
    // background fill/clip rect never draws over or under the tab bar -- see this file's own
    // header comment.
    HdsFxGuiViewState fx_view;

    // The Play tab's own view state (hds_clap_play_draw.c) -- unlike fx_view, it doesn't need its
    // own window_w/window_h copy: every Play-tab function takes this HdsClapGuiViewState's own
    // window_w/window_h directly as plain parameters (see hds_clap_gui_render()'s own dispatch),
    // since hds_clap_play_draw.c has no equivalent of hds_fx_gui_render()'s single-struct-in
    // calling convention to match.
    HdsClapPlayViewState play_view;
    HdsClapCombiViewState combi_view; // the Play tab's own Combi-mode sub-view -- see hds_clap_combi_draw.c's own header comment and this file's own HDS_CLAP_TAB_PLAY dispatch for why it's a sibling of play_view, not nested inside it
    HdsClapFiltersViewState filters_view; // same convention as play_view -- see hds_clap_filters_draw.c's own header comment
    HdsClapLfosViewState lfos_view;       // same convention -- see hds_clap_lfos_draw.c's own header comment
    HdsClapEnvelopesViewState envelopes_view; // same convention -- see hds_clap_envelopes_draw.c's own header comment
    HdsClapRoutingViewState routing_view;     // same convention -- see hds_clap_routing_draw.c's own header comment
    HdsClapDevicesViewState devices_view;      // same convention -- see hds_clap_devices_draw.c's own header comment
    HdsClapGlobalViewState global_view;        // same convention -- see hds_clap_global_draw.c's own header comment
} HdsClapGuiViewState;

static void hds_clap_gui_view_state_init(HdsClapGuiViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->window_w = HDS_CLAP_GUI_DEFAULT_W;
    vs->window_h = HDS_CLAP_GUI_DEFAULT_H;
    vs->active_tab = HDS_CLAP_TAB_PLAY;
    hds_fx_gui_view_state_init(&vs->fx_view);
    vs->fx_view.window_w = vs->window_w;
    vs->fx_view.window_h = vs->window_h - (int)HDS_CLAP_TAB_BAR_H;
    hds_clap_play_view_state_init(&vs->play_view);
    hds_clap_combi_view_state_init(&vs->combi_view);
    hds_clap_filters_view_state_init(&vs->filters_view);
    hds_clap_lfos_view_state_init(&vs->lfos_view);
    hds_clap_envelopes_view_state_init(&vs->envelopes_view);
    hds_clap_routing_view_state_init(&vs->routing_view);
    hds_clap_devices_view_state_init(&vs->devices_view);
    hds_clap_global_view_state_init(&vs->global_view);
}

typedef struct { double x, y, w, h; } HdsClapTabRect;

static void hds_clap_gui_tab_rect(int window_w, int tab_index, HdsClapTabRect *out) {
    double tab_w = (double)window_w / (double)HDS_CLAP_TAB_COUNT;
    out->x = tab_index * tab_w;
    out->y = 0;
    out->w = tab_w;
    out->h = HDS_CLAP_TAB_BAR_H;
}

static void hds_clap_gui_render_placeholder(cairo_t *cr, double w, double h, const char *label) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14.5);
    hds_fx_rgb(cr, 0.45, 0.45, 0.48);
    hds_fx_centered_text(cr, w / 2, h / 2, label);
}

static void hds_clap_gui_render(cairo_t *cr, HdsClapPlugin *p, HdsClapGuiViewState *vs) {
    // Gates the Routing tab's own ext-in VU meters (see HdsClapPlugin.ext_in_peak's own comment) --
    // mirrors frontend.py's own _on_main_tab_changed(), which sends /hds/gui/routing_tab_visible
    // whenever the Routing tab becomes/stops being the active tab. Cheap and always correct to just
    // set this on every redraw rather than hooking the tab-bar's own click handler specifically.
    p->knobs.gui_routing_tab_visible = (vs->active_tab == HDS_CLAP_TAB_ROUTING) ? 1.0f : 0.0f;

    // The persistent left sidebar -- full window height, no tab bar over it (see this file's own
    // header comment). Drawn first, in the untranslated coordinate space (it starts at x=0).
    hds_clap_global_render(cr, p, &vs->global_view, HDS_CLAP_GLOBAL_SIDEBAR_W, vs->window_h);

    // Everything else (tab bar + active tab's content) lives to the RIGHT of the sidebar --
    // translate once here so every tab's own render function keeps using plain (0,0)-relative
    // drawing exactly as it did before the sidebar existed, over a tab_area_w that already
    // excludes the sidebar's own width.
    double tab_area_w = vs->window_w - HDS_CLAP_GLOBAL_SIDEBAR_W;
    cairo_save(cr);
    cairo_translate(cr, HDS_CLAP_GLOBAL_SIDEBAR_W, 0);

    // Tab bar itself.
    hds_fx_rgb(cr, 0.14, 0.14, 0.17);
    cairo_rectangle(cr, 0, 0, tab_area_w, HDS_CLAP_TAB_BAR_H);
    cairo_fill(cr);
    for (int i = 0; i < HDS_CLAP_TAB_COUNT; i++) {
        HdsClapTabRect r;
        hds_clap_gui_tab_rect((int)tab_area_w, i, &r);
        bool active = (i == (int)vs->active_tab);
        if (active) {
            hds_fx_rgb(cr, 0.10, 0.10, 0.12);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_fill(cr);
            hds_fx_rgb(cr, 0.95, 0.55, 0.15);
            cairo_rectangle(cr, r.x, r.y + r.h - 2, r.w, 2);
            cairo_fill(cr);
        }
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, active ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        hds_fx_rgb(cr, active ? 0.92 : 0.6, active ? 0.92 : 0.6, active ? 0.95 : 0.63);
        hds_fx_centered_text(cr, r.x + r.w / 2, r.y + r.h / 2 + 3.5, HDS_CLAP_TAB_LABELS[i]);
    }

    // Active tab's own content, drawn below the bar. The FX tab reuses hds_fx_gui_render()
    // verbatim: cairo_translate() shifts its own (0,0)-relative drawing down past the bar, and
    // vs->fx_view.window_h (kept shrunk by hds_clap_gui_on_resize()) makes its own background
    // fill/clip rect stop exactly at the real window's bottom edge -- no change needed to that
    // file at all.
    cairo_save(cr);
    cairo_rectangle(cr, 0, HDS_CLAP_TAB_BAR_H, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
    cairo_clip(cr);
    cairo_translate(cr, 0, HDS_CLAP_TAB_BAR_H);
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_FX:
            hds_fx_gui_render(cr, &p->gui_host, &vs->fx_view);
            break;
        case HDS_CLAP_TAB_PLAY: {
            // Mode toggle (Single/Combi) lives here, not in either sub-view's own file -- see
            // hds_clap_play_draw.c's own hds_clap_play_mode_toggle_rects() comment for why. Drawn
            // AFTER the sub-view's own render, not before: both hds_clap_play_render() and
            // hds_clap_combi_render() start by filling the ENTIRE tab-content rect as their own
            // background (same "flood-fill background first" convention every tab in this GUI
            // uses), which would otherwise immediately paint over the pills.
            bool combi_mode = ((int)p->knobs.performance_mode == 1);
            if (combi_mode) hds_clap_combi_render(cr, p, &vs->combi_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            else hds_clap_play_render(cr, p, &vs->play_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            HdsFxRect single_r, combi_r;
            hds_clap_play_mode_toggle_rects(&single_r, &combi_r);
            hds_fx_draw_pill(cr, single_r.x, single_r.y, single_r.w, single_r.h, "Single", !combi_mode, false);
            hds_fx_draw_pill(cr, combi_r.x, combi_r.y, combi_r.w, combi_r.h, "Combi", combi_mode, false);
            break;
        }
        case HDS_CLAP_TAB_FILTERS:
            hds_clap_filters_render(cr, p, &vs->filters_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            break;
        case HDS_CLAP_TAB_LFOS:
            hds_clap_lfos_render(cr, p, &vs->lfos_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            break;
        case HDS_CLAP_TAB_ENVELOPES:
            hds_clap_envelopes_render(cr, p, &vs->envelopes_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            break;
        case HDS_CLAP_TAB_ROUTING:
            hds_clap_routing_render(cr, p, &vs->routing_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            break;
        case HDS_CLAP_TAB_DEVICES:
            hds_clap_devices_render(cr, p, &vs->devices_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H);
            break;
        case HDS_CLAP_TAB_MAPPING:
            hds_clap_gui_render_placeholder(cr, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H, "Mapping (Sample Zone editor) -- not available in this GUI yet");
            break;
        default:
            hds_clap_gui_render_placeholder(cr, vs->window_w, vs->window_h - HDS_CLAP_TAB_BAR_H, "Coming soon");
            break;
    }
    // The shared file browser (see hds_fx_gui_draw.c's own HdsFxBrowser comment) draws on top of
    // whichever tab is active, REGARDLESS of active_tab -- it's opened from the FX tab's own NAM
    // pills, but also from the Play tab's own Multisample .sfz loader and the Global sidebar's own
    // Scale .scl loader (neither of which is the FX tab), so it can't be nested inside the FX-only
    // branch above the way it effectively was before this GUI had any OTHER file-picking need.
    if (vs->fx_view.browser.open) hds_fx_gui_render_browser(cr, &vs->fx_view);

    cairo_restore(cr); // closes the inner cairo_save() at this function's own line ~187 (the active-tab clip/translate)
    cairo_restore(cr); // closes the OUTER cairo_save() at this function's own line ~157 (the sidebar-width translate) --
                        // without this second restore, cr's CTM (this is the persistent backbuffer context, reused
                        // every redraw, not recreated per frame -- see hds_clap_gui_x11.c's own hds_clap_gui_redraw())
                        // stays translated by +HDS_CLAP_GLOBAL_SIDEBAR_W after every single call, so the NEXT redraw's
                        // "untranslated" sidebar draw lands further right than the last, and un-cleared pixels from
                        // every previous frame's offset are left behind underneath -- this is exactly what produced
                        // the "panel repeating itself" symptom (a new copy marching rightward every ~33ms).
}

static bool hds_clap_gui_on_button_press(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y, int button) {
    // The shared file browser (see hds_clap_gui_render()'s own comment) is a true modal: while
    // open, it owns every click regardless of where it lands or which tab is actually active --
    // checked FIRST, even before the sidebar. hds_fx_gui_on_button_press() itself already checks
    // vs->browser.open before anything else, so this is safe to call with coordinates that may be
    // negative/out of its own usual range (a click on the sidebar, say) -- it just reads as "outside
    // the panel," which already means "close the browser," exactly the right behavior.
    if (vs->fx_view.browser.open) {
        return hds_fx_gui_on_button_press(&p->gui_host, &vs->fx_view, x - HDS_CLAP_GLOBAL_SIDEBAR_W, y - HDS_CLAP_TAB_BAR_H, button);
    }
    // The persistent sidebar owns the whole left strip, full height -- checked FIRST, before the
    // tab bar's own y<BAR_H check, since the sidebar has no tab bar of its own at all (see this
    // file's own header comment).
    if (x < HDS_CLAP_GLOBAL_SIDEBAR_W) {
        return hds_clap_global_on_button_press(p, &vs->global_view, &vs->fx_view, vs->window_h, x, y, button);
    }
    double tab_x = x - HDS_CLAP_GLOBAL_SIDEBAR_W;
    double tab_area_w = vs->window_w - HDS_CLAP_GLOBAL_SIDEBAR_W;
    if (y < HDS_CLAP_TAB_BAR_H) {
        for (int i = 0; i < HDS_CLAP_TAB_COUNT; i++) {
            HdsClapTabRect r;
            hds_clap_gui_tab_rect((int)tab_area_w, i, &r);
            if (tab_x >= r.x && tab_x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                vs->active_tab = (HdsClapTab)i;
                return true;
            }
        }
        return true; // still consumed -- clicking empty bar space is a no-op, not a click-through
    }
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_FX:
            return hds_fx_gui_on_button_press(&p->gui_host, &vs->fx_view, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        case HDS_CLAP_TAB_PLAY: {
            HdsFxRect single_r, combi_r;
            hds_clap_play_mode_toggle_rects(&single_r, &combi_r);
            double py = y - HDS_CLAP_TAB_BAR_H;
            if (hds_play_rect_contains(&single_r, tab_x, py)) { if (button == 1) p->knobs.performance_mode = 0.0f; return true; }
            if (hds_play_rect_contains(&combi_r, tab_x, py)) { if (button == 1) p->knobs.performance_mode = 1.0f; return true; }
            if ((int)p->knobs.performance_mode == 1)
                return hds_clap_combi_on_button_press(p, &vs->combi_view, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, py, button);
            return hds_clap_play_on_button_press(p, &vs->play_view, &vs->fx_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, py, button);
        }
        case HDS_CLAP_TAB_FILTERS:
            return hds_clap_filters_on_button_press(p, &vs->filters_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        case HDS_CLAP_TAB_LFOS:
            return hds_clap_lfos_on_button_press(p, &vs->lfos_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        case HDS_CLAP_TAB_ENVELOPES:
            return hds_clap_envelopes_on_button_press(p, &vs->envelopes_view, tab_area_w, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        case HDS_CLAP_TAB_ROUTING:
            return hds_clap_routing_on_button_press(p, &vs->routing_view, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        case HDS_CLAP_TAB_DEVICES:
            return hds_clap_devices_on_button_press(p, &vs->devices_view, vs->window_h - HDS_CLAP_TAB_BAR_H, tab_x, y - HDS_CLAP_TAB_BAR_H, button);
        default:
            return true;
    }
}

static void hds_clap_gui_on_motion(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y) {
    // Unconditional, not gated by x/active_tab: a drag that started in the sidebar (or the tab
    // area) must keep receiving motion even if the pointer strays outside that region afterward --
    // each handler already no-ops on its own if it has no drag in flight (same pattern every other
    // per-tab on_motion in this dispatch already relies on).
    hds_clap_global_on_motion(&vs->global_view, x, y);

    double tab_x = x - HDS_CLAP_GLOBAL_SIDEBAR_W;
    double tab_area_w = vs->window_w - HDS_CLAP_GLOBAL_SIDEBAR_W;
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_motion(&p->gui_host, &vs->fx_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
    else if (vs->active_tab == HDS_CLAP_TAB_PLAY) {
        if ((int)p->knobs.performance_mode == 1) hds_clap_combi_on_motion(&vs->combi_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
        else hds_clap_play_on_motion(p, &vs->play_view, tab_area_w, tab_x, y - HDS_CLAP_TAB_BAR_H);
    }
    else if (vs->active_tab == HDS_CLAP_TAB_FILTERS) hds_clap_filters_on_motion(&vs->filters_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_motion(&vs->lfos_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_motion(p, &vs->envelopes_view, tab_area_w, tab_x, y - HDS_CLAP_TAB_BAR_H);
    else if (vs->active_tab == HDS_CLAP_TAB_ROUTING) hds_clap_routing_on_motion(&vs->routing_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
    else if (vs->active_tab == HDS_CLAP_TAB_DEVICES) hds_clap_devices_on_motion(&vs->devices_view, tab_x, y - HDS_CLAP_TAB_BAR_H);
}

static void hds_clap_gui_on_button_release(HdsClapGuiViewState *vs) {
    // Same "unconditional, each side no-ops on its own" reasoning as on_motion just above.
    hds_clap_global_on_button_release(&vs->global_view);
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_button_release(&vs->fx_view);
    else if (vs->active_tab == HDS_CLAP_TAB_PLAY) { hds_clap_play_on_button_release(&vs->play_view); hds_clap_combi_on_button_release(&vs->combi_view); }
    else if (vs->active_tab == HDS_CLAP_TAB_FILTERS) hds_clap_filters_on_button_release(&vs->filters_view);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_button_release(&vs->lfos_view);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_button_release(&vs->envelopes_view);
    else if (vs->active_tab == HDS_CLAP_TAB_ROUTING) hds_clap_routing_on_button_release(&vs->routing_view);
    else if (vs->active_tab == HDS_CLAP_TAB_DEVICES) hds_clap_devices_on_button_release(&vs->devices_view);
}

static void hds_clap_gui_on_key(HdsClapPlugin *p, HdsClapGuiViewState *vs, int action, char ch) {
    // The shared file browser's own path bar needs key events regardless of active_tab too -- same
    // "modal owns everything" reasoning as hds_clap_gui_on_button_press()'s own browser check.
    if (vs->fx_view.browser.open || vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_key(&p->gui_host, &vs->fx_view, action, ch);
}

static void hds_clap_gui_on_scroll(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y, int delta) {
    if (vs->fx_view.browser.open) { hds_fx_gui_on_scroll(&vs->fx_view, delta); return; }
    if (x < HDS_CLAP_GLOBAL_SIDEBAR_W) { hds_clap_global_on_scroll(p, &vs->global_view, vs->window_h, delta); return; }
    if (y < HDS_CLAP_TAB_BAR_H) return;
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_scroll(&vs->fx_view, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_scroll(&vs->lfos_view, vs->window_h - HDS_CLAP_TAB_BAR_H, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_scroll(&vs->envelopes_view, vs->window_h - HDS_CLAP_TAB_BAR_H, delta);
}

static void hds_clap_gui_on_resize(HdsClapGuiViewState *vs, int w, int h) {
    if (w < HDS_CLAP_GUI_MIN_W) w = HDS_CLAP_GUI_MIN_W;
    if (h < HDS_CLAP_GUI_MIN_H) h = HDS_CLAP_GUI_MIN_H;
    vs->window_w = w;
    vs->window_h = h;
    hds_fx_gui_on_resize(&vs->fx_view, w, h - (int)HDS_CLAP_TAB_BAR_H);
}
