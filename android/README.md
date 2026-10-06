# HDS Android backend test

Answers one question: does the real HDS synth engine (`backend.c`/`fx_chain.c`) produce correct
audio on a real phone from a real MIDI keyboard. No OSC control, no Sequencer -- still
deliberately deferred. The GUI itself is NO LONGER deferred -- see "The native GUI" below -- this
is now the real, same `hds_clap_gui_draw.c` tab bar hds.clap/the standalone app already have,
landscape-only, drawn straight into the phone's own screen.

**A real, signed, installable APK now exists.** First, ONCE per checkout, cross-build the GUI's
own Cairo/FreeType/pixman (not needed before the GUI existed -- see "The native GUI" below):

```
nix-shell
tools/build_android_cairo_deps.sh
```

Then, same entry point every other platform target in this project uses (see `shell.nix`'s own
header comment):

```
make android
```

from the repo root. `nix-shell` puts the Android SDK/NDK on `PATH`
(`ANDROID_SDK_ROOT`/`ANDROID_NDK_ROOT`, see `shell.nix`'s `androidSdk` binding) the same way it
already does for the Windows/arm64 cross toolchains; `make android` then runs
`tools/build_android_manual.sh` against them. (The script also works run directly, without
`nix-shell` first -- it falls back to calling `nix-build` itself -- just slower, since it
re-resolves the SDK/NDK derivation instead of reusing an already-entered shell's.) Either way
this produces `android/app/build/manual/hds-backend-test.apk`. Install it:

```
adb install -r android/app/build/manual/hds-backend-test.apk
```

No Play Console signup, no developer verification -- this is a plain sideload, and GrapheneOS's
installer doesn't defer to Google Play Services' Play Integrity gate the way stock Android now
does. Plug in a USB MIDI keyboard (a USB-C-to-USB adapter if yours is USB-A), launch "HDS", and
the real tab bar (Play/Filters/LFOs/Envelopes/FX/Routing/Devices/Global) fills the whole screen,
locked to landscape -- play, tap/drag knobs and pills exactly like the desktop build, swipe
vertically to scroll a tab past the screen's own bottom edge. MIDI connection status (device
name, hotplug) now goes to `adb logcat -s HdsMidi` instead of an on-screen line -- the real GUI
has nowhere of its own to show it, and the old status line was only ever there because this build
had no GUI yet.

## Why there's a build script instead of `./gradlew assembleDebug`

There's a real Gradle project here (`android/build.gradle`, `settings.gradle`,
`app/build.gradle`) and it's the normal way to build this on a machine with ordinary network
access -- `cd android && gradle wrapper --gradle-version 8.7 && ./gradlew assembleDebug` will
work there. It does NOT work in whatever sandbox this was first built in, because Gradle's
`google()` repository (and `maven.google.com`, tried as a substitute) both ultimately resolve to
`dl.google.com`, which that sandbox's egress policy blocks -- confirmed with `curl -v`, not
assumed: every path on `maven.google.com` 301-redirects straight back to the blocked host.

The Android SDK and NDK themselves are a different story -- see the next section -- so
`tools/build_android_manual.sh` drives `aapt2`/`javac`/`d8`/`apksigner`/`zipalign` directly
against that real SDK/NDK, skipping Gradle/AGP (and the dependency resolution it needs)
entirely. It produces a genuinely equivalent APK to what Gradle would, just assembled by hand.

## How the SDK/NDK get here without `dl.google.com`

`android/nix/android-sdk.nix` asks Nix (via `nixpkgs`' `androidenv`) for a real Android SDK
(platform-tools, build-tools 34.0.0, platform 34) plus NDK 26.1.10909125. Despite the Android SDK
being licensed "unfree" (which normally means Nix's own build farm won't build or cache it),
someone has already pushed these exact package hashes to `cache.nixos.org` -- so `nix-build`
pulls a complete, real NDK (2.1GB, confirmed: real `clang`, real `aarch64-linux-android` sysroot
with `libc.so` for multiple API levels) entirely from Nix's substituters. Checked directly, not
assumed: the proxy's own connection-failure log shows zero new attempts to reach `dl.google.com`
across this entire build.

## What's real here vs. what's a workaround

**Real, unmodified**: `src/hds_android.c` (the native glue: AAudio output callback calling
`render_audio_block()` directly into AAudio's own buffer, a hand-written raw MIDI 1.0 parser
feeding `dispatch_midi_event()`) compiles and links cleanly with the actual NDK clang --
confirmed via `file`/`llvm-nm`, which show a real ARM64 shared object exporting exactly the three
JNI symbols (`nativeInit`/`nativeOnMidi`/`nativeShutdown`) Java expects. `backend.c` itself is
untouched.

**A real workaround, not a hack that skips anything**: `backend.c` pulls in `<lo/lo.h>` directly
for its OSC control protocol (34 handler functions, ~70 call sites) that this build never
actually reaches (nothing here starts a server or registers a handler). `app/src/main/cpp/stub/
lo/lo.h` stands in for the real liblo: real type/function shapes, so `backend.c` compiles and
links completely unmodified, every body an inert no-op. OSC is off, not broken; see that header's
own comment for how to swap in a real, cross-built liblo later.

**Plain Java, not Kotlin**: `MainActivity.java`/`NativeBridge.java`, not `.kt` -- avoids needing
`kotlinc` at all (not downloadable here either) on top of everything else. A real, separate
compatibility bug was found and fixed along the way: this sandbox's bundled D8 (part of the
Nix-provided build-tools, `R8 8.2.2-dev`) crashes with an internal `NullPointerException` on any
**non-static inner class** compiled by a modern (JDK 21) `javac` -- confirmed by testing multiple
theories (it wasn't lambdas, and it wasn't anonymous-vs-named classes; a *named* non-static inner
class crashed it too, and making the exact same class `static` fixed it). Every nested class in
`MainActivity.java` is `static`, taking the activity as an explicit constructor parameter instead
of capturing it implicitly, specifically to avoid this. `tools/build_android_manual.sh` also
compiles with `-source 8 -target 8`, which avoids a separate, unrelated `javac`/`d8` version
mismatch. Building the real Gradle project on another machine, with a properly AGP-matched D8,
may not need either workaround -- see `app/build.gradle`'s own comment.

**A real on-device bug, found from an actual crash report, not predicted**: the very first
installed build crashed immediately with `UnsatisfiedLinkError: dlopen failed: cannot locate
symbol "exp2f"` (used by HardTune/DBX120X's own pitch-glide math). Root cause, confirmed via
`readelf -d` on the built `.so`: the link command only passed `-llog -laaudio`, never `-lm`, so
the finished library had no `NEEDED libm.so` entry at all. The build-time link still "succeeded"
with no warning either way, because the NDK's own stub `libm.so`/`libc.so` both declare `exp2f`
via Bionic's symbol-versioning scheme -- only the real split on an actual device exposed the
missing dependency. Fixed by adding `-lm`; see `tools/build_android_manual.sh`'s own comment on
that exact line for the full story. The general lesson this leaves behind: this sandbox can
verify a lot (real compiler, real linker, real signature check) but never "does it actually run
on a phone" -- that step stays genuinely necessary, and finding a real bug through it is the
expected, correct outcome of doing it, not a sign anything upstream was skipped.

**A second real on-device bug, this time found by checking rather than waiting for the next
crash**: after the `-lm` fix above, rather than ship another APK and wait for whatever the next
missing symbol happened to be, every undefined dynamic symbol in the finished `.so` was listed
directly (`nm -D -u`) and checked one by one against what's actually linked. 51 of them were
libc/libm/liblog/libaaudio symbols (all genuinely satisfied -- confirmed, not assumed, by
`nm -D` on the NDK's own stub libraries for a couple of the less obvious ones). Three were not:
`hds_nam_create`/`hds_nam_process`/`hds_nam_destroy`, the C API over NeuralAmpModelerCore
(`nam_bridge.h`/`.cpp`) that `backend.c`'s NAM FX type calls into -- never compiled or linked for
Android at all, since the real implementation needs a cross-built C++/Eigen toolchain this build
doesn't have. Unlike liblo, `nam_bridge.h` is pulled in via `#include "nam_bridge.h"` (a
quote-include), which always resolves to the real header regardless of any `-I` path, so the
header-shadowing trick `stub/lo/lo.h` uses doesn't apply here -- but the real header is pure
`extern "C"` declarations with no C++ types exposed, so it's already safe to include as-is.
`app/src/main/cpp/stub/nam_bridge_stub.c` instead provides the three function *bodies*, compiled
as an ordinary second C source file: `hds_nam_create()` always returns `NULL`, which is already
documented as the real contract's own "any failure, including an unreadable file" case, not a
fabricated success -- NAM simply never has a model loaded on this build. Every other FX type
(Gate, EQ3, Limiter, HT76, Fairchild670, PeakMax, LA2A, the multi-slope filter, DeEsser, HardTune,
FDNVerb, DBX120X, BroadcastDelay) is pure C and fully functional.

**Stuttering/dropouts reported with 3+ notes held in each hand**: a real performance bug, found
directly in the code rather than guessed at. The actual root cause wasn't a missing AAudio
performance-mode flag (`AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` was already requested from the
start) -- it was a `pthread_mutex` shared between the AAudio realtime callback thread and
Android's own MIDI Binder thread, guarding every call into the engine. Holding any lock inside a
realtime audio callback is a textbook priority-inversion hazard: if the MIDI thread is holding it
-- however briefly -- exactly when the callback fires, the callback stalls waiting, eating
directly into its hard deadline. That matches the reported symptom precisely: a chord is several
Note On messages arriving on the MIDI thread within the same few milliseconds, each one taking
the lock. Fixed by replacing the mutex with a true lock-free single-producer/single-consumer
ring buffer (`hds_midi_ring_push`/`hds_midi_ring_drain_and_dispatch` in `src/hds_android.c`): the
MIDI thread only ever writes `head`, the audio thread only ever writes `tail`, each side only
ever reads the other's index, and the audio thread drains the ring (applying every queued event)
at the top of its own callback, before `render_audio_block()` -- so neither thread can ever block
on the other.

While fixing this, two AAudio tuning changes were tried alongside it -- and real on-device
testing immediately showed BOTH made things worse, not better, which is exactly why they're
documented here as reverted rather than left in: first, trying `AAUDIO_SHARING_MODE_EXCLUSIVE`
before falling back to `SHARED` turned "stutter only under heavy chords" into constant
dropouts/crunch -- reverted back to `SHARED` only, which is the mode that was actually confirmed
working. Second, explicitly setting the buffer to a fixed `2x AAudioStream_getFramesPerBurst()`
(the standard Android audio latency guidance for a stream seeing underruns) made things worse
too, almost certainly because that fixed value happened to be SMALLER than whatever buffer size
AAudio's own default had already picked -- silently shrinking the one thing that was already
working, not growing it. Replaced with `AAudioStream_getBufferCapacityInFrames()` (the real
ceiling the stream was opened with) instead of a guessed multiplier: this maximizes buffer
headroom rather than picking a specific tradeoff point, the right call for a build whose purpose
is proving basic correctness, not chasing the lowest possible latency. `nativeInit()` logs the
sharing mode/performance mode/burst size/buffer size it actually got
(`adb logcat -s hds_native`), and the audio callback logs `AAudioStream_getXRunCount()` roughly
once a second -- a real, always-available signal for whether the device itself is still
glitching, worth checking first before assuming any further dropout report is a new bug rather
than this same tradeoff needing to move further.

## The native GUI

The real tab bar (`src/hds_clap_gui_draw.c` -- the SAME file hds.clap and the standalone app
already embed) now fills the whole screen, landscape-only. Three real, non-obvious problems were
found and fixed getting there, each confirmed directly rather than assumed:

**Architecture: direct-buffer Cairo, no Java Bitmap, no pixel copy.** `src/hds_android_gui.c`
locks the `SurfaceView`'s own `ANativeWindow` buffer (`ANativeWindow_lock`), wraps that EXACT
memory in a `cairo_image_surface_t` (`cairo_image_surface_create_for_data`), calls
`hds_clap_gui_render()` straight into it, then posts (`ANativeWindow_unlockAndPost`). There is no
intermediate surface of this file's own, no Java-side `Bitmap`, and nothing ever crosses the JNI
boundary as pixel data -- only a `Surface` handle (once, at `surfaceCreated`) and raw touch
coordinates. This is deliberate, not incidental: an earlier "render to a bitmap, blit into a Java
`Bitmap`" sketch was explicitly rejected as the wrong shape before any of this was written.

**A real, confirmed pixel-format mismatch, not a hypothetical one.** Cairo's `CAIRO_FORMAT_ARGB32`
is `B,G,R,A` in memory on a little-endian device (confirmed earlier in this port's own research,
via `test_noFontconfig.c`'s own by-hand PPM writer, which has to permute B and R explicitly for
exactly this reason) -- Android's `ANativeWindow` `RGBA_8888`/`RGBX_8888` formats are `R,G,B,A/X`
(Android's own documented `Bitmap.Config.ARGB_8888` byte order, the same one OpenGL's
`GL_RGBA`/`GL_UNSIGNED_BYTE` uses). There is no public `ANativeWindow` format matching Cairo's own
byte order, so `hds_android_gui_redraw()` does one in-place R<->B byte swap over the SAME locked
buffer Cairo just drew into, right before posting -- the one unavoidable translation between two
pixel-format CONVENTIONS, not a second render pass or a copy to new memory.

**Touch input via the same lock-free ring the MIDI path already proved.** Touch callbacks arrive
on the UI thread; this window's own redraw happens on its own dedicated thread (spawned at
`surfaceCreated`, joined at `surfaceDestroyed` -- mirrors `hds_standalone_gui_x11.c`'s own
create/destroy ordering exactly). Rather than inventing new cross-thread synchronization, touch
events are queued through an SPSC ring shaped identically to `hds_android.c`'s own
`g_midi_ring`/`hds_midi_ring_push`/`_drain_and_dispatch` -- same proven pattern, new purpose.
Swipe-to-scroll reuses the existing `hds_clap_gui_on_scroll(..., +-1)` entry point at a fixed
24px-per-"click" threshold, firing only when `hds_android_gui_knob_drag_active()` says no tab's
own knob-drag (or the FX tab's own `drag_kind`) is currently in flight -- a small, tab-aware check
reading each tab's own already-public view-state field, not a new field added to any of them.

**Two real build-time bugs, found by actually compiling, not assumed fixed.** (1)
`hds_engine_instance.h`'s shared `HdsClapPlugin` struct has an `lo_server osc_server` field that
`stub/lo/lo.h` had no type for at all (only `lo_address`/`lo_message`/`lo_bundle` -- nothing in
`hds_android.c` ever needed `lo_server` before the GUI's own shared-context type pulled that
header in) -- fixed with one more inert `typedef void *lo_server;`, never actually constructed
with a real value, same posture as every other symbol that stub already covers. (2) This
codebase's own draw files are inconsistent about `#include <cairo.h>` (`hds_clap_adsr.h`) vs.
`#include <cairo/cairo.h>` (`hds_fx_gui_draw.c`) -- the native desktop build gets away with only
the latter needing an explicit `-I` (confirmed directly: an apt-installed `libcairo2-dev` on THIS
particular build host happens to put `cairo.h` on gcc's own default `/usr/include` search path,
which the bare form resolves against purely by accident, not because the Makefile's own
`pkg-config --cflags cairo` output is actually sufficient on its own), but the NDK's cross `clang`
has no host-system fallback of any kind -- fixed by passing BOTH
`third_party/android-deps/include` and `.../include/cairo` as separate `-I` flags, same two-path
fix `WIN_CFLAGS` already uses in the Makefile for the identical dual-style situation on the
Windows cross-build.

`tools/build_android_cairo_deps.sh` (new, mirrors `tools/build_windows_deps.sh`) cross-builds
FreeType/pixman/Cairo for this same NDK target via meson -- FreeType enabled, Fontconfig
explicitly disabled (Cairo's own toy-font backend, `cairo_select_font_face()`, is wired to `NULL`
without Fontconfig, confirmed directly in Cairo's own source; this GUI never calls it at all --
see `hds_fx_gui_draw.c`'s own `hds_font_select_face()` macro substitution and
`hds_android_gui.c`'s own `cairo_ft_font_face_create_for_ft_face()` call instead). Includes the
one real, pre-existing (not Android-specific) Cairo build-script bug this port's own earlier
research found: `src/cairo-ft-private.h` references the `FT_Color` type under an
`HAVE_FT_COLR_V1` guard that's set whenever FreeType exports `FT_Palette_Select` (every FreeType
>= 2.10), while the type itself is only pulled in under a different, unrelated
`HAVE_FT_SVG_DOCUMENT` guard a few lines later -- patched with one `#include FT_COLOR_H`, applied
idempotently (checked before inserting, so re-running the script is safe). Needs
`shell.nix`'s new `HDS_ANDROID_{PIXMAN,FREETYPE,CAIRO}_SRC` exports (same already-hash-pinned
nixpkgs-tarball posture `HDS_WIN_{PIXMAN,CAIRO}_SRC` already has for the Windows build) -- run
once per checkout, output lives in `third_party/android-deps/` (gitignored, same as
`third_party/win64-deps/`).

The two DejaVu Sans fonts (`third_party/dejavu-fonts/`, vendored with their own `LICENSE`) are
`.incbin`'d directly into `libhds_native.so`'s own `.rodata` by `embed_font.S` -- no
`AAssetManager`/JNI asset-passing plumbing at all; `hds_android_gui.c` loads them via plain
in-memory FreeType (`FT_New_Memory_Face` against the `.incbin` symbols' own start/end addresses).
Confirmed for real: a standalone NDK `clang` assemble of `embed_font.S` produces exactly the
expected symbol offsets (0 and each font file's own real byte size).

**What this does NOT attempt**: the shared file browser (`hds_fx_gui_draw.c`'s own `HdsFxBrowser`
-- NAM model/IR pickers, the Multisample `.sfz` loader, the Global "Load Scale" picker) is
included and reachable exactly as on desktop, but nothing here has looked at whether its plain
POSIX directory listing (`opendir`/`readdir`) actually reaches anything meaningful under
Android's own scoped-storage rules on a real device -- a real, separate question for whenever
loading a file on-device is actually tried, not assumed to already work. Physical-keyboard input
(`hds_clap_gui_on_key()`) is also not wired to anything -- there's no on-screen keyboard call site
anywhere in this GUI that would need it, same as every other tab.

## What's still unverified

Everything above was verified for real (real compiler, real linker, real `apksigner` signature
check, real `aapt2 dump badging` on the finished APK, and a real zero-undefined-symbols check via
`nm -D -u`, now covering the GUI's own cairo/freetype/pixman/`libandroid` dependencies too) in a
sandboxed environment with no actual Android device or emulator attached, so the one thing none
of this can confirm is runtime behavior itself: does AAudio actually open the stream and produce
audible sound, does `android.media.midi` actually hand this app real bytes from a real USB
keyboard, does the engine hold up over an extended play session, and now also: does the GUI
actually render correctly on a real screen (in particular, is the R<->B byte-swap fix above
actually right, and does touch/swipe feel reasonable) -- the color-channel-order claim above was
derived from this port's own earlier `test_noFontconfig.c` research plus Android's own documented
pixel format, not confirmed against an actual rendered frame on an actual device. That's exactly
what installing the APK answers -- and it's also exactly how every bug documented on this page
was found, since none of them are something a build-time check alone can catch.
