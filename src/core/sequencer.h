// sequencer.h -- interface of the Sequencer transport/lane playback, external MIDI Clock sync and
// song-as-MIDI save/load (sequencer.c). Standalone-only: linked into synth_engine, never hds.clap.
#ifndef HDS_SEQUENCER_H
#define HDS_SEQUENCER_H

#include "core/backend.h"

#define HDS_MIDI_CLOCK_TIMEOUT_SEC 2.0 // no MIDI Clock tick for this long -> treat the external clock as gone

void poll_seq_audio_load_requests(HdsStreamsState *streams, const SynthSharedControls *knobs);
void poll_seq_audio_preview_triggers(HdsStreamsState *streams, const SynthSharedControls *knobs);
void poll_transport_triggers(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm);
void poll_seq_audio_transport_catchup(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs);
void advance_transport_playhead(HdsEngineState *engine, int block_size);
void seq_playback_midi_events(HdsEngineState *engine, HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], MonoNoteStack *mono_stack, const MidiLiveState *midi_live);
double hds_monotonic_seconds(void);
void hds_handle_midi_clock_tick(HdsEngineState *engine);
void hds_handle_midi_transport_start(HdsStreamsState *streams, HdsEngineState *engine, const SynthSharedControls *knobs);
void hds_handle_midi_transport_continue(HdsEngineState *engine);
void hds_handle_midi_transport_stop(HdsEngineState *engine);
int song_save_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);
int song_load_midi_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data);

#endif // HDS_SEQUENCER_H
