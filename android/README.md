# HDS Android backend test

Answers one question: does the real HDS synth engine (`backend.c`/`fx_chain.c`) produce correct
audio on a real phone from a real MIDI keyboard. No GUI, no OSC control, no Sequencer -- all
deliberately deferred (see the top-level `README.md` for why the GUI in particular is a separate
decision from this).

**A real, signed, installable APK now exists.** Build it with:

```
nix-shell
make android
```

from the repo root -- same entry point every other platform target in this project uses (see
`shell.nix`'s own header comment). `nix-shell` puts the Android SDK/NDK on `PATH`
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
does. Plug in a USB MIDI keyboard (a USB-C-to-USB adapter if yours is USB-A), launch "HDS Backend
Test", and it auto-connects to the first MIDI device it sees -- the on-screen status line shows
the device name and a live MIDI byte count as you play.

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

While fixing this, two more real latency/glitch-resistance gaps got closed, not just the one bug
that was reported: the stream now tries `AAUDIO_SHARING_MODE_EXCLUSIVE` first (the real dedicated
hardware path most devices either grant immediately or refuse outright) and falls back to
`SHARED` only if that's refused; and the buffer size is now explicitly grown to 2x
`AAudioStream_getFramesPerBurst()` after opening (AAudio's own bare default leaves no headroom at
all, and 2x bursts of slack is the standard Android audio latency guidance for a stream seeing
underruns). `nativeInit()` now logs the sharing mode/performance mode/buffer size it actually got
(`adb logcat -s hds_native`), and the audio callback logs `AAudioStream_getXRunCount()` roughly
once a second -- a real, always-available signal for whether the device itself is still
glitching, worth checking first before assuming any further dropout report is a new bug rather
than this same tradeoff needing to move further.

## What's still unverified

Everything above was verified for real (real compiler, real linker, real `apksigner` signature
check, real `aapt2 dump badging` on the finished APK, and now a real zero-undefined-symbols check
via `nm -D -u`) in a sandboxed environment with no actual Android device or emulator attached, so
the one thing none of this can confirm is runtime behavior itself: does AAudio actually open the
stream and produce audible sound, does `android.media.midi` actually hand this app real bytes
from a real USB keyboard, does the engine hold up over an extended play session. That's exactly
what installing the APK and plugging in a keyboard answers -- and it's also exactly how the two
bugs above were found, since neither a missing `DT_NEEDED` entry nor a missing symbol is
something any of the build-time checks here can catch on their own.
