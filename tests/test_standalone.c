// test_standalone.c -- runs the real synth_engine headless and drives it over OSC, covering what
// only the standalone app does: its own audio loop, the Sequencer transport, and recording to disk.
//
// synth_engine is started with --no-midi and ALSA's "null" PCM for both directions (no audio or
// MIDI hardware needed) and its own OSC ports. Checks:
//   - it starts and answers /hds/ping
//   - the audio loop runs: status pushes keep arriving
//   - /hds/request_state returns one message per control address
//   - an OSC parameter write shows up in the next state dump
//   - end to end: import a generated MIDI file, start recording to a WAV, press Play on the
//     Sequencer, stop; the WAV is a valid, finalized file and contains the notes (not silence)
//   - SIGINT shuts it down cleanly (exit status 0) within a few seconds
// The null device doesn't pace the loop, so audio renders faster than real time; that's fine
// for these checks, and nothing here is fingerprinted (timing between OSC messages and rendered
// blocks isn't deterministic).
// Usage: test_standalone <scratch dir> <synth_engine>
#include "test_common.h"

#include <fcntl.h>
#include <lo/lo.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_PORT "9100"
#define MAX_MSGS 8192

typedef struct { char path[96]; char types[8]; float f; char s[256]; } Msg;
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

static lo_server g_srv;
static lo_address g_app;
static pid_t g_pid = -1;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
// Receives for up to `ms`, returning early once a message with path `until` (if given) has arrived.
static void collect(int ms, const char *until) {
    double end = now() + ms / 1000.0;
    while (now() < end) {
        lo_server_recv_noblock(g_srv, 20);
        if (until && find_msg(until)) return;
    }
}
static void send_f(const char *path, float v) { lo_send_from(g_app, g_srv, LO_TT_IMMEDIATE, path, "f", v); }
static void send_s(const char *path, const char *v) { lo_send_from(g_app, g_srv, LO_TT_IMMEDIATE, path, "s", v); }

static int request_state(void) {
    g_nmsgs = 0;
    lo_send_from(g_app, g_srv, LO_TT_IMMEDIATE, "/hds/request_state", "");
    collect(5000, "/hds/status/state_dump_complete");
    int n = 0;
    for (int i = 0; i < g_nmsgs; i++) if (strncmp(g_msgs[i].path, "/hds/status/", 12)) n++;
    return n;
}
static const Msg *state_value(const char *path) {
    for (int i = 0; i < g_nmsgs; i++) if (!strcmp(g_msgs[i].path, path)) return &g_msgs[i];
    return NULL;
}

// --- a small type-1 MIDI file: two note tracks, 120 BPM ---
typedef struct { unsigned char b[512]; int n; } Buf;
static void put(Buf *x, const void *d, int n) { memcpy(x->b + x->n, d, (size_t)n); x->n += n; }
static void put_vlq(Buf *x, uint32_t v) { unsigned char t[4]; int n = 0; t[n++] = v & 0x7f; while (v >>= 7) t[n++] = 0x80 | (v & 0x7f); while (n) put(x, &t[--n], 1); }
static void ev3(Buf *x, uint32_t delta, int a, int b, int c) { put_vlq(x, delta); unsigned char e[3] = { (unsigned char)a, (unsigned char)b, (unsigned char)c }; put(x, e, 3); }
static void put_track(FILE *f, const Buf *t) { unsigned char h[8] = { 'M','T','r','k', (unsigned char)(t->n >> 24), (unsigned char)(t->n >> 16), (unsigned char)(t->n >> 8), (unsigned char)t->n }; fwrite(h, 1, 8, f); fwrite(t->b, 1, (size_t)t->n, f); }
static void write_smf(const char *path) {
    Buf t0 = {0}, t1 = {0}, t2 = {0};
    unsigned char tempo[] = { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 }, eot[] = { 0xFF, 0x2F, 0x00 };
    put_vlq(&t0, 0); put(&t0, tempo, 6); put_vlq(&t0, 0); put(&t0, eot, 3);
    ev3(&t1, 0, 0x90, 60, 110); ev3(&t1, 480, 0x80, 60, 0); ev3(&t1, 0, 0x90, 67, 100); ev3(&t1, 480, 0x80, 67, 0); put_vlq(&t1, 0); put(&t1, eot, 3);
    ev3(&t2, 0, 0x91, 48, 110); ev3(&t2, 960, 0x81, 48, 0); put_vlq(&t2, 0); put(&t2, eot, 3);
    FILE *f = fopen(path, "wb");
    unsigned char hdr[14] = { 'M','T','h','d', 0,0,0,6, 0,1, 0,3, 0x01,0xE0 };
    fwrite(hdr, 1, 14, f); put_track(f, &t0); put_track(f, &t1); put_track(f, &t2); fclose(f);
}

// --- recorded WAV: valid 32-bit float RIFF with a finalized data size; returns its peak ---
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int check_wav(const char *path, uint32_t *frames, float *peak) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)size); if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return 0; }
    fclose(f);
    int ok = size > 44 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4) && rd32(buf + 4) == (uint32_t)(size - 8);
    long pos = 12; int channels = 0, fmt_ok = 0; const unsigned char *data = NULL; uint32_t data_len = 0;
    while (ok && pos + 8 <= size) {
        uint32_t len = rd32(buf + pos + 4);
        if (!memcmp(buf + pos, "fmt ", 4)) { fmt_ok = rd16(buf + pos + 8) == 3 && rd16(buf + pos + 22) == 32; channels = rd16(buf + pos + 10); }
        if (!memcmp(buf + pos, "data", 4)) { data = buf + pos + 8; data_len = len; }
        pos += 8 + len + (len & 1);
    }
    ok = ok && fmt_ok && channels > 0 && data && (long)(data - buf) + (long)data_len <= size;
    *frames = ok ? data_len / (4u * (uint32_t)channels) : 0; *peak = 0;
    for (uint32_t i = 0; ok && i < data_len / 4; i++) { float v; memcpy(&v, data + 4 * i, 4); if (fabsf(v) > *peak) *peak = fabsf(v); }
    free(buf);
    return ok;
}

static int stop_app(int timeout_ms) {
    if (g_pid <= 0) return -1;
    kill(g_pid, SIGINT);
    for (int t = 0; t < timeout_ms; t += 50) {
        int st; pid_t r = waitpid(g_pid, &st, WNOHANG);
        if (r == g_pid) { g_pid = -1; return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }
        struct timespec d = {0, 50000000}; nanosleep(&d, NULL);
    }
    kill(g_pid, SIGKILL); waitpid(g_pid, NULL, 0); g_pid = -1;
    return -2;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s scratchdir synth_engine\n", argv[0]); return 2; }
    const char *dir = argv[1];
    g_srv = lo_server_new(NULL, NULL);
    lo_server_add_method(g_srv, NULL, NULL, on_any, NULL);
    char reply_port[16]; snprintf(reply_port, sizeof reply_port, "%d", lo_server_get_port(g_srv));
    g_app = lo_address_new("127.0.0.1", LISTEN_PORT);

    char logpath[512]; snprintf(logpath, sizeof logpath, "%s/standalone.log", dir);
    g_pid = fork();
    if (g_pid == 0) {
        int in = open("/dev/null", O_RDONLY), log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(in, 0); dup2(log, 1); dup2(log, 2);
        execl(argv[2], argv[2], "--no-midi", "--backend", "alsa", "--output-device", "null", "--input-device", "null",
              "--osc-listen-port", LISTEN_PORT, "--osc-reply-port", reply_port, (char *)NULL);
        _exit(127);
    }

    // startup: ping until it answers
    const Msg *pong = NULL;
    for (int i = 0; i < 50 && !pong; i++) { g_nmsgs = 0; lo_send_from(g_app, g_srv, LO_TT_IMMEDIATE, "/hds/ping", ""); collect(100, "/hds/pong"); pong = find_msg("/hds/pong"); }
    test_check(pong != NULL, "standalone/startup", "no /hds/pong within 5 s (see %s; is UDP port %s taken?)", logpath, LISTEN_PORT);
    if (!pong) { stop_app(2000); return test_exit_code(); }

    // audio loop: the main loop pushes a status bundle every few blocks, so they keep arriving
    g_nmsgs = 0; collect(1000, NULL);
    int pushes = 0;
    for (int i = 0; i < g_nmsgs; i++) if (!strcmp(g_msgs[i].path, "/hds/status/peak")) pushes++;
    test_check(pushes >= 5, "standalone/audio_loop", "only %d status pushes in 1 s -- the audio loop isn't running", pushes);

    // full state dump
    int n = request_state();
    test_check(find_msg("/hds/status/state_dump_complete") && n == 5117, "standalone/state_dump", "%d control messages, expected 5117", n);

    // parameter write
    send_f("/hds/master_volume", 0.5f); collect(200, NULL);
    request_state();
    const Msg *mv = state_value("/hds/master_volume");
    test_check(mv && mv->f == 0.5f, "standalone/params/echo", "master_volume reads %g after setting 0.5", mv ? mv->f : -1);

    // end to end: import MIDI -> record -> play -> stop
    char mid[512], wav[512];
    snprintf(mid, sizeof mid, "%s/standalone_song.mid", dir); snprintf(wav, sizeof wav, "%s/standalone_take.wav", dir);
    write_smf(mid); remove(wav);
    g_nmsgs = 0; lo_send_from(g_app, g_srv, LO_TT_IMMEDIATE, "/hds/song/import_smf", "sf", mid, 1.0f);
    collect(2000, "/hds/status/smf_import/lanes_used");
    const Msg *ok = find_msg("/hds/status/smf_import/ok"), *lanes = find_msg("/hds/status/smf_import/lanes_used");
    test_check(ok && ok->f == 1.0f && lanes && lanes->f == 2.0f, "standalone/smf_import", "import reply ok=%g lanes=%g", ok ? ok->f : -1, lanes ? lanes->f : -1);

    send_s("/hds/record/path", wav);
    send_f("/hds/record/format", 0.0f);
    send_f("/hds/record/include_synth_output", 1.0f);
    send_f("/hds/record/enable", 1.0f);
    g_nmsgs = 0; collect(1000, NULL);
    const Msg *rec = find_msg("/hds/status/is_recording"), *el = find_msg("/hds/status/elapsed_seconds"); // elapsed = length of the take so far
    test_check(rec && rec->f == 1.0f && el && el->f > 0.0f, "standalone/record/start", "is_recording %g, take length %g s after /hds/record/enable 1",
               rec ? rec->f : -1, el ? el->f : -1);
    send_f("/hds/seq/transport/play", 1.0f);
    collect(1500, NULL);
    send_f("/hds/seq/transport/stop", 1.0f);
    send_f("/hds/record/enable", 0.0f);
    for (int i = 0; i < 40; i++) { g_nmsgs = 0; collect(100, "/hds/status/is_recording"); const Msg *r = find_msg("/hds/status/is_recording"); if (r && r->f == 0.0f) break; }
    uint32_t frames = 0; float peak = 0;
    int valid = check_wav(wav, &frames, &peak);
    test_check(valid && frames > 48000, "standalone/record/wav", "recording %s: valid=%d, %u frames (expected a finalized float WAV over 1 s)", wav, valid, frames);
    test_check(peak > 0.01f, "standalone/record/has_audio", "recorded WAV is silent (peak %g): the Sequencer didn't play the imported notes", peak);

    // clean shutdown
    int st = stop_app(5000);
    test_check(st == 0, "standalone/shutdown", "SIGINT gave exit status %d (expected 0; -2 = still running after 5 s)", st);

    lo_address_free(g_app); lo_server_free(g_srv);
    return test_exit_code();
}
