package com.hds.androidtest;

import android.app.Activity;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicLong;

// No layout XML, no custom drawing, no Cairo -- this build exists to answer one question (does
// the real synth engine make correct sound on a phone from a real MIDI keyboard), not to be a
// GUI. See android/README.md and this repo's own top-level README for why the GUI is a separate,
// later decision. The one TextView below exists only so a quick glance confirms a MIDI device
// connected and bytes are actually arriving, without needing `adb logcat` open.
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
    private TextView statusView;
    private MidiDevice openDevice;
    private final AtomicLong bytesReceived = new AtomicLong(0);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        statusView = new TextView(this);
        statusView.setTextSize(16f);
        statusView.setGravity(Gravity.CENTER);
        statusView.setPadding(32, 32, 32, 32);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(statusView);
        setContentView(root);

        boolean audioOk = NativeBridge.nativeInit();
        statusView.setText(audioOk
                ? "Audio engine running.\nScanning for MIDI devices..."
                : "AAudio stream failed to open -- check logcat for \"hds_native\".");

        scanAndConnectMidi();
    }

    private void scanAndConnectMidi() {
        MidiManager midiManager = (MidiManager) getSystemService(MIDI_SERVICE);
        if (midiManager == null) {
            statusView.setText("No MidiManager on this device (android.software.midi feature missing).");
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

    void setStatusOnUiThread(String message) {
        runOnUiThread(new SetStatus(this, message));
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

    private static final class SetStatus implements Runnable {
        private final MainActivity activity;
        private final String message;

        SetStatus(MainActivity activity, String message) {
            this.activity = activity;
            this.message = message;
        }

        @Override
        public void run() {
            activity.statusView.setText(message);
        }
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
                activity.setStatusOnUiThread("Failed to open MIDI device \"" + deviceName(info) + "\".");
                return;
            }
            MidiOutputPort port = device.openOutputPort(0);
            if (port == null) {
                activity.setStatusOnUiThread("Opened \"" + deviceName(info) + "\" but its output port 0 wouldn't open.");
                closeQuietly(device);
                return;
            }
            activity.setOpenDevice(device);
            port.connect(new MidiRelay(activity, info));
            activity.setStatusOnUiThread("Connected: " + deviceName(info) + "\nWaiting for notes...");
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
            long total = activity.addBytesReceived(count);
            activity.setStatusOnUiThread("Connected: " + deviceName(info) + "\nMIDI bytes received: " + total);
        }
    }
}
