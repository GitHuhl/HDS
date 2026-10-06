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
#include <pthread.h>

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

// Guards every call into the engine (dispatch_midi_event and render_audio_block alike) against the
// one real concurrency hazard here: Android's MIDI framework delivers each message on its own
// Binder callback thread, completely independent of the AAudio data-callback thread that calls
// render_audio_block(). hds_standalone.c's own main loop never needs this because MIDI decode and
// render happen back-to-back on the SAME thread every iteration (see its own process_midi_input()
// call sitting directly above render_audio_block() in that file) -- that invariant doesn't hold
// here, so this takes its place. Coarse-grained on purpose: this build exists to answer "does the
// engine make correct sound from a real keyboard", not to be real-time-safe yet.
static pthread_mutex_t g_engine_lock = PTHREAD_MUTEX_INITIALIZER;

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

// Dispatches one fully-decoded channel voice message. channel_1_16 matches dispatch_midi_event's
// own 1-16 convention (see its callers in hds_standalone.c, which add 1 to ALSA's 0-15 channel).
static void hds_android_dispatch_decoded(unsigned char status, unsigned char d1, unsigned char d2) {
    int channel_1_16 = (status & 0x0F) + 1;
    unsigned char hi = status & 0xF0;
    pthread_mutex_lock(&g_engine_lock);
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
    pthread_mutex_unlock(&g_engine_lock);
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
            hds_android_dispatch_decoded(g_running_status, g_pending_data[0],
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
static aaudio_data_callback_result_t hds_android_audio_callback(
    AAudioStream *stream, void *userData, void *audioData, int32_t numFrames) {
    (void)stream; (void)userData;
    float *out = (float *)audioData;
    int frames_to_render = numFrames;
    if (frames_to_render > BUFFER_SIZE) frames_to_render = BUFFER_SIZE; // render_audio_block's own hard ceiling (effective_block_size() clamps to this)
    pthread_mutex_lock(&g_engine_lock);
    g_knobs.block_size_frames = (float)frames_to_render;
    render_audio_block(&g_sample_engine, &g_engine, g_voices, &g_knobs, g_global_lfo_phases,
                        g_ext_in_silence, 2, 2, &g_midi_live, out, 120.0f,
                        g_ext_in_peak, NULL, g_fx_gr_db, g_fx_dump_avail_sec);
    pthread_mutex_unlock(&g_engine_lock);
    if (numFrames > frames_to_render) {
        // AAudio asked for more than render_audio_block's hard per-block ceiling can produce in one
        // call (shouldn't happen at any realistic buffer size, but silence, never garbage, for the
        // remainder if it ever does).
        memset(out + (size_t)frames_to_render * 2, 0, (size_t)(numFrames - frames_to_render) * 2 * sizeof(float));
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// -----------------------------------------------------------------------------------------------
// JNI entry points. Package/class match MainActivity.kt's own `NativeBridge` object -- see
// android/app/src/main/java/.../NativeBridge.kt.

JNIEXPORT jboolean JNICALL
Java_com_hds_androidtest_NativeBridge_nativeInit(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    hds_android_engine_init();

    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return JNI_FALSE;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, hds_android_audio_callback, NULL);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &g_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || g_stream == NULL) return JNI_FALSE;

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
