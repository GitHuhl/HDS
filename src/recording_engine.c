// src/recording_engine.c -- the WAV/WavPack recording engine (Recording tab's backend half),
// extracted verbatim from hds_standalone.c so hds_android.c can #include it too (see that file's
// own include-order comment): RecordRing/WavRecorder/WavPackRecorder/RecordEngine/RecordGatherState
// and the dedicated writer thread are all platform-agnostic (plain POSIX FILE*/pthread, no ALSA/
// PipeWire dependency of any kind) -- only the audio-thread-side glue that feeds
// wav_recorder_gather_block() every block and flips record_engine.command on the record_enable
// edge stays platform-specific (hds_standalone.c's own main() loop; hds_android.c's own AAudio
// callback, see that file's own Recording section). #include'd AFTER backend.c (needs SAMPLE_RATE/
// BUFFER_SIZE/RECORD_MAX_CHANNELS/RECORD_PATH_LEN) and, if HDS_WAVPACK_ENABLED is going to be 1 in
// this translation unit, AFTER <wavpack/wavpack.h> too -- hds_standalone.c already satisfies both
// at its own #include site; hds_android.c never defines HDS_WAVPACK_ENABLED at all, so the stub
// branch below is used there and no wavpack.h is needed.

// --- WAV recording (Recording tab) ---
//
// Writes 32-bit float PCM (WAVE_FORMAT_IEEE_FLOAT, format tag 3) -- matches the engine's own
// native sample format exactly, so no int conversion/dithering is needed, and is natively
// readable by every modern DAW/editor (Audacity, REAPER, ffmpeg, etc). RIFF/data/fact chunk
// sizes are written as placeholders and patched in on stop, once the real length is known.
//
// REALTIME SAFETY: every function in this section that touches the filesystem (fopen/fwrite/
// fseek/fclose) runs ONLY on the dedicated writer thread below (record_writer_thread_main), never
// on the main audio thread. Buffered stdio's fwrite() is not bounded-time-safe -- it periodically
// triggers a blocking write(2) syscall once its internal buffer fills, and disk I/O latency
// (especially USB/SD/network storage, or under system I/O contention) is not bounded the way the
// audio thread's per-block real-time budget requires. A stall there would show up as exactly the
// kind of underrun this file works hard to avoid everywhere else. So the audio thread only ever
// does bounded, allocation-free work for recording: computing the gather/downmix arithmetic
// (wav_recorder_gather_block, pure math, no I/O) and a wait-free ring-buffer push
// (record_ring_push, never blocks -- see RecordRing). All the actual file I/O, including opening
// the file and writing the header on start and patching/closing it on stop, happens on the writer
// thread instead, driven by a tiny atomic command handshake (RecordEngine) from the audio thread.
typedef struct {
    FILE *file;
    int channels;
    uint32_t frames_written;
    bool active;
    long data_size_offset;  // ftell() position of the "data" chunk's 4-byte size field
    long fact_count_offset; // ftell() position of the "fact" chunk's 4-byte sample-count field

    // Channel selection is frozen at record-start time from the live knobs, and used for every
    // block written until the recording stops -- so toggling a checkbox mid-take can't desync
    // the interleaving the file header already committed to. Start a new recording to pick up
    // a changed selection.
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count; // detected_input_channels at record-start, for indexing capture_buffer's stride
} WavRecorder;

// Lock-free single-producer/single-consumer ring buffer of raw interleaved float samples, used
// to hand recorded audio from the audio thread (producer) to the writer thread (consumer)
// without either side ever blocking on the other. Capacity is a power of two so index wraparound
// is a cheap mask; read_idx/write_idx are monotonically increasing counters (never actually
// wrapped themselves), which is what makes "how much is available/free" a plain subtraction with
// no ambiguity between empty and full. 1<<22 floats (16MB) gives ~4.8s of backlog headroom even
// at the worst case of 18 channels @ 48kHz -- far more than any realistic disk stall -- while
// still being cheap enough to just keep as a static allocation (see the "static" rationale on
// capture_buffer/ext_in_stereo above: this is far too large to put on main()'s stack).
#define RECORD_RING_CAPACITY (1 << 22)
typedef struct {
    float data[RECORD_RING_CAPACITY];
    _Atomic size_t write_idx;       // only ever written by the audio thread (producer)
    _Atomic size_t read_idx;        // only ever written by the writer thread (consumer)
    _Atomic uint64_t dropped_samples; // diagnostic only: incremented by the producer when full
} RecordRing;

// Wait-free: never blocks, never loops waiting for the consumer. If there isn't room, the new
// data is dropped (and counted) rather than overwriting unread data or stalling the caller --
// on the audio thread, dropping a moment of the *recording* is far preferable to risking an
// audible dropout in the *live output*, which is the one thing that must never stall.
static void record_ring_push(RecordRing *ring, const float *data, size_t n) {
    if (n == 0) return;
    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);
    size_t used = w - r;
    size_t free_space = RECORD_RING_CAPACITY - used;
    if (n > free_space) {
        atomic_fetch_add_explicit(&ring->dropped_samples, n, memory_order_relaxed);
        return;
    }
    for (size_t i = 0; i < n; i++) ring->data[(w + i) & (RECORD_RING_CAPACITY - 1)] = data[i];
    atomic_store_explicit(&ring->write_idx, w + n, memory_order_release);
}

// Consumer side (writer thread only). Pops up to max_n samples into out[], returns how many were
// actually available. Never blocks -- if the ring is empty, returns 0 immediately.
static size_t record_ring_pop(RecordRing *ring, float *out, size_t max_n) {
    size_t r = atomic_load_explicit(&ring->read_idx, memory_order_relaxed);
    size_t w = atomic_load_explicit(&ring->write_idx, memory_order_acquire);
    size_t available = w - r;
    size_t n = available < max_n ? available : max_n;
    for (size_t i = 0; i < n; i++) out[i] = ring->data[(r + i) & (RECORD_RING_CAPACITY - 1)];
    atomic_store_explicit(&ring->read_idx, r + n, memory_order_release);
    return n;
}

static void wav_write_u32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; fwrite(b, 1, 4, f); }
static void wav_write_u16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v>>8) }; fwrite(b, 1, 2, f); }

// Pure arithmetic, no I/O -- how many channels a given selection resolves to (0 means "nothing
// selected"). Used by the audio thread (to size its gather buffer synchronously, before the
// writer thread has even seen the start request) and by the writer thread's actual file-open
// (wav_recorder_start below), so both sides are guaranteed to agree on the channel count without
// either one having to wait on the other.
static int wav_recorder_compute_channels(bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    int selected = include_synth_output ? 2 : 0;
    for (int c = 0; c < input_channel_count; c++) if (input_channel_enable[c]) selected++;
    if (selected <= 0) return 0;
    return downmix_stereo ? 2 : selected;
}

// Computes the selected channel count and freezes the selection into *rec, opens the WAV file,
// and writes its header. Returns false (leaving *rec untouched, still inactive) if nothing is
// selected or the file can't be opened -- the caller should treat either as "did not start."
// Writer-thread-only: performs blocking file I/O.
static bool wav_recorder_start(WavRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count) {
    int channels = wav_recorder_compute_channels(include_synth_output, downmix_stereo, input_channel_enable, input_channel_count);
    if (channels <= 0) { fprintf(stderr, "[record] cannot start: no channels selected\n"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[record] failed to open '%s' for writing: %s\n", path, strerror(errno)); return false; }

    fwrite("RIFF", 1, 4, f); wav_write_u32(f, 0); // total size, patched on stop
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f); wav_write_u32(f, 18); // fmt chunk body size (extended, cbSize=0)
    wav_write_u16(f, 3);                            // wFormatTag: IEEE float
    wav_write_u16(f, (uint16_t)channels);
    wav_write_u32(f, SAMPLE_RATE);
    wav_write_u32(f, (uint32_t)(SAMPLE_RATE * channels * 4)); // avg bytes/sec
    wav_write_u16(f, (uint16_t)(channels * 4));                // block align
    wav_write_u16(f, 32);                                       // bits per sample
    wav_write_u16(f, 0);                                        // cbSize

    fwrite("fact", 1, 4, f); wav_write_u32(f, 4);
    rec->fact_count_offset = ftell(f);
    wav_write_u32(f, 0); // total frames, patched on stop

    fwrite("data", 1, 4, f);
    rec->data_size_offset = ftell(f);
    wav_write_u32(f, 0); // data byte count, patched on stop

    rec->file = f;
    rec->channels = channels;
    rec->frames_written = 0;
    rec->active = true;
    rec->sel_include_synth_output = include_synth_output;
    rec->sel_downmix_stereo = downmix_stereo;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) rec->sel_input_channel[c] = (c < input_channel_count) && input_channel_enable[c];
    rec->sel_input_channel_count = input_channel_count;
    return true;
}

static void wav_recorder_write(WavRecorder *rec, const float *interleaved, int num_frames) {
    if (!rec->active || num_frames <= 0) return;
    fwrite(interleaved, sizeof(float), (size_t)num_frames * (size_t)rec->channels, rec->file);
    rec->frames_written += (uint32_t)num_frames;
}

static void wav_recorder_stop(WavRecorder *rec) {
    if (!rec->active) return;
    long final_size = ftell(rec->file);
    uint32_t data_bytes = rec->frames_written * (uint32_t)rec->channels * 4;

    fseek(rec->file, 4, SEEK_SET);
    wav_write_u32(rec->file, (uint32_t)(final_size - 8)); // RIFF chunk size excludes "RIFF"+size itself

    fseek(rec->file, rec->fact_count_offset, SEEK_SET);
    wav_write_u32(rec->file, rec->frames_written);

    fseek(rec->file, rec->data_size_offset, SEEK_SET);
    wav_write_u32(rec->file, data_bytes);

    fclose(rec->file);
    rec->file = NULL;
    rec->active = false;
    fprintf(stderr, "[record] stopped: %u frames (%.2fs), %d channel(s)\n", rec->frames_written, rec->frames_written / (float)SAMPLE_RATE, rec->channels);
}

// --- WavPack recording: an additional lossless (or, in Hybrid mode, lossy-with-an-optional-
// lossless-correction-file) format alongside WAV, reusing the exact same RecordRing/writer-thread
// architecture unchanged -- same wait-free push from the audio thread, same writer-thread-only
// file I/O discipline as WavRecorder above. WavPack (like FLAC) losslessly compresses integer PCM,
// not float, so incoming float samples are converted to 24-bit (packed in int32_t) just before
// handing them to the encoder. Uses libwavpack's callback-based block output
// (WavpackOpenFileOutput) rather than the RIFF-style open-now/backpatch-on-stop approach WAV
// uses -- WavPack's own format has no equivalent backpatch step, so the encoder is simply flushed
// and closed on stop.
//
// Below the real implementation is a small stub with the identical signatures/struct shape (used
// only if HDS_WAVPACK_ENABLED is ever flipped back to 0, e.g. building on a system with no
// libwavpack-dev, or on a platform like Android that never defines it at all -- see this file's
// own header comment), so every call site (record_writer_drain, record_writer_thread_main) needs
// zero changes either way. ---
#if HDS_WAVPACK_ENABLED
typedef struct {
    WavpackContext *ctx;
    FILE *file;
    FILE *file_wvc; // the optional lossless correction file -- see wavpack_recorder_start()'s own comment on hybrid/correction_file; NULL whenever one wasn't requested
    int channels;
    uint32_t frames_written;
    bool active;
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count;
} WavPackRecorder;

// libwavpack's WavpackBlockOutput callback: just appends the encoder's already-formatted block
// bytes to whichever file `id` names -- the SAME callback serves both the main .wv output and the
// optional .wvc correction-file output (WavpackOpenFileOutput takes one callback but two separate
// `id`s, one per stream; the encoder itself decides which one a given block belongs to). Writer-
// thread-only (the encoder calls this synchronously from inside WavpackPackSamples/
// WavpackFlushSamples, both called only from the writer thread).
static int wavpack_block_output(void *id, void *data, int32_t bcount) {
    FILE *f = (FILE *)id;
    return fwrite(data, 1, (size_t)bcount, f) == (size_t)bcount;
}

// hybrid/correction_file/hybrid_bitrate/compression_level mirror SynthSharedControls'
// record_wavpack_* fields exactly -- see their own comments in backend.c for what each means and
// why "lossy/hybrid vs. lossless" collapses to the single `hybrid` bool. compression_level is
// 0=Fast/1=Normal/2=High/3=Very High (anything else clamps to Normal).
static bool wavpack_recorder_start(WavPackRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count,
                                    bool hybrid, bool correction_file, float hybrid_bitrate, int compression_level) {
    int channels = wav_recorder_compute_channels(include_synth_output, downmix_stereo, input_channel_enable, input_channel_count);
    if (channels <= 0) { fprintf(stderr, "[record] cannot start (WavPack): no channels selected\n"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[record] failed to open '%s' for writing: %s\n", path, strerror(errno)); return false; }

    // A correction file only makes sense alongside Hybrid mode (see record_wavpack_correction_file's
    // own comment) -- named exactly like WavPack's own reference CLI does, the same base path with
    // a trailing 'c' appended to the extension ("foo.wv" -> "foo.wvc").
    bool want_wvc = hybrid && correction_file;
    FILE *f_wvc = NULL;
    if (want_wvc) {
        char wvc_path[RECORD_PATH_LEN + 1];
        // %.*s bounds the copied path explicitly to RECORD_PATH_LEN-1 chars -- same reasoning as
        // hds_recording_generate_path()'s own comment in hds_clap_recording_draw.c: without this,
        // GCC can't rule out the trailing 'c' overflowing a same-sized buffer (path itself is
        // already <= RECORD_PATH_LEN-1 chars by construction, so this never actually truncates).
        snprintf(wvc_path, sizeof(wvc_path), "%.*sc", RECORD_PATH_LEN - 1, path);
        f_wvc = fopen(wvc_path, "wb");
        if (!f_wvc) { fprintf(stderr, "[record] failed to open '%s' for writing: %s\n", wvc_path, strerror(errno)); fclose(f); return false; }
    }

    WavpackContext *ctx = WavpackOpenFileOutput(wavpack_block_output, f, f_wvc);
    if (!ctx) { fprintf(stderr, "[record] WavpackOpenFileOutput failed\n"); fclose(f); if (f_wvc) fclose(f_wvc); return false; }

    WavpackConfig config; memset(&config, 0, sizeof(config));
    config.bytes_per_sample = 3;   // 24-bit PCM, packed 3 bytes/sample into each int32_t
    config.bits_per_sample = 24;
    config.num_channels = channels;
    config.sample_rate = SAMPLE_RATE;

    if (hybrid) {
        config.flags |= CONFIG_HYBRID_FLAG;
        config.bitrate = fmaxf(2.0f, fminf(24.0f, hybrid_bitrate));
        if (want_wvc) config.flags |= CONFIG_CREATE_WVC | CONFIG_OPTIMIZE_WVC; // CONFIG_OPTIMIZE_WVC: matches the reference CLI's own "-c" behavior, see this effect's own comment in backend.c
    }
    switch (compression_level) {
        case 0: config.flags |= CONFIG_FAST_FLAG; break;
        case 2: config.flags |= CONFIG_HIGH_FLAG; break;
        case 3: config.flags |= CONFIG_VERY_HIGH_FLAG; break;
        default: break; // 1 (Normal) or anything unrecognized -- WavPack's own default, no flag needed
    }

    // (uint32_t)-1 total_samples signals "length not known in advance" (this is a live, streamed
    // recording of unknown duration) -- WavPack supports this the same way its CLI does when
    // encoding from a pipe/stdin.
    if (!WavpackSetConfiguration(ctx, &config, (uint32_t)-1) || !WavpackPackInit(ctx)) {
        fprintf(stderr, "[record] WavPack configuration/init failed: %s\n", WavpackGetErrorMessage(ctx));
        WavpackCloseFile(ctx); fclose(f); if (f_wvc) fclose(f_wvc); return false;
    }

    rec->ctx = ctx;
    rec->file = f;
    rec->file_wvc = f_wvc;
    rec->channels = channels;
    rec->frames_written = 0;
    rec->active = true;
    rec->sel_include_synth_output = include_synth_output;
    rec->sel_downmix_stereo = downmix_stereo;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) rec->sel_input_channel[c] = (c < input_channel_count) && input_channel_enable[c];
    rec->sel_input_channel_count = input_channel_count;
    return true;
}

static void wavpack_recorder_write(WavPackRecorder *rec, const float *interleaved, int num_frames) {
    if (!rec->active || num_frames <= 0) return;
    static int32_t pcm_buf[8192];
    int total_values = num_frames * rec->channels;
    int max_values = (int)(sizeof(pcm_buf) / sizeof(pcm_buf[0]));
    // Chunk defensively in case a single drain hands us more than pcm_buf can hold at once (the
    // writer thread's own pop_buf is already this size, so in practice this never loops more
    // than once, but there's no reason to assume that ordering holds forever).
    int offset = 0;
    while (offset < total_values) {
        int chunk_values = total_values - offset; if (chunk_values > max_values) chunk_values = max_values;
        // Round down to a whole number of frames so a chunk boundary never splits one frame's
        // channels across two WavpackPackSamples calls.
        chunk_values -= chunk_values % rec->channels;
        if (chunk_values <= 0) break;
        for (int i = 0; i < chunk_values; i++) {
            float s = interleaved[offset + i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            pcm_buf[i] = (int32_t)(s * 8388607.0f); // 24-bit signed full scale (2^23 - 1)
        }
        WavpackPackSamples(rec->ctx, pcm_buf, (uint32_t)(chunk_values / rec->channels));
        offset += chunk_values;
    }
    rec->frames_written += (uint32_t)num_frames;
}

static void wavpack_recorder_stop(WavPackRecorder *rec) {
    if (!rec->active) return;
    WavpackFlushSamples(rec->ctx);
    WavpackCloseFile(rec->ctx); // frees the WavpackContext only -- both FILE*s below are ours to close, same as WAV's own fclose after its backpatch
    rec->ctx = NULL;
    fclose(rec->file);
    rec->file = NULL;
    if (rec->file_wvc) { fclose(rec->file_wvc); rec->file_wvc = NULL; }
    rec->active = false;
    fprintf(stderr, "[record] stopped (WavPack): %u frames (%.2fs), %d channel(s)\n", rec->frames_written, rec->frames_written / (float)SAMPLE_RATE, rec->channels);
}
#else // !HDS_WAVPACK_ENABLED -- stub with the same shape, never reached from the frontend since
      // its format radio button is disabled, but wavpack_recorder_start() fails loudly (not
      // silently) if ever reached directly via raw OSC.
typedef struct {
    int channels;
    uint32_t frames_written;
    bool active;
} WavPackRecorder;
static bool wavpack_recorder_start(WavPackRecorder *rec, const char *path, bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count,
                                    bool hybrid, bool correction_file, float hybrid_bitrate, int compression_level) {
    (void)rec; (void)path; (void)include_synth_output; (void)downmix_stereo; (void)input_channel_enable; (void)input_channel_count;
    (void)hybrid; (void)correction_file; (void)hybrid_bitrate; (void)compression_level;
    fprintf(stderr, "[record] WavPack support is currently disabled (HDS_WAVPACK_ENABLED=0) -- recording not started\n");
    return false;
}
static void wavpack_recorder_write(WavPackRecorder *rec, const float *interleaved, int num_frames) { (void)rec; (void)interleaved; (void)num_frames; }
static void wavpack_recorder_stop(WavPackRecorder *rec) { (void)rec; }
#endif

// Audio-thread-side mirror of a WavRecorder's selection (NOT its file state -- the audio thread
// never touches a FILE*). Set synchronously on the record-start edge from the same live knobs
// the writer thread's copy is derived from, via the identical wav_recorder_compute_channels(), so
// the two sides can never disagree about the channel count despite computing it independently.
typedef struct {
    bool active;
    bool sel_include_synth_output;
    bool sel_downmix_stereo;
    bool sel_input_channel[RECORD_MAX_CHANNELS];
    int sel_input_channel_count;
    int channels;
} RecordGatherState;

typedef enum { RECORD_CMD_NONE = 0, RECORD_CMD_START, RECORD_CMD_STOP } RecordCommand;

// Cross-thread interface between the audio thread and the dedicated recording writer thread.
// The audio thread only ever: (1) writes cmd_* fields then publishes `command` with a release
// store, (2) pushes gathered audio into `ring` (wait-free), (3) reads the is_active/start_failed/
// frames_written status fields for the GUI's EngineStatus feedback. Everything else --
// wav_recorder_start/write/stop and all their file I/O -- happens only on the writer thread.
typedef struct {
    RecordRing ring;
    pthread_t thread;
    _Atomic int command;   // RecordCommand
    _Atomic bool shutdown;

    // Parameters for a pending START, valid once `command` is observed as RECORD_CMD_START by
    // the writer thread. Plain (non-atomic) fields are safe here because the release store to
    // `command` (audio thread) paired with the acquire exchange of `command` (writer thread)
    // establishes a happens-before edge covering everything written before that store.
    char cmd_path[RECORD_PATH_LEN];
    bool cmd_include_synth_output;
    bool cmd_downmix_stereo;
    bool cmd_input_channel_enable[RECORD_MAX_CHANNELS];
    int cmd_input_channel_count;
    int cmd_record_format; // RecordFormat: 0=WAV, 1=WavPack
    // WavPack-only encoding options, frozen at the same record-start edge as everything else
    // above -- see record_wavpack_hybrid/correction_file/hybrid_bitrate/compression_level's own
    // comments in backend.c. Harmless, unread when cmd_record_format is WAV.
    bool cmd_wavpack_hybrid;
    bool cmd_wavpack_correction_file;
    float cmd_wavpack_hybrid_bitrate;
    int cmd_wavpack_compression_level;

    // Writer thread -> audio thread status, polled for the GUI's EngineStatus feedback.
    _Atomic bool is_active;
    _Atomic bool start_failed;
    _Atomic uint32_t frames_written;
} RecordEngine;

// Promoted to file scope (was a local `static`/plain local inside main() until the Recording tab's
// own native-GUI port needed to read them too -- see hds_clap_recording_draw.c's own header
// comment): record_engine already had static storage duration either way (its own ring buffer is
// far too large for main()'s stack, see main()'s own comment at its declaration site), so this is
// purely a visibility change, not a lifetime one. record_gather didn't strictly need static storage
// inside main() itself (a plain local declared once before an infinite loop already keeps its value
// across iterations), but needs file scope for the exact same cross-function-visibility reason.
static RecordEngine record_engine;
static RecordGatherState record_gather;

// Gathers one block's worth of frames from the synth's own post-FX stereo output and the raw
// N-channel capture buffer, per the SELECTION FROZEN AT RECORD-START (sel->sel_* -- not the live
// knobs, so a checkbox toggled mid-take can't desync the interleaving the writer thread's file
// header already committed to), and pushes the result into the ring for the writer thread to
// actually write to disk. `capture_buffer`'s actual stride must still match
// sel->sel_input_channel_count -- the caller is responsible for stopping the recording first if
// the live capture device's channel count has changed since it started (see the safety check in
// main()/hds_android.c's own callback). Pure arithmetic plus a wait-free ring push -- no I/O, safe
// to call every block from the audio thread.
static void wav_recorder_gather_block(RecordGatherState *sel, RecordEngine *eng, const float *synth_buffer, int synth_channels, const float *capture_buffer, int block_size) {
    if (!sel->active) return;
    static float frame_buf[BUFFER_SIZE * RECORD_MAX_CHANNELS];
    int stride = sel->sel_input_channel_count;
    if (sel->sel_downmix_stereo) {
        // n = how many signals get summed into EACH side (not the total signal count): the
        // synth output contributes exactly one signal per side (synth_L into L, synth_R into
        // R -- never both into the same side), and every selected raw input channel (mono)
        // contributes one signal to BOTH sides. So "Synth Output only" is a pure passthrough
        // (n=1, scale=1.0, no attenuation) rather than needlessly getting -3dB as if two
        // signals were being summed on each side when only one actually is.
        int n = sel->sel_include_synth_output ? 1 : 0;
        for (int c = 0; c < stride; c++) if (sel->sel_input_channel[c]) n++;
        float scale = (n > 0) ? (1.0f / sqrtf((float)n)) : 1.0f;
        for (int i = 0; i < block_size; i++) {
            float l = 0.0f, r = 0.0f;
            if (sel->sel_include_synth_output) { l += synth_buffer[i*synth_channels]; r += synth_buffer[i*synth_channels+1]; }
            for (int c = 0; c < stride; c++) {
                if (sel->sel_input_channel[c]) { float s = capture_buffer[i*stride + c]; l += s; r += s; }
            }
            frame_buf[i*2] = l * scale;
            frame_buf[i*2+1] = r * scale;
        }
    } else {
        for (int i = 0; i < block_size; i++) {
            int ch = 0;
            if (sel->sel_include_synth_output) {
                frame_buf[i*sel->channels + ch++] = synth_buffer[i*synth_channels];
                frame_buf[i*sel->channels + ch++] = synth_buffer[i*synth_channels+1];
            }
            for (int c = 0; c < stride; c++) {
                if (sel->sel_input_channel[c]) frame_buf[i*sel->channels + ch++] = capture_buffer[i*stride + c];
            }
        }
    }
    record_ring_push(&eng->ring, frame_buf, (size_t)block_size * (size_t)sel->channels);
}

// Pops and writes everything currently available in the ring to whichever recorder is active
// (at most one of rec/wprec is ever active at a time -- only one file is open per take) -- or
// just discards it (if neither is active; e.g. a START is still in flight, or the recording
// already stopped) so the ring's indices keep moving and a producer that's slightly ahead of us
// doesn't spuriously see "full". Writer-thread-only.
static void record_writer_drain(RecordEngine *eng, WavRecorder *rec, WavPackRecorder *wprec) {
    static float pop_buf[8192];
    int channels = rec->active ? rec->channels : (wprec->active ? wprec->channels : 0);
    if (channels > 0) {
        size_t chunk_frames = (sizeof(pop_buf) / sizeof(float)) / (size_t)channels;
        size_t max_floats = chunk_frames * (size_t)channels;
        size_t n;
        while ((n = record_ring_pop(&eng->ring, pop_buf, max_floats)) > 0) {
            int num_frames = (int)(n / (size_t)channels);
            if (rec->active) { wav_recorder_write(rec, pop_buf, num_frames); atomic_store_explicit(&eng->frames_written, rec->frames_written, memory_order_relaxed); }
            else { wavpack_recorder_write(wprec, pop_buf, num_frames); atomic_store_explicit(&eng->frames_written, wprec->frames_written, memory_order_relaxed); }
        }
    } else {
        while (record_ring_pop(&eng->ring, pop_buf, sizeof(pop_buf) / sizeof(float)) > 0) {}
    }
}

// Entry point for the dedicated recording writer thread -- owns the ONLY WavRecorder/
// WavPackRecorder instances that ever touch a real FILE*. Polls for a command every
// RECORD_WRITER_POLL_MS via nanosleep (rather than being condvar-signaled from the audio thread)
// specifically to avoid putting even a brief mutex/futex wait on the audio thread's side of the
// handshake; the audio thread's only synchronization primitives are the wait-free ring and plain
// atomic loads/stores.
#define RECORD_WRITER_POLL_MS 5
static void *record_writer_thread_main(void *arg) {
    RecordEngine *eng = (RecordEngine *)arg;
    WavRecorder rec = {0};
    WavPackRecorder wprec = {0};

    for (;;) {
        int cmd = atomic_exchange_explicit(&eng->command, RECORD_CMD_NONE, memory_order_acquire);
        if (cmd == RECORD_CMD_START) {
            if (rec.active) wav_recorder_stop(&rec); // shouldn't happen, but never leak a handle
            if (wprec.active) wavpack_recorder_stop(&wprec);
            bool ok;
            if (eng->cmd_record_format == 1) {
                ok = wavpack_recorder_start(&wprec, eng->cmd_path, eng->cmd_include_synth_output, eng->cmd_downmix_stereo,
                                             eng->cmd_input_channel_enable, eng->cmd_input_channel_count,
                                             eng->cmd_wavpack_hybrid, eng->cmd_wavpack_correction_file,
                                             eng->cmd_wavpack_hybrid_bitrate, eng->cmd_wavpack_compression_level);
            } else {
                ok = wav_recorder_start(&rec, eng->cmd_path, eng->cmd_include_synth_output, eng->cmd_downmix_stereo,
                                         eng->cmd_input_channel_enable, eng->cmd_input_channel_count);
            }
            atomic_store_explicit(&eng->frames_written, 0, memory_order_relaxed);
            if (!ok) atomic_store_explicit(&eng->start_failed, true, memory_order_relaxed);
            atomic_store_explicit(&eng->is_active, rec.active || wprec.active, memory_order_release);
        } else if (cmd == RECORD_CMD_STOP) {
            if (rec.active) wav_recorder_stop(&rec);
            if (wprec.active) wavpack_recorder_stop(&wprec);
            atomic_store_explicit(&eng->is_active, false, memory_order_release);
        }

        record_writer_drain(eng, &rec, &wprec);

        bool shutdown_requested = atomic_load_explicit(&eng->shutdown, memory_order_acquire);
        if (shutdown_requested && atomic_load_explicit(&eng->command, memory_order_acquire) == RECORD_CMD_NONE) {
            record_writer_drain(eng, &rec, &wprec); // final catch-all in case the audio thread pushed between our drain and observing shutdown
            if (rec.active) wav_recorder_stop(&rec);
            if (wprec.active) wavpack_recorder_stop(&wprec);
            break;
        }

        struct timespec poll_delay = { .tv_sec = 0, .tv_nsec = RECORD_WRITER_POLL_MS * 1000L * 1000L };
        nanosleep(&poll_delay, NULL);
    }
    return NULL;
}
