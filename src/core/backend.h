// backend.h -- public interface of the shared synth engine (backend.c): the functions and globals
// that the platform layers (hds_standalone.c, clap_plugin.c), sequencer.c and the GUI draw files
// call into. Types and limits live in backend_types.h, included below.
//
// The g_* globals are the standalone app's single engine instance; clap_plugin.c keeps per-instance
// copies in HdsClapPlugin instead and passes them to these functions explicitly.
#ifndef HDS_BACKEND_H
#define HDS_BACKEND_H

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <dirent.h> // native SFZ parser's case-insensitive sample-path fallback (sfz_resolve_sample_path)
#include <limits.h> // PATH_MAX, for the same parser's #include cycle-guard/path buffers
#include <ctype.h>  // isalpha/isalnum/isspace, for the same parser's hand-written tokenizer
#include <strings.h> // strcasecmp -- SFZ opcode names are conventionally lowercase, matched case-insensitively here for robustness
#include <lo/lo.h>

#include "core/backend_types.h"
#include "fx/fx_chain.h"

// --- Engine globals (standalone app's single instance) ---
extern volatile sig_atomic_t g_synth_running;
extern HdsStreamsState g_streams_state;
extern HdsEngineState g_engine;
extern HdsTuningTable g_tuning_table;
extern OscDispatchSlot osc_dispatch_table[OSC_DISPATCH_TABLE_SIZE];

// Short aliases for fields of the globals above (kept from before the multi-instance refactor).
#define g_sample_streams g_streams_state.sample_streams
#define g_sample_disk_thread g_streams_state.sample_disk_thread
#define g_sample_disk_thread_shutdown g_streams_state.sample_disk_thread_shutdown
#define g_seq_audio g_streams_state.seq_audio
#define g_seq_midi g_engine.seq_midi
#define g_transport_state g_engine.transport_state
#define g_transport_playhead_sec g_engine.transport_playhead_sec
#define g_transport_last_play_trigger g_engine.transport_last_play_trigger
#define g_transport_last_pause_trigger g_engine.transport_last_pause_trigger
#define g_transport_last_stop_trigger g_engine.transport_last_stop_trigger
#define g_transport_last_seek_start_trigger g_engine.transport_last_seek_start_trigger
#define g_transport_last_seek_back_trigger g_engine.transport_last_seek_back_trigger
#define g_transport_last_seek_fwd_trigger g_engine.transport_last_seek_fwd_trigger
#define g_metronome_next_click_sec g_engine.metronome_next_click_sec
#define g_metronome_click_samples_left g_engine.metronome_click_samples_left
#define g_metronome_click_phase g_engine.metronome_click_phase

// Clamp the live polyphony/block-size controls into their hard array-allocation ceilings.
// Centralized here so every loop bound and buffer-fill site derives the same clamped value the
// same way, rather than each repeating its own (fmaxf/fminf + cast) inline.
static inline int effective_voice_limit(const SynthSharedControls *knobs) {
    int n = (int)knobs->active_voice_limit;
    if (n < 1) n = 1;
    if (n > MAX_VOICES) n = MAX_VOICES;
    return n;
}
static inline int effective_block_size(const SynthSharedControls *knobs) {
    int n = (int)knobs->block_size_frames;
    if (n < 1) n = 1;
    if (n > BUFFER_SIZE) n = BUFFER_SIZE;
    return n;
}

// --- Functions (in backend.c definition order) ---
void handle_sigint(int sig);
float compute_current_tempo_bpm(const SynthSharedControls *knobs, double elapsed_seconds);
void build_osc_dispatch_table(void);
const OscDispatchSlot *osc_dispatch_lookup(const char *address);
void hds_json_free(HdsJsonValue *v);
HdsJsonValue *hds_json_parse(const char *text);
HdsJsonValue *hds_json_object_get(const HdsJsonValue *obj, const char *key);
char **hds_scan_programs_folder(int *out_count);
int generic_osc_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
void osc_error_handler(int num, const char *msg, const char *path);
int request_state_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int ping_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int subscribe_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
void init_default_controls(SynthSharedControls *knobs);
void init_voice(SynthVoice *voice);
void release_sample_stream(HdsSampleEngine *se, int stream_index);
void *sample_disk_thread_main(void *arg);
void drain_pending_sample_loads(HdsSampleEngine *se);
// The sample loader thread (sample_engine.c): start it alongside the disk thread, stop it before
// the engine state can go away, and free whatever it still holds once the engine is destroyed.
// While it isn't running, loads fall back to the old in-thread path (tests, Android).
bool hds_sample_loader_start(HdsSampleEngine *se);
void hds_sample_loader_stop(HdsSampleEngine *se);
void hds_sample_loader_free(HdsSampleEngine *se);
void apply_zone_upsert(HdsSampleEngine *se, const ZoneRecordWire *rec);
void apply_zone_remove(HdsSampleEngine *se, int32_t zone_id);
int zone_upsert_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int zone_remove_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int zone_drum_bus_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int hds_load_sfz_for_osc(HdsSampleEngine *se, int osc_index, int owner_program, const char *sfz_path);
void poll_vco_shape_unload(HdsSampleEngine *se, const SynthSharedControls *knobs);
int zone_load_sfz_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
void assert_fm_algorithms_acyclic(void);
bool hds_tuning_load_scl(const char *file_path);
void handle_note_on(HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], int target_program, int note, int velocity, bool poly_mode_active, MonoNoteStack *mono_stack);
void handle_note_off(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs, const MidiLiveState *midi_live, int target_program, int note, bool poly_mode_active, MonoNoteStack *mono_stack);
int fx_upload_begin_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int fx_upload_chunk_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int fx_upload_end_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
void poll_metronome(HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm, int block_size);
int song_import_smf_track_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int song_import_smf_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
void poll_arpeggiator(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], const MidiLiveState *midi_live, int block_size);
void dispatch_midi_event(HdsEngineState *engine, HdsSampleEngine *se, HdsMidiEventType type, int midi_channel_1_16, int data1, int value, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1], MidiLiveState *midi_live, float current_tempo_bpm);
float render_audio_block(HdsSampleEngine *se, HdsEngineState *engine, SynthVoice voices[], const SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], const float *ext_in_buffer, int ext_in_channels, int output_channels, const MidiLiveState *midi_live, float *out_buffer, float current_tempo_bpm, float ext_in_peak_out[EXT_IN_PAIR_COUNT], float *per_program_out, float gr_db_out[FX_SLOT_COUNT], float dump_avail_out[FX_SLOT_COUNT]);
void send_osc_status(lo_address reply_addr, const EngineStatus *status);
void send_seq_audio_status(HdsStreamsState *streams, lo_address reply_addr);

#endif // HDS_BACKEND_H
