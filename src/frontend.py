import sys
import os
import socket
import struct
import math
import random
import subprocess
import re
import time
import json
from datetime import datetime
import numpy as np
from pythonosc.udp_client import SimpleUDPClient
from pythonosc.osc_message_builder import OscMessageBuilder
from pythonosc.osc_bundle_builder import OscBundleBuilder, IMMEDIATELY
from pythonosc.osc_packet import OscPacket, ParseError as OscParseError
from PySide6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QComboBox, QGroupBox, QRadioButton, QTabWidget,
                             QSlider, QPushButton, QProgressBar, QSpinBox, QLineEdit, QCheckBox,
                             QScrollArea, QMenu, QInputDialog, QFileDialog, QGridLayout, QMessageBox)
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

# OSC control protocol endpoints -- must match backend.c's --osc-listen-port/--osc-reply-port
# defaults exactly (both localhost-only for now; see connect_to_backend()).
OSC_HOST = "127.0.0.1"
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
# Multi-source/multi-output audio routing: must match EXT_IN_PAIR_COUNT/SOURCE_COUNT/
# OUTPUT_PAIR_MAX in backend.c exactly. Source id 0 = Program, 1..EXT_IN_PAIR_COUNT = ext-in
# pairs -- see the bus/source model comment on SynthSharedControls in backend.c.
EXT_IN_PAIR_COUNT = 8
SOURCE_COUNT = 1 + EXT_IN_PAIR_COUNT
OUTPUT_PAIR_MAX = 8
SOURCE_NAMES = ["Program"] + [f"Ext In {p + 1}" for p in range(EXT_IN_PAIR_COUNT)]
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
# Combi/Sequencer lane scaffolding: must match SEQ_LANE_COUNT/PROGRAM_LANE_RESOURCE_COUNT/
# AUDIO_LANE_RESOURCE_COUNT in backend.c exactly.
SEQ_LANE_COUNT = 32
PROGRAM_LANE_RESOURCE_COUNT = 16
AUDIO_LANE_RESOURCE_COUNT = 16
ENGINE_SAMPLE_RATE = 48000  # must match SAMPLE_RATE in backend.c -- used only to warn when a
                              # loaded Sequencer audio lane's native rate differs (no resampling
                              # exists yet; see _apply_seq_audio_status)
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

def _list_alsa_devices(list_cmd):
    """Best-effort enumeration of ALSA PCM device names via `aplay -L` / `arecord -L`. Falls
    back to just ["default"] if the tool isn't installed, times out, or returns nothing --
    "default" is always a safe starting point."""
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

# Sentinel shown in the MIDI Input combo for auto-detect mode; encoded on the wire as "auto" --
# must match the backend's own "auto"/empty-string convention in midi_client_should_autoconnect.
MIDI_AUTO_LABEL = "Auto (recommended)"

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
        for zone in reversed(self.sample_zones):
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
            for zone in self.sample_zones:
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
            for zone in self.sample_zones: zone.is_selected = m_rect.intersects(self.get_zone_rect(zone))
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
        group_members = {}
        for zone in self.sample_zones:
            if zone.round_robin_group > 0:
                group_members.setdefault(zone.round_robin_group, []).append(zone)
        cascade_offset = {}
        for members in group_members.values():
            if len(members) > 1:
                for idx, zone in enumerate(sorted(members, key=lambda z: z.zone_id)):
                    cascade_offset[id(zone)] = idx * 9

        for zone in self.sample_zones:
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
_PROGRAM_SCOPE_EXCLUDED_PREFIXES = ("/hds/device/", "/hds/record/", "/hds/routing/", "/hds/ext_in/", "/hds/audio/")
def _is_program_scope_address(addr):
    if addr == "/hds/performance_mode" or addr.startswith("/hds/seq/"):
        return False
    # master_volume is deliberately excluded -- a mixing/session-level level, not part of "the
    # patch" itself (per the user's explicit direction), so loading a Program never yanks the
    # current output level out from under whatever it was set to.
    if addr == "/hds/master_volume":
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
        # /hds/status/* feedback on --osc-reply-port (default 9001). Both ends are on localhost --
        # see OSC_HOST/OSC_SEND_PORT/OSC_RECV_PORT below the imports for where to change that.
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
        # Program save/load: one subfolder per Program (~/HDS_Programs/<name>/<name>.hdsprog
        # [+ <name>.sfz]) -- mirrors the existing ~/HDS_Recordings convention (self.record_folder).
        self.programs_folder = os.path.expanduser("~/HDS_Programs")
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
        self.master_vol = ResetSlider(Qt.Orientation.Vertical, 50, self)
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
        _retain_hidden(self.vco1_pulse_box)
        v1_strip.addWidget(self.vco1_pulse_box)

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
        self.vco1_vol = ResetSlider(Qt.Orientation.Vertical, 80)
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
        _retain_hidden(self.vco2_pulse_box)
        v2_strip.addWidget(self.vco2_pulse_box)

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
        self.vco2_vol = ResetSlider(Qt.Orientation.Vertical, 80)
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
        _retain_hidden(self.vco3_pulse_box)
        v3_strip.addWidget(self.vco3_pulse_box)

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
        _retain_hidden(self.vco4_pulse_box)
        v4_strip.addWidget(self.vco4_pulse_box)

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
                mode_combo = QComboBox(); mode_combo.addItems(["Seq", "Mute", "Rec"])
                lay.addWidget(mode_combo)
            lay.addStretch()
            return (strip, fader, pan_dial, type_combo, mode_combo)

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

            offset_widget = QWidget()
            offset_lay = QVBoxLayout(offset_widget); offset_lay.setContentsMargins(0, 0, 0, 0); offset_lay.setSpacing(0)
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
                    "offset_dial": offset_dial, "offset_widget": offset_widget, "path": ""}

        # Combi view: 16 lanes, no type/seq-mode dropdowns -- every Combi lane is implicitly
        # Program/MIDI (Combi has no audio-lane concept at all, per the user's own "these two
        # dropdowns will not be necessary for combi mode"). Stays in the Play tab -- never moved
        # or merged with Sequencer.
        self.osc_combi_widget = QWidget()
        combi_lay = QVBoxLayout(self.osc_combi_widget)
        combi_lay.setContentsMargins(2, 2, 2, 2); combi_lay.setSpacing(2)
        combi_note = QLabel("Combi \u2014 up to 16 stacked Programs (fader + pan scaffolded; real per-lane synthesis is future work)")
        combi_note.setStyleSheet("color: #777; font-style: italic;")
        combi_lay.addWidget(combi_note)
        # Per-lane "pick a saved Program" dropdown -- the loading mechanism the user described,
        # added now as pure GUI-local bookkeeping (no backend field/OSC address -- see the
        # Program save/load plan's own reasoning: this is a load-TIME instruction, not a
        # continuously-read mixer parameter the way volume/pan/lane_type/seq_mode are). Doesn't
        # load anything into anything yet -- there's no per-lane engine to load into until real
        # N-instance synthesis exists.
        self.combi_lane_program_choice = [None] * PROGRAM_LANE_RESOURCE_COUNT
        self.combi_program_combos = []
        self.combi_lane_rows = []  # 16 x (strip, fader, pan_dial, None, None)
        combi_strips_widget = QWidget()
        combi_strips_lay = QHBoxLayout(combi_strips_widget)
        combi_strips_lay.setContentsMargins(0, 0, 0, 0); combi_strips_lay.setSpacing(3)
        for i in range(PROGRAM_LANE_RESOURCE_COUNT):
            lane = _build_seq_lane_row(i, show_seq_controls=False)
            prog_combo = QComboBox()
            prog_combo.setToolTip("Not yet functional -- picks a saved Program for this lane once per-lane synthesis exists")
            lane[0].layout().addWidget(prog_combo)
            prog_combo.currentTextChanged.connect(lambda text, idx=i: self.combi_lane_program_choice.__setitem__(idx, text or None))
            self.combi_program_combos.append(prog_combo)
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
        combi_mixer_row.addWidget(_build_lane_legend(["#", "Level", "Pan", "Program"]))
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
        # comment), a transport panel with a real (if inert) elapsed-time/tempo readout, and
        # Play/Pause/Record buttons that exist but aren't wired to anything yet -- explicitly
        # scaffolding, not a working sequencer transport. "Load Audio" (below) is real.
        # ----------------------------------------------------
        seq_outer_lay = QVBoxLayout(self.tab_sequencer)
        seq_outer_lay.setContentsMargins(6, 6, 6, 6); seq_outer_lay.setSpacing(6)

        transport_box = QGroupBox("Transport")
        transport_lay = QHBoxLayout(transport_box); transport_lay.setSpacing(8)
        self.seq_btn_play = QPushButton("\u25b6 Play")
        self.seq_btn_pause = QPushButton("\u23f8 Pause")
        self.seq_btn_record = QPushButton("\u23fa Rec")
        self.seq_btn_record.setStyleSheet("color: #c0392b; font-weight: bold;")
        for btn in (self.seq_btn_play, self.seq_btn_pause, self.seq_btn_record):
            btn.setEnabled(False)  # inert this pass -- no transport/playhead engine exists yet
            btn.setToolTip("Not yet functional -- scaffolding only")
            transport_lay.addWidget(btn)
        transport_lay.addSpacing(16)
        self.seq_elapsed_label = QLabel("00:00.0")
        self.seq_elapsed_label.setStyleSheet("font-family: monospace; font-size: 14px;")
        transport_lay.addWidget(QLabel("Elapsed:")); transport_lay.addWidget(self.seq_elapsed_label)
        self.seq_bars_beats_label = QLabel("1:1")
        self.seq_bars_beats_label.setStyleSheet("font-family: monospace; font-size: 14px;")
        transport_lay.addWidget(QLabel("Bars\u200a:\u200aBeats:")); transport_lay.addWidget(self.seq_bars_beats_label)
        transport_lay.addStretch()
        seq_outer_lay.addWidget(transport_box)

        self.seq_status_label = QLabel("")
        self.seq_status_label.setStyleSheet("color: #c0392b; font-style: italic;")
        seq_outer_lay.addWidget(self.seq_status_label)

        seq_strips_widget = QWidget()
        seq_strips_lay = QHBoxLayout(seq_strips_widget)
        seq_strips_lay.setContentsMargins(0, 0, 0, 0); seq_strips_lay.setSpacing(3)
        self.seq_lane_rows = []  # 32 x (strip, fader, pan_dial, type_combo, mode_combo)
        self.seq_lane_audio_widgets = []  # 32 x dict, see _build_seq_audio_controls
        for i in range(SEQ_LANE_COUNT):
            lane = _build_seq_lane_row(i, show_seq_controls=True)
            audio_widgets = _build_seq_audio_controls(i)
            lane[0].layout().addWidget(audio_widgets["box"])
            self.seq_lane_audio_widgets.append(audio_widgets)
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
        seq_mixer_row.addWidget(_build_lane_legend(["#", "Level", "Pan", "Type", "Mode", "Audio"]))
        seq_mixer_row.addWidget(seq_scroll, stretch=1)
        seq_outer_lay.addLayout(seq_mixer_row, 1)

        # Audio/MIDI type enforcement: capped at PROGRAM_LANE_RESOURCE_COUNT/AUDIO_LANE_RESOURCE_COUNT
        # active of each kind, since each type is meant to eventually draw from its own backend
        # resource pool (neither exists yet -- this is frontend-only validation this pass). Also
        # shows/hides that lane's "Load Audio" box -- meaningless for a Program/MIDI lane.
        def _enforce_seq_lane_type_cap(idx, new_index):
            # Only the Audio direction is actually capped: AUDIO_LANE_RESOURCE_COUNT is a real
            # future scarce-resource ceiling (RAM-cached audio buffers), so blocking a 17th Audio
            # lane is meaningful. Program/MIDI has no equivalent scarcity here -- every lane that
            # ISN'T Audio is trivially "the rest" (up to all 32, never more, since SEQ_LANE_COUNT
            # itself already bounds that) -- so there is nothing to enforce going the other way.
            # (PROGRAM_LANE_RESOURCE_COUNT is Combi's own, unrelated ceiling on simultaneous
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
        for i, lane in enumerate(self.seq_lane_rows):
            lane[3].currentIndexChanged.connect(lambda new_index, idx=i: _enforce_seq_lane_type_cap(idx, new_index))
        # ----------------------------------------------------
        # FILTERS TAB CONTENT (2-FILTER OVERHAUL)
        # ----------------------------------------------------
        filters_outer_lay = QVBoxLayout(self.tab_filters)
        filters_column = QVBoxLayout()
        filters_column.setSpacing(6)

        # Filter Routing Matrix: governs how Filter A and Filter B combine (Series/Parallel) --
        # filter-specific, so it lives here rather than in the Global sidebar.
        routing_box = QGroupBox("Filter Routing Matrix")
        routing_lay = QVBoxLayout(); routing_lay.setSpacing(2); routing_lay.setContentsMargins(4, 4, 4, 4)

        # Filter Routing quick-pick: a convenience preset on top of the detailed Signal Mode
        # combo below (never a separate source of truth -- picking one just sets Signal Mode /
        # Filter B's Off-Active / Filter A's Slope, the same knobs the detailed controls edit
        # directly). Mirrors a classic hardware synth's own 4-way "Filter Routing" parameter
        # (Single/Serial/Parallel/24dB-oct); "Custom" is never user-selectable -- it's what this
        # combo falls back to (see _sync_filter_routing_preset_combo below) whenever the
        # underlying knobs don't match one of the 4 named combinations, so it can never silently
        # lie about the current state.
        preset_row = QHBoxLayout()
        preset_row.addWidget(QLabel("Filter Routing:"))
        self.filter_routing_preset_combo = QComboBox()
        self.filter_routing_preset_combo.addItems(["Single", "Serial", "Parallel", "24dB/oct", "Custom"])
        preset_row.addWidget(self.filter_routing_preset_combo)
        preset_row.addStretch()
        routing_lay.addLayout(preset_row)

        routing_header_row = QHBoxLayout()
        routing_header_row.addWidget(QLabel("Signal Mode:"))
        routing_header_row.addStretch()
        routing_lay.addLayout(routing_header_row)

        self.filter_routing_mode = QComboBox()
        self.filter_routing_mode.addItems([
            "Series (mixer -> A -> B)",
            "Series Reversed (mixer -> B -> A)",
            "Parallel (A // B, level/pan below)",
        ])
        routing_lay.addWidget(self.filter_routing_mode)

        # Independent level/pan only matter in Parallel mode -- in either Series mode the chain
        # order alone decides which filter is heard last, so these are hidden rather than just
        # disabled. Replaces the old single A/B Blend knob (see the Filter Routing plan) with
        # real independent control per filter, matching a classic hardware synth's own Parallel
        # filter routing description ("individual control over both level and pan").
        self.filter_parallel_box = QWidget()
        par_lay = QHBoxLayout(self.filter_parallel_box)
        par_lay.setContentsMargins(0, 4, 0, 0); par_lay.setSpacing(6)
        par_lay.addWidget(QLabel("A Level:"))
        self.filter_a_level_dial = PotentiometerKnob("Filter A Level", self)
        self.filter_a_level_dial.setRange(0, 100); self.filter_a_level_dial.setValue(50)
        par_lay.addWidget(self.filter_a_level_dial)
        par_lay.addWidget(QLabel("A Pan:"))
        self.filter_a_pan_dial = PotentiometerKnob("Filter A Pan", self)
        self.filter_a_pan_dial.setRange(-100, 100); self.filter_a_pan_dial.setValue(0)
        par_lay.addWidget(self.filter_a_pan_dial)
        par_lay.addWidget(QLabel("B Level:"))
        self.filter_b_level_dial = PotentiometerKnob("Filter B Level", self)
        self.filter_b_level_dial.setRange(0, 100); self.filter_b_level_dial.setValue(50)
        par_lay.addWidget(self.filter_b_level_dial)
        par_lay.addWidget(QLabel("B Pan:"))
        self.filter_b_pan_dial = PotentiometerKnob("Filter B Pan", self)
        self.filter_b_pan_dial.setRange(-100, 100); self.filter_b_pan_dial.setValue(0)
        par_lay.addWidget(self.filter_b_pan_dial)
        par_lay.addStretch()
        routing_lay.addWidget(self.filter_parallel_box)
        self.filter_routing_mode.currentIndexChanged.connect(
            lambda idx: self.filter_parallel_box.setVisible(idx == 2)
        )
        self.filter_parallel_box.setVisible(False)

        routing_box.setLayout(routing_lay)
        filters_column.addWidget(routing_box)

        # Container list definitions for loop compilation connections
        self.cutoff_dials, self.q_dials, self.vcf_poles, self.vcf_types = [], [], [], []
        # Multi Filter blend: 5 filter-unit entries (index order matches cutoff_dials/vcf_types
        # above exactly -- 0=Filter A Normal, 1-3=Filter A Multi units 0-2, 4=Filter B), each a
        # (mode1_knobs[5], mode2_knobs[5], crossfade_knob, container_box) tuple. Container boxes
        # are shown only when that unit's Type combo is "Active", mirroring the existing
        # filter_a_multi_view show/hide convention one level up.
        self.filter_blend_units = []

        def _build_filter_blend_row(crossfade_target):
            box = QWidget()
            lay = QVBoxLayout(box); lay.setContentsMargins(0, 2, 0, 2); lay.setSpacing(2)
            mode1_knobs, mode2_knobs = [], []
            for mode_label, knob_list in (("Mode 1:", mode1_knobs), ("Mode 2:", mode2_knobs)):
                row = QHBoxLayout(); row.setSpacing(3)
                row.addWidget(QLabel(f"<b>{mode_label}</b>"))
                for tap_label in ["LP", "BP", "HP", "Notch", "Dry"]:
                    k = PotentiometerKnob(f"{mode_label} {tap_label}", self)
                    k.setRange(-100, 100); k.setValue(0)
                    row.addWidget(QLabel(tap_label + ":")); row.addWidget(k)
                    knob_list.append(k)
                row.addStretch()
                lay.addLayout(row)
            if mode1_knobs:
                mode1_knobs[0].setValue(100)  # default one-hot Low Pass, matches the backend default
            xf_row = QHBoxLayout(); xf_row.setSpacing(3)
            xf_row.addWidget(QLabel("Crossfade:"))
            xf_dial = PotentiometerKnob("Mode 1 <-> Mode 2 Crossfade", self)
            xf_dial.setRange(0, 100); xf_dial.setValue(0)
            xf_dial.setContextMenuCallback(lambda pos, t=crossfade_target: self._show_dest_assign_menu(t, pos))
            xf_row.addWidget(xf_dial)
            xf_row.addStretch()
            lay.addLayout(xf_row)
            self.filter_blend_units.append((mode1_knobs, mode2_knobs, xf_dial, box))
            return box

        # --- FILTER A VIEW PANEL ---
        filter_a_box = QGroupBox("VCF Filter A")
        filter_a_lay = QVBoxLayout()
        filter_a_lay.setSpacing(2); filter_a_lay.setContentsMargins(4, 4, 4, 4)

        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("<b>Engine Mode:</b>"))
        self.filter_a_mode_combo = QComboBox()
        self.filter_a_mode_combo.addItems(["Normal (Single Filter Core)", "Multi (3-Filter Cascade)"])
        mode_row.addWidget(self.filter_a_mode_combo)
        mode_row.addStretch()
        filter_a_lay.addLayout(mode_row)

        # View sub-widget stack for Engine Modes
        self.filter_a_stack_widget = QWidget()
        a_stack_lay = QVBoxLayout(self.filter_a_stack_widget)
        a_stack_lay.setContentsMargins(0, 0, 0, 0); a_stack_lay.setSpacing(4)

        # Filter A Sub-Widget: Normal (Single filter parameters mapped onto cutoff index 0),
        # stacked vertically over its own Multi Filter blend row (a0_blend_box below) so both
        # share this view's own show/hide with filter_a_multi_view.
        self.filter_a_normal_view = QWidget()
        a_norm_outer = QVBoxLayout(self.filter_a_normal_view)
        a_norm_outer.setContentsMargins(0, 0, 0, 0); a_norm_outer.setSpacing(2)
        a_norm_lay = QHBoxLayout()
        a_norm_lay.setContentsMargins(0, 4, 0, 4)

        c_dial_a0 = PotentiometerKnob("FA Freq", self)
        c_dial_a0.setRange(40, 18000); c_dial_a0.setValue(800)
        c_dial_a0.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(8, pos))
        self.cutoff_dials.append(c_dial_a0)

        q_dial_a0 = PotentiometerKnob("FA Res", self)
        q_dial_a0.setRange(0, 100); q_dial_a0.setValue(20)
        q_dial_a0.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(9, pos))
        self.q_dials.append(q_dial_a0)

        v_pole_a0 = QComboBox(); v_pole_a0.addItems(["2-Pole (12dB/oct)", "4-Pole (24dB/oct)"])
        self.vcf_poles.append(v_pole_a0)

        # Type is now just Off/Active -- the actual filter shape (Low/High/Band Pass, Notch, any
        # manual blend of them) comes from the Multi Filter blend row below, not an exclusive pick.
        v_type_a0 = QComboBox(); v_type_a0.addItems(["Off", "Active (Blend)"])
        v_type_a0.setCurrentIndex(1)
        self.vcf_types.append(v_type_a0)

        a_norm_lay.addWidget(QLabel("Freq:")); a_norm_lay.addWidget(c_dial_a0)
        a_norm_lay.addWidget(QLabel("Res:")); a_norm_lay.addWidget(q_dial_a0)
        a_norm_lay.addWidget(QLabel("Slope:")); a_norm_lay.addWidget(v_pole_a0)
        a_norm_lay.addWidget(QLabel("Type:")); a_norm_lay.addWidget(v_type_a0)
        a_norm_outer.addLayout(a_norm_lay)

        a0_blend_box = _build_filter_blend_row(86)  # target 86 = Filter A Crossfade
        v_type_a0.currentIndexChanged.connect(lambda idx: a0_blend_box.setVisible(idx == 1))
        a_norm_outer.addWidget(a0_blend_box)
        a_stack_lay.addWidget(self.filter_a_normal_view)

        # Filter A Sub-Widget: Multi engine (deploys 3 distinct filter blocks in a sub-grid)
        self.filter_a_multi_view = QWidget()
        a_mult_outer = QVBoxLayout(self.filter_a_multi_view)
        a_mult_outer.setContentsMargins(0, 4, 0, 4); a_mult_outer.setSpacing(4)

        routing_row = QHBoxLayout()
        routing_row.addWidget(QLabel("Internal Topology Mode:"))
        self.filter_a_multi_route_combo = QComboBox()
        self.filter_a_multi_route_combo.addItems(["Series (0->1->2)", "Parallel (Summed)", "Split (0//1 -> 2)"])
        routing_row.addWidget(self.filter_a_multi_route_combo)
        routing_row.addStretch()
        a_mult_outer.addLayout(routing_row)

        for m_idx in range(3):
            sub_row = QHBoxLayout()
            c_dial_am = PotentiometerKnob(f"F1.M{m_idx} Freq", self)
            c_dial_am.setRange(40, 18000); c_dial_am.setValue(400 + (m_idx * 200))
            # Multi unit 0 shares Filter A's own mod slot (targets 8/9, since Normal and Multi
            # unit 0 are mutually exclusive -- only one is ever active); unit 1 (middle) is the
            # new Filter A-M2 slot (84/85); unit 2 keeps its existing slot (12/13).
            multi_freq_target = {0: 8, 1: 84, 2: 12}[m_idx]
            multi_res_target = {0: 9, 1: 85, 2: 13}[m_idx]
            c_dial_am.setContextMenuCallback(lambda pos, t=multi_freq_target: self._show_dest_assign_menu(t, pos))
            self.cutoff_dials.append(c_dial_am)

            q_dial_am = PotentiometerKnob(f"F1.M{m_idx} Res", self)
            q_dial_am.setRange(0, 100); q_dial_am.setValue(20)
            q_dial_am.setContextMenuCallback(lambda pos, t=multi_res_target: self._show_dest_assign_menu(t, pos))
            self.q_dials.append(q_dial_am)

            v_pole_am = QComboBox(); v_pole_am.addItems(["2-Pole", "4-Pole"])
            self.vcf_poles.append(v_pole_am)

            v_type_am = QComboBox(); v_type_am.addItems(["Off", "Active (Blend)"])
            v_type_am.setCurrentIndex(1)
            self.vcf_types.append(v_type_am)

            sub_row.addWidget(QLabel(f"Unit {m_idx} Freq:")); sub_row.addWidget(c_dial_am)
            sub_row.addWidget(QLabel("Res:")); sub_row.addWidget(q_dial_am)
            sub_row.addWidget(QLabel("Slope:")); sub_row.addWidget(v_pole_am)
            sub_row.addWidget(QLabel("Type:")); sub_row.addWidget(v_type_am)
            a_mult_outer.addLayout(sub_row)

            # Crossfade mod target mirrors the same per-unit slot the Freq/Res dials above use:
            # unit 0 shares Filter A's own slot (86, since Normal and Multi unit 0 are mutually
            # exclusive), unit 1 (middle) is the new Filter A-M2 slot (89), unit 2 keeps 88.
            am_crossfade_target = {0: 86, 1: 89, 2: 88}[m_idx]
            am_blend_box = _build_filter_blend_row(am_crossfade_target)
            v_type_am.currentIndexChanged.connect(lambda idx, box=am_blend_box: box.setVisible(idx == 1))
            a_mult_outer.addWidget(am_blend_box)

        a_stack_lay.addWidget(self.filter_a_multi_view)
        filter_a_lay.addWidget(self.filter_a_stack_widget)
        filter_a_box.setLayout(filter_a_lay)
        filters_column.addWidget(filter_a_box)

        self.filter_a_mode_combo.currentIndexChanged.connect(
            lambda idx: (self.filter_a_normal_view.setVisible(idx == 0), self.filter_a_multi_view.setVisible(idx == 1))
        )
        self.filter_a_multi_view.setVisible(False)
        # --- FILTER B VIEW PANEL ---
        filter_b_box = QGroupBox("VCF Filter B (Secondary Global Filter Core)")
        filter_b_outer = QVBoxLayout()
        filter_b_outer.setContentsMargins(0, 0, 0, 0); filter_b_outer.setSpacing(2)
        filter_b_lay = QHBoxLayout()
        filter_b_lay.setSpacing(4); filter_b_lay.setContentsMargins(4, 4, 4, 4)

        c_dial_b = PotentiometerKnob("FB Freq", self)
        c_dial_b.setRange(40, 18000); c_dial_b.setValue(4000)
        c_dial_b.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(10, pos))
        self.cutoff_dials.append(c_dial_b)

        q_dial_b = PotentiometerKnob("FB Res", self)
        q_dial_b.setRange(0, 100); q_dial_b.setValue(20)
        q_dial_b.setContextMenuCallback(lambda pos: self._show_dest_assign_menu(11, pos))
        self.q_dials.append(q_dial_b)

        v_pole_b = QComboBox(); v_pole_b.addItems(["2-Pole (12dB/oct)", "4-Pole (24dB/oct)"])
        self.vcf_poles.append(v_pole_b)

        v_type_b = QComboBox(); v_type_b.addItems(["Off", "Active (Blend)"])
        v_type_b.setCurrentIndex(0)  # Off by default to match C default presets cleanly
        self.vcf_types.append(v_type_b)

        filter_b_lay.addWidget(QLabel("Freq:")); filter_b_lay.addWidget(c_dial_b)
        filter_b_lay.addWidget(QLabel("Res:")); filter_b_lay.addWidget(q_dial_b)
        filter_b_lay.addWidget(QLabel("Slope:")); filter_b_lay.addWidget(v_pole_b)
        filter_b_lay.addWidget(QLabel("Type:")); filter_b_lay.addWidget(v_type_b)
        filter_b_outer.addLayout(filter_b_lay)

        b_blend_box = _build_filter_blend_row(87)  # target 87 = Filter B Crossfade
        v_type_b.currentIndexChanged.connect(lambda idx: b_blend_box.setVisible(idx == 1))
        b_blend_box.setVisible(False)  # Filter B defaults to Off -- matches v_type_b's own default index
        filter_b_outer.addWidget(b_blend_box)
        filter_b_box.setLayout(filter_b_outer)
        filters_column.addWidget(filter_b_box)

        # Filter B's Freq/Res/Slope controls (not its own Off/Active combo, which must stay
        # reachable) grey out whenever it's Off -- matches a classic hardware synth's own "Filter
        # B controls will be grayed out" behavior for both Single and 24dB/oct, generalized to
        # whenever Filter B is Off for any reason (not just via the preset selector below).
        def _sync_filter_b_enabled(idx):
            active = (idx == 1)
            c_dial_b.setEnabled(active); q_dial_b.setEnabled(active); v_pole_b.setEnabled(active)
        v_type_b.currentIndexChanged.connect(_sync_filter_b_enabled)
        _sync_filter_b_enabled(v_type_b.currentIndex())

        # Filter Routing preset selector (see its own construction comment above): applies the
        # underlying knob combination for each named preset, and the reverse comparator that
        # keeps the combo honest about "Custom" whenever the knobs drift away from a preset.
        def _apply_filter_routing_preset(idx):
            if idx == 4:  # Custom is never user-applied -- only ever landed on by the comparator
                return
            self.filter_routing_mode.blockSignals(True)
            v_type_b.blockSignals(True)
            self.vcf_poles[0].blockSignals(True)
            if idx == 0:    # Single: Filter A alone, forced 2-pole, Filter B off
                self.filter_routing_mode.setCurrentIndex(0); v_type_b.setCurrentIndex(0); self.vcf_poles[0].setCurrentIndex(0)
            elif idx == 1:  # Serial: both filters, A's output feeds B
                self.filter_routing_mode.setCurrentIndex(0); v_type_b.setCurrentIndex(1); self.vcf_poles[0].setCurrentIndex(0)
            elif idx == 2:  # Parallel: both filters, independent level/pan below
                self.filter_routing_mode.setCurrentIndex(2); v_type_b.setCurrentIndex(1); self.vcf_poles[0].setCurrentIndex(0)
            elif idx == 3:  # 24dB/oct: Filter A alone at 4-pole, Filter B off
                self.filter_routing_mode.setCurrentIndex(0); v_type_b.setCurrentIndex(0); self.vcf_poles[0].setCurrentIndex(1)
            self.filter_routing_mode.blockSignals(False)
            v_type_b.blockSignals(False)
            self.vcf_poles[0].blockSignals(False)
            # Signals were blocked above (to avoid re-entrant preset-sync churn while applying),
            # so the dependent visibility/enabled state each of those combos normally drives on
            # its own currentIndexChanged needs one explicit refresh here.
            self.filter_parallel_box.setVisible(self.filter_routing_mode.currentIndex() == 2)
            b_blend_box.setVisible(v_type_b.currentIndex() == 1)
            _sync_filter_b_enabled(v_type_b.currentIndex())
            self.transmit_knob_packet()

        def _sync_filter_routing_preset_combo():
            mode = self.filter_routing_mode.currentIndex()
            b_active = v_type_b.currentIndex() == 1
            a_4pole = self.vcf_poles[0].currentIndex() == 1
            if not b_active and not a_4pole and mode in (0, 1): preset = 0       # Single
            elif b_active and not a_4pole and mode in (0, 1): preset = 1         # Serial
            elif mode == 2 and not a_4pole: preset = 2                           # Parallel
            elif not b_active and a_4pole and mode in (0, 1): preset = 3         # 24dB/oct
            else: preset = 4                                                     # Custom
            self.filter_routing_preset_combo.blockSignals(True)
            self.filter_routing_preset_combo.setCurrentIndex(preset)
            self.filter_routing_preset_combo.blockSignals(False)

        self.filter_routing_preset_combo.currentIndexChanged.connect(_apply_filter_routing_preset)
        self.filter_routing_mode.currentIndexChanged.connect(lambda idx: _sync_filter_routing_preset_combo())
        v_type_b.currentIndexChanged.connect(lambda idx: _sync_filter_routing_preset_combo())
        self.vcf_poles[0].currentIndexChanged.connect(lambda idx: _sync_filter_routing_preset_combo())
        _sync_filter_routing_preset_combo()

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
            # FIXED: these 6 slots are stale leftovers from the old "3 equal stacked filters"
            # model. The backend's filter overhaul repurposed these exact same numeric targets
            # (8-13) to mean: 8/9 = Filter A's primary filter (filter_a when Normal, or the
            # first internal filter when Filter A is in Multi mode), 10/11 = Filter B, 12/13 =
            # Filter A's 3rd internal filter (Multi mode only -- a no-op otherwise). The old
            # "F1/F2/F3" labels didn't reflect that at all, so picking e.g. "F2 Freq" (which
            # sounds like "the second stacked filter") silently modulated Filter B instead, and
            # "F3 Freq" did nothing unless Filter A happened to be in Multi mode.
            "Filter A Freq", "Filter A Res (Q)", "Filter B Freq", "Filter B Res (Q)",
            "Filter A-M3 Freq (Multi)", "Filter A-M3 Res (Multi)",
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
            "Filter A-M2 Freq (Multi)", "Filter A-M2 Res (Multi)",
            # Targets 86-89: Multi Filter blend Crossfade, one per filter unit (mirrors the same
            # 4-slot grouping filter_cutoff_mods/filter_res_mods already use).
            "Filter A Crossfade", "Filter B Crossfade", "Filter A-M3 Crossfade", "Filter A-M2 Crossfade",
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
        # source, laid out in a compact grid since there are 9 of them (Program + 8 ext-in pairs).
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
            source_combo.setToolTip("Which source (Program or an external-input pair) feeds this slot")
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

        self.routing_ext_in_enable_cbs = []
        self.routing_ext_in_gain_dials = []
        self.routing_output_pair_cbs = []  # [source_idx][pair_idx] -> QCheckBox
        for src in range(SOURCE_COUNT):
            row = src + 1
            self.routing_grid.addWidget(QLabel(SOURCE_NAMES[src]), row, 0)
            if src == 0:
                # Program has no enable/gain of its own here -- it's always live (master_volume
                # lives on the main page); only its output routing is configured on this tab.
                self.routing_ext_in_enable_cbs.append(None)
                self.routing_ext_in_gain_dials.append(None)
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
        self.record_format_wavpack = QRadioButton("WavPack (Recommended)")
        self.record_format_wavpack.setToolTip("Lossless compression (~40-60% smaller than WAV), encoded off the realtime audio thread.")
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

        self.set_mapping_status(None)

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

        def _wire_osc_strip(combo, vis, unison_box, pulse_box, pulse_label, pulse_knob):
            def on_shape_changed(idx):
                vis.set_waveform(idx, is_lfo=False)
                unison_box.setVisible(idx == UNISON_SAW_INDEX)
                pulse_box.setVisible(idx in (PULSE_INDEX, NOISE_INDEX))
                if idx == NOISE_INDEX:
                    pulse_label.setText("Tilt")
                else:
                    pulse_label.setText("Pulse Width")
            combo.currentIndexChanged.connect(on_shape_changed)
            on_shape_changed(combo.currentIndex())

        _wire_osc_strip(self.vco1_combo, self.vco1_vis, self.vco1_unison_box, self.vco1_pulse_box, self.vco1_pulse_label, self.vco1_pulse_width)
        _wire_osc_strip(self.vco2_combo, self.vco2_vis, self.vco2_unison_box, self.vco2_pulse_box, self.vco2_pulse_label, self.vco2_pulse_width)
        _wire_osc_strip(self.vco3_combo, self.vco3_vis, self.vco3_unison_box, self.vco3_pulse_box, self.vco3_pulse_label, self.vco3_pulse_width)
        _wire_osc_strip(self.vco4_combo, self.vco4_vis, self.vco4_unison_box, self.vco4_pulse_box, self.vco4_pulse_label, self.vco4_pulse_width)

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
        and bypass -- so it can be moved wholesale to another slot position by _swap_fx_slots."""
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

    def _swap_fx_slots(self, a, b):
        """Moves slot a's whole configuration to slot b's position and vice versa -- the row
        widgets stay put (so nothing about their signal wiring changes), only the values they
        hold move. Series routing processes slots strictly by array position, so this alone is
        what makes reordering actually change processing order once retransmitted; Parallel
        routing sums all slots so order is a no-op for it either way."""
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
            self.mapping_grid.sample_zones.extend(zones)
            self.mapping_grid.update()
            self.transmit_zone_upsert(zones)
        filename = os.path.basename(path)
        self.set_mapping_status(f"Imported {len(zones)} zone(s) from {filename}" if zones else f"Imported 0 zones from {filename}", error=not zones)
        if warnings:
            QMessageBox.warning(self, "SFZ Import", f"Imported {len(zones)} zone(s) from {filename}, with some issues:\n\n" + "\n".join(f"- {w}" for w in warnings))

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
                # No resampling exists yet (see backend.c's seq_audio_load_wav_from_disk --
                # frame_count/duration are already correct for THIS file's own rate, but nothing
                # currently reads g_seq_audio[] for playback either, so this is purely an early
                # warning: once playback exists, a mismatched-rate clip would play back at the
                # wrong pitch/speed unless resampled first).
                if sample_rate > 0 and int(sample_rate) != ENGINE_SAMPLE_RATE:
                    text += f" ⚠ {int(sample_rate)}Hz ≠ engine {ENGINE_SAMPLE_RATE}Hz, will need resampling"
                widgets["status_label"].setText(text)
                widgets["offset_widget"].setVisible(True)
            elif state == 3:  # SEQ_AUDIO_ERROR
                widgets["status_label"].setText(f"{name}: load failed")
                widgets["offset_widget"].setVisible(False)
            elif state == 1:  # SEQ_AUDIO_LOADING
                widgets["status_label"].setText(f"Loading {name}…")
                widgets["offset_widget"].setVisible(False)

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
        self.transmit_knob_packet()  # freshen self._last_wire_state before capturing it
        fields = {addr: value for addr, value in self._last_wire_state.items() if _is_program_scope_address(addr)}
        prog_dir = os.path.join(self.programs_folder, name)
        try:
            os.makedirs(prog_dir, exist_ok=True)
            data = {"format_version": 1, "name": name, "fields": fields}
            if self.mapping_grid.sample_zones:
                sfz_filename = f"{name}.sfz"
                with open(os.path.join(prog_dir, sfz_filename), "w") as f:
                    f.write(_build_sfz_from_zones(self.mapping_grid.sample_zones))
                data["sfz_file"] = sfz_filename
            with open(os.path.join(prog_dir, f"{name}.hdsprog"), "w") as f:
                json.dump(data, f, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Save Program", f"Could not save '{name}':\n{e}")
            return
        self.program_name_field.setText(name)
        self._refresh_combi_program_combos()

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
        for addr, dial in (("/hds/vco1/vol", self.vco1_vol), ("/hds/vco2/vol", self.vco2_vol),
                           ("/hds/vco3/vol", self.vco3_vol), ("/hds/vco4/vol", self.vco4_vol)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * 100.0))); dial.blockSignals(False)
        for addr, dial in (("/hds/vco1/pan", self.vco1_pan), ("/hds/vco2/pan", self.vco2_pan),
                           ("/hds/vco3/pan", self.vco3_pan), ("/hds/vco4/pan", self.vco4_pan)):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * 100.0))); dial.blockSignals(False)
        for addr, combo in (("/hds/vco1/shape", self.vco1_combo), ("/hds/vco2/shape", self.vco2_combo),
                            ("/hds/vco3/shape", self.vco3_combo), ("/hds/vco4/shape", self.vco4_combo)):
            if addr in fields:
                combo.blockSignals(True); combo.setCurrentIndex(int(round(fields[addr]))); combo.blockSignals(False)

        self.transmit_knob_packet()

    def _apply_program_fields_filters(self, fields):
        """Mirrors transmit_knob_packet()'s own Filter A/B section (incl. the Multi Filter blend
        loop) in reverse -- same cutoff_dials/q_dials/vcf_poles/vcf_types/filter_blend_units
        index convention (0=Filter A Normal, 1-3=Filter A Multi units 0-2, 4=Filter B)."""
        def sd(addr, dial, scale=1.0):
            if addr in fields:
                dial.blockSignals(True); dial.setValue(int(round(fields[addr] * scale))); dial.blockSignals(False)
        def sc(addr, combo):
            if addr in fields:
                combo.blockSignals(True); combo.setCurrentIndex(int(round(fields[addr]))); combo.blockSignals(False)

        sc("/hds/filter/ab_routing_mode", self.filter_routing_mode)
        sd("/hds/filter/a/level", self.filter_a_level_dial)
        sd("/hds/filter/b/level", self.filter_b_level_dial)
        sd("/hds/filter/a/pan", self.filter_a_pan_dial, 100.0)
        sd("/hds/filter/b/pan", self.filter_b_pan_dial, 100.0)
        sc("/hds/filter/a/mode", self.filter_a_mode_combo)

        blend_taps = ["lp", "bp", "hp", "notch", "dry"]
        def apply_blend(addr_prefix, unit_idx):
            mode1_knobs, mode2_knobs, xf_dial, _box = self.filter_blend_units[unit_idx]
            for tap, k in zip(blend_taps, mode1_knobs):
                sd(f"{addr_prefix}/mode1/{tap}", k)
            for tap, k in zip(blend_taps, mode2_knobs):
                sd(f"{addr_prefix}/mode2/{tap}", k)
            sd(f"{addr_prefix}/crossfade", xf_dial)

        sd("/hds/filter/a/cutoff", self.cutoff_dials[0])
        sd("/hds/filter/a/res_q", self.q_dials[0])
        sc("/hds/filter/a/slope", self.vcf_poles[0])
        sc("/hds/filter/a/type", self.vcf_types[0])
        apply_blend("/hds/filter/a", 0)
        sc("/hds/filter/a_multi/routing", self.filter_a_multi_route_combo)

        for m_idx in range(1, 4):
            backend_idx = m_idx - 1
            sd(f"/hds/filter/a_multi/{backend_idx}/cutoff", self.cutoff_dials[m_idx])
            sd(f"/hds/filter/a_multi/{backend_idx}/res_q", self.q_dials[m_idx])
            sc(f"/hds/filter/a_multi/{backend_idx}/slope", self.vcf_poles[m_idx])
            sc(f"/hds/filter/a_multi/{backend_idx}/type", self.vcf_types[m_idx])
            apply_blend(f"/hds/filter/a_multi/{backend_idx}", m_idx)

        sd("/hds/filter/b/cutoff", self.cutoff_dials[4])
        sd("/hds/filter/b/res_q", self.q_dials[4])
        sc("/hds/filter/b/slope", self.vcf_poles[4])
        sc("/hds/filter/b/type", self.vcf_types[4])
        apply_blend("/hds/filter/b", 4)

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
                self.lfo_combos[idx].blockSignals(True)
                self.lfo_combos[idx].setCurrentIndex(int(round(fields[f"/hds/lfo/{idx}/wave"])))
                self.lfo_combos[idx].blockSignals(False)
            if f"/hds/lfo/{idx}/rate" in fields:
                self.lfo_dials[idx].blockSignals(True)
                self.lfo_dials[idx].setValue(int(round(fields[f"/hds/lfo/{idx}/rate"] * 10.0)))
                self.lfo_dials[idx].blockSignals(False)
                self.lfo_readouts[idx].setText(f"{fields[f'/hds/lfo/{idx}/rate']:.1f} Hz")
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

    def _load_program_from_path(self, path):
        """Shared load logic behind both the file-dialog-driven Load button and the </>
        prev/next cycling buttons: apply every included field via _apply_program_fields(), and
        if the file references a colocated .sfz, REPLACE (not merge with) the current Sample
        Bank -- a real, deliberate behavior difference from the SFZ importer's own additive-only
        posture, because loading a *Program* means "this Program's own samples," not "add these
        on top of whatever was already loaded.\""""
        try:
            with open(path) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Load Program", f"Could not read '{path}':\n{e}")
            return
        self._apply_program_fields(data.get("fields", {}))

        sfz_file = data.get("sfz_file")
        if sfz_file:
            sfz_path = os.path.join(os.path.dirname(path), sfz_file)
            if self.mapping_grid.sample_zones:
                old_ids = [z.zone_id for z in self.mapping_grid.sample_zones]
                self.transmit_zone_remove(old_ids)
                self.mapping_grid.sample_zones.clear()
                self.mapping_grid.update()
            if os.path.isfile(sfz_path):
                zones, warnings = _build_zones_from_sfz(sfz_path, self.mapping_grid._next_zone_id)
                if zones:
                    self.mapping_grid._next_zone_id += len(zones)
                    self.mapping_grid.sample_zones.extend(zones)
                    self.mapping_grid.update()
                    self.transmit_zone_upsert(zones)
                if warnings:
                    QMessageBox.warning(self, "Load Program", f"Loaded '{data.get('name', '')}' with some sample issues:\n\n" + "\n".join(f"- {w}" for w in warnings))
            self.set_mapping_status(None)

        self.program_name_field.setText(data.get("name", os.path.splitext(os.path.basename(path))[0]))

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
        """Status line under the Mapping tab's top bar -- shows the live zone count by default
        (no ceiling to report against -- the catalog has no product-facing cap)."""
        if message is None:
            n = len(self.mapping_grid.sample_zones)
            message = f"Sample Bank: {n} zone{'s' if n != 1 else ''}"
            error = False
        self.mapping_status_label.setText(message)
        self.mapping_status_label.setStyleSheet(f"font-size: 10px; color: {'#ff5555' if error else '#888'};")

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
            self.status_sock.bind((OSC_HOST, OSC_RECV_PORT))
        except OSError as e:
            print(f"--- could not bind OSC status port {OSC_RECV_PORT}: {e} ---")
            self.status_sock = None
        self.mock_mode = True  # flips to False the first time a real status bundle arrives
        print(f"OSC client -> {OSC_HOST}:{OSC_SEND_PORT}, status listener on {OSC_RECV_PORT}")

    # Status feedback: 7 /hds/status/* messages (peak, is_recording, elapsed_seconds,
    # record_error, detected_input_channels, detected_output_channels, tempo_bpm_live) sent as
    # one OSC bundle -- see send_osc_status() in backend.c. Each incoming UDP datagram is already
    # one complete OSC packet (bundle or single message), so unlike the old stream-socket read
    # there's no partial-message reassembly to do here at all.
    _STATUS_ADDRESSES = (
        "/hds/status/peak", "/hds/status/is_recording", "/hds/status/elapsed_seconds",
        "/hds/status/record_error", "/hds/status/detected_input_channels",
        "/hds/status/detected_output_channels", "/hds/status/tempo_bpm_live",
        "/hds/status/seq_elapsed_seconds",
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
            for timed_msg in packet.messages:
                msg = timed_msg.message
                if msg.address in self._STATUS_ADDRESSES and msg.params:
                    values[msg.address] = msg.params[0]
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
            if seq_audio_updates:
                self._apply_seq_audio_status(seq_audio_updates)
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
            # Sequencer tab's transport readout -- real, live data (elapsed engine uptime +
            # current tempo) even though the Play/Pause/Record buttons themselves are inert this
            # pass. Bars:Beats is plain presentation-layer math (assumes 4/4), same spirit as the
            # rest of this file's habit of keeping such conversions out of the DSP hot path.
            if "/hds/status/seq_elapsed_seconds" in values and "/hds/status/tempo_bpm_live" in values:
                elapsed = values["/hds/status/seq_elapsed_seconds"]
                bpm = max(1.0, values["/hds/status/tempo_bpm_live"])
                minutes, seconds = divmod(elapsed, 60.0)
                self.seq_elapsed_label.setText(f"{int(minutes):02d}:{seconds:04.1f}")
                beats_total = elapsed * bpm / 60.0
                bar = int(beats_total // 4) + 1
                beat = int(beats_total % 4) + 1
                self.seq_bars_beats_label.setText(f"{bar}:{beat}")

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

        bundle = OscBundleBuilder(IMMEDIATELY)
        def add_f(address, value):
            msg = OscMessageBuilder(address=address)
            msg.add_arg(float(value), "f")
            bundle.add_content(msg.build())
            self._last_wire_state[address] = float(value)  # see __init__'s own comment on this dict
        def add_s(address, value):
            msg = OscMessageBuilder(address=address)
            msg.add_arg(str(value), "s")
            bundle.add_content(msg.build())
            self._last_wire_state[address] = str(value)

        vassign = 1.0 if self.poly_radio.isChecked() else 0.0
        v1_shape = float(self.vco1_combo.currentIndex())
        v2_shape = float(self.vco2_combo.currentIndex())
        fm = float(self.fm_depth_dials["fm_intensity"].value()) / 100.0

        # Raw layout mixing levels
        v1_vol = float(self.vco1_vol.value()) / 100.0
        v2_vol = float(self.vco2_vol.value()) / 100.0
        g_time = float(glide_ms)
        m_vol = float(self.master_vol.value()) / 100.0
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

        # --- Filter A/B ---
        add_f("/hds/filter/ab_routing_mode", self.filter_routing_mode.currentIndex())
        # Independent Parallel-mode level/pan (replaces the old single A/B Blend knob).
        add_f("/hds/filter/a/level", self.filter_a_level_dial.value())
        add_f("/hds/filter/b/level", self.filter_b_level_dial.value())
        add_f("/hds/filter/a/pan", float(self.filter_a_pan_dial.value()) / 100.0)
        add_f("/hds/filter/b/pan", float(self.filter_b_pan_dial.value()) / 100.0)
        add_f("/hds/filter/a/mode", self.filter_a_mode_combo.currentIndex())

        # Multi Filter blend: filter_blend_units[i] is a (mode1_knobs[5], mode2_knobs[5],
        # crossfade_knob, box) tuple in LP/BP/HP/Notch/Dry order, index-aligned with
        # cutoff_dials/q_dials/vcf_poles/vcf_types (0=Filter A Normal, 1-3=Filter A Multi
        # units 0-2, 4=Filter B) -- built alongside those widgets in the Filters tab.
        blend_taps = ["lp", "bp", "hp", "notch", "dry"]
        def add_blend(addr_prefix, unit_idx):
            mode1_knobs, mode2_knobs, xf_dial, _box = self.filter_blend_units[unit_idx]
            for tap, k in zip(blend_taps, mode1_knobs):
                add_f(f"{addr_prefix}/mode1/{tap}", k.value())
            for tap, k in zip(blend_taps, mode2_knobs):
                add_f(f"{addr_prefix}/mode2/{tap}", k.value())
            add_f(f"{addr_prefix}/crossfade", xf_dial.value())

        # Filter A Normal Parameters
        add_f("/hds/filter/a/cutoff", self.cutoff_dials[0].value())
        add_f("/hds/filter/a/res_q", self.q_dials[0].value())
        add_f("/hds/filter/a/slope", self.vcf_poles[0].currentIndex())
        add_f("/hds/filter/a/type", self.vcf_types[0].currentIndex())
        add_blend("/hds/filter/a", 0)
        add_f("/hds/filter/a_multi/routing", self.filter_a_multi_route_combo.currentIndex())

        # Filter A Multi Engine sub-units: Python's cutoff_dials[1..3] are backend's
        # filter_a_multi[0..2] -- index 0 there is Filter A Normal, index 4 is Filter B.
        for m_idx in range(1, 4):
            backend_idx = m_idx - 1
            add_f(f"/hds/filter/a_multi/{backend_idx}/cutoff", self.cutoff_dials[m_idx].value())
            add_f(f"/hds/filter/a_multi/{backend_idx}/res_q", self.q_dials[m_idx].value())
            add_f(f"/hds/filter/a_multi/{backend_idx}/slope", self.vcf_poles[m_idx].currentIndex())
            add_f(f"/hds/filter/a_multi/{backend_idx}/type", self.vcf_types[m_idx].currentIndex())
            add_blend(f"/hds/filter/a_multi/{backend_idx}", m_idx)

        # Filter B Engine Parameters (index 4 in these Python arrays)
        add_f("/hds/filter/b/cutoff", self.cutoff_dials[4].value())
        add_f("/hds/filter/b/res_q", self.q_dials[4].value())
        add_f("/hds/filter/b/slope", self.vcf_poles[4].currentIndex())
        add_f("/hds/filter/b/type", self.vcf_types[4].currentIndex())
        add_blend("/hds/filter/b", 4)

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
            add_f(f"/hds/seq/{i}/midi_channel", 0)  # Omni -- no per-lane MIDI UI yet
            add_f(f"/hds/seq/{i}/mode", mode_combo.currentIndex())
            audio_widgets = self.seq_lane_audio_widgets[i]
            add_s(f"/hds/seq/{i}/audio_path", audio_widgets.get("path", "") or "")
            add_f(f"/hds/seq/{i}/audio_offset_sec", float(audio_widgets["offset_dial"].value()) / 10.0)
        if perf_mode == 1:
            for i, (_row, fader, pan_dial, _type_combo, _mode_combo) in enumerate(self.combi_lane_rows):
                add_f(f"/hds/seq/{i}/lane_type", 0)  # Combi lanes are always Program/MIDI
                add_f(f"/hds/seq/{i}/volume", fader.value())
                add_f(f"/hds/seq/{i}/pan", float(pan_dial.value()) / 100.0)
                add_f(f"/hds/seq/{i}/midi_channel", 0)
                add_f(f"/hds/seq/{i}/mode", 0)  # meaningless in Combi mode

        # LFOs (incl. phase offset + mod source CC)
        for idx in range(LFO_COUNT):
            lfo_hz = float(self.lfo_dials[idx].value()) / 10.0
            self.lfo_readouts[idx].setText(f"{lfo_hz:.1f} Hz")
            lfo_dp = float(self.lfo_depth_dials[idx].value()) / 100.0
            add_f(f"/hds/lfo/{idx}/wave", self.lfo_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/rate", lfo_hz)
            add_f(f"/hds/lfo/{idx}/depth", lfo_dp)
            add_f(f"/hds/lfo/{idx}/key_sync", self.lfo_sync_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/dest", self.lfo_dest_combos[idx].currentIndex())
            add_f(f"/hds/lfo/{idx}/phase_offset", self.lfo_phase_dials[idx].value())
            add_f(f"/hds/lfo/{idx}/mod_source_cc", self.lfo_mod_source_cc[idx])

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

        # Osc 3: vol, pan, mute, pulse-width-or-tilt
        add_f("/hds/vco3/vol", self.vco3_vol.value() / 100.0)
        add_f("/hds/vco3/pan", self.vco3_pan.value() / 100.0)
        add_f("/hds/vco3/mute", 1.0 if self.vco3_mute.isChecked() else 0.0)
        add_f("/hds/vco3/pulse_width", self.vco3_pulse_width.value())

        # Osc 4: vol, pan, mute, pulse-width-or-tilt
        add_f("/hds/vco4/vol", self.vco4_vol.value() / 100.0)
        add_f("/hds/vco4/pan", self.vco4_pan.value() / 100.0)
        add_f("/hds/vco4/mute", 1.0 if self.vco4_mute.isChecked() else 0.0)
        add_f("/hds/vco4/pulse_width", self.vco4_pulse_width.value())

        # External Audio Input: enable + gain per ext-in pair (Routing tab).
        for p in range(EXT_IN_PAIR_COUNT):
            src = p + 1
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
        # between (see compute_current_tempo_bpm() in backend.c). No marker editor UI exists yet,
        # so marker_count is always 0 and the arrays always zero -- reserved for a future editor.
        add_f("/hds/tempo/bpm", self.tempo_dial.value())
        add_f("/hds/tempo/marker_count", 0.0)
        for idx in range(TEMPO_MARKER_MAX):
            add_f(f"/hds/tempo/marker/{idx}/time", 0.0)
        for idx in range(TEMPO_MARKER_MAX):
            add_f(f"/hds/tempo/marker/{idx}/bpm", 0.0)

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

        if self.osc_client is not None:
            try: self.osc_client.send(bundle.build())
            except OSError: pass

    # --- Sample Bank catalog: two dedicated OSC addresses, "/hds/zone/upsert" and
    # "/hds/zone/remove" (see the matching comment block above zone_upsert_handler() in
    # backend.c). Each OSC message carries exactly ONE zone -- a Mapping tab edit touching several
    # zones just sends several messages back to back, unlike the old wire-framed socket's
    # one-message-covers-many-zones batching.
    # zone_id (i) + 17 floats (root_note, min_note, max_note, min_velocity, max_velocity,
    # unpitched, loop_start, loop_end, loop_enabled, round_robin_group, tune_cents, gain_db, pan,
    # start_offset_frac, reverse, loop_release_exit, end_frac) + path (s) -- must match
    # ZoneRecordWire's field order in backend.c exactly.
    _ZONE_UPSERT_TYPES = "i" + "f" * 17 + "s"

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
            zone.file_path,
        )

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
    gui.vco1_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pulse_width.valueChanged.connect(gui.transmit_knob_packet)

    # Oscillator 3 (full peer of Osc 1/2 now, was the dedicated Noise oscillator)
    gui.vco3_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.vco3_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco3_mute.toggled.connect(gui.transmit_knob_packet)
    gui.vco3_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
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
    gui.filter_routing_mode.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.filter_a_level_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.filter_b_level_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.filter_a_pan_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.filter_b_pan_dial.valueChanged.connect(gui.transmit_knob_packet)

    # Active filter configuration bindings
    gui.filter_a_mode_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.filter_a_multi_route_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for dial in gui.cutoff_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)
    for dial in gui.q_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)
    for combo in gui.vcf_poles:
        combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    for combo in gui.vcf_types:
        combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    # Multi Filter blend knobs (Mode 1/Mode 2 taps + Crossfade), one set per filter unit.
    for mode1_knobs, mode2_knobs, xf_dial, _box in gui.filter_blend_units:
        for k in mode1_knobs + mode2_knobs:
            k.valueChanged.connect(gui.transmit_knob_packet)
        xf_dial.valueChanged.connect(gui.transmit_knob_packet)

    # Combi/Sequencer lane scaffolding: fader/pan (both views) + type/seq-mode combos
    # (Sequencer view only -- Combi's rows have None in those two tuple slots).
    for _row, fader, pan_dial, type_combo, mode_combo in gui.combi_lane_rows + gui.seq_lane_rows:
        fader.valueChanged.connect(gui.transmit_knob_packet)
        pan_dial.valueChanged.connect(gui.transmit_knob_packet)
        if type_combo is not None:
            type_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
        if mode_combo is not None:
            mode_combo.currentIndexChanged.connect(gui.transmit_knob_packet)

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
        gui.lfo_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_depth_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_phase_dials[idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.lfo_sync_combos[idx].currentIndexChanged.connect(gui.transmit_knob_packet)
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
    for btn in gui.fx_source_routing_btns:
        btn.toggled.connect(gui.transmit_knob_packet)

    # Routing tab: per-pair ext-in enable/gain + the source x output-pair grid
    for src in range(1, SOURCE_COUNT):
        gui.routing_ext_in_enable_cbs[src].toggled.connect(gui.transmit_knob_packet)
        gui.routing_ext_in_gain_dials[src].valueChanged.connect(gui.transmit_knob_packet)
    for src in range(SOURCE_COUNT):
        for p in range(OUTPUT_PAIR_MAX):
            gui.routing_output_pair_cbs[src][p].toggled.connect(gui.transmit_knob_packet)

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

    # Broadcast early configuration packet on boot
    gui.transmit_knob_packet()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    main_window = SynthGui()
    runtime_connect_and_bind(main_window)
    main_window.show()
    sys.exit(app.exec())
