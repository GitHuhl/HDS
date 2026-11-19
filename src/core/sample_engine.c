// sample_engine.c -- the Sample Bank / DFD (direct-from-disk) sample engine: loading sample files
// into slots (RAM pre-roll + metadata), the stream pool and its disk thread, the zone OSC
// protocol (/hds/zone/*), turning parsed SFZ regions into zones (hds_load_sfz_for_osc), zone
// matching (keyswitches, round-robin, crossfades, velocity curves, choke groups), and the
// per-voice sample oscillator: starting, rendering and releasing streams. Moved verbatim out of
// backend.c; see sample_engine.h for what the voice code in backend.c calls.
#include "core/backend.h"
#include "core/sfz_parser.h"

// --- DFD (direct-from-disk) sample playback: WAVE_SAMPLE oscillator source ---
//
// A loaded .wav's audio data is far too large to keep resident in RAM (a multi-second sample at
// 48kHz/stereo/float is tens of MB), so instead: the first SAMPLE_PRE_ROLL_LIMIT frames are
// decoded into RAM once at load time (a few hundred KB, giving zero-latency note-on), and
// everything after that streams from disk in real time on a dedicated thread into a lock-free
// ring buffer -- the SAME architecture, and the same realtime-safety rationale, as the WAV
// recorder above: fopen/fseek/fread are all blocking-time-unbounded, and this engine's main loop
// paces live audio output via blocking ALSA writes that cannot tolerate a stall. The audio
// thread here only ever does bounded, allocation-free work: a wait-free ring read plus linear
// interpolation (render_sample_osc), exactly mirroring record_ring_push/record_ring_pop's role
// in the recording engine. Ownership follows the identical convention: the disk thread is the
// ONLY thing that ever opens/reads/closes a FILE* or writes into a ring buffer; the audio thread
// only ever reads a ring buffer and publishes tiny atomic start/stop commands.
//
// Sample playback itself is adapted to HDS's own mature EG/filter/LFO/mod-matrix/FX-bus pipeline
// (see render_sample_osc's comment) rather than bringing in a parallel one. Zone selection --
// which loaded slot a given (note, velocity) actually plays -- is the real per-note/velocity
// mapping system (SFZ-style): see find_matching_sample_slot(), populated via the /hds/zone/* OSC
// messages or by loading an .sfz file directly.

// HDS_SFZ_TRIGGER_*: SampleSlot.trigger_type/ZoneRecordWire.trigger_type values, mirroring SFZ's
// own trigger= opcode. ATTACK (0, the default -- every zone that never sets trigger= at all, and
// every zone built by the OSC-driven Mapping-tab path, which doesn't carry this opcode) is the
// ordinary note-on match every pre-existing zone already behaves as. RELEASE/RELEASE_KEY fire on
// note-off instead of note-on (see fire_release_trigger_zones()'s own comment). FIRST/LEGATO
// restrict an ordinary note-on match to only the first key of a phrase / only an overlapping
// legato note (see hds_sfz_trigger_matches_note_context()).
#define HDS_SFZ_TRIGGER_ATTACK      0
#define HDS_SFZ_TRIGGER_RELEASE     1
#define HDS_SFZ_TRIGGER_RELEASE_KEY 2
#define HDS_SFZ_TRIGGER_FIRST       3
#define HDS_SFZ_TRIGGER_LEGATO      4

#define SAMPLE_CHECK_READ(ptr, size, count, stream) \
    do { if (fread(ptr, size, count, stream) != (size_t)(count)) { fclose(stream); return false; } } while (0)

// Decodes `frames` interleaved WAV frames from `src` into l[]/r[] as float -1..1: the first two
// channels, a mono file duplicated to both. The same conversions the engine has always used
// (16/24/32-bit PCM, 32-bit float); shared by the pre-roll decode and the disk thread.
static void decode_wav_frames(const unsigned char *src, unsigned int frames, unsigned short channels,
                              unsigned short bits, unsigned short audio_format, float *l, float *r) {
    const unsigned int bps = bits / 8u, frame_bytes = (unsigned int)channels * bps;
    for (unsigned int i = 0; i < frames; i++) {
        const unsigned char *p = src + (size_t)i * frame_bytes;
        float v[2];
        for (int c = 0; c < 2; c++) {
            const unsigned char *s = p + (size_t)((c < channels) ? c : 0) * bps;
            if (audio_format == 1) {
                if (bits == 16) { int16_t x; memcpy(&x, s, 2); v[c] = (float)x / 32768.0f; }
                else if (bits == 24) { int x = (s[0] << 8) | (s[1] << 16) | (s[2] << 24); v[c] = (float)x / 2147483648.0f; }
                else { int32_t x; memcpy(&x, s, 4); v[c] = (float)x / 2147483648.0f; }
            } else {
                memcpy(&v[c], s, 4);
            }
        }
        l[i] = v[0];
        r[i] = v[1];
    }
}

static void free_prepared_sample(HdsPreparedSample *ps) {
    free(ps->pre_roll_l); free(ps->pre_roll_r);
    ps->pre_roll_l = ps->pre_roll_r = NULL;
}

// Parses a standard RIFF/WAVE header (skipping any unrecognized chunks rather than assuming a
// fixed layout), accepts PCM (16/24/32-bit int) or IEEE float (32-bit) data, and decodes the
// first SAMPLE_PRE_ROLL_LIMIT frames into freshly allocated RAM pre-roll buffers. Touches no
// engine state, so it runs on the sample loader thread (or, with no loader thread, wherever the
// caller is). Only the first two channels of a multichannel file are kept.
// The disk thread's open-file cache (DiskFileCache) must never read a file replaced on disk and
// then reloaded through a handle opened on the old one. Normally the reload itself handles that:
// the loader passes its freshly opened handle to the disk thread, which retires any older handle
// for that path (loader_hand_file_to_disk_thread). When a load can't hand its file over (no
// loader thread, or the hand-off queue is full), it bumps this counter instead, and the disk
// thread drops every cached handle. Process-wide: an extra bump only costs reopens.
static _Atomic unsigned int g_sample_file_generation = 0;

static bool mpmc_push(HdsMpmcQueue *q, const void *elem);

// Gives `f` (just used to read `path`'s header and pre-roll) to the disk thread instead of
// closing it. `ld` is NULL when the caller has no loader to hand it through.
static void loader_hand_file_to_disk_thread(HdsSampleLoaderState *ld, const char *path, FILE *f) {
    if (ld && ld->initialized) {
        HdsOpenedFile of;
        of.f = f;
        snprintf(of.path, sizeof(of.path), "%s", path);
        if (mpmc_push(&ld->opened_files, &of)) return;
    }
    fclose(f);
    atomic_fetch_add_explicit(&g_sample_file_generation, 1, memory_order_release);
}

static bool prepare_sample_file(const char *path, HdsPreparedSample *out, HdsSampleLoaderState *handoff) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[sample] failed to open '%s': %s\n", path, strerror(errno)); return false; }

    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }

    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    SAMPLE_CHECK_READ(&subchunk1_size, 4, 1, f);
    SAMPLE_CHECK_READ(&audio_format, 2, 1, f);
    SAMPLE_CHECK_READ(&num_channels, 2, 1, f);
    SAMPLE_CHECK_READ(&sample_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&byte_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&block_align, 2, 1, f);
    SAMPLE_CHECK_READ(&bits_per_sample, 2, 1, f);
    (void)byte_rate; (void)block_align;

    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "[sample] '%s': unsupported WAV format tag %u (need 1=PCM or 3=float)\n", path, audio_format);
        fclose(f); return false;
    }
    if (audio_format == 1 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "[sample] '%s': unsupported %u-bit PCM\n", path, bits_per_sample);
        fclose(f); return false;
    }
    if (audio_format == 3 && bits_per_sample != 32) {
        fprintf(stderr, "[sample] '%s': float WAV must be 32-bit\n", path);
        fclose(f); return false;
    }
    if (num_channels == 0) {
        fprintf(stderr, "[sample] '%s': WAV with no channels\n", path);
        fclose(f); return false;
    }
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);

    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }
    unsigned int data_size;
    SAMPLE_CHECK_READ(&data_size, 4, 1, f);

    unsigned int bytes_per_sample = bits_per_sample / 8u;
    unsigned int frame_bytes = (unsigned int)num_channels * bytes_per_sample;
    unsigned int total_frames = data_size / frame_bytes;

    out->total_frames = total_frames;
    out->sample_rate_hz = sample_rate;
    out->num_channels = num_channels;
    out->bits_per_sample = bits_per_sample;
    out->audio_format = audio_format;
    out->data_start_offset = (unsigned int)ftell(f);

    unsigned int pre_roll_frames = (total_frames < SAMPLE_PRE_ROLL_LIMIT) ? total_frames : SAMPLE_PRE_ROLL_LIMIT;
    out->pre_roll_frames = pre_roll_frames;
    out->pre_roll_l = calloc((size_t)pre_roll_frames + 2, sizeof(float)); // +2: render_sample_osc reads one frame ahead
    out->pre_roll_r = calloc((size_t)pre_roll_frames + 2, sizeof(float));
    unsigned char *raw = malloc((size_t)pre_roll_frames * frame_bytes + 1);
    if (!out->pre_roll_l || !out->pre_roll_r || !raw) { free(raw); free_prepared_sample(out); fclose(f); return false; }
    // One read for the whole pre-roll (at most SAMPLE_PRE_ROLL_LIMIT frames), not one per sample.
    if (pre_roll_frames > 0 && fread(raw, frame_bytes, pre_roll_frames, f) != pre_roll_frames) {
        fprintf(stderr, "[sample] '%s': file is shorter than its data chunk says\n", path);
        free(raw); free_prepared_sample(out); fclose(f); return false;
    }
    loader_hand_file_to_disk_thread(handoff, path, f);
    decode_wav_frames(raw, pre_roll_frames, num_channels, bits_per_sample, audio_format, out->pre_roll_l, out->pre_roll_r);
    free(raw);
    fprintf(stderr, "[sample] prepared '%s' (%u frames)\n", path, total_frames);
    return true;
}

static void retire_buffer(HdsSampleEngine *se, void *ptr);

// Installs a prepared file into catalog slot `index`: swaps in its header and pre-roll in one go
// (render_sample_osc never sees a half-installed slot) and hands the old pre-roll buffers to the
// loader thread to free. No file I/O and no free(), so it is safe on the audio thread. Loop points
// and end= are reset to a safe "off, full-range" placeholder -- the caller reapplies the zone's
// real ones right after, once total_frames is known.
static void install_prepared_sample(HdsSampleEngine *se, int index, const char *path, int root_note, HdsPreparedSample *ps) {
    SampleSlot *slot = &se->slots->sample_slots[index];
    retire_buffer(se, slot->pre_roll_l);
    retire_buffer(se, slot->pre_roll_r);
    snprintf(slot->file_path, SAMPLE_PATH_LEN, "%s", path);
    slot->root_note = root_note;
    slot->total_frames = ps->total_frames;
    slot->sample_rate_hz = ps->sample_rate_hz;
    slot->num_channels = ps->num_channels;
    slot->bits_per_sample = ps->bits_per_sample;
    slot->audio_format = ps->audio_format;
    slot->data_start_offset = ps->data_start_offset;
    slot->loop_enabled = false;
    slot->loop_start = 0;
    slot->loop_end = ps->total_frames;
    // Unlike the loop points, playback_end_frame is consulted on EVERY non-looping stream, so its
    // placeholder must be the full length, never 0 (which would end every note before it starts).
    slot->playback_end_frame = ps->total_frames;
    slot->loop_release_exit = false;
    slot->pre_roll_frames = ps->pre_roll_frames;
    slot->pre_roll_l = ps->pre_roll_l;
    slot->pre_roll_r = ps->pre_roll_r;
    ps->pre_roll_l = ps->pre_roll_r = NULL; // the slot owns them now
    slot->loaded = true;
    // A successful load always un-tombstones the slot -- a loaded-but-inactive slot would never
    // match in find_matching_sample_slot().
    slot->active = true;
}

// Prepare + install in the caller's thread: the path every load took before the loader thread
// existed, still used when it isn't running (tests that drive the engine directly, Android).
static bool load_sample_slot(HdsSampleEngine *se, int index, const char *path, int root_note) {
    if (index < 0 || index >= SAMPLE_SLOT_CAPACITY) return false;
    HdsPreparedSample ps;
    if (!prepare_sample_file(path, &ps, NULL)) return false;
    install_prepared_sample(se, index, path, root_note, &ps);
    return true;
}
// Header-only counterpart to load_sample_slot()'s own RIFF chunk walk (same chunk-skip shape,
// stopping right after reading data_size, with none of the pre-roll PCM decode that follows) --
// needed by the SFZ parser to convert loop_start=/loop_end=/offset=/end= (given in the SFZ file as
// absolute sample frames) into the
// 0-1 fractions ZoneRecordWire actually carries. Deliberately a small, independent duplicate of
// load_sample_slot()'s chunk-walk rather than a shared sub-helper both call -- load_sample_slot()
// is a heavily-exercised, already-correct hot path; this keeps that function untouched.
static bool read_wav_frame_count(const char *path, unsigned int *out_total_frames, unsigned int *out_sample_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }
    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    SAMPLE_CHECK_READ(&subchunk1_size, 4, 1, f);
    SAMPLE_CHECK_READ(&audio_format, 2, 1, f);
    SAMPLE_CHECK_READ(&num_channels, 2, 1, f);
    SAMPLE_CHECK_READ(&sample_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&byte_rate, 4, 1, f);
    SAMPLE_CHECK_READ(&block_align, 2, 1, f);
    SAMPLE_CHECK_READ(&bits_per_sample, 2, 1, f);
    (void)audio_format; (void)byte_rate; (void)block_align;
    if (out_sample_rate) *out_sample_rate = sample_rate;
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);
    SAMPLE_CHECK_READ(chunk_id, 1, 4, f);
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        SAMPLE_CHECK_READ(&chunk_size, 4, 1, f);
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return false; }
    }
    unsigned int data_size;
    SAMPLE_CHECK_READ(&data_size, 4, 1, f);
    fclose(f);
    int bytes_per_sample = bits_per_sample / 8;
    if (num_channels == 0 || bytes_per_sample == 0) return false;
    *out_total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);
    return true;
}

// A fixed pool of "currently streaming from disk" slots (see MAX_SAMPLE_STREAMS's comment on
// why this is a pool, not one embedded per voice/oscillator). A voice's oscillator claims one at
// note-on (if its shape is WAVE_SAMPLE) and releases it once that oscillator's amp envelope goes
// idle -- see claim_sample_stream/release_sample_stream and their call sites in handle_note_on/
// render_audio_block. Ownership mirrors RecordEngine/RecordRing exactly: the disk thread is the
// ONLY thing that ever opens/reads/closes disk_file_handle or writes into the ring buffer; the
// audio thread is the sole consumer (read_ptr, advanced by render_sample_osc). Start/stop is a
// tiny atomic command handshake -- the audio thread publishes a command, the disk thread performs
// the actual I/O -- plus a session number (see SampleStream.session) that tells the audio thread
// when the ring belongs to its own note.
typedef enum { SAMPLE_CMD_NONE = 0, SAMPLE_CMD_START, SAMPLE_CMD_STOP } SampleStreamCommand;

// g_sample_streams[]/g_sample_disk_thread/g_sample_disk_thread_shutdown are declared as part of
// HdsStreamsState, just below SeqAudioLane (see that struct's own comment for why the combined
// declaration lives there).

// Claims a free pool slot for a NEW stream session (audio-thread only, called from
// handle_note_on's WAVE_SAMPLE setup). Returns the pool index, or -1 if every slot is busy (can't
// play more simultaneous WAVE_SAMPLE oscillators than MAX_SAMPLE_STREAMS; the caller treats -1
// as "this oscillator stays silent for this note" rather than stealing another stream).
// The loop/end settings a stream's disk reads follow -- republished on every live edit of its
// zone (see SampleStream.claim_path's comment).
static void stream_publish_zone(SampleStream *st, const SampleSlot *slot) {
    atomic_store_explicit(&st->live_loop_enabled, slot->loop_enabled, memory_order_relaxed);
    atomic_store_explicit(&st->live_loop_release_exit, slot->loop_release_exit, memory_order_relaxed);
    atomic_store_explicit(&st->live_loop_start, slot->loop_start, memory_order_relaxed);
    atomic_store_explicit(&st->live_loop_end, slot->loop_end, memory_order_relaxed);
    atomic_store_explicit(&st->live_playback_end, slot->playback_end_frame, memory_order_relaxed);
    atomic_store_explicit(&st->live_loop_crossfade, slot->loop_crossfade_frames, memory_order_relaxed);
}

// Audio thread: a zone's loop/end settings changed in place (no reload) -- pass them on to every
// stream currently playing it.
static void publish_zone_edit_to_streams(HdsSampleEngine *se, int slot_index) {
    SampleStream *streams = se->streams->sample_streams;
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        if (streams[i].in_use && atomic_load_explicit(&streams[i].slot_index, memory_order_relaxed) == slot_index)
            stream_publish_zone(&streams[i], &se->slots->sample_slots[slot_index]);
    }
}

// claim_path as atomic words (see its comment): up to and including the word holding the NUL.
static void stream_store_path(SampleStream *st, const char *path) {
    char buf[SAMPLE_PATH_LEN];
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%s", path);
    size_t words = strlen(buf) / 8 + 1;
    for (size_t w = 0; w < words; w++) {
        uint64_t v; memcpy(&v, buf + w * 8, 8);
        atomic_store_explicit(&st->claim_path[w], v, memory_order_relaxed);
    }
}

static void stream_load_path(SampleStream *st, char out[SAMPLE_PATH_LEN]) {
    for (size_t w = 0; w < SAMPLE_PATH_LEN / 8; w++) {
        uint64_t v = atomic_load_explicit(&st->claim_path[w], memory_order_relaxed);
        memcpy(out + w * 8, &v, 8);
        if (memchr(out + w * 8, '\0', 8)) return;
    }
    out[SAMPLE_PATH_LEN - 1] = '\0';
}

static int claim_sample_stream(HdsSampleEngine *se, int slot_index, unsigned int start_frame) {
    SampleStream *streams = se->streams->sample_streams;
    const SampleSlot *slot = &se->slots->sample_slots[slot_index];
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        if (!streams[i].in_use) {
            SampleStream *st = &streams[i];
            st->in_use = true;
            unsigned int session = atomic_load_explicit(&st->session, memory_order_relaxed) + 1;
            // Everything the disk thread will need about this zone, copied now (it never reads
            // SampleSlot itself -- see SampleStream.claim_path).
            stream_store_path(st, slot->file_path);
            atomic_store_explicit(&st->claim_loaded, slot->loaded, memory_order_relaxed);
            atomic_store_explicit(&st->claim_reverse, slot->reverse, memory_order_relaxed);
            atomic_store_explicit(&st->claim_data_offset, slot->data_start_offset, memory_order_relaxed);
            atomic_store_explicit(&st->claim_total_frames, slot->total_frames, memory_order_relaxed);
            atomic_store_explicit(&st->claim_pre_roll_frames, slot->pre_roll_frames, memory_order_relaxed);
            atomic_store_explicit(&st->claim_channels, slot->num_channels, memory_order_relaxed);
            atomic_store_explicit(&st->claim_bits, slot->bits_per_sample, memory_order_relaxed);
            atomic_store_explicit(&st->claim_audio_format, slot->audio_format, memory_order_relaxed);
            stream_publish_zone(st, slot);
            atomic_store_explicit(&streams[i].slot_index, slot_index, memory_order_relaxed);
            atomic_store_explicit(&streams[i].start_frame, start_frame, memory_order_relaxed);
            // Reset the release flag for this new session -- a reused pool slot otherwise carries
            // over the previous note's release state.
            atomic_store_explicit(&streams[i].release_requested, false, memory_order_relaxed);
            // Release-store: a disk thread that reads this session number also sees the
            // slot_index/start_frame written above (or newer ones, whose session it then won't
            // match -- see sample_disk_start_session()).
            atomic_store_explicit(&streams[i].session, session, memory_order_release);
            atomic_store_explicit(&streams[i].command, SAMPLE_CMD_START, memory_order_release);
            return i;
        }
    }
    return -1;
}

// Releases a claimed stream back to the pool (audio-thread only) and tells the disk thread to
// close its file.
void release_sample_stream(HdsSampleEngine *se, int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&se->streams->sample_streams[stream_index].command, SAMPLE_CMD_STOP, memory_order_release);
    se->streams->sample_streams[stream_index].in_use = false;
}

// Marks a claimed stream's note as having entered RELEASE (audio-thread only, called once from
// trigger_voice_release() per oscillator). Read by the disk thread to decide whether a
// loop_release_exit zone should keep honoring its loop or fall through to its natural end -- see
// SampleSlot.loop_release_exit's own comment. A no-op on an already-released/invalid stream index,
// same defensive shape as release_sample_stream() above.
void request_sample_stream_release(HdsSampleEngine *se, int stream_index) {
    if (stream_index < 0 || stream_index >= MAX_SAMPLE_STREAMS) return;
    atomic_store_explicit(&se->streams->sample_streams[stream_index].release_requested, true, memory_order_release);
}

// --- The disk thread ---
//
// One pass of its loop: take every pending START/STOP command, then read ONE chunk (up to
// DISK_CHUNK_FRAMES) for the stream with the least audio buffered, so a note that just started
// (empty ring) is served before rings that already hold a second or more. Each chunk is one
// fread() of raw frames, decoded in bulk; consecutive chunks of a stream skip the fseek. Sleeps
// 1 ms only when a pass found nothing to do.
#define DISK_CHUNK_FRAMES 4096
#define DISK_SCRATCH_BYTES (DISK_CHUNK_FRAMES * 8 * 4) // a chunk of up to 8 channels at 32 bits

// --- The disk thread's open-file cache ---
//
// Opening a file costs a directory lookup and, on a cold disk, a seek -- doing it on every note-on
// let a burst of notes run their rings dry before any audio arrived. The disk thread instead keeps
// up to DISK_FILE_CACHE_SIZE files open, keyed by path, and shares one FILE* between every stream
// playing that file (each entry remembers where its FILE* is positioned, so a read from another
// stream just seeks first). Entries no stream is using stay open until evicted
// least-recently-used. All of it is private to the disk thread: no locks.
#define DISK_FILE_CACHE_SIZE 64 // > MAX_SAMPLE_STREAMS, so a free or idle entry always exists

typedef struct {
    char path[SAMPLE_PATH_LEN];
    FILE *f;
    int refs;                   // streams currently using it
    unsigned long long last_use;
    long long pos;              // byte offset f is at, -1: unknown
    bool stale;                 // opened before the last g_sample_file_generation bump: close once unused
} DiskFileEntry;

typedef struct {
    DiskFileEntry e[DISK_FILE_CACHE_SIZE];
    unsigned long long clock;
    unsigned int generation;
} DiskFileCache;

static void disk_cache_close_entry(DiskFileEntry *ent) {
    if (ent->f) fclose(ent->f);
    ent->f = NULL; ent->path[0] = '\0'; ent->refs = 0; ent->stale = false; ent->pos = -1;
}

// A zone was reloaded since the last check: close every idle entry, and mark the busy ones so
// they close as soon as their last stream lets go (new sessions never pick a stale entry).
static void disk_cache_check_generation(DiskFileCache *c) {
    unsigned int g = atomic_load_explicit(&g_sample_file_generation, memory_order_acquire);
    if (g == c->generation) return;
    c->generation = g;
    for (int i = 0; i < DISK_FILE_CACHE_SIZE; i++) {
        if (!c->e[i].f) continue;
        if (c->e[i].refs == 0) disk_cache_close_entry(&c->e[i]);
        else c->e[i].stale = true;
    }
}

// Returns an entry index holding `path` open (refs already taken), or -1 if it can't be opened.
static int disk_cache_acquire(DiskFileCache *c, const char *path) {
    disk_cache_check_generation(c);
    int victim = -1;
    for (int i = 0; i < DISK_FILE_CACHE_SIZE; i++) {
        DiskFileEntry *ent = &c->e[i];
        if (ent->f && !ent->stale && strcmp(ent->path, path) == 0) {
            ent->refs++; ent->last_use = ++c->clock;
            return i;
        }
        if (ent->refs > 0) continue;
        if (victim < 0 || !ent->f || (c->e[victim].f && ent->last_use < c->e[victim].last_use)) victim = i;
    }
    if (victim < 0) return -1; // unreachable while DISK_FILE_CACHE_SIZE > MAX_SAMPLE_STREAMS
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    DiskFileEntry *ent = &c->e[victim];
    disk_cache_close_entry(ent);
    snprintf(ent->path, sizeof(ent->path), "%s", path);
    ent->f = f; ent->refs = 1; ent->last_use = ++c->clock; ent->pos = 0;
    return victim;
}

static bool mpmc_pop(HdsMpmcQueue *q, void *out);

// Takes every file the loader handed over (see loader_hand_file_to_disk_thread). A handle for a
// path already cached is the newer one -- the file may have been replaced on disk -- so the old
// entry is retired (closed now if idle, once its last stream ends otherwise) and the new handle
// cached idle, ready for the zone's first note.
static void disk_cache_adopt_loaded_files(DiskFileCache *c, HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    if (!atomic_load_explicit(&ld->opened_files_ready, memory_order_acquire)) return;
    HdsOpenedFile of;
    while (mpmc_pop(&ld->opened_files, &of)) {
        int victim = -1;
        for (int i = 0; i < DISK_FILE_CACHE_SIZE; i++) {
            DiskFileEntry *ent = &c->e[i];
            if (ent->f && !ent->stale && strcmp(ent->path, of.path) == 0) {
                if (ent->refs == 0) disk_cache_close_entry(ent); else ent->stale = true;
            }
            if (ent->refs > 0) continue;
            if (victim < 0 || !ent->f || (c->e[victim].f && ent->last_use < c->e[victim].last_use)) victim = i;
        }
        if (victim < 0) { fclose(of.f); continue; } // unreachable while DISK_FILE_CACHE_SIZE > MAX_SAMPLE_STREAMS
        DiskFileEntry *ent = &c->e[victim];
        disk_cache_close_entry(ent);
        snprintf(ent->path, sizeof(ent->path), "%s", of.path);
        ent->f = of.f; ent->refs = 0; ent->last_use = ++c->clock; ent->pos = -1; // position: wherever the loader left it
    }
}

static void disk_cache_release_stream(DiskFileCache *c, SampleStream *st) {
    if (st->disk_file_entry >= 0) {
        DiskFileEntry *ent = &c->e[st->disk_file_entry];
        if (--ent->refs == 0 && ent->stale) disk_cache_close_entry(ent);
    }
    st->disk_file_entry = -1;
    st->disk_file_handle = NULL;
}

typedef struct {
    DiskFileCache *files;
    unsigned char *raw;   // DISK_SCRATCH_BYTES
    float *l, *r;         // DISK_CHUNK_FRAMES each: the chunk being written
    float *xl, *xr;       // DISK_CHUNK_FRAMES each: the loop crossfade's pre-loop frames
} DiskScratch;

// Reads up to `count` frames starting at file frame `pos` into l[]/r[]. Returns how many it got
// (fewer only at a read error or a file shorter than its header says).
// The FILE* may be shared with other streams playing the same file (see DiskFileCache): the
// entry's `pos` says where it is, so a stream's sequential reads still skip the fseek and keep
// stdio's read buffer as long as nothing else read from it in between.
static unsigned int disk_read_frames(SampleStream *st, DiskFileCache *files, unsigned int pos, unsigned int count, float *l, float *r, unsigned char *raw) {
    unsigned int frame_bytes = (unsigned int)st->disk_num_channels * (st->disk_bits_per_sample / 8u);
    if (frame_bytes == 0 || st->disk_file_entry < 0) return 0;
    DiskFileEntry *ent = &files->e[st->disk_file_entry];
    unsigned int max_frames = DISK_SCRATCH_BYTES / frame_bytes;
    if (count > max_frames) count = max_frames;
    if (count == 0) return 0;
    long long byte_offset = (long long)st->disk_data_start_offset + (long long)pos * (long long)frame_bytes;
    if (ent->pos != byte_offset) {
        if (fseek(ent->f, (long)byte_offset, SEEK_SET) != 0) { ent->pos = -1; return 0; }
    }
    unsigned int got = (unsigned int)fread(raw, frame_bytes, count, ent->f);
    ent->pos = (got == count) ? byte_offset + (long long)got * frame_bytes : -1;
    decode_wav_frames(raw, got, st->disk_num_channels, st->disk_bits_per_sample, st->disk_audio_format, l, r);
    return got;
}

static void disk_ring_write(SampleStream *st, const float *l, const float *r, unsigned int n, bool reversed) {
    unsigned int w = atomic_load_explicit(&st->write_ptr, memory_order_relaxed);
    for (unsigned int i = 0; i < n; i++) {
        unsigned int src = reversed ? n - 1 - i : i;
        st->ring_l[(w + i) & SAMPLE_RING_MASK] = l[src];
        st->ring_r[(w + i) & SAMPLE_RING_MASK] = r[src];
    }
    atomic_store_explicit(&st->write_ptr, w + n, memory_order_release);
}

static void disk_mark_exhausted(SampleStream *st) {
    st->disk_exhausted = true;
    atomic_store_explicit(&st->disk_reached_eof, true, memory_order_release);
}

// The zone settings a disk read follows, from the stream's own copies (see SampleStream.claim_path).
typedef struct {
    bool loop_active;
    unsigned int loop_start, loop_end, playback_end, total_frames;
    int loop_crossfade;
} DiskZone;

// SFZ loop_sustain: once this stream's owning voice has entered release AND the zone is
// loop_release_exit, the loop is no longer honored for the rest of this session -- playback
// continues straight through loop_end to the zone's natural end (the "sustain loop, then play out
// the tail" behavior), a no-op for loop_continuous zones.
static DiskZone disk_zone(SampleStream *st) {
    DiskZone z;
    z.total_frames = st->disk_total_frames;
    z.loop_start = atomic_load_explicit(&st->live_loop_start, memory_order_relaxed);
    z.loop_end = atomic_load_explicit(&st->live_loop_end, memory_order_relaxed);
    z.playback_end = atomic_load_explicit(&st->live_playback_end, memory_order_relaxed);
    z.loop_crossfade = atomic_load_explicit(&st->live_loop_crossfade, memory_order_relaxed);
    if (z.playback_end > z.total_frames) z.playback_end = z.total_frames;
    bool enabled = atomic_load_explicit(&st->live_loop_enabled, memory_order_relaxed);
    bool release_exit = atomic_load_explicit(&st->live_loop_release_exit, memory_order_relaxed);
    bool release_requested = atomic_load_explicit(&st->release_requested, memory_order_acquire);
    // A loop pair caught mid-edit (start updated, end not yet) or out of range just doesn't loop
    // for this chunk.
    z.loop_active = enabled && !(release_exit && release_requested) && z.loop_end > z.loop_start && z.loop_end <= z.total_frames;
    return z;
}

// Forward: one chunk from disk_read_frame_pos toward the loop end (looping) or end= (not).
//
// Loop splice: the jump from loop_end back to loop_start is crossfaded here, as the ring is
// written, the standard pre-loop way -- the last `xf` frames before loop_end are blended toward
// the `xf` frames just before loop_start, so that by the time playback jumps to loop_start the
// signal is already the audio that naturally leads into it. Nothing on the audio thread is
// involved, and the crossfade length is no longer tied to the ring's starvation margin. Width:
// SFZ loop_crossfade= if the zone set it (0 = a deliberate hard cut), SAMPLE_XFADE_SIZE
// otherwise, limited to the loop's own length and to the audio available before loop_start.
static void disk_fill_forward(SampleStream *st, unsigned int space, DiskScratch *sc) {
    DiskZone z = disk_zone(st);
    bool loop_active = z.loop_active;
    unsigned int pos = st->disk_read_frame_pos;
    if (loop_active && pos >= z.loop_end) pos = z.loop_start;
    unsigned int end = loop_active ? z.loop_end : z.playback_end;
    if (pos >= end) { disk_mark_exhausted(st); return; } // only reachable when not looping

    unsigned int n = end - pos;
    if (n > space) n = space;
    if (n > DISK_CHUNK_FRAMES) n = DISK_CHUNK_FRAMES;
    unsigned int got = disk_read_frames(st, sc->files, pos, n, sc->l, sc->r, sc->raw);
    if (got == 0) { disk_mark_exhausted(st); return; }

    if (loop_active) {
        unsigned int loop_len = z.loop_end - z.loop_start;
        unsigned int xf = (z.loop_crossfade < 0) ? (unsigned int)SAMPLE_XFADE_SIZE : (unsigned int)z.loop_crossfade;
        if (xf > loop_len) xf = loop_len;
        if (xf > z.loop_start) xf = z.loop_start;
        unsigned int xf_start = z.loop_end - xf;
        unsigned int lo = (pos > xf_start) ? pos : xf_start, hi = pos + got;
        if (xf > 0 && hi > lo) {
            unsigned int pre = disk_read_frames(st, sc->files, lo - loop_len, hi - lo, sc->xl, sc->xr, sc->raw);
            for (unsigned int k = 0; k < pre; k++) {
                float t = (float)(lo + k - xf_start + 1) / (float)(xf + 1);
                unsigned int i = lo - pos + k;
                sc->l[i] += t * (sc->xl[k] - sc->l[i]);
                sc->r[i] += t * (sc->xr[k] - sc->r[i]);
            }
        }
    }

    disk_ring_write(st, sc->l, sc->r, got, false);
    pos += got;
    if (loop_active && pos >= z.loop_end) pos = z.loop_start;
    st->disk_read_frame_pos = pos;
    if (!loop_active && pos >= end) disk_mark_exhausted(st);
}

// Reverse: one chunk walking backward from disk_read_frame_pos ("the next frame to read"),
// read forward from disk in one piece and written to the ring back to front. A looping zone
// jumps from loop_start back up to loop_end - 1 (no crossfade in this direction); a zone that
// started below its loop_start, or isn't looping, ends at frame 0.
static void disk_fill_reverse(SampleStream *st, unsigned int space, DiskScratch *sc) {
    DiskZone z = disk_zone(st);
    bool loop_active = z.loop_active;
    unsigned int pos = st->disk_read_frame_pos;
    if (z.total_frames == 0) { disk_mark_exhausted(st); return; }
    if (pos >= z.total_frames) pos = z.total_frames - 1;
    unsigned int floor = (loop_active && pos >= z.loop_start) ? z.loop_start : 0;

    unsigned int n = pos - floor + 1;
    if (n > space) n = space;
    if (n > DISK_CHUNK_FRAMES) n = DISK_CHUNK_FRAMES;
    unsigned int first = pos - n + 1;
    unsigned int got = disk_read_frames(st, sc->files, first, n, sc->l, sc->r, sc->raw);
    if (got != n) { disk_mark_exhausted(st); return; }
    disk_ring_write(st, sc->l, sc->r, n, true);

    if (first > floor) st->disk_read_frame_pos = first - 1;
    else if (loop_active) st->disk_read_frame_pos = z.loop_end - 1;
    else disk_mark_exhausted(st);
}

// SAMPLE_CMD_START: begin the session the audio thread most recently published. The ring, the
// EOF flag and the file all get reset BEFORE ready_session is stored, so the audio thread can
// never read this pool entry's ring for its new note while it still holds an earlier session's
// audio. If the session number read here is already older than the audio thread's current one
// (it claimed again while this ran), the mismatch just keeps the voice silent until the next
// START -- already pending -- is processed.
static void sample_disk_start_session(SampleStream *st, DiskFileCache *files, HdsSampleEngine *se) {
    disk_cache_release_stream(files, st);
    // Again here, not just once per pass: the zone this note plays may have been installed (and
    // its file handed over) after this pass's first check. The acquire on `command` makes that
    // hand-off visible now.
    disk_cache_adopt_loaded_files(files, se);
    unsigned int session = atomic_load_explicit(&st->session, memory_order_acquire);
    st->active_slot_index = atomic_load_explicit(&st->slot_index, memory_order_relaxed);
    st->active_start_frame = atomic_load_explicit(&st->start_frame, memory_order_relaxed);
    st->disk_exhausted = false;
    atomic_store_explicit(&st->disk_reached_eof, false, memory_order_relaxed);
    st->disk_reverse = atomic_load_explicit(&st->claim_reverse, memory_order_relaxed);
    st->disk_total_frames = atomic_load_explicit(&st->claim_total_frames, memory_order_relaxed);
    st->disk_data_start_offset = atomic_load_explicit(&st->claim_data_offset, memory_order_relaxed);
    st->disk_num_channels = (unsigned short)atomic_load_explicit(&st->claim_channels, memory_order_relaxed);
    st->disk_bits_per_sample = (unsigned short)atomic_load_explicit(&st->claim_bits, memory_order_relaxed);
    st->disk_audio_format = (unsigned short)atomic_load_explicit(&st->claim_audio_format, memory_order_relaxed);
    unsigned int pre_roll_frames = atomic_load_explicit(&st->claim_pre_roll_frames, memory_order_relaxed);
    if (atomic_load_explicit(&st->claim_loaded, memory_order_relaxed)) {
        char path[SAMPLE_PATH_LEN];
        stream_load_path(st, path);
        st->disk_file_entry = disk_cache_acquire(files, path);
        if (st->disk_file_entry >= 0) st->disk_file_handle = files->e[st->disk_file_entry].f;
    }
    if (st->disk_file_handle) {
        // Forward zones get the pre-roll fast path (it always covers frames [0, pre_roll_frames)):
        // if the requested start falls inside that window, the ring begins exactly at the
        // pre_roll_frames boundary; otherwise -- or for any reverse zone, which skips the pre-roll
        // entirely (see start_sample_osc) -- at the requested frame.
        st->disk_read_frame_pos = (!st->disk_reverse && st->active_start_frame <= pre_roll_frames)
            ? pre_roll_frames : st->active_start_frame;
    } else {
        disk_mark_exhausted(st);
    }
    atomic_store_explicit(&st->write_ptr, 0, memory_order_relaxed);
    atomic_store_explicit(&st->read_ptr, 0, memory_order_relaxed);
    atomic_store_explicit(&st->ready_session, session, memory_order_release);
}

void *sample_disk_thread_main(void *arg) {
    HdsSampleEngine *se = (HdsSampleEngine *)arg;
    DiskScratch sc = {
        calloc(1, sizeof(DiskFileCache)),
        malloc(DISK_SCRATCH_BYTES),
        malloc(sizeof(float) * DISK_CHUNK_FRAMES), malloc(sizeof(float) * DISK_CHUNK_FRAMES),
        malloc(sizeof(float) * DISK_CHUNK_FRAMES), malloc(sizeof(float) * DISK_CHUNK_FRAMES),
    };
    if (!sc.files || !sc.raw || !sc.l || !sc.r || !sc.xl || !sc.xr) {
        fprintf(stderr, "[sample] disk thread: out of memory -- WAVE_SAMPLE streaming unavailable\n");
        free(sc.files); free(sc.raw); free(sc.l); free(sc.r); free(sc.xl); free(sc.xr);
        return NULL;
    }
    for (int i = 0; i < DISK_FILE_CACHE_SIZE; i++) sc.files->e[i].pos = -1;
    sc.files->generation = atomic_load_explicit(&g_sample_file_generation, memory_order_acquire);
    // Streams still pointing at a cache from an earlier run of this thread (deactivate/activate).
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) { se->streams->sample_streams[i].disk_file_entry = -1; se->streams->sample_streams[i].disk_file_handle = NULL; }
    while (!atomic_load_explicit(&se->streams->sample_disk_thread_shutdown, memory_order_acquire)) {
        bool work_done = false;
        SampleStream *streams = se->streams->sample_streams;
        disk_cache_adopt_loaded_files(sc.files, se);

        for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
            SampleStream *st = &streams[i];
            // Acquire-exchange: pairs with claim_sample_stream's release-store of `command`.
            int cmd = atomic_exchange_explicit(&st->command, SAMPLE_CMD_NONE, memory_order_acquire);
            if (cmd == SAMPLE_CMD_START) {
                sample_disk_start_session(st, sc.files, se);
                work_done = true;
            } else if (cmd == SAMPLE_CMD_STOP) {
                disk_cache_release_stream(sc.files, st); // the file stays open in the cache
                work_done = true;
            }
        }

        int best = -1;
        unsigned int best_filled = UINT_MAX, best_space = 0;
        for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
            SampleStream *st = &streams[i];
            if (!st->disk_file_handle || st->disk_exhausted) continue;
            unsigned int r_ptr = atomic_load_explicit(&st->read_ptr, memory_order_acquire);
            unsigned int w_ptr = atomic_load_explicit(&st->write_ptr, memory_order_relaxed);
            unsigned int filled = (w_ptr - r_ptr) & SAMPLE_RING_MASK;
            unsigned int space = SAMPLE_RING_SIZE - 1 - filled;
            if (space < (unsigned int)BUFFER_SIZE) continue; // plenty already buffered
            if (filled < best_filled) { best = i; best_filled = filled; best_space = space; }
        }
        if (best >= 0) {
            SampleStream *st = &streams[best];
            if (st->disk_reverse) disk_fill_reverse(st, best_space, &sc);
            else disk_fill_forward(st, best_space, &sc);
            work_done = true;
        }

        if (!work_done) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); } // 1ms idle poll
    }
    // Every handle is the cache's: close them all, and leave no stream pointing at one (the hosts'
    // post-join cleanup loops then find nothing left to fclose).
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) { se->streams->sample_streams[i].disk_file_entry = -1; se->streams->sample_streams[i].disk_file_handle = NULL; }
    for (int i = 0; i < DISK_FILE_CACHE_SIZE; i++) disk_cache_close_entry(&sc.files->e[i]);
    free(sc.files); free(sc.raw); free(sc.l); free(sc.r); free(sc.xl); free(sc.xr);
    return NULL;
}

// --- Sample Bank catalog protocol: two dedicated OSC addresses ---
//
// The Mapping tab's zone catalog travels as its own pair of OSC messages, entirely separate from
// SynthSharedControls/OSC_PARAMS[] above (see the historical BREAKING CHANGE comment on
// SynthSharedControls for why it was already kept out of that struct). Each message carries
// exactly ONE zone -- a drag/edit that touches several zones just sends several messages back to
// back, rather than one batched wire record the way the old two-phase TCP framing (accumulate a
// header, size a body buffer from its claimed count, accumulate the body) needed to. That whole
// framing layer is gone: liblo already delivers each UDP datagram as one complete, self-contained
// message, so there is no partial-message state to track between polls.
//   "/hds/zone/upsert" (types "i" + "f"*18 + "s") -- add-or-update one zone by zone_id (on
//     fresh connect the frontend sends one upsert per zone in its current list, since this
//     side's catalog starts empty).
//   "/hds/zone/remove" (types "i") -- delete one zone by zone_id.
// See zone_upsert_handler()/zone_remove_handler() below, registered in main().

// Fills every SFZ-first-class field of `rec` with "this feature is off for this zone" -- the
// state every OSC-driven zone (never carries these opcodes at all) must end up in. Called first by
// both zone_upsert_handler() (whose own core-19-floats loop leaves them untouched afterward) and
// hds_load_sfz_for_osc() (which then overwrites the ones its own parsed SfzRegion actually sets),
// so neither call site can forget a field and leave it as uninitialized stack garbage.
static void zone_record_wire_set_sfz_defaults(ZoneRecordWire *rec) {
    rec->loop_crossfade_frames = -1;
    rec->seq_length = 0; rec->seq_position = 0;
    rec->lorand = 0.0f; rec->hirand = 1.0f;
    rec->choke_group = 0; rec->choke_off_by = 0;
    rec->trigger_type = HDS_SFZ_TRIGGER_ATTACK;
    rec->sw_lokey = -1; rec->sw_hikey = -1; rec->sw_last = -1;
    rec->rt_decay = 0.0f;
    rec->delay_seconds = 0.0f;
    rec->has_own_ampeg = 0;
    rec->ampeg_delay = 0.0f; rec->ampeg_attack = 0.0f; rec->ampeg_hold = 0.0f; rec->ampeg_decay = 0.0f;
    rec->ampeg_sustain = 100.0f; rec->ampeg_release = 0.0f; rec->ampeg_start = 0.0f;
    rec->note_polyphony = 0;
    rec->velcurve_count = 0;
    rec->xfin_lovel = -1; rec->xfin_hivel = -1; rec->xfout_lovel = -1; rec->xfout_hivel = -1;
    rec->xfin_lokey = -1; rec->xfin_hikey = -1; rec->xfout_lokey = -1; rec->xfout_hikey = -1;
    rec->drum_bus = 0;
}

// How many queued sample loads the audio thread performs itself per block when the loader thread
// is NOT running (tests, Android) -- each is a blocking file-open+pre-roll-decode, so this is the
// throttle that keeps a large bulk-add from stalling audio in one long burst. With the loader
// thread running the audio thread never opens a file at all; see drain_pending_sample_loads().
#define SAMPLE_LOADS_PER_BLOCK 1
// With the loader thread running: finished results applied per block, and SFZ regions installed
// per block (each is cheap, but a catalog scan per region adds up on a large instrument).
#define SAMPLE_RESULTS_PER_BLOCK 32
#define SFZ_REGIONS_PER_BLOCK 64

// Clamps a wire-carried oscillator index (a plain float, like every other OSC-carried enum in
// this file) into the valid 0-3 range -- shared by every site that reads ZoneRecordWire.assigned_osc.
static inline int clamp_assigned_osc(float raw) {
    int v = (int)raw;
    if (v < 0) return 0;
    if (v > 3) return 3;
    return v;
}

// Same idea as clamp_assigned_osc() above, for ZoneRecordWire.owner_program's 0..SINGLE_PROGRAM_SLOT range.
static inline int clamp_owner_program(float raw) {
    int v = (int)raw;
    if (v < 0) return 0;
    if (v > SINGLE_PROGRAM_SLOT) return SINGLE_PROGRAM_SLOT;
    return v;
}

// Copies a zone record into a PendingSampleLoad -- the one place the record's fields are mapped.
static void fill_pending_from_record(PendingSampleLoad *p, int slot_index, const ZoneRecordWire *rec) {
    p->slot_index = slot_index;
    p->zone_id = rec->zone_id;
    snprintf(p->path, SAMPLE_PATH_LEN, "%s", rec->path);
    p->root_note = (int)rec->root_note;
    p->min_note = (int)rec->min_note; p->max_note = (int)rec->max_note;
    p->min_velocity = (int)rec->min_velocity; p->max_velocity = (int)rec->max_velocity;
    p->unpitched = rec->unpitched != 0.0f;
    p->loop_start_frac = rec->loop_start; p->loop_end_frac = rec->loop_end;
    p->loop_enabled = rec->loop_enabled != 0.0f;
    p->round_robin_group = (int)rec->round_robin_group;
    p->tune_cents = rec->tune_cents; p->gain_db = rec->gain_db; p->pan = rec->pan;
    p->start_offset_frac = rec->start_offset_frac;
    p->reverse = rec->reverse != 0.0f;
    p->loop_release_exit = rec->loop_release_exit != 0.0f;
    p->end_frac = rec->end_frac;
    p->assigned_osc = clamp_assigned_osc(rec->assigned_osc);
    // SFZ first-class opcode support -- straight field-for-field carry-through, see
    // ZoneRecordWire's own matching block for what each one means and its OSC-safe default.
    p->loop_crossfade_frames = rec->loop_crossfade_frames;
    p->seq_length = rec->seq_length; p->seq_position = rec->seq_position;
    p->lorand = rec->lorand; p->hirand = rec->hirand;
    p->choke_group = rec->choke_group; p->choke_off_by = rec->choke_off_by;
    p->trigger_type = rec->trigger_type;
    p->sw_lokey = rec->sw_lokey; p->sw_hikey = rec->sw_hikey; p->sw_last = rec->sw_last;
    p->rt_decay = rec->rt_decay;
    p->delay_seconds = rec->delay_seconds;
    p->has_own_ampeg = rec->has_own_ampeg != 0;
    p->ampeg_delay = rec->ampeg_delay; p->ampeg_attack = rec->ampeg_attack; p->ampeg_hold = rec->ampeg_hold;
    p->ampeg_decay = rec->ampeg_decay; p->ampeg_sustain = rec->ampeg_sustain; p->ampeg_release = rec->ampeg_release;
    p->ampeg_start = rec->ampeg_start;
    p->note_polyphony = rec->note_polyphony;
    p->velcurve_count = rec->velcurve_count;
    for (int i = 0; i < rec->velcurve_count && i < SFZ_MAX_VELCURVE_POINTS; i++) {
        p->velcurve_point[i] = rec->velcurve_point[i]; p->velcurve_gain[i] = rec->velcurve_gain[i];
    }
    p->xfin_lovel = rec->xfin_lovel; p->xfin_hivel = rec->xfin_hivel;
    p->xfout_lovel = rec->xfout_lovel; p->xfout_hivel = rec->xfout_hivel;
    p->xfin_lokey = rec->xfin_lokey; p->xfin_hikey = rec->xfin_hikey;
    p->xfout_lokey = rec->xfout_lokey; p->xfout_hikey = rec->xfout_hikey;
    p->drum_bus = rec->drum_bus;
}

static void enqueue_pending_sample_load(HdsSampleEngine *se, int slot_index, const ZoneRecordWire *rec) {
    if (se->pending->pending_load_count >= PENDING_SAMPLE_LOAD_CAPACITY) {
        fprintf(stderr, "[samples] pending-load queue full -- dropping load for zone_id %d (re-send to retry)\n", rec->zone_id);
        return;
    }
    fill_pending_from_record(&se->pending->pending_loads[se->pending->pending_load_tail], slot_index, rec);
    se->pending->pending_load_tail = (se->pending->pending_load_tail + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
    se->pending->pending_load_count++;
}

// Converts loop-point fractions to real frame indices (shared by drain_pending_sample_loads()'s
// result path and the "path unchanged" branch of an UPSERT, both of which need total_frames --
// only known once a file is actually loaded -- to do this conversion). Keeps a minimum loop length
// of one default crossfade window (SAMPLE_XFADE_SIZE), so a tiny loop isn't one long crossfade.
static void apply_loop_points(SampleSlot *slot, float start_frac, float end_frac) {
    if (!slot->loaded || slot->total_frames <= 1) return;
    unsigned int total = slot->total_frames;
    start_frac = fmaxf(0.0f, fminf(1.0f, start_frac));
    end_frac = fmaxf(0.0f, fminf(1.0f, end_frac));
    unsigned int loop_start = (unsigned int)(start_frac * (float)(total - 1));
    unsigned int loop_end = (unsigned int)(end_frac * (float)(total - 1));
    unsigned int min_len = (unsigned int)SAMPLE_XFADE_SIZE;
    if (total - 1 < min_len) min_len = (total > 1) ? total - 1 : 0; // degenerate tiny file
    if (loop_end < loop_start + min_len) {
        loop_end = loop_start + min_len;
        if (loop_end > total - 1) { loop_end = total - 1; loop_start = (loop_end > min_len) ? loop_end - min_len : 0; }
    }
    slot->loop_start = loop_start;
    slot->loop_end = loop_end;
}

// Converts SFZ's end= (a 0-1 fraction, same convention as loop_start/loop_end/start_offset_frac)
// to an exclusive frame bound -- mirrors apply_loop_points()'s shape and call sites exactly.
// end_frac == 1.0 (the default when a zone specifies no end=) reproduces the pre-existing
// behavior of using the full file length verbatim (playback_end_frame == total_frames).
static void apply_playback_end(SampleSlot *slot, float end_frac) {
    if (!slot->loaded || slot->total_frames == 0) { slot->playback_end_frame = slot->total_frames; return; }
    end_frac = fmaxf(0.0f, fminf(1.0f, end_frac));
    unsigned int frame = (unsigned int)(end_frac * (float)slot->total_frames + 0.5f);
    if (frame < 1) frame = 1;
    if (frame > slot->total_frames) frame = slot->total_frames;
    slot->playback_end_frame = frame;
}

// A queued load can go stale before it completes: a ZONE_OP_REMOVE can tombstone its slot (added,
// then removed again before its turn), and apply_zone_upsert() can reuse that freed slot index for
// a brand new, unrelated zone. The removal wins -- checked by zone_id, not just `active`, so a slow
// load for the OLD zone can never resurrect or overwrite the NEW zone at the same index.
static bool pending_load_still_wanted(HdsSampleEngine *se, const PendingSampleLoad *p) {
    const SampleSlot *slot = &se->slots->sample_slots[p->slot_index];
    return slot->active && slot->zone_id == p->zone_id;
}

// Copies a finished load's per-zone fields onto its slot. Runs right after the file is installed
// -- loop points and end= are fractions that need the new total_frames -- so a zone whose
// metadata arrived while its load was still queued never stays on the placeholder loop points.
static void apply_pending_fields(SampleSlot *slot, const PendingSampleLoad *p) {
    slot->min_note = p->min_note; slot->max_note = p->max_note;
    slot->min_velocity = p->min_velocity; slot->max_velocity = p->max_velocity;
    slot->unpitched = p->unpitched;
    slot->round_robin_group = p->round_robin_group;
    slot->assigned_osc = p->assigned_osc;
    slot->loop_enabled = p->loop_enabled;
    apply_loop_points(slot, p->loop_start_frac, p->loop_end_frac);
    slot->tune_cents = p->tune_cents; slot->gain_db = p->gain_db; slot->pan = p->pan;
    slot->start_offset_frac = p->start_offset_frac;
    slot->reverse = p->reverse;
    slot->loop_release_exit = p->loop_release_exit;
    apply_playback_end(slot, p->end_frac);
    // SFZ first-class opcode support -- straight field-for-field carry-through from the
    // queued load, same convention as every field just above.
    slot->loop_crossfade_frames = p->loop_crossfade_frames;
    slot->seq_length = p->seq_length; slot->seq_position = p->seq_position;
    slot->lorand = p->lorand; slot->hirand = p->hirand;
    slot->choke_group = p->choke_group; slot->choke_off_by = p->choke_off_by;
    slot->trigger_type = p->trigger_type;
    slot->sw_lokey = p->sw_lokey; slot->sw_hikey = p->sw_hikey; slot->sw_last = p->sw_last;
    slot->rt_decay = p->rt_decay;
    slot->delay_seconds = p->delay_seconds;
    slot->has_own_ampeg = p->has_own_ampeg;
    slot->ampeg_delay = p->ampeg_delay; slot->ampeg_attack = p->ampeg_attack; slot->ampeg_hold = p->ampeg_hold;
    slot->ampeg_decay = p->ampeg_decay; slot->ampeg_sustain = p->ampeg_sustain; slot->ampeg_release = p->ampeg_release;
    slot->ampeg_start = p->ampeg_start;
    slot->note_polyphony = p->note_polyphony;
    slot->velcurve_count = p->velcurve_count;
    for (int i = 0; i < p->velcurve_count && i < SFZ_MAX_VELCURVE_POINTS; i++) {
        slot->velcurve_point[i] = p->velcurve_point[i]; slot->velcurve_gain[i] = p->velcurve_gain[i];
    }
    slot->xfin_lovel = p->xfin_lovel; slot->xfin_hivel = p->xfin_hivel;
    slot->xfout_lovel = p->xfout_lovel; slot->xfout_hivel = p->xfout_hivel;
    slot->xfin_lokey = p->xfin_lokey; slot->xfin_hikey = p->xfin_hikey;
    slot->xfout_lokey = p->xfout_lokey; slot->xfout_hikey = p->xfout_hikey;
    slot->drum_bus = p->drum_bus;
}

// Claims a catalog slot for a brand new zone and marks it active and owned (its file still to be
// loaded). Returns the slot index, or -1 when the Sample Bank is full.
static int claim_catalog_slot_for_new_zone(HdsSampleEngine *se, const ZoneRecordWire *rec) {
    int found_index = -1;
    // Brand new zone -- reuse an earlier REMOVED zone's now-inactive slot if one exists,
    // rather than always appending a new one. Without this, swapping a Multisample
    // oscillator's SFZ over and over (standard sampler behavior: loading a new instrument
    // into a slot replaces the old one, it doesn't pile up alongside it) left every previous
    // load's own SampleSlot entry sitting inactive-but-present in the catalog forever --
    // apply_zone_remove() already frees the big part (pre_roll_l/r) and clears `active`, but
    // sample_slot_count itself only ever grew, so every catalog scan (find_matching_sample_
    // slot, this same reuse scan, round-robin cycling, ...) kept walking an ever-longer tail
    // of dead entries for the rest of the session -- a real, unbounded-with-usage cost despite
    // no individual zone actually leaking its own RAM. A removed slot is always safe to hand
    // to a brand new zone: apply_zone_remove() already leaves it fully reset (loaded=false,
    // pre_roll_l/r=NULL), identical to a never-used BSS-zeroed entry -- see
    // drain_pending_sample_loads()'s own zone_id check for the one place slot-index reuse
    // needed a matching fix (a stale queued load for the OLD zone that used to live at this
    // index must never be allowed to resurrect it as if it were the new zone).
    int reuse_index = -1;
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (!se->slots->sample_slots[s].active) { reuse_index = s; break; }
    }
    if (reuse_index >= 0) {
        found_index = reuse_index;
    } else {
        // Nothing to reuse -- append. sample_slots is a plain static array (zero-initialized
        // BSS), so a never-before-used position already has loaded=false, pre_roll_l/r=NULL,
        // etc.; only zone_id/active/owner_program need setting explicitly here.
        if (se->slots->sample_slot_count >= SAMPLE_SLOT_CAPACITY) {
            fprintf(stderr, "[samples] Sample Bank at its %d-zone capacity -- ignoring zone_id %d\n", SAMPLE_SLOT_CAPACITY, rec->zone_id);
            return -1;
        }
        found_index = se->slots->sample_slot_count++;
    }
    se->slots->sample_slots[found_index].zone_id = rec->zone_id;
    se->slots->sample_slots[found_index].active = true;
    se->slots->sample_slots[found_index].owner_program = clamp_owner_program(rec->owner_program);
    return found_index;
}

// Applies one zone upsert -- unchanged logic from the old wire-parsed version, just fed an
// already-populated ZoneRecordWire (built from OSC args in zone_upsert_handler() below) instead
// of parsing one out of a raw byte buffer.
//
// BUG FIX (the actual one, not an earlier mode-gated attempt that was tried and reverted -- see
// git history/README): every zone used to live in one flat, unpartitioned-by-Program catalog,
// split only by oscillator. Loading/restoring ANY Program's own saved samples meant every OTHER
// Program that happened to share an oscillator index saw its zones silently dragged along too --
// "patches existing" (in the shared catalog) was being treated as a reason to load their samples,
// even for a Program nothing currently referenced. Fixed by owner_program (see SampleSlot's own
// comment): each zone now genuinely belongs to ONE Program, so restoring/loading a Program's own
// saved state only ever touches that Program's own zones. Loading itself stays exactly as
// immediate as it always was -- there's no separate "is this relevant right now" gate here at
// all; a zone loads because its owning Program actually has it assigned, full stop.
void apply_zone_upsert(HdsSampleEngine *se, const ZoneRecordWire *rec) {
    int found_index = -1;
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (se->slots->sample_slots[s].active && se->slots->sample_slots[s].zone_id == rec->zone_id) { found_index = s; break; }
    }

    if (found_index < 0) {
        found_index = claim_catalog_slot_for_new_zone(se, rec);
        if (found_index < 0) return;
        enqueue_pending_sample_load(se, found_index, rec);
        return;
    }

    SampleSlot *slot = &se->slots->sample_slots[found_index];
    if (strncmp(slot->file_path, rec->path, SAMPLE_PATH_LEN) != 0 || !slot->loaded) {
        // Path changed (or a previous load of this same slot never finished/failed) -- (re)queue
        // rather than apply cheap fields in place.
        enqueue_pending_sample_load(se, found_index, rec);
        return;
    }

    // Path unchanged and already loaded: apply the cheap fields immediately, no reload.
    slot->root_note = (int)rec->root_note;
    slot->min_note = (int)rec->min_note; slot->max_note = (int)rec->max_note;
    slot->min_velocity = (int)rec->min_velocity; slot->max_velocity = (int)rec->max_velocity;
    slot->unpitched = rec->unpitched != 0.0f;
    slot->round_robin_group = (int)rec->round_robin_group;
    slot->assigned_osc = clamp_assigned_osc(rec->assigned_osc);
    slot->loop_enabled = rec->loop_enabled != 0.0f;
    apply_loop_points(slot, rec->loop_start, rec->loop_end);
    slot->tune_cents = rec->tune_cents; slot->gain_db = rec->gain_db; slot->pan = rec->pan;
    slot->start_offset_frac = rec->start_offset_frac;
    slot->reverse = rec->reverse != 0.0f;
    slot->loop_release_exit = rec->loop_release_exit != 0.0f;
    apply_playback_end(slot, rec->end_frac);
    // SFZ first-class opcode support -- same field-for-field carry-through as the fresh-load path
    // in drain_pending_sample_loads() above (this in-place branch just skips the reload itself).
    slot->loop_crossfade_frames = rec->loop_crossfade_frames;
    slot->seq_length = rec->seq_length; slot->seq_position = rec->seq_position;
    slot->lorand = rec->lorand; slot->hirand = rec->hirand;
    slot->choke_group = rec->choke_group; slot->choke_off_by = rec->choke_off_by;
    slot->trigger_type = rec->trigger_type;
    slot->sw_lokey = rec->sw_lokey; slot->sw_hikey = rec->sw_hikey; slot->sw_last = rec->sw_last;
    slot->rt_decay = rec->rt_decay;
    slot->delay_seconds = rec->delay_seconds;
    slot->has_own_ampeg = rec->has_own_ampeg != 0;
    slot->ampeg_delay = rec->ampeg_delay; slot->ampeg_attack = rec->ampeg_attack; slot->ampeg_hold = rec->ampeg_hold;
    slot->ampeg_decay = rec->ampeg_decay; slot->ampeg_sustain = rec->ampeg_sustain; slot->ampeg_release = rec->ampeg_release;
    slot->ampeg_start = rec->ampeg_start;
    slot->note_polyphony = rec->note_polyphony;
    slot->velcurve_count = rec->velcurve_count;
    for (int i = 0; i < rec->velcurve_count && i < SFZ_MAX_VELCURVE_POINTS; i++) {
        slot->velcurve_point[i] = rec->velcurve_point[i]; slot->velcurve_gain[i] = rec->velcurve_gain[i];
    }
    slot->xfin_lovel = rec->xfin_lovel; slot->xfin_hivel = rec->xfin_hivel;
    slot->xfout_lovel = rec->xfout_lovel; slot->xfout_hivel = rec->xfout_hivel;
    slot->xfin_lokey = rec->xfin_lokey; slot->xfin_hikey = rec->xfin_hikey;
    slot->xfout_lokey = rec->xfout_lokey; slot->xfout_hikey = rec->xfout_hikey;
    publish_zone_edit_to_streams(se, found_index); // held notes on this zone follow loop/end edits
    // drum_bus deliberately NOT copied here, unlike every field above -- this in-place branch
    // fires for an ordinary "/hds/zone/upsert" (the 19-float wire, zone_record_wire_set_sfz_
    // defaults() always zeroes rec->drum_bus for that path since it's not one of the 19), so
    // copying it here would silently stomp whatever "/hds/zone/drum_bus" (or the owning SFZ's
    // own hds_drum_bus= opcode, on a fresh load -- see drain_pending_sample_loads()'s own copy)
    // most recently set back to 0 the instant a frontend edits ANY other cheap field (Tune/Gain/
    // Pan/Round-Robin/loop points) on the same zone. Leaving this slot field untouched here keeps
    // it exactly what the dedicated non-wire assignment last set, same "stays off the wire"
    // posture as SampleSlot.drum_bus's own comment describes.
}

void apply_zone_remove(HdsSampleEngine *se, int32_t zone_id) {
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (se->slots->sample_slots[s].active && se->slots->sample_slots[s].zone_id == zone_id) {
            se->slots->sample_slots[s].active = false;
            se->slots->sample_slots[s].loaded = false; // matches the pre-existing "empty path" unload behavior
            // Handed to the loader thread to free (never free() on the audio thread).
            retire_buffer(se, se->slots->sample_slots[s].pre_roll_l); se->slots->sample_slots[s].pre_roll_l = NULL;
            retire_buffer(se, se->slots->sample_slots[s].pre_roll_r); se->slots->sample_slots[s].pre_roll_r = NULL;
            break;
        }
    }
}

// Removes every zone owned by ONE (oscillator, Program) pair -- shared by hds_load_sfz_for_osc()
// (loading a new SFZ onto an oscillator always replaces whatever that same oscillator/Program had
// before) and poll_vco_shape_unload() below (switching an oscillator's own Shape away from
// WAVE_SAMPLE entirely). Scoped by owner_program too (not just osc_index) so clearing one Program's
// own VCO1 zones never touches a DIFFERENT Program's own VCO1 zones that happen to share the same
// oscillator index -- see SampleSlot.owner_program's own comment. Collects zone_ids first (a plain
// forward scan while tombstoning in place is safe here too, same as apply_zone_remove's own loop,
// but collecting first keeps this obviously correct regardless of how apply_zone_remove's own loop
// shape might change later) -- a no-op scan over an already-empty catalog, the ordinary case.
static void unload_zones_for_osc(HdsSampleEngine *se, int osc_index, int owner_program) {
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        SampleSlot *slot = &se->slots->sample_slots[s];
        if (slot->active && slot->assigned_osc == osc_index && slot->owner_program == owner_program) apply_zone_remove(se, slot->zone_id);
    }
}

// "/hds/zone/upsert" i + 19 f's + s -- zone_id + 19 floats in ZoneRecordWire's own field order
// (root_note, min_note, max_note, min_velocity, max_velocity, unpitched, loop_start, loop_end,
// loop_enabled, round_robin_group, tune_cents, gain_db, pan, start_offset_frac, reverse,
// loop_release_exit, end_frac, assigned_osc, owner_program) + path. liblo only dispatches here
// when the incoming type string matches exactly, so argc/types are already guaranteed correct --
// no manual validation needed, unlike generic_osc_handler's table.
int zone_upsert_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    ZoneRecordWire rec;
    zone_record_wire_set_sfz_defaults(&rec); // this path never carries SFZ opcodes -- see that function's own comment
    rec.zone_id = argv[0]->i;
    float *f = &rec.root_note; // root_note..owner_program are 19 consecutive floats
    for (int i = 0; i < 19; i++) f[i] = argv[1 + i]->f;
    snprintf(rec.path, SAMPLE_PATH_LEN, "%s", (const char *)&argv[20]->s);
    apply_zone_upsert(se, &rec);
    return 0;
}
// "/hds/zone/remove" i -- a single zone_id.
int zone_remove_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    apply_zone_remove(se, argv[0]->i);
    return 0;
}
// "/hds/zone/drum_bus" i i -- zone_id, bus (0 = off, 1..DRUM_BUS_COUNT) -- see SampleSlot.drum_bus's
// own comment. A dedicated, minimal handler rather than another ZoneRecordWire field threaded
// through the wire's own 19-float upsert message: this is an HDS-specific extension, not part of
// the Mapping-tab wire protocol, and it's a cheap in-place field update on an already-loaded zone
// (like apply_zone_upsert's own "path unchanged" branch), never a reason to reload the sample
// itself. Silently a no-op if zone_id isn't currently active (e.g. a stale message after removal).
int zone_drum_bus_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    int32_t zone_id = argv[0]->i;
    int bus = argv[1]->i;
    if (bus < 0) bus = 0;
    if (bus > DRUM_BUS_COUNT) bus = DRUM_BUS_COUNT;
    for (int s = 0; s < se->slots->sample_slot_count; s++) {
        if (se->slots->sample_slots[s].active && se->slots->sample_slots[s].zone_id == zone_id) {
            se->slots->sample_slots[s].drum_bus = bus;
            break;
        }
    }
    return 0;
}

// Backend-native SFZ loading -- lets a whole multisample instrument be loaded directly from a
// local .sfz file, for one oscillator (0-3), without any frontend ever deriving/pushing zone
// data itself. The shared core both the CLI --sfz-vcoN startup flags and the /hds/zone/load_sfz
// OSC method call (see main() and zone_load_sfz_handler, further down) -- they differ only in
// WHEN/HOW this gets triggered, not in what it does once triggered. Reuses apply_zone_upsert()
// (the exact same function an OSC-driven zone upsert already calls) for every parsed region, so
// DFD streaming/pre-roll setup happens through the identical, already-proven throttled-load path
// either way -- one code path, not two to keep in sync.
static _Atomic int32_t g_next_native_zone_id = -1; // decrements per native-loaded zone (atomic: every plugin
    // instance's loader thread draws from it); frontend-assigned
    // zone_ids are always >= 1 (SamplerMappingGrid._next_zone_id only ever increments from 1), so
    // this negative space can never collide with one.

// g_sfz_parse_scratch is one process-wide buffer; every loader thread (one per plugin instance)
// and the in-thread fallback path take turns with it.
static pthread_mutex_t g_sfz_parse_lock = PTHREAD_MUTEX_INITIALIZER;

// Parses `sfz_path` into one zone record per playable region, each with a fresh native zone_id,
// for oscillator `osc_index` of Program `owner_program`. With `prepare`, also prepares every
// region's file (prepared[i]/ok[i]); its header then doubles as the frame count the SFZ's frame
// opcodes are converted with. Touches no engine state: runs on the loader thread. Returns the
// record count; the three arrays are malloc'd (NULL when the count is 0).
static int sfz_build_records(const char *sfz_path, int osc_index, int owner_program, bool prepare,
                             ZoneRecordWire **out_records, HdsPreparedSample **out_prepared, bool **out_ok, int *out_sw_default,
                             HdsSampleLoaderState *handoff) {
    *out_records = NULL; if (out_prepared) *out_prepared = NULL; if (out_ok) *out_ok = NULL;
    *out_sw_default = -1;
    pthread_mutex_lock(&g_sfz_parse_lock);
    char default_path[SAMPLE_PATH_LEN];
    int region_count = sfz_parse_file(sfz_path, g_sfz_parse_scratch, SFZ_MAX_REGIONS_PER_FILE, default_path, sizeof(default_path));
    if (region_count == 0) {
        pthread_mutex_unlock(&g_sfz_parse_lock);
        fprintf(stderr, "[sfz] '%s': no regions parsed (osc %d)\n", sfz_path, osc_index + 1);
        return 0;
    }
    ZoneRecordWire *records = (ZoneRecordWire *)malloc(sizeof(ZoneRecordWire) * (size_t)region_count);
    HdsPreparedSample *prepared = prepare ? (HdsPreparedSample *)calloc((size_t)region_count, sizeof(HdsPreparedSample)) : NULL;
    bool *ok = prepare ? (bool *)calloc((size_t)region_count, sizeof(bool)) : NULL;
    if (!records || (prepare && (!prepared || !ok))) {
        pthread_mutex_unlock(&g_sfz_parse_lock);
        free(records); free(prepared); free(ok);
        fprintf(stderr, "[sfz] '%s': out of memory\n", sfz_path);
        return 0;
    }

    // seq_length=-based round-robin bucketing, local to this one load, DECOUPLED from group=
    // entirely (see SfzRegion.has_group's own comment on why bucketing by group used to be wrong:
    // real SFZ content routinely uses the seq_length=/seq_position= idiom with no group= opcode at
    // all, which the old code silently never cycled). A "bucket" is just every region in this file
    // sharing the exact same (lokey,hikey,lovel,hivel) match range AND seq_length>0 -- the way
    // round-robin variants are actually authored in practice (same trigger range, different
    // sample=). seq_position (see below) then orders cycling within a bucket explicitly.
    typedef struct { int lokey, hikey, minvel, maxvel, bucket_id; } SfzSeqBucket;
    #define SFZ_MAX_SEQ_BUCKETS 4096
    SfzSeqBucket *seq_buckets = (SfzSeqBucket *)malloc(sizeof(SfzSeqBucket) * SFZ_MAX_SEQ_BUCKETS);
    int seq_bucket_count = 0;

    // sw_default=: the keyswitch state a (Program, oscillator) pair starts in before any real
    // keyswitch note has ever been pressed -- see HdsPendingLoadState.keyswitch_note_plus1's own
    // comment. Applied once, after the region loop below, from the first region that sets it
    // (matches every real keyswitch library's own convention of declaring this once, control-
    // block-wide, even though HDS tracks it per-region like any other inherited opcode).
    bool have_sw_default = false;
    int sw_default_note = -1;

    int queued = 0;
    for (int i = 0; i < region_count; i++) {
        SfzRegion *r = &g_sfz_parse_scratch[i];
        if (!r->has_sample || !r->sample[0]) continue; // no sample= at all -- nothing to play

        const char *lokey_str = r->has_lokey ? r->lokey : (r->has_key ? r->key : NULL);
        const char *hikey_str = r->has_hikey ? r->hikey : (r->has_key ? r->key : NULL);
        int lokey = lokey_str ? sfz_note_to_number(lokey_str) : 0; if (lokey < 0) lokey = 0;
        int hikey = hikey_str ? sfz_note_to_number(hikey_str) : 127; if (hikey < 0) hikey = 127;

        bool is_drum = r->has_pitch_keytrack && r->pitch_keytrack == 0.0f;
        int key_center = lokey;
        if (!is_drum) {
            const char *keycenter_str = r->has_pitch_keycenter ? r->pitch_keycenter : lokey_str;
            if (keycenter_str) { int kc = sfz_note_to_number(keycenter_str); if (kc >= 0) key_center = kc; }
        }

        int min_vel = r->has_lovel ? r->lovel : 0; if (min_vel < 0) min_vel = 0; if (min_vel > 127) min_vel = 127;
        int max_vel = r->has_hivel ? r->hivel : 127; if (max_vel < 0) max_vel = 0; if (max_vel > 127) max_vel = 127;

        bool loop_enabled = false, loop_release_exit = false;
        if (r->has_loop_mode) {
            if (strcasecmp(r->loop_mode, "loop_sustain") == 0) { loop_enabled = true; loop_release_exit = true; }
            else if (strcasecmp(r->loop_mode, "loop_continuous") == 0) { loop_enabled = true; }
            // "no_loop"/"one_shot"/anything else: loop_enabled stays false.
        }

        unsigned int total_frames = 0, sample_rate = 44100;
        if (prepare) {
            ok[queued] = prepare_sample_file(r->sample, &prepared[queued], handoff);
            if (ok[queued]) { total_frames = prepared[queued].total_frames; sample_rate = prepared[queued].sample_rate_hz; }
        } else {
            read_wav_frame_count(r->sample, &total_frames, &sample_rate); // best-effort -- 0/44100 leaves every frac/frame count below at its safe default
        }

        float loop_start_frac = 0.0f, loop_end_frac = 1.0f, offset_frac = 0.0f, end_frac = 1.0f;
        if (r->has_loop_start) sfz_frame_to_frac(r->loop_start, total_frames, &loop_start_frac);
        if (r->has_loop_end) sfz_frame_to_frac(r->loop_end, total_frames, &loop_end_frac);
        if (r->has_offset) sfz_frame_to_frac(r->offset, total_frames, &offset_frac);
        if (r->has_end) sfz_frame_to_frac(r->end, total_frames, &end_frac);

        int round_robin_group = 0, seq_position = 0;
        if (r->has_seq_length && r->seq_length > 0) {
            int found = -1;
            for (int b = 0; b < seq_bucket_count; b++) {
                if (seq_buckets[b].lokey == lokey && seq_buckets[b].hikey == hikey &&
                    seq_buckets[b].minvel == min_vel && seq_buckets[b].maxvel == max_vel) { found = b; break; }
            }
            if (found < 0 && seq_bucket_count < SFZ_MAX_SEQ_BUCKETS) {
                found = seq_bucket_count;
                seq_buckets[found].lokey = lokey; seq_buckets[found].hikey = hikey;
                seq_buckets[found].minvel = min_vel; seq_buckets[found].maxvel = max_vel;
                seq_buckets[found].bucket_id = seq_bucket_count + 1;
                seq_bucket_count++;
            }
            if (found >= 0) round_robin_group = seq_buckets[found].bucket_id;
            seq_position = r->has_seq_position ? r->seq_position : 0;
        }

        // group=/off_by=: the REAL SFZ choke-group opcodes now, completely independent of the
        // round-robin bucketing above -- passed through as their raw SFZ ids (0 = none), see
        // apply_choke_group()'s own comment for how they're actually enforced at note-on.
        int choke_group = r->has_group ? r->group : 0; if (choke_group < 0) choke_group = 0;
        int choke_off_by = r->has_off_by ? r->off_by : 0; if (choke_off_by < 0) choke_off_by = 0;

        int trigger_type = HDS_SFZ_TRIGGER_ATTACK;
        if (r->has_trigger) {
            if (strcasecmp(r->trigger, "release") == 0) trigger_type = HDS_SFZ_TRIGGER_RELEASE;
            else if (strcasecmp(r->trigger, "release_key") == 0) trigger_type = HDS_SFZ_TRIGGER_RELEASE_KEY;
            else if (strcasecmp(r->trigger, "first") == 0) trigger_type = HDS_SFZ_TRIGGER_FIRST;
            else if (strcasecmp(r->trigger, "legato") == 0) trigger_type = HDS_SFZ_TRIGGER_LEGATO;
            // "attack"/anything else: stays HDS_SFZ_TRIGGER_ATTACK.
        }

        int sw_lokey = r->has_sw_lokey ? sfz_note_to_number(r->sw_lokey) : -1;
        int sw_hikey = r->has_sw_hikey ? sfz_note_to_number(r->sw_hikey) : -1;
        int sw_last = r->has_sw_last ? sfz_note_to_number(r->sw_last) : -1;
        if (!have_sw_default && r->has_sw_default) {
            int sd = sfz_note_to_number(r->sw_default);
            if (sd >= 0) { sw_default_note = sd; have_sw_default = true; }
        }

        bool has_own_ampeg = r->has_ampeg_delay || r->has_ampeg_attack || r->has_ampeg_hold ||
                              r->has_ampeg_decay || r->has_ampeg_sustain || r->has_ampeg_release || r->has_ampeg_start;

        int loop_crossfade_frames = r->has_loop_crossfade ? (int)(r->loop_crossfade * (float)sample_rate + 0.5f) : -1;
        if (loop_crossfade_frames < 0 && r->has_loop_crossfade) loop_crossfade_frames = 0;

        ZoneRecordWire rec;
        zone_record_wire_set_sfz_defaults(&rec); // every field this region doesn't explicitly set below keeps its "feature off" default
        rec.zone_id = atomic_fetch_sub_explicit(&g_next_native_zone_id, 1, memory_order_relaxed);
        rec.root_note = is_drum ? 60.0f : (float)key_center;
        rec.min_note = (float)lokey; rec.max_note = (float)hikey;
        rec.min_velocity = (float)min_vel; rec.max_velocity = (float)max_vel;
        rec.unpitched = is_drum ? 1.0f : 0.0f;
        rec.loop_start = loop_start_frac; rec.loop_end = loop_end_frac;
        rec.loop_enabled = loop_enabled ? 1.0f : 0.0f;
        rec.round_robin_group = (float)round_robin_group;
        rec.tune_cents = r->has_tune ? r->tune : 0.0f;
        rec.gain_db = r->has_volume ? r->volume : 0.0f;
        rec.pan = (r->has_pan ? r->pan : 0.0f) / 100.0f;
        rec.start_offset_frac = offset_frac;
        rec.reverse = (r->has_direction && strcasecmp(r->direction, "reverse") == 0) ? 1.0f : 0.0f;
        rec.loop_release_exit = loop_release_exit ? 1.0f : 0.0f;
        rec.end_frac = end_frac;
        rec.assigned_osc = (float)osc_index;
        rec.owner_program = (float)owner_program;
        snprintf(rec.path, SAMPLE_PATH_LEN, "%s", r->sample);

        rec.loop_crossfade_frames = loop_crossfade_frames;
        rec.seq_length = r->has_seq_length ? r->seq_length : 0;
        rec.seq_position = seq_position;
        rec.lorand = r->has_lorand ? r->lorand : 0.0f;
        rec.hirand = r->has_hirand ? r->hirand : 1.0f;
        rec.choke_group = choke_group; rec.choke_off_by = choke_off_by;
        rec.trigger_type = trigger_type;
        rec.sw_lokey = sw_lokey; rec.sw_hikey = sw_hikey; rec.sw_last = sw_last;
        rec.rt_decay = r->has_rt_decay ? r->rt_decay : 0.0f;
        rec.delay_seconds = r->has_delay ? r->delay : 0.0f;
        rec.has_own_ampeg = has_own_ampeg ? 1 : 0;
        rec.ampeg_delay = r->has_ampeg_delay ? r->ampeg_delay : 0.0f;
        rec.ampeg_attack = r->has_ampeg_attack ? r->ampeg_attack : 0.0f;
        rec.ampeg_hold = r->has_ampeg_hold ? r->ampeg_hold : 0.0f;
        rec.ampeg_decay = r->has_ampeg_decay ? r->ampeg_decay : 0.0f;
        rec.ampeg_sustain = r->has_ampeg_sustain ? r->ampeg_sustain : 100.0f;
        rec.ampeg_release = r->has_ampeg_release ? r->ampeg_release : 0.0f;
        rec.ampeg_start = r->has_ampeg_start ? r->ampeg_start : 0.0f;
        rec.note_polyphony = r->has_note_polyphony ? r->note_polyphony : 0;
        rec.velcurve_count = r->velcurve_count;
        for (int vc = 0; vc < r->velcurve_count && vc < SFZ_MAX_VELCURVE_POINTS; vc++) {
            rec.velcurve_point[vc] = r->velcurve_point[vc]; rec.velcurve_gain[vc] = r->velcurve_gain[vc];
        }
        rec.xfin_lovel = r->has_xfin_lovel ? r->xfin_lovel : -1;
        rec.xfin_hivel = r->has_xfin_hivel ? r->xfin_hivel : -1;
        rec.xfout_lovel = r->has_xfout_lovel ? r->xfout_lovel : -1;
        rec.xfout_hivel = r->has_xfout_hivel ? r->xfout_hivel : -1;
        rec.xfin_lokey = r->has_xfin_lokey ? sfz_note_to_number(r->xfin_lokey) : -1;
        rec.xfin_hikey = r->has_xfin_hikey ? sfz_note_to_number(r->xfin_hikey) : -1;
        rec.xfout_lokey = r->has_xfout_lokey ? sfz_note_to_number(r->xfout_lokey) : -1;
        rec.xfout_hikey = r->has_xfout_hikey ? sfz_note_to_number(r->xfout_hikey) : -1;
        rec.drum_bus = 0;
        if (r->has_drum_bus) {
            rec.drum_bus = r->drum_bus;
            if (rec.drum_bus < 0) rec.drum_bus = 0;
            if (rec.drum_bus > DRUM_BUS_COUNT) rec.drum_bus = DRUM_BUS_COUNT;
        }

        records[queued++] = rec;
    }
    pthread_mutex_unlock(&g_sfz_parse_lock);
    free(seq_buckets);
    #undef SFZ_MAX_SEQ_BUCKETS
    if (have_sw_default) *out_sw_default = sw_default_note;
    if (queued == 0) { free(records); free(prepared); free(ok); return 0; }
    *out_records = records;
    if (out_prepared) *out_prepared = prepared;
    if (out_ok) *out_ok = ok;
    return queued;
}

// sw_default= (if the file set one anywhere) becomes this (Program, oscillator) pair's starting
// keyswitch state -- a fresh load always resets it, matching "loading a new instrument replaces
// the old one's state entirely."
static void apply_sfz_sw_default(HdsSampleEngine *se, int osc_index, int owner_program, int sw_default_note) {
    if (sw_default_note >= 0 && owner_program <= SINGLE_PROGRAM_SLOT && osc_index >= 0 && osc_index < 4) {
        se->pending->keyswitch_note_plus1[owner_program][osc_index] = sw_default_note + 1;
    }
}

static bool hds_sample_loader_submit(HdsSampleEngine *se, const HdsLoadJob *job);

// Loads an .sfz onto one oscillator of one Program, replacing whatever zones that pair had. With
// the loader thread running, this only submits a job and returns 0 (callable from the audio
// thread and the GUI thread alike): the parse, every region's file and the catalog update happen
// later -- see drain_pending_sample_loads(). Without it (startup before the threads exist, tests,
// Android), it parses here and queues each region's load the old way, returning the zone count.
int hds_load_sfz_for_osc(HdsSampleEngine *se, int osc_index, int owner_program, const char *sfz_path) {
    if (osc_index < 0 || osc_index > 3) return 0;
    owner_program = clamp_owner_program((float)owner_program);

    if (atomic_load_explicit(&se->pending->loader.running, memory_order_acquire)) {
        HdsLoadJob job;
        memset(&job, 0, sizeof(job));
        job.kind = HDS_LOAD_JOB_SFZ;
        job.osc_index = osc_index;
        job.owner_program = owner_program;
        snprintf(job.sfz_path, sizeof(job.sfz_path), "%s", sfz_path);
        if (!hds_sample_loader_submit(se, &job)) fprintf(stderr, "[sfz] loader queue full -- '%s' not loaded (retry)\n", sfz_path);
        return 0;
    }

    // Always "replace THIS PROGRAM's own zone set on this oscillator" -- a no-op at empty-catalog
    // startup time.
    unload_zones_for_osc(se, osc_index, owner_program);
    ZoneRecordWire *records;
    int sw_default_note;
    int n = sfz_build_records(sfz_path, osc_index, owner_program, false, &records, NULL, NULL, &sw_default_note, NULL);
    for (int i = 0; i < n; i++) apply_zone_upsert(se, &records[i]);
    free(records);
    apply_sfz_sw_default(se, osc_index, owner_program, sw_default_note);
    return n;
}

// Detects an oscillator's own Shape knob (prog->vco{1-4}_shape) moving AWAY from WAVE_SAMPLE and
// unloads whatever Multisample/SFZ zones that (oscillator, Program) pair still owns when it does --
// a plain continuous knob has no "on change" event of its own (unlike /hds/zone/load_sfz, a real
// discrete OSC method), so this polls once per block from the same non-realtime context that
// already does this exact kind of work (drain_pending_sample_loads()'s own malloc/free calls,
// called from main()'s loop in the standalone build and from process() in clap_plugin.c -- see
// both call sites, always right next to drain_pending_sample_loads() itself). Without this, an
// oscillator switched away from Multisample kept its whole catalog (RAM pre-roll buffers and all)
// sitting there loaded but unused -- harmless to playback (find_matching_sample_slot() is only ever
// consulted for an oscillator whose CURRENT Shape is WAVE_SAMPLE) but a real leak of both RAM and,
// worse, of STALE STATE: it's exactly what let a trigger=release zone from the OLD Multisample
// still fire on note-off after switching to a plain waveform (see trigger_voice_release()'s own
// fix for the immediate symptom -- this is the same bug's other half, cleaning up the cause instead
// of only guarding the effect). se->pending->vco_shape_last[p][o] uses -1 (never a real WaveType)
// as "not yet observed" so the very first poll after startup/a fresh instance never unloads
// anything -- see HdsPendingLoadState.vco_shape_last's own comment.
void poll_vco_shape_unload(HdsSampleEngine *se, const SynthSharedControls *knobs) {
    if (!se->pending->vco_shape_last_init) {
        for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) for (int o = 0; o < 4; o++) se->pending->vco_shape_last[p][o] = -1.0f;
        se->pending->vco_shape_last_init = true;
    }
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) {
        const ProgramState *prog = &knobs->programs[p];
        const float shape_now[4] = { prog->vco1_shape, prog->vco2_shape, prog->vco3_shape, prog->vco4_shape };
        for (int o = 0; o < 4; o++) {
            float prev = se->pending->vco_shape_last[p][o];
            if (prev >= 0.0f && (int)prev == WAVE_SAMPLE && (int)shape_now[o] != WAVE_SAMPLE) {
                unload_zones_for_osc(se, o, p);
            }
            se->pending->vco_shape_last[p][o] = shape_now[o];
        }
    }
}

// "/hds/zone/load_sfz" i i s -- oscillator index (0-3), owner Program index (0-7 a Combi lane,
// SINGLE_PROGRAM_SLOT a Single Program), and a path, valid on the BACKEND's own filesystem (never
// a payload the frontend decoded itself -- see this pass's own design rationale: a backend on
// another system has its samples with it locally, so only a path needs to cross the wire). Reuses
// the exact same hds_load_sfz_for_osc() the CLI --sfz-vcoN flags call.
int zone_load_sfz_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsSampleEngine *se = (HdsSampleEngine *)user_data;
    int osc_index = argv[0]->i;
    int owner_program = argv[1]->i;
    const char *sfz_path = (const char *)&argv[2]->s;
    bool async = atomic_load_explicit(&se->pending->loader.running, memory_order_acquire);
    int n = hds_load_sfz_for_osc(se, osc_index, owner_program, sfz_path);
    if (async) fprintf(stderr, "[sfz] /hds/zone/load_sfz osc=%d program=%d '%s': handed to the loader thread\n", osc_index + 1, owner_program, sfz_path);
    else fprintf(stderr, "[sfz] /hds/zone/load_sfz osc=%d program=%d '%s': %d zone(s) queued\n", osc_index + 1, owner_program, sfz_path, n);
    return 0;
}

// --- Sample loader thread ---
//
// Everything a zone or SFZ load reads from disk happens here, off the audio thread: the WAV
// header and RAM pre-roll decode (prepare_sample_file), and for an SFZ the .sfz text plus every
// region's file (sfz_build_records). The audio thread only moves small jobs and results through
// lock-free queues and installs finished results into the catalog (install_prepared_sample: a
// pointer swap), a bounded amount per block. Buffers the audio thread replaces or removes are
// handed back on the garbage queue and freed here, so the audio thread never calls free() either.
// Before this, every load ran on the audio thread (drain_pending_sample_loads, one file per
// block), and an SFZ parsed and read every region's header there in one go -- a slow disk or a
// network mount stalled audio for as long as the file took.
//
// Queues: a bounded multi-producer/multi-consumer ring (Dmitry Vyukov's well-known design: one
// sequence number per cell, so a push or pop is one CAS and never waits on another thread).

static void mpmc_init(HdsMpmcQueue *q, _Atomic size_t *seq, void *cells, size_t elem_size, size_t capacity) {
    q->seq = seq;
    q->data = (unsigned char *)cells;
    q->elem_size = elem_size;
    q->mask = capacity - 1;
    for (size_t i = 0; i < capacity; i++) atomic_store_explicit(&seq[i], i, memory_order_relaxed);
    atomic_store_explicit(&q->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->dequeue_pos, 0, memory_order_relaxed);
}

static bool mpmc_push(HdsMpmcQueue *q, const void *elem) {
    size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    for (;;) {
        size_t seq = atomic_load_explicit(&q->seq[pos & q->mask], memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) break;
        } else if (diff < 0) {
            return false; // full
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }
    memcpy(q->data + (pos & q->mask) * q->elem_size, elem, q->elem_size);
    atomic_store_explicit(&q->seq[pos & q->mask], pos + 1, memory_order_release);
    return true;
}

static bool mpmc_pop(HdsMpmcQueue *q, void *out) {
    size_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
    for (;;) {
        size_t seq = atomic_load_explicit(&q->seq[pos & q->mask], memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) break;
        } else if (diff < 0) {
            return false; // empty
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }
    memcpy(out, q->data + (pos & q->mask) * q->elem_size, q->elem_size);
    atomic_store_explicit(&q->seq[pos & q->mask], pos + q->mask + 1, memory_order_release);
    return true;
}

// Frees `ptr` without doing it on the calling thread when the loader exists: the pointer goes on
// the garbage queue. Only if that queue is full (or there is no loader at all) is it freed here.
static void retire_buffer(HdsSampleEngine *se, void *ptr) {
    if (!ptr) return;
    HdsSampleLoaderState *ld = &se->pending->loader;
    if (ld->initialized) {
        if (mpmc_push(&ld->garbage, &ptr)) return;
        atomic_fetch_add_explicit(&ld->inline_frees, 1, memory_order_relaxed);
    }
    free(ptr);
}

static void retire_prepared_sample(HdsSampleEngine *se, HdsPreparedSample *ps) {
    retire_buffer(se, ps->pre_roll_l);
    retire_buffer(se, ps->pre_roll_r);
    ps->pre_roll_l = ps->pre_roll_r = NULL;
}

static bool hds_sample_loader_submit(HdsSampleEngine *se, const HdsLoadJob *job) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    if (!atomic_load_explicit(&ld->running, memory_order_acquire)) return false;
    return mpmc_push(&ld->requests, job);
}

// Frees everything a result owns (loader thread at shutdown, or teardown).
static void free_load_result(HdsLoadResult *r) {
    if (r->kind == HDS_LOAD_JOB_SAMPLE) {
        free_prepared_sample(&r->prepared);
    } else if (r->kind == HDS_LOAD_JOB_SFZ) {
        for (int i = 0; r->prepared_list && i < r->count; i++) free_prepared_sample(&r->prepared_list[i]);
        free(r->records); free(r->prepared_list); free(r->prepared_ok);
        r->records = NULL; r->prepared_list = NULL; r->prepared_ok = NULL;
    }
}

static void loader_free_garbage(HdsSampleLoaderState *ld) {
    void *ptr;
    while (mpmc_pop(&ld->garbage, &ptr)) free(ptr);
}

static void *sample_loader_thread_main(void *arg) {
    HdsSampleEngine *se = (HdsSampleEngine *)arg;
    HdsSampleLoaderState *ld = &se->pending->loader;
    HdsLoadJob job;
    HdsLoadResult res;
    while (!atomic_load_explicit(&ld->shutdown, memory_order_acquire)) {
        loader_free_garbage(ld);
        if (!mpmc_pop(&ld->requests, &job)) { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); continue; }
        memset(&res, 0, sizeof(res));
        res.kind = job.kind;
        if (job.kind == HDS_LOAD_JOB_SAMPLE) {
            res.sample = job.sample;
            res.ok = prepare_sample_file(job.sample.path, &res.prepared, ld);
        } else {
            res.osc_index = job.osc_index;
            res.owner_program = job.owner_program;
            res.count = sfz_build_records(job.sfz_path, job.osc_index, job.owner_program, true,
                                          &res.records, &res.prepared_list, &res.prepared_ok, &res.sw_default_note, ld);
            fprintf(stderr, "[sfz] '%s': %d zone(s) prepared (osc %d)\n", job.sfz_path, res.count, job.osc_index + 1);
        }
        // The results queue only fills if the audio thread has stopped draining it.
        while (!mpmc_push(&ld->results, &res)) {
            if (atomic_load_explicit(&ld->shutdown, memory_order_acquire)) {
                fprintf(stderr, "[samples] loader stopping with its results queue full -- a load was dropped\n");
                free_load_result(&res);
                break;
            }
            loader_free_garbage(ld);
            struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL);
        }
    }
    loader_free_garbage(ld);
    return NULL;
}

// Starts the loader thread (CLAP activate(), standalone startup). Queued jobs and unapplied
// results survive a stop/start cycle: the queues are set up once, on the first start.
bool hds_sample_loader_start(HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    if (atomic_load_explicit(&ld->running, memory_order_acquire)) return true;
    if (!ld->initialized) {
        mpmc_init(&ld->requests, ld->request_seq, ld->request_cells, sizeof(HdsLoadJob), HDS_LOADER_QUEUE_CAPACITY);
        mpmc_init(&ld->results, ld->result_seq, ld->result_cells, sizeof(HdsLoadResult), HDS_LOADER_QUEUE_CAPACITY);
        mpmc_init(&ld->garbage, ld->garbage_seq, ld->garbage_cells, sizeof(void *), HDS_LOADER_GARBAGE_CAPACITY);
        mpmc_init(&ld->opened_files, ld->opened_file_seq, ld->opened_file_cells, sizeof(HdsOpenedFile), HDS_OPENED_FILE_CAPACITY);
        atomic_store_explicit(&ld->opened_files_ready, true, memory_order_release);
        ld->initialized = true;
    }
    atomic_store_explicit(&ld->shutdown, false, memory_order_release);
    if (pthread_create(&ld->thread, NULL, sample_loader_thread_main, se) != 0) {
        fprintf(stderr, "[samples] failed to start the sample loader thread -- loading on the audio thread instead\n");
        return false;
    }
    atomic_store_explicit(&ld->running, true, memory_order_release);
    return true;
}

// Stops the loader thread (CLAP deactivate(), standalone shutdown). Returns once it has exited;
// a load it is in the middle of finishes first.
void hds_sample_loader_stop(HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    if (!atomic_load_explicit(&ld->running, memory_order_acquire)) return;
    atomic_store_explicit(&ld->running, false, memory_order_release);
    atomic_store_explicit(&ld->shutdown, true, memory_order_release);
    pthread_join(ld->thread, NULL);
}

// Frees whatever the loader's queues still hold (CLAP destroy(), after the last deactivate()).
void hds_sample_loader_free(HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    hds_sample_loader_stop(se);
    if (!ld->initialized) return;
    HdsLoadResult r;
    while (mpmc_pop(&ld->results, &r)) free_load_result(&r);
    HdsLoadJob j;
    while (mpmc_pop(&ld->requests, &j)) {}
    if (ld->sfz_applying) { free_load_result(&ld->sfz_current); ld->sfz_applying = false; }
    loader_free_garbage(ld);
    // Files the disk thread never took (it is stopped by now: CLAP deactivate(), shutdown).
    HdsOpenedFile of;
    while (mpmc_pop(&ld->opened_files, &of)) fclose(of.f);
}

// Audio thread: one finished zone load into the catalog.
static void apply_sample_result(HdsSampleEngine *se, HdsLoadResult *r) {
    PendingSampleLoad *p = &r->sample;
    if (!pending_load_still_wanted(se, p)) { retire_prepared_sample(se, &r->prepared); return; }
    if (!r->ok) { se->slots->sample_slots[p->slot_index].active = false; return; } // prepare_sample_file logged why
    install_prepared_sample(se, p->slot_index, p->path, p->root_note, &r->prepared);
    apply_pending_fields(&se->slots->sample_slots[p->slot_index], p);
}

// Audio thread: an SFZ result replaces its (oscillator, Program) pair's zones at once, then its
// regions go in SFZ_REGIONS_PER_BLOCK per block (sfz_apply_step).
static void sfz_apply_begin(HdsSampleEngine *se, const HdsLoadResult *r) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    unload_zones_for_osc(se, r->osc_index, r->owner_program);
    ld->sfz_current = *r;
    ld->sfz_cursor = 0;
    ld->sfz_applying = true;
}

static void sfz_apply_step(HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    HdsLoadResult *r = &ld->sfz_current;
    int end = ld->sfz_cursor + SFZ_REGIONS_PER_BLOCK;
    if (end > r->count) end = r->count;
    for (; ld->sfz_cursor < end; ld->sfz_cursor++) {
        int i = ld->sfz_cursor;
        if (!r->prepared_ok[i]) continue; // its file didn't load; prepare_sample_file logged why
        int idx = claim_catalog_slot_for_new_zone(se, &r->records[i]);
        if (idx < 0) { retire_prepared_sample(se, &r->prepared_list[i]); continue; }
        PendingSampleLoad p;
        fill_pending_from_record(&p, idx, &r->records[i]);
        install_prepared_sample(se, idx, p.path, p.root_note, &r->prepared_list[i]);
        apply_pending_fields(&se->slots->sample_slots[idx], &p);
    }
    if (ld->sfz_cursor >= r->count) {
        apply_sfz_sw_default(se, r->osc_index, r->owner_program, r->sw_default_note);
        retire_buffer(se, r->records);
        retire_buffer(se, r->prepared_list);
        retire_buffer(se, r->prepared_ok);
        ld->sfz_applying = false;
    }
}

// Called once per audio block (standalone main loop, CLAP process()). With the loader thread:
// applies finished results (a bounded number of pointer swaps -- one SFZ step, or up to
// SAMPLE_RESULTS_PER_BLOCK zone loads), then hands queued zone loads to the loader; no file I/O,
// no malloc/free. Without it, falls back to loading SAMPLE_LOADS_PER_BLOCK files here.
void drain_pending_sample_loads(HdsSampleEngine *se) {
    HdsSampleLoaderState *ld = &se->pending->loader;
    HdsPendingLoadState *pend = se->pending;

    if (ld->initialized) {
        if (ld->sfz_applying) {
            sfz_apply_step(se);
        } else {
            HdsLoadResult r;
            for (int n = 0; n < SAMPLE_RESULTS_PER_BLOCK && mpmc_pop(&ld->results, &r); n++) {
                if (r.kind == HDS_LOAD_JOB_SAMPLE) {
                    apply_sample_result(se, &r);
                } else {
                    sfz_apply_begin(se, &r);
                    sfz_apply_step(se);
                    break; // results after an SFZ wait until it is fully in, so order is kept
                }
            }
        }
    }

    if (atomic_load_explicit(&ld->running, memory_order_acquire)) {
        while (pend->pending_load_count > 0) {
            PendingSampleLoad *p = &pend->pending_loads[pend->pending_load_head];
            if (pending_load_still_wanted(se, p)) {
                HdsLoadJob job;
                job.kind = HDS_LOAD_JOB_SAMPLE;
                job.sample = *p;
                if (!mpmc_push(&ld->requests, &job)) break; // full: the rest go next block
            }
            pend->pending_load_head = (pend->pending_load_head + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
            pend->pending_load_count--;
        }
        return;
    }

    for (int i = 0; i < SAMPLE_LOADS_PER_BLOCK && pend->pending_load_count > 0; i++) {
        PendingSampleLoad *p = &pend->pending_loads[pend->pending_load_head];
        pend->pending_load_head = (pend->pending_load_head + 1) % PENDING_SAMPLE_LOAD_CAPACITY;
        pend->pending_load_count--;
        if (!pending_load_still_wanted(se, p)) continue;
        if (load_sample_slot(se, p->slot_index, p->path, p->root_note)) {
            apply_pending_fields(&se->slots->sample_slots[p->slot_index], p);
        } else {
            fprintf(stderr, "[samples] failed to load queued sample '%s' (zone_id %d)\n", p->path, p->zone_id);
            se->slots->sample_slots[p->slot_index].active = false;
        }
    }
}

// Round-robin hit counters, one per possible group id (index 0 unused -- group 0 means "not
// grouped", see SampleSlot.round_robin_group). Persists across calls the same way limiter_gain/
// lfo_phase_mod_state do elsewhere in this file (one synth instance, one audio thread -- no
// atomics needed, this is audio-thread-only state).

// Finds the loaded sample slot whose zone (key range AND velocity range, both inclusive) covers
// this (note, velocity) -- the real per-note/velocity mapping (SFZ-style): a note outside every
// loaded zone is silent, not "nearest root note always plays something." Overlapping zones (the
// Mapping tab's rectangles are freeform -- nothing stops the user from drawing two that overlap)
// are resolved by preferring the SMALLEST matching zone (key range * velocity range), the most
// deliberately-specific one. Among ties at that smallest area (the normal case for several
// round-robin variations all dropped at the exact same key+velocity range), slots sharing a
// non-zero round_robin_group cycle through each other one hit at a time instead of the same one
// always winning; an ungrouped tie (or a genuine unintentional overlap the user hasn't grouped)
// keeps the original deterministic behavior. Returns -1 if nothing loaded matches.
//
// Uses O(1) extra memory (three linear passes over the catalog) instead of building candidate/
// group-member arrays sized to a compile-time constant, since the catalog no longer has one.
// "First encountered while scanning in ascending array-index order" is all either tie-break here
// actually needs -- a stable, consistent ordering so the SAME physical zone is always picked first
// among ties and round-robin's own ordinal-to-member mapping never shifts between successive hits
// within a session. It is NOT "lowest zone_id"/creation order any more now that apply_zone_upsert()
// can reuse a removed zone's freed slot index for a brand new zone (see its own comment) -- a
// reused low index can hold a zone created later than one sitting at a higher, never-reused index.
// That's a harmless cosmetic difference (which round-robin member is nominally "first" among ties
// with no round_robin_group set is undefined/arbitrary either way), never a correctness one: which
// zone(s) match a given note+velocity, and how many round-robin members exist among them, are
// unaffected by array position.
// True when `note` is a keyswitch key for at least one zone currently assigned to this
// (owner_program, assigned_osc) pair -- an incoming note-on at this note number should update
// this pair's own tracked keyswitch state (HdsPendingLoadState.keyswitch_note_plus1) and produce
// NO sound of its own on this oscillator, rather than being treated as an ordinary playable note.
// Mirrors every real keyswitch sample library's own convention. See start_sample_osc's own
// interception of this at note-on.
static bool hds_sfz_is_keyswitch_note(HdsSampleEngine *se, int assigned_osc, int owner_program, int note) {
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc || slot->owner_program != owner_program) continue;
        if (slot->sw_lokey < 0 && slot->sw_hikey < 0 && slot->sw_last < 0) continue;
        int lo, hi;
        if (slot->sw_lokey >= 0 || slot->sw_hikey >= 0) {
            lo = (slot->sw_lokey >= 0) ? slot->sw_lokey : slot->sw_hikey;
            hi = (slot->sw_hikey >= 0) ? slot->sw_hikey : slot->sw_lokey;
        } else { lo = hi = slot->sw_last; }
        if (note >= lo && note <= hi) return true;
    }
    return false;
}

// True when `slot` isn't keyswitch-gated at all (the ordinary, unaffected case: matches on every
// note exactly as it always has), or IS gated and this (owner_program, assigned_osc) pair's own
// currently-tracked keyswitch state satisfies its declared sw_lokey/sw_hikey/sw_last range. See
// HdsPendingLoadState.keyswitch_note_plus1's own comment for the "never pressed, no sw_default
// either" 0 sentinel this treats as "never matches."
static bool hds_sfz_keyswitch_matches(HdsSampleEngine *se, const SampleSlot *slot) {
    if (slot->sw_lokey < 0 && slot->sw_hikey < 0 && slot->sw_last < 0) return true;
    int plus1 = se->pending->keyswitch_note_plus1[slot->owner_program][slot->assigned_osc];
    if (plus1 <= 0) return false;
    int current = plus1 - 1;
    if (slot->sw_lokey >= 0 || slot->sw_hikey >= 0) {
        int lo = (slot->sw_lokey >= 0) ? slot->sw_lokey : slot->sw_hikey;
        int hi = (slot->sw_hikey >= 0) ? slot->sw_hikey : slot->sw_lokey;
        return current >= lo && current <= hi;
    }
    return current == slot->sw_last;
}

// True when `slot` declares ANY crossfade-layering opcode (xfin_*/xfout_*, velocity or key axis)
// -- the gate on whether find_matching_sample_slot() even considers pairing it with a second,
// simultaneously-sounding zone at all. A zone that never sets these is completely unaffected by
// this whole feature, by construction (this always returns false for it).
static bool sfz_has_crossfade(const SampleSlot *slot) {
    return slot->xfin_lovel >= 0 || slot->xfin_hivel >= 0 || slot->xfout_lovel >= 0 || slot->xfout_hivel >= 0 ||
           slot->xfin_lokey >= 0 || slot->xfin_hikey >= 0 || slot->xfout_lokey >= 0 || slot->xfout_hikey >= 0;
}

// This zone's own linear crossfade gain (0-1) at this note-on's (note, velocity) -- the product of
// up to 4 independent ramps (fade-in/fade-out, each on the velocity axis and the key axis; a zone
// not using a given axis/direction at all contributes a factor of 1.0 for it, unaffected). Static
// for the life of a note: SFZ evaluates a crossfade once, at the triggering note-on, never
// per-sample -- see SynthVoice.vco1_xfade_gain's own comment.
static float sfz_xfade_gain(const SampleSlot *slot, int note, int velocity) {
    float g = 1.0f;
    if (slot->xfin_lovel >= 0 && slot->xfin_hivel >= 0 && slot->xfin_hivel > slot->xfin_lovel) {
        if (velocity <= slot->xfin_lovel) g *= 0.0f;
        else if (velocity < slot->xfin_hivel) g *= (float)(velocity - slot->xfin_lovel) / (float)(slot->xfin_hivel - slot->xfin_lovel);
    }
    if (slot->xfout_lovel >= 0 && slot->xfout_hivel >= 0 && slot->xfout_hivel > slot->xfout_lovel) {
        if (velocity >= slot->xfout_hivel) g *= 0.0f;
        else if (velocity > slot->xfout_lovel) g *= 1.0f - (float)(velocity - slot->xfout_lovel) / (float)(slot->xfout_hivel - slot->xfout_lovel);
    }
    if (slot->xfin_lokey >= 0 && slot->xfin_hikey >= 0 && slot->xfin_hikey > slot->xfin_lokey) {
        if (note <= slot->xfin_lokey) g *= 0.0f;
        else if (note < slot->xfin_hikey) g *= (float)(note - slot->xfin_lokey) / (float)(slot->xfin_hikey - slot->xfin_lokey);
    }
    if (slot->xfout_lokey >= 0 && slot->xfout_hikey >= 0 && slot->xfout_hikey > slot->xfout_lokey) {
        if (note >= slot->xfout_hikey) g *= 0.0f;
        else if (note > slot->xfout_lokey) g *= 1.0f - (float)(note - slot->xfout_lokey) / (float)(slot->xfout_hikey - slot->xfout_lokey);
    }
    return g;
}

// amp_velcurve_<N>=<value>: this zone's own custom, piecewise-linear velocity-to-gain response
// curve -- SFZ_MAX_VELCURVE_POINTS breakpoints, always kept sorted by ascending velocity as
// sfz_apply_opcode's own insertion-sort applies them, so this never needs to sort. Returns 1.0
// (no effect at all) for every zone with velcurve_count 0 -- the ordinary, unaffected case for
// every zone that never sets amp_velcurve_N at all -- so this is always safe to multiply into a
// zone's own static per-note gain unconditionally.
static float sfz_velcurve_gain(const SampleSlot *slot, int velocity) {
    if (slot->velcurve_count <= 0) return 1.0f;
    int last = slot->velcurve_count - 1;
    if (velocity <= slot->velcurve_point[0]) return slot->velcurve_gain[0];
    if (velocity >= slot->velcurve_point[last]) return slot->velcurve_gain[last];
    for (int i = 0; i < last; i++) {
        int p0 = slot->velcurve_point[i], p1 = slot->velcurve_point[i + 1];
        if (velocity < p0 || velocity > p1) continue;
        if (p1 == p0) return slot->velcurve_gain[i];
        float t = (float)(velocity - p0) / (float)(p1 - p0);
        return slot->velcurve_gain[i] + t * (slot->velcurve_gain[i + 1] - slot->velcurve_gain[i]);
    }
    return 1.0f; // unreachable given velcurve_point[] is sorted and spans [point[0], point[last]]
}

// Finds the zone (if any) covering (note, velocity) on this (assigned_osc, owner_program) pair.
// `is_first_key_of_phrase` gates trigger=first/legato zones (see HDS_SFZ_TRIGGER_* -- ordinary
// trigger=attack zones, the vast majority, are unaffected by it either way); trigger=release/
// release_key zones never match here at all (they only ever fire from fire_release_trigger_zones()
// on note-off). On a successful match, also resolves an optional CROSSFADE-LAYER partner (see
// sfz_has_crossfade()'s own comment): *out_layer_slot_idx is set to a second zone that should
// sound SIMULTANEOUSLY with the primary, mixed by *out_primary_gain/*out_layer_gain, or left at -1
// (gain 1.0/0.0) when the primary doesn't use crossfade layering at all -- the ordinary, unaffected
// case for every zone that never sets xfin_*/xfout_*, by construction.
static int find_matching_sample_slot(HdsSampleEngine *se, int assigned_osc, int owner_program, int note, int velocity,
                                      bool is_first_key_of_phrase, int *out_layer_slot_idx, float *out_primary_gain, float *out_layer_gain) {
    if (out_layer_slot_idx) *out_layer_slot_idx = -1;
    if (out_primary_gain) *out_primary_gain = 1.0f;
    if (out_layer_gain) *out_layer_gain = 0.0f;

    // One shared per-note-on random draw for every lorand/hirand-gated zone this call considers --
    // drawn once, not once per candidate, so a zone using it is evaluated against the exact same
    // "dice roll" as every other candidate zone in this same note-on, matching real SFZ semantics
    // (one random draw per note, not one per region).
    float rand_draw = (float)rand() / ((float)RAND_MAX + 1.0f);

    long best_area = -1; // -1 sentinel: "no candidate yet", distinct from a real zero-area zone
    int first_best_idx = -1;
    int tie_count = 0;
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc) continue;
        if (slot->owner_program != owner_program) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        // Only actually filters when the pair genuinely describes a real sub-range (hirand >
        // lorand) -- a defensive posture, not just an optimization: it means a bare zero-
        // initialized ZoneRecordWire/SampleSlot (lorand=hirand=0.0, what any caller that forgets
        // zone_record_wire_set_sfz_defaults() would end up with) still always matches on this axis,
        // instead of a 0.0/0.0 pair being misread as "never matches" (rand_draw is always >= 0.0).
        if (slot->hirand > slot->lorand && (rand_draw < slot->lorand || rand_draw >= slot->hirand)) continue;
        if (slot->trigger_type == HDS_SFZ_TRIGGER_RELEASE || slot->trigger_type == HDS_SFZ_TRIGGER_RELEASE_KEY) continue;
        if (slot->trigger_type == HDS_SFZ_TRIGGER_FIRST && !is_first_key_of_phrase) continue;
        if (slot->trigger_type == HDS_SFZ_TRIGGER_LEGATO && is_first_key_of_phrase) continue;
        if (!hds_sfz_keyswitch_matches(se, slot)) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (best_area < 0 || area < best_area) { best_area = area; first_best_idx = i; tie_count = 1; }
        else if (area == best_area) { tie_count++; }
    }
    if (tie_count == 0) return -1;

    int primary_idx = first_best_idx;
    if (tie_count > 1) {
        int group = se->slots->sample_slots[first_best_idx].round_robin_group;
        if (group > 0) {
            int member_count = 0;
            for (int i = 0; i < se->slots->sample_slot_count; i++) {
                SampleSlot *slot = &se->slots->sample_slots[i];
                if (!slot->active || !slot->loaded) continue;
                if (slot->assigned_osc != assigned_osc || slot->owner_program != owner_program) continue;
                if (note < slot->min_note || note > slot->max_note) continue;
                if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
                long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
                if (area == best_area && slot->round_robin_group == group) member_count++;
            }
            if (member_count > 1) {
                int group_idx = group;
                if (group_idx < 1) group_idx = 1; else if (group_idx > MAX_ROUND_ROBIN_GROUPS) group_idx = MAX_ROUND_ROBIN_GROUPS;
                unsigned int hit = se->pending->round_robin_hit_count[owner_program][assigned_osc][group_idx]++;
                unsigned int target_ordinal = hit % (unsigned int)member_count;
                // Walk members in seq_position order (an explicit 1-based ordinal; members that
                // don't set it, seq_position 0, sort after every member that does, in their own
                // slot-array/file-encounter order) -- picks the target_ordinal-th member by
                // repeatedly finding the next-smallest (seq_position, slot index) key strictly
                // after the last one picked. O(member_count^2) worst case, fine at the small
                // (typically 2-8) member counts round-robin groups actually have in practice.
                long last_key_seq = -1; int last_key_idx = -1;
                int chosen = -1;
                for (unsigned int step = 0; step <= target_ordinal; step++) {
                    long best_key_seq = LONG_MAX; int best_key_idx = INT_MAX; int best_slot_i = -1;
                    for (int i = 0; i < se->slots->sample_slot_count; i++) {
                        SampleSlot *slot = &se->slots->sample_slots[i];
                        if (!slot->active || !slot->loaded) continue;
                        if (slot->assigned_osc != assigned_osc || slot->owner_program != owner_program) continue;
                        if (note < slot->min_note || note > slot->max_note) continue;
                        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
                        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
                        if (area != best_area || slot->round_robin_group != group) continue;
                        long key_seq = slot->seq_position > 0 ? (long)slot->seq_position : LONG_MAX;
                        if (key_seq < last_key_seq || (key_seq == last_key_seq && i <= last_key_idx)) continue;
                        if (key_seq < best_key_seq || (key_seq == best_key_seq && i < best_key_idx)) { best_key_seq = key_seq; best_key_idx = i; best_slot_i = i; }
                    }
                    if (best_slot_i < 0) break; // shouldn't happen given member_count > 0, but keeps this loop safe
                    last_key_seq = best_key_seq; last_key_idx = best_key_idx;
                    chosen = best_slot_i;
                }
                if (chosen >= 0) primary_idx = chosen;
            }
        }
    }

    if (out_primary_gain) *out_primary_gain = sfz_xfade_gain(&se->slots->sample_slots[primary_idx], note, velocity);
    if (sfz_has_crossfade(&se->slots->sample_slots[primary_idx]) && out_layer_slot_idx) {
        int best_layer = -1; float best_layer_gain = 0.0f;
        for (int i = 0; i < se->slots->sample_slot_count; i++) {
            if (i == primary_idx) continue;
            SampleSlot *slot = &se->slots->sample_slots[i];
            if (!slot->active || !slot->loaded) continue;
            if (slot->assigned_osc != assigned_osc || slot->owner_program != owner_program) continue;
            if (note < slot->min_note || note > slot->max_note) continue;
            if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
            if (!sfz_has_crossfade(slot)) continue;
            if (!hds_sfz_keyswitch_matches(se, slot)) continue;
            float g = sfz_xfade_gain(slot, note, velocity);
            if (g > best_layer_gain) { best_layer_gain = g; best_layer = i; }
        }
        if (best_layer >= 0 && best_layer_gain > 0.001f) {
            *out_layer_slot_idx = best_layer;
            if (out_layer_gain) *out_layer_gain = best_layer_gain;
        }
    }
    return primary_idx;
}

// Starts (or restarts) an oscillator's sample playback for a fresh note -- releases whatever
// stream it might already hold (a retrigger/voice-steal case) and claims a new one if
// Counts how many currently-claimed streams are playing a zone with this exact zone_id -- the
// note_polyphony= gate (start_sample_osc's own comment on where this is used). O(MAX_SAMPLE_
// STREAMS), fine at note-on rate (not per-sample).
static int count_active_streams_for_zone(HdsSampleEngine *se, int32_t zone_id) {
    int count = 0;
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        SampleStream *stream = &se->streams->sample_streams[i];
        if (!stream->in_use) continue;
        int slot_idx = atomic_load_explicit(&stream->slot_index, memory_order_relaxed);
        if (se->slots->sample_slots[slot_idx].zone_id == zone_id) count++;
    }
    return count;
}

// SFZ's real group=/off_by= choke groups: the instant a zone with choke_off_by>0 triggers, every
// currently-sounding stream anywhere in this SAME owner_program whose OWN zone declares that exact
// choke_group is cut immediately (release_sample_stream(), the same hard, immediate stop a stream
// exhausting its ring already gets -- render_sample_osc sees stream->in_use go false on its very
// next call and returns silence right then, no separate envelope-release ramp). This is genuinely
// independent of round-robin's own (unrelated) group concept -- see SampleSlot.choke_group's own
// comment. A no-op (off_by<=0) for every zone that doesn't use choke groups at all, by construction.
static void apply_choke_group(HdsSampleEngine *se, int owner_program, int off_by_group) {
    if (off_by_group <= 0) return;
    for (int i = 0; i < MAX_SAMPLE_STREAMS; i++) {
        SampleStream *stream = &se->streams->sample_streams[i];
        if (!stream->in_use) continue;
        int slot_idx = atomic_load_explicit(&stream->slot_index, memory_order_relaxed);
        SampleSlot *slot = &se->slots->sample_slots[slot_idx];
        if (slot->owner_program != owner_program) continue;
        if (slot->choke_group != off_by_group) continue;
        release_sample_stream(se, i);
    }
}

// SFZ per-zone amp envelope override: builds a full EGDerivedRates directly from a zone's own
// ampeg_* fields (already SFZ-defaulted by hds_load_sfz_for_osc -- see SampleSlot.has_own_ampeg's
// own comment), bypassing compute_eg_derived_rates()/its 7-mode PackEGParams abstraction entirely.
// That abstraction can't express delay AND hold simultaneously (only mode 5/DADSR gets a nonzero
// t_dly, only mode 4/6 get a nonzero t_hld) -- real SFZ content can set ampeg_delay and ampeg_hold
// together freely, so this computes both unconditionally instead. mode is fixed at 6 purely as an
// env_type/diagnostic label (tick_adsr's own mode==1/2/3/4 special-cases -- AR/ASR skipping decay,
// AD/AHD skipping sustain and going idle -- do not apply to an SFZ envelope, which always has a
// real, distinct decay+sustain+release regardless of what sustain level it happens to be at).
static void compute_zone_owned_eg_rates(const SampleSlot *slot, EGDerivedRates *out) {
    out->mode = 6;
    out->sustain_level = fmaxf(0.0f, fminf(1.0f, slot->ampeg_sustain / 100.0f));
    out->atk_level = fmaxf(0.0f, fminf(1.0f, slot->ampeg_start / 100.0f));
    out->peak_level = 1.0f; // SFZ's ampeg_* has no "peak level" opcode of its own -- always full-swing
    out->t_dly = fmaxf(0.0f, slot->ampeg_delay);
    out->t_hld = fmaxf(0.0f, slot->ampeg_hold);
    float t_att = fmaxf(0.001f, slot->ampeg_attack);
    float t_dec = fmaxf(0.001f, slot->ampeg_decay);
    // SFZ's own release default (0, an unset ampeg_release) would divide into a literal instant
    // drop -- floored at 5ms, an inaudible but non-degenerate ramp, rather than a true 0 that could
    // click or (at exactly 0) divide by zero.
    float t_rel = fmaxf(0.005f, slot->ampeg_release);
    out->attack_rate = fmaxf(0.0f, out->peak_level - out->atk_level) / (t_att * SAMPLE_RATE);
    out->decay_rate = fmaxf(0.0f, out->peak_level - out->sustain_level) / (t_dec * SAMPLE_RATE);
    out->release_rate_per_level = 1.0f / (t_rel * SAMPLE_RATE);
}

// SFZ per-zone amp envelope override resolution (see ProgramState.vco1_force_own_amp_eg's own
// comment) -- looks up the zone a just-claimed stream belongs to (if any) and decides whether this
// note's oscillator should use THAT zone's own ampeg_* envelope instead of the Program's shared
// one. Returns false (out_rates left untouched) for every oscillator not currently playing a zone
// with its own ampeg_* opcodes at all, or with the override checkbox forcing the Program's own EG
// -- the ordinary, unaffected case for the vast majority of zones/Programs.
bool resolve_zone_owned_ampeg(HdsSampleEngine *se, int stream_idx, bool force_own_amp_eg, EGDerivedRates *out_rates) {
    if (stream_idx < 0 || force_own_amp_eg) return false;
    int slot_idx = atomic_load_explicit(&se->streams->sample_streams[stream_idx].slot_index, memory_order_relaxed);
    SampleSlot *slot = &se->slots->sample_slots[slot_idx];
    if (!slot->has_own_ampeg) return false;
    compute_zone_owned_eg_rates(slot, out_rates);
    return true;
}

// Drum Bus routing resolution (see DRUM_BUS_COUNT's own comment) -- looks up the zone a just-
// claimed stream belongs to and returns its own drum_bus assignment (0 = off, the ordinary case
// for every zone that never sets it). Same "resolve once at note-on, cache on the voice" posture
// as resolve_zone_owned_ampeg() just above, for the same reason.
int resolve_zone_drum_bus(HdsSampleEngine *se, int stream_idx) {
    if (stream_idx < 0) return 0;
    int slot_idx = atomic_load_explicit(&se->streams->sample_streams[stream_idx].slot_index, memory_order_relaxed);
    return se->slots->sample_slots[slot_idx].drum_bus;
}

// Shared "where does this envelope INSTANCE start" logic for handle_note_on's own EG-init block --
// factored out once the SFZ per-zone amp envelope override needed a second, parallel source of
// truth (a zone's own compute_zone_owned_eg_rates() output) alongside the Program's own knob-driven
// eg_mode[]/eg[] -- see ProgramState.vco1_force_own_amp_eg's own comment. Behavior is byte-
// identical to the original inline code when use_own_ampeg is false, the ordinary case.
void init_amp_eg_start(ADSR *eg, bool use_own_ampeg, const EGDerivedRates *own_rates, int prog_mode, const PackEGParams *prog_eg) {
    if (use_own_ampeg) {
        eg->state = (own_rates->t_dly > 0.0f) ? STATE_DELAY : STATE_ATTACK;
        eg->current_level = (own_rates->t_dly > 0.0f) ? 0.0f : own_rates->atk_level;
    } else {
        eg->state = (prog_mode == 5) ? STATE_DELAY : STATE_ATTACK;
        eg->current_level = (prog_mode == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog_eg->al / 100.0f));
    }
}

// Shared "claim this one zone and seed its playback position" logic -- used identically for the
// primary zone and an optional crossfade-layer partner (see find_matching_sample_slot's own
// comment), since both are just independent DFD streams from find_matching_sample_slot's point of
// view. Returns the claimed stream index (or -1 if slot_idx < 0 or the pool is exhausted).
static int start_one_sample_stream(HdsSampleEngine *se, double *ptr, bool *preroll, int slot_idx) {
    unsigned int start_frame = 0;
    if (slot_idx >= 0) {
        SampleSlot *slot = &se->slots->sample_slots[slot_idx];
        // start_offset_frac is where in the file playback begins, in the travel direction (see
        // SampleSlot.reverse's comment) -- computed ONCE here, on the audio thread, from the one
        // live copy of this field, then handed to the disk thread atomically via
        // claim_sample_stream (see SampleStream.start_frame's comment for why: independently
        // re-deriving this on both threads from the same mutable field at two different,
        // unsynchronized moments could disagree if a catalog edit landed in between).
        float frac = fmaxf(0.0f, fminf(1.0f, slot->start_offset_frac));
        start_frame = (unsigned int)(frac * (float)(slot->total_frames > 0 ? slot->total_frames - 1 : 0));
        if (slot->reverse) {
            // "In the travel direction": a reverse zone's offset counts back from its last frame,
            // so offset 0 starts at the end. (It used to be taken as a forward file frame, so a
            // reverse zone with no offset started at frame 0 and stopped after one frame.)
            unsigned int last = slot->total_frames > 0 ? slot->total_frames - 1 : 0;
            start_frame = last - start_frame;
            // Reverse zones skip the pre-roll fast path entirely (it's only ever decoded forward
            // from file frame 0, useless for a zone that wants to start near the end) -- go
            // straight to ring consumption, accepting a brief real disk-catch-up latency window,
            // the same class of trade-off already accepted elsewhere in this file.
            *preroll = false;
            *ptr = 0.0;
        } else {
            // Forward: if start_frame falls inside the pre-roll window, seed ptr there directly
            // and let normal pre-roll interpolation proceed; otherwise seed ptr at EXACTLY the
            // pre_roll_frames boundary so render_sample_osc's existing "ptr >= pre_roll_frames ->
            // hand off to ring" logic fires on the very first call, against a ring the disk
            // thread has ALSO been told (via start_frame) to start filling from start_frame.
            *ptr = (double)(start_frame < slot->pre_roll_frames ? start_frame : slot->pre_roll_frames);
            *preroll = true;
        }
    } else {
        *ptr = 0.0;
        *preroll = true;
    }
    return (slot_idx >= 0) ? claim_sample_stream(se, slot_idx, start_frame) : -1;
}

// find_matching_sample_slot finds a zone covering this note+velocity. Leaves *stream_idx at -1
// (silent) if nothing matches or the stream pool is exhausted, rather than failing the whole
// note-on. `is_first_key_of_phrase` gates trigger=first/legato zones -- see
// find_matching_sample_slot's own comment (irrelevant, and harmless to pass either way, for every
// ordinary trigger=attack zone). *out_xfade_gain/*out_layer_xfade_gain and the layer_* trio are an
// optional CROSSFADE-LAYER partner stream (see SynthVoice.vco1_layer_sample_stream's own comment);
// pass NULL for all four when a caller doesn't support layering at all (none currently do, but the
// option costs nothing). *out_delay_frames (SFZ's delay=, in frames at SAMPLE_RATE) is 0 for the
// ordinary, unaffected case -- the caller is responsible for actually holding playback silent for
// that many samples once a stream is claimed (see SynthVoice.vco1_sample_delay_remaining's own
// comment); this function claims the stream/seeds its ring position immediately either way, since
// the disk thread filling ahead of an still-silent oscillator is harmless.
void start_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr, bool *preroll, int assigned_osc, int owner_program, int note, int velocity,
                              bool is_first_key_of_phrase, int *out_delay_frames,
                              int *layer_stream_idx, double *layer_ptr, bool *layer_preroll,
                              float *out_xfade_gain, float *out_layer_xfade_gain) {
    if (*stream_idx >= 0) { release_sample_stream(se, *stream_idx); *stream_idx = -1; }
    if (layer_stream_idx && *layer_stream_idx >= 0) { release_sample_stream(se, *layer_stream_idx); *layer_stream_idx = -1; }
    if (out_delay_frames) *out_delay_frames = 0;
    if (out_xfade_gain) *out_xfade_gain = 1.0f;
    if (out_layer_xfade_gain) *out_layer_xfade_gain = 0.0f;

    // Keyswitching interception: a note-on landing inside ANY loaded zone's own declared keyswitch
    // range on this (owner_program, assigned_osc) pair updates that pair's tracked state and
    // produces NO sound of its own -- see hds_sfz_is_keyswitch_note's own comment. Matches every
    // real keyswitch library's own convention; a total no-op for every instrument that never uses
    // sw_lokey/sw_hikey/sw_last at all, by construction.
    if (hds_sfz_is_keyswitch_note(se, assigned_osc, owner_program, note)) {
        se->pending->keyswitch_note_plus1[owner_program][assigned_osc] = note + 1;
        *ptr = 0.0; *preroll = true;
        return;
    }

    int layer_idx = -1; float primary_gain = 1.0f, layer_gain = 0.0f;
    int slot_idx = find_matching_sample_slot(se, assigned_osc, owner_program, note, velocity, is_first_key_of_phrase, &layer_idx, &primary_gain, &layer_gain);

    if (slot_idx >= 0) {
        SampleSlot *slot = &se->slots->sample_slots[slot_idx];
        if (slot->note_polyphony > 0 && count_active_streams_for_zone(se, slot->zone_id) >= slot->note_polyphony) {
            slot_idx = -1; layer_idx = -1; // at this zone's own polyphony cap -- stays silent rather than stealing an older voice
        } else {
            if (slot->choke_off_by > 0) apply_choke_group(se, owner_program, slot->choke_off_by);
            if (out_delay_frames) *out_delay_frames = (int)(slot->delay_seconds * (float)SAMPLE_RATE + 0.5f);
        }
    }

    // amp_velcurve_N (see sfz_velcurve_gain's own comment): folded into this same static per-note
    // gain multiplier as the crossfade-layer gain above, rather than a separate SynthVoice field --
    // both are just "a static, note-on-computed gain this zone's own render output gets scaled by
    // for the life of the note," so there's nothing to gain from tracking them independently. 1.0
    // (no effect) for every zone that never sets amp_velcurve_N at all.
    if (out_xfade_gain) *out_xfade_gain = primary_gain * (slot_idx >= 0 ? sfz_velcurve_gain(&se->slots->sample_slots[slot_idx], velocity) : 1.0f);
    *stream_idx = start_one_sample_stream(se, ptr, preroll, slot_idx);

    if (layer_stream_idx && layer_ptr && layer_preroll) {
        if (layer_idx >= 0) {
            SampleSlot *lslot = &se->slots->sample_slots[layer_idx];
            if (lslot->note_polyphony > 0 && count_active_streams_for_zone(se, lslot->zone_id) >= lslot->note_polyphony) layer_idx = -1;
        }
        if (out_layer_xfade_gain) *out_layer_xfade_gain = (layer_idx >= 0) ? layer_gain * sfz_velcurve_gain(&se->slots->sample_slots[layer_idx], velocity) : 0.0f;
        *layer_stream_idx = start_one_sample_stream(se, layer_ptr, layer_preroll, layer_idx);
    }
}

// SFZ trigger=release/release_key: fires at NOTE-OFF (see trigger_voice_release's own call site),
// completely independent of the ordinary voice/oscillator that's simultaneously entering its own
// release phase -- a release-triggered zone plays out with no amp EG, pitch modulation, or filter
// routing of its own (matching every real sampler's own "release samples are a fire-and-forget
// effect layer, not a normal voice" convention), using a small fixed pool
// (HdsReleaseTailPlayer/HDS_RELEASE_TAIL_POOL_SIZE) rather than stealing a real polyphony voice.
// Only ever matches a zone whose own trigger= is release/release_key -- see HDS_SFZ_TRIGGER_*'s
// own comment; a complete no-op for every ordinary (trigger=attack, the vast majority) zone.
// Picks the smallest-area matching zone, same tie-break rule as find_matching_sample_slot's own
// first pass, but WITHOUT that function's round-robin/crossfade-layer machinery -- release
// triggers are typically a single mechanical-noise layer per note range, not a variant pool.
// rt_decay= (if the winning zone sets it) is applied here as a flat, elapsed-real-time dB/sec
// decay on the release tail's own playback level; it does NOT additionally scale by how long the
// original note was actually held (a further refinement real SFZ implementations sometimes add) --
// a documented, narrower scope than the spec's full behavior, not a bug.
void fire_release_trigger_zones(HdsSampleEngine *se, int assigned_osc, int owner_program, int note, int velocity) {
    long best_area = -1;
    int best_idx = -1;
    for (int i = 0; i < se->slots->sample_slot_count; i++) {
        SampleSlot *slot = &se->slots->sample_slots[i];
        if (!slot->active || !slot->loaded) continue;
        if (slot->assigned_osc != assigned_osc || slot->owner_program != owner_program) continue;
        if (slot->trigger_type != HDS_SFZ_TRIGGER_RELEASE && slot->trigger_type != HDS_SFZ_TRIGGER_RELEASE_KEY) continue;
        if (note < slot->min_note || note > slot->max_note) continue;
        if (velocity < slot->min_velocity || velocity > slot->max_velocity) continue;
        long area = (long)(slot->max_note - slot->min_note + 1) * (long)(slot->max_velocity - slot->min_velocity + 1);
        if (best_area < 0 || area < best_area) { best_area = area; best_idx = i; }
    }
    if (best_idx < 0) return;

    int player_idx = -1;
    for (int i = 0; i < HDS_RELEASE_TAIL_POOL_SIZE; i++) {
        if (!se->pending->release_tails[i].active) { player_idx = i; break; }
    }
    if (player_idx < 0) return; // pool exhausted -- drop this one release tail rather than steal an already-playing one

    SampleSlot *slot = &se->slots->sample_slots[best_idx];
    HdsReleaseTailPlayer *rt = &se->pending->release_tails[player_idx];
    double ptr = 0.0; bool preroll = true;
    int stream_idx = start_one_sample_stream(se, &ptr, &preroll, best_idx);
    if (stream_idx < 0) return; // pool exhausted (SampleStream side) -- nothing to play
    rt->active = true;
    rt->stream_idx = stream_idx;
    rt->ptr = ptr;
    rt->preroll = preroll;
    rt->owner_program = owner_program;
    rt->play_hz = slot->unpitched ? (440.0f * powf(2.0f, (float)(slot->root_note - 69) / 12.0f)) : (440.0f * powf(2.0f, (float)(note - 69) / 12.0f));
    rt->gain = 1.0f;
    rt->decay_per_sample = (slot->rt_decay > 0.0f) ? powf(10.0f, -slot->rt_decay / 20.0f / (float)SAMPLE_RATE) : 1.0f;
}

// Called once per active voice per sample from render_audio_block, right after the EG tick loop
// -- if an oscillator's amp envelope has gone idle but it's still holding a claimed sample
// stream, releases it. Idempotent (stream_index stays -1 afterward, so this is a cheap no-op on
// every subsequent call) and cheap enough to run unconditionally.
void release_idle_sample_streams(HdsSampleEngine *se, SynthVoice *voice) {
    if (voice->vco1_eg.state == STATE_IDLE && voice->vco1_sample_stream >= 0) {
        release_sample_stream(se, voice->vco1_sample_stream);
        voice->vco1_sample_stream = -1;
    }
    if (voice->vco1_eg.state == STATE_IDLE && voice->vco1_layer_sample_stream >= 0) {
        release_sample_stream(se, voice->vco1_layer_sample_stream);
        voice->vco1_layer_sample_stream = -1;
    }
    if (voice->vco2_eg.state == STATE_IDLE && voice->vco2_sample_stream >= 0) {
        release_sample_stream(se, voice->vco2_sample_stream);
        voice->vco2_sample_stream = -1;
    }
    if (voice->vco2_eg.state == STATE_IDLE && voice->vco2_layer_sample_stream >= 0) {
        release_sample_stream(se, voice->vco2_layer_sample_stream);
        voice->vco2_layer_sample_stream = -1;
    }
    if (voice->vco3_eg.state == STATE_IDLE && voice->vco3_sample_stream >= 0) {
        release_sample_stream(se, voice->vco3_sample_stream);
        voice->vco3_sample_stream = -1;
    }
    if (voice->vco3_eg.state == STATE_IDLE && voice->vco3_layer_sample_stream >= 0) {
        release_sample_stream(se, voice->vco3_layer_sample_stream);
        voice->vco3_layer_sample_stream = -1;
    }
    if (voice->vco4_eg.state == STATE_IDLE && voice->vco4_sample_stream >= 0) {
        release_sample_stream(se, voice->vco4_sample_stream);
        voice->vco4_sample_stream = -1;
    }
    if (voice->vco4_eg.state == STATE_IDLE && voice->vco4_layer_sample_stream >= 0) {
        release_sample_stream(se, voice->vco4_layer_sample_stream);
        voice->vco4_layer_sample_stream = -1;
    }
}

// Drop-in mono waveform source for a WAVE_SAMPLE oscillator, mirroring lookup_osc_sample's role
// (called once per voice per sample from render_audio_block; its return value flows into the
// exact same amp-EG/pan/filter/FX pipeline every other oscillator uses -- that's the entire
// point of this design, per the user's direction: adapt sample playback to HDS's existing mature
// architecture, not bring in a parallel one). `hz` is the SAME fully-modulated frequency (fine
// tune, pitch bend, every mod-matrix pitch destination, portamento) already computed for the
// periodic oscillators -- sample playback rate is just that divided by the sample's own
// root-note frequency, so pitch bend/vibrato/EG-to-pitch all work on a sample voice for free,
// with zero duplicated pitch logic.
//
// A natively stereo file's real L/R pair is also handed back via the optional out_l/out_r
// pointers (NULL if the caller doesn't want them), exactly mirroring render_unison_saw's
// side_l/side_r convention -- the mono return value stays available for FM-modulator use and
// mono-file callers. The pre-roll cache and ring buffer already track L and R completely
// independently end to end (interpolation, loop-splice crossfade, everything); mono is only
// ever collapsed at the very last step here, so a stereo drum loop or pad keeps its real width
// instead of being downmixed, the same fix WAVE_UNISON_SAW's own stereo image needed earlier.
float render_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr_inout, bool *preroll_inout, float hz, float *out_l_ptr, float *out_r_ptr) {
    if (*stream_idx < 0) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }
    SampleStream *stream = &se->streams->sample_streams[*stream_idx];
    if (!stream->in_use) { *stream_idx = -1; if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; } // reclaimed elsewhere -- defensive, shouldn't happen
    SampleSlot *slot = &se->slots->sample_slots[atomic_load_explicit(&stream->slot_index, memory_order_relaxed)];
    if (!slot->loaded) { if (out_l_ptr) *out_l_ptr = 0.0f; if (out_r_ptr) *out_r_ptr = 0.0f; return 0.0f; }

    // The pre-roll/ring read-pointer advances by `ratio` FILE FRAMES per OUTPUT sample, so if this
    // file wasn't recorded at this engine's own fixed SAMPLE_RATE, reading it 1 file-frame-per-
    // output-sample (ratio 1.0) does not actually reproduce it at its own native rate -- a 44.1kHz
    // file read that way comes out ~1.47 semitones sharp and finishes early on a 48kHz engine,
    // regardless of what musical pitch was asked for. rate_comp corrects that mismatch; it is
    // multiplied into EVERY case below (including `unpitched`, whose own "always plays at its
    // native recorded rate" comment already stated this exact intent -- rate_comp is what actually
    // achieves it instead of only being true by coincidence when a file happens to already be
    // 48kHz). 0 (a SampleSlot never routed through load_sample_slot(), e.g. some direct test setup)
    // falls back to 1.0 -- "assume it matches" is the only sane default with no real rate to go on.
    float rate_comp = (slot->sample_rate_hz > 0) ? ((float)slot->sample_rate_hz / (float)SAMPLE_RATE) : 1.0f;
    float ratio;
    if (slot->unpitched) {
        ratio = rate_comp; // DRUM (Unpitched) -- always plays at its native recorded rate
    } else {
        // Deliberately always standard 12-TET here, never hds_note_to_hz() -- see that function's
        // own comment for why WAVE_SAMPLE's root-note reference is a documented microtuning scope
        // boundary rather than an oversight.
        float root_hz = 440.0f * powf(2.0f, (float)(slot->root_note - 69) / 12.0f);
        ratio = (root_hz > 1.0f) ? fmaxf(0.01f, hz / root_hz) : 1.0f;
        ratio *= rate_comp;
    }
    // Per-zone fine tune (cents) -- applied even when unpitched, a nudge independent of the
    // root-note-relative pitch tracking `unpitched` specifically disables.
    if (slot->tune_cents != 0.0f) ratio *= powf(2.0f, slot->tune_cents / 1200.0f);

    double ptr = *ptr_inout;
    bool preroll = *preroll_inout;
    float out_l = 0.0f, out_r = 0.0f;

    if (preroll) {
        unsigned int int_ptr = (unsigned int)ptr;
        if (int_ptr >= slot->pre_roll_frames) {
            // Hand off to the ring buffer. Carry the fractional remainder across the boundary
            // (rather than hard-resetting to 0.0) so pitched playback (ratio != 1.0) doesn't lose
            // or duplicate a fraction of a sample here -- ring index 0 is exactly file frame
            // pre_roll_frames (see sample_disk_thread_main's SAMPLE_CMD_START handler), so this
            // stays sample-accurate with the pre-roll cache's own indexing.
            preroll = false;
            ptr -= (double)slot->pre_roll_frames;
        } else {
            // Last valid pre-roll index (pre_roll_frames - 1) has no real "next" sample in this
            // cache -- pre_roll_l/r are only populated up to pre_roll_frames - 1 (the trailing +2
            // calloc'd frames are zero-padding purely to make this interpolation read safe, not
            // real audio). Hold flat there instead of interpolating toward that zero pad, which
            // would otherwise dip the output toward silence right before the transition.
            unsigned int next_idx = int_ptr + 1;
            bool has_next = next_idx < slot->pre_roll_frames;
            float next_l = has_next ? slot->pre_roll_l[next_idx] : slot->pre_roll_l[int_ptr];
            float next_r = has_next ? slot->pre_roll_r[next_idx] : slot->pre_roll_r[int_ptr];
            float frac = (float)(ptr - (double)int_ptr);
            out_l = slot->pre_roll_l[int_ptr] + frac * (next_l - slot->pre_roll_l[int_ptr]);
            out_r = slot->pre_roll_r[int_ptr] + frac * (next_r - slot->pre_roll_r[int_ptr]);
            ptr += ratio;
        }
    }

    // The ring (and the EOF flag) only belong to this note once the disk thread has started this
    // session -- see SampleStream.session. Until then, hold silent exactly like starvation, keeping
    // ptr/preroll as they are, and leave read_ptr alone (the disk thread is about to reset it).
    if (!preroll && atomic_load_explicit(&stream->ready_session, memory_order_acquire) != atomic_load_explicit(&stream->session, memory_order_relaxed)) {
        *ptr_inout = ptr; *preroll_inout = preroll;
        if (out_l_ptr) *out_l_ptr = 0.0f;
        if (out_r_ptr) *out_r_ptr = 0.0f;
        return 0.0f;
    }

    if (!preroll) {
        unsigned int r_ptr = atomic_load_explicit(&stream->read_ptr, memory_order_acquire);
        unsigned int w_ptr = atomic_load_explicit(&stream->write_ptr, memory_order_acquire);
        unsigned int available = (w_ptr - r_ptr) & SAMPLE_RING_MASK;

        // Frames this call reads (idx0/idx1) plus how far it moves read_ptr: never past write_ptr.
        unsigned int need = (unsigned int)ptr + 2u + (unsigned int)ratio;
        bool at_eof = atomic_load_explicit(&stream->disk_reached_eof, memory_order_acquire);
        // While the file still has audio to give, wait for a margin of SAMPLE_XFADE_SIZE + 2
        // frames before resuming after starvation (as before). Once the disk thread has hit the
        // end, play out what's left down to the last frames -- the old fixed margin cut the final
        // ~1000 frames (about 21 ms) off the end of every streamed one-shot.
        if (available < need || (!at_eof && available < (unsigned int)(SAMPLE_XFADE_SIZE + 2))) {
            // Ring starvation alone does NOT mean "reached the end of the file" -- it's also
            // exactly what a freshly-claimed stream's ring looks like before the (single, shared)
            // disk thread has gotten around to filling it, which can genuinely take a while under
            // a burst of many simultaneous/rapid retriggers across several Multisample
            // oscillators (see disk_reached_eof's own comment). Only release for a non-looping
            // zone once the disk thread has explicitly confirmed there's truly nothing left;
            // otherwise -- exactly like the always-silent-hold behavior a looping zone already
            // gets below -- just hold silent and wait for it to catch up. This is what used to
            // silently and permanently kill a note the instant it should have started audibly
            // playing, under exactly the kind of stress-test load that makes the disk thread fall
            // behind.
            if (!slot->loop_enabled && at_eof && available < need) {
                release_sample_stream(se, *stream_idx);
                *stream_idx = -1;
                *ptr_inout = ptr; *preroll_inout = preroll;
                if (out_l_ptr) *out_l_ptr = 0.0f;
                if (out_r_ptr) *out_r_ptr = 0.0f;
                return 0.0f;
            }
            *ptr_inout = ptr; *preroll_inout = preroll;
            if (out_l_ptr) *out_l_ptr = 0.0f;
            if (out_r_ptr) *out_r_ptr = 0.0f;
            return 0.0f;
        }

        unsigned int int_ptr = (unsigned int)ptr;
        float frac = (float)(ptr - (double)int_ptr);
        unsigned int idx0 = (r_ptr + int_ptr) & SAMPLE_RING_MASK;
        unsigned int idx1 = (idx0 + 1) & SAMPLE_RING_MASK;

        // Loop splices are already crossfaded in the ring by the disk thread (disk_fill_forward),
        // so this is plain linear interpolation.
        out_l = stream->ring_l[idx0] + frac * (stream->ring_l[idx1] - stream->ring_l[idx0]);
        out_r = stream->ring_r[idx0] + frac * (stream->ring_r[idx1] - stream->ring_r[idx0]);

        ptr += ratio;
        while (ptr >= 1.0) { ptr -= 1.0; r_ptr++; }
        atomic_store_explicit(&stream->read_ptr, r_ptr, memory_order_release);
    }

    *ptr_inout = ptr;
    *preroll_inout = preroll;
    if (slot->gain_db != 0.0f) {
        float gain_mult = powf(10.0f, slot->gain_db / 20.0f);
        out_l *= gain_mult; out_r *= gain_mult;
    }
    if (out_l_ptr) *out_l_ptr = out_l;
    if (out_r_ptr) *out_r_ptr = out_r;
    return (out_l + out_r) * 0.5f;
}
