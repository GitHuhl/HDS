package com.hds.androidtest

import android.app.Activity
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Bundle
import android.util.AttributeSet
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import java.util.concurrent.atomic.AtomicLong

// No layout XML, no custom drawing, no Cairo -- this build exists to answer one question (does
// the real synth engine make correct sound on a phone from a real MIDI keyboard), not to be a
// GUI. See android/README.md and this repo's own top-level README for why the GUI is a separate,
// later decision. The one TextView below exists only so a quick glance confirms a MIDI device
// connected and bytes are actually arriving, without needing `adb logcat` open.
class MainActivity : Activity() {
    private lateinit var statusView: TextView
    private var openDevice: MidiDevice? = null
    private val bytesReceived = AtomicLong(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        statusView = TextView(this).apply {
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(32, 32, 32, 32)
        }
        val root = LinearLayout(this, null as AttributeSet?).apply {
            orientation = LinearLayout.VERTICAL
            addView(statusView)
        }
        setContentView(root)

        val audioOk = NativeBridge.nativeInit()
        statusView.text = if (audioOk) {
            "Audio engine running.\nScanning for MIDI devices..."
        } else {
            "AAudio stream failed to open -- check logcat for \"hds_native\"."
        }

        scanAndConnectMidi()
    }

    private fun scanAndConnectMidi() {
        val midiManager = getSystemService(MIDI_SERVICE) as? MidiManager
        if (midiManager == null) {
            statusView.text = "No MidiManager on this device (android.software.midi feature missing)."
            return
        }

        // Connect to whatever's already plugged in, then keep listening for hotplug -- a USB MIDI
        // keyboard attached after the app launches is the realistic test case, not just "already
        // connected at boot".
        midiManager.devices.forEach { tryOpenDevice(midiManager, it) }
        midiManager.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(device: MidiDeviceInfo) {
                if (openDevice == null) tryOpenDevice(midiManager, device)
            }
        }, null)
    }

    private fun tryOpenDevice(midiManager: MidiManager, info: MidiDeviceInfo) {
        // An output port, from the DEVICE's own perspective, is what sends note data OUT of the
        // keyboard -- i.e. exactly what this app needs to receive. (openInputPort would be for
        // sending TO the device, e.g. driving its LEDs -- not needed here.)
        if (info.outputPortCount <= 0) return
        midiManager.openDevice(info, { device: MidiDevice? ->
            if (device == null) {
                statusView.text = "Failed to open MIDI device \"${deviceName(info)}\"."
                return@openDevice
            }
            val port = device.openOutputPort(0)
            if (port == null) {
                statusView.text = "Opened \"${deviceName(info)}\" but its output port 0 wouldn't open."
                device.close()
                return@openDevice
            }
            openDevice = device
            port.connect(object : MidiReceiver() {
                override fun onSend(data: ByteArray, offset: Int, count: Int, timestamp: Long) {
                    NativeBridge.nativeOnMidi(data, offset, count)
                    val total = bytesReceived.addAndGet(count.toLong())
                    runOnUiThread {
                        statusView.text = "Connected: ${deviceName(info)}\nMIDI bytes received: $total"
                    }
                }
            })
            runOnUiThread { statusView.text = "Connected: ${deviceName(info)}\nWaiting for notes..." }
        }, null, null)
    }

    private fun deviceName(info: MidiDeviceInfo): String =
        info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "MIDI device"

    override fun onDestroy() {
        openDevice?.close()
        NativeBridge.nativeShutdown()
        super.onDestroy()
    }
}
