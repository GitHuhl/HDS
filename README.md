# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, captures external audio from an input
  device, and exposes every synth parameter over a Unix domain socket
  (`/tmp/synth.sock`) as a fixed-layout packed struct (`SynthSharedControls`,
  532 floats + 3 device name strings + 1 file path string + 1 flat sample-
  path-bank blob). Also streams
  back a small status struct (`EngineStatus`: peak level, recording state/
  elapsed time/error, detected input channel count) over the same socket --
  used to be a bare peak float before the Recording tab needed live state
  back from the engine. The final output stage carries an always-on
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

  **Recording** (`Recording` tab) writes 32-bit float WAV (`WavRecorder` in
  backend.c -- IEEE float format tag, so no int conversion/dithering, and
  natively readable by any modern DAW/editor). Two source kinds, freely
  combinable in one take: the synth's own final post-FX stereo output
  (which already includes any processed External Input once `ext_in_enable`
  is on -- they're the same signal by the time either is "processed"), and
  raw per-channel taps directly off whatever the currently-selected Input
  device provides (independent of `ext_in_enable` -- works as a plain
  multichannel recorder for a class-compliant interface with no notes
  playing at all). The capture stream itself is opened with however many
  channels `query_device_channel_count()` detects for that device (not
  hardcoded stereo), so a multichannel interface's extra inputs are
  actually reachable, not just its first two -- `render_audio_block`'s own
  External Input FX-mixing still only ever uses channels 0/1 of that
  capture, extracted into a plain stereo buffer before the call so its
  signature/logic never had to change. Selected channels combine into
  either a genuine multichannel WAV (one file channel per selected source)
  or a stereo downmix (every selected channel summed equally into L/R,
  gain-normalized by `1/sqrt(n)` where `n` is signals-per-side, so
  recording just the synth output alone is a pure unscaled passthrough
  rather than needlessly getting -3dB as if something were being summed).
  The channel *selection* is frozen at record-start time
  (`WavRecorder.sel_*`) -- toggling a checkbox mid-take can't desync an
  already-open file's committed channel layout; start a new recording to
  pick up a changed selection. A capture-device channel-count change mid-
  recording (e.g. switching Input devices) stops the take automatically
  rather than silently scrambling the interleaving.

  All actual disk I/O for recording (`fopen`/`fwrite`/`fseek`/`fclose` --
  opening the file, writing the header, every sample block, and patching/
  closing on stop) runs on a **dedicated writer thread**
  (`record_writer_thread_main`), never on the main audio thread. Buffered
  stdio's `fwrite()` isn't bounded-time-safe: it periodically triggers a
  blocking `write(2)` syscall once its internal buffer fills, and disk
  latency (USB/SD/network storage, or the system under I/O contention) has
  no upper bound the way a block's real-time budget does -- doing that
  write inline in the same loop that paces live audio output would risk
  exactly the underruns the rest of the engine works hard to avoid
  elsewhere. Instead, the audio thread only ever does bounded, allocation-
  free work for recording: `wav_recorder_gather_block()` (pure arithmetic --
  the same downmix/channel-selection logic as always, just no longer
  followed by a write) and `record_ring_push()`, a wait-free push into a
  16MB lock-free SPSC ring buffer (`RecordRing`, C11 `<stdatomic.h>`,
  1<<22 floats -- roughly 4.8s of backlog even at the worst case of 18
  channels @ 48kHz). If the writer thread ever falls behind far enough to
  fill the ring (a genuinely stalled disk), new audio is dropped and
  counted (`[record] writer thread falling behind -- N sample(s)
  dropped...`) rather than the push ever blocking -- losing a moment of
  the *recording* is preferable to risking a dropout in the *live output*,
  which must never stall. Starting/stopping a take is the same kind of
  handshake: the audio thread publishes parameters into `RecordEngine` and
  atomically posts a `RECORD_CMD_START`/`RECORD_CMD_STOP` command (a
  plain-write-then-release-store, paired with an acquire-exchange on the
  writer side), so even the `fopen`/header-write on start and the
  `fseek`-patch/`fclose` on stop happen off the audio thread. Verified with
  a synchronous single-threaded simulation (byte-for-byte WAV output
  unchanged from before this refactor), a real-thread stress test that
  hammers pushes with no throttling at all -- deliberately outrunning the
  writer -- while timing every call (observed max ~0.2ms against a 2.667ms
  budget for a 128-frame block, with reconciled drop counts and zero
  corruption in what did get written), and a clean `-fsanitize=thread` run
  confirming the atomic handshake is race-free.

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

  **Sample playback** (`WAVE_SAMPLE`, a 5th oscillator Shape alongside Saw/
  Square/Triangle/Sine/Unison Saw/Noise/Pulse on Osc 1/2) is direct-from-disk
  (DFD) streaming, not a fully RAM-resident sampler: a small "pre-roll"
  cache (up to `SAMPLE_PRE_ROLL_LIMIT` = 7200 frames, ~150ms) is read into
  RAM synchronously at load time for zero-latency note-on, and everything
  after that streams from disk in real time into a per-voice ring buffer
  (`SampleStream.ring_l/ring_r`, `SAMPLE_RING_SIZE` = 32768 frames). Adapted
  to this engine's own architecture rather than bringing in a separate one:
  a `WAVE_SAMPLE` oscillator's output is just another mono float handed to
  the exact same downstream amp-EG/pan/filter/mod-matrix/FX pipeline every
  other oscillator uses, and its playback rate is the same fully-modulated
  `hz` (fine tune, pitch bend, every mod-matrix pitch destination,
  portamento) already computed for the periodic oscillators, divided by the
  sample's own root-note frequency -- pitch bend/vibrato/EG-to-pitch all
  work on a sample voice for free, with zero duplicated pitch logic. A
  small fixed pool (`MAX_SAMPLE_STREAMS` = 32, well under `MAX_VOICES`)
  is claimed by an oscillator at note-on and released once that oscillator's
  own amp envelope reaches `STATE_IDLE` (`release_idle_sample_streams()`,
  called once per active voice per block) -- mirroring how FX-slot state
  and voice-stealing already work here, rather than embedding a ring buffer
  per-voice-per-oscillator.

  All disk I/O (`fopen`/`fseek`/`fread`/`fclose`) runs on a **dedicated
  streaming thread** (`sample_disk_thread_main`), the same realtime-safety
  discipline as the WAV recorder above: the audio thread only ever claims/
  releases a pool slot and reads/advances a lock-free ring buffer (C11
  `<stdatomic.h>`, acquire/release read/write indices); starting or
  stopping a stream is the same tiny atomic command handshake
  (`SAMPLE_CMD_START`/`STOP`, `atomic_exchange_explicit`) used for the
  recorder's start/stop. A claimed pool slot's `slot_index` (which
  `g_sample_slots[]` entry it's streaming) is itself atomic and captured
  once into a disk-thread-private copy the instant `SAMPLE_CMD_START` is
  consumed, rather than re-read from the audio-thread-owned field on every
  later pass -- closing a real race a fast release-then-reclaim of the same
  pool slot could otherwise hit (caught and fixed via `-fsanitize=thread`).
  A loop region (`loop_start`/`loop_end`, real per-zone points set from the
  Mapping tab's waveform editor -- whole-file by default) crossfades the
  tail of the ring into its start over `SAMPLE_XFADE_SIZE` (1024) frames to
  avoid an audible seam, with a minimum enforced loop length of one full
  crossfade window so a very tight loop can't leave that crossfade
  permanently active instead of a brief transition; `loop_enabled` (also
  per-zone) turned off makes it a genuine one-shot that releases its
  stream cleanly at natural end-of-data instead of looping at all -- e.g.
  drum hits. Only the first two channels of a multichannel WAV are read;
  PCM 16/24/32-bit and 32-bit float WAV are supported.

  A genuinely stereo file keeps its real L/R width instead of being
  downmixed -- the pre-roll cache and DFD ring buffer already track L and R
  completely independently end to end (interpolation, loop-splice
  crossfade, everything), so `render_sample_osc()` hands its real stereo
  pair back via optional `out_l`/`out_r` pointers, exactly mirroring
  `render_unison_saw()`'s own `side_l`/`side_r` convention (its mono return
  value stays available for FM-modulator use). `render_audio_block()`
  treats a stereo `WAVE_SAMPLE` zone the same way it already treats Unison
  Saw for the stereo-vs-mono mixer selection: the file's own L/R image is
  used directly, with the oscillator's Pan knob (plus any per-zone Pan, see
  the Mapping tab's Zone Inspector) applied as an overall balance shift on
  top of that width rather than collapsing it to mono first -- a mono file
  degenerates to the exact same math as before (L and R are trivially
  identical), so nothing changes there.

  Which loaded slot actually plays is real per-note/velocity zone mapping
  (SFZ-style): every loaded slot is also a zone (`min_note`/`max_note`,
  `min_velocity`/`max_velocity`, plus an `unpitched` flag for DRUM/one-shot
  samples that should always play at their native rate).
  `find_matching_sample_slot(note, velocity)` picks whichever loaded slot's
  key range AND velocity range both cover the note -- a note outside every
  zone is silent, not "nearest root note always plays something."
  Overlapping zones resolve to the smallest (most specific) match. This
  depends on the engine actually tracking MIDI velocity, which nothing here
  did before it (not even amp/filter response for the periodic
  oscillators): note-on velocity is now captured per voice
  (`SynthVoice.midi_velocity`) and threaded through `handle_note_on`; mono
  mode's held-note fallback stack (`MonoNoteStack`) carries each stacked
  note's own velocity too, so falling back to an earlier still-held key
  restores that key's velocity rather than leaking whichever note was
  struck last.

  Several slots sharing a **round-robin group** (`round_robin_group`, 0 =
  independent, up to `MAX_ROUND_ROBIN_GROUPS` = 256 distinct groups) and an
  equally-specific matching zone cycle through each other one hit at a time
  instead of the same one always winning -- `find_matching_sample_slot()`
  collects every tied-most-specific match (tie-broken by lowest `zone_id`,
  not raw array position), then rotates through whichever group the first
  candidate belongs to via a small per-group hit counter, falling back to
  the original deterministic behavior for an ungrouped tie so nothing
  changes for anyone not opting in. There's still no program-save
  capability -- that remains future work once this and the rest of the
  engine are proven solid together.

  **The Sample Bank has no product-facing cap on zone count** -- a
  multisample instrument with thousands of zones is exactly as valid as one
  with three. This works because the catalog lives entirely off
  `SynthSharedControls` (the fixed struct every other knob rides, resent in
  full on every unrelated control change): it travels instead over its own
  dedicated, event-driven Unix socket (`/tmp/synth_samples.sock`, see
  `setup_sample_catalog_socket()`/`poll_sample_catalog_socket()`), carrying
  two incremental message types -- `ZONE_OP_UPSERT` (add-or-update one or
  more zones by a stable `zone_id`, the frontend's own ever-incrementing,
  never-reused counter) and `ZONE_OP_REMOVE` (delete by `zone_id`) -- so
  editing one zone stays cheap even with thousands mapped, and every other
  knob's own packet shrinks back down as a side effect. `g_sample_slots[]`
  is one large array (`SAMPLE_SLOT_CAPACITY` = 100,000, ~34MB) preallocated
  once at startup and never resized: `sample_disk_thread_main()`
  dereferences into it directly with no lock, relying on its base address
  never moving for the process's whole life, so true `realloc()`-based
  growth would be a genuine use-after-free race against that thread rather
  than a style choice -- a fixed-but-huge ceiling sidesteps that
  entirely. Removed zones are tombstoned (`SampleSlot.active = false`), not
  compacted or reused, avoiding any need to reason about recycling a slot a
  streaming voice might still reference. Loading a WAV is still a blocking
  call on the audio thread, but throttled rather than done inline: a newly
  added or path-changed zone queues into `g_pending_loads[]` (audio-thread-
  only, no cross-thread synchronization needed), drained at
  `SAMPLE_LOADS_PER_BLOCK` (1) per block -- bounding the worst case added
  latency to "one file's blocking I/O," the same worst case already
  accepted before this. A zone whose queued load is still pending when a
  `ZONE_OP_REMOVE` for it arrives is left alone by the drain once it
  finally runs (checked via `.active`), rather than resurrecting a
  just-removed slot.
- **`src/frontend.py`** — a PyQt6 control surface. Custom-drawn potentiometer
  knobs and draggable ADSR envelope graphs, tabbed into Play / Filters /
  LFOs / Envelopes / FX / Audio / Recording / Mapping. Packs the same struct
  (`struct.pack("372f64s64s64s256s", ...)`) and sends it over the socket
  whenever a control changes; the Mapping tab's Sample Bank zones ride a
  second, separate socket instead (see below) rather than this one. If it
  can't connect to the backend it falls back to a mock mode so the UI still
  runs standalone.

  The **Recording** tab: a "Synth Output" checkbox, a grid of "In N"
  checkboxes rebuilt live to match whatever channel count the backend's
  status feedback reports for the current Input device, a Multichannel/
  Stereo Downmix radio choice, a Recordings-folder picker (default
  `~/HDS_Recordings`, browsable), and a Record/Stop toggle. Pressing
  Record generates a fresh `HDS_<timestamp>.wav` path in that folder --
  consecutive takes never collide -- and the status line shows live
  elapsed time (or a failure message, e.g. nothing selected/unwritable
  folder, that also cleanly clears `record_enable` on the wire rather than
  leaving the button checked against a backend that already gave up).

  The **Mapping** tab is the real Sample Bank zone editor for the DFD
  engine above: drag-and-drop a `.wav` file onto a reference piano
  keyboard to create a zone there (or use the "+ Add Sample" button as a
  fallback, which does the same thing at Middle C); drag a zone's body to
  move it, an edge to resize its key or velocity range; shift-click or
  marquee-drag to select multiple zones; Delete/Backspace to remove them.
  A Root Key dropdown (128 notes + "DRUM (Unpitched)") sets the selected
  zone's own pitch-reference note independent of its key range. Every
  edit is live -- no separate "Apply" step, same convention as every
  other control in this file -- but instead of riding the shared knob
  packet, each edit calls `transmit_zone_upsert()`/`transmit_zone_remove()`
  (`SamplerMappingGrid`, adapted from a standalone prototype into this
  file's real backend instead of that prototype's `print()`-based stubs),
  sending only the zone(s) actually touched over the dedicated catalog
  socket. No cap on how many zones can exist at once -- each is identified
  by its own stable `zone_id` (an ever-incrementing counter,
  `SampleZone.zone_id`), not a fixed backend slot index. Set an
  oscillator's Shape (Play tab) to "Sample" and play. Each note's own lane
  (in both the grid above and the
  reference keyboard below it) is a strict, non-overlapping partition of
  the available width, not a physically-realistic key shape that
  overhangs its neighbors -- a real piano's black keys visually overhang
  the seam between their two white neighbors, which is correct for a
  piano but means a drop or drag near a boundary could resolve to the
  wrong note here; every key gets its own exclusive pixel range instead,
  so a zone always snaps cleanly to the note it's actually mapped to.

  Selecting exactly one zone also shows it in the **Zone Inspector**
  (`ZoneInspectorPanel`) below the keyboard: a Loop checkbox, a
  Round-Robin Group spin box (0 = independent), and a waveform view
  (`SampleLoopEditor`, decoded straight from the WAV file with numpy --
  entirely separate from the backend's own DFD streaming, purely to draw
  a picture) with draggable orange/red Loop Start/Loop End markers over
  a shaded loop region. Dragging a marker sends the new loop point live,
  same as every other edit here. Give two or more overlapping zones
  (typically several velocity-matched variations of the same hit, e.g.
  a handful of snare samples) the same non-zero Round-Robin Group and
  they cycle through each other on successive hits instead of the same
  one always winning; round-robin zones are cascaded a few pixels apart
  in the grid (with a colored "RRn" badge) rather than fully overlapping,
  so every member stays visible and individually clickable.

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
  Sync, Pitch Bend Range, Sustain Pedal Polarity, FM Depth, Glide, and
  Tempo all live in the always-visible left sidebar (above the Output/VU
  strip) instead, since they're relevant no matter which tab is open. The
  Tempo knob (20-300 BPM, default 120) sets `tempo_bpm`; a small label
  under it mirrors the backend's own resolved value (`EngineStatus.
  tempo_bpm_live`, round-tripped over the same status socket as the VU
  meter/recording state), proving the value actually reaches the engine
  end to end. Nothing in the DSP consumes tempo yet -- this is groundwork
  for future tempo-synced features (e.g. delay time, LFO rate) -- but the
  backend already implements a full programmable tempo-MAP system, not
  just a flat number: `tempo_marker_count` + up to `TEMPO_MARKER_MAX`
  (16) `(time, bpm)` marker pairs, linearly interpolated between whichever
  two bracket the engine's own elapsed time (`compute_current_tempo_bpm()`
  in `backend.c`; time is seconds since the engine process started, holding
  flat before the first marker and after the last, same convention as a
  DAW tempo track). There's no marker editor UI yet, so the frontend always
  sends `tempo_marker_count=0` (the knob is the whole story for now) -- the
  marker arrays are still reserved in the wire layout so a future editor
  (or anything else driving the protocol directly) can populate them
  without another breaking wire-protocol change. The Pitch
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
Saw/Noise/Pulse/Sample) with FM and hard sync -- both PolyBLEP-corrected
against aliasing. Osc 1's BLEP window width tracks the true instantaneous
phase velocity under FM instead of just the carrier's own nominal step. Hard
sync is alias-free on both timing and amplitude: Osc 2's phase (including
each of its 7 Unison Saw detuned voices) resets to the true fractional-sample
instant Osc 1 wrapped rather than snapping to the next sample boundary, and
for Saw/Square/Pulse the instantaneous amplitude discontinuity that reset
creates in Osc 2's own output is itself BLEP-corrected (split across the
sample before and the sample after the sync instant) -- Triangle needs no
such correction (its leaky integrator already smooths through an
intentionally-untouched state across the reset) and Sine/Unison-Saw's own
amplitude correction remain a possible future extension -- a dedicated sub oscillator and
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
ready for more effects to be ported into the dispatch switch. Five are
implemented so far, all ported from Airwindows (MIT license):

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
- **Density3** — a highpass (a leaky-integrator lowpass subtracted from
  the signal) feeding a density/waveshaping saturator: a Taylor-series
  sine-based clipper above its neutral point, a softer arcsine-family
  curve below it, blending smoothly through the middle. Params: Density
  (0 = softest curve, 100 = hardest sine clip), Highpass (0 = off, cubic
  response so most of the knob's useful range is near the low end), and
  Output (makeup gain). Unlike the original plugin, there's no separate
  Dry/Wet param here — the slot's generic Mix knob already does that job,
  so exposing both would just be two controls doing the same thing (see
  the `FX_TYPE_DENSITY3` comment in `backend.c`).
- **StereoChorus** — a stereo-widened flanger/chorus: two independently
  LFO-swept delay reads (L and R start their sweep at different phases,
  so they never modulate in lockstep) into an integer-quantized ring
  buffer, plus the same "air" comb-filter stage (see below) restoring
  some of the top end a short modulated delay's interpolation smears
  away. Params: Speed and Depth.
- **StereoDoubler** — a granular micro-pitch detuner: 8 simultaneous
  delay-read "voices" per channel walking at slightly different (and,
  between L/R, oppositely-signed) speeds, crossfade-spliced between
  whichever pair conflicts least whenever one wraps, avoiding the
  audible clicks a naive single-tap pitch shifter would produce. Params:
  Detune and Dry/Wet (kept as its own control here, unlike Density3's —
  see the `FX_TYPE_STEREODOUBLER` comment in `backend.c` for why).

  StereoChorus and StereoDoubler share an "air" comb-filter stage
  (compensating for the top-end loss a modulated/interpolated delay
  read causes) and a downsampling-ladder reconstruction technique for
  CPU savings at higher sample rates, both also used in `run_fx_ensemble`
  above — all three originate from the same family of Airwindows
  plugins and share this DNA. At this engine's fixed 48kHz, the ladder
  is always a no-op (`cycle_end` computes to 1), but is kept generic in
  case `SAMPLE_RATE` ever changes.

Adding a new effect means: a `FX_TYPE_*` define + its processing function
+ a case in `process_fx_chain()`'s switch (`backend.c`), and a matching
entry in `FX_TYPE_METADATA` (`frontend.py`) giving its real param count,
labels, and ranges — the frontend uses that to relabel/show only the
knobs a given effect actually uses instead of 8 generic "P1..P8" dials.

## Build & run

Requires ALSA dev headers (`libasound2-dev` on Debian/Ubuntu) and Python 3
with PyQt6 and numpy (the Mapping tab's waveform display).

```sh
make                    # builds ./synth_engine
pip install -r requirements.txt

./synth_engine &        # start the audio/MIDI engine
python3 src/frontend.py # start the GUI (connects over /tmp/synth.sock)
```

The backend must be running before (or while) the frontend starts for a
live connection; otherwise the frontend runs in mock mode.
