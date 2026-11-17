// voices.c -- note handling and voice allocation: handle_note_on/off, voice release, sustain pedal release, the mono note stack, glide randomization, note -> frequency (incl. microtuning), and init_voice.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"

void init_voice(SynthVoice *voice) {
    voice->midi_note = -1;
    voice->midi_velocity = 100;
    voice->program_index = 0;
    voice->vco1_phase = 0.0f; voice->vco1_target_freq = 0.0f; voice->vco1_current_freq = 0.0f; voice->vco1_tri_state = 0.0f; voice->vco1_prev_fm_phase = 0.0f;
    voice->vco2_phase = 0.0f; voice->vco2_target_freq = 0.0f; voice->vco2_current_freq = 0.0f; voice->vco2_tri_state = 0.0f; voice->vco2_prev_fm_phase = 0.0f; voice->vco2_pending_sync_correction = 0.0f;
    voice->vco3_phase = 0.0f; voice->vco3_target_freq = 0.0f; voice->vco3_current_freq = 0.0f; voice->vco3_tri_state = 0.0f; voice->vco3_prev_fm_phase = 0.0f; voice->vco3_pending_sync_correction = 0.0f;
    voice->vco4_phase = 0.0f; voice->vco4_target_freq = 0.0f; voice->vco4_current_freq = 0.0f; voice->vco4_tri_state = 0.0f; voice->vco4_prev_fm_phase = 0.0f; voice->vco4_pending_sync_correction = 0.0f;
    for(int u=0; u<UNISON_SAW_VOICES; u++) { voice->vco1_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco2_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco3_unison_saw_phases[u] = (float)rand()/RAND_MAX; voice->vco4_unison_saw_phases[u] = (float)rand()/RAND_MAX; }
    for(int c=0; c<2; c++) { for(int f=0; f<5; f++) { voice->f_low[c][f][0]=0.0f; voice->f_low[c][f][1]=0.0f; voice->f_band[c][f][0]=0.0f; voice->f_band[c][f][1]=0.0f; } }
    voice->glide_time_multiplier = 1.0f;
    voice->sustain_pending = false;
    voice->vco1_eg.state = STATE_IDLE; voice->vco1_eg.current_level = 0.0f; voice->vco1_eg.release_level = 0.0f;
    voice->vco2_eg.state = STATE_IDLE; voice->vco2_eg.current_level = 0.0f; voice->vco2_eg.release_level = 0.0f;
    voice->vco3_eg.state = STATE_IDLE; voice->vco3_eg.current_level = 0.0f; voice->vco3_eg.release_level = 0.0f;
    voice->vco4_eg.state = STATE_IDLE; voice->vco4_eg.current_level = 0.0f; voice->vco4_eg.release_level = 0.0f;
    voice->vco1_tilt_lp = 0.0f; voice->vco2_tilt_lp = 0.0f; voice->vco3_tilt_lp = 0.0f; voice->vco4_tilt_lp = 0.0f;
    for(int e=0; e<ASSIGNABLE_EG_COUNT; e++) { voice->assign_eg[e].state = STATE_IDLE; voice->assign_eg[e].current_level = 0.0f; voice->assign_eg[e].release_level = 0.0f; }
    voice->vco1_sample_stream = -1; voice->vco1_sample_ptr = 0.0; voice->vco1_sample_preroll = false;
    voice->vco2_sample_stream = -1; voice->vco2_sample_ptr = 0.0; voice->vco2_sample_preroll = false;
    voice->vco3_sample_stream = -1; voice->vco3_sample_ptr = 0.0; voice->vco3_sample_preroll = false;
    voice->vco4_sample_stream = -1; voice->vco4_sample_ptr = 0.0; voice->vco4_sample_preroll = false;
    voice->vco1_layer_sample_stream = -1; voice->vco1_layer_sample_ptr = 0.0; voice->vco1_layer_sample_preroll = false;
    voice->vco2_layer_sample_stream = -1; voice->vco2_layer_sample_ptr = 0.0; voice->vco2_layer_sample_preroll = false;
    voice->vco3_layer_sample_stream = -1; voice->vco3_layer_sample_ptr = 0.0; voice->vco3_layer_sample_preroll = false;
    voice->vco4_layer_sample_stream = -1; voice->vco4_layer_sample_ptr = 0.0; voice->vco4_layer_sample_preroll = false;
    voice->vco1_xfade_gain = 1.0f; voice->vco1_layer_xfade_gain = 0.0f; voice->vco1_sample_delay_remaining = 0;
    voice->vco2_xfade_gain = 1.0f; voice->vco2_layer_xfade_gain = 0.0f; voice->vco2_sample_delay_remaining = 0;
    voice->vco3_xfade_gain = 1.0f; voice->vco3_layer_xfade_gain = 0.0f; voice->vco3_sample_delay_remaining = 0;
    voice->vco4_xfade_gain = 1.0f; voice->vco4_layer_xfade_gain = 0.0f; voice->vco4_sample_delay_remaining = 0;
    voice->vco1_using_own_ampeg = false; voice->vco2_using_own_ampeg = false;
    voice->vco3_using_own_ampeg = false; voice->vco4_using_own_ampeg = false;
    voice->vco1_drum_bus = 0; voice->vco2_drum_bus = 0; voice->vco3_drum_bus = 0; voice->vco4_drum_bus = 0;
    for (int l = 0; l < LFO_COUNT; l++) {
        voice->lfo_phase[l] = 0.0f;
        voice->lfo_decim_counter[l] = 0;
        voice->lfo_decim_last_value[l] = 0.0f;
        voice->lfo_decim_target_value[l] = 0.0f;
    }
}
// Rolls a randomized glide-time multiplier for Mono Legato: 1.0 = no variation. glide_random_pct
// (0-100) controls how far the multiplier can wander from 1.0 in either direction; floored well
// above 0 so a glide can never become instant/negative-time from an unlucky roll.
static float roll_glide_multiplier(float glide_random_pct) {
    float amount = fmaxf(0.0f, fminf(100.0f, glide_random_pct)) / 100.0f;
    if (amount <= 0.0f) return 1.0f;
    float rand_frac = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f; // -1..1
    return fmaxf(0.05f, 1.0f + (rand_frac * amount));
}

// Engine-wide monophonic "currently held note" tracker for FX_TYPE_HARDTUNE's MIDI Target Source
// mode -- deliberately separate from any Program's own per-Program mono_stack (which only exists/
// updates while that Program is in Mono mode): this tracks every note-on/note-off globally,
// regardless of which Program it targets or whether that Program is in Poly or Mono mode, since a
// vocalist using this effect wants it to follow whatever note a MIDI controller is playing, not
// tied to one Program's own voice-allocation mode. Last-note-priority monophonic, reusing the same
// MonoNoteStack machinery already used everywhere else in this file. Updated only in
// handle_note_on/handle_note_off (both unconditional on poly_mode_active), read only in
// run_fx_hardtune -- safe as a plain global under this engine's single-threaded architecture (MIDI
// dispatch and audio rendering already run on the same thread).
// (g_hardtune_held_note/g_hardtune_bend_semitones -- the two plain floats run_fx_hardtune itself
// reads -- are declared earlier in this file, right before FX_TYPE_HARDTUNE's own implementation,
// since that function is defined well before MonoNoteStack is; only this struct-typed stack needs
// to live here, where MonoNoteStack exists, since it's touched only by handle_note_on/
// handle_note_off below, both much later in the file than run_fx_hardtune.)
static MonoNoteStack g_hardtune_note_stack = {0};

void mono_stack_push(MonoNoteStack *stack, int note, int velocity) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
            stack->count--;
            break;
        }
    }
    if (stack->count < MONO_STACK_SIZE) { stack->notes[stack->count] = note; stack->velocities[stack->count] = velocity; stack->count++; }
    else {
        for (int j = 0; j < MONO_STACK_SIZE - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
        stack->notes[MONO_STACK_SIZE - 1] = note;
        stack->velocities[MONO_STACK_SIZE - 1] = velocity;
    }
}

void mono_stack_remove(MonoNoteStack *stack, int note) {
    for (int i = 0; i < stack->count; i++) {
        if (stack->notes[i] == note) {
            for (int j = i; j < stack->count - 1; j++) { stack->notes[j] = stack->notes[j + 1]; stack->velocities[j] = stack->velocities[j + 1]; }
            stack->count--;
            return;
        }
    }
}

// target_program: which of knobs->programs[COMBI_PROGRAM_COUNT] this note belongs to (0 in
// The one lookup every note-on/note-off pitch calculation goes through -- see this section's own
// header comment for the mapping. Falls back to standard 12-TET (identical to this file's
// original hardcoded formula) whenever tuning is off, nothing has been loaded yet, or the loaded
// table is somehow empty, so a build/session that never touches tuning behaves byte-identically
// to before this feature existed.
static float hds_note_to_hz(const SynthSharedControls *knobs, int note) {
    float standard_hz = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    if (knobs->tuning_enable == 0.0f || !g_tuning_table.loaded || g_tuning_table.degree_count <= 0) return standard_hz;
    int n = g_tuning_table.degree_count;
    int reference_note = (int)knobs->tuning_reference_note;
    int offset = note - reference_note;
    int period_count = (offset >= 0) ? (offset / n) : -(((-offset) + n - 1) / n); // floor division, correct for negative offset too
    int degree = offset - period_count * n; // always in [0, n-1] regardless of offset's sign
    float degree_ratio = (degree == 0) ? 1.0f : g_tuning_table.degree_ratio[degree - 1];
    float period_ratio = g_tuning_table.degree_ratio[n - 1];
    return knobs->tuning_reference_freq * powf(period_ratio, (float)period_count) * degree_ratio;
}

// Single Program/Sequencer modes; 0-7 in Combi mode). Poly-mode voice claiming stays fully
// program-agnostic (searches/steals across the whole shared pool, per the user's explicit "one
// shared budget" requirement) -- only the claimed voice's program_index stamp and which
// ProgramState's fields/EGs/LFO-key-sync get read are program-specific. Mono mode reserves voice
// slot `target_program` itself (0-7) as that Program's own dedicated single voice, so multiple
// simultaneously-mono-mode Combi Programs never collide on voice 0 -- a direct generalization of
// today's hardcoded "mono always uses voice 0", byte-identical when target_program is always 0
// (Single Program/Sequencer modes).
void handle_note_on(HdsSampleEngine *se, SynthVoice voices[], SynthSharedControls *knobs, float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT], int target_program, int note, int velocity, bool poly_mode_active, MonoNoteStack *mono_stack) {
    // FX_TYPE_HARDTUNE's own global note tracker (see its own comment) -- unconditional on
    // poly_mode_active, unlike the Program-scoped mono_stack just below.
    mono_stack_push(&g_hardtune_note_stack, note, velocity);
    g_hardtune_held_note = (float)note;

    const ProgramState *prog = &knobs->programs[target_program];
    int slot = -1;
    if (poly_mode_active) {
        // BUG FIX: this used to only check vco1_eg/vco2_eg, so a voice whose vco3_eg was
        // still audibly releasing (Osc 3 Amp envelope) could get stolen and reset mid-tail
        // -- an audible click, not just a modulation glitch, since Osc 3 is a
        // real sound source. vco3_eg must be idle too before a voice counts as free.
        int voice_limit = effective_voice_limit(knobs);
        for (int v = 0; v < voice_limit; v++) { if (voices[v].vco1_eg.state == STATE_IDLE && voices[v].vco2_eg.state == STATE_IDLE && voices[v].vco3_eg.state == STATE_IDLE && voices[v].vco4_eg.state == STATE_IDLE) { slot = v; break; } }
        if (slot == -1) {
            // Polyphony ceiling hit (active_voice_limit simultaneously active voices) -- every
            // slot is busy. FIXED: this used to unconditionally steal slot 0, resetting
            // whatever was playing there regardless of how audible it currently was -- a hard,
            // audible click/cutoff on an arbitrary note, every time. Instead, steal the voice a
            // listener is least likely to notice disappearing: prefer one already in RELEASE
            // (fading out on its own) with the lowest combined level, and only fall back to a
            // still-sustaining/attacking voice (by lowest level) if literally none are
            // releasing.
            float best_score = 1e30f; // lower = safer to steal
            for (int v = 0; v < voice_limit; v++) {
                float level = voices[v].vco1_eg.current_level + voices[v].vco2_eg.current_level + voices[v].vco3_eg.current_level + voices[v].vco4_eg.current_level;
                bool releasing = (voices[v].vco1_eg.state == STATE_RELEASE || voices[v].vco2_eg.state == STATE_RELEASE || voices[v].vco3_eg.state == STATE_RELEASE || voices[v].vco4_eg.state == STATE_RELEASE);
                // A releasing voice always beats a non-releasing one regardless of level --
                // it's already fading out by the player's own intent (key lifted), so cutting
                // it a little early reads as natural rather than as a glitch.
                float score = releasing ? level : (level + 1000.0f);
                if (score < best_score) { best_score = score; slot = v; }
            }
            // DIAGNOSTIC: confirms whether you're actually hitting the polyphony ceiling at
            // all -- if this never prints, the Polyphony control isn't the thing to look at for
            // an underrun/glitch you're chasing.
            fprintf(stderr, "[voices] polyphony ceiling hit (%d active) -- stole slot %d (score %.4f)\n", voice_limit, slot, best_score);
        }
    } else { slot = target_program; }

    float target_hz = hds_note_to_hz(knobs, note);
    voices[slot].program_index = target_program;
    voices[slot].midi_note = note;
    voices[slot].midi_velocity = velocity;
    voices[slot].vco1_target_freq = target_hz;
    voices[slot].vco2_target_freq = target_hz;
    voices[slot].vco3_target_freq = target_hz;
    voices[slot].vco4_target_freq = target_hz;
    // Any note-on onto this slot makes a previously-deferred sustain release moot -- either the
    // slot was stolen from a sustain-pending voice (about to be fully reset below regardless),
    // or this is a fresh key that has nothing to do with whatever the pedal was holding.
    voices[slot].sustain_pending = false;

    // Mono Legato: only retrigger (reset phase/filters/envelopes) when this is genuinely the
    // first key of a phrase. If another key is already held when this one comes down, it's an
    // overlapping legato note -- just glide pitch (portamento) into the still-running envelope.
    bool is_first_key_of_phrase = poly_mode_active || (mono_stack->count == 0);

    if (is_first_key_of_phrase) {
        voices[slot].vco1_phase = 0.0f; voices[slot].vco2_phase = 0.0f; voices[slot].vco3_phase = 0.0f; voices[slot].vco4_phase = 0.0f;
        voices[slot].vco1_prev_fm_phase = 0.0f; voices[slot].vco3_prev_fm_phase = 0.0f; voices[slot].vco4_prev_fm_phase = 0.0f;
        voices[slot].vco1_tri_state = 0.0f; voices[slot].vco2_tri_state = 0.0f; voices[slot].vco3_tri_state = 0.0f; voices[slot].vco4_tri_state = 0.0f;
        voices[slot].vco2_pending_sync_correction = 0.0f; voices[slot].vco3_pending_sync_correction = 0.0f; voices[slot].vco4_pending_sync_correction = 0.0f;
        voices[slot].vco1_current_freq = target_hz; voices[slot].vco2_current_freq = target_hz;
        voices[slot].vco3_current_freq = target_hz; voices[slot].vco4_current_freq = target_hz;
        voices[slot].glide_time_multiplier = 1.0f; // fresh note -- no glide happening, nothing to randomize

        // WAVE_SAMPLE: (re)claim a DFD stream for whichever oscillator is currently set to it,
        // and release any stream a stolen/retriggered voice was still holding from a previous
        // note even if this new note doesn't need one -- see start_sample_osc()'s comment.
        // Note: trigger=legato zones (see HDS_SFZ_TRIGGER_LEGATO) can only ever match on this
        // call, gated is_first_key_of_phrase==false -- but this whole block only runs when
        // is_first_key_of_phrase is TRUE (Mono Legato's own overlapping-note glide path never
        // reaches start_sample_osc at all, by original design: it deliberately never retriggers
        // the sample oscillator, only glides pitch on the already-running stream). A genuinely
        // legato-triggered zone is therefore a documented, narrow limitation, not a bug: it's
        // parsed and matched correctly everywhere is_first_key_of_phrase reaches false, which
        // today is nowhere. trigger=first zones are unaffected (Poly mode, the common case,
        // always has is_first_key_of_phrase true here).
        if ((WaveType)((int)prog->vco1_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco1_sample_stream, &voices[slot].vco1_sample_ptr, &voices[slot].vco1_sample_preroll, 0, target_program, note, velocity,
                              is_first_key_of_phrase, &voices[slot].vco1_sample_delay_remaining,
                              &voices[slot].vco1_layer_sample_stream, &voices[slot].vco1_layer_sample_ptr, &voices[slot].vco1_layer_sample_preroll,
                              &voices[slot].vco1_xfade_gain, &voices[slot].vco1_layer_xfade_gain);
        } else {
            if (voices[slot].vco1_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco1_sample_stream); voices[slot].vco1_sample_stream = -1; }
            if (voices[slot].vco1_layer_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco1_layer_sample_stream); voices[slot].vco1_layer_sample_stream = -1; }
        }
        if ((WaveType)((int)prog->vco2_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco2_sample_stream, &voices[slot].vco2_sample_ptr, &voices[slot].vco2_sample_preroll, 1, target_program, note, velocity,
                              is_first_key_of_phrase, &voices[slot].vco2_sample_delay_remaining,
                              &voices[slot].vco2_layer_sample_stream, &voices[slot].vco2_layer_sample_ptr, &voices[slot].vco2_layer_sample_preroll,
                              &voices[slot].vco2_xfade_gain, &voices[slot].vco2_layer_xfade_gain);
        } else {
            if (voices[slot].vco2_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco2_sample_stream); voices[slot].vco2_sample_stream = -1; }
            if (voices[slot].vco2_layer_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco2_layer_sample_stream); voices[slot].vco2_layer_sample_stream = -1; }
        }
        if ((WaveType)((int)prog->vco3_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco3_sample_stream, &voices[slot].vco3_sample_ptr, &voices[slot].vco3_sample_preroll, 2, target_program, note, velocity,
                              is_first_key_of_phrase, &voices[slot].vco3_sample_delay_remaining,
                              &voices[slot].vco3_layer_sample_stream, &voices[slot].vco3_layer_sample_ptr, &voices[slot].vco3_layer_sample_preroll,
                              &voices[slot].vco3_xfade_gain, &voices[slot].vco3_layer_xfade_gain);
        } else {
            if (voices[slot].vco3_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco3_sample_stream); voices[slot].vco3_sample_stream = -1; }
            if (voices[slot].vco3_layer_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco3_layer_sample_stream); voices[slot].vco3_layer_sample_stream = -1; }
        }
        if ((WaveType)((int)prog->vco4_shape) == WAVE_SAMPLE) {
            start_sample_osc(se, &voices[slot].vco4_sample_stream, &voices[slot].vco4_sample_ptr, &voices[slot].vco4_sample_preroll, 3, target_program, note, velocity,
                              is_first_key_of_phrase, &voices[slot].vco4_sample_delay_remaining,
                              &voices[slot].vco4_layer_sample_stream, &voices[slot].vco4_layer_sample_ptr, &voices[slot].vco4_layer_sample_preroll,
                              &voices[slot].vco4_xfade_gain, &voices[slot].vco4_layer_xfade_gain);
        } else {
            if (voices[slot].vco4_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco4_sample_stream); voices[slot].vco4_sample_stream = -1; }
            if (voices[slot].vco4_layer_sample_stream >= 0) { release_sample_stream(se, voices[slot].vco4_layer_sample_stream); voices[slot].vco4_layer_sample_stream = -1; }
        }
        for(int c=0; c<2; c++) { for(int f=0; f<5; f++) { voices[slot].f_low[c][f][0]=0.0f; voices[slot].f_low[c][f][1]=0.0f; voices[slot].f_band[c][f][0]=0.0f; voices[slot].f_band[c][f][1]=0.0f; } }
        for (int l = 0; l < LFO_COUNT; l++) {
            if (prog->lfo[l].key_sync == 0.0f) continue;
            // Per-Key mode: reset only THIS voice's own phase (every other voice's own per-key
            // cycle is untouched) -- the whole point of Per-Key mode. Global mode: reset the one
            // shared phase every voice of this Program reads, exactly as today.
            if (prog->lfo[l].per_key_mode != 0.0f) {
                voices[slot].lfo_phase[l] = 0.0f;
                voices[slot].lfo_decim_counter[l] = 0;
                voices[slot].lfo_decim_last_value[l] = 0.0f;
                voices[slot].lfo_decim_target_value[l] = 0.0f;
            } else {
                global_lfo_phases[target_program][l] = 0.0f;
            }
        }

        // SFZ per-zone amp envelope override: resolve, per oscillator, whether the zone just
        // (re)claimed above sets its own ampeg_* opcodes and the Program's own override checkbox
        // isn't forcing its shared EG instead -- see ProgramState.vco1_force_own_amp_eg's own
        // comment. False (own_eg_rates untouched/unused) for every oscillator not currently playing
        // such a zone, which then falls through to the Program's own eg_mode[]/eg[] exactly as
        // before, byte-identical to pre-this-feature behavior.
        voices[slot].vco1_using_own_ampeg = resolve_zone_owned_ampeg(se, voices[slot].vco1_sample_stream, prog->vco1_force_own_amp_eg != 0.0f, &voices[slot].vco1_own_eg_rates);
        voices[slot].vco2_using_own_ampeg = resolve_zone_owned_ampeg(se, voices[slot].vco2_sample_stream, prog->vco2_force_own_amp_eg != 0.0f, &voices[slot].vco2_own_eg_rates);
        voices[slot].vco3_using_own_ampeg = resolve_zone_owned_ampeg(se, voices[slot].vco3_sample_stream, prog->vco3_force_own_amp_eg != 0.0f, &voices[slot].vco3_own_eg_rates);
        voices[slot].vco4_using_own_ampeg = resolve_zone_owned_ampeg(se, voices[slot].vco4_sample_stream, prog->vco4_force_own_amp_eg != 0.0f, &voices[slot].vco4_own_eg_rates);

        // Drum Bus routing: resolve, per oscillator, whether the zone just (re)claimed above is
        // routed to a Drum Bus -- see DRUM_BUS_COUNT's own comment. 0 (off) for every oscillator
        // not currently playing such a zone, the ordinary, unaffected case.
        voices[slot].vco1_drum_bus = resolve_zone_drum_bus(se, voices[slot].vco1_sample_stream);
        voices[slot].vco2_drum_bus = resolve_zone_drum_bus(se, voices[slot].vco2_sample_stream);
        voices[slot].vco3_drum_bus = resolve_zone_drum_bus(se, voices[slot].vco3_sample_stream);
        voices[slot].vco4_drum_bus = resolve_zone_drum_bus(se, voices[slot].vco4_sample_stream);

        int m0 = (int)prog->eg_mode[0];
        init_amp_eg_start(&voices[slot].vco1_eg, voices[slot].vco1_using_own_ampeg, &voices[slot].vco1_own_eg_rates, m0, &prog->eg[0]);
        int m1 = (int)prog->eg_mode[1];
        init_amp_eg_start(&voices[slot].vco2_eg, voices[slot].vco2_using_own_ampeg, &voices[slot].vco2_own_eg_rates, m1, &prog->eg[1]);
        // Osc 3/4 Amp envelopes: eg[2]/eg_mode[2] and eg[3]/eg_mode[3].
        int mn = (int)prog->eg_mode[2];
        init_amp_eg_start(&voices[slot].vco3_eg, voices[slot].vco3_using_own_ampeg, &voices[slot].vco3_own_eg_rates, mn, &prog->eg[2]);
        int m3 = (int)prog->eg_mode[3];
        init_amp_eg_start(&voices[slot].vco4_eg, voices[slot].vco4_using_own_ampeg, &voices[slot].vco4_own_eg_rates, m3, &prog->eg[3]);
        for(int e=0; e<ASSIGNABLE_EG_COUNT; e++) {
            int me = (int)prog->eg_mode[4 + e]; voices[slot].assign_eg[e].state = (me == 5) ? STATE_DELAY : STATE_ATTACK;
            voices[slot].assign_eg[e].current_level = (me == 5) ? 0.0f : fmaxf(0.0f, fminf(1.0f, prog->eg[4 + e].al / 100.0f));
        }
    } else {
        // An overlapping legato note: this is an actual audible glide, so reroll its time.
        voices[slot].glide_time_multiplier = roll_glide_multiplier(prog->mono_glide_random);
    }
    if (!poly_mode_active) mono_stack_push(mono_stack, note, velocity);
}

// Puts a voice's envelopes into RELEASE from wherever they currently are. Factored out since
// it's needed from three places: a normal note-off (poly and mono), and releasing every voice
// that was deferred by the sustain pedal once it lifts (see handle_sustain_release).
static void trigger_voice_release(HdsSampleEngine *se, SynthVoice *voice) {
    voice->vco1_eg.release_level = voice->vco1_eg.current_level; voice->vco1_eg.state = STATE_RELEASE;
    voice->vco2_eg.release_level = voice->vco2_eg.current_level; voice->vco2_eg.state = STATE_RELEASE;
    voice->vco3_eg.release_level = voice->vco3_eg.current_level; voice->vco3_eg.state = STATE_RELEASE;
    voice->vco4_eg.release_level = voice->vco4_eg.current_level; voice->vco4_eg.state = STATE_RELEASE;
    for (int e = 0; e < ASSIGNABLE_EG_COUNT; e++) { voice->assign_eg[e].release_level = voice->assign_eg[e].current_level; voice->assign_eg[e].state = STATE_RELEASE; }
    // SFZ loop_sustain support: tell each oscillator's claimed sample stream (if any) that this
    // voice just entered release -- sample_disk_thread_main reads this to decide whether a
    // loop_release_exit zone should keep looping. A no-op for any oscillator not currently
    // playing a WAVE_SAMPLE zone (request_sample_stream_release ignores stream_index -1).
    request_sample_stream_release(se, voice->vco1_sample_stream);
    request_sample_stream_release(se, voice->vco2_sample_stream);
    request_sample_stream_release(se, voice->vco3_sample_stream);
    request_sample_stream_release(se, voice->vco4_sample_stream);
    // Crossfade-layer partner streams (SFZ xfin_*/xfout_*) need the exact same loop_sustain
    // release notification as the primary stream above -- see SynthVoice.vco1_layer_sample_stream's
    // own comment. A no-op for every oscillator without an active layer partner (stream -1).
    request_sample_stream_release(se, voice->vco1_layer_sample_stream);
    request_sample_stream_release(se, voice->vco2_layer_sample_stream);
    request_sample_stream_release(se, voice->vco3_layer_sample_stream);
    request_sample_stream_release(se, voice->vco4_layer_sample_stream);
    // SFZ trigger=release/release_key: fire any matching release-tail zone, using this voice's own
    // held note/velocity/Program -- see fire_release_trigger_zones's own comment. Gated on this
    // voice's own vco{N}_sample_stream actually being a live claimed stream (exactly what
    // request_sample_stream_release above already reads, unchanged by that call -- it only flags
    // the stream struct itself, never touches this field), NOT just "does this oscillator's slot
    // catalog contain a matching zone": that catalog-only check used to fire a STALE multisample's
    // own release zone even after the oscillator's Shape had been switched away to a plain
    // waveform (an oscillator with no live sample stream can never have entered WAVE_SAMPLE
    // playback for THIS voice in the first place, so this is a strictly narrower, correct
    // condition, not a behavior change for any voice that actually is playing a sample).
    if (voice->midi_note >= 0) {
        if (voice->vco1_sample_stream >= 0) fire_release_trigger_zones(se, 0, voice->program_index, voice->midi_note, voice->midi_velocity);
        if (voice->vco2_sample_stream >= 0) fire_release_trigger_zones(se, 1, voice->program_index, voice->midi_note, voice->midi_velocity);
        if (voice->vco3_sample_stream >= 0) fire_release_trigger_zones(se, 2, voice->program_index, voice->midi_note, voice->midi_velocity);
        if (voice->vco4_sample_stream >= 0) fire_release_trigger_zones(se, 3, voice->program_index, voice->midi_note, voice->midi_velocity);
    }
}

void handle_note_off(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs, const MidiLiveState *midi_live, int target_program, int note, bool poly_mode_active, MonoNoteStack *mono_stack) {
    // FX_TYPE_HARDTUNE's own global note tracker (see its own comment) -- unconditional on
    // poly_mode_active, unlike the Program-scoped mono_stack below.
    mono_stack_remove(&g_hardtune_note_stack, note);
    g_hardtune_held_note = (g_hardtune_note_stack.count > 0)
        ? (float)g_hardtune_note_stack.notes[g_hardtune_note_stack.count - 1] : -1.0f;

    if (poly_mode_active) {
        int voice_limit = effective_voice_limit(knobs);
        for (int v = 0; v < voice_limit; v++) { if (voices[v].midi_note == note && voices[v].program_index == target_program) {
            // Sustain pedal held: defer the actual release until it lifts (see
            // handle_sustain_release) instead of releasing right now.
            if (midi_live->sustain_held) voices[v].sustain_pending = true;
            else trigger_voice_release(se, &voices[v]);
        } }
        return;
    }

    // Mono mode: this Program's dedicated voice is always voices[target_program] -- see
    // handle_note_on's own comment on why (matches its slot = target_program assignment).
    SynthVoice *mono_voice = &voices[target_program];
    mono_stack_remove(mono_stack, note);
    if (mono_stack->count > 0) {
        // FIXED: another key is still physically held -- fall back to it (the note most
        // recently pressed among those still down) instead of cutting the sound. This is the
        // core mono-mode bug: releasing whichever key came down last used to kill the note
        // even when an earlier key was still held.
        int fallback_note = mono_stack->notes[mono_stack->count - 1];
        float target_hz = hds_note_to_hz(knobs, fallback_note);
        mono_voice->midi_note = fallback_note;
        mono_voice->midi_velocity = mono_stack->velocities[mono_stack->count - 1];
        mono_voice->vco1_target_freq = target_hz;
        mono_voice->vco2_target_freq = target_hz;
        mono_voice->vco3_target_freq = target_hz;
        mono_voice->vco4_target_freq = target_hz;
        // Falling back to a still-held note is also a real audible glide -- reroll its time too.
        mono_voice->glide_time_multiplier = roll_glide_multiplier(knobs->programs[target_program].mono_glide_random);
        // Falling back to a still-held note never retriggers or releases -- the sound never stopped.
    } else if (midi_live->sustain_held) {
        mono_voice->sustain_pending = true;
    } else {
        trigger_voice_release(se, mono_voice);
    }
}

// Called when the sustain pedal transitions held -> released: actually releases every voice
// that had a note-off deferred while it was held.
void handle_sustain_release(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs) {
    int voice_limit = effective_voice_limit(knobs);
    for (int v = 0; v < voice_limit; v++) {
        if (voices[v].sustain_pending) {
            voices[v].sustain_pending = false;
            trigger_voice_release(se, &voices[v]);
        }
    }
}
