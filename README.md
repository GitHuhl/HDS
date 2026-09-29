# HDS

A software synthesizer with a shared DSP core, distributed as three builds:

- **`synth_engine`** — standalone binary. Owns real audio I/O (ALSA or
  PipeWire) and MIDI, controlled over OSC by `frontend.py` (or anything
  else that speaks the wire protocol below). Can also open its own native
  GUI window (`--gui`).
- **`hds.clap`** — the synth as a CLAP instrument plugin for any CLAP host
  (Bitwig, REAPER, ...). No audio/MIDI I/O of its own — the host owns
  that. Ships a native, embedded GUI.
- **`hds_fx.clap`** — an 8-port, 32-slot multi-effects CLAP plugin (no
  synth engine) for running the same FX chains on other tracks. Also has
  a native embedded GUI.

All three share one DSP/state core (`src/backend.c`), so a patch, an FX
chain, or a bug fix behaves identically everywhere.

## Building

Everything builds inside `nix-shell` (`shell.nix` pins every dependency —
Cairo, X11, ALSA, PipeWire, liblo, the NAM/Eigen headers, the Python
venv for `frontend.py`). Enter it once per session, then:

```
nix-shell
make            # -> ./synth_engine
make clap       # -> ./hds.clap
make hds-fx     # -> ./hds_fx.clap
make arm64      # -> synth_engine-arm64 (aarch64 Linux, e.g. Raspberry Pi 5)
```

Windows CLAP builds are cross-compiled from Linux (mingw-w64) and have
been runtime-verified under Wine:

```
tools/build_windows_deps.sh   # once per checkout
make clap-windows             # -> hds-windows.clap
make hds-fx-windows           # -> hds_fx-windows.clap
```

Run the Python frontend (for the standalone build) with:

```
python3 src/frontend.py
```

## Source layout

| File | What it is |
| --- | --- |
| `src/backend.c` | The shared DSP/state core: oscillators, filters, envelopes, LFOs, the FX chain dispatch, the OSC address table (`OSC_PARAMS[]`). `#include`d by both entry points below — never compiled or linked on its own. |
| `src/hds_standalone.c` | `synth_engine`'s own `main()`: ALSA/PipeWire I/O, MIDI sequencer, recording, the Sample Bank disk-streaming thread, the interactive terminal/arrow-key controls, and (opt-in) the native GUI. |
| `src/clap_plugin.c` | `hds.clap`'s CLAP plugin wrapper: calls the same backend functions a block at a time instead of owning a device, plus CLAP's params/state/gui extensions. |
| `src/sequencer.c` | The Sequencer engine (lanes, transport, MIDI/audio playback) — standalone-only, since a DAW host already owns sequencing for its plugins. |
| `src/fx_chain.c` / `src/fx_types.h` | The FX slot engine and every `FX_TYPE_*` shared by all three builds. |
| `src/hds_fx_plugin.c` | `hds_fx.clap`'s own CLAP plugin wrapper around just the FX chain. |
| `src/hds_clap_*_draw.c`, `src/hds_fx_gui_draw.c` | The native Cairo-drawn GUI: one file per tab (Play, Filters, LFOs, Envelopes, Routing, Devices, Global sidebar, FX), platform-agnostic (no X11/Win32 calls). |
| `src/hds_clap_gui_x11.c` / `_win32.c`, `src/hds_fx_gui_x11.c` / `_win32.c`, `src/hds_standalone_gui_x11.c` | The platform shims (window creation, event pump) for each of the above, per OS and per host context. |
| `src/frontend.py` | The Python/PySide6 GUI for the standalone build — the original, most complete control surface, talking to `synth_engine` purely over OSC. |

## Control protocol: OSC

`synth_engine` listens for OSC (UDP, via `liblo`) on `--osc-listen-port`
(default `9000`) and sends status feedback to `--osc-reply-host:--osc-reply-port`
(default `127.0.0.1:9001`). Every parameter has its own address (e.g.
`/hds/vco1/shape`, `/hds/filter/2/cutoff`) written directly into the shared
control struct — there's no positional binary blob to keep in sync by hand.
A few notable addresses:

- `/hds/request_state` — pull this instance's full current state (a DAW-restored
  CLAP patch, or a frontend that just connected, use this instead of assuming
  GUI defaults).
- `/hds/ping` → `/hds/pong` — instance discovery (see `frontend.py`'s
  instance switcher for scanning multiple running instances/ports).
- `/hds/subscribe` — register for periodic `/hds/status/*` pushes.
- `/hds/zone/*` — the Sample Bank (SFZ/WAVE_SAMPLE zone upsert/remove/load).
  First-class SFZ opcode support: round-robin (`seq_length`/`seq_position`,
  independent of `group=`), real choke groups (`group=`/`off_by=`),
  keyswitching (`sw_lokey`/`sw_hikey`/`sw_last`/`sw_default`), `trigger=`
  (`attack`/`release`/`release_key`/`first`/`legato`), velocity/key crossfade
  layering (`xfin_*`/`xfout_*`), `lorand`/`hirand`, `amp_velcurve_<N>`,
  `delay`, `rt_decay`, `loop_crossfade`, `#define` macros, and a per-zone amp
  envelope (`ampeg_*`) that's either fully used as-is or fully overridden by
  the oscillator's own Amp EG via its "Use Amp EG" checkbox (Multisample
  oscillators only) — never blended.
- `/hds/zone/drum_bus` (`ii`: zone id, bus 0-24) — assigns a Sample Bank zone
  to one of 24 Drum Buses (0 = off). A zone routed to a Drum Bus contributes
  NOTHING to its owning oscillator's Volume/Mute/Pan/vel-to-amp or that
  Program's Filter chain — only the zone's own gain/pan and its own amp EG
  level reach the bus — so individual multisample keys (e.g. drum kit
  elements) can be sent straight to their own FX chain and output routing,
  fully bypassing the owning Program. Drum Buses reuse the same FX-slot
  (`/hds/fx/<slot>/source`) and output-routing (`/hds/routing/<source>/out/<pair>`)
  machinery as every other source — Drum Bus 1..24 sit at source ids
  `COMBI_PROGRAM_COUNT + 1 + EXT_IN_PAIR_COUNT` upward. Also settable per-zone
  via the non-standard SFZ opcode `hds_drum_bus=<1-24>`. The native GUI's
  Routing tab has a "Drum Bus Key Routing" grid (below the Source rows,
  scroll to reach it) for this directly: a stepper picks which of the 24
  buses you're editing, and a 7 (natural note letter) x 8 (octave) checkbox
  grid assigns/clears every Single-Program-scope zone covering that note,
  live, no reload needed. The FX and Routing tabs' Source rows/combos
  already show/label Drum Bus sources correctly too.

A CLAP plugin instance runs its own embedded OSC server too (default
`9010`/`9011`, incrementing per instance), so `frontend.py` can drive a
CLAP instance exactly like the standalone, just pointed at a different port.

## Command-line flags

**`./synth_engine`:**

| Flag | Default | Meaning |
| --- | --- | --- |
| `--osc-listen-port <port>` | `9000` | Incoming control port. |
| `--osc-reply-host <host>` / `--osc-reply-port <port>` | `127.0.0.1` / `9001` | Where status feedback is sent. |
| `--output-device <name>` / `--input-device <name>` | `default` | ALSA device string, or a PipeWire node name under `--backend pipewire`. An unopenable output device is fatal at startup (with an interactive recovery prompt if stdin is a terminal); an unopenable input device just leaves external input silent. |
| `--backend alsa\|pipewire` | `alsa` | Audio platform shim. `pipewire` requires a build where `libpipewire-0.3` was found. |
| `--pipewire-force-quantum` | off | Forces the whole PipeWire graph's buffer size down to match this engine's Latency control — affects every other PipeWire client on the system while running. |
| `--sfz-vco1..4 <path>` | none | Loads an SFZ instrument onto that oscillator at startup. |
| `--program <path.hdsprog>` | none | Loads a full Program at startup. |
| `--master-volume <0.0-1.0>` | none | Sets master volume once at startup. |
| `--gui` | off | Opens the native Cairo GUI as this process's own top-level window. |

Arrow keys work as live controls whenever stdin is a real terminal:
Up/Down nudge master volume, Left/Right cycle through saved Programs in
`~/HDS_Programs/`.

**`python3 src/frontend.py`:** `--osc-host`, `--osc-listen-port`,
`--osc-reply-port` — point it at a non-default backend (a remote machine,
or a specific CLAP instance's own OSC pair). Any other argument passes
straight through to Qt.

## FX slots

Up to 32 shared slots, each independently routable, each settable to any of:

- **Dynamics**: Gate, Limiter (brick-wall), FetComp (1176-style FET
  compressor), FairKid67 (Fairchild 670-style tube leveling amp), PeakMax
  (lookahead limiter/maximizer), OptoComp (LA-2A-style optical compressor)
  — all report live gain-reduction metering back to the GUI.
- **Tone**: EQ3 (3-band), MultiFilter (wide-range steep filter), Density3
  (highpass into a density/waveshaping saturator), PointyGuitar (amp-style
  distortion), GuitarConditioner, Cabs2 (speaker-cabinet tone shaping),
  DeEsser (split-band), HardTune (real-time pitch correction, with an
  optional LFO-routable vibrato).
- **Time-based**: Ensemble (fixed-delay chorus), CreamCoat (FDN reverb),
  FDNVerb (algorithmic reverb), StereoChorus, StereoDoubler (micro-pitch
  detune), TapeDelay2 (tape echo, with optional stereo ping-pong).
- **NAM**: Neural Amp Modeler — loads a trained `.nam` amp model plus an
  optional cabinet impulse response.

Several of the tone/time-based effects are ports of MIT-licensed
[Airwindows](https://www.airwindows.com/) plugins — see each `FX_TYPE_*`'s
own comment in `src/fx_types.h` for which one and any changes made porting it.

## Native GUI

`hds.clap` and `hds_fx.clap` each embed a real, host-native window (X11 on
Linux, Win32 on Windows) drawn with Cairo — no toolkit dependency, no
second process. `synth_engine` can open the same GUI as its own top-level
window via `--gui` (off by default, so a headless deployment is unaffected).

Tempo: a CLAP instance defaults to following the host's own transport
tempo (toggle to "Internal" in the Global sidebar's Tempo section to use
the Tempo knob/tempo-map instead — this choice is saved with the host
project like any other parameter).

Knobs: click-drag for a normal-sensitivity adjustment, double-click to
reset to 0, right-click-drag for a 10x finer adjustment. A knob that also
modulates something else (oscillator Pitch/Phase/Pulse-Width, Filter
Cutoff/Resonance, LFO Rate/Depth/Phase) opens a "Modulate From" menu on a
right-click that doesn't drag, letting any LFO or assignable EG target it
directly — the same field its own Destination dropdown already sets, just
a second way to reach it.

## Platform notes

- **Raspberry Pi / aarch64 Linux**: `make arm64` cross-compiles
  `synth_engine` (headless; no GUI target for this build) from an x86_64
  nix-shell.
- **Windows**: `hds-windows.clap` / `hds_fx-windows.clap` are cross-compiled
  with mingw-w64 and need `libmcfgthread-N.dll` alongside them (copied
  automatically by the build) — everything else statically links.
- **PipeWire**: auto-detected at build time via pkg-config; `make` prints
  which backend it found. Falls back to ALSA-only if PipeWire isn't present.

## Third-party code

Vendored under `third_party/`: the CLAP SDK, Eigen, and Neural Amp Modeler
Core (NAM). The circuit-modeled dynamics processors (FetComp, FairKid67,
OptoComp) and the Airwindows ports above are otherwise self-contained C
ports with no external dependency — see `src/fx_types.h` for sources.
