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

## What's still unverified

Everything above was verified for real (real compiler, real linker, real `apksigner` signature
check, real `aapt2 dump badging` on the finished APK) in a sandboxed environment with no actual
Android device or emulator attached, so the one thing this hasn't confirmed is runtime behavior
on real hardware: does AAudio actually open the stream and produce audible sound, does
`android.media.midi` actually hand this app real bytes from a real USB keyboard, does the engine
hold up over an extended play session. That's exactly what installing the APK and plugging in a
keyboard answers.
