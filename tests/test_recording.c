// test_recording.c -- records a 1 s stereo sine through recording_engine.o as WAV and as WavPack,
// then reads both files back and checks they hold exactly the samples that were recorded (bit-exact
// float for WAV; for WavPack, which stores 24-bit integer PCM, the exact 24-bit conversion).
#include "test_common.h"
#include "recording_engine.h"

#include <stdlib.h>
#include <time.h>
#include <wavpack/wavpack.h>

static void sleep_ms(int ms) { struct timespec t = {0, ms * 1000000L}; nanosleep(&t, NULL); }

#define BLOCK 256
#define BLOCKS (SAMPLE_RATE / BLOCK)
#define FRAMES (BLOCKS * BLOCK)
static float g_expected[FRAMES * 2];

static int take(const char *path, int format) {
    record_engine.cmd_include_synth_output = true; record_engine.cmd_downmix_stereo = false;
    record_engine.cmd_input_channel_count = 0; snprintf(record_engine.cmd_path, RECORD_PATH_LEN, "%s", path);
    record_engine.cmd_record_format = format; record_engine.cmd_wavpack_compression_level = 1;
    atomic_store(&record_engine.start_failed, false);
    atomic_store(&record_engine.command, RECORD_CMD_START);
    for (int i = 0; !atomic_load(&record_engine.is_active); i++) {
        if (atomic_load(&record_engine.start_failed) || i > 2000) return -1;
        sleep_ms(1);
    }
    record_gather = (RecordGatherState){ .active = true, .sel_include_synth_output = true,
                                         .channels = wav_recorder_compute_channels(true, false, NULL, 0) };
    for (int b = 0; b < BLOCKS; b++) wav_recorder_gather_block(&record_gather, &record_engine, &g_expected[b * BLOCK * 2], 2, NULL, BLOCK);
    record_gather.active = false;
    sleep_ms(50);
    atomic_store(&record_engine.command, RECORD_CMD_STOP);
    for (int i = 0; atomic_load(&record_engine.is_active); i++) { if (i > 2000) return -1; sleep_ms(1); }
    return (int)atomic_load(&record_engine.frames_written);
}

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void check_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { test_check(0, "recording/wav/open", "cannot open %s", path); return; }
    static unsigned char buf[FRAMES * 8 + 1024];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    int riff = n > 44 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4);
    test_check(riff, "recording/wav/header", "not a RIFF/WAVE file");
    if (!riff) return;
    size_t pos = 12; int fmt_ok = 0; const unsigned char *data = NULL; uint32_t data_len = 0;
    while (pos + 8 <= n) {
        uint32_t len = rd32(buf + pos + 4);
        if (!memcmp(buf + pos, "fmt ", 4)) fmt_ok = rd16(buf + pos + 8) == 3 && rd16(buf + pos + 10) == 2 && rd32(buf + pos + 12) == SAMPLE_RATE && rd16(buf + pos + 22) == 32;
        if (!memcmp(buf + pos, "data", 4)) { data = buf + pos + 8; data_len = len; }
        pos += 8 + len + (len & 1);
    }
    test_check(fmt_ok, "recording/wav/format", "expected 32-bit float, 2 channels, %d Hz", SAMPLE_RATE);
    test_check(data && data_len == sizeof g_expected, "recording/wav/length", "data chunk is %u bytes, expected %zu", data_len, sizeof g_expected);
    if (data && data_len == sizeof g_expected)
        test_check(!memcmp(data, g_expected, sizeof g_expected), "recording/wav/samples", "samples differ from what was recorded");
}

static void check_wavpack(const char *path) {
    char err[80];
    WavpackContext *wpc = WavpackOpenFileInput(path, err, OPEN_WVC, 0);
    if (!wpc) { test_check(0, "recording/wavpack/open", "%s", err); return; }
    int ch = WavpackGetNumChannels(wpc); uint32_t frames = WavpackGetNumSamples(wpc);
    test_check(ch == 2 && frames == FRAMES && WavpackGetSampleRate(wpc) == SAMPLE_RATE, "recording/wavpack/format",
               "got %d ch, %u frames, %u Hz", ch, frames, WavpackGetSampleRate(wpc));
    static int32_t raw[FRAMES * 2];
    uint32_t got = WavpackUnpackSamples(wpc, raw, FRAMES);
    int bits = WavpackGetBitsPerSample(wpc);
    WavpackCloseFile(wpc);
    test_check(got == FRAMES, "recording/wavpack/decode", "decoded %u of %d frames", got, FRAMES);
    test_check(bits == 24, "recording/wavpack/bits", "%d bits per sample, expected 24", bits);
    // The recorder stores 24-bit integer PCM: each float clamped to [-1, 1] and scaled by 2^23 - 1.
    int wrong = 0;
    for (int i = 0; got == FRAMES && i < FRAMES * 2; i++) {
        float s = g_expected[i]; if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        if (raw[i] != (int32_t)(s * 8388607.0f)) wrong++;
    }
    if (got == FRAMES) test_check(wrong == 0, "recording/wavpack/lossless", "%d decoded samples differ from the recorded 24-bit values", wrong);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    double ph = 0;
    for (int i = 0; i < FRAMES; i++) { g_expected[2 * i] = g_expected[2 * i + 1] = 0.5f * (float)sin(ph); ph += 2 * M_PI * 440 / SAMPLE_RATE; }
    pthread_t th;
    if (pthread_create(&th, NULL, record_writer_thread_main, &record_engine) != 0) { puts("FAIL recording/thread"); return 1; }
    char wav[512], wv[512];
    snprintf(wav, sizeof wav, "%s/test_recording.wav", dir); snprintf(wv, sizeof wv, "%s/test_recording.wv", dir);
    int n1 = take(wav, 0), n2 = take(wv, 1);
    atomic_store(&record_engine.shutdown, true); pthread_join(th, NULL);
    test_check(n1 == FRAMES, "recording/wav/frames_written", "%d frames, expected %d", n1, FRAMES);
    test_check(n2 == FRAMES, "recording/wavpack/frames_written", "%d frames, expected %d", n2, FRAMES);
    check_wav(wav);
    check_wavpack(wv);
    return test_exit_code();
}
