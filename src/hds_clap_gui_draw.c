// hds_clap_gui_draw.c: hds.clap's own top-level GUI shell -- a persistent left sidebar (see
// hds_clap_global_draw.c's own header comment: the always-visible "Global Configurations" panel,
// a fixed-width column beside the tab area rather than a tab of its own) plus a tab bar (Play,
// Filters, LFOs, Envelopes, FX, Routing, Devices, Recording -- MINUS Mapping and Sequencer, see
// below) with per-tab dispatch. The ONE exception: the sidebar is hidden specifically while the
// FX tab is active (hds_clap_gui_sidebar_w() returns 0 there, HDS_CLAP_GLOBAL_SIDEBAR_W on every
// other tab) -- that tab's own slot list (hds_fx_gui_draw.c, a 10-column table) genuinely needs
// the width back, and Global's own settings aren't something anyone is toggling mid-FX-edit
// anyway. Explicit product direction: an earlier pass made Global its own tab universally (and,
// briefly, Android-only); this is the real, final shape, on every platform alike -- not
// Android-specific (so it needed no HDS_ANDROID_GUI_ENABLED branching restored here at all).
// Platform-agnostic like hds_fx_gui_draw.c itself (plain cairo_t*, no Xlib) -- hds_clap_gui_x11.c
// is this file's own platform shim, same split for the same reason.
//
// Composition: the sidebar occupies x=[0, HDS_CLAP_GLOBAL_SIDEBAR_W), full window height, with NO
// tab bar over it. The tab
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
// idiom as the FX tab. Combi mode's own separate 8-lane mixer view is real too (see
// hds_clap_combi_draw.c), dispatched as a sibling of play_view rather than nested inside it.
//
// Every other listed tab (Filters, LFOs, Envelopes, Routing, Devices, Recording) has its own real
// content now, each in its own draw file. There used to be a PERMANENT Mapping placeholder here
// too (a Sample Zone editor/mapping grid, deliberately out of scope) -- removed entirely once
// native SFZ loading (the Play tab's own per-oscillator "Load .sfz..." button, see
// hds_clap_play_draw.c) made it moot: there's no longer any native-GUI workflow this tab would
// need to serve, and no separate Mapping-tab UI exists anywhere else in this codebase either.
//
// #include'd by clap_plugin.c after hds_fx_gui_draw.c (needs HdsFxGuiViewState/hds_fx_gui_render/
// hds_fx_gui_on_*) and hds_clap_play_draw.c (needs HdsClapPlayViewState), and after
// HdsFxGuiHost/HdsClapPlugin are both defined.

#define HDS_CLAP_TAB_BAR_H 36.0
// 1080 (every tab's own original effective width) + HDS_CLAP_GLOBAL_SIDEBAR_W (260, the sidebar
// added alongside it) -- so no existing tab's own layout math needed to change to make room for
// the sidebar; it's purely additional space to its left. See this file's own header comment.
// Irrelevant on Android (HDS_ANDROID_GUI_ENABLED): the actual window size there always comes from
// the real device screen, never this constant -- left untouched rather than special-cased, since
// nothing on that path ever reads it.
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
#ifdef HDS_STANDALONE_GUI_ENABLED
    HDS_CLAP_TAB_RECORDING,
#endif
    HDS_CLAP_TAB_COUNT
} HdsClapTab;

// Every reference to the sidebar's own reserved width in THIS file (not hds_clap_global_draw.c,
// whose own internal layout keeps using HDS_CLAP_GLOBAL_SIDEBAR_W directly, unchanged) goes
// through this instead: the real sidebar width on every tab except FX, where it's 0 -- that tab's
// own slot list (hds_fx_gui_draw.c, a 10-column table) genuinely needs the width back, and
// Global's own settings aren't something anyone is toggling mid-FX-edit anyway. See this file's
// own header comment. Must come after the HdsClapTab enum above (it takes one as its parameter).
static double hds_clap_gui_sidebar_w(HdsClapTab active_tab) {
    return (active_tab == HDS_CLAP_TAB_FX) ? 0.0 : HDS_CLAP_GLOBAL_SIDEBAR_W;
}

// Most tabs here draw at FIXED absolute pixel positions and never stretch their own content to
// fill whatever tab_area_w they're actually given -- confirmed on a real device: correctly drawn,
// correctly colored, but simply too small, with plain unfilled background (not a clip edge)
// covering the rest of the screen past each tab's own fixed content, because that content was
// never asked to grow into the extra room at all. hds_clap_gui_tab_fill_scale() below is how this
// file (not any individual tab file -- zero changes needed there) fixes that: an ADDITIONAL
// uniform cairo_scale() applied only around the active tab's own content (after the sidebar/
// tab-bar/BAR_H translates, which must stay crisp and tab_area_w-wide themselves), stretching
// that tab's own real content width to actually fill tab_area_w.
//
// Each width below is that tab's own ACTUAL rightmost drawn element (not a single shared "every
// tab is 1150/1080 wide" guess -- confirmed by reading each file's own row/column layout math
// directly, since they differ a lot: Play's 4 oscillator columns stop at ~660, LFOs' own row
// background is an explicit 1080, Devices on CLAP/Android shows only Polyphony+Block Size at
// ~290 -- its device/MIDI/latency pickers are HDS_STANDALONE_GUI_ENABLED-only). FX and Envelopes
// are excluded (return tab_area_w itself, making the scale below always exactly 1.0 for them)
// because their own layouts are already proportional -- FX's own row/column widths are computed
// AS A FRACTION of whatever tab_area_w it's handed (see hds_fx_gui_draw.c's own row-layout
// comment), and Envelopes' own panel_w is literally `window_w - 2*MARGIN` -- scaling either a
// second time here would double them.
static double hds_clap_gui_tab_content_w(HdsClapPlugin *p, HdsClapTab active_tab, double tab_area_w) {
    switch (active_tab) {
        case HDS_CLAP_TAB_FX:
        case HDS_CLAP_TAB_ENVELOPES:
            return tab_area_w;
        case HDS_CLAP_TAB_PLAY:
            // Combi mode is a sibling view (hds_clap_combi_draw.c) with its own, much wider,
            // 8-lane layout (hds_combi_lane_x(7) + HDS_COMBI_LANE_W landing at ~1180) -- NOT
            // Play's own 4-column one (HDS_PLAY_MARGIN + 4*HDS_PLAY_OSC_COL_W + 3*HDS_PLAY_OSC_GAP
            // + a trailing margin, landing at ~660).
            return ((int)p->knobs.performance_mode == 1) ? 1180.0 : 660.0;
        case HDS_CLAP_TAB_FILTERS:
            return 600.0; // per-filter row (type_pill's own right edge) and the 3-band EQ section below it land in the same ~590-600 range
        case HDS_CLAP_TAB_LFOS:
            return 1080.0; // HdsLfoRowLayout.row's own explicit width
        case HDS_CLAP_TAB_ROUTING:
#ifdef HDS_STANDALONE_GUI_ENABLED
            return 1150.0; // standalone's own per-output-pair pill row (OUTPUT_PAIR_MAX wide) needs much more room
#else
            return 560.0; // CLAP/Android: a single mainout_pill + vu_bar only
#endif
        case HDS_CLAP_TAB_DEVICES:
#ifdef HDS_STANDALONE_GUI_ENABLED
            return 450.0; // standalone's own Output/Input/MIDI/Latency pickers
#else
            return 290.0; // CLAP/Android: Polyphony + Block Size only -- device pickers are standalone-only
#endif
#ifdef HDS_STANDALONE_GUI_ENABLED
        case HDS_CLAP_TAB_RECORDING:
            return 540.0; // folder_field + browse_btn's own right edge
#endif
        default:
            return tab_area_w; // not yet individually measured -- a harmless no-stretch fallback, same as FX/Envelopes above
    }
}

// Never shrinks below 1.0: a tab_area_w NARROWER than the fixed design width relies on that tab's
// own existing scroll/clip machinery instead (exactly like vertical overflow already does) --
// squeezing content down to fit would make it just as hard to hit as the dead-space problem this
// exists to fix in the first place.
static double hds_clap_gui_tab_fill_scale(HdsClapPlugin *p, HdsClapTab active_tab, double tab_area_w) {
    double content_w = hds_clap_gui_tab_content_w(p, active_tab, tab_area_w);
    double s = tab_area_w / content_w;
    return (s > 1.0) ? s : 1.0;
}

// Mapping and Sequencer never exist here at all (not even as a placeholder tab): Mapping's own
// native-GUI placeholder was removed once the Play tab's per-oscillator "Load .sfz..." button made
// it moot (see this file's own header comment); Sequencer is 100% standalone-only (the Sequencer
// transport/lane/MIDI-playback workflow -- see clap_plugin_process()'s own comment), per explicit
// product direction -- a DAW host already owns sequencing for its plugins. Recording is likewise
// standalone-only (WavRecorder/the record_enable writer thread), which is why it's conditional on
// HDS_STANDALONE_GUI_ENABLED both in the HdsClapTab enum above and the label array below, rather
// than being excluded outright like Mapping/Sequencer.
// Global is never a tab of its own, on any platform: it lives in the persistent left sidebar,
// alongside the tab bar/content, on every tab except FX (see hds_clap_gui_sidebar_w() above and
// this file's own header comment). hds_clap_global_render()/_on_button_press()/_on_scroll()
// themselves are called completely unchanged either way (not even a parameter differs) -- only
// WHERE this file calls them changes.
static const char *const HDS_CLAP_TAB_LABELS[HDS_CLAP_TAB_COUNT] = {
    "Play", "Filters", "LFOs", "Envelopes", "FX", "Routing", "Devices",
#ifdef HDS_STANDALONE_GUI_ENABLED
    "Recording",
#endif
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
#ifdef HDS_STANDALONE_GUI_ENABLED
    HdsClapRecordingViewState recording_view;  // same convention -- see hds_clap_recording_draw.c's own header comment (standalone-only tab)
#endif
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
#ifdef HDS_STANDALONE_GUI_ENABLED
    hds_clap_recording_view_state_init(&vs->recording_view);
#endif
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
    // set directly whenever the Routing tab becomes/stops being the active tab. Cheap and always
    // correct to just set this on every redraw rather than hooking the tab-bar's own click handler
    // specifically.
    p->knobs.gui_routing_tab_visible = (vs->active_tab == HDS_CLAP_TAB_ROUTING) ? 1.0f : 0.0f;

    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);

    // The persistent left sidebar -- full window height, no tab bar over it (see this file's own
    // header comment). Drawn first, in the untranslated coordinate space (it starts at x=0).
    // Hidden (sidebar_w == 0) specifically while the FX tab is active -- that tab's own slot list
    // genuinely needs the width back.
    if (sidebar_w > 0.0) {
        hds_clap_global_render(cr, p, &vs->global_view, sidebar_w, vs->window_h);
    }

    // Everything else (tab bar + active tab's content) lives to the RIGHT of the sidebar --
    // translate once here so every tab's own render function keeps using plain (0,0)-relative
    // drawing exactly as it did before the sidebar existed, over a tab_area_w that already
    // excludes the sidebar's own width.
    double tab_area_w = vs->window_w - sidebar_w;
    cairo_save(cr);
    cairo_translate(cr, sidebar_w, 0);

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
    // See hds_clap_gui_tab_fill_scale()'s own comment: stretches the active tab's own fixed
    // ~1150-wide design to actually fill tab_area_w instead of leaving dead space past its own
    // right edge. Always exactly 1.0 on FX (a pure no-op there). Every non-FX tab below is called
    // with its OWN NATIVE content width/height (content_w, content_h -- i.e. "as if tab_area_w
    // were exactly 1150 wide", which after this cairo_scale() is now true in device-pixel terms)
    // instead of the real tab_area_w/window_h, exactly mirroring hds_clap_gui_on_button_press()'s
    // own identical fill_scale division on the hit-test side.
    double fill_scale = hds_clap_gui_tab_fill_scale(p, vs->active_tab, tab_area_w);
    double content_w = hds_clap_gui_tab_content_w(p, vs->active_tab, tab_area_w);
    double content_h = (vs->window_h - HDS_CLAP_TAB_BAR_H) / fill_scale;
    // Scoped to its own save/restore, NOT the outer one -- the shared file browser drawn just
    // below is sized/positioned against vs->fx_view.window_w/h (real, un-fill-scaled dimensions,
    // set by hds_clap_gui_on_resize()) regardless of which tab opened it, so it must render in
    // the SAME un-scaled coordinate space those dimensions assume, never through this tab's own
    // extra zoom.
    cairo_save(cr);
    cairo_scale(cr, fill_scale, fill_scale);
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
            if (combi_mode) hds_clap_combi_render(cr, p, &vs->combi_view, content_w, content_h);
            else hds_clap_play_render(cr, p, &vs->play_view, content_w, content_h);
            HdsFxRect single_r, combi_r;
            hds_clap_play_mode_toggle_rects(&single_r, &combi_r);
            hds_fx_draw_pill(cr, single_r.x, single_r.y, single_r.w, single_r.h, "Single", !combi_mode, false);
            hds_fx_draw_pill(cr, combi_r.x, combi_r.y, combi_r.w, combi_r.h, "Combi", combi_mode, false);
            break;
        }
        case HDS_CLAP_TAB_FILTERS:
            hds_clap_filters_render(cr, p, &vs->filters_view, content_w, content_h);
            break;
        case HDS_CLAP_TAB_LFOS:
            hds_clap_lfos_render(cr, p, &vs->lfos_view, content_w, content_h);
            break;
        case HDS_CLAP_TAB_ENVELOPES:
            hds_clap_envelopes_render(cr, p, &vs->envelopes_view, content_w, content_h);
            break;
        case HDS_CLAP_TAB_ROUTING:
            hds_clap_routing_render(cr, p, &vs->routing_view, content_w, content_h);
            break;
        case HDS_CLAP_TAB_DEVICES:
            hds_clap_devices_render(cr, p, &vs->devices_view, content_w, content_h);
            break;
#ifdef HDS_STANDALONE_GUI_ENABLED
        case HDS_CLAP_TAB_RECORDING:
            hds_clap_recording_render(cr, p, &vs->recording_view, content_w, content_h);
            break;
#endif
        default:
            hds_clap_gui_render_placeholder(cr, content_w, content_h, "Coming soon");
            break;
    }
    cairo_restore(cr); // closes the fill_scale save just above -- back to the un-scaled coordinate space for the browser below
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
    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);
    if (vs->fx_view.browser.open) {
        return hds_fx_gui_on_button_press(&p->gui_host, &vs->fx_view, x - sidebar_w, y - HDS_CLAP_TAB_BAR_H, button);
    }
    // The persistent sidebar owns the whole left strip, full height -- checked FIRST, before the
    // tab bar's own y<BAR_H check, since the sidebar has no tab bar of its own at all (see this
    // file's own header comment). Hidden on the FX tab (sidebar_w == 0 there), so this check
    // naturally never matches any real coordinate while that tab is active.
    if (sidebar_w > 0.0 && x < sidebar_w) {
        return hds_clap_global_on_button_press(p, &vs->global_view, &vs->fx_view, vs->window_h, x, y, button);
    }
    double tab_x = x - sidebar_w;
    double tab_area_w = vs->window_w - sidebar_w;
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
    // See hds_clap_gui_tab_fill_scale()'s own comment (and hds_clap_gui_render()'s identical
    // divide-by-fill_scale treatment, which this mirrors exactly): every non-FX tab's own content
    // now renders stretched to fill tab_area_w, so hit-testing it has to divide back down into
    // that tab's own NATIVE (pre-stretch) coordinate space first, or every click would land
    // somewhere other than what the user is actually looking at under their finger.
    double fill_scale = hds_clap_gui_tab_fill_scale(p, vs->active_tab, tab_area_w);
    double content_w = hds_clap_gui_tab_content_w(p, vs->active_tab, tab_area_w);
    double content_tab_x = tab_x / fill_scale;
    double content_py = (y - HDS_CLAP_TAB_BAR_H) / fill_scale;
    double content_h = (vs->window_h - HDS_CLAP_TAB_BAR_H) / fill_scale;
    switch (vs->active_tab) {
        case HDS_CLAP_TAB_FX:
            return hds_fx_gui_on_button_press(&p->gui_host, &vs->fx_view, tab_x, y - HDS_CLAP_TAB_BAR_H, button); // fill_scale always 1.0 here
        case HDS_CLAP_TAB_PLAY: {
            HdsFxRect single_r, combi_r;
            hds_clap_play_mode_toggle_rects(&single_r, &combi_r);
            if (hds_play_rect_contains(&single_r, content_tab_x, content_py)) { if (button == 1) p->knobs.performance_mode = 0.0f; return true; }
            if (hds_play_rect_contains(&combi_r, content_tab_x, content_py)) { if (button == 1) p->knobs.performance_mode = 1.0f; return true; }
            if ((int)p->knobs.performance_mode == 1)
                return hds_clap_combi_on_button_press(p, &vs->combi_view, content_h, content_tab_x, content_py, button);
            return hds_clap_play_on_button_press(p, &vs->play_view, &vs->fx_view, content_w, content_h, content_tab_x, content_py, button);
        }
        case HDS_CLAP_TAB_FILTERS:
            return hds_clap_filters_on_button_press(p, &vs->filters_view, content_w, content_h, content_tab_x, content_py, button);
        case HDS_CLAP_TAB_LFOS:
            return hds_clap_lfos_on_button_press(p, &vs->lfos_view, content_w, content_h, content_tab_x, content_py, button);
        case HDS_CLAP_TAB_ENVELOPES:
            return hds_clap_envelopes_on_button_press(p, &vs->envelopes_view, content_w, content_h, content_tab_x, content_py, button);
        case HDS_CLAP_TAB_ROUTING:
            return hds_clap_routing_on_button_press(p, &vs->routing_view, content_tab_x, content_py, button);
        case HDS_CLAP_TAB_DEVICES:
            return hds_clap_devices_on_button_press(p, &vs->devices_view, content_h, content_tab_x, content_py, button);
#ifdef HDS_STANDALONE_GUI_ENABLED
        case HDS_CLAP_TAB_RECORDING:
            return hds_clap_recording_on_button_press(p, &vs->recording_view, &vs->fx_view, content_tab_x, content_py, button);
#endif
        default:
            return true;
    }
}

static void hds_clap_gui_on_motion(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y) {
    // Unconditional, not gated by x/active_tab: a drag that started in the sidebar (or the tab
    // area) must keep receiving motion even if the pointer strays outside that region afterward --
    // each handler already no-ops on its own if it has no drag in flight (same pattern every other
    // per-tab on_motion in this dispatch already relies on). Harmless to call even on the FX tab,
    // where the sidebar isn't shown at all: there's no drag in flight on it to continue.
    hds_clap_global_on_motion(&vs->global_view, x, y);

    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);
    double tab_x = x - sidebar_w;
    double tab_area_w = vs->window_w - sidebar_w;
    // See hds_clap_gui_on_button_press()'s own identical comment -- same divide-by-fill_scale
    // treatment, kept in sync with it and with hds_clap_gui_render()'s own matching cairo_scale().
    double fill_scale = hds_clap_gui_tab_fill_scale(p, vs->active_tab, tab_area_w);
    double content_w = hds_clap_gui_tab_content_w(p, vs->active_tab, tab_area_w);
    double content_tab_x = tab_x / fill_scale;
    double content_py = (y - HDS_CLAP_TAB_BAR_H) / fill_scale;
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_motion(&p->gui_host, &vs->fx_view, tab_x, y - HDS_CLAP_TAB_BAR_H); // fill_scale always 1.0 here
    else if (vs->active_tab == HDS_CLAP_TAB_PLAY) {
        if ((int)p->knobs.performance_mode == 1) hds_clap_combi_on_motion(&vs->combi_view, content_tab_x, content_py);
        else hds_clap_play_on_motion(p, &vs->play_view, content_w, content_tab_x, content_py);
    }
    else if (vs->active_tab == HDS_CLAP_TAB_FILTERS) hds_clap_filters_on_motion(&vs->filters_view, content_tab_x, content_py);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_motion(&vs->lfos_view, content_tab_x, content_py);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_motion(p, &vs->envelopes_view, content_w, content_tab_x, content_py);
    else if (vs->active_tab == HDS_CLAP_TAB_ROUTING) hds_clap_routing_on_motion(&vs->routing_view, content_tab_x, content_py);
    else if (vs->active_tab == HDS_CLAP_TAB_DEVICES) hds_clap_devices_on_motion(&vs->devices_view, content_tab_x, content_py);
#ifdef HDS_STANDALONE_GUI_ENABLED
    else if (vs->active_tab == HDS_CLAP_TAB_RECORDING) hds_clap_recording_on_motion(&vs->recording_view, content_tab_x, content_py);
#endif
}

static void hds_clap_gui_on_button_release(HdsClapPlugin *p, HdsClapGuiViewState *vs) {
    // Same "unconditional, each side no-ops on its own" reasoning as on_motion just above.
    hds_clap_global_on_button_release(&vs->global_view);
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_button_release(&p->gui_host, &vs->fx_view);
    else if (vs->active_tab == HDS_CLAP_TAB_PLAY) { hds_clap_play_on_button_release(&vs->play_view); hds_clap_combi_on_button_release(&vs->combi_view); }
    else if (vs->active_tab == HDS_CLAP_TAB_FILTERS) hds_clap_filters_on_button_release(&vs->filters_view);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_button_release(&vs->lfos_view);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_button_release(&vs->envelopes_view);
    else if (vs->active_tab == HDS_CLAP_TAB_ROUTING) hds_clap_routing_on_button_release(&vs->routing_view);
    else if (vs->active_tab == HDS_CLAP_TAB_DEVICES) hds_clap_devices_on_button_release(&vs->devices_view);
#ifdef HDS_STANDALONE_GUI_ENABLED
    else if (vs->active_tab == HDS_CLAP_TAB_RECORDING) hds_clap_recording_on_button_release(&vs->recording_view);
#endif
}

static void hds_clap_gui_on_key(HdsClapPlugin *p, HdsClapGuiViewState *vs, int action, char ch) {
    // The shared file browser's own path bar needs key events regardless of active_tab too -- same
    // "modal owns everything" reasoning as hds_clap_gui_on_button_press()'s own browser check.
    if (vs->fx_view.browser.open || vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_key(&p->gui_host, &vs->fx_view, action, ch);
}

static void hds_clap_gui_on_scroll(HdsClapPlugin *p, HdsClapGuiViewState *vs, double x, double y, int delta) {
    if (vs->fx_view.browser.open) { hds_fx_gui_on_scroll(&p->gui_host, &vs->fx_view, delta); return; }
    double sidebar_w = hds_clap_gui_sidebar_w(vs->active_tab);
    // Hidden on the FX tab (sidebar_w == 0 there), so this naturally never matches while it's active.
    if (sidebar_w > 0.0 && x < sidebar_w) { hds_clap_global_on_scroll(p, &vs->global_view, vs->window_h, delta); return; }
    if (y < HDS_CLAP_TAB_BAR_H) return;
    // See hds_clap_gui_on_button_press()'s own identical comment -- content_h keeps this tab's own
    // max-scroll math in the same NATIVE (pre-fill_scale) units its content_h-vs-content-height
    // comparison already assumes.
    double tab_area_w = vs->window_w - sidebar_w;
    double fill_scale = hds_clap_gui_tab_fill_scale(p, vs->active_tab, tab_area_w);
    double content_h = (vs->window_h - HDS_CLAP_TAB_BAR_H) / fill_scale;
    if (vs->active_tab == HDS_CLAP_TAB_FX) hds_fx_gui_on_scroll(&p->gui_host, &vs->fx_view, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_PLAY && (int)p->knobs.performance_mode != 1)
        hds_clap_play_on_scroll(&vs->play_view, hds_play_program(p), content_h, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_LFOS) hds_clap_lfos_on_scroll(&vs->lfos_view, content_h, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_ENVELOPES) hds_clap_envelopes_on_scroll(&vs->envelopes_view, content_h, delta);
    else if (vs->active_tab == HDS_CLAP_TAB_ROUTING) hds_clap_routing_on_scroll(&vs->routing_view, content_h, delta);
#ifdef HDS_STANDALONE_GUI_ENABLED
    else if (vs->active_tab == HDS_CLAP_TAB_RECORDING) hds_clap_recording_on_scroll(p, &vs->recording_view, content_h, delta);
#endif
}

static void hds_clap_gui_on_resize(HdsClapGuiViewState *vs, int w, int h) {
    if (w < HDS_CLAP_GUI_MIN_W) w = HDS_CLAP_GUI_MIN_W;
    if (h < HDS_CLAP_GUI_MIN_H) h = HDS_CLAP_GUI_MIN_H;
    vs->window_w = w;
    vs->window_h = h;
    hds_fx_gui_on_resize(&vs->fx_view, w, h - (int)HDS_CLAP_TAB_BAR_H);
}
