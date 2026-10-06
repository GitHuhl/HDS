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
# Requires: nix-build (this repo's own shell.nix already assumes Nix; see its own header comment
# for the pinned nixpkgs-24.05 release android/nix/android-sdk.nix also pins). Everything else
# (aapt2, javac, d8, apksigner, zipalign, the NDK clang, android.jar) comes from that one Nix
# build -- no system Android SDK/NDK install needed at all.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT/android"
WORK="$ANDROID_DIR/app/build/manual"  # gitignored, same as every other build/ dir in this repo
PACKAGE="com.hds.androidtest"
MIN_API=26
TARGET_API=34
BUILD_TOOLS_VERSION=34.0.0
NDK_VERSION=26.1.10909125

echo "=== Fetching Android SDK + NDK via Nix (cache.nixos.org, not dl.google.com) ==="
SDK_ROOT="$(NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE=1 NIXPKGS_ALLOW_UNFREE=1 \
  nix-build --no-out-link "$ANDROID_DIR/nix/android-sdk.nix")/libexec/android-sdk"
BUILD_TOOLS="$SDK_ROOT/build-tools/$BUILD_TOOLS_VERSION"
ANDROID_JAR="$SDK_ROOT/platforms/android-$TARGET_API/android.jar"
NDK_ROOT="$SDK_ROOT/ndk/$NDK_VERSION"
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
"$NDK_CLANG" -shared -o "$WORK/lib/arm64-v8a/libhds_native.so" \
  "$WORK/hds_android.o" -llog -laaudio

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
