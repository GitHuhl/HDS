// hds_clap_routing_draw.c: this native GUI's own Routing tab -- shared by hds.clap AND the
// standalone app (compiled into both TUs, like every other hds_clap_*_draw.c file), but its own
// output-routing section deliberately DIFFERS between the two hosts, gated by
// HDS_STANDALONE_GUI_ENABLED (same per-TU macro convention as hds_clap_devices_draw.c's own
// standalone-only sections): one row per real Source (the same 17, Program 1-8/Combi Mix/Ext In
// 1-8, frontend.py's own SOURCE_NAMES list the FX tab's Source combo already ports -- see
// hds_clap_source_label() in clap_plugin.c, reused here directly), each with, for the 8 Ext In rows
// only, Enable/Gain/Stereo controls and a live VU meter, PLUS:
//
//   - hds.clap: a single Main Out toggle (source_routing[src].enable[0]) only. SCOPE DECISION
//     (confirmed with the user when this file was first written): clap_plugin_process() always
//     calls render_audio_block() with output_channels hardcoded to 2 -- confirmed by reading that
//     call site -- so only enable[0] ever reaches this plugin's actual audio output; enable[1..7]
//     compute internally (the routing-matrix summing loop in render_audio_block() runs
//     unconditionally over the full OUTPUT_PAIR_MAX) but their summed result is discarded before
//     ever reaching out_buffer or any CLAP port. Showing the full 8-column grid here would ship 7
//     columns that visibly do nothing -- so only the one column that's actually audible is shown;
//     columns 1-7 are left exactly as whatever state they already hold (never zeroed, never touched
//     by this GUI), so a save file or a future architecture change that actually wires up more CLAP
//     outputs loses nothing.
//   - the standalone app: the REAL, full 8-column source-to-output-pair grid (one checkbox per
//     OUTPUT_PAIR_MAX column, exactly matching frontend.py's own Routing tab) -- its own
//     multichannel ALSA device genuinely makes every one of those 8 pairs audible (see
//     render_audio_block()'s own output_pairs loop, driven by the real detected_output_channels).
//     Not yet dynamically shown/hidden by the connected device's real channel count the way
//     frontend.py's own _rebuild_routing_output_columns() does -- routing to a pair the device
//     doesn't have is harmless (that same output_pairs clamp/zero-fill already discards it), so
//     showing all 8 unconditionally is a correct, if not yet capability-aware, port. Tracked as
//     real follow-up work, not attempted here.
//
// Also revives the ext-in VU meters, which are otherwise fully dead in hds.clap (backend.c's own
// gui_routing_tab_visible is never set from a CLAP instance's control surface, since it has no
// OSC-driven Routing tab of its own to report from) -- see HdsClapPlugin.ext_in_peak's own comment
// in clap_plugin.c and hds_clap_gui_render()'s own comment on setting gui_routing_tab_visible.

typedef struct {
    HdsPlayKnobDrag knob_drag; // reused verbatim -- see hds_clap_filters_draw.c's own header comment on this pattern
    double scroll_offset_px;   // whole tab's own vertical scroll (source rows + the Drum Bus Key
                                // Routing section below them) -- same convention as
                                // hds_clap_lfos_draw.c's own scroll_offset_px. Needed now that
                                // SOURCE_COUNT (41, since Drum Buses were appended -- see
                                // DRUM_BUS_COUNT's own comment in backend.c) plus the new section
                                // below it overflows any reasonable window height.
    int drum_bus_selected;     // 1..DRUM_BUS_COUNT -- which Drum Bus the Key Routing grid below
                                // currently shows/edits (see its own comment).
} HdsClapRoutingViewState;

static void hds_clap_routing_view_state_init(HdsClapRoutingViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->drum_bus_selected = 1;
}

#define HDS_ROUTING_MARGIN 10.0
#define HDS_ROUTING_HEADER_H 28.0
#define HDS_ROUTING_ROW_H 38.0
#define HDS_ROUTING_KNOB_R 10.0

// Standalone only: the real thing frontend.py's own Routing tab has always been (an 8-column,
// many-to-many source-to-output-pair grid, one checkbox per OUTPUT_PAIR_MAX column -- see
// SynthSharedControls.source_routing/OUTPUT_PAIR_MAX in backend.c) -- the standalone's own
// multichannel ALSA device genuinely makes every one of the 8 pairs audible (render_audio_block's
// own output_pairs = output_channels/2 loop), unlike hds.clap's hardcoded 2-channel process(). Not
// dynamically shown/hidden by the currently-connected device's real channel count the way
// frontend.py's own _rebuild_routing_output_columns() does (that needs a live detected-channel-count
// field threaded into this draw file, real follow-up work) -- routing to a pair the connected
// device doesn't actually have is harmless (render_audio_block's own output_pairs clamp/zero-fill
// already discards it), so showing all 8 unconditionally is a correct, if not yet
// capability-aware, port.
#define HDS_ROUTING_OUT_PILL_W 26.0
#define HDS_ROUTING_OUT_PILL_STRIDE 28.0

typedef struct {
    HdsFxRect name_area, enable_pill, gain_knob, stereo_pill, vu_bar;
#ifdef HDS_STANDALONE_GUI_ENABLED
    HdsFxRect out_pill[OUTPUT_PAIR_MAX];
#else
    HdsFxRect mainout_pill; // CLAP: only enable[0] is ever audible -- see this file's own header comment
#endif
} HdsRoutingRowLayout;

static void hds_clap_routing_compute_row_layout(int src, double scroll_offset_px, HdsRoutingRowLayout *out) {
    memset(out, 0, sizeof(*out));
    double y = HDS_ROUTING_HEADER_H + src * HDS_ROUTING_ROW_H - scroll_offset_px;
    out->name_area = (HdsFxRect){HDS_ROUTING_MARGIN, y, 110.0, HDS_ROUTING_ROW_H};
    out->enable_pill = (HdsFxRect){126.0, y + 8.0, 46.0, 18.0};
    out->gain_knob = (HdsFxRect){195.0, y + HDS_ROUTING_KNOB_R + 6.0, HDS_ROUTING_KNOB_R, HDS_ROUTING_KNOB_R};
    out->stereo_pill = (HdsFxRect){225.0, y + 8.0, 54.0, 18.0};
#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
        out->out_pill[p] = (HdsFxRect){290.0 + p * HDS_ROUTING_OUT_PILL_STRIDE, y + 8.0, HDS_ROUTING_OUT_PILL_W, 18.0};
    }
    out->vu_bar = (HdsFxRect){290.0 + OUTPUT_PAIR_MAX * HDS_ROUTING_OUT_PILL_STRIDE + 14.0, y + 11.0, 160.0, 12.0};
#else
    out->mainout_pill = (HdsFxRect){290.0, y + 8.0, 80.0, 18.0};
    out->vu_bar = (HdsFxRect){380.0, y + 11.0, 160.0, 12.0};
#endif
}

// BUG FIX (real, not cosmetic): this used to be `src >= COMBI_PROGRAM_COUNT + 1`, true for every
// Drum Bus source id too (they sit even further past the ext-in pairs -- see DRUM_BUS_COUNT's own
// comment in backend.c). That made every Drum Bus row render fake Enable/Gain/Stereo controls and
// compute hds_routing_ext_in_pair() values of EXT_IN_PAIR_COUNT..EXT_IN_PAIR_COUNT+23 -- reading
// and WRITING straight past the real, 8-element ext_in_enable[]/ext_in_gain[]/ext_in_stereo[]
// arrays on every click, corrupting whatever SynthSharedControls fields happen to sit adjacent in
// memory. Bounding the range to the real ext-in pairs only fixes it.
static bool hds_routing_is_ext_in(int src) { return src >= COMBI_PROGRAM_COUNT + 1 && src < COMBI_PROGRAM_COUNT + 1 + EXT_IN_PAIR_COUNT; }
static int hds_routing_ext_in_pair(int src) { return src - COMBI_PROGRAM_COUNT - 1; }

// --- Drum Bus Key Routing: a compact, low-footprint way to actually assign Sample Bank zones to
// Drum Buses from the GUI (see DRUM_BUS_COUNT's own comment in backend.c) -- the only other way is
// hand-editing an SFZ file's own hds_drum_bus= opcode or raw OSC. One 7 (natural note letter) x 8
// (octave) checkbox grid, reused for whichever Drum Bus the stepper above it currently selects
// (rather than a separate 56-checkbox grid per bus, which would be 24x the footprint for the same
// information) -- ticking a cell assigns every zone (Single Program scope, any oscillator) whose
// key range covers that note to the selected bus; unticking sets those same zones back to 0 (off).
// Operates directly on the live Sample Bank catalog via p->sample_engine.slots -- both hosts'
// sample_engine.slots always points at the SAME struct the audio thread itself reads (see
// HdsSampleEngine's own comment), so this is a real, immediate, in-process struct write, no OSC
// round-trip needed and no reload required. ---
#define HDS_ROUTING_DB_MARGIN_TOP 26.0
#define HDS_ROUTING_DB_TITLE_H 18.0
#define HDS_ROUTING_DB_STEPPER_H 22.0
#define HDS_ROUTING_DB_STEPPER_GAP 8.0
#define HDS_ROUTING_DB_COLHDR_H 16.0
#define HDS_ROUTING_DB_CELL_W 32.0
#define HDS_ROUTING_DB_CELL_H 22.0
#define HDS_ROUTING_DB_CELL_PAD 3.0
#define HDS_ROUTING_DB_ROWLBL_W 26.0
#define HDS_ROUTING_DB_COLS 7
#define HDS_ROUTING_DB_ROWS 8

// Natural note letters, exactly the order asked for (A B C D E F G left-to-right), each mapped to
// its own standard scientific-pitch semitone offset within an octave (C=0 .. B=11) so the actual
// MIDI note computed per cell is correct regardless of this display order.
static const char HDS_ROUTING_DB_LETTERS[HDS_ROUTING_DB_COLS] = {'A', 'B', 'C', 'D', 'E', 'F', 'G'};
static const int HDS_ROUTING_DB_SEMITONE[HDS_ROUTING_DB_COLS] = {9, 11, 0, 2, 4, 5, 7};

// col 0-6 = note letter (A-G), row 0-7 = octave 1-8 (row 0 = octave 1, at the top).
static int hds_routing_db_note(int col, int row) {
    int octave = row + 1;
    return (octave + 1) * 12 + HDS_ROUTING_DB_SEMITONE[col];
}

typedef struct {
    HdsFxRect title_area;
    HdsFxRect prev_btn, next_btn;
    HdsFxRect cell[HDS_ROUTING_DB_COLS][HDS_ROUTING_DB_ROWS];
} HdsRoutingDrumBusLayout;

static double hds_routing_db_top_y(double scroll_offset_px) {
    return HDS_ROUTING_HEADER_H + SOURCE_COUNT * HDS_ROUTING_ROW_H + HDS_ROUTING_DB_MARGIN_TOP - scroll_offset_px;
}

static void hds_clap_routing_compute_drumbus_layout(double scroll_offset_px, HdsRoutingDrumBusLayout *out) {
    memset(out, 0, sizeof(*out));
    double top = hds_routing_db_top_y(scroll_offset_px);
    out->title_area = (HdsFxRect){HDS_ROUTING_MARGIN, top, 300.0, HDS_ROUTING_DB_TITLE_H};
    double stepper_y = top + HDS_ROUTING_DB_TITLE_H + 6.0;
    out->prev_btn = (HdsFxRect){HDS_ROUTING_MARGIN + 130.0, stepper_y, HDS_ROUTING_DB_STEPPER_H, HDS_ROUTING_DB_STEPPER_H};
    out->next_btn = (HdsFxRect){HDS_ROUTING_MARGIN + 130.0 + HDS_ROUTING_DB_STEPPER_H + 60.0, stepper_y, HDS_ROUTING_DB_STEPPER_H, HDS_ROUTING_DB_STEPPER_H};
    double grid_top = stepper_y + HDS_ROUTING_DB_STEPPER_H + HDS_ROUTING_DB_STEPPER_GAP + HDS_ROUTING_DB_COLHDR_H;
    double grid_left = HDS_ROUTING_MARGIN + HDS_ROUTING_DB_ROWLBL_W;
    for (int col = 0; col < HDS_ROUTING_DB_COLS; col++) {
        for (int row = 0; row < HDS_ROUTING_DB_ROWS; row++) {
            out->cell[col][row] = (HdsFxRect){
                grid_left + col * HDS_ROUTING_DB_CELL_W, grid_top + row * HDS_ROUTING_DB_CELL_H,
                HDS_ROUTING_DB_CELL_W - HDS_ROUTING_DB_CELL_PAD, HDS_ROUTING_DB_CELL_H - HDS_ROUTING_DB_CELL_PAD
            };
        }
    }
}

// The whole tab's own content height, unscrolled -- feeds hds_clap_routing_on_scroll()'s own
// max-scroll clamp. Always the Key Routing grid's own last row, since it's the last thing drawn.
static double hds_clap_routing_content_h(void) {
    HdsRoutingDrumBusLayout L;
    hds_clap_routing_compute_drumbus_layout(0.0, &L);
    return L.cell[0][HDS_ROUTING_DB_ROWS - 1].y + L.cell[0][HDS_ROUTING_DB_ROWS - 1].h + HDS_ROUTING_MARGIN;
}

// True if any active, Single-Program-scope zone whose key range covers `note` is currently
// assigned to `bus` -- drives both the checkbox's drawn state and which way a click toggles it.
static bool hds_routing_db_note_on(HdsSampleEngine *se, int note, int bus) {
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        SampleSlot *slot = &se->slots->sample_slots[s];
        if (!slot->active || slot->owner_program != SINGLE_PROGRAM_SLOT) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (slot->drum_bus == bus) return true;
    }
    return false;
}

// Toggles EVERY matching zone (see hds_routing_db_note_on's own scope) between `bus` and off (0)
// for one note -- if several velocity layers/round-robin zones share this key, they all move
// together, since a per-note checkbox has no way to address just one of them individually.
static void hds_routing_db_toggle_note(HdsSampleEngine *se, int note, int bus) {
    bool currently_on = hds_routing_db_note_on(se, note, bus);
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        SampleSlot *slot = &se->slots->sample_slots[s];
        if (!slot->active || slot->owner_program != SINGLE_PROGRAM_SLOT) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        slot->drum_bus = currently_on ? 0 : bus;
    }
}

static void hds_clap_routing_render(cairo_t *cr, HdsClapPlugin *p, HdsClapRoutingViewState *vs, double window_w, double window_h) {
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.0);
    hds_fx_rgb(cr, 0.5, 0.5, 0.53);
    hds_fx_left_text(cr, HDS_ROUTING_MARGIN, HDS_ROUTING_HEADER_H - 10.0, "Source");
    hds_fx_left_text(cr, 126.0, HDS_ROUTING_HEADER_H - 10.0, "Enable");
    hds_fx_left_text(cr, 190.0, HDS_ROUTING_HEADER_H - 10.0, "Gain");
    hds_fx_left_text(cr, 225.0, HDS_ROUTING_HEADER_H - 10.0, "Stereo");
#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int p = 0; p < OUTPUT_PAIR_MAX; p++) {
        char hdr[8]; snprintf(hdr, sizeof(hdr), "%d", p + 1);
        hds_fx_centered_text(cr, 290.0 + p * HDS_ROUTING_OUT_PILL_STRIDE + HDS_ROUTING_OUT_PILL_W / 2.0, HDS_ROUTING_HEADER_H - 10.0, hdr);
    }
    hds_fx_left_text(cr, 290.0 + OUTPUT_PAIR_MAX * HDS_ROUTING_OUT_PILL_STRIDE + 14.0, HDS_ROUTING_HEADER_H - 10.0, "VU (Ext In)");
#else
    hds_fx_left_text(cr, 290.0, HDS_ROUTING_HEADER_H - 10.0, "Main Out");
    hds_fx_left_text(cr, 380.0, HDS_ROUTING_HEADER_H - 10.0, "VU (Ext In)");
#endif

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    for (int src = 0; src < SOURCE_COUNT; src++) {
        HdsRoutingRowLayout L;
        hds_clap_routing_compute_row_layout(src, vs->scroll_offset_px, &L);
        bool ext_in = hds_routing_is_ext_in(src);

        char name[16]; hds_clap_source_label(src, name, sizeof(name));
        hds_fx_rgb(cr, 0.75, 0.75, 0.78);
        hds_fx_left_text(cr, L.name_area.x, L.name_area.y + HDS_ROUTING_ROW_H / 2 + 3.0, name);

#ifdef HDS_STANDALONE_GUI_ENABLED
        for (int op = 0; op < OUTPUT_PAIR_MAX; op++) {
            bool on = p->knobs.source_routing[src].enable[op] != 0.0f;
            hds_fx_draw_pill(cr, L.out_pill[op].x, L.out_pill[op].y, L.out_pill[op].w, L.out_pill[op].h, on ? "\xe2\x9c\x93" : "", on, false);
        }
#else
        bool main_out = p->knobs.source_routing[src].enable[0] != 0.0f;
        hds_fx_draw_pill(cr, L.mainout_pill.x, L.mainout_pill.y, L.mainout_pill.w, L.mainout_pill.h, main_out ? "On" : "Off", main_out, false);
#endif

        if (!ext_in) continue;
        int pair = hds_routing_ext_in_pair(src);
        bool enabled = p->knobs.ext_in_enable[pair] != 0.0f;
        hds_fx_draw_pill(cr, L.enable_pill.x, L.enable_pill.y, L.enable_pill.w, L.enable_pill.h, enabled ? "On" : "Off", enabled, false);

        float gain = p->knobs.ext_in_gain[pair];
        char gtxt[16]; snprintf(gtxt, sizeof(gtxt), "%d%%", (int)(gain * 100.0f + 0.5f));
        hds_play_draw_knob(cr, L.gain_knob.x, L.gain_knob.y, L.gain_knob.w, gain / 2.0f, false, NULL, gtxt);

        bool stereo = p->knobs.ext_in_stereo[pair] != 0.0f;
        hds_fx_draw_pill(cr, L.stereo_pill.x, L.stereo_pill.y, L.stereo_pill.w, L.stereo_pill.h, stereo ? "Stereo" : "Mono", stereo, false);

        float peak = p->ext_in_peak[pair];
        if (peak < 0.0f) peak = 0.0f; else if (peak > 1.0f) peak = 1.0f;
        hds_fx_rgb(cr, 0.16, 0.16, 0.19);
        cairo_new_path(cr); cairo_rectangle(cr, L.vu_bar.x, L.vu_bar.y, L.vu_bar.w, L.vu_bar.h); cairo_fill(cr);
        if (peak > 0.001f) {
            double fill_w = L.vu_bar.w * peak;
            if (peak < 0.7) hds_fx_rgb(cr, 0.25, 0.75, 0.35);
            else if (peak < 0.92) hds_fx_rgb(cr, 0.85, 0.75, 0.20);
            else hds_fx_rgb(cr, 0.85, 0.25, 0.25);
            cairo_new_path(cr); cairo_rectangle(cr, L.vu_bar.x, L.vu_bar.y, fill_w, L.vu_bar.h); cairo_fill(cr);
        }
    }

    // Drum Bus Key Routing -- see this section's own header comment above hds_routing_db_note().
    {
        HdsRoutingDrumBusLayout DL;
        hds_clap_routing_compute_drumbus_layout(vs->scroll_offset_px, &DL);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        hds_fx_rgb(cr, 0.75, 0.75, 0.78);
        hds_fx_left_text(cr, DL.title_area.x, DL.title_area.y + 12.0, "Drum Bus Key Routing");

        hds_fx_draw_pill(cr, DL.prev_btn.x, DL.prev_btn.y, DL.prev_btn.w, DL.prev_btn.h, "\xe2\x97\x84", false, false);
        hds_fx_draw_pill(cr, DL.next_btn.x, DL.next_btn.y, DL.next_btn.w, DL.next_btn.h, "\xe2\x96\xb6", false, false);
        char bus_label[16]; snprintf(bus_label, sizeof(bus_label), "Bus %d", vs->drum_bus_selected);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        hds_fx_rgb(cr, 0.85, 0.85, 0.88);
        hds_fx_centered_text(cr, (DL.prev_btn.x + DL.prev_btn.w + DL.next_btn.x) / 2.0, DL.prev_btn.y + DL.prev_btn.h / 2.0 + 4.0, bus_label);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.0);
        hds_fx_rgb(cr, 0.5, 0.5, 0.53);
        for (int col = 0; col < HDS_ROUTING_DB_COLS; col++) {
            char letter[2] = {HDS_ROUTING_DB_LETTERS[col], '\0'};
            hds_fx_centered_text(cr, DL.cell[col][0].x + DL.cell[col][0].w / 2.0, DL.cell[col][0].y - HDS_ROUTING_DB_COLHDR_H / 2.0, letter);
        }
        for (int row = 0; row < HDS_ROUTING_DB_ROWS; row++) {
            char oct[4]; snprintf(oct, sizeof(oct), "%d", row + 1);
            hds_fx_centered_text(cr, HDS_ROUTING_MARGIN + HDS_ROUTING_DB_ROWLBL_W / 2.0, DL.cell[0][row].y + DL.cell[0][row].h / 2.0 + 3.0, oct);
        }

        for (int col = 0; col < HDS_ROUTING_DB_COLS; col++) {
            for (int row = 0; row < HDS_ROUTING_DB_ROWS; row++) {
                int note = hds_routing_db_note(col, row);
                bool on = hds_routing_db_note_on(&p->sample_engine, note, vs->drum_bus_selected);
                HdsFxRect *c = &DL.cell[col][row];
                hds_fx_rgb(cr, on ? 0.20 : 0.16, on ? 0.55 : 0.16, on ? 0.28 : 0.19);
                cairo_new_path(cr); cairo_rectangle(cr, c->x, c->y, c->w, c->h); cairo_fill(cr);
                hds_fx_rgba(cr, 1, 1, 1, 0.12);
                cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, c->x + 0.5, c->y + 0.5, c->w - 1, c->h - 1); cairo_stroke(cr);
                if (on) {
                    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                    cairo_set_font_size(cr, 10.0);
                    hds_fx_rgb(cr, 0.85, 0.98, 0.88);
                    hds_fx_centered_text(cr, c->x + c->w / 2.0, c->y + c->h / 2.0 + 3.5, "\xe2\x9c\x93");
                }
            }
        }
    }
}

static bool hds_clap_routing_on_button_press(HdsClapPlugin *p, HdsClapRoutingViewState *vs, double x, double y, int button) {
    if (y < HDS_ROUTING_HEADER_H) return false;

    // Drum Bus Key Routing section, checked first: its own y range sits past every Source row
    // (SOURCE_COUNT * HDS_ROUTING_ROW_H), which the ordinary per-source hit test just below would
    // otherwise reject outright via its own src>=SOURCE_COUNT bound.
    {
        HdsRoutingDrumBusLayout DL;
        hds_clap_routing_compute_drumbus_layout(vs->scroll_offset_px, &DL);
        if (hds_play_rect_contains(&DL.prev_btn, x, y)) {
            if (button == 1) { vs->drum_bus_selected--; if (vs->drum_bus_selected < 1) vs->drum_bus_selected = DRUM_BUS_COUNT; }
            return true;
        }
        if (hds_play_rect_contains(&DL.next_btn, x, y)) {
            if (button == 1) { vs->drum_bus_selected++; if (vs->drum_bus_selected > DRUM_BUS_COUNT) vs->drum_bus_selected = 1; }
            return true;
        }
        for (int col = 0; col < HDS_ROUTING_DB_COLS; col++) {
            for (int row = 0; row < HDS_ROUTING_DB_ROWS; row++) {
                if (hds_play_rect_contains(&DL.cell[col][row], x, y)) {
                    if (button == 1) hds_routing_db_toggle_note(&p->sample_engine, hds_routing_db_note(col, row), vs->drum_bus_selected);
                    return true;
                }
            }
        }
    }

    int src = (int)((y - HDS_ROUTING_HEADER_H + vs->scroll_offset_px) / HDS_ROUTING_ROW_H);
    if (src < 0 || src >= SOURCE_COUNT) return false;

    HdsRoutingRowLayout L;
    hds_clap_routing_compute_row_layout(src, vs->scroll_offset_px, &L);

#ifdef HDS_STANDALONE_GUI_ENABLED
    for (int op = 0; op < OUTPUT_PAIR_MAX; op++) {
        if (hds_play_rect_contains(&L.out_pill[op], x, y)) {
            if (button == 1) {
                float *e = &p->knobs.source_routing[src].enable[op];
                *e = (*e != 0.0f) ? 0.0f : 1.0f;
            }
            return true;
        }
    }
#else
    if (hds_play_rect_contains(&L.mainout_pill, x, y)) {
        if (button == 1) {
            float *e = &p->knobs.source_routing[src].enable[0];
            *e = (*e != 0.0f) ? 0.0f : 1.0f;
        }
        return true;
    }
#endif
    if (hds_routing_is_ext_in(src)) {
        int pair = hds_routing_ext_in_pair(src);
        if (hds_play_rect_contains(&L.enable_pill, x, y)) {
            if (button == 1) p->knobs.ext_in_enable[pair] = (p->knobs.ext_in_enable[pair] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
        if (hds_play_knob_hit_test(&L.gain_knob, x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.ext_in_gain[pair], 0.0, 2.0, x, y, button, HDS_PLAY_NO_MENU);
            return true;
        }
        if (hds_play_rect_contains(&L.stereo_pill, x, y)) {
            if (button == 1) p->knobs.ext_in_stereo[pair] = (p->knobs.ext_in_stereo[pair] != 0.0f) ? 0.0f : 1.0f;
            return true;
        }
    }

    (void)button;
    return true;
}

static void hds_clap_routing_on_motion(HdsClapRoutingViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

static void hds_clap_routing_on_button_release(HdsClapRoutingViewState *vs) { hds_play_release_knob_drag(&vs->knob_drag, NULL, NULL); }

// Whole-tab vertical scroll (same convention as hds_clap_lfos_draw.c's own scroll_offset_px) --
// needed now that SOURCE_COUNT (41) plus the Drum Bus Key Routing section below it overflows any
// reasonable window height, with no way to reach the later rows/the grid at all otherwise.
static void hds_clap_routing_on_scroll(HdsClapRoutingViewState *vs, double window_h, int delta) {
    double content_h = hds_clap_routing_content_h();
    double max_scroll = content_h - window_h;
    if (max_scroll < 0.0) max_scroll = 0.0;
    vs->scroll_offset_px -= delta * HDS_ROUTING_ROW_H;
    if (vs->scroll_offset_px < 0.0) vs->scroll_offset_px = 0.0;
    if (vs->scroll_offset_px > max_scroll) vs->scroll_offset_px = max_scroll;
}
