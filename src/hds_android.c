// src/hds_android.c -- Android NDK entry points for the HDS DSP engine, written to answer one
// question first: does the actual synth engine (backend.c/fx_chain.c/sequencer.c) produce correct
// audio on a real phone from a real MIDI keyboard. No GUI of any kind lives here -- see this
// project's own README for why the GUI is a separate, later decision. This file owns exactly two
// jobs: feed decoded MIDI events into dispatch_midi_event() (same function hds_standalone.c's own
// process_midi_input() calls, just fed from Android's MIDI framework instead of an ALSA sequencer
// port), and feed render_audio_block()'s output directly into an AAudio stream's own callback
// buffer -- no intermediate copy, no Java-side pixel/sample handling of any kind.
//
// Same single-translation-unit-per-binary convention clap_plugin.c/hds_fx_plugin.c already use:
// #include backend.c directly (which itself #includes fx_types.h/fx_chain.c -- see backend.c's own
// header) rather than linking a separate object, so every type/function below is the real one, not
// a reimplementation.
//
// What this deliberately has NO opinion on yet: GUI (headless), OSC networking (backend.c still
// pulls in liblo via its own #include <lo/lo.h> -- that's a link-time dependency this file doesn't
// touch or start a server for), Sequencer/transport (current_tempo_bpm is a fixed 120.0f stand-in),
// external audio input (ext_in is fed silence, same convention hds_standalone.c's own capture_buffer
// falls back to when no capture device is open).
#include "backend.c"

#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <stdatomic.h>

#define HDS_LOG_TAG "hds_native"
#define HDS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HDS_LOG_TAG, __VA_ARGS__)
#define HDS_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HDS_LOG_TAG, __VA_ARGS__)

// -----------------------------------------------------------------------------------------------
// Engine state -- one process-wide instance, same ownership shape as hds_standalone.c's own
// g_sample_engine/g_engine globals (g_engine itself is already declared static inside backend.c --
// see its own comment there -- so only the pieces backend.c does NOT already own for us are
// declared here, mirroring hds_standalone.c lines ~219-233 exactly).
static HdsSampleSlotState g_android_sample_slot_state;
static HdsPendingLoadState g_android_pending_load_state;
static HdsSampleEngine g_sample_engine = { &g_android_sample_slot_state, &g_streams_state, &g_android_pending_load_state };

static SynthSharedControls g_knobs;
static SynthVoice g_voices[MAX_VOICES];
static float g_global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
static MonoNoteStack g_mono_stacks[COMBI_PROGRAM_COUNT + 1];
static MidiLiveState g_midi_live;
static float g_fx_gr_db[FX_SLOT_COUNT];
static float g_fx_dump_avail_sec[FX_SLOT_COUNT];
static float g_ext_in_peak[EXT_IN_PAIR_COUNT];
// Fed to render_audio_block as a permanently-silent 2-channel "external input" -- same fallback
// hds_standalone.c's own capture_buffer uses when no capture device is open, just permanent here
// since this build never opens one. Sized to the hard per-block ceiling, same convention as that
// file's own capture_buffer/buffer statics.
static float g_ext_in_silence[BUFFER_SIZE * 2];

static AAudioStream *g_stream = NULL;
static bool g_engine_initialized = false;

static void hds_android_engine_init(void) {
    if (g_engine_initialized) return;
    init_default_controls(&g_knobs);
    for (int i = 0; i < MAX_VOICES; i++) init_voice(&g_voices[i]);
    for (int p = 0; p <= COMBI_PROGRAM_COUNT; p++) g_mono_stacks[p] = (MonoNoteStack){0};
    memset(g_global_lfo_phases, 0, sizeof(g_global_lfo_phases));
    g_midi_live = (MidiLiveState){0};
    memset(g_fx_gr_db, 0, sizeof(g_fx_gr_db));
    memset(g_fx_dump_avail_sec, 0, sizeof(g_fx_dump_avail_sec));
    memset(g_ext_in_peak, 0, sizeof(g_ext_in_peak));
    memset(g_ext_in_silence, 0, sizeof(g_ext_in_silence));
    g_engine_initialized = true;
}

// -----------------------------------------------------------------------------------------------
// Raw MIDI 1.0 byte-stream parser. Android's MidiReceiver hands this file a raw byte buffer
// (android.media.midi does no decoding of its own -- that's the whole reason this exists);
// everything ALSA's own sequencer API already did for hds_standalone.c's process_midi_input()
// (status-byte decode, running status) has to happen by hand here instead. Deliberately stateful
// across calls (running_status survives between onMidiDataReceived calls) since a real keyboard is
// free to omit repeated status bytes exactly as MIDI 1.0 allows.
static unsigned char g_running_status = 0;
static unsigned char g_pending_data[2];
static int g_pending_data_needed = 0;
static int g_pending_data_have = 0;

// -----------------------------------------------------------------------------------------------
// Lock-free single-producer/single-consumer ring: Android's MIDI framework delivers each message
// on its own Binder callback thread, completely independent of the AAudio data-callback thread
// that calls render_audio_block() -- hds_standalone.c's own main loop never needs any
// cross-thread story at all, since its MIDI decode and render happen back-to-back on the SAME
// thread every iteration. An earlier version of this file bridged that gap with a plain
// pthread_mutex shared by both threads -- which turned out to be a real, reported bug, not a
// hypothetical one: holding any lock inside a realtime AAudio callback is a textbook
// priority-inversion hazard (if the MIDI thread is holding the lock -- however briefly -- exactly
// when the callback fires, the callback stalls waiting for it, and the stall eats directly into
// that callback's hard real-time deadline), and it matched the reported symptom exactly: audible
// stutter specifically when playing chords, i.e. exactly when several Note On messages arrive on
// the MIDI thread in the same few milliseconds. A true SPSC ring needs no lock at all: the MIDI
// thread only ever writes `head`, the audio thread only ever writes `tail`, and each side only
// ever READS the other's index -- so hds_midi_ring_push() (MIDI thread) and
// hds_midi_ring_drain_and_dispatch() (audio thread, called once at the top of each callback,
// before render_audio_block()) can never block on each other.
#define HDS_MIDI_RING_CAPACITY 256 // generous relative to realistic MIDI event rates -- a real
                                    // keyboard produces nowhere near 256 events between two
                                    // ~5-10ms audio callbacks; this is slack, not a tight budget
typedef struct { unsigned char status, d1, d2; } HdsMidiRingEvent;
static HdsMidiRingEvent g_midi_ring[HDS_MIDI_RING_CAPACITY];
static atomic_uint g_midi_ring_head = 0; // next slot the MIDI thread will write
static atomic_uint g_midi_ring_tail = 0; // next slot the audio thread will read

static void hds_android_dispatch_decoded_now(unsigned char status, unsigned char d1, unsigned char d2);

// Called from Android's MIDI Binder thread only (via hds_android_feed_midi_bytes below). Never
// blocks, never touches engine state directly -- just enqueues for the audio thread to apply.
static void hds_midi_ring_push(unsigned char status, unsigned char d1, unsigned char d2) {
    unsigned int head = atomic_load_explicit(&g_midi_ring_head, memory_order_relaxed);
    unsigned int next = (head + 1) % HDS_MIDI_RING_CAPACITY;
    if (next == atomic_load_explicit(&g_midi_ring_tail, memory_order_acquire)) {
        return; // ring genuinely full (never observed -- see capacity comment above) -- drop this one event rather than block the MIDI thread
    }
    g_midi_ring[head] = (HdsMidiRingEvent){ status, d1, d2 };
    atomic_store_explicit(&g_midi_ring_head, next, memory_order_release);
}

// Called from the AAudio callback thread only, once per callback, before render_audio_block() --
// drains everything queued since the previous callback and dispatches it via the same
// dispatch_midi_event() calls as before, just with no lock involved anywhere now, since only this
// one thread ever touches engine state at all.
static void hds_midi_ring_drain_and_dispatch(void) {
    unsigned int tail = atomic_load_explicit(&g_midi_ring_tail, memory_order_relaxed);
    unsigned int head = atomic_load_explicit(&g_midi_ring_head, memory_order_acquire);
    while (tail != head) {
        HdsMidiRingEvent ev = g_midi_ring[tail];
        hds_android_dispatch_decoded_now(ev.status, ev.d1, ev.d2);
        tail = (tail + 1) % HDS_MIDI_RING_CAPACITY;
    }
    atomic_store_explicit(&g_midi_ring_tail, tail, memory_order_release);
}

// Dispatches one fully-decoded channel voice message. channel_1_16 matches dispatch_midi_event's
// own 1-16 convention (see its callers in hds_standalone.c, which add 1 to ALSA's 0-15 channel).
// Only ever called from the audio callback thread (via hds_midi_ring_drain_and_dispatch above) --
// never directly from the MIDI thread, which only ever calls hds_midi_ring_push.
static void hds_android_dispatch_decoded_now(unsigned char status, unsigned char d1, unsigned char d2) {
    int channel_1_16 = (status & 0x0F) + 1;
    unsigned char hi = status & 0xF0;
    if (hi == 0x90) {
        // Note On velocity 0 is a Note Off in disguise -- standard MIDI 1.0 convention, and the
        // one case worth normalizing here since dispatch_midi_event has no velocity-0 special case
        // of its own (it isn't ALSA sequencer's problem to solve either -- snd_seq already hands
        // hds_standalone.c pre-decoded, already-normalized SND_SEQ_EVENT_NOTEON/OFF events).
        if (d2 == 0) {
            dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_NOTE_OFF, channel_1_16, d1, d2,
                                 g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
        } else {
            dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_NOTE_ON, channel_1_16, d1, d2,
                                 g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
        }
    } else if (hi == 0x80) {
        dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_NOTE_OFF, channel_1_16, d1, d2,
                             g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
    } else if (hi == 0xB0) {
        dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_CC, channel_1_16, d1, d2,
                             g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
    } else if (hi == 0xE0) {
        // 14-bit pitch bend, LSB first (d1) then MSB (d2), centered at 8192 -- dispatch_midi_event
        // itself does `value / 8192.0f` and expects 0 at center (see its own HDS_MIDI_PITCH_BEND
        // branch), matching ALSA sequencer's own already-centered convention exactly.
        int raw14 = ((int)d2 << 7) | (int)d1;
        dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_PITCH_BEND, channel_1_16, 0, raw14 - 8192,
                             g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
    } else if (hi == 0xD0) {
        dispatch_midi_event(&g_engine, &g_sample_engine, HDS_MIDI_CHANNEL_PRESSURE, channel_1_16, 0, d1,
                             g_voices, &g_knobs, g_global_lfo_phases, g_mono_stacks, &g_midi_live, 120.0f);
    }
    // Poly aftertouch (0xA0) and Program Change (0xC0) have no HdsMidiEventType yet -- dropped for
    // this first test build, same as this parser already drops System Common/Realtime below.
}

static void hds_android_feed_midi_bytes(const unsigned char *data, int length) {
    for (int i = 0; i < length; i++) {
        unsigned char b = data[i];
        if (b >= 0xF8) {
            // System Realtime (clock/start/stop/active-sensing/reset) -- single byte, never
            // touches running status, irrelevant to a basic note-in/audio-out test.
            continue;
        }
        if (b == 0xF0) { g_running_status = 0; continue; } // SysEx start -- not parsed, just skip bytes until F7
        if (b == 0xF7) { g_running_status = 0; continue; } // SysEx end
        if (g_running_status == 0xF0) continue; // inside a SysEx block -- keep discarding until F7 above
        if (b & 0x80) {
            unsigned char hi = b & 0xF0;
            if (hi == 0xC0 || hi == 0xD0) g_pending_data_needed = 1;      // Program Change / Channel Pressure: 1 data byte
            else if (hi >= 0x80 && hi <= 0xE0) g_pending_data_needed = 2; // Note On/Off, Poly AT, CC, Pitch Bend: 2 data bytes
            else { g_running_status = 0; continue; }                     // other System Common -- not handled
            g_running_status = b;
            g_pending_data_have = 0;
            continue;
        }
        if (g_running_status == 0 || g_running_status >= 0xF0) continue; // stray data byte with no live status
        g_pending_data[g_pending_data_have++] = b;
        if (g_pending_data_have >= g_pending_data_needed) {
            hds_midi_ring_push(g_running_status, g_pending_data[0],
                                g_pending_data_needed == 2 ? g_pending_data[1] : 0);
            g_pending_data_have = 0; // running status stays armed for the next message, per MIDI 1.0
        }
    }
}

// -----------------------------------------------------------------------------------------------
// AAudio output. Renders directly into the buffer AAudio itself hands this callback -- no Bitmap,
// no Java-side copy, no intermediate buffer of this file's own; see this file's own header comment
// for why that matters. audioData is already PCM_FLOAT, already interleaved stereo, because that's
// exactly what the stream was opened as below.
static long g_callback_count = 0; // diagnostic only -- paces the xrun log below to roughly once/sec

static aaudio_data_callback_result_t hds_android_audio_callback(
    AAudioStream *stream, void *userData, void *audioData, int32_t numFrames) {
    (void)userData;
    hds_midi_ring_drain_and_dispatch(); // apply everything the MIDI thread queued since the last callback -- no lock, see this file's own comment on the ring above
    float *out = (float *)audioData;
    int frames_to_render = numFrames;
    if (frames_to_render > BUFFER_SIZE) frames_to_render = BUFFER_SIZE; // render_audio_block's own hard ceiling (effective_block_size() clamps to this)
    g_knobs.block_size_frames = (float)frames_to_render;
    render_audio_block(&g_sample_engine, &g_engine, g_voices, &g_knobs, g_global_lfo_phases,
                        g_ext_in_silence, 2, 2, &g_midi_live, out, 120.0f,
                        g_ext_in_peak, NULL, g_fx_gr_db, g_fx_dump_avail_sec);
    if (numFrames > frames_to_render) {
        // AAudio asked for more than render_audio_block's hard per-block ceiling can produce in one
        // call (shouldn't happen at any realistic buffer size, but silence, never garbage, for the
        // remainder if it ever does).
        memset(out + (size_t)frames_to_render * 2, 0, (size_t)(numFrames - frames_to_render) * 2 * sizeof(float));
    }
    // Diagnostic only, paced to ~once/sec (not every callback -- logging itself isn't free and
    // this runs on the realtime thread): AAudioStream_getXRunCount() is a real, cheap, always-
    // available signal for "is the device itself glitching," independent of anything this file
    // can infer on its own -- check `adb logcat -s hds_native` for this if dropouts are ever
    // reported again, before assuming it's a NEW bug: a rising xrun count under heavy polyphony
    // means the callback (or the device's own buffer) is still too tight, not that something is
    // newly broken.
    g_callback_count++;
    int frames_per_sec_worth = SAMPLE_RATE / (frames_to_render > 0 ? frames_to_render : 1);
    if (frames_per_sec_worth > 0 && g_callback_count % frames_per_sec_worth == 0) {
        HDS_LOGI("xrun_count=%d callback=%d frames", AAudioStream_getXRunCount(stream), (int)numFrames);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// -----------------------------------------------------------------------------------------------
// JNI entry points. Package/class match MainActivity.kt's own `NativeBridge` object -- see
// android/app/src/main/java/.../NativeBridge.kt.

// Builds and opens a SHARED stream. Previously tried AAUDIO_SHARING_MODE_EXCLUSIVE first (the
// real dedicated-hardware path, in principle lower latency and more glitch-resistant than
// SHARED) -- reverted after real on-device testing made things WORSE, not better: dropouts went
// from "only under heavy chords" to "constantly," which points at EXCLUSIVE landing on a worse
// HAL path on that device than AudioFlinger's own SHARED mixer, not at anything this file's own
// code got wrong. SHARED is the one that was actually confirmed working (modulo the chord-stutter
// bug fixed elsewhere in this file); EXCLUSIVE is not worth the risk for a build whose whole
// point is proving basic correctness, not chasing the lowest possible latency.
static aaudio_result_t hds_android_try_open(AAudioStream **out_stream) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return result;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, hds_android_audio_callback, NULL);
    result = AAudioStreamBuilder_openStream(builder, out_stream);
    AAudioStreamBuilder_delete(builder);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_hds_androidtest_NativeBridge_nativeInit(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    hds_android_engine_init();

    aaudio_result_t result = hds_android_try_open(&g_stream);
    if (result != AAUDIO_OK || g_stream == NULL) {
        HDS_LOGW("SHARED open failed (result=%d)", (int)result);
        return JNI_FALSE;
    }

    // Maximize buffer headroom rather than picking a specific multiplier -- a previous version of
    // this code set a fixed 2x framesPerBurst, confirmed on real hardware to make dropouts WORSE
    // (constant, not just under heavy chords): 2x may well have been SMALLER than whatever buffer
    // size AAudio's own default already picked, silently shrinking the one thing that was already
    // working. AAudioStream_getBufferCapacityInFrames() is the real ceiling the stream was opened
    // with (not a guess); setting the buffer to exactly that maximizes glitch resistance, trading
    // away some latency -- the right tradeoff for a build whose whole point is proving basic
    // correctness, not chasing the lowest possible latency. getFramesPerBurst() is logged for
    // context only (not used to compute anything here anymore); setBufferSizeInFrames's return
    // value is the buffer size AAudio actually applied (it can still clamp), logged rather than
    // assumed.
    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(g_stream);
    int32_t buffer_capacity = AAudioStream_getBufferCapacityInFrames(g_stream);
    int32_t actual_buffer_frames = AAudioStream_setBufferSizeInFrames(g_stream, buffer_capacity);
    HDS_LOGI("opened: sharing=%d performance=%d framesPerBurst=%d bufferCapacity=%d bufferFrames=%d",
             (int)AAudioStream_getSharingMode(g_stream), (int)AAudioStream_getPerformanceMode(g_stream),
             (int)frames_per_burst, (int)buffer_capacity, (int)actual_buffer_frames);

    if (AAudioStream_requestStart(g_stream) != AAUDIO_OK) {
        AAudioStream_close(g_stream);
        g_stream = NULL;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeOnMidi(JNIEnv *env, jobject thiz, jbyteArray data, jint offset, jint length) {
    (void)thiz;
    // Plain-C JNI calling convention (this is a .c TU, not .cpp -- JNIEnv* is a pointer to a
    // pointer-to-struct-of-function-pointers here, unlike the env->Method() sugar C++ gets).
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (bytes) {
        hds_android_feed_midi_bytes((const unsigned char *)(bytes + offset), length);
        (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT); // JNI_ABORT: read-only pass-through, nothing to copy back
    }
}

JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeShutdown(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (g_stream) {
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = NULL;
    }
}
