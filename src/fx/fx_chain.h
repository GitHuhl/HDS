// fx_chain.h -- public interface of the FX slot bus (fx_chain.c).
//
// Every function takes the slot count (and, for process_fx_chain, the sidechain source count) at
// runtime, so callers with different bus sizes (synth: FX_SLOT_COUNT 12; HDS FX: 32) share one
// compiled fx_chain.o. Array arguments must hold at least that many elements.
#ifndef HDS_FX_CHAIN_H
#define HDS_FX_CHAIN_H

#include <stdbool.h>

#include "core/hds_config.h"
#include "fx/fx_types.h"

// Constants the FX GUI shares with the DSP.
#define HDS_BCASTDLY_MAX_SEC 60          // broadcast delay's maximum delay time
#define FX_DBX120X_TRACK_MIN_HZ 30.0f    // dbx 120X pitch-tracker range
#define FX_DBX120X_TRACK_MAX_HZ 300.0f

// Marks every slot's engine as FX_TYPE_OFF with no state (no frees -- call once at init).
void reset_fx_engine(FXSlotEngine *engine, int slot_count);

// Control-rate housekeeping: (re)allocates/frees per-slot effect state whenever a slot's type changed.
void sync_fx_engine_types(const FXSlotParams *fx_params, FXSlotEngine *fx_engine, int slot_count);

// Control-rate: loads/unloads NAM models and cab IRs whose paths changed (no-op without NAM).
void poll_fx_nam_requests(const FXSlotParams *fx, FXSlotEngine *fx_engine, int slot_count);

// Runs the bus in place on one stereo sample for one source. sc_snapshot_l/r hold every source's
// previous-sample output (source_count entries) for sidechain keying. gr_db_out, dump_avail_out,
// slot_out_l and slot_out_r must each hold slot_count entries; slot_out_l/r persist across calls.
void process_fx_chain(const FXSlotParams *fx_params, FXSlotEngine *fx_engine, int slot_count,
                      int routing_mode, int source_id,
                      const float *sc_snapshot_l, const float *sc_snapshot_r, int source_count,
                      float *l, float *r, float *gr_db_out, float *dump_avail_out,
                      float *slot_out_l, float *slot_out_r);

// Decodes a WAV file into newly malloc'd interleaved float PCM (caller frees). NULL on failure.
float *seq_audio_load_wav_from_disk(const char *path, int *out_frames, int *out_channels, int *out_sample_rate);

// Engine-wide MIDI-derived state the effects read (HardTune's MIDI mode, the broadcast delay's
// note gate). Written by whichever MIDI dispatcher the host build has.
extern float g_hardtune_held_note;        // last-note-priority held note, -1 when none
extern float g_hardtune_bend_semitones;
extern float g_hardtune_modwheel;         // 0..1
extern float g_hardtune_channel_pressure; // 0..1
extern int g_bcastdly_notes_held;
extern bool g_bcastdly_sustain_held;

#endif // HDS_FX_CHAIN_H
