# HDS Android backend test

Answers exactly one question: does the real HDS synth engine (`backend.c`/`fx_chain.c`) produce
correct audio on a real phone from a real MIDI keyboard. No GUI, no OSC control, no Sequencer --
all deliberately deferred (see the top-level `README.md` for why the GUI in particular is a
separate decision from this).

## What's here

- `src/hds_android.c` (repo root, not under `android/`) -- the native glue. `#include`s
  `backend.c` directly, same single-translation-unit convention `clap_plugin.c`/`hds_fx_plugin.c`
  already use on other platforms. Owns two jobs: parse raw MIDI 1.0 bytes from Android's
  `android.media.midi` into `dispatch_midi_event()` calls, and run an AAudio output stream whose
  callback renders straight into AAudio's own buffer via `render_audio_block()` -- no Bitmap, no
  Java-side pixel or sample copy anywhere in the signal path.
- `android/app/src/main/cpp/CMakeLists.txt` -- builds that one file. Links `aaudio`/`log` only.
  `backend.c` pulls in `<lo/lo.h>` directly for its OSC control protocol (34 handler functions,
  ~70 call sites) that this build never actually uses (nothing here starts a server or registers
  a handler) -- rather than cross-building real liblo for `android-arm64` right now, which is one
  more variable this first test doesn't need, `stub/lo/lo.h` stands in for it: the real
  type/function shapes so `backend.c` compiles and links completely unmodified, every body an
  inert no-op. OSC is simply off for this build, not broken -- see that header's own comment for
  exactly how to swap in the real, cross-built liblo later, once basic audio+MIDI is verified on
  a real phone.
- `android/app/src/main/java/com/hds/androidtest/` -- `NativeBridge.kt` (the JNI boundary, no
  logic) and `MainActivity.kt` (scans for a MIDI device via `MidiManager`, auto-connects, forwards
  raw bytes to native code, shows a one-line status so you can tell it's alive without `adb
  logcat`). No layout XML, no custom view, no icon -- nothing here beyond what proves the backend
  works.

## What's actually verified vs. what's unverified

**Verified**: `src/hds_android.c` was syntax-checked with the host's own `gcc -fsyntax-only`
against the *real* `backend.c`/`fx_chain.c` (not a mockup) -- every type, struct field, and
function signature it calls (`render_audio_block`, `dispatch_midi_event`, `init_default_controls`,
`HdsSampleEngine`, etc.) is the actual one, confirmed to match. The same check, re-run with
`stub/lo/lo.h` on the include path ahead of the host's own real liblo-dev headers (confirmed via
`gcc -H`, not assumed), compiles clean too -- all 71 real `lo_*` call sites in `backend.c` resolve
against the stub's signatures with zero warnings under `-Wall -Wextra`. The JNI/AAudio calls
themselves were checked against hand-written stub headers with real NDK signatures (not
invented), stored at `/tmp/.../scratchpad/ndk_stub/` for this session -- a genuine type-check, but
it cannot catch anything the real NDK headers would (ABI details, deprecated-signature changes
between NDK versions, etc.).

**Unverified, because this sandbox cannot reach it**: the Gradle/Kotlin project was never built.
This session's egress policy blocks `dl.google.com` (403, "policy denial") -- the only place the
Android SDK platforms and NDK are distributed from -- and per this session's own proxy rules, a
blocked host gets reported, not routed around with a third-party mirror. No `sdkmanager`, no AGP
sync, no `gradlew` run, no APK, has happened or could happen here.

## To actually build this

On any machine with Android Studio (or just the command-line SDK + NDK) installed, and normal
network access:

```
cd android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

No other steps needed first -- liblo is stubbed out (see above), so this is the whole build.

If this sandbox's own network policy can be configured to allow `dl.google.com` for a future
session (see this session's environment docs for how environments configure egress), the build
can instead finish right here, no other machine needed.

## Testing it on your Pixel 8a

No Play Console signup, no developer verification -- `adb install` (or copying the APK over and
opening it) is a plain sideload, and GrapheneOS's installer doesn't defer to Google Play
Services' Play Integrity gate the way stock Android now does. Plug in a USB MIDI keyboard (a
USB-C-to-USB adapter if yours is USB-A), launch the app, and it auto-connects to the first MIDI
device it sees -- the status line will say so, and show a live byte count as you play.
