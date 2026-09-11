# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, captures external audio from an input
  device, and exposes every synth parameter over a Unix domain socket
  (`/tmp/synth.sock`) as a fixed-layout packed struct (`SynthSharedControls`,
  315 floats + 3 device name strings). Also streams back a peak-level float
  for the GUI's VU meter. The final output stage carries an always-on
  safety limiter (instant attack, ~50ms release) ahead of a hard clamp, so
  an overload (e.g. several voices lining up, or Parallel FX routing
  summing 12 hot slots) degrades to smooth gain reduction rather than
  brick-wall clipping.

  MIDI input auto-connects with no `aconnect` ever needed: at startup it
  scans ALSA sequencer clients and subscribes to every real (kernel-backed,
  not "Midi Through") MIDI source found, and stays subscribed to the ALSA
  announce port so a keyboard plugged in later connects live too (see
  `midi_autoconnect_scan()`/the `SND_SEQ_EVENT_PORT_START` handling in
  `process_midi_input()`). The Audio tab's MIDI Input dropdown can pin one
  specific device by name instead of auto-detecting everything; a pinned
  device still auto-reconnects after an unplug/replug.

  Beyond notes, the MIDI pipeline reads CC (every controller number, live
  in `MidiLiveState.cc[128]`), pitch bend, channel aftertouch, and the
  sustain pedal (CC64, with a polarity switch for pedals wired backwards --
  see `sustain_pedal_polarity`). Pitch bend has independently specifiable
  up/down ranges in semitones (`pb_range_up`/`pb_range_down`). Sustain
  correctly defers note-off until the pedal lifts, in both poly and mono
  modes, without leaking a stale "pending release" into a note that gets
  retriggered while the pedal is still down.
- **`src/frontend.py`** — a PyQt6 control surface. Custom-drawn potentiometer
  knobs and draggable ADSR envelope graphs, tabbed into Global / Play /
  Filters / LFOs / Envelopes / FX / Audio. Packs the same struct
  (`struct.pack("315f64s64s64s", ...)`) and sends it over the socket
  whenever a control changes. If it can't connect to the backend it falls
  back to a mock mode so the UI still runs standalone.

  The Global tab has a Pitch Bend Range group (two spin boxes, up/down, not
  pots -- an incremental control that reads out in semitones) and a Sustain
  Pedal Polarity group (Normal/Inverted), both near the Mono/Poly switch.
  Right-clicking any LFO's Depth pot (without dragging -- a right-click
  *drag* still does the existing fine-adjustment gesture) opens a
  "Modulation Source" menu: None, Mod Wheel, Breath, Foot Controller,
  Expression, Channel Aftertouch, or a custom CC number, which scales that
  LFO's output live off the chosen MIDI source.

Engine features: 2 main oscillators (Saw/Square/Triangle/Sine/Unison
Saw/Noise/Pulse) with FM and hard sync, a dedicated sub oscillator and
noise oscillator, a 2-filter (A/B) architecture where Filter A can itself
be a 3-filter legacy engine, 9 envelope generators (Osc1/Osc2/Noise amp +
6 assignable, each switchable between ADSR/AR/AD/ASR/AHD/DADSR/AHDSR, with
the Sustain stage chaseable in real time while a note is held), 4 LFOs
(each with its own MIDI Modulation Source), and a shared mod matrix
routing LFOs and assignable EGs to pitch, amp, pan, pulse width, filter
cutoff/resonance, FM depth, phase, unison detune, and the noise/sub
oscillators' own amp, pan, and tilt.

A 12-slot FX bus (`FX` tab) runs on the full stereo mix, after Filter A/B
and before master volume, routable Series (each slot fed the previous
slot's output) or Parallel (every slot fed the same input independently,
all 12 summed and averaged) via the routing button at the top of the tab.
External audio (`Audio` tab: output/input device selection, live-
switchable; input enable/gain) is summed in just before the FX bus, so it
passes through the same 12 slots as the synth's own voices. Each slot has
a generic 8-param bank + mix + bypass (`process_fx_chain()` in
`backend.c`); most slot types are still an unimplemented pass-through,
ready for more effects to be ported into the dispatch switch. Two are
implemented so far, both ported from Airwindows (MIT license):

- **Ensemble** — a 12-tap fixed-delay ensemble/chorus (not a reverb: no
  feedback loop). Params: Depth (widens/lengthens the delay taps) and
  FX Level (the effect's own internal wet/dry balance, independent of
  the slot's generic Mix knob).
- **CreamCoat** — a genuine reverb: a 4-stage, 16-line Householder
  feedback delay network run at a reduced, continuously variable internal
  rate with a bezier-curve reconstruction back to full sample rate. Params:
  Select (17 room-size presets), Regen (feedback amount), DeRez (tank
  downsample rate — a character control, not just a CPU knob), Predelay,
  and Wetness (also its own internal blend, not a crossfade — see the
  comment in `run_fx_creamcoat`).

Adding a new effect means: a `FX_TYPE_*` define + its processing function
+ a case in `process_fx_chain()`'s switch (`backend.c`), and a matching
entry in `FX_TYPE_METADATA` (`frontend.py`) giving its real param count,
labels, and ranges — the frontend uses that to relabel/show only the
knobs a given effect actually uses instead of 8 generic "P1..P8" dials.

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
