package com.hds.androidtest;

// Thin JNI boundary -- matches hds_android.c's own `Java_com_hds_androidtest_NativeBridge_*`
// function names exactly (see that file's header comment). Carries no logic of its own on
// purpose: every real decision (MIDI decode, AAudio setup, the engine itself) lives in native
// code, same as this project's existing x11/win32 GUI shims carry no drawing logic of their own.
//
// Plain Java, not Kotlin: this build is assembled by hand with aapt2/d8/apksigner (no Gradle/AGP
// -- see android/README.md for why), and javac needs no extra compiler download the way kotlinc
// would. The original Kotlin source is preserved in git history if Gradle/AGP ever becomes usable
// here and this moves back to a real Gradle build.
public final class NativeBridge {
    static {
        System.loadLibrary("hds_native");
    }

    private NativeBridge() {}

    public static native boolean nativeInit();
    public static native void nativeOnMidi(byte[] data, int offset, int length);
    public static native void nativeShutdown();
}
