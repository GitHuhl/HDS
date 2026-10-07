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

// WAV/WavPack recording engine (RecordRing/WavRecorder/WavPackRecorder/RecordEngine/
// RecordGatherState, the writer thread, record_engine/record_gather themselves) -- shared
// verbatim with hds_standalone.c, see that file's own include-site comment. HDS_WAVPACK_ENABLED is
// never defined in this translation unit (no wavpack cross-build for the NDK yet -- see
// CMakeLists.txt), so the preprocessor's "undefined == 0" rule alone selects recording_engine.c's
// WavPack STUB automatically; nothing here needs to say so explicitly. This file's own Recording
// section (further down, in the AAudio callback) drives record_engine/record_gather the same way
// hds_standalone.c's main loop does.
#include "recording_engine.c"

#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <stdatomic.h>

#define HDS_LOG_TAG "hds_native"
#define HDS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HDS_LOG_TAG, __VA_ARGS__)
#define HDS_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HDS_LOG_TAG, __VA_ARGS__)

// -----------------------------------------------------------------------------------------------
// Native GUI (HDS_ANDROID_GUI_ENABLED) -- the same Cairo-drawn tab bar hds.clap/the standalone
// app already embed, this time drawn into a SurfaceView's own ANativeWindow -- see
// hds_android_gui.c's own header comment for the real differences from hds_standalone_gui_x11.c.
// hds_fx_gui_host.h/hds_engine_instance.h have no Cairo/Android dependency of their own (plain
// struct definitions over backend.c's own types), same posture as hds_standalone.c's own
// identical include -- but unlike that file, THIS build has no headless mode at all to keep
// compiling when the GUI is left out, so every one of these includes (and the HdsClapPlugin
// g_gui_ctx declaration just below) is gated on HDS_ANDROID_GUI_ENABLED end-to-end, not
// unconditional the way hds_standalone.c's own are.
#if defined(HDS_ANDROID_GUI_ENABLED)
#include "../third_party/clap/include/clap/clap.h"
#include "hds_fx_gui_host.h"
#include "hds_engine_instance.h"
// Mirrors clap_plugin.c's/hds_standalone.c's own identical #define -- this host likewise has no
// flat N-port Input/Output concept (a slot's Input is which SOURCE feeds it), so SOURCE_COUNT is
// the nearest analogous count.
#define HDS_FX_PORT_COUNT SOURCE_COUNT
#include "hds_fx_gui_draw.c"
#include "hds_clap_play_draw.c"
#include "hds_clap_combi_draw.c"
#include "hds_clap_filters_draw.c"
#include "hds_clap_lfos_draw.c"
#include "hds_clap_envelopes_draw.c"
#include "hds_clap_routing_draw.c"
#include "hds_clap_devices_draw.c"
#include "hds_clap_global_draw.c"
// Now a REAL tab here, not a no-op include: hds_clap_recording_draw.c's own guard is
// defined(HDS_STANDALONE_GUI_ENABLED) || defined(HDS_ANDROID_GUI_ENABLED) (see that file's own
// header comment), and record_engine/record_gather/RecordEngine/RecordGatherState are already
// defined above via src/recording_engine.c -- this file's own Recording section (further down,
// in the AAudio callback) drives them exactly like hds_standalone.c's main loop does.
#include "hds_clap_recording_draw.c"
#include "hds_clap_gui_draw.c"
#include "hds_android_gui.c"
#endif

// -----------------------------------------------------------------------------------------------
// Engine state -- one process-wide instance, same ownership shape as hds_standalone.c's own
// g_sample_engine/g_engine globals (g_engine itself is already declared static inside backend.c --
// see its own comment there -- so only the pieces backend.c does NOT already own for us are
// declared here, mirroring hds_standalone.c lines ~219-233 exactly).
static HdsSampleSlotState g_android_sample_slot_state;
static HdsPendingLoadState g_android_pending_load_state;
static HdsSampleEngine g_sample_engine = { &g_android_sample_slot_state, &g_streams_state, &g_android_pending_load_state };

#if defined(HDS_ANDROID_GUI_ENABLED)
// g_gui_ctx is a real, live, process-wide HdsClapPlugin -- g_gui_ctx.knobs IS the actual state
// the audio engine reads every block, not a second copy, same design hds_standalone.c's own
// g_gui_ctx already established (see that file's own header comment). The #define aliases below
// make every EXISTING reference to g_knobs/g_fx_gr_db/g_fx_dump_avail_sec/g_ext_in_peak further
// down in this file (predating the GUI port, written against plain top-level globals) resolve to
// the real HdsClapPlugin fields the GUI draw files themselves read via `p->knobs`/`p->fx_gr_db`/
// etc, with zero edits to any of those existing call sites -- same "rewrite every call site via
// one #define" idiom hds_fx_gui_draw.c's own cairo_select_font_face substitution already uses.
static HdsClapPlugin g_gui_ctx;
#define g_knobs g_gui_ctx.knobs
#define g_fx_gr_db g_gui_ctx.fx_gr_db
#define g_fx_dump_avail_sec g_gui_ctx.fx_dump_avail_sec
#define g_ext_in_peak g_gui_ctx.ext_in_peak
#else
static SynthSharedControls g_knobs;
static float g_fx_gr_db[FX_SLOT_COUNT];
static float g_fx_dump_avail_sec[FX_SLOT_COUNT];
static float g_ext_in_peak[EXT_IN_PAIR_COUNT];
#endif

static SynthVoice g_voices[MAX_VOICES];
static float g_global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
static MonoNoteStack g_mono_stacks[COMBI_PROGRAM_COUNT + 1];
static MidiLiveState g_midi_live;
// -----------------------------------------------------------------------------------------------
// Real external audio input (mic / USB class-compliant interface) -- a second, independent AAudio
// INPUT stream feeding a lock-free ring, read by the OUTPUT callback right before
// render_audio_block(), same SPSC-ring idiom as the MIDI ring above (two AAudio callbacks run on
// their own independent threads/timing, exactly like the MIDI Binder thread does relative to the
// audio callback -- no lock may ever sit between them). The SAME captured buffer this produces is
// used both as render_audio_block's own `ext_in_buffer` (so FX ext-in sources/the Routing tab's
// External Input source actually receive real audio) and as the Recording engine's own
// `capture_buffer` (so raw Input Channel checkboxes on the Recording tab capture something real) --
// exactly the dual-use relationship hds_standalone.c's own capture_buffer already has (see
// recording_engine.c's own wav_recorder_gather_block comment).
//
// Channel count is NOT hardcoded to 2: both this stream and the output stream below are opened
// against whatever channel count MainActivity.java's own AudioManager.getDevices() enumeration
// found for the chosen device (preferring a class-compliant USB interface's real channel count
// over the built-in mic/speaker's usual stereo-or-mono), capped at RECORD_MAX_CHANNELS/
// OUTPUT_MAX_CHANNELS -- the same multichannel ceiling the desktop build's own Routing tab already
// exposes. A USB interface with, say, 8 real inputs is therefore a genuine 8-channel ext-in/
// Recording-tab source here, not silently downmixed to stereo.
#define HDS_ANDROID_INPUT_RING_CAPACITY (1 << 17) // power-of-two, interleaved floats across however many channels are actually open -- generous jitter headroom (~170ms at 16ch/48kHz) between the two independent AAudio callback threads, see this section's own header comment
typedef struct {
    float data[HDS_ANDROID_INPUT_RING_CAPACITY];
    _Atomic size_t write_idx; // only ever written by the input callback thread (producer)
    _Atomic size_t read_idx;  // only ever written by the output callback thread (consumer)
} HdsAndroidInputRing;
static HdsAndroidInputRing g_input_ring;

// Wait-free push (producer: input callback thread) -- drops the newest samples rather than
// blocking or overwriting unread ones if the ring is ever full (same tradeoff RecordRing makes:
// losing a moment of ext-in audio is far preferable to stalling either realtime callback).
static void hds_android_input_ring_push(const float *data, size_t n) {
    size_t w = atomic_load_explicit(&g_input_ring.write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&g_input_ring.read_idx, memory_order_acquire);
    size_t free_space = HDS_ANDROID_INPUT_RING_CAPACITY - (w - r);
    if (n > free_space) return;
    for (size_t i = 0; i < n; i++) g_input_ring.data[(w + i) & (HDS_ANDROID_INPUT_RING_CAPACITY - 1)] = data[i];
    atomic_store_explicit(&g_input_ring.write_idx, w + n, memory_order_release);
}

// Wait-free pop (consumer: output callback thread). Returns how many floats were actually
// available (never blocks) -- the caller zero-fills whatever's short, same "silence, never
// garbage, on underrun" posture as the output callback's own numFrames>frames_to_render fallback.
static size_t hds_android_input_ring_pop(float *out, size_t max_n) {
    size_t r = atomic_load_explicit(&g_input_ring.read_idx, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g_input_ring.write_idx, memory_order_acquire);
    size_t n = (w - r) < max_n ? (w - r) : max_n;
    for (size_t i = 0; i < n; i++) out[i] = g_input_ring.data[(r + i) & (HDS_ANDROID_INPUT_RING_CAPACITY - 1)];
    atomic_store_explicit(&g_input_ring.read_idx, r + n, memory_order_release);
    return n;
}

// Popped into every output callback, then handed to render_audio_block as ext_in_buffer AND to
// wav_recorder_gather_block as capture_buffer -- see this section's own header comment. Sized to
// the hard per-block ceiling at the hard channel ceiling, same convention as RecordRing's own
// pop_buf/frame_buf statics. Zero-filled up front; only ever partially overwritten by an actual
// pop (see hds_android_audio_callback's own zero-fill-the-shortfall comment), so a channel count of
// 0 (no input stream open at all -- no permission, or still mid-permission-request) or a
// momentary ring underrun both degrade to silence, never garbage, exactly like g_ext_in_silence
// used to be used unconditionally before this real input path existed.
static float g_ext_in_captured[BUFFER_SIZE * RECORD_MAX_CHANNELS];

static AAudioStream *g_stream = NULL;       // output (always opened)
static AAudioStream *g_input_stream = NULL; // input (best-effort -- NULL if never opened: no RECORD_AUDIO permission yet, or the open itself failed)
static int g_input_channels = 0;            // the INPUT stream's own real, AAudio-granted channel count -- 0 whenever g_input_stream is NULL
static int g_output_channels = 2;           // the OUTPUT stream's own real, AAudio-granted channel count -- defaults to 2 until nativeInit() actually opens the stream and reads back the true value
// Requested device id/channel count, set by nativeConfigureAudio() (called from MainActivity.java
// BEFORE nativeInit() -- see that file's own AudioManager.getDevices() enumeration) and consumed
// by hds_android_try_open_output()/hds_android_try_open_input() below. AAUDIO_UNSPECIFIED (0) for
// either id means "let AAudio/the OS pick the current default route," same as this file's own
// pre-existing behavior entirely lacked any opinion of its own.
static int32_t g_requested_input_device_id = AAUDIO_UNSPECIFIED;
static int32_t g_requested_input_channels = 2;
static int32_t g_requested_output_device_id = AAUDIO_UNSPECIFIED;
static int32_t g_requested_output_channels = 2;

static bool g_engine_initialized = false;

// Recording engine lifecycle/edge-detection state -- mirrors hds_standalone.c's own main()-local
// variables of the identical names exactly (see that file's own Recording section), just promoted
// to file scope here since this file has no single top-level loop to declare them in: the AAudio
// output callback IS the per-block loop, called repeatedly by AAudio itself rather than written as
// one here.
static bool record_writer_thread_ok = false;
static bool applied_record_enable = false;
static int recorder_input_channels_at_start = 0;
static uint64_t record_last_reported_drops = 0;

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
    memset(g_ext_in_captured, 0, sizeof(g_ext_in_captured));
    // Starts the SAME dedicated recording writer thread hds_standalone.c's own main() starts --
    // see recording_engine.c's own record_writer_thread_main comment. Started once, here, rather
    // than lazily on the first record_enable edge, so a start request the very first callback
    // already sees (unlikely, but no reason to race it) always has a thread ready to receive it.
    record_writer_thread_ok = (pthread_create(&record_engine.thread, NULL, record_writer_thread_main, &record_engine) == 0);
    if (!record_writer_thread_ok) HDS_LOGW("recording writer thread failed to start -- Recording tab will report start_failed");
#if defined(HDS_ANDROID_GUI_ENABLED)
    // Wires the HdsFxGuiHost view onto this process's own real fx[] -- mirrors
    // hds_standalone.c's own main()/clap_factory_create_plugin()'s identical lines exactly (see
    // hds_engine_instance.h's own header comment on why this is the right shared context type).
    g_gui_ctx.gui_host.fx = g_gui_ctx.knobs.fx;
    g_gui_ctx.gui_host.fx_gr_db = g_gui_ctx.fx_gr_db;
    g_gui_ctx.gui_host.fx_dump_avail_sec = g_gui_ctx.fx_dump_avail_sec;
    g_gui_ctx.gui_host.source_count = SOURCE_COUNT;
    g_gui_ctx.gui_host.source_label = hds_clap_source_label;
    g_gui_ctx.gui_host.source_routing_flat = &g_gui_ctx.knobs.source_routing[0].enable[0];
    g_gui_ctx.gui_host.output_pair_count = OUTPUT_PAIR_MAX;
    // Same plain-struct-copy reasoning as hds_standalone.c's own identical line: HdsSampleEngine
    // is just 3 pointers, both copies end up pointing at the SAME g_android_sample_slot_state/
    // g_streams_state/g_android_pending_load_state globals g_sample_engine itself already wraps.
    g_gui_ctx.sample_engine = g_sample_engine;
    if (!hds_android_gui_init(&g_gui_ctx)) {
        HDS_LOGW("gui init failed -- continuing with no GUI");
    }
#endif
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
// AAudio input (mic / USB interface) -- opened best-effort (see nativeOpenAudioInput below); this
// callback's only job is to push whatever AAudio handed it into the SPSC ring, as fast and as
// simply as possible (no engine call of any kind happens on this thread -- see this file's own
// "Real external audio input" section header comment for why the two streams never share more
// than that ring).
static aaudio_data_callback_result_t hds_android_audio_input_callback(
    AAudioStream *stream, void *userData, void *audioData, int32_t numFrames) {
    (void)stream; (void)userData;
    hds_android_input_ring_push((const float *)audioData, (size_t)numFrames * (size_t)g_input_channels);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// -----------------------------------------------------------------------------------------------
// AAudio output. Renders directly into the buffer AAudio itself hands this callback -- no Bitmap,
// no Java-side copy, no intermediate buffer of this file's own; see this file's own header comment
// for why that matters. audioData is already PCM_FLOAT, already interleaved at whatever channel
// count the stream was opened with (g_output_channels) -- a class-compliant multichannel USB
// interface gets every one of its own real output channels here, not a stereo downmix.
static long g_callback_count = 0; // diagnostic only -- paces the xrun log below to roughly once/sec

static aaudio_data_callback_result_t hds_android_audio_callback(
    AAudioStream *stream, void *userData, void *audioData, int32_t numFrames) {
    (void)userData;
    hds_midi_ring_drain_and_dispatch(); // apply everything the MIDI thread queued since the last callback -- no lock, see this file's own comment on the ring above
    float *out = (float *)audioData;
    int frames_to_render = numFrames;
    if (frames_to_render > BUFFER_SIZE) frames_to_render = BUFFER_SIZE; // render_audio_block's own hard ceiling (effective_block_size() clamps to this)
    g_knobs.block_size_frames = (float)frames_to_render;

    // Pop this block's worth of real captured input (mic/USB) out of the ring -- see this file's
    // own "Real external audio input" section header comment. Zero-filled up front so a channel
    // count of 0 (no input stream open) or a momentary ring underrun (startup, or the input
    // thread's callback briefly lagging the output one) both degrade to silence, never garbage,
    // for whatever the pop came up short on.
    memset(g_ext_in_captured, 0, sizeof(g_ext_in_captured));
    if (g_input_channels > 0) {
        hds_android_input_ring_pop(g_ext_in_captured, (size_t)frames_to_render * (size_t)g_input_channels);
    }

    render_audio_block(&g_sample_engine, &g_engine, g_voices, &g_knobs, g_global_lfo_phases,
                        g_ext_in_captured, g_input_channels, g_output_channels, &g_midi_live, out, 120.0f,
                        g_ext_in_peak, NULL, g_fx_gr_db, g_fx_dump_avail_sec);

    // --- Recording (Recording tab) ---
    // Edge-triggered start/stop on record_enable, exactly like hds_standalone.c's own main loop
    // (see recording_engine.c's own header comment for why this logic is platform-specific glue,
    // not part of the shared engine file). A mid-take input-channel-count change (unplugging a USB
    // interface, or the RECORD_AUDIO permission being granted/revoked) desyncs capture_buffer's
    // stride from what the open file committed to -- same safety stop hds_standalone.c's own
    // detected_input_channels check performs.
    if (record_gather.active && g_input_channels != recorder_input_channels_at_start) {
        HDS_LOGW("input channel count changed mid-recording -- stopping for safety");
        atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
        record_gather.active = false;
    }
    bool want_recording = record_writer_thread_ok && g_knobs.record_enable != 0.0f;
    if (want_recording && !applied_record_enable) {
        bool sel_include_synth_output = g_knobs.record_include_synth_output != 0.0f;
        bool sel_downmix_stereo = g_knobs.record_downmix_stereo != 0.0f;
        bool sel_input_channel[RECORD_MAX_CHANNELS];
        for (int c = 0; c < RECORD_MAX_CHANNELS; c++) sel_input_channel[c] = (c < g_input_channels) && (g_knobs.record_input_channel_enable[c] != 0.0f);
        int channels = wav_recorder_compute_channels(sel_include_synth_output, sel_downmix_stereo, sel_input_channel, g_input_channels);

        if (channels <= 0) {
            HDS_LOGW("record: cannot start -- no channels selected");
            atomic_store_explicit(&record_engine.start_failed, true, memory_order_relaxed);
        } else {
            snprintf(record_engine.cmd_path, sizeof(record_engine.cmd_path), "%s", g_knobs.record_path);
            record_engine.cmd_include_synth_output = sel_include_synth_output;
            record_engine.cmd_downmix_stereo = sel_downmix_stereo;
            record_engine.cmd_input_channel_count = g_input_channels;
            record_engine.cmd_record_format = (int)g_knobs.record_format;
            record_engine.cmd_wavpack_hybrid = g_knobs.record_wavpack_hybrid != 0.0f;
            record_engine.cmd_wavpack_correction_file = g_knobs.record_wavpack_correction_file != 0.0f;
            record_engine.cmd_wavpack_hybrid_bitrate = g_knobs.record_wavpack_hybrid_bitrate;
            record_engine.cmd_wavpack_compression_level = (int)g_knobs.record_wavpack_compression_level;
            for (int c = 0; c < RECORD_MAX_CHANNELS; c++) record_engine.cmd_input_channel_enable[c] = sel_input_channel[c];

            record_gather.sel_include_synth_output = sel_include_synth_output;
            record_gather.sel_downmix_stereo = sel_downmix_stereo;
            record_gather.sel_input_channel_count = g_input_channels;
            for (int c = 0; c < RECORD_MAX_CHANNELS; c++) record_gather.sel_input_channel[c] = sel_input_channel[c];
            record_gather.channels = channels;
            record_gather.active = true;
            recorder_input_channels_at_start = g_input_channels;

            atomic_store_explicit(&record_engine.start_failed, false, memory_order_relaxed);
            atomic_store_explicit(&record_engine.command, RECORD_CMD_START, memory_order_release);
            HDS_LOGI("record: start requested '%s', %d channel(s)%s", g_knobs.record_path, channels, sel_downmix_stereo ? " (downmixed)" : "");
        }
    } else if (!want_recording && applied_record_enable) {
        atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
        record_gather.active = false;
    }
    applied_record_enable = want_recording;
    if (record_gather.active) wav_recorder_gather_block(&record_gather, &record_engine, out, g_output_channels, g_ext_in_captured, frames_to_render);

    uint64_t record_dropped_now = atomic_load_explicit(&record_engine.ring.dropped_samples, memory_order_relaxed);
    if (record_dropped_now != record_last_reported_drops) {
        HDS_LOGW("record: writer thread falling behind -- %llu sample(s) dropped so far", (unsigned long long)record_dropped_now);
        record_last_reported_drops = record_dropped_now;
    }

    if (numFrames > frames_to_render) {
        // AAudio asked for more than render_audio_block's hard per-block ceiling can produce in one
        // call (shouldn't happen at any realistic buffer size, but silence, never garbage, for the
        // remainder if it ever does).
        memset(out + (size_t)frames_to_render * (size_t)g_output_channels, 0, (size_t)(numFrames - frames_to_render) * (size_t)g_output_channels * sizeof(float));
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
// JNI entry points. Package/class match MainActivity.java's own `NativeBridge` class -- see
// android/app/src/main/java/.../NativeBridge.java.

// Builds and opens a SHARED OUTPUT stream against `device_id` (AAUDIO_UNSPECIFIED lets AAudio/the
// OS pick the current default route) at `channel_count` real channels. Previously tried
// AAUDIO_SHARING_MODE_EXCLUSIVE first (the real dedicated-hardware path, in principle lower
// latency and more glitch-resistant than SHARED) -- reverted after real on-device testing made
// things WORSE, not better: dropouts went from "only under heavy chords" to "constantly," which
// points at EXCLUSIVE landing on a worse HAL path on that device than AudioFlinger's own SHARED
// mixer, not at anything this file's own code got wrong. SHARED is the one that was actually
// confirmed working (modulo the chord-stutter bug fixed elsewhere in this file); EXCLUSIVE is not
// worth the risk for a build whose whole point is proving basic correctness, not chasing the
// lowest possible latency.
//
// On failure, retries ONCE with AAUDIO_UNSPECIFIED device id/channel count (the OS's own default
// route/count) -- output must never silently fail to open just because a specifically-requested
// multichannel USB device didn't grant LOW_LATENCY/that exact channel count on a given phone; a
// stereo default route is far preferable to no sound at all.
static aaudio_result_t hds_android_try_open_output(AAudioStream **out_stream, int32_t device_id, int32_t channel_count) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return result;
    AAudioStreamBuilder_setDeviceId(builder, device_id);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, channel_count);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, hds_android_audio_callback, NULL);
    result = AAudioStreamBuilder_openStream(builder, out_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK && (device_id != AAUDIO_UNSPECIFIED || channel_count != AAUDIO_UNSPECIFIED)) {
        HDS_LOGW("output open failed for device=%d channels=%d (result=%d) -- retrying with the OS default route",
                 (int)device_id, (int)channel_count, (int)result);
        return hds_android_try_open_output(out_stream, AAUDIO_UNSPECIFIED, AAUDIO_UNSPECIFIED);
    }
    return result;
}

// Builds and opens a SHARED INPUT stream, same device/channel-count posture as output above --
// but best-effort, never retried: a failure here (most commonly RECORD_AUDIO not granted yet)
// just means ext-in/Recording-tab raw channels stay silent, never that the app can't run at all.
static aaudio_result_t hds_android_try_open_input(AAudioStream **out_stream, int32_t device_id, int32_t channel_count) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return result;
    AAudioStreamBuilder_setDeviceId(builder, device_id);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, channel_count);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, hds_android_audio_input_callback, NULL);
    result = AAudioStreamBuilder_openStream(builder, out_stream);
    AAudioStreamBuilder_delete(builder);
    return result;
}

// Called from MainActivity.java's own AudioManager.getDevices() enumeration, BEFORE nativeInit()
// -- stores which device id/channel count nativeInit() should open the OUTPUT stream against, and
// which hds_android_gui_... no, which nativeOpenAudioInput() below should use for the INPUT
// stream. Harmless to call more than once (e.g. if a device is hot-plugged later and Java
// re-enumerates) -- just updates these globals; an already-open stream isn't itself reopened by
// this call alone (see nativeOpenAudioInput's own comment on retrying).
JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeConfigureAudio(JNIEnv *env, jobject thiz,
    jint inputDeviceId, jint inputChannels, jint outputDeviceId, jint outputChannels) {
    (void)env; (void)thiz;
    g_requested_input_device_id = (int32_t)inputDeviceId;
    g_requested_input_channels = (inputChannels > 0) ? (int32_t)inputChannels : 2;
    if (g_requested_input_channels > RECORD_MAX_CHANNELS) g_requested_input_channels = RECORD_MAX_CHANNELS;
    g_requested_output_device_id = (int32_t)outputDeviceId;
    g_requested_output_channels = (outputChannels > 0) ? (int32_t)outputChannels : 2;
    if (g_requested_output_channels > OUTPUT_MAX_CHANNELS) g_requested_output_channels = OUTPUT_MAX_CHANNELS;
    HDS_LOGI("configured: input device=%d channels=%d, output device=%d channels=%d",
             (int)g_requested_input_device_id, (int)g_requested_input_channels,
             (int)g_requested_output_device_id, (int)g_requested_output_channels);
}

// Opens the INPUT stream against whatever nativeConfigureAudio() last stored, reading back
// AAudioStream_getChannelCount()'s own real granted value into g_input_channels (which is what
// gates every use of g_input_ring/g_ext_in_captured elsewhere in this file -- see this file's own
// "Real external audio input" section). Called opportunistically from nativeInit() (in case
// RECORD_AUDIO was already granted from a previous run) AND from MainActivity.java's own
// onRequestPermissionsResult once the user actually grants it this run -- a no-op (returns true
// immediately) if a stream is already open, so calling it twice is always safe.
JNIEXPORT jboolean JNICALL
Java_com_hds_androidtest_NativeBridge_nativeOpenAudioInput(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (g_input_stream != NULL) return JNI_TRUE;

    aaudio_result_t result = hds_android_try_open_input(&g_input_stream, g_requested_input_device_id, g_requested_input_channels);
    if (result != AAUDIO_OK || g_input_stream == NULL) {
        // Expected and harmless before RECORD_AUDIO is granted -- not logged as a warning the
        // first time (every cold start hits this before the permission dialog resolves), just
        // info: ext-in/Recording-tab raw channels stay silent until a later call succeeds.
        HDS_LOGI("input stream not open yet (result=%d) -- RECORD_AUDIO not granted, or no input device available", (int)result);
        g_input_stream = NULL;
        g_input_channels = 0;
        return JNI_FALSE;
    }
    g_input_channels = AAudioStream_getChannelCount(g_input_stream);
    if (AAudioStream_requestStart(g_input_stream) != AAUDIO_OK) {
        AAudioStream_close(g_input_stream);
        g_input_stream = NULL;
        g_input_channels = 0;
        return JNI_FALSE;
    }
    HDS_LOGI("input stream opened: %d channel(s)", g_input_channels);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_hds_androidtest_NativeBridge_nativeInit(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    hds_android_engine_init();

    aaudio_result_t result = hds_android_try_open_output(&g_stream, g_requested_output_device_id, g_requested_output_channels);
    if (result != AAUDIO_OK || g_stream == NULL) {
        HDS_LOGW("SHARED output open failed (result=%d)", (int)result);
        return JNI_FALSE;
    }
    // The real, AAudio-granted channel count -- may differ from what was requested (a device can
    // decline a specific count), so every subsequent render_audio_block/interleave call in the
    // output callback reads THIS back, never the request. See hds_android_try_open_output()'s own
    // comment on why open() itself still never fails outright over a channel-count mismatch (it
    // falls back to the OS default route/count instead).
    g_output_channels = AAudioStream_getChannelCount(g_stream);

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
    HDS_LOGI("opened: sharing=%d performance=%d channels=%d framesPerBurst=%d bufferCapacity=%d bufferFrames=%d",
             (int)AAudioStream_getSharingMode(g_stream), (int)AAudioStream_getPerformanceMode(g_stream),
             (int)g_output_channels, (int)frames_per_burst, (int)buffer_capacity, (int)actual_buffer_frames);

    if (AAudioStream_requestStart(g_stream) != AAUDIO_OK) {
        AAudioStream_close(g_stream);
        g_stream = NULL;
        return JNI_FALSE;
    }

    // Opportunistic: succeeds immediately if RECORD_AUDIO was already granted in a previous run
    // (Android remembers the grant across launches), otherwise harmlessly no-ops until
    // MainActivity.java's own onRequestPermissionsResult calls nativeOpenAudioInput() again once
    // the user actually grants it this run -- see that function's own comment.
    Java_com_hds_androidtest_NativeBridge_nativeOpenAudioInput(env, thiz);
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
#if defined(HDS_ANDROID_GUI_ENABLED)
    // Defensive only -- HdsSurfaceView's own surfaceDestroyed() callback (see
    // android/README.md) already calls nativeSurfaceDestroyed() before this normally runs;
    // hds_android_gui_surface_destroyed() is itself a no-op if the window is already gone (see
    // its own comment), so this is safe to call unconditionally here too.
    hds_android_gui_surface_destroyed();
#endif
    if (g_input_stream) {
        AAudioStream_requestStop(g_input_stream);
        AAudioStream_close(g_input_stream);
        g_input_stream = NULL;
        g_input_channels = 0;
    }
    if (g_stream) {
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = NULL;
    }
    // Flush/finalize a still-running take: signal the writer thread to stop (if a recording was
    // active) and to exit, then join it so its final fclose actually happens before the process
    // considers itself shut down -- same ordering as hds_standalone.c's own main() teardown (both
    // AAudio streams are already stopped above, so nothing can still be calling into
    // record_gather/record_engine from here on).
    if (record_gather.active) atomic_store_explicit(&record_engine.command, RECORD_CMD_STOP, memory_order_release);
    if (record_writer_thread_ok) {
        atomic_store_explicit(&record_engine.shutdown, true, memory_order_release);
        pthread_join(record_engine.thread, NULL);
        record_writer_thread_ok = false;
    }
}

#if defined(HDS_ANDROID_GUI_ENABLED)
// GUI JNI entry points -- match HdsSurfaceView.java's own SurfaceHolder.Callback/
// View.OnTouchListener implementation (see android/README.md). `surface` is the
// android.view.Surface SurfaceHolder.getSurface() returns.
JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jobject surface) {
    (void)thiz;
    hds_android_gui_surface_created(env, surface);
}

JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    hds_android_gui_surface_destroyed();
}

// action: HdsSurfaceView.java's own OnTouchListener collapses MotionEvent's real ACTION_*
// constants down to 0=sequence-start/1=move/2=sequence-end/3=pointer-count-changed before
// calling this -- see that file's own comment. pointerCount/x/y: one real finger's own
// coordinates when pointerCount==1, or the plain centroid (average) of every active pointer,
// already computed in Java, when pointerCount>=2 -- see hds_android_gui.c's own
// hds_android_touch_ring_drain_and_dispatch() for how finger count alone decides whether a touch
// sequence drives a parameter (one finger) or a scroll (two or more), per explicit product
// direction.
JNIEXPORT void JNICALL
Java_com_hds_androidtest_NativeBridge_nativeTouchEvent(JNIEnv *env, jobject thiz, jint action, jint pointerCount, jfloat x, jfloat y) {
    (void)env; (void)thiz;
    hds_android_gui_touch_event((int)action, (int)pointerCount, (float)x, (float)y);
}
#endif
