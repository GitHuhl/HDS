// engine_internal.h -- the few cross-file pieces inside the engine split out of backend.c
// (render.c, voices.c, midi_dispatch.c, arpeggiator.c, metronome.c). Not a public interface:
// everything other files use is in backend.h.
#ifndef HDS_ENGINE_INTERNAL_H
#define HDS_ENGINE_INTERNAL_H

#include "core/backend.h"

// Metronome click sound (poll_metronome schedules clicks; render_audio_block renders them).
// --- Metronome state (declared here, ahead of poll_transport_triggers below, so a Seek can
// resync it -- see the `seeked` block's own comment further down). A cheap, deliberately
// hardcoded 400Hz/8ms square-wave click on each beat while PLAYING -- not a Program voice, so it
// never touches the oscillator/filter/envelope machinery (per the ask: "low quality, efficient").
// Scheduled at block granularity (matching how the transport playhead itself only updates once
// per block -- see advance_transport_playhead), which is more than adequate for a monitoring
// click. ---
#define METRONOME_CLICK_HZ 400.0f
#define METRONOME_CLICK_DURATION_SEC 0.008f
#define METRONOME_CLICK_GAIN 0.25f

// Mono note stack (voices.c), also driven by the arpeggiator's note tracking.
void mono_stack_push(MonoNoteStack *stack, int note, int velocity);
void mono_stack_remove(MonoNoteStack *stack, int note);
// Sustain pedal release (voices.c), called from MIDI dispatch.
void handle_sustain_release(HdsSampleEngine *se, SynthVoice voices[], const SynthSharedControls *knobs);
// Arpeggiator note tracking (arpeggiator.c), fed by MIDI dispatch.
void arp_note_on(HdsEngineState *engine, int target_program, int note, int velocity);
void arp_note_off(HdsEngineState *engine, const ProgramState *prog, int target_program, int note);

#endif // HDS_ENGINE_INTERNAL_H
