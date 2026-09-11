# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, captures external audio from an input
  device, reads MIDI note on/off from an ALSA sequencer port, and exposes
  every synth parameter over a Unix domain socket (`/tmp/synth.sock`) as a
  fixed-layout packed struct (`SynthSharedControls`, 285 floats + 2 device
  name strings). Also streams back a peak-level float for the GUI's VU
  meter.
- **`src/frontend.py`** — a PyQt6 control surface. Custom-drawn potentiometer
  knobs and draggable ADSR envelope graphs, tabbed into Global / Play /
  Filters / LFOs / Envelopes / FX / Audio. Packs the same struct
  (`struct.pack("285f64s64s", ...)`) and sends it over the socket whenever
  a control changes. If it can't connect to the backend it falls back to a
  mock mode so the UI still runs standalone.

Engine features: 2 main oscillators (Saw/Square/Triangle/Sine/Unison
Saw/Noise/Pulse) with FM and hard sync, a dedicated sub oscillator and
noise oscillator, a 2-filter (A/B) architecture where Filter A can itself
be a 3-filter legacy engine, 7 envelope generators (Osc1/Osc2/Noise amp +
4 assignable, each switchable between ADSR/AR/AD/ASR/AHD/DADSR/AHDSR, with
the Sustain stage chaseable in real time while a note is held), 4 LFOs,
and a shared mod matrix routing LFOs and assignable EGs to pitch, amp,
filter cutoff/resonance, FM depth, phase, and unison detune.

A 12-slot serial FX bus (`FX` tab) runs on the full stereo mix, after
Filter A/B and before master volume. External audio (`Audio` tab: output/
input device selection, live-switchable; input enable/gain) is summed in
just before the FX bus, so it passes through the same 12 slots as the
synth's own voices. Every slot is currently a pass-through (`process_fx_chain()`
in `backend.c`) — the routing, wire protocol, and per-slot generic
parameter bank (8 params + mix + bypass) are in place, waiting on real
effect algorithms to be ported into the dispatch switch.

## Build & run

Requires ALSA dev headers (`libasound2-dev` on Debian/Ubuntu) and Python 3
with PyQt6.

```sh
make                    # builds ./synth_engine
pip install -r requirements.txt

./synth_engine &        # start the audio/MIDI engine
python3 src/frontend.py # start the GUI (connects over /tmp/synth.sock)
```

The backend must be running before (or while) the frontend starts for a
live connection; otherwise the frontend runs in mock mode.
