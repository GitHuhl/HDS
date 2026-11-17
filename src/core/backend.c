// hds_core: the shared DSP/OSC/state engine -- oscillators, filters, the FX chain, the OSC
// control protocol, the sequencer/arpeggiator, sample streaming, and every other part of this
// synth that doesn't touch a physical device directly. Compiled once as its own object
// (build/backend.o) and linked into both synth_engine (src/hds_standalone.c, which owns the
// ALSA/PipeWire audio device and main()) and hds.clap (src/clap_plugin.c, whose host owns the
// device). backend.h is the interface those files use; backend_types.h holds the data model.
//
// This project targets Linux only -- see shell.nix's own header comment for why Windows support
// was dropped entirely (the real, ongoing cost of maintaining a second audio backend, WASAPI vs.
// ALSA).
#include "core/backend.h"
#include "core/sample_engine.h" // sample-oscillator hooks (src/core/sample_engine.c)
#include "core/engine_internal.h"

#include "core/hds_compat.h" // realpath()/NAME_MAX on Windows

// FX_TYPE_NAM (Neural Amp Modeler) needs a small vendored C++ library (nam_bridge.h/.cpp over
// third_party/NeuralAmpModelerCore) linked in -- same "opt-in dependency, detected/toggled by the
// Makefile" posture as HDS_WAVPACK_ENABLED/HDS_PIPEWIRE_ENABLED (now in hds_standalone.c -- see
// its own header comment), needed for one real reason: the `arm64` cross-build target (see the
// Makefile's own comment) has no C++ cross-compiler
// or cross-built Eigen wired up, so it builds with this off, and every bridge-touching line below
// (not the harmless plain-data FX_TYPE_NAM enum value/param layout/wire fields, which stay
// identical either way so a project file is portable between an HDS_NAM_ENABLED and non-enabled
// build) is guarded accordingly. On by default -- unlike PipeWire, this isn't probing for an
// optional system package (Eigen/NeuralAmpModelerCore are vendored, always present), so every
// normal x86_64 build gets it for free with nothing to detect.
// HDS_NAM_ENABLED's default (1) lives in hds_config.h.
#if HDS_NAM_ENABLED
#include "fx/nam_bridge.h" // extern "C" shim over NeuralAmpModelerCore -- see that header's own
                         // comment and FX_TYPE_NAM below; this is the ONLY C++ this file touches,
                         // and only indirectly through these few functions.
#endif

volatile sig_atomic_t g_synth_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    g_synth_running = 0;
}

// Programmable tempo: resolves the Tempo knob (tempo_bpm) plus an optional tempo-map
// (tempo_marker_count time/bpm pairs) into a single "what's the tempo right now" value.
//
// With no markers programmed (the common case, and currently the only case the frontend ever
// sends), this is just tempo_bpm -- a plain flat tempo. With markers, it finds whichever two
// markers bracket `elapsed_seconds` and linearly interpolates between their bpm values; before
// the first marker it holds that marker's bpm flat (rather than extrapolating backwards), and
// after the last it holds the last marker's bpm flat -- the same convention a DAW tempo track
// uses. Markers don't need to be given in time order: this scans for the nearest-before and
// nearest-after markers directly, so out-of-order input still resolves correctly.
float compute_current_tempo_bpm(const SynthSharedControls *knobs, double elapsed_seconds) {
    // Tempo is Program-scoped (see the Program Save/Load allowlist), so "the current tempo" has
    // to resolve to a specific slot -- Combi lane 1 (programs[0]) in Combi mode, matching every
    // other per-Program precompute's own resolution; Single Program mode's own dedicated slot
    // otherwise. Byte-identical to before this fix outside Combi mode (both were programs[0]
    // before Single Program mode and Combi lane 1 were split apart).
    const ProgramState *prog = &knobs->programs[((int)knobs->performance_mode == 1) ? 0 : SINGLE_PROGRAM_SLOT];
    int count = (int)prog->tempo_marker_count;
    if (count < 0) count = 0;
    if (count > TEMPO_MARKER_MAX) count = TEMPO_MARKER_MAX;
    if (count == 0) return prog->tempo_bpm;

    int prev = -1; double prev_time = -1e18;
    int next = -1; double next_time = 1e18;
    for (int i = 0; i < count; i++) {
        double t = (double)prog->tempo_marker_time[i];
        // >= : of several markers at the SAME time, the last one wins once that time is reached,
        // so a pair (t, old_bpm), (t, new_bpm) is an instant step rather than a ramp. Markers at
        // distinct times behave exactly as before. (The SMF importer writes tempo changes this way.)
        if (t <= elapsed_seconds && t >= prev_time) { prev = i; prev_time = t; }
        if (t > elapsed_seconds && t < next_time) { next = i; next_time = t; }
    }
    if (prev < 0 && next < 0) return prog->tempo_bpm; // defensive fallback, shouldn't happen when count > 0
    if (prev < 0) return prog->tempo_marker_bpm[next]; // before the first marker: hold its bpm flat
    if (next < 0) return prog->tempo_marker_bpm[prev]; // after the last marker: hold its bpm flat

    double span = next_time - prev_time;
    double frac = (span > 1e-9) ? (elapsed_seconds - prev_time) / span : 0.0;
    double bpm = (double)prog->tempo_marker_bpm[prev] + frac * ((double)prog->tempo_marker_bpm[next] - (double)prog->tempo_marker_bpm[prev]);
    return (float)bpm;
}

// ============================================================================
// Sequencer audio-lane RAM loading ("Load Audio", SeqLaneState.audio_path)
// ============================================================================

HdsStreamsState g_streams_state;

// The standalone app's own single instance -- same storage duration (process lifetime) and
// zero-initialization (BSS) as every global/static it replaces, and main() remains the only
// caller in the whole program, reaching every value through this one unchanging pointer instead
// of implicitly. Provably behavior-identical by construction; verified empirically per-phase via
// the determinism-check harness (see the plan doc) rather than trusted on this reasoning alone.
HdsEngineState g_engine;

// Temporary bridge macros (multi-instance Phase 2 only): every pre-existing g_* name below now
// expands to the matching g_engine field, so this phase is a pure rename/regroup with zero
// control-flow change. Removed incrementally in later phases as each subsystem's functions gain
// a real `engine` parameter (see the plan doc's phase breakdown).
