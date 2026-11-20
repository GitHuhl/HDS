// test_clap_patches.c -- renders a set of sounds ("patches": combinations of synth settings, not
// files) through the built hds.clap and fingerprints each, so the voice engine's code paths beyond
// the default saw patch -- every oscillator shape, multiple oscillators, FM, hard sync, filters,
// envelopes, LFO modulation, mono glide, the arpeggiator -- are covered by make test.
//
// Each patch gets a fresh plugin instance (fresh default settings), its settings applied over OSC
// (UDP 9010, drained at the start of each process() block), then the same note sequence: a C major
// chord for 0.5 s, then two overlapping notes (exercising legato/glide). Checks per patch: output
// finite and audible; and every patch must sound different from every other one, so a setting that
// silently stops having any effect fails here rather than passing unnoticed. Where a patch needs
// extra settings to make its feature audible (fm, filter_keytrk), a control patch with just those
// extra settings (two_sines, filter_lp300) makes "distinct" prove the feature itself is working.
// Usage: test_clap_patches <hds.clap>
#include "test_common.h"
#include "clap_test_host.h"

#include <lo/lo.h>
#include <stdlib.h>

#define BS 256
#define TOTAL_BLOCKS 300 // 1.6 s

typedef struct { const char *addr; float value; } Setting;
typedef struct { const char *name; Setting set[8]; } Patch;

static const Patch PATCHES[] = {
    { "default",       { {0} } },
    { "square",        { {"/hds/vco1/shape", 1} } },
    { "triangle",      { {"/hds/vco1/shape", 2} } },
    { "sine",          { {"/hds/vco1/shape", 3} } },
    { "unison_saw",    { {"/hds/vco1/shape", 4}, {"/hds/vco1/unison/detune", 30}, {"/hds/vco1/unison/width", 80} } },
    { "noise",         { {"/hds/vco1/shape", 5} } },
    { "pulse",         { {"/hds/vco1/shape", 6}, {"/hds/vco1/pulse_width", 20} } },
    { "two_osc",       { {"/hds/vco2/shape", 0}, {"/hds/vco2/vol", 0.6f}, {"/hds/vco2/pitch_offset", 7} } },
    { "hard_sync",     { {"/hds/vco2/shape", 0}, {"/hds/vco2/vol", 0.6f}, {"/hds/vco2/pitch_offset", 7}, {"/hds/hard_sync_mode", 1} } },
    { "two_sines",     { {"/hds/vco1/shape", 3}, {"/hds/vco2/shape", 3}, {"/hds/vco2/vol", 0.5f} } },                    // control for "fm"
    { "fm",            { {"/hds/vco1/shape", 3}, {"/hds/vco2/shape", 3}, {"/hds/vco2/vol", 0.5f}, {"/hds/fm/intensity", 0.5f}, {"/hds/fm/depth/1_2", 0.5f} } },
    { "filter_lp",     { {"/hds/filter/0/type", 1}, {"/hds/filter/0/cutoff", 600}, {"/hds/filter/0/res_q", 70}, {"/hds/filter/0/slope", 1} } },
    { "filter_lp300",  { {"/hds/filter/0/type", 1}, {"/hds/filter/0/cutoff", 300} } },                                   // control for "filter_keytrk"
    { "filter_keytrk", { {"/hds/filter/0/type", 1}, {"/hds/filter/0/cutoff", 300}, {"/hds/filter/0/key_track", 100} } },
    { "amp_env",       { {"/hds/eg/0/attack", 200}, {"/hds/eg/0/decay", 300}, {"/hds/eg/0/sustain", 40}, {"/hds/eg/0/release", 400} } },
    { "lfo_pitch",     { {"/hds/lfo/0/wave", 0}, {"/hds/lfo/0/rate", 6}, {"/hds/lfo/0/depth", 30}, {"/hds/lfo/0/dest", 1} } },
    { "lfo_filter",    { {"/hds/filter/0/type", 1}, {"/hds/filter/0/cutoff", 800}, {"/hds/lfo/0/wave", 0}, {"/hds/lfo/0/rate", 3}, {"/hds/lfo/0/depth", 60}, {"/hds/lfo/0/dest", 90} } },
    { "mono_glide",    { {"/hds/is_polyphonic", 0}, {"/hds/portamento_time_ms", 200} } },
    { "arpeggiator",   { {"/hds/arp/enabled", 1}, {"/hds/arp/rate", 2} } },
    { "string_pluck",  { {"/hds/vco1/shape", 10} } },
    { "string_hammer", { {"/hds/vco1/shape", 10}, {"/hds/str/0/exciter", 2}, {"/hds/str/0/stiffness", 40}, {"/hds/str/0/buzz", 30} } },
    { "string_bow",    { {"/hds/vco1/shape", 10}, {"/hds/str/0/exciter", 4}, {"/hds/str/0/harmonic_node", 3} } },
};
#define NPATCHES ((int)(sizeof PATCHES / sizeof PATCHES[0]))

typedef struct { clap_event_note_t ev[8]; int n; } EvList;
static uint32_t ev_size(const clap_input_events_t *l) { return (uint32_t)((const EvList *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((const EvList *)l->ctx)->ev[i].header; }
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }
static void add_note(EvList *l, uint16_t type, int key, double vel) {
    clap_event_note_t *e = &l->ev[l->n++]; memset(e, 0, sizeof *e);
    e->header = (clap_event_header_t){ sizeof *e, 0, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
    e->note_id = -1; e->key = (int16_t)key; e->velocity = vel;
}

// Renders one patch on a fresh instance; returns its fingerprint (0 on load failure).
static uint64_t render_patch(const char *clap_path, lo_address osc, const Patch *pt) {
    TestPlugin tp; if (test_plugin_load(&tp, clap_path)) { g_test_failures++; return 0; }
    const clap_plugin_t *p = tp.plugin;
    const clap_plugin_audio_ports_t *ap = (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    uint32_t nin = ap->count(p, true), nout = ap->count(p, false), outch[32];
    clap_audio_buffer_t in[32], out[32]; float *indata[32][2], *outdata[32][2];
    for (uint32_t i = 0; i < nin; i++) { indata[i][0] = calloc(BS, 4); indata[i][1] = calloc(BS, 4); in[i] = (clap_audio_buffer_t){ .data32 = indata[i], .channel_count = 2 }; }
    for (uint32_t i = 0; i < nout; i++) {
        clap_audio_port_info_t info; ap->get(p, i, false, &info);
        outch[i] = info.channel_count > 2 ? 2 : info.channel_count;
        for (uint32_t c = 0; c < outch[i]; c++) outdata[i][c] = calloc(BS, 4);
        out[i] = (clap_audio_buffer_t){ .data32 = outdata[i], .channel_count = outch[i] };
    }
    p->activate(p, 48000, BS, BS); p->start_processing(p);
    for (int k = 0; k < 8 && pt->set[k].addr; k++) lo_send(osc, pt->set[k].addr, "f", pt->set[k].value);

    TestHash h; test_hash_init(&h); float peak = 0; int bad = 0;
    for (int b = 0; b < TOTAL_BLOCKS; b++) {
        EvList ev = { .n = 0 };
        // block 2: chord on; 94 (~0.5 s): chord off; 112: D4 on; 150: F4 on (overlapping); 169: D4 off; 206: F4 off
        if (b == 2) { add_note(&ev, CLAP_EVENT_NOTE_ON, 60, 0.9); add_note(&ev, CLAP_EVENT_NOTE_ON, 64, 0.7); add_note(&ev, CLAP_EVENT_NOTE_ON, 67, 0.5); }
        if (b == 94) { add_note(&ev, CLAP_EVENT_NOTE_OFF, 60, 0); add_note(&ev, CLAP_EVENT_NOTE_OFF, 64, 0); add_note(&ev, CLAP_EVENT_NOTE_OFF, 67, 0); }
        if (b == 112) add_note(&ev, CLAP_EVENT_NOTE_ON, 62, 0.8);
        if (b == 150) add_note(&ev, CLAP_EVENT_NOTE_ON, 65, 0.8);
        if (b == 169) add_note(&ev, CLAP_EVENT_NOTE_OFF, 62, 0);
        if (b == 206) add_note(&ev, CLAP_EVENT_NOTE_OFF, 65, 0);
        clap_input_events_t iev = { &ev, ev_size, ev_get }; clap_output_events_t oev = { NULL, ev_push };
        clap_process_t pr = { .steady_time = (int64_t)b * BS, .frames_count = BS, .audio_inputs = in, .audio_outputs = out,
                              .audio_inputs_count = nin, .audio_outputs_count = nout, .in_events = &iev, .out_events = &oev };
        p->process(p, &pr);
        for (uint32_t i = 0; i < nout; i++) for (uint32_t c = 0; c < outch[i]; c++) {
            for (int s = 0; s < BS; s++) { float v = outdata[i][c][s]; if (!isfinite(v)) bad++; else if (fabsf(v) > peak) peak = fabsf(v); }
            test_hash_bytes(&h, outdata[i][c], BS * sizeof(float));
        }
    }
    p->stop_processing(p); p->deactivate(p);
    test_plugin_unload(&tp);
    for (uint32_t i = 0; i < nin; i++) { free(indata[i][0]); free(indata[i][1]); }
    for (uint32_t i = 0; i < nout; i++) for (uint32_t c = 0; c < outch[i]; c++) free(outdata[i][c]);

    char name[96]; snprintf(name, sizeof name, "clap_patches/%s", pt->name);
    test_check(bad == 0 && peak > 0.01f && peak < 8.0f, name, "%d non-finite samples, peak %g (expected audible and bounded)", bad, peak);
    test_hash_print(&h, name);
    return h.h;
}

// --- State save/load (CLAP state extension) ---
// clap_state_load() runs on the main thread while process() may be running, so it only publishes
// the loaded knobs and zones; the next process() applies them. A save in between must still
// return the loaded state, and the engine must hold it once process() has run.
typedef struct { char buf[1 << 20]; size_t len, pos; } MemStream;
static int64_t mem_write(const clap_ostream_t *s, const void *b, uint64_t n) {
    MemStream *m = s->ctx; if (m->len + n >= sizeof m->buf) return -1;
    memcpy(m->buf + m->len, b, n); m->len += n; m->buf[m->len] = '\0'; return (int64_t)n;
}
static int64_t mem_read(const clap_istream_t *s, void *b, uint64_t n) {
    MemStream *m = s->ctx; size_t k = m->len - m->pos; if (k > n) k = n;
    memcpy(b, m->buf + m->pos, k); m->pos += k; return (int64_t)k;
}
static void state_save(const clap_plugin_state_t *st, const clap_plugin_t *pl, MemStream *m) {
    m->len = 0; m->buf[0] = '\0';
    clap_ostream_t os = { m, mem_write }; st->save(pl, &os);
}
static bool state_load(const clap_plugin_state_t *st, const clap_plugin_t *pl, MemStream *m) {
    m->pos = 0; clap_istream_t is = { m, mem_read }; return st->load(pl, &is);
}
static void process_block(const clap_plugin_t *pl) {
    static float l[BS], r[BS]; float *ch[2] = { l, r };
    clap_audio_buffer_t out; memset(&out, 0, sizeof out); out.data32 = ch; out.channel_count = 2;
    EvList evs = {0};
    clap_input_events_t in = { &evs, ev_size, ev_get };
    clap_output_events_t oe = { NULL, ev_push };
    clap_process_t pr; memset(&pr, 0, sizeof pr);
    pr.frames_count = BS; pr.audio_outputs = &out; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &oe;
    pl->process(pl, &pr);
}
// Replaces the value on `addr`'s K line in a saved state.
static bool set_knob_line(MemStream *m, const char *addr, const char *value) {
    char key[160]; snprintf(key, sizeof key, "\nK %s ", addr);
    char *at = strstr(m->buf, key); if (!at) return false;
    char *val = at + strlen(key), *eol = strchr(val, '\n'); if (!eol) return false;
    size_t tail = m->len - (size_t)(eol - m->buf), vlen = strlen(value);
    memmove(val + vlen, eol, tail + 1); memcpy(val, value, vlen);
    m->len = (size_t)(val - m->buf) + vlen + tail;
    return true;
}
static bool has_knob_line(const MemStream *m, const char *addr, const char *value) {
    char line[200]; snprintf(line, sizeof line, "\nK %s %s\n", addr, value);
    return strstr(m->buf, line) != NULL;
}

static void test_state_load(const char *clap_path) {
    static MemStream m;
    TestPlugin tp;
    if (test_plugin_load(&tp, clap_path) != 0) { test_check(0, "clap_patches/state/load_plugin", "could not load %s", clap_path); return; }
    const clap_plugin_t *pl = tp.plugin;
    const clap_plugin_state_t *st = (const clap_plugin_state_t *)pl->get_extension(pl, CLAP_EXT_STATE);
    const char *addr = "/hds/filter/0/cutoff";

    // Not activated: process() can't run, so a load applies at once.
    state_save(st, pl, &m);
    test_check(set_knob_line(&m, addr, "1234"), "clap_patches/state/has_knob", "no K %s line in a saved state", addr);
    test_check(state_load(st, pl, &m), "clap_patches/state/load_inactive", "state.load failed");
    state_save(st, pl, &m);
    test_check(has_knob_line(&m, addr, "1234"), "clap_patches/state/inactive_applied", "a load while inactive was not applied");

    // Activated: the load waits for process(), but a save before then returns the loaded state.
    pl->activate(pl, 48000, 1, BS); pl->start_processing(pl);
    process_block(pl);
    set_knob_line(&m, addr, "777");
    test_check(state_load(st, pl, &m), "clap_patches/state/load_active", "state.load failed");
    state_save(st, pl, &m);
    test_check(has_knob_line(&m, addr, "777"), "clap_patches/state/save_before_process", "a save right after load did not return the loaded value");
    process_block(pl);
    state_save(st, pl, &m);
    test_check(has_knob_line(&m, addr, "777"), "clap_patches/state/applied_by_process", "the engine does not hold the loaded value after process()");

    // A load process() never takes is applied by deactivate().
    set_knob_line(&m, addr, "555");
    state_load(st, pl, &m);
    pl->stop_processing(pl); pl->deactivate(pl);
    state_save(st, pl, &m);
    test_check(has_knob_line(&m, addr, "555"), "clap_patches/state/applied_by_deactivate", "a load pending at deactivate() was lost");
    test_plugin_unload(&tp);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s hds.clap\n", argv[0]); return 2; }
    lo_address osc = lo_address_new("127.0.0.1", "9010");
    uint64_t hashes[NPATCHES];
    for (int i = 0; i < NPATCHES; i++) hashes[i] = render_patch(argv[1], osc, &PATCHES[i]);
    int same = 0;
    for (int a = 0; a < NPATCHES; a++) for (int b = a + 1; b < NPATCHES; b++)
        if (hashes[a] && hashes[a] == hashes[b]) { same++; printf("     (%s and %s sound identical)\n", PATCHES[a].name, PATCHES[b].name); }
    test_check(same == 0, "clap_patches/all_distinct", "%d pairs of patches rendered identically -- a setting had no effect", same);
    test_state_load(argv[1]);
    lo_address_free(osc);
    return test_exit_code();
}
