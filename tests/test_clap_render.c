// test_clap_render.c -- renders audio through the built plugins, offline, and checks the result.
//   hds.clap:    plays a fixed sequence (single note, three-note chord, single note) on the default
//                patch for 3.5 s, capturing every output port. Checks the output is finite, actually
//                makes sound, and is silent again once every note has released; fingerprints it.
//   hds_fx.clap: feeds deterministic noise into all 8 stereo ports with every slot Off (the
//                default) and checks each output port is a bit-exact copy of its input.
// Usage: test_clap_render <hds.clap> <hds_fx.clap>
#include "test_common.h"
#include "clap_test_host.h"

#include <stdlib.h>

#define BS 256
#define MAX_PORTS 32
#define MAX_EVENTS 16

typedef struct { clap_event_note_t ev[MAX_EVENTS]; int n; } EventList;
static uint32_t ev_size(const clap_input_events_t *l) { return (uint32_t)((const EventList *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((const EventList *)l->ctx)->ev[i].header; }
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }

typedef struct {
    uint32_t n_in, n_out, in_ch[MAX_PORTS], out_ch[MAX_PORTS];
    clap_audio_buffer_t in[MAX_PORTS], out[MAX_PORTS];
    float *in_data[MAX_PORTS][2], *out_data[MAX_PORTS][2];
} Ports;

static void ports_setup(const clap_plugin_t *p, Ports *pt) {
    const clap_plugin_audio_ports_t *ap = (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    memset(pt, 0, sizeof *pt);
    pt->n_in = ap->count(p, true); pt->n_out = ap->count(p, false);
    for (int dir = 0; dir < 2; dir++) {
        uint32_t n = dir ? pt->n_out : pt->n_in;
        for (uint32_t i = 0; i < n; i++) {
            clap_audio_port_info_t info; ap->get(p, i, dir == 0, &info);
            uint32_t ch = info.channel_count > 2 ? 2 : info.channel_count;
            float *(*data)[2] = dir ? pt->out_data : pt->in_data;
            for (uint32_t c = 0; c < ch; c++) data[i][c] = calloc(BS, sizeof(float));
            clap_audio_buffer_t *b = dir ? &pt->out[i] : &pt->in[i];
            *b = (clap_audio_buffer_t){ .data32 = data[i], .channel_count = ch };
            (dir ? pt->out_ch : pt->in_ch)[i] = ch;
        }
    }
}

static void add_note(EventList *l, uint32_t t, uint16_t type, int key, double vel) {
    clap_event_note_t *e = &l->ev[l->n++]; memset(e, 0, sizeof *e);
    e->header = (clap_event_header_t){ sizeof *e, t, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
    e->note_id = -1; e->key = (int16_t)key; e->velocity = vel;
}

static void test_synth(const char *path) {
    TestPlugin tp; if (test_plugin_load(&tp, path)) { g_test_failures++; return; }
    const clap_plugin_t *p = tp.plugin; Ports pt; ports_setup(p, &pt);
    p->activate(p, 48000, BS, BS); p->start_processing(p);
    struct { uint32_t t; uint16_t type; int key; double vel; } sched[] = {
        { 1000, CLAP_EVENT_NOTE_ON, 60, 0.8 }, { 20000, CLAP_EVENT_NOTE_OFF, 60, 0 },
        { 30000, CLAP_EVENT_NOTE_ON, 48, 1.0 }, { 30100, CLAP_EVENT_NOTE_ON, 55, 0.6 }, { 30200, CLAP_EVENT_NOTE_ON, 64, 0.4 },
        { 70000, CLAP_EVENT_NOTE_OFF, 48, 0 }, { 70000, CLAP_EVENT_NOTE_OFF, 55, 0 }, { 90000, CLAP_EVENT_NOTE_OFF, 64, 0 },
        { 100000, CLAP_EVENT_NOTE_ON, 72, 0.9 }, { 120000, CLAP_EVENT_NOTE_OFF, 72, 0 } };
    const uint32_t total = 48000 * 7 / 2, tail_start = total - 48000 / 4; // last note-off is at 2.5 s
    TestHash h; test_hash_init(&h); int bad = 0; float peak = 0, tail_peak = 0;
    for (uint32_t pos = 0; pos < total; pos += BS) {
        EventList ev = { .n = 0 };
        for (size_t k = 0; k < sizeof sched / sizeof sched[0]; k++)
            if (sched[k].t >= pos && sched[k].t < pos + BS) add_note(&ev, sched[k].t - pos, sched[k].type, sched[k].key, sched[k].vel);
        clap_input_events_t in_ev = { &ev, ev_size, ev_get }; clap_output_events_t out_ev = { NULL, ev_push };
        clap_process_t pr = { .steady_time = pos, .frames_count = BS, .audio_inputs = pt.in, .audio_outputs = pt.out,
                              .audio_inputs_count = pt.n_in, .audio_outputs_count = pt.n_out, .in_events = &in_ev, .out_events = &out_ev };
        p->process(p, &pr);
        for (uint32_t i = 0; i < pt.n_out; i++) for (uint32_t c = 0; c < pt.out_ch[i]; c++) {
            const float *x = pt.out_data[i][c];
            for (int s = 0; s < BS; s++) {
                if (!isfinite(x[s])) bad++;
                float a = fabsf(x[s]); if (a > peak) peak = a; if (pos >= tail_start && a > tail_peak) tail_peak = a;
            }
            test_hash_bytes(&h, x, BS * sizeof(float));
        }
    }
    p->stop_processing(p); p->deactivate(p);
    test_check(bad == 0, "clap_render/hds/finite", "%d non-finite samples", bad);
    test_check(peak > 0.05f && peak < 4.0f, "clap_render/hds/level", "peak %g outside 0.05..4", peak);
    test_check(tail_peak < 1e-6f, "clap_render/hds/release", "still sounding 750 ms after the last note-off (peak %g)", tail_peak);
    test_hash_print(&h, "clap_render/hds/notes");
    test_plugin_unload(&tp);
}

static void test_fx_passthrough(const char *path) {
    TestPlugin tp; if (test_plugin_load(&tp, path)) { g_test_failures++; return; }
    const clap_plugin_t *p = tp.plugin; Ports pt; ports_setup(p, &pt);
    test_check(pt.n_in == 8 && pt.n_out == 8, "clap_render/hds_fx/ports", "%u in / %u out ports, expected 8/8", pt.n_in, pt.n_out);
    p->activate(p, 48000, BS, BS); p->start_processing(p);
    EventList ev = { .n = 0 };
    clap_input_events_t in_ev = { &ev, ev_size, ev_get }; clap_output_events_t out_ev = { NULL, ev_push };
    test_rng_seed(99); int mismatched = 0;
    for (uint32_t pos = 0; pos < 48000; pos += BS) {
        for (uint32_t i = 0; i < pt.n_in; i++) for (uint32_t c = 0; c < pt.in_ch[i]; c++)
            for (int s = 0; s < BS; s++) pt.in_data[i][c][s] = test_frand() * 2 - 1;
        clap_process_t pr = { .steady_time = pos, .frames_count = BS, .audio_inputs = pt.in, .audio_outputs = pt.out,
                              .audio_inputs_count = pt.n_in, .audio_outputs_count = pt.n_out, .in_events = &in_ev, .out_events = &out_ev };
        p->process(p, &pr);
        for (uint32_t i = 0; i < pt.n_out && i < pt.n_in; i++) for (uint32_t c = 0; c < pt.out_ch[i]; c++)
            if (memcmp(pt.out_data[i][c], pt.in_data[i][c], BS * sizeof(float))) mismatched++;
    }
    p->stop_processing(p); p->deactivate(p);
    test_check(mismatched == 0, "clap_render/hds_fx/passthrough", "%d port-channel blocks differ from their input", mismatched);
    test_plugin_unload(&tp);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s hds.clap hds_fx.clap\n", argv[0]); return 2; }
    test_synth(argv[1]);
    test_fx_passthrough(argv[2]);
    return test_exit_code();
}
