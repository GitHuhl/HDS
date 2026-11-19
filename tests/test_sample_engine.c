// test_sample_engine.c -- the DFD sample engine on its own (sample_engine.c + sfz_parser.c only):
// stream sessions, the disk thread's loop splice, and that the audio-thread entry points never
// do file I/O while the sample loader thread is running.
//
// Every check here is a pass/fail check (no golden hashes): the disk and loader threads run in
// real time, so exact sample timing between them is not deterministic -- only the properties are.
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/backend.h"
#include "core/sample_engine.h"
#include "test_common.h"

static HdsSampleSlotState g_slots;
static HdsStreamsState g_streams;
static HdsPendingLoadState g_pending;
static HdsSampleEngine g_se = { &g_slots, &g_streams, &g_pending };
static char g_dir[256];

static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

// Mono 32-bit float WAV at SAMPLE_RATE whose frame i holds value(i).
static void write_wav(const char *path, unsigned int frames, float (*value)(unsigned int)) {
    FILE *f = fopen(path, "wb");
    unsigned int data = frames * 4, riff = 36 + data, fmt_size = 16, rate = SAMPLE_RATE, byte_rate = SAMPLE_RATE * 4;
    unsigned short fmt = 3, ch = 1, align = 4, bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (unsigned int i = 0; i < frames; i++) { float v = value(i); fwrite(&v, 4, 1, f); }
    fclose(f);
}
static float value_half(unsigned int i) { (void)i; return 0.5f; }
static float value_minus_quarter(unsigned int i) { (void)i; return -0.25f; }
static float value_ramp(unsigned int i) { return (float)i / 100000.0f; } // 1e-5 per frame

// One zone covering every note/velocity on VCO1 of the Single program, unpitched (plays at its
// recorded rate), every SFZ-only feature off.
static ZoneRecordWire zone(int zone_id, const char *path) {
    ZoneRecordWire r; memset(&r, 0, sizeof r);
    r.zone_id = zone_id; r.root_note = 60; r.min_note = 0; r.max_note = 127; r.min_velocity = 0; r.max_velocity = 127;
    r.unpitched = 1.0f; r.loop_end = 1.0f; r.end_frac = 1.0f; r.assigned_osc = 0; r.owner_program = (float)SINGLE_PROGRAM_SLOT;
    snprintf(r.path, sizeof r.path, "%s", path);
    r.loop_crossfade_frames = -1; r.hirand = 1.0f; r.trigger_type = 0;
    r.sw_lokey = r.sw_hikey = r.sw_last = -1; r.ampeg_sustain = 100.0f;
    r.xfin_lovel = r.xfin_hivel = r.xfout_lovel = r.xfout_hivel = -1;
    r.xfin_lokey = r.xfin_hikey = r.xfout_lokey = r.xfout_hikey = -1;
    return r;
}

static void remove_all_zones(void) {
    for (int s = 0; s < g_slots.sample_slot_count; s++) if (g_slots.sample_slots[s].active) apply_zone_remove(&g_se, g_slots.sample_slots[s].zone_id);
}

// Upserts `r` and drains until its slot is loaded (or ~2 s pass).
static bool load_zone(const ZoneRecordWire *r) {
    apply_zone_upsert(&g_se, r);
    for (int i = 0; i < 2000; i++) {
        drain_pending_sample_loads(&g_se);
        for (int s = 0; s < g_slots.sample_slot_count; s++)
            if (g_slots.sample_slots[s].active && g_slots.sample_slots[s].zone_id == r->zone_id && g_slots.sample_slots[s].loaded) return true;
        sleep_ms(1);
    }
    return false;
}

typedef struct { int idx; double ptr; bool preroll; } Osc;
static Osc start_note(void) {
    Osc o = { -1, 0.0, true };
    start_sample_osc(&g_se, &o.idx, &o.ptr, &o.preroll, 0, SINGLE_PROGRAM_SLOT, 60, 100, true, NULL, NULL, NULL, NULL, NULL, NULL);
    return o;
}
static float render(Osc *o) { float l = 0, r = 0; render_sample_osc(&g_se, &o->idx, &o->ptr, &o->preroll, 261.63f, &l, &r); return l; }

static bool g_disk_running = false;
static void disk_start(void) {
    atomic_store(&g_streams.sample_disk_thread_shutdown, false);
    g_disk_running = pthread_create(&g_streams.sample_disk_thread, NULL, sample_disk_thread_main, &g_se) == 0;
}
static void disk_stop(void) {
    if (!g_disk_running) return;
    atomic_store(&g_streams.sample_disk_thread_shutdown, true);
    pthread_join(g_streams.sample_disk_thread, NULL);
    g_disk_running = false;
}

// A new stream must never play audio left in its ring by the pool entry's previous session, and
// must never be ended by that session's end-of-file flag: until the disk thread has started the
// new session, the voice holds silent. Reverse zones are the case that reads the ring at once
// (they have no RAM pre-roll), so the disk thread is paused across the note-on to make the
// window deterministic.
static void test_new_session_never_sees_old_ring(void) {
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/a.wav", g_dir); snprintf(b, sizeof b, "%s/b.wav", g_dir);
    write_wav(a, 20000, value_half);
    write_wav(b, 20000, value_minus_quarter);

    ZoneRecordWire za = zone(1, a);
    za.reverse = 1.0f; // straight from the ring, like the note that follows
    test_check(load_zone(&za), "sample_engine/session/load_a", "zone A never loaded");
    Osc o = start_note();
    float va = 0.0f;
    for (int i = 0; i < 2000 && va == 0.0f; i++) { va = render(&o); if (va == 0.0f) sleep_ms(1); }
    test_check(va == 0.5f, "sample_engine/session/a_plays", "zone A rendered %g, expected 0.5", va);
    for (int i = 0; i < 4000; i++) render(&o); // leave plenty of A buffered in the ring
    int pool_idx = o.idx;
    release_sample_stream(&g_se, o.idx);
    disk_stop(); // the disk thread is "busy elsewhere" across the next note-on

    remove_all_zones();
    ZoneRecordWire zb = zone(2, b);
    zb.reverse = 1.0f;
    test_check(load_zone(&zb), "sample_engine/session/load_b", "zone B never loaded");
    Osc ob = start_note();
    test_check(ob.idx == pool_idx, "sample_engine/session/reuses_pool_entry", "expected pool entry %d to be reused, got %d", pool_idx, ob.idx);
    float worst = 0.0f;
    for (int i = 0; i < 256; i++) { float v = render(&ob); if (fabsf(v) > fabsf(worst)) worst = v; }
    test_check(worst == 0.0f, "sample_engine/session/no_stale_audio",
               "a reverse note started before the disk thread caught up played %g (zone A's audio) instead of silence", worst);
    test_check(ob.idx >= 0, "sample_engine/session/not_killed_early", "the new note was released before it ever played");

    disk_start();
    float vb = 0.0f;
    for (int i = 0; i < 2000 && vb == 0.0f; i++) { vb = render(&ob); if (vb == 0.0f) sleep_ms(1); }
    test_check(vb == -0.25f, "sample_engine/session/b_plays", "zone B rendered %g once the disk thread caught up, expected -0.25", vb);
    if (ob.idx >= 0) release_sample_stream(&g_se, ob.idx);
    remove_all_zones();
}

// Same, for a non-looping forward session that reached its end: its end-of-file flag must not
// end the next session on that pool entry.
static void test_old_eof_does_not_end_new_note(void) {
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/short.wav", g_dir); snprintf(b, sizeof b, "%s/b2.wav", g_dir);
    write_wav(a, SAMPLE_PRE_ROLL_LIMIT + 3000, value_half);
    write_wav(b, 20000, value_minus_quarter);
    ZoneRecordWire za = zone(3, a);
    test_check(load_zone(&za), "sample_engine/eof/load_a", "zone never loaded");
    Osc o = start_note();
    int rendered = 0;
    for (int i = 0; i < 200000 && o.idx >= 0; i++) { render(&o); if (++rendered % 256 == 0) sleep_ms(1); }
    test_check(o.idx < 0, "sample_engine/eof/a_ends", "the short zone never reached its end");
    disk_stop();
    remove_all_zones();
    ZoneRecordWire zb = zone(4, b);
    zb.reverse = 1.0f;
    test_check(load_zone(&zb), "sample_engine/eof/load_b", "zone never loaded");
    Osc ob = start_note();
    for (int i = 0; i < 64; i++) render(&ob);
    test_check(ob.idx >= 0, "sample_engine/eof/new_note_survives", "the previous session's end-of-file ended the new note");
    disk_start();
    if (ob.idx >= 0) release_sample_stream(&g_se, ob.idx);
    remove_all_zones();
}

// A looping zone's loop end -> loop start jump is crossfaded: on a slow ramp (1e-5 per frame),
// the splice must not jump by the loop's whole height.
static void test_loop_splice_is_crossfaded(void) {
    char a[300]; snprintf(a, sizeof a, "%s/ramp.wav", g_dir);
    unsigned int frames = 96000;
    write_wav(a, frames, value_ramp);
    ZoneRecordWire z = zone(5, a);
    z.loop_enabled = 1.0f;
    z.loop_start = 20000.0f / (float)(frames - 1);
    z.loop_end = 40000.0f / (float)(frames - 1);
    test_check(load_zone(&z), "sample_engine/loop/load", "zone never loaded");
    Osc o = start_note();
    float prev = render(&o), worst = 0.0f;
    int played = 0;
    for (int i = 0; i < 2000000 && played < 110000; i++) {
        float v = render(&o);
        if (v == 0.0f && prev != 0.0f) { sleep_ms(1); continue; } // the disk thread fell behind: wait, don't count it
        if (played > 0) { float d = fabsf(v - prev); if (d > worst) worst = d; }
        prev = v; played++;
        if (played % 2048 == 0) sleep_ms(1);
    }
    // 20000-frame loop over a 1024-frame crossfade: the steepest crossfaded step is
    // 1e-5 * (1 + 20000/1024) ~= 2.1e-4. A hard splice jumps 0.2.
    test_check(played >= 110000 && worst < 5e-4f, "sample_engine/loop/no_splice_jump",
               "largest step between consecutive samples was %g over %d samples (a hard splice jumps 0.2)", worst, played);
    if (o.idx >= 0) release_sample_stream(&g_se, o.idx);
    remove_all_zones();
}

// Stress: two reverse zones on two oscillators (constant 0.5 and -0.25), retriggered thousands of
// times with the disk thread live, so pool entries are released and reclaimed constantly. Each
// voice must only ever output silence or its own file's value.
static void test_retrigger_stress_never_mixes_files(void) {
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/sa.wav", g_dir); snprintf(b, sizeof b, "%s/sb.wav", g_dir);
    write_wav(a, 30000, value_half);
    write_wav(b, 30000, value_minus_quarter);
    ZoneRecordWire za = zone(7, a), zb = zone(8, b);
    za.reverse = 1.0f; zb.reverse = 1.0f; zb.assigned_osc = 1.0f;
    test_check(load_zone(&za) && load_zone(&zb), "sample_engine/stress/load", "zones never loaded");
    int wrong = 0, heard_a = 0, heard_b = 0;
    float bad = 0.0f;
    for (int round = 0; round < 1200; round++) {
        Osc oa = { -1, 0.0, true }, ob = { -1, 0.0, true };
        start_sample_osc(&g_se, &oa.idx, &oa.ptr, &oa.preroll, 0, SINGLE_PROGRAM_SLOT, 60, 100, true, NULL, NULL, NULL, NULL, NULL, NULL);
        start_sample_osc(&g_se, &ob.idx, &ob.ptr, &ob.preroll, 1, SINGLE_PROGRAM_SLOT, 60, 100, true, NULL, NULL, NULL, NULL, NULL, NULL);
        // Rounds alternate between very short notes (released before the disk thread catches up)
        // and ones held until both have been heard, so both the "not ready yet" and the playing
        // paths get exercised.
        bool hold = (round % 4) == 3;
        int frames = hold ? 48000 : 16 + (round % 7) * 40;
        bool a_on = false, b_on = false;
        for (int i = 0; i < frames && !(hold && a_on && b_on); i++) {
            float va = render(&oa), vb = render(&ob);
            if (va != 0.0f && va != 0.5f) { wrong++; bad = va; }
            if (vb != 0.0f && vb != -0.25f) { wrong++; bad = vb; }
            heard_a += va != 0.0f; heard_b += vb != 0.0f;
            a_on |= va != 0.0f; b_on |= vb != 0.0f;
            if (hold && i % 64 == 63) { struct timespec ts = {0, 100000}; nanosleep(&ts, NULL); }
        }
        // Release in alternating order, so the two zones keep trading pool entries.
        if (round & 1) { release_sample_stream(&g_se, oa.idx); release_sample_stream(&g_se, ob.idx); }
        else { release_sample_stream(&g_se, ob.idx); release_sample_stream(&g_se, oa.idx); }
        if (round % 50 == 0) sleep_ms(1);
    }
    test_check(wrong == 0, "sample_engine/stress/never_mixes_files", "%d samples came from the other zone's file (e.g. %g)", wrong, bad);
    test_check(heard_a > 0 && heard_b > 0, "sample_engine/stress/both_play", "zone A played %d samples, zone B %d", heard_a, heard_b);
    remove_all_zones();
}

// The disk thread keeps files open between notes (DiskFileCache). Many notes of one zone must
// keep streaming it, and a file replaced on disk and then reloaded must stream the new contents,
// not the old file through a handle still open from earlier notes.
static float play_reverse_until_heard(void) {
    Osc o = start_note();
    float v = 0.0f;
    for (int i = 0; i < 2000 && v == 0.0f; i++) { v = render(&o); if (v == 0.0f) sleep_ms(1); }
    if (o.idx >= 0) release_sample_stream(&g_se, o.idx);
    return v;
}
static void test_cached_file_reused_then_reloaded(void) {
    char a[300], tmp[300];
    snprintf(a, sizeof a, "%s/cached.wav", g_dir); snprintf(tmp, sizeof tmp, "%s/cached.tmp", g_dir);
    write_wav(a, 20000, value_half);
    ZoneRecordWire z = zone(9, a);
    z.reverse = 1.0f; // every note streams from disk at once
    test_check(load_zone(&z), "sample_engine/file_cache/load", "zone never loaded");
    int good = 0;
    for (int n = 0; n < 50; n++) good += play_reverse_until_heard() == 0.5f;
    test_check(good == 50, "sample_engine/file_cache/repeat_notes", "%d of 50 repeated notes played the file", good);

    // Replace the file (a new inode, as an editor's save does) and reload the zone.
    write_wav(tmp, 20000, value_minus_quarter);
    rename(tmp, a);
    remove_all_zones();
    test_check(load_zone(&z), "sample_engine/file_cache/reload", "zone never reloaded");
    float v = play_reverse_until_heard();
    test_check(v == -0.25f, "sample_engine/file_cache/reload_reads_new_file", "after the reload a note played %g, expected the new file's -0.25", v);
    remove_all_zones();
}

#ifndef HDS_TEST_BASELINE
// Watchdog: these checks call audio-thread entry points with a zone/SFZ path that is a FIFO with
// no writer -- fopen() on it blocks until someone opens the other end. On the audio thread, that
// would be a hung engine; with the loader thread running, the call must return at once.
static _Atomic int g_watchdog_done = 0;
static const char *g_watchdog_name = "";
static void *watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000 && !g_watchdog_done; i++) sleep_ms(1);
    if (!g_watchdog_done) { printf("FAIL %s: the call blocked on file I/O\n", g_watchdog_name); fflush(stdout); _exit(1); }
    return NULL;
}
static void unblock_fifo(const char *path) { FILE *w = fopen(path, "wb"); if (w) fclose(w); }

static void test_audio_thread_never_opens_files(void) {
    char fifo[300], sfz_fifo[300];
    snprintf(fifo, sizeof fifo, "%s/never_ready.wav", g_dir); snprintf(sfz_fifo, sizeof sfz_fifo, "%s/never_ready.sfz", g_dir);
    unlink(fifo); unlink(sfz_fifo);
    if (mkfifo(fifo, 0600) != 0 || mkfifo(sfz_fifo, 0600) != 0) { test_check(0, "sample_engine/loader/setup", "mkfifo failed"); return; }

    pthread_t wd;
    g_watchdog_done = 0; g_watchdog_name = "sample_engine/loader/zone_load_off_audio_thread";
    pthread_create(&wd, NULL, watchdog, NULL);
    ZoneRecordWire z = zone(6, fifo);
    apply_zone_upsert(&g_se, &z);
    for (int i = 0; i < 10; i++) drain_pending_sample_loads(&g_se);
    g_watchdog_done = 1; pthread_join(wd, NULL);
    test_check(1, "sample_engine/loader/zone_load_off_audio_thread", "");

    g_watchdog_done = 0; g_watchdog_name = "sample_engine/loader/sfz_load_off_audio_thread";
    pthread_create(&wd, NULL, watchdog, NULL);
    hds_load_sfz_for_osc(&g_se, 1, SINGLE_PROGRAM_SLOT, sfz_fifo);
    for (int i = 0; i < 10; i++) drain_pending_sample_loads(&g_se);
    g_watchdog_done = 1; pthread_join(wd, NULL);
    test_check(1, "sample_engine/loader/sfz_load_off_audio_thread", "");

    // Let the loader thread through both opens; neither "file" holds anything playable.
    unblock_fifo(fifo); unblock_fifo(sfz_fifo);
    for (int i = 0; i < 200; i++) { drain_pending_sample_loads(&g_se); sleep_ms(1); }
    bool any_loaded = false;
    for (int s = 0; s < g_slots.sample_slot_count; s++) if (g_slots.sample_slots[s].zone_id == 6 && g_slots.sample_slots[s].loaded) any_loaded = true;
    test_check(!any_loaded, "sample_engine/loader/failed_load_stays_unloaded", "an unreadable file ended up loaded");
    unlink(fifo); unlink(sfz_fifo);
    remove_all_zones();
}

// An SFZ loaded through the loader thread plays the right file.
static void test_sfz_loads_through_loader(void) {
    char w[300], s[300];
    snprintf(w, sizeof w, "%s/sfz_tone.wav", g_dir); snprintf(s, sizeof s, "%s/t.sfz", g_dir);
    write_wav(w, 9000, value_minus_quarter);
    FILE *f = fopen(s, "w"); fprintf(f, "<region> sample=sfz_tone.wav lokey=0 hikey=127 pitch_keycenter=60 pitch_keytrack=0\n"); fclose(f);
    hds_load_sfz_for_osc(&g_se, 0, SINGLE_PROGRAM_SLOT, s);
    bool loaded = false;
    for (int i = 0; i < 2000 && !loaded; i++) {
        drain_pending_sample_loads(&g_se);
        for (int k = 0; k < g_slots.sample_slot_count; k++) if (g_slots.sample_slots[k].active && g_slots.sample_slots[k].loaded) loaded = true;
        if (!loaded) sleep_ms(1);
    }
    test_check(loaded, "sample_engine/loader/sfz_loads", "the SFZ's zone never loaded");
    Osc o = start_note();
    float v = render(&o);
    test_check(v == -0.25f, "sample_engine/loader/sfz_plays", "first sample %g, expected -0.25 from the RAM pre-roll", v);
    if (o.idx >= 0) release_sample_stream(&g_se, o.idx);
    remove_all_zones();
}
#endif

// Usage: test_sample_engine [scratch dir] (default: a fresh directory under /tmp)
int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_dir, sizeof g_dir, "%s/sample_engine", argv[1]);
    else snprintf(g_dir, sizeof g_dir, "/tmp/hds_test_sample_engine_%d", (int)getpid());
    mkdir(g_dir, 0700);
#ifndef HDS_TEST_BASELINE
    test_check(hds_sample_loader_start(&g_se), "sample_engine/loader/start", "the loader thread did not start");
#endif
    disk_start();
    test_new_session_never_sees_old_ring();
    test_old_eof_does_not_end_new_note();
    test_loop_splice_is_crossfaded();
    test_retrigger_stress_never_mixes_files();
    test_cached_file_reused_then_reloaded();
#ifndef HDS_TEST_BASELINE
    test_sfz_loads_through_loader();
    test_audio_thread_never_opens_files();
#endif
    disk_stop();
#ifndef HDS_TEST_BASELINE
    hds_sample_loader_stop(&g_se);
    hds_sample_loader_free(&g_se);
#endif
    return test_exit_code();
}
