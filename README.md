# HDS

A digital synthesizer split into two processes:

- **`src/backend.c`** — the DSP/audio engine. Plain C against ALSA: renders
  audio directly to a PCM device, captures external audio from an input
  device, and exposes every synth parameter over OSC (Open Sound Control,
  via `liblo`, plain UDP) -- every parameter addressed by name (e.g.
  `/hds/vco1/shape`), written directly into `SynthSharedControls` (see
  `OSC_PARAMS[]`), rather than a fixed-layout packed struct sent as one
  positional binary blob. Also streams back a small status struct
  (`EngineStatus`: peak level, recording state/elapsed time/error, detected
  input/output channel counts) as an OSC bundle to a configured reply address --
  see "Control protocol: OSC" under Build & run. The final output stage
  carries an always-on
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

  **Recording** (`Recording` tab) writes 32-bit float WAV
  (`WavRecorder` in backend.c -- IEEE float format tag, so no int
  conversion/dithering, and natively readable by any modern DAW/editor).
  A second, lossless WavPack format (`WavPackRecorder`, reusing the exact
  same realtime-safe ring/writer-thread architecture, just branching the
  writer thread's start/write/stop calls) exists in the code but is
  currently disabled (`HDS_WAVPACK_ENABLED` in backend.c) -- the Format
  radio button for it is greyed out in the GUI until this is revisited.
  Two source kinds, freely
  combinable in one take: the Program's own final post-FX output (output
  pair 0 specifically -- see "Multi-source/multi-output audio routing"
  below; External Input pairs are their own independent sources and are
  NOT automatically folded into this unless explicitly routed there), and
  raw per-channel taps directly off whatever the currently-selected Input
  device provides (independent of any `ext_in_enable[p]` -- works as a
  plain multichannel recorder for a class-compliant interface with no
  notes playing at all). The capture stream itself is opened with however
  many channels `query_device_channel_count()` detects for that device
  (not hardcoded stereo), so a multichannel interface's extra inputs are
  actually reachable, not just its first two. Selected channels combine into
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
  `SynthSharedControls` (which every other knob's own OSC message writes
  into): it travels instead over its own two dedicated OSC addresses --
  `/hds/zone/upsert` (add-or-update one zone by a stable `zone_id`, the
  frontend's own ever-incrementing, never-reused counter -- see
  `zone_upsert_handler()`) and `/hds/zone/remove` (delete by `zone_id`,
  see `zone_remove_handler()`) -- so editing one zone stays cheap even with
  thousands mapped: one OSC message per touched zone, not a resend of the
  whole catalog. `g_sample_slots[]`
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
- **`src/frontend.py`** — a PySide6 control surface, ported to speak OSC
  (see "Control protocol: OSC" under Build & run) via `python-osc`: one
  `SimpleUDPClient` sends every knob change plus the Sample Bank catalog
  (`/hds/zone/upsert`/`/hds/zone/remove`) to the backend's
  `--osc-listen-port`, and a plain non-blocking UDP socket receives
  `/hds/status/*` feedback on the backend's `--osc-reply-port`. Every
  address it sends must match `backend.c`'s `OSC_PARAMS[]` table exactly --
  see `scratchpad/check_osc_address_parity.py`'s approach if that ever
  needs re-verifying. UDP has no connect-time liveness signal, so
  "connected vs. mock mode" is now inferred from whether a status bundle
  has arrived recently (a short timeout after the last one) rather than
  from a socket `connect()` succeeding.

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
  bundle, each edit calls `transmit_zone_upsert()`/`transmit_zone_remove()`
  (`SamplerMappingGrid`, adapted from a standalone prototype into this
  file's real backend instead of that prototype's `print()`-based stubs),
  sending only the zone(s) actually touched as their own
  `/hds/zone/upsert`/`/hds/zone/remove` OSC messages. No cap on how many
  zones can exist at once -- each is identified by its own stable
  `zone_id` (an ever-incrementing counter, `SampleZone.zone_id`), not a
  fixed backend slot index. Set an oscillator's Shape (Play tab) to
  "Sample" and play.

  **Import SFZ...** bulk-populates the Sample Bank from an SFZ multisample
  library instead of dragging in one `.wav` at a time -- parses the `.sfz`
  file (including `#include`d sub-files), maps its opcodes onto the same
  zone fields the grid above already edits (key/velocity range, loop
  points, tune/gain/pan, start offset, round-robin group), and sends the
  result through the exact same `transmit_zone_upsert()` path a manual
  edit uses. Always additive, never replaces the existing bank. WAV
  samples only (matching the DFD engine's own format support -- convert
  anything else first). `loop_mode=loop_sustain` and `end=` are both fully
  supported: a loop_sustain zone genuinely exits its loop once the voice
  releases (playing forward into the file's own post-loop material instead
  of looping forever), and `end=` truncates one-shot playback at the exact
  requested point. Samples that couldn't be found (or aren't `.wav`) are
  surfaced in a warning dialog after import rather than silently dropped.
  See `_build_zones_from_sfz()`/`_parse_sfz()` in `frontend.py` for the
  full opcode mapping, and `SampleSlot.loop_release_exit`/
  `playback_end_frame` in `backend.c` for how these are applied.

  Each note's own lane
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
  Sync (a 3-way Off/Parallel/Serial control), Pitch Bend Range, Sustain
  Pedal Polarity, the FM Algorithm section, Glide, and Tempo all live in
  the always-visible left sidebar (above the Output/VU strip) instead,
  since they're relevant no matter which tab is open. The
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

Engine features: **4 fully identical oscillators** (Osc 1-4, each with the
same full shape palette -- Saw/Square/Triangle/Sine/Unison Saw/Noise/Pulse/
Sample/**Off**), each with its own vol/pan/mute, fine tune, pitch offset,
phase offset, and Unison Saw detune/width. **Off** is a genuine compute
bypass (skips that oscillator's pan trig, waveform render, and mix
entirely, not just a silent volume) -- a real CPU concern once every
oscillator can independently be a full-featured voice. Selecting Noise
gives every oscillator the same one-pole Tilt-color filter (a dual-duty
knob that reads as Pulse Width under Shape=Pulse, or Tilt under
Shape=Noise: -1 dark/low-passed, 0 flat white, +1 bright/high-passed).

FM is a **12-algorithm system** (FM Algorithm section, left sidebar): a
fixed, hand-verified-acyclic set of 4-operator routing topologies (not a
general routing matrix, which risks cycles) -- Algorithm 0 ("Classic",
Osc2->Osc1 only) reproduces the engine's original fixed FM pair exactly
and stays the default; the other 11 range from simple chains and
many-to-one stars to fully independent pairs. Only the depth knob(s) an
algorithm actually uses are shown. Hard Sync is a 3-way control: Off,
**Parallel** (Osc 1 resets Osc 2, 3, and 4 independently, each keeping its
own shape/detune), or **Serial** (Osc 1 resets Osc 2, whose reset resets
Osc 3, whose reset resets Osc 4 -- a cascading "telephone game" of phase
truncation, where a stage still fires its own natural wrap if no upstream
reset arrives that sample). Both FM and hard sync are fully
PolyBLEP-corrected against aliasing, including through Off oscillators
(phase bookkeeping never stops, only audible rendering does) and through a
hard-synced oscillator that's simultaneously an FM carrier or modulator.

Also: a 2-filter (A/B) architecture where Filter A can itself be a
3-filter Multi engine, 10 envelope generators (Osc 1-4 amp + 6 assignable,
each switchable between ADSR/AR/AD/ASR/AHD/DADSR/AHDSR, with the Sustain
stage chaseable in real time while a note is held), **16 LFOs** (each with
its own MIDI Modulation Source; LFOs 1-2 run at full audio rate, LFOs
3-16 run at a decimated ~2kHz control rate with interpolation -- 16
always-otherwise-running LFOs would otherwise cost as much CPU as 16 more
oscillators), and a shared mod matrix routing LFOs and assignable EGs to
pitch, amp, pan, pulse width/tilt, filter cutoff/resonance, phase, and
unison detune on all 4 oscillators.

Filter A and B combine via the Filter Routing Matrix (Filters tab): Series
A->B, Series B->A (the same two filters, reversed order -- audibly
identical to A->B for static cutoff/resonance since two LTI filter cascades
commute, but genuinely different once either is modulated in real time), or
Parallel with an adjustable Blend knob (0 = all A, 100 = all B, 50 = even --
replacing the old fixed 50/50 sum). Filter A's own Multi mode additionally
offers Series/Parallel/Split routing across its 3 internal filters.

**Multi-source/multi-output audio routing** (`Routing` tab, plus each FX
slot's own Source combo on the `FX` tab): the routable units are the
Program (the single patch's own post-Filter-A/B mix -- oscillators are
always summed and run through Filter A/B together; that internal path is
never split apart by any of this) and up to 8 independent External Input
pairs, each tapping its own pair of capture channels (pair *p* = capture
channels `2p`/`2p+1`) with its own Enable/Gain. Each of the 12 FX slots
(`FX` tab) is assigned to exactly one of these 9 sources (`fx_source` --
default: Program, reproducing a single-source bus if nothing else is
configured), and Series/Parallel routing is a per-source choice (a
source's own assigned slots run in array order in Series, or are all fed
the same pre-FX input and averaged in Parallel -- the averaging divides by
however many slots that source actually owns, not a fixed 12). A source
with no slots assigned is a dry passthrough. Each source's final signal
(post its own FX chain) can then be routed to one or more hardware output
pairs on the `Routing` tab's many-to-many grid -- a source can feed
several pairs at once, and several sources can share one pair, summed
together. Output is opened with however many channels
`query_output_device_channel_count()` detects (mirroring the input side's
`query_device_channel_count()`), not hardcoded stereo, so a class-
compliant multichannel interface's extra outputs are reachable, not just
its first pair. The always-on safety limiter (see above) is evaluated once
per output pair, after every source routed there has been summed in, not
once per block. `process_fx_chain()` in `backend.c` implements the
per-source slot dispatch; each slot still has the same generic param
bank (raised from 8 to 10 slots for PointyGuitar's own real param
count — see `FX_SLOT_PARAM_COUNT`'s own comment in `backend.c`) + mix +
bypass it always did. Most slot types are still an unimplemented
pass-through, ready for more effects to be ported into the dispatch
switch. Nine are implemented so far, all ported from Airwindows
(MIT license):

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
- **TapeDelay2** — a single-tap tape echo: a variable-speed
  (tape-flutter-modulated) delay line whose feedback path runs through a
  resonant lowpass inside the loop (each repeat darkens progressively,
  the classic tape-echo "regen" filter), plus a second, slightly
  higher-resonance lowpass on the way out of the delay. Params: Time
  (inverted, as in the original — 0 is the longest delay, 100 the
  shortest, since the knob really controls tape speed), Regen (feedback),
  Freq/Reso (the regen/out filters' shared cutoff and resonance), Flutter
  (delay-speed vibrato depth), Dry/Wet (this effect's own internal blend,
  not a crossfade — like CreamCoat's Wetness, 50 is both dry and wet at
  full strength, so it can sit on a submix without pulling back everything
  else), and Ping-Pong — not part of the original plugin, a small,
  additive toggle (default Off) offering a "Stereo Ping-Pong" mode that
  both cross-feeds each channel's delay output into the other channel's
  feedback path AND diverges L/R's own delay time (a fixed +/-15% spread
  around the shared Time setting) -- both together, not the cross-feed
  alone (tried and cut: without the time divergence too, cross-feeding is
  nearly inaudible on anything but a strongly asymmetric source, since L
  and R still land on the exact same beat). Off reproduces the original
  algorithm exactly, bit for bit; see `run_fx_tapedelay2`'s own comment in
  `backend.c` for how that's kept true.
- **PointyGuitar** — a guitar-amp-style distortion: the input runs
  through a cascade of 6-16 identical stages (driven by Drive), each one
  splitting the signal into 9 successively-slower one-pole lowpass
  "bands" and re-summing them weighted by 5 independent band gains
  before a hard clip, so the harmonic content the distortion generates
  is shaped by those gains on the way through, not just tacked on after.
  A zero-crossing-aware noise gate tracks the clean input but is applied
  to the distorted output, closing toward a soft cosine curve instead of
  hard silence. Two speaker-cabinet-style filters finish the chain:
  HSpeakr (a genuine multi-pole lowpass — a cabinet's top-end rolloff)
  and LSpeakr (a partial bass cut — a small speaker's inability to
  reproduce sub content). Params: Drive, Presence/High/Mid/Low/Sub (the
  5 band gains), HSpeakr, LSpeakr, Gate (threshold), and Output (makeup
  gain) — all 10 real, independent controls, none foldable into the
  slot's generic Mix the way an effect's own plain Dry/Wet sometimes is,
  which is why `FX_SLOT_PARAM_COUNT` needed raising to fit this one.
- **GuitarConditioner** — genuinely parameterless (matching the
  original's own zero-knob design): splits the signal into a treble
  path and a bass path, each run through its own one-pole highpass
  whose cutoff is dynamically set by the CURRENT sample's own level
  (not a fixed coefficient), then slew-limits each path's
  sample-to-sample delta before summing them back together. Both
  highpasses alternate between two independent one-pole memories every
  other sample — the original's own technique, ported as-is. Only the
  slot's generic Mix/Bypass apply; there's nothing else to expose.
- **Cabs2** — a simple speaker-cabinet-style tone shaper: a 21-stage
  highpass (built, like GuitarConditioner's own filters, from cascaded
  one-pole lowpass stages subtracted from the signal) followed by a
  13-stage direct one-pole lowpass. Both stages self-bypass at their own
  "wide open" extreme, zeroing their own filter memory the instant they
  do so a later re-engage starts clean rather than resuming from stale
  state. Params: Lowpass (0 = darkest, 100 = fully open/bypassed) and
  Hipass (0 = fully open/bypassed, 100 = most filtered) — the two knobs
  are deliberately inverted relative to each other, ported as-is from
  the original.

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

**3-band EQ** (`Filters` tab, post-filter): each Program gets its own Low
Shelf / Mid Peak / High Shelf EQ (`ProgramState.eq_bands[3]` in
`backend.c`), applied once per Program per sample right after the 5-slot
filter chain and before FX/output routing (`apply_program_eq()` in
`render_audio_block()`) -- RBJ Audio-EQ-Cookbook biquads, ±18dB per band,
with Q exposed only on the Mid Peak band (the two Shelf bands use a fixed,
musical shelf slope). A flat (0dB) band is a true zero-cost bypass, so an
unused EQ costs nothing.

**Sequencer record-quantize** (`Sequencer` tab, Transport panel): a
Quantize toggle + Resolution dropdown (1/4 down to 1/32, including
triplets) snaps a Rec-armed lane's recorded note-ON timestamps to the
nearest grid line at the engine's current tempo (`seq_record_midi_event()`
in `backend.c`) -- note-OFF timestamps, and therefore note length, are
deliberately left exactly as played.

**Combi per-lane Transpose** (Combi view, Play tab): each of the 8 Combi
lanes gets its own ±48-semitone Transpose spinbox
(`combi_lane_transpose[COMBI_PROGRAM_COUNT]` in `backend.c`), composed as
just another additive-cents term alongside pitch bend/fine tune/pitch
offset -- lets the same or different Programs be stacked at different
octaves ("octave layering") without touching either Program's own patch
content. Session-level, like the lane's MIDI channel/Key/Velocity zone --
not saved as part of a `.hdsprog` Program file.

**Combi save/load** (`Save Combi` / `Load Combi` buttons, Combi view): a
`.hdscombi` file (`~/HDS_Combis/<name>/<name>.hdscombi`, plain JSON)
captures all 8 lanes' session-level setup (which Program is picked, MIDI
channel, Key/Velocity zone, Transpose, fader/pan) but deliberately
**references each lane's Program by name** rather than embedding its full
~1150-field parameter set -- the same relationship the Combi lane picker
itself already has to `~/HDS_Programs/`, so a Combi file stays a few
hundred bytes, and re-saving a referenced Program updates every Combi that
uses it with nothing to keep in sync by hand. Entirely frontend-side
(`_pick_and_save_combi()`/`_pick_and_load_combi()` in `frontend.py`),
mirroring Program save/load's own existing JSON convention.

**Song save/load** (`Save Song` / `Load Song` buttons, Sequencer tab): a
song folder (`~/HDS_Songs/<name>/`) is a complete, self-contained snapshot
of the Sequencer's 32 lanes -- each lane's own config (type, level/pan,
MIDI channel, mute/rec/overdub, an Audio lane's own offset) round-trips
through the same `/hds/seq/...` OSC knobs everything else already uses
(`song.json`, plain JSON, written/read entirely by `frontend.py`), and an
Audio lane's referenced WAV is **copied into** the folder
(`audio/lane_<N>.wav`) so the song stays loadable even if the original
file is later moved or deleted. A Program/MIDI lane's actual **recorded
performance** (`SeqMidiLane.events[]` in `backend.c`) is pure runtime
state with no OSC-addressable "current value" the way a knob has one, and
is too large to safely fit in a UDP OSC bundle in the worst case -- so it's
saved/loaded backend-natively instead: the frontend sends just a file path
over OSC (`/hds/song/save_midi` / `/hds/song/load_midi`), and the backend
does the actual file I/O directly (`hds_song_save_midi()`/
`hds_song_load_midi()` in `backend.c`), the same "OSC carries a path
string, the backend does the file work" shape `--sfz-vcoN`/
`/hds/zone/load_sfz` already use for SFZ loading, applied here to a small
custom binary dump instead. This intentionally runs synchronously on the
Save/Load button press rather than through a dedicated background thread
(unlike continuous WAV recording/sample streaming) -- a rare, discrete,
user-initiated action, and even a fully-packed worst case (32 lanes x 8192
events) is only a couple of megabytes.

**Routing tab VU meters**: each of the 8 External Input pairs gets its own
small VU meter on the `Routing` tab, filling the row's spare horizontal
space next to the output-pair checkboxes. Metering is computed
backend-side only while the Routing tab is actually the visible tab
(`gui_routing_tab_visible` in `backend.c`, set by the frontend on every
`QTabWidget.currentChanged` -- see `_on_main_tab_changed()` in
`frontend.py`) -- a true no-op the rest of the time, not just "computed
but ignored."

**External tempo sync** (Sequencer tab's Sync control): Internal (default,
unchanged -- follows the Tempo knob/tempo-map) or External, which slaves
`current_tempo_bpm` -- and therefore the Arp, LFO Tempo Sync, Quantize, and
the metronome, all of which already just read whatever that one value
resolves to -- to an outside source instead. In the standalone app,
External means real incoming MIDI Clock (0xF8) over an ALSA sequencer
connection, with MIDI Start/Continue/Stop (0xFA/0xFB/0xFC) driving the
Sequencer's own transport the same way its Play/Pause/Stop buttons do
(`hds_handle_midi_clock_tick()`/`hds_handle_midi_transport_*()` in
backend.c); in the CLAP plugin, External instead reads the host's own
`process->transport->tempo` directly (no MIDI bytes involved at all --
`clap_plugin_process()` in clap_plugin.c), tempo-only, since a host's
timeline position doesn't necessarily correspond to "the start of HDS's
own recorded take" the way a real MIDI Start message does. Falls back to
Internal automatically if no clock tick has arrived recently (a
disconnected/unplugged external source degrades gracefully instead of
freezing at a stale BPM) -- the Sync control's own lock indicator
("Locked"/"Waiting for clock...") reflects this live.

**Microtuning** (Scala `.scl` import, sidebar's Microtuning box): off by
default (exact standard 12-tone equal temperament everywhere, unchanged).
Loading a `.scl` file (`hds_tuning_load_scl()`/`hds_note_to_hz()` in
backend.c -- backend-native file I/O, the OSC message carries just the
path, matching `--sfz-vcoN`/`/hds/zone/load_sfz`'s own convention) and
checking Enable retunes the primary oscillator pitch -- what frequency a
played MIDI note actually sounds at -- to that scale, mapped chromatically
from an adjustable reference note/frequency (defaults: Middle C at its own
standard 12-TET pitch). Handles non-12-note and non-octave-period scales
correctly (verified against a Bohlen-Pierce-style 13-tone/tritave test
scale, not just plain 12-EDO). WAVE_SAMPLE playback's own root-note
reference frequency is a deliberate, documented exception -- it stays
12-TET regardless, rather than threading tuning state through the sample-
streaming engine's own call chain for a comparatively obscure interaction.

**CLAP: multi-out audio ports.** Alongside the existing single main stereo
mix (port 0, unchanged for any host that doesn't care about multi-out),
the plugin now also declares one more stereo output port per Combi Program
(ports 1-8, `clap_audio_ports_get()` in clap_plugin.c) -- each carrying
that Program's own fully-processed (post-EQ, post-FX) signal, so a host
can route each part to its own track/bus instead of only ever getting the
combined mix. A port the host doesn't connect costs nothing beyond
`render_audio_block()`'s own per-sample capture into a scratch buffer
(`per_program_out`, NULL-able and unused by the standalone app, which
already has its own hardware-output-pair routing for this).

**CLAP: audio input ports (use HDS as a pure FX processor).** The plugin
also declares 8 stereo input ports (`Ext In 1`-`Ext In 8`,
`clap_audio_ports_get()`), one per the standalone app's own "external
input" bus (`EXT_IN_PAIR_COUNT` -- the same one a real hardware line-in
already feeds there). Insert HDS on an audio track (a guitar, a vocal,
anything), route that track's signal into input port 1, turn on
`/hds/ext_in/0/enable` (the Routing tab's own Ext In 1 Enable checkbox in
`frontend.py`), and set an FX slot's Source to `Ext In 1` -- the track's
real audio now reaches that slot's own FX chain (the new NAM slot above,
or any other effect) exactly like a Program's own voices do, no MIDI or
notes required at all. Input port 1 is `IS_MAIN`, so a host that
auto-wires an inserted effect's main input picks it correctly. A port the
host doesn't connect costs nothing beyond the one-time per-block "is this
connected" check (`any_ext_in_connected` in `clap_plugin.c`'s own
`process()`) -- the common case for every existing instrument-only
patch, unaffected either way.

**CLAP: params extension (host automation).** A curated ~55-parameter
subset (`HDS_CLAP_PARAMS` in clap_plugin.c) -- all 5 filters'
Cutoff/Resonance, the post-filter 3-band EQ, each oscillator's Volume/Pan
and Amp EG (Attack/Decay/Sustain/Release), all 12 FX slots' Mix, Master
Volume, and Transpose -- is exposed as real, host-automatable CLAP
parameters, each with a hand-verified min/max/default matching
`frontend.py`'s own widget ranges. Deliberately curated rather than
exhaustive: backend.c's `OSC_PARAMS[]` (the single source of truth for
every one of the ~1150 knobs' address/offset) carries no range metadata at
all, since nothing needed it before this -- exposing all ~1150 would mean
hand-authoring ~1150 rows of range data with no way to keep them in sync
with the GUI, and would hand the host's automation-lane picker an
unusably long flat list (the same reason most commercial synth plugins
expose a curated "macro" page rather than every internal register).
Bi-directional: a value changed via the GUI/OSC while hosted is also
echoed back to the host as an output parameter-change event
(`hds_clap_push_changed_params()`), so the host's own automation lane/UI
stays in sync regardless of which side changed it.

**Standard MIDI File import** (Sequencer tab's "Import MIDI / Song
Markers" box): reads a real `.mid`/`.midi` file -- Type 0 or Type 1, e.g.
exported from Reaper or any other DAW -- via a genuine SMF parser
(`hds_song_import_smf()` in backend.c, not the small internal round-trip
format `hds_song_save_midi`/`load_midi` above use for HDS's own Save/Load
Song). Each note-carrying track becomes one Sequencer lane, up to
`SEQ_LANE_COUNT` (32); every lane is cleared first (a real Load, not a
merge). Handles running status and every standard meta/sysex event
correctly by skipping unrecognized ones via their own length prefix, and
scans every track for the file's Tempo (`0xFF 0x51`) and Marker
(`0xFF 0x06`) meta-events rather than assuming Track 0 only, so a Type 0
file (tempo/markers/notes all interleaved on one track) works the same
way. "No partial application": any structural corruption in the file
aborts before touching a single lane or knob; a capacity overflow (more
tracks/tempo changes/markers than this engine reserves room for) is not
such a failure -- it's reported on stderr and the rest of the file still
imports. Note timing always uses the file's own tempo map to convert
ticks to real seconds -- that's simply what correct playback requires --
independent of the "Import tempo" checkbox, which controls only whether
the file's tempo map is *also* applied to Single Program mode's own
Tempo knob/tempo-map (so the Arp, LFO Tempo Sync, metronome, and Quantize
grid follow it too). The file's own text markers always populate the
Markers list regardless of that checkbox; double-clicking one seeks the
Sequencer playhead straight there (`/hds/seq/transport/seek_to_target_sec`
+ `seek_to`, a new arbitrary-position seek trigger alongside the existing
seek-to-start/back/forward-one-measure ones). Markers are frontend-owned
state from that point on (round-trips through Save/Load Song's own
`song.json`, not a live backend knob), the same way Sequencer lane
config already is.

**Per-lane "Load MIDI..."** (each Program/MIDI lane's own strip on the
Sequencer tab, next to its Mute/Rec/Overdub controls): imports a single
`.mid`/`.midi` file straight into THAT one lane only, replacing whatever
it currently holds (`hds_song_import_smf_track()` in backend.c) -- for
when you just want to drop one part onto one lane rather than replace
the whole song. Every other lane, and the tempo map/Markers list, are
completely untouched; if the file happens to have more than one
note-carrying track, they're all merged together by time into that one
lane rather than silently dropping the extras. Shares its file-parsing
internals with the whole-song importer above (same header validation,
same running-status/meta/sysex handling, same "no partial application"
posture) -- they differ only in what each one does with what it finds.

Fixed alongside this: the Tempo knob/tempo-map's own elapsed-time axis
used to be the engine's running uptime, a dormant bug that never mattered
while the tempo map was always empty (nothing populated it before this).
An imported song's tempo map is inherently relative to *song position*,
not how long the process has been running, so it now interpolates
against the Sequencer's own playhead instead -- matching what the
arpeggiator was already independently doing for the same reason.

## Build & run

Requires ALSA dev headers (`libasound2-dev` on Debian/Ubuntu) and liblo dev
headers (`liblo-dev`, the OSC control protocol -- see below). WavPack dev
headers are NOT required -- WavPack recording support is currently disabled
(see `HDS_WAVPACK_ENABLED` in `src/hds_standalone.c`). PipeWire dev headers
(`libpipewire-0.3-dev`) are also NOT required -- the optional native PipeWire
backend described below is auto-detected at build time and the ALSA backend
is unaffected either way.

```sh
make                    # builds ./synth_engine
pip install -r requirements.txt

./synth_engine &        # start the audio/MIDI engine
python3 src/frontend.py # start the GUI -- talks to the engine over OSC (see below)
```

On NixOS/Nix, `nix-shell` at the repo root provides every dependency above (plus the Raspberry Pi
backend cross-toolchain) in one shell, and auto-provisions a `.venv/` for `requirements.txt` on
entry -- see `shell.nix`'s own header comment for exactly what each target needs from it.
**`nix-shell` is the only supported way to build every target in this repo, including plain
`make`/`make clap`** -- there is no "build outside nix-shell" path, and none should ever be
suggested again.

Linux is the only supported platform -- there is no Windows build of any kind (standalone or
CLAP), and none is planned: this project will not maintain a second *platform* audio backend
(WASAPI) alongside ALSA. ALSA remains the default and always-available backend either way; the
PipeWire backend described below is an additional, opt-in way to talk to audio on the *same*
platform, not a second platform.

### Source layout: backend.c is the shared core, not a standalone entry point

Three files, one architecture: `src/backend.c` is the shared DSP/OSC/state engine (oscillators,
filters, the FX chain, the OSC control protocol, the sequencer/arpeggiator, sample streaming --
everything that doesn't touch a physical device directly), and it is never compiled on its own.
Both real targets pull it in with a plain `#include` (one translation unit per binary, this
project's existing convention -- see `clap_plugin.c`'s own header comment): `src/hds_standalone.c`
adds the ALSA/PipeWire audio shim, the ALSA MIDI sequencer client, recording, the `--program` JSON
loader, the interactive arrow-key CLI, and `main()` itself, and is what `$(BIN)`/`$(ARM64_BIN)`
actually compile; `src/clap_plugin.c` adds only what a CLAP host needs, since the host owns the
actual audio device and hands MIDI in through its own event API.

This split (`hds_standalone.c` used to just be the tail end of `backend.c`) exists so a host-driven
target never needs real device code compiled in at all -- confirmed directly, not assumed: `hds.clap`
now links with zero ALSA or PipeWire symbols (`nm -D`/`ldd` show nothing), whereas before this split
`backend.c`'s unconditional `#include <alsa/asoundlib.h>` meant the CLAP build always carried ALSA
along as unreferenced-but-compiled-in dead code, and could never be retargeted at a non-ALSA platform
without either writing a second full platform backend or untangling this exact coupling first. It's
a pure code-motion refactor -- `hds_standalone.c` still `#include`s `backend.c`, so both binaries
compile as one translation unit each exactly as before, with no new cross-file call, no inlining
lost, and no behavior change (verified: re-ran the FDNVerb RT60 and Rev Saw isolated checks
post-split, both matched their pre-split results exactly).

**A real, pre-existing gap this surfaced, left alone on purpose**: `render_audio_block()`'s own
Sequencer-audio-lane playback block reads the standalone app's `g_streams_state` global directly (via
a `g_seq_audio` macro) rather than through a parameter, even though every other piece of state it
touches is passed in. A CLAP plugin instance's Sequencer "Load Audio" loads into that instance's own
per-plugin `p->streams_state`, but this one playback path reads the standalone's global instead --
which the CLAP `.so` never populates. In other words, Sequencer audio-lane playback likely does not
work in the CLAP plugin today, independent of this refactor entirely (confirmed via `grep`: nothing
in `clap_plugin.c` ever touches the bare `g_streams_state` global). Fixing it means parametrizing
`render_audio_block()` to take a streams pointer like it does everything else -- a real, scoped, not
yet attempted follow-up, not something to bundle into a file-organization change.

### Command-line flags

Every flag `./synth_engine` and `python3 src/frontend.py` accept, in one place -- each one is also
covered in more depth in its own section elsewhere in this file (linked below) if you need the full
story, but this is the quick reference for "what can I even pass."

**`./synth_engine`:**

| Flag | Default | Meaning |
| --- | --- | --- |
| `--osc-listen-port <port>` | `9000` | Where incoming control messages (every knob, plus the Sample Bank's `/hds/zone/*`) are received. See "Control protocol: OSC". |
| `--osc-reply-host <host>` | `127.0.0.1` | Where `/hds/status/*` feedback (VU meter, recording state, `/hds/request_state` replies, ...) is sent. Point this at a remote frontend machine's own IP for a cross-machine setup. |
| `--osc-reply-port <port>` | `9001` | The port half of the above. |
| `--output-device <name>` | `default` | ALSA device string (`hw:1,0`, `plughw:1,0`, `default`, ...) under `--backend alsa`; a PipeWire target node name (`wpctl status`-style), or `default`/empty for PipeWire's own session default, under `--backend pipewire`. See "If the engine won't start because of the audio device". |
| `--input-device <name>` | `default` | Same naming rules as `--output-device`, for the External Input capture device. A capture device failing to open is never fatal (silent until a working name is picked live) -- only `--output-device` failing is. |
| `--backend alsa\|pipewire` | `alsa` | Chooses the audio platform shim. `pipewire` only exists in a build where `libpipewire-0.3` was found at build time (`make` prints which way it went) -- passing it to a build without that support fails fast with a clear message. See "PipeWire backend". |
| `--pipewire-force-quantum` | off | `--backend pipewire` only. Forces the whole PipeWire graph's buffer size down to match this engine's own Latency control, for real (not just a hint) -- but that also forces every OTHER PipeWire client on the same system down to that same small buffer while this engine is running, which can make an unrelated app (a browser tab, a video call) underrun/glitch. Opt-in, off by default, and trivially reversible (just don't pass it) precisely because of that system-wide side effect. See "PipeWire backend". |
| `--sfz-vco1`/`--sfz-vco2`/`--sfz-vco3`/`--sfz-vco4 <path>` | none | Loads a `.sfz` instrument onto that oscillator at startup, before the audio device even opens (so a subsequent live `/hds/zone/load_sfz` can still layer on top). |
| `--program <path.hdsprog>` | none | Loads a full Program (every field a saved `.hdsprog`'s own `fields` object carries, plus any `sfz_file` map) directly off disk at startup -- no frontend needs to ever connect. Reuses the exact same address-dispatch table live OSC control already goes through, so a Program field can never mean something different loaded this way than it does over the network. A bad/missing/malformed file logs and continues starting, same posture as a bad `--sfz-vcoN` path. |
| `--master-volume <0.0-1.0>` | none (leaves `init_default_controls()`'s own default) | Sets master volume once at startup. Independent of `--program` -- a Program's own saved fields never include master volume at all (`_is_program_scope_address()` in `frontend.py` excludes it as a mixing/session-level control, not patch content), so there's no interaction or ordering to know about between the two flags. |

**Interactive arrow-key controls (no flag needed)**: whenever `./synth_engine`'s stdin is a real
terminal (checked via `isatty()` -- a piped, redirected, or systemd/headless launch is completely
unaffected, and stdin is left in its normal line-buffered/echoing mode in that case), the terminal is
put into raw mode for the life of the process and the arrow keys become live controls, for the same
"somewhat adjustable, not totally launch-bound" reason `--program`/`--master-volume` exist:
**Up**/**Down** nudge master volume by 0.02 (clamped 0..1); **Left**/**Right** load the
previous/next saved Program from `~/HDS_Programs/`, alphabetically, wrapping around -- the exact
same list and order `src/frontend.py`'s own "◀"/"▶" Program-cycle buttons use
(`_cycle_program()`/`_scan_programs_folder()`), just triggered from the backend's own terminal
instead. Reuses the same `hds_load_program_file()` `--program` already uses, so it's the same
small-text-file-only, never-blocks-the-audio-thread work an already-live `/hds/zone/load_sfz` OSC
call does today -- not a new category of runtime risk. The terminal's original settings are always
restored on exit (normal shutdown or Ctrl+C).

**`python3 src/frontend.py`:**

| Flag | Default | Meaning |
| --- | --- | --- |
| `--osc-host <host>` | `127.0.0.1` | Hostname/IP the backend is running on. Set this for a cross-machine setup -- but the backend's own `--osc-reply-host` must then point back at THIS machine, or status feedback (VU meter, tempo, recording state) won't find its way back (knob control alone would still work one-way). |
| `--osc-listen-port <port>` | `9000` | The backend's OSC listen port -- where this frontend sends knob/zone messages. `9000` for the standalone app, `9010`/`9012`/... for a CLAP instance (see "More than one instance in the same host" for the exact pattern, and check the host's own log for which pair a given instance actually bound). |
| `--osc-reply-port <port>` | `9001` | The backend's OSC reply port -- where `/hds/status/*` feedback (including `/hds/request_state` replies) is received. `9001` standalone, `9011`/`9013`/... for a CLAP instance, one more than its own listen port. |

Any other argument (`-style`, `-platform`, ...) is passed straight through to Qt untouched --
`frontend.py` only intercepts the three flags above, via `parse_known_args`.

### If the engine won't start because of the audio device

An output device that fails to open used to be an unconditional, unrecoverable startup failure --
exactly the situation a machine whose "default" device doesn't work at all (a broken ALSA config,
a PipeWire ALSA-compat plugin that fails to load, a USB interface that isn't there yet) would hit
every single time, with no way to fix it short of editing a config file or passing
`--output-device`/`--backend` blind and hoping. If launched from a real terminal (not a systemd
unit, a script, or anything else with its stdin closed/redirected -- those can never answer a
prompt, so they keep the old immediate-failure behavior instead of hanging), a failed output device
now drops into an interactive picker instead of exiting: it lists the real ALSA devices found on
the machine (the same information `aplay -L` shows) and lets you pick one, retrying until either
one opens or you type `q` to give up. Under `--backend pipewire` (see below) it instead asks for a
PipeWire target node name (or a blank line to retry PipeWire's own default sink), and can fall back
to the ALSA backend entirely for that session by typing `alsa`.

### PipeWire backend

`--backend pipewire` (default: `alsa`, entirely unchanged) opens a native PipeWire client stream
(via `libpipewire`) instead of an ALSA PCM device -- talking to the PipeWire daemon directly over
its own client protocol, the same way `pw-play`/`pw-cat`/`pw-record` do. This is genuinely
different from just pointing `--output-device` at a `pipewire`-named ALSA PCM: that route still
goes through ALSA's own `libasound_module_pcm_pipewire.so` compatibility plugin, which has its own
separate load-time/ABI requirements and can fail even while the PipeWire daemon itself is running
fine (a real, observed failure: `... cannot be opened or _snd_pcm_pipewire_open was not defined
inside`, immediately followed by `default` itself failing too since it was configured to route
through that same broken plugin). The native backend has no dependency on that plugin at all.

Being a real PipeWire graph client is also what lets other applications keep making sound at the
same time -- PipeWire's own session manager mixes every client stream in software, the same way a
browser tab and a media player already share one output today, with no exclusive hardware lock
involved (unlike opening a raw ALSA `hw:` device directly).

This is an *additional* backend, not a replacement: the ALSA path above is completely unchanged
and remains the default, and the two are never mixed within one process (`--backend` is a
startup-only choice, not a live Audio-tab dropdown item -- switching backends means restarting the
engine, or using the interactive picker's own PipeWire→ALSA fallback described above).

**Build-time only, auto-detected, no manual flag to flip:** the Makefile probes for
`libpipewire-0.3` via `pkg-config` and only compiles the PipeWire code path in when it's found
(`HDS_PIPEWIRE_ENABLED` in `backend.c`) -- a plain `libasound2-dev`-only setup (e.g. the Pi) still
gets a normal ALSA-only build with nothing to change by hand, while `nix-shell` (which lists
`pkgs.pipewire`/`pkgs.pkg-config` in `shell.nix`'s `buildInputs`) gets it automatically. `make`
prints which way it went (`PipeWire backend (--backend pipewire): enabled`/`disabled`). Passing
`--backend pipewire` to a build where it wasn't detected fails fast with a clear message rather
than silently falling back to ALSA.

**Device naming differs by backend:** an ALSA device string (`hw:1,0`, `plughw:1,0`, `default`,
...) under the default `--backend alsa`, vs. a PipeWire target node name (as `wpctl status`/`pw-cli
ls Node` print it) -- or `default`/empty for PipeWire's own session-manager default sink/source --
under `--backend pipewire`.

**Known scope limits, by design rather than oversight:** the PipeWire path always negotiates a
fixed stereo stream (PipeWire's own graph remixes to whatever the real target/hardware actually
has, so this doesn't lose audio, but it does mean the ALSA backend's own reach-every-raw-channel
multichannel routing doesn't apply under PipeWire); and there's no cheap local way to enumerate the
PipeWire graph's nodes the way `snd_device_name_hint` enumerates ALSA's (that's an async registry
round-trip, not a synchronous local query), so the interactive recovery picker asks for a PipeWire
target name directly under this backend rather than listing one -- see the previous section.

**Higher latency than ALSA, and why:** this backend sets `PW_KEY_NODE_LATENCY` from the Audio tab's
own Latency control, mapped onto PipeWire's node latency property the same way the ALSA backend
maps it onto the ALSA buffer target. That's only a *hint*, though -- PipeWire negotiates one shared
buffer size (the graph's "quantum") across every client currently connected to the daemon, so a low
request from this one node can still be silently overridden upward if anything else sharing the
same daemon needs a bigger buffer. That's the structural reason this backend can measure higher
latency than the ALSA one, which gets an exclusive, tightly-sized hardware buffer directly rather
than negotiating a shared one.

`--pipewire-force-quantum` (opt-in, off by default) is the real lever, not just a hint:
`PW_KEY_NODE_FORCE_QUANTUM` actively forces the whole graph's quantum to this engine's own Latency
setting for as long as this stream stays connected. Real tradeoff, not a free lunch: every OTHER
PipeWire client on the same system (a browser tab, a video call, a media player) gets forced down
to that same small buffer too while HDS is running, and can underrun/glitch if it can't keep up
with it -- exactly why this is a separate opt-in flag rather than baked into `--backend pipewire`
unconditionally: turning it off again needs nothing more than not passing the flag on the next
launch, no code change or rebuild.

### Building for Raspberry Pi

**Raspberry Pi 5** (or any other aarch64 Linux machine) needs no cross-compile target if you're
building directly on the device -- it's a native Linux host like any other for this exact
Makefile: `sudo apt-get install libasound2-dev liblo-dev` then plain `make`.

If the Pi itself runs plain Raspberry Pi OS rather than NixOS (the common case, and the reason
this option exists at all), `nix-shell` won't be available there to build directly on the device.
For that case, `make arm64` cross-compiles `synth_engine` (the backend engine only -- no GUI
client for this target) for aarch64 Linux from this x86_64 machine instead -- run from inside
`nix-shell` at the repo root (see `shell.nix`'s own aarch64 section), then copy the resulting
`synth_engine-arm64` binary **and its `arm64-libs/` directory** over to the Pi (keep them in the
same relative layout -- see below for why). Every dependency this needs -- the cross compiler,
alsa-lib, liblo -- comes straight from `nixpkgs.pkgsCross.aarch64-multiplatform` as a real,
already-existing package for this target.

**Deploying the output binary -- three real, confirmed problems, not hypothetical ones.** Cross-
compiling successfully is not the same as the binary actually running once copied to the Pi;
building `synth_engine-arm64` and copying it over hit exactly this in practice. Three separate
issues, each confirmed by directly inspecting a real built binary's own ELF headers and library
files rather than assumed, and each now fixed by the Makefile's `arm64` rule:

1. **The Nix cross linker bakes this build machine's own `/nix/store/...` paths into the output
   ELF** -- both as an RPATH (where to look for shared libraries) and, more consequentially, as
   the ELF interpreter itself (`PT_INTERP`, the dynamic loader the kernel `execve()`s to start the
   program at all). Neither path exists on the Pi. A missing RPATH entry just means the loader
   falls through to the next search path, but a missing *interpreter* means the kernel can't start
   the binary at all -- and most shells report that as "No such file or directory," which is very
   easily (and reasonably) mistaken for the binary itself being missing, even though `ls`/`file`
   show it's right there. Fixed: both rules run `patchelf --set-interpreter /lib/ld-linux-aarch64.so.1`
   (the standard path real aarch64 Debian/Raspberry Pi OS systems already have their own loader
   installed at) as a last build step.
2. **liblo isn't installed on a stock Pi.** It's bundled directly: the `arm64` rule copies
   `liblo.so.7` into an `arm64-libs/` directory next to the binary and sets the RPATH to
   `$ORIGIN/arm64-libs` (relative to wherever the binary itself ends up) -- safe to do because it's
   a pure protocol library with no external config files or hardware coupling. This is why the
   binary and `arm64-libs/` need to travel together in the same relative layout.
3. **alsa-lib is deliberately NOT bundled** -- confirmed to be a real, different kind of risk, not
   just an unbundled convenience. `strings` on the cross-built `libasound.so.2` shows its
   compiled-in default config directory is this build machine's own Nix store path (where
   `alsa.conf`, card definitions, and dmix setups live) -- that directory won't exist on the Pi,
   and even if it did, it would describe the wrong sound hardware entirely; only the Pi's own
   apt-installed alsa-lib is built against its own real `/usr/share/alsa` and actually knows how
   to talk to the Pi's real sound chip. On the Pi: `sudo apt-get install libasound2`.

One more real caveat, not yet hit but worth stating plainly: this cross toolchain's glibc (2.39,
from nixpkgs nixos-24.05) needs to be no newer than the Pi's own installed glibc, or the binary
will refuse to start with a `GLIBC_2.39' not found` error -- check the Pi's glibc version
(`ldd --version`) against this if a binary that has the right interpreter and libraries still
won't run.

4. **`shell.nix` used to resolve `<nixpkgs>` unpinned** -- whatever nixpkgs revision each
   machine's own `nix-channel` state happened to have registered, which is exactly what it says:
   not reproducible across machines or across time on the same machine. Hit for real: a
   `synth_engine-arm64` built on one machine needed `liblo.so.7` (matching its own freshly-copied
   `arm64-libs/liblo.so.7`, so that one machine's own build was internally consistent), while a
   separately-built copy on another machine needed a completely different SONAME,
   `liblo.so.3` -- an old liblo ABI from a nixpkgs revision several years older, resolved from
   whatever channel state that second machine happened to have. `shell.nix` now pins nixpkgs to a
   fixed, hash-verified `nixos-24.05` release tarball via `builtins.fetchTarball`, so every
   `nix-shell` entry -- on this machine, a developer's machine, or the same machine a year from now
   -- resolves the exact same dependency versions (and SONAMEs) every time. This closes the
   cross-machine/cross-time drift risk, not the "I copied a stale `arm64-libs/` next to a newer
   binary" risk -- always build and copy a binary and its own `arm64-libs/` together, from the same
   `make arm64` run.

**`make arm64`** (the backend alone) was verified fully end to end in this repo's own sandbox,
including the full deployment fix above and the nixpkgs-pin fix: a real `nix-shell` (against the
now-pinned `shell.nix`) with the cross toolchain + alsa-lib/liblo (every one of which resolves
straight from cache.nixos.org in seconds, no local build) produced a genuine `synth_engine-arm64`
-- confirmed via `file` to be an ELF 64-bit ARM aarch64 executable correctly linked against the
cross-built libraries, then confirmed via `readelf -d` and `strings` that after the
`patchelf`/bundling step it carries a `$ORIGIN`-relative RPATH, a standard system interpreter
path, and zero remaining `/nix/store` references anywhere in the binary. Verified one step further
than ELF-header inspection this time: actually **executed** under `qemu-aarch64-static` (with only
a real aarch64 glibc + loader supplied as the emulated "system," and the binary's own
`arm64-libs/` sitting next to it exactly as a real deployment would have it, nothing else) -- the
process starts, `liblo.so.7` resolves correctly from the bundle, the OSC listener comes up, and
the only failure is the audio device open (`libasound.so.2` / a real sound card), which is the
expected, documented, by-design gap this environment can't supply either (no real ALSA hardware in
a sandbox, same as the Pi needs its own `apt-get install libasound2`).

### Neural Amp Modeler (NAM) FX slot

An FX slot (Type: `NAM`) that runs a trained [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
amp model (a WaveNet or LSTM network trained to mimic a real amp/pedal's own nonlinear response --
genuinely different from, and generally far more accurate than, the filter-based amp/cab shaping
`GuitarConditioner`/`Cabs2` already provide) plus an optional cabinet impulse-response convolution
stage after it. Three knobs: **Input Gain** and **Output Gain** (+/-20dB trim before/after the
model -- most trained models expect a specific input level, so this is how you match a hotter or
quieter source, and add makeup gain afterward) and **Cab Enable** (A/B a loaded cab IR against the
bare model without unloading it). Two buttons on the slot itself, **Mdl** and **Cab**, each open a
file picker -- `.nam` for the model, a mono or stereo `.wav` for the cabinet impulse response --
and the loaded file's path is saved with everything else (a Program file, a DAW's own saved CLAP
state) exactly like a Sample Bank zone's own file path already is, so a saved patch reloads its
model/cab automatically. Runs on a real guitar/vocal/etc track's own audio too, not just a
Program's synthesized voices -- see "CLAP: audio input ports" below for routing a track's signal
into an FX slot's Source.

**Loading a model/IR onto a remote instance** (via the Instance Switcher above): those same **Mdl**/
**Cab** buttons send the actual file bytes, not a path, whenever the current target isn't this same
machine -- a path on your machine means nothing on a different one. Unlike Sample Bank zones
(SFZ/WAV), which stay purely path-based always (a sample *library* can be gigabytes, genuinely
impractical to ship over a control protocol), a `.nam` model or cab IR is typically well under a
few MB, small enough that "just send the bytes" is actually useful. Chunked over OSC (a
`/hds/fx/upload_begin`/`upload_chunk`/`upload_end` sequence, `fx_upload_begin_handler`'s own comment
in `backend.c`), verified with a whole-transfer CRC32 before anything touches disk or a real FX
slot's path -- a truncated or corrupted upload must never reach the model loader as if it were the
real file, unlike the periodic status stream where a dropped, stale value is fine since a fresher
one is always coming. A failed verification retries the whole transfer (up to 3 times) rather than
a per-chunk ACK scheme, a perfectly adequate answer to "OSC/UDP isn't reliable" given how small and
quick these transfers are. One real limitation, not papered over: the backend writes an upload to
its own temp file and points the live FX slot at that immediately, but that path isn't portable --
a Program saved while pointed at a remote target won't meaningfully restore an uploaded model/IR
reference on a future session the way a local path already does. Getting the file onto the remote
instance's disk right now is what this solves; a portable reference is a separate problem.

**Why this needed any C++ at all**: NeuralAmpModelerCore is a C++/Eigen library with no C API of
its own, and this is otherwise an all-C codebase. `src/nam_bridge.h`/`.cpp` is the entire cost of
that: a handful of `extern "C"` functions (`hds_nam_create`/`process`/`destroy`) wrapping the real
C++ API, compiled as its own translation unit and linked into the otherwise-C `synth_engine`/
`hds.clap` binaries via `g++` as the final link driver -- `backend.c` itself never sees a line of
C++. Both NeuralAmpModelerCore and Eigen (its own linear-algebra dependency, needed only for
`<Eigen/Dense>` and the FFT module one of NAM's layers uses) are vendored whole under `third_party/`
(pinned to Eigen 3.4.0, the long-standing stable release, not the library's own bleeding-edge
default branch) -- not a system package like PipeWire above, since there's nothing to
version-mismatch against by vendoring a fixed copy, and every normal build gets this for free with
nothing to `apt-get install`.

**Two independent model instances per slot** (one per channel), not one shared between L and R --
each carries real internal state (WaveNet dilated-convolution history, LSTM hidden state) that must
never leak between channels, the same principle every other stereo effect in `backend.c` already
follows for its own per-channel filter memory. The cabinet stage is plain C, not more C++:
NeuralAmpModelerCore's own `wav.h` only exposes a raw WAV-decode helper, not a ready convolution
engine, so the cab stage reuses this file's own existing WAV loader (`seq_audio_load_wav_from_disk`,
already used by Sequencer audio lanes) into a hand-rolled direct-form FIR (capped at 4096 taps,
~85ms at 48kHz -- comfortably covers real guitar cab IRs, which decay within tens of milliseconds)
rather than pulling in a second file-format dependency for the one thing actually needed.

**Now wired into the `make arm64` cross-build too**: there never was a real "no C++ cross-compiler"
problem, only a missing Makefile recipe -- `shell.nix`'s `aarch64.stdenv.cc` already installs a full
`aarch64-unknown-linux-gnu-g++` alongside its own `-gcc`, it just went unused. `make arm64` now
cross-compiles NAM's own objects into a separate `build/arm64-nam/` directory (so a native `make`
and a `make arm64` in the same tree never fight over the same `.o` files) and links
`synth_engine-arm64` with the cross `g++` for the same reason the native build already links with
`$(CXX)` instead of `$(CC)` -- pulling in libstdc++ for the NAM objects linked alongside an
otherwise-C binary. Eigen needed nothing extra either way (header-only, architecture-independent).
`shell.nix` now also bundles the cross-built `libstdc++.so.6`/`libgcc_s.so.1` alongside the binary,
same reasoning as the existing liblo bundling (don't gamble on whatever version happens to already
be on a given Pi's own OS image).

### Gate / EQ3 / Limiter FX slots

Three more FX slot types, all plain C (no new dependencies), each a straightforward standalone
effect rather than a ported third-party plugin:

- **Gate** -- a simple, zero-latency noise gate. **Threshold** (-60..0dB) sets the level below which
  the gate closes; **Attack** (0.1..20ms) and **Release** (5..500ms) are independent exponential
  ramp times for opening/closing; **Range** (0..-80dB) is how much the signal is attenuated while
  closed (Range=0 makes the gate a no-op regardless of Threshold). "Zero-latency" means exactly
  that: no lookahead buffer anywhere, so gain reduction is computed and applied to the very same
  sample that crossed the threshold -- the same posture the master bus's own always-on safety
  limiter takes (see below), and the tradeoff is the same one every zero-latency gate has: a very
  hard, very sudden transient can leak a few ms of attack time before the gate finishes opening.
- **EQ3** -- the Program's own post-filter 3-band EQ (Low Shelf/Mid Peak/High Shelf), ported
  verbatim onto an FX slot: same RBJ Audio-EQ-Cookbook biquad math (`compute_eq_band_coeffs`/
  `run_eq_biquad` in `backend.c`), same +/-18dB gain range per band, same fixed 0.707 Q on the
  shelves with an adjustable Mid Q (0.1..10). The one real difference from the Program EQ's own
  dedicated dial is Freq: that dial uses a raw 20Hz-20kHz range directly, but every other FX slot
  param rides a generic 0-100 dial that reaches the backend as a plain 0.0-1.0 fraction, so this
  slot's Freq params use a log-frequency mapping (`20 * 1000^fraction`) to stay consistent with
  every other effect's own dial convention instead of being a special case.
- **Limiter** -- a digital brick-wall peak limiter: **Ceiling** (-20..0dB, the hard output cap),
  **Input Gain** (0..+24dB, drive into the limiter), **Release** (10..500ms). Instant-attack (gain
  reduction is computed and applied to the same sample, so nothing can ever exceed Ceiling even on
  the very first sample of a transient) with an exponential smooth release back toward unity gain
  -- the exact same technique `render_audio_block`'s own always-on master bus safety limiter already
  uses, just exposed as a slot with its own adjustable Ceiling/Input Gain/Release instead of that
  fixed, hardcoded safety net. Deliberately **not** a port of Waves L1: L1 is closed-source with no
  public algorithm reference to port from, so this is an original implementation of the same
  well-known instant-attack/smooth-release brick-wall technique real limiters (L1 included) are
  built on, not a clone of L1's own internals.

### FetComp FX slot (1176-style FET limiter)

An FX slot (Type: `FetComp`) ported from [fetcomp-dsp](https://github.com/Paulllux/fetcomp-dsp)
(Paul Ulrix, MIT license) -- a circuit-derived model of the UREI 1176LN FET limiting amplifier's
own gain cell, detector/timing network, Class A line amp
and output transformer. Unlike Gate/EQ3/Limiter above, this isn't a simple filter or envelope
follower -- it's a real analog-circuit model: a voltage-divider gain cell (a JFET solved via a
closed-form quadratic every sample, derived directly from the transistor's own device equation),
fed back through a diode detector and a two-node timing network tapped *after* the cell (closing
the loop the way the real hardware does, which is the structural reason a 1176 "grabs" rather than
"clamps"), driving a Class A tanh output stage and a Jiles-Atherton hysteresis model of the output
transformer. Eight params: **Input** (-20..+40dB) and **Output** (-20..+20dB) -- the real hardware
has no separate Threshold knob; you drive *into* a fixed one with Input, which is why input level,
gain reduction and distortion are genuinely inseparable on this unit, kept faithfully here rather
than smoothed away into a conventional threshold/ratio compressor; **Ratio** (4:1/8:1/12:1/20:1/
All -- All shorts every ratio position's resistor tap in parallel, the real "all buttons in" trick,
steepest slope and lowest threshold of the five); **Attack** (20-800us) and **Release**
(50-1100ms); **Limiting** (Limit / Character Only -- the real unit's Attack knob has an OFF detent
past its slowest setting where the cell stays open and no gain reduction happens, but the signal
still runs the whole Class A/transformer path, a genuine "color box" use of the circuit);
**Character** (0-0.5, the LN linearisation network's feedback fraction -- 0.5 exactly cancels the
gain cell's own 2nd harmonic, 0.0 is the earlier, dirtier Rev A response); **Iron** (0-1, how hard
the Class A stage and output transformer are driven -- 0 takes both out of the path entirely,
leaving just the bare gain cell). No separate Mix param -- the slot's own generic Mix knob already
does that job.

**What's ported faithfully vs. what changed going from the original's JUCE-based C++ to this file's
plain C**: the gain-cell quadratic solve, the diode detector law, the two-node attack/release
timing network (including its log/pow calibration curves -- these fit the timing network's own
measured behavior, not a physical potentiometer's rotation feel, so they apply just as much to this
slot's plain 0-100 dial as to the original hardware's own knob), the ratio bank's per-position
drive/bias constants, the Class A stage, and the full Jiles-Atherton transformer hysteresis model
are all kept as-is. What changed: (1) `juce::jmin`/`jmax`/`jlimit`/`Decibels` are gone, replaced
with plain `fminf`/`fmaxf`/`powf` -- this port exists specifically so this effect needs no
framework at all; (2) the original's own "fast" tanh/exp2 polynomial approximations are replaced
with real libm `tanhf`/`expf` -- strictly more accurate, and it removes an overflow-to-NaN failure
mode the original's own comments describe guarding against, since a real `tanhf` simply can't
overflow; (3) the original's runtime-switchable JUCE polyphase halfband oversampler (1x/2x/4x) is
replaced with a fixed 2x oversampling stage built from this file's own existing RBJ-cookbook biquad
runner (one Butterworth lowpass on the way up, a matched one on the way down) -- less stopband
rejection than a true polyphase halfband filter, but self-contained in a few lines and more than
enough to tame the aliasing a 20us attack's control-signal energy would otherwise fold back into
the audible band; (4) the original's bench-only A/B toggles (iron model, release-network model,
diode-knee shape) are fixed at the author's own shipping defaults rather than exposed as user
controls, since those exist upstream for the *author's* own shipped-vs-experimental comparison, not
as a listener-facing knob; (5) the "selected FET part" latitude (Idss/pinch-off voltage) stays
fixed at a typical small-signal JFET's values rather than exposed as a param, since that's a
real per-unit trimming detail, not something a software knob should model. See `run_fx_fetcomp()`'s
own (long) comment in `backend.c` for the complete list with the reasoning behind each change.

Like every other FX slot, this runs on a real guitar/vocal/etc track's own audio too, not just a
Program's synthesized voices -- see "CLAP: audio input ports" above for routing a track's signal
into an FX slot's Source. Driving Input hard with Limiting set to Character Only can produce a very
loud, heavily colored signal by design (that's the real hardware's own documented behavior) -- the
always-on master bus safety limiter downstream catches any resulting overs before final output,
the same as it would for a hot synth voice.

### FairKid67 FX slot (vari-mu tube leveling amplifier)

An FX slot (Type: `FairKid67`) ported from [PHU-FAIR-KID-67](https://github.com/huberp/phu-fair-kid-67)
(Paul Huber, MIT license; DSP core from its sibling [phu-audio-lib](https://github.com/huberp/phu-audio-lib),
also MIT) -- a circuit-derived model of a classic vari-mu tube leveling amplifier. Structurally
different from FetComp's own single closed-form gain cell: every gain stage here (the preamp, a
3-stage sidechain tube cascade, and both arms of a push-pull output stage -- six stages total, per
channel) is its own small nonlinear circuit, solved every sample via warm-started Newton-Raphson on
a Koren-model triode (Norman L. Koren, "Improved Vacuum-Tube Models for SPICE", 1996) -- the same
plate/cathode KCL equations and closed-form 2x2 Jacobian solve the original library itself uses, not
a simplified stand-in. Eight params: **Input** (-20..+40dB) and **Output** (-20..+20dB) -- no
separate Threshold knob; **Threshold** (0..5V, this port's own calibration against what its detector
chain can actually produce -- see `run_fx_fairkid67()`'s own comment) is subtracted from the raw
detector voltage before any gain reduction can happen; **Time Constant** (choice: 6 positions
matching the original's own timing-preset table, traced to the real schematic's own component
values -- P5/P6 use a program-dependent dual-branch release for quick recovery after a brief
transient and a long tail after sustained heavy drive); **Link Mode** (Independent/Linked-Max/
Linked-Avg/Mid-Side -- how the L/R detector voltages combine before driving both channels); **Bias**
(0..2V, an always-on additive offset to the detector voltage, for permanent light gain reduction if
wanted); **Iron** (0.5-3.0x, scales all three transformers' own saturation drive together); **SC
Gain** (0..2.0x, scales the sidechain tube cascade's own drive into the detector, independent of
Input). No Ratio param -- like the real hardware, compression amount emerges entirely from
Input/Threshold/tube nonlinearity, not a dialled-in slope.

**Sidechain source, including external keying**: every FX slot now has a second combo next to its
existing Source selector -- **Sidechain Source** (`/hds/fx/%d/sidechain_source`), defaulting to
"Same as Source" (self-key, ordinary non-external-sidechain compression, the only mode the real
hardware and the original plugin support). Pick any other source instead -- an individual Combi
Program, the Combi Mix bus, or an ext-in pair -- and FairKid67's own detector keys off THAT signal
while still compressing this slot's own audio, e.g. ducking a synth pad's Program under a kick drum
routed in on an ext-in pair. Implemented via a per-sample snapshot of every source's own pre-FX dry
signal (`sc_snapshot_l/r` in `process_fx_chain()`), taken once before any source's FX chain has
mutated its own bus in place -- reading the live, in-place-mutated bus array instead would give a
slot an order-dependent (sometimes pre-FX, sometimes post-FX) view of another source depending
purely on which of the 17 sources happens to be processed first that sample. Only FairKid67 reads
this field today, but it's a generic per-slot field (like `fx_source` itself), so a future
sidechain-capable effect gets it for free.

**What's ported faithfully vs. simplified off the original's JUCE-based C++**: the Koren triode
model and its analytic partial derivatives; each stage's exact KCL equations and Cramer's-rule
Newton step; warm-starting the solve from the previous SAMPLE's own converged operating point
(what makes an 8-iteration cap enough -- steady-state audio converges in far fewer); the
grid-conduction clamp; the push-pull topology's differential combination for even-harmonic
cancellation (verified directly -- a symmetric-drive test shows a near-zero DC output average);
the sidechain detector's forward-voltage dead zone and its full timing-preset table; the
cubic-Hermite soft-knee CV limiter; and every link mode's combination formula. Changed: no JUCE
anywhere; each stage's optional cathode bypass capacitor is omitted (nothing in this signal chain
appears to actually enable one); each stage's small-signal quiescent gain (used to normalize it to
unity at rest) is found via a one-time finite-difference probe rather than an analytic Jacobian --
the same derivative, computed numerically, at zero per-sample cost since it only runs once when a
slot first activates the effect; the NR iteration budget is fixed at 8 (the original's own "Draft"
ceiling) rather than runtime-switchable; the output transformer's 2nd-order filter pair is
reimplemented with this file's own existing RBJ-cookbook biquad code rather than guessing at a
distinct class whose exact source wasn't available to this port's research. See
`run_fx_fairkid67()`'s own (long) comment in `backend.c` for the complete list with the reasoning
behind each change, and its own DSP test (compression scaling, external-sidechain routing, link
mode differences, push-pull symmetry, and finiteness under pathological input) for how it was
verified.

### PeakMax FX slot (lookahead peak limiter / loudness maximizer)

An FX slot (Type: `PeakMax`) ported from [JClones_L2](https://github.com/JClones/JSFXClones) (MIT
license) -- a clone of a widely-used mastering peak limiter/loudness maximizer. The first FX type in
this file that's genuinely lookahead: it delays its own audio by a fixed ~1.3ms (64 samples at this
engine's 48kHz) so gain reduction can already account for a peak before it reaches the output --
true brickwall behavior, not just a very fast attack. That delay is real, unavoidable added latency
on whichever slot uses this effect (nothing else in this file adds any); this engine has no
plugin-delay-compensation mechanism to time-align a delayed source's bus against an undelayed one
live, and doesn't report the latency to a CLAP host yet either -- both real, currently-unaddressed
follow-ups if this ever needs to sit alongside undelayed material in a live mix.

Seven params: **Threshold** (-30..0dB -- really an input gain boost of the opposite sign, so
limiting always happens against a fixed internal 0dBFS point, the same "boost in, brickwall at a
fixed point, trim on the way out" shape the original uses) and **Ceiling** (-30..0dB, final output
trim); **Auto Release** (Off/On) and **Release** (six fixed times, 0.01ms-1000ms, ignored while Auto
Release is On) -- On replaces the fixed release with a 3-stage adaptive detector (slow/fast/dynamic,
with a piecewise-linear release-time-vs-program-material curve) that's the original's own signature
"intelligent release" behavior, ported in full; **Quantize** (16/18/20/22/24-bit), **Dither**
(None/Type1/Type2), **Shaping** (None/Moderate/Normal/Ultra) -- a complete IDR-style dither/noise-
shaping stage (full coefficient table, both PRNGs, truncate-with-error-feedback), included in full
even though it's arguably a mastering/bit-depth-delivery feature rather than something this always-
float engine strictly needs -- it's still a well-defined, self-contained coloration effect on a
float signal regardless of what's downstream, the same reasoning any bit-crusher effect already
relies on. At the defaults (24-bit, Type1, Normal), the added dither noise is only about ±0.5 LSB of
a 24-bit signal (roughly -144dBFS) -- inaudible unless Quantize is deliberately lowered.

**What's ported faithfully vs. changed**: the entire signal path is kept faithful -- the gain-staging
shape, the lookahead peak detector (an exact minimum of the per-sample gain-needed value over the
upcoming window), the two-stage triangular smoothing after the release envelope, the manual release,
the full auto-release cascade with its 3 fixed calibration time constants, and the complete dither
stage. Changed: no JSFX/EEL2 runtime, so its dynamic memory pool becomes fixed-size C arrays; the
original's own bespoke branching-factor-8 min-tree is replaced with a standard monotonic-deque
sliding-window minimum -- a different exact algorithm for the identical well-defined quantity ("the
minimum of the last N pushed values"), not an approximation of it; the dither's 32-bit LCG uses plain
`uint32_t` wraparound arithmetic rather than EEL2's own value-reinterpreted-as-integer semantics
(the same modular arithmetic either way); no internal Mix param and no sidechain input, matching the
original, which works on its own stereo signal only. See `run_fx_peakmax()`'s own (long) comment in
`backend.c` for the complete list, and its own DSP test (brickwall accuracy against a known input
level, quiet-signal gain staging, exact lookahead-latency verification via an impulse, auto-release
engage/release behavior, and dithered-output finiteness) for how it was verified.

### MultiFilter FX slot (wide-range steepness filter)

An FX slot (Type: `MultiFilter`) for a much steeper (or shallower) filter than the per-Program
filter slots (Filters tab, `ProgramState.filters[5]`) offer -- those stay exactly as they are; this
is a separate, self-contained implementation reusing the same underlying technique (Vadim
Zavalishin's topology-preserving-transform state-variable filter, "The Art of VA Filter Design" --
the same reference this engine's own per-voice filters already credit) so this addition can never
affect that existing feature's behavior.

Four params: **Type** (Lowpass/Highpass/Bandpass/Notch, the same exclusive-pick vocabulary the
per-voice filters use), **Cutoff** (20Hz-20kHz, log-mapped), **Resonance** (0-100, same damping-floor
formula the per-voice filters use, so it feels identical), and **Slope** -- 12 choices from 6dB/oct
up to 96dB/oct in 6dB steps, built by cascading 1-8 identical 2-pole TPT stages (12dB/oct each) plus
a single extra 1-pole stage for an odd 6dB remainder on Lowpass/Highpass. Bandpass/Notch have no
natural 1-pole equivalent: an odd Slope choice on those two types rounds down to the next 12dB step,
and 6dB itself clamps up to the smallest meaningful bandpass/notch this topology can express (one
2-pole stage, 12dB/oct) rather than silently doing nothing. Every cascaded stage shares one Cutoff/
Resonance, so resonance compounds at steep slopes -- an expected "cascaded analog stages" character,
not a textbook per-stage-tuned Butterworth design, and not a bug.

Verified with a standalone frequency-response test: measured attenuation two octaves from cutoff
scales almost exactly linearly with the Slope choice (~-12.5dB per 6dB step, matching a clean N-pole
rolloff) until the test's own measurement floor takes over well past -90dB; the passband stays flat
(under 1dB of ripple) even at 96dB/oct; the Bandpass/Notch odd-slope clamp behaves correctly (6dB and
12dB give identical, filtered -- not silently bypassed -- output); and an 8-stage cascade at maximum
resonance stays finite and bounded over 200,000 samples (~4 seconds).

### OptoComp FX slot (feed-forward optical-compressor approximation)

An FX slot (Type: `OptoComp`) ported from [4a2a](https://github.com/aim-qmul/4a2a) (MPL-2.0
license) -- a feed-forward digital approximation of a classic optical leveling amplifier/compressor.
Genuinely compact: a hard-knee power-law gain computer with no separate detector/envelope stage at
all (the instantaneous absolute sample value directly drives `min(1, (|x|/thresholdGain)^-slope)`),
followed by one asymmetric-coefficient one-pole smoother applied to that resulting gain value itself,
then makeup gain.

Five params: **Threshold** (-60..0dB), **Ratio** (1..20 -- 1 is the original's own default, meaning
no compression at all until turned up), **Attack** (0.1..100ms), **Release** (100..1000ms), and
**Makeup Gain** (-12..+12dB). Left out: the original's own sixth ("Peak Reduction," a single-knob
macro that maps onto these five params in its own GUI layer -- UI convenience, not DSP) and seventh
("Limit Mode," a boolean the original declares but its own `processBlock()` never actually reads).
The original is mono-only (its own JUCE input/output buses are both declared `.mono()`); this stereo
adaptation shares one detector/gain value off `max(|L|,|R|)` across both channels, the same posture
FX_TYPE_GATE/FX_TYPE_LIMITER already use and for the identical reason (an independent per-channel
envelope would skew the stereo image every time the compressor moved).

**What's ported faithfully vs. adapted**: the entire DSP is kept faithful, including the original's
own specific `m2c()` time-constant calibration (`1 - exp(-2200/(ms*srate))` -- note `ms` is used
exactly as given, not converted to seconds first; ported as-is rather than "corrected" to a more
usual convention, since this specific formula is what gives the port its actual timing feel). The
only adaptation: the original's own per-BLOCK array-with-carry state (JUCE's typical block-processing
style) becomes a single persistent float here, since this engine calls every FX effect once per
SAMPLE rather than once per block -- an adaptation of the reference's own block-oriented plumbing to
this engine's calling convention, not a change to the DSP (sample-for-sample identical results
either way). Verified with a standalone test harness: bit-exact transparency at Ratio=1, steady-state
gain reduction matching the reference formula to 5 decimal places, correct relative attack/release
timing, exact makeup-gain math, and correct shared-detector stereo behavior (a loud channel driving
gain reduction on both).

### DeEsser FX slot (split-band sibilance control)

An FX slot (Type: `DeEsser`) -- unlike the compressors above, this one isn't a clone of any specific
hardware or plugin (there's no "coveted vintage character" angle to a de-esser the way there is for
the LA-2A/L2/1176/Fairchild-style units), just this engine's own original implementation of the
standard modern technique: split the signal into a low band and a high (sibilance) band, compress
only the high band, then recombine.

The split reuses ONE highpass -- FX_TYPE_MULTIFILTER's own 2-pole TPT stage, fixed at Butterworth Q
with no resonance -- to get the high band, and derives the low band as `input - high` rather than
running a second, separately-designed lowpass: since `low + high == input` is then true BY
CONSTRUCTION regardless of the highpass's own shape, this guarantees exact, phase-coherent
recombination with only one filter running and no matched-crossover design needed at all (verified
directly: output equals input bit-for-bit whenever the high band's own level stays under Threshold).
The high band's own level (self-detecting, no external sidechain) then drives a hard-knee power-law
gain computer identical in shape to OptoComp's, using this file's own native exponential attack/
release convention (matching Gate/Limiter) rather than borrowing 4a2a's own specific calibration,
since nothing here is emulating that plugin.

Six params: **Frequency** (1-16kHz, log -- the crossover/detection point, typically 4-8kHz for
sibilance), **Threshold** (-40..0dB, keyed off the high band's own level), **Ratio** (1..20),
**Attack** (0.1..20ms -- fast, since sibilance is a short transient), **Release** (5..300ms -- also
fast, to avoid audibly pumping the top end), and **Listen** (Off/On -- solos the RAW, pre-gain-
reduction high band in place of the normal output, so Frequency can be dialed in by ear before
trusting Threshold/Ratio to do anything useful). Verified with a standalone test harness: exact
(bit-identical) reconstruction below threshold, correct gain reduction on a loud high-frequency
tone, Listen mode correctly isolating the high band (near-silent on a pure low-frequency input,
passing a pure high-frequency input through), and stability under extreme settings.

### HardTune FX slot (real-time pitch correction)

An FX slot (Type: `HardTune`) -- like MultiFilter/DeEsser, an original implementation of a standard,
decades-old, published technique (YIN pitch detection + PSOLA-style resynthesis), not a clone of any
specific commercial pitch-correction product (whose exact internals aren't public, and some of whose
retune-speed/formant-preserving behavior is patented). This is the first FX type whose added latency
(~14ms) is structural and unconditional: the lookahead/tap-delay buffer runs continuously whenever
this slot is active, whether or not a pitch is currently being corrected -- there's no way to "engage
the delay only when correcting" without the buffer already being primed to catch a pitch the instant
one starts. Same category of trade-off as PeakMax's own lookahead limiter.

Three stages: **(1)** YIN (de Cheveigne & Kawahara, 2002) pitch detection, run every 10ms on a
mono-summed analysis window; **(2)** the detected pitch is quantized to a target -- either the
nearest note in a chosen Key/Scale, or whatever MIDI note is currently held (monophonic, last-note-
priority, engine-wide -- see `g_hardtune_held_note`) -- with two independent smoothing stages:
Tracking Speed (smooths the raw detected pitch before quantization) and Retune Speed (smooths the
final corrected ratio); **(3)** a period-synchronous two-tap crossfaded delay-line read applies the
correction -- PSOLA's own core resynthesis mechanism, without needing explicit pitch-mark placement:
a phase accumulator advances at the pitch-shift rate and wraps modulo the detected input period, and
two read taps 50% of a period apart, each windowed by a Hann function of its own phase, crossfade
into each other with zero gain ripple at the wrap point (a Hann window's value at phase x and at
x+50% sum to exactly 1, the standard constant-overlap-add property -- no extra normalization needed).

Ten params: **Key** (12 notes) and **Scale** (Chromatic/Major/Minor/Major Pentatonic/Minor
Pentatonic) for the target grid; **Target Source** (Scale/Key, or MIDI -- mutually exclusive);
**Amount** (0-100%, blends none..full correction in semitone space); **Retune Speed** (glide time
into the corrected note -- low is the instant "2006-era" hard-tune snap, higher is a natural,
musical portamento; defaults moderate, not the extreme snap, so it's usable for a natural
performance out of the box); **Tracking Speed** (a separate smoothing stage on the raw detected
pitch, before quantization -- how quickly the detector "believes" a new note started vs. treating a
fast wobble as noise); **Voice Range** (Bass/Tenor/Alto/Soprano/Wide -- narrows the detector's
own search window to the expected fundamental range for that voice type, which measurably reduces
octave-detection errors, the same real reason commercial pitch correctors offer this, not just a
cosmetic preset); and **Vibrato Rate**/**Vibrato Depth**/**Vibrato Mod Source** (this effect's own
dedicated sine LFO -- Rate 0.1-10Hz, Depth 0-1.5 semitones peak, defaulting to 0/off so an existing
patch's sound doesn't change until it's dialed in; Mod Source is None/Mod Wheel/Aftertouch, gating
Depth by a live controller the same way `PackLfoParams.mod_source_cc` gates a regular LFO's depth).

Three inputs apply unconditionally, regardless of Target Source: live **MIDI pitch bend** (reusing
Program 0's own pitch-bend-range knobs, the same convention `render_audio_block`'s per-Program pitch
bend already uses), the **Vibrato** LFO just described, and Target Source's own MIDI mode (which
reads an engine-wide monophonic note tracker, not any one Program's). Pitch bend and Vibrato are
each a final multiplicative adjustment layered on top of the already-corrected ratio, computed AFTER
Retune Speed's own smoothing and never fed back into detection or the quantization decision above --
so neither one can ever fight the correction. Vibrato is a dedicated per-slot LFO rather than routed
through the per-Program LFO matrix: HardTune is a global FX effect with no single Program or voice
of its own for that matrix's destinations to target.

**A real bug found and fixed during testing**: the PSOLA read-position math initially accumulated
the tap phase at the raw pitch-shift ratio, but since the read position is `write_pos + phase_offset`
and `write_pos` itself already advances by 1 every sample, this made the effective read speed
`1 + ratio` instead of `ratio` -- confirmed directly (a 233Hz input targeting 246.94Hz measured at
241-247Hz depending on test signal, inconsistent with the intended target) and fixed by accumulating
phase at `(ratio - 1)` instead, plus adding the negative-wraparound case needed for downward
corrections (only the positive direction was handled before). Verified after the fix: a realistic
sawtooth (glottal-pulse-like) test signal, autocorrelation-measured with parabolic sub-sample
interpolation, lands within 0.1Hz of the intended target for both upward and downward corrections;
latency measured via a tone-burst threshold matches the designed ~14ms exactly; Amount=0 stays
transparent; silence produces no instability.

**A known, precisely-measured limit, not a bug**: systematic testing (sweeping correction size in
1-semitone steps against a real periodic test signal) found the two-tap crossfade is accurate to
within 0.1% for corrections up to about 3 semitones (a minor third) either direction, then degrades
in a characteristic, exactly-reproducible way rather than just "getting worse": the two windows' own
crossfade rate is exactly `target_hz - input_hz`, and once that's comparable to the target pitch
itself, the perceived output locks onto a rational sub-multiple of the intended target instead --
measured exactly 2/3 of target for 4-9 semitones of correction, exactly 1/2 (an octave low) for 10+
semitones. This can never happen in Scale/Key mode: snapping to the nearest note in any of the 5
scales offered is at most ~1.5-2 semitones away by construction. Only MIDI Target Source with a held
note more than a minor third from the input (a deliberate wide-interval harmony, not "correction"),
or a large pitch bend stacked on top, can reach it -- a real but narrow, documented limitation of
this simple technique at large intervals, not something ordinary pitch-correction use ever hits.

### FDNVerb FX slot (algorithmic hall/plate/room reverb)

An FX slot (Type: `FDNVerb`) -- like MultiFilter/DeEsser/HardTune, an original implementation of a
well-established, published DSP architecture (an 8x8 Feedback Delay Network with an
energy-preserving Hadamard mixing matrix -- textbook Jot/Stautner-Puckette-style reverb design),
not a clone of any specific commercial unit's own internals. Ported from and informed by the
MIT-licensed [ninuxi/MAI_verb](https://github.com/ninuxi/MAI_verb) -- specifically its FDN8x8 and
EarlyReflections engines -- restructured from JUCE's block-oriented API into this engine's own
per-sample dispatch.

Signal path: input -> Pre-Delay -> **(a)** a 16-tap, dual-diffused, L/R-decorrelated Early
Reflections bank (alternating-polarity tap gains simulate real wall bounces) and **(b)** the FDN8x8
late reverb -- 4 nested allpass filters diffuse the input, 8 prime-length delay lines (each with its
own decorrelated sinusoidal read-position wobble, killing metallic ringing) feed an 8x8 Hadamard
matrix mix blended against their own raw taps by Diffusion, each tap damped by a one-pole lowpass
and scaled by an RT60-derived feedback gain before recirculating (soft-clipped via `tanh` for
safety) -- with (a) and (b) summed into the final wet output.

Ten params: **Decay** (0.3-12s RT60, log taper -- the actual -60dB decay time, verified end-to-end
via windowed energy measurement against a real impulse: measured RT60 tracked the target formula
closely across the full range, e.g. 0.62 -> measured 3.00s vs. a 3.00s target); **Size** (scales all
8 delay lengths 0.3x-1.5x -- bigger room, longer pre-mix time before the tail proper); **Pre-Delay**
(0-250ms, the gap before any reflection arrives -- reads as physical room distance); **Damping**
(a one-pole lowpass in the feedback loop -- higher = darker, faster-decaying highs, like real room
air absorption); **Diffusion** (glassy/metallic with audible early echoes at low settings, a dense
smooth wash at high); **Mod Rate**/**Mod Depth** (a per-delay-line sinusoidal read-position wobble,
decorrelated per line via fixed rate/phase offsets -- the standard "chorused feedback" trick that
keeps a long, high-diffusion tail from ringing at a fixed metallic pitch); **Early Reflections
Level**/**Early Reflections Decay** (the 16-tap bank's overall amount and how quickly its taps fall
off -- Level near 0 isolates the FDN's own smooth tail for critical listening); and **Freeze**
(Off/On -- forces every feedback gain to 1.0 and stops new input from entering the delay lines, the
standard "infinite sustain" trick: whatever's already circulating loops forever, verified to hold
within 0.4dB over 5 seconds once engaged after the tank has filled). Overall dry/wet uses this
slot's own generic Mix, like every other FX type.

**Two dead knobs found in the reference and deliberately not carried over** (found by reading its
own call graph, not guessed): its Decay Low/Decay High controls are stored but never read by
anything that touches audio -- its own feedback-gain calculation only ever reads the Mid decay time,
so only ONE decay control does anything acoustically; and its Damping Low control computes a
highpass frequency/coefficient that its own feedback filter's `process()` never applies (only that
one-pole filter's lowpass half is ever used) -- Damping Low is silently inert too. Porting either as
a real knob here would just be two dead pots on the panel; the param budget saved went to Early
Reflections Decay instead, a control the reference's own panel already exposes and one that IS real.

**One genuine redundancy was quietly corrected**: the reference bakes Early (Reflections) Level into
each tap's own gain, then multiplies the already-scaled result by Early Level a SECOND time when
combining with the FDN tail -- so its knob shapes the taps by Level² while a separate dry-signal
leak into the same sum (see below) only sees Level¹, a mismatched, almost certainly unintentional
double-application nothing in the source explains. Level is applied exactly once here, giving a
knob that responds linearly like every other control in this engine.

**Preserved deliberately, not cleaned up**: Early Level's mix is not a clean crossfade of just the
reflections -- the reference's own ER working buffer starts as a copy of the (pre-delayed) dry
signal, the 16 taps get added onto that copy in place, and the WHOLE thing (dry copy plus taps
together) is what gets scaled by Early Level before summing with the FDN tail. Raising Early Level
therefore also proportionally reintroduces a scaled copy of the dry signal into the wet bus, on top
of this slot's own generic Mix -- a real, audible characteristic of how the reference algorithm
actually sounds, kept here on purpose.

### Reverse Sawtooth (Rev Saw): oscillator waveform and LFO shape

Osc 1-4's Shape dropdown gains a `Rev Saw` entry (a descending ramp, +1 -> -1 across the cycle,
appended after `Off` rather than inserted next to `Saw` so `Off`'s own numeric index never shifts),
and the LFO section's own `Saw` dropdown entry (which was already a descending ramp, just not
labeled as one) is relabeled `Rev Saw` for clarity, with a genuine ascending `Saw` appended as a new
entry after `Off`. Both are alias-free (PolyBLEP-corrected) and both support hard sync.

A plain sawtooth and its reverse are **spectrally identical** -- every harmonic has the same
magnitude, just phase-inverted -- so a single Rev Saw oscillator sounds the same as a single Saw
oscillator alone; that's expected, not a bug (verified: measured RMS energy identical to 6 decimal
places, and `Saw(t) == -RevSaw(t)` exactly at every sample). It matters the moment something else is
introduced:

- **Two oscillators summed**: a Saw and a Rev Saw at the exact same phase are perfect negatives of
  each other and cancel to silence (verified directly). At ANY other relative phase offset, their
  equal-and-opposite slopes make the sum's slope zero everywhere except at the two (now separated)
  discontinuities, producing an exact two-level rectangle wave -- 180 degrees gives a symmetric
  square, other offsets give an asymmetric pulse, and detuning one oscillator against the other
  continuously sweeps the offset, i.e. PWM. This was verified by direct algebra (not just visually):
  at a 90-degree offset the ideal sum lands on exactly two flat levels, `[-1.5, 0.5]` -- a real,
  25/75-duty pulse, not the symmetric square a quick skim of some forum explanations claims (180
  degrees is the special case that gives an exact symmetric square; 90 degrees gives a specific
  asymmetric one).
- **Hard sync**: a sync reset snaps phase back to 0, which lands on the OPPOSITE value for a Rev Saw
  (+1) vs. a Saw (-1) -- giving a genuinely different sync-sweep timbre, and both are BLEP-corrected
  for the discontinuity that reset creates.
- **As an LFO/mod source**: rising vs. falling gives an audibly opposite modulation contour even
  though the two would sound identical as a lone audio tone -- snap-then-rise vs. rise-then-snap when
  routed to something like filter cutoff. This is the reason hardware like the Moog Model D/Roland
  SE-02 offers a dedicated reverse-saw option specifically on a modulation oscillator.

### Per-slot gain reduction (GR) metering

Gate, Limiter, FetComp, FairKid67, PeakMax, OptoComp, and DeEsser -- the 7 FX types with a real gain-reduction stage --
each report their current reduction in dB over OSC as `/hds/status/fx_gr/<slot>` (0 for a bypassed,
Off, or non-GR-capable slot), sent every status tick (~31Hz) alongside the rest of
`/hds/status/*`. The frontend shows this as a small "GR" readout next to each FX slot's Mix/Bypass
controls, visible only while that slot's Type is one of the seven above. Computed per effect from
whatever state that effect already tracks, no extra DSP added purely for metering:
- **Gate/Limiter**: both already track one applied linear gain multiplier (`gain`, 1.0 = no
  reduction) -- `-20*log10(gain)`, clamped >=0.
- **FetComp**: the gain cell's own control voltage (`ctrl_volts`) plugs directly into the same
  closed-form solve `run_fx_fetcomp()` already uses, since at `ctrl_volts=0` (quiescent) that
  solve's small-signal gain is exactly 1 -- `20*log10(1 + R5*two_beta*ctrl_volts)`, no separate
  quiescent probe needed.
- **FairKid67**: the push-pull output stage (the only one of its 6 triode stages a control voltage
  ever drives away from quiescent) doesn't have a closed-form gain -- its small-signal gain is
  found via implicit differentiation of the same Newton-Raphson KCL residuals `fk67_stage_nr_solve()`
  already zeroes every sample (`dVp/dVin = gm*(J11-J01)/det`, from the same Jacobian terms that
  solve already computes), then compared against that stage's own cached `cv=0` quiescent gain
  (found once, at slot activation, via a finite-difference probe -- see the FairKid67 section
  above) to get a ratio, worst-case (max) across both arms/both channels. One extra model
  evaluation per stage per sample, no extra Newton iterations.
- **PeakMax**: unlike the other four, this one already computes a real applied gain multiplier
  (`output_gr`, after its own triangular smoothing) as part of normal operation, purely because
  computing it IS how the limiter works -- `-20*log10(output_gr)`, clamped >=0, no extra work at all.
- **OptoComp**: same shape as Gate/Limiter -- already tracks one applied linear gain multiplier
  (`gain_smoothed`) as part of normal operation -- `-20*log10(gain_smoothed)`, clamped >=0.
- **DeEsser**: same shape again -- `gain_smoothed` here is the gain applied to the high (sibilance)
  band specifically, not the full signal -- `-20*log10(gain_smoothed)`, clamped >=0.

### Control protocol: OSC

The backend is controlled entirely over OSC (Open Sound Control), via a
plain UDP `lo_server` -- every parameter is addressed by name (e.g.
`/hds/vco1/shape`, `/hds/eg/3/attack`, `/hds/fx/0/param/2`), not by a
positional byte offset in a shared struct. This replaced an earlier
Unix-socket protocol that packed every control into one big binary blob at
a fixed byte position; a single misplaced field in that scheme could
silently corrupt unrelated data (as happened once with `record_format`).
Addressing every parameter by name instead makes that whole bug class
impossible: a wrong field name is a compile error, not a silent miswrite.

- `--osc-listen-port <port>` (default `9000`) -- incoming control messages
  (every knob, plus `/hds/zone/upsert`/`/hds/zone/remove` for the Sample
  Bank catalog).
- `--osc-reply-host <host>` / `--osc-reply-port <port>` (default
  `127.0.0.1`/`9001`) -- where periodic `/hds/status/*` feedback (VU peak,
  recording state, detected input channel count, live tempo) is sent.

See the `OSC_PARAMS[]` table and its surrounding comment in `backend.c`
for the full address list. Any OSC-capable tool or library (TouchOSC, Max/
MSP, Pure Data, `python-osc`, `liblo` itself) can act as a controller --
`src/frontend.py` is one such controller (via `python-osc`), but nothing
about the protocol is specific to it.

#### Running the frontend and backend on separate machines

Both sides support this today -- the backend's OSC listener already binds
to all network interfaces (not just loopback), and `frontend.py`'s own
status-reply listener does too, so nothing in either program is
loopback-only. To run them on two different machines:

1. On the backend machine, start `synth_engine` (or `synth_engine-arm64`)
   with `--osc-reply-host <frontend-machine-IP>`, pointing status replies
   back at the machine that will run the GUI.
2. On the frontend machine, run `python3 src/frontend.py --osc-host
   <backend-machine-IP>`, pointing outgoing knob/zone traffic at the
   backend machine.

`--osc-listen-port`/`--osc-reply-port` (on the backend) and
`--osc-listen-port`/`--osc-reply-port` (on `frontend.py`, same flag names,
controlling the same two ports from the opposite end of the wire) only
need to change from their defaults if you're running more than one
backend/frontend pair that would otherwise collide on the same machine or
network.

Only run this across machines/networks you trust -- like the rest of this
protocol (see the liveness note below), OSC here has no authentication,
so anything that can reach the listening port can send it commands.

Since UDP has no connect-time liveness signal, a controller can't tell
"backend not running" from "backend running but I haven't heard from it
yet" just from sending -- `frontend.py` infers this from whether a status
bundle has arrived recently (see `OSC_STATUS_TIMEOUT_S`), falling back to
its standalone mock mode if not. Multiple simultaneous controllers (e.g.
`frontend.py` and TouchOSC both talking to the same backend at once) each
work independently but don't mirror each other's state live -- that would
need each controller to also listen for every other controller's writes,
which none of them currently do.

#### Using the frontend GUI with the CLAP plugin

`hds.clap`, once loaded into a host, has no visible UI of its own -- it
embeds an OSC control server (see "CLAP: multi-out audio ports"/"CLAP:
params extension" above), the same protocol the standalone app speaks, just
on different ports so it never collides with a `synth_engine` you might
also have running. `frontend.py` is the recommended way to actually see
and drive it: point it at the plugin instance's ports instead of the
standalone app's defaults --

```sh
python3 src/frontend.py --osc-listen-port 9010 --osc-reply-port 9011
```

That's it for the common case (one host, one instance, running on the same
machine as the GUI) -- `--osc-host` stays at its `127.0.0.1` default since
the plugin runs in the same host process on the same machine.

Port numbers to use for less common cases:

- **More than one instance in the same host** (e.g. HDS on several
  tracks): each instance claims the next free pair 2 ports up from the
  last -- instance 1 gets 9010/9011, instance 2 gets 9012/9013, instance 3
  gets 9014/9015, and so on (`CLAP_OSC_LISTEN_PORT_BASE`/
  `CLAP_OSC_REPLY_PORT_BASE` in `clap_plugin.c`). There's no way to choose
  which instance gets which pair -- it's whichever port each one manages to
  bind first, in whatever order the host activates them -- so if you have
  more than one instance loaded, check which pair a given one actually got
  rather than assuming. Each bound pair is logged to the host's own log/
  console (look for a line like `[hds-clap] OSC listening on UDP port
  9012, status replies to 127.0.0.1:9013`, either on the host's stderr or,
  for hosts that surface it, via the CLAP `log` extension), and run one
  `frontend.py --osc-listen-port <N> --osc-reply-port <N+1>` per instance
  you want a GUI open for.
- **A `synth_engine` instance already running on 9000/9001 at the same
  time**: no conflict -- the plugin's 9010+ range never overlaps the
  standalone app's 9000/9001, so both can be driven from separate
  `frontend.py` processes simultaneously.
- **The host is on a different machine than the GUI** (e.g. a headless DAW
  render box): same as the standalone cross-machine case above, but there's
  no `--osc-reply-host`-equivalent flag on the plugin side to point status
  replies back at a remote frontend -- the plugin always replies to
  `127.0.0.1` (`CLAP_OSC_REPLY_HOST` in `clap_plugin.c`). Run `frontend.py`
  on the same machine as the host for now if you need live status feedback
  (VU meter, tempo, recording state); knob control alone could still reach
  a remote plugin instance via `--osc-host`, but its status replies
  wouldn't find their way back.

Connecting a fresh `frontend.py` to an already-configured instance (a DAW's
restored patch, or simply a second GUI pointed at a different instance's
port pair) now correctly pulls and displays that instance's real
Performance Mode and every Sequencer/Combi lane setting, not just its
oscillator/filter/EG knobs -- previously, any of these Sequencer/Combi
fields the GUI didn't already have loaded would get silently blasted back
to that fresh GUI's own defaults the instant it connected (see
`_apply_seq_and_combi_fields()` in `frontend.py`), which made editing an
existing preset through a second address effectively impossible. Fixed:
that pull is now complete for everything `transmit_knob_packet()` can
actually overwrite.

That pull itself also used to be fragile: `frontend.py` requests the
instance's current state exactly once, at its own startup. If that request
went out before the plugin's embedded OSC server was even listening yet
(a completely normal ordering when launching a GUI before the DAW has
actually loaded/activated the instance it's meant to control -- exactly
the situation running several instances tends to produce), the request was
simply lost with no retry, leaving that GUI stuck showing its own
construction-time defaults indefinitely, indistinguishable from the state
pull itself being broken. Fixed: `frontend.py` now re-requests the current
state every time it notices the backend transition from unreachable to
reachable (`listen_for_status_frames()`'s own mock-mode tracking), not
just once at boot -- so the pull happens whenever the instance actually
becomes available, in whatever order things were launched, and self-heals
if the backend later restarts.

Also fixed alongside this: a handful of GUI sliders (`master_vol`,
`vco1_vol`, `vco2_vol`) only ever recorded their intended default as a
double-click-reset target, never actually applying it at construction --
so a disconnected/mock-mode GUI, or the brief window before a real
backend's state pull completes, showed silence (slider position 0) rather
than the intended audible level. Oscillator 2 and 3 also defaulted to an
audible shape (Sine/Noise) rather than Off, and Filter 1 defaulted to an
active LPF rather than Off like every other slot -- both now match
`init_default_controls()`'s own backend-side defaults, which were changed
to match (Off/Off/Off), so a fresh, never-configured instance's actual
starting patch is now silence-free on Oscillators 2-3 and filter-free,
not just the frontend's own pre-connection appearance.

A second pass found the same bug class in three more places: an
address-by-address audit of every OSC address `transmit_knob_packet()`
ever sends against everything the `_apply_*` functions actually restore
(rather than reasoning about it one report at a time) turned up each FX
slot's own source routing (`/hds/fx/{i}/source`) and each source's own
Series/Parallel FX routing mode (`/hds/fx_routing/{i}/mode`) -- both
deliberately excluded from Program Load's own restore behavior (correctly
-- a Program shouldn't dictate session routing), but with nothing else
restoring them for the reconnect case either, so reconnecting silently
reset any ext-in FX routing back to Program/Series. Fixed by a new
`_apply_fx_routing_fields()`, called from `_apply_full_state_fields()`
alongside the existing Sequencer/Combi restore. Programmable tempo markers
(`/hds/tempo/marker_count`, `/hds/tempo/marker/{i}/time|bpm` -- e.g. from
importing a Standard MIDI File) and microtuning
(`/hds/tuning/enable|reference_note|reference_freq`) were a different
shape of the same bug: genuinely Program-scoped, so they should already
have been restored by `_apply_program_fields()` for both Program Load and
reconnect alike -- but nothing there ever applied them either, so both use
cases silently dropped an imported tempo map or an active tuning scale
back to flat/12-TET, and that function's own trailing
`transmit_knob_packet()` then re-sent the emptied-out state, permanently
erasing the real values rather than just failing to display them. Fixed
by restoring both directly inside `_apply_program_fields()`, which fixes
Program Load too, not just reconnect.

A third pass found the actual root cause of reconnecting still silently
corrupting a DAW-restored CLAP patch even with every field above correctly
restorable: a genuine race, invisible to a script that feeds
`_apply_full_state_fields()` a ready-made dict directly (how every fix
above was verified) rather than exercising the real, asynchronous
`/hds/request_state` round trip over actual sockets. `transmit_knob_packet()`'s
very first-ever call after connecting -- from ANY connected signal,
including the user simply touching a single knob -- sends a full snapshot
of every widget's CURRENT value, since its own change-tracking dict
(`_last_wire_state`) starts empty (see that function's own "first call
after startup... sends everything" comment -- a deliberate, correct
design for a genuinely fresh/never-configured backend). But if that first
touch happens before the async state pull (send request -> backend builds
a ~1150-field reply -> several chunked bundles arrive over possibly
several polls -> a sentinel closes it out -> `_apply_full_state_fields()`
applies it) has actually finished, this frontend's own construction-time
defaults are still sitting in every widget the reply hasn't reached yet --
so that first-ever transmit sends e.g. every oscillator reset to Off,
permanently overwriting the DAW's real state, with the correct dump either
arriving too late to matter or arriving into a backend that's already been
overwritten. A real user has no reason to wait for an invisible background
request before touching a knob, and reconnecting to a busy/loaded backend
serving other instances makes the round trip slower, not faster -- so this
wasn't a rare edge case, it was the common one. Fixed with an explicit
guard (`_pending_state_restore`, set by `_begin_state_restore_guard()`):
`transmit_knob_packet()` now refuses to send anything at all while a
state pull is in flight (the touched widget still updates normally on
screen -- only the OSC send is skipped), until either the dump's sentinel
arrives (lifted from inside `_apply_program_fields()`'s own trailing
transmit, which both call sites -- reconnect and Program Load -- funnel
through, so a Program Load started in this same window isn't itself
silently swallowed) or a 1.5-second timeout elapses with no reply at all
(an old backend build that doesn't implement `/hds/request_state`, so
normal operation resumes rather than leaving every control permanently
inert). Verified with a real fake-backend-over-real-UDP-sockets test
(offscreen Qt, real `pythonosc` bundle encoding/parsing, a genuine
background thread standing in for the backend) reproducing the exact
failure -- an immediate post-connect knob touch, with zero artificial
delay -- both before this fix (a full default-laden packet went out) and
after (nothing goes out until the real state has actually landed).

#### Instance Switcher

Running several instances at once (HDS on multiple DAW tracks, or a mix
of CLAP instances and a standalone `synth_engine`) used to mean one
`frontend.py --osc-listen-port N --osc-reply-port N+1` per instance, each
its own window, launched by hand once you'd found the right port pair in
the host's log. The sidebar's **Instance** box replaces that with a
**Switch...** button: it scans for every reachable HDS instance (CLAP or
standalone, local or on the network -- the protocol is identical either
way, so discovery doesn't need to know or care which kind it found) and
lets you retarget the SAME window at a different one instantly, no
relaunch, no manually working out port numbers.

**Discovery** sends `/hds/ping` (an addition with no other purpose) to
127.0.0.1 across the standalone default port plus CLAP's own documented
sequential range, to a LAN broadcast address on the same ports
(best-effort -- works within one broadcast domain, silently does nothing
wherever the network/OS blocks it, e.g. inside a container; never the
only path), and to an optional manually-entered remote host (for a
machine outside broadcast reach -- a different subnet, a VPN). Whoever
replies with `/hds/pong` within half a second is genuinely alive right
now -- "replied just now" IS the liveness check, no separate registry or
heartbeat bookkeeping to keep consistent. Each instance can be given its
own name (`/hds/instance_name`, a plain settable field excluded from
Program scope like Master Volume/Transpose -- loading a Program never
renames the track it's on) so the scan list shows "Track 3 Guitar Amp"
instead of a bare port number.

**Switching** rebinds the frontend's own status-listening socket to a
fresh local port, points it at the new instance, sends `/hds/subscribe`
(tells that instance to redirect its own ongoing VU/tempo/recording-state
pushes here), and immediately pulls its complete live state via the
existing `/hds/request_state` mechanism above -- so the GUI instantly
shows the new instance's real state, not stale leftovers from whichever
one it was just looking at. This needed one real protocol change:
`request_state_handler` and the two SMF import handlers used to always
reply to whatever `--osc-reply-host`/`--osc-reply-port` (or CLAP's own
paired port) was configured at startup; they now reply to whoever
actually sent the request (liblo's `lo_message_get_source()`), so a
frontend that scanned its way to a completely different instance than
whichever one it originally launched alongside still gets its replies
routed back correctly. The periodic, unprompted status pushes are
different -- there's no incoming message to reply to for those, so
`/hds/subscribe` is what tells an instance where to keep sending them;
the last frontend to subscribe wins (this engine only ever had one live
status target to begin with, so "switch which instance I'm watching" and
"become the new status target" are the same action). An old, no-longer-
subscribed instance simply keeps pushing to its last-known port forever
with nothing listening there -- the OS silently drops those packets, and
the fresh-port-per-switch design means that can never collide with a
newly-subscribed instance's own pushes landing on the exact same socket.

### `/hds/request_state` never actually worked -- not a race, dead on arrival every time

Every fix above (the Sequencer/Combi/FX-routing/tempo/tuning restore gaps,
then the reconnect-race guard) was real and independently verified, but
none of them mattered in practice: the state-pull mechanism they all
depend on -- a frontend sending `/hds/request_state` and the backend
replying with its live values -- never actually ran, in either the CLAP
plugin or the standalone engine, from the moment it was first added.
Root-caused by writing a minimal liblo C harness that reproduces the
exact method-registration pattern both `backend.c` and `clap_plugin.c`
used: `generic_osc_handler` -- a real catch-all registered with a NULL
path and NULL types, matching literally every incoming address -- was
registered BEFORE `/hds/request_state`'s own specific handler. liblo
tries a message's matching methods in REGISTRATION ORDER and stops at
the first one that returns 0 ("fully handled") -- it does not prefer a
more specific path match over a less specific one registered earlier.
So every `/hds/request_state` message matched the catch-all first, which
(correctly, from its own narrow perspective) treats an address absent
from `osc_dispatch_table` as "unrecognized," logs it, and returns 0 --
permanently preventing liblo from ever trying the real handler. Verified
both ways with the same harness: the catch-all registered first silently
swallows the message (the specific handler's own log line never prints);
registering the specific handler first lets it fire correctly, with the
catch-all still available afterward for the ~1150 plain knob addresses
that have no handler of their own. The exact same registration-order bug
also silently killed `/hds/song/import_smf` and `/hds/song/import_smf_track`
(Standard MIDI File import's own OSC replies) in both files. Fixed by
moving each file's single `generic_osc_handler` registration to be the
LAST `lo_server_add_method()` call, after every specific-path handler,
in both `backend.c` and `clap_plugin.c` -- this was never a race, never
host-dependent, and never worked once before this fix, exactly as
reported.

### A single missing field in a real dump silently aborted the ENTIRE restore

Caught from a real user-reported traceback (not a synthetic test): a
`KeyError: '/hds/eg/0/attack'` inside `_apply_program_fields_egs()`.
That function gated a whole EG's restore on only `/hds/eg/{i}/delay`
being present, then indexed `hold`/`attack`/`decay`/`sustain`/`release`
directly with no further check -- silently assuming that if one field of
the group survived, all six must have too. A real `/hds/request_state`
reply is several chunked UDP datagrams with no delivery-order or
delivery guarantee at all; losing or reordering even one of them left
exactly this kind of hole. The uncaught `KeyError` aborted
`_apply_program_fields()` PARTWAY THROUGH -- before oscillators/filters
(restored later in that same function) ever got their turn, and before
that function's own trailing `transmit_knob_packet()` (which also clears
the reconnect-restore guard) ever ran. One crashed EG was enough to make
the entire restore look completely non-functional. Fixed by making this
function check each field it reads individually and fall back to the
panel's own current value for whatever a partial dump dropped, matching
every other restore helper in this chain -- audited the rest of the
chain for the same "check one key, assume the whole group" pattern and
found no other instance of it. Also added a safety net around the
sentinel handler itself: any future exception during restore is now
caught, logged loudly, and leaves the reconnect-restore guard alone
(it lifts on its own via its existing timeout) instead of letting a
silent crash corrupt the backend via a half-restored broadcast.

Separately, but from the same traceback: `closeEvent()` crashed with
`AttributeError: 'SimpleUDPClient' object has no attribute 'close'` on
an older installed `pythonosc` version that predates that method. Since
PySide6 only logs an exception raised inside an overridden `closeEvent`
rather than propagating it, `event.accept()` and the next line
(explicitly closing `status_sock`) never ran on every single window
close -- exactly the kind of thing that can leave a socket lingering
into the next launch of the same command. Fixed to fall back to closing
the wrapped raw socket (`_sock`, present in every `pythonosc` release)
when `.close()` itself doesn't exist.

### The status-reply socket's receive buffer silently truncated most of every real dump

The last and, it turned out, dominant cause of erratic recall (working
"mostly" on one connect, then looking just as broken as before on the
very next one, with only a generic `Could not identify content type of
dgram` warning and no traceback to point at): `listen_for_status_frames()`
read every incoming status/reply datagram with `self.status_sock.recvfrom(4096)`.
Measured directly against this build's own real, current address list:
`send_full_state_dump()`'s own 150-message chunks (`backend.c`'s
`HDS_STATE_DUMP_CHUNK_SIZE`) run ~4900-5464 bytes each -- 8 of the 9
chunks in a full ~1235-address dump silently exceed that 4096-byte
buffer. UDP truncates a datagram larger than the receiving buffer with
no error and no way to detect it happened after the fact -- `recvfrom()`
just quietly hands back the first 4096 bytes and discards the rest, so
nearly every chunk of nearly every real dump arrived missing its tail,
with whichever addresses happened to land past the truncation point
(different every time, since it depends on exact byte alignment) simply
absent or feeding a mangled remainder into the next parse attempt. This
is what made the previous `_apply_program_fields_egs()` `KeyError` fix
look incomplete rather than resolved: that fix stopped one specific
missing-field shape from crashing the whole restore, but the underlying
truncation kept producing new, different missing-field shapes on every
single connection, some worse than others. The normal small per-block
status broadcasts (VU meter, transport, and the rest of
`_STATUS_ADDRESSES`) never exceed a few hundred bytes, which is exactly
why ordinary live operation always looked fine and only the (much
larger) state-dump reply was ever affected. Verified with the real
current 1235-address list, real chunking, and real UDP sockets: 0
mismatches restoring against a 65536-byte buffer, versus a mathematical
certainty of truncation on 8 of 9 chunks at the old 4096-byte one. Fixed
by raising the buffer to 65536 bytes -- comfortably above the largest a
UDP datagram can legally be (65507 payload bytes), so no message this or
any future growth of this protocol could ever send on this socket is at
risk of truncation again. (Checked the other direction too, in case the
backend's own receive path had the same class of bug for
`transmit_knob_packet()`'s ~40KB single, unchunked bundle: it doesn't --
liblo's own server-side receive delivered a realistic ~40KB/1235-message
bundle intact with zero loss in a direct test.)

### First connect to a just-loaded DAW session still lagged one reconnect behind

With every transport-layer bug above fixed, one gap remained, isolated
by its own distinctive symptom: connecting fresh to a CLAP instance
right as a DAW session finished loading came back wrong, but simply
closing and reopening `frontend.py` against that SAME already-running
instance moments later came back correct. That shape -- first connect
wrong, every reconnect after it right, against unchanged backend memory
-- is a startup race, not a data or transport bug: the CLAP spec allows
a host to call `state.load()` (restoring a saved project's values into
`p->knobs`) either before OR after `activate()` (which is what starts
this instance's own embedded OSC server), and real hosts differ. If
`frontend.py`'s own connect-time `/hds/request_state` happened to land
in the narrow window between whichever of those two runs first and
whichever runs second, the reply it got was completely valid, complete,
and internally consistent -- and entirely wrong, reporting `p->knobs`
in whatever state it happened to be in at that exact moment, which
depending on the ordering could be pre-load defaults.

A first attempt fixed this on the backend side: `clap_plugin_activate()`
and `clap_state_load()` each proactively pushing a fresh
`send_full_state_dump()` once THEY finished. Reverted -- confirmed to
make things WORSE, turning the previously-reliable reconnect case
intermittent too: an unsolicited push from one of those two functions
can land while a frontend's own request-driven reply is still arriving
in chunks, and `listen_for_status_frames()` buffers every plain knob
address into the SAME `_state_dump_buffer` regardless of which request
it logically belongs to (the wire protocol has no request/response
token to tell two overlapping dumps apart) -- so two dumps in flight at
once can interleave into one corrupted hybrid before either sentinel
arrives. That's a worse failure mode than the original race, not a fix
for it.

Fixed on the frontend side instead, where retries can be sequenced so
at most one dump is ever in flight at a time. A first version of this
fix scheduled follow-up resends on independent `QTimer.singleShot`
callbacks, each skipping itself if `_state_dump_buffer` already had
content or the restore guard had already lifted. That still had its own,
subtler copy of the same race: the retry check ran on a timer completely
unsynchronized with `listen_for_status_frames()`'s own polling timer --
the only thing that actually drains the socket and populates that
buffer -- so a reply already sitting in the kernel's socket buffer, or
still in flight over the wire, was invisible to a check running on a
different clock. Confirmed in real use under a continuously-playing DAW
session: roughly half of all reconnects still came back with pre-restore
defaults, worse than before that fix existed.

Fixed properly by moving the retry decision entirely inside
`listen_for_status_frames()` itself, called right after that same
function's own drain-everything-available loop for the current poll
tick -- the one place that ever reads this socket, so "has anything
arrived yet" is always checked in the same breath as actually checking
for it, never from a second, independently-scheduled clock that can't
see what's already sitting in the kernel buffer. `_maybe_retry_request_state()`
tracks its own send timestamp and retry count directly (`_state_restore_last_send_time`/
`_state_restore_retries_sent`, both reset by `_send_request_state()`)
rather than inferring "is one in flight" from buffer contents alone.
Verified two ways: a fake backend that ignores the first two requests
entirely (simulating the original activate/state.load race window) and
only answers the third; and, specifically targeting the timer-desync
bug, a fake backend whose reply is deliberately delayed to land right at
the retry boundary (simulating real system load, not an idle sandbox) --
10/10 trials restored all 1235 addresses with zero mismatches under the
second test, where the independent-timer version would have been
expected to intermittently corrupt the buffer.

### Window was forced obnoxiously wide, LFO tab's knobs strayed from their labels

Measuring `SynthGui`'s own minimum size hint traced this to the Sequencer
tab's Transport toolbar: every seek/play/pause/stop/metronome/quantize
button plus the resolution and tempo-sync-source combos and the
elapsed/bars:beats readouts sat in one single, unbroken `QHBoxLayout`,
whose combined minimum width measured ~1764px. `QTabWidget` sizes itself
to fit its WIDEST tab's minimum size, not just whichever tab is actually
showing (the same reason the EG/FX tabs already needed their own
`QScrollArea` wrap, per those tabs' own comments) -- so that one toolbar
was forcing the ENTIRE window (every tab, not just Sequencer) to a
~1868px minimum width, overriding the ~1350px `setGeometry()` call at
construction outright. On a tab whose own content needs far less than
that -- the LFO tab measured ~522px -- the leftover ~1300px of forced
width had nowhere to go but into any child widget still sitting at Qt's
default "Expanding" horizontal size policy: a plain `QComboBox` with no
`setFixedWidth` of its own. Three of the LFO row's five combos (the
waveform selector, the Tempo Sync note-division selector, and the Key
Sync selector) were exactly that, so they silently stretched to soak up
that space, visibly separating each row's knobs from their own labels --
"lots of free space padding" was that same forced width with nowhere
else to be absorbed. Fixed two ways: the Transport toolbar is now two
stacked rows instead of one (roughly halving its own forced minimum
width, which brought the whole window's minimum back in line with its
intended ~1350px), and all three previously-unconstrained LFO combos now
have an explicit `setFixedWidth()` (62/90/65px) matching the existing
pattern the LFO row's own destination-assignment combo, and the FX/EG
tabs' combos, already used.

### The Sample Bank was one shared catalog, so loading any Program's SFZ loaded them all

The Sample Bank was a single, unpartitioned catalog per oscillator
(`SampleSlot` entries split only by `assigned_osc`, 0-3): nothing on the
slot itself recorded WHICH Program a zone actually belonged to. That was
invisible with one Program loaded at a time, but broke as soon as more
than one thing could reference the sampler at once -- loading an SFZ for
any Combi lane, or restoring a CLAP project with several Programs each
carrying their own sample set, loaded every one of them into that same
shared pool immediately and unconditionally, whether or not that
Program was the one actually in use. Console output confirmed it: the
CLAP build was visibly loading sample data for patches nothing had
selected, purely because a `SampleSlot` for them happened to exist
somewhere in memory.

Fixed by giving every `SampleSlot` (backend.c), `ZoneRecordWire` (the
OSC/state wire struct), and `SampleZone` (frontend.py) an explicit
`owner_program` field: `0-7` for a Combi lane, or `SINGLE_PROGRAM_SLOT`
(`8`) for the Single Program slot. `find_matching_sample_slot()` now
filters on `owner_program` in all three of its internal search loops
(round-robin hit-counts are indexed by owner too), so two Programs can
each own a completely distinct sample set on the same oscillator without
any collision or cross-talk between them. `hds_load_sfz_for_osc()`,
`apply_zone_upsert()`, and every `handle_note_on` call site that starts
sample playback (`start_sample_osc()`) all take/pass `owner_program`
through explicitly; Program Save/Load and the Mapping tab's own zone
list (`SamplerMappingGrid`) filter and tag by it the same way.

The loading trigger itself stayed deliberately simple: a zone loads the
moment it is actually assigned to a Program -- via Program Load, a Combi
lane's Program picker, live editing in the Mapping tab, or CLAP state
restore -- and for no other reason. An earlier version of this fix
additionally gated loading on `performance_mode` (deferring a Combi
lane's zones until Combi mode was actually the live mode), reasoning
that an inactive lane's samples shouldn't need to be resident yet. That
turned out to be solving a problem that doesn't exist: a lane with
nothing assigned has no zones to defer in the first place, so mode-gating
only added a second, redundant condition on top of ownership without
preventing anything ownership alone didn't already prevent. Reverted
entirely (`zone_owner_is_relevant()`, the deferred-load queue and its
poll function, `HdsSampleEngine.knobs`) in favor of the simpler rule:
**Programs own their SFZ zones, and that ownership alone is both
necessary and sufficient to trigger loading.**

The Mapping tab itself gained no new UI for this -- no "Program:"
selector was added. Every other per-Program editing tab (Filters, EGs,
LFOs, ...) already implicitly operates on "whatever Program is currently
loaded into the one live editing surface," with no selector of its own,
and the Mapping tab now follows the exact same convention:
`SamplerMappingGrid.visible_zones()` filters to
`owner_program == SINGLE_PROGRAM_SLOT` unconditionally, matching Combi
lanes' zones being edited by loading that lane into the live surface the
same way any other per-Program state already is.

This is a wire-format-breaking change: `/hds/zone/upsert` gained a 19th
float argument (`owner_program`, sent as `SINGLE_PROGRAM_SLOT` for the
Single Program slot) and `/hds/zone/load_sfz` gained a leading int
argument, so `HDS_CLAP_STATE_VERSION` was bumped `1` -> `2`. A version-1
CLAP state file (no `owner_program` field on its `Z` lines) still loads
correctly -- `clap_state_load()` branches on the saved version and
defaults every zone from an old save to `SINGLE_PROGRAM_SLOT`, which
matches how those zones actually behaved before this change existed.
