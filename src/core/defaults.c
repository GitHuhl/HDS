// defaults.c -- init_default_controls -- every control's default value.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"
#include "core/string_model.h"

// FIXED: Restored 'init_default_controls' macro mapping setup routine missing from compilation stack
void init_default_controls(SynthSharedControls *knobs) {
    knobs->programs[0].is_polyphonic = 1.0f; knobs->programs[0].vco1_shape = 0.0f; knobs->programs[0].vco2_shape = (float)WAVE_OFF;
    knobs->programs[0].fm_intensity = 0.0f; knobs->programs[0].vco1_vol = 0.8f; knobs->programs[0].vco2_vol = 0.5f;
    knobs->programs[0].portamento_time_ms = 150.0f; knobs->master_volume = 0.50f;
    knobs->transpose_semitones = 0.0f; // centered, no transposition
    knobs->tuning_enable = 0.0f; // standard 12-TET
    knobs->tuning_reference_note = 60.0f;   // Middle C
    knobs->tuning_reference_freq = 261.6256f; // Middle C's own standard 12-TET frequency
    knobs->programs[0].vco1_pan = 0.0f; knobs->programs[0].vco2_pan = 0.0f;
    knobs->programs[0].vco1_fine_tune = 0.0f; knobs->programs[0].vco2_pitch_offset = 0.0f; knobs->programs[0].vco2_fine_tune = 0.0f;
    // Filters: 5 flat slots, Series by default, every slot Off -- a fresh patch pays no filter
    // cost at all until a player actually reaches for one (same "Off is a true bypass" posture
    // as the post-filter EQ bands below). Filter 0's cutoff/res still default to a sensible
    // 800Hz/Q20 starting point for whenever it IS switched on, just not engaged automatically.
    knobs->programs[0].filter_chain_mode = 0.0f; // Series
    knobs->programs[0].filters[0].cutoff = 800.0f; knobs->programs[0].filters[0].res_q = 20.0f;
    knobs->programs[0].filters[0].slope_poles = 0.0f; knobs->programs[0].filters[0].filter_type = 0.0f; // Off -- every filter slot off by default
    knobs->programs[0].filters[0].key_track = 0.0f; knobs->programs[0].filters[0].cutoff_mod_source_cc = -1.0f;
    for (int f = 1; f < 5; f++) {
        knobs->programs[0].filters[f].cutoff = 2000.0f; knobs->programs[0].filters[f].res_q = 20.0f;
        knobs->programs[0].filters[f].slope_poles = 0.0f; knobs->programs[0].filters[f].filter_type = 0.0f; // Off
        knobs->programs[0].filters[f].key_track = 0.0f; knobs->programs[0].filters[f].cutoff_mod_source_cc = -1.0f;
    }
    // Post-filter 3-band EQ: flat (0dB) by default on every band -- see compute_eq_band_coeffs'
    // own "gain_db==0 is a true bypass" comment, so a fresh patch pays no EQ cost at all until a
    // player actually reaches for it. Freq defaults are just sensible starting points for each
    // band's own useful range (Low Shelf/Mid Peak/High Shelf).
    knobs->programs[0].eq_bands[0].freq = 100.0f;  knobs->programs[0].eq_bands[0].gain_db = 0.0f; knobs->programs[0].eq_bands[0].q = 0.707f;
    knobs->programs[0].eq_bands[1].freq = 1000.0f; knobs->programs[0].eq_bands[1].gain_db = 0.0f; knobs->programs[0].eq_bands[1].q = 0.707f;
    knobs->programs[0].eq_bands[2].freq = 5000.0f; knobs->programs[0].eq_bands[2].gain_db = 0.0f; knobs->programs[0].eq_bands[2].q = 0.707f;
    for(int l=0; l<LFO_COUNT; l++) { knobs->programs[0].lfo[l].wave_type = 4.0f; /* Off */ knobs->programs[0].lfo[l].rate_hz = 5.0f; knobs->programs[0].lfo[l].mod_depth = 0.0f; knobs->programs[0].lfo[l].key_sync = 1.0f; knobs->programs[0].lfo[l].dest_target = 0.0f; knobs->programs[0].lfo[l].phase_offset = 0.0f; knobs->programs[0].lfo[l].mod_source_cc = -1.0f; knobs->programs[0].lfo[l].per_key_mode = 0.0f; knobs->programs[0].lfo[l].key_track = 0.0f; knobs->programs[0].lfo[l].vel_track = 0.0f; knobs->programs[0].lfo[l].tempo_sync_division = 0.0f; knobs->programs[0].lfo[l].curve_drive = 0.0f; }
    for(int e=0; e<ENV_COUNT; e++) { knobs->programs[0].eg[e].d = 0.0f; knobs->programs[0].eg[e].h = 0.0f; knobs->programs[0].eg[e].a = 10.0f; knobs->programs[0].eg[e].dec = 150.0f; knobs->programs[0].eg[e].s = 80.0f; knobs->programs[0].eg[e].r = 300.0f; knobs->programs[0].eg[e].al = 0.0f; knobs->programs[0].eg[e].pk = 100.0f; knobs->programs[0].eg_mode[e] = 0.0f; }
    for(int i=0; i<ASSIGNABLE_EG_COUNT; i++) { knobs->programs[0].eg_intensity[i] = 1.0f; knobs->programs[0].eg_dest_target[i] = 0.0f; }
    // Subtle default detune (matches the old fixed +/-1.5% spread) and no width until the user asks for it
    knobs->programs[0].vco1_unison_saw_detune = 25.0f; knobs->programs[0].vco1_unison_saw_width = 50.0f;
    knobs->programs[0].vco2_unison_saw_detune = 25.0f; knobs->programs[0].vco2_unison_saw_width = 50.0f;
    knobs->programs[0].mono_glide_random = 0.0f;
    knobs->programs[0].vco1_mute = 0.0f; knobs->programs[0].vco2_mute = 0.0f;
    knobs->programs[0].vco1_force_own_amp_eg = 0.0f; knobs->programs[0].vco2_force_own_amp_eg = 0.0f;
    knobs->programs[0].vco3_force_own_amp_eg = 0.0f; knobs->programs[0].vco4_force_own_amp_eg = 0.0f;
    knobs->programs[0].hard_sync_mode = 0.0f;
    // Arpeggiator: off by default (byte-identical to today's behavior for every existing
    // patch), Up/1 octave/1-8/70% gate/no Hold when a user does turn it on.
    knobs->programs[0].arp_enabled = 0.0f;
    knobs->programs[0].arp_style = 0.0f;       // Up
    knobs->programs[0].arp_octave_range = 1.0f;
    knobs->programs[0].arp_rate = 1.0f;        // 1/8
    knobs->programs[0].arp_gate_pct = 70.0f;
    knobs->programs[0].arp_hold = 0.0f;
    knobs->programs[0].arp_velocity = 0.0f; // As Played -- reproduces today's exact behavior
    knobs->programs[0].vco1_phase_offset = 0.0f; knobs->programs[0].vco2_phase_offset = 0.0f;

    knobs->programs[0].vco1_pulse_width = 50.0f; knobs->programs[0].vco2_pulse_width = 50.0f;
    knobs->programs[0].vco3_vol = 0.0f; knobs->programs[0].vco3_pan = 0.0f; knobs->programs[0].vco3_mute = 0.0f; knobs->programs[0].vco3_pulse_width = 0.0f;
    knobs->programs[0].vco3_shape = (float)WAVE_OFF; knobs->programs[0].vco3_fine_tune = 0.0f; knobs->programs[0].vco3_pitch_offset = 0.0f;
    knobs->programs[0].vco3_phase_offset = 0.0f; knobs->programs[0].vco3_unison_saw_detune = 25.0f; knobs->programs[0].vco3_unison_saw_width = 50.0f;
    knobs->programs[0].vco4_vol = 0.0f; knobs->programs[0].vco4_pan = 0.0f; knobs->programs[0].vco4_mute = 0.0f; knobs->programs[0].vco4_pulse_width = 50.0f;
    knobs->programs[0].vco4_shape = (float)WAVE_OFF; knobs->programs[0].vco4_fine_tune = 0.0f; knobs->programs[0].vco4_pitch_offset = 0.0f;
    knobs->programs[0].vco4_phase_offset = 0.0f; knobs->programs[0].vco4_unison_saw_detune = 25.0f; knobs->programs[0].vco4_unison_saw_width = 50.0f;
    knobs->programs[0].fm_algorithm = 0.0f;
    knobs->programs[0].fm_depth_3_1 = 0.0f; knobs->programs[0].fm_depth_3_2 = 0.0f;
    knobs->programs[0].fm_depth_4_1 = 0.0f; knobs->programs[0].fm_depth_4_3 = 0.0f;
    knobs->programs[0].fm_depth_1_2 = 0.0f; knobs->programs[0].fm_depth_3_4 = 0.0f; knobs->programs[0].fm_depth_1_3 = 0.0f;
    knobs->programs[0].vco1_pitch = 0.0f; knobs->programs[0].vco2_pitch = 0.0f; knobs->programs[0].vco3_pitch = 0.0f; knobs->programs[0].vco4_pitch = 0.0f;
    knobs->programs[0].vco1_mono_noise = 0.0f; knobs->programs[0].vco2_mono_noise = 0.0f;
    knobs->programs[0].vco3_mono_noise = 0.0f; knobs->programs[0].vco4_mono_noise = 0.0f;
    // Velocity -> Amp: 0 reproduces today's exact velocity-insensitive output level regardless of
    // what velocity value a note-on carries -- see ProgramState.vco{1-4}_vel_to_amp's own comment.
    knobs->programs[0].vco1_vel_to_amp = 0.0f; knobs->programs[0].vco2_vel_to_amp = 0.0f;
    knobs->programs[0].vco3_vel_to_amp = 0.0f; knobs->programs[0].vco4_vel_to_amp = 0.0f;
    // Curve/Drive: 0.0f = linear ramp (Saw/Tri) / clean sine, reproducing today's exact output on
    // every shape -- see ProgramState.vco{1-4}_curve_drive's own comment.
    knobs->programs[0].vco1_curve_drive = 0.0f; knobs->programs[0].vco2_curve_drive = 0.0f;
    knobs->programs[0].vco3_curve_drive = 0.0f; knobs->programs[0].vco4_curve_drive = 0.0f;
    // Osc 3/4 Amp envelopes live at eg[2]/eg[3] (and eg_mode[2]/eg_mode[3]) and were covered by
    // the generic `for(int e=0; e<ENV_COUNT; e++)` default-fill loop above -- nothing extra
    // needed here.

    // Programs 1-7 (Combi mode's remaining lanes, inert until a real per-lane Program picker
    // loads something onto them -- see the "Real Combi mode" plan's Phase 5) and
    // programs[SINGLE_PROGRAM_SLOT] (Single Program mode's own dedicated ProgramState -- see that
    // slot's own comment on the mutual-exclusivity fix) all start from the exact same sane,
    // musical defaults as programs[0] above, not raw zero-init -- a plain struct copy is safe
    // here since ProgramState holds no pointers. Whatever gets loaded/edited onto a slot later
    // simply overwrites this, independently of every other slot.
    for (int o = 0; o < 4; o++) hds_string_default_params(&knobs->programs[0].str[o]);
    for (int p = 1; p <= COMBI_PROGRAM_COUNT; p++) knobs->programs[p] = knobs->programs[0];

    // ext_in_gain defaults to 0.0 (silence/-inf), not unity: an upstream mic pre or interface
    // could already be set hot, and External Input must never be able to burst into the FX bus
    // the moment ext_in_enable goes live (e.g. right after this engine (re)starts, before the
    // frontend's own dial -- which also always inits to 0 -- has sent its first packet). The
    // user always has to bring this up manually.
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) { knobs->ext_in_enable[p] = 0.0f; knobs->ext_in_gain[p] = 0.0f; knobs->ext_in_stereo[p] = 1.0f; }
    for (int b = 0; b < DRUM_BUS_COUNT; b++) knobs->drum_bus_gain[b] = 1.0f; // unity -- see this field's own comment on why (unlike ext_in_gain) it doesn't default to silence
    knobs->gui_routing_tab_visible = 0.0f;
    for (int f = 0; f < FX_SLOT_COUNT; f++) {
        knobs->fx[f].fx_type = FX_TYPE_OFF;
        for (int p = 0; p < FX_SLOT_PARAM_COUNT; p++) knobs->fx[f].param[p] = 0.0f;
        knobs->fx[f].mix = 100.0f; // fully wet once a real effect is assigned to this slot
        knobs->fx[f].bypass = 0.0f;
        knobs->fx[f].fx_source = 0.0f; // Program -- reproduces today's exact behavior by default
        knobs->fx[f].chain_to_next = 0.0f; // every slot its own one-slot branch in Parallel mode -- today's exact behavior
        knobs->fx[f].sidechain_source = -1.0f; // "Same as Source" -- self-key, ordinary compression by default
        knobs->fx[f].input_from_slot = -1.0f; // not used -- see FXSlotParams' own comment
        knobs->fx[f].nam_model_path[0] = '\0';
        knobs->fx[f].nam_ir_path[0] = '\0';
    }
    // Routing defaults: only the Program is routed anywhere (to output pair 0, i.e. today's only
    // output pair), every ext-in pair unrouted -- this combination reproduces today's exact
    // single-output-pair, Program-only behavior bit-for-bit in Single Program mode (Programs 1-7
    // never claim a voice outside Combi mode, so routing them anywhere is a harmless no-op there).
    for (int s = 0; s < SOURCE_COUNT; s++) {
        for (int o = 0; o < OUTPUT_PAIR_MAX; o++) knobs->source_routing[s].enable[o] = 0.0f;
    }
    // Every Combi Program (source ids 0..COMBI_PROGRAM_COUNT-1) defaults to output pair 0, not
    // just Program 1 -- otherwise switching to Combi mode and loading a Program onto lane 2+
    // produces total silence for that lane until the user separately visits the Routing tab,
    // which nothing prompts them to do. Combi Mix (source id COMBI_PROGRAM_COUNT) stays
    // unrouted by default -- routing it too would double-count every individually-routed
    // Program's output. Ext-in pairs stay unrouted by default, unchanged.
    for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) knobs->source_routing[p].enable[0] = 1.0f;
    knobs->programs[0].pb_range_up = 2.0f; knobs->programs[0].pb_range_down = 2.0f; // standard +/-2 semitone default
    knobs->programs[0].sustain_pedal_polarity = 0.0f; // Normal
    knobs->audio_latency_ms = 25.0f; // matches the old hardcoded 25ms target -- no behavior change by default
    knobs->active_voice_limit = 32.0f; // matches the last-measured-safe default -- live-adjustable from here
    knobs->block_size_frames = 128.0f; // matches the old hardcoded BUFFER_SIZE -- no behavior change by default
    knobs->record_enable = 0.0f;
    knobs->record_include_synth_output = 1.0f;
    knobs->record_downmix_stereo = 0.0f;
    for (int c = 0; c < RECORD_MAX_CHANNELS; c++) knobs->record_input_channel_enable[c] = 0.0f;
    knobs->record_path[0] = '\0';
    knobs->record_format = 0.0f; // WAV
    knobs->record_wavpack_hybrid = 0.0f; // Lossless
    knobs->record_wavpack_correction_file = 0.0f;
    knobs->record_wavpack_hybrid_bitrate = 4.0f; // bits/sample -- a common "near-transparent hybrid" target
    knobs->record_wavpack_compression_level = 1.0f; // Normal
    knobs->programs[0].tempo_bpm = 120.0f; // a common, neutral default tempo
    knobs->programs[0].tempo_marker_count = 0.0f; // no tempo map programmed -- tempo_bpm alone applies
    for (int t = 0; t < TEMPO_MARKER_MAX; t++) { knobs->programs[0].tempo_marker_time[t] = 0.0f; knobs->programs[0].tempo_marker_bpm[t] = 0.0f; }
    // No sample-bank defaults to fill here -- the Sample Bank isn't part of this struct anymore
    // (see its own dedicated socket/g_sample_slots[]).
    // Combi/Sequencer scaffolding: Single Program (today's only real mode) by default, every
    // lane inert -- see SeqLaneState's own comment for why none of this affects the DSP yet.
    knobs->performance_mode = 0.0f; // Single Program
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        knobs->seq_lanes[l].lane_type = 0.0f;    // Program/MIDI
        knobs->seq_lanes[l].volume = 80.0f;
        knobs->seq_lanes[l].pan = 0.0f;
        knobs->seq_lanes[l].midi_channel = 0.0f; // Omni
        knobs->seq_lanes[l].seq_mode = 0.0f;     // Seq
        knobs->seq_lanes[l].mute = 0.0f;
        knobs->seq_lanes[l].rec_armed = 0.0f;
        knobs->seq_lanes[l].rec_overdub = 0.0f;  // Overwrite
        knobs->seq_lanes[l].audio_path[0] = '\0';
        knobs->seq_lanes[l].audio_offset_sec = 0.0f;
        knobs->seq_lanes[l].audio_preview_trigger = 0.0f;
    }
    knobs->seq_transport_play_trigger = 0.0f;
    knobs->seq_transport_pause_trigger = 0.0f;
    knobs->seq_transport_stop_trigger = 0.0f;
    knobs->seq_transport_seek_start_trigger = 0.0f;
    knobs->seq_transport_seek_back_trigger = 0.0f;
    knobs->seq_transport_seek_fwd_trigger = 0.0f;
    knobs->seq_transport_seek_to_target_sec = 0.0f;
    knobs->seq_transport_seek_to_trigger = 0.0f;
    knobs->seq_quantize_enable = 0.0f;
    knobs->seq_quantize_resolution = 4.0f; // sixteenth notes
    knobs->seq_sync_source = 0.0f; // Internal
    knobs->metronome_enable = 0.0f;
    for (int p = 0; p < COMBI_PROGRAM_COUNT; p++) {
        knobs->combi_lane_midi_channel[p] = 0.0f; // Omni
        knobs->combi_lane_key_lo[p] = 0.0f; knobs->combi_lane_key_hi[p] = 127.0f; // full range
        knobs->combi_lane_vel_lo[p] = 1.0f; knobs->combi_lane_vel_hi[p] = 127.0f; // full range
        knobs->combi_lane_transpose[p] = 0.0f; // centered, no transposition
    }
    snprintf(knobs->output_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->input_device, AUDIO_DEVICE_NAME_LEN, "default");
    snprintf(knobs->midi_device, AUDIO_DEVICE_NAME_LEN, "auto");
}
