// metronome.c -- the metronome clock (poll_metronome); its click is rendered by render_audio_block.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"

// Called once per block. Deliberately independent of the Sequencer transport (own free-running
// metronome_elapsed_sec clock, not transport_playhead_sec, and no TRANSPORT_PLAYING gate) so the
// metronome works as a plain practice/click-track tool with or without the Sequencer ever being
// used -- matching the Arpeggiator's own transport-independence. Schedules (but does not itself
// render) upcoming clicks: crossing one or more beat boundaries this block arms/re-arms the
// click-sample countdown that render_audio_block's per-sample loop actually renders from.
void poll_metronome(HdsEngineState *engine, const SynthSharedControls *knobs, float current_tempo_bpm, int block_size) {
    if (knobs->metronome_enable == 0.0f) {
        // Not engaged -- keep the next-click schedule pinned to "right now" so re-enabling
        // starts cleanly on the very next beat instead of firing a backlog of missed clicks, and
        // leave the clock itself frozen (nothing needs to advance while there's no schedule to
        // keep).
        engine->metronome_next_click_sec = engine->metronome_elapsed_sec;
        engine->metronome_click_samples_left = 0;
        return;
    }
    float beat_sec = 60.0f / fmaxf(1.0f, current_tempo_bpm);
    while (engine->metronome_elapsed_sec >= engine->metronome_next_click_sec) {
        engine->metronome_next_click_sec += (double)beat_sec;
        engine->metronome_click_samples_left = (int)(METRONOME_CLICK_DURATION_SEC * (float)SAMPLE_RATE);
        engine->metronome_click_phase = 0;
    }
    engine->metronome_elapsed_sec += (double)block_size / (double)SAMPLE_RATE;
}
