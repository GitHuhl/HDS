package com.hds.androidtest

// Thin JNI boundary -- matches hds_android.c's own `Java_com_hds_androidtest_NativeBridge_*`
// function names exactly (see that file's header comment). Carries no logic of its own on
// purpose: every real decision (MIDI decode, AAudio setup, the engine itself) lives in native
// code, same as this project's existing x11/win32 GUI shims carry no drawing logic of their own.
object NativeBridge {
    init {
        System.loadLibrary("hds_native")
    }

    external fun nativeInit(): Boolean
    external fun nativeOnMidi(data: ByteArray, offset: Int, length: Int)
    external fun nativeShutdown()
}
