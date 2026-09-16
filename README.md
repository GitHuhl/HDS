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

  **Recording** (`Recording` tab) writes either 32-bit float WAV
  (`WavRecorder` in backend.c -- IEEE float format tag, so no int
  conversion/dithering, and natively readable by any modern DAW/editor) or
  WavPack (`WavPackRecorder`, an additional lossless format alongside WAV,
  ~40-60% smaller; converted to 24-bit PCM before encoding since WavPack --
  like FLAC -- losslessly compresses integer PCM, not float), picked via
  the Output section's Format radio buttons. Both formats reuse the exact
  same realtime-safe ring/writer-thread architecture -- only the writer
  thread's start/write/stop calls branch on the format, so there's no
  additional realtime-thread exposure. Two source kinds, freely
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

  **`src/pi_touch_frontend.cpp`** is a second, much smaller controller for the same backend --
  an ImGui + GLFW touch UI sized for a small square panel (720x720px, e.g. a 4" Raspberry Pi
  touchscreen), talking real `liblo` OSC to the backend's actual `/hds/...` address space (not a
  fictional protocol -- see the file's own header comment on the exploratory draft it replaced).
  The ADSR page (Osc 1's Amp EG, Master Volume, Oscillator 1 waveform) is wired to real fields,
  plus a live Peak/Tempo readback from `/hds/status/*` shown on every page. The **Program
  Library** page reads and writes HDS's real `~/HDS_Programs/<name>/<name>.hdsprog` files (the
  same format `frontend.py`'s own Save/Load uses, JSON-parsed via a vendored nlohmann/json) --
  Load sends a saved file's entire field set straight to the backend (a real whole-patch load,
  not just the handful of fields this page's own sliders show); Save captures this client's own
  observed backend state (see `g_full_state`/`/hds/request_state` in the file) through a C++ port
  of `frontend.py`'s own `_is_program_scope_address()` allowlist, so either app can load what the
  other saved. Category is a real, open (not fixed-enum) metadata field on the file, extending the
  schema `frontend.py`'s own Save dialog now also writes -- the Program Library's category tabs
  are built from whatever's actually in use across saved files, not a hardcoded list. A loaded
  file's colocated `.sfz` (Sample Bank content) is flagged, not applied -- this client has no
  Mapping-tab equivalent. The Comb/Seq/Rec/Glob pages remain placeholder text. A Raspberry Pi 5
  (or any other aarch64 Linux box) needs no cross-compile target -- it's just another native Linux
  host for this same Makefile: install the same apt packages and run `make`/`make pi-touch`
  directly on the Pi. See "Building the Pi/touchscreen client" under Build & run.

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
per-source slot dispatch; each slot still has the same generic 8-param
bank + mix + bypass it always did. Most slot types are still an
unimplemented pass-through, ready for more effects to be ported into the
dispatch switch. Five are implemented so far, all ported from Airwindows
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

Requires ALSA dev headers (`libasound2-dev` on Debian/Ubuntu), WavPack dev
headers (`libwavpack-dev`, for the Recording tab's WavPack format option),
and liblo dev headers (`liblo-dev`, the OSC control protocol -- see below).

```sh
make                    # builds ./synth_engine
pip install -r requirements.txt

./synth_engine &        # start the audio/MIDI engine
python3 src/frontend.py # start the GUI -- talks to the engine over OSC (see below)
```

On NixOS/Nix, `nix-shell` at the repo root provides every dependency above (plus GLFW/OpenGL for
the Pi-touch client below and the Windows cross-toolchain) in one shell, and auto-provisions a
`.venv/` for `requirements.txt` on entry -- see `shell.nix`'s own header comment for exactly what
each target needs from it.

### Building for Windows

Linux (with ALSA) is the primary, actively-developed platform. A Windows build of the backend
also exists, using WASAPI (via the vendored [miniaudio](https://miniaudio.h) single-header
library, `third_party/miniaudio.h`) for audio and WinMM for MIDI instead of ALSA -- see the
"Windows audio/MIDI platform shim" comments near the top of `src/backend.c` for how that's kept
completely isolated from the DSP/synth code, which is identical on both platforms. This is
cross-compiled and link-verified in CI-like conditions, but has **not** been run on real Windows
hardware yet -- treat it as a first real smoke test to confirm on an actual machine, same
limitation as this project's other platform-specific work (real multichannel audio hardware,
etc.).

Cross-compiling from Linux (or WSL) needs the mingw-w64 toolchain:

```sh
sudo apt-get install mingw-w64       # Debian/Ubuntu
tools/build_windows_deps.sh          # one-time: cross-builds liblo + wavpack for Windows
make windows                         # produces synth_engine.exe
```

`tools/build_windows_deps.sh` fetches liblo and WavPack's own source (the same portable C
libraries used on Linux -- there's no Windows-specific fork of either) and cross-builds them as
static libraries under `third_party/win64-deps/` (not committed -- see the script's own header
comment). Audio and MIDI need nothing beyond mingw-w64 itself.

To ship the GUI alongside it as a single double-clickable `.exe`, build `synth_engine.exe` first
(above), then **on an actual Windows machine** (PyInstaller does not cross-compile):

```sh
pip install -r requirements.txt pyinstaller
pyinstaller frontend.spec
```

This produces `dist/HDS.exe`, with `synth_engine.exe` bundled inside it; `frontend.py`'s own
`_maybe_launch_bundled_backend()` detects it's running from a frozen bundle and launches the
bundled engine automatically on startup (a no-op in every other run mode, including the normal
`python3 src/frontend.py` this project is developed against).

### Building the Pi/touchscreen client

`src/pi_touch_frontend.cpp` (see its own description above) is an optional third build target,
not part of plain `make`. Needs GLFW3 + OpenGL + liblo dev headers; Dear ImGui (pinned `v1.91.5`)
and [nlohmann/json](https://github.com/nlohmann/json) (pinned `v3.11.3`, single-header, MIT
licensed, used to read/write real `.hdsprog` files) are vendored into `third_party/` rather than
assumed to be system packages, the same precedent as `third_party/miniaudio.h`/the CLAP SDK headers.

```sh
sudo apt-get install libglfw3-dev liblo-dev    # Debian/Ubuntu
make pi-touch                                   # builds ./hds_pi_touch
./synth_engine &
./hds_pi_touch                                  # talks to the engine over the same OSC ports
```

**Raspberry Pi 5** (or any other aarch64 Linux machine) needs no cross-compile target if you're
building directly on the device -- it's a native Linux host like any other for this exact
Makefile: install the same four apt packages (add `libwavpack-dev` too if also building
`synth_engine` on-device) and run the commands above directly on the Pi.

If the Pi itself runs plain Raspberry Pi OS rather than NixOS (the common case, and the reason
this option exists at all), `nix-shell` won't be available there to build directly on the device.
For that case, `make arm64`/`make pi-touch-arm64` cross-compile `synth_engine`/`hds_pi_touch` for
aarch64 Linux from this x86_64 machine instead -- run from inside `nix-shell` at the repo root
(see `shell.nix`'s own aarch64 section), then copy the resulting `synth_engine-arm64`/
`hds_pi_touch-arm64` binary over to the Pi. Every dependency this needs -- the cross compiler,
alsa-lib, liblo, wavpack, glfw -- comes straight from `nixpkgs.pkgsCross.aarch64-multiplatform` as
a real, already-existing package for this target; unlike the Windows cross-build (which needs
`tools/build_windows_deps.sh` to cross-build liblo/wavpack from source, since mingw builds of them
aren't in nixpkgs), no separate deps-building script is needed here.

**`make arm64`** (the backend alone) was verified fully end to end in this repo's own sandbox: a
real `nix-shell` with the cross toolchain + alsa-lib/liblo/wavpack (every one of which resolves
straight from cache.nixos.org in seconds, no local build) produced a genuine
`synth_engine-arm64` -- confirmed via `file` to be an ELF 64-bit ARM aarch64 executable, correctly
linked against the cross-built libraries.

**`make pi-touch-arm64`** has one real, substantial cost worth stating plainly rather than
glossing over: `glfw`'s own X11+Wayland+Mesa dependency chain is not covered by nixpkgs' binary
cache for this specific cross target as of the channel this was checked against, so the first
`nix-shell` entry that touches this target triggers a genuine from-source build of a sizeable part
of that chain -- confirmed directly by actually running it: openssl, curl, and sqlite (probably
pulled in by Wayland's own build tooling) all built from source in turn, and it then moved on to
building **all of LLVM 17.0.6 from source** (Mesa's dependency for its `llvmpipe` software
rasterizer) -- a build that can easily take multiple hours even on capable hardware. This is a
real, one-time cost on whichever machine first enters this shell and builds this target -- cached
in that machine's own Nix store for every entry after that, same as every other cross toolchain
this file already pulls in -- but it is genuinely large, large enough that it wasn't practical to
wait out to full completion in this sandbox. The Makefile/shell.nix code itself is correct and
consistent with `make arm64`'s own already-verified mechanism (same toolchain, same variable
plumbing, just one more library); it just hasn't been proven end-to-end the way `make arm64` has.

Verified in this repo's own sandbox (no real Pi/touchscreen hardware available here) via Xvfb and
`xdotool`-driven real mouse/keyboard input against a synthetic fake-backend OSC peer: clean builds
with zero warnings across `make`/`make clap`/`make pi-touch`; a captured real `/hds/request_state`
datagram on the wire at startup; screenshots confirming the ADSR sliders/waveform combo update to
match a synthetic reply and the Peak/Tempo readout flips "NO SIGNAL"->"LIVE" on a real
`/hds/status/*` bundle; the Program Library's category tabs building themselves correctly from two
real `.hdsprog` files with different categories; a real double-click Load sending a loaded file's
entire field set over OSC (not just the fields this page's own sliders show) and the ADSR page
updating to match; and a real Save round-trip whose written file correctly includes a Program-scope
field the UI never displays while correctly excluding `master_volume`, an `/hds/audio/`-prefixed
field, and a `/hds/combi/`-lane mirror -- proving the ported allowlist filter matches
`frontend.py`'s own. Real hardware, touch input, and 4" panel readability are still the user's own
confirmation to make, same limitation as this project's other platform-specific work.

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

Since UDP has no connect-time liveness signal, a controller can't tell
"backend not running" from "backend running but I haven't heard from it
yet" just from sending -- `frontend.py` infers this from whether a status
bundle has arrived recently (see `OSC_STATUS_TIMEOUT_S`), falling back to
its standalone mock mode if not. Multiple simultaneous controllers (e.g.
`frontend.py` and TouchOSC both talking to the same backend at once) each
work independently but don't mirror each other's state live -- that would
need each controller to also listen for every other controller's writes,
which none of them currently do.
