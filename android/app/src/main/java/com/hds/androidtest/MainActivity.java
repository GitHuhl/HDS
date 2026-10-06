package com.hds.androidtest;

import android.app.Activity;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicLong;

// The real native GUI (src/hds_clap_gui_draw.c, drawn straight into this Activity's one
// HdsSurfaceView -- see that class's own header comment and hds_android_gui.c's own header
// comment for the full architecture) now fills the whole screen; no layout XML, no other Java
// widget of any kind. MIDI connection status, which used to be this Activity's only visible
// content back when this build was a headless backend-only test (see android/README.md), now
// just goes to logcat (Log.i, tag "HdsMidi") instead of a TextView -- the real GUI has nowhere
// of its own to show it, and `adb logcat -s HdsMidi` already tells a developer everything the
// old on-screen status line did.
//
// Plain Java, not Kotlin -- see NativeBridge.java's own header comment for why. Every nested
// class below is `static`, taking `MainActivity activity` as an explicit constructor parameter
// instead of capturing it implicitly -- a plain (non-static) inner class here crashed this
// sandbox's bundled D8 (R8 8.2.2-dev) with an internal NPE while parsing that class's synthetic
// outer-instance constructor parameter, confirmed by testing (an anonymous-class version hit the
// same crash; switching those to named-but-still-inner classes didn't help; only dropping the
// implicit outer reference entirely, via `static`, did). If a real Gradle/AGP build ever becomes
// possible here with a newer, non-broken D8/R8, this restriction can be dropped.
public class MainActivity extends Activity {
    private static final String TAG = "HdsMidi";
    private MidiDevice openDevice;
    private final AtomicLong bytesReceived = new AtomicLong(0);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // This is now a real playable instrument, not a backend test harness someone glances at
        // occasionally -- the screen must never sleep mid-session on its own, same expectation
        // any other instrument/DAW app sets. AndroidManifest.xml's own .NoActionBar.Fullscreen
        // theme already drops the ActionBar and status bar at the window level; hideSystemUi()
        // below additionally hides the navigation bar (which that theme alone does not touch) and
        // is re-applied on every focus regain, since swiping in from a screen edge (or a system
        // dialog taking focus) temporarily brings system bars back regardless of this flag.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemUi();
        allowDrawingUnderDisplayCutout();

        setContentView(new HdsSurfaceView(this));

        boolean audioOk = NativeBridge.nativeInit();
        Log.i(TAG, audioOk ? "Audio engine running. Scanning for MIDI devices..."
                           : "AAudio stream failed to open -- check logcat for \"hds_native\".");

        scanAndConnectMidi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    // View.SYSTEM_UI_FLAG_* (not the newer WindowInsetsController, API 30+): deprecated since API
    // 30 but still fully functional through the current target API, and this build's own minSdk
    // (26, see tools/build_android_manual.sh's own MIN_API) would otherwise need two separate
    // code paths for the same one-time immersive-mode setup -- not worth it for what this does.
    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    // Without this, a device with a notch/camera cutout reserves that area as dead space even in
    // a fullscreen window -- confirmed on a real device: a black region the real GUI's own
    // hds_android_gui_redraw() never gets drawn into at all, because ANativeWindow's own buffer
    // dimensions simply exclude it, not because the native scale-to-fill math is wrong.
    // LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES (not _ALWAYS) is the standard choice for an app
    // that is ALWAYS fullscreen like this one (see hideSystemUi() above) -- it lets content draw
    // under the cutout specifically while in that state, which this Activity never leaves.
    // API 28+ only (Build.VERSION_CODES.P) -- this build's own minSdk is 26 (see
    // tools/build_android_manual.sh's own MIN_API), and the flag plus its whole LayoutParams
    // field don't exist before P at all; a device below API 28 simply keeps the cutout
    // letterboxed, same as it always was.
    private void allowDrawingUnderDisplayCutout() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attrs = getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attrs);
        }
    }

    private void scanAndConnectMidi() {
        MidiManager midiManager = (MidiManager) getSystemService(MIDI_SERVICE);
        if (midiManager == null) {
            Log.i(TAG, "No MidiManager on this device (android.software.midi feature missing).");
            return;
        }

        // Connect to whatever's already plugged in, then keep listening for hotplug -- a USB MIDI
        // keyboard attached after the app launches is the realistic test case, not just "already
        // connected at boot".
        for (MidiDeviceInfo info : midiManager.getDevices()) {
            tryOpenDevice(midiManager, info);
        }
        midiManager.registerDeviceCallback(new HotplugCallback(this, midiManager), null);
    }

    void tryOpenDevice(MidiManager midiManager, MidiDeviceInfo info) {
        // An output port, from the DEVICE's own perspective, is what sends note data OUT of the
        // keyboard -- i.e. exactly what this app needs to receive. (openInputPort would be for
        // sending TO the device, e.g. driving its LEDs -- not needed here.)
        if (info.getOutputPortCount() <= 0) return;
        midiManager.openDevice(info, new DeviceOpenedListener(this, info), null);
    }

    boolean hasOpenDevice() {
        return openDevice != null;
    }

    void setOpenDevice(MidiDevice device) {
        openDevice = device;
    }

    long addBytesReceived(int count) {
        return bytesReceived.addAndGet(count);
    }

    static String deviceName(MidiDeviceInfo info) {
        String name = info.getProperties().getString(MidiDeviceInfo.PROPERTY_NAME);
        return name != null ? name : "MIDI device";
    }

    // MidiDevice.close() declares `throws IOException` (a real Binder call under the hood) --
    // nothing useful to do about a failed close on the way out, so this just swallows it rather
    // than making every call site handle a checked exception it can't meaningfully recover from.
    static void closeQuietly(MidiDevice device) {
        try {
            device.close();
        } catch (IOException e) {
            // Nothing to do -- we're tearing down anyway.
        }
    }

    @Override
    protected void onDestroy() {
        if (openDevice != null) closeQuietly(openDevice);
        NativeBridge.nativeShutdown();
        super.onDestroy();
    }

    private static final class HotplugCallback extends MidiManager.DeviceCallback {
        private final MainActivity activity;
        private final MidiManager midiManager;

        HotplugCallback(MainActivity activity, MidiManager midiManager) {
            this.activity = activity;
            this.midiManager = midiManager;
        }

        @Override
        public void onDeviceAdded(MidiDeviceInfo device) {
            if (!activity.hasOpenDevice()) activity.tryOpenDevice(midiManager, device);
        }
    }

    private static final class DeviceOpenedListener implements MidiManager.OnDeviceOpenedListener {
        private final MainActivity activity;
        private final MidiDeviceInfo info;

        DeviceOpenedListener(MainActivity activity, MidiDeviceInfo info) {
            this.activity = activity;
            this.info = info;
        }

        @Override
        public void onDeviceOpened(MidiDevice device) {
            if (device == null) {
                Log.i(TAG, "Failed to open MIDI device \"" + deviceName(info) + "\".");
                return;
            }
            MidiOutputPort port = device.openOutputPort(0);
            if (port == null) {
                Log.i(TAG, "Opened \"" + deviceName(info) + "\" but its output port 0 wouldn't open.");
                closeQuietly(device);
                return;
            }
            activity.setOpenDevice(device);
            port.connect(new MidiRelay(activity, info));
            Log.i(TAG, "Connected: " + deviceName(info) + " -- waiting for notes...");
        }
    }

    private static final class MidiRelay extends MidiReceiver {
        private final MainActivity activity;
        private final MidiDeviceInfo info;

        MidiRelay(MainActivity activity, MidiDeviceInfo info) {
            this.activity = activity;
            this.info = info;
        }

        @Override
        public void onSend(byte[] data, int offset, int count, long timestamp) {
            NativeBridge.nativeOnMidi(data, offset, count);
            activity.addBytesReceived(count); // not re-logged per message -- see this class's own header comment on why status is logcat-only now
        }
    }
}
