package com.hds.androidtest;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
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
    private static final int RECORD_AUDIO_REQUEST_CODE = 1;
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

        // MUST run before nativeInit(): tells the native OUTPUT stream which real device/channel
        // count to open against (a class-compliant USB interface's own real channel count, not a
        // hardcoded stereo guess) -- see hds_android.c's own "Real external audio input" header
        // comment and NativeBridge.java's own comment on nativeConfigureAudio.
        configureAudioDevices();

        // Also before nativeInit(): the Recording tab's own default folder has nowhere sane to
        // point to on Android otherwise (no $HOME, no writable /tmp in a normal app process) --
        // see hds_android.c's own g_android_files_dir comment. getExternalFilesDir(null) is this
        // app's own private-but-shared-storage directory (visible under Android/data/<package>/
        // files via a file manager or a USB cable, handy for actually retrieving a recording later)
        // and needs no runtime permission at all; getFilesDir() (pure internal storage) is the
        // fallback on the rare device where external storage isn't currently available.
        java.io.File filesDir = getExternalFilesDir(null);
        if (filesDir == null) filesDir = getFilesDir();
        NativeBridge.nativeSetFilesDir(filesDir.getAbsolutePath());

        boolean audioOk = NativeBridge.nativeInit();
        Log.i(TAG, audioOk ? "Audio engine running. Scanning for MIDI devices..."
                           : "AAudio stream failed to open -- check logcat for \"hds_native\".");

        // RECORD_AUDIO is a dangerous (runtime) permission: nativeInit() above already tried
        // nativeOpenAudioInput() opportunistically (succeeds immediately if a previous run already
        // granted it), so this only matters the first time -- requestPermissions() shows the
        // system dialog, and onRequestPermissionsResult below retries the native open once the
        // user actually answers it. Until then (or if denied), ext-in/the Recording tab's raw
        // Input Channel checkboxes simply stay silent -- see hds_android.c's own
        // nativeOpenAudioInput() comment.
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, RECORD_AUDIO_REQUEST_CODE);
        }

        scanAndConnectMidi();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != RECORD_AUDIO_REQUEST_CODE) return;
        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        Log.i(TAG, granted ? "RECORD_AUDIO granted -- opening the input stream."
                           : "RECORD_AUDIO denied -- ext-in/Recording-tab raw input channels will stay silent.");
        if (granted) NativeBridge.nativeOpenAudioInput();
    }

    // Picks the best real input/output device to open against: a class-compliant USB interface's
    // OWN device id and real channel count if one is currently connected (TYPE_USB_DEVICE/
    // TYPE_USB_HEADSET/TYPE_USB_ACCESSORY -- the three types Android reports a USB audio class
    // device under, depending on how it enumerates), falling back to device id 0 (AAUDIO_UNSPECIFIED
    // -- "let AAudio/the OS pick the current default route") at 2 channels otherwise, the exact
    // behavior this app always had before real device targeting existed. See hds_android.c's own
    // "Real external audio input" section header comment for why channel count isn't hardcoded to
    // 2 anymore: AudioDeviceInfo.getChannelCounts() reports the device's own real supported counts,
    // and this picks the largest (a genuinely multichannel interface is used as one, not downmixed).
    private void configureAudioDevices() {
        AudioManager audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        int inputDeviceId = 0, inputChannels = 2;
        int outputDeviceId = 0, outputChannels = 2;
        if (audioManager != null) {
            for (AudioDeviceInfo info : audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (isUsbAudioType(info.getType())) {
                    inputDeviceId = info.getId();
                    inputChannels = bestChannelCount(info);
                    break; // first class-compliant USB input wins -- a real interface reports itself once
                }
            }
            for (AudioDeviceInfo info : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                if (isUsbAudioType(info.getType())) {
                    outputDeviceId = info.getId();
                    outputChannels = bestChannelCount(info);
                    break;
                }
            }
        }
        NativeBridge.nativeConfigureAudio(inputDeviceId, inputChannels, outputDeviceId, outputChannels);
    }

    private static boolean isUsbAudioType(int type) {
        return type == AudioDeviceInfo.TYPE_USB_DEVICE
                || type == AudioDeviceInfo.TYPE_USB_HEADSET
                || type == AudioDeviceInfo.TYPE_USB_ACCESSORY;
    }

    // getChannelCounts() returns every discrete count the device advertises support for (e.g. an
    // 8-in interface might report {1, 2, 4, 6, 8}) -- the largest is the real "use every channel"
    // count this app wants; an empty array (seen on some devices/API levels, meaning "no specific
    // constraint") falls back to the same 2 this app always assumed.
    private static int bestChannelCount(AudioDeviceInfo info) {
        int best = 2;
        for (int c : info.getChannelCounts()) if (c > best) best = c;
        return best;
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
