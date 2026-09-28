// hds_clap_global_draw.c: hds.clap's own persistent LEFT SIDEBAR -- ports frontend.py's own
// always-visible "Global Configurations" panel (Voice Mode, Hard Sync, Pitch Bend Range,
// Transpose, Microtuning, Sustain Pedal Polarity, FM Algorithm + depths, Glide Time/Random, Tempo,
// Metronome, Arpeggiator) verbatim in both CONTENT and PLACEMENT: a fixed-width column down the
// left edge of the window, visible no matter which tab is active, not a tab of its own (an
// earlier version of this port made it a "Global" tab instead -- wrong: frontend.py's own comment
// at this panel's construction site is explicit that "there used to be a dedicated Global tab...
// so the tab was removed" in favor of exactly this always-visible placement). Every control here
// reads/writes p->knobs (SynthSharedControls, transpose/tuning/metronome -- session-level, matching
// frontend.py's own _is_program_scope_address exclusions) or p->knobs.programs[SINGLE_PROGRAM_SLOT]
// (everything else, Program-scoped) DIRECTLY, in-process -- unlike frontend.py's own OSC round trip
// to a separate backend process, there is no transmit_knob_packet()-style bundling to replicate
// here at all (see this repo's own README on the architecture difference between the standalone
// app and this in-process plugin).
//
// Deliberately NOT ported (disclosed scope boundary, not an oversight):
//   - Program Load/Save and "Load Scale (.scl)": all three are FILE-PATH operations (open/save a
//     named file from disk) -- frontend.py implements them via Qt file dialogs, which this GUI has
//     no equivalent widget for at all (hds_fx_gui_browser.c's own file browser lists an existing
//     directory's contents for picking a NAM model/IR to load, not for typing a save filename+
//     category) -- real, follow-up work, not attempted here.
//   - Instance Switcher: a standalone-app-only concept (frontend.py's own OSC client can retarget
//     which separate backend PROCESS it talks to) -- this plugin has no separate backend process to
//     switch to at all, so this control simply doesn't apply.
//   - the live tempo readout label: frontend.py keeps a second read-only label in sync with the
//     backend's OWN resolved tempo (which can diverge from the tempo_bpm knob once tempo markers
//     are authored) via a live status-frame round trip; in this in-process plugin the knob field
//     IS what the audio engine reads every block, so a second "live" readout would always just
//     equal the knob -- nothing to build.
//   - Arp Octave Range/Rate/Hold: real, working, already-Program-scoped backend fields
//     (arp_octave_range/arp_rate/arp_hold) with no widget in frontend.py's OWN Global panel either
//     (a pre-existing gap in the Python reference, not something this port introduces or fixes).
//   - The "Output" group box (VU meter + master volume slider) stacked below Global Configurations
//     in frontend.py's own sidebar: a separate panel, not part of "Global Configurations" itself --
//     out of scope for this port, real follow-up work.
//
// #include'd by clap_plugin.c after hds_clap_play_draw.c (needs HdsPlayKnobDrag/hds_play_draw_knob/
// hds_play_knob_hit_test/hds_play_start_knob_drag/hds_play_rect_contains, same reuse convention
// hds_clap_devices_draw.c's own header comment already established) and before hds_clap_gui_draw.c
// (which owns the tab-bar/tab-content region this sidebar sits beside -- see its own header comment
// on how the two are composed together).

// Fixed width, not resizable independently -- matches frontend.py's own left_sidebar_scroll
// QScrollArea (setMaximumWidth(276), its content capped at 260) exactly: a permanent, constant-
// width column, not a panel that grows/shrinks with the rest of the window. hds_clap_gui_draw.c
// reads this same constant to know where the tab bar/content region starts.
#define HDS_CLAP_GLOBAL_SIDEBAR_W 210.0 // ~75% of the previous 280 -- little in this panel is genuinely wide (a couple of side-by-side knob pairs, some pill labels), so it doesn't need a full-width column

#define HDS_GLOBAL_DROPDOWN_NONE 0
#define HDS_GLOBAL_DROPDOWN_HARDSYNC 1
#define HDS_GLOBAL_DROPDOWN_FMALGO 2
#define HDS_GLOBAL_DROPDOWN_ARPSTYLE 3

typedef struct {
    int dropdown_kind; // HDS_GLOBAL_DROPDOWN_*
    HdsFxRect dropdown_anchor;
    HdsPlayKnobDrag knob_drag;
    double scroll_offset_px; // the whole sidebar's own vertical scroll -- see hds_clap_lfos_draw.c's own identical field/convention; the stacked content is taller than a typical window, same as frontend.py's own left_sidebar_scroll QScrollArea
} HdsClapGlobalViewState;

static void hds_clap_global_view_state_init(HdsClapGlobalViewState *vs) { memset(vs, 0, sizeof(*vs)); }

#define HDS_GLOBAL_MARGIN 12.0
#define HDS_GLOBAL_CONTENT_TOP 12.0
#define HDS_GLOBAL_KNOB_R 10.0
#define HDS_GLOBAL_PILL_H 18.0
#define HDS_GLOBAL_KNOB_ROW_H 46.0   // one label+knob+value-readout stride, same convention as hds_clap_play_draw.c's own HDS_PLAY_KNOB_STRIDE
#define HDS_GLOBAL_SECTION_GAP 18.0  // vertical gap between one section and the next

static const char *const HDS_GLOBAL_HARDSYNC_NAMES[3] = {"Off", "Parallel", "Serial"};
static const char *const HDS_GLOBAL_ARPSTYLE_NAMES[6] = {"Up", "Down", "Up/Down", "Down/Up", "Random", "As Played"};
#define HDS_GLOBAL_FMALGO_COUNT 12
// Plain ASCII "->", not the UTF-8 right-arrow glyph (U+2192) this used to draw: cairo's "toy" text
// API (cairo_show_text via hds_fx_left_text/hds_play_draw_knob's own value-text rendering) picks
// ONE font via fontconfig and does no per-glyph fallback the way a real text-layout engine (Pango,
// HarfBuzz) would -- if that resolved "sans-serif" font happens to lack a given Unicode glyph on
// some system, it simply doesn't render anything for it, with no substitution. ASCII always has a
// glyph in any font this API could plausibly resolve to.
static const char *const HDS_GLOBAL_FMALGO_NAMES[HDS_GLOBAL_FMALGO_COUNT] = {
    "0: Classic", "1: 3-Op Chain", "2: 4-Op Chain", "3: Dual Serial", "4: 3,4->1",
    "5: 3->1,2", "6: 1,3->2", "7: 4->1", "8: Dual Indep",
    "9: 1->3->4", "10: Star", "11: Independent",
};

// 8 FM depth fields in a fixed order (matches frontend.py's own fm_depth_dials dict order, see
// this file's own header comment) -- which ones are meaningful for a given algorithm mirrors
// frontend.py's own FM_ALGORITHMS[n]["depths"] exactly (same "not every field applies to every
// topology" convention as hds_clap_play_draw.c's own HDS_PLAY_EG_FIELD_ORDER).
#define HDS_GLOBAL_FM_DEPTH_COUNT 8
static const char *const HDS_GLOBAL_FM_DEPTH_LABELS[HDS_GLOBAL_FM_DEPTH_COUNT] = {
    "2->1", "3->1", "3->2", "4->1",
    "4->3", "1->2", "3->4", "1->3",
};
static void hds_global_fm_depth_visibility(int algo, bool *show /* [8] */) {
    for (int i = 0; i < HDS_GLOBAL_FM_DEPTH_COUNT; i++) show[i] = false;
    switch (algo) {
        case 0: show[0] = true; break;
        case 1: show[2] = show[0] = true; break;
        case 2: show[4] = show[2] = show[0] = true; break;
        case 3: show[4] = show[0] = true; break;
        case 4: show[1] = show[3] = true; break;
        case 5: show[1] = show[2] = true; break;
        case 6: show[5] = show[2] = true; break;
        case 7: show[3] = true; break;
        case 8: show[5] = show[6] = true; break;
        case 9: show[7] = show[6] = true; break;
        case 10: show[0] = show[1] = show[3] = true; break;
        default: break; // 11: All independent -- no depth knob applies at all
    }
}
static float *hds_global_fm_depth_target(ProgramState *prog, int i) {
    switch (i) {
        case 0: return &prog->fm_intensity;
        case 1: return &prog->fm_depth_3_1;
        case 2: return &prog->fm_depth_3_2;
        case 3: return &prog->fm_depth_4_1;
        case 4: return &prog->fm_depth_4_3;
        case 5: return &prog->fm_depth_1_2;
        case 6: return &prog->fm_depth_3_4;
        default: return &prog->fm_depth_1_3;
    }
}
// How many of the (at most 3, per hds_global_fm_depth_visibility()) visible depth knobs for this
// algorithm, and which field each visual slot (left to right) maps to -- same "spread evenly over
// however many are actually visible" idiom as hds_clap_play_draw.c's own HDS_PLAY_EG_FIELD_ORDER/
// hds_play_eg_dial_count(), not a fixed 8-slot arrangement with per-slot visibility toggling.
static int hds_global_fm_depth_visible_fields(int algo, int *fields /* [8] */) {
    bool show[HDS_GLOBAL_FM_DEPTH_COUNT];
    hds_global_fm_depth_visibility(algo, show);
    int n = 0;
    for (int i = 0; i < HDS_GLOBAL_FM_DEPTH_COUNT; i++) if (show[i]) fields[n++] = i;
    return n;
}

// One vertical column, top to bottom -- order matches frontend.py's own global_layout.addWidget()
// call sequence exactly (Voice Mode -> Hard Sync -> Pitch Bend Range -> Transpose -> Microtuning ->
// Sustain Pedal Polarity -> FM Algorithm -> Glide Time -> Glide Random -> Tempo -> Metronome ->
// Arpeggiator), not the earlier 5-column-tab layout this port originally shipped with.
typedef struct {
    HdsFxRect poly_pill, mono_pill;
    HdsFxRect hardsync_pill;
    HdsFxRect pb_up_knob, pb_down_knob;
    HdsFxRect transpose_knob;
    HdsFxRect tuning_enable_pill;
    HdsFxRect tuning_note_knob, tuning_freq_knob;
    HdsFxRect sustain_normal_pill, sustain_inverted_pill;
    HdsFxRect fm_algo_pill;
    HdsFxRect fm_depth_knob[HDS_GLOBAL_FM_DEPTH_COUNT]; // only fields[0..count) (see hds_global_fm_depth_visible_fields()) are ever populated/valid for the current algorithm
    HdsFxRect glide_time_knob, glide_random_knob, tempo_knob;
    HdsFxRect metronome_pill;
    HdsFxRect arp_enabled_pill, arp_style_pill;
    HdsFxRect arp_velocity_knob, arp_gate_knob;
    double content_h; // total stacked height, for scroll clamping -- constant regardless of state (the FM depth row's own width varies by algorithm, never its height)
} HdsGlobalLayout;

static void hds_clap_global_compute_layout(int fm_algo, double scroll_offset_px, HdsGlobalLayout *out) {
    memset(out, 0, sizeof(*out));
    double content_x = HDS_GLOBAL_MARGIN;
    double content_w = HDS_CLAP_GLOBAL_SIDEBAR_W - 2.0 * HDS_GLOBAL_MARGIN;
    double lane_l_cx = content_x + content_w * 0.28;   // left knob of a 2-knob row
    double lane_r_cx = content_x + content_w * 0.72;   // right knob of a 2-knob row
    double y = HDS_GLOBAL_CONTENT_TOP - scroll_offset_px;

    // Voice Mode
    y += 12.0;
    out->poly_pill = (HdsFxRect){content_x, y, content_w / 2.0 - 3.0, HDS_GLOBAL_PILL_H};
    out->mono_pill = (HdsFxRect){content_x + content_w / 2.0 + 3.0, y, content_w / 2.0 - 3.0, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + HDS_GLOBAL_SECTION_GAP;

    // Hard Sync
    y += 12.0;
    out->hardsync_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + HDS_GLOBAL_SECTION_GAP;

    // Pitch Bend Range (Up/Down side by side)
    y += 12.0;
    out->pb_up_knob = (HdsFxRect){lane_l_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    out->pb_down_knob = (HdsFxRect){lane_r_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Transpose
    y += 12.0;
    out->transpose_knob = (HdsFxRect){content_x + content_w / 2.0, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Microtuning: Enable pill, then Ref Note/Ref Freq knobs side by side
    y += 12.0;
    out->tuning_enable_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + 6.0;
    out->tuning_note_knob = (HdsFxRect){lane_l_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    out->tuning_freq_knob = (HdsFxRect){lane_r_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Sustain Pedal Polarity
    y += 12.0;
    out->sustain_normal_pill = (HdsFxRect){content_x, y, content_w / 2.0 - 3.0, HDS_GLOBAL_PILL_H};
    out->sustain_inverted_pill = (HdsFxRect){content_x + content_w / 2.0 + 3.0, y, content_w / 2.0 - 3.0, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + HDS_GLOBAL_SECTION_GAP;

    // FM Algorithm: pill, then up to 3 depth knobs spread evenly across content_w
    y += 12.0;
    out->fm_algo_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + 6.0;
    int fields[HDS_GLOBAL_FM_DEPTH_COUNT];
    int fcount = hds_global_fm_depth_visible_fields(fm_algo, fields);
    if (fcount > 0) {
        double stride = content_w / (double)fcount;
        for (int i = 0; i < fcount; i++) {
            out->fm_depth_knob[fields[i]] = (HdsFxRect){content_x + i * stride + stride / 2.0, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
        }
    }
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Glide Time
    y += 12.0;
    out->glide_time_knob = (HdsFxRect){content_x + content_w / 2.0, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Glide Random
    y += 12.0;
    out->glide_random_knob = (HdsFxRect){content_x + content_w / 2.0, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Tempo
    y += 12.0;
    out->tempo_knob = (HdsFxRect){content_x + content_w / 2.0, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H + HDS_GLOBAL_SECTION_GAP;

    // Metronome (own toggle, no separate label)
    out->metronome_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + HDS_GLOBAL_SECTION_GAP;

    // Arpeggiator: ARP toggle, Style pill, then Velocity/Gate knobs side by side
    out->arp_enabled_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + 6.0;
    out->arp_style_pill = (HdsFxRect){content_x, y, content_w, HDS_GLOBAL_PILL_H};
    y += HDS_GLOBAL_PILL_H + 6.0;
    out->arp_velocity_knob = (HdsFxRect){lane_l_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    out->arp_gate_knob = (HdsFxRect){lane_r_cx, y + HDS_GLOBAL_KNOB_R + 9.0, HDS_GLOBAL_KNOB_R, HDS_GLOBAL_KNOB_R};
    y += HDS_GLOBAL_KNOB_ROW_H;

    out->content_h = y + scroll_offset_px + HDS_GLOBAL_MARGIN; // undo the scroll subtraction this function started with, so callers get the SAME content height regardless of current scroll position
}

static void hds_global_section_label(cairo_t *cr, double x, double y, const char *text) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    hds_fx_rgb(cr, 0.55, 0.55, 0.58);
    hds_fx_left_text(cr, x, y, text);
}

static void hds_clap_global_render(cairo_t *cr, HdsClapPlugin *p, HdsClapGlobalViewState *vs, double window_w, double window_h) {
    (void)window_w; // always HDS_CLAP_GLOBAL_SIDEBAR_W wide -- see this file's own header comment
    hds_fx_rgb(cr, 0.12, 0.12, 0.145); // one shade lighter than the tab-content area, reads as a distinct panel (matches frontend.py's own bordered QGroupBox convention)
    cairo_rectangle(cr, 0, 0, HDS_CLAP_GLOBAL_SIDEBAR_W, window_h);
    cairo_fill(cr);

    ProgramState *prog = hds_play_program(p);
    int algo = (int)prog->fm_algorithm; if (algo < 0 || algo >= HDS_GLOBAL_FMALGO_COUNT) algo = 0;
    HdsGlobalLayout L;
    hds_clap_global_compute_layout(algo, vs->scroll_offset_px, &L);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, HDS_CLAP_GLOBAL_SIDEBAR_W, window_h);
    cairo_clip(cr);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.poly_pill.y - 4.0, "Voice Mode");
    bool poly = (prog->is_polyphonic != 0.0f);
    hds_fx_draw_pill(cr, L.poly_pill.x, L.poly_pill.y, L.poly_pill.w, L.poly_pill.h, "Poly", poly, false);
    hds_fx_draw_pill(cr, L.mono_pill.x, L.mono_pill.y, L.mono_pill.w, L.mono_pill.h, "Mono", !poly, false);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.hardsync_pill.y - 4.0, "Hard Sync");
    int hardsync = (int)prog->hard_sync_mode; if (hardsync < 0 || hardsync > 2) hardsync = 0;
    bool hardsync_open = (vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC);
    hds_fx_draw_pill(cr, L.hardsync_pill.x, L.hardsync_pill.y, L.hardsync_pill.w, L.hardsync_pill.h,
                      HDS_GLOBAL_HARDSYNC_NAMES[hardsync], hardsync_open, false);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.pb_up_knob.y - HDS_GLOBAL_KNOB_R - 12.0, "Pitch Bend Range");
    char pbtxt[16];
    snprintf(pbtxt, sizeof(pbtxt), "%d st", (int)(prog->pb_range_up + 0.5f));
    hds_play_draw_knob(cr, L.pb_up_knob.x, L.pb_up_knob.y, L.pb_up_knob.w, (float)(prog->pb_range_up / 24.0), false, "Up", pbtxt);
    snprintf(pbtxt, sizeof(pbtxt), "%d st", (int)(prog->pb_range_down + 0.5f));
    hds_play_draw_knob(cr, L.pb_down_knob.x, L.pb_down_knob.y, L.pb_down_knob.w, (float)(prog->pb_range_down / 24.0), false, "Down", pbtxt);

    char transtxt[8]; snprintf(transtxt, sizeof(transtxt), "%d", (int)(p->knobs.transpose_semitones));
    hds_play_draw_knob(cr, L.transpose_knob.x, L.transpose_knob.y, L.transpose_knob.w,
                        (float)((p->knobs.transpose_semitones + 16.0) / 32.0), false, "Transpose", transtxt);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.tuning_enable_pill.y - 4.0, "Microtuning");
    bool tuning_on = (p->knobs.tuning_enable != 0.0f);
    hds_fx_draw_pill(cr, L.tuning_enable_pill.x, L.tuning_enable_pill.y, L.tuning_enable_pill.w, L.tuning_enable_pill.h, "Enable", tuning_on, false);
    char notetxt[8]; snprintf(notetxt, sizeof(notetxt), "%d", (int)(p->knobs.tuning_reference_note + 0.5f));
    hds_play_draw_knob(cr, L.tuning_note_knob.x, L.tuning_note_knob.y, L.tuning_note_knob.w,
                        (float)(p->knobs.tuning_reference_note / 127.0), false, "Ref Note", notetxt);
    char freqtxt[16]; snprintf(freqtxt, sizeof(freqtxt), "%.1f Hz", p->knobs.tuning_reference_freq);
    hds_play_draw_knob(cr, L.tuning_freq_knob.x, L.tuning_freq_knob.y, L.tuning_freq_knob.w,
                        (float)((p->knobs.tuning_reference_freq - 20.0) / (2000.0 - 20.0)), false, "Ref Freq", freqtxt);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.sustain_normal_pill.y - 4.0, "Sustain Polarity");
    bool inverted = (prog->sustain_pedal_polarity != 0.0f);
    hds_fx_draw_pill(cr, L.sustain_normal_pill.x, L.sustain_normal_pill.y, L.sustain_normal_pill.w, L.sustain_normal_pill.h, "Normal", !inverted, false);
    hds_fx_draw_pill(cr, L.sustain_inverted_pill.x, L.sustain_inverted_pill.y, L.sustain_inverted_pill.w, L.sustain_inverted_pill.h, "Inverted", inverted, false);

    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.fm_algo_pill.y - 4.0, "FM Algorithm");
    bool fmalgo_open = (vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO);
    hds_fx_draw_pill(cr, L.fm_algo_pill.x, L.fm_algo_pill.y, L.fm_algo_pill.w, L.fm_algo_pill.h, HDS_GLOBAL_FMALGO_NAMES[algo], fmalgo_open, false);
    int fields[HDS_GLOBAL_FM_DEPTH_COUNT];
    int fcount = hds_global_fm_depth_visible_fields(algo, fields);
    for (int i = 0; i < fcount; i++) {
        int f = fields[i];
        float *target = hds_global_fm_depth_target(prog, f);
        char vtxt[8]; snprintf(vtxt, sizeof(vtxt), "%d%%", (int)((*target) * 100.0f + 0.5f));
        hds_play_draw_knob(cr, L.fm_depth_knob[f].x, L.fm_depth_knob[f].y, L.fm_depth_knob[f].w,
                            *target / 2.0f, false, HDS_GLOBAL_FM_DEPTH_LABELS[f], vtxt);
    }

    char glidetxt[16]; snprintf(glidetxt, sizeof(glidetxt), "%dms", (int)(prog->portamento_time_ms + 0.5f));
    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.glide_time_knob.y - HDS_GLOBAL_KNOB_R - 12.0, "Glide Time");
    hds_play_draw_knob(cr, L.glide_time_knob.x, L.glide_time_knob.y, L.glide_time_knob.w,
                        (float)(prog->portamento_time_ms / 1000.0), false, "Glide Time", glidetxt);
    char grandtxt[8]; snprintf(grandtxt, sizeof(grandtxt), "%d%%", (int)(prog->mono_glide_random + 0.5f));
    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.glide_random_knob.y - HDS_GLOBAL_KNOB_R - 12.0, "Glide Random");
    hds_play_draw_knob(cr, L.glide_random_knob.x, L.glide_random_knob.y, L.glide_random_knob.w,
                        (float)(prog->mono_glide_random / 100.0), false, "Glide Rnd", grandtxt);
    char tempotxt[16]; snprintf(tempotxt, sizeof(tempotxt), "%d BPM", (int)(prog->tempo_bpm + 0.5f));
    hds_global_section_label(cr, HDS_GLOBAL_MARGIN, L.tempo_knob.y - HDS_GLOBAL_KNOB_R - 12.0, "Tempo");
    hds_play_draw_knob(cr, L.tempo_knob.x, L.tempo_knob.y, L.tempo_knob.w,
                        (float)((prog->tempo_bpm - 20.0) / (300.0 - 20.0)), false, "Tempo", tempotxt);

    bool metronome_on = (p->knobs.metronome_enable != 0.0f);
    hds_fx_draw_pill(cr, L.metronome_pill.x, L.metronome_pill.y, L.metronome_pill.w, L.metronome_pill.h, "\xf0\x9f\x8e\xb5 Metronome", metronome_on, false);

    bool arp_on = (prog->arp_enabled != 0.0f);
    hds_fx_draw_pill(cr, L.arp_enabled_pill.x, L.arp_enabled_pill.y, L.arp_enabled_pill.w, L.arp_enabled_pill.h, "ARP", arp_on, false);
    int arp_style = (int)prog->arp_style; if (arp_style < 0 || arp_style > 5) arp_style = 0;
    bool arpstyle_open = (vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_ARPSTYLE);
    hds_fx_draw_pill(cr, L.arp_style_pill.x, L.arp_style_pill.y, L.arp_style_pill.w, L.arp_style_pill.h,
                      HDS_GLOBAL_ARPSTYLE_NAMES[arp_style], arpstyle_open, !arp_on);
    int arp_vel = (int)(prog->arp_velocity + 0.5f);
    char veltxt[16]; if (arp_vel <= 0) snprintf(veltxt, sizeof(veltxt), "As Played"); else snprintf(veltxt, sizeof(veltxt), "%d", arp_vel);
    hds_play_draw_knob(cr, L.arp_velocity_knob.x, L.arp_velocity_knob.y, L.arp_velocity_knob.w,
                        (float)(prog->arp_velocity / 127.0), !arp_on, "Velocity", veltxt);
    char gatetxt[8]; snprintf(gatetxt, sizeof(gatetxt), "%d%%", (int)(prog->arp_gate_pct + 0.5f));
    hds_play_draw_knob(cr, L.arp_gate_knob.x, L.arp_gate_knob.y, L.arp_gate_knob.w,
                        (float)(prog->arp_gate_pct / 100.0), !arp_on, "Gate", gatetxt);

    cairo_restore(cr);

    // Whichever dropdown (if any) is open draws LAST, same rule every other tab's own dropdown follows.
    if (vs->dropdown_kind != HDS_GLOBAL_DROPDOWN_NONE) {
        int count = vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC ? 3
                  : vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO ? HDS_GLOBAL_FMALGO_COUNT : 6;
        const char *const *names = vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC ? HDS_GLOBAL_HARDSYNC_NAMES
                                  : vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO ? HDS_GLOBAL_FMALGO_NAMES : HDS_GLOBAL_ARPSTYLE_NAMES;
        double list_h = count * 16.0;
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
        int current = vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC ? hardsync
                     : vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO ? algo : arp_style;
        for (int i = 0; i < count; i++) {
            HdsFxRect r = {bg.x, bg.y + i * 16.0, bg.w, 16.0};
            if (i == current) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
            }
            hds_fx_rgb(cr, i == current ? 0.92 : 0.78, i == current ? 0.95 : 0.78, i == current ? 0.92 : 0.81);
            hds_fx_left_text(cr, r.x + 5, r.y + r.h / 2 + 3.2, names[i]);
        }
    }
}

static bool hds_clap_global_on_button_press(HdsClapPlugin *p, HdsClapGlobalViewState *vs, double window_h, double x, double y, int button) {
    ProgramState *prog = hds_play_program(p);

    if (vs->dropdown_kind != HDS_GLOBAL_DROPDOWN_NONE) {
        int count = vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC ? 3
                  : vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO ? HDS_GLOBAL_FMALGO_COUNT : 6;
        double list_h = count * 16.0;
        double below_y = vs->dropdown_anchor.y + vs->dropdown_anchor.h + 2.0;
        double y0 = (below_y + list_h > window_h) ? (vs->dropdown_anchor.y - list_h - 2.0) : below_y;
        if (y0 < 0.0) y0 = 0.0;
        for (int i = 0; i < count; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * 16.0, vs->dropdown_anchor.w, 16.0};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) {
                    if (vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_HARDSYNC) prog->hard_sync_mode = (float)i;
                    else if (vs->dropdown_kind == HDS_GLOBAL_DROPDOWN_FMALGO) prog->fm_algorithm = (float)i;
                    else prog->arp_style = (float)i;
                }
                vs->dropdown_kind = HDS_GLOBAL_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_GLOBAL_DROPDOWN_NONE;
        return true;
    }

    int algo = (int)prog->fm_algorithm; if (algo < 0 || algo >= HDS_GLOBAL_FMALGO_COUNT) algo = 0;
    HdsGlobalLayout L;
    hds_clap_global_compute_layout(algo, vs->scroll_offset_px, &L);
    bool arp_on = (prog->arp_enabled != 0.0f);

    if (hds_play_rect_contains(&L.poly_pill, x, y)) { if (button == 1) prog->is_polyphonic = 1.0f; return true; }
    if (hds_play_rect_contains(&L.mono_pill, x, y)) { if (button == 1) prog->is_polyphonic = 0.0f; return true; }
    if (hds_play_rect_contains(&L.hardsync_pill, x, y)) {
        vs->dropdown_kind = HDS_GLOBAL_DROPDOWN_HARDSYNC; vs->dropdown_anchor = L.hardsync_pill; return true;
    }
    if (hds_play_knob_hit_test(&L.pb_up_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->pb_range_up, 0.0, 24.0, y); return true; }
    if (hds_play_knob_hit_test(&L.pb_down_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->pb_range_down, 0.0, 24.0, y); return true; }
    if (hds_play_knob_hit_test(&L.transpose_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.transpose_semitones, -16.0, 16.0, y); return true; }

    if (hds_play_rect_contains(&L.tuning_enable_pill, x, y)) {
        if (button == 1) p->knobs.tuning_enable = (p->knobs.tuning_enable != 0.0f) ? 0.0f : 1.0f;
        return true;
    }
    if (hds_play_knob_hit_test(&L.tuning_note_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.tuning_reference_note, 0.0, 127.0, y); return true; }
    if (hds_play_knob_hit_test(&L.tuning_freq_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.tuning_reference_freq, 20.0, 2000.0, y); return true; }

    if (hds_play_rect_contains(&L.sustain_normal_pill, x, y)) { if (button == 1) prog->sustain_pedal_polarity = 0.0f; return true; }
    if (hds_play_rect_contains(&L.sustain_inverted_pill, x, y)) { if (button == 1) prog->sustain_pedal_polarity = 1.0f; return true; }

    if (hds_play_rect_contains(&L.fm_algo_pill, x, y)) {
        vs->dropdown_kind = HDS_GLOBAL_DROPDOWN_FMALGO; vs->dropdown_anchor = L.fm_algo_pill; return true;
    }
    int fields[HDS_GLOBAL_FM_DEPTH_COUNT];
    int fcount = hds_global_fm_depth_visible_fields(algo, fields);
    for (int i = 0; i < fcount; i++) {
        int f = fields[i];
        if (hds_play_knob_hit_test(&L.fm_depth_knob[f], x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, hds_global_fm_depth_target(prog, f), 0.0, 2.0, y);
            return true;
        }
    }

    if (hds_play_knob_hit_test(&L.glide_time_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->portamento_time_ms, 0.0, 1000.0, y); return true; }
    if (hds_play_knob_hit_test(&L.glide_random_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->mono_glide_random, 0.0, 100.0, y); return true; }
    if (hds_play_knob_hit_test(&L.tempo_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->tempo_bpm, 20.0, 300.0, y); return true; }
    if (hds_play_rect_contains(&L.metronome_pill, x, y)) {
        if (button == 1) p->knobs.metronome_enable = (p->knobs.metronome_enable != 0.0f) ? 0.0f : 1.0f;
        return true;
    }

    if (hds_play_rect_contains(&L.arp_enabled_pill, x, y)) {
        if (button == 1) prog->arp_enabled = (prog->arp_enabled != 0.0f) ? 0.0f : 1.0f;
        return true;
    }
    if (arp_on && hds_play_rect_contains(&L.arp_style_pill, x, y)) {
        vs->dropdown_kind = HDS_GLOBAL_DROPDOWN_ARPSTYLE; vs->dropdown_anchor = L.arp_style_pill; return true;
    }
    if (arp_on && hds_play_knob_hit_test(&L.arp_velocity_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->arp_velocity, 0.0, 127.0, y); return true; }
    if (arp_on && hds_play_knob_hit_test(&L.arp_gate_knob, x, y)) { hds_play_start_knob_drag(&vs->knob_drag, &prog->arp_gate_pct, 1.0, 100.0, y); return true; }

    (void)button;
    return true;
}

static void hds_clap_global_on_motion(HdsClapGlobalViewState *vs, double x, double y) {
    (void)x;
    if (!vs->knob_drag.target) return;
    double delta = vs->knob_drag.start_y - y;
    double v = vs->knob_drag.start_val + (delta / HDS_PLAY_DRAG_PIXELS_FULL_RANGE) * (vs->knob_drag.hi - vs->knob_drag.lo);
    if (v < vs->knob_drag.lo) v = vs->knob_drag.lo; else if (v > vs->knob_drag.hi) v = vs->knob_drag.hi;
    *vs->knob_drag.target = (float)v;
}

static void hds_clap_global_on_button_release(HdsClapGlobalViewState *vs) { vs->knob_drag.target = NULL; }

// Same scroll convention as hds_clap_lfos_draw.c/hds_clap_envelopes_draw.c's own scroll handlers --
// content_h comes from the SAME layout function render/hit-test already use, so it can never
// disagree with what's actually on screen.
static void hds_clap_global_on_scroll(HdsClapPlugin *p, HdsClapGlobalViewState *vs, double window_h, int delta) {
    ProgramState *prog = hds_play_program(p);
    int algo = (int)prog->fm_algorithm; if (algo < 0 || algo >= HDS_GLOBAL_FMALGO_COUNT) algo = 0;
    HdsGlobalLayout L;
    hds_clap_global_compute_layout(algo, 0.0, &L); // scroll_offset_px=0 here purely to read content_h, which doesn't depend on it
    double max_scroll = L.content_h - window_h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    vs->scroll_offset_px -= delta * 24.0;
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}
