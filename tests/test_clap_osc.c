// test_clap_osc.c -- drives the built hds.clap over its real OSC control protocol, the way a remote
// controller would, from an in-process CLAP host. The plugin drains its OSC socket at the start of
// every process() call, so each step is: send, run one process() block, read the replies.
//   - /hds/ping -> /hds/pong with the instance name
//   - /hds/request_state -> one message per control address, then state_dump_complete; the whole
//     dump is fingerprinted (default patch), and must cover every address exactly once
//   - parameter writes over OSC show up in the next dump, and change the rendered sound
//   - /hds/zone/load_sfz: a generated sine-wave WAV + one-region .sfz loads and plays
//   - /hds/song/import_smf: a generated 3-track type-1 MIDI file (two tempos, one marker) imports
//     with the expected lanes/tempo/marker report
// Usage: test_clap_osc <scratch dir> <hds.clap>
#include "test_common.h"
#include "clap_test_host.h"

#include <lo/lo.h>
#include <stdlib.h>
#include <sys/socket.h>

#define BS 256
#define MAX_MSGS 8192

// ---- received-message log ---------------------------------------------------------------------
typedef struct { char path[96]; char types[8]; float f; char s[128]; } Msg;
static Msg g_msgs[MAX_MSGS];
static int g_nmsgs = 0;
static int on_any(const char *path, const char *types, lo_arg **argv, int argc, lo_message m, void *ud) {
    (void)m; (void)ud;
    if (g_nmsgs >= MAX_MSGS) return 0;
    Msg *x = &g_msgs[g_nmsgs++]; memset(x, 0, sizeof *x);
    snprintf(x->path, sizeof x->path, "%s", path); snprintf(x->types, sizeof x->types, "%s", types);
    if (argc > 0 && types[0] == 'f') x->f = argv[0]->f;
    if (argc > 0 && types[0] == 's') snprintf(x->s, sizeof x->s, "%s", &argv[0]->s);
    return 0;
}
static const Msg *find_msg(const char *path) { for (int i = g_nmsgs - 1; i >= 0; i--) if (!strcmp(g_msgs[i].path, path)) return &g_msgs[i]; return NULL; }

// ---- plugin + audio ---------------------------------------------------------------------------
static TestPlugin g_tp;
static lo_server g_srv;
static lo_address g_plugin_addr;
static clap_audio_buffer_t g_out[32], g_in[32];
static uint32_t g_nout, g_nin, g_outch[32];
static float *g_outdata[32][2], *g_indata[32][2];
static uint64_t g_steady = 0;

typedef struct { clap_event_note_t ev[8]; int n; } EvList;
static uint32_t ev_size(const clap_input_events_t *l) { return (uint32_t)((const EvList *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((const EvList *)l->ctx)->ev[i].header; }
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }

// Runs one block; `key` >= 0 adds a note on (vel > 0) or off (vel == 0) at frame 0. Hashes output into h if given.
static float process_block(int key, double vel, TestHash *h) {
    EvList ev = { .n = 0 };
    if (key >= 0) {
        clap_event_note_t *e = &ev.ev[ev.n++]; memset(e, 0, sizeof *e);
        e->header = (clap_event_header_t){ sizeof *e, 0, CLAP_CORE_EVENT_SPACE_ID, vel > 0 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, 0 };
        e->note_id = -1; e->key = (int16_t)key; e->velocity = vel;
    }
    clap_input_events_t in_ev = { &ev, ev_size, ev_get }; clap_output_events_t out_ev = { NULL, ev_push };
    clap_process_t pr = { .steady_time = (int64_t)g_steady, .frames_count = BS, .audio_inputs = g_in, .audio_outputs = g_out,
                          .audio_inputs_count = g_nin, .audio_outputs_count = g_nout, .in_events = &in_ev, .out_events = &out_ev };
    g_tp.plugin->process(g_tp.plugin, &pr);
    g_steady += BS;
    float peak = 0;
    for (uint32_t i = 0; i < g_nout; i++) for (uint32_t c = 0; c < g_outch[i]; c++) {
        for (int s = 0; s < BS; s++) { float a = fabsf(g_outdata[i][c][s]); if (!(a <= 1e6f)) a = 1e6f; if (a > peak) peak = a; }
        if (h) test_hash_bytes(h, g_outdata[i][c], BS * sizeof(float));
    }
    return peak;
}

// Lets replies arrive: alternates draining our socket with short waits, up to `ms`, stopping early
// once a message with path `until` (if given) has been seen.
static void collect(int ms, const char *until) {
    for (int t = 0; t < ms; t += 5) {
        while (lo_server_recv_noblock(g_srv, 0) > 0) {}
        if (until && find_msg(until)) return;
        lo_server_recv_noblock(g_srv, 5);
    }
}

// Plays C4 for 0.25 s then releases for 0.25 s, fingerprinting everything; returns the peak.
static float play_phrase(TestHash *h) {
    float peak = 0;
    for (int b = 0; b < 94; b++) {
        float p = process_block(b == 0 ? 60 : (b == 47 ? 60 : -1), b == 0 ? 0.9 : 0.0, h);
        if (p > peak) peak = p;
    }
    return peak;
}

// ---- state dump -------------------------------------------------------------------------------
static int request_state(TestHash *h) {
    g_nmsgs = 0;
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/request_state", "");
    process_block(-1, 0, NULL);
    collect(3000, "/hds/status/state_dump_complete");
    int n = 0;
    for (int i = 0; i < g_nmsgs; i++) {
        if (strncmp(g_msgs[i].path, "/hds/status/", 12) == 0) continue; // unprompted status pushes / completion marker
        n++;
        if (h) { test_hash_bytes(h, g_msgs[i].path, strlen(g_msgs[i].path) + 1); test_hash_bytes(h, g_msgs[i].types, strlen(g_msgs[i].types) + 1);
                 test_hash_bytes(h, &g_msgs[i].f, sizeof(float)); test_hash_bytes(h, g_msgs[i].s, strlen(g_msgs[i].s) + 1); }
    }
    return n;
}
static const Msg *state_value(const char *path) {
    for (int i = 0; i < g_nmsgs; i++) if (!strcmp(g_msgs[i].path, path)) return &g_msgs[i];
    return NULL;
}

// ---- generated input files --------------------------------------------------------------------
static void write_sine_wav(const char *path, int frames) {
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)frames * 2;
    unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                            0x80,0xBB,0,0, 0,0x77,1,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t riff = 36 + data; memcpy(h + 4, &riff, 4); memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < frames; i++) { int16_t s = (int16_t)(12000 * sin(2 * M_PI * 440.0 * i / 48000.0)); fwrite(&s, 2, 1, f); }
    fclose(f);
}
typedef struct { unsigned char b[512]; int n; } Buf;
static void put(Buf *x, const void *d, int n) { memcpy(x->b + x->n, d, (size_t)n); x->n += n; }
static void put_vlq(Buf *x, uint32_t v) { unsigned char t[4]; int n = 0; t[n++] = v & 0x7f; while (v >>= 7) t[n++] = 0x80 | (v & 0x7f); while (n) put(x, &t[--n], 1); }
static void put_track(FILE *f, const Buf *t) { unsigned char h[8] = { 'M','T','r','k', (unsigned char)(t->n >> 24), (unsigned char)(t->n >> 16), (unsigned char)(t->n >> 8), (unsigned char)t->n }; fwrite(h, 1, 8, f); fwrite(t->b, 1, (size_t)t->n, f); }
static void ev3(Buf *x, uint32_t delta, int a, int b, int c) { put_vlq(x, delta); unsigned char e[3] = { (unsigned char)a, (unsigned char)b, (unsigned char)c }; put(x, e, 3); }
static void write_smf(const char *path) {
    Buf t0 = {0}, t1 = {0}, t2 = {0};
    unsigned char tempo120[] = { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 }, tempo100[] = { 0xFF, 0x51, 0x03, 0x09, 0x27, 0xC0 };
    unsigned char marker[] = { 0xFF, 0x06, 0x05, 'V','e','r','s','e' }, eot[] = { 0xFF, 0x2F, 0x00 };
    put_vlq(&t0, 0); put(&t0, tempo120, 6); put_vlq(&t0, 0); put(&t0, marker, 8); put_vlq(&t0, 1920); put(&t0, tempo100, 6); put_vlq(&t0, 0); put(&t0, eot, 3);
    ev3(&t1, 0, 0x90, 60, 100); ev3(&t1, 480, 0x80, 60, 0); ev3(&t1, 0, 0x90, 64, 90); ev3(&t1, 480, 0x80, 64, 0); put_vlq(&t1, 0); put(&t1, eot, 3);
    ev3(&t2, 0, 0x91, 48, 110); ev3(&t2, 1920, 0x81, 48, 0); put_vlq(&t2, 0); put(&t2, eot, 3);
    FILE *f = fopen(path, "wb");
    unsigned char hdr[14] = { 'M','T','h','d', 0,0,0,6, 0,1, 0,3, 0x01,0xE0 }; // format 1, 3 tracks, 480 ticks/quarter
    fwrite(hdr, 1, 14, f); put_track(f, &t0); put_track(f, &t1); put_track(f, &t2); fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s scratchdir hds.clap\n", argv[0]); return 2; }
    const char *dir = argv[1];
    if (test_plugin_load(&g_tp, argv[2])) return 1;
    const clap_plugin_t *p = g_tp.plugin;
    const clap_plugin_audio_ports_t *ap = (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    g_nin = ap->count(p, true); g_nout = ap->count(p, false);
    for (uint32_t i = 0; i < g_nin; i++) { g_indata[i][0] = calloc(BS, 4); g_indata[i][1] = calloc(BS, 4); g_in[i] = (clap_audio_buffer_t){ .data32 = g_indata[i], .channel_count = 2 }; }
    for (uint32_t i = 0; i < g_nout; i++) {
        clap_audio_port_info_t info; ap->get(p, i, false, &info);
        g_outch[i] = info.channel_count > 2 ? 2 : info.channel_count;
        for (uint32_t c = 0; c < g_outch[i]; c++) g_outdata[i][c] = calloc(BS, 4);
        g_out[i] = (clap_audio_buffer_t){ .data32 = g_outdata[i], .channel_count = g_outch[i] };
    }
    p->activate(p, 48000, BS, BS); p->start_processing(p);

    g_srv = lo_server_new(NULL, NULL);
    int big = 8 << 20; setsockopt(lo_server_get_socket_fd(g_srv), SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof big);
    setsockopt(lo_server_get_socket_fd(g_srv), SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    lo_server_add_method(g_srv, NULL, NULL, on_any, NULL);
    g_plugin_addr = lo_address_new("127.0.0.1", "9010");

    // ping
    g_nmsgs = 0; lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/ping", "");
    process_block(-1, 0, NULL); collect(1000, "/hds/pong");
    const Msg *pong = find_msg("/hds/pong");
    test_check(pong && pong->types[0] == 's', "clap_osc/ping", "no /hds/pong reply (is UDP port 9010 already taken?)");
    if (!pong) { p->stop_processing(p); p->deactivate(p); test_plugin_unload(&g_tp); return test_exit_code(); }

    // full state dump, default patch
    TestHash hs; test_hash_init(&hs);
    int n = request_state(&hs);
    test_check(find_msg("/hds/status/state_dump_complete") != NULL, "clap_osc/state_dump/complete", "no state_dump_complete (got %d messages)", g_nmsgs);
    test_check(n == 5117, "clap_osc/state_dump/count", "%d control messages, expected one per OSC address (5117)", n);
    test_hash_print(&hs, "clap_osc/state_dump/default");

    // parameter writes
    TestHash ha; test_hash_init(&ha); play_phrase(&ha);
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/vco2/shape", "f", 1.0f);
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/vco2/vol", "f", 60.0f);
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/master_volume", "f", 0.5f);
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/tuning/reference_freq", "f", 432.0f);
    process_block(-1, 0, NULL);
    request_state(NULL);
    const Msg *s1 = state_value("/hds/vco2/shape"), *s2 = state_value("/hds/vco2/vol"), *s3 = state_value("/hds/master_volume"), *s4 = state_value("/hds/tuning/reference_freq");
    test_check(s1 && s1->f == 1.0f && s2 && s2->f == 60.0f && s3 && s3->f == 0.5f && s4 && s4->f == 432.0f, "clap_osc/params/echo",
               "written values not reflected in the state dump (%g %g %g %g)", s1 ? s1->f : -1, s2 ? s2->f : -1, s3 ? s3->f : -1, s4 ? s4->f : -1);
    TestHash hb; test_hash_init(&hb); float pk = play_phrase(&hb);
    test_check(pk > 0.01f && hb.h != ha.h, "clap_osc/params/audible", "changing parameters over OSC didn't change the sound (peak %g)", pk);
    test_hash_print(&hb, "clap_osc/params/audio");

    // SFZ
    char wav[512], sfz[512];
    snprintf(wav, sizeof wav, "%s/osc_test_tone.wav", dir); snprintf(sfz, sizeof sfz, "%s/osc_test.sfz", dir);
    write_sine_wav(wav, 4800); // 0.1 s: fits in the RAM pre-roll, so playback never waits on the disk thread
    FILE *f = fopen(sfz, "w"); fprintf(f, "<region> sample=osc_test_tone.wav lokey=0 hikey=127 pitch_keycenter=69\n"); fclose(f);
    // VCO1 -> Sample (WAVE_SAMPLE = 7), VCO2 silent: with no zones loaded yet, a note must be silent,
    // so any sound after load_sfz can only come from the loaded sample.
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/vco2/vol", "f", 0.0f);
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/vco1/shape", "f", 7.0f);
    for (int b = 0; b < 4; b++) process_block(-1, 0, NULL);
    pk = play_phrase(NULL);
    test_check(pk < 1e-6f, "clap_osc/sfz/silent_before_load", "VCO1 in Sample mode with no zones made sound (peak %g)", pk);
    // owner_program 8 = SINGLE_PROGRAM_SLOT, the program Single mode plays (0..7 are the Combi programs)
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/zone/load_sfz", "iis", 0, 8, sfz);
    for (int b = 0; b < 20; b++) process_block(-1, 0, NULL);
    TestHash hz; test_hash_init(&hz); pk = play_phrase(&hz);
    test_check(pk > 0.01f, "clap_osc/sfz/plays", "no sound from the loaded SFZ (peak %g)", pk);
    test_hash_print(&hz, "clap_osc/sfz/audio");

    // SMF import
    char mid[512]; snprintf(mid, sizeof mid, "%s/osc_test.mid", dir); write_smf(mid);
    g_nmsgs = 0;
    lo_send_from(g_plugin_addr, g_srv, LO_TT_IMMEDIATE, "/hds/song/import_smf", "sf", mid, 1.0f);
    process_block(-1, 0, NULL); collect(2000, "/hds/status/smf_import/marker_count");
    collect(200, NULL);
    const Msg *ok = find_msg("/hds/status/smf_import/ok"), *lanes = find_msg("/hds/status/smf_import/lanes_used"),
              *tm = find_msg("/hds/status/smf_import/tempo_marker_count"),
              *t0 = find_msg("/hds/status/smf_import/tempo_marker/0/time"), *b0 = find_msg("/hds/status/smf_import/tempo_marker/0/bpm"),
              *t1 = find_msg("/hds/status/smf_import/tempo_marker/1/time"), *b1 = find_msg("/hds/status/smf_import/tempo_marker/1/bpm"),
              *mc = find_msg("/hds/status/smf_import/marker_count"), *ml = find_msg("/hds/status/smf_import/marker/0/label"),
              *err = find_msg("/hds/status/smf_import/error");
    test_check(ok && ok->f == 1.0f, "clap_osc/smf/ok", "import failed: %s", err ? err->s : "(no reply)");
    test_check(lanes && lanes->f == 2.0f, "clap_osc/smf/lanes", "lanes_used %g, expected 2 (two note tracks)", lanes ? lanes->f : -1);
    // Two tempo events -> a tempo map: 120 BPM at 0 s, then 100 BPM at tick 1920 = 4 beats at 120 = 2.0 s
    test_check(tm && tm->f == 2.0f && t0 && b0 && t1 && b1 && t0->f == 0.0f && fabsf(b0->f - 120.0f) < 1e-3f &&
               fabsf(t1->f - 2.0f) < 1e-4f && fabsf(b1->f - 100.0f) < 1e-3f, "clap_osc/smf/tempo_map",
               "tempo map wrong: %g markers, (%g s, %g BPM), (%g s, %g BPM)", tm ? tm->f : -1, t0 ? t0->f : -1, b0 ? b0->f : -1, t1 ? t1->f : -1, b1 ? b1->f : -1);
    test_check(mc && mc->f == 1.0f && ml && strcmp(ml->s, "Verse") == 0, "clap_osc/smf/marker", "marker report wrong");
    TestHash hm; test_hash_init(&hm);
    for (int i = 0; i < g_nmsgs; i++) {
        if (strncmp(g_msgs[i].path, "/hds/status/smf_import/", 23)) continue;
        test_hash_bytes(&hm, g_msgs[i].path, strlen(g_msgs[i].path) + 1); test_hash_bytes(&hm, &g_msgs[i].f, sizeof(float));
        test_hash_bytes(&hm, g_msgs[i].s, strlen(g_msgs[i].s) + 1);
    }
    test_hash_print(&hm, "clap_osc/smf/report");

    p->stop_processing(p); p->deactivate(p);
    lo_address_free(g_plugin_addr); lo_server_free(g_srv);
    test_plugin_unload(&g_tp);
    return test_exit_code();
}
