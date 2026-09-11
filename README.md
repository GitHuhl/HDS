# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, reads MIDI note on/off from an ALSA
  sequencer port, and exposes every synth parameter over a Unix domain
  socket (`/tmp/synth.sock`) as a fixed-layout packed struct
  (`SynthSharedControls`, 151 floats). Also streams back a peak-level float
  for the GUI's VU meter.
- **`src/frontend.py`** — a PyQt6 control surface. Custom-drawn potentiometer
  knobs and draggable ADSR envelope graphs, tabbed into Global / Play /
  Filters / LFOs / Envelopes. Packs the same 151-float struct
  (`struct.pack("151f", ...)`) and sends it over the socket whenever a
  control changes. If it can't connect to the backend it falls back to a
  mock mode so the UI still runs standalone.

Engine features: 2 main oscillators (Saw/Square/Triangle/Sine/Unison
Saw/Noise/Pulse) with FM and hard sync, a dedicated sub oscillator and
noise oscillator, a 2-filter (A/B) architecture where Filter A can itself
be a 3-filter legacy engine, 7 envelope generators (Osc1/Osc2/Noise amp +
4 assignable, each switchable between ADSR/AR/AD/ASR/AHD/DADSR/AHDSR), 4
LFOs, and a shared mod matrix routing LFOs and assignable EGs to pitch,
amp, filter cutoff/resonance, FM depth, phase, and unison detune.

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
