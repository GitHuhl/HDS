#!/usr/bin/env bash
# tools/build_android_manual.sh: builds a real, installable, signed APK for the Android backend
# test (android/README.md) WITHOUT Gradle/AGP -- aapt2/javac/d8/apksigner/zipalign driven by hand
# instead. Why: this project's own Gradle project (android/build.gradle, settings.gradle) can't
# resolve the com.android.application plugin or any AndroidX artifact in a network-restricted
# sandbox, because Gradle's `google()` repository (and, confirmed by testing, maven.google.com
# too -- every path on that host 301-redirects back to dl.google.com) ultimately bottoms out at
# the one host such a sandbox's egress policy blocks. The SDK/NDK themselves are NOT blocked:
# android/nix/android-sdk.nix fetches them through Nix's own substituters (cache.nixos.org),
# confirmed to need zero direct contact with dl.google.com (checked via the proxy's own failure
# log, not assumed) -- this script drives that real SDK/NDK directly with aapt2/javac/d8/
# apksigner, which is just as real an APK as Gradle would produce, only Gradle itself skipped.
#
# On a machine with normal network access and Android Studio, you don't need this at all --
# `cd android && ./gradlew assembleDebug` after `gradle wrapper` (see android/README.md) is the
# normal path and will work fine there, where google()/maven.google.com genuinely resolve.
#
# Preferred entry point: `make android` (from the repo root), which runs inside `nix-shell` --
# shell.nix's own androidSdk binding already exports ANDROID_SDK_ROOT/ANDROID_NDK_ROOT and puts
# aapt2/d8/apksigner/zipalign and the NDK clang on PATH for every platform target the same way, so
# this script just uses them directly when already set. Run directly without `nix-shell` (e.g.
# `tools/build_android_manual.sh` on its own), it falls back to calling `nix-build` itself against
# android/nix/android-sdk.nix -- slower (re-resolves the SDK/NDK derivation instead of reusing an
# already-entered shell's), but self-sufficient. Either way: aapt2, javac, d8, apksigner, zipalign,
# the NDK clang, and android.jar all come from that one real SDK/NDK -- no system Android SDK/NDK
# install needed at all.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT/android"
WORK="$ANDROID_DIR/app/build/manual"  # gitignored, same as every other build/ dir in this repo
PACKAGE="com.hds.androidtest"
MIN_API=26
TARGET_API=34
BUILD_TOOLS_VERSION=34.0.0
NDK_VERSION=26.1.10909125

if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT" ]; then
  echo "=== Using the Android SDK/NDK already on PATH (ANDROID_SDK_ROOT set -- inside nix-shell) ==="
  SDK_ROOT="$ANDROID_SDK_ROOT"
  NDK_ROOT="${ANDROID_NDK_ROOT:-$SDK_ROOT/ndk/$NDK_VERSION}"
else
  echo "=== ANDROID_SDK_ROOT not set -- not inside nix-shell. Falling back to a standalone"
  echo "    nix-build (prefer \`make android\` or \`nix-shell\` first; see this script's own"
  echo "    header comment for why) ==="
  SDK_ROOT="$(NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE=1 NIXPKGS_ALLOW_UNFREE=1 \
    nix-build --no-out-link "$ANDROID_DIR/nix/android-sdk.nix")/libexec/android-sdk"
  NDK_ROOT="$SDK_ROOT/ndk/$NDK_VERSION"
fi
BUILD_TOOLS="$SDK_ROOT/build-tools/$BUILD_TOOLS_VERSION"
ANDROID_JAR="$SDK_ROOT/platforms/android-$TARGET_API/android.jar"
NDK_CLANG="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${MIN_API}-clang"
echo "SDK: $SDK_ROOT"
echo "NDK clang: $NDK_CLANG"

rm -rf "$WORK"
mkdir -p "$WORK/classes" "$WORK/lib/arm64-v8a"

echo "=== Compiling the native engine (hds_android.c -> libhds_native.so) ==="
# -fPIC: Android .so's must be position-independent (missed on the first attempt -- the linker
# error that produced is a clear, unambiguous signal if this is ever dropped again).
"$NDK_CLANG" -c -std=gnu11 -O2 -fPIC -Wall -Wextra \
  -I "$ANDROID_DIR/app/src/main/cpp/stub" \
  "$ROOT/src/hds_android.c" -o "$WORK/hds_android.o"
#
# -lm: required even though it LOOKS like the build-time link succeeds without it -- the NDK's
# stub libm.so/libc.so both declare exp2f/log2f/etc. (used by HardTune/DBX120X's glide and pitch
# math) at build time via Bionic's symbol-versioning scheme, so the linker resolves the reference
# either way with no error or warning. The real libm.so/libc.so split only exists on-DEVICE, and
# without -lm here there's no DT_NEEDED libm.so entry in the finished .so -- confirmed via
# `readelf -d`, and confirmed as the real bug via a real on-device crash log:
# "UnsatisfiedLinkError: dlopen failed: cannot locate symbol exp2f". Link-time success on this
# host proves nothing about runtime symbol resolution on an actual phone -- this is exactly why.
"$NDK_CLANG" -shared -o "$WORK/lib/arm64-v8a/libhds_native.so" \
  "$WORK/hds_android.o" -llog -laaudio -lm

echo "=== Compiling the Java UI (javac, source/target 8 -- see MainActivity.java's own header
     comment for why: a non-static inner/anonymous class here crashes this SDK's bundled D8) ==="
javac -source 8 -target 8 -nowarn -cp "$ANDROID_JAR" \
  -d "$WORK/classes" \
  "$ANDROID_DIR/app/src/main/java/com/hds/androidtest/MainActivity.java" \
  "$ANDROID_DIR/app/src/main/java/com/hds/androidtest/NativeBridge.java"

echo "=== Dexing ==="
"$BUILD_TOOLS/d8" --output "$WORK" --min-api "$MIN_API" \
  $(find "$WORK/classes" -name "*.class")

echo "=== Linking resources + manifest (aapt2) ==="
"$BUILD_TOOLS/aapt2" link \
  -o "$WORK/base.apk" \
  -I "$ANDROID_JAR" \
  --manifest "$ANDROID_DIR/app/src/main/AndroidManifest.xml" \
  --min-sdk-version "$MIN_API" \
  --target-sdk-version "$TARGET_API"

echo "=== Assembling the APK (base resources + classes.dex + native lib) ==="
cp "$WORK/base.apk" "$WORK/unsigned.apk"
( cd "$WORK" && zip -q unsigned.apk classes.dex lib/arm64-v8a/libhds_native.so )

echo "=== Aligning + signing (debug key, generated once and reused) ==="
if [ ! -f "$WORK/debug.keystore" ]; then
  keytool -genkeypair -v \
    -keystore "$WORK/debug.keystore" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US"
fi
"$BUILD_TOOLS/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
"$BUILD_TOOLS/apksigner" sign \
  --ks "$WORK/debug.keystore" --ks-pass pass:android --key-pass pass:android \
  --out "$WORK/hds-backend-test.apk" \
  "$WORK/aligned.apk"

echo "=== Verifying ==="
"$BUILD_TOOLS/apksigner" verify -v "$WORK/hds-backend-test.apk"
"$BUILD_TOOLS/aapt2" dump badging "$WORK/hds-backend-test.apk" | grep -E "^package:|launchable-activity|native-code"

echo
echo "Built: $WORK/hds-backend-test.apk"
echo "Install with: adb install -r '$WORK/hds-backend-test.apk'"
