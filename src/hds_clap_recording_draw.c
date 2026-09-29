// hds_clap_recording_draw.c: the Recording tab -- standalone-app-only, like the Sequencer tab (see
// hds_clap_gui_draw.c's own header comment on why both are 100% standalone-only: a DAW host already
// owns recording/sequencing for its plugins, so hds.clap has no equivalent tab at all). Ports
// frontend.py's own Recording tab: which sources to capture (the synth's own post-FX stereo output,
// and/or up to RECORD_MAX_CHANNELS raw Input-device channels), Multichannel-WAV vs Stereo-Downmix,
// WAV vs WavPack (WavPack stays disabled here too -- see HDS_WAVPACK_ENABLED in hds_standalone.c,
// same reason frontend.py's own WavPack radio is disabled), an output folder, and the actual
// Record/Stop control with a live elapsed-time/error readout.
//
// This file's own content is ENTIRELY wrapped in #ifdef HDS_STANDALONE_GUI_ENABLED: it reads
// record_engine/record_gather directly (file-scope statics in hds_standalone.c, promoted from
// locals inside main() specifically so this file could reach them -- see their own declaration
// comment), which simply don't exist in clap_plugin.c's own translation unit at all. Still
// #include'd unconditionally from both hosts (same convention as every other hds_clap_*_draw.c
// file) -- in a CLAP build this whole file compiles to nothing.
//
// #include'd by clap_plugin.c/hds_standalone.c after hds_clap_play_draw.c (needs HdsPlayKnobDrag/
// hds_play_rect_contains) and hds_fx_gui_draw.c (needs HdsFxGuiViewState/hds_fx_gui_browser_open_folder),
// and (in hds_standalone.c specifically) after RecordEngine/RecordGatherState/record_engine/
// record_gather are all defined -- see hds_standalone.c's own include-order comment.

#ifdef HDS_STANDALONE_GUI_ENABLED

#include <sys/stat.h> // mkdir() -- creating the output folder if it doesn't exist yet, same "os.makedirs(exist_ok=True)" posture frontend.py's own _generate_record_filename() has

typedef struct {
    bool folder_initialized; // lazy-inits p->record_folder to "~/HDS_Recordings" on first render -- see this file's own render function
    double scroll_offset_px;
} HdsClapRecordingViewState;

static void hds_clap_recording_view_state_init(HdsClapRecordingViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_REC_MARGIN 12.0
#define HDS_REC_PILL_H 18.0
#define HDS_REC_ROW_GAP 8.0
#define HDS_REC_CHANNEL_COLS 4
#define HDS_REC_CHANNEL_CELL_W 90.0

typedef struct {
    HdsFxRect synth_output_cb;
    HdsFxRect channel_cb[RECORD_MAX_CHANNELS];
    HdsFxRect mode_multichannel_pill, mode_downmix_pill;
    HdsFxRect format_wav_pill, format_wavpack_pill;
    HdsFxRect folder_field, browse_btn;
    HdsFxRect record_btn;
    double content_h;
} HdsRecordingLayout;

static void hds_clap_recording_compute_layout(double scroll_offset_px, HdsRecordingLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = 56.0 - scroll_offset_px; // below this file's own header text block

    out->synth_output_cb = (HdsFxRect){HDS_REC_MARGIN, y, 280.0, HDS_REC_PILL_H};
    y += HDS_REC_PILL_H + 20.0; // extra gap for the "Raw Input Channels" label drawn just above the grid

    int rows = (RECORD_MAX_CHANNELS + HDS_REC_CHANNEL_COLS - 1) / HDS_REC_CHANNEL_COLS;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) {
        int col = c % HDS_REC_CHANNEL_COLS, row = c / HDS_REC_CHANNEL_COLS;
        out->channel_cb[c] = (HdsFxRect){HDS_REC_MARGIN + col * HDS_REC_CHANNEL_CELL_W, y + row * (HDS_REC_PILL_H + 4.0), HDS_REC_CHANNEL_CELL_W - 6.0, HDS_REC_PILL_H};
    }
    y += rows * (HDS_REC_PILL_H + 4.0) + 24.0;

    out->mode_multichannel_pill = (HdsFxRect){HDS_REC_MARGIN, y, 150.0, HDS_REC_PILL_H};
    out->mode_downmix_pill = (HdsFxRect){HDS_REC_MARGIN + 156.0, y, 130.0, HDS_REC_PILL_H};
    y += HDS_REC_PILL_H + HDS_REC_ROW_GAP;

    out->format_wav_pill = (HdsFxRect){HDS_REC_MARGIN, y, 70.0, HDS_REC_PILL_H};
    out->format_wavpack_pill = (HdsFxRect){HDS_REC_MARGIN + 76.0, y, 150.0, HDS_REC_PILL_H};
    y += HDS_REC_PILL_H + HDS_REC_ROW_GAP;

    out->folder_field = (HdsFxRect){HDS_REC_MARGIN, y, 420.0, HDS_REC_PILL_H};
    out->browse_btn = (HdsFxRect){HDS_REC_MARGIN + 426.0, y, 90.0, HDS_REC_PILL_H};
    y += HDS_REC_PILL_H + 28.0;

    out->record_btn = (HdsFxRect){HDS_REC_MARGIN, y, 160.0, 36.0};
    y += 36.0 + HDS_REC_MARGIN;

    out->content_h = y + scroll_offset_px;
}

static void hds_recording_ensure_folder_default(HdsClapPlugin *p, HdsClapRecordingViewState *vs) {
    if (vs->folder_initialized) return;
    vs->folder_initialized = true;
    if (p->record_folder[0]) return; // already set (e.g. a saved session) -- never stomp it
    const char *home = getenv("HOME");
    snprintf(p->record_folder, sizeof(p->record_folder), "%s/HDS_Recordings", home ? home : "/tmp");
}

// Mirrors frontend.py's own _generate_record_filename() exactly: HDS_<timestamp>.<ext> inside the
// remembered folder, ext chosen by record_format (WAV only in practice -- see this file's own
// header comment on why WavPack stays disabled). Called once, right before the record_enable 0->1
// edge the backend itself watches for (see hds_standalone.c's own comment on why record_path must
// already hold the real target before that edge fires).
static void hds_recording_generate_path(HdsClapPlugin *p) {
    mkdir(p->record_folder, 0755); // tolerates an already-existing folder (errno EEXIST) same as os.makedirs(exist_ok=True)
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tmv);
    const char *ext = (p->knobs.record_format != 0.0f) ? "wv" : "wav";
    // Every %s here is explicitly bounded by precision so the total is provably < RECORD_PATH_LEN:
    // stamp is at most 19 chars ("YYYY-MM-DD_HH-MM-SS"), ext at most 3 ("wav"). Without these,
    // GCC can't rule out the concatenation overflowing (both record_folder and record_path are
    // themselves capped at RECORD_PATH_LEN by their own array sizes) and warns
    // (-Wformat-truncation); this doesn't change real-world behavior at all (no legitimate folder
    // path gets anywhere close to using its own full RECORD_PATH_LEN budget), it just proves the
    // same safety snprintf already gives at runtime.
    snprintf(p->knobs.record_path, RECORD_PATH_LEN, "%.*s/HDS_%.19s.%.3s", RECORD_PATH_LEN - 40, p->record_folder, stamp, ext);
}

// Same "gate the ability to START, never yank an in-progress take" posture as frontend.py's own
// _update_record_button_enabled(): at least one source (Synth Output or any raw input channel) must
// be selected before the Record button will do anything.
static bool hds_recording_any_source_selected(const HdsClapPlugin *p) {
    if (p->knobs.record_include_synth_output != 0.0f) return true;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) if (p->knobs.record_input_channel_enable[c] != 0.0f) return true;
    return false;
}

static void hds_clap_recording_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRecordingViewState *vs, double window_w, double window_h) {
    hds_recording_ensure_folder_default(p, vs);

    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_clip(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    hds_fx_left_text(cr, HDS_REC_MARGIN, 20.0 - vs->scroll_offset_px,
                      "Record the synth's own output, raw channels from the Input device (Devices tab), or both");
    hds_fx_left_text(cr, HDS_REC_MARGIN, 34.0 - vs->scroll_offset_px,
                      "together -- to a genuine multichannel WAV file or downmixed to stereo.");

    HdsRecordingLayout L;
    hds_clap_recording_compute_layout(vs->scroll_offset_px, &L);

    bool synth_out_on = p->knobs.record_include_synth_output != 0.0f;
    hds_fx_draw_pill(cr, L.synth_output_cb.x, L.synth_output_cb.y, L.synth_output_cb.w, L.synth_output_cb.h,
                      "Synth Output (post-FX, L/R)", synth_out_on, false);

    hds_global_section_label(cr, HDS_REC_MARGIN, L.synth_output_cb.y + HDS_REC_PILL_H + 14.0, "Raw Input Channels (Devices tab's own Input device)");
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) {
        bool on = p->knobs.record_input_channel_enable[c] != 0.0f;
        char label[8]; snprintf(label, sizeof(label), "In %d", c + 1);
        hds_fx_draw_pill(cr, L.channel_cb[c].x, L.channel_cb[c].y, L.channel_cb[c].w, L.channel_cb[c].h, label, on, false);
    }

    bool downmix = p->knobs.record_downmix_stereo != 0.0f;
    hds_fx_draw_pill(cr, L.mode_multichannel_pill.x, L.mode_multichannel_pill.y, L.mode_multichannel_pill.w, L.mode_multichannel_pill.h, "Multichannel WAV", !downmix, false);
    hds_fx_draw_pill(cr, L.mode_downmix_pill.x, L.mode_downmix_pill.y, L.mode_downmix_pill.w, L.mode_downmix_pill.h, "Stereo Downmix", downmix, false);

    bool wavpack = p->knobs.record_format != 0.0f; // always false in practice -- see this file's own header comment
    hds_fx_draw_pill(cr, L.format_wav_pill.x, L.format_wav_pill.y, L.format_wav_pill.w, L.format_wav_pill.h, "WAV", !wavpack, false);
    hds_fx_draw_pill(cr, L.format_wavpack_pill.x, L.format_wavpack_pill.y, L.format_wavpack_pill.w, L.format_wavpack_pill.h, "WavPack (disabled)", false, true);

    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_new_path(cr); cairo_rectangle(cr, L.folder_field.x, L.folder_field.y, L.folder_field.w, L.folder_field.h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.15);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, L.folder_field.x + 0.5, L.folder_field.y + 0.5, L.folder_field.w - 1, L.folder_field.h - 1); cairo_stroke(cr);
    cairo_save(cr);
    cairo_rectangle(cr, L.folder_field.x, L.folder_field.y, L.folder_field.w, L.folder_field.h);
    cairo_clip(cr);
    hds_fx_rgb(cr, 0.0, 0.68, 0.71);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    hds_fx_left_text(cr, L.folder_field.x + 6, L.folder_field.y + L.folder_field.h / 2 + 3.5, p->record_folder);
    cairo_restore(cr);
    hds_fx_draw_pill(cr, L.browse_btn.x, L.browse_btn.y, L.browse_btn.w, L.browse_btn.h, "Browse\xe2\x80\xa6", false, false);

    bool recording = p->knobs.record_enable != 0.0f;
    hds_fx_rgb(cr, recording ? 0.69 : 0.19, recording ? 0.0 : 0.19, recording ? 0.13 : 0.22);
    hds_fx_rounded_rect(cr, L.record_btn.x, L.record_btn.y, L.record_btn.w, L.record_btn.h, 4);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    hds_fx_rgb(cr, 0.95, 0.95, 0.97);
    hds_fx_centered_text(cr, L.record_btn.x + L.record_btn.w / 2, L.record_btn.y + L.record_btn.h / 2 + 4.5,
                          recording ? "\xe2\x96\xa0 Stop" : "\xe2\x97\x8f Record");

    bool start_failed = atomic_load_explicit(&record_engine.start_failed, memory_order_relaxed);
    char status[80];
    if (start_failed) {
        hds_fx_rgb(cr, 1.0, 0.33, 0.33);
        snprintf(status, sizeof(status), "Recording failed to start -- check the folder/channels and try again.");
    } else if (recording && record_gather.active) {
        float elapsed = (float)atomic_load_explicit(&record_engine.frames_written, memory_order_relaxed) / (float)SAMPLE_RATE;
        int mins = (int)elapsed / 60, secs = (int)elapsed % 60;
        const char *slash = strrchr(p->knobs.record_path, '/');
        hds_fx_rgb(cr, 1.0, 0.33, 0.33);
        // %.*s bounds the filename explicitly -- see hds_recording_generate_path's own comment on
        // why GCC can't otherwise rule out overflowing status[]'s own fixed size.
        snprintf(status, sizeof(status), "\xe2\x97\x8f Recording %.48s -- %02d:%02d", slash ? slash + 1 : p->knobs.record_path, mins, secs);
    } else if (recording) {
        hds_fx_rgb(cr, 0.71, 0.54, 0.0);
        snprintf(status, sizeof(status), "Starting\xe2\x80\xa6");
    } else {
        hds_fx_rgb(cr, 0.55, 0.55, 0.58);
        snprintf(status, sizeof(status), "Not recording");
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    hds_fx_left_text(cr, L.record_btn.x + L.record_btn.w + 12.0, L.record_btn.y + L.record_btn.h / 2 + 4.0, status);

    cairo_restore(cr);
}

static bool hds_clap_recording_on_button_press(HdsClapPlugin *p, HdsClapRecordingViewState *vs, HdsFxGuiViewState *fxvs, double x, double y, int button) {
    if (button != 1) return true;
    HdsRecordingLayout L;
    hds_clap_recording_compute_layout(vs->scroll_offset_px, &L);

    if (hds_play_rect_contains(&L.synth_output_cb, x, y)) {
        p->knobs.record_include_synth_output = (p->knobs.record_include_synth_output != 0.0f) ? 0.0f : 1.0f;
        return true;
    }
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) {
        if (hds_play_rect_contains(&L.channel_cb[c], x, y)) {
            p->knobs.record_input_channel_enable[c] = (p->knobs.record_input_channel_enable[c] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
    }
    if (hds_play_rect_contains(&L.mode_multichannel_pill, x, y)) { p->knobs.record_downmix_stereo = 0.0f; return true; }
    if (hds_play_rect_contains(&L.mode_downmix_pill, x, y)) { p->knobs.record_downmix_stereo = 1.0f; return true; }
    if (hds_play_rect_contains(&L.format_wav_pill, x, y)) { p->knobs.record_format = 0.0f; return true; }
    // format_wavpack_pill: deliberately no click handler at all -- see this file's own header comment on why WavPack stays disabled.
    if (hds_play_rect_contains(&L.browse_btn, x, y)) {
        hds_fx_gui_browser_open_folder(fxvs, "Recordings Folder", p->record_folder, sizeof(p->record_folder));
        return true;
    }
    if (hds_play_rect_contains(&L.record_btn, x, y)) {
        bool currently_recording = p->knobs.record_enable != 0.0f;
        if (!currently_recording) {
            if (!hds_recording_any_source_selected(p)) return true; // matches frontend.py's own "button does nothing with no source selected" gating
            hds_recording_generate_path(p);
            p->knobs.record_enable = 1.0f;
        } else {
            p->knobs.record_enable = 0.0f;
        }
        return true;
    }
    return true;
}

static void hds_clap_recording_on_button_release(HdsClapRecordingViewState *vs) { (void)vs; }
static void hds_clap_recording_on_motion(HdsClapRecordingViewState *vs, double x, double y) { (void)vs; (void)x; (void)y; }

static void hds_clap_recording_on_scroll(HdsClapRecordingViewState *vs, double window_h, int delta) {
    HdsRecordingLayout L;
    hds_clap_recording_compute_layout(0.0, &L);
    double max_scroll = L.content_h - window_h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    vs->scroll_offset_px -= delta * 24.0;
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}

#endif // HDS_STANDALONE_GUI_ENABLED
