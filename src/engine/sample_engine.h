// sample_engine.h -- the hooks backend.c's voice code (note-on/off, render_audio_block) uses to
// drive WAVE_SAMPLE oscillators; implemented in sample_engine.c. The rest of the sample engine's
// public surface (zone OSC handlers, hds_load_sfz_for_osc, drain_pending_sample_loads,
// poll_vco_shape_unload, release_sample_stream, sample_disk_thread_main) is in backend.h.
#ifndef HDS_SAMPLE_ENGINE_H
#define HDS_SAMPLE_ENGINE_H

#include "../backend.h"

void request_sample_stream_release(HdsSampleEngine *se, int stream_index);
bool resolve_zone_owned_ampeg(HdsSampleEngine *se, int stream_idx, bool force_own_amp_eg, EGDerivedRates *out_rates);
int resolve_zone_drum_bus(HdsSampleEngine *se, int stream_idx);
void init_amp_eg_start(ADSR *eg, bool use_own_ampeg, const EGDerivedRates *own_rates, int prog_mode, const PackEGParams *prog_eg);
void start_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr, bool *preroll, int assigned_osc, int owner_program, int note, int velocity, bool is_first_key_of_phrase, int *out_delay_frames, int *layer_stream_idx, double *layer_ptr, bool *layer_preroll, float *out_xfade_gain, float *out_layer_xfade_gain);
void fire_release_trigger_zones(HdsSampleEngine *se, int assigned_osc, int owner_program, int note, int velocity);
void release_idle_sample_streams(HdsSampleEngine *se, SynthVoice *voice);
float render_sample_osc(HdsSampleEngine *se, int *stream_idx, double *ptr_inout, bool *preroll_inout, float hz, float *out_l_ptr, float *out_r_ptr);

#endif // HDS_SAMPLE_ENGINE_H
