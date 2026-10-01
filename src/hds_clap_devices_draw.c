// hds_clap_devices_draw.c: the Devices tab. Polyphony (active_voice_limit) and Block Size
// (block_size_frames) are real SynthSharedControls fields that affect this engine's internal
// behavior (voice-stealing ceiling, render-chunking) regardless of host, so they're built
// unconditionally for hds.clap/hds_fx.clap/synth_engine alike.
//
// Everything else on this Devices tab -- ALSA output/input device pickers, the
// MIDI device picker, and Latency (ALSA buffer depth) -- is a standalone-app-only concept a CLAP
// host already owns for its own plugins (device selection, buffer size, MIDI routing), so it's
// compiled in only for synth_engine's own --gui, guarded by HDS_STANDALONE_GUI_ENABLED (defined
// only inside hds_standalone.c's own compilation, never clap_plugin.c/hds_fx_plugin.c -- see that
// macro's own comment in hds_standalone.c). Every one of these fields (output_device/input_device/
// midi_device/audio_latency_ms) is a plain SynthSharedControls field the standalone's own main
// loop already polls every block and reopens the affected stream on a live change (see
// hds_standalone.c's own "Defensive: an empty device name..." comment) -- exactly the same
// plain-field-write convention every other knob in this GUI already uses, no new plumbing needed.
//
// Block Size uses a fixed preset list (32/64/128/256/512/1024 samples) rather than a free-drag
// knob -- unlike Polyphony's own free integer range, a fixed small preset set is exactly the kind
// of control this GUI already renders as a dropdown pill (Type/Slope/Waveform/Mode, ...) elsewhere.

#define HDS_DEV_DROPDOWN_NONE 0
#define HDS_DEV_DROPDOWN_BLOCKSIZE 1
#define HDS_DEV_DROPDOWN_OUTPUT 2
#define HDS_DEV_DROPDOWN_INPUT 3
#define HDS_DEV_DROPDOWN_MIDI 4
#define HDS_DEV_DROPDOWN_LATENCY 5

#define HDS_DEV_BLOCKSIZE_COUNT 6
static const int HDS_DEV_BLOCKSIZE_VALUES[HDS_DEV_BLOCKSIZE_COUNT] = {32, 64, 128, 256, 512, 1024};

#ifdef HDS_STANDALONE_GUI_ENABLED
// Six presets spanning the real usable range: Ultra/Extra Snappy trade safety margin for the
// lowest latency -- worth pairing either with a smaller Block Size (below) on hardware/drivers
// that can actually sustain it, see the warning this file renders under the Latency pill whenever
// one of those two is selected. Safe/Extra Safe trade the reverse way for marginal, occasional CPU
// overruns. See audio_latency_ms's own comment in backend.c for what this control actually does.
#define HDS_DEV_LATENCY_COUNT 6
static const char *const HDS_DEV_LATENCY_LABELS[HDS_DEV_LATENCY_COUNT] = {
    "Ultra Snappy (5ms)", "Extra Snappy (10ms)", "Snappy (15ms)", "Normal (25ms)", "Safe (50ms)", "Extra Safe (100ms)",
};
static const float HDS_DEV_LATENCY_VALUES[HDS_DEV_LATENCY_COUNT] = {5.0f, 10.0f, 15.0f, 25.0f, 50.0f, 100.0f};

static int hds_dev_latency_index(float ms) {
    int best = 3; float best_d = 1e9f; // default to Normal (25ms) if nothing matches exactly
    for (int i = 0; i < HDS_DEV_LATENCY_COUNT; i++) {
        float d = fabsf(HDS_DEV_LATENCY_VALUES[i] - ms);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}
#endif

typedef struct {
    int dropdown_kind; // HDS_DEV_DROPDOWN_*
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
#ifdef HDS_STANDALONE_GUI_ENABLED
    // Scanned once at GUI startup and again on "Rescan Devices" -- a real subprocess spawn+read
    // (see hds_scan_alsa_devices()/hds_scan_midi_devices() in hds_standalone.c), never per-frame.
    bool devices_scanned;
    HdsDeviceList output_devices, input_devices, midi_devices;
#endif
} HdsClapDevicesViewState;

static void hds_clap_devices_view_state_init(HdsClapDevicesViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_DEV_MARGIN 10.0
#define HDS_DEV_CONTENT_TOP 34.0
#define HDS_DEV_KNOB_R 10.0

typedef struct {
    HdsFxRect polyphony_knob, blocksize_pill;
#ifdef HDS_STANDALONE_GUI_ENABLED
    HdsFxRect output_pill, input_pill, rescan_btn, midi_pill, latency_pill;
#endif
} HdsDevicesLayout;

static void hds_clap_devices_compute_layout(HdsDevicesLayout *out) {
    memset(out, 0, sizeof(*out));
    out->polyphony_knob = (HdsFxRect){HDS_DEV_MARGIN + 60.0, HDS_DEV_CONTENT_TOP + HDS_DEV_KNOB_R + 12.0, HDS_DEV_KNOB_R, HDS_DEV_KNOB_R};
    out->blocksize_pill = (HdsFxRect){HDS_DEV_MARGIN + 160.0, HDS_DEV_CONTENT_TOP + 12.0, 100.0, 18.0};
#ifdef HDS_STANDALONE_GUI_ENABLED
    double dev_top = HDS_DEV_CONTENT_TOP + HDS_DEV_KNOB_R * 2.0 + 60.0;
    out->output_pill = (HdsFxRect){HDS_DEV_MARGIN + 60.0, dev_top, 220.0, 18.0};
    out->input_pill = (HdsFxRect){HDS_DEV_MARGIN + 60.0, dev_top + 26.0, 220.0, 18.0};
    out->rescan_btn = (HdsFxRect){HDS_DEV_MARGIN + 290.0, dev_top, 110.0, 18.0};
    double midi_top = dev_top + 74.0;
    out->midi_pill = (HdsFxRect){HDS_DEV_MARGIN + 60.0, midi_top, 220.0, 18.0};
    double lat_top = midi_top + 36.0;
    out->latency_pill = (HdsFxRect){HDS_DEV_MARGIN + 60.0, lat_top, 160.0, 18.0};
#endif
}

static int hds_dev_blocksize_index(int value) {
    for (int i = 0; i < HDS_DEV_BLOCKSIZE_COUNT; i++) if (HDS_DEV_BLOCKSIZE_VALUES[i] == value) return i;
    return 2; // 128, matching the backend's own default
}

// Generic string-list dropdown renderer, shared by Output/Input/MIDI (each backed by a real,
// runtime-scanned HdsDeviceList rather than a fixed preset array like Block Size/Latency above).
// `display()` lets MIDI substitute "Auto (recommended)" for the literal stored value "auto" at
// index 0 (see hds_scan_midi_devices()'s own comment) without the list itself needing two parallel
// arrays.
#ifdef HDS_STANDALONE_GUI_ENABLED
static const char *hds_dev_midi_display(const char *raw) { return strcmp(raw, "auto") == 0 ? "Auto (recommended)" : raw; }

static void hds_dev_render_string_dropdown(cairo_t *cr, const HdsFxRect *anchor, double window_h, const HdsDeviceList *list, const char *current, bool is_midi) {
    double row_h = 16.0;
    double list_h = list->count * row_h;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    HdsFxRect bg = {anchor->x, y0, 260.0, list_h};
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    for (int i = 0; i < list->count; i++) {
        bool cur = (strcmp(list->names[i], current) == 0);
        HdsFxRect r = {bg.x, bg.y + i * row_h, bg.w, row_h};
        if (cur) {
            hds_fx_rgb(cr, 0.22, 0.32, 0.36);
            cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
        }
        hds_fx_rgb(cr, cur ? 0.92 : 0.78, cur ? 0.95 : 0.78, cur ? 0.92 : 0.81);
        hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.0, is_midi ? hds_dev_midi_display(list->names[i]) : list->names[i]);
    }
}

// Returns true (and writes *out_target) if (x,y) hit a real row inside this dropdown's own
// background rect (recomputed identically to the render function above); false for a miss.
static bool hds_dev_string_dropdown_hit_test(const HdsFxRect *anchor, double window_h, const HdsDeviceList *list, double x, double y, int *out_index) {
    double row_h = 16.0;
    double list_h = list->count * row_h;
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    HdsFxRect bg = {anchor->x, y0, 260.0, list_h};
    if (!hds_play_rect_contains(&bg, x, y)) return false;
    int i = (int)((y - bg.y) / row_h);
    if (i < 0 || i >= list->count) return false;
    *out_index = i;
    return true;
}
#endif

static void hds_clap_devices_render(cairo_t *cr, HdsClapPlugin *p, HdsClapDevicesViewState *vs, double window_w, double window_h) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

#ifdef HDS_STANDALONE_GUI_ENABLED
    if (!vs->devices_scanned) {
        hds_scan_alsa_devices("aplay", &vs->output_devices);
        hds_scan_alsa_devices("arecord", &vs->input_devices);
        hds_scan_midi_devices(&vs->midi_devices);
        vs->devices_scanned = true;
    }
#endif

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
    bool bs_dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_BLOCKSIZE);
    hds_fx_draw_pill(cr, L.blocksize_pill.x, L.blocksize_pill.y, L.blocksize_pill.w, L.blocksize_pill.h, bstxt, bs_dd_open, false);

#ifdef HDS_STANDALONE_GUI_ENABLED
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.output_pill.y - 8.0, "Audio Devices");

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.output_pill.y + L.output_pill.h / 2 + 3.0, "Output:");
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.input_pill.y + L.input_pill.h / 2 + 3.0, "Input:");

    bool out_dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_OUTPUT);
    bool in_dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_INPUT);
    hds_fx_draw_pill(cr, L.output_pill.x, L.output_pill.y, L.output_pill.w, L.output_pill.h, p->knobs.output_device, out_dd_open, false);
    hds_fx_draw_pill(cr, L.input_pill.x, L.input_pill.y, L.input_pill.w, L.input_pill.h, p->knobs.input_device, in_dd_open, false);
    hds_fx_draw_pill(cr, L.rescan_btn.x, L.rescan_btn.y, L.rescan_btn.w, L.rescan_btn.h, "Rescan Devices", false, false);

    hds_fx_rgb(cr, 0.6, 0.6, 0.63);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.midi_pill.y - 8.0, "MIDI Input");
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.midi_pill.y + L.midi_pill.h / 2 + 3.0, "Device:");
    bool midi_dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_MIDI);
    hds_fx_draw_pill(cr, L.midi_pill.x, L.midi_pill.y, L.midi_pill.w, L.midi_pill.h, hds_dev_midi_display(p->knobs.midi_device), midi_dd_open, false);

    hds_fx_left_text(cr, HDS_DEV_MARGIN, L.latency_pill.y + L.latency_pill.h / 2 + 3.0, "Latency:");
    int lat_idx = hds_dev_latency_index(p->knobs.audio_latency_ms);
    bool lat_dd_open = (vs->dropdown_kind == HDS_DEV_DROPDOWN_LATENCY);
    hds_fx_draw_pill(cr, L.latency_pill.x, L.latency_pill.y, L.latency_pill.w, L.latency_pill.h, HDS_DEV_LATENCY_LABELS[lat_idx], lat_dd_open, false);
    if (lat_idx <= 1) { // Ultra/Extra Snappy -- leaves little to no margin above the Block Size itself
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.5);
        hds_fx_rgb(cr, 0.75, 0.55, 0.3);
        hds_fx_left_text(cr, L.latency_pill.x + L.latency_pill.w + 8.0, L.latency_pill.y + L.latency_pill.h / 2 + 3.0,
                          "may glitch on some hardware -- try a smaller Block Size too");
    }
#endif

    // Whichever dropdown (if any) is open draws LAST, on top of everything else.
    if (bs_dd_open) {
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
#ifdef HDS_STANDALONE_GUI_ENABLED
    else if (vs->dropdown_kind == HDS_DEV_DROPDOWN_OUTPUT) {
        hds_dev_render_string_dropdown(cr, &vs->dropdown_anchor, window_h, &vs->output_devices, p->knobs.output_device, false);
    } else if (vs->dropdown_kind == HDS_DEV_DROPDOWN_INPUT) {
        hds_dev_render_string_dropdown(cr, &vs->dropdown_anchor, window_h, &vs->input_devices, p->knobs.input_device, false);
    } else if (vs->dropdown_kind == HDS_DEV_DROPDOWN_MIDI) {
        hds_dev_render_string_dropdown(cr, &vs->dropdown_anchor, window_h, &vs->midi_devices, p->knobs.midi_device, true);
    } else if (vs->dropdown_kind == HDS_DEV_DROPDOWN_LATENCY) {
        double list_h = HDS_DEV_LATENCY_COUNT * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        HdsFxRect bg = {vs->dropdown_anchor.x, y0, 160.0, list_h};
        hds_fx_rgb(cr, 0.13, 0.13, 0.16);
        cairo_new_path(cr); cairo_rectangle(cr, bg.x, bg.y, bg.w, bg.h); cairo_fill(cr);
        hds_fx_rgba(cr, 1, 1, 1, 0.25);
        cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, bg.x + 0.5, bg.y + 0.5, bg.w - 1, bg.h - 1); cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5);
        for (int i = 0; i < HDS_DEV_LATENCY_COUNT; i++) {
            HdsFxRect r = {bg.x, bg.y + i * 16.0, bg.w, 16.0};
            if (i == lat_idx) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
            }
            hds_fx_rgb(cr, i == lat_idx ? 0.92 : 0.78, i == lat_idx ? 0.95 : 0.78, i == lat_idx ? 0.92 : 0.81);
            hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.0, HDS_DEV_LATENCY_LABELS[i]);
        }
    }
#endif
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
#ifdef HDS_STANDALONE_GUI_ENABLED
    if (vs->dropdown_kind == HDS_DEV_DROPDOWN_OUTPUT || vs->dropdown_kind == HDS_DEV_DROPDOWN_INPUT || vs->dropdown_kind == HDS_DEV_DROPDOWN_MIDI) {
        const HdsDeviceList *list = (vs->dropdown_kind == HDS_DEV_DROPDOWN_OUTPUT) ? &vs->output_devices
                                   : (vs->dropdown_kind == HDS_DEV_DROPDOWN_INPUT) ? &vs->input_devices : &vs->midi_devices;
        int idx;
        if (hds_dev_string_dropdown_hit_test(&vs->dropdown_anchor, window_h, list, x, y, &idx)) {
            if (button == 1) {
                char *dest = (vs->dropdown_kind == HDS_DEV_DROPDOWN_OUTPUT) ? p->knobs.output_device
                           : (vs->dropdown_kind == HDS_DEV_DROPDOWN_INPUT) ? p->knobs.input_device : p->knobs.midi_device;
                snprintf(dest, AUDIO_DEVICE_NAME_LEN, "%s", list->names[idx]);
            }
        }
        vs->dropdown_kind = HDS_DEV_DROPDOWN_NONE;
        return true;
    }
    if (vs->dropdown_kind == HDS_DEV_DROPDOWN_LATENCY) {
        double list_h = HDS_DEV_LATENCY_COUNT * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        for (int i = 0; i < HDS_DEV_LATENCY_COUNT; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, 160.0, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) p->knobs.audio_latency_ms = HDS_DEV_LATENCY_VALUES[i];
                vs->dropdown_kind = HDS_DEV_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_DEV_DROPDOWN_NONE;
        return true;
    }
#endif

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
#ifdef HDS_STANDALONE_GUI_ENABLED
    if (hds_play_rect_contains(&L.output_pill, x, y)) {
        vs->dropdown_kind = HDS_DEV_DROPDOWN_OUTPUT; vs->dropdown_anchor = L.output_pill;
        return true;
    }
    if (hds_play_rect_contains(&L.input_pill, x, y)) {
        vs->dropdown_kind = HDS_DEV_DROPDOWN_INPUT; vs->dropdown_anchor = L.input_pill;
        return true;
    }
    if (hds_play_rect_contains(&L.rescan_btn, x, y)) {
        if (button == 1) {
            hds_scan_alsa_devices("aplay", &vs->output_devices);
            hds_scan_alsa_devices("arecord", &vs->input_devices);
            hds_scan_midi_devices(&vs->midi_devices);
        }
        return true;
    }
    if (hds_play_rect_contains(&L.midi_pill, x, y)) {
        vs->dropdown_kind = HDS_DEV_DROPDOWN_MIDI; vs->dropdown_anchor = L.midi_pill;
        return true;
    }
    if (hds_play_rect_contains(&L.latency_pill, x, y)) {
        vs->dropdown_kind = HDS_DEV_DROPDOWN_LATENCY; vs->dropdown_anchor = L.latency_pill;
        return true;
    }
#endif

    (void)button;
    return true;
}

static void hds_clap_devices_on_motion(HdsClapDevicesViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

static void hds_clap_devices_on_button_release(HdsClapDevicesViewState *vs) { hds_play_release_knob_drag(&vs->knob_drag, NULL, NULL); }
