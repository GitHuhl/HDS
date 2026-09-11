# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, captures external audio from an input
  device, and exposes every synth parameter over a Unix domain socket
  (`/tmp/synth.sock`) as a fixed-layout packed struct (`SynthSharedControls`,
  319 floats + 3 device name strings). Also streams back a peak-level float
  for the GUI's VU meter. The final output stage carries an always-on
  safety limiter (instant attack, ~50ms release) ahead of a hard clamp, so
  an overload (e.g. several voices lining up, or Parallel FX routing
  summing 12 hot slots) degrades to smooth gain reduction rather than
  brick-wall clipping.

  Polyphony and render block size are **live, user-adjustable controls**
  (Audio tab: Polyphony, Block Size), not fixed numbers picked once and
  shipped -- the right value for either is a property of the actual machine
  this runs on, and guessing one number for every deployment either wastes
  headroom on fast hardware or underruns on slow hardware. `MAX_VOICES`
  (256) and `BUFFER_SIZE` (1024) are now just hard array-allocation
  ceilings; `active_voice_limit` and `block_size_frames` are the real,
  live values, clamped into those ceilings by `effective_voice_limit()`/
  `effective_block_size()` and used everywhere a loop needs to know "how
  many voices/samples right now" instead of the old compile-time macros.
  Both apply on the very next block, no restart or stream reopen needed.
  Idle voice slots are still near-free -- the per-voice render loops skip
  a slot the instant it goes fully silent -- so raising Polyphony only
  costs real CPU once that many voices are genuinely active. Voice-stealing
  (once every active slot is full) picks the quietest/already-releasing
  voice instead of blindly resetting slot 0, so hitting the ceiling
  degrades gracefully rather than clicking.

  Three stderr-only diagnostics make tuning Polyphony/Block Size/Latency an
  informed process instead of guesswork: `[voices]` fires from
  `handle_note_on` whenever the polyphony ceiling is actually hit (confirms
  whether Polyphony is even the relevant factor before touching it);
  `[perf]` fires when a block itself took longer to render than its own
  real-time budget, with active voice count and block size; `[audio]` fires
  alongside any ALSA underrun/xrun `snd_pcm_recover` recovers from, with
  the same context. Push Polyphony up, watch the terminal under real
  playing, back off if you see warnings.

  Every active voice's LFO/EG modulation dispatch also skips straight past
  its ~19-branch destination-matching chain when that source isn't routed
  anywhere (`dest_target == 0`/"None") instead of evaluating the whole
  chain for nothing -- a real, measured (~6-10% per block) win on a plain
  unmodulated patch, found via the same real-hardware profiling that led to
  the live Polyphony/Block Size controls above.

  Output latency is set by the ALSA buffer target in `open_pcm_stream()`
  (`audio_latency_ms`, the Audio tab's Latency control -- default 25ms).
  This is independent of Block Size: Latency is how deep ALSA's own ring
  buffer is (the real floor on output latency), Block Size is how many
  frames get rendered and handed to ALSA per main-loop iteration.

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
  knobs and draggable ADSR envelope graphs, tabbed into Play / Filters /
  LFOs / Envelopes / FX / Audio. Packs the same struct
  (`struct.pack("319f64s64s64s", ...)`) and sends it over the socket
  whenever a control changes. If it can't connect to the backend it falls
  back to a mock mode so the UI still runs standalone.

  The Audio tab's Performance group has three real, direct engine controls:
  **Polyphony** (a plain spin box, 1-256 voices, default 32), **Block Size**
  (a dropdown of concrete sample counts: 32/64/128/256/512/1024, default
  128), and **Latency** (a friendly Snappy/Normal/Safe/Extra Safe preset
  dropdown, 15-100ms, over the raw ALSA buffer target -- kept as presets
  since unlike the other two there's no more "direct" unit than milliseconds
  itself). Polyphony and Block Size apply on the very next block; Latency
  applies live too but closes and reopens the output/input stream(s) to do
  it. None of these are a fix for a genuinely overloaded patch (too many
  voices/too expensive a patch stays a "turn Polyphony down or use a
  lighter patch" problem, however big the buffer is) -- they're real,
  serious tuning controls for matching the engine to whatever hardware
  it's actually running on, watched via the backend's stderr diagnostics.

  There is no Global tab -- Program name/Load/Save, Poly/Mono Legato, Hard
  Sync, Pitch Bend Range, Sustain Pedal Polarity, FM Depth, and Glide all
  live in the always-visible left sidebar (above the Output/VU strip)
  instead, since they're relevant no matter which tab is open. The Pitch
  Bend Range group is two spin boxes, up/down, not pots -- an incremental
  control that reads out in semitones. Right-clicking any LFO's Depth pot
  (without dragging -- a right-click *drag* still does the existing
  fine-adjustment gesture) opens a "Modulation Source" menu: None, Mod
  Wheel, Breath, Foot Controller, Expression, Channel Aftertouch, or a
  custom CC number, which scales that LFO's output live off the chosen
  MIDI source.

  Envelope Attack/Decay/Release pots use a logarithmic ("audio taper")
  response (`PotentiometerKnob.setLogTaper`) instead of a linear one, and
  their floor dropped from 5-10ms to 1ms -- turning the knob now spends most
  of its travel on the fast end (1-50ms) where the resolution actually
  matters, rather than spreading it evenly across the full 1-2000ms range.
  The Oscillators/Amp-EGs panel on the Play tab and the Envelopes tab's 6
  ADSRPanels are both wrapped in a `QScrollArea` (same pattern as the FX
  tab) so the window can be resized freely without clipping anything --
  each panel scrolls instead of forcing the whole app to whatever size its
  content needs. Without the Envelopes tab's wrap specifically, its 6
  stacked panels forced a ~1139px minimum window height even while a
  totally different tab was open, since QTabWidget sizes itself to fit its
  largest tab, not just the active one. The LFO/Envelopes tab layouts also
  pin their content to the top (a trailing `addStretch()`) instead of
  letting Qt spread leftover vertical space evenly between rows when the
  window is taller than the content needs.

Engine features: 2 main oscillators (Saw/Square/Triangle/Sine/Unison
Saw/Noise/Pulse) with FM and hard sync, a dedicated sub oscillator and
noise oscillator, a 2-filter (A/B) architecture where Filter A can itself
be a 3-filter Multi engine, 9 envelope generators (Osc1/Osc2/Noise amp +
6 assignable, each switchable between ADSR/AR/AD/ASR/AHD/DADSR/AHDSR, with
the Sustain stage chaseable in real time while a note is held), 4 LFOs
(each with its own MIDI Modulation Source), and a shared mod matrix
routing LFOs and assignable EGs to pitch, amp, pan, pulse width, filter
cutoff/resonance, FM depth, phase, unison detune, and the noise/sub
oscillators' own amp, pan, and tilt.

Filter A and B combine via the Filter Routing Matrix (Filters tab): Series
A->B, Series B->A (the same two filters, reversed order -- audibly
identical to A->B for static cutoff/resonance since two LTI filter cascades
commute, but genuinely different once either is modulated in real time), or
Parallel with an adjustable Blend knob (0 = all A, 100 = all B, 50 = even --
replacing the old fixed 50/50 sum). Filter A's own Multi mode additionally
offers Series/Parallel/Split routing across its 3 internal filters.

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
