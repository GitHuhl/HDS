// hds_clap_devices_draw.c: hds.clap's own reduced "Devices" tab -- just Polyphony
// (active_voice_limit) and Block Size (block_size_frames), the only two real SynthSharedControls
// fields on frontend.py's own Audio/Devices tab that affect this engine's internal behavior
// (voice-stealing ceiling, render-chunking) regardless of host. Every other control on that tab in
// frontend.py -- the ALSA output/input device pickers, the MIDI device picker, and Latency (ALSA
// buffer depth) -- is a standalone-app-only concept a CLAP host already owns for its own plugins
// (device selection, buffer size, MIDI routing), so none of it belongs in this GUI at all; this
// isn't a "not yet built" deferral like every other tab's own scope notes, it's a permanent,
// deliberate reduction (see this repo's own README, "Devices becomes a much smaller 'Performance'
// panel").
//
// Block Size uses a fixed preset list (32/64/128/256/512/1024 samples, frontend.py's own
// _BLOCK_SIZE_PRESETS) rather than a free-drag knob, matching its own QComboBox in frontend.py --
// unlike a SpinBox's free integer range (Polyphony), a fixed small preset set is exactly the kind
// of control this GUI already renders as a dropdown pill (Type/Slope/Waveform/Mode, ...) elsewhere.

#define HDS_DEV_DROPDOWN_NONE 0
#define HDS_DEV_DROPDOWN_BLOCKSIZE 1

#define HDS_DEV_BLOCKSIZE_COUNT 6
static const int HDS_DEV_BLOCKSIZE_VALUES[HDS_DEV_BLOCKSIZE_COUNT] = {32, 64, 128, 256, 512, 1024};

typedef struct {
    int dropdown_kind; // HDS_DEV_DROPDOWN_*
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
} HdsClapDevicesViewState;

static void hds_clap_devices_view_state_init(HdsClapDevicesViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_DEV_MARGIN 10.0
#define HDS_DEV_CONTENT_TOP 34.0
#define HDS_DEV_KNOB_R 10.0

typedef struct {
    HdsFxRect polyphony_knob, blocksize_pill;
} HdsDevicesLayout;

static void hds_clap_devices_compute_layout(HdsDevicesLayout *out) {
    out->polyphony_knob = (HdsFxRect){HDS_DEV_MARGIN + 60.0, HDS_DEV_CONTENT_TOP + HDS_DEV_KNOB_R + 12.0, HDS_DEV_KNOB_R, HDS_DEV_KNOB_R};
    out->blocksize_pill = (HdsFxRect){HDS_DEV_MARGIN + 160.0, HDS_DEV_CONTENT_TOP + 12.0, 100.0, 18.0};
}

static int hds_dev_blocksize_index(int value) {
    for (int i = 0; i < HDS_DEV_BLOCKSIZE_COUNT; i++) if (HDS_DEV_BLOCKSIZE_VALUES[i] == value) return i;
    return 2; // 128, matching the backend's own default
}

static void hds_clap_devices_render(cairo_t *cr, HdsClapPlugin *p, HdsClapDevicesViewState *vs, double window_w, double window_h) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, HDS_DEV_CONTENT_TOP - 8.0, "Performance");

    HdsDevicesLayout L;
    hds_clap_devices_compute_layout(&L);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.polyphony_knob.y + 3.0, "Polyphony:");
    hds_fx_left_text(cr, L.blocksize_pill.x - 66.0, L.blocksize_pill.y + L.blocksize_pill.h / 2 + 3.0, "Block Size:");

    int poly = (int)p->knobs.active_voice_limit;
    char ptxt[16]; snprintf(ptxt, sizeof(ptxt), "%d", poly);
    float pfrac = (float)((poly - 1) / 255.0);
    hds_play_draw_knob(cr, L.polyphony_knob.x, L.polyphony_knob.y, L.polyphony_knob.w, pfrac, false, NULL, ptxt);

    int bs = (int)p->knobs.block_size_frames;
    int bs_idx = hds_dev_blocksize_index(bs);
    char bstxt[24]; snprintf(bstxt, sizeof(bstxt), "%d samples", HDS_DEV_BLOCKSIZE_VALUES[bs_idx]);
    bool dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_BLOCKSIZE);
    hds_fx_draw_pill(cr, L.blocksize_pill.x, L.blocksize_pill.y, L.blocksize_pill.w, L.blocksize_pill.h, bstxt, dd_open, false);

    if (dd_open) {
        double list_h = HDS_DEV_BLOCKSIZE_COUNT * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        HdsFxRect bg = {vs->dropdown_anchor.x, y0, vs->dropdown_anchor.w, list_h};
        hds_fx_rgb(cr, 0.13, 0.13, 0.16);
        cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
        hds_fx_rgba(cr, 1, 1, 1, 0.25);
        cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.0);
        for (int i = 0; i < HDS_DEV_BLOCKSIZE_COUNT; i++) {
            HdsFxRect r = {bg.x, bg.y + i * 16.0, bg.w, 16.0};
            if (i == bs_idx) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
            }
            hds_fx_rgb(cr, i == bs_idx ? 0.92 : 0.78, i == bs_idx ? 0.95 : 0.78, i == bs_idx ? 0.92 : 0.81);
            char opt[24]; snprintf(opt, sizeof(opt), "%d samples", HDS_DEV_BLOCKSIZE_VALUES[i]);
            hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, opt);
        }
    }
}

static bool hds_clap_devices_on_button_press(HdsClapPlugin *p, HdsClapDevicesViewState *vs, double window_h, double x, double y, int button) {
    if (vs->dropdown_kind == HDS_DEV_DROPDOWN_BLOCKSIZE) {
        double list_h = HDS_DEV_BLOCKSIZE_COUNT * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        for (int i = 0; i < HDS_DEV_BLOCKSIZE_COUNT; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, vs->dropdown_anchor.w, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) p->knobs.block_size_frames = (float)HDS_DEV_BLOCKSIZE_VALUES[i];
                vs->dropdown_kind = HDS_DEV_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_DEV_DROPDOWN_NONE;
        return true;
    }

    HdsDevicesLayout L;
    hds_clap_devices_compute_layout(&L);
    if (hds_play_knob_hit_test(&L.polyphony_knob, x, y)) {
        hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.active_voice_limit, 1.0, 256.0, x, y, button, HDS_PLAY_NO_MENU);
        return true;
    }
    if (hds_play_rect_contains(&L.blocksize_pill, x, y)) {
        vs->dropdown_kind = HDS_DEV_DROPDOWN_BLOCKSIZE; vs->dropdown_anchor = L.blocksize_pill;
        return true;
    }

    (void)button;
    return true;
}

static void hds_clap_devices_on_motion(HdsClapDevicesViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

static void hds_clap_devices_on_button_release(HdsClapDevicesViewState *vs) { hds_play_release_knob_drag(&vs->knob_drag, NULL, NULL); }
