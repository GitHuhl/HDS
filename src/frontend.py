import sys
import os
import shutil
import socket
import struct
import math
import random
import subprocess
import re
import time
import json
import argparse
from datetime import datetime
import numpy as np
from pythonosc.udp_client import SimpleUDPClient
from pythonosc.osc_message_builder import OscMessageBuilder
from pythonosc.osc_bundle_builder import OscBundleBuilder, IMMEDIATELY
from pythonosc.osc_packet import OscPacket, ParseError as OscParseError
from PySide6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QComboBox, QGroupBox, QRadioButton, QTabWidget,
                             QSlider, QPushButton, QProgressBar, QSpinBox, QDoubleSpinBox, QLineEdit, QCheckBox,
                             QScrollArea, QMenu, QInputDialog, QFileDialog, QGridLayout, QMessageBox,
                             QListWidget, QListWidgetItem)
from PySide6.QtGui import QPainter, QPainterPath, QColor, QPen, QBrush, QWheelEvent, QCursor, QAction, QFont
from PySide6.QtCore import Qt, QTimer, QPoint, QPointF, QRectF, Signal

def _format_semitones_cents(total_cents):
    """Format a raw cents value as 'SS.CC st' (e.g. 2401 -> '24.01 st')."""
    total_cents = int(total_cents)
    semis, cents = divmod(total_cents, 100)
    return f"{semis}.{cents:02d} st"

def _retain_hidden(widget):
    """Make a widget keep its layout footprint even while hidden."""
    policy = widget.sizePolicy()
    policy.setRetainSizeWhenHidden(True)
    widget.setSizePolicy(policy)
    return widget

# Volume taper: the oscillator Volume sliders (vco1_vol..vco4_vol) and Master Volume used to
# send their raw 0-100 position straight through as a 0.0-1.0 linear gain fraction (pos/100.0).
# Linear taper on a volume control is the classic reason a "quiet" setting is nearly impossible to
# dial in by hand: equal steps of knob travel do NOT correspond to equal steps of perceived
# loudness, so almost the entire audible-but-quiet range (roughly -40dB to -60dB) got crushed into
# under 1% of the slider's own travel -- under a single pixel on an 80px-tall slider. A power-law
# (audio) taper fixes this by spreading that same quiet range across much more of the slider's
# travel, while still hitting exactly 0.0 at position 0 and exactly 1.0 at position 100. This is a
# GUI-side mapping ONLY -- backend.c's gain math (including master_volume's own existing one-pole
# smoothing/chase, untouched), the FM sidechain scaling, mix_bal, and every mod-matrix amp-
# modulation path all keep reading/writing the exact same plain linear 0.0-1.0 fraction they
# always have; nothing about how that fraction is actually used as a gain changed.
AUDIO_VOL_TAPER_EXPONENT = 3.0  # cubic -- a common, simple approximation of a real log/audio pot

def _audio_vol_taper(slider_pos):
    """Volume slider position (0-100) -> linear gain fraction (0.0-1.0)."""
    t = max(0.0, min(100.0, float(slider_pos))) / 100.0
    return t ** AUDIO_VOL_TAPER_EXPONENT

def _audio_vol_untaper(linear_gain):
    """Exact inverse of _audio_vol_taper: linear gain fraction (0.0-1.0) -> slider position
    (0-100) -- used when a saved Program's/Combi lane's/the live backend's real gain value needs
    to be reflected back onto the slider so a later transmit_knob_packet() reproduces that exact
    gain again."""
    t = max(0.0, min(1.0, float(linear_gain))) ** (1.0 / AUDIO_VOL_TAPER_EXPONENT)
    return int(round(t * 100.0))

def _decode_wav_peaks(path, num_buckets=600):
    """Reads a WAV file's audio data (mirroring backend.c's load_sample_slot format support --
    PCM 16/24/32-bit integer or 32-bit IEEE float, any channel count) and downsamples it to
    num_buckets (min, max) peak pairs for the Mapping tab's waveform display. Entirely
    independent of the backend's own WAV parsing/DFD streaming -- this only ever runs on the GUI
    side, purely to draw a picture, so the engine never needs to round-trip sample data over the
    socket just so the GUI can show a waveform. Returns (mins, maxs, total_frames) as float32
    numpy arrays / int, or None if the file can't be opened or isn't a format we understand.
    Channels are averaged into one mono trace the same way render_sample_osc's own downmix does
    (first two channels only), matching what the zone will actually sound like."""
    try:
        with open(path, "rb") as f:
            riff = f.read(12)
            if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
                return None
            fmt = None
            data_offset, data_size = None, None
            while True:
                header = f.read(8)
                if len(header) < 8:
                    break
                chunk_id, chunk_size = header[0:4], struct.unpack("<I", header[4:8])[0]
                if chunk_id == b"fmt ":
                    fmt_bytes = f.read(chunk_size)
                    audio_format, num_channels, _sr, _br, _ba, bits_per_sample = struct.unpack("<HHIIHH", fmt_bytes[:16])
                    fmt = (audio_format, num_channels, bits_per_sample)
                elif chunk_id == b"data":
                    data_offset, data_size = f.tell(), chunk_size
                    break
                else:
                    f.seek(chunk_size, 1)
            if fmt is None or data_offset is None:
                return None
            audio_format, num_channels, bits_per_sample = fmt
            f.seek(data_offset)
            raw = f.read(data_size)

        if audio_format == 1 and bits_per_sample == 16:
            samples = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
        elif audio_format == 1 and bits_per_sample == 24:
            n = len(raw) // 3
            buf = np.frombuffer(raw[: n * 3], dtype=np.uint8).reshape(-1, 3)
            as_i32 = (buf[:, 0].astype(np.int32) << 8) | (buf[:, 1].astype(np.int32) << 16) | (buf[:, 2].astype(np.int32) << 24)
            samples = as_i32.astype(np.float32) / 2147483648.0
        elif audio_format == 1 and bits_per_sample == 32:
            samples = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
        elif audio_format == 3 and bits_per_sample == 32:
            samples = np.frombuffer(raw, dtype="<f4").astype(np.float32)
        else:
            return None

        if num_channels < 1:
            return None
        total_frames = len(samples) // num_channels
        if total_frames < 1:
            return None
        samples = samples[: total_frames * num_channels].reshape(total_frames, num_channels)
        mono = samples[:, :2].mean(axis=1) if num_channels >= 2 else samples[:, 0]

        num_buckets = max(1, min(num_buckets, total_frames))
        edges = np.linspace(0, total_frames, num_buckets + 1).astype(np.int64)
        mins = np.empty(num_buckets, dtype=np.float32)
        maxs = np.empty(num_buckets, dtype=np.float32)
        for i in range(num_buckets):
            chunk = mono[edges[i]:edges[i + 1]]
            if chunk.size == 0:
                mins[i] = maxs[i] = 0.0
            else:
                mins[i] = chunk.min()
                maxs[i] = chunk.max()
        return mins, maxs, total_frames
    except (OSError, ValueError, struct.error):
        return None

def _wav_total_frames(path):
    """Cheap RIFF-header-only frame count -- shares _decode_wav_peaks()'s chunk-walking logic but
    stops right after the fmt/data chunk headers instead of reading and decoding the actual audio.
    Used by the SFZ importer, which needs total_frames (to convert loop_start=/loop_end=/offset=
    from absolute sample frames to the 0.0-1.0 fractions SampleZone uses) for potentially hundreds
    of regions -- decoding full peak data for each just to get a frame count would be wasteful.
    Returns None if the file can't be opened or isn't a format we understand."""
    try:
        with open(path, "rb") as f:
            riff = f.read(12)
            if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
                return None
            fmt = None
            data_size = None
            while True:
                header = f.read(8)
                if len(header) < 8:
                    break
                chunk_id, chunk_size = header[0:4], struct.unpack("<I", header[4:8])[0]
                if chunk_id == b"fmt ":
                    fmt_bytes = f.read(chunk_size)
                    _audio_format, num_channels, _sr, _br, _ba, bits_per_sample = struct.unpack("<HHIIHH", fmt_bytes[:16])
                    fmt = (num_channels, bits_per_sample)
                elif chunk_id == b"data":
                    data_size = chunk_size
                    break
                else:
                    f.seek(chunk_size, 1)
        if fmt is None or data_size is None:
            return None
        num_channels, bits_per_sample = fmt
        bytes_per_sample = bits_per_sample // 8
        if num_channels < 1 or bytes_per_sample < 1:
            return None
        return data_size // (num_channels * bytes_per_sample)
    except (OSError, ValueError, struct.error):
        return None

_SFZ_NOTE_LETTERS = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
_SFZ_NOTE_NAME_RE = re.compile(r"^([a-gA-G])([#sSbB]?)(-?\d+)$")

def _sfz_note_to_number(value):
    """Parses an SFZ key opcode value as either a bare MIDI number ("60") or a note name
    ("C4", "F#3", "Ab-1", "cs2" -- 's' is a common alternate spelling for sharp). Uses the same
    C4=60 octave convention this file's own root_key_box already uses (octave = note//12 - 1),
    which is the convention most SFZ libraries assume -- a library authored under the other
    common convention (C3=60) will end up an octave off; a known, accepted limitation rather than
    something auto-detected. Returns None if the value doesn't parse as either shape."""
    value = value.strip()
    if not value:
        return None
    try:
        return int(value)
    except ValueError:
        pass
    m = _SFZ_NOTE_NAME_RE.match(value)
    if not m:
        return None
    letter, accidental, octave_str = m.groups()
    pitch_class = _SFZ_NOTE_LETTERS[letter.lower()]
    if accidental.lower() in ("#", "s"):
        pitch_class += 1
    elif accidental.lower() == "b":
        pitch_class -= 1
    note = (int(octave_str) + 1) * 12 + pitch_class
    return note if 0 <= note <= 127 else None

_SFZ_TOKEN_RE = re.compile(r"<(\w+)>|([a-zA-Z_][a-zA-Z0-9_]*)=")

def _parse_sfz(path, _visited=None):
    """Parses an SFZ file (including #include'd sub-files) into a flat list of per-region opcode
    dicts, with <global>/<group>/<master> inheritance already resolved (each region's dict starts
    as a copy of whatever group dict was active, then applies its own opcodes on top -- last
    value for a key wins, same as real SFZ). Also returns the <control> header's default_path=
    (resolved relative to `path`'s own directory), used for sample path resolution.

    Tokenization is the one genuinely tricky part of this format: opcodes are `key=value` pairs
    where `value` can contain unescaped spaces (sample paths routinely do) and isn't quoted, so
    this can't be a whitespace split -- each opcode's value is instead whatever text sits between
    its own `=` and the START of the next opcode/header token, found by scanning for those token
    boundaries directly rather than guessing where a value ends.

    #define/$variable macro substitution is NOT supported (rarer, real added complexity) --
    a region using it parses with the literal "$name" text still in its value, which will fail
    path resolution or a later int()/float() conversion and be skipped with a warning by the
    caller, not silently accepted or crashed on here.

    Returns (regions, default_path) where regions is a list of {opcode: value_string} dicts.
    """
    if _visited is None:
        _visited = set()
    real_path = os.path.realpath(path)
    if real_path in _visited:
        return [], None  # #include cycle guard -- silently stop recursing, already-seen content
    _visited.add(real_path)

    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw_text = f.read()
    except OSError:
        return [], None

    base_dir = os.path.dirname(os.path.abspath(path))

    # Strip //-to-end-of-line comments before tokenizing. #include lines are blanked out here
    # (not left in place) -- they contain no '=' or '<...>' the token scan below would recognize
    # as a boundary, so left as-is their text would get swallowed into whichever opcode's value
    # was still open, corrupting it (e.g. "volume=-9\n#include \"x.sfz\"" would otherwise become
    # volume's literal value). #include is expanded separately, by directly re-scanning raw_text
    # below, once this file's own regions/default_path are fully parsed.
    lines = []
    for line in raw_text.splitlines():
        idx = line.find("//")
        line = line[:idx] if idx >= 0 else line
        if re.search(r'#include\s+"[^"]+"', line):
            line = ""
        lines.append(line)
    text = "\n".join(lines)

    control_opcodes = {}
    global_opcodes = {}
    group_opcodes = dict(global_opcodes)
    regions = []
    current_dict = None
    last_key = None
    last_value_start = None
    default_path = None

    def flush_value(end_pos):
        nonlocal last_key
        if last_key is not None and current_dict is not None:
            current_dict[last_key] = text[last_value_start:end_pos].strip()
        last_key = None

    matches = list(_SFZ_TOKEN_RE.finditer(text))
    for m in matches:
        flush_value(m.start())
        header, opcode_key = m.group(1), m.group(2)
        if header is not None:
            header = header.lower()
            if header == "global":
                global_opcodes = {}
                current_dict = global_opcodes
            elif header in ("group", "master"):
                group_opcodes = dict(global_opcodes)
                current_dict = group_opcodes
            elif header == "region":
                region_dict = dict(group_opcodes)
                regions.append(region_dict)
                current_dict = region_dict
            elif header == "control":
                current_dict = control_opcodes
            else:
                current_dict = {}  # unrecognized header (e.g. <curve>, <effect>) -- parsed but ignored
        else:
            last_key = opcode_key.lower()
            last_value_start = m.end()
    flush_value(len(text))

    if "default_path" in control_opcodes:
        default_path = os.path.join(base_dir, control_opcodes["default_path"].replace("\\", "/"))

    # Expand any #include'd files now, inserting their regions where the include appeared. Since
    # regions before/after an include already accumulated correctly above (the marker line has no
    # '=' or '<...>' in it, so the tokenizer just ignores it), we only need to actually splice in
    # the included regions themselves -- order relative to surrounding regions doesn't matter for
    # this importer (round-robin grouping keys off group=/seq_length=, not file position).
    for line in raw_text.splitlines():
        m = re.search(r'#include\s+"([^"]+)"', line)
        if m:
            inc_path = os.path.join(base_dir, m.group(1).replace("\\", "/"))
            inc_regions, inc_default_path = _parse_sfz(inc_path, _visited)
            regions.extend(inc_regions)
            if default_path is None:
                default_path = inc_default_path

    return regions, default_path

def _resolve_sfz_sample_path(sample_value, sfz_dir, default_path):
    """Resolves an SFZ region's sample= value to a real file on disk. Tries, in order: (1) as an
    absolute path if it looks like one; (2) joined with default_path (the <control> header's
    default_path=, already resolved relative to the .sfz file's own directory by _parse_sfz);
    (3) joined with the .sfz file's own directory directly; (4) a case-insensitive filename match
    within whichever of (2)/(3) resolved to an existing directory, for the common cross-platform
    case-mismatch issue (many libraries are authored on Windows/macOS). Returns None if nothing
    matches -- the caller skips that region with a warning rather than failing the whole import."""
    normalized = sample_value.replace("\\", "/")
    if os.path.isabs(normalized) and os.path.isfile(normalized):
        return normalized
    candidates = []
    if default_path:
        candidates.append(os.path.join(default_path, normalized))
    candidates.append(os.path.join(sfz_dir, normalized))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    for candidate in candidates:
        directory = os.path.dirname(candidate)
        target_name = os.path.basename(candidate).lower()
        try:
            for entry in os.listdir(directory):
                if entry.lower() == target_name:
                    return os.path.join(directory, entry)
        except OSError:
            continue
    return None

def _sfz_frame_to_frac(frame_str, total_frames):
    """SFZ loop_start=/loop_end=/offset= are absolute sample frames; SampleZone wants a 0.0-1.0
    fraction of total_frames -- the exact inverse of apply_loop_points()'s frac*(total-1) in
    backend.c. Returns None if frame_str doesn't parse as a number."""
    try:
        frame = float(frame_str)
    except ValueError:
        return None
    if total_frames <= 1:
        return 0.0
    return max(0.0, min(1.0, frame / (total_frames - 1)))

def _build_zones_from_sfz(path, zone_id_start):
    """Converts one parsed SFZ file into (zones, warnings) -- a list of fully-populated
    SampleZone objects (not yet appended to any mapping_grid, kept testable standalone) plus a
    list of human-readable warning strings for anything skipped or simplified. See the opcode ->
    SampleZone mapping table in the SFZ import plan for the full rationale on each field."""
    regions, default_path = _parse_sfz(path)
    sfz_dir = os.path.dirname(os.path.abspath(path))
    zones = []
    warnings = []
    missing_samples = 0
    define_usage_count = 0
    round_robin_ids = {}  # SFZ group= value -> compacted small int, only for regions with seq_length
    next_zone_id = zone_id_start

    for region in regions:
        sample_value = region.get("sample")
        if not sample_value:
            continue  # a region with no sample= isn't a playable zone at all
        if "$" in sample_value:
            define_usage_count += 1
        resolved_path = _resolve_sfz_sample_path(sample_value, sfz_dir, default_path)
        if resolved_path is None or not resolved_path.lower().endswith(".wav"):
            missing_samples += 1
            continue
        total_frames = _wav_total_frames(resolved_path)
        if total_frames is None or total_frames < 1:
            missing_samples += 1
            continue

        # key=/lokey=/hikey=/pitch_keycenter= -- key= is shorthand for all three at once, but
        # never overrides an opcode the region separately specifies.
        key_shorthand = region.get("key")
        lokey_raw = region.get("lokey", key_shorthand)
        hikey_raw = region.get("hikey", key_shorthand)
        keycenter_raw = region.get("pitch_keycenter", region.get("lokey", key_shorthand))
        lokey = _sfz_note_to_number(lokey_raw) if lokey_raw is not None else None
        hikey = _sfz_note_to_number(hikey_raw) if hikey_raw is not None else None
        keycenter = _sfz_note_to_number(keycenter_raw) if keycenter_raw is not None else None
        if lokey is None or hikey is None:
            missing_samples += 1  # no usable key range -- can't place this zone at all
            continue
        if keycenter is None:
            keycenter = lokey

        zone = SampleZone(resolved_path, lokey, next_zone_id)
        next_zone_id += 1
        zone.root_note = lokey
        zone.max_note = hikey
        try:
            pitch_keytrack = float(region.get("pitch_keytrack", "100"))
        except ValueError:
            pitch_keytrack = 100.0
        zone.key_center = -1 if pitch_keytrack == 0.0 else keycenter

        def _int_opt(key, default):
            try:
                return int(float(region[key]))
            except (KeyError, ValueError):
                return default
        zone.min_velocity = max(0, min(127, _int_opt("lovel", 0)))
        zone.max_velocity = max(0, min(127, _int_opt("hivel", 127)))

        loop_mode = region.get("loop_mode", "").strip().lower()
        if loop_mode == "loop_sustain":
            # Full support (this engine now has a real release-triggered loop exit) -- see
            # backend.c's SampleSlot.loop_release_exit.
            zone.loop_enabled = True
            zone.loop_release_exit = True
        elif loop_mode == "loop_continuous":
            zone.loop_enabled = True
        elif loop_mode in ("no_loop", "one_shot"):
            zone.loop_enabled = False
        # else: opcode absent -- leave SampleZone's own default (whole-file, loop enabled)

        if "loop_start" in region:
            frac = _sfz_frame_to_frac(region["loop_start"], total_frames)
            if frac is not None:
                zone.loop_start = frac
        if "loop_end" in region:
            frac = _sfz_frame_to_frac(region["loop_end"], total_frames)
            if frac is not None:
                zone.loop_end = frac
        if "offset" in region:
            frac = _sfz_frame_to_frac(region["offset"], total_frames)
            if frac is not None:
                zone.start_offset = frac
        if "end" in region:
            # Full support: an independent one-shot truncation point -- see backend.c's
            # SampleSlot.playback_end_frame.
            frac = _sfz_frame_to_frac(region["end"], total_frames)
            if frac is not None:
                zone.end_frac = frac

        def _float_opt(key, default):
            try:
                return float(region[key])
            except (KeyError, ValueError):
                return default
        zone.tune_cents = _float_opt("tune", _float_opt("pitch", 0.0))
        zone.gain_db = _float_opt("volume", 0.0)
        zone.pan = _float_opt("pan", 0.0)
        zone.reverse = region.get("direction", "").strip().lower() == "reverse"

        # Round-robin: group= is overloaded in SFZ (also used for unrelated choke/polyphony
        # groups) -- only treat it as round-robin when seq_length is actually present, and
        # compact each distinct qualifying group= value into a small sequential id, since SFZ's
        # own group numbers can collide across unrelated articulations in the same file.
        if "seq_length" in region and "group" in region:
            sfz_group = region["group"]
            if sfz_group not in round_robin_ids:
                round_robin_ids[sfz_group] = len(round_robin_ids) + 1
            zone.round_robin_group = round_robin_ids[sfz_group]

        zones.append(zone)

    if missing_samples:
        warnings.append(f"{missing_samples} region(s) skipped (sample not found, not a .wav, or missing a usable key range)")
    if define_usage_count:
        warnings.append(f"{define_usage_count} region(s) appear to use #define/$variable substitution (not supported) -- their sample path may be wrong")

    return zones, warnings

def _sfz_frac_to_frame(frac, total_frames):
    """The exact inverse of _sfz_frame_to_frac() above -- SampleZone's 0.0-1.0 fraction back to
    an absolute sample frame for writing loop_start=/loop_end=/offset=/end=."""
    if total_frames <= 1:
        return 0
    return round(max(0.0, min(1.0, frac)) * (total_frames - 1))

def _build_sfz_from_zones(zones):
    """Direct inverse of _build_zones_from_sfz()'s own opcode table -- one <region> per zone,
    plain vanilla SFZ with no HDS-specific extensions, so the file stays genuinely spec-compliant
    on its own (Program Save references it by filename rather than embedding it, specifically to
    keep HDS's own save format and the SFZ spec cleanly separate). Samples themselves are NOT
    copied -- sample= points at each zone's existing file_path, same posture the importer already
    takes on sample ownership. Round-robin groups are re-numbered into sequential seq_position
    values per group (SFZ has no direct "group index" concept to preserve verbatim)."""
    lines = ["// Exported by HDS -- see the Program that references this file for its own", "// non-SFZ parameters (this file itself carries none).", ""]
    group_members = {}  # round_robin_group -> ordered list of zone indices, for seq_length/seq_position
    for i, zone in enumerate(zones):
        if zone.round_robin_group > 0:
            group_members.setdefault(zone.round_robin_group, []).append(i)
    group_position = {}  # zone index -> (seq_length, seq_position)
    for members in group_members.values():
        for pos, idx in enumerate(members):
            group_position[idx] = (len(members), pos + 1)

    for i, zone in enumerate(zones):
        total_frames = _wav_total_frames(zone.file_path)
        lines.append("<region>")
        lines.append(f"sample={zone.file_path}")
        lines.append(f"lokey={int(zone.root_note)}")
        lines.append(f"hikey={int(zone.max_note)}")
        if zone.key_center == -1:
            lines.append("pitch_keytrack=0")
            lines.append(f"pitch_keycenter={int(zone.root_note)}")
        else:
            lines.append(f"pitch_keycenter={int(zone.key_center)}")
        lines.append(f"lovel={int(zone.min_velocity)}")
        lines.append(f"hivel={int(zone.max_velocity)}")
        if zone.loop_enabled:
            lines.append(f"loop_mode={'loop_sustain' if zone.loop_release_exit else 'loop_continuous'}")
        else:
            lines.append("loop_mode=no_loop")
        if total_frames and total_frames > 1:
            if zone.loop_enabled:
                lines.append(f"loop_start={_sfz_frac_to_frame(zone.loop_start, total_frames)}")
                lines.append(f"loop_end={_sfz_frac_to_frame(zone.loop_end, total_frames)}")
            if zone.start_offset > 0.0:
                lines.append(f"offset={_sfz_frac_to_frame(zone.start_offset, total_frames)}")
            if zone.end_frac < 1.0:
                lines.append(f"end={_sfz_frac_to_frame(zone.end_frac, total_frames)}")
        if zone.tune_cents:
            lines.append(f"tune={zone.tune_cents}")
        if zone.gain_db:
            lines.append(f"volume={zone.gain_db}")
        if zone.pan:
            lines.append(f"pan={zone.pan}")
        if zone.reverse:
            lines.append("direction=reverse")
        if i in group_position:
            seq_length, seq_position = group_position[i]
            lines.append(f"group={zone.round_robin_group}")
            lines.append(f"seq_length={seq_length}")
            lines.append(f"seq_position={seq_position}")
        lines.append("")
    return "\n".join(lines)

# Distinct badge colors for round-robin groups 1, 2, 3, ... (cycles if there are more groups
# than colors -- purely cosmetic, doesn't affect which group a zone actually belongs to).
_ROUND_ROBIN_COLORS = ["#c0392b", "#2980b9", "#27ae60", "#8e44ad", "#d35400", "#16a085"]
def _round_robin_group_color(group):
    return QColor(_ROUND_ROBIN_COLORS[(group - 1) % len(_ROUND_ROBIN_COLORS)])

# OSC control protocol endpoints -- must match whichever backend this frontend is pointed at:
# the standalone app's --osc-listen-port/--osc-reply-port defaults (9000/9001), or a CLAP
# plugin instance's own embedded OSC server (9010/9011 by default -- see clap_plugin.c). These
# module-level names are the actual, live values used everywhere below (connect_to_backend(),
# etc.); the CLI flags parsed in __main__ (--osc-host/--osc-listen-port/--osc-reply-port, the
# latter two matching the backend's own flag names exactly) override them before SynthGui() is
# constructed, i.e. before connect_to_backend() ever reads them.
#
# OSC_HOST is where the BACKEND lives -- defaults to localhost (the historical, still-most-common
# case), but is NOT the only supported value: the backend's own OSC listener already binds to all
# interfaces (lo_server_new_with_proto() with no host given -- confirmed directly against
# backend.c), so pointing OSC_HOST at a real LAN address via --osc-host and starting the backend
# with a matching --osc-reply-host (its own existing flag, pointed back at THIS machine) is a
# fully supported way to run the two on separate machines, not a hack.
OSC_HOST = "127.0.0.1"
# OSC_BIND_HOST is where THIS frontend's own status-reply listener binds -- deliberately always
# "0.0.0.0" (every interface), not OSC_HOST, and not independently configurable: binding to
# 127.0.0.1 only accepts loopback traffic, which would silently discard every /hds/status/* reply
# from a backend running on a different machine even after OSC_HOST above is pointed at it. This
# was the second (and, until fixed, undiscovered) half of what made cross-machine operation not
# actually work -- OSC_HOST alone being configurable wasn't enough. "0.0.0.0" is strictly more
# permissive than "127.0.0.1" and behaves identically for the local-only case too, so there's no
# real reason to ever want the narrower binding -- no flag needed for it.
OSC_BIND_HOST = "0.0.0.0"
OSC_SEND_PORT = 9000  # backend's --osc-listen-port: every knob + Sample Bank catalog goes here
OSC_RECV_PORT = 9001  # backend's --osc-reply-port: /hds/status/* feedback arrives here
# How long without a status bundle before the GUI considers the backend gone (a few missed
# vu_timer ticks' worth) -- see listen_for_status_frames()/_backend_is_live().
OSC_STATUS_TIMEOUT_S = 1.5

# Device name byte width must match AUDIO_DEVICE_NAME_LEN in backend.c exactly.
AUDIO_DEVICE_NAME_LEN = 64
# FX bus geometry must match FX_SLOT_COUNT / FX_SLOT_PARAM_COUNT in backend.c exactly.
FX_SLOT_COUNT = 12
FX_SLOT_PARAM_COUNT = 8
# Combi mode: up to COMBI_PROGRAM_COUNT independently-synthesizing Programs sharing one voice
# pool -- must match COMBI_PROGRAM_COUNT in backend.c exactly. Defined here, ahead of
# SOURCE_NAMES/SOURCE_COUNT below, since the bus/source model is built directly on top of it.
# Sequencer record-quantize grid resolutions, as "divisions per beat" -- must match
# seq_quantize_resolution's own convention in backend.c exactly (a plain divisor, not a musical-
# fraction enum). Index into this list is the Resolution combo's currentIndex().
SEQ_QUANTIZE_RESOLUTIONS = [1, 2, 3, 4, 6, 8]
COMBI_PROGRAM_COUNT = 8
# Multi-source/multi-output audio routing: must match EXT_IN_PAIR_COUNT/SOURCE_COUNT/
# OUTPUT_PAIR_MAX in backend.c exactly. BREAKING CHANGE (real Combi mode): source ids 0-7 are
# now the COMBI_PROGRAM_COUNT Combi Programs (0 = today's single Program, unchanged), id 8 is
# the new "Combi Mix" bus (the whole stack's raw dry pre-FX sum), and ext-in pairs moved from
# ids 1-8 to COMBI_PROGRAM_COUNT+1..+8 (9-16) -- see SOURCE_COUNT's own comment in backend.c.
EXT_IN_PAIR_COUNT = 8
OUTPUT_PAIR_MAX = 8
SOURCE_NAMES = [f"Program {p + 1}" for p in range(COMBI_PROGRAM_COUNT)] + ["Combi Mix"] + [f"Ext In {p + 1}" for p in range(EXT_IN_PAIR_COUNT)]
SOURCE_COUNT = len(SOURCE_NAMES)
# Hard array-allocation ceilings -- must match MAX_VOICES / BUFFER_SIZE in backend.c exactly.
# The ACTUAL live polyphony/block size are user-adjustable (Audio tab) up to these ceilings;
# see active_voice_limit/block_size_frames and effective_voice_limit()/effective_block_size()
# in backend.c for why this isn't just one fixed number anymore.
MAX_VOICES_HARD_LIMIT = 256
BUFFER_SIZE_HARD_LIMIT = 1024
# Recording tab: must match RECORD_MAX_CHANNELS / RECORD_PATH_LEN in backend.c exactly.
RECORD_MAX_CHANNELS = 16
RECORD_PATH_LEN = 256
# Programmable tempo: must match TEMPO_MARKER_MAX in backend.c exactly. There's no marker editor
# UI yet -- the frontend always sends tempo_marker_count=0 -- but the wire layout still reserves
# this many (time, bpm) marker slots so a future editor doesn't need another breaking change.
TEMPO_MARKER_MAX = 16
# Sample Bank: the zone catalog itself has no product-facing cap (see SAMPLE_SLOT_CAPACITY's
# comment in backend.c) and travels over its own dedicated socket -- see transmit_zone_upsert()/
# transmit_zone_remove(). SAMPLE_PATH_LEN is still a per-path length bound, must match backend.c
# exactly. MAX_ROUND_ROBIN_GROUPS is a real, small, legitimately-boundable UI concept (how many
# independent round-robin groups can exist at once), unrelated to catalog size -- must match
# backend.c exactly.
SAMPLE_PATH_LEN = 256
MAX_ROUND_ROBIN_GROUPS = 256
# 4-oscillator architecture: all 4 oscillators (Osc 1-4) share this exact shape palette --
# must match WaveType in backend.c exactly (append-only order, "Off" last).
OSC_SHAPE_NAMES = ["Saw", "Square", "Triangle", "Sine", "Unison Saw", "Noise", "Pulse", "Sample", "Off"]
# LFO count: must match LFO_COUNT in backend.c exactly.
LFO_COUNT = 16
# LFO Tempo Sync divisions -- index 0 ("Free") means "use the Rate dial's own Hz value", matching
# PackLfoParams.tempo_sync_division == 0 exactly; indices 1-24 are 1-based, matching backend.c's
# own LFO_SYNC_BEATS_PER_CYCLE table order exactly (grouped by note name -- 2 Bars/2 Bars Dotted/
# 2 Bars Triplet, then 1 Bar/../.., down to 1/64/../.. -- not sorted strictly by resulting speed;
# see that table's own comment for why). Combo currentIndex() IS the wire value sent directly --
# no separate lookup/offset needed on either side.
LFO_SYNC_DIVISION_LABELS = [
    "Free",
    "2 Bars", "2 Bars D", "2 Bars T",
    "1 Bar", "1 Bar D", "1 Bar T",
    "1/2", "1/2 D", "1/2 T",
    "1/4", "1/4 D", "1/4 T",
    "1/8", "1/8 D", "1/8 T",
    "1/16", "1/16 D", "1/16 T",
    "1/32", "1/32 D", "1/32 T",
    "1/64", "1/64 D", "1/64 T",
]
# Combi/Sequencer lane scaffolding: must match SEQ_LANE_COUNT/AUDIO_LANE_RESOURCE_COUNT in
# backend.c exactly. COMBI_PROGRAM_COUNT itself is defined earlier now (ahead of SOURCE_NAMES/
# SOURCE_COUNT, which are built directly on top of it) -- see that definition's own comment.
SEQ_LANE_COUNT = 32
AUDIO_LANE_RESOURCE_COUNT = 16
# FM Algorithm system: must match FM_ALGORITHM_COUNT/FM_ALGORITHMS in backend.c exactly. Each
# entry names which depth dial(s) are relevant for that algorithm (used to show/hide the depth
# knobs conditionally, mirroring the FX slot param show/hide pattern) -- "fm_intensity" is the
# always-existing "2->1" depth (the old single FM Depth dial).
FM_ALGORITHMS = [
    {"name": "0: Classic (2→1)", "depths": ["fm_intensity"]},
    {"name": "1: 3-Op Chain (3→2→1), Osc4 independent", "depths": ["fm_depth_3_2", "fm_intensity"]},
    {"name": "2: 4-Op Chain (4→3→2→1)", "depths": ["fm_depth_4_3", "fm_depth_3_2", "fm_intensity"]},
    {"name": "3: Dual Serial Pairs (4→3, 2→1)", "depths": ["fm_depth_4_3", "fm_intensity"]},
    {"name": "4: Many-to-One into 1 (3→1, 4→1)", "depths": ["fm_depth_3_1", "fm_depth_4_1"]},
    {"name": "5: One-to-Many from 3 (3→1, 3→2)", "depths": ["fm_depth_3_1", "fm_depth_3_2"]},
    {"name": "6: Many-to-One into 2 (1→2, 3→2)", "depths": ["fm_depth_1_2", "fm_depth_3_2"]},
    {"name": "7: 4→1 direct, Osc2/3 independent", "depths": ["fm_depth_4_1"]},
    {"name": "8: Dual Independent Pairs (1→2, 3→4)", "depths": ["fm_depth_1_2", "fm_depth_3_4"]},
    {"name": "9: 1→3→4, Osc2 independent", "depths": ["fm_depth_1_3", "fm_depth_3_4"]},
    {"name": "10: Star (2/3/4 all → 1)", "depths": ["fm_intensity", "fm_depth_3_1", "fm_depth_4_1"]},
    {"name": "11: All independent", "depths": []},
]

# CreamCoat's 17 "Select" room-size presets, in order -- text taken verbatim from the original
# plugin's own per-case comments (e.g. "5 to 51 ms, 96 seat room").
CREAMCOAT_ROOM_PRESETS = [
    "0: 96-Seat Room (5-51ms)",
    "1: 107-Seat Club (7-52ms)",
    "2: 135-Seat Club (8-58ms)",
    "3: 143-Seat Club (7-61ms)",
    "4: 166-Seat Club (8-66ms)",
    "5: 189-Seat Club (9-70ms)",
    "6: 225-Seat Club (7-79ms)",
    "7: 252-Seat Club (11-80ms)",
    "8: 255-Seat Club (8-83ms)",
    "9: 323-Seat Club (10-93ms)",
    "10: 427-Seat Theater (9-110ms)",
    "11: 470-Seat Theater (15-110ms)",
    "12: 606-Seat Theater (11-131ms)",
    "13: 643-Seat Theater (14-132ms)",
    "14: 809-Seat Hall (5-159ms)",
    "15: 984-Seat Hall (10-171ms)",
    "16: 1541-Seat Hall (24-203ms)",
]

# Per-FX-type control metadata: index must match FX_TYPE_* in backend.c exactly. Two param
# kinds:
# - A plain dial entry ({"label", "range", "default"}) always transmits as
#   (raw dial value / 100.0) -- see transmit_knob_packet -- so a param meant to reach the
#   backend as 0.0-1.0 gets a 0-100 range here, and one meant as -1.0..1.0 gets -100..100.
# - A discrete choice ({"label", "type": "choice", "options": [...], "default": index}) shows a
#   dropdown of real option text instead of a knob, and transmits as
#   (selected index / (len(options) - 1)). This matches the Airwindows convention (seen here in
#   CreamCoat's Select) of decoding such a param on the backend as
#   (int)(value * (N - 1 + 0.999)) -- follow that same convention for any future ported effect's
#   own discrete-choice param so this formula keeps working unchanged.
# Slots use only the first len(params) widgets; the rest are hidden (see _apply_fx_slot_type).
# Add a new effect's entry here the same time its FX_TYPE_* case is added to backend.c's
# process_fx_chain -- the two must stay in step.
FX_TYPE_METADATA = [
    {"name": "Off", "params": []},
    {"name": "Ensemble", "params": [
        {"label": "Depth", "range": (0, 100), "default": 50},
        {"label": "FX Level", "range": (0, 100), "default": 100},
    ]},
    {"name": "CreamCoat", "params": [
        {"label": "Select", "type": "choice", "options": CREAMCOAT_ROOM_PRESETS, "default": 8},
        {"label": "Regen", "range": (0, 100), "default": 50},
        {"label": "DeRez", "range": (0, 100), "default": 100},
        {"label": "Predelay", "range": (0, 100), "default": 0},
        {"label": "Wetness", "range": (0, 100), "default": 25},
    ]},
    {"name": "Density3", "params": [
        # Default 50 -> density=2.5 on the backend (well into the sine-clipper range) rather
        # than the original plugin's raw 0.0 init -- a density of 0 is barely audible, a poor
        # first impression of what this effect actually does.
        {"label": "Density", "range": (0, 100), "default": 50},
        {"label": "Highpass", "range": (0, 100), "default": 0},
        {"label": "Output", "range": (0, 100), "default": 100},
        # No Dry/Wet param here -- the slot's generic Mix knob already does that job; see the
        # FX_TYPE_DENSITY3 comment in backend.c.
    ]},
    {"name": "StereoChorus", "params": [
        # Defaults 30/30 rather than the original plugin's raw 0/0 init -- at Depth=0 this
        # effect is a silent no-op (no modulation at all), a poor first impression.
        {"label": "Speed", "range": (0, 100), "default": 30},
        {"label": "Depth", "range": (0, 100), "default": 30},
    ]},
    {"name": "StereoDoubler", "params": [
        {"label": "Detune", "range": (0, 100), "default": 60},
        {"label": "Dry/Wet", "range": (0, 100), "default": 60},
    ]},
]

# Sentinel shown in the MIDI Input combo for auto-detect mode; encoded on the wire as "auto" --
# must match the backend's own "auto"/empty-string convention in midi_client_should_autoconnect.
MIDI_AUTO_LABEL = "Auto (recommended)"

def _list_alsa_devices(list_cmd):
    """Best-effort enumeration of PCM device names via `aplay -L` / `arecord -L`. Falls back to
    just ["default"] if the tool isn't installed, times out, or returns nothing -- "default" is
    always a safe starting point."""
    names = []
    try:
        result = subprocess.run(list_cmd, capture_output=True, text=True, timeout=2)
        # `aplay -L`/`arecord -L` list one device name per unindented line, with indented
        # description lines following each -- only the unindented lines are device names.
        names = [line for line in result.stdout.splitlines() if line and not line[0].isspace()]
    except (OSError, subprocess.SubprocessError):
        pass
    if "default" in names:
        names.remove("default")
    return ["default"] + names

_ACONNECT_CLIENT_RE = re.compile(r"^client\s+(\d+):\s*'(.*)'\s*\[type=(\w+)")

def _list_midi_input_devices():
    """Best-effort enumeration of real MIDI source device names via `aconnect -i` (lists
    input-capable clients, i.e. things that GENERATE MIDI -- keyboards/controllers -- as opposed
    to `aconnect -o`'s output-capable/receiving clients). Falls back to just [MIDI_AUTO_LABEL] if
    the tool isn't installed, times out, or nothing qualifies -- backend.c applies the exact same
    System-client/kernel-type/"Midi Through" filtering independently when it does its own live
    scan, so this list is a preview of what auto-connect would pick up, not the source of truth."""
    names = []
    try:
        result = subprocess.run(["aconnect", "-i"], capture_output=True, text=True, timeout=2)
        for line in result.stdout.splitlines():
            m = _ACONNECT_CLIENT_RE.match(line)
            if not m:
                continue
            client_id, name, client_type = int(m.group(1)), m.group(2), m.group(3)
            if client_id == 0 or client_type != "kernel" or name == "Midi Through":
                continue
            if name not in names:
                names.append(name)
    except (OSError, subprocess.SubprocessError):
        pass
    return [MIDI_AUTO_LABEL] + names

class ResetSlider(QSlider):
    """Vertical volume slider that resets to a default value when double-clicked."""
    def __init__(self, orientation, default_value, parent=None):
        super().__init__(orientation, parent)
        self._default_value = default_value

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setValue(self._default_value)
            event.accept()
class WaveformVisualizer(QWidget):
    """Draws a tiny, minimalist static visualization of the selected waveform."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(45, 20)
        self.waveform_type = 0
        self.is_lfo = True

    def set_waveform(self, index, is_lfo=True):
        self.waveform_type = index
        self.is_lfo = is_lfo
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor("#1e1e24"))

        pen = QPen(QColor("#00adb5"), 2)
        painter.setPen(pen)

        w, h = self.width(), self.height()
        mid_y = h / 2
        margin = 3
        draw_w = w - (margin * 2)
        path = QPainterPath()

        if self.is_lfo:
            if self.waveform_type == 0:  # Triangle
                path.moveTo(margin, mid_y)
                path.lineTo(margin + draw_w/4, margin)
                path.lineTo(margin + 3*draw_w/4, h - margin)
                path.lineTo(margin + draw_w, mid_y)
            elif self.waveform_type == 1:  # Sine
                path.moveTo(margin, mid_y)
                for x in range(int(draw_w)):
                    rel_x = x / draw_w
                    y = mid_y - math.sin(rel_x * 2 * math.pi) * (mid_y - margin)
                    path.lineTo(margin + x, y)
            elif self.waveform_type == 2:  # Saw Down
                path.moveTo(margin, margin)
                path.lineTo(margin + draw_w, h - margin)
                path.lineTo(margin + draw_w, margin)
            elif self.waveform_type == 3:  # Square
                path.moveTo(margin, mid_y)
                path.lineTo(margin, margin)
                path.lineTo(margin + draw_w/2, margin)
                path.lineTo(margin + draw_w/2, h - margin)
                path.lineTo(margin + draw_w, h - margin)
                path.lineTo(margin + draw_w, mid_y)
            elif self.waveform_type == 4:  # Off -- flat dashed line, same convention as the
                # oscillator visualizer's own Off case.
                pen.setStyle(Qt.PenStyle.DashLine)
                painter.setPen(pen)
                path.moveTo(margin, mid_y)
                path.lineTo(margin + draw_w, mid_y)
        else:
            if self.waveform_type == 0:  # Saw Up
                path.moveTo(margin, h - margin)
                path.lineTo(margin + draw_w, margin)
                path.lineTo(margin + draw_w, h - margin)
            elif self.waveform_type == 1:  # Square
                path.moveTo(margin, mid_y)
                path.lineTo(margin, margin)
                path.lineTo(margin + draw_w/2, margin)
                path.lineTo(margin + draw_w/2, h - margin)
                path.lineTo(margin + draw_w, h - margin)
                path.lineTo(margin + draw_w, mid_y)
            elif self.waveform_type == 2:  # Triangle
                path.moveTo(margin, mid_y)
                path.lineTo(margin + draw_w/4, margin)
                path.lineTo(margin + 3*draw_w/4, h - margin)
                path.lineTo(margin + draw_w, mid_y)
            elif self.waveform_type == 3:  # Sine
                path.moveTo(margin, mid_y)
                for x in range(int(draw_w)):
                    rel_x = x / draw_w
                    y = mid_y - math.sin(rel_x * 2 * math.pi) * (mid_y - margin)
                    path.lineTo(margin + x, y)
            elif self.waveform_type == 4:  # Unison Saw
                path.moveTo(margin, h - margin)
                path.lineTo(margin + draw_w/3, margin)
                path.moveTo(margin + draw_w/3, h - margin)
                path.lineTo(margin + 2*draw_w/3, margin)
                path.moveTo(margin + 2*draw_w/3, h - margin)
                path.lineTo(margin + draw_w, margin)
            elif self.waveform_type == 5:  # Noise
                rng = random.Random(42)
                path.moveTo(margin, mid_y)
                steps = 14
                for i in range(1, steps + 1):
                    x = margin + (draw_w * i / steps)
                    y = margin + rng.random() * (h - margin * 2)
                    path.lineTo(x, y)
            elif self.waveform_type == 6:  # Pulse
                duty = draw_w * 0.3
                path.moveTo(margin, mid_y)
                path.lineTo(margin, margin)
                path.lineTo(margin + duty, margin)
                path.lineTo(margin + duty, h - margin)
                path.lineTo(margin + draw_w, h - margin)
                path.lineTo(margin + draw_w, mid_y)
            elif self.waveform_type == 7:  # Sample (DFD playback) -- a small clip/bar icon,
                # visually distinct from Noise's jagged random line, to suggest a fixed recorded
                # waveform rather than a live-generated one.
                bar_heights = [0.3, 0.7, 1.0, 0.5, 0.8, 0.4]
                bar_w = draw_w / (len(bar_heights) * 1.6)
                for i, frac in enumerate(bar_heights):
                    bar_h = (h - margin * 2) * frac
                    x = margin + i * (draw_w / len(bar_heights))
                    path.moveTo(x, mid_y - bar_h / 2)
                    path.lineTo(x, mid_y + bar_h / 2)
                    path.moveTo(x, mid_y - bar_h / 2)
                    path.lineTo(x + bar_w, mid_y - bar_h / 2)
                    path.moveTo(x, mid_y + bar_h / 2)
                    path.lineTo(x + bar_w, mid_y + bar_h / 2)
            elif self.waveform_type == 8:  # Off -- a flat dashed line, distinct from any real
                # waveform shape, signaling "nothing computed here."
                pen.setStyle(Qt.PenStyle.DashLine)
                painter.setPen(pen)
                path.moveTo(margin, mid_y)
                path.lineTo(margin + draw_w, mid_y)

        painter.drawPath(path)
class PotentiometerKnob(QWidget):
    """Custom vector-drawn potentiometer style dial with drag, scrollwheel, click reset,
    a high-visibility pop-up readout, and a 500ms hover delay trigger to view values passively."""
    valueChanged = Signal(int)

    def __init__(self, label_hint="Value", parent=None):
        super().__init__(parent)
        self._min_value = 0
        self._max_value = 100
        self._current_value = 50
        self._label_hint = label_hint
        self.value_formatter = None  # optional callable(int) -> str for custom popup display

        self.setFixedSize(32, 32)
        self.setCursor(Qt.CursorShape.SizeVerCursor)
        self._value_start_drag = 0

        # Drag state configuration
        self._drag_button = None
        self._drag_sensitivity_scale = 1.0
        self._drag_anchor_global = None
        self._suppress_next_move = False

        # Right-click fine-adjustment status flags
        self._right_press_pos = None
        self._right_dragged = False
        self._cursor_hidden = False

        # Optional callable(QPoint) invoked on a plain right-click (press+release with no
        # drag in between) -- lets a caller show a context menu (e.g. LFO depth's "Modulation
        # Source" menu) without stealing the existing right-drag fine-adjustment gesture.
        self._context_menu_callback = None

        # Logarithmic ("audio taper") response: off by default. When enabled via setLogTaper(),
        # drag/wheel input is mapped through log space instead of linearly, so a knob spanning
        # e.g. 1-2000ms gives dramatically finer control near the low end (where fast envelope
        # times live) at the cost of coarser control near the high end -- the same taper real
        # synth hardware uses for time/frequency pots. Requires min_value > 0.
        self._log_taper = False

        self._fade_timer = QTimer(self)
        self._fade_timer.setSingleShot(True)
        self._fade_timer.timeout.connect(self._hide_popup)

        self._hover_timer = QTimer(self)
        self._hover_timer.setSingleShot(True)
        self._hover_timer.timeout.connect(self._show_popup)

    def setContextMenuCallback(self, callback):
        """callback(global_pos: QPoint) is called on a plain right-click (no drag)."""
        self._context_menu_callback = callback

    def setLogTaper(self, enabled=True):
        self._log_taper = enabled

    def _value_to_norm(self, value):
        """Real value -> normalized [0,1] drag/wheel position, honoring the log taper."""
        if not self._log_taper:
            span = self._max_value - self._min_value
            return 0.0 if span <= 0 else (value - self._min_value) / span
        lo = max(1e-6, float(self._min_value))
        hi = max(lo * 1.0001, float(self._max_value))
        v = max(lo, min(hi, float(value)))
        return math.log(v / lo) / math.log(hi / lo)

    def _norm_to_value(self, t):
        """Normalized [0,1] drag/wheel position -> real value, honoring the log taper."""
        t = max(0.0, min(1.0, t))
        if not self._log_taper:
            return self._min_value + t * (self._max_value - self._min_value)
        lo = max(1e-6, float(self._min_value))
        hi = max(lo * 1.0001, float(self._max_value))
        return lo * ((hi / lo) ** t)

    def setRange(self, min_val, max_val):
        self._min_value = min_val
        self._max_value = max_val
        self.setValue(self._current_value)

    def value(self):
        return self._current_value

    def value_range(self):
        return self._min_value, self._max_value

    def setValue(self, val):
        clamped = max(self._min_value, min(self._max_value, val))
        if self._current_value != clamped:
            self._current_value = clamped
            self.valueChanged.emit(self._current_value)
            self.update()
            if self.underMouse() or self.hasFocus():
                self._show_popup()

    def _begin_drag(self, event, sensitivity_scale):
        self._drag_sensitivity_scale = sensitivity_scale
        self._drag_anchor_global = event.globalPosition()
        self._value_start_drag = self._current_value
        self._hide_cursor()

    def _hide_cursor(self):
        if not self._cursor_hidden:
            self._cursor_hidden = True
            QApplication.setOverrideCursor(Qt.CursorShape.BlankCursor)

    def _restore_cursor(self):
        if self._cursor_hidden:
            self._cursor_hidden = False
            QApplication.restoreOverrideCursor()
    def _apply_drag(self, event):
        global_pos = event.globalPosition()
        delta_y = self._drag_anchor_global.y() - global_pos.y()
        sensitivity = 150.0 / self._drag_sensitivity_scale
        if self._log_taper:
            t_start = self._value_to_norm(self._value_start_drag)
            t_new = max(0.0, min(1.0, t_start + delta_y / sensitivity))
            val_delta = int(round(self._norm_to_value(t_new))) - self._value_start_drag
        else:
            range_span = self._max_value - self._min_value
            val_delta = int((delta_y / sensitivity) * range_span)
        if val_delta != 0:
            self.setValue(self._value_start_drag + val_delta)
            self._value_start_drag = self._current_value
            self._suppress_next_move = True
            QCursor.setPos(self._drag_anchor_global.toPoint())

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._hover_timer.stop()
            self._drag_button = "left"
            self._begin_drag(event, sensitivity_scale=1.0)
            self._show_popup()
        elif event.button() == Qt.MouseButton.RightButton:
            self._hover_timer.stop()
            self._right_press_pos = event.position()
            self._right_dragged = False

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setValue(0)
            event.accept()

    def mouseMoveEvent(self, event):
        if self._suppress_next_move:
            self._suppress_next_move = False
            return

        buttons = event.buttons()
        if self._drag_button == "left" and (buttons & Qt.MouseButton.LeftButton):
            self._apply_drag(event)
        elif self._right_press_pos is not None and (buttons & Qt.MouseButton.RightButton):
            if not self._right_dragged:
                moved = (event.position() - self._right_press_pos).manhattanLength()
                if moved > 3:
                    self._right_dragged = True
                    self._drag_button = "right"
                    self._begin_drag(event, sensitivity_scale=0.1)
                    self._show_popup()
            else:
                self._apply_drag(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_button = None
            self._restore_cursor()
            self._hide_popup()
            event.accept()
        elif event.button() == Qt.MouseButton.RightButton:
            was_dragged = self._right_dragged
            self._right_press_pos = None
            self._right_dragged = False
            self._drag_button = None
            self._restore_cursor()
            self._hide_popup()
            if not was_dragged and self._context_menu_callback is not None:
                self._context_menu_callback(event.globalPosition().toPoint())
            event.accept()

    def wheelEvent(self, event: QWheelEvent):
        self._hover_timer.stop()
        degrees = event.angleDelta().y() / 8
        steps = degrees / 15

        if self._log_taper:
            t = self._value_to_norm(self._current_value)
            step_t = 0.02  # 2% of the log-taper range per wheel click
            if steps > 0:
                self.setValue(int(round(self._norm_to_value(t + step_t))))
            elif steps < 0:
                self.setValue(int(round(self._norm_to_value(t - step_t))))
        else:
            range_span = self._max_value - self._min_value
            delta = max(1, int(range_span * 0.02))
            if steps > 0:
                self.setValue(self._current_value + delta)
            elif steps < 0:
                self.setValue(self._current_value - delta)

        self._fade_timer.start(500)
        event.accept()

    def enterEvent(self, event):
        if self.isEnabled() and self.isVisible():
            self._hover_timer.start(500)
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._hover_timer.stop()
        self._hide_popup()
        super().leaveEvent(event)

    def _show_popup(self):
        if not self.isEnabled() or not self.isVisible():
            return
        from PySide6.QtWidgets import QToolTip
        global_pos = self.mapToGlobal(QPointF(self.width() / 2, -15).toPoint())
        display_value = self.value_formatter(self._current_value) if self.value_formatter else str(self._current_value)
        QToolTip.showText(global_pos, f"{self._label_hint}: {display_value}", self)

    def _hide_popup(self):
        from PySide6.QtWidgets import QToolTip
        QToolTip.hideText()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w, h = self.width(), self.height()
        radius = min(w, h) / 2.0 - 2.0
        center_x, center_y = w / 2.0, h / 2.0

        painter.setPen(QPen(QColor("#44444e"), 1.5))
        painter.setBrush(QBrush(QColor("#2b2b36")))
        painter.drawEllipse(QPointF(center_x, center_y), radius, radius)

        span = self._max_value - self._min_value
        fraction = (self._current_value - self._min_value) / float(span) if span > 0 else 0.0
        angle_deg = 135.0 + (fraction * 270.0)
        angle_rad = math.radians(angle_deg)

        pointer_len = radius - 1.0
        target_x = center_x + pointer_len * math.cos(angle_rad)
        target_y = center_y + pointer_len * math.sin(angle_rad)

        color = QColor("#00adb5") if self.isEnabled() else QColor("#445557")
        painter.setPen(QPen(color, 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        painter.drawLine(QPointF(center_x, center_y), QPointF(target_x, target_y))
class InteractiveADSREnvelope(QWidget):
    """Draws an envelope curve and lets the user drag its nodes based on the 7 active types.
    Attack Start Level is controlled by dragging the (0,0) node vertically; Attack Peak Level
    is controlled by dragging the attack node itself vertically."""
    valuesChanged = Signal(int, int, int, int, int, int, int, int)

    def __init__(self, attack_range_ms, decay_range_ms, release_range_ms, parent=None):
        super().__init__(parent)
        self.setMinimumSize(120, 65)

        self.attack_min_ms, self.attack_max_ms = attack_range_ms
        self.decay_min_ms, self.decay_max_ms = decay_range_ms
        self.release_min_ms, self.release_max_ms = release_range_ms

        self.delay = 0.0
        self.hold = 0.0
        self.attack = 0.1
        self.decay = 0.2
        self.sustain = 0.5
        self.release = 0.3
        self.attack_level = 0.0
        self.attack_peak = 1.0
        self.env_type = 0

        self.points = []
        self.point_tags = []
        self.point_anchor_x = []
        self.active_node = None
        self.handle_radius = 4
        self.setMouseTracking(True)

    def set_env_type(self, index):
        self.env_type = index
        self.update()

    def update_values(self, delay_ms, hold_ms, attack_ms, decay_ms, sustain_pct, release_ms, attack_level_pct, attack_peak_pct):
        if self.active_node is None:
            self.delay = delay_ms / 1000.0
            self.hold = hold_ms / 1000.0
            self.attack = attack_ms / 1000.0
            self.decay = decay_ms / 1000.0
            self.sustain = sustain_pct / 100.0
            self.release = release_ms / 1000.0
            self.attack_level = attack_level_pct / 100.0
            self.attack_peak = attack_peak_pct / 100.0
            self.update()

    def calculate_geometry(self):
        w, h = self.width(), self.height()
        margin = 10
        graph_w = w - (margin * 2)
        graph_h = h - (margin * 2)

        t_dly = max(0.01, self.delay) if self.env_type == 5 else 0.0
        t_att = max(0.01, self.attack)
        t_hld = max(0.01, self.hold) if self.env_type in (4, 6) else 0.0
        t_dec = max(0.01, self.decay) if self.env_type in (0, 2, 4, 5, 6) else 0.0
        t_sus = 0.3 if self.env_type in (0, 3, 5, 6) else 0.0
        t_rel = max(0.01, self.release) if self.env_type in (0, 1, 3, 5, 6) else 0.0

        total_time = t_dly + t_att + t_hld + t_dec + t_sus + t_rel
        scale_x = graph_w / total_time
        self._scale_x = scale_x

        x0, y0 = margin, h - margin
        peak_y = margin + (graph_h * (1.0 - self.attack_peak))
        start_y = margin + (graph_h * (1.0 - self.attack_level))

        self.points = []
        self.point_tags = []
        self.point_anchor_x = []

        cur_x = x0

        if self.env_type != 5:
            self.points.append(QPointF(x0, start_y))
            self.point_tags.append("start")
            self.point_anchor_x.append(x0)

        if t_dly > 0:
            cur_x += (t_dly * scale_x)
            self.points.append(QPointF(cur_x, y0))
            self.point_tags.append("delay")
            self.point_anchor_x.append(x0)

        attack_start_x = cur_x
        cur_x += (t_att * scale_x)
        self.points.append(QPointF(cur_x, peak_y))
        self.point_tags.append("attack")
        self.point_anchor_x.append(attack_start_x)

        if t_hld > 0:
            hold_start_x = cur_x
            cur_x += (t_hld * scale_x)
            self.points.append(QPointF(cur_x, peak_y))
            self.point_tags.append("hold")
            self.point_anchor_x.append(hold_start_x)

        if t_dec > 0:
            decay_start_x = cur_x
            cur_x += (t_dec * scale_x)
            sus_y = margin + (graph_h * (1.0 - self.sustain)) if self.env_type in (0, 5, 6) else y0
            self.points.append(QPointF(cur_x, sus_y))
            self.point_tags.append("decay")
            self.point_anchor_x.append(decay_start_x)

        if t_sus > 0:
            sustain_start_x = cur_x
            cur_x += (t_sus * scale_x)
            sus_y = margin + (graph_h * (1.0 - self.sustain))
            self.points.append(QPointF(cur_x, sus_y))
            self.point_tags.append("sustain")
            self.point_anchor_x.append(sustain_start_x)

        if t_rel > 0:
            release_start_x = cur_x
            cur_x += (t_rel * scale_x)
            self.points.append(QPointF(cur_x, y0))
            self.point_tags.append("release")
            self.point_anchor_x.append(release_start_x)

        return x0, y0
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor("#1e1e24"))
        x0, y0 = self.calculate_geometry()

        path = QPainterPath()
        if self.points and self.point_tags[0] == "start":
            path.moveTo(self.points[0])
            remaining_points = self.points[1:]
        else:
            path.moveTo(x0, y0)
            remaining_points = self.points
        for pt in remaining_points:
            path.lineTo(pt)

        painter.setPen(QPen(QColor("#00adb5"), 2))
        painter.drawPath(path)

        for idx, pt in enumerate(self.points):
            if idx == self.active_node:
                painter.setBrush(QBrush(QColor("#ffffff")))
                painter.setPen(QPen(QColor("#00adb5"), 1.5))
            else:
                painter.setBrush(QBrush(QColor("#00adb5")))
                painter.setPen(QPen(QColor("#1e1e24"), 1))
            painter.drawEllipse(pt, self.handle_radius, self.handle_radius)

    def mousePressEvent(self, event):
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pos = event.position()
        for idx, pt in enumerate(self.points):
            if (pos - pt).manhattanLength() < self.handle_radius * 3:
                self.active_node = idx
                self.update()
                return
        for idx, tag in enumerate(self.point_tags):
            if tag not in ("hold", "sustain"):
                continue
            pt = self.points[idx]
            left_x = self.point_anchor_x[idx]
            if left_x is not None and left_x <= pos.x() <= pt.x() and abs(pos.y() - pt.y()) < self.handle_radius * 3:
                self.active_node = idx
                self.update()
                return

    def mouseMoveEvent(self, event):
        if self.active_node is None or not self.points:
            return
        pos = event.position()
        w, h = self.width(), self.height()
        margin = 10
        graph_h = h - (margin * 2)
        scale_x = getattr(self, "_scale_x", 100.0)

        clamped_x = max(margin, min(pos.x(), w - margin))
        clamped_y = max(margin, min(pos.y(), h - margin))
        level_frac = max(0.0, min(1.0, 1.0 - ((clamped_y - margin) / graph_h)))

        tag = self.point_tags[self.active_node]
        anchor_x = self.point_anchor_x[self.active_node]

        if tag == "start":
            self.attack_level = level_frac
        elif tag == "delay":
            self.delay = max(0.001, (clamped_x - anchor_x) / scale_x)
        elif tag == "attack":
            self.attack = max(0.001, (clamped_x - anchor_x) / scale_x)
            self.attack_peak = level_frac
        elif tag == "hold":
            self.hold = max(0.001, (clamped_x - anchor_x) / scale_x)
        elif tag == "decay":
            self.decay = max(0.001, (clamped_x - anchor_x) / scale_x)
            self.sustain = level_frac
        elif tag == "sustain":
            self.sustain = level_frac
        elif tag == "release":
            self.release = max(0.001, (w - margin - clamped_x) / scale_x)

        self.valuesChanged.emit(
            int(self.delay * 1000), int(self.hold * 1000), int(self.attack * 1000),
            int(self.decay * 1000), int(self.sustain * 100), int(self.release * 1000),
            int(self.attack_level * 100), int(self.attack_peak * 100)
        )
        self.update()

    def mouseReleaseEvent(self, event):
        self.active_node = None
        self.update()
ENV_TYPE_KNOB_ORDER = {
    0: ["A", "D_phase", "S", "R"],        # ADSR
    1: ["A", "R"],                         # AR
    2: ["A", "D_phase"],                   # AD
    3: ["A", "S", "R"],                    # ASR
    4: ["A", "H", "D_phase"],              # AHD
    5: ["D", "A", "D_phase", "S", "R"],    # DADSR
    6: ["A", "H", "D_phase", "S", "R"],    # AHDSR
}

class ADSRPanel(QGroupBox):
    """A titled group box panel wrapper that physically shows, hides, and reorders knob
    columns on the screen so the visible knobs always read left-to-right in the same
    order as the letters in the selected envelope type's name."""
    valuesChanged = Signal(int, int, int, int, int, int, int, int)
    typeChanged = Signal(int)

    def __init__(self, title_text, attack_range_ms, decay_range_ms, release_range_ms, parent=None):
        super().__init__("", parent)

        outer = QVBoxLayout()
        outer.setContentsMargins(4, 4, 4, 4); outer.setSpacing(2)

        self.attack_level_pct = 0
        self.attack_peak_pct = 100

        self.title_layout = QHBoxLayout(); self.title_layout.setContentsMargins(6, 2, 6, 2)
        self.title_label = QLabel(f"<b>{title_text}</b>")
        self.title_layout.addWidget(self.title_label)
        self.title_layout.addStretch()
        outer.addLayout(self.title_layout)

        self.graph = InteractiveADSREnvelope(attack_range_ms, decay_range_ms, release_range_ms)
        outer.addWidget(self.graph)

        self.dial_row = QHBoxLayout(); self.dial_row.setSpacing(1)

        type_box = QVBoxLayout(); type_box.setSpacing(0)
        lbl_t = QLabel("Mode"); lbl_t.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_t.setStyleSheet("font-size: 8px; font-weight: bold;")
        type_box.addWidget(lbl_t)
        self.combo_type = QComboBox()
        self.combo_type.addItems(["ADSR", "AR", "AD", "ASR", "AHD", "DADSR", "AHDSR"])
        self.combo_type.setStyleSheet("font-size: 9px; font-weight: bold; color: #00adb5; background-color: #2b2b36;")
        self.combo_type.setFixedWidth(60)
        type_box.addWidget(self.combo_type)
        self.dial_row.addLayout(type_box)

        self.columns = {}
        self.dials = {}

        self.columns["D"], self.dials["D"], self.dly_readout = self._make_dial_column("Dly", (0, 1000), 0, " ms")
        self.columns["H"], self.dials["H"], self.hold_readout = self._make_dial_column("Hld", (0, 1000), 0, " ms")
        self.columns["A"], self.dials["A"], self.a_readout = self._make_dial_column("A", attack_range_ms, 10, " ms", log_taper=True)
        self.columns["D_phase"], self.dials["D_phase"], self.d_readout = self._make_dial_column("Dec", decay_range_ms, 150, " ms", log_taper=True)
        self.columns["S"], self.dials["S"], self.s_readout = self._make_dial_column("S", (0, 100), 80, "%")
        self.columns["R"], self.dials["R"], self.r_readout = self._make_dial_column("R", release_range_ms, 300, " ms", log_taper=True)

        outer.addLayout(self.get_dial_row())
        self.setLayout(outer)

        for dial in self.dials.values():
            dial.valueChanged.connect(self._on_dial_moved)
        self.graph.valuesChanged.connect(self._on_graph_dragged)
        self.combo_type.currentIndexChanged.connect(self._on_type_dropdown_swapped)

        self._on_type_dropdown_swapped(0)

    def get_dial_row(self):
        return self.dial_row

    def _make_dial_column(self, label_text, value_range, default_value, suffix, log_taper=False):
        box_container = QWidget()
        box = QVBoxLayout(box_container); box.setContentsMargins(0, 0, 0, 0); box.setSpacing(0)
        lbl = QLabel(label_text); lbl.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl.setStyleSheet("font-size: 9px; font-weight: bold;")
        box.addWidget(lbl)

        dial = PotentiometerKnob(f"{label_text}", self)
        dial.setRange(*value_range); dial.setValue(default_value)
        if log_taper:
            dial.setLogTaper(True)
        box.addWidget(dial, alignment=Qt.AlignmentFlag.AlignCenter)

        readout = QLabel(f"{default_value}{suffix}"); readout.setAlignment(Qt.AlignmentFlag.AlignCenter); readout.setStyleSheet("font-size: 8px;")
        box.addWidget(readout)

        self.dial_row.addWidget(box_container)
        return box_container, dial, readout

    def _on_type_dropdown_swapped(self, index):
        self.graph.set_env_type(index)
        order = ENV_TYPE_KNOB_ORDER.get(index, ENV_TYPE_KNOB_ORDER[0])
        for key, box in self.columns.items():
            box.setVisible(key in order)
        for key in ("D", "H", "A", "D_phase", "S", "R"):
            self.dial_row.removeWidget(self.columns[key])
        for key in order:
            self.dial_row.addWidget(self.columns[key])
        self.typeChanged.emit(index)

    def _current_values(self):
        return (self.dials["D"].value(), self.dials["H"].value(), self.dials["A"].value(),
                self.dials["D_phase"].value(), self.dials["S"].value(), self.dials["R"].value())

    def _refresh_readouts(self, d, h, a, dec, s, r):
        self.dly_readout.setText(f"{d} ms"); self.hold_readout.setText(f"{h} ms")
        self.a_readout.setText(f"{a} ms"); self.d_readout.setText(f"{dec} ms")
        self.s_readout.setText(f"{s}%"); self.r_readout.setText(f"{r} ms")

    def _on_dial_moved(self):
        d, h, a, dec, s, r = self._current_values()
        self._refresh_readouts(d, h, a, dec, s, r)
        self.graph.update_values(d, h, a, dec, s, r, self.attack_level_pct, self.attack_peak_pct)
        self.valuesChanged.emit(d, h, a, dec, s, r, self.attack_level_pct, self.attack_peak_pct)

    def _on_graph_dragged(self, d, h, a, dec, s, r, al, pk):
        self.attack_level_pct = al
        self.attack_peak_pct = pk
        for dial in self.dials.values(): dial.blockSignals(True)
        self.dials["D"].setValue(d); self.dials["H"].setValue(h); self.dials["A"].setValue(a)
        self.dials["D_phase"].setValue(dec); self.dials["S"].setValue(s); self.dials["R"].setValue(r)
        for dial in self.dials.values(): dial.blockSignals(False)
        self._refresh_readouts(d, h, a, dec, s, r)
        self.valuesChanged.emit(d, h, a, dec, s, r, al, pk)

    def get_state(self):
        d, h, a, dec, s, r = self._current_values()
        return {"type": self.combo_type.currentIndex(), "d": d, "h": h, "a": a,
                "dec": dec, "s": s, "r": r, "al": self.attack_level_pct, "pk": self.attack_peak_pct}

    def set_state(self, state):
        self.combo_type.blockSignals(True)
        self.combo_type.setCurrentIndex(state["type"])
        self.combo_type.blockSignals(False)
        self._on_type_dropdown_swapped(state["type"])

        for dial, key in ((self.dials["D"], "d"), (self.dials["H"], "h"), (self.dials["A"], "a"),
                          (self.dials["D_phase"], "dec"), (self.dials["S"], "s"), (self.dials["R"], "r")):
            dial.blockSignals(True); dial.setValue(state[key]); dial.blockSignals(False)

        self.attack_level_pct = state.get("al", 0)
        self.attack_peak_pct = state.get("pk", 100)
        self._on_dial_moved()

# ==========================================================================================
# MAPPING TAB -- Sample Bank zone editor. Adapted from a standalone prototype the same author
# provided (a drag-and-drop key-range/velocity-range zone grid + a scaled piano keyboard), wired
# here into the real backend instead of the prototype's print()-based stubs: every zone edit
# (drop, drag/resize, delete, root-key change) calls transmit_zone_upsert()/transmit_zone_remove()
# -- a dedicated, event-driven socket, NOT transmit_knob_packet() -- and each SampleZone is synced
# to the backend's g_sample_slots[] catalog by its own stable zone_id (see ZONE_OP_UPSERT/
# ZONE_OP_REMOVE in backend.c), not by array position -- the catalog itself has no product-facing
# cap (see SAMPLE_SLOT_CAPACITY's comment in backend.c).
# ==========================================================================================
class SampleZone:
    """One loaded sample mapped across a key range and a velocity range -- one entry in the
    Sample Bank, synced to the backend's catalog by its own stable zone_id."""
    def __init__(self, file_path, note_number, zone_id):
        self.file_path = file_path
        self.filename = os.path.basename(file_path)
        self.root_note = note_number   # lower bound of the key range
        self.max_note = note_number    # upper bound of the key range
        self.min_velocity = 40
        self.max_velocity = 90
        self.is_selected = False
        self.key_center = note_number  # pitch reference note; -1 = DRUM (unpitched)
        self.zone_id = zone_id  # stable identity for ZONE_OP_UPSERT/ZONE_OP_REMOVE, never reused
        # Loop points as fractions (0.0-1.0) of the file's own length -- whole-file by default,
        # least surprising for a freshly dropped, not-yet-edited sample (see SampleLoopEditor).
        self.loop_start = 0.0
        self.loop_end = 1.0
        self.loop_enabled = True
        self.round_robin_group = 0  # 0 = independent zone; >0 = cycles with same-group zones
        # SFZ-style per-zone params (Zone Inspector's second control row), groundwork toward
        # eventual SFZ import/export -- see backend.c's SampleSlot fields of the same names.
        self.tune_cents = 0       # fine pitch offset in cents
        self.gain_db = 0          # per-zone level trim in dB
        self.pan = 0              # -100 (full left) .. 100 (full right), matches vco1_pan's own
                                   # UI-unit convention -- divided by 100.0 only when packed onto
                                   # the wire, ADDS to (doesn't override) the oscillator's own Pan
                                   # knob on the backend side
        self.start_offset = 0.0   # 0.0-1.0, where in the file playback begins
        self.reverse = False      # plays the file/loop region backward
        # Full SFZ loop_sustain/end= support (appended): loop_release_exit is SFZ's loop_sustain
        # (True) vs loop_continuous (False) -- the loop is exited for good once the owning voice
        # enters release, instead of looping forever. end_frac is SFZ's end= (0.0-1.0, same
        # fraction-of-file convention as loop_start/loop_end/start_offset), where a one-shot's
        # playback is truncated; 1.0 (default) means no truncation. See backend.c's
        # SampleSlot.loop_release_exit/playback_end_frame for how these are actually applied.
        self.loop_release_exit = False
        self.end_frac = 1.0
        # Which oscillator (0-3) this zone belongs to -- see backend.c's SampleSlot.assigned_osc.
        # Set at creation time (drag-drop tags it with the Mapping tab's currently-selected
        # oscillator; SFZ import tags every zone it creates the same way) and otherwise left alone.
        self.assigned_osc = 0

class MidiKeyButton(QPushButton):
    """A single piano key on the Mapping tab's reference keyboard -- visual only (no MIDI is
    sent), used to click-preview where a note falls while editing zones."""
    def __init__(self, note_number, is_black, parent=None):
        super().__init__(parent)
        self.note_number = note_number
        self.is_black = is_black
        self.is_latched = False
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setCheckable(True)
        if not self.is_black and (self.note_number % 12 == 0):
            self.setText(f"C{(self.note_number // 12) - 1}")
        self.update_style()

    def update_style(self):
        if self.is_black:
            self.setStyleSheet("QPushButton { background-color: black; color: white; font-size: 8px; border: 1px solid #111; border-radius: 1px; padding-bottom: 2px; } QPushButton:checked { background-color: #007acc; }")
        else:
            self.setStyleSheet("QPushButton { background-color: white; color: #444; font-size: 9px; font-weight: bold; border: 1px solid #bbb; border-bottom-left-radius: 3px; border-bottom-right-radius: 3px; text-align: bottom; padding-bottom: 6px; } QPushButton:checked { background-color: #0098ff; color: white; }")

class SamplerMappingGrid(QWidget):
    """The interactive key-range/velocity-range zone editor: drag-and-drop a .wav to create a
    zone, drag its body to move it, drag an edge to resize its key or velocity range, marquee-
    select multiple zones, Delete/Backspace to remove selected zones."""
    def __init__(self, parent_window, parent=None):
        super().__init__(parent)
        self.parent_window = parent_window  # the SynthGui instance -- for transmit_knob_packet()/status
        self.key_geometries = {}
        self.black_key_offsets = {1, 3, 6, 8, 10}
        self.sample_zones = []
        self._next_zone_id = 1  # ever-incrementing, never reused/recycled -- see SampleZone.zone_id
        # Which oscillator (0-3) the grid currently displays/edits -- see visible_zones(). The
        # catalog itself (self.sample_zones) stays one flat, session-wide list shared across all
        # 4 oscillators (matching backend.c's own catalog shape); this is purely a view filter.
        self.selected_osc = 0
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.active_zone = None
        self.drag_mode = None
        self.drag_start_pos = QPoint()
        self.marquee_current_pos = QPoint()
        self.zone_start_snapshots = {}

    def update_grid_layout(self, key_geometries):
        self.key_geometries = key_geometries
        self.update()

    def get_note_at_x(self, x_pos):
        for note in range(128):
            if (note % 12) in self.black_key_offsets and note in self.key_geometries:
                lx, w = self.key_geometries[note]
                if lx <= x_pos <= (lx + w): return note
        for note in range(128):
            if (note % 12) not in self.black_key_offsets and note in self.key_geometries:
                lx, w = self.key_geometries[note]
                if lx <= x_pos <= (lx + w): return note
        return None

    def get_y_for_velocity(self, vel): return int((1.0 - (vel / 127.0)) * self.height())
    def get_velocity_for_y(self, y): return max(0, min(127, int((1.0 - (y / self.height())) * 127)))

    def get_zone_rect(self, zone):
        if zone.root_note not in self.key_geometries or zone.max_note not in self.key_geometries: return QRectF()
        lx, _ = self.key_geometries[zone.root_note]
        elx, ew = self.key_geometries[zone.max_note]
        return QRectF(float(lx), float(self.get_y_for_velocity(zone.max_velocity)), float((elx + ew) - lx), float(self.get_y_for_velocity(zone.min_velocity) - self.get_y_for_velocity(zone.max_velocity)))

    def visible_zones(self):
        """Zones belonging to the currently-selected oscillator -- every interaction/rendering
        method below iterates this instead of the full self.sample_zones list, so a zone on a
        different oscillator is neither visible, clickable, nor reachable by a marquee/Delete
        while that oscillator isn't selected."""
        return [z for z in self.sample_zones if z.assigned_osc == self.selected_osc]

    def get_marquee_rect(self): return QRectF(QPointF(self.drag_start_pos), QPointF(self.marquee_current_pos)).normalized()
    def dragEnterEvent(self, e): e.acceptProposedAction() if e.mimeData().hasUrls() else None
    def dragMoveEvent(self, e): e.acceptProposedAction() if e.mimeData().hasUrls() else None

    def add_zone_at_note(self, file_path, note_number):
        """Creates a new zone for file_path at note_number -- no cap on how many zones can exist
        (see SAMPLE_SLOT_CAPACITY's comment in backend.c). Shared by drag-and-drop and the
        "+ Add Sample" fallback button. Keeps its bool return since dropEvent still consumes it."""
        zone_id = self._next_zone_id
        self._next_zone_id += 1
        zone = SampleZone(file_path, note_number, zone_id)
        zone.assigned_osc = self.selected_osc
        self.sample_zones.append(zone)
        if self.parent_window:
            self.parent_window.set_mapping_status(None)
            self.parent_window.transmit_zone_upsert([zone])
        self.update()
        return True

    def dropEvent(self, e):
        if e.mimeData().hasUrls():
            any_added = False
            for url in e.mimeData().urls():
                fp = url.toLocalFile()
                if fp.lower().endswith('.wav'):
                    tn = self.get_note_at_x(e.position().toPoint().x())
                    if tn is not None and self.add_zone_at_note(fp, tn): any_added = True
            e.acceptProposedAction()
            if any_added: self.update()

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            removed_ids = [z.zone_id for z in self.sample_zones if z.is_selected]
            self.sample_zones = [z for z in self.sample_zones if not z.is_selected]
            if removed_ids:
                if self.parent_window:
                    self.parent_window.synchronize_root_key_dropdown(None)
                    self.parent_window.sync_loop_editor_to_zone(None)
                    self.parent_window.set_mapping_status(None)
                self.update()
                if self.parent_window: self.parent_window.transmit_zone_remove(removed_ids)
        else:
            super().keyPressEvent(event)

    def mousePressEvent(self, event):
        if event.button() != Qt.MouseButton.LeftButton: return
        self.setFocus()
        pos = event.position().toPoint()
        for zone in reversed(self.visible_zones()):
            rect = self.get_zone_rect(zone)
            if rect.contains(QPointF(pos)):
                self.active_zone, self.drag_start_pos = zone, pos
                if not zone.is_selected and not (event.modifiers() & Qt.KeyboardModifier.ShiftModifier):
                    for z in self.sample_zones: z.is_selected = False
                zone.is_selected = True

                if self.parent_window:
                    self.parent_window.synchronize_root_key_dropdown(zone)
                    self.parent_window.sync_loop_editor_to_zone(zone)

                self.zone_start_snapshots = {z: (z.root_note, z.max_note, z.min_velocity, z.max_velocity) for z in self.sample_zones if z.is_selected}
                margin = 6
                if abs(pos.x() - rect.left()) < margin: self.drag_mode = "resize_left"
                elif abs(pos.x() - rect.right()) < margin: self.drag_mode = "resize_right"
                elif abs(pos.y() - rect.top()) < margin: self.drag_mode = "resize_top"
                elif abs(pos.y() - rect.bottom()) < margin: self.drag_mode = "resize_bottom"
                else: self.drag_mode = "move"
                return
        if not (event.modifiers() & Qt.KeyboardModifier.ShiftModifier):
            for z in self.sample_zones: z.is_selected = False
        if self.parent_window:
            self.parent_window.synchronize_root_key_dropdown(None)
            self.parent_window.sync_loop_editor_to_zone(None)
        self.drag_mode, self.drag_start_pos, self.marquee_current_pos = "marquee", pos, pos
        self.update()

    def mouseMoveEvent(self, event):
        pos = event.position().toPoint()
        if self.drag_mode is None:
            c_set = False
            for zone in self.visible_zones():
                rect = self.get_zone_rect(zone)
                if rect.contains(QPointF(pos)):
                    margin = 6
                    if abs(pos.x() - rect.left()) < margin or abs(pos.x() - rect.right()) < margin:
                        self.setCursor(QCursor(Qt.CursorShape.SizeHorCursor))
                        c_set = True
                    elif abs(pos.y() - rect.top()) < margin or abs(pos.y() - rect.bottom()) < margin:
                        self.setCursor(QCursor(Qt.CursorShape.SizeVerCursor))
                        c_set = True
                    break
            if not c_set: self.unsetCursor()
        if self.drag_mode is None: return
        if self.drag_mode == "marquee":
            self.marquee_current_pos = pos
            m_rect = self.get_marquee_rect()
            # Gated to the visible oscillator's own zones only -- a marquee drawn while viewing
            # osc 1 must never reach into osc 2-4's zones (which aren't even drawn), or Delete
            # could remove zones the user never saw get selected.
            for zone in self.visible_zones(): zone.is_selected = m_rect.intersects(self.get_zone_rect(zone))
            self.update()
            return
        current_note, start_click_note = self.get_note_at_x(pos.x()), self.get_note_at_x(self.drag_start_pos.x())
        if current_note is None or start_click_note is None: return
        n_off, v_off = current_note - start_click_note, int(((pos.y() - self.drag_start_pos.y()) / self.height()) * 127)
        for zone, snap in self.zone_start_snapshots.items():
            sr, sm, smin, smax = snap
            if self.drag_mode == "resize_left" and (sr + n_off) <= zone.max_note and 0 <= (sr + n_off) < 128: zone.root_note = sr + n_off
            elif self.drag_mode == "resize_right" and (sm + n_off) >= zone.root_note and 0 <= (sm + n_off) < 128: zone.max_note = sm + n_off
            elif self.drag_mode == "resize_top" and max(0, min(127, smax - v_off)) >= zone.min_velocity: zone.max_velocity = max(0, min(127, smax - v_off))
            elif self.drag_mode == "resize_bottom" and max(0, min(127, smin - v_off)) <= zone.max_velocity: zone.min_velocity = max(0, min(127, smin - v_off))
            elif self.drag_mode == "move":
                if 0 <= (sr + n_off) < 128 and 0 <= (sm + n_off) < 128: zone.root_note, zone.max_note = sr + n_off, sm + n_off
                if 0 <= (smin - v_off) <= 127 and 0 <= (smax - v_off) <= 127: zone.min_velocity, zone.max_velocity = smin - v_off, smax - v_off
        self.update()
        if self.parent_window: self.parent_window.transmit_zone_upsert(list(self.zone_start_snapshots.keys()))

    def mouseReleaseEvent(self, event):
        self.active_zone, self.drag_mode = None, None
        self.zone_start_snapshots.clear()
        self.unsetCursor()
        self.update()
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.fillRect(self.rect(), QColor("#1e1e1e"))
        if not self.key_geometries: return
        for note in range(128):
            if (note % 12) not in self.black_key_offsets and note in self.key_geometries:
                lx, w = self.key_geometries[note]
                painter.fillRect(lx, 0, w, self.height(), QColor("#242424") if note % 2 == 0 else QColor("#202020"))
                painter.setPen(QPen(QColor("#2d2d2d"), 1))
                painter.drawLine(lx + w - 1, 0, lx + w - 1, self.height())
        for note in range(128):
            if (note % 12) in self.black_key_offsets and note in self.key_geometries:
                lx, w = self.key_geometries[note]
                painter.fillRect(lx, 0, w, self.height(), QColor("#161616"))
                painter.setPen(QPen(QColor("#111111"), 1))
                painter.drawLine(lx, 0, lx, self.height())
                painter.drawLine(lx + w - 1, 0, lx + w - 1, self.height())
        # Round-robin visual stacking: zones sharing a group typically share the same key+
        # velocity range too (that's the point of grouping them), so drawing them at their real,
        # identical rects would fully hide all but the topmost. Cascade each group's members by a
        # few pixels so every one stays visible and individually clickable -- purely a drawing
        # offset; hit-testing/dragging (get_zone_rect) always use the real, un-cascaded rect.
        visible = self.visible_zones()
        group_members = {}
        for zone in visible:
            if zone.round_robin_group > 0:
                group_members.setdefault(zone.round_robin_group, []).append(zone)
        cascade_offset = {}
        for members in group_members.values():
            if len(members) > 1:
                for idx, zone in enumerate(sorted(members, key=lambda z: z.zone_id)):
                    cascade_offset[id(zone)] = idx * 9

        for zone in visible:
            rect = self.get_zone_rect(zone)
            if rect.isValid():
                offset = cascade_offset.get(id(zone), 0)
                draw_rect = rect.translated(offset, offset) if offset else rect

                painter.fillRect(draw_rect, QColor(0, 152, 255, 150) if zone.is_selected else QColor(80, 80, 80, 110))
                painter.setPen(QPen(QColor("#00c0ff") if zone.is_selected else QColor("#aaaaaa"), 1.5))
                painter.drawRect(draw_rect)

                if zone.key_center != -1 and zone.key_center in self.key_geometries:
                    cx, cw = self.key_geometries[zone.key_center]
                    painter.fillRect(QRectF(float(cx) + offset, draw_rect.bottom() - 4, float(cw), 4), QColor("#ffaa00"))

                if zone.round_robin_group > 0:
                    badge_rect = QRectF(draw_rect.left() + 2, draw_rect.top() + 2, 26, 12)
                    painter.setPen(Qt.PenStyle.NoPen)
                    painter.setBrush(QBrush(_round_robin_group_color(zone.round_robin_group)))
                    painter.drawRoundedRect(badge_rect, 2, 2)
                    painter.setPen(QColor("white"))
                    painter.setFont(QFont("Arial", 7, QFont.Weight.Bold))
                    painter.drawText(badge_rect, Qt.AlignmentFlag.AlignCenter, f"RR{zone.round_robin_group}")

                painter.save()
                painter.setPen(QColor("white"))
                painter.setFont(QFont("Arial", 8, QFont.Weight.Bold))
                painter.translate(draw_rect.left() + (draw_rect.width() / 2) + 3, draw_rect.bottom() - 6)
                painter.rotate(-90)
                painter.drawText(0, 0, painter.fontMetrics().elidedText(zone.filename, Qt.TextElideMode.ElideRight, int(draw_rect.height()) - 12))
                painter.restore()
        if self.drag_mode == "marquee":
            painter.setPen(QPen(QColor(0, 192, 255, 200), 1.5, Qt.PenStyle.DashLine))
            painter.setBrush(QBrush(QColor(0, 152, 255, 40)))
            painter.drawRect(self.get_marquee_rect())

class ScalablePianoKeyboard(QWidget):
    """A fluid-scaling reference keyboard that forwards its key geometry to the mapping grid so
    zone rectangles line up with real key positions. Click/drag previews a key visually (no MIDI
    is sent); right-click latches a key held for reference while editing."""
    def __init__(self, grid_widget, parent=None):
        super().__init__(parent)
        self.grid_widget = grid_widget
        self.white_keys, self.black_keys, self.currently_pressed_key = [], [], None
        self.setFixedHeight(160)
        self.init_keys()

    def init_keys(self):
        self.black_key_offsets = {1, 3, 6, 8, 10}
        for note in range(128):
            is_black = (note % 12) in self.black_key_offsets
            key = MidiKeyButton(note, is_black, self)
            if not is_black: self.white_keys.append(key)
            else: self.black_keys.append(key)

    def resizeEvent(self, event):
        total_width, total_height = self.width(), self.height()
        black_height, white_height = int(total_height * 0.60), total_height

        # The visual key buttons and the mapping grid's snapping lanes are two SEPARATE geometries
        # computed independently -- they are not the same object and don't have to agree pixel-
        # for-pixel. The key buttons are purely a visual/click-preview reference (no MIDI is sent,
        # they don't affect matching at all -- see the class docstring), so they're free to look
        # like a real piano: black keys centered over the seam between their two white neighbors,
        # physically overhanging a sliver of each neighbor's lane, exactly like an actual keyboard.
        white_width = total_width / len(self.white_keys)
        black_width = white_width * 0.65
        current_white_idx = 0
        white_left_positions = {}
        for key in self.white_keys:
            left_pos = int(current_white_idx * white_width)
            actual_w = int((current_white_idx + 1) * white_width) - left_pos
            key.setGeometry(left_pos, 0, actual_w, white_height)
            white_left_positions[key.note_number] = left_pos
            current_white_idx += 1
        for key in self.black_keys:
            base_white_note = key.note_number - 1
            if base_white_note in white_left_positions:
                left_pos = (white_left_positions[base_white_note] + int(white_width)) - int(black_width / 2)
                key.setGeometry(left_pos, 0, int(black_width), black_height)
        for b_key in self.black_keys: b_key.raise_()

        # The mapping grid's lanes are a completely separate, strict left-to-right partition --
        # every note gets its own exclusive, contiguous x-range with zero gap and zero overlap
        # with its neighbors (black lanes stay narrower, 0.65x, for the same visual distinction
        # as the keys above, just placed edge-to-edge rather than overlapping). This is what
        # actually drives get_note_at_x/zone-rect drawing in SamplerMappingGrid -- an overlapping
        # lane here (matching the physically-realistic key shape above) is what let a drop or drag
        # near a boundary resolve to the wrong note, since get_note_at_x checks black keys first.
        # Computed fresh here rather than reusing the key buttons' own rects above, precisely so
        # fixing this can't touch how the keyboard looks.
        total_units = sum(0.65 if (n % 12) in self.black_key_offsets else 1.0 for n in range(128))
        unit_width = total_width / total_units
        grid_positions = {}
        x = 0.0
        for note in range(128):
            w = (0.65 if (note % 12) in self.black_key_offsets else 1.0) * unit_width
            left_pos = int(round(x))
            right_pos = int(round(x + w))
            grid_positions[note] = (left_pos, right_pos - left_pos)
            x += w
        if self.grid_widget: self.grid_widget.update_grid_layout(grid_positions)

    def getKeyAtPos(self, pos: QPoint):
        for key in self.black_keys:
            if key.geometry().contains(pos): return key
        for key in self.white_keys:
            if key.geometry().contains(pos): return key
        return None

    def calculate_velocity(self, key, pos: QPoint):
        return max(1, min(int((max(0, min(pos.y() - key.y(), key.height())) / key.height()) * 127), 127))

    def update_mouse_playback(self, pos: QPoint):
        target_key = self.getKeyAtPos(pos)
        if target_key != self.currently_pressed_key:
            if self.currently_pressed_key and not self.currently_pressed_key.is_latched: self.currently_pressed_key.setChecked(False)
            self.currently_pressed_key = target_key
            if self.currently_pressed_key and not self.currently_pressed_key.is_latched:
                self.currently_pressed_key.setChecked(True)

    def mousePressEvent(self, event):
        pos = event.position().toPoint()
        target_key = self.getKeyAtPos(pos)
        if not target_key: return
        if event.button() == Qt.MouseButton.LeftButton: self.update_mouse_playback(pos)
        elif event.button() == Qt.MouseButton.RightButton:
            target_key.is_latched = not target_key.is_latched
            target_key.setChecked(target_key.is_latched)

    def mouseMoveEvent(self, event):
        if event.buttons() & Qt.MouseButton.LeftButton: self.update_mouse_playback(event.position().toPoint())

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            if self.currently_pressed_key and not self.currently_pressed_key.is_latched: self.currently_pressed_key.setChecked(False)
            self.currently_pressed_key = None

class SampleLoopEditor(QWidget):
    """Waveform display + draggable loop-start/loop-end markers for the Mapping tab's currently
    selected zone. Decodes each zone's WAV file once (cached by path, purely for this picture --
    entirely separate from the backend's own DFD streaming/pre-roll cache) and sends loop_start/
    loop_end back to the backend as fractions of the file's length whenever a marker moves.
    Dragging inside the shaded region (not on either marker) slides the whole loop, preserving
    its length; scrolling anywhere over the widget resizes it symmetrically around its center."""
    def __init__(self, parent_window, parent=None):
        super().__init__(parent)
        self.parent_window = parent_window
        self.setMinimumHeight(110)
        self.setMouseTracking(True)
        self.zone = None
        self._peaks_cache = {}  # file_path -> (mins, maxs, total_frames) or None (decode failed)
        self._drag_target = None  # "start" | "end" | "slide" | None
        self._slide_click_offset = 0.0  # frac(click) - loop_start at drag start, only for "slide"

    def _get_peaks(self, path):
        if path not in self._peaks_cache:
            self._peaks_cache[path] = _decode_wav_peaks(path)
        return self._peaks_cache[path]

    def set_zone(self, zone):
        self.zone = zone
        self._drag_target = None
        self.update()

    def _margin(self): return 6
    def _marker_x(self, frac):
        m = self._margin()
        return m + frac * (self.width() - m * 2)

    def _x_to_frac(self, x):
        m = self._margin()
        w = max(1, self.width() - m * 2)
        return max(0.0, min(1.0, (x - m) / w))

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.fillRect(self.rect(), QColor("#1a1a1f"))
        if self.zone is None:
            painter.setPen(QColor("#666"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Select a single zone to edit its loop points")
            return

        peaks = self._get_peaks(self.zone.file_path)
        m = self._margin()
        w = self.width() - m * 2
        h = self.height()
        mid_y = h / 2

        if peaks is None:
            painter.setPen(QColor("#a55"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Couldn't read waveform for this file")
        else:
            mins, maxs, _total = peaks
            n = len(mins)
            bar_w = w / n if n else w
            painter.setPen(QPen(QColor("#00adb5"), 1))
            for i in range(n):
                x = m + i * bar_w
                y_top = mid_y - float(maxs[i]) * (mid_y - 4)
                y_bot = mid_y - float(mins[i]) * (mid_y - 4)
                painter.drawLine(int(x), int(y_top), int(x), int(y_bot))

        loop_x0 = self._marker_x(self.zone.loop_start)
        loop_x1 = self._marker_x(self.zone.loop_end)
        shade = QColor(0, 173, 181, 45) if self.zone.loop_enabled else QColor(120, 120, 120, 35)
        painter.fillRect(QRectF(loop_x0, 0, loop_x1 - loop_x0, h), shade)

        for frac, color in ((self.zone.loop_start, QColor("#ffaa00")), (self.zone.loop_end, QColor("#ff5555"))):
            x = self._marker_x(frac)
            painter.setPen(QPen(color, 2))
            painter.drawLine(int(x), 0, int(x), h)

    def _marker_hit(self, x, frac):
        return abs(x - self._marker_x(frac)) < 6

    def _closer_marker(self, frac):
        return "start" if abs(frac - self.zone.loop_start) <= abs(frac - self.zone.loop_end) else "end"

    def mousePressEvent(self, event):
        if self.zone is None: return
        x = event.position().x()
        frac = self._x_to_frac(x)
        if event.button() == Qt.MouseButton.LeftButton:
            if self._marker_hit(x, self.zone.loop_end): self._drag_target = "end"  # end marker checked
            elif self._marker_hit(x, self.zone.loop_start): self._drag_target = "start"  # first, in case they overlap
            elif self.zone.loop_start < frac < self.zone.loop_end:
                # Clicked inside the shaded region itself, not on either marker -- slide the whole
                # loop, preserving its length. Remember the click's offset from loop_start so the
                # grabbed point stays under the cursor instead of the region jumping to re-center.
                self._drag_target = "slide"
                self._slide_click_offset = frac - self.zone.loop_start
            else:
                self._drag_target = None
        elif event.button() == Qt.MouseButton.RightButton:
            # Right-click+drag anywhere -- inside the region, outside it, doesn't matter -- grabs
            # whichever marker (start or end) is currently closer, a faster way to nudge whichever
            # end you mean without having to land the cursor precisely on its thin marker line.
            self._drag_target = self._closer_marker(frac)

    def mouseMoveEvent(self, event):
        if self.zone is None: return
        x = event.position().x()
        if self._drag_target is None:
            near_marker = self._marker_hit(x, self.zone.loop_start) or self._marker_hit(x, self.zone.loop_end)
            inside_body = self.zone.loop_start < self._x_to_frac(x) < self.zone.loop_end
            if near_marker: self.setCursor(QCursor(Qt.CursorShape.SizeHorCursor))
            elif inside_body: self.setCursor(QCursor(Qt.CursorShape.SizeAllCursor))
            else: self.setCursor(QCursor(Qt.CursorShape.ArrowCursor))
            return
        frac = self._x_to_frac(x)
        min_gap = 0.002  # keep start strictly before end -- a visually tiny but real minimum gap
        if self._drag_target == "start":
            self.zone.loop_start = min(frac, self.zone.loop_end - min_gap)
        elif self._drag_target == "end":
            self.zone.loop_end = max(frac, self.zone.loop_start + min_gap)
        else:  # "slide"
            length = self.zone.loop_end - self.zone.loop_start
            new_start = max(0.0, min(1.0 - length, frac - self._slide_click_offset))
            self.zone.loop_start, self.zone.loop_end = new_start, new_start + length
        self.update()
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def mouseReleaseEvent(self, event):
        self._drag_target = None

    def wheelEvent(self, event):
        """Resizes the loop region symmetrically around its own center, one step per wheel notch
        -- works whether the cursor is just hovering over the widget or a marker/slide drag is
        already in progress, since this is an independent event from the mouse-button drags above."""
        if self.zone is None: return
        notches = event.angleDelta().y() / 120.0  # +1.0 per standard wheel click; fractional for trackpads
        if notches == 0: return
        step = 0.01 * notches  # 1% of the file's length per notch, applied to EACH side
        min_gap = 0.002
        new_start = max(0.0, self.zone.loop_start - step)
        new_end = min(1.0, self.zone.loop_end + step)
        if new_end - new_start < min_gap:
            # Shrunk past the minimum -- collapse symmetrically around the center instead of
            # letting start/end cross each other.
            center = (self.zone.loop_start + self.zone.loop_end) / 2.0
            new_start = max(0.0, min(1.0 - min_gap, center - min_gap / 2.0))
            new_end = new_start + min_gap
        self.zone.loop_start, self.zone.loop_end = new_start, new_end
        self.update()
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])
        event.accept()

class ZoneInspectorPanel(QWidget):
    """Per-selected-zone controls for the Mapping tab: loop enable + the waveform/marker editor
    above, Round-Robin Group alongside. Shows a placeholder instead of the whole panel content
    when zero or more than one zone is selected -- editing loop points or a round-robin group for
    several zones at once doesn't make sense, each has its own file/waveform."""
    def __init__(self, parent_window, parent=None):
        super().__init__(parent)
        self.parent_window = parent_window
        self.zone = None

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(4)

        header = QHBoxLayout()
        self.filename_label = QLabel("No zone selected")
        self.filename_label.setStyleSheet("font-size: 10px; color: #aaa;")
        header.addWidget(self.filename_label, stretch=1)

        self.loop_enabled_cb = QCheckBox("Loop")
        self.loop_enabled_cb.setToolTip("On: sustains by looping between the markers below while a note is held.\nOff: plays once and stops (one-shot) -- e.g. drum hits.")
        self.loop_enabled_cb.toggled.connect(self._on_loop_enabled_toggled)
        header.addWidget(self.loop_enabled_cb)

        header.addWidget(QLabel("Round-Robin Group:"))
        self.round_robin_spin = QSpinBox()
        self.round_robin_spin.setRange(0, MAX_ROUND_ROBIN_GROUPS)
        self.round_robin_spin.setSpecialValueText("None")
        self.round_robin_spin.setToolTip("0 = independent zone. Give two or more overlapping zones the same non-zero\ngroup number to cycle through them on successive hits instead of always\nplaying the same one -- e.g. several velocity-matched snare hit variations.")
        self.round_robin_spin.valueChanged.connect(self._on_round_robin_changed)
        header.addWidget(self.round_robin_spin)
        outer.addLayout(header)

        # SFZ-style per-zone params row -- groundwork toward eventual SFZ import/export.
        params_row = QHBoxLayout()
        params_row.addWidget(QLabel("Tune:"))
        self.tune_spin = QSpinBox()
        self.tune_spin.setRange(-100, 100)
        self.tune_spin.setSuffix(" c")
        self.tune_spin.setToolTip("Fine pitch offset in cents, independent of the Root Key dropdown above.")
        self.tune_spin.valueChanged.connect(self._on_tune_changed)
        params_row.addWidget(self.tune_spin)

        params_row.addWidget(QLabel("Gain:"))
        self.gain_spin = QSpinBox()
        self.gain_spin.setRange(-24, 12)
        self.gain_spin.setSuffix(" dB")
        self.gain_spin.setToolTip("Per-zone level trim -- useful for balancing round-robin/velocity-layer variations against each other.")
        self.gain_spin.valueChanged.connect(self._on_gain_changed)
        params_row.addWidget(self.gain_spin)

        params_row.addWidget(QLabel("Pan:"))
        self.pan_spin = QSpinBox()
        self.pan_spin.setRange(-100, 100)
        self.pan_spin.setToolTip("-100 (full left) .. 100 (full right). Adds to the oscillator's own Pan knob (Play tab), doesn't override it.")
        self.pan_spin.valueChanged.connect(self._on_pan_changed)
        params_row.addWidget(self.pan_spin)

        params_row.addWidget(QLabel("Start Offset:"))
        self.start_offset_spin = QSpinBox()
        self.start_offset_spin.setRange(0, 100)
        self.start_offset_spin.setSuffix(" %")
        self.start_offset_spin.setToolTip("Where in the file playback begins, as a percentage of its length.")
        self.start_offset_spin.valueChanged.connect(self._on_start_offset_changed)
        params_row.addWidget(self.start_offset_spin)

        self.reverse_cb = QCheckBox("Reverse")
        self.reverse_cb.setToolTip("Plays the file (or loop region) backward.")
        self.reverse_cb.toggled.connect(self._on_reverse_toggled)
        params_row.addWidget(self.reverse_cb)
        params_row.addStretch(1)
        outer.addLayout(params_row)

        self.waveform = SampleLoopEditor(parent_window=parent_window)
        outer.addWidget(self.waveform)

        self.set_zone(None)

    def set_zone(self, zone):
        self.zone = zone
        for w in (self.loop_enabled_cb, self.round_robin_spin, self.tune_spin, self.gain_spin,
                  self.pan_spin, self.start_offset_spin, self.reverse_cb):
            w.blockSignals(True)
        if zone is None:
            self.filename_label.setText("No zone selected")
            self.loop_enabled_cb.setChecked(True)
            self.round_robin_spin.setValue(0)
            self.tune_spin.setValue(0)
            self.gain_spin.setValue(0)
            self.pan_spin.setValue(0)
            self.start_offset_spin.setValue(0)
            self.reverse_cb.setChecked(False)
            for w in (self.loop_enabled_cb, self.round_robin_spin, self.tune_spin, self.gain_spin,
                      self.pan_spin, self.start_offset_spin, self.reverse_cb):
                w.setEnabled(False)
        else:
            self.filename_label.setText(zone.filename)
            self.loop_enabled_cb.setChecked(zone.loop_enabled)
            self.round_robin_spin.setValue(zone.round_robin_group)
            self.tune_spin.setValue(zone.tune_cents)
            self.gain_spin.setValue(zone.gain_db)
            self.pan_spin.setValue(zone.pan)
            self.start_offset_spin.setValue(int(round(zone.start_offset * 100)))
            self.reverse_cb.setChecked(zone.reverse)
            for w in (self.loop_enabled_cb, self.round_robin_spin, self.tune_spin, self.gain_spin,
                      self.pan_spin, self.start_offset_spin, self.reverse_cb):
                w.setEnabled(True)
        for w in (self.loop_enabled_cb, self.round_robin_spin, self.tune_spin, self.gain_spin,
                  self.pan_spin, self.start_offset_spin, self.reverse_cb):
            w.blockSignals(False)
        self.waveform.set_zone(zone)

    def _on_loop_enabled_toggled(self, checked):
        if self.zone is None: return
        self.zone.loop_enabled = checked
        self.waveform.update()
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_round_robin_changed(self, value):
        if self.zone is None: return
        self.zone.round_robin_group = value
        if self.parent_window: self.parent_window.mapping_grid.update()  # redraw group badges
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_tune_changed(self, value):
        if self.zone is None: return
        self.zone.tune_cents = value
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_gain_changed(self, value):
        if self.zone is None: return
        self.zone.gain_db = value
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_pan_changed(self, value):
        if self.zone is None: return
        self.zone.pan = value
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_start_offset_changed(self, value):
        if self.zone is None: return
        self.zone.start_offset = value / 100.0
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

    def _on_reverse_toggled(self, checked):
        if self.zone is None: return
        self.zone.reverse = checked
        # A reverse zone left at the default start_offset=0.0 plays essentially nothing
        # (immediate exhaustion at frame 0) -- a likely trap with no good backend-side fix, so
        # raise it to the far end automatically the first time Reverse is turned on, purely as a
        # UI convenience. Only when it's still untouched (0.0); never overrides a value the user
        # already set, and toggling Reverse back off leaves whatever offset is there by then.
        if checked and self.zone.start_offset == 0.0:
            self.zone.start_offset = 1.0
            self.start_offset_spin.blockSignals(True)
            self.start_offset_spin.setValue(100)
            self.start_offset_spin.blockSignals(False)
        if self.parent_window: self.parent_window.transmit_zone_upsert([self.zone])

# Program save/load: which OSC addresses count as "the Program" (saved/restored by
# Save/Load) vs. "the rig" (device selection, recording, routing/ext-in, hardware performance
# tuning, and Combi/Sequencer's own session-level stacking state -- none of which belong inside
# a single self-contained patch file). See the plan's own field-scope section for the reasoning
# behind each exclusion. One exception that isn't a plain prefix rule: /hds/fx_routing/%d/mode is
# per-SOURCE (0=Program, 1-8=ext-in pairs) -- index 0 is genuinely patch content (the Program's
# own FX Series/Parallel choice), the rest are rig-level.
_PROGRAM_SCOPE_EXCLUDED_PREFIXES = ("/hds/device/", "/hds/record/", "/hds/routing/", "/hds/ext_in/", "/hds/audio/",
                                    # combi_lane_midi_channel/key_lo/key_hi/vel_lo/vel_hi (the
                                    # only /hds/combi/ addresses transmit_knob_packet's own
                                    # widget loop ever populates into _last_wire_state -- a
                                    # lane's own ProgramState content, e.g. /hds/combi/0/vco1/
                                    # shape, is sent separately by _apply_program_fields_to_
                                    # combi_lane and never reaches _last_wire_state at all) are
                                    # session-level Combi performance setup, not patch content.
                                    "/hds/combi/")
def _is_program_scope_address(addr):
    if addr == "/hds/performance_mode" or addr.startswith("/hds/seq/"):
        return False
    # master_volume is deliberately excluded -- a mixing/session-level level, not part of "the
    # patch" itself (per the user's explicit direction), so loading a Program never yanks the
    # current output level out from under whatever it was set to.
    if addr == "/hds/master_volume":
        return False
    # transpose_semitones is deliberately excluded too -- a live performance control (like a
    # hardware keyboard's own Transpose button), not a patch property, so switching Programs
    # never yanks the performer's current transposition out from under them.
    if addr == "/hds/transpose_semitones":
        return False
    if addr.startswith(_PROGRAM_SCOPE_EXCLUDED_PREFIXES):
        return False
    if addr.startswith("/hds/fx/") and addr.endswith("/source"):
        return False
    if addr.startswith("/hds/fx_routing/"):
        return addr == "/hds/fx_routing/0/mode"
    return True

class SynthGui(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("YourSynth Custom Potentiometer Console")
        self.setGeometry(30, 30, 1350, 680)  # Height reduced from 950 to 680 since window footprint is optimized

        # OSC control protocol (see backend.c's OSC_PARAMS[]/the "Control protocol: OSC" section
        # of README.md): one UDP client sends every knob + the Sample Bank catalog to the
        # backend's --osc-listen-port (default 9000); one plain non-blocking UDP socket receives
        # /hds/status/* feedback on --osc-reply-port (default 9001). Defaults to localhost, but
        # this frontend and the backend can run on separate machines -- see
        # OSC_HOST/OSC_BIND_HOST/OSC_SEND_PORT/OSC_RECV_PORT below the imports for how.
        self.osc_client = None
        self.status_sock = None
        self.mock_mode = True  # connect_to_backend() below owns the real initial value
        # UDP has no connect-time liveness signal (unlike the old stream socket, whose connect()
        # failing meant "backend not running") -- so "is the backend actually there" is now
        # inferred from whether a status bundle has arrived recently. See
        # listen_for_status_frames()/_backend_is_live().
        self._last_status_time = 0.0
        self._last_detected_input_channels = -1
        # Program save/load: transmit_knob_packet()'s own add_f()/add_s() closures keep this
        # dict current on every send (see their own comment there) -- a complete, always-fresh
        # address->value snapshot of everything this GUI last transmitted, with zero duplicated
        # widget-reading logic. Program Save filters this through an allowlist rather than
        # re-deriving values from widgets a second time.
        self._last_wire_state = {}
        # "Pull current state on connect" (see _apply_full_state_fields()): accumulates the
        # backend's chunked /hds/request_state reply as it streams in across possibly several
        # listen_for_status_frames() polls, keyed by address exactly like _last_wire_state above,
        # until /hds/status/state_dump_complete arrives.
        self._state_dump_buffer = {}
        # Reflected by _update_seq_sync_lock_label() next to the Sequencer tab's Sync combo -- see
        # /hds/status/midi_clock_locked's own comment in listen_for_status_frames().
        self._midi_clock_locked = False
        # Program save/load: one subfolder per Program (~/HDS_Programs/<name>/<name>.hdsprog
        # [+ <name>.sfz]) -- mirrors the existing ~/HDS_Recordings convention (self.record_folder).
        self.programs_folder = os.path.expanduser("~/HDS_Programs")
        # Combi save/load: one subfolder per Combi (~/HDS_Combis/<name>/<name>.hdscombi), same
        # convention as programs_folder above -- see _pick_and_save_combi()'s own docstring.
        self.combis_folder = os.path.expanduser("~/HDS_Combis")
        # Song save/load: one subfolder per song (~/HDS_Songs/<name>/song.json + song_midi.dat +
        # audio/lane_<i>.wav) -- see _pick_and_save_song()'s own docstring.
        self.songs_folder = os.path.expanduser("~/HDS_Songs")
        # Song Markers (Sequencer tab): (time_sec, label) pairs, either typed in by hand or
        # populated by importing a Standard MIDI File's own text markers (0xFF 0x06 -- see
        # _pick_and_import_midi()/backend.c's hds_song_import_smf()). Frontend-authoritative, same
        # as seq_lane_rows' own widget state -- the backend only ever REPORTS markers back once,
        # right after an import; from then on this list (and song.json, via Song save/load) is
        # the source of truth, not a live backend knob.
        self.song_markers = []  # list of (time_sec: float, label: str), kept sorted by time
        # Imported tempo map cache: transmit_knob_packet() must resend these EXACT values on every
        # call (it blasts the whole live-knob state on any change) or the next unrelated knob
        # tweak would silently wipe an imported tempo map back to flat/zero -- see that method's
        # own comment on this block, and song_import_smf_handler()'s reply in backend.c which
        # populates this list right after a successful tempo import.
        self._imported_tempo_markers = []  # list of (time_sec: float, bpm: float), <= TEMPO_MARKER_MAX
        self.seq_transport_seek_to_target_sec = 0.0
        self.seq_transport_seek_to_trigger = 0
        # The currently-loaded Program's category (schema field alongside "fields"/"name" --
        # pure metadata, never applied to any OSC address), so re-saving after a Load defaults
        # to the category it was already filed under instead of forgetting it every time.
        self._current_program_category = ""
        self.connect_to_backend()

        # The master outer structural layout
        outer_layout = QHBoxLayout()
        outer_layout.setContentsMargins(6, 6, 6, 6); outer_layout.setSpacing(6)

        # ----------------------------------------------------
        # THE TOUCH TAB DECK (ON THE RIGHT SIDE)
        # ----------------------------------------------------
        self.tab_widget = QTabWidget()
        self.tab_widget.setStyleSheet("QTabBar::tab { font-size: 13px; font-weight: bold; padding: 12px 18px; }")

        # Create independent content views for our tab deck
        self.tab_play = QWidget()  # Renamed from tab_oscillators to tab_play
        self.tab_filters = QWidget()
        self.tab_lfos = QWidget()
        self.tab_egs = QWidget()
        self.tab_fx = QWidget()
        self.tab_routing = QWidget()
        self.tab_audio = QWidget()
        self.tab_recording = QWidget()
        self.tab_mapping = QWidget()
        self.tab_sequencer = QWidget()  # Combi/Sequencer scaffolding -- always reachable regardless of Performance Mode

        # ----------------------------------------------------
        # GLOBAL CONTROLS: live in the always-visible left sidebar (not a tab) so Program,
        # Poly/Mono, Hard Sync, Pitch Bend Range, Sustain Polarity, FM, and Glide are reachable
        # no matter which tab is open -- there used to be a dedicated Global tab, but with the
        # Filter Routing Matrix moved onto the Filters tab (it's filter-specific, not global),
        # everything else here fit naturally into the sidebar instead, so the tab was removed.
        # ----------------------------------------------------
        global_group = QGroupBox("Global Configurations")
        global_layout = QVBoxLayout(); global_layout.setSpacing(4)

        # Program management bar
        prog_box = QGroupBox("")
        prog_outer_lay = QVBoxLayout(); prog_outer_lay.setSpacing(2); prog_outer_lay.setContentsMargins(2, 2, 2, 2)

        prog_title_row = QHBoxLayout(); prog_title_row.setContentsMargins(0, 0, 0, 0); prog_title_row.setSpacing(2)
        prog_title_row.addWidget(QLabel("<b>Program</b>"))
        prog_title_row.addStretch()
        self.btn_load = QPushButton("Load"); self.btn_load.setFixedSize(45, 22); self.btn_load.setStyleSheet("font-size: 10px; font-weight: bold;")
        self.btn_save = QPushButton("Save"); self.btn_save.setFixedSize(45, 22); self.btn_save.setStyleSheet("font-size: 10px; font-weight: bold;")
        prog_title_row.addWidget(self.btn_load); prog_title_row.addWidget(self.btn_save)
        prog_outer_lay.addLayout(prog_title_row)

        prog_lay = QHBoxLayout(); prog_lay.setSpacing(2); prog_lay.setContentsMargins(0, 0, 0, 0)
        self.btn_prev_prog = QPushButton("◀"); self.btn_prev_prog.setFixedSize(30, 26)
        prog_lay.addWidget(self.btn_prev_prog)

        self.program_name_field = QLineEdit()
        # Real placeholder text, not literal contents -- it should vanish once the user types
        # their own name instead of having to be selected-and-deleted first.
        self.program_name_field.setPlaceholderText("Unsaved Program")
        self.program_name_field.setStyleSheet("color: #00adb5; background-color: #1e1e24; font-weight: bold; font-size: 12px; padding: 3px;")
        prog_lay.addWidget(self.program_name_field)

        self.btn_next_prog = QPushButton("▶"); self.btn_next_prog.setFixedSize(30, 26)
        prog_lay.addWidget(self.btn_next_prog)
        prog_outer_lay.addLayout(prog_lay)
        prog_box.setLayout(prog_outer_lay)

        global_layout.addWidget(prog_box)
        global_layout.addSpacing(4)
        self.poly_radio = QRadioButton("Poly")
        self.poly_radio.setChecked(True)
        self.mono_radio = QRadioButton("Mono Legato")
        global_layout.addWidget(self.poly_radio)
        global_layout.addWidget(self.mono_radio)
        global_layout.addSpacing(4)

        hard_sync_box = QGroupBox("Hard Sync")
        hard_sync_lay = QVBoxLayout(); hard_sync_lay.setContentsMargins(4, 4, 4, 4)
        self.hard_sync_combo = QComboBox()
        self.hard_sync_combo.addItems(["Off", "Parallel", "Serial"])
        self.hard_sync_combo.setToolTip(
            "Off: no hard sync.\n"
            "Parallel: Osc 1 resets Osc 2, 3, AND 4 independently -- each slave shares Osc 1's\n"
            "fundamental but keeps its own shape/detune.\n"
            "Serial: Osc 1 resets Osc 2, Osc 2's reset resets Osc 3, Osc 3's reset resets Osc 4 --\n"
            "a cascading \"telephone game\" of phase truncation."
        )
        hard_sync_lay.addWidget(self.hard_sync_combo)
        hard_sync_box.setLayout(hard_sync_lay)
        global_layout.addWidget(hard_sync_box)
        global_layout.addSpacing(4)

        pb_box = QGroupBox("Pitch Bend Range")
        pb_lay = QHBoxLayout(); pb_lay.setSpacing(6); pb_lay.setContentsMargins(4, 4, 4, 4)

        pb_up_col = QVBoxLayout(); pb_up_col.setSpacing(0)
        pb_up_col.addWidget(QLabel("Up"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.pb_range_up_spin = QSpinBox()
        self.pb_range_up_spin.setRange(0, 24)
        self.pb_range_up_spin.setValue(2)
        self.pb_range_up_spin.setSuffix(" st")
        self.pb_range_up_spin.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.pb_range_up_spin.setFixedWidth(58)
        pb_up_col.addWidget(self.pb_range_up_spin, alignment=Qt.AlignmentFlag.AlignCenter)
        pb_lay.addLayout(pb_up_col)

        pb_down_col = QVBoxLayout(); pb_down_col.setSpacing(0)
        pb_down_col.addWidget(QLabel("Down"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.pb_range_down_spin = QSpinBox()
        self.pb_range_down_spin.setRange(0, 24)
        self.pb_range_down_spin.setValue(2)
        self.pb_range_down_spin.setSuffix(" st")
        self.pb_range_down_spin.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.pb_range_down_spin.setFixedWidth(58)
        pb_down_col.addWidget(self.pb_range_down_spin, alignment=Qt.AlignmentFlag.AlignCenter)
        pb_lay.addLayout(pb_down_col)

        pb_box.setLayout(pb_lay)
        global_layout.addWidget(pb_box)
        global_layout.addSpacing(4)

        # Global performance Transpose (-16..16 semitones, centered at 0) -- a live performance
        # control, not a patch property (mirrors master_volume's own "session-level, excluded
        # from Program save/load" design -- see _is_program_scope_address), so switching
        # Programs never yanks the performer's current transposition out from under them.
        global_layout.addWidget(QLabel("Transpose"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.transpose_dial = PotentiometerKnob("Transpose", self)
        self.transpose_dial.setRange(-16, 16); self.transpose_dial.setValue(0)
        global_layout.addWidget(self.transpose_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.transpose_readout = QLabel("0 st"); self.transpose_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        global_layout.addWidget(self.transpose_readout)
        global_layout.addSpacing(4)

        # Microtuning (Scala .scl import) -- session-level, same "live performance control, not a
        # patch property" placement as Transpose just above (see backend.c's tuning_enable
        # comment). Off by default: standard 12-TET everywhere until a scale is loaded AND enabled.
        tuning_box = QGroupBox("Microtuning")
        tuning_lay = QVBoxLayout(); tuning_lay.setSpacing(4); tuning_lay.setContentsMargins(4, 4, 4, 4)
        self.tuning_enable_check = QCheckBox("Enable")
        self.tuning_enable_check.setToolTip("When off, always standard 12-tone equal temperament, regardless of any scale loaded below.")
        tuning_lay.addWidget(self.tuning_enable_check)
        self.btn_load_scale = QPushButton("Load Scale (.scl)…")
        tuning_lay.addWidget(self.btn_load_scale)
        self.tuning_scale_label = QLabel("No scale loaded (12-TET)")
        self.tuning_scale_label.setStyleSheet("font-size: 9px; color: #888;")
        self.tuning_scale_label.setWordWrap(True)
        tuning_lay.addWidget(self.tuning_scale_label)
        ref_row = QHBoxLayout(); ref_row.setSpacing(4)
        ref_row.addWidget(QLabel("Ref Note:"))
        self.tuning_ref_note_spin = QSpinBox()
        self.tuning_ref_note_spin.setRange(0, 127)
        self.tuning_ref_note_spin.setValue(60)
        self.tuning_ref_note_spin.setToolTip("Which MIDI note is the scale's own 1/1 (unison) -- 60 = Middle C")
        ref_row.addWidget(self.tuning_ref_note_spin)
        tuning_lay.addLayout(ref_row)
        freq_row = QHBoxLayout(); freq_row.setSpacing(4)
        freq_row.addWidget(QLabel("Ref Freq:"))
        self.tuning_ref_freq_spin = QDoubleSpinBox()
        self.tuning_ref_freq_spin.setRange(20.0, 2000.0)
        self.tuning_ref_freq_spin.setDecimals(2)
        self.tuning_ref_freq_spin.setValue(261.6256)
        self.tuning_ref_freq_spin.setSuffix(" Hz")
        self.tuning_ref_freq_spin.setToolTip("What frequency the reference note above should sound at -- default is Middle C's own standard 12-TET pitch")
        freq_row.addWidget(self.tuning_ref_freq_spin)
        tuning_lay.addLayout(freq_row)
        tuning_box.setLayout(tuning_lay)
        global_layout.addWidget(tuning_box)
        self.btn_load_scale.clicked.connect(self._pick_and_load_scale)
        self.tuning_enable_check.toggled.connect(self.transmit_knob_packet)
        self.tuning_ref_note_spin.valueChanged.connect(self.transmit_knob_packet)
        self.tuning_ref_freq_spin.valueChanged.connect(self.transmit_knob_packet)

        sustain_box = QGroupBox("Sustain Pedal Polarity")
        sustain_lay = QHBoxLayout(); sustain_lay.setSpacing(6); sustain_lay.setContentsMargins(4, 4, 4, 4)
        self.sustain_polarity_normal = QRadioButton("Normal")
        self.sustain_polarity_normal.setChecked(True)
        self.sustain_polarity_inverted = QRadioButton("Inverted")
        self.sustain_polarity_inverted.setToolTip("For a pedal wired backwards (sends 0 when pressed instead of when released)")
        sustain_lay.addWidget(self.sustain_polarity_normal)
        sustain_lay.addWidget(self.sustain_polarity_inverted)
        sustain_box.setLayout(sustain_lay)
        global_layout.addWidget(sustain_box)
        global_layout.addSpacing(4)

        # FM Algorithm system: a fixed, cycle-free set of 12 selectable 4-operator routing
        # topologies (see FM_ALGORITHMS/render_fm_oscillator in backend.c). Algorithm 0
        # ("Classic", 2->1 only) reproduces this synth's original fixed FM pair and stays the
        # default. Only the depth dial(s) that algorithm actually uses are shown (mirroring the
        # FX slot param show/hide pattern) -- fm_intensity is the always-existing "2->1" depth
        # (the old single FM Depth dial), the other 7 are new.
        fm_box = QGroupBox("FM Algorithm")
        fm_lay = QVBoxLayout(); fm_lay.setContentsMargins(4, 4, 4, 4); fm_lay.setSpacing(4)
        self.fm_algorithm_combo = QComboBox()
        self.fm_algorithm_combo.addItems([alg["name"] for alg in FM_ALGORITHMS])
        # Capped -- some algorithm names ("Star: 2/3/4 all into 1", "One-to-Many from 3") are
        # wide enough to force the whole left sidebar (horizontal scrolling disabled, see the
        # depth-knob-row comment below) wider than intended, which drags the entire window wider
        # with it. The closed box elides long names; the dropdown popup still shows them in full.
        self.fm_algorithm_combo.setMaximumWidth(200)
        self.fm_algorithm_combo.setToolTip("Full algorithm names are visible in the dropdown list.")
        fm_lay.addWidget(self.fm_algorithm_combo)

        self.fm_depth_dials = {}
        self.fm_depth_rows = {}
        _fm_depth_labels = {
            "fm_intensity": "2→1",
            "fm_depth_3_1": "3→1",
            "fm_depth_3_2": "3→2",
            "fm_depth_4_1": "4→1",
            "fm_depth_4_3": "4→3",
            "fm_depth_1_2": "1→2",
            "fm_depth_3_4": "3→4",
            "fm_depth_1_3": "1→3",
        }
        fm_depth_grid = QHBoxLayout(); fm_depth_grid.setSpacing(4)
        for key, label in _fm_depth_labels.items():
            row = QVBoxLayout(); row.setSpacing(0)
            row.addWidget(QLabel(label), alignment=Qt.AlignmentFlag.AlignCenter)
            dial = PotentiometerKnob(f"FM Depth {label}", self)
            dial.setRange(0, 200); dial.setValue(0)
            row.addWidget(dial, alignment=Qt.AlignmentFlag.AlignCenter)
            row_widget = QWidget(); row_widget.setLayout(row)
            # Deliberately NOT _retain_hidden here, unlike the oscillator strips' Pulse/Unison
            # boxes: those retain their footprint so switching Shape doesn't jerk that one
            # strip's width, but at most 3 of these 8 depth knobs are ever visible at once (see
            # FM_ALGORITHMS) and the left sidebar has horizontal scrolling disabled -- retaining
            # all 8's width would permanently force the whole sidebar panel wide enough for 8
            # knobs side by side. Left at Qt's default (hidden = 0 width) so the row only takes
            # up space for whichever 1-3 knobs the selected algorithm actually shows.
            fm_depth_grid.addWidget(row_widget)
            self.fm_depth_dials[key] = dial
            self.fm_depth_rows[key] = row_widget
        fm_lay.addLayout(fm_depth_grid)
        fm_box.setLayout(fm_lay)
        global_layout.addWidget(fm_box)

        global_layout.addWidget(QLabel("Glide Time"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.glide_dial = PotentiometerKnob("Glide", self)
        self.glide_dial.setRange(0, 1000); self.glide_dial.setValue(150)
        global_layout.addWidget(self.glide_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.glide_readout = QLabel("150 ms"); self.glide_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        global_layout.addWidget(self.glide_readout)
        global_layout.addSpacing(4)

        global_layout.addWidget(QLabel("Glide Random"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.glide_random_dial = PotentiometerKnob("Glide Random", self)
        self.glide_random_dial.setRange(0, 100); self.glide_random_dial.setValue(0)
        global_layout.addWidget(self.glide_random_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.glide_random_readout = QLabel("0%"); self.glide_random_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        global_layout.addWidget(self.glide_random_readout)
        global_layout.addSpacing(4)

        # Tempo: a flat "what tempo is it" knob, plus a programmable tempo-map system in the
        # backend (tempo markers, interpolated between -- see compute_current_tempo_bpm() in
        # backend.c). No marker editor UI exists yet, so this knob is currently the only way to
        # set it; tempo_readout mirrors the knob locally (matching Glide's pattern), and
        # tempo_live_readout shows what the backend actually resolved it to (round-trips through
        # EngineStatus -- always equal to the knob for now, since no markers are ever sent, but
        # this proves the wire-up end to end and is ready for when markers exist).
        global_layout.addWidget(QLabel("Tempo"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.tempo_dial = PotentiometerKnob("Tempo", self)
        self.tempo_dial.setRange(20, 300); self.tempo_dial.setValue(120)
        global_layout.addWidget(self.tempo_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.tempo_readout = QLabel("120 BPM"); self.tempo_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        global_layout.addWidget(self.tempo_readout)
        self.tempo_live_readout = QLabel("")
        self.tempo_live_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.tempo_live_readout.setStyleSheet("color: #888; font-size: 9px;")
        global_layout.addWidget(self.tempo_live_readout)

        # Arpeggiator -- a patch property (like Hard Sync/Poly-Mono above), not a live
        # performance override (unlike Transpose), so it's Program-scoped and saves/loads with
        # the patch -- see ProgramState's own "Arpeggiator" comment in backend.c. On/off, Style,
        # Velocity, and Gate are exposed here; Octave Range/Rate/Hold remain real, working,
        # OSC-addressable backend fields (see init_default_controls' own sensible defaults: 1
        # octave, 1/8 rate, no Hold) without dedicated widgets yet -- a deliberate scope boundary
        # for this pass, not an oversight (see check_osc_address_parity.py's own explicit note).
        arp_box = QGroupBox("Arpeggiator")
        arp_lay = QVBoxLayout(); arp_lay.setContentsMargins(4, 4, 4, 4); arp_lay.setSpacing(4)
        self.arp_enabled_btn = QPushButton("ARP")
        self.arp_enabled_btn.setCheckable(True)
        self.arp_enabled_btn.setStyleSheet("QPushButton:checked { background-color: #2ecc71; color: black; font-weight: bold; }")
        arp_lay.addWidget(self.arp_enabled_btn)

        self.arp_style_combo = QComboBox()
        self.arp_style_combo.addItems(["Up", "Down", "Up/Down", "Down/Up", "Random", "As Played"])
        arp_lay.addWidget(self.arp_style_combo)

        arp_knob_row = QHBoxLayout(); arp_knob_row.setSpacing(4)
        vel_col = QVBoxLayout(); vel_col.setSpacing(0)
        vel_col.addWidget(QLabel("Velocity"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.arp_velocity_dial = PotentiometerKnob("Arp Velocity", self)
        # 0 = "As Played" (use each held note's own captured velocity -- the default, matching
        # today's exact behavior); 1-127 = a fixed override velocity for every arp-triggered note.
        self.arp_velocity_dial.setRange(0, 127); self.arp_velocity_dial.setValue(0)
        vel_col.addWidget(self.arp_velocity_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.arp_velocity_readout = QLabel("As Played"); self.arp_velocity_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        vel_col.addWidget(self.arp_velocity_readout)
        arp_knob_row.addLayout(vel_col)

        gate_col = QVBoxLayout(); gate_col.setSpacing(0)
        gate_col.addWidget(QLabel("Gate"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.arp_gate_dial = PotentiometerKnob("Arp Gate", self)
        self.arp_gate_dial.setRange(1, 100); self.arp_gate_dial.setValue(70)
        gate_col.addWidget(self.arp_gate_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        self.arp_gate_readout = QLabel("70%"); self.arp_gate_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        gate_col.addWidget(self.arp_gate_readout)
        arp_knob_row.addLayout(gate_col)
        arp_lay.addLayout(arp_knob_row)

        arp_box.setLayout(arp_lay)
        global_layout.addWidget(arp_box)

        global_layout.addStretch()

        global_group.setLayout(global_layout)

        # ----------------------------------------------------
        # MASTER OUTPUT STRIP
        # ----------------------------------------------------
        master_strip_widget = QGroupBox("Output")
        master_strip_lay = QVBoxLayout(master_strip_widget)
        master_strip_lay.setContentsMargins(4, 6, 4, 6); master_strip_lay.setSpacing(4)

        meters_strip = QHBoxLayout()
        vu_box = QVBoxLayout()
        vu_box.addWidget(QLabel("VU"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vu_meter = QProgressBar(); self.vu_meter.setOrientation(Qt.Orientation.Vertical)
        self.vu_meter.setRange(0, 100); self.vu_meter.setValue(0); self.vu_meter.setTextVisible(False)
        self.vu_meter.setMinimumHeight(150)
        self.vu_meter.setStyleSheet("""
            QProgressBar { border: 1px solid #444; background: #222; width: 16px; }
            QProgressBar::chunk { background-color: qlineargradient(x1:0, y1:1, x2:0, y2:0, stop:0 #0f0, stop:0.7 #ff0, stop:1 #f00); }
        """)
        vu_box.addWidget(self.vu_meter, alignment=Qt.AlignmentFlag.AlignCenter); meters_strip.addLayout(vu_box)

        vol_box = QVBoxLayout()
        vol_box.addWidget(QLabel("VOL"), alignment=Qt.AlignmentFlag.AlignCenter)
        # Reset position 79, not 50: _audio_vol_untaper(0.5) -- the audio taper (see its own
        # comment) changed what slider position produces the backend's default 0.5 linear gain,
        # so this double-click-reset target is recomputed to still land on that exact same gain.
        self.master_vol = ResetSlider(Qt.Orientation.Vertical, 79, self)
        self.master_vol.setRange(0, 100); self.master_vol.setMinimumHeight(150)
        vol_box.addWidget(self.master_vol, alignment=Qt.AlignmentFlag.AlignCenter); meters_strip.addLayout(vol_box)
        master_strip_lay.addLayout(meters_strip)

        # Left sidebar: Global Configurations stacked above Output, always visible regardless of
        # which tab is open. Wrapped in a vertical QScrollArea (same pattern as the FX tab and
        # the Play tab's oscillator panel) since this column is now much taller than it used to
        # be as a single "Output" strip -- this way a shorter window scrolls the sidebar instead
        # of clipping it or forcing the whole app taller.
        left_sidebar_content = QWidget()
        left_sidebar_lay = QVBoxLayout(left_sidebar_content)
        left_sidebar_lay.setContentsMargins(0, 0, 0, 0); left_sidebar_lay.setSpacing(6)
        left_sidebar_lay.addWidget(global_group)
        left_sidebar_lay.addWidget(master_strip_widget)
        left_sidebar_lay.addStretch()
        # Static width cap -- belt-and-suspenders on top of individually-capped wide widgets
        # (e.g. fm_algorithm_combo above): with the sidebar's horizontal scrollbar off, any
        # future wide child would otherwise drag the whole window wider with it.
        left_sidebar_content.setMaximumWidth(260)

        left_sidebar_scroll = QScrollArea()
        left_sidebar_scroll.setWidgetResizable(True)
        left_sidebar_scroll.setMaximumWidth(276)
        left_sidebar_scroll.setWidget(left_sidebar_content)
        left_sidebar_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        outer_layout.addWidget(left_sidebar_scroll, stretch=1)
        # ----------------------------------------------------
        # PLAY TAB CONTENT (FORMERLY OSCILLATORS)
        # ----------------------------------------------------
        tab_osc_root_lay = QVBoxLayout(self.tab_play)

        # Performance Mode: Single Program (today's only real mode) / Combi / Sequencer -- a real
        # 3-way now (backed by the new performance_mode field), replacing the old 2-way Prog/Combi
        # toggle. Picking Sequencer also jumps the main tab deck to the new Sequencer tab, since
        # its lane list + transport live there now rather than nested in this Play tab (Combi's
        # lane view stays here, right below, matching where its placeholder already lived).
        osc_mode_row = QHBoxLayout()
        osc_mode_row.setContentsMargins(2, 2, 2, 2)
        osc_mode_row.addWidget(QLabel("<b>Mode:</b>"))
        self.osc_prog_radio = QRadioButton("Prog")
        self.osc_prog_radio.setChecked(True)
        self.osc_combi_radio = QRadioButton("Combi")
        self.osc_sequencer_radio = QRadioButton("Sequencer")
        osc_mode_row.addWidget(self.osc_prog_radio)
        osc_mode_row.addWidget(self.osc_combi_radio)
        osc_mode_row.addWidget(self.osc_sequencer_radio)
        osc_mode_row.addStretch()
        tab_osc_root_lay.addLayout(osc_mode_row)

        self.osc_prog_widget = QWidget()
        osc_outer_lay = QHBoxLayout(self.osc_prog_widget)
        osc_outer_lay.setContentsMargins(0, 0, 0, 0)
        osc_outer_lay.setSpacing(6)

        # Left Column Group: Sound Source Strips
        mixer_group = QGroupBox("Oscillators")
        mixer_lay = QHBoxLayout()
        mixer_lay.setContentsMargins(6, 6, 6, 6)
        mixer_lay.setSpacing(8)

        # Oscillator 1 Strip Configuration
        v1_strip = QVBoxLayout()
        v1_strip.setSpacing(4)
        v1_strip.addWidget(QLabel("<b>Osc 1</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco1_vis = WaveformVisualizer()
        v1_strip.addWidget(self.vco1_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco1_combo = QComboBox()
        self.vco1_combo.addItems(OSC_SHAPE_NAMES)
        v1_strip.addWidget(self.vco1_combo)

        # Dual-duty Pulse Width (Shape=Pulse) / Tilt (Shape=Noise) knob -- one physical knob/wire
        # field serving both meanings (see backend.c's Tilt-noise unification). Label text
        # switches between "Pulse Width" and "Tilt" based on the selected shape (wired below,
        # alongside the other combo-driven visibility rules). Kept right under Shape, not down by
        # Volume, so it reads as part of "what this oscillator's wave looks like" rather than a
        # mixing control.
        self.vco1_pulse_box = QWidget()
        v1_pw_lay = QVBoxLayout(self.vco1_pulse_box)
        v1_pw_lay.setContentsMargins(0, 2, 0, 0)
        self.vco1_pulse_label = QLabel("Pulse Width")
        self.vco1_pulse_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v1_pw_lay.addWidget(self.vco1_pulse_label)
        self.vco1_pulse_width = PotentiometerKnob("O1 Pulse Width/Tilt", self)
        self.vco1_pulse_width.setRange(-100, 100)
        self.vco1_pulse_width.setValue(50)
        self.vco1_pulse_width.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(34, pos))
        v1_pw_lay.addWidget(self.vco1_pulse_width, alignment=Qt.AlignmentFlag.AlignCenter)
        # Monophonic Noise -- only visible (and only meaningful to the backend) when this
        # oscillator's Shape is Noise. See ProgramState.vco1_mono_noise's own comment.
        self.vco1_mono_noise = QCheckBox("Mono")
        self.vco1_mono_noise.setToolTip("Every voice shares one noise generator instead of each voice having its own independent noise.")
        _retain_hidden(self.vco1_mono_noise)
        v1_pw_lay.addWidget(self.vco1_mono_noise, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco1_pulse_box)
        v1_strip.addWidget(self.vco1_pulse_box)

        # Curve/Drive: one physical pot, reinterpreted by Shape -- Curve (bipolar, center=linear
        # ramp) for Sawtooth/Triangle, Drive/Saturation (only the right/positive half has effect,
        # 0=clean) for Sine. See ProgramState.vco1_curve_drive's own comment (backend.c).
        self.vco1_curve_box = QWidget()
        v1_cd_lay = QVBoxLayout(self.vco1_curve_box)
        v1_cd_lay.setContentsMargins(0, 2, 0, 0)
        self.vco1_curve_label = QLabel("Curve")
        self.vco1_curve_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v1_cd_lay.addWidget(self.vco1_curve_label)
        self.vco1_curve_drive = PotentiometerKnob("O1 Curve/Drive", self)
        self.vco1_curve_drive.setRange(-100, 100)
        self.vco1_curve_drive.setValue(0)
        self.vco1_curve_drive.setToolTip("Sawtooth/Triangle: bipolar Curve, center = linear ramp, left = concave, right = convex.\nSine: Drive/Saturation -- only the right half (0..100) has effect, 0 = clean sine.")
        v1_cd_lay.addWidget(self.vco1_curve_drive, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco1_curve_box)
        v1_strip.addWidget(self.vco1_curve_box)

        v1_tune_block = QVBoxLayout()
        v1_pitch_placeholder_lbl = QLabel("Pitch Offset")
        v1_pitch_placeholder_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(v1_pitch_placeholder_lbl)
        v1_pitch_placeholder_lbl.setVisible(False)
        v1_tune_block.addWidget(v1_pitch_placeholder_lbl, alignment=Qt.AlignmentFlag.AlignCenter)

        v1_pitch_placeholder_spin = QSpinBox()
        _retain_hidden(v1_pitch_placeholder_spin)
        v1_pitch_placeholder_spin.setVisible(False)
        v1_tune_block.addWidget(v1_pitch_placeholder_spin, alignment=Qt.AlignmentFlag.AlignCenter)

        v1_fine_lay = QVBoxLayout()
        v1_fine_lay.addWidget(QLabel("Fine"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_fine_tune = QSpinBox()
        self.vco1_fine_tune.setRange(-100, 100)
        self.vco1_fine_tune.setValue(0)
        self.vco1_fine_tune.setSuffix(" c")
        self.vco1_fine_tune.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco1_fine_tune.setFixedWidth(54)
        v1_fine_lay.addWidget(self.vco1_fine_tune, alignment=Qt.AlignmentFlag.AlignCenter)
        v1_tune_block.addLayout(v1_fine_lay)

        v1_phase_lay = QVBoxLayout()
        v1_phase_lay.addWidget(QLabel("Phase"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_phase = PotentiometerKnob("O1 Phase", self)
        self.vco1_phase.setRange(0, 360)
        self.vco1_phase.setValue(0)
        self.vco1_phase.setToolTip("Osc 1 Phase (right-click to assign a modulation source)")
        self.vco1_phase.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(24, pos))
        v1_phase_lay.addWidget(self.vco1_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v1_tune_block.addLayout(v1_phase_lay)

        # Manual Pitch knob (new): +-24 semitones, independent of Fine (cents trim). The same
        # underlying control an LFO/EG's "Osc 1 Pitch" destination already modulates -- see
        # backend.c's vco1_pitch field -- so this knob and live modulation just sum.
        v1_pitch_lay = QVBoxLayout()
        v1_pitch_lay.addWidget(QLabel("Pitch"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_pitch = PotentiometerKnob("O1 Pitch", self)
        self.vco1_pitch.setRange(-24, 24)
        self.vco1_pitch.setValue(0)
        self.vco1_pitch.value_formatter = lambda v: f"{v:+d} st"
        self.vco1_pitch.setToolTip("Osc 1 Pitch, +-24 semitones (right-click to assign a modulation source)")
        self.vco1_pitch.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(1, pos))
        v1_pitch_lay.addWidget(self.vco1_pitch, alignment=Qt.AlignmentFlag.AlignCenter)
        v1_tune_block.addLayout(v1_pitch_lay)
        v1_strip.addLayout(v1_tune_block)

        self.vco1_unison_box = QWidget()
        v1_unison_lay = QHBoxLayout(self.vco1_unison_box)
        v1_unison_lay.setContentsMargins(0, 2, 0, 0)
        v1_unison_lay.setSpacing(6)

        v1_unison_w_col = QVBoxLayout()
        v1_unison_w_col.addWidget(QLabel("Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_unison_width = PotentiometerKnob("O1 Width", self)
        self.vco1_unison_width.setRange(0, 100)
        self.vco1_unison_width.setValue(50)
        v1_unison_w_col.addWidget(self.vco1_unison_width, alignment=Qt.AlignmentFlag.AlignCenter)
        v1_unison_lay.addLayout(v1_unison_w_col)

        v1_unison_d_col = QVBoxLayout()
        v1_unison_d_col.addWidget(QLabel("Detune"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_unison_detune = PotentiometerKnob("O1 Detune", self)
        self.vco1_unison_detune.setRange(0, 4200)
        self.vco1_unison_detune.setValue(25)
        self.vco1_unison_detune.value_formatter = _format_semitones_cents
        v1_unison_d_col.addWidget(self.vco1_unison_detune, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco1_unison_detune_readout = QLabel(_format_semitones_cents(25))
        self.vco1_unison_detune_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.vco1_unison_detune_readout.setStyleSheet("font-size: 8px;")
        v1_unison_d_col.addWidget(self.vco1_unison_detune_readout)
        self.vco1_unison_detune.valueChanged.connect(lambda v: self.vco1_unison_detune_readout.setText(_format_semitones_cents(v)))
        v1_unison_lay.addLayout(v1_unison_d_col)
        _retain_hidden(self.vco1_unison_box)
        v1_strip.addWidget(self.vco1_unison_box)

        v1_vol_layout = QVBoxLayout()
        v1_vol_layout.setSpacing(2)
        v1_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        # Reset position 93, not 80: _audio_vol_untaper(0.8) -- the audio taper (see its own
        # comment) changed what slider position produces the backend's default 0.8 linear gain,
        # so this double-click-reset target is recomputed to still land on that exact same gain.
        self.vco1_vol = ResetSlider(Qt.Orientation.Vertical, 93)
        self.vco1_vol.setRange(0, 100)
        self.vco1_vol.setFixedHeight(80)
        v1_vol_layout.addWidget(self.vco1_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco1_mute = QPushButton("M")
        self.vco1_mute.setCheckable(True)
        self.vco1_mute.setFixedSize(20, 20)
        self.vco1_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        v1_vol_layout.addWidget(self.vco1_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco1_pan = PotentiometerKnob("O1 Pan", self)
        self.vco1_pan.setRange(-100, 100)
        self.vco1_pan.setValue(0)
        v1_vol_layout.addWidget(self.vco1_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_p1 = QLabel("Pan"); lbl_p1.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_p1.setStyleSheet("font-size: 9px;")
        v1_vol_layout.addWidget(lbl_p1)
        v1_strip.addLayout(v1_vol_layout)
        mixer_lay.addLayout(v1_strip)
        # Oscillator 2 Strip Configuration
        v2_strip = QVBoxLayout()
        v2_strip.setSpacing(4)
        v2_strip.addWidget(QLabel("<b>Osc 2</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco2_vis = WaveformVisualizer()
        v2_strip.addWidget(self.vco2_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco2_combo = QComboBox()
        self.vco2_combo.addItems(OSC_SHAPE_NAMES)
        self.vco2_combo.setCurrentIndex(3)
        v2_strip.addWidget(self.vco2_combo)

        # Dual-duty Pulse Width (Shape=Pulse) / Tilt (Shape=Noise) knob -- see Osc 1's own
        # comment above.
        self.vco2_pulse_box = QWidget()
        v2_pw_lay = QVBoxLayout(self.vco2_pulse_box)
        v2_pw_lay.setContentsMargins(0, 2, 0, 0)
        self.vco2_pulse_label = QLabel("Pulse Width")
        self.vco2_pulse_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v2_pw_lay.addWidget(self.vco2_pulse_label)
        self.vco2_pulse_width = PotentiometerKnob("O2 Pulse Width/Tilt", self)
        self.vco2_pulse_width.setRange(-100, 100)
        self.vco2_pulse_width.setValue(50)
        self.vco2_pulse_width.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(35, pos))
        v2_pw_lay.addWidget(self.vco2_pulse_width, alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_mono_noise = QCheckBox("Mono")
        self.vco2_mono_noise.setToolTip("Every voice shares one noise generator instead of each voice having its own independent noise.")
        _retain_hidden(self.vco2_mono_noise)
        v2_pw_lay.addWidget(self.vco2_mono_noise, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco2_pulse_box)
        v2_strip.addWidget(self.vco2_pulse_box)

        self.vco2_curve_box = QWidget()
        v2_cd_lay = QVBoxLayout(self.vco2_curve_box)
        v2_cd_lay.setContentsMargins(0, 2, 0, 0)
        self.vco2_curve_label = QLabel("Curve")
        self.vco2_curve_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v2_cd_lay.addWidget(self.vco2_curve_label)
        self.vco2_curve_drive = PotentiometerKnob("O2 Curve/Drive", self)
        self.vco2_curve_drive.setRange(-100, 100)
        self.vco2_curve_drive.setValue(0)
        self.vco2_curve_drive.setToolTip("Sawtooth/Triangle: bipolar Curve, center = linear ramp, left = concave, right = convex.\nSine: Drive/Saturation -- only the right half (0..100) has effect, 0 = clean sine.")
        v2_cd_lay.addWidget(self.vco2_curve_drive, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco2_curve_box)
        v2_strip.addWidget(self.vco2_curve_box)

        v2_tune_block = QVBoxLayout()
        v2_tune_block.addWidget(QLabel("Pitch Offset"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_pitch_offset = QSpinBox()
        self.vco2_pitch_offset.setRange(-48, 48)
        self.vco2_pitch_offset.setValue(0)
        self.vco2_pitch_offset.setSuffix(" st")
        self.vco2_pitch_offset.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco2_pitch_offset.setFixedWidth(54)
        v2_tune_block.addWidget(self.vco2_pitch_offset, alignment=Qt.AlignmentFlag.AlignCenter)

        v2_fine_lay = QVBoxLayout()
        v2_fine_lay.addWidget(QLabel("Fine"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_fine_tune = QSpinBox()
        self.vco2_fine_tune.setRange(-100, 100)
        self.vco2_fine_tune.setValue(0)
        self.vco2_fine_tune.setSuffix(" c")
        self.vco2_fine_tune.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco2_fine_tune.setFixedWidth(54)
        v2_fine_lay.addWidget(self.vco2_fine_tune, alignment=Qt.AlignmentFlag.AlignCenter)
        v2_tune_block.addLayout(v2_fine_lay)

        v2_phase_lay = QVBoxLayout()
        v2_phase_lay.addWidget(QLabel("Phase"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_phase = PotentiometerKnob("O2 Phase", self)
        self.vco2_phase.setRange(0, 360)
        self.vco2_phase.setValue(0)
        self.vco2_phase.setToolTip("Osc 2 Phase (right-click to assign a modulation source)")
        self.vco2_phase.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(25, pos))
        v2_phase_lay.addWidget(self.vco2_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v2_tune_block.addLayout(v2_phase_lay)

        # Manual Pitch knob (new) -- see Osc 1's own comment above.
        v2_pitch_lay = QVBoxLayout()
        v2_pitch_lay.addWidget(QLabel("Pitch"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_pitch = PotentiometerKnob("O2 Pitch", self)
        self.vco2_pitch.setRange(-24, 24)
        self.vco2_pitch.setValue(0)
        self.vco2_pitch.value_formatter = lambda v: f"{v:+d} st"
        self.vco2_pitch.setToolTip("Osc 2 Pitch, +-24 semitones (right-click to assign a modulation source)")
        self.vco2_pitch.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(2, pos))
        v2_pitch_lay.addWidget(self.vco2_pitch, alignment=Qt.AlignmentFlag.AlignCenter)
        v2_tune_block.addLayout(v2_pitch_lay)
        v2_strip.addLayout(v2_tune_block)

        self.vco2_unison_box = QWidget()
        v2_unison_lay = QHBoxLayout(self.vco2_unison_box)
        v2_unison_lay.setContentsMargins(0, 2, 0, 0)
        v2_unison_lay.setSpacing(6)

        v2_unison_w_col = QVBoxLayout()
        v2_unison_w_col.addWidget(QLabel("Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_unison_width = PotentiometerKnob("O2 Width", self)
        self.vco2_unison_width.setRange(0, 100)
        self.vco2_unison_width.setValue(50)
        v2_unison_w_col.addWidget(self.vco2_unison_width, alignment=Qt.AlignmentFlag.AlignCenter)
        v2_unison_lay.addLayout(v2_unison_w_col)

        v2_unison_d_col = QVBoxLayout()
        v2_unison_d_col.addWidget(QLabel("Detune"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_unison_detune = PotentiometerKnob("O2 Detune", self)
        self.vco2_unison_detune.setRange(0, 4200)
        self.vco2_unison_detune.setValue(25)
        self.vco2_unison_detune.value_formatter = _format_semitones_cents
        v2_unison_d_col.addWidget(self.vco2_unison_detune, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco2_unison_detune_readout = QLabel(_format_semitones_cents(25))
        self.vco2_unison_detune_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.vco2_unison_detune_readout.setStyleSheet("font-size: 8px;")
        v2_unison_d_col.addWidget(self.vco2_unison_detune_readout)
        self.vco2_unison_detune.valueChanged.connect(lambda v: self.vco2_unison_detune_readout.setText(_format_semitones_cents(v)))
        v2_unison_lay.addLayout(v2_unison_d_col)
        _retain_hidden(self.vco2_unison_box)
        v2_strip.addWidget(self.vco2_unison_box)

        v2_vol_layout = QVBoxLayout()
        v2_vol_layout.setSpacing(2)
        v2_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        # Reset position 79, not 80: _audio_vol_untaper(0.5) -- see the matching comment on
        # vco1_vol's own construction above.
        self.vco2_vol = ResetSlider(Qt.Orientation.Vertical, 79)
        self.vco2_vol.setRange(0, 100)
        self.vco2_vol.setFixedHeight(80)
        v2_vol_layout.addWidget(self.vco2_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco2_mute = QPushButton("M")
        self.vco2_mute.setCheckable(True)
        self.vco2_mute.setFixedSize(20, 20)
        self.vco2_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        v2_vol_layout.addWidget(self.vco2_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco2_pan = PotentiometerKnob("O2 Pan", self)
        self.vco2_pan.setRange(-100, 100)
        self.vco2_pan.setValue(0)
        v2_vol_layout.addWidget(self.vco2_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_p2 = QLabel("Pan"); lbl_p2.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_p2.setStyleSheet("font-size: 9px;")
        v2_vol_layout.addWidget(lbl_p2)
        v2_strip.addLayout(v2_vol_layout)
        mixer_lay.addLayout(v2_strip)

        # Oscillator 3 Strip Configuration -- a full peer of Osc 1/2 (was the dedicated Noise
        # oscillator; now selects its shape from the same OSC_SHAPE_NAMES palette, including Off).
        v3_strip = QVBoxLayout()
        v3_strip.setSpacing(4)
        v3_strip.addWidget(QLabel("<b>Osc 3</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco3_vis = WaveformVisualizer()
        v3_strip.addWidget(self.vco3_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco3_combo = QComboBox()
        self.vco3_combo.addItems(OSC_SHAPE_NAMES)
        self.vco3_combo.setCurrentIndex(5)  # Noise, matching the old dedicated Noise oscillator's default
        v3_strip.addWidget(self.vco3_combo)

        # Dual-duty Pulse Width (Shape=Pulse) / Tilt (Shape=Noise) knob -- one physical knob/wire
        # field serving both meanings, exactly like Osc 1/2's own pulse_width (see backend.c's
        # Tilt-noise unification). Label text switches between "Pulse Width" and "Tilt" based on
        # the selected shape (wired below, alongside the other combo-driven visibility rules).
        self.vco3_pulse_box = QWidget()
        v3_pw_lay = QVBoxLayout(self.vco3_pulse_box)
        v3_pw_lay.setContentsMargins(0, 2, 0, 0)
        self.vco3_pulse_label = QLabel("Tilt")
        self.vco3_pulse_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v3_pw_lay.addWidget(self.vco3_pulse_label)
        self.vco3_pulse_width = PotentiometerKnob("O3 Pulse Width/Tilt", self)
        self.vco3_pulse_width.setRange(-100, 100)
        self.vco3_pulse_width.setValue(0)
        self.vco3_pulse_width.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(38, pos))
        v3_pw_lay.addWidget(self.vco3_pulse_width, alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_mono_noise = QCheckBox("Mono")
        self.vco3_mono_noise.setToolTip("Every voice shares one noise generator instead of each voice having its own independent noise.")
        _retain_hidden(self.vco3_mono_noise)
        v3_pw_lay.addWidget(self.vco3_mono_noise, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco3_pulse_box)
        v3_strip.addWidget(self.vco3_pulse_box)

        self.vco3_curve_box = QWidget()
        v3_cd_lay = QVBoxLayout(self.vco3_curve_box)
        v3_cd_lay.setContentsMargins(0, 2, 0, 0)
        self.vco3_curve_label = QLabel("Curve")
        self.vco3_curve_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v3_cd_lay.addWidget(self.vco3_curve_label)
        self.vco3_curve_drive = PotentiometerKnob("O3 Curve/Drive", self)
        self.vco3_curve_drive.setRange(-100, 100)
        self.vco3_curve_drive.setValue(0)
        self.vco3_curve_drive.setToolTip("Sawtooth/Triangle: bipolar Curve, center = linear ramp, left = concave, right = convex.\nSine: Drive/Saturation -- only the right half (0..100) has effect, 0 = clean sine.")
        v3_cd_lay.addWidget(self.vco3_curve_drive, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco3_curve_box)
        v3_strip.addWidget(self.vco3_curve_box)

        v3_tune_block = QVBoxLayout()
        v3_tune_block.addWidget(QLabel("Pitch Offset"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_pitch_offset = QSpinBox()
        self.vco3_pitch_offset.setRange(-48, 48)
        self.vco3_pitch_offset.setValue(0)
        self.vco3_pitch_offset.setSuffix(" st")
        self.vco3_pitch_offset.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco3_pitch_offset.setFixedWidth(54)
        v3_tune_block.addWidget(self.vco3_pitch_offset, alignment=Qt.AlignmentFlag.AlignCenter)

        v3_fine_lay = QVBoxLayout()
        v3_fine_lay.addWidget(QLabel("Fine"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_fine_tune = QSpinBox()
        self.vco3_fine_tune.setRange(-100, 100)
        self.vco3_fine_tune.setValue(0)
        self.vco3_fine_tune.setSuffix(" c")
        self.vco3_fine_tune.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco3_fine_tune.setFixedWidth(54)
        v3_fine_lay.addWidget(self.vco3_fine_tune, alignment=Qt.AlignmentFlag.AlignCenter)
        v3_tune_block.addLayout(v3_fine_lay)

        v3_phase_lay = QVBoxLayout()
        v3_phase_lay.addWidget(QLabel("Phase"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_phase = PotentiometerKnob("O3 Phase", self)
        self.vco3_phase.setRange(0, 360)
        self.vco3_phase.setValue(0)
        self.vco3_phase.setToolTip("Osc 3 Phase (right-click to assign a modulation source)")
        self.vco3_phase.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(43, pos))
        v3_phase_lay.addWidget(self.vco3_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v3_tune_block.addLayout(v3_phase_lay)

        # Manual Pitch knob (new) -- see Osc 1's own comment above.
        v3_pitch_lay = QVBoxLayout()
        v3_pitch_lay.addWidget(QLabel("Pitch"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_pitch = PotentiometerKnob("O3 Pitch", self)
        self.vco3_pitch.setRange(-24, 24)
        self.vco3_pitch.setValue(0)
        self.vco3_pitch.value_formatter = lambda v: f"{v:+d} st"
        self.vco3_pitch.setToolTip("Osc 3 Pitch, +-24 semitones (right-click to assign a modulation source)")
        self.vco3_pitch.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(41, pos))
        v3_pitch_lay.addWidget(self.vco3_pitch, alignment=Qt.AlignmentFlag.AlignCenter)
        v3_tune_block.addLayout(v3_pitch_lay)
        v3_strip.addLayout(v3_tune_block)

        self.vco3_unison_box = QWidget()
        v3_unison_lay = QHBoxLayout(self.vco3_unison_box)
        v3_unison_lay.setContentsMargins(0, 2, 0, 0)
        v3_unison_lay.setSpacing(6)

        v3_unison_w_col = QVBoxLayout()
        v3_unison_w_col.addWidget(QLabel("Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_unison_width = PotentiometerKnob("O3 Width", self)
        self.vco3_unison_width.setRange(0, 100)
        self.vco3_unison_width.setValue(50)
        v3_unison_w_col.addWidget(self.vco3_unison_width, alignment=Qt.AlignmentFlag.AlignCenter)
        v3_unison_lay.addLayout(v3_unison_w_col)

        v3_unison_d_col = QVBoxLayout()
        v3_unison_d_col.addWidget(QLabel("Detune"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_unison_detune = PotentiometerKnob("O3 Detune", self)
        self.vco3_unison_detune.setRange(0, 4200)
        self.vco3_unison_detune.setValue(25)
        self.vco3_unison_detune.value_formatter = _format_semitones_cents
        v3_unison_d_col.addWidget(self.vco3_unison_detune, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco3_unison_detune_readout = QLabel(_format_semitones_cents(25))
        self.vco3_unison_detune_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.vco3_unison_detune_readout.setStyleSheet("font-size: 8px;")
        v3_unison_d_col.addWidget(self.vco3_unison_detune_readout)
        self.vco3_unison_detune.valueChanged.connect(lambda v: self.vco3_unison_detune_readout.setText(_format_semitones_cents(v)))
        v3_unison_lay.addLayout(v3_unison_d_col)
        _retain_hidden(self.vco3_unison_box)
        v3_strip.addWidget(self.vco3_unison_box)

        v3_vol_layout = QVBoxLayout()
        v3_vol_layout.setSpacing(2)
        v3_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco3_vol = ResetSlider(Qt.Orientation.Vertical, 0)
        self.vco3_vol.setRange(0, 100)
        self.vco3_vol.setFixedHeight(80)
        v3_vol_layout.addWidget(self.vco3_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco3_mute = QPushButton("M")
        self.vco3_mute.setCheckable(True)
        self.vco3_mute.setFixedSize(20, 20)
        self.vco3_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        v3_vol_layout.addWidget(self.vco3_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco3_pan = PotentiometerKnob("O3 Pan", self)
        self.vco3_pan.setRange(-100, 100)
        self.vco3_pan.setValue(0)
        v3_vol_layout.addWidget(self.vco3_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_p3 = QLabel("Pan"); lbl_p3.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_p3.setStyleSheet("font-size: 9px;")
        v3_vol_layout.addWidget(lbl_p3)
        v3_strip.addLayout(v3_vol_layout)
        mixer_lay.addLayout(v3_strip)

        # Oscillator 4 Strip Configuration -- a full peer of Osc 1/2/3, replacing the old fixed
        # "Sub" oscillator (which had no shape choice beyond Saw/Square/Triangle/Sine, no pitch
        # controls of its own, and always tracked exactly one octave below Osc 1). Set
        # vco4_pitch_offset to -12 to reproduce that old fixed-octave-down behavior manually.
        v4_strip = QVBoxLayout()
        v4_strip.setSpacing(4)
        v4_strip.addWidget(QLabel("<b>Osc 4</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco4_vis = WaveformVisualizer()
        v4_strip.addWidget(self.vco4_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco4_combo = QComboBox()
        self.vco4_combo.addItems(OSC_SHAPE_NAMES)
        self.vco4_combo.setCurrentIndex(8)  # Off by default -- silent until configured, like Sub's old vol=0 default
        v4_strip.addWidget(self.vco4_combo)

        self.vco4_pulse_box = QWidget()
        v4_pw_lay = QVBoxLayout(self.vco4_pulse_box)
        v4_pw_lay.setContentsMargins(0, 2, 0, 0)
        self.vco4_pulse_label = QLabel("Pulse Width")
        self.vco4_pulse_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v4_pw_lay.addWidget(self.vco4_pulse_label)
        self.vco4_pulse_width = PotentiometerKnob("O4 Pulse Width/Tilt", self)
        self.vco4_pulse_width.setRange(-100, 100)
        self.vco4_pulse_width.setValue(50)
        self.vco4_pulse_width.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(47, pos))
        v4_pw_lay.addWidget(self.vco4_pulse_width, alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_mono_noise = QCheckBox("Mono")
        self.vco4_mono_noise.setToolTip("Every voice shares one noise generator instead of each voice having its own independent noise.")
        _retain_hidden(self.vco4_mono_noise)
        v4_pw_lay.addWidget(self.vco4_mono_noise, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco4_pulse_box)
        v4_strip.addWidget(self.vco4_pulse_box)

        self.vco4_curve_box = QWidget()
        v4_cd_lay = QVBoxLayout(self.vco4_curve_box)
        v4_cd_lay.setContentsMargins(0, 2, 0, 0)
        self.vco4_curve_label = QLabel("Curve")
        self.vco4_curve_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v4_cd_lay.addWidget(self.vco4_curve_label)
        self.vco4_curve_drive = PotentiometerKnob("O4 Curve/Drive", self)
        self.vco4_curve_drive.setRange(-100, 100)
        self.vco4_curve_drive.setValue(0)
        self.vco4_curve_drive.setToolTip("Sawtooth/Triangle: bipolar Curve, center = linear ramp, left = concave, right = convex.\nSine: Drive/Saturation -- only the right half (0..100) has effect, 0 = clean sine.")
        v4_cd_lay.addWidget(self.vco4_curve_drive, alignment=Qt.AlignmentFlag.AlignCenter)
        _retain_hidden(self.vco4_curve_box)
        v4_strip.addWidget(self.vco4_curve_box)

        v4_tune_block = QVBoxLayout()
        v4_tune_block.addWidget(QLabel("Pitch Offset"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_pitch_offset = QSpinBox()
        self.vco4_pitch_offset.setRange(-48, 48)
        self.vco4_pitch_offset.setValue(-12)  # matches the old Sub's fixed octave-down, as a starting point
        self.vco4_pitch_offset.setSuffix(" st")
        self.vco4_pitch_offset.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco4_pitch_offset.setFixedWidth(54)
        v4_tune_block.addWidget(self.vco4_pitch_offset, alignment=Qt.AlignmentFlag.AlignCenter)

        v4_fine_lay = QVBoxLayout()
        v4_fine_lay.addWidget(QLabel("Fine"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_fine_tune = QSpinBox()
        self.vco4_fine_tune.setRange(-100, 100)
        self.vco4_fine_tune.setValue(0)
        self.vco4_fine_tune.setSuffix(" c")
        self.vco4_fine_tune.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.vco4_fine_tune.setFixedWidth(54)
        v4_fine_lay.addWidget(self.vco4_fine_tune, alignment=Qt.AlignmentFlag.AlignCenter)
        v4_tune_block.addLayout(v4_fine_lay)

        v4_phase_lay = QVBoxLayout()
        v4_phase_lay.addWidget(QLabel("Phase"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_phase = PotentiometerKnob("O4 Phase", self)
        self.vco4_phase.setRange(0, 360)
        self.vco4_phase.setValue(0)
        self.vco4_phase.setToolTip("Osc 4 Phase (right-click to assign a modulation source)")
        self.vco4_phase.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(44, pos))
        v4_phase_lay.addWidget(self.vco4_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v4_tune_block.addLayout(v4_phase_lay)

        # Manual Pitch knob (new) -- see Osc 1's own comment above.
        v4_pitch_lay = QVBoxLayout()
        v4_pitch_lay.addWidget(QLabel("Pitch"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_pitch = PotentiometerKnob("O4 Pitch", self)
        self.vco4_pitch.setRange(-24, 24)
        self.vco4_pitch.setValue(0)
        self.vco4_pitch.value_formatter = lambda v: f"{v:+d} st"
        self.vco4_pitch.setToolTip("Osc 4 Pitch, +-24 semitones (right-click to assign a modulation source)")
        self.vco4_pitch.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(42, pos))
        v4_pitch_lay.addWidget(self.vco4_pitch, alignment=Qt.AlignmentFlag.AlignCenter)
        v4_tune_block.addLayout(v4_pitch_lay)
        v4_strip.addLayout(v4_tune_block)

        self.vco4_unison_box = QWidget()
        v4_unison_lay = QHBoxLayout(self.vco4_unison_box)
        v4_unison_lay.setContentsMargins(0, 2, 0, 0)
        v4_unison_lay.setSpacing(6)

        v4_unison_w_col = QVBoxLayout()
        v4_unison_w_col.addWidget(QLabel("Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_unison_width = PotentiometerKnob("O4 Width", self)
        self.vco4_unison_width.setRange(0, 100)
        self.vco4_unison_width.setValue(50)
        v4_unison_w_col.addWidget(self.vco4_unison_width, alignment=Qt.AlignmentFlag.AlignCenter)
        v4_unison_lay.addLayout(v4_unison_w_col)

        v4_unison_d_col = QVBoxLayout()
        v4_unison_d_col.addWidget(QLabel("Detune"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_unison_detune = PotentiometerKnob("O4 Detune", self)
        self.vco4_unison_detune.setRange(0, 4200)
        self.vco4_unison_detune.setValue(25)
        self.vco4_unison_detune.value_formatter = _format_semitones_cents
        v4_unison_d_col.addWidget(self.vco4_unison_detune, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco4_unison_detune_readout = QLabel(_format_semitones_cents(25))
        self.vco4_unison_detune_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.vco4_unison_detune_readout.setStyleSheet("font-size: 8px;")
        v4_unison_d_col.addWidget(self.vco4_unison_detune_readout)
        self.vco4_unison_detune.valueChanged.connect(lambda v: self.vco4_unison_detune_readout.setText(_format_semitones_cents(v)))
        v4_unison_lay.addLayout(v4_unison_d_col)
        _retain_hidden(self.vco4_unison_box)
        v4_strip.addWidget(self.vco4_unison_box)

        v4_vol_layout = QVBoxLayout()
        v4_vol_layout.setSpacing(2)
        v4_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco4_vol = ResetSlider(Qt.Orientation.Vertical, 0)
        self.vco4_vol.setRange(0, 100)
        self.vco4_vol.setFixedHeight(80)
        v4_vol_layout.addWidget(self.vco4_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco4_mute = QPushButton("M")
        self.vco4_mute.setCheckable(True)
        self.vco4_mute.setFixedSize(20, 20)
        self.vco4_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        v4_vol_layout.addWidget(self.vco4_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.vco4_pan = PotentiometerKnob("O4 Pan", self)
        self.vco4_pan.setRange(-100, 100)
        self.vco4_pan.setValue(0)
        v4_vol_layout.addWidget(self.vco4_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_p4 = QLabel("Pan"); lbl_p4.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_p4.setStyleSheet("font-size: 9px;")
        v4_vol_layout.addWidget(lbl_p4)
        v4_strip.addLayout(v4_vol_layout)
        mixer_lay.addLayout(v4_strip)

        mixer_group.setLayout(mixer_lay)
        osc_outer_lay.addWidget(mixer_group, stretch=4)
        # Right Column Group: Side-by-Side Amp EGs Horizontal Layout Overhaul
        amp_eg_group = QGroupBox("Dedicated Amplitude Envelopes")
        amp_eg_lay = QHBoxLayout()  # Overhauled layout: Horizontal alignment blocks
        amp_eg_lay.setContentsMargins(4, 6, 4, 6)
        amp_eg_lay.setSpacing(6)

        self.vco1_amp_eg = ADSRPanel("Osc 1 Amp EG", (1, 1000), (1, 2000), (1, 2000))
        self.vco2_amp_eg = ADSRPanel("Osc 2 Amp EG", (1, 1000), (1, 2000), (1, 2000))
        self.vco3_amp_eg = ADSRPanel("Osc 3 Amp EG", (1, 1000), (1, 2000), (1, 2000))
        self.vco4_amp_eg = ADSRPanel("Osc 4 Amp EG", (1, 1000), (1, 2000), (1, 2000))

        # "1->All": one-shot static copy of Osc 1's Amp EG onto Osc 2/3/4 (was "1->2", Osc 2 only).
        self.btn_sync_amp_eg = QPushButton("1\u2192All")
        self.btn_sync_amp_eg.setFixedSize(30, 16)
        self.btn_sync_amp_eg.setStyleSheet("font-size: 8px; font-weight: bold;")
        self.btn_sync_amp_eg.setToolTip("Copy Osc 1 Amp EG settings to Osc 2/3/4 Amp EG (one-time static copy)")
        self.btn_sync_amp_eg.clicked.connect(self._copy_amp_eg_to_all)

        # "Sync": a live, continuous version of the same copy -- while checked, Osc 2/3/4's Amp EG
        # keeps mirroring Osc 1's in real time (handy for realtime sustain-stage auditing, per the
        # user), instead of only copying once. Osc 2/3/4's panels are disabled while engaged so a
        # manual edit there can't silently fight the continuous overwrite from Osc 1.
        self.btn_live_sync_amp_eg = QPushButton("Sync")
        self.btn_live_sync_amp_eg.setCheckable(True)
        self.btn_live_sync_amp_eg.setFixedSize(30, 16)
        self.btn_live_sync_amp_eg.setStyleSheet(
            "QPushButton { font-size: 8px; font-weight: bold; } "
            "QPushButton:checked { background-color: #00adb5; color: #1b1b22; }"
        )
        self.btn_live_sync_amp_eg.setToolTip(
            "While engaged, Osc 2/3/4 Amp EG continuously live-mirrors Osc 1's (disables their panels)"
        )
        self.btn_live_sync_amp_eg.toggled.connect(self._on_amp_eg_live_sync_toggled)

        sync_btn_box = QWidget()
        sync_btn_lay = QVBoxLayout(sync_btn_box)
        sync_btn_lay.setContentsMargins(0, 0, 0, 0); sync_btn_lay.setSpacing(2)
        sync_btn_lay.addWidget(self.btn_sync_amp_eg)
        sync_btn_lay.addWidget(self.btn_live_sync_amp_eg)
        self.vco1_amp_eg.title_layout.addWidget(sync_btn_box)

        # Osc 2/3/4 have no sync buttons of their own -- reserve the exact same footprint (now 2
        # stacked buttons tall) in their title rows anyway so all four panels stay the same
        # height/alignment instead of only Osc 1's row being taller.
        for panel in (self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg):
            sync_btn_spacer = QWidget()
            sync_btn_spacer.setFixedSize(30, 34)
            _retain_hidden(sync_btn_spacer)
            sync_btn_spacer.setVisible(False)
            panel.title_layout.addWidget(sync_btn_spacer)

        # Velocity -> Amp, per oscillator (-100..100, bipolar; 0 = velocity-insensitive, matching
        # today's exact behavior by default -- see ProgramState.vco{1-4}_vel_to_amp's own comment
        # in backend.c). Lives on each oscillator's own Amp EG title row rather than inside
        # ADSRPanel itself, since that class is shared with the 6 assignable EGs, which have no
        # oscillator of their own to scale -- this is oscillator-specific, not EG-generic. Deliberately
        # NOT wired to _show_dest_assign_menu/setContextMenuCallback like the modulatable knobs
        # around it: velocity is a one-shot value captured at note-on, not a continuously-varying
        # per-sample signal an LFO/EG can substitute for, so it doesn't fit that "Modulate From"
        # source-menu system -- it's a plain, direct knob, same as Fine Tune or Pan.
        for osc_n, panel in ((1, self.vco1_amp_eg), (2, self.vco2_amp_eg), (3, self.vco3_amp_eg), (4, self.vco4_amp_eg)):
            vel_box = QWidget()
            vel_lay = QVBoxLayout(vel_box); vel_lay.setContentsMargins(0, 0, 0, 0); vel_lay.setSpacing(0)
            vel_lbl = QLabel("Vel→Amp"); vel_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            vel_lbl.setStyleSheet("font-size: 8px; font-weight: bold;")
            vel_lay.addWidget(vel_lbl)
            vel_knob = PotentiometerKnob(f"O{osc_n} Vel→Amp", self)
            vel_knob.setRange(-100, 100)
            vel_knob.setValue(0)
            vel_knob.setToolTip(
                f"Osc {osc_n}: how much MIDI note velocity scales this oscillator's output level. "
                "0 = insensitive (today's default behavior). Positive = harder hits louder, "
                "negative = harder hits quieter."
            )
            vel_lay.addWidget(vel_knob, alignment=Qt.AlignmentFlag.AlignCenter)
            panel.title_layout.addWidget(vel_box)
            setattr(self, f"vco{osc_n}_vel_to_amp", vel_knob)

        amp_eg_lay.addWidget(self.vco1_amp_eg)
        amp_eg_lay.addWidget(self.vco2_amp_eg)
        amp_eg_lay.addWidget(self.vco3_amp_eg)
        amp_eg_lay.addWidget(self.vco4_amp_eg)
        amp_eg_group.setLayout(amp_eg_lay)

        osc_outer_lay.addWidget(amp_eg_group, stretch=6)

        # The 4 oscillator strips + 3 amp EGs side by side have a real minimum width (each strip
        # reserves room for its Unison/Pulse Width sub-panels even while hidden, so the layout
        # doesn't jump when the Shape combo changes -- see _retain_hidden). Wrapping the whole
        # thing in a QScrollArea (same pattern as the FX tab) lets the window/panel itself be
        # resized/collapsed narrower than that without clipping anything -- it just scrolls
        # horizontally instead of forcing the whole app wide.
        self.osc_prog_scroll = QScrollArea()
        self.osc_prog_scroll.setWidgetResizable(True)
        self.osc_prog_scroll.setWidget(self.osc_prog_widget)
        self.osc_prog_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.osc_prog_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        tab_osc_root_lay.addWidget(self.osc_prog_scroll)

        # --- Combi/Sequencer lane scaffolding (see the plan's own "Combi/Sequencer scaffolding"
        # section). Combi and Sequencer remain two separate modes/tabs -- Combi stays in the Play
        # tab, Sequencer stays in its own dedicated tab, never merged. Only the STRIP-BUILDING
        # code below is shared between them (one shared builder), mirroring how Filter A's
        # Normal-vs-Multi views are already two separate widget sets built from shared helpers.
        # Purely inert this pass (lane fader/pan/type/mode): nothing in the DSP path reads any of
        # it yet (verified directly -- see test_seq_scaffolding.c's zero-DSP-change test). "Load
        # Audio" is the one REAL piece here -- see g_seq_audio/poll_seq_audio_load_requests in
        # backend.c, which actually reads a WAV file into a RAM buffer on a background thread.
        #
        # Layout: a MIXER-PANEL strip, not a track-list row -- each lane is one narrow vertical
        # strip (number/fader/pan/dropdowns stacked top-to-bottom) and lanes sit side by side in
        # a horizontally-scrolling band, the way a physical mixing console's channel strips do
        # (corrected from an earlier vertically-stacked track-list layout per direct feedback). A
        # static, non-scrolling legend column to the strips' left carries the row labels once
        # (see _build_lane_legend) since there's no room to repeat "Level"/"Pan"/etc. on every one
        # of up to 32 strips.
        def _build_lane_legend(labels):
            legend = QWidget()
            lay = QVBoxLayout(legend); lay.setContentsMargins(2, 2, 2, 2); lay.setSpacing(3)
            for text in labels:
                lbl = QLabel(text)
                lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                lbl.setStyleSheet("font-weight: bold; font-size: 10px; color: #888;")
                lay.addWidget(lbl)
            lay.addStretch()
            return legend

        def _build_seq_lane_row(idx, show_seq_controls):
            strip = QWidget()
            lay = QVBoxLayout(strip); lay.setContentsMargins(2, 2, 2, 2); lay.setSpacing(3)
            num_label = QLabel(f"{idx + 1:02d}")
            num_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            num_label.setStyleSheet("font-weight: bold; font-size: 10px;")
            lay.addWidget(num_label)
            fader = ResetSlider(Qt.Orientation.Vertical, 80)
            fader.setRange(0, 100); fader.setValue(80); fader.setFixedHeight(100)
            lay.addWidget(fader, alignment=Qt.AlignmentFlag.AlignCenter)
            pan_dial = PotentiometerKnob(f"Lane {idx + 1} Pan", self)
            pan_dial.setRange(-100, 100); pan_dial.setValue(0)
            lay.addWidget(pan_dial, alignment=Qt.AlignmentFlag.AlignCenter)
            type_combo, mode_combo = None, None
            if show_seq_controls:
                type_combo = QComboBox(); type_combo.addItems(["Program/MIDI", "Audio"])
                lay.addWidget(type_combo)
                # Mute and Rec used to be two of this combo's own entries (a single exclusive
                # Seq/Mute/Rec choice) -- per direct feedback, they're now independent toggle
                # buttons instead (see _build_seq_lane_rec_mute_controls below), so this dropdown
                # only carries what's left: Seq (the lane runs) vs Off (a real compute bypass --
                # see SeqLaneState's own precedence comment in backend.c for exactly how Off
                # overrides Mute/Rec when combined).
                mode_combo = QComboBox(); mode_combo.addItems(["Seq", "Off"])
                lay.addWidget(mode_combo)
            lay.addStretch()
            return (strip, fader, pan_dial, type_combo, mode_combo)

        # Mute/Rec toggle buttons + the Overwrite/Overdub dropdown for one Sequencer lane
        # (Sequencer tab only -- Combi has no per-lane recording concept at all, matching how
        # _build_seq_audio_controls is likewise only ever called for the Sequencer view). Built
        # separately and appended onto the strip's own layout afterward, same idiom
        # _build_seq_audio_controls/the Combi picker's "Edit" button already use, to keep
        # _build_seq_lane_row's own return shape unchanged (other code destructures it as a fixed
        # 5-tuple). Both buttons mirror the oscillator Mute buttons' own checkable/red-when-
        # engaged styling exactly (see vco1_mute) for a consistent "this is armed/engaged" look.
        def _build_seq_lane_rec_mute_controls(idx):
            mute_btn = QPushButton("Mute")
            mute_btn.setCheckable(True)
            mute_btn.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
            mute_btn.setToolTip("Silence this lane's output live -- position stays in sync, resumes exactly where it left off when un-muted")
            rec_btn = QPushButton("● Rec")
            rec_btn.setCheckable(True)
            rec_btn.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
            rec_btn.setToolTip("Arm this lane to record incoming MIDI the next time Play is pressed (filtered by its own MIDI Channel below)")
            overdub_combo = QComboBox()
            overdub_combo.addItems(["Overwrite", "Overdub"])
            overdub_combo.setStyleSheet("font-size: 9px;")
            overdub_combo.setToolTip(
                "Overwrite: a fresh Rec take clears this lane's previously recorded notes.\n"
                "Overdub: a fresh take keeps them -- newly recorded notes are merged in by time, nothing is lost."
            )
            return mute_btn, rec_btn, overdub_combo

        # "Load MIDI..." controls for one Sequencer lane (Program/MIDI lanes only -- the
        # complement of _build_seq_audio_controls just below, same "visible only for this lane's
        # current Type" idiom, see _enforce_seq_lane_type_cap). Imports a single .mid/.midi file's
        # notes straight into THIS lane, replacing whatever it currently holds -- distinct from
        # the Sequencer tab's own whole-song Import MIDI (_pick_and_import_midi()), which
        # repopulates every lane at once and can also apply the file's tempo map. This is scoped
        # to exactly one lane and never touches tempo or the Markers list at all -- see
        # hds_song_import_smf_track()'s own comment in backend.c.
        def _build_seq_midi_controls(idx):
            box = QWidget()
            box_lay = QVBoxLayout(box); box_lay.setContentsMargins(0, 0, 0, 0); box_lay.setSpacing(2)
            load_btn = QPushButton("Load MIDI…")
            load_btn.setStyleSheet("font-size: 9px;")
            load_btn.setToolTip(
                "Imports a single .mid/.midi file's note events straight into this lane, "
                "replacing whatever it currently holds. Every note-carrying track in the file is "
                "merged together (most such files only have one) -- this lane's own tempo/"
                "markers are untouched; use this tab's whole-song Import MIDI for that instead."
            )
            load_btn.clicked.connect(lambda: self._pick_and_import_midi_track(idx))
            box_lay.addWidget(load_btn)
            status_lbl = QLabel("(no file)")
            status_lbl.setStyleSheet("font-size: 8px; color: #888;")
            status_lbl.setWordWrap(True)
            status_lbl.setFixedWidth(66)
            box_lay.addWidget(status_lbl)
            _retain_hidden(box)
            box.setVisible(True)  # Program/MIDI (type index 0) is every lane's own default type
            return {"box": box, "status_label": status_lbl}

        # "Load Audio" controls for one Sequencer lane (Sequencer only -- Combi has no audio-lane
        # concept at all). Built separately from _build_seq_lane_row and appended onto the strip's
        # own layout afterward, same idiom Combi's per-lane Program-picker combo already uses --
        # keeps the shared row builder's tuple shape unchanged (other code destructures it as a
        # fixed 5-tuple). Box/status retain their footprint while hidden (_retain_hidden) so a
        # strip's size doesn't jerk around as its Type combo is toggled; the Offset knob is
        # additionally gated on the backend actually reporting the load as READY (see
        # _apply_seq_audio_status), since offsetting nothing loaded is meaningless.
        def _build_seq_audio_controls(idx):
            box = QWidget()
            box_lay = QVBoxLayout(box); box_lay.setContentsMargins(0, 0, 0, 0); box_lay.setSpacing(2)
            load_btn = QPushButton("Load Audio\u2026")
            load_btn.setStyleSheet("font-size: 9px;")
            load_btn.clicked.connect(lambda: self._pick_seq_audio_file(idx))
            box_lay.addWidget(load_btn)
            status_lbl = QLabel("(no file)")
            status_lbl.setStyleSheet("font-size: 8px; color: #888;")
            status_lbl.setWordWrap(True)
            status_lbl.setFixedWidth(66)
            box_lay.addWidget(status_lbl)

            # Controls that only make sense once the backend actually reports this lane's audio
            # as loaded (SEQ_AUDIO_READY) -- a real one-shot "Preview" playback trigger (see
            # poll_seq_audio_preview_triggers/render_seq_audio_preview_sample in backend.c -- this
            # genuinely plays the loaded RAM buffer, mixed straight into the output, independent
            # of any other lane's own Preview state so several lanes can play back
            # simultaneously) plus the Offset knob. Shown/hidden together as one unit.
            offset_widget = QWidget()
            offset_lay = QVBoxLayout(offset_widget); offset_lay.setContentsMargins(0, 0, 0, 0); offset_lay.setSpacing(2)
            preview_btn = QPushButton("▶ Preview")
            preview_btn.setStyleSheet("font-size: 9px;")
            preview_btn.clicked.connect(lambda: self._preview_seq_audio(idx))
            offset_lay.addWidget(preview_btn)
            offset_lay.addWidget(QLabel("Offset"), alignment=Qt.AlignmentFlag.AlignCenter)
            offset_dial = PotentiometerKnob(f"Lane {idx + 1} Audio Offset", self)
            offset_dial.setRange(-100, 100); offset_dial.setValue(0)  # tenths of a second: -10.0s..+10.0s
            offset_lay.addWidget(offset_dial, alignment=Qt.AlignmentFlag.AlignCenter)
            offset_readout = QLabel("0.0s"); offset_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
            offset_readout.setStyleSheet("font-size: 8px;")
            offset_lay.addWidget(offset_readout)
            offset_dial.valueChanged.connect(lambda v, lbl=offset_readout: lbl.setText(f"{v / 10.0:+.1f}s"))
            _retain_hidden(offset_widget)
            offset_widget.setVisible(False)
            box_lay.addWidget(offset_widget)

            _retain_hidden(box)
            box.setVisible(False)
            return {"box": box, "load_btn": load_btn, "status_label": status_lbl,
                    "offset_dial": offset_dial, "offset_widget": offset_widget,
                    "preview_btn": preview_btn, "path": "", "preview_trigger": 0}

        # Combi view: COMBI_PROGRAM_COUNT (8) lanes, no type/seq-mode dropdowns -- every Combi
        # lane is implicitly Program/MIDI (Combi has no audio-lane concept at all, per the user's
        # own "these two dropdowns will not be necessary for combi mode"). Stays in the Play tab
        # -- never moved or merged with Sequencer. Real synthesis now: each lane is one of
        # programs[0..7] in backend.c, sharing one voice pool (capped by the same Polyphony knob
        # every other mode uses -- no separate per-lane budget), independently MIDI-triggerable
        # (channel selector below) and independently routable to any FX slot or the whole-stack
        # "Combi Mix" bus (see the FX tab's source combo, Combi-mode-aware).
        self.osc_combi_widget = QWidget()
        combi_lay = QVBoxLayout(self.osc_combi_widget)
        combi_lay.setContentsMargins(2, 2, 2, 2); combi_lay.setSpacing(2)
        combi_note = QLabel(
            f"Combi \u2014 up to {COMBI_PROGRAM_COUNT} independently-synthesizing Programs, one shared voice pool. "
            "Pick a saved Program per lane below; each lane's own Level/Pan shape that Program's "
            "output here on top of its own oscillator Pan/Vol knobs and FX routing."
        )
        combi_note.setWordWrap(True)
        combi_note.setStyleSheet("color: #777; font-style: italic;")
        combi_lay.addWidget(combi_note)
        # Combi Save/Load: a .hdscombi file references each lane's constituent Program BY NAME
        # (not by embedding its full ~1150-field parameter set), the same "reference, don't
        # duplicate" relationship the lane picker below already has to ~/HDS_Programs -- see
        # _pick_and_save_combi()/_pick_and_load_combi()'s own docstrings.
        combi_save_load_row = QHBoxLayout()
        self.btn_save_combi = QPushButton("Save Combi")
        self.btn_load_combi = QPushButton("Load Combi")
        combi_save_load_row.addWidget(self.btn_save_combi)
        combi_save_load_row.addWidget(self.btn_load_combi)
        combi_save_load_row.addStretch()
        combi_lay.addLayout(combi_save_load_row)
        self.btn_save_combi.clicked.connect(self._pick_and_save_combi)
        self.btn_load_combi.clicked.connect(self._pick_and_load_combi)
        # Per-lane "pick a saved Program" dropdown -- loads that .hdsprog's fields onto this
        # lane's own /hds/combi/{lane}/... address set (programs[lane] in backend.c). Every lane
        # (0-7) is its own fully independent ProgramState, completely separate from Single
        # Program mode's own dedicated slot (see ProgramState's own comment in backend.c on the
        # mutual-exclusivity fix) -- none of them have a widget of their own, so the picker sends
        # directly over OSC. See _apply_program_fields_to_combi_lane()'s own comment for why.
        # The "Edit" button below each picker jumps into Single Program mode with that same
        # Program loaded into its own editing buffer -- see
        # _edit_combi_lane_in_program_mode()'s own docstring.
        self.combi_lane_program_choice = [None] * COMBI_PROGRAM_COUNT
        self.combi_program_combos = []
        self.combi_lane_edit_buttons = []
        self.combi_lane_rows = []  # COMBI_PROGRAM_COUNT x (strip, fader, pan_dial, None, None)
        # Per-lane MIDI channel selector -- mirrors the Sequencer lanes' own
        # seq_lane_midi_channel_combos pattern exactly: "Omni" (index 0) or a specific channel
        # 1-16, filtering which Combi lane(s) a live incoming note dispatches to
        # (process_midi_input's Combi-mode branch in backend.c).
        self.combi_lane_midi_channel_combos = []  # COMBI_PROGRAM_COUNT x QComboBox
        # Per-lane Key/Velocity zones: lets a lane act as a keyboard split ("only sound above
        # C3") and/or a velocity switch ("only sound on hits above 100"), gating live note
        # dispatch alongside the MIDI channel filter above (see dispatch_midi_event's Combi-mode
        # branch in backend.c). Defaults (0-127 key, 1-127 velocity) are the full range,
        # reproducing today's exact unconditional dispatch.
        self.combi_lane_key_lo_spins, self.combi_lane_key_hi_spins = [], []
        self.combi_lane_vel_lo_spins, self.combi_lane_vel_hi_spins = [], []
        # Per-lane Transpose (octave layering): -48..+48 semitones, session-level (see backend.c's
        # combi_lane_transpose comment) -- lets the same or different Programs be stacked at
        # different octaves for a thicker layered sound. A plain QSpinBox, same "st"-suffixed
        # up/down-arrow incrementer convention as vco2/3/4's own Pitch Offset spinboxes.
        self.combi_lane_transpose_spins = []
        combi_strips_widget = QWidget()
        combi_strips_lay = QHBoxLayout(combi_strips_widget)
        combi_strips_lay.setContentsMargins(0, 0, 0, 0); combi_strips_lay.setSpacing(3)
        for i in range(COMBI_PROGRAM_COUNT):
            lane = _build_seq_lane_row(i, show_seq_controls=False)
            prog_combo = QComboBox()
            prog_combo.setToolTip(f"Loads a saved Program onto this lane (programs[{i}] in the backend)")
            lane[0].layout().addWidget(prog_combo)
            prog_combo.currentTextChanged.connect(lambda text, idx=i: self._on_combi_lane_program_picked(idx, text))
            self.combi_program_combos.append(prog_combo)
            edit_btn = QPushButton("Edit")
            edit_btn.setToolTip(
                "Load this lane's Program into Single Program mode to edit and save it -- "
                "editing here never changes what this lane plays until you explicitly reload it "
                "onto the lane afterward (the two are independent, see the picker's own tooltip)."
            )
            edit_btn.setStyleSheet("font-size: 9px; padding: 1px 4px;")
            edit_btn.clicked.connect(lambda checked=False, idx=i: self._edit_combi_lane_in_program_mode(idx))
            lane[0].layout().addWidget(edit_btn)
            self.combi_lane_edit_buttons.append(edit_btn)
            channel_combo = QComboBox()
            channel_combo.addItems(["Omni"] + [str(c) for c in range(1, 17)])
            channel_combo.setStyleSheet("font-size: 9px;")
            channel_combo.setToolTip("Which incoming MIDI channel plays this lane (Omni = every channel)")
            lane[0].layout().addWidget(channel_combo)
            self.combi_lane_midi_channel_combos.append(channel_combo)

            def _make_zone_spin(lo, hi, default, width=32):
                spin = QSpinBox()
                spin.setRange(lo, hi)
                spin.setValue(default)
                spin.setFixedWidth(width)
                spin.setStyleSheet("font-size: 9px;")
                return spin

            zone_box = QWidget()
            zone_lay = QVBoxLayout(zone_box)
            zone_lay.setContentsMargins(0, 0, 0, 0)
            zone_lay.setSpacing(1)

            key_row = QHBoxLayout(); key_row.setSpacing(1)
            key_lo_spin = _make_zone_spin(0, 127, 0)
            key_hi_spin = _make_zone_spin(0, 127, 127)
            key_row.addWidget(QLabel("Key:"))
            key_row.addWidget(key_lo_spin)
            key_row.addWidget(QLabel("-"))
            key_row.addWidget(key_hi_spin)
            zone_lay.addLayout(key_row)

            vel_row = QHBoxLayout(); vel_row.setSpacing(1)
            vel_lo_spin = _make_zone_spin(1, 127, 1)
            vel_hi_spin = _make_zone_spin(1, 127, 127)
            vel_row.addWidget(QLabel("Vel:"))
            vel_row.addWidget(vel_lo_spin)
            vel_row.addWidget(QLabel("-"))
            vel_row.addWidget(vel_hi_spin)
            zone_lay.addLayout(vel_row)

            zone_box.setToolTip(
                "Keyboard split / velocity switch for this lane -- only notes inside BOTH ranges\n"
                "trigger it (0=C-1, 60=Middle C/C4, 127=G9; velocity 1-127, harder hits = higher).\n"
                "Defaults (0-127 / 1-127) are the full range -- every note reaches this lane."
            )
            # Keep lo <= hi always true (a widget that can silently disagree with itself is worse
            # than just clamping) -- moving one bound past the other drags the other along with it.
            key_lo_spin.valueChanged.connect(lambda v, hi=key_hi_spin: hi.setValue(v) if v > hi.value() else None)
            key_hi_spin.valueChanged.connect(lambda v, lo=key_lo_spin: lo.setValue(v) if v < lo.value() else None)
            vel_lo_spin.valueChanged.connect(lambda v, hi=vel_hi_spin: hi.setValue(v) if v > hi.value() else None)
            vel_hi_spin.valueChanged.connect(lambda v, lo=vel_lo_spin: lo.setValue(v) if v < lo.value() else None)

            lane[0].layout().addWidget(zone_box)
            self.combi_lane_key_lo_spins.append(key_lo_spin)
            self.combi_lane_key_hi_spins.append(key_hi_spin)
            self.combi_lane_vel_lo_spins.append(vel_lo_spin)
            self.combi_lane_vel_hi_spins.append(vel_hi_spin)

            transpose_spin = QSpinBox()
            transpose_spin.setRange(-48, 48)
            transpose_spin.setValue(0)
            transpose_spin.setSuffix(" st")
            transpose_spin.setFixedWidth(56)
            transpose_spin.setStyleSheet("font-size: 9px;")
            transpose_spin.setToolTip(
                "Octave/semitone transpose for THIS lane only (-48..+48 semitones) -- lets the "
                "same or different Programs be stacked at different octaves (octave layering)."
            )
            transpose_row = QHBoxLayout(); transpose_row.setSpacing(1)
            transpose_row.addWidget(QLabel("Transpose:"))
            transpose_row.addWidget(transpose_spin)
            lane[0].layout().addLayout(transpose_row)
            self.combi_lane_transpose_spins.append(transpose_spin)

            combi_strips_lay.addWidget(lane[0])
            self.combi_lane_rows.append(lane)
        combi_strips_lay.addStretch()
        self._refresh_combi_program_combos()

        combi_scroll = QScrollArea()
        combi_scroll.setWidgetResizable(True)
        combi_scroll.setWidget(combi_strips_widget)
        combi_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        combi_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        combi_mixer_row = QHBoxLayout(); combi_mixer_row.setSpacing(4)
        combi_mixer_row.addWidget(_build_lane_legend(["#", "Level", "Pan", "Program", "MIDI Ch"]))
        combi_mixer_row.addWidget(combi_scroll, stretch=1)
        combi_lay.addLayout(combi_mixer_row, 1)
        tab_osc_root_lay.addWidget(self.osc_combi_widget)
        self.osc_combi_widget.setVisible(False)

        self.osc_prog_radio.toggled.connect(self.osc_prog_scroll.setVisible)
        self.osc_combi_radio.toggled.connect(self.osc_combi_widget.setVisible)
        self.osc_prog_radio.toggled.connect(self.transmit_knob_packet)
        self.osc_combi_radio.toggled.connect(self.transmit_knob_packet)
        self.osc_sequencer_radio.toggled.connect(self.transmit_knob_packet)
        self.osc_sequencer_radio.toggled.connect(
            lambda checked: self.tab_widget.setCurrentWidget(self.tab_sequencer) if checked else None
        )

        # ----------------------------------------------------
        # SEQUENCER TAB CONTENT (its own dedicated top-level tab, not nested in Play -- per the
        # user's explicit correction, and never merged with Combi's view). 32 lanes (up to 16
        # Program/MIDI + up to 16 Audio, a single flexible pool -- see backend.c's SEQ_LANE_COUNT
        # comment), a real transport panel (Play/Pause/Stop, a live state readout, and an
        # elapsed-time/bars:beats readout) -- see poll_transport_triggers in backend.c. Pressing
        # Play auto-starts every Seq-mode Audio lane and begins recording every Rec-armed
        # Program/MIDI lane (armed via that lane's own Seq/Mute/Rec dropdown below); there's no
        # separate global Record button/trigger.
        # ----------------------------------------------------
        seq_outer_lay = QVBoxLayout(self.tab_sequencer)
        seq_outer_lay.setContentsMargins(6, 6, 6, 6); seq_outer_lay.setSpacing(6)

        transport_box = QGroupBox("Transport")
        transport_lay = QHBoxLayout(transport_box); transport_lay.setSpacing(8)
        # Seek controls (Reset/Back/Forward) precede Play/Pause/Stop, standard transport layout
        # order. All three work in every transport state (Stopped/Paused/Playing) and reposition
        # every lane's own playback position, not just the display -- see
        # poll_transport_triggers/transport_reposition_lanes in backend.c.
        self.seq_btn_seek_start = QPushButton("\u23ee")
        self.seq_btn_seek_start.setToolTip("Reset to the very beginning (0:00)")
        self.seq_btn_seek_back = QPushButton("\u23ea 1 Measure")
        self.seq_btn_seek_back.setToolTip("Back one measure")
        for btn in (self.seq_btn_seek_start, self.seq_btn_seek_back):
            transport_lay.addWidget(btn)
        self.seq_btn_play = QPushButton("\u25b6 Play")
        self.seq_btn_pause = QPushButton("\u23f8 Pause")
        self.seq_btn_stop = QPushButton("\u23f9 Stop")
        for btn in (self.seq_btn_play, self.seq_btn_pause, self.seq_btn_stop):
            transport_lay.addWidget(btn)
        self.seq_btn_seek_fwd = QPushButton("1 Measure \u23e9")
        self.seq_btn_seek_fwd.setToolTip("Forward one measure")
        transport_lay.addWidget(self.seq_btn_seek_fwd)
        # Metronome: a plain on/off toggle (not an edge-triggered counter like the buttons above
        # -- this is sustained state), engaging a cheap, hardcoded 400Hz/8ms square-wave click on
        # each beat while the transport is PLAYING -- see poll_metronome() in backend.c. Not a
        # Program voice, so it deliberately never touches the oscillator/filter/envelope
        # machinery, and stays off by default (metronome_enable defaults to 0).
        self.seq_btn_metronome = QPushButton("\U0001f3b5 Metronome")
        self.seq_btn_metronome.setCheckable(True)
        self.seq_btn_metronome.setToolTip("Click on each beat while playing (400Hz, 8ms square wave)")
        transport_lay.addWidget(self.seq_btn_metronome)
        # Quantize: snaps a Rec-armed lane's recorded note-ON timestamps to the nearest grid line
        # at the current tempo (note lengths are left as-played -- see backend.c's
        # seq_quantize_enable comment). Off by default, same "opt-in, doesn't change existing
        # behavior" posture as Metronome above.
        self.seq_btn_quantize = QPushButton("Quantize")
        self.seq_btn_quantize.setCheckable(True)
        self.seq_btn_quantize.setToolTip(
            "Snap recorded note-ON timestamps to the nearest grid line at the current tempo "
            "(note lengths are left exactly as played)."
        )
        transport_lay.addWidget(self.seq_btn_quantize)
        self.seq_quantize_resolution_combo = QComboBox()
        self.seq_quantize_resolution_combo.addItems(["1/4", "1/8", "1/8 Triplet", "1/16", "1/16 Triplet", "1/32"])
        self.seq_quantize_resolution_combo.setCurrentIndex(3)  # 1/16 -- matches backend's own default (seq_quantize_resolution=4)
        self.seq_quantize_resolution_combo.setToolTip("Quantize grid resolution")
        transport_lay.addWidget(QLabel("Resolution:"))
        transport_lay.addWidget(self.seq_quantize_resolution_combo)
        # Tempo sync source: Internal (Tempo knob/tempo-map, unchanged default) or External --
        # slaves tempo to incoming MIDI Clock (standalone app, real MIDI cable/hardware sequencer)
        # or the host's own transport tempo (CLAP, hosted in a DAW) -- see backend.c's
        # seq_sync_source comment. The lock label reflects /hds/status/midi_clock_locked so a
        # missing/misrouted clock source shows as "Waiting..." instead of silently doing nothing.
        self.seq_sync_source_combo = QComboBox()
        self.seq_sync_source_combo.addItems(["Internal", "External (MIDI Clock / Host Tempo)"])
        self.seq_sync_source_combo.setToolTip(
            "Internal: follow the Tempo knob/tempo-map. External: slave to incoming MIDI Clock "
            "(standalone, from a cable/hardware sequencer) or the host's own tempo (CLAP, in a DAW)."
        )
        transport_lay.addWidget(QLabel("Sync:"))
        transport_lay.addWidget(self.seq_sync_source_combo)
        self.seq_sync_lock_label = QLabel("")
        self.seq_sync_lock_label.setStyleSheet("font-size: 9px; color: #888;")
        transport_lay.addWidget(self.seq_sync_lock_label)
        self.seq_sync_source_combo.currentIndexChanged.connect(self._update_seq_sync_lock_label)
        self._update_seq_sync_lock_label()
        # Real, edge-triggered counters -- same idiom as each Sequencer audio lane's own Preview
        # button (_preview_seq_audio/preview_trigger): the backend's poll_transport_triggers
        # detects a change in the value since last block, so each click just increments and
        # resends the whole packet. There's no separate global "Record" button/trigger -- Rec is
        # armed per lane via that lane's own Seq/Mute/Rec dropdown (already in each lane strip);
        # pressing Play here starts every Rec-armed lane recording automatically, matching a
        # classic hardware step-sequencer's transport (recording is a per-track arm state, not a
        # separate global action).
        self.seq_transport_play_trigger = 0
        self.seq_transport_pause_trigger = 0
        self.seq_transport_stop_trigger = 0
        self.seq_transport_seek_start_trigger = 0
        self.seq_transport_seek_back_trigger = 0
        self.seq_transport_seek_fwd_trigger = 0
        self.seq_btn_play.clicked.connect(self._seq_transport_play)
        self.seq_btn_pause.clicked.connect(self._seq_transport_pause)
        self.seq_btn_stop.clicked.connect(self._seq_transport_stop)
        self.seq_btn_seek_start.clicked.connect(self._seq_transport_seek_start)
        self.seq_btn_seek_back.clicked.connect(self._seq_transport_seek_back)
        self.seq_btn_seek_fwd.clicked.connect(self._seq_transport_seek_fwd)
        self.seq_btn_metronome.toggled.connect(self.transmit_knob_packet)
        self.seq_btn_quantize.toggled.connect(self.transmit_knob_packet)
        self.seq_quantize_resolution_combo.currentIndexChanged.connect(self.transmit_knob_packet)
        self.seq_sync_source_combo.currentIndexChanged.connect(self.transmit_knob_packet)
        transport_lay.addSpacing(16)
        self.seq_transport_state_label = QLabel("STOPPED")
        self.seq_transport_state_label.setStyleSheet("font-weight: bold;")
        transport_lay.addWidget(self.seq_transport_state_label)
        transport_lay.addSpacing(16)
        self.seq_elapsed_label = QLabel("00:00.0")
        self.seq_elapsed_label.setStyleSheet("font-family: monospace; font-size: 14px;")
        transport_lay.addWidget(QLabel("Elapsed:")); transport_lay.addWidget(self.seq_elapsed_label)
        self.seq_bars_beats_label = QLabel("0:1")
        self.seq_bars_beats_label.setStyleSheet("font-family: monospace; font-size: 14px;")
        transport_lay.addWidget(QLabel("Bars\u200a:\u200aBeats:")); transport_lay.addWidget(self.seq_bars_beats_label)
        transport_lay.addStretch()
        # Song save/load: a self-contained folder (config + recorded MIDI + copied-in audio) you
        # can come back to and keep working on later -- see _pick_and_save_song()'s own docstring.
        self.btn_save_song = QPushButton("Save Song")
        self.btn_load_song = QPushButton("Load Song")
        transport_lay.addWidget(self.btn_save_song)
        transport_lay.addWidget(self.btn_load_song)
        self.btn_save_song.clicked.connect(self._pick_and_save_song)
        self.btn_load_song.clicked.connect(self._pick_and_load_song)
        seq_outer_lay.addWidget(transport_box)

        self.seq_status_label = QLabel("")
        self.seq_status_label.setStyleSheet("color: #c0392b; font-style: italic;")
        seq_outer_lay.addWidget(self.seq_status_label)

        # MIDI import (Standard MIDI File, e.g. exported from Reaper) + the Markers list it (or a
        # hand-typed marker) populates -- see _pick_and_import_midi()/hds_song_import_smf() in
        # backend.c. A real .mid/.midi reader, distinct from Save/Load Song's own small internal
        # round-trip format above (which can't read a file from another program at all).
        import_box = QGroupBox("Import MIDI / Song Markers")
        import_lay = QHBoxLayout(import_box)
        self.btn_import_midi = QPushButton("Import MIDI (.mid)…")
        self.btn_import_midi.setToolTip(
            "Reads a Standard MIDI File (Type 0 or 1): each note-carrying track becomes one "
            "Sequencer lane, and the file's own text markers appear in the list to the right."
        )
        import_lay.addWidget(self.btn_import_midi)
        self.smf_import_tempo_check = QCheckBox("Import tempo")
        self.smf_import_tempo_check.setChecked(True)
        self.smf_import_tempo_check.setToolTip(
            "Also apply the file's own tempo map to the Tempo knob/tempo-map (so the Arp, LFO "
            "Tempo Sync, metronome, and Quantize grid follow it too). Note timing itself always "
            "uses the file's own tempo regardless of this -- this only controls whether HDS's "
            "live tempo follows along as well."
        )
        import_lay.addWidget(self.smf_import_tempo_check)
        self.smf_import_status_label = QLabel("")
        self.smf_import_status_label.setStyleSheet("font-style: italic; color: #888;")
        import_lay.addWidget(self.smf_import_status_label)
        import_lay.addStretch()
        self.marker_list_widget = QListWidget()
        self.marker_list_widget.setFixedHeight(70)
        self.marker_list_widget.setToolTip("Double-click a marker to jump the Sequencer playhead there.")
        self.marker_list_widget.itemDoubleClicked.connect(self._jump_to_marker)
        import_lay.addWidget(self.marker_list_widget, 1)
        self.btn_import_midi.clicked.connect(self._pick_and_import_midi)
        seq_outer_lay.addWidget(import_box)

        seq_strips_widget = QWidget()
        seq_strips_lay = QHBoxLayout(seq_strips_widget)
        seq_strips_lay.setContentsMargins(0, 0, 0, 0); seq_strips_lay.setSpacing(3)
        self.seq_lane_rows = []  # 32 x (strip, fader, pan_dial, type_combo, mode_combo)
        self.seq_lane_audio_widgets = []  # 32 x dict, see _build_seq_audio_controls
        # Mute/Rec toggle buttons + Overwrite-vs-Overdub dropdown (Program/MIDI lanes only in
        # spirit -- Audio lanes still get a Mute button since they can be transport-driven too,
        # but Rec/Overdub are meaningless for them and simply have no effect on the backend side;
        # not worth hiding conditionally here since Type can be flipped live). See
        # _build_seq_lane_rec_mute_controls's own comment.
        self.seq_lane_mute_buttons = []  # 32 x QPushButton (checkable)
        self.seq_lane_rec_buttons = []   # 32 x QPushButton (checkable)
        self.seq_lane_overdub_combos = []  # 32 x QComboBox
        # Per-lane MIDI channel selector (Sequencer only -- Combi lanes stay hardcoded Omni, no
        # real independent lanes exist there yet). "Omni" (index 0, sent as backend's 0) or a
        # specific channel 1-16 (combo index N sent as N) -- filters which Rec-armed lane(s) an
        # incoming note gets RECORDED into (seq_record_midi_event in backend.c); live Program
        # dispatch stays fully channel-agnostic regardless of this. Built separately and appended
        # onto the strip afterward, same idiom _build_seq_audio_controls already uses, to keep
        # _build_seq_lane_row's own return shape unchanged.
        self.seq_lane_midi_channel_combos = []  # 32 x QComboBox
        # Per-lane "Load MIDI..." (Program/MIDI lanes only) -- see _build_seq_midi_controls' own
        # comment. Parallel to seq_lane_audio_widgets, same "list of dicts, one per lane" shape.
        self.seq_lane_midi_import_widgets = []  # 32 x dict, see _build_seq_midi_controls
        for i in range(SEQ_LANE_COUNT):
            lane = _build_seq_lane_row(i, show_seq_controls=True)
            mute_btn, rec_btn, overdub_combo = _build_seq_lane_rec_mute_controls(i)
            lane[0].layout().addWidget(mute_btn)
            lane[0].layout().addWidget(rec_btn)
            lane[0].layout().addWidget(overdub_combo)
            self.seq_lane_mute_buttons.append(mute_btn)
            self.seq_lane_rec_buttons.append(rec_btn)
            self.seq_lane_overdub_combos.append(overdub_combo)
            midi_import_widgets = _build_seq_midi_controls(i)
            lane[0].layout().addWidget(midi_import_widgets["box"])
            self.seq_lane_midi_import_widgets.append(midi_import_widgets)
            audio_widgets = _build_seq_audio_controls(i)
            lane[0].layout().addWidget(audio_widgets["box"])
            self.seq_lane_audio_widgets.append(audio_widgets)
            channel_combo = QComboBox()
            channel_combo.addItems(["Omni"] + [str(c) for c in range(1, 17)])
            channel_combo.setStyleSheet("font-size: 9px;")
            lane[0].layout().addWidget(channel_combo)
            self.seq_lane_midi_channel_combos.append(channel_combo)
            seq_strips_lay.addWidget(lane[0])
            self.seq_lane_rows.append(lane)
        seq_strips_lay.addStretch()

        # QScrollArea, matching the FX/LFO tabs' existing pattern for many stacked rows -- here
        # scrolling HORIZONTALLY (a strip band, not a tall list), same as Combi's above.
        seq_scroll = QScrollArea()
        seq_scroll.setWidgetResizable(True)
        seq_scroll.setWidget(seq_strips_widget)
        seq_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        seq_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        seq_mixer_row = QHBoxLayout(); seq_mixer_row.setSpacing(4)
        seq_mixer_row.addWidget(_build_lane_legend(["#", "Level", "Pan", "Type", "Mode", "Mute/Rec", "MIDI", "Audio"]))
        seq_mixer_row.addWidget(seq_scroll, stretch=1)
        seq_outer_lay.addLayout(seq_mixer_row, 1)

        # Audio/MIDI type enforcement: capped at COMBI_PROGRAM_COUNT/AUDIO_LANE_RESOURCE_COUNT
        # active of each kind, since each type is meant to eventually draw from its own backend
        # resource pool (neither exists yet -- this is frontend-only validation this pass). Also
        # shows/hides that lane's "Load Audio" box -- meaningless for a Program/MIDI lane.
        def _enforce_seq_lane_type_cap(idx, new_index):
            # Only the Audio direction is actually capped: AUDIO_LANE_RESOURCE_COUNT is a real
            # future scarce-resource ceiling (RAM-cached audio buffers), so blocking a 17th Audio
            # lane is meaningful. Program/MIDI has no equivalent scarcity here -- every lane that
            # ISN'T Audio is trivially "the rest" (up to all 32, never more, since SEQ_LANE_COUNT
            # itself already bounds that) -- so there is nothing to enforce going the other way.
            # (COMBI_PROGRAM_COUNT is Combi's own, unrelated ceiling on simultaneous
            # Program-synthesis instances -- conflating the two previously meant every lane
            # defaulting to Program/MIDI already "exceeded" a same-named cap, making it impossible
            # to ever revert a lane back to Program/MIDI. Fixed here.)
            type_combo = self.seq_lane_rows[idx][3]
            if new_index == 1:
                audio_count = sum(1 for row in self.seq_lane_rows if row[3].currentIndex() == 1)
                if audio_count > AUDIO_LANE_RESOURCE_COUNT:
                    type_combo.blockSignals(True)
                    type_combo.setCurrentIndex(0)
                    type_combo.blockSignals(False)
                    self.seq_status_label.setText(f"Lane {idx + 1}: already {AUDIO_LANE_RESOURCE_COUNT} Audio lanes active -- reverted")
                    new_index = 0
            self.seq_lane_audio_widgets[idx]["box"].setVisible(new_index == 1)
            self.seq_lane_midi_import_widgets[idx]["box"].setVisible(new_index == 0)
            self.seq_lane_midi_channel_combos[idx].setVisible(new_index == 0)
        for i, lane in enumerate(self.seq_lane_rows):
            lane[3].currentIndexChanged.connect(lambda new_index, idx=i: _enforce_seq_lane_type_cap(idx, new_index))
        # ----------------------------------------------------
        # FILTERS TAB CONTENT (flat 5-slot chain, exclusive Off/LPF/HPF/BPF/Notch per slot)
        # ----------------------------------------------------
        filters_outer_lay = QVBoxLayout(self.tab_filters)
        filters_column = QVBoxLayout()
        filters_column.setSpacing(6)

        # Chain Mode: governs how the 5 filter slots combine -- Series (0->1->2->3->4, each slot
        # fed by the previous one's output) or Parallel (all 5 read the same mixer input, summed
        # and averaged over however many are actually active -- an Off slot is silent, excluded
        # from both the sum and the divisor, per the user's own explicit choice). Deliberately
        # simple: no A/B distinction, no blend weights, no Multi sub-engine -- see backend.c's
        # own comments on ProgramState.filters/FilterParams for the full design.
        chain_mode_row = QHBoxLayout()
        chain_mode_row.addWidget(QLabel("<b>Filter Chain Mode:</b>"))
        self.filter_chain_mode_combo = QComboBox()
        self.filter_chain_mode_combo.addItems(["Series (1->2->3->4->5)", "Parallel (Summed)"])
        chain_mode_row.addWidget(self.filter_chain_mode_combo)
        chain_mode_row.addStretch()
        filters_column.addLayout(chain_mode_row)

        # Key Track and ext-MIDI CC/aftertouch modulation have no dedicated always-visible knob --
        # both live inside each slot's own Cutoff right-click menu (_show_filter_cutoff_menu), a
        # short preset list mirroring the existing CC-source preset list's own UI pattern. Cached
        # here as plain Python lists (not widget-backed), same shape as self.lfo_mod_source_cc.
        self.filter_key_track = [0.0] * 5
        self.filter_cutoff_mod_source_cc = [-1.0] * 5
        self._KEY_TRACK_PRESETS = [("Off", 0.0), ("50%", 50.0), ("100%", 100.0), ("150%", 150.0)]

        self.filter_cutoff_dials, self.filter_res_dials = [], []
        self.filter_type_combos, self.filter_slope_combos = [], []

        for i in range(5):
            box = QGroupBox(f"Filter {i + 1}")
            lay = QHBoxLayout(); lay.setSpacing(4); lay.setContentsMargins(4, 4, 4, 4)

            c_dial = PotentiometerKnob(f"F{i + 1} Freq", self)
            c_dial.setRange(40, 18000); c_dial.setValue(800 if i == 0 else 2000)
            c_dial.setContextMenuCallback(lambda pos, idx=i: self._show_filter_cutoff_menu(idx, pos))
            self.filter_cutoff_dials.append(c_dial)

            q_dial = PotentiometerKnob(f"F{i + 1} Res", self)
            q_dial.setRange(0, 100); q_dial.setValue(20)
            q_dial.setContextMenuCallback(lambda pos, idx=i: self._show_dest_assign_menu(91 + idx * 2, pos))
            self.filter_res_dials.append(q_dial)

            v_slope = QComboBox(); v_slope.addItems(["2-Pole (12dB/oct)", "4-Pole (24dB/oct)"])
            self.filter_slope_combos.append(v_slope)

            v_type = QComboBox(); v_type.addItems(["Off", "LPF", "HPF", "BPF", "Notch"])
            v_type.setCurrentIndex(1 if i == 0 else 0)  # Filter 1 defaults to an active LPF, rest Off
            self.filter_type_combos.append(v_type)

            lay.addWidget(QLabel("Freq:")); lay.addWidget(c_dial)
            lay.addWidget(QLabel("Res:")); lay.addWidget(q_dial)
            lay.addWidget(QLabel("Slope:")); lay.addWidget(v_slope)
            lay.addWidget(QLabel("Type:")); lay.addWidget(v_type)
            box.setLayout(lay)
            filters_column.addWidget(box)

        # --- Post-filter 3-band EQ (Low Shelf / Mid Peak / High Shelf) -- see backend.c's
        # EqBandParams/ProgramState.eq_bands comment. Applied once per Program per sample, right
        # after the 5-slot filter chain above, before FX/routing. Shelf bands (Low/High) have no
        # Q control -- backend.c fixes their shelf slope at S=1, matching a typical hardware
        # channel-strip EQ rather than a fully general parametric-shelf design; only the Mid Peak
        # band exposes Q.
        eq_box = QGroupBox("3-Band EQ (post-filter)")
        eq_lay = QHBoxLayout(); eq_lay.setSpacing(4); eq_lay.setContentsMargins(4, 4, 4, 4)
        self.eq_freq_dials, self.eq_gain_dials, self.eq_q_dials = [], [], []
        eq_band_names = ["Low Shelf", "Mid Peak", "High Shelf"]
        eq_freq_defaults = [100, 1000, 5000]
        for b in range(3):
            band_box = QGroupBox(eq_band_names[b])
            band_lay = QHBoxLayout(); band_lay.setSpacing(4); band_lay.setContentsMargins(4, 4, 4, 4)

            f_dial = PotentiometerKnob(f"{eq_band_names[b]} Freq", self)
            f_dial.setRange(20, 20000); f_dial.setValue(eq_freq_defaults[b])
            f_dial.value_formatter = lambda v: f"{v} Hz"
            self.eq_freq_dials.append(f_dial)

            g_dial = PotentiometerKnob(f"{eq_band_names[b]} Gain", self)
            g_dial.setRange(-18, 18); g_dial.setValue(0)
            g_dial.value_formatter = lambda v: f"{v:+d} dB"
            self.eq_gain_dials.append(g_dial)

            band_lay.addWidget(QLabel("Freq:")); band_lay.addWidget(f_dial)
            band_lay.addWidget(QLabel("Gain:")); band_lay.addWidget(g_dial)

            if b == 1:  # Mid Peak only
                q_dial = PotentiometerKnob("Mid Q", self)
                q_dial.setRange(1, 100); q_dial.setValue(7)  # /10.0 at transmit time -> Q 0.1-10.0, default ~0.7
                q_dial.value_formatter = lambda v: f"{v / 10.0:.1f}"
                band_lay.addWidget(QLabel("Q:")); band_lay.addWidget(q_dial)
                self.eq_q_dials.append(q_dial)
            else:
                self.eq_q_dials.append(None)

            band_box.setLayout(band_lay)
            eq_lay.addWidget(band_box)
        eq_box.setLayout(eq_lay)
        filters_column.addWidget(eq_box)

        filters_column.addStretch()
        filters_outer_lay.addLayout(filters_column)
        # ----------------------------------------------------
        # LFOs TAB CONTENT
        # ----------------------------------------------------
        lfo_outer_lay = QVBoxLayout(self.tab_lfos)
        lfo_group = QGroupBox("")
        lfo_layout = QVBoxLayout()
        lfo_layout.setSpacing(4)
        lfo_layout.setContentsMargins(6, 6, 6, 6)

        lfo_title_row = QHBoxLayout()
        lfo_title_row.setContentsMargins(6, 2, 6, 2)
        lfo_title_row.addWidget(QLabel("<b>Assignable LFOs</b>"))
        lfo_title_row.addStretch()

        lbl_assign_header = QLabel("<b>Assign</b>")
        lbl_assign_header.setFixedWidth(110)
        lbl_assign_header.setStyleSheet("font-size: 10px; color: #00adb5;")
        lbl_assign_header.setAlignment(Qt.AlignmentFlag.AlignCenter)
        lfo_title_row.addWidget(lbl_assign_header)
        lfo_layout.addLayout(lfo_title_row)

        self.lfo_combos, self.lfo_dials, self.lfo_readouts = [], [], []
        self.lfo_sync_combos, self.lfo_dest_combos, self.lfo_depth_dials, self.lfo_visualizers = [], [], [], []
        # Tempo Sync -- a SEPARATE concept from lfo_sync_combos above (which is key_sync, a
        # note-on phase-reset toggle): this instead decides whether Rate is a free Hz value or a
        # note-division of the current tempo. See LFO_SYNC_DIVISION_LABELS' own comment.
        self.lfo_tempo_sync_combos = []
        # Curve/Drive: same single physical pot, reinterpreted by wave -- bipolar Curve
        # (center=linear ramp) for Tri/Saw, Drive/Saturation (right half only) for Sine, hidden
        # for Square/Off. See PackLfoParams.curve_drive's own comment (backend.c).
        self.lfo_curve_labels, self.lfo_curve_dials = [], []
        self.lfo_per_key_checks, self.lfo_key_track_dials, self.lfo_vel_track_dials, self.lfo_key_track_boxes = [], [], [], []
        self.lfo_phase_dials = []
        # Modulation Source (right-click on an LFO's Depth pot): -1.0 = None, 0.0-127.0 = that
        # MIDI CC number, 128.0 = channel aftertouch. Mirrors PackLfoParams.mod_source_cc.
        self.lfo_mod_source_cc = [-1.0] * LFO_COUNT
        # Common Modulation Source choices offered on every LFO's right-click menu.
        self._MOD_SOURCE_PRESETS = [
            ("None", -1.0),
            ("Mod Wheel (CC1)", 1.0),
            ("Breath Controller (CC2)", 2.0),
            ("Foot Controller (CC4)", 4.0),
            ("Expression (CC11)", 11.0),
            ("Channel Aftertouch", 128.0),
        ]

        matrix_shared_routes = [
            "None", "Osc 1 Pitch", "Osc 2 Pitch", "Osc 1+2 Pitch", "O1 Fine", "O2 Fine", "Osc 1 Amp", "Osc 2 Amp",
            # Targets 8-13 are retired -- the old Filter A/B/Multi-unit slots these used to name no
            # longer exist (see the filter section simplification: 5 flat filter slots replaced
            # the whole Filter A Normal/Multi + Filter B system). Left as harmless neutral
            # placeholders, list position unchanged, rather than reused for something else, so an
            # old saved Program's LFO/EG still pointed at one of these becomes a no-op, not a
            # crash or a silent reinterpretation -- see backend.c's own retired-target comment.
            "(unused)", "(unused)", "(unused)", "(unused)",
            "(unused)", "(unused)",
            "LFO 1 Rate", "LFO 2 Rate", "LFO 3 Rate", "LFO 4 Rate",
            "LFO 1 Dp", "LFO 2 Dp", "LFO 3 Dp", "LFO 4 Dp", "FM Depth", "Mix Balance",
            "Osc 1 Phase", "Osc 2 Phase", "O1 Unison Detune", "O2 Unison Detune",
            "LFO 1 Phase", "LFO 2 Phase", "LFO 3 Phase", "LFO 4 Phase",
            # Added for full oscillator/mixer coverage: pan (targets 32/33), pulse width
            # (34/35, only audible when that oscillator's Shape is Pulse), and Osc 3/4's own
            # amp/pan/tilt (36-40, targets 36/37/38 were "Noise Amp/Pan/Tilt" and 39/40 were
            # "Sub Amp/Pan" before Osc 3/4 became full peers -- same target numbers, relabeled).
            "Osc 1 Pan", "Osc 2 Pan", "Osc 1 Pulse Width", "Osc 2 Pulse Width",
            "Osc 3 Amp", "Osc 3 Pan", "Osc 3 Tilt", "Osc 4 Amp", "Osc 4 Pan",
            # Osc 3/4 pitch/phase/unison-detune + Osc 4 tilt (targets 41-47, appended -- see
            # backend.c's render_audio_block dispatch loops).
            "Osc 3 Pitch", "Osc 4 Pitch", "Osc 3 Phase", "Osc 4 Phase",
            "Osc 3 Unison Detune", "Osc 4 Unison Detune", "Osc 4 Tilt",
        ] + [f"LFO {n} Rate" for n in range(5, LFO_COUNT + 1)] \
          + [f"LFO {n} Dp" for n in range(5, LFO_COUNT + 1)] \
          + [f"LFO {n} Phase" for n in range(5, LFO_COUNT + 1)] + [
            # Targets 48-83 (above): LFO 5-16 Rate/Dp/Phase -- closes a real pre-existing gap
            # (LFO 1-4 Rate/Depth, targets 14-21, used to be selectable here but were completely
            # unwired in the backend; LFO 1-4 Phase, 28-31, already worked). All 16 LFOs now
            # support all three as mod destinations -- see lfo_rate_target_index/
            # lfo_depth_target_index/lfo_phase_target_index in backend.c.
            # Targets 84-89 are retired (see the 8-13 comment above -- same filter simplification).
            "(unused)", "(unused)",
            "(unused)", "(unused)", "(unused)", "(unused)",
            # Targets 90-99: Filter 1-5 Cutoff/Resonance, one flat slot per filter -- the new
            # 5-slot filter chain's own mod destinations (see ProgramState.filters in backend.c).
            "Filter 1 Freq", "Filter 1 Res", "Filter 2 Freq", "Filter 2 Res",
            "Filter 3 Freq", "Filter 3 Res", "Filter 4 Freq", "Filter 4 Res",
            "Filter 5 Freq", "Filter 5 Res",
        ]
        # Stored on self -- the generalized right-click "Modulate From" menu (see
        # _show_dest_assign_menu) needs this list later, well outside __init__, to render each
        # LFO/EG's currently-assigned destination as a human-readable label.
        self.matrix_shared_routes = matrix_shared_routes

        for x in range(1, LFO_COUNT + 1):
            lfo_row = QHBoxLayout()
            lfo_row.setSpacing(4)
            lfo_row.addWidget(QLabel(f"<b>LFO {x}</b>"))

            combo = QComboBox()
            combo.addItems(["Tri", "Sine", "Saw", "Sq", "Off"])
            combo.setCurrentIndex(4)  # Off by default (per-user request: all 16 LFOs start bypassed)
            combo.setStyleSheet("font-size: 10px;")
            vis = WaveformVisualizer()
            self.lfo_visualizers.append(vis)
            lfo_row.addWidget(combo)
            lfo_row.addWidget(vis)
            self.lfo_combos.append(combo)

            # Curve/Drive: visible only for Tri/Sine/Saw (indices 0/1/2), hidden for Sq/Off --
            # see PackLfoParams.curve_drive's own comment. Label swaps "Curve"<->"Drive" on wave
            # change (wired in the shared post-construction signal loop, below).
            curve_label = QLabel("Curve")
            curve_label.setStyleSheet("font-size: 9px;")
            curve_dial = PotentiometerKnob(f"LFO {x} Curve/Drive", self)
            curve_dial.setRange(-100, 100)
            curve_dial.setValue(0)
            curve_dial.setToolTip("Tri/Saw: bipolar Curve, center = linear. Sine: Drive/Saturation, only the right half (0..100) has effect.")
            lfo_row.addWidget(curve_label)
            lfo_row.addWidget(curve_dial)
            self.lfo_curve_labels.append(curve_label)
            self.lfo_curve_dials.append(curve_dial)

            lfo_idx_for_menu = x - 1

            dial = PotentiometerKnob(f"LFO {x} Rate", self)
            dial.setRange(1, 2000)
            dial.setValue(45)
            dial.setContextMenuCallback(lambda pos, i=lfo_idx_for_menu: self._show_lfo_row_menu(i, 'rate', pos))

            readout = QLabel("4.5 Hz")
            readout.setFixedWidth(50)
            readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
            readout.setStyleSheet("QLabel { background-color: #2b2b36; color: #00adb5; font-weight: bold; font-size: 9px; }")
            lfo_row.addWidget(dial)
            lfo_row.addWidget(readout)
            self.lfo_dials.append(dial)
            self.lfo_readouts.append(readout)

            # Tempo Sync: "Free" (index 0, default) means Rate's own Hz dial above sets this
            # LFO's speed, exactly as before this feature existed; any other selection instead
            # locks the rate to that note division of the current tempo, and the Rate dial (still
            # transmitted, just ignored by the backend while synced) is grayed out here so it
            # doesn't look like it's doing something it isn't.
            sync_div_combo = QComboBox()
            sync_div_combo.addItems(LFO_SYNC_DIVISION_LABELS)
            sync_div_combo.setStyleSheet("font-size: 10px;")
            sync_div_combo.setToolTip("Tempo Sync: locks this LFO's rate to a note division of the current tempo instead of the Rate dial's own Hz value")
            sync_div_combo.currentIndexChanged.connect(lambda idx, d=dial: d.setEnabled(idx == 0))
            lfo_row.addWidget(sync_div_combo)
            self.lfo_tempo_sync_combos.append(sync_div_combo)

            dp_dial = PotentiometerKnob(f"LFO {x} Dp", self)
            dp_dial.setRange(0, 100)
            dp_dial.setValue(50)
            lfo_row.addWidget(QLabel("Dp:"))
            lfo_row.addWidget(dp_dial)
            self.lfo_depth_dials.append(dp_dial)
            dp_dial.setContextMenuCallback(lambda pos, i=lfo_idx_for_menu: self._show_lfo_row_menu(i, 'depth', pos))
            self._update_mod_source_tooltip(lfo_idx_for_menu)

            phase_dial = PotentiometerKnob(f"LFO {x} Phase", self)
            phase_dial.setRange(0, 360)
            phase_dial.setValue(0)
            phase_dial.setToolTip("Shifts this LFO's read-phase; modulatable via LFO/EG (right-click to assign)")
            lfo_row.addWidget(QLabel("Ph:"))
            lfo_row.addWidget(phase_dial)
            self.lfo_phase_dials.append(phase_dial)
            phase_dial.setContextMenuCallback(lambda pos, i=lfo_idx_for_menu: self._show_lfo_row_menu(i, 'phase', pos))

            s_combo = QComboBox()
            s_combo.addItems(["Free", "Sync", "Whl"])
            s_combo.setStyleSheet("font-size: 10px;")
            lfo_row.addWidget(s_combo)
            self.lfo_sync_combos.append(s_combo)

            # Per-Key mode: this LFO becomes a per-VOICE resource (its own independent phase per
            # note, instead of one phase shared by every voice of the Program) -- the standard
            # "per-key LFO" feature (per-note vibrato/tremolo starting points, Rate tracking key
            # position/velocity). ALWAYS uses the efficient decimated/interpolated computation on
            # the backend, even for LFO 1/2 which are otherwise full audio-rate in the shared
            # mode -- per-voice computation multiplies cost by polyphony, so this mode never pays
            # the exact per-sample path. Key/Vel Track only matter (and are only shown) in this
            # mode -- see PackLfoParams.per_key_mode/key_track/vel_track in backend.c.
            pk_check = QCheckBox("PK")
            pk_check.setToolTip("Per-Key mode: independent LFO phase per voice/note (instead of one shared phase for the whole Program). Always uses the efficient decimated computation.")
            lfo_row.addWidget(pk_check)
            self.lfo_per_key_checks.append(pk_check)

            kt_dial = PotentiometerKnob(f"LFO {x} Key Track", self)
            kt_dial.setRange(-100, 100)
            kt_dial.setValue(0)
            kt_dial.setToolTip("Per-Key only: Rate scales by keyboard position (+-100 = a full octave-per-octave multiplier, centered on middle C)")
            vt_dial = PotentiometerKnob(f"LFO {x} Vel Track", self)
            vt_dial.setRange(-100, 100)
            vt_dial.setValue(0)
            vt_dial.setToolTip("Per-Key only: Rate scales by note-on velocity (+-100 = full range, centered on velocity 64)")
            kt_box = QWidget(); kt_lay = QHBoxLayout(kt_box); kt_lay.setContentsMargins(0, 0, 0, 0); kt_lay.setSpacing(2)
            kt_lay.addWidget(QLabel("Key:")); kt_lay.addWidget(kt_dial); kt_lay.addWidget(QLabel("Vel:")); kt_lay.addWidget(vt_dial)
            _retain_hidden(kt_box)
            kt_box.setVisible(False)
            lfo_row.addWidget(kt_box)
            self.lfo_key_track_dials.append(kt_dial)
            self.lfo_vel_track_dials.append(vt_dial)
            self.lfo_key_track_boxes.append(kt_box)
            pk_check.toggled.connect(lambda checked, box=kt_box: box.setVisible(checked))

            d_combo = QComboBox()
            d_combo.addItems(matrix_shared_routes)
            d_combo.setStyleSheet("font-size: 10px;")
            d_combo.setFixedWidth(110)
            lfo_row.addWidget(d_combo)
            self.lfo_dest_combos.append(d_combo)
            lfo_layout.addLayout(lfo_row)

        # Without a trailing stretch, a bare QVBoxLayout given more height than its content
        # needs (e.g. because another tab, like FX, forces the window taller) spreads that
        # leftover space roughly evenly between every row instead of leaving it below the last
        # one -- reading as the title floating in the middle of a mostly-empty box with the LFO
        # rows pushed toward the bottom. Pinning both the inner content and the outer group box
        # to the top fixes that at both levels.
        lfo_layout.addStretch()
        lfo_group.setLayout(lfo_layout)
        # 16 LFO rows don't fit in a fixed-height panel -- wrap in a QScrollArea (same pattern as
        # the EG/FX tabs) so the window can still be resized/shrunk freely.
        lfo_scroll = QScrollArea()
        lfo_scroll.setWidgetResizable(True)
        lfo_scroll.setWidget(lfo_group)
        lfo_outer_lay.addWidget(lfo_scroll)
        # ----------------------------------------------------
        # EGs TAB CONTENT
        # ----------------------------------------------------
        eg_outer_lay = QVBoxLayout(self.tab_egs)
        eg_group = QGroupBox("")
        eg_layout = QVBoxLayout()
        eg_layout.setSpacing(4)
        eg_layout.setContentsMargins(6, 6, 6, 6)

        eg_header_row = QHBoxLayout()
        eg_header_row.setContentsMargins(6, 2, 6, 2)
        eg_header_row.addWidget(QLabel("<b>Assignable Envelopes</b>"))
        eg_header_row.addStretch()

        self.btn_randomize_eg = QPushButton("🎲")
        self.btn_randomize_eg.setFixedSize(24, 22)
        self.btn_randomize_eg.setStyleSheet("font-size: 11px;")
        self.btn_randomize_eg.setToolTip("Randomize all 6 assignable EGs")
        eg_header_row.addWidget(self.btn_randomize_eg)
        eg_layout.addLayout(eg_header_row)
        self.btn_randomize_eg.clicked.connect(self.randomize_assignable_egs)

        self.envelopes, self.eg_dest_combos, self.eg_intensity_dials = [], [], []

        for x in range(1, 7):
            panel = ADSRPanel(f"EG {x}", (1, 1000), (1, 2000), (1, 2000))
            panel.title_layout.addStretch()

            lbl_int = QLabel("<font color='#00adb5'>Int:</font>")
            lbl_int.setStyleSheet("font-size: 10px;")
            panel.title_layout.addWidget(lbl_int)

            int_dial = PotentiometerKnob(f"EG {x} Int", self)
            int_dial.setRange(-100, 100)
            int_dial.setValue(100)
            panel.title_layout.addWidget(int_dial)
            self.eg_intensity_dials.append(int_dial)

            eg_drop = QComboBox()
            eg_drop.addItems(matrix_shared_routes)
            eg_drop.setStyleSheet("font-size: 10px;")
            eg_drop.setFixedWidth(110)
            panel.title_layout.addWidget(eg_drop)

            self.eg_dest_combos.append(eg_drop)
            eg_layout.addWidget(panel)
            self.envelopes.append(panel)

        eg_layout.addStretch()
        eg_group.setLayout(eg_layout)

        # 6 full ADSRPanels (each with its own graph + dial row) stacked vertically with no
        # scroll wrap used to force a huge minimum window height (~1139px measured) regardless
        # of which tab was actually open -- QTabWidget sizes itself to fit the LARGEST tab, not
        # just the active one. Same QScrollArea fix as the Play tab's oscillator panel and the
        # FX tab: lets the window be resized/shrunk freely, scrolling this tab's content instead
        # of dictating the whole app's minimum size.
        eg_scroll = QScrollArea()
        eg_scroll.setWidgetResizable(True)
        eg_scroll.setWidget(eg_group)
        eg_outer_lay.addWidget(eg_scroll)

        # ----------------------------------------------------
        # FX TAB CONTENT (12-slot serial FX bus -- runs on the full mix, post-filter,
        # pre-master-volume, and after the external input has been summed in; see
        # process_fx_chain() in backend.c)
        # ----------------------------------------------------
        fx_outer_lay = QVBoxLayout(self.tab_fx)
        fx_outer_lay.setContentsMargins(4, 4, 4, 4)

        fx_header_row = QHBoxLayout()
        fx_header = QLabel("<b>FX Bus</b> — 12 slots, applied to the full mix (synth + External Input)")
        fx_header.setStyleSheet("font-size: 10px; color: #888; padding: 2px;")
        fx_header_row.addWidget(fx_header)
        fx_header_row.addStretch()

        fx_outer_lay.addLayout(fx_header_row)

        # Series/Parallel is now a per-source choice (each source's OWN assigned slots run in
        # Series or Parallel independently of every other source) rather than one tab-wide
        # switch -- see fx_source_routing_mode[SOURCE_COUNT] in backend.c. One small toggle per
        # source, laid out in a compact grid since there are SOURCE_COUNT (17) of them (8 Combi
        # Programs + Combi Mix + 8 ext-in pairs).
        routing_box = QGroupBox("FX Routing Mode (per source)")
        routing_box.setStyleSheet("QGroupBox { font-size: 9px; font-weight: bold; }")
        routing_grid = QGridLayout(routing_box)
        routing_grid.setSpacing(3)
        self.fx_source_routing_btns = []
        routing_cols = 3
        for src in range(SOURCE_COUNT):
            btn = QPushButton(f"{SOURCE_NAMES[src]}: Series")
            btn.setCheckable(True)
            btn.setFixedSize(140, 20)
            btn.setStyleSheet("font-size: 9px;")
            btn.setToolTip(
                "Series: this source's own assigned slots run in array order, each fed the\n"
                "previous one's output.\n"
                "Parallel: this source's own assigned slots all fed the same input independently,\n"
                "summed and averaged."
            )
            btn.toggled.connect(
                lambda checked, s=src, b=btn: b.setText(f"{SOURCE_NAMES[s]}: {'Parallel' if checked else 'Series'}")
            )
            routing_grid.addWidget(btn, src // routing_cols, src % routing_cols)
            self.fx_source_routing_btns.append(btn)
        fx_outer_lay.addWidget(routing_box)

        fx_scroll = QScrollArea()
        fx_scroll.setWidgetResizable(True)
        fx_scroll_content = QWidget()
        fx_scroll_lay = QVBoxLayout(fx_scroll_content)
        fx_scroll_lay.setSpacing(3)
        fx_scroll_lay.setContentsMargins(2, 2, 2, 2)

        self.fx_type_combos, self.fx_mix_dials, self.fx_bypass_buttons = [], [], []
        # Which source (Program or an ext-in pair) feeds each slot -- see fx_source on
        # FXSlotParams in backend.c. Mirrors fx_type_combos in every way that matters for
        # snapshot/restore/swap (a slot's source assignment moves with it on reorder).
        self.fx_source_combos = []
        # Chain To Next -- see FXSlotParams.chain_to_next in backend.c and the button's own
        # tooltip/comment at its construction site below.
        self.fx_chain_to_next_buttons = []
        # Per-slot lists of the 8 generic param widgets, so _apply_fx_slot_type can relabel,
        # re-range/re-populate, and show/hide them once a slot's Type is known. Each param
        # position carries BOTH a knob and a combo (only one shown at a time) so a discrete
        # choice param (see FX_TYPE_METADATA) can present real option text instead of a knob.
        self.fx_param_dials, self.fx_param_combos, self.fx_param_labels, self.fx_param_columns = [], [], [], []

        for slot_idx in range(FX_SLOT_COUNT):
            slot_box = QGroupBox(f"FX {slot_idx + 1}")
            slot_box.setStyleSheet("QGroupBox { font-size: 10px; font-weight: bold; }")
            slot_lay = QHBoxLayout(slot_box)
            slot_lay.setSpacing(4)
            slot_lay.setContentsMargins(4, 2, 4, 2)

            type_combo = QComboBox()
            # Each entry here has a matching FX_TYPE_* case in the backend's process_fx_chain
            # switch and an entry in FX_TYPE_METADATA -- the three must stay in step.
            type_combo.addItems([entry["name"] for entry in FX_TYPE_METADATA])
            type_combo.setFixedWidth(80)
            type_combo.setStyleSheet("font-size: 9px;")
            slot_lay.addWidget(type_combo)
            self.fx_type_combos.append(type_combo)

            source_combo = QComboBox()
            source_combo.addItems(SOURCE_NAMES)
            source_combo.setFixedWidth(70)
            source_combo.setStyleSheet("font-size: 9px;")
            source_combo.setToolTip(
                "Which source feeds this slot -- an individual Combi Program, the whole-stack\n"
                "\"Combi Mix\" bus (only meaningful in Combi mode -- see the Performance Mode\n"
                "selector), or an external-input pair. A Program can be both individually routed\n"
                "AND included in Combi Mix at the same time -- that's intentional."
            )
            slot_lay.addWidget(source_combo)
            self.fx_source_combos.append(source_combo)

            param_dials, param_combos, param_labels, param_columns = [], [], [], []
            for p_idx in range(FX_SLOT_PARAM_COUNT):
                p_col_widget = QWidget()
                p_col = QVBoxLayout(p_col_widget); p_col.setContentsMargins(0, 0, 0, 0); p_col.setSpacing(0)
                p_lbl = QLabel(f"P{p_idx + 1}"); p_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter); p_lbl.setStyleSheet("font-size: 7px;")
                p_col.addWidget(p_lbl)
                p_dial = PotentiometerKnob(f"FX {slot_idx + 1} P{p_idx + 1}", self)
                p_dial.setRange(-100, 100)
                p_dial.setValue(0)
                p_col.addWidget(p_dial, alignment=Qt.AlignmentFlag.AlignCenter)
                p_combo = QComboBox()
                p_combo.setStyleSheet("font-size: 9px;")
                p_combo.setVisible(False) # shown instead of p_dial only for a "choice" param
                p_col.addWidget(p_combo)
                slot_lay.addWidget(p_col_widget)
                param_dials.append(p_dial)
                param_combos.append(p_combo)
                param_labels.append(p_lbl)
                param_columns.append(p_col_widget)
            self.fx_param_dials.append(param_dials)
            self.fx_param_combos.append(param_combos)
            self.fx_param_labels.append(param_labels)
            self.fx_param_columns.append(param_columns)

            mix_col = QVBoxLayout(); mix_col.setSpacing(0)
            mix_col.addWidget(QLabel("Mix"), alignment=Qt.AlignmentFlag.AlignCenter)
            mix_dial = PotentiometerKnob(f"FX {slot_idx + 1} Mix", self)
            mix_dial.setRange(0, 100)
            mix_dial.setValue(100)
            mix_col.addWidget(mix_dial, alignment=Qt.AlignmentFlag.AlignCenter)
            slot_lay.addLayout(mix_col)
            self.fx_mix_dials.append(mix_dial)

            bypass_btn = QPushButton("By")
            bypass_btn.setCheckable(True)
            bypass_btn.setFixedSize(22, 22)
            bypass_btn.setStyleSheet("QPushButton { font-size: 8px; } QPushButton:checked { background-color: #b58900; color: black; }")
            bypass_btn.setToolTip("Bypass this slot (keeps its state alive -- e.g. a delay's tail -- unlike switching Type to Off)")
            slot_lay.addWidget(bypass_btn)
            self.fx_bypass_buttons.append(bypass_btn)

            # Chain To Next: only meaningful when this slot's SOURCE is running in Parallel mode
            # (see the per-source Series/Parallel toggles below) -- inert (harmless, no effect) in
            # Series mode, where every slot is already chained unconditionally. When on, the NEXT
            # slot belonging to the same source is fed THIS slot's own output instead of starting
            # a fresh parallel branch, letting several slots run in series as one branch alongside
            # a source's other parallel slots/branches -- e.g. two independent 3-slot serial
            # chains summed together. No "next slot" exists on the very last slot, so its own
            # button is disabled there (matching the Down-reorder button's own boundary
            # convention just below).
            chain_btn = QPushButton("→")
            chain_btn.setCheckable(True)
            chain_btn.setFixedSize(18, 22)
            chain_btn.setStyleSheet("QPushButton { font-size: 10px; } QPushButton:checked { background-color: #2e8b57; color: white; }")
            chain_btn.setToolTip(
                "Chain this slot's output into the next slot (same source), continuing this as one\n"
                "series sub-chain instead of a separate parallel branch. Only has an effect while\n"
                "this slot's source is set to Parallel routing -- see the Routing buttons below."
            )
            chain_btn.setEnabled(slot_idx < FX_SLOT_COUNT - 1)
            slot_lay.addWidget(chain_btn)
            self.fx_chain_to_next_buttons.append(chain_btn)

            # Reorder: these move the slot's whole configuration (type/params/mix/bypass) to the
            # neighboring position -- the row widgets themselves never move, only the values they
            # hold, so nothing else about the slot's wiring/signal connections needs to change.
            # Fixed at construction time since a button's own row index never changes: slot 0 has
            # no Up, the last slot has no Down.
            reorder_col = QVBoxLayout(); reorder_col.setSpacing(1)
            up_btn = QPushButton("▲")
            up_btn.setFixedSize(16, 16)
            up_btn.setStyleSheet("QPushButton { font-size: 8px; padding: 0px; }")
            up_btn.setToolTip("Move this slot up (swaps with the slot above -- Series routing processes top to bottom)")
            up_btn.setEnabled(slot_idx > 0)
            up_btn.clicked.connect(lambda checked=False, i=slot_idx: self._swap_fx_slots(i, i - 1))
            down_btn = QPushButton("▼")
            down_btn.setFixedSize(16, 16)
            down_btn.setStyleSheet("QPushButton { font-size: 8px; padding: 0px; }")
            down_btn.setToolTip("Move this slot down (swaps with the slot below -- Series routing processes top to bottom)")
            down_btn.setEnabled(slot_idx < FX_SLOT_COUNT - 1)
            down_btn.clicked.connect(lambda checked=False, i=slot_idx: self._swap_fx_slots(i, i + 1))
            reorder_col.addWidget(up_btn)
            reorder_col.addWidget(down_btn)
            slot_lay.addLayout(reorder_col)

            fx_scroll_lay.addWidget(slot_box)
            self._apply_fx_slot_type(slot_idx, 0) # start on "Off": no params shown

        fx_scroll_lay.addStretch()
        fx_scroll.setWidget(fx_scroll_content)
        fx_outer_lay.addWidget(fx_scroll)

        # ----------------------------------------------------
        # ROUTING TAB CONTENT: which sources (Program + each ext-in pair) feed which hardware
        # output pair(s) -- a many-to-many grid, since a source can feed multiple output pairs at
        # once and several sources can share one pair. See source_routing/ext_in_enable/
        # ext_in_gain on SynthSharedControls in backend.c. The output-pair COLUMNS are rebuilt
        # (shown/hidden) from /hds/status/detected_output_channels, mirroring the Recording tab's
        # _rebuild_record_channel_checkboxes() pattern (which rebuilds ROWS from
        # detected_input_channels) -- same idiom, transposed.
        # ----------------------------------------------------
        routing_outer_lay = QVBoxLayout(self.tab_routing)
        routing_outer_lay.setContentsMargins(6, 6, 6, 6)
        routing_outer_lay.setSpacing(6)

        routing_header = QLabel(
            "Each source's own FX-processed signal can be sent to one or more hardware output "
            "pairs. Ext In pairs also need Enable + Gain here before anything reaches them at all "
            "(gain always starts at -inf on launch, regardless of what it was last set to)."
        )
        routing_header.setStyleSheet("font-size: 9px; color: #888;")
        routing_header.setWordWrap(True)
        routing_outer_lay.addWidget(routing_header)

        routing_scroll = QScrollArea()
        routing_scroll.setWidgetResizable(True)
        routing_content = QWidget()
        self.routing_grid = QGridLayout(routing_content)
        self.routing_grid.setSpacing(4)
        self.routing_grid.setContentsMargins(4, 4, 4, 4)

        # Column layout: 0=Source label, 1=Enable, 2=Gain, 3..3+OUTPUT_PAIR_MAX-1 = one checkbox
        # per output pair. Header row (row 0) is built once; the "Out N" column headers are
        # created for every possible pair up to OUTPUT_PAIR_MAX and shown/hidden by
        # _rebuild_routing_output_columns() as detected_output_channels changes live.
        self.routing_grid.addWidget(QLabel("<b>Source</b>"), 0, 0)
        self.routing_grid.addWidget(QLabel("<b>Enable</b>"), 0, 1)
        self.routing_grid.addWidget(QLabel("<b>Gain</b>"), 0, 2)
        self.routing_output_col_headers = []
        for p in range(OUTPUT_PAIR_MAX):
            hdr = QLabel(f"Out {p + 1}")
            hdr.setStyleSheet("font-size: 9px;")
            hdr.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.routing_grid.addWidget(hdr, 0, 3 + p)
            self.routing_output_col_headers.append(hdr)
        # One more column after the output-pair checkboxes: a per-Ext-In-pair VU meter (there's
        # plenty of spare horizontal space in each row already). Only ext-in pairs are real audio
        # INPUTS this tab has anything to meter -- Programs/Combi Mix are synth-generated, not
        # captured, and already have their own VU meter on the Output strip. Metering itself is
        # only ever computed backend-side while this tab is actually on-screen (see
        # gui_routing_tab_visible/_on_main_tab_changed) -- a true no-op the rest of the time.
        self.routing_grid.addWidget(QLabel("<b>VU</b>"), 0, 3 + OUTPUT_PAIR_MAX)

        self.routing_ext_in_enable_cbs = []
        self.routing_ext_in_gain_dials = []
        self.routing_ext_in_vu_meters = []
        self.routing_output_pair_cbs = []  # [source_idx][pair_idx] -> QCheckBox
        for src in range(SOURCE_COUNT):
            row = src + 1
            self.routing_grid.addWidget(QLabel(SOURCE_NAMES[src]), row, 0)
            if src < COMBI_PROGRAM_COUNT + 1:
                # A Program (0..COMBI_PROGRAM_COUNT-1) or the Combi Mix bus (COMBI_PROGRAM_COUNT)
                # has no enable/gain of its own here -- Programs are always live (master_volume
                # lives on the main page, and each oscillator has its own Vol knob); only their
                # output routing is configured on this tab.
                self.routing_ext_in_enable_cbs.append(None)
                self.routing_ext_in_gain_dials.append(None)
                self.routing_ext_in_vu_meters.append(None)
            else:
                enable_cb = QCheckBox()
                self.routing_grid.addWidget(enable_cb, row, 1)
                self.routing_ext_in_enable_cbs.append(enable_cb)
                gain_dial = PotentiometerKnob(f"{SOURCE_NAMES[src]} Gain", self)
                gain_dial.setRange(0, 200)
                # Always starts at -inf (silence), never at unity -- an upstream mic pre/interface
                # can be left at any gain, and there must be no chance of a burst hitting the FX
                # bus the instant Enable is checked. The user always has to manually bring this up.
                gain_dial.setValue(0)
                gain_dial.value_formatter = lambda v: "-inf dB" if v <= 0 else f"{v}%"
                self.routing_grid.addWidget(gain_dial, row, 2)
                self.routing_ext_in_gain_dials.append(gain_dial)

                vu_meter = QProgressBar()
                vu_meter.setRange(0, 100); vu_meter.setValue(0); vu_meter.setTextVisible(False)
                vu_meter.setFixedSize(70, 14)
                vu_meter.setStyleSheet("""
                    QProgressBar { border: 1px solid #444; background: #222; }
                    QProgressBar::chunk { background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0f0, stop:0.7 #ff0, stop:1 #f00); }
                """)
                self.routing_grid.addWidget(vu_meter, row, 3 + OUTPUT_PAIR_MAX)
                self.routing_ext_in_vu_meters.append(vu_meter)

            pair_cbs = []
            for p in range(OUTPUT_PAIR_MAX):
                pair_cb = QCheckBox()
                pair_cb.setToolTip(f"Route {SOURCE_NAMES[src]} to output pair {p + 1}")
                self.routing_grid.addWidget(pair_cb, row, 3 + p, alignment=Qt.AlignmentFlag.AlignCenter)
                pair_cbs.append(pair_cb)
            self.routing_output_pair_cbs.append(pair_cbs)

        # Default: Program -> output pair 1, matching backend init_default_controls()'s default
        # (today's only output pair) -- everything else starts unrouted/disabled, same as the
        # backend's own defaults, so a freshly-launched frontend doesn't fight a running backend
        # over what the "current" state is.
        self.routing_output_pair_cbs[0][0].setChecked(True)

        routing_content.setLayout(self.routing_grid)
        routing_scroll.setWidget(routing_content)
        routing_outer_lay.addWidget(routing_scroll)

        self._last_detected_output_channels = -1
        self._rebuild_routing_output_columns(1)  # 1 pair (today's stereo-only default) until status arrives

        # ----------------------------------------------------
        # AUDIO TAB CONTENT (device selection + external input)
        # ----------------------------------------------------
        audio_outer_lay = QVBoxLayout(self.tab_audio)
        audio_outer_lay.setContentsMargins(6, 6, 6, 6)
        audio_outer_lay.setSpacing(6)

        device_box = QGroupBox("Audio Devices")
        device_lay = QVBoxLayout()
        device_lay.setSpacing(6)

        out_row = QHBoxLayout()
        out_row.addWidget(QLabel("Output:"))
        self.output_device_combo = QComboBox()
        self.output_device_combo.addItems(_list_alsa_devices(["aplay", "-L"]))
        out_row.addWidget(self.output_device_combo, stretch=1)
        device_lay.addLayout(out_row)

        in_row = QHBoxLayout()
        in_row.addWidget(QLabel("Input:"))
        self.input_device_combo = QComboBox()
        self.input_device_combo.addItems(_list_alsa_devices(["arecord", "-L"]))
        in_row.addWidget(self.input_device_combo, stretch=1)
        device_lay.addLayout(in_row)

        refresh_row = QHBoxLayout()
        self.btn_refresh_devices = QPushButton("Rescan Devices")
        self.btn_refresh_devices.setFixedHeight(24)
        refresh_row.addWidget(self.btn_refresh_devices)
        refresh_row.addStretch()
        device_lay.addLayout(refresh_row)
        self.btn_refresh_devices.clicked.connect(self.refresh_audio_devices)

        device_note = QLabel("Changing a device applies live — the backend closes and reopens that stream.")
        device_note.setStyleSheet("font-size: 9px; color: #888;")
        device_note.setWordWrap(True)
        device_lay.addWidget(device_note)

        device_box.setLayout(device_lay)
        audio_outer_lay.addWidget(device_box)

        # ----------------------------------------------------
        # PERFORMANCE: real, direct engine-tuning controls -- how many voices can be active at
        # once, how many samples get rendered per cycle, and the ALSA buffer depth. These used
        # to be numbers picked once at compile time and shipped fixed (see MAX_VOICES/
        # BUFFER_SIZE in backend.c); the right value for any of them is a property of the actual
        # machine this runs on, not something to guess once for every deployment. All three
        # apply live -- no restart needed -- and the backend's stderr diagnostics ([voices]/
        # [perf]/[audio]) exist specifically so raising these is an informed process: push a
        # value up, watch for warnings under real playing, back off if you see them.
        # ----------------------------------------------------
        perf_box = QGroupBox("Performance")
        perf_lay = QVBoxLayout()
        perf_lay.setSpacing(6)

        poly_row = QHBoxLayout()
        poly_row.addWidget(QLabel("Polyphony:"))
        self.polyphony_spin = QSpinBox()
        self.polyphony_spin.setRange(1, MAX_VOICES_HARD_LIMIT)
        self.polyphony_spin.setValue(32)
        self.polyphony_spin.setSuffix(" voices")
        self.polyphony_spin.setStyleSheet("font-size: 10px; color: #00adb5; background-color: #2b2b36; font-weight: bold;")
        self.polyphony_spin.setToolTip(
            "How many notes can be sounding at once before the oldest/quietest one gets stolen\n"
            "to make room for a new one (stealing always prefers an already-fading voice over a\n"
            "held one, so it's rarely audible). Higher = more headroom for dense chords and\n"
            "sustain-pedal buildup, at a real, roughly linear CPU cost -- watch the terminal for\n"
            f"[voices]/[perf]/[audio] warnings while raising this. Hard ceiling: {MAX_VOICES_HARD_LIMIT}."
        )
        poly_row.addWidget(self.polyphony_spin, stretch=1)
        perf_lay.addLayout(poly_row)

        bs_row = QHBoxLayout()
        bs_row.addWidget(QLabel("Block Size:"))
        self.block_size_combo = QComboBox()
        self._BLOCK_SIZE_PRESETS = [32, 64, 128, 256, 512, 1024]
        self.block_size_combo.addItems([f"{v} samples" for v in self._BLOCK_SIZE_PRESETS])
        self.block_size_combo.setCurrentIndex(self._BLOCK_SIZE_PRESETS.index(128)) # matches the backend default
        self.block_size_combo.setToolTip(
            "How many samples get rendered and handed to the audio driver per cycle. Smaller =\n"
            "snappier MIDI-to-audio response and finer-grained modulation, at more per-block\n"
            "overhead; larger = less overhead per sample, at coarser MIDI timing. Independent of\n"
            "Latency (the ALSA buffer depth) -- this only controls the render/write chunk size."
        )
        bs_row.addWidget(self.block_size_combo, stretch=1)
        perf_lay.addLayout(bs_row)

        # Latency: a friendly stand-in for the raw ALSA buffer/period sizing -- kept as presets
        # rather than a bare millisecond field, since unlike Polyphony/Block Size there isn't
        # really a meaningful "direct" unit here beyond milliseconds itself, and presets keep the
        # common choices one click away. Real, useful knob though, not a placebo: it's a
        # safety-margin/responsiveness tradeoff for marginal, occasional CPU overruns -- see
        # audio_latency_ms in backend.c. Doesn't fix a genuinely sustained overload (too many
        # voices, too expensive a patch, consistently over budget); for that, the fix is Polyphony
        # or a lighter patch, not a bigger buffer.
        latency_row = QHBoxLayout()
        latency_row.addWidget(QLabel("Latency:"))
        self.audio_latency_combo = QComboBox()
        self._LATENCY_PRESETS = [
            ("Snappy (15ms)", 15.0),
            ("Normal (25ms)", 25.0),
            ("Safe (50ms)", 50.0),
            ("Extra Safe (100ms)", 100.0),
        ]
        self.audio_latency_combo.addItems([label for label, _ in self._LATENCY_PRESETS])
        self.audio_latency_combo.setCurrentIndex(1) # Normal (25ms), matches the backend default
        self.audio_latency_combo.setToolTip(
            "Bigger = more forgiving of an occasional slow block (dense chords/heavy patches)\n"
            "at the cost of added delay between playing a note and hearing it.\n"
            "If you're hearing crackling/dropouts under heavy playing, try Safe or Extra Safe."
        )
        latency_row.addWidget(self.audio_latency_combo, stretch=1)
        perf_lay.addLayout(latency_row)

        perf_note = QLabel("Polyphony and Block Size apply on the next block, no reopen needed. Latency applies live too — the backend closes and reopens the output/input stream(s).")
        perf_note.setStyleSheet("font-size: 9px; color: #888;")
        perf_note.setWordWrap(True)
        perf_lay.addWidget(perf_note)

        perf_box.setLayout(perf_lay)
        audio_outer_lay.addWidget(perf_box)

        midi_box = QGroupBox("MIDI Input")
        midi_lay = QVBoxLayout()
        midi_lay.setSpacing(6)

        midi_row = QHBoxLayout()
        midi_row.addWidget(QLabel("Device:"))
        self.midi_device_combo = QComboBox()
        self.midi_device_combo.addItems(_list_midi_input_devices())
        midi_row.addWidget(self.midi_device_combo, stretch=1)
        midi_lay.addLayout(midi_row)

        midi_note = QLabel(
            "Auto connects to every real MIDI device present, and keeps listening for ones "
            "plugged in later — no aconnect needed. Pick a specific device here to use only that "
            "one (it still reconnects automatically if unplugged and replugged)."
        )
        midi_note.setStyleSheet("font-size: 9px; color: #888;")
        midi_note.setWordWrap(True)
        midi_lay.addWidget(midi_note)

        midi_box.setLayout(midi_lay)
        audio_outer_lay.addWidget(midi_box)

        audio_outer_lay.addStretch()

        # ----------------------------------------------------
        # RECORDING TAB CONTENT -- this is also an FX processor: loopback the synth's own
        # post-FX output (which already includes any processed External Input, once
        # ext_in_enable is on -- they're the same signal once mixed) to disk, AND/OR use the
        # currently-selected Input device as a plain multichannel recorder in its own right
        # (raw capture, independent of ext_in_enable -- works with no notes playing at all).
        # Writes 32-bit float WAV via the backend's WavRecorder -- see record_enable/
        # record_include_synth_output/record_downmix_stereo/record_input_channel_enable/
        # record_path in backend.c.
        # ----------------------------------------------------
        recording_outer_lay = QVBoxLayout(self.tab_recording)
        recording_outer_lay.setContentsMargins(6, 6, 6, 6)
        recording_outer_lay.setSpacing(6)

        rec_header = QLabel(
            "Record the synth's own output, raw channels from the Input device (Audio tab), or both together — "
            "to a genuine multichannel WAV file or downmixed to stereo."
        )
        rec_header.setStyleSheet("font-size: 9px; color: #888;")
        rec_header.setWordWrap(True)
        recording_outer_lay.addWidget(rec_header)

        sources_box = QGroupBox("Sources")
        sources_lay = QVBoxLayout()
        sources_lay.setSpacing(6)

        self.record_synth_output_cb = QCheckBox("Synth Output (post-FX, L/R)")
        self.record_synth_output_cb.setChecked(True)
        self.record_synth_output_cb.setToolTip(
            "The final stereo mix, exactly what's going to the DAC -- already includes any\n"
            "processed External Input (Audio tab) once that's enabled, since it's summed in\n"
            "before the FX bus. There's no way to record 'just the synth' vs 'just the\n"
            "processed external input' separately once they've been mixed and processed."
        )
        sources_lay.addWidget(self.record_synth_output_cb)

        input_channels_label = QLabel("<b>Raw Input Channels</b> (direct from the Input device, Audio tab — works with no notes playing)")
        input_channels_label.setStyleSheet("font-size: 10px;")
        input_channels_label.setWordWrap(True)
        sources_lay.addWidget(input_channels_label)

        self.record_channels_container = QWidget()
        self.record_channels_grid = QGridLayout(self.record_channels_container)
        self.record_channels_grid.setContentsMargins(12, 0, 0, 0)
        self.record_channels_grid.setSpacing(4)
        sources_lay.addWidget(self.record_channels_container)
        self.record_input_channel_cbs = []

        self.record_channels_detect_label = QLabel("Detecting input channels…")
        self.record_channels_detect_label.setStyleSheet("font-size: 9px; color: #888;")
        sources_lay.addWidget(self.record_channels_detect_label)

        sources_box.setLayout(sources_lay)
        recording_outer_lay.addWidget(sources_box)

        output_box = QGroupBox("Output")
        output_lay = QVBoxLayout()
        output_lay.setSpacing(6)

        mode_row = QHBoxLayout()
        self.record_mode_multichannel = QRadioButton("Multichannel WAV")
        self.record_mode_multichannel.setChecked(True)
        self.record_mode_multichannel.setToolTip("Every selected channel becomes its own channel in one WAV file.")
        self.record_mode_downmix = QRadioButton("Stereo Downmix")
        self.record_mode_downmix.setToolTip("Every selected channel is summed equally into a single L/R pair.")
        mode_row.addWidget(self.record_mode_multichannel)
        mode_row.addWidget(self.record_mode_downmix)
        mode_row.addStretch()
        output_lay.addLayout(mode_row)

        format_row = QHBoxLayout()
        format_row.addWidget(QLabel("Format:"))
        self.record_format_wav = QRadioButton("WAV")
        self.record_format_wav.setChecked(True)
        self.record_format_wav.setToolTip("32-bit float, lossless, larger files -- the widest-compatibility choice.")
        self.record_format_wavpack = QRadioButton("WavPack (disabled for now)")
        self.record_format_wavpack.setToolTip("Temporarily disabled -- see HDS_WAVPACK_ENABLED in backend.c. Recording uses WAV only until this is revisited.")
        self.record_format_wavpack.setEnabled(False)
        format_row.addWidget(self.record_format_wav)
        format_row.addWidget(self.record_format_wavpack)
        format_row.addStretch()
        output_lay.addLayout(format_row)

        folder_row = QHBoxLayout()
        folder_row.addWidget(QLabel("Folder:"))
        self.record_folder = os.path.expanduser("~/HDS_Recordings")
        self.record_folder_field = QLineEdit(self.record_folder)
        self.record_folder_field.setReadOnly(True)
        self.record_folder_field.setStyleSheet("color: #00adb5; background-color: #1e1e24; font-size: 10px; padding: 3px;")
        folder_row.addWidget(self.record_folder_field, stretch=1)
        self.btn_browse_record_folder = QPushButton("Browse…")
        self.btn_browse_record_folder.setFixedHeight(24)
        folder_row.addWidget(self.btn_browse_record_folder)
        output_lay.addLayout(folder_row)

        folder_note = QLabel("Each take is named HDS_<timestamp>.wav (or .wv for WavPack) in this folder, so consecutive takes never collide.")
        folder_note.setStyleSheet("font-size: 9px; color: #888;")
        folder_note.setWordWrap(True)
        output_lay.addWidget(folder_note)

        output_box.setLayout(output_lay)
        recording_outer_lay.addWidget(output_box)

        control_row = QHBoxLayout()
        self.record_toggle_btn = QPushButton("● Record")
        self.record_toggle_btn.setCheckable(True)
        self.record_toggle_btn.setFixedHeight(36)
        self.record_toggle_btn.setStyleSheet(
            "QPushButton { font-size: 13px; font-weight: bold; } "
            "QPushButton:checked { background-color: #b00020; color: white; }"
        )
        control_row.addWidget(self.record_toggle_btn)

        self.record_status_label = QLabel("Not recording")
        self.record_status_label.setStyleSheet("color: #888; font-size: 11px;")
        control_row.addWidget(self.record_status_label, stretch=1)
        recording_outer_lay.addLayout(control_row)

        recording_outer_lay.addStretch()

        self._current_record_path = ""
        self.btn_browse_record_folder.clicked.connect(self._pick_record_folder)
        self.record_toggle_btn.toggled.connect(self._on_record_toggled)
        for cb in (self.record_synth_output_cb,):
            cb.toggled.connect(self._update_record_button_enabled)
        self._update_record_button_enabled()

        # ----------------------------------------------------
        # MAPPING TAB CONTENT -- the real Sample Bank zone editor: drag-and-drop a .wav onto the
        # keyboard to map it, drag a zone's body to move it or an edge to resize its key/velocity
        # range, right-click-free click to preview a key. Every edit here is live (no separate
        # "Apply" step) -- same convention as every other control in this file, see
        # SamplerMappingGrid's mouseMoveEvent/dropEvent/keyPressEvent. No cap on how many zones
        # can exist; each is synced to the backend's catalog by its own stable SampleZone.zone_id.
        # ----------------------------------------------------
        mapping_outer_lay = QVBoxLayout(self.tab_mapping)
        mapping_outer_lay.setContentsMargins(6, 6, 6, 6)
        mapping_outer_lay.setSpacing(6)

        mapping_header = QLabel(
            "Drag a .wav file onto the keyboard below to map it. Drag a zone's body to move it, an edge to "
            "resize its key/velocity range. Shift-click or marquee-drag to select multiple; Delete/Backspace "
            "to remove. Set an oscillator's Shape (Play tab) to \"Sample\" to hear it."
        )
        mapping_header.setStyleSheet("font-size: 9px; color: #888;")
        mapping_header.setWordWrap(True)
        mapping_outer_lay.addWidget(mapping_header)

        mapping_top_bar = QHBoxLayout()
        mapping_top_bar.addWidget(QLabel("Oscillator:"))
        self.mapping_osc_combo = QComboBox()
        self.mapping_osc_combo.addItems(["Osc 1", "Osc 2", "Osc 3", "Osc 4"])
        self.mapping_osc_combo.setToolTip(
            "Which oscillator's own sample zones this grid shows/edits -- each of the 4 "
            "oscillators has its own independent zone set (see backend.c's SampleSlot."
            "assigned_osc), so a multisample patch can spread across more than one."
        )
        self.mapping_osc_combo.currentIndexChanged.connect(self._on_mapping_osc_changed)
        mapping_top_bar.addWidget(self.mapping_osc_combo)

        self.mapping_sfz_readout = QLabel("SFZ: (none loaded)")
        self.mapping_sfz_readout.setStyleSheet("font-size: 10px; color: #888;")
        self.mapping_sfz_readout.setToolTip(
            "Only reflects an SFZ this frontend itself loaded (Import SFZ… or a Program load) "
            "onto the selected oscillator -- a backend-native load (the --sfz-vcoN startup flags, "
            "or an /hds/zone/load_sfz message from elsewhere) has no way to report its path back "
            "here, and won't update this readout."
        )
        mapping_top_bar.addWidget(self.mapping_sfz_readout)

        mapping_top_bar.addWidget(QLabel("Root Key (selected zone):"))
        self.root_key_box = QComboBox()
        self.root_key_box.setFixedWidth(190)
        note_names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        for note in range(128):
            octave = (note // 12) - 1
            self.root_key_box.addItem(f"{note_names[note % 12]}{octave} ({note})", note)
        self.root_key_box.addItem("DRUM (Unpitched)", -1)
        self.root_key_box.setCurrentIndex(-1)
        self.root_key_box.currentIndexChanged.connect(self._on_root_key_selection_changed)
        mapping_top_bar.addWidget(self.root_key_box)

        btn_add_sample = QPushButton("+ Add Sample…")
        btn_add_sample.setToolTip("Fallback for when dragging a file in isn't convenient -- adds it at Middle C, same as a drop there.")
        btn_add_sample.clicked.connect(self._pick_and_add_sample)
        mapping_top_bar.addWidget(btn_add_sample)

        btn_import_sfz = QPushButton("Import SFZ…")
        btn_import_sfz.setToolTip("Bulk-adds zones from an SFZ multisample library (.wav samples only). Adds to the existing Sample Bank -- never replaces it.")
        btn_import_sfz.clicked.connect(self._pick_and_import_sfz)
        mapping_top_bar.addWidget(btn_import_sfz)

        btn_save_sfz = QPushButton("Save SFZ…")
        btn_save_sfz.setToolTip("Exports the current Sample Bank as a plain, standard .sfz file -- spec-compliant zone/region data only, nothing HDS- or OSC-specific. Lets this tab double as a standalone SFZ editor/creator.")
        btn_save_sfz.clicked.connect(self._pick_and_save_sfz)
        mapping_top_bar.addWidget(btn_save_sfz)

        mapping_top_bar.addStretch()
        self.mapping_status_label = QLabel("")
        self.mapping_status_label.setStyleSheet("font-size: 10px; color: #888;")
        mapping_top_bar.addWidget(self.mapping_status_label)
        mapping_outer_lay.addLayout(mapping_top_bar)

        self.mapping_grid = SamplerMappingGrid(parent_window=self)
        self.mapping_keyboard = ScalablePianoKeyboard(grid_widget=self.mapping_grid)
        mapping_outer_lay.addWidget(self.mapping_grid, stretch=1)
        mapping_outer_lay.addWidget(self.mapping_keyboard, stretch=0)

        self.zone_inspector = ZoneInspectorPanel(parent_window=self)
        mapping_outer_lay.addWidget(self.zone_inspector, stretch=0)

        # Which .sfz path (if any) this frontend itself most recently loaded onto each oscillator
        # -- see mapping_sfz_readout's own tooltip for the limits of what this can reflect.
        self.mapping_loaded_sfz_path = [None, None, None, None]
        self._on_mapping_osc_changed(0)

        # ----------------------------------------------------
        # ASSEMBLE AND ANCHOR TOUCH DECK TO ROOT LAYOUT
        # ----------------------------------------------------
        self.tab_widget.addTab(self.tab_play, "Play")
        self.tab_widget.addTab(self.tab_filters, "Filters")
        self.tab_widget.addTab(self.tab_lfos, "LFOs")
        self.tab_widget.addTab(self.tab_egs, "Envelopes")
        self.tab_widget.addTab(self.tab_fx, "FX")
        self.tab_widget.addTab(self.tab_routing, "Routing")
        self.tab_widget.addTab(self.tab_audio, "Devices")
        self.tab_widget.addTab(self.tab_recording, "Recording")
        self.tab_widget.addTab(self.tab_mapping, "Mapping")
        self.tab_widget.addTab(self.tab_sequencer, "Sequencer")

        outer_layout.addWidget(self.tab_widget, stretch=5)
        self.setLayout(outer_layout)

        # Connect visualizer hooks to drive static icon drawings, and the Unison Saw/dual-duty
        # Pulse-Width-or-Tilt boxes' visibility, identically for all 4 oscillators (they all share
        # the exact same OSC_SHAPE_NAMES palette now).
        UNISON_SAW_INDEX = OSC_SHAPE_NAMES.index("Unison Saw")
        NOISE_INDEX = OSC_SHAPE_NAMES.index("Noise")
        PULSE_INDEX = OSC_SHAPE_NAMES.index("Pulse")
        SAW_INDEX = OSC_SHAPE_NAMES.index("Saw")
        TRIANGLE_INDEX = OSC_SHAPE_NAMES.index("Triangle")
        SINE_INDEX = OSC_SHAPE_NAMES.index("Sine")

        def _wire_osc_strip(combo, vis, unison_box, pulse_box, pulse_label, pulse_knob, mono_check, curve_box, curve_label):
            def on_shape_changed(idx):
                vis.set_waveform(idx, is_lfo=False)
                unison_box.setVisible(idx == UNISON_SAW_INDEX)
                pulse_box.setVisible(idx in (PULSE_INDEX, NOISE_INDEX))
                # Monophonic Noise checkbox: visible only for Noise, and unchecked (backend no-op,
                # since mono_noise is meaningless for any other shape) the moment the shape leaves
                # Noise -- so a checked box can never sit silently behind a shape it doesn't apply
                # to, and switching back to Noise always starts from the safe default (unchecked).
                mono_check.setVisible(idx == NOISE_INDEX)
                if idx != NOISE_INDEX and mono_check.isChecked():
                    mono_check.setChecked(False)
                if idx == NOISE_INDEX:
                    pulse_label.setText("Tilt")
                else:
                    pulse_label.setText("Pulse Width")
                # Curve/Drive: same single physical pot, reinterpreted by Shape (see
                # ProgramState.vco{1-4}_curve_drive's own comment, backend.c) -- visible only for
                # the 3 shapes it applies to, label swaps between the two meanings.
                curve_box.setVisible(idx in (SAW_INDEX, TRIANGLE_INDEX, SINE_INDEX))
                curve_label.setText("Drive" if idx == SINE_INDEX else "Curve")
            combo.currentIndexChanged.connect(on_shape_changed)
            on_shape_changed(combo.currentIndex())

        _wire_osc_strip(self.vco1_combo, self.vco1_vis, self.vco1_unison_box, self.vco1_pulse_box, self.vco1_pulse_label, self.vco1_pulse_width, self.vco1_mono_noise, self.vco1_curve_box, self.vco1_curve_label)
        _wire_osc_strip(self.vco2_combo, self.vco2_vis, self.vco2_unison_box, self.vco2_pulse_box, self.vco2_pulse_label, self.vco2_pulse_width, self.vco2_mono_noise, self.vco2_curve_box, self.vco2_curve_label)
        _wire_osc_strip(self.vco3_combo, self.vco3_vis, self.vco3_unison_box, self.vco3_pulse_box, self.vco3_pulse_label, self.vco3_pulse_width, self.vco3_mono_noise, self.vco3_curve_box, self.vco3_curve_label)
        _wire_osc_strip(self.vco4_combo, self.vco4_vis, self.vco4_unison_box, self.vco4_pulse_box, self.vco4_pulse_label, self.vco4_pulse_width, self.vco4_mono_noise, self.vco4_curve_box, self.vco4_curve_label)

        # FM Algorithm: show only the depth dial(s) the selected algorithm actually uses.
        def _on_fm_algorithm_changed(idx):
            active_depths = set(FM_ALGORITHMS[idx]["depths"]) if 0 <= idx < len(FM_ALGORITHMS) else set()
            for key, row_widget in self.fm_depth_rows.items():
                row_widget.setVisible(key in active_depths)
        self.fm_algorithm_combo.currentIndexChanged.connect(_on_fm_algorithm_changed)
        _on_fm_algorithm_changed(self.fm_algorithm_combo.currentIndex())
    def randomize_assignable_egs(self):
        for i, panel in enumerate(self.envelopes):
            a_lo, a_hi = panel.dials["A"].value_range()
            h_lo, h_hi = panel.dials["H"].value_range()
            d_lo, d_hi = panel.dials["D"].value_range()
            dec_lo, dec_hi = panel.dials["D_phase"].value_range()
            r_lo, r_hi = panel.dials["R"].value_range()
            state = {
                "type": random.randint(0, panel.combo_type.count() - 1),
                "a": random.randint(a_lo, a_hi),
                "h": random.randint(h_lo, h_hi),
                "d": random.randint(d_lo, d_hi),
                "dec": random.randint(dec_lo, dec_hi),
                "s": random.randint(0, 100),
                "r": random.randint(r_lo, r_hi),
                "al": random.randint(0, 100),
                "pk": random.randint(20, 100),
            }
            panel.set_state(state)
            self.eg_dest_combos[i].setCurrentIndex(random.randint(0, self.eg_dest_combos[i].count() - 1))

    def _apply_fx_slot_type(self, slot_idx, type_index):
        """Reconfigures one FX slot's 8 generic param widgets for the effect Type just selected:
        relabels/re-ranges/shows the ones that effect actually uses (per FX_TYPE_METADATA) and
        hides the rest, so the UI never shows controls that don't do anything for the current
        effect. A "choice" param shows its combo (real option text) instead of a knob -- see
        FX_TYPE_METADATA's own comment. Values reset to that param's default -- a generic
        dial/combo position under one effect type has no meaningful relationship to another's."""
        entry = FX_TYPE_METADATA[type_index] if 0 <= type_index < len(FX_TYPE_METADATA) else FX_TYPE_METADATA[0]
        params = entry["params"]
        dials = self.fx_param_dials[slot_idx]
        combos = self.fx_param_combos[slot_idx]
        labels = self.fx_param_labels[slot_idx]
        columns = self.fx_param_columns[slot_idx]
        for p_idx in range(FX_SLOT_PARAM_COUNT):
            if p_idx < len(params):
                spec = params[p_idx]
                labels[p_idx].setText(spec["label"])
                if spec.get("type") == "choice":
                    combos[p_idx].blockSignals(True)
                    combos[p_idx].clear()
                    combos[p_idx].addItems(spec["options"])
                    combos[p_idx].setCurrentIndex(spec.get("default", 0))
                    combos[p_idx].blockSignals(False)
                    combos[p_idx].setVisible(True)
                    dials[p_idx].setVisible(False)
                else:
                    dials[p_idx].blockSignals(True)
                    dials[p_idx].setRange(*spec["range"])
                    dials[p_idx].setValue(spec["default"])
                    dials[p_idx].blockSignals(False)
                    dials[p_idx].setVisible(True)
                    combos[p_idx].setVisible(False)
                columns[p_idx].setVisible(True)
            else:
                dials[p_idx].setVisible(False)
                combos[p_idx].setVisible(False)
                columns[p_idx].setVisible(False)

    def _fx_slot_snapshot(self, slot_idx):
        """Captures everything that makes up one FX slot's configuration -- type, all 8 params
        (as whichever representation, dial value or combo index, that type actually uses), mix,
        bypass, and chain-to-next -- so it can be moved wholesale to another slot position by
        _swap_fx_slots."""
        type_idx = self.fx_type_combos[slot_idx].currentIndex()
        entry = FX_TYPE_METADATA[type_idx] if 0 <= type_idx < len(FX_TYPE_METADATA) else FX_TYPE_METADATA[0]
        params_meta = entry["params"]
        params = []
        for p_idx in range(FX_SLOT_PARAM_COUNT):
            spec = params_meta[p_idx] if p_idx < len(params_meta) else None
            if spec is not None and spec.get("type") == "choice":
                params.append(("combo", self.fx_param_combos[slot_idx][p_idx].currentIndex()))
            else:
                params.append(("dial", self.fx_param_dials[slot_idx][p_idx].value()))
        return {
            "type_idx": type_idx,
            "source_idx": self.fx_source_combos[slot_idx].currentIndex(),
            "params": params,
            "mix": self.fx_mix_dials[slot_idx].value(),
            "bypass": self.fx_bypass_buttons[slot_idx].isChecked(),
            "chain_to_next": self.fx_chain_to_next_buttons[slot_idx].isChecked(),
        }

    def _fx_slot_restore(self, slot_idx, snapshot):
        """Writes a snapshot from _fx_slot_snapshot into slot_idx's widgets -- relabels/re-ranges
        the param widgets for the snapshot's type first (same reconfiguration a manual Type change
        triggers), THEN sets the actual values, so a moved effect keeps its real settings instead
        of resetting to that type's defaults."""
        self.fx_type_combos[slot_idx].blockSignals(True)
        self.fx_type_combos[slot_idx].setCurrentIndex(snapshot["type_idx"])
        self.fx_type_combos[slot_idx].blockSignals(False)
        self._apply_fx_slot_type(slot_idx, snapshot["type_idx"])
        self.fx_source_combos[slot_idx].blockSignals(True)
        self.fx_source_combos[slot_idx].setCurrentIndex(snapshot["source_idx"])
        self.fx_source_combos[slot_idx].blockSignals(False)
        for p_idx, (kind, value) in enumerate(snapshot["params"]):
            if kind == "combo":
                combo = self.fx_param_combos[slot_idx][p_idx]
                combo.blockSignals(True)
                combo.setCurrentIndex(value)
                combo.blockSignals(False)
            else:
                dial = self.fx_param_dials[slot_idx][p_idx]
                dial.blockSignals(True)
                dial.setValue(value)
                dial.blockSignals(False)
        self.fx_mix_dials[slot_idx].blockSignals(True)
        self.fx_mix_dials[slot_idx].setValue(snapshot["mix"])
        self.fx_mix_dials[slot_idx].blockSignals(False)
        self.fx_bypass_buttons[slot_idx].blockSignals(True)
        self.fx_bypass_buttons[slot_idx].setChecked(snapshot["bypass"])
        self.fx_bypass_buttons[slot_idx].blockSignals(False)
        self.fx_chain_to_next_buttons[slot_idx].blockSignals(True)
        self.fx_chain_to_next_buttons[slot_idx].setChecked(snapshot["chain_to_next"])
        self.fx_chain_to_next_buttons[slot_idx].blockSignals(False)

    def _swap_fx_slots(self, a, b):
        """Moves slot a's whole configuration to slot b's position and vice versa -- the row
        widgets stay put (so nothing about their signal wiring changes), only the values they
        hold move. Series routing processes slots strictly by array position, so this alone is
        what makes reordering actually change processing order once retransmitted; Parallel
        routing sums all slots so order is a no-op for it either way -- chain_to_next moving with
        its slot is what lets a chained group stay grouped/coherent across a reorder too. Note
        fx_chain_to_next_buttons[slot_idx]'s own ENABLED state (disabled on the very last slot) is
        NOT part of the swap -- it's a fixed property of the row's physical POSITION (no "next
        slot" exists there, ever), not of whatever content currently occupies that position."""
        if not (0 <= a < FX_SLOT_COUNT and 0 <= b < FX_SLOT_COUNT):
            return
        snap_a = self._fx_slot_snapshot(a)
        snap_b = self._fx_slot_snapshot(b)
        self._fx_slot_restore(a, snap_b)
        self._fx_slot_restore(b, snap_a)
        self.transmit_knob_packet()

    def refresh_audio_devices(self):
        """Re-scans ALSA audio + MIDI device names, preserving whatever's currently selected if
        it's still present in the rescanned list."""
        for combo, lister in (
            (self.output_device_combo, lambda: _list_alsa_devices(["aplay", "-L"])),
            (self.input_device_combo, lambda: _list_alsa_devices(["arecord", "-L"])),
            (self.midi_device_combo, _list_midi_input_devices),
        ):
            current_text = combo.currentText()
            combo.blockSignals(True)
            combo.clear()
            combo.addItems(lister())
            combo.setCurrentText(current_text)
            combo.blockSignals(False)

    def _pick_record_folder(self):
        folder = QFileDialog.getExistingDirectory(self, "Recordings Folder", self.record_folder)
        if folder:
            self.record_folder = folder
            self.record_folder_field.setText(folder)

    def _pick_and_add_sample(self):
        """Fallback for the Mapping tab's "+ Add Sample" button -- creates a zone at Middle C,
        same as dropping the file there. Drag-and-drop directly onto a specific key is still the
        primary way to place a zone somewhere other than Middle C."""
        path, _ = QFileDialog.getOpenFileName(self, "Add Sample", os.path.expanduser("~"), "WAV files (*.wav)")
        if path:
            self.mapping_grid.add_zone_at_note(path, 60)

    def _pick_and_import_sfz(self):
        """Bulk-populates the Sample Bank from an SFZ multisample library -- see
        _build_zones_from_sfz()/_parse_sfz() for the parsing/mapping. Always additive (matches
        every other zone-creation path in this tab): existing zones are never removed or
        replaced. Sends every new zone in one transmit_zone_upsert() call."""
        path, _ = QFileDialog.getOpenFileName(self, "Import SFZ", os.path.expanduser("~"), "SFZ files (*.sfz)")
        if not path:
            return
        zones, warnings = _build_zones_from_sfz(path, self.mapping_grid._next_zone_id)
        if zones:
            self.mapping_grid._next_zone_id += len(zones)
            for z in zones: z.assigned_osc = self.mapping_grid.selected_osc
            self.mapping_grid.sample_zones.extend(zones)
            self.mapping_loaded_sfz_path[self.mapping_grid.selected_osc] = path
            self.mapping_sfz_readout.setText(f"SFZ: {os.path.basename(path)}")
            self.mapping_grid.update()
            self.transmit_zone_upsert(zones)
        filename = os.path.basename(path)
        self.set_mapping_status(f"Imported {len(zones)} zone(s) from {filename}" if zones else f"Imported 0 zones from {filename}", error=not zones)
        if warnings:
            QMessageBox.warning(self, "SFZ Import", f"Imported {len(zones)} zone(s) from {filename}, with some issues:\n\n" + "\n".join(f"- {w}" for w in warnings))

    def _pick_and_save_sfz(self):
        """Exports the current Sample Bank as a standalone, plain .sfz file -- the direct
        inverse of _pick_and_import_sfz(), and independent of Program save (which only ever
        writes a colocated .sfz bundled inside a Program's own folder). Reuses
        _build_sfz_from_zones() unchanged, so the output is exactly what Program save already
        writes: standard SFZ-spec region opcodes only, zero HDS/OSC-specific content -- letting
        this tab double as a general-purpose, portable SFZ editor/creator even for someone who
        never touches the rest of HDS."""
        if not self.mapping_grid.sample_zones:
            QMessageBox.information(self, "Save SFZ", "The Sample Bank is empty -- nothing to export.")
            return
        default_path = os.path.join(os.path.expanduser("~"), "Untitled.sfz")
        path, _ = QFileDialog.getSaveFileName(self, "Save SFZ", default_path, "SFZ files (*.sfz)")
        if not path:
            return
        if not path.lower().endswith(".sfz"):
            path += ".sfz"
        try:
            with open(path, "w") as f:
                f.write(_build_sfz_from_zones(self.mapping_grid.sample_zones))
        except OSError as e:
            QMessageBox.warning(self, "Save SFZ", f"Could not save '{path}':\n{e}")
            return
        self.set_mapping_status(f"Saved {len(self.mapping_grid.sample_zones)} zone(s) to {os.path.basename(path)}")

    def _scan_programs_folder(self):
        """Lists saved Program names -- one per ~/HDS_Programs/<name>/<name>.hdsprog subfolder."""
        if not os.path.isdir(self.programs_folder):
            return []
        names = []
        for entry in sorted(os.listdir(self.programs_folder)):
            subdir = os.path.join(self.programs_folder, entry)
            if os.path.isdir(subdir) and os.path.isfile(os.path.join(subdir, f"{entry}.hdsprog")):
                names.append(entry)
        return names

    def _refresh_combi_program_combos(self):
        """Repopulates every Combi lane's Program-picker dropdown from the Programs folder,
        preserving each lane's current selection where it still exists."""
        names = self._scan_programs_folder()
        for i, combo in enumerate(self.combi_program_combos):
            combo.blockSignals(True)
            combo.clear()
            combo.addItem("")  # none selected
            combo.addItems(names)
            current = self.combi_lane_program_choice[i]
            if current in names:
                combo.setCurrentText(current)
            else:
                self.combi_lane_program_choice[i] = None
            combo.blockSignals(False)

    def _on_combi_lane_program_picked(self, lane_idx, name):
        """Combi lane Program-picker change handler: records the choice (same GUI-local
        bookkeeping the scaffolding already did) and, when a real name was picked (not the blank
        "none selected" entry), actually loads that saved Program onto this lane via
        _load_program_from_path(..., lane_idx=lane_idx) -- see that method's own docstring for
        what "loading onto a lane" means (every lane 0-7 is its own independent Program now, see
        the mutual-exclusivity fix)."""
        self.combi_lane_program_choice[lane_idx] = name or None
        if not name:
            return
        path = os.path.join(self.programs_folder, name, f"{name}.hdsprog")
        if os.path.isfile(path):
            self._load_program_from_path(path, lane_idx=lane_idx)

    def _edit_combi_lane_in_program_mode(self, lane_idx):
        """'Edit' button handler below each Combi lane's Program picker: loads that same saved
        .hdsprog into Single Program mode's own dedicated editing buffer (lane_idx=None --
        _apply_program_fields(), the exact same path the main Load button uses) and switches
        Performance Mode to Single Program so it's immediately audible/editable, then switches
        the main tab deck to Oscillators (where Program editing actually happens) as a
        convenience. This is a one-way copy, not a live link: the Combi lane keeps playing
        whatever it already had (its own independent programs[lane_idx], per the
        mutual-exclusivity fix) until you explicitly pick this Program for that lane again from
        its own picker -- editing here and clicking Save writes a new/updated .hdsprog file, it
        does not reach back and change the lane."""
        name = self.combi_lane_program_choice[lane_idx]
        if not name:
            self.set_mapping_status(f"Lane {lane_idx + 1} has no Program loaded to edit", error=True)
            return
        path = os.path.join(self.programs_folder, name, f"{name}.hdsprog")
        if not os.path.isfile(path):
            QMessageBox.warning(self, "Edit Program", f"'{name}.hdsprog' no longer exists at:\n{path}")
            return
        self._load_program_from_path(path)  # lane_idx=None: Single Program mode's own slot
        self.osc_prog_radio.setChecked(True)  # jump into Single Program mode so it's audible/editable
        self.tab_widget.setCurrentWidget(self.tab_play)

    def _pick_and_save_combi(self):
        """Saves the current Combi to ~/HDS_Combis/<name>/<name>.hdscombi. Deliberately references
        each lane's constituent Program BY NAME (self.combi_lane_program_choice[i], the same
        bookkeeping the lane picker itself already maintains) rather than embedding that Program's
        full ~1150-field parameter set -- exactly the "reference, don't duplicate" relationship
        the picker already has to ~/HDS_Programs, so a Combi file stays a few hundred bytes
        instead of megabytes, and always reflects whatever that Program currently is (re-saving
        the Program updates every Combi that references it, with nothing to keep in sync by
        hand). Also captures each lane's own session-level routing (MIDI channel, Key/Velocity
        zone, Transpose, fader/pan) -- see _pick_and_load_combi() for the load-side mirror."""
        name, ok = QInputDialog.getText(self, "Save Combi", "Combi name:", QLineEdit.EchoMode.Normal, "My Combi")
        if not ok or not name.strip():
            return
        name = name.strip()
        lanes = []
        for i in range(COMBI_PROGRAM_COUNT):
            _strip, fader, pan_dial, _type_combo, _mode_combo = self.combi_lane_rows[i]
            lanes.append({
                "program": self.combi_lane_program_choice[i],
                "midi_channel": self.combi_lane_midi_channel_combos[i].currentIndex(),
                "key_lo": self.combi_lane_key_lo_spins[i].value(),
                "key_hi": self.combi_lane_key_hi_spins[i].value(),
                "vel_lo": self.combi_lane_vel_lo_spins[i].value(),
                "vel_hi": self.combi_lane_vel_hi_spins[i].value(),
                "transpose": self.combi_lane_transpose_spins[i].value(),
                "volume": fader.value(),
                "pan": pan_dial.value(),
            })
        data = {"format_version": 1, "name": name, "lanes": lanes}
        combi_dir = os.path.join(self.combis_folder, name)
        try:
            os.makedirs(combi_dir, exist_ok=True)
            with open(os.path.join(combi_dir, f"{name}.hdscombi"), "w") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Save Combi", f"Could not save '{name}':\n{e}")

    def _pick_and_load_combi(self):
        """Loads a .hdscombi saved by _pick_and_save_combi(): for each lane with a referenced
        Program, loads that Program's CURRENT .hdsprog onto the lane (via the same
        _load_program_from_path(..., lane_idx=i) the picker itself uses -- a Program renamed or
        re-saved since this Combi was captured is picked up as-is, not frozen at save time), then
        restores that lane's own session-level routing. A referenced Program that no longer exists
        on disk is reported once at the end rather than aborting the whole load, so the rest of the
        Combi still comes in."""
        os.makedirs(self.combis_folder, exist_ok=True)
        path, _ = QFileDialog.getOpenFileName(self, "Load Combi", self.combis_folder, "HDS Combis (*.hdscombi)")
        if not path:
            return
        try:
            with open(path) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Load Combi", f"Could not read '{path}':\n{e}")
            return
        lanes = data.get("lanes", [])
        missing = []
        for i in range(COMBI_PROGRAM_COUNT):
            lane = lanes[i] if i < len(lanes) else {}
            prog_name = lane.get("program")
            if prog_name:
                prog_path = os.path.join(self.programs_folder, prog_name, f"{prog_name}.hdsprog")
                if os.path.isfile(prog_path):
                    self._load_program_from_path(prog_path, lane_idx=i)
                    self.combi_lane_program_choice[i] = prog_name
                else:
                    missing.append(prog_name)
                    self.combi_lane_program_choice[i] = None
            else:
                self.combi_lane_program_choice[i] = None

            channel_combo = self.combi_lane_midi_channel_combos[i]
            channel_combo.blockSignals(True); channel_combo.setCurrentIndex(int(lane.get("midi_channel", 0))); channel_combo.blockSignals(False)
            for spin, key, default in (
                (self.combi_lane_key_lo_spins[i], "key_lo", 0), (self.combi_lane_key_hi_spins[i], "key_hi", 127),
                (self.combi_lane_vel_lo_spins[i], "vel_lo", 1), (self.combi_lane_vel_hi_spins[i], "vel_hi", 127),
                (self.combi_lane_transpose_spins[i], "transpose", 0),
            ):
                spin.blockSignals(True); spin.setValue(int(lane.get(key, default))); spin.blockSignals(False)
            _strip, fader, pan_dial, _type_combo, _mode_combo = self.combi_lane_rows[i]
            fader.blockSignals(True); fader.setValue(int(lane.get("volume", 80))); fader.blockSignals(False)
            pan_dial.blockSignals(True); pan_dial.setValue(int(lane.get("pan", 0))); pan_dial.blockSignals(False)

        self._refresh_combi_program_combos()
        self.transmit_knob_packet()
        if missing:
            QMessageBox.warning(self, "Load Combi", "Some referenced Programs were not found:\n" + "\n".join(missing))

    def _pick_seq_audio_file(self, lane_idx):
        """'Load Audio' button handler for one Sequencer lane. Sends the chosen path over OSC
        (/hds/seq/<lane>/audio_path); the backend actually reads the WAV into RAM on a background
        loader thread -- see g_seq_audio/poll_seq_audio_load_requests in backend.c. This function
        only kicks the load off; _apply_seq_audio_status (fed by listen_for_status_frames) is what
        updates the status label / reveals the Offset knob once the backend reports it loaded."""
        path, _ = QFileDialog.getOpenFileName(self, f"Load Audio — Lane {lane_idx + 1}", "", "WAV files (*.wav)")
        if not path:
            return
        widgets = self.seq_lane_audio_widgets[lane_idx]
        widgets["path"] = path
        widgets["status_label"].setText(f"Loading {os.path.basename(path)}…")
        widgets["offset_widget"].setVisible(False)
        self.transmit_knob_packet()

    def _preview_seq_audio(self, lane_idx):
        """'Preview' button: increments this lane's edge-triggered counter and resends the whole
        packet -- the backend detects the change (poll_seq_audio_preview_triggers in backend.c)
        and (re)starts that lane's one-shot RAM playback from the beginning (or Offset seconds
        in), mixed straight into the output. Independent per lane -- multiple lanes' Preview
        buttons can be clicked and will play back simultaneously, proving multitrack playback
        rather than one lane at a time."""
        widgets = self.seq_lane_audio_widgets[lane_idx]
        widgets["preview_trigger"] = widgets.get("preview_trigger", 0) + 1
        self.transmit_knob_packet()

    def _pick_and_load_scale(self):
        """Loads a Scala .scl file -- sends just the file path to the backend
        (/hds/tuning/load_scl), which parses it directly (see hds_tuning_load_scl() in
        backend.c), the same "OSC carries a path, the backend does the file I/O" shape as Song's
        own MIDI save/load. There's no round-trip status message for this yet, so the label below
        the button updates optimistically to the picked filename; a malformed/unreadable file is
        instead reported on the backend's own stderr rather than back to this GUI."""
        path, _ = QFileDialog.getOpenFileName(self, "Load Scale", "", "Scala scale files (*.scl)")
        if not path:
            return
        msg = OscMessageBuilder(address="/hds/tuning/load_scl")
        msg.add_arg(path, "s")
        try: self.osc_client.send(msg.build())
        except OSError: pass
        self.tuning_scale_label.setText(f"Loaded: {os.path.basename(path)}")

    def _pick_and_save_song(self):
        """Saves the Sequencer's current state to ~/HDS_Songs/<name>/ -- each of the 32 lanes' own
        config (type, level/pan, MIDI channel, mute/rec/overdub, an Audio lane's own offset) plus,
        for a Program/MIDI lane, its actual RECORDED PERFORMANCE (SeqMidiLane.events[] in
        backend.c) -- pure runtime state, never an OSC-addressable knob the way a plain fader
        value is, so the backend dumps it directly to a compact binary file
        (/hds/song/save_midi) rather than this function trying to read it back one event at a
        time over OSC. An Audio lane's referenced WAV is COPIED into the song folder
        (audio/lane_<i>.wav) so the song stays loadable even if the original file is later moved
        or renamed -- a "song folder" is meant to be one complete, self-contained unit you can
        archive or move as a whole."""
        name, ok = QInputDialog.getText(self, "Save Song", "Song name:", QLineEdit.EchoMode.Normal, "My Song")
        if not ok or not name.strip():
            return
        name = name.strip()
        song_dir = os.path.join(self.songs_folder, name)
        audio_dir = os.path.join(song_dir, "audio")
        try:
            os.makedirs(audio_dir, exist_ok=True)
        except OSError as e:
            QMessageBox.warning(self, "Save Song", f"Could not create '{song_dir}':\n{e}")
            return

        lanes = []
        for i in range(SEQ_LANE_COUNT):
            _strip, fader, pan_dial, type_combo, mode_combo = self.seq_lane_rows[i]
            audio_widgets = self.seq_lane_audio_widgets[i]
            src_path = audio_widgets.get("path", "") or ""
            rel_audio_path = ""
            if src_path and os.path.isfile(src_path):
                rel_audio_path = f"audio/lane_{i}.wav"
                try:
                    shutil.copyfile(src_path, os.path.join(song_dir, rel_audio_path))
                except OSError as e:
                    QMessageBox.warning(self, "Save Song", f"Could not copy audio for lane {i + 1}:\n{e}")
                    rel_audio_path = ""
            lanes.append({
                "lane_type": type_combo.currentIndex(),
                "mode": mode_combo.currentIndex(),
                "volume": fader.value(),
                "pan": pan_dial.value(),
                "midi_channel": self.seq_lane_midi_channel_combos[i].currentIndex(),
                "mute": 1 if self.seq_lane_mute_buttons[i].isChecked() else 0,
                "rec_armed": 1 if self.seq_lane_rec_buttons[i].isChecked() else 0,
                "rec_overdub": self.seq_lane_overdub_combos[i].currentIndex(),
                "audio_path": rel_audio_path,
                "audio_offset_sec": audio_widgets["offset_dial"].value() / 10.0,
            })
        # Song Markers -- see self.song_markers' own comment. format_version bumped to 2 since an
        # older (version-1) song.json simply has no "markers" key at all; _pick_and_load_song()
        # reads it with .get("markers", []), so loading an older file just starts with none.
        markers = [{"time": t, "label": lbl} for (t, lbl) in self.song_markers]
        data = {"format_version": 2, "name": name, "lanes": lanes, "markers": markers}
        try:
            with open(os.path.join(song_dir, "song.json"), "w") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Save Song", f"Could not write song.json for '{name}':\n{e}")
            return

        # Recorded MIDI performance data -- backend-native, see this method's own docstring.
        midi_path = os.path.join(song_dir, "song_midi.dat")
        msg = OscMessageBuilder(address="/hds/song/save_midi")
        msg.add_arg(midi_path, "s")
        try: self.osc_client.send(msg.build())
        except OSError: pass

    def _pick_and_load_song(self):
        """Loads a song saved by _pick_and_save_song(): restores every lane's own config from
        song.json straight onto the same widgets transmit_knob_packet() itself already reads
        (one normal transmit_knob_packet() call at the end pushes it all to the backend -- these
        are all regular /hds/seq/... knobs already, nothing special needed there), points each
        Audio lane at its own copy inside the song folder, and asks the backend to load the
        recorded MIDI performance data back into memory via /hds/song/load_midi."""
        folder = QFileDialog.getExistingDirectory(self, "Load Song", self.songs_folder)
        if not folder:
            return
        manifest_path = os.path.join(folder, "song.json")
        try:
            with open(manifest_path) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Load Song", f"Could not read '{manifest_path}':\n{e}")
            return

        lanes = data.get("lanes", [])
        for i in range(SEQ_LANE_COUNT):
            lane = lanes[i] if i < len(lanes) else {}
            _strip, fader, pan_dial, type_combo, mode_combo = self.seq_lane_rows[i]
            type_combo.blockSignals(True); type_combo.setCurrentIndex(int(lane.get("lane_type", 0))); type_combo.blockSignals(False)
            mode_combo.blockSignals(True); mode_combo.setCurrentIndex(int(lane.get("mode", 0))); mode_combo.blockSignals(False)
            fader.blockSignals(True); fader.setValue(int(lane.get("volume", 80))); fader.blockSignals(False)
            pan_dial.blockSignals(True); pan_dial.setValue(int(lane.get("pan", 0))); pan_dial.blockSignals(False)
            channel_combo = self.seq_lane_midi_channel_combos[i]
            channel_combo.blockSignals(True); channel_combo.setCurrentIndex(int(lane.get("midi_channel", 0))); channel_combo.blockSignals(False)
            mute_btn = self.seq_lane_mute_buttons[i]
            mute_btn.blockSignals(True); mute_btn.setChecked(bool(lane.get("mute", 0))); mute_btn.blockSignals(False)
            rec_btn = self.seq_lane_rec_buttons[i]
            rec_btn.blockSignals(True); rec_btn.setChecked(bool(lane.get("rec_armed", 0))); rec_btn.blockSignals(False)
            overdub_combo = self.seq_lane_overdub_combos[i]
            overdub_combo.blockSignals(True); overdub_combo.setCurrentIndex(int(lane.get("rec_overdub", 0))); overdub_combo.blockSignals(False)

            audio_widgets = self.seq_lane_audio_widgets[i]
            rel_audio_path = lane.get("audio_path", "") or ""
            if rel_audio_path:
                abs_path = os.path.join(folder, rel_audio_path)
                audio_widgets["path"] = abs_path if os.path.isfile(abs_path) else ""
                audio_widgets["status_label"].setText(os.path.basename(abs_path) if audio_widgets["path"] else "(missing file)")
            else:
                audio_widgets["path"] = ""
                audio_widgets["status_label"].setText("(no file)")
            audio_widgets["offset_widget"].setVisible(False)
            offset_dial = audio_widgets["offset_dial"]
            offset_dial.blockSignals(True); offset_dial.setValue(int(round(float(lane.get("audio_offset_sec", 0.0)) * 10.0))); offset_dial.blockSignals(False)

        # Song Markers -- absent entirely in a pre-version-2 song.json, hence .get() with a
        # default rather than a hard requirement.
        self.song_markers = sorted(
            (float(m.get("time", 0.0)), str(m.get("label", ""))) for m in data.get("markers", [])
        )
        self._rebuild_marker_list_widget()

        self.transmit_knob_packet()

        midi_path = os.path.join(folder, "song_midi.dat")
        if os.path.isfile(midi_path):
            msg = OscMessageBuilder(address="/hds/song/load_midi")
            msg.add_arg(midi_path, "s")
            try: self.osc_client.send(msg.build())
            except OSError: pass

    def _rebuild_marker_list_widget(self):
        """Repopulates the Sequencer tab's Markers list from self.song_markers (the frontend's own
        authoritative copy -- see its own comment), sorted by time. Called after an SMF import, a
        Song load, or any hand-edit of the list."""
        self.marker_list_widget.clear()
        for t, label in self.song_markers:
            minutes, seconds = divmod(max(0.0, t), 60.0)
            item = QListWidgetItem(f"{int(minutes):02d}:{seconds:04.1f}   {label}")
            item.setData(Qt.ItemDataRole.UserRole, t)
            self.marker_list_widget.addItem(item)

    def _jump_to_marker(self, item):
        """Double-click handler for the Markers list: seeks the Sequencer playhead straight to
        that marker's position via the backend's seek-to-arbitrary-position transport trigger
        (poll_transport_triggers() in backend.c) -- works in every transport state, same as the
        other seek buttons."""
        target_sec = item.data(Qt.ItemDataRole.UserRole)
        if target_sec is None:
            return
        self.seq_transport_seek_to_target_sec = float(target_sec)
        self.seq_transport_seek_to_trigger += 1
        self.transmit_knob_packet()

    def _pick_and_import_midi(self):
        """Imports a real Standard MIDI File (Type 0 or 1) -- e.g. exported from Reaper -- via
        /hds/song/import_smf (path, import_tempo). Backend-native parsing (hds_song_import_smf()
        in backend.c), same "OSC carries a path, the backend does the file I/O" shape as Load
        Scale/Song's own MIDI load -- but unlike those, this DOES get a real reply (see
        listen_for_status_frames()'s /hds/status/smf_import/ handling and
        _apply_smf_import_result()), since the result (which lanes got populated, what markers/
        tempo were found) is new information this GUI has no other way to learn."""
        path, _ = QFileDialog.getOpenFileName(self, "Import MIDI", "", "Standard MIDI Files (*.mid *.midi)")
        if not path:
            return
        self.smf_import_status_label.setText(f"Importing {os.path.basename(path)}…")
        msg = OscMessageBuilder(address="/hds/song/import_smf")
        msg.add_arg(path, "s")
        msg.add_arg(1.0 if self.smf_import_tempo_check.isChecked() else 0.0, "f")
        try: self.osc_client.send(msg.build())
        except OSError: pass

    def _apply_smf_import_result(self, fields):
        """Applies one /hds/song/import_smf reply (see listen_for_status_frames()'s
        /hds/status/smf_import/ handling): updates the status label, rebuilds the Markers list
        from whatever markers the file had, and -- if tempo import was requested -- syncs the
        Tempo dial to the file's own tempo_bpm and, if it had more than one tempo change, caches
        the returned tempo-map values so transmit_knob_packet() can keep echoing them back on
        every future send (see self._imported_tempo_markers' own comment -- without this cache,
        the very next unrelated knob tweak would silently wipe the just-imported tempo map)."""
        ok = fields.get("ok", 0.0) != 0.0
        error = fields.get("error", "")
        lanes_used = int(fields.get("lanes_used", 0))
        tempo_applied = fields.get("tempo_applied", 0.0) != 0.0
        if not ok:
            self.smf_import_status_label.setText(f"Import failed: {error}" if error else "Import failed")
            return
        marker_count = int(fields.get("marker_count", 0))
        self.song_markers = []
        for i in range(marker_count):
            t = fields.get(f"marker/{i}/time")
            label = fields.get(f"marker/{i}/label")
            if t is not None and label is not None:
                self.song_markers.append((float(t), str(label)))
        self._rebuild_marker_list_widget()

        if tempo_applied:
            tempo_marker_count = int(fields.get("tempo_marker_count", 0))
            if tempo_marker_count > 0:
                self._imported_tempo_markers = []
                for i in range(tempo_marker_count):
                    t = fields.get(f"tempo_marker/{i}/time")
                    bpm = fields.get(f"tempo_marker/{i}/bpm")
                    if t is not None and bpm is not None:
                        self._imported_tempo_markers.append((float(t), float(bpm)))
            else:
                self._imported_tempo_markers = []
            if "tempo_bpm" in fields:
                self.tempo_dial.setValue(int(round(fields["tempo_bpm"])))

        status = f"Imported {lanes_used} lane(s), {marker_count} marker(s)"
        if tempo_applied:
            status += ", tempo applied"
        self.smf_import_status_label.setText(status)
        self.transmit_knob_packet()

    def _pick_and_import_midi_track(self, idx):
        """Per-lane 'Load MIDI...' button (Program/MIDI lanes only, see
        _build_seq_midi_controls()): imports a single .mid/.midi file's note events directly into
        lane `idx`, replacing whatever it currently holds. Unlike the Sequencer tab's whole-song
        Import MIDI (_pick_and_import_midi()), this touches ONLY this one lane and never the
        tempo map or Markers list -- see hds_song_import_smf_track()'s own comment in backend.c."""
        path, _ = QFileDialog.getOpenFileName(self, "Load MIDI", "", "Standard MIDI Files (*.mid *.midi)")
        if not path:
            return
        self.seq_lane_midi_import_widgets[idx]["status_label"].setText(f"Loading {os.path.basename(path)}…")
        msg = OscMessageBuilder(address="/hds/song/import_smf_track")
        msg.add_arg(path, "s")
        msg.add_arg(float(idx), "f")
        try: self.osc_client.send(msg.build())
        except OSError: pass

    def _apply_smf_track_import_result(self, fields):
        """Applies one /hds/song/import_smf_track reply (see listen_for_status_frames()'s
        /hds/status/smf_import_track/ handling): updates that specific lane's own status label --
        the only per-lane, per-import UI feedback this needs, since the recorded note data itself
        stays backend-only, exactly like a normal live Rec take."""
        lane = int(fields.get("lane", -1))
        if not (0 <= lane < SEQ_LANE_COUNT):
            return
        status_label = self.seq_lane_midi_import_widgets[lane]["status_label"]
        if fields.get("ok", 0.0) == 0.0:
            error = fields.get("error", "")
            status_label.setText(f"Failed: {error}" if error else "Failed")
            return
        note_count = int(fields.get("note_count", 0))
        tracks_with_notes = int(fields.get("tracks_with_notes", 0))
        text = f"{note_count} note(s)"
        if tracks_with_notes > 1:
            text += f" ({tracks_with_notes} tracks merged)"
        status_label.setText(text)

    def _update_seq_sync_lock_label(self, _index=None):
        """Reflects /hds/status/midi_clock_locked next to the Sequencer tab's Sync combo -- blank
        when Internal (nothing to lock onto), Locked/Waiting when External, so a missing or
        misrouted clock source is visible instead of silently doing nothing."""
        if self.seq_sync_source_combo.currentIndex() == 0:
            self.seq_sync_lock_label.setText("")
            return
        locked = self._midi_clock_locked
        self.seq_sync_lock_label.setText("● Locked" if locked else "○ Waiting for clock…")
        self.seq_sync_lock_label.setStyleSheet(f"font-size: 9px; color: {'#2ecc71' if locked else '#e67e22'};")

    def _seq_transport_play(self):
        """Play button: edge-triggered counter, same idiom as _preview_seq_audio -- the backend's
        poll_transport_triggers (backend.c) detects the change and transitions STOPPED/PAUSED ->
        PLAYING, auto-starting every Seq-mode Audio lane and clearing every Rec-armed MIDI lane's
        previous take on a fresh (from-STOPPED) Play."""
        self.seq_transport_play_trigger += 1
        self.transmit_knob_packet()

    def _seq_transport_pause(self):
        """Pause button: edge-triggered counter -- freezes the playhead and gates every
        transport-driven Audio lane off, without resetting anything (Play resumes exactly where
        it left off)."""
        self.seq_transport_pause_trigger += 1
        self.transmit_knob_packet()

    def _seq_transport_stop(self):
        """Stop button: edge-triggered counter -- resets the playhead to 0, stops every
        transport-driven Audio lane (a manually Preview-triggered lane is unaffected -- see
        driven_by_transport in backend.c), and rewinds every MIDI lane's playback cursor."""
        self.seq_transport_stop_trigger += 1
        self.transmit_knob_packet()

    def _seq_transport_seek_start(self):
        """"Very beginning" reset: edge-triggered counter, same idiom as Play/Pause/Stop --
        jumps the playhead to 0:00 and repositions every lane to match (backend.c's
        transport_reposition_lanes), without changing the transport's Play/Pause/Stop state."""
        self.seq_transport_seek_start_trigger += 1
        self.transmit_knob_packet()

    def _seq_transport_seek_back(self):
        """Back one measure (4 beats at the current tempo) -- clamped at 0:00, never negative.
        Works in every transport state, repositioning every lane to match the new playhead."""
        self.seq_transport_seek_back_trigger += 1
        self.transmit_knob_packet()

    def _seq_transport_seek_fwd(self):
        """Forward one measure (4 beats at the current tempo). Works in every transport state,
        repositioning every lane to match the new playhead."""
        self.seq_transport_seek_fwd_trigger += 1
        self.transmit_knob_packet()

    def _apply_seq_audio_status(self, updates):
        """updates: {lane_index: {"state": float, "frames"?: float, "channels"?: float,
        "sample_rate"?: float}}, decoded from /hds/status/seq_audio/<lane>/... -- see
        send_seq_audio_status() in backend.c. SEQ_AUDIO_EMPTY=0/LOADING=1/READY=2/ERROR=3."""
        for lane_idx, fields in updates.items():
            if lane_idx < 0 or lane_idx >= len(self.seq_lane_audio_widgets):
                continue
            widgets = self.seq_lane_audio_widgets[lane_idx]
            state = int(fields.get("state", -1))
            name = os.path.basename(widgets.get("path", "")) or "file"
            if state == 2:  # SEQ_AUDIO_READY
                frames = fields.get("frames", 0.0)
                channels = int(fields.get("channels", 0))
                sample_rate = fields.get("sample_rate", 0.0)
                secs = (frames / sample_rate) if sample_rate > 0 else 0.0
                text = f"{name} ({secs:.1f}s, {channels}ch)"
                # seq_audio_loader_thread_main (backend.c) always resamples a loaded file to the
                # engine's own SAMPLE_RATE before storing it, so sample_rate reported here will
                # always equal ENGINE_SAMPLE_RATE post-load -- there's no mismatch state left to
                # warn about.
                widgets["status_label"].setText(text)
                widgets["offset_widget"].setVisible(True)
            elif state == 3:  # SEQ_AUDIO_ERROR
                widgets["status_label"].setText(f"{name}: load failed")
                widgets["offset_widget"].setVisible(False)
            elif state == 1:  # SEQ_AUDIO_LOADING
                widgets["status_label"].setText(f"Loading {name}…")
                widgets["offset_widget"].setVisible(False)

    def _scan_program_categories(self):
        """Reads every saved Program's own "category" field (skipping ones that don't have one --
        older files, or ones deliberately left uncategorized) to seed the Save dialog's category
        picker with whatever categories are actually in use, rather than a fixed invented list."""
        seen = []
        for name in self._scan_programs_folder():
            path = os.path.join(self.programs_folder, name, f"{name}.hdsprog")
            try:
                with open(path) as f:
                    cat = json.load(f).get("category", "").strip()
            except (OSError, json.JSONDecodeError):
                continue
            if cat and cat not in seen:
                seen.append(cat)
        return sorted(seen)

    def _pick_and_save_program(self):
        """Save/Load's Save handler -- writes the current Program (every field
        _is_program_scope_address() allows) to ~/HDS_Programs/<name>/<name>.hdsprog, plus a
        colocated <name>.sfz (via _build_sfz_from_zones()) when the Sample Bank has any zones.
        Deliberately does NOT touch performance_mode/seq_lanes or any rig/routing/device field --
        see _is_program_scope_address()'s own comment for why a Program is a self-contained unit
        with no reference to the session it happens to be running in."""
        default_name = self.program_name_field.text().strip() or "Unsaved Program"
        name, ok = QInputDialog.getText(self, "Save Program", "Program name:", QLineEdit.EchoMode.Normal, default_name)
        if not ok or not name.strip():
            return
        name = name.strip()

        # Category: a plain metadata field (never applied to any OSC address, see the "fields"
        # dict below) -- an editable combo, not a fixed enum, so it's a convenience list of
        # what's already in use plus whatever the user types, not a rigid taxonomy.
        categories = self._scan_program_categories()
        default_cat = self._current_program_category
        combo_items = categories if default_cat in categories or not default_cat else categories + [default_cat]
        start_idx = combo_items.index(default_cat) if default_cat in combo_items else 0
        category, ok = QInputDialog.getItem(self, "Save Program", "Category (optional, type to add a new one):",
                                             combo_items, start_idx, True)
        if not ok:
            return
        category = category.strip()

        self.transmit_knob_packet()  # freshen self._last_wire_state before capturing it
        fields = {addr: value for addr, value in self._last_wire_state.items() if _is_program_scope_address(addr)}
        prog_dir = os.path.join(self.programs_folder, name)
        try:
            os.makedirs(prog_dir, exist_ok=True)
            data = {"format_version": 1, "name": name, "category": category, "fields": fields}
            # Per-oscillator SFZ export: only the oscillators that actually have zones get their
            # own file (<name>_vco{N}.sfz), recorded as a {"<osc_index>": "<filename>"} map --
            # replaces the old single shared file, which couldn't tell which oscillator a zone
            # belonged to. See _load_program_from_path()'s load-side mirror of this map.
            sfz_map = {}
            for osc in range(4):
                osc_zones = [z for z in self.mapping_grid.sample_zones if z.assigned_osc == osc]
                if not osc_zones:
                    continue
                sfz_filename = f"{name}_vco{osc + 1}.sfz"
                with open(os.path.join(prog_dir, sfz_filename), "w") as f:
                    f.write(_build_sfz_from_zones(osc_zones))
                sfz_map[str(osc)] = sfz_filename
            if sfz_map:
                data["sfz_file"] = sfz_map
            with open(os.path.join(prog_dir, f"{name}.hdsprog"), "w") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Save Program", f"Could not save '{name}':\n{e}")
            return
        self.program_name_field.setText(name)
        self._current_program_category = category
        self._refresh_combi_program_combos()

    def _apply_full_state_fields(self, fields):
        """Applies a /hds/request_state reply (see listen_for_status_frames()'s sentinel
        handling) to this GUI's own widgets, so connecting to an already-configured backend
        instance (e.g. a DAW-hosted CLAP instance with a restored patch) pulls and displays its
        real current state instead of overwriting it with GUI defaults -- see the boot-time call
        site in runtime_connect_and_bind() for the connect-time flow this serves.

        Deliberately reuses _apply_program_fields() rather than a second hand-maintained reverse-
        mapping table: everything _apply_program_fields() already covers (oscillators, filters,
        EGs, LFOs, FX, FM, glide, hard sync, pb range, sustain polarity, tempo, poly/mono) is
        exactly what needs pulling here too. master_volume and transpose_semitones are the two
        fields Program scope deliberately excludes (see _is_program_scope_address) that DO need
        pulling for this use case -- applied directly here, before _apply_program_fields()'s own
        call, so its trailing transmit_knob_packet() call round-trips the correct values along
        with everything else.

        Known, deliberate gap (not silently dropped): Sequencer/Combi-lane fields
        (/hds/seq/..., /hds/performance_mode) and the rig-level fields Program scope also excludes
        (/hds/device/, /hds/record/, /hds/routing/, /hds/ext_in/, /hds/audio/) are not applied by
        this pass -- the first because this GUI has no reverse-mapping for the Sequencer/Combi
        lane widgets yet, the second because they're either functionally inert for a CLAP instance
        (no audio-device I/O at all) or not landmines the way knob state is (transmit_knob_packet()
        doesn't re-blast them, so an unsynced GUI default there is stale-but-harmless, not a
        clobber risk)."""
        if "/hds/master_volume" in fields:
            self.master_vol.blockSignals(True)
            self.master_vol.setValue(_audio_vol_untaper(fields["/hds/master_volume"]))
            self.master_vol.blockSignals(False)
        if "/hds/transpose_semitones" in fields:
            self.transpose_dial.blockSignals(True)
            self.transpose_dial.setValue(int(round(fields["/hds/transpose_semitones"])))
            self.transpose_dial.blockSignals(False)
        self._apply_program_fields(fields)

    def _apply_program_fields(self, fields):
        """Save/Load's Load handler's field-application step -- the mirror image of
        transmit_knob_packet()'s own value-computation, in reverse: for each address in `fields`,
        finds the matching widget and sets it (signals blocked throughout to avoid a storm of
        individual transmit_knob_packet() calls), then transmits the fully-restored state once at
        the end. Missing keys (an older/partial file, or an address this build doesn't know about
        yet) simply leave that widget at whatever it already was -- never an error."""
        widgets_touched = []
        def set_dial(addr, dial):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr]))); dial.blockSignals(False)
                widgets_touched.append(dial)
        def set_combo_index(addr, combo):
            if addr in fields:
                combo.blockSignals(True); combo.setCurrentIndex(int(round(fields[addr]))); combo.blockSignals(False)
                widgets_touched.append(combo)
        def set_radio_from_bool(addr, radio_if_true, radio_if_false):
            if addr in fields:
                checked_true = fields[addr] != 0.0
                for r in (radio_if_true, radio_if_false):
                    r.blockSignals(True)
                (radio_if_true if checked_true else radio_if_false).setChecked(True)
                for r in (radio_if_true, radio_if_false):
                    r.blockSignals(False)
                widgets_touched.extend([radio_if_true, radio_if_false])

        set_radio_from_bool("/hds/is_polyphonic", self.poly_radio, self.mono_radio)
        if "/hds/arp/enabled" in fields:
            self.arp_enabled_btn.blockSignals(True)
            self.arp_enabled_btn.setChecked(fields["/hds/arp/enabled"] != 0.0)
            self.arp_enabled_btn.blockSignals(False)
            widgets_touched.append(self.arp_enabled_btn)
        set_combo_index("/hds/arp/style", self.arp_style_combo)
        set_dial("/hds/arp/velocity", self.arp_velocity_dial)
        set_dial("/hds/arp/gate_pct", self.arp_gate_dial)
        # master_volume is deliberately never applied here -- excluded from Program scope (see
        # _is_program_scope_address), so even an older .hdsprog saved before this exclusion
        # existed won't have loading silently yank the current output level around.
        set_dial("/hds/portamento_time_ms", self.glide_dial)
        set_dial("/hds/mono_glide_random", self.glide_random_dial)
        set_combo_index("/hds/hard_sync_mode", self.hard_sync_combo)
        if "/hds/pb/range_up" in fields: self.pb_range_up_spin.setValue(int(round(fields["/hds/pb/range_up"])))
        if "/hds/pb/range_down" in fields: self.pb_range_down_spin.setValue(int(round(fields["/hds/pb/range_down"])))
        set_radio_from_bool("/hds/sustain/polarity", self.sustain_polarity_inverted, self.sustain_polarity_normal)
        if "/hds/tempo/bpm" in fields: self.tempo_dial.setValue(int(round(fields["/hds/tempo/bpm"])))

        # Filters (incl. Multi Filter blend), EGs, LFOs, FX, FM depths: same loop-shaped groups
        # transmit_knob_packet() itself uses, applied in reverse. (Oscillator scalars are handled
        # directly below, alongside the rest of this function's plain single-field restores.)
        self._apply_program_fields_filters(fields)
        self._apply_program_fields_eq(fields)
        self._apply_program_fields_egs(fields)
        self._apply_program_fields_lfos(fields)
        self._apply_program_fields_fx(fields)
        for key, dial in self.fm_depth_dials.items():
            addr = {
                "fm_intensity": "/hds/fm/intensity", "fm_depth_3_1": "/hds/fm/depth/3_1",
                "fm_depth_3_2": "/hds/fm/depth/3_2", "fm_depth_4_1": "/hds/fm/depth/4_1",
                "fm_depth_4_3": "/hds/fm/depth/4_3", "fm_depth_1_2": "/hds/fm/depth/1_2",
                "fm_depth_3_4": "/hds/fm/depth/3_4", "fm_depth_1_3": "/hds/fm/depth/1_3",
            }[key]
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * 100.0))); dial.blockSignals(False)
        if "/hds/fm/algorithm" in fields:
            self.fm_algorithm_combo.blockSignals(True)
            self.fm_algorithm_combo.setCurrentIndex(int(round(fields["/hds/fm/algorithm"])))
            self.fm_algorithm_combo.blockSignals(False)

        # Remaining scalar oscillator/global fields transmit_knob_packet() sends raw (no scale)
        # or as a simple checkbox/bool -- mirrors each of those lines directly.
        set_dial("/hds/vco1/unison/detune", self.vco1_unison_detune)
        set_dial("/hds/vco1/unison/width", self.vco1_unison_width)
        set_dial("/hds/vco2/unison/detune", self.vco2_unison_detune)
        set_dial("/hds/vco2/unison/width", self.vco2_unison_width)
        set_dial("/hds/vco3/unison/detune", self.vco3_unison_detune)
        set_dial("/hds/vco3/unison/width", self.vco3_unison_width)
        set_dial("/hds/vco4/unison/detune", self.vco4_unison_detune)
        set_dial("/hds/vco4/unison/width", self.vco4_unison_width)
        set_dial("/hds/vco1/phase_offset", self.vco1_phase)
        set_dial("/hds/vco2/phase_offset", self.vco2_phase)
        set_dial("/hds/vco3/phase_offset", self.vco3_phase)
        set_dial("/hds/vco4/phase_offset", self.vco4_phase)
        set_dial("/hds/vco1/pulse_width", self.vco1_pulse_width)
        set_dial("/hds/vco2/pulse_width", self.vco2_pulse_width)
        set_dial("/hds/vco3/pulse_width", self.vco3_pulse_width)
        set_dial("/hds/vco4/pulse_width", self.vco4_pulse_width)
        set_dial("/hds/vco1/fine_tune", self.vco1_fine_tune)
        set_dial("/hds/vco2/fine_tune", self.vco2_fine_tune)
        set_dial("/hds/vco2/pitch_offset", self.vco2_pitch_offset)
        set_dial("/hds/vco3/fine_tune", self.vco3_fine_tune)
        set_dial("/hds/vco3/pitch_offset", self.vco3_pitch_offset)
        set_dial("/hds/vco4/fine_tune", self.vco4_fine_tune)
        set_dial("/hds/vco4/pitch_offset", self.vco4_pitch_offset)
        for addr, dial in (("/hds/vco1/pitch", self.vco1_pitch), ("/hds/vco2/pitch", self.vco2_pitch),
                           ("/hds/vco3/pitch", self.vco3_pitch), ("/hds/vco4/pitch", self.vco4_pitch)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] / 100.0))); dial.blockSignals(False)
        for addr, cb in (("/hds/vco1/mute", self.vco1_mute), ("/hds/vco2/mute", self.vco2_mute),
                         ("/hds/vco3/mute", self.vco3_mute), ("/hds/vco4/mute", self.vco4_mute)):
            if addr in fields:
                cb.blockSignals(True); cb.setChecked(fields[addr] != 0.0); cb.blockSignals(False)
        for addr, cb in (("/hds/vco1/mono_noise", self.vco1_mono_noise), ("/hds/vco2/mono_noise", self.vco2_mono_noise),
                         ("/hds/vco3/mono_noise", self.vco3_mono_noise), ("/hds/vco4/mono_noise", self.vco4_mono_noise)):
            if addr in fields:
                cb.blockSignals(True); cb.setChecked(fields[addr] != 0.0); cb.blockSignals(False)
        for addr, dial in (("/hds/vco1/vel_to_amp", self.vco1_vel_to_amp), ("/hds/vco2/vel_to_amp", self.vco2_vel_to_amp),
                           ("/hds/vco3/vel_to_amp", self.vco3_vel_to_amp), ("/hds/vco4/vel_to_amp", self.vco4_vel_to_amp)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr]))); dial.blockSignals(False)
        for addr, dial in (("/hds/vco1/vol", self.vco1_vol), ("/hds/vco2/vol", self.vco2_vol),
                           ("/hds/vco3/vol", self.vco3_vol), ("/hds/vco4/vol", self.vco4_vol)):
            if addr in fields:
                # Inverse of _audio_vol_taper -- fields[addr] is the real linear gain a saved
                # Program stored, so the slider's own position has to be un-tapered to land back
                # on the exact same audible gain when transmit_knob_packet() re-tapers it.
                dial.blockSignals(True); dial.setValue(_audio_vol_untaper(fields[addr])); dial.blockSignals(False)
        for addr, dial in (("/hds/vco1/pan", self.vco1_pan), ("/hds/vco2/pan", self.vco2_pan),
                           ("/hds/vco3/pan", self.vco3_pan), ("/hds/vco4/pan", self.vco4_pan)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * 100.0))); dial.blockSignals(False)
        for addr, dial in (("/hds/vco1/curve_drive", self.vco1_curve_drive), ("/hds/vco2/curve_drive", self.vco2_curve_drive),
                           ("/hds/vco3/curve_drive", self.vco3_curve_drive), ("/hds/vco4/curve_drive", self.vco4_curve_drive)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * 100.0))); dial.blockSignals(False)
        for addr, combo in (("/hds/vco1/shape", self.vco1_combo), ("/hds/vco2/shape", self.vco2_combo),
                            ("/hds/vco3/shape", self.vco3_combo), ("/hds/vco4/shape", self.vco4_combo)):
            if addr in fields:
                combo.blockSignals(True); combo.setCurrentIndex(int(round(fields[addr]))); combo.blockSignals(False)

        # Bug fix: every setter above uses blockSignals to avoid a storm of individual
        # transmit_knob_packet() calls mid-load -- but several of these combos also drive purely
        # VISUAL state through their own currentIndexChanged connection (the Unison Saw/Pulse-
        # Width-or-Tilt box show/hide per oscillator Shape, each filter unit's blend-knob-row
        # show/hide per Type, the Filter Routing preset combo's derived label, the Parallel
        # level/pan box per Routing mode) -- blocking signals silently skipped all of that, so a
        # loaded Program's VALUES were always correct but the GUI could visually look like the
        # oscillator/filter was still in its old configuration until some other control was
        # touched. Re-emitting each combo's own currentIndexChanged (its current value hasn't
        # changed since the block above, so this is a pure "run my own side effects" signal, not
        # a value change) makes the GUI actually recall exactly as if it had been set that way by
        # hand -- matches every other widget class's real-time behavior.
        for combo in (self.vco1_combo, self.vco2_combo, self.vco3_combo, self.vco4_combo,
                      *self.filter_type_combos, self.filter_chain_mode_combo):
            combo.currentIndexChanged.emit(combo.currentIndex())

        self.transmit_knob_packet()

    def _apply_program_fields_filters(self, fields):
        """Mirrors transmit_knob_packet()'s own flat 5-slot filter section in reverse -- same
        filter_cutoff_dials/filter_res_dials/filter_slope_combos/filter_type_combos index
        convention (one entry per filter slot, 0-4). filter_key_track/filter_cutoff_mod_source_cc
        aren't widget-backed (see their own construction comment), so they're restored directly
        into their cache lists rather than through a dial/combo setter."""
        def sd(addr, dial, scale=1.0):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * scale))); dial.blockSignals(False)
        def sc(addr, combo):
            if addr in fields:
                combo.blockSignals(True); combo.setCurrentIndex(int(round(fields[addr]))); combo.blockSignals(False)

        sc("/hds/filter/chain_mode", self.filter_chain_mode_combo)
        for i in range(5):
            sd(f"/hds/filter/{i}/cutoff", self.filter_cutoff_dials[i])
            sd(f"/hds/filter/{i}/res_q", self.filter_res_dials[i])
            sc(f"/hds/filter/{i}/slope", self.filter_slope_combos[i])
            sc(f"/hds/filter/{i}/type", self.filter_type_combos[i])
            key_track_addr = f"/hds/filter/{i}/key_track"
            if key_track_addr in fields:
                self.filter_key_track[i] = fields[key_track_addr]
            cc_source_addr = f"/hds/filter/{i}/cc_source"
            if cc_source_addr in fields:
                self.filter_cutoff_mod_source_cc[i] = fields[cc_source_addr]

    def _apply_program_fields_eq(self, fields):
        """Mirrors transmit_knob_packet()'s own 3-band EQ section in reverse -- see backend.c's
        EqBandParams/ProgramState.eq_bands comment. eq_q_dials[0]/[2] are None (shelf bands have
        no Q control), so the Q restore is skipped for those two bands."""
        def sd(addr, dial, scale=1.0):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * scale))); dial.blockSignals(False)

        for b in range(3):
            sd(f"/hds/eq/{b}/freq", self.eq_freq_dials[b])
            sd(f"/hds/eq/{b}/gain", self.eq_gain_dials[b])
            if self.eq_q_dials[b] is not None:
                sd(f"/hds/eq/{b}/q", self.eq_q_dials[b], scale=10.0)

    def _apply_program_fields_egs(self, fields):
        """Mirrors transmit_knob_packet()'s EG-params loop, reusing each EG panel's own existing
        get_state()/set_state() pair (already built for the "1->All"/Sync copy buttons) rather
        than re-deriving the dial-by-dial logic a second time."""
        all_panels = [self.vco1_amp_eg, self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg] + self.envelopes
        for eg_idx, panel in enumerate(all_panels):
            if f"/hds/eg/{eg_idx}/delay" not in fields:
                continue  # this EG wasn't in the saved file -- leave it untouched
            state = {
                "type": int(round(fields.get(f"/hds/eg/{eg_idx}/topology_mode", panel.combo_type.currentIndex()))),
                "d": fields[f"/hds/eg/{eg_idx}/delay"],
                "h": fields[f"/hds/eg/{eg_idx}/hold"],
                "a": fields[f"/hds/eg/{eg_idx}/attack"],
                "dec": fields[f"/hds/eg/{eg_idx}/decay"],
                "s": fields[f"/hds/eg/{eg_idx}/sustain"],
                "r": fields[f"/hds/eg/{eg_idx}/release"],
                "al": fields.get(f"/hds/eg/{eg_idx}/attack_start_pct", panel.attack_level_pct),
                "pk": fields.get(f"/hds/eg/{eg_idx}/attack_peak_pct", panel.attack_peak_pct),
            }
            panel.set_state(state)

    def _apply_program_fields_lfos(self, fields):
        """Mirrors transmit_knob_packet()'s LFO loop in reverse."""
        for idx in range(LFO_COUNT):
            if f"/hds/lfo/{idx}/wave" in fields:
                wave_idx = int(round(fields[f"/hds/lfo/{idx}/wave"]))
                self.lfo_combos[idx].blockSignals(True)
                self.lfo_combos[idx].setCurrentIndex(wave_idx)
                self.lfo_combos[idx].blockSignals(False)
                # Curve/Drive box visibility + label don't ride the (blocked) currentIndexChanged
                # signal -- set directly, mirroring _on_lfo_wave_changed's own logic.
                self.lfo_curve_labels[idx].setVisible(wave_idx in (0, 1, 2))
                self.lfo_curve_dials[idx].setVisible(wave_idx in (0, 1, 2))
                self.lfo_curve_labels[idx].setText("Drive" if wave_idx == 1 else "Curve")
            if f"/hds/lfo/{idx}/curve_drive" in fields:
                self.lfo_curve_dials[idx].blockSignals(True)
                self.lfo_curve_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/curve_drive"] * 100.0)))
                self.lfo_curve_dials[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/rate" in fields:
                self.lfo_dials[idx].blockSignals(True)
                self.lfo_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/rate"] * 10.0)))
                self.lfo_dials[idx].blockSignals(False)
                self.lfo_readouts[idx].setText(f"{fields[f'/hds/lfo/{idx}/rate']:.1f} Hz")
            if f"/hds/lfo/{idx}/tempo_sync" in fields:
                sync_idx = int(round(fields[f"/hds/lfo/{idx}/tempo_sync"]))
                sync_idx = max(0, min(len(LFO_SYNC_DIVISION_LABELS) - 1, sync_idx))
                self.lfo_tempo_sync_combos[idx].blockSignals(True)
                self.lfo_tempo_sync_combos[idx].setCurrentIndex(sync_idx)
                self.lfo_tempo_sync_combos[idx].blockSignals(False)
                self.lfo_dials[idx].setEnabled(sync_idx == 0)
                if sync_idx != 0:
                    self.lfo_readouts[idx].setText(LFO_SYNC_DIVISION_LABELS[sync_idx])
            if f"/hds/lfo/{idx}/depth" in fields:
                self.lfo_depth_dials[idx].blockSignals(True)
                self.lfo_depth_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/depth"] * 100.0)))
                self.lfo_depth_dials[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/key_sync" in fields:
                self.lfo_sync_combos[idx].blockSignals(True)
                self.lfo_sync_combos[idx].setCurrentIndex(int(round(fields[f"/hds/lfo/{idx}/key_sync"])))
                self.lfo_sync_combos[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/dest" in fields:
                self.lfo_dest_combos[idx].blockSignals(True)
                self.lfo_dest_combos[idx].setCurrentIndex(int(round(fields[f"/hds/lfo/{idx}/dest"])))
                self.lfo_dest_combos[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/phase_offset" in fields:
                self.lfo_phase_dials[idx].blockSignals(True)
                self.lfo_phase_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/phase_offset"])))
                self.lfo_phase_dials[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/mod_source_cc" in fields:
                self.lfo_mod_source_cc[idx] = fields[f"/hds/lfo/{idx}/mod_source_cc"]
            if f"/hds/lfo/{idx}/per_key" in fields:
                self.lfo_per_key_checks[idx].blockSignals(True)
                checked = fields[f"/hds/lfo/{idx}/per_key"] != 0.0
                self.lfo_per_key_checks[idx].setChecked(checked)
                self.lfo_per_key_checks[idx].blockSignals(False)
                self.lfo_key_track_boxes[idx].setVisible(checked)
            if f"/hds/lfo/{idx}/key_track" in fields:
                self.lfo_key_track_dials[idx].blockSignals(True)
                self.lfo_key_track_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/key_track"])))
                self.lfo_key_track_dials[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/vel_track" in fields:
                self.lfo_vel_track_dials[idx].blockSignals(True)
                self.lfo_vel_track_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/vel_track"])))
                self.lfo_vel_track_dials[idx].blockSignals(False)

        # 6 assignable EG intensity multipliers + their own mod destinations.
        for idx in range(6):
            if f"/hds/eg_mod/{idx}/intensity" in fields:
                self.eg_intensity_dials[idx].blockSignals(True)
                self.eg_intensity_dials[idx].setValue(int(round(fields[f"/hds/eg_mod/{idx}/intensity"] * 100.0)))
                self.eg_intensity_dials[idx].blockSignals(False)
            if f"/hds/eg_mod/{idx}/dest" in fields:
                self.eg_dest_combos[idx].blockSignals(True)
                self.eg_dest_combos[idx].setCurrentIndex(int(round(fields[f"/hds/eg_mod/{idx}/dest"])))
                self.eg_dest_combos[idx].blockSignals(False)

    def _apply_program_fields_fx(self, fields):
        """Mirrors transmit_knob_packet()'s FX Bus loop in reverse, reusing the existing
        _fx_slot_restore() (built for the drag-reorder buttons) so per-type param widget
        reconfiguration (dial vs. choice, range, visibility) happens exactly the same way a
        manual Type change already triggers it -- rather than re-deriving that logic a second
        time. fx_source (which bus a slot serves) is deliberately left untouched -- routing, not
        patch content, per _is_program_scope_address()."""
        for slot_idx in range(FX_SLOT_COUNT):
            type_addr = f"/hds/fx/{slot_idx}/type"
            if type_addr not in fields:
                continue
            type_idx = int(round(fields[type_addr]))
            entry = FX_TYPE_METADATA[type_idx] if 0 <= type_idx < len(FX_TYPE_METADATA) else FX_TYPE_METADATA[0]
            params_meta = entry["params"]
            # Re-configure this slot's param widgets (ranges, choice-combo items, visibility) for
            # the TARGET type before reading anything off them below -- otherwise a choice param's
            # combo.count() would still reflect whatever type this slot was previously showing,
            # not the one being loaded. _fx_slot_restore() does this same reconfiguration too, but
            # only after this loop has already (mis-)read the stale widget state.
            self._apply_fx_slot_type(slot_idx, type_idx)
            params = []
            for p_idx in range(FX_SLOT_PARAM_COUNT):
                addr = f"/hds/fx/{slot_idx}/param/{p_idx}"
                spec = params_meta[p_idx] if p_idx < len(params_meta) else None
                raw = fields.get(addr, 0.0)
                if spec is not None and spec.get("type") == "choice":
                    combo = self.fx_param_combos[slot_idx][p_idx]
                    option_count = combo.count()
                    idx = int(round(raw * (option_count - 1))) if option_count > 1 else 0
                    params.append(("combo", idx))
                else:
                    params.append(("dial", int(round(raw * 100.0))))
            snapshot = {
                "type_idx": type_idx,
                "source_idx": self.fx_source_combos[slot_idx].currentIndex(),  # untouched -- routing, not patch
                "params": params,
                "mix": int(round(fields.get(f"/hds/fx/{slot_idx}/mix", self.fx_mix_dials[slot_idx].value()))),
                "bypass": fields.get(f"/hds/fx/{slot_idx}/bypass", 0.0) != 0.0,
                "chain_to_next": fields.get(f"/hds/fx/{slot_idx}/chain_to_next", 0.0) != 0.0,
            }
            self._fx_slot_restore(slot_idx, snapshot)

    def _pick_and_load_program(self):
        """Save/Load's Load handler -- browse ~/HDS_Programs/*/*.hdsprog and load it via
        _load_program_from_path(). See that method's docstring for the Sample-Bank-replace
        behavior."""
        os.makedirs(self.programs_folder, exist_ok=True)
        path, _ = QFileDialog.getOpenFileName(self, "Load Program", self.programs_folder, "HDS Programs (*.hdsprog)")
        if not path:
            return
        self._load_program_from_path(path)

    def _apply_program_fields_to_combi_lane(self, lane_idx, fields):
        """Loads a saved Program's fields onto Combi lane `lane_idx` (0-7, matching
        programs[0..7] in backend.c -- ALL 8 real Combi lanes, no special case). `lane_idx=None`
        instead means "Single Program mode's own dedicated ProgramState" (backend.c's
        SINGLE_PROGRAM_SLOT) -- the Oscillators/Filters/LFO/EG tabs' own widgets -- so it goes
        through the normal widget-syncing _apply_program_fields().
        Combi lane 1 (programs[0]) and Single Program mode used to be the SAME backend storage
        (editing one silently rewrote the other -- a real bug); they are now fully independent,
        each addressed under its own OSC prefix, so this function no longer special-cases
        lane_idx==0 as "the Program-mode Program." Every real Combi lane (0-7) has no widgets of
        its own (this pass deliberately doesn't add a live per-lane knob-editing UI for 8
        simultaneous Programs -- see the Combi mode plan's own scope note), so those addresses
        are sent directly over OSC with each address's leading /hds remapped to
        /hds/combi/{lane_idx}, bypassing the widget layer entirely -- the backend's own
        generic_osc_handler applies them to programs[lane_idx] exactly as if a widget had sent
        them. self._last_wire_state is intentionally left untouched for Combi lanes (it mirrors
        the currently-EDITED Program, i.e. Single Program mode's own slot, not every lane at
        once)."""
        if lane_idx is None:
            self._apply_program_fields(fields)
            return
        if not fields:
            return
        bundle = OscBundleBuilder(IMMEDIATELY)
        for addr, value in fields.items():
            if not addr.startswith("/hds"):
                continue
            new_addr = f"/hds/combi/{lane_idx}" + addr[len("/hds"):]
            msg = OscMessageBuilder(address=new_addr)
            msg.add_arg(float(value), "f")
            bundle.add_content(msg.build())
        try:
            self.osc_client.send(bundle.build())
        except OSError:
            pass

    def _load_program_from_path(self, path, lane_idx=None):
        """Shared load logic behind the file-dialog-driven Load button, the </> prev/next
        cycling buttons (lane_idx=None: Single Program mode's own dedicated ProgramState), the
        Edit button below each loaded Combi lane (also lane_idx=None -- see
        _edit_combi_lane_in_program_mode), and each Combi lane's own Program picker (lane_idx
        0-7): apply every included field via _apply_program_fields_to_combi_lane(), and -- only
        when loading into Single Program mode's own slot (lane_idx is None) -- if the file
        references any colocated per-oscillator .sfz files, REPLACE (not merge with) only the
        REFERENCED oscillators' own zones -- a real, deliberate behavior difference from the SFZ
        importer's own additive-only posture, because loading a *Program* means "this Program's
        own samples for the oscillators it actually uses," not "add these on top of whatever was
        already loaded" or "wipe every oscillator regardless of what this Program touches." An
        oscillator this Program's sfz_file map doesn't mention is left completely untouched. The
        Sample Bank is a single session-wide catalog with no per-Program partitioning (see the
        Mapping tab's own zone-catalog design), so this pass deliberately does not attempt to load
        a Combi lane's own colocated .sfz -- a documented scope boundary, not an oversight."""
        try:
            with open(path) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Load Program", f"Could not read '{path}':\n{e}")
            return
        self._apply_program_fields_to_combi_lane(lane_idx, data.get("fields", {}))
        if lane_idx is not None:
            return

        sfz_file = data.get("sfz_file")
        # Backward-compat: an old save's sfz_file was a bare filename (one shared file, no
        # oscillator tagging at all) -- treated as "every zone in it was Osc 1" (index "0"), the
        # same convention _build_zones_from_sfz()'s own default assigned_osc=0 already implies.
        if isinstance(sfz_file, str):
            sfz_map = {"0": sfz_file}
        elif isinstance(sfz_file, dict):
            sfz_map = sfz_file
        else:
            sfz_map = {}

        if sfz_map:
            prog_dir = os.path.dirname(path)
            all_warnings = []
            for osc_str, sfz_filename in sfz_map.items():
                try:
                    osc = max(0, min(3, int(osc_str)))
                except (TypeError, ValueError):
                    continue
                sfz_path = os.path.join(prog_dir, sfz_filename)
                old_ids = [z.zone_id for z in self.mapping_grid.sample_zones if z.assigned_osc == osc]
                if old_ids:
                    self.transmit_zone_remove(old_ids)
                    self.mapping_grid.sample_zones = [z for z in self.mapping_grid.sample_zones if z.assigned_osc != osc]
                if os.path.isfile(sfz_path):
                    zones, warnings = _build_zones_from_sfz(sfz_path, self.mapping_grid._next_zone_id)
                    if zones:
                        self.mapping_grid._next_zone_id += len(zones)
                        for z in zones: z.assigned_osc = osc
                        self.mapping_grid.sample_zones.extend(zones)
                        self.transmit_zone_upsert(zones)
                    self.mapping_loaded_sfz_path[osc] = sfz_path
                    all_warnings.extend(warnings)
                else:
                    all_warnings.append(f"{sfz_filename} (Osc {osc + 1}): file not found")
            self.mapping_grid.update()
            self._on_mapping_osc_changed(self.mapping_grid.selected_osc)  # refresh readout/status
            if all_warnings:
                QMessageBox.warning(self, "Load Program", f"Loaded '{data.get('name', '')}' with some sample issues:\n\n" + "\n".join(f"- {w}" for w in all_warnings))

        self.program_name_field.setText(data.get("name", os.path.splitext(os.path.basename(path))[0]))
        self._current_program_category = data.get("category", "")

    def _cycle_program(self, delta):
        """'◀'/'▶' buttons: load the previous/next saved Program alphabetically from
        ~/HDS_Programs/, wrapping around. If the current program_name_field text matches a saved
        Program, cycling starts relative to it; otherwise (an unsaved/renamed patch) '▶'
        starts at the first saved Program and '◀' at the last. No-op if no Programs are
        saved yet."""
        names = self._scan_programs_folder()
        if not names:
            return
        current = self.program_name_field.text().strip()
        if current in names:
            next_idx = (names.index(current) + delta) % len(names)
        else:
            next_idx = 0 if delta > 0 else len(names) - 1
        next_name = names[next_idx]
        path = os.path.join(self.programs_folder, next_name, f"{next_name}.hdsprog")
        if os.path.isfile(path):
            self._load_program_from_path(path)

    def set_mapping_status(self, message, error=False):
        """Status line under the Mapping tab's top bar -- shows the live zone count (for the
        CURRENTLY SELECTED oscillator only, since that's what's actually visible/editable in the
        grid right now) by default -- no ceiling to report against, the catalog has no
        product-facing cap."""
        if message is None:
            n = len(self.mapping_grid.visible_zones())
            message = f"Sample Bank (Osc {self.mapping_grid.selected_osc + 1}): {n} zone{'s' if n != 1 else ''}"
            error = False
        self.mapping_status_label.setText(message)
        self.mapping_status_label.setStyleSheet(f"font-size: 10px; color: {'#ff5555' if error else '#888'};")

    def _on_mapping_osc_changed(self, index):
        """Switches the Mapping tab's grid to show/edit a different oscillator's own zone set --
        updates the grid's selected_osc (triggering a repaint via visible_zones()), the SFZ-path
        readout, and the zone-count status line."""
        osc = max(0, min(3, index))
        self.mapping_grid.selected_osc = osc
        self.mapping_grid.update()
        path = self.mapping_loaded_sfz_path[osc]
        self.mapping_sfz_readout.setText(f"SFZ: {os.path.basename(path)}" if path else "SFZ: (none loaded)")
        self.set_mapping_status(None)

    def synchronize_root_key_dropdown(self, zone):
        """Keeps the Mapping tab's Root Key combo showing whichever selected zone's key center
        was most recently clicked (or blank/-1 if none is selected) -- mirrors how a knob's
        readout follows selection elsewhere in this file."""
        self.root_key_box.blockSignals(True)
        if zone is None: self.root_key_box.setCurrentIndex(-1)
        elif zone.key_center == -1: self.root_key_box.setCurrentIndex(128)
        else: self.root_key_box.setCurrentIndex(zone.key_center)
        self.root_key_box.blockSignals(False)

    def sync_loop_editor_to_zone(self, zone):
        """Keeps the Mapping tab's loop/round-robin inspector (ZoneInspectorPanel) showing
        whichever single zone is currently selected, same convention as the Root Key dropdown
        above -- called from the same SamplerMappingGrid selection-change sites."""
        self.zone_inspector.set_zone(zone)

    def _on_root_key_selection_changed(self, index):
        if index == -1: return
        target_value = self.root_key_box.itemData(index)
        changed_zones = []
        for zone in self.mapping_grid.sample_zones:
            if zone.is_selected:
                zone.key_center = target_value
                changed_zones.append(zone)
        if changed_zones:
            self.mapping_grid.update()
            self.transmit_zone_upsert(changed_zones)

    def _generate_record_filename(self):
        os.makedirs(self.record_folder, exist_ok=True)
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        ext = "wv" if self.record_format_wavpack.isChecked() else "wav"
        return os.path.join(self.record_folder, f"HDS_{stamp}.{ext}")

    def _update_record_button_enabled(self, *_args):
        any_source = self.record_synth_output_cb.isChecked() or any(cb.isChecked() for cb in self.record_input_channel_cbs)
        # Don't yank the button out from under an in-progress recording just because every
        # source got unchecked live -- only gate the ability to START a new one.
        if not self.record_toggle_btn.isChecked():
            self.record_toggle_btn.setEnabled(any_source)

    def _on_record_toggled(self, checked):
        self.record_toggle_btn.setText("■ Stop" if checked else "● Record")
        if checked:
            self._current_record_path = self._generate_record_filename()
            self.record_status_label.setText(f"Starting… {os.path.basename(self._current_record_path)}")
            self.record_status_label.setStyleSheet("color: #b58900; font-weight: bold;")
        self.transmit_knob_packet()

    def _rebuild_routing_output_columns(self, n_pairs):
        """Shows/hides the Routing tab's output-pair columns (header label + every source's
        checkbox in that column) to match n_pairs = detected_output_channels // 2 -- unlike the
        Recording tab's per-channel checkboxes (which are destroyed/recreated since the count
        drives which raw channel index a checkbox even means), an output-pair checkbox's meaning
        never changes, so it's simplest and safest to just show/hide it (never resetting whatever
        was checked) rather than replace it."""
        n_pairs = max(0, min(n_pairs, OUTPUT_PAIR_MAX))
        for p in range(OUTPUT_PAIR_MAX):
            visible = p < n_pairs
            self.routing_output_col_headers[p].setVisible(visible)
            for src in range(SOURCE_COUNT):
                self.routing_output_pair_cbs[src][p].setVisible(visible)

    def _rebuild_record_channel_checkboxes(self, n):
        while self.record_channels_grid.count():
            item = self.record_channels_grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.setParent(None)
        self.record_input_channel_cbs = []
        cols = 4
        for i in range(n):
            cb = QCheckBox(f"In {i + 1}")
            cb.toggled.connect(self._update_record_button_enabled)
            cb.toggled.connect(self.transmit_knob_packet)
            self.record_channels_grid.addWidget(cb, i // cols, i % cols)
            self.record_input_channel_cbs.append(cb)
        device_name = self.input_device_combo.currentText() if hasattr(self, "input_device_combo") else ""
        self.record_channels_detect_label.setText(f"Detected {n} channel(s) on '{device_name}'." if n else "No input channels detected.")
        self._update_record_button_enabled()

    def _update_recording_status(self, is_recording, elapsed_seconds, record_error, detected_channels):
        if detected_channels > 0 and detected_channels != self._last_detected_input_channels:
            self._last_detected_input_channels = detected_channels
            self._rebuild_record_channel_checkboxes(min(detected_channels, RECORD_MAX_CHANNELS))

        if record_error:
            self.record_status_label.setText("Recording failed to start — check the folder/channels and try again.")
            self.record_status_label.setStyleSheet("color: #ff5555; font-weight: bold;")
            if self.record_toggle_btn.isChecked():
                self.record_toggle_btn.blockSignals(True)
                self.record_toggle_btn.setChecked(False)
                self.record_toggle_btn.blockSignals(False)
                self.record_toggle_btn.setText("● Record")  # blockSignals above skipped _on_record_toggled's own text update
                self.transmit_knob_packet() # actually tell the backend to clear record_enable too
        elif is_recording:
            mins, secs = divmod(int(elapsed_seconds), 60)
            name = os.path.basename(self._current_record_path) if self._current_record_path else ""
            self.record_status_label.setText(f"● Recording {name} — {mins:02d}:{secs:02d}")
            self.record_status_label.setStyleSheet("color: #ff5555; font-weight: bold;")
        elif not self.record_toggle_btn.isChecked():
            self.record_status_label.setText("Not recording")
            self.record_status_label.setStyleSheet("color: #888;")

    def _mod_source_label(self, cc_value):
        for label, val in self._MOD_SOURCE_PRESETS:
            if val == cc_value:
                return label
        if 0.0 <= cc_value < 128.0:
            return f"CC {int(cc_value)}"
        return "None"

    def _update_mod_source_tooltip(self, idx):
        label = self._mod_source_label(self.lfo_mod_source_cc[idx])
        self.lfo_depth_dials[idx].setToolTip(f"LFO {idx + 1} Depth  |  Modulation Source: {label}\n(right-click to assign)")

    def _set_mod_source(self, idx, cc_value):
        self.lfo_mod_source_cc[idx] = cc_value
        self._update_mod_source_tooltip(idx)
        self.transmit_knob_packet()

    def _prompt_custom_cc(self, idx):
        current = int(self.lfo_mod_source_cc[idx]) if 0 <= self.lfo_mod_source_cc[idx] < 128 else 1
        cc_num, ok = QInputDialog.getInt(self, "Custom MIDI CC", "CC number (0-127):", current, 0, 127, 1)
        if ok:
            self._set_mod_source(idx, float(cc_num))

    # --- Generalized "Modulate From" mod-matrix assignment menu ---
    #
    # An LFO's/EG's "Destination" IS one field: dest_target (LFO) / eg_dest_target (EG), shown
    # today via the LFO section's own Destination combo (lfo_dest_combos/eg_dest_combos). The
    # right-click menu below is just a SECOND way to set that exact same field -- toggling an
    # entry here calls .setCurrentIndex() on the very same combo box, so the two views can never
    # disagree; there is no separate state to keep in sync. This absorbed the older
    # LFO-Depth-only _show_mod_source_menu (still available, folded into _show_lfo_row_menu's
    # 'depth' case, since Depth ALSO has its own separate multiplicative CC/aftertouch source --
    # mod_source_cc -- which coexists with, and is unrelated to, this additive routing).

    # Target numbers for LFO Rate/Depth/Phase-as-mod-destination, mirroring backend.c's
    # lfo_rate_target_index/lfo_depth_target_index/lfo_phase_target_index exactly (inverse
    # direction: LFO index -> target number). Each family spans two ranges -- LFO 1-4's original
    # slots and LFO 5-16's slots appended after target 47 -- kept in sync with those C helpers.
    def _lfo_rate_dest_target(self, idx):
        return 14 + idx if idx < 4 else 44 + idx

    def _lfo_depth_dest_target(self, idx):
        return 18 + idx if idx < 4 else 56 + idx

    def _lfo_phase_dest_target(self, idx):
        return 28 + idx if idx < 4 else 68 + idx

    def _toggle_modulate_from(self, combo, target, checked):
        # Checking an entry points that LFO's/EG's own Destination combo at `target`; unchecking
        # it (re-clicking the already-checked entry) sets it back to "None" (index 0). Either way
        # this fires the combo's existing currentIndexChanged wiring, which already calls
        # transmit_knob_packet() -- but an explicit call here too costs nothing and keeps this
        # method correct even if that wiring ever changes.
        combo.setCurrentIndex(target if checked else 0)
        self.transmit_knob_packet()

    def _populate_modulate_from_menu(self, menu, target):
        # Every LFO + every assignable EG, each toggling that source's own Destination combo to
        # `target`. Self-modulation is allowed (an LFO/EG may target its own Rate/Depth/Phase),
        # per this feature's own design decision. An entry already pointed elsewhere shows its
        # current destination in the label, so stealing it from there is never a silent surprise
        # -- multiple sources CAN already simultaneously target the same destination (their
        # contributions just sum), this is not a single-owner model.
        menu.addSection("Modulate From")
        sources = [(f"LFO {i + 1}", self.lfo_dest_combos[i]) for i in range(LFO_COUNT)]
        sources += [(f"EG {i + 1}", self.eg_dest_combos[i]) for i in range(len(self.eg_dest_combos))]
        for label, combo in sources:
            current = combo.currentIndex()
            checked = (current == target)
            display = label
            if not checked and 0 < current < len(self.matrix_shared_routes):
                display = f"{label}  (→ {self.matrix_shared_routes[current]})"
            action = QAction(display, self)
            action.setCheckable(True)
            action.setChecked(checked)
            action.triggered.connect(lambda ch, c=combo, t=target: self._toggle_modulate_from(c, t, ch))
            menu.addAction(action)

    def _show_lfo_row_menu(self, idx, knob_kind, global_pos):
        # Right-click menu for one LFO row's Rate/Depth/Phase knob. Depth additionally offers the
        # existing CC/aftertouch "Modulation Source" section (mod_source_cc) -- a separate,
        # multiplicative scale on this LFO's own output, unrelated to (and coexisting with) the
        # additive "Modulate From" destination routing every knob here shares.
        target = {
            'rate': self._lfo_rate_dest_target(idx),
            'depth': self._lfo_depth_dest_target(idx),
            'phase': self._lfo_phase_dest_target(idx),
        }[knob_kind]
        menu = QMenu(self)
        if knob_kind == 'depth':
            menu.addSection("Modulation Source (CC)")
            current = self.lfo_mod_source_cc[idx]
            for label, val in self._MOD_SOURCE_PRESETS:
                action = QAction(label, self)
                action.setCheckable(True)
                action.setChecked(current == val)
                action.triggered.connect(lambda checked, i=idx, v=val: self._set_mod_source(i, v))
                menu.addAction(action)
            menu.addSeparator()
            custom_action = QAction("Custom CC…", self)
            custom_action.setCheckable(True)
            custom_action.setChecked(current >= 0.0 and current < 128.0 and current not in (1.0, 2.0, 4.0, 11.0))
            custom_action.triggered.connect(lambda checked, i=idx: self._prompt_custom_cc(i))
            menu.addAction(custom_action)
        self._populate_modulate_from_menu(menu, target)
        menu.exec(global_pos)

    def _show_dest_assign_menu(self, target, global_pos):
        # Right-click menu for a generic destination knob (Filter cutoff/res, pulse width,
        # oscillator phase/pitch): just the "Modulate From" section -- these have no CC-scale
        # mechanism of their own the way LFO Depth does.
        menu = QMenu(self)
        self._populate_modulate_from_menu(menu, target)
        menu.exec(global_pos)

    # --- Filter Cutoff right-click menu: Key Track preset list + ext-MIDI CC/aftertouch source +
    # the generic "Modulate From" section, mirroring _show_lfo_row_menu's 'depth' case exactly
    # (same three-section shape). Resonance has no Key Track/ext-MIDI section of its own -- its
    # right-click stays on the plain _show_dest_assign_menu above, per the literal ask ("right
    # click menu on cutoff freqs").

    def _filter_cutoff_dest_target(self, filter_idx):
        return 90 + filter_idx * 2  # 90, 92, 94, 96, 98 -- matches backend.c's case labels exactly

    def _set_filter_key_track(self, filter_idx, value):
        self.filter_key_track[filter_idx] = value
        self.transmit_knob_packet()

    def _set_filter_mod_source(self, filter_idx, cc_value):
        self.filter_cutoff_mod_source_cc[filter_idx] = cc_value
        self.transmit_knob_packet()

    def _prompt_custom_filter_cc(self, filter_idx):
        current_cc = self.filter_cutoff_mod_source_cc[filter_idx]
        current = int(current_cc) if 0 <= current_cc < 128 else 1
        cc_num, ok = QInputDialog.getInt(self, "Custom MIDI CC", "CC number (0-127):", current, 0, 127, 1)
        if ok:
            self._set_filter_mod_source(filter_idx, float(cc_num))

    def _show_filter_cutoff_menu(self, filter_idx, global_pos):
        menu = QMenu(self)
        menu.addSection("Key Track")
        current_kt = self.filter_key_track[filter_idx]
        for label, val in self._KEY_TRACK_PRESETS:
            action = QAction(label, self)
            action.setCheckable(True)
            action.setChecked(current_kt == val)
            action.triggered.connect(lambda checked, i=filter_idx, v=val: self._set_filter_key_track(i, v))
            menu.addAction(action)
        menu.addSection("Modulation Source (ext MIDI)")
        current_cc = self.filter_cutoff_mod_source_cc[filter_idx]
        for label, val in self._MOD_SOURCE_PRESETS:
            action = QAction(label, self)
            action.setCheckable(True)
            action.setChecked(current_cc == val)
            action.triggered.connect(lambda checked, i=filter_idx, v=val: self._set_filter_mod_source(i, v))
            menu.addAction(action)
        menu.addSeparator()
        custom_action = QAction("Custom CC…", self)
        custom_action.setCheckable(True)
        custom_action.setChecked(current_cc >= 0.0 and current_cc < 128.0 and current_cc not in (1.0, 2.0, 4.0, 11.0))
        custom_action.triggered.connect(lambda checked, i=filter_idx: self._prompt_custom_filter_cc(i))
        menu.addAction(custom_action)
        self._populate_modulate_from_menu(menu, self._filter_cutoff_dest_target(filter_idx))
        menu.exec(global_pos)

    # --- Osc 1 Amp EG -> Osc 2/3/4 copy/sync ---

    def _copy_amp_eg_to_all(self):
        """'1->All' button: one-shot static copy of Osc 1's Amp EG onto Osc 2/3/4."""
        state = self.vco1_amp_eg.get_state()
        for panel in (self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg):
            panel.set_state(state)
        # set_state() already fires valuesChanged (wired to transmit_knob_packet), but an
        # explicit call here costs nothing and keeps this correct even if that wiring ever changes.
        self.transmit_knob_packet()

    def _mirror_amp_eg_live(self, *_unused):
        """Connected to vco1_amp_eg's valuesChanged/typeChanged while the Sync toggle is engaged
        -- re-copies Osc 1's current Amp EG state onto Osc 2/3/4 on every change."""
        state = self.vco1_amp_eg.get_state()
        for panel in (self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg):
            panel.set_state(state)
        self.transmit_knob_packet()

    def _on_amp_eg_live_sync_toggled(self, checked):
        """'Sync' toggle button: while checked, Osc 2/3/4's Amp EG continuously live-mirrors
        Osc 1's (an immediate mirror on check, then on every subsequent Osc 1 Amp EG change) and
        their panels are disabled to avoid a tug-of-war with manual edits; unchecking stops the
        mirroring and re-enables them, left wherever they last mirrored to."""
        targets = (self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg)
        if checked:
            self._mirror_amp_eg_live()
            for panel in targets:
                panel.setEnabled(False)
            self.vco1_amp_eg.valuesChanged.connect(self._mirror_amp_eg_live)
            self.vco1_amp_eg.typeChanged.connect(self._mirror_amp_eg_live)
        else:
            self.vco1_amp_eg.valuesChanged.disconnect(self._mirror_amp_eg_live)
            self.vco1_amp_eg.typeChanged.disconnect(self._mirror_amp_eg_live)
            for panel in targets:
                panel.setEnabled(True)

    def connect_to_backend(self):
        # UDP: construction never fails just because nothing's listening yet (unlike the old
        # AF_UNIX SOCK_STREAM connect(), whose success/failure used to BE the mock-mode signal).
        # Real liveness is inferred from whether /hds/status/* actually arrives -- see
        # listen_for_status_frames()/_backend_is_live(). Also carries the Sample Bank catalog
        # (/hds/zone/upsert, /hds/zone/remove) now -- the backend's OSC listener has a single
        # port for everything, so the old second socket is gone.
        self.osc_client = SimpleUDPClient(OSC_HOST, OSC_SEND_PORT)

        self.status_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status_sock.setblocking(False)
        try:
            self.status_sock.bind((OSC_BIND_HOST, OSC_RECV_PORT))
        except OSError as e:
            print(f"--- could not bind OSC status port {OSC_RECV_PORT}: {e} ---")
            self.status_sock = None
        self.mock_mode = True  # flips to False the first time a real status bundle arrives
        print(f"OSC client -> {OSC_HOST}:{OSC_SEND_PORT}, status listener on {OSC_BIND_HOST}:{OSC_RECV_PORT}")

    # Status feedback: 7 /hds/status/* messages (peak, is_recording, elapsed_seconds,
    # record_error, detected_input_channels, detected_output_channels, tempo_bpm_live) sent as
    # one OSC bundle -- see send_osc_status() in backend.c. Each incoming UDP datagram is already
    # one complete OSC packet (bundle or single message), so unlike the old stream-socket read
    # there's no partial-message reassembly to do here at all.
    _STATUS_ADDRESSES = (
        "/hds/status/peak", "/hds/status/is_recording", "/hds/status/elapsed_seconds",
        "/hds/status/record_error", "/hds/status/detected_input_channels",
        "/hds/status/detected_output_channels", "/hds/status/tempo_bpm_live",
        "/hds/status/seq_elapsed_seconds", "/hds/status/seq_transport_state",
        "/hds/status/midi_clock_locked",
    )

    def listen_for_status_frames(self):
        if self.status_sock is None:
            return
        got_any = False
        while True:
            try:
                dgram, _addr = self.status_sock.recvfrom(4096)
            except (BlockingIOError, OSError):
                break
            try:
                packet = OscPacket(dgram)
            except OscParseError:
                continue
            got_any = True
            values = {}
            seq_audio_updates = {}  # lane_index -> {"state": ..., "frames"?: ..., "channels"?: ..., "sample_rate"?: ...}
            smf_import_fields = {}  # field name (with any "marker/N/..." or "tempo_marker/N/..." prefix kept) -> value
            smf_import_track_fields = {}  # field name -> value, see /hds/status/smf_import_track/ handling below
            for timed_msg in packet.messages:
                msg = timed_msg.message
                if msg.address in self._STATUS_ADDRESSES and msg.params:
                    values[msg.address] = msg.params[0]
                elif msg.address.startswith("/hds/status/smf_import/") and msg.params:
                    # One-shot reply to /hds/song/import_smf (see song_import_smf_handler() in
                    # backend.c) -- sent as a single small bundle, so unlike the chunked
                    # /hds/request_state dump there's no sentinel to wait for: everything this
                    # import produced arrives in this one packet, applied once the loop below ends.
                    smf_import_fields[msg.address[len("/hds/status/smf_import/"):]] = msg.params[0]
                elif msg.address.startswith("/hds/status/smf_import_track/") and msg.params:
                    # One-shot reply to /hds/song/import_smf_track (per-lane "Load MIDI..." --
                    # see song_import_smf_track_handler() in backend.c). Same single-bundle, no-
                    # sentinel shape as smf_import_fields just above, just scoped to one lane.
                    smf_import_track_fields[msg.address[len("/hds/status/smf_import_track/"):]] = msg.params[0]
                elif msg.address.startswith("/hds/status/seq_audio/") and msg.params:
                    # /hds/status/seq_audio/<lane>/<field> -- a variable-length, mostly-empty set
                    # (see send_seq_audio_status() in backend.c), so unlike the fixed-shape
                    # _STATUS_ADDRESSES tuple above these are parsed by prefix, not by exact
                    # membership.
                    parts = msg.address.split("/")
                    try:
                        lane_idx = int(parts[4]); field = parts[5]
                    except (IndexError, ValueError):
                        continue
                    seq_audio_updates.setdefault(lane_idx, {})[field] = msg.params[0]
                elif msg.address.startswith("/hds/status/ext_in_peak/") and msg.params:
                    # /hds/status/ext_in_peak/<pair> -- Routing tab's per-Ext-In VU meters (see
                    # gui_routing_tab_visible/_on_main_tab_changed); only ever non-zero while this
                    # tab is actually on-screen. Applied directly (not buffered) since there's
                    # nothing to reconcile against -- each message is already the latest value.
                    try:
                        pair = int(msg.address.rsplit("/", 1)[1])
                    except ValueError:
                        continue
                    src = COMBI_PROGRAM_COUNT + 1 + pair
                    if 0 <= src < len(self.routing_ext_in_vu_meters) and self.routing_ext_in_vu_meters[src] is not None:
                        self.routing_ext_in_vu_meters[src].setValue(int(max(0, min(100, msg.params[0] * 100.0))))
                elif msg.address == "/hds/status/state_dump_complete":
                    # Sentinel closing out a /hds/request_state reply -- see
                    # send_full_state_dump() in backend.c. Apply everything buffered so far, then
                    # reset for next time (a stray sentinel with nothing buffered is harmless).
                    self._apply_full_state_fields(self._state_dump_buffer)
                    self._state_dump_buffer = {}
                elif (msg.params and not msg.address.startswith("/hds/status/")
                        and msg.address not in self._STATUS_ADDRESSES):
                    # Everything else /hds/request_state's reply carries: a plain live knob
                    # address+value, same address namespace transmit_knob_packet() itself uses --
                    # buffer it until the sentinel above closes out the dump.
                    self._state_dump_buffer[msg.address] = msg.params[0]
            if seq_audio_updates:
                self._apply_seq_audio_status(seq_audio_updates)
            if smf_import_fields:
                self._apply_smf_import_result(smf_import_fields)
            if smf_import_track_fields:
                self._apply_smf_track_import_result(smf_import_track_fields)
            if "/hds/status/peak" in values:
                self.vu_meter.setValue(int(max(0, min(100, values["/hds/status/peak"] * 100.0))))
            if all(a in values for a in self._STATUS_ADDRESSES[1:5]):
                self._update_recording_status(
                    bool(values["/hds/status/is_recording"]),
                    values["/hds/status/elapsed_seconds"],
                    bool(values["/hds/status/record_error"]),
                    int(values["/hds/status/detected_input_channels"]),
                )
            if "/hds/status/detected_output_channels" in values:
                out_channels = int(values["/hds/status/detected_output_channels"])
                if out_channels > 0 and out_channels != self._last_detected_output_channels:
                    self._last_detected_output_channels = out_channels
                    self._rebuild_routing_output_columns(min(out_channels // 2, OUTPUT_PAIR_MAX))
            if "/hds/status/tempo_bpm_live" in values:
                self.tempo_live_readout.setText(f"engine: {values['/hds/status/tempo_bpm_live']:.1f} BPM")
            if "/hds/status/midi_clock_locked" in values:
                self._midi_clock_locked = bool(values["/hds/status/midi_clock_locked"])
                self._update_seq_sync_lock_label()
            # Sequencer tab's transport readout -- real, live data (the transport's own real
            # playhead, seq_transport_state, plus current tempo). Bars:Beats is plain
            # presentation-layer math (assumes 4/4), same spirit as the rest of this file's habit
            # of keeping such conversions out of the DSP hot path.
            #
            # Bar numbering is 0-indexed: the playhead itself starts at plain, non-negative 0.0 the
            # instant Play is pressed (backend.c never runs it negative -- nothing needs a "before
            # zero" to avoid clipping, since recording is already active from that first instant).
            # What "measure 0" means is purely this display choice -- the first measure a performer
            # hears/plays in is labeled "0", so the nominal downbeat they're counting in towards is
            # "1" -- exactly like a hardware sequencer's one-bar count-in, without the playhead or
            # any recorded timestamp ever going negative.
            if "/hds/status/seq_elapsed_seconds" in values and "/hds/status/tempo_bpm_live" in values:
                elapsed = values["/hds/status/seq_elapsed_seconds"]
                bpm = max(1.0, values["/hds/status/tempo_bpm_live"])
                minutes, seconds = divmod(elapsed, 60.0)
                self.seq_elapsed_label.setText(f"{int(minutes):02d}:{seconds:04.1f}")
                beats_total = elapsed * bpm / 60.0
                bar = int(beats_total // 4)
                beat = int(beats_total % 4) + 1
                self.seq_bars_beats_label.setText(f"{bar}:{beat}")
            if "/hds/status/seq_transport_state" in values:
                state = int(values["/hds/status/seq_transport_state"])
                self.seq_transport_state_label.setText(("STOPPED", "PLAYING", "PAUSED")[state] if 0 <= state <= 2 else "?")

        if got_any:
            self._last_status_time = time.monotonic()
        # Liveness: UDP has no connect-time signal, so "is the backend there" is inferred from
        # whether status has arrived recently (a few missed vu_timer ticks' worth of silence).
        self.mock_mode = (self._last_status_time == 0.0) or (time.monotonic() - self._last_status_time > OSC_STATUS_TIMEOUT_S)
    def transmit_knob_packet(self):
        """Sends every knob's current value as one OSC bundle (one UDP datagram) to the backend --
        the OSC replacement for the old struct.pack'd binary_packet. Every value-computation line
        below is unchanged from the pre-OSC version; only the destination (an addressed OSC
        message added to `bundle` via add_f()/add_s(), instead of packet_list.append()) differs.
        Addresses must match backend.c's OSC_PARAMS[] table exactly -- see
        scratchpad/check_osc_address_parity.py for the cross-check between the two."""
        glide_ms = self.glide_dial.value()
        self.glide_readout.setText(f"{glide_ms} ms")
        glide_random_pct = self.glide_random_dial.value()
        self.glide_random_readout.setText(f"{glide_random_pct}%")
        self.tempo_readout.setText(f"{self.tempo_dial.value()} BPM")
        self.transpose_readout.setText(f"{self.transpose_dial.value():+d} st")
        arp_vel = self.arp_velocity_dial.value()
        self.arp_velocity_readout.setText("As Played" if arp_vel == 0 else str(arp_vel))
        self.arp_gate_readout.setText(f"{self.arp_gate_dial.value()}%")

        # BUG FIX: this used to unconditionally build and queue a message for every single
        # address below (~700+ of them) on EVERY call -- and this function is called on every
        # single widget change, including every intermediate value a knob/dial emits while being
        # dragged (dozens of times a second). A UDP packet that size, that often, sent
        # synchronously from the same widget-drag event loop, is exactly the kind of load that
        # can visibly compete with the backend's own per-block OSC drain for real-time budget --
        # more prone to show up as an audible glitch on whichever feature happens to be most
        # timing-sensitive at that moment (the arpeggiator, reported directly). It also silently
        # blocked ever running the frontend and backend on separate machines: flooding a real
        # network link (not loopback) with a full ~700-field resend on every mouse-move of a
        # knob drag is a real, not theoretical, problem the moment this stops being localhost.
        # Fixed at the root: only add a message for an address whose value actually changed since
        # the last transmit (self._last_wire_state already tracks exactly that, see __init__'s own
        # comment on it) -- a knob drag now sends a bundle containing just the one changed field,
        # not the entire control surface, every single time. First call after startup (or after a
        # fresh connect/Program load) still sends everything, since _last_wire_state starts empty
        # -- a genuine one-time full sync, not a regression.
        bundle = OscBundleBuilder(IMMEDIATELY)
        bundle_has_content = [False]  # list, not bool: mutable from the nested closures below
        def add_f(address, value):
            value = float(value)
            if self._last_wire_state.get(address) == value:
                return
            msg = OscMessageBuilder(address=address)
            msg.add_arg(value, "f")
            bundle.add_content(msg.build())
            bundle_has_content[0] = True
            self._last_wire_state[address] = value  # see __init__'s own comment on this dict
        def add_s(address, value):
            value = str(value)
            if self._last_wire_state.get(address) == value:
                return
            msg = OscMessageBuilder(address=address)
            msg.add_arg(value, "s")
            bundle.add_content(msg.build())
            bundle_has_content[0] = True
            self._last_wire_state[address] = value

        vassign = 1.0 if self.poly_radio.isChecked() else 0.0
        v1_shape = float(self.vco1_combo.currentIndex())
        v2_shape = float(self.vco2_combo.currentIndex())
        fm = float(self.fm_depth_dials["fm_intensity"].value()) / 100.0

        # Raw layout mixing levels. vco1_vol/vco2_vol/master_vol all go through the audio
        # (power-law) taper -- see _audio_vol_taper's own comment.
        v1_vol = _audio_vol_taper(self.vco1_vol.value())
        v2_vol = _audio_vol_taper(self.vco2_vol.value())
        g_time = float(glide_ms)
        m_vol = _audio_vol_taper(self.master_vol.value())
        v1_p = float(self.vco1_pan.value()) / 100.0
        v2_p = float(self.vco2_pan.value()) / 100.0

        v1_fine = float(self.vco1_fine_tune.value())
        v2_pitch = float(self.vco2_pitch_offset.value())
        v2_fine = float(self.vco2_fine_tune.value())

        # Baseline: 13 core parameters
        add_f("/hds/is_polyphonic", vassign)
        add_f("/hds/vco1/shape", v1_shape)
        add_f("/hds/vco2/shape", v2_shape)
        add_f("/hds/fm/intensity", fm)
        add_f("/hds/vco1/vol", v1_vol)
        add_f("/hds/vco2/vol", v2_vol)
        add_f("/hds/portamento_time_ms", g_time)
        add_f("/hds/master_volume", m_vol)
        add_f("/hds/transpose_semitones", self.transpose_dial.value())
        add_f("/hds/tuning/enable", 1.0 if self.tuning_enable_check.isChecked() else 0.0)
        add_f("/hds/tuning/reference_note", self.tuning_ref_note_spin.value())
        add_f("/hds/tuning/reference_freq", self.tuning_ref_freq_spin.value())
        add_f("/hds/vco1/pan", v1_p)
        add_f("/hds/vco2/pan", v2_p)
        add_f("/hds/vco1/fine_tune", v1_fine)
        add_f("/hds/vco2/pitch_offset", v2_pitch)
        add_f("/hds/vco2/fine_tune", v2_fine)

        # 8 parameters (Delay, Hold, Attack, Decay, Sustain, Release, Attack Start, Attack Peak)
        # for all 10 envelope instances: Osc 1-4 Amp (index 0-3), then EG 1-6 (index 4-9) -- must
        # match ENV_COUNT/PackEGParams order in backend.c exactly.
        all_panels = [self.vco1_amp_eg, self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg] + self.envelopes
        for eg_idx, panel in enumerate(all_panels):
            add_f(f"/hds/eg/{eg_idx}/delay", panel.dials["D"].value())
            add_f(f"/hds/eg/{eg_idx}/hold", panel.dials["H"].value())
            add_f(f"/hds/eg/{eg_idx}/attack", panel.dials["A"].value())
            add_f(f"/hds/eg/{eg_idx}/decay", panel.dials["D_phase"].value())
            add_f(f"/hds/eg/{eg_idx}/sustain", panel.dials["S"].value())
            add_f(f"/hds/eg/{eg_idx}/release", panel.dials["R"].value())
            add_f(f"/hds/eg/{eg_idx}/attack_start_pct", panel.attack_level_pct)
            add_f(f"/hds/eg/{eg_idx}/attack_peak_pct", panel.attack_peak_pct)

        # --- Filters: 5 flat, interchangeable slots, chained in Series or summed in Parallel
        # (filter_chain_mode) -- see backend.c's own comments on ProgramState.filters/
        # FilterParams for the design this mirrors. ---
        add_f("/hds/filter/chain_mode", self.filter_chain_mode_combo.currentIndex())
        for i in range(5):
            add_f(f"/hds/filter/{i}/cutoff", self.filter_cutoff_dials[i].value())
            add_f(f"/hds/filter/{i}/res_q", self.filter_res_dials[i].value())
            add_f(f"/hds/filter/{i}/slope", self.filter_slope_combos[i].currentIndex())
            add_f(f"/hds/filter/{i}/type", self.filter_type_combos[i].currentIndex())
            add_f(f"/hds/filter/{i}/key_track", self.filter_key_track[i])
            add_f(f"/hds/filter/{i}/cc_source", self.filter_cutoff_mod_source_cc[i])

        # --- Post-filter 3-band EQ (Low Shelf / Mid Peak / High Shelf) -- see backend.c's
        # EqBandParams/ProgramState.eq_bands comment. Q is only meaningful for the Mid Peak band
        # (index 1); eq_q_dials[0]/[2] are None for the two Shelf bands.
        for b in range(3):
            add_f(f"/hds/eq/{b}/freq", self.eq_freq_dials[b].value())
            add_f(f"/hds/eq/{b}/gain", self.eq_gain_dials[b].value())
            if self.eq_q_dials[b] is not None:
                add_f(f"/hds/eq/{b}/q", self.eq_q_dials[b].value() / 10.0)

        # --- Combi/Sequencer scaffolding (see backend.c's SEQ_LANE_COUNT comment) ---
        if self.osc_combi_radio.isChecked():
            perf_mode = 1
        elif self.osc_sequencer_radio.isChecked():
            perf_mode = 2
        else:
            perf_mode = 0
        add_f("/hds/performance_mode", perf_mode)
        # Sequencer's 32-lane widget set is always sent, unconditionally, regardless of
        # Performance Mode -- these are the canonical values for every /hds/seq/... address,
        # including lanes 16-31 (Audio-lane range) which Combi's view doesn't have at all. In
        # Combi mode specifically, Combi's own 16-lane widgets are sent AFTER (so they win for
        # the overlapping lanes 0-15) -- Combi lanes are always Program/MIDI with no real
        # seq_mode concept, matching "these two dropdowns will not be necessary for combi mode."
        for i, (_row, fader, pan_dial, type_combo, mode_combo) in enumerate(self.seq_lane_rows):
            add_f(f"/hds/seq/{i}/lane_type", type_combo.currentIndex())
            add_f(f"/hds/seq/{i}/volume", fader.value())
            add_f(f"/hds/seq/{i}/pan", float(pan_dial.value()) / 100.0)
            add_f(f"/hds/seq/{i}/midi_channel", self.seq_lane_midi_channel_combos[i].currentIndex())  # 0=Omni, else 1-16
            add_f(f"/hds/seq/{i}/mode", mode_combo.currentIndex())
            add_f(f"/hds/seq/{i}/mute", 1 if self.seq_lane_mute_buttons[i].isChecked() else 0)
            add_f(f"/hds/seq/{i}/rec_armed", 1 if self.seq_lane_rec_buttons[i].isChecked() else 0)
            add_f(f"/hds/seq/{i}/rec_overdub", self.seq_lane_overdub_combos[i].currentIndex())
            audio_widgets = self.seq_lane_audio_widgets[i]
            add_s(f"/hds/seq/{i}/audio_path", audio_widgets.get("path", "") or "")
            add_f(f"/hds/seq/{i}/audio_offset_sec", float(audio_widgets["offset_dial"].value()) / 10.0)
            add_f(f"/hds/seq/{i}/audio_preview_trigger", audio_widgets.get("preview_trigger", 0))
        if perf_mode == 1:
            for i, (_row, fader, pan_dial, _type_combo, _mode_combo) in enumerate(self.combi_lane_rows):
                add_f(f"/hds/seq/{i}/lane_type", 0)  # Combi lanes are always Program/MIDI
                add_f(f"/hds/seq/{i}/volume", fader.value())
                add_f(f"/hds/seq/{i}/pan", float(pan_dial.value()) / 100.0)
                add_f(f"/hds/seq/{i}/midi_channel", 0)
                add_f(f"/hds/seq/{i}/mode", 0)  # meaningless in Combi mode
                add_f(f"/hds/seq/{i}/mute", 0)
                add_f(f"/hds/seq/{i}/rec_armed", 0)
                add_f(f"/hds/seq/{i}/rec_overdub", 0)

        # Combi lanes' own real MIDI channel routing (0=Omni, else 1-16) -- separate from the
        # /hds/seq/{i}/midi_channel line above, which is Sequencer's own inert per-lane field.
        # Sent unconditionally (same convention as every other knob here), consumed only by
        # process_midi_input's Combi-mode dispatch branch in backend.c.
        for i, combo in enumerate(self.combi_lane_midi_channel_combos):
            add_f(f"/hds/combi/{i}/midi_channel", combo.currentIndex())

        # Combi lanes' own Key/Velocity zones (keyboard split / velocity switch) -- see the
        # spinboxes' own construction comment. Sent unconditionally, same convention as every
        # other knob here.
        for i in range(COMBI_PROGRAM_COUNT):
            add_f(f"/hds/combi/{i}/key_lo", self.combi_lane_key_lo_spins[i].value())
            add_f(f"/hds/combi/{i}/key_hi", self.combi_lane_key_hi_spins[i].value())
            add_f(f"/hds/combi/{i}/vel_lo", self.combi_lane_vel_lo_spins[i].value())
            add_f(f"/hds/combi/{i}/vel_hi", self.combi_lane_vel_hi_spins[i].value())
            add_f(f"/hds/combi/{i}/transpose", self.combi_lane_transpose_spins[i].value())

        # Sequencer transport -- edge-triggered counters, same idiom as each audio lane's own
        # Preview trigger (see poll_transport_triggers in backend.c).
        add_f("/hds/seq/transport/play", self.seq_transport_play_trigger)
        add_f("/hds/seq/transport/pause", self.seq_transport_pause_trigger)
        add_f("/hds/seq/transport/stop", self.seq_transport_stop_trigger)
        add_f("/hds/seq/transport/seek_start", self.seq_transport_seek_start_trigger)
        add_f("/hds/seq/transport/seek_back", self.seq_transport_seek_back_trigger)
        add_f("/hds/seq/transport/seek_fwd", self.seq_transport_seek_fwd_trigger)
        add_f("/hds/seq/transport/seek_to_target_sec", self.seq_transport_seek_to_target_sec)
        add_f("/hds/seq/transport/seek_to", self.seq_transport_seek_to_trigger)
        add_f("/hds/seq/transport/metronome", 1.0 if self.seq_btn_metronome.isChecked() else 0.0)
        add_f("/hds/seq/quantize/enable", 1.0 if self.seq_btn_quantize.isChecked() else 0.0)
        add_f("/hds/seq/quantize/resolution", float(SEQ_QUANTIZE_RESOLUTIONS[self.seq_quantize_resolution_combo.currentIndex()]))
        add_f("/hds/seq/sync_source", self.seq_sync_source_combo.currentIndex())

        # LFOs (incl. phase offset + mod source CC + Tempo Sync)
        for idx in range(LFO_COUNT):
            lfo_hz = float(self.lfo_dials[idx].value()) / 10.0
            sync_div_idx = self.lfo_tempo_sync_combos[idx].currentIndex()
            # Readout shows the actual Hz value while Free, or the division label while Synced --
            # showing "4.5 Hz" for a rate the backend is actually ignoring would be misleading.
            self.lfo_readouts[idx].setText(f"{lfo_hz:.1f} Hz" if sync_div_idx == 0 else LFO_SYNC_DIVISION_LABELS[sync_div_idx])
            lfo_dp = float(self.lfo_depth_dials[idx].value()) / 100.0
            add_f(f"/hds/lfo/{idx}/wave", self.lfo_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/rate", lfo_hz)
            add_f(f"/hds/lfo/{idx}/depth", lfo_dp)
            add_f(f"/hds/lfo/{idx}/key_sync", self.lfo_sync_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/dest", self.lfo_dest_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/phase_offset", self.lfo_phase_dials[idx].value())
            add_f(f"/hds/lfo/{idx}/mod_source_cc", self.lfo_mod_source_cc[idx])
            add_f(f"/hds/lfo/{idx}/per_key", 1.0 if self.lfo_per_key_checks[idx].isChecked() else 0.0)
            add_f(f"/hds/lfo/{idx}/key_track", self.lfo_key_track_dials[idx].value())
            add_f(f"/hds/lfo/{idx}/vel_track", self.lfo_vel_track_dials[idx].value())
            add_f(f"/hds/lfo/{idx}/tempo_sync", sync_div_idx)
            add_f(f"/hds/lfo/{idx}/curve_drive", self.lfo_curve_dials[idx].value() / 100.0)

        # 6 custom EG intensity multipliers
        for idx in range(6):
            add_f(f"/hds/eg_mod/{idx}/intensity", self.eg_intensity_dials[idx].value() / 100.0)

        # Each assignable EG's own mod destination
        for idx in range(6):
            add_f(f"/hds/eg_mod/{idx}/dest", self.eg_dest_combos[idx].currentIndex())

        # 10 active envelope mode selection indices: Osc 1-4 Amp (0-3), then EG 1-6 (4-9) -- same
        # eg_idx convention as the EG-params loop above.
        add_f("/hds/eg/0/topology_mode", self.vco1_amp_eg.combo_type.currentIndex())
        add_f("/hds/eg/1/topology_mode", self.vco2_amp_eg.combo_type.currentIndex())
        add_f("/hds/eg/2/topology_mode", self.vco3_amp_eg.combo_type.currentIndex())
        add_f("/hds/eg/3/topology_mode", self.vco4_amp_eg.combo_type.currentIndex())
        for env_idx, env in enumerate(self.envelopes):
            add_f(f"/hds/eg/{4 + env_idx}/topology_mode", env.combo_type.currentIndex())

        # Unison Saw shape controls (Osc 1/2 only)
        add_f("/hds/vco1/unison/detune", self.vco1_unison_detune.value())
        add_f("/hds/vco1/unison/width", self.vco1_unison_width.value())
        add_f("/hds/vco2/unison/detune", self.vco2_unison_detune.value())
        add_f("/hds/vco2/unison/width", self.vco2_unison_width.value())

        # Mono Legato glide time randomization amount
        add_f("/hds/mono_glide_random", self.glide_random_dial.value())

        # Mute flags, decoupled from FM
        add_f("/hds/vco1/mute", 1.0 if self.vco1_mute.isChecked() else 0.0)
        add_f("/hds/vco2/mute", 1.0 if self.vco2_mute.isChecked() else 0.0)

        # Hard Sync mode: 0=Off, 1=Parallel, 2=Serial
        add_f("/hds/hard_sync_mode", self.hard_sync_combo.currentIndex())

        # Phase pots, degrees 0-360
        add_f("/hds/vco1/phase_offset", self.vco1_phase.value())
        add_f("/hds/vco2/phase_offset", self.vco2_phase.value())

        # Manual Pitch knobs (new): dial range is +-24 semitones, wire field is cents (*100) to
        # match fine_tune/mod_vcoN_pitch's own units -- see backend.c's vco{1..4}_pitch fields.
        add_f("/hds/vco1/pitch", self.vco1_pitch.value() * 100.0)
        add_f("/hds/vco2/pitch", self.vco2_pitch.value() * 100.0)
        add_f("/hds/vco3/pitch", self.vco3_pitch.value() * 100.0)
        add_f("/hds/vco4/pitch", self.vco4_pitch.value() * 100.0)

        # Pulse width, Osc 1/2
        add_f("/hds/vco1/pulse_width", self.vco1_pulse_width.value())
        add_f("/hds/vco2/pulse_width", self.vco2_pulse_width.value())
        # Monophonic Noise (Noise shape only -- see the checkbox's own tooltip/comment)
        add_f("/hds/vco1/mono_noise", 1.0 if self.vco1_mono_noise.isChecked() else 0.0)
        add_f("/hds/vco2/mono_noise", 1.0 if self.vco2_mono_noise.isChecked() else 0.0)
        # Velocity -> Amp (see the knob's own tooltip/comment, on each Amp EG's title row)
        add_f("/hds/vco1/vel_to_amp", self.vco1_vel_to_amp.value())
        add_f("/hds/vco2/vel_to_amp", self.vco2_vel_to_amp.value())
        # Curve/Drive (see each box's own tooltip; dial range -100..100 -> wire -1.0..1.0, same
        # convention as Pan)
        add_f("/hds/vco1/curve_drive", self.vco1_curve_drive.value() / 100.0)
        add_f("/hds/vco2/curve_drive", self.vco2_curve_drive.value() / 100.0)

        # Osc 3: vol, pan, mute, pulse-width-or-tilt. vol goes through the audio taper, same as
        # Osc 1/2 above -- see _audio_vol_taper's own comment.
        add_f("/hds/vco3/vol", _audio_vol_taper(self.vco3_vol.value()))
        add_f("/hds/vco3/pan", self.vco3_pan.value() / 100.0)
        add_f("/hds/vco3/mute", 1.0 if self.vco3_mute.isChecked() else 0.0)
        add_f("/hds/vco3/pulse_width", self.vco3_pulse_width.value())
        add_f("/hds/vco3/mono_noise", 1.0 if self.vco3_mono_noise.isChecked() else 0.0)
        add_f("/hds/vco3/vel_to_amp", self.vco3_vel_to_amp.value())
        add_f("/hds/vco3/curve_drive", self.vco3_curve_drive.value() / 100.0)

        # Osc 4: vol, pan, mute, pulse-width-or-tilt. vol goes through the audio taper, same as
        # Osc 1/2/3 above -- see _audio_vol_taper's own comment.
        add_f("/hds/vco4/vol", _audio_vol_taper(self.vco4_vol.value()))
        add_f("/hds/vco4/pan", self.vco4_pan.value() / 100.0)
        add_f("/hds/vco4/mute", 1.0 if self.vco4_mute.isChecked() else 0.0)
        add_f("/hds/vco4/pulse_width", self.vco4_pulse_width.value())
        add_f("/hds/vco4/mono_noise", 1.0 if self.vco4_mono_noise.isChecked() else 0.0)
        add_f("/hds/vco4/vel_to_amp", self.vco4_vel_to_amp.value())
        add_f("/hds/vco4/curve_drive", self.vco4_curve_drive.value() / 100.0)

        # External Audio Input: enable + gain per ext-in pair (Routing tab). Ext-in pairs sit at
        # source ids COMBI_PROGRAM_COUNT+1..+8 (9-16), after the 8 Combi Programs + Combi Mix.
        for p in range(EXT_IN_PAIR_COUNT):
            src = COMBI_PROGRAM_COUNT + 1 + p
            add_f(f"/hds/ext_in/{p}/enable", 1.0 if self.routing_ext_in_enable_cbs[src].isChecked() else 0.0)
            add_f(f"/hds/ext_in/{p}/gain", self.routing_ext_in_gain_dials[src].value() / 100.0)

        # Multi-output hardware routing: which output pair(s) each source feeds (Routing tab).
        for src in range(SOURCE_COUNT):
            for p in range(OUTPUT_PAIR_MAX):
                add_f(f"/hds/routing/{src}/out/{p}", 1.0 if self.routing_output_pair_cbs[src][p].isChecked() else 0.0)

        # FX Bus: 12 slots, each type + source + 8 params + mix + bypass. A plain dial always sends
        # value/100.0 -- a 0-100-range dial (per FX_TYPE_METADATA) reaches the backend as
        # 0.0-1.0, a -100..100 one as -1.0..1.0. A "choice" param instead sends (selected index /
        # (option count - 1)) -- see FX_TYPE_METADATA's own comment on that convention. Mix
        # itself stays 0-100 raw (backend divides it by 100 on its own side).
        for slot_idx in range(FX_SLOT_COUNT):
            type_idx = self.fx_type_combos[slot_idx].currentIndex()
            add_f(f"/hds/fx/{slot_idx}/type", type_idx)
            add_f(f"/hds/fx/{slot_idx}/source", float(self.fx_source_combos[slot_idx].currentIndex()))
            entry = FX_TYPE_METADATA[type_idx] if 0 <= type_idx < len(FX_TYPE_METADATA) else FX_TYPE_METADATA[0]
            params_meta = entry["params"]
            for p_idx in range(FX_SLOT_PARAM_COUNT):
                spec = params_meta[p_idx] if p_idx < len(params_meta) else None
                if spec is not None and spec.get("type") == "choice":
                    combo = self.fx_param_combos[slot_idx][p_idx]
                    option_count = combo.count()
                    value = float(combo.currentIndex()) / float(option_count - 1) if option_count > 1 else 0.0
                else:
                    value = float(self.fx_param_dials[slot_idx][p_idx].value()) / 100.0
                add_f(f"/hds/fx/{slot_idx}/param/{p_idx}", value)
            add_f(f"/hds/fx/{slot_idx}/mix", self.fx_mix_dials[slot_idx].value())
            add_f(f"/hds/fx/{slot_idx}/bypass", 1.0 if self.fx_bypass_buttons[slot_idx].isChecked() else 0.0)
            add_f(f"/hds/fx/{slot_idx}/chain_to_next", 1.0 if self.fx_chain_to_next_buttons[slot_idx].isChecked() else 0.0)

        # FX Bus routing: 0 = Series, 1 = Parallel -- now per-source (see SOURCE_COUNT).
        for src in range(SOURCE_COUNT):
            add_f(f"/hds/fx_routing/{src}/mode", 1.0 if self.fx_source_routing_btns[src].isChecked() else 0.0)

        # Pitch Bend range, up/down, semitones
        add_f("/hds/pb/range_up", self.pb_range_up_spin.value())
        add_f("/hds/pb/range_down", self.pb_range_down_spin.value())

        # Sustain pedal polarity: 0 = Normal, 1 = Inverted
        add_f("/hds/sustain/polarity", 1.0 if self.sustain_polarity_inverted.isChecked() else 0.0)

        # Audio latency, ms -- friendly preset dropdown, see audio_latency_ms in backend.c
        latency_idx = self.audio_latency_combo.currentIndex()
        latency_ms = self._LATENCY_PRESETS[latency_idx][1] if 0 <= latency_idx < len(self._LATENCY_PRESETS) else 25.0
        add_f("/hds/audio/latency_ms", latency_ms)

        # Live polyphony ceiling -- see active_voice_limit in backend.c
        add_f("/hds/audio/polyphony", self.polyphony_spin.value())

        # Live render block size, samples -- see block_size_frames in backend.c
        bs_idx = self.block_size_combo.currentIndex()
        block_size = self._BLOCK_SIZE_PRESETS[bs_idx] if 0 <= bs_idx < len(self._BLOCK_SIZE_PRESETS) else 128
        add_f("/hds/audio/block_size", block_size)

        # Recording: enable, include-synth-output, downmix, 16 per-channel enables
        add_f("/hds/record/enable", 1.0 if self.record_toggle_btn.isChecked() else 0.0)
        add_f("/hds/record/include_synth_output", 1.0 if self.record_synth_output_cb.isChecked() else 0.0)
        add_f("/hds/record/downmix_stereo", 1.0 if self.record_mode_downmix.isChecked() else 0.0)
        for idx in range(RECORD_MAX_CHANNELS):
            checked = idx < len(self.record_input_channel_cbs) and self.record_input_channel_cbs[idx].isChecked()
            add_f(f"/hds/record/channel/{idx}/enable", 1.0 if checked else 0.0)

        # Tempo: the Tempo knob (tempo_bpm) plus a programmable tempo-map system in the backend --
        # tempo_marker_count + up to TEMPO_MARKER_MAX (time, bpm) marker pairs, interpolated
        # between (see compute_current_tempo_bpm() in backend.c). There's still no marker EDITOR
        # UI, but importing a Standard MIDI File (_pick_and_import_midi()) can now populate a real
        # map -- self._imported_tempo_markers caches whatever song_import_smf_handler() last
        # reported, and MUST be resent as-is on every call here (this method blasts the whole live
        # state on any change at all), or the very next unrelated knob tweak would silently wipe
        # the just-imported map back to flat/zero. Manually clearing it back to "no map" isn't
        # exposed anywhere yet -- re-importing (or restarting the GUI) is the only way, matching
        # the same gap Load Scale's own tuning table has.
        add_f("/hds/tempo/bpm", self.tempo_dial.value())
        add_f("/hds/arp/enabled", 1.0 if self.arp_enabled_btn.isChecked() else 0.0)
        add_f("/hds/arp/style", self.arp_style_combo.currentIndex())
        add_f("/hds/arp/velocity", float(self.arp_velocity_dial.value()))
        add_f("/hds/arp/gate_pct", float(self.arp_gate_dial.value()))
        add_f("/hds/tempo/marker_count", float(len(self._imported_tempo_markers)))
        for idx in range(TEMPO_MARKER_MAX):
            t = self._imported_tempo_markers[idx][0] if idx < len(self._imported_tempo_markers) else 0.0
            add_f(f"/hds/tempo/marker/{idx}/time", t)
        for idx in range(TEMPO_MARKER_MAX):
            bpm = self._imported_tempo_markers[idx][1] if idx < len(self._imported_tempo_markers) else 0.0
            add_f(f"/hds/tempo/marker/{idx}/bpm", bpm)

        # Sample Bank: NOT part of this bundle -- the Mapping tab's zone catalog travels over its
        # own dedicated OSC addresses instead (see transmit_zone_upsert()/transmit_zone_remove()),
        # sent only when the catalog itself actually changes, not resent on every unrelated knob
        # turn like everything above. See SAMPLE_SLOT_CAPACITY's comment in backend.c for why.

        # Osc 3 identity/tuning: shape, fine tune, pitch offset, phase offset, unison detune/width.
        add_f("/hds/vco3/shape", self.vco3_combo.currentIndex())
        add_f("/hds/vco3/fine_tune", self.vco3_fine_tune.value())
        add_f("/hds/vco3/pitch_offset", self.vco3_pitch_offset.value())
        add_f("/hds/vco3/phase_offset", self.vco3_phase.value())
        add_f("/hds/vco3/unison/detune", self.vco3_unison_detune.value())
        add_f("/hds/vco3/unison/width", self.vco3_unison_width.value())

        # Osc 4 identity/tuning, mirrors Osc 3's block exactly.
        add_f("/hds/vco4/shape", self.vco4_combo.currentIndex())
        add_f("/hds/vco4/fine_tune", self.vco4_fine_tune.value())
        add_f("/hds/vco4/pitch_offset", self.vco4_pitch_offset.value())
        add_f("/hds/vco4/phase_offset", self.vco4_phase.value())
        add_f("/hds/vco4/unison/detune", self.vco4_unison_detune.value())
        add_f("/hds/vco4/unison/width", self.vco4_unison_width.value())

        # FM Algorithm system: algorithm index, then every depth field. fm_intensity itself was
        # already sent above (the "2->1" depth, used by every algorithm that has that edge).
        add_f("/hds/fm/algorithm", self.fm_algorithm_combo.currentIndex())
        add_f("/hds/fm/depth/3_1", self.fm_depth_dials["fm_depth_3_1"].value() / 100.0)
        add_f("/hds/fm/depth/3_2", self.fm_depth_dials["fm_depth_3_2"].value() / 100.0)
        add_f("/hds/fm/depth/4_1", self.fm_depth_dials["fm_depth_4_1"].value() / 100.0)
        add_f("/hds/fm/depth/4_3", self.fm_depth_dials["fm_depth_4_3"].value() / 100.0)
        add_f("/hds/fm/depth/1_2", self.fm_depth_dials["fm_depth_1_2"].value() / 100.0)
        add_f("/hds/fm/depth/3_4", self.fm_depth_dials["fm_depth_3_4"].value() / 100.0)
        add_f("/hds/fm/depth/1_3", self.fm_depth_dials["fm_depth_1_3"].value() / 100.0)

        # Recording format: 0=WAV, 1=WavPack
        add_f("/hds/record/format", 1.0 if self.record_format_wavpack.isChecked() else 0.0)

        # Device/MIDI selection + record path -- OSC strings, not fixed-size byte buffers (the
        # backend truncates safely on its own side via snprintf into its fixed char buffers).
        midi_selection = self.midi_device_combo.currentText()
        midi_wire_value = "auto" if midi_selection == MIDI_AUTO_LABEL else midi_selection
        # Defensive fallback: an ALSA PCM device name of "" is never valid (unlike MIDI's "auto"
        # convention above) -- if the combo is ever caught between clear() and repopulation (or
        # any other transient empty state), fall back to "default" rather than transmitting an
        # empty device name, which the backend would otherwise ignore as "no change requested"
        # (see main()'s output_device_valid/input_device_valid guard) -- sending "default"
        # explicitly is still the more useful behavior for the user here.
        output_device_text = self.output_device_combo.currentText() or "default"
        input_device_text = self.input_device_combo.currentText() or "default"
        add_s("/hds/device/output", output_device_text)
        add_s("/hds/device/input", input_device_text)
        add_s("/hds/device/midi", midi_wire_value)
        add_s("/hds/record/path", self._current_record_path)

        if self.osc_client is not None and bundle_has_content[0]:
            try: self.osc_client.send(bundle.build())
            except OSError: pass

    # --- Sample Bank catalog: two dedicated OSC addresses, "/hds/zone/upsert" and
    # "/hds/zone/remove" (see the matching comment block above zone_upsert_handler() in
    # backend.c). Each OSC message carries exactly ONE zone -- a Mapping tab edit touching several
    # zones just sends several messages back to back, unlike the old wire-framed socket's
    # one-message-covers-many-zones batching.
    # zone_id (i) + 18 floats (root_note, min_note, max_note, min_velocity, max_velocity,
    # unpitched, loop_start, loop_end, loop_enabled, round_robin_group, tune_cents, gain_db, pan,
    # start_offset_frac, reverse, loop_release_exit, end_frac, assigned_osc) + path (s) -- must
    # match ZoneRecordWire's field order in backend.c exactly.
    _ZONE_UPSERT_TYPES = "i" + "f" * 18 + "s"

    def _zone_to_osc_args(self, zone):
        is_drum = (zone.key_center == -1)
        root_note = 60.0 if is_drum else float(zone.key_center)  # DRUM sends 60 as a placeholder --
        # sample_slot_unpitched forces playback rate to 1.0 regardless of this value.
        return (
            zone.zone_id,
            root_note, float(zone.root_note), float(zone.max_note),
            float(zone.min_velocity), float(zone.max_velocity),
            1.0 if is_drum else 0.0,
            float(zone.loop_start), float(zone.loop_end),
            1.0 if zone.loop_enabled else 0.0,
            float(zone.round_robin_group),
            # SFZ-style per-zone params -- must match ZoneRecordWire's field order exactly.
            float(zone.tune_cents), float(zone.gain_db), float(zone.pan) / 100.0,
            float(zone.start_offset), 1.0 if zone.reverse else 0.0,
            1.0 if zone.loop_release_exit else 0.0, float(zone.end_frac),
            float(zone.assigned_osc),
            zone.file_path,
        )

    def _on_main_tab_changed(self, index):
        """Gates the Routing tab's per-ext-in VU meters (see backend.c's gui_routing_tab_visible
        comment) so that metering cost is only ever paid while a player is actually looking at
        this tab, not for the life of the process. Sent as its own one-off message (not folded
        into the next transmit_knob_packet) so switching tabs takes effect immediately rather than
        waiting on the next incidental knob change."""
        if self.osc_client is None:
            return
        visible = (self.tab_widget.widget(index) is self.tab_routing)
        msg = OscMessageBuilder(address="/hds/gui/routing_tab_visible")
        msg.add_arg(1.0 if visible else 0.0, "f")
        try: self.osc_client.send(msg.build())
        except OSError: pass

    def transmit_zone_upsert(self, zones):
        """Sends one "/hds/zone/upsert" OSC message per given zone (never the whole catalog) --
        called from every Mapping tab edit site: drop, drag/resize, delete's sibling add,
        loop-marker drag, root-key change, round-robin-group change."""
        if self.osc_client is None or not zones:
            return
        for zone in zones:
            msg = OscMessageBuilder(address="/hds/zone/upsert")
            for arg, type_tag in zip(self._zone_to_osc_args(zone), self._ZONE_UPSERT_TYPES):
                msg.add_arg(arg, type_tag)
            try: self.osc_client.send(msg.build())
            except OSError: pass

    def transmit_zone_remove(self, zone_ids):
        """Sends one "/hds/zone/remove" OSC message per given zone_id."""
        if self.osc_client is None or not zone_ids:
            return
        for zid in zone_ids:
            msg = OscMessageBuilder(address="/hds/zone/remove")
            msg.add_arg(int(zid), "i")
            try: self.osc_client.send(msg.build())
            except OSError: pass

    def closeEvent(self, event):
        if self.osc_client is not None: self.osc_client.close()
        if self.status_sock is not None: self.status_sock.close()
        event.accept()
def runtime_connect_and_bind(gui):
    # Program save/load
    gui.btn_save.clicked.connect(gui._pick_and_save_program)
    gui.btn_load.clicked.connect(gui._pick_and_load_program)
    gui.btn_prev_prog.clicked.connect(lambda: gui._cycle_program(-1))
    gui.btn_next_prog.clicked.connect(lambda: gui._cycle_program(1))

    # Core global mode switches
    gui.poly_radio.toggled.connect(gui.transmit_knob_packet)
    gui.mono_radio.toggled.connect(gui.transmit_knob_packet)
    gui.hard_sync_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.pb_range_up_spin.valueChanged.connect(gui.transmit_knob_packet)
    gui.pb_range_down_spin.valueChanged.connect(gui.transmit_knob_packet)
    gui.sustain_polarity_normal.toggled.connect(gui.transmit_knob_packet)
    gui.sustain_polarity_inverted.toggled.connect(gui.transmit_knob_packet)
    gui.master_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.transpose_dial.valueChanged.connect(gui.transmit_knob_packet)

    # Oscillator selection changes
    gui.vco1_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.vco2_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.vco1_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_mute.toggled.connect(gui.transmit_knob_packet)
    gui.vco2_mute.toggled.connect(gui.transmit_knob_packet)
    for dial in gui.fm_depth_dials.values():
        dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.fm_algorithm_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.glide_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.glide_random_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.tempo_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.arp_enabled_btn.toggled.connect(gui.transmit_knob_packet)
    gui.arp_style_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.arp_velocity_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.arp_gate_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_mono_noise.toggled.connect(gui.transmit_knob_packet)
    gui.vco2_mono_noise.toggled.connect(gui.transmit_knob_packet)
    gui.vco1_vel_to_amp.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_vel_to_amp.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_curve_drive.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_curve_drive.valueChanged.connect(gui.transmit_knob_packet)

    # Oscillator 3 (full peer of Osc 1/2 now, was the dedicated Noise oscillator)
    gui.vco3_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.vco3_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_mute.toggled.connect(gui.transmit_knob_packet)
    gui.vco3_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_mono_noise.toggled.connect(gui.transmit_knob_packet)
    gui.vco3_vel_to_amp.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_curve_drive.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_pitch_offset.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_phase.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_unison_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_unison_detune.valueChanged.connect(gui.transmit_knob_packet)

    # Oscillator 4 (full peer of Osc 1/2/3 now, was the fixed Sub oscillator)
    gui.vco4_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.vco4_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_mute.toggled.connect(gui.transmit_knob_packet)
    gui.vco4_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_mono_noise.toggled.connect(gui.transmit_knob_packet)
    gui.vco4_vel_to_amp.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_curve_drive.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_pitch_offset.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_phase.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_unison_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_unison_detune.valueChanged.connect(gui.transmit_knob_packet)

    # Detuning, pitch transposition, and basic phases
    gui.vco1_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pitch_offset.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_phase.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_phase.valueChanged.connect(gui.transmit_knob_packet)
    # Bug fix: these 4 Pitch knobs (vco1_pitch..vco4_pitch) were built with a working backend
    # (knobs->vcoN_pitch sums directly into the same mod_vcoN_pitch accumulator LFO/EG modulation
    # already writes into -- see backend.c's render_audio_block) and a working OSC address, but
    # were never connected to transmit_knob_packet -- turning one did nothing audible until some
    # OTHER control happened to trigger a resend. LFO/EG-driven pitch modulation (via the
    # Destination dropdown/right-click menu) was never affected by this, since that path already
    # transmits on its own combo's currentIndexChanged.
    gui.vco1_pitch.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pitch.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_pitch.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco4_pitch.valueChanged.connect(gui.transmit_knob_packet)
    # Filters: 5 flat, interchangeable slots + the Series/Parallel chain-mode combo.
    gui.filter_chain_mode_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for dial in gui.filter_cutoff_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)
    for dial in gui.filter_res_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)
    for combo in gui.filter_slope_combos:
        combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for combo in gui.filter_type_combos:
        combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    # Post-filter 3-band EQ: eq_q_dials[0]/[2] are None (shelf bands have no Q control).
    for dial in gui.eq_freq_dials + gui.eq_gain_dials + [d for d in gui.eq_q_dials if d is not None]:
        dial.valueChanged.connect(gui.transmit_knob_packet)

    # Combi/Sequencer lane scaffolding: fader/pan (both views) + type/seq-mode combos
    # (Sequencer view only -- Combi's rows have None in those two tuple slots).
    for _row, fader, pan_dial, type_combo, mode_combo in gui.combi_lane_rows + gui.seq_lane_rows:
        fader.valueChanged.connect(gui.transmit_knob_packet)
        pan_dial.valueChanged.connect(gui.transmit_knob_packet)
        if type_combo is not None:
            type_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
        if mode_combo is not None:
            mode_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for channel_combo in gui.seq_lane_midi_channel_combos:
        channel_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    # Combi lanes' own MIDI channel selector was never wired to transmit_knob_packet -- a
    # pre-existing gap (only seq_lane_midi_channel_combos, above, was) that would otherwise leave
    # a changed channel silently unsent until some other control happened to transmit. Fixed here
    # alongside the closely-related Key/Velocity zone spinboxes below, for the same reason.
    for channel_combo in gui.combi_lane_midi_channel_combos:
        channel_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for spin_list in (gui.combi_lane_key_lo_spins, gui.combi_lane_key_hi_spins,
                      gui.combi_lane_vel_lo_spins, gui.combi_lane_vel_hi_spins,
                      gui.combi_lane_transpose_spins):
        for spin in spin_list:
            spin.valueChanged.connect(gui.transmit_knob_packet)
    for mute_btn, rec_btn, overdub_combo in zip(gui.seq_lane_mute_buttons, gui.seq_lane_rec_buttons, gui.seq_lane_overdub_combos):
        mute_btn.toggled.connect(gui.transmit_knob_packet)
        rec_btn.toggled.connect(gui.transmit_knob_packet)
        overdub_combo.currentIndexChanged.connect(gui.transmit_knob_packet)

    # Unison Unison Saw control loops
    gui.vco1_unison_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_unison_detune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_unison_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_unison_detune.valueChanged.connect(gui.transmit_knob_packet)

    # Amplitude and matrix multi-envelope engines
    all_egs = [gui.vco1_amp_eg, gui.vco2_amp_eg, gui.vco3_amp_eg, gui.vco4_amp_eg] + gui.envelopes
    for eg in all_egs:
        eg.valuesChanged.connect(gui.transmit_knob_packet)
        eg.combo_type.currentIndexChanged.connect(gui.transmit_knob_packet)

    for drop in gui.eg_dest_combos:
        drop.currentIndexChanged.connect(gui.transmit_knob_packet)
    for dial in gui.eg_intensity_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)

    # Assignable LFO engines and small static vector visualizations
    for idx in range(LFO_COUNT):
        gui.lfo_combos[idx].currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.lfo_combos[idx].currentIndexChanged.connect(
            lambda idx_cb, i=idx: gui.lfo_visualizers[i].set_waveform(idx_cb, is_lfo=True)
        )
        gui.lfo_visualizers[idx].set_waveform(gui.lfo_combos[idx].currentIndex(), is_lfo=True)
        # Curve/Drive visibility + label ("Curve" for Tri/Saw = idx 0/2, "Drive" for Sine = idx 1,
        # hidden for Sq/Off = idx 3/4) -- combo indices match LFO_WAVE order ["Tri","Sine","Saw","Sq","Off"].
        def _on_lfo_wave_changed(wave_idx, i=idx):
            gui.lfo_curve_labels[i].setVisible(wave_idx in (0, 1, 2))
            gui.lfo_curve_dials[i].setVisible(wave_idx in (0, 1, 2))
            gui.lfo_curve_labels[i].setText("Drive" if wave_idx == 1 else "Curve")
        gui.lfo_combos[idx].currentIndexChanged.connect(_on_lfo_wave_changed)
        _on_lfo_wave_changed(gui.lfo_combos[idx].currentIndex())
        gui.lfo_curve_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_depth_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_phase_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_sync_combos[idx].currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.lfo_tempo_sync_combos[idx].currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.lfo_per_key_checks[idx].toggled.connect(gui.transmit_knob_packet)
        gui.lfo_key_track_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_vel_track_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_dest_combos[idx].currentIndexChanged.connect(gui.transmit_knob_packet)

    # FX Bus: 12 slots, each wired the same way
    for slot_idx in range(FX_SLOT_COUNT):
        # Reconfigure the slot's param dials (labels/ranges/defaults/visibility) BEFORE
        # transmitting, so a Type change sends the new effect's actual default params rather
        # than whatever the previous effect's dials happened to be sitting at.
        gui.fx_type_combos[slot_idx].currentIndexChanged.connect(
            lambda idx_cb, i=slot_idx: gui._apply_fx_slot_type(i, idx_cb)
        )
        gui.fx_type_combos[slot_idx].currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.fx_source_combos[slot_idx].currentIndexChanged.connect(gui.transmit_knob_packet)
        for p_dial in gui.fx_param_dials[slot_idx]:
            p_dial.valueChanged.connect(gui.transmit_knob_packet)
        for p_combo in gui.fx_param_combos[slot_idx]:
            p_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.fx_mix_dials[slot_idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.fx_bypass_buttons[slot_idx].toggled.connect(gui.transmit_knob_packet)
        gui.fx_chain_to_next_buttons[slot_idx].toggled.connect(gui.transmit_knob_packet)
    for btn in gui.fx_source_routing_btns:
        btn.toggled.connect(gui.transmit_knob_packet)

    # Routing tab: per-pair ext-in enable/gain + the source x output-pair grid. Only ext-in
    # sources (ids COMBI_PROGRAM_COUNT+1..SOURCE_COUNT-1) have enable/gain widgets -- Programs
    # and Combi Mix don't (see their construction's own comment).
    for src in range(COMBI_PROGRAM_COUNT + 1, SOURCE_COUNT):
        gui.routing_ext_in_enable_cbs[src].toggled.connect(gui.transmit_knob_packet)
        gui.routing_ext_in_gain_dials[src].valueChanged.connect(gui.transmit_knob_packet)
    for src in range(SOURCE_COUNT):
        for p in range(OUTPUT_PAIR_MAX):
            gui.routing_output_pair_cbs[src][p].toggled.connect(gui.transmit_knob_packet)

    # Routing tab's per-Ext-In VU meters are only ever metered backend-side while this tab is
    # actually on-screen (see gui_routing_tab_visible) -- tell it whenever the visible tab changes,
    # and once now for whatever tab happens to be showing at startup.
    gui.tab_widget.currentChanged.connect(gui._on_main_tab_changed)
    gui._on_main_tab_changed(gui.tab_widget.currentIndex())

    # Audio device selection
    gui.output_device_combo.currentTextChanged.connect(gui.transmit_knob_packet)
    gui.input_device_combo.currentTextChanged.connect(gui.transmit_knob_packet)
    gui.audio_latency_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.polyphony_spin.valueChanged.connect(gui.transmit_knob_packet)
    gui.block_size_combo.currentIndexChanged.connect(gui.transmit_knob_packet)

    # Recording tab: the toggle button itself and per-channel checkboxes are wired at
    # construction time (they also need to update UI state, not just retransmit); these two
    # just need to retransmit on change.
    gui.record_synth_output_cb.toggled.connect(gui.transmit_knob_packet)
    gui.record_mode_multichannel.toggled.connect(gui.transmit_knob_packet)
    gui.record_mode_downmix.toggled.connect(gui.transmit_knob_packet)
    gui.record_format_wav.toggled.connect(gui.transmit_knob_packet)
    gui.record_format_wavpack.toggled.connect(gui.transmit_knob_packet)
    gui.midi_device_combo.currentTextChanged.connect(gui.transmit_knob_packet)

    # Initialize separate telemetry thread loop for VU meter updates
    gui.vu_timer = QTimer()
    gui.vu_timer.timeout.connect(gui.listen_for_status_frames)
    gui.vu_timer.start(25)

    # Pull this instance's actual current state on connect, rather than broadcasting GUI defaults
    # over it (which used to silently clobber e.g. a DAW-restored CLAP patch the instant this
    # frontend attached -- see _apply_full_state_fields()/listen_for_status_frames()'s sentinel
    # handling for the receiving side, and send_full_state_dump()/request_state_handler() in
    # backend.c for the reply). A backend with nothing to report needs no reply at all: its own
    # init_default_controls() already established sensible defaults before this frontend ever
    # connected, so simply not transmitting is the correct fallback -- transmitting defaults is
    # exactly the bug this replaces.
    if gui.osc_client is not None:
        request_msg = OscMessageBuilder(address="/hds/request_state")
        try: gui.osc_client.send(request_msg.build())
        except OSError: pass


if __name__ == "__main__":
    # parse_known_args (not parse_args): leaves any Qt-specific args (-style, -platform, etc.)
    # untouched in sys.argv for QApplication(sys.argv) below, rather than erroring on them.
    _arg_parser = argparse.ArgumentParser(add_help=False)
    _arg_parser.add_argument("--osc-host", type=str, default=OSC_HOST,
                              help="Hostname/IP the backend is running on. Defaults to localhost; set this "
                                   "to run the frontend and backend on separate machines -- the backend's "
                                   "own OSC listener already accepts remote connections, but its "
                                   "--osc-reply-host must be pointed back at THIS machine for status "
                                   "feedback (VU meter, tempo, recording state) to find its way back.")
    _arg_parser.add_argument("--osc-listen-port", type=int, default=OSC_SEND_PORT,
                              help="Backend's OSC listen port (where this frontend sends knob/zone messages). "
                                   "9000 for the standalone app, 9010 for the CLAP plugin's embedded server.")
    _arg_parser.add_argument("--osc-reply-port", type=int, default=OSC_RECV_PORT,
                              help="Backend's OSC reply port (where /hds/status/* feedback is received). "
                                   "9001 for the standalone app, 9011 for the CLAP plugin's embedded server.")
    _args, _qt_argv_rest = _arg_parser.parse_known_args(sys.argv[1:])
    OSC_HOST = _args.osc_host
    OSC_SEND_PORT = _args.osc_listen_port
    OSC_RECV_PORT = _args.osc_reply_port

    app = QApplication([sys.argv[0]] + _qt_argv_rest)
    main_window = SynthGui()
    runtime_connect_and_bind(main_window)
    main_window.show()
    sys.exit(app.exec())
