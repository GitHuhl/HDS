// hds_standalone_devices.h -- ALSA/MIDI device enumeration (defined in hds_standalone.c) for the
// standalone GUI's Devices tab. Standalone-only: hds.clap never scans devices (the host owns them).
#ifndef HDS_STANDALONE_DEVICES_H
#define HDS_STANDALONE_DEVICES_H

#include "backend.h" // AUDIO_DEVICE_NAME_LEN

// Real ALSA/MIDI device enumeration for the native GUI's own Devices tab (standalone-only --
// hds_clap_devices_draw.c's own header comment explains why hds.clap/hds_fx.clap never call these
// at all: a CLAP host owns its own device selection). Shells out to aplay/arecord -L and aconnect
// -i via popen()/fgets() rather than using libasound directly (unlike enumerate_pcm_devices()
// above), with a fallback to one safe entry if the tool itself is missing. A real subprocess
// spawn+read, so only ever called from a "Rescan" click or the GUI's own startup, never per-frame.
#define HDS_DEVLIST_MAX 24
typedef struct {
    char names[HDS_DEVLIST_MAX][AUDIO_DEVICE_NAME_LEN];
    int count;
} HdsDeviceList;

// `tool` is "aplay" or "arecord"; fills `out` from `<tool> -L`, falling back to one "default" entry.
void hds_scan_alsa_devices(const char *tool, HdsDeviceList *out);
// Fills `out` from `aconnect -i`, falling back to an empty list.
void hds_scan_midi_devices(HdsDeviceList *out);

#endif // HDS_STANDALONE_DEVICES_H
