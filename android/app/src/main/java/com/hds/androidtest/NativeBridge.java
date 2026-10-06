package com.hds.androidtest;

import android.view.Surface;

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

    // GUI entry points -- see HdsSurfaceView.java's own header comment and hds_android_gui.c's
    // own header comment for the real architecture (straight into a locked ANativeWindow buffer
    // via Cairo, no Java Bitmap, no pixel copy of any kind -- these three calls carry nothing but
    // the Surface handle and raw touch coordinates/action across the JNI boundary).
    public static native void nativeSurfaceCreated(Surface surface);
    public static native void nativeSurfaceDestroyed();
    public static native void nativeTouchEvent(int action, int pointerCount, float x, float y);
}
