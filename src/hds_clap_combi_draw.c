// hds_clap_combi_draw.c: hds.clap's own Combi mode 8-lane mixer view (task #106) -- the Play tab's
// own Mode toggle (Single/Combi, see hds_clap_play_draw.c's hds_clap_play_mode_toggle_rects() and
// hds_clap_gui_draw.c's own HDS_CLAP_TAB_PLAY dispatch) switches into THIS view instead of the
// oscillator/EG content when p->knobs.performance_mode == 1. A mixer-strip legend of "#", "Level",
// "Pan", "Program", "MIDI Ch" -- one lane per programs[0..7]/seq_lanes[0..7] (COMBI_PROGRAM_COUNT),
// every lane independently synthesizing, sharing one voice pool. backend.c's own render loop
// already does the real Combi mixing unconditionally (see its "Combi lane Level/Pan" comment near
// active_program_count) --
// this file is purely the GUI half; simply flipping performance_mode via the Mode toggle is enough
// to make a lane already carrying real programs[]/seq_lanes[] state audible.
//
// SCOPE for this pass: exactly the mixer-strip legend's own 5 columns. Several further per-lane
// controls are left out as real, separate follow-up
// work, not part of "the 8-lane mixer view" this task is scoped to: Key/Velocity zone (keyboard
// split/velocity switch), Transpose, Save Combi/Load Combi (a whole separate .hdscombi-file format
// referencing lanes BY NAME, itself session-level state), and the "Edit" button (jumps a lane's
// Program into Single Program mode for editing -- meaningless here anyway, since this native GUI's
// own Single Program view already IS the live editing surface, just for a different slot). A lane
// not yet touched here plays with its combi_lane_midi_channel/key/vel/transpose defaults (Omni,
// full key/vel range, no transpose) -- see backend.c's own init_default_controls.
//
// Program picker: a plain sorted dropdown of every ~/HDS_Programs/<name>/<name>.hdsprog (via
// backend.c's own hds_scan_programs_folder(), re-scanned fresh on every open -- cheap, a small
// local directory listing, never a hot path) -- every real Program lives flat under that one root,
// so a flat alphabetical list is far simpler than
// generalizing hds_fx_gui_browser.c's own directory-navigating browser (built for .nam/.wav models,
// which can live anywhere) for a job that never needs navigation at all. Picking an entry calls
// hds_load_program_file_to_combi_lane() directly (backend.c) -- no OSC round-trip, no frontend
// needed, matching how every other control in this native GUI reads/writes p->knobs in-process.
// Deliberately does NOT load sfz_file (a Program's own sampled-oscillator zones): this native GUI
// has no way to assign a Combi lane's own Multisample zones at all yet (the Play tab's own
// per-oscillator "Load .sfz..." button only ever targets Single Program mode's own SINGLE_PROGRAM_
// SLOT, never a Combi lane) -- see hds_apply_program_fields_to_combi_lane()'s own comment below.
// The list is capped at HDS_COMBI_PROGRAM_LIST_MAX entries with no scrolling -- a deliberate v1
// scale limit (see this file's own dropdown renderer): comfortably covers real-world use (a few
// dozen saved Programs) without building a second scrollable-list mechanism just for this.
//
// A picked name is cached here purely as a GUI-side label -- the backend has no "name of Program
// that produced these knob values" concept, so this label is never part of a saved patch/session;
// a fresh GUI open shows "(none)" for every lane regardless of what's actually loaded onto
// programs[].
//
// #include'd by clap_plugin.c right after hds_clap_play_draw.c (needs its HdsPlayKnobDrag/
// hds_play_draw_knob()/hds_play_knob_hit_test()/hds_play_rect_contains()/hds_play_start_knob_drag()/
// HDS_PLAY_DRAG_PIXELS_FULL_RANGE/HDS_PLAY_CONTENT_TOP -- same generic-primitive-reuse pattern every
// tab file after Play has used) and before hds_clap_gui_draw.c (whose own HDS_CLAP_TAB_PLAY
// dispatch calls this file's render/press/motion/release functions directly, not
// hds_clap_play_draw.c -- the two tab-file bodies never call each other).

#include "hds_clap_gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Loads a saved .hdsprog's "fields" object onto Combi lane `lane_idx` (0..COMBI_PROGRAM_COUNT-1,
// i.e. programs[lane_idx]) DIRECTLY -- no OSC round-trip, no frontend needed, matching
// hds_load_program_file()'s own "reuse osc_dispatch_table, no OSC needed" posture in
// hds_standalone.c (backend.c's own JSON reader + hds_scan_programs_folder() are shared with that
// file; these two functions are CLAP-only, so they live here instead -- see backend.c's own
// comment at the point they used to sit). Does the same address-prefix rewrite (leading
// "/hds" -> "/hds/combi/{lane_idx}") an OSC-driven loader would, but in-process: applying straight
// into `knobs` since the GUI and the audio thread already share it.
// Deliberately does NOT touch sfz_file (a Program's own per-oscillator sampled zones) -- this
// native GUI has no way to assign a Combi lane's own Multisample zones at all yet (see this file's
// own header comment), so a Combi lane picked here gets every synthesis field (oscillators/
// filters/LFOs/EGs/arp/etc.) but keeps whatever sample zones (if any) that lane already owned;
// loading a Program that itself relies on sampled oscillators is a known, documented gap.
static void hds_apply_program_fields_to_combi_lane(SynthSharedControls *knobs, const HdsJsonValue *fields_obj, int lane_idx) {
    if (!fields_obj || fields_obj->type != HDS_JSON_OBJECT) return;
    for (const HdsJsonMember *m = fields_obj->first_member; m; m = m->next) {
        if (!m->key || strncmp(m->key, "/hds", 4) != 0) continue;
        char new_addr[192];
        snprintf(new_addr, sizeof(new_addr), "/hds/combi/%d%s", lane_idx, m->key + 4);
        const OscDispatchSlot *slot = osc_dispatch_lookup(new_addr);
        if (!slot) continue; // e.g. a session-level or sfz_file-adjacent key with no per-lane mirror
        if (slot->kind == OSC_KIND_FLOAT) {
            if (m->value->type != HDS_JSON_NUMBER) continue;
            *(float *)((char *)knobs + slot->offset) = (float)m->value->number;
        } else { // OSC_KIND_STRING
            if (m->value->type != HDS_JSON_STRING) continue;
            snprintf((char *)knobs + slot->offset, slot->str_len, "%s", m->value->string);
        }
    }
}

// Reads a whole .hdsprog file and applies its "fields" onto Combi lane `lane_idx` via
// hds_apply_program_fields_to_combi_lane() above -- the file-read/parse half of the same job
// hds_load_program_file() does in hds_standalone.c, kept as its own small copy here (rather than a
// shared helper taking a callback) since the two differ in exactly one thing: the destination
// (SINGLE_PROGRAM_SLOT there, an arbitrary Combi lane here) and whether sfz_file applies (it
// doesn't, here -- see hds_apply_program_fields_to_combi_lane()'s own comment). A bad/missing/
// malformed file is silently ignored, same failure posture as every other Program-loading path in
// this codebase.
static void hds_load_program_file_to_combi_lane(SynthSharedControls *knobs, const char *path, int lane_idx) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';

    HdsJsonValue *root = hds_json_parse(buf);
    free(buf);
    if (!root || root->type != HDS_JSON_OBJECT) { hds_json_free(root); return; }

    const HdsJsonValue *fields = hds_json_object_get(root, "fields");
    if (fields) hds_apply_program_fields_to_combi_lane(knobs, fields, lane_idx);

    hds_json_free(root);
}

#define HDS_COMBI_DROPDOWN_NONE 0
#define HDS_COMBI_DROPDOWN_PROGRAM 1
#define HDS_COMBI_DROPDOWN_MIDI 2

#define HDS_COMBI_MARGIN 10.0
#define HDS_COMBI_LANE_W 140.0
#define HDS_COMBI_LANE_GAP 6.0
#define HDS_COMBI_KNOB_R 10.0
#define HDS_COMBI_PILL_H 16.0
#define HDS_COMBI_DD_ROW_H 16.0

void hds_clap_combi_view_state_init(HdsClapCombiViewState *vs) {
    memset(vs, 0, sizeof(*vs));
    vs->dropdown_kind = HDS_COMBI_DROPDOWN_NONE;
}

typedef struct {
    HdsFxRect level_knob, pan_knob, program_pill, midi_pill;
} HdsCombiLaneLayout;

static double hds_combi_lane_x(int lane) { return HDS_COMBI_MARGIN + lane * (HDS_COMBI_LANE_W + HDS_COMBI_LANE_GAP); }

static void hds_combi_compute_lane_layout(int lane, double content_top, HdsCombiLaneLayout *out) {
    double x = hds_combi_lane_x(lane);
    double cx = x + HDS_COMBI_LANE_W / 2.0;
    double level_cy = content_top + 34.0;
    double pan_cy = level_cy + 42.0;
    out->level_knob = (HdsFxRect){cx, level_cy, HDS_COMBI_KNOB_R, HDS_COMBI_KNOB_R};
    out->pan_knob = (HdsFxRect){cx, pan_cy, HDS_COMBI_KNOB_R, HDS_COMBI_KNOB_R};
    double pill_y = pan_cy + HDS_COMBI_KNOB_R + 18.0; // room for Pan's own value text below it
    out->program_pill = (HdsFxRect){x, pill_y, HDS_COMBI_LANE_W, HDS_COMBI_PILL_H};
    out->midi_pill = (HdsFxRect){x, pill_y + HDS_COMBI_PILL_H + 4.0, HDS_COMBI_LANE_W, HDS_COMBI_PILL_H};
}

// Shared dropdown-list chrome (background/border, "opens upward if it wouldn't fit below")-- same
// shape as every other dropdown in this GUI (see hds_clap_devices_draw.c's own Block Size list for
// the most recent copy of this exact pattern). Returns the list's own top-left y so callers can
// hit-test the exact same rows they just drew.
static double hds_combi_dropdown_list_y(const HdsFxRect *anchor, double list_h, double window_h) {
    double below_y = anchor->y + anchor->h + 2.0;
    double y0 = (below_y + list_h > window_h) ? (anchor->y - list_h - 2.0) : below_y;
    if (y0 < 0.0) y0 = 0.0;
    return y0;
}

static void hds_combi_render_dropdown_chrome(cairo_t *cr, const HdsFxRect *anchor, double list_h, double window_h, HdsFxRect *out_bg) {
    double y0 = hds_combi_dropdown_list_y(anchor, list_h, window_h);
    *out_bg = (HdsFxRect){anchor->x, y0, anchor->w, list_h};
    hds_fx_rgb(cr, 0.13, 0.13, 0.16);
    cairo_new_path(cr); cairo_rectangle(cr, out_bg->x, out_bg->y, out_bg->w, out_bg->h); cairo_fill(cr);
    hds_fx_rgba(cr, 1, 1, 1, 0.25);
    cairo_new_path(cr); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, out_bg->x + 0.5, out_bg->y + 0.5, out_bg->w - 1, out_bg->h - 1); cairo_stroke(cr);
}

static const char *hds_combi_midi_channel_label(int ch, char *buf, size_t buflen) {
    if (ch <= 0) { snprintf(buf, buflen, "Omni"); return buf; }
    snprintf(buf, buflen, "Ch %d", ch);
    return buf;
}

void hds_clap_combi_render(cairo_t *cr, HdsClapPlugin *p, HdsClapCombiViewState *vs, double window_w, double window_h) {
    (void)window_w;
    hds_fx_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, window_w, window_h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9.5);
    for (int lane = 0; lane < COMBI_PROGRAM_COUNT; lane++) {
        HdsCombiLaneLayout L;
        hds_combi_compute_lane_layout(lane, HDS_PLAY_CONTENT_TOP, &L);
        double cx = hds_combi_lane_x(lane) + HDS_COMBI_LANE_W / 2.0;

        char lane_num[4]; snprintf(lane_num, sizeof(lane_num), "%02d", lane + 1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.5);
        hds_fx_rgb(cr, 0.80, 0.82, 0.85);
        hds_fx_centered_text(cr, cx, HDS_PLAY_CONTENT_TOP + 2.0, lane_num);

        float level = p->knobs.seq_lanes[lane].volume;
        char level_txt[16]; snprintf(level_txt, sizeof(level_txt), "%d", (int)(level + (level >= 0 ? 0.5f : -0.5f)));
        hds_play_draw_knob(cr, L.level_knob.x, L.level_knob.y, L.level_knob.w, (float)(level / 100.0), false, "Level", level_txt);

        float pan = p->knobs.seq_lanes[lane].pan;
        char pan_txt[16]; snprintf(pan_txt, sizeof(pan_txt), "%+d%%", (int)(pan * 100.0 + (pan >= 0 ? 0.5 : -0.5)));
        hds_play_draw_knob(cr, L.pan_knob.x, L.pan_knob.y, L.pan_knob.w, (float)((pan + 1.0) / 2.0), false, "Pan", pan_txt);

        const char *prog_label = vs->lane_program_label[lane][0] ? vs->lane_program_label[lane] : "(none)";
        bool prog_open = (vs->dropdown_kind == HDS_COMBI_DROPDOWN_PROGRAM && vs->dropdown_lane == lane);
        hds_fx_draw_pill(cr, L.program_pill.x, L.program_pill.y, L.program_pill.w, L.program_pill.h, prog_label, prog_open, false);

        char ch_buf[16];
        hds_combi_midi_channel_label((int)p->knobs.combi_lane_midi_channel[lane], ch_buf, sizeof(ch_buf));
        bool midi_open = (vs->dropdown_kind == HDS_COMBI_DROPDOWN_MIDI && vs->dropdown_lane == lane);
        hds_fx_draw_pill(cr, L.midi_pill.x, L.midi_pill.y, L.midi_pill.w, L.midi_pill.h, ch_buf, midi_open, false);
    }

    // Whichever dropdown is open draws LAST, on top of every lane -- same rule every other tab's
    // own dropdown follows.
    if (vs->dropdown_kind == HDS_COMBI_DROPDOWN_PROGRAM) {
        int n = vs->program_list_count;
        double list_h = (n > 0 ? n : 1) * HDS_COMBI_DD_ROW_H;
        HdsFxRect bg;
        hds_combi_render_dropdown_chrome(cr, &vs->dropdown_anchor, list_h, window_h, &bg);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5);
        if (n == 0) {
            hds_fx_rgb(cr, 0.55, 0.55, 0.58);
            hds_fx_left_text(cr, bg.x + 5, bg.y + HDS_COMBI_DD_ROW_H / 2 + 3.0, "(no saved Programs)");
        } else {
            for (int i = 0; i < n; i++) {
                double ry = bg.y + i * HDS_COMBI_DD_ROW_H;
                hds_fx_rgb(cr, 0.85, 0.87, 0.89);
                hds_fx_left_text(cr, bg.x + 5, ry + HDS_COMBI_DD_ROW_H / 2 + 3.0, vs->program_list_cache[i]);
            }
        }
    } else if (vs->dropdown_kind == HDS_COMBI_DROPDOWN_MIDI) {
        double list_h = 17.0 * HDS_COMBI_DD_ROW_H;
        HdsFxRect bg;
        hds_combi_render_dropdown_chrome(cr, &vs->dropdown_anchor, list_h, window_h, &bg);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5);
        int current_ch = (int)p->knobs.combi_lane_midi_channel[vs->dropdown_lane];
        for (int i = 0; i < 17; i++) {
            double ry = bg.y + i * HDS_COMBI_DD_ROW_H;
            if (i == current_ch) {
                hds_fx_rgb(cr, 0.22, 0.32, 0.36);
                cairo_new_path(cr); cairo_rectangle(cr, bg.x, ry, bg.w, HDS_COMBI_DD_ROW_H); cairo_fill(cr);
            }
            char buf[16]; hds_combi_midi_channel_label(i, buf, sizeof(buf));
            hds_fx_rgb(cr, i == current_ch ? 0.92 : 0.78, i == current_ch ? 0.95 : 0.78, i == current_ch ? 0.92 : 0.81);
            hds_fx_left_text(cr, bg.x + 5, ry + HDS_COMBI_DD_ROW_H / 2 + 3.0, buf);
        }
    }
}

bool hds_clap_combi_on_button_press(HdsClapPlugin *p, HdsClapCombiViewState *vs, double window_h, double x, double y, int button) {
    if (vs->dropdown_kind == HDS_COMBI_DROPDOWN_PROGRAM) {
        int n = vs->program_list_count;
        double list_h = (n > 0 ? n : 1) * HDS_COMBI_DD_ROW_H;
        double y0 = hds_combi_dropdown_list_y(&vs->dropdown_anchor, list_h, window_h);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * HDS_COMBI_DD_ROW_H, vs->dropdown_anchor.w, HDS_COMBI_DD_ROW_H};
                if (hds_play_rect_contains(&r, x, y)) {
                    if (button == 1) {
                        int lane = vs->dropdown_lane;
                        // Copied to a local stack buffer first (rather than reading
                        // vs->program_list_cache[i] directly into vs->lane_program_label[lane]
                        // below) so the two are provably disjoint objects to the compiler -- with
                        // this whole call chain inlined all the way up into the X11 event pump
                        // (hds_clap_gui_pump_events), GCC's -Wrestrict otherwise can't prove two
                        // DIFFERENT members of the same vs-> struct never alias and flags a false
                        // positive.
                        char picked_name[HDS_COMBI_PROGRAM_NAME_LEN];
                        snprintf(picked_name, sizeof(picked_name), "%s", vs->program_list_cache[i]);
                        const char *home = getenv("HOME");
                        if (home) {
                            char path[SAMPLE_PATH_LEN + 2 * (HDS_COMBI_PROGRAM_NAME_LEN + 1) + 32];
                            snprintf(path, sizeof(path), "%s/HDS_Programs/%s/%s.hdsprog", home, picked_name, picked_name);
                            hds_load_program_file_to_combi_lane(&p->knobs, path, lane);
                            snprintf(vs->lane_program_label[lane], sizeof(vs->lane_program_label[lane]), "%s", picked_name);
                        }
                    }
                    vs->dropdown_kind = HDS_COMBI_DROPDOWN_NONE;
                    return true;
                }
            }
        }
        vs->dropdown_kind = HDS_COMBI_DROPDOWN_NONE; // clicked outside the open list -- just close it
        return true;
    }
    if (vs->dropdown_kind == HDS_COMBI_DROPDOWN_MIDI) {
        double list_h = 17.0 * HDS_COMBI_DD_ROW_H;
        double y0 = hds_combi_dropdown_list_y(&vs->dropdown_anchor, list_h, window_h);
        for (int i = 0; i < 17; i++) {
            HdsFxRect r = {vs->dropdown_anchor.x, y0 + i * HDS_COMBI_DD_ROW_H, vs->dropdown_anchor.w, HDS_COMBI_DD_ROW_H};
            if (hds_play_rect_contains(&r, x, y)) {
                if (button == 1) p->knobs.combi_lane_midi_channel[vs->dropdown_lane] = (float)i; // 0=Omni
                vs->dropdown_kind = HDS_COMBI_DROPDOWN_NONE;
                return true;
            }
        }
        vs->dropdown_kind = HDS_COMBI_DROPDOWN_NONE;
        return true;
    }

    for (int lane = 0; lane < COMBI_PROGRAM_COUNT; lane++) {
        HdsCombiLaneLayout L;
        hds_combi_compute_lane_layout(lane, HDS_PLAY_CONTENT_TOP, &L);
        if (hds_play_knob_hit_test(&L.level_knob, x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.seq_lanes[lane].volume, 0.0, 100.0, x, y, button, HDS_PLAY_NO_MENU);
            return true;
        }
        if (hds_play_knob_hit_test(&L.pan_knob, x, y)) {
            hds_play_start_knob_drag(&vs->knob_drag, &p->knobs.seq_lanes[lane].pan, -1.0, 1.0, x, y, button, HDS_PLAY_NO_MENU);
            return true;
        }
        if (hds_play_rect_contains(&L.program_pill, x, y)) {
            vs->dropdown_kind = HDS_COMBI_DROPDOWN_PROGRAM;
            vs->dropdown_lane = lane;
            vs->dropdown_anchor = L.program_pill;
            int count = 0;
            char **names = hds_scan_programs_folder(&count);
            vs->program_list_count = 0;
            if (names) {
                for (int i = 0; i < count && i < HDS_COMBI_PROGRAM_LIST_MAX; i++) {
                    snprintf(vs->program_list_cache[i], HDS_COMBI_PROGRAM_NAME_LEN, "%s", names[i]);
                    vs->program_list_count++;
                }
                for (int i = 0; i < count; i++) free(names[i]);
                free(names);
            }
            return true;
        }
        if (hds_play_rect_contains(&L.midi_pill, x, y)) {
            vs->dropdown_kind = HDS_COMBI_DROPDOWN_MIDI;
            vs->dropdown_lane = lane;
            vs->dropdown_anchor = L.midi_pill;
            return true;
        }
    }
    (void)button;
    return false;
}

void hds_clap_combi_on_motion(HdsClapCombiViewState *vs, double x, double y) {
    hds_play_apply_knob_drag(&vs->knob_drag, x, y);
}

void hds_clap_combi_on_button_release(HdsClapCombiViewState *vs) { hds_play_release_knob_drag(&vs->knob_drag, NULL, NULL); }
