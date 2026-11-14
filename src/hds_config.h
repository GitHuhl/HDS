// hds_config.h -- build-wide audio constants shared by every compilation unit that needs them
// (backend.c, recording_engine.c, ...). Header-only, no dependencies.
#ifndef HDS_CONFIG_H
#define HDS_CONFIG_H

#define SAMPLE_RATE 48000
// BUFFER_SIZE and MAX_VOICES are now hard CEILINGS for array allocation only -- how many
// frames actually get rendered per block, and how many voices are actually allowed to be
// simultaneously active, are live user controls (SynthSharedControls.block_size_frames and
// .active_voice_limit, both on the Audio tab) rather than a number picked once at compile time
// and shipped fixed. This exists because the right value for either one is a property of the
// ACTUAL machine this is running on -- a fast desktop and a constrained embedded board don't
// have the same answer, and guessing one number for every deployment either wastes headroom on
// fast hardware or underruns on slow hardware. The diagnostics on write_audio_block/
// handle_note_on/the main loop's [perf] check exist so that tuning these live is an informed
// process (raise it, watch for [perf]/[voices]/[audio] warnings, back off if you see them) --
// see effective_voice_limit()/effective_block_size() for how the live values get clamped into
// these ceilings.
#define BUFFER_SIZE 1024

#endif // HDS_CONFIG_H
