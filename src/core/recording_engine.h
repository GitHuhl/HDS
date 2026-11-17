// recording_engine.h -- interface to the WAV/WavPack recording engine (recording_engine.c).
//
// Threading model: the audio thread only ever writes RecordEngine.cmd_* and publishes `command`
// (release store), calls wav_recorder_gather_block() each block (wait-free), and polls the
// is_active/start_failed/frames_written status fields. All file I/O happens on the writer thread
// started with record_writer_thread_main(&record_engine).
#ifndef HDS_RECORDING_ENGINE_H
#define HDS_RECORDING_ENGINE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hds_config.h"

// Recording: hard ceiling on how many raw input channels can be captured/selected at once
// (generous for a class-compliant multichannel interface -- 2/4/8 are the common real cases).
// Also doubles as the channel count actually requested when opening the capture PCM stream --
// see query_device_channel_count()/open_pcm_stream() in main().
#define RECORD_MAX_CHANNELS 16
// Fixed-size output path buffer for the WAV recorder -- bigger than AUDIO_DEVICE_NAME_LEN since
// a real filesystem path (not just an ALSA device name) needs the room.
#define RECORD_PATH_LEN 256

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

// The single recording engine instance and its audio-thread-side selection mirror. File scope
// because both the platform layer's audio loop and the Recording tab GUI read them.
extern RecordEngine record_engine;
extern RecordGatherState record_gather;

// Channel count a take will have for this selection (identical on audio and writer threads).
int wav_recorder_compute_channels(bool include_synth_output, bool downmix_stereo, const bool *input_channel_enable, int input_channel_count);

// Audio thread, once per block while sel->active: gathers the selected channels and pushes them
// into eng->ring. Pure arithmetic plus a wait-free push -- no I/O.
void wav_recorder_gather_block(RecordGatherState *sel, RecordEngine *eng, const float *synth_buffer, int synth_channels, const float *capture_buffer, int block_size);

// Writer thread entry point; `arg` is the RecordEngine to serve (normally &record_engine).
void *record_writer_thread_main(void *arg);

#endif // HDS_RECORDING_ENGINE_H
