import sys
import os
import socket
import struct
import math
import random
import subprocess
import re
from datetime import datetime
import numpy as np
from PySide6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QComboBox, QGroupBox, QRadioButton, QTabWidget,
                             QSlider, QPushButton, QProgressBar, QSpinBox, QLineEdit, QCheckBox,
                             QScrollArea, QMenu, QInputDialog, QFileDialog, QGridLayout)
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

# Distinct badge colors for round-robin groups 1, 2, 3, ... (cycles if there are more groups
# than colors -- purely cosmetic, doesn't affect which group a zone actually belongs to).
_ROUND_ROBIN_COLORS = ["#c0392b", "#2980b9", "#27ae60", "#8e44ad", "#d35400", "#16a085"]
def _round_robin_group_color(group):
    return QColor(_ROUND_ROBIN_COLORS[(group - 1) % len(_ROUND_ROBIN_COLORS)])

# Device name byte width must match AUDIO_DEVICE_NAME_LEN in backend.c exactly.
AUDIO_DEVICE_NAME_LEN = 64
# FX bus geometry must match FX_SLOT_COUNT / FX_SLOT_PARAM_COUNT in backend.c exactly.
FX_SLOT_COUNT = 12
FX_SLOT_PARAM_COUNT = 8
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

def _encode_device_name(name):
    """Pack a device name into the fixed AUDIO_DEVICE_NAME_LEN-byte, null-terminated field the
    backend's struct expects. Truncates to fit rather than overflowing the fixed-size C buffer."""
    return name.encode("utf-8", errors="replace")[:AUDIO_DEVICE_NAME_LEN - 1].ljust(AUDIO_DEVICE_NAME_LEN, b"\x00")

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

class SynthGui(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("YourSynth Custom Potentiometer Console")
        self.setGeometry(30, 30, 1350, 680)  # Height reduced from 950 to 680 since window footprint is optimized

        self.server_address = '/tmp/synth.sock'
        self.zone_catalog_server_address = '/tmp/synth_samples.sock'
        self.sock = None
        self.zone_catalog_sock = None
        self.mock_mode = False
        self._status_recv_buf = b""  # accumulates partial reads of the EngineStatus struct below
        self._last_detected_input_channels = -1
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
        self.tab_audio = QWidget()
        self.tab_recording = QWidget()
        self.tab_mapping = QWidget()

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
            _retain_hidden(row_widget)
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

        left_sidebar_scroll = QScrollArea()
        left_sidebar_scroll.setWidgetResizable(True)
        left_sidebar_scroll.setWidget(left_sidebar_content)
        left_sidebar_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        outer_layout.addWidget(left_sidebar_scroll, stretch=1)
        # ----------------------------------------------------
        # PLAY TAB CONTENT (FORMERLY OSCILLATORS)
        # ----------------------------------------------------
        tab_osc_root_lay = QVBoxLayout(self.tab_play)

        osc_mode_row = QHBoxLayout()
        osc_mode_row.setContentsMargins(2, 2, 2, 2)
        osc_mode_row.addWidget(QLabel("<b>Mode:</b>"))
        self.osc_prog_radio = QRadioButton("Prog")
        self.osc_prog_radio.setChecked(True)
        self.osc_combi_radio = QRadioButton("Combi")
        osc_mode_row.addWidget(self.osc_prog_radio)
        osc_mode_row.addWidget(self.osc_combi_radio)
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
        v1_phase_lay.addWidget(self.vco1_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v1_tune_block.addLayout(v1_phase_lay)
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
        v2_phase_lay.addWidget(self.vco2_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v2_tune_block.addLayout(v2_phase_lay)
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
        v3_phase_lay.addWidget(self.vco3_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v3_tune_block.addLayout(v3_phase_lay)
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
        v4_phase_lay.addWidget(self.vco4_phase, alignment=Qt.AlignmentFlag.AlignCenter)
        v4_tune_block.addLayout(v4_phase_lay)
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

        self.btn_sync_amp_eg = QPushButton("1\u21922")
        self.btn_sync_amp_eg.setFixedSize(30, 16)
        self.btn_sync_amp_eg.setStyleSheet("font-size: 8px; font-weight: bold;")
        self.btn_sync_amp_eg.setToolTip("Copy Osc 1 Amp EG settings to Osc 2 Amp EG")
        self.vco1_amp_eg.title_layout.addWidget(self.btn_sync_amp_eg)
        self.btn_sync_amp_eg.clicked.connect(lambda: self.vco2_amp_eg.set_state(self.vco1_amp_eg.get_state()))

        # Osc 2/3/4 have no sync button of their own -- reserve the exact same footprint in their
        # title rows anyway so all four panels stay the same height/alignment instead of only
        # Osc 1's row being taller.
        for panel in (self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg):
            sync_btn_spacer = QWidget()
            sync_btn_spacer.setFixedSize(30, 16)
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

        # Combi mode content placeholder
        self.osc_combi_widget = QWidget()
        combi_lay = QVBoxLayout(self.osc_combi_widget)
        combi_placeholder = QLabel("Combi mode \u2014 program stacking coming soon")
        combi_placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        combi_placeholder.setStyleSheet("color: #777; font-style: italic; padding: 40px;")
        combi_lay.addWidget(combi_placeholder)
        combi_lay.addStretch()
        tab_osc_root_lay.addWidget(self.osc_combi_widget)
        self.osc_combi_widget.setVisible(False)

        self.osc_prog_radio.toggled.connect(self.osc_prog_scroll.setVisible)
        self.osc_combi_radio.toggled.connect(self.osc_combi_widget.setVisible)
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

        routing_header_row = QHBoxLayout()
        routing_header_row.addWidget(QLabel("Signal Mode:"))
        routing_header_row.addStretch()
        routing_lay.addLayout(routing_header_row)

        self.filter_routing_mode = QComboBox()
        self.filter_routing_mode.addItems([
            "Series (mixer -> A -> B)",
            "Series Reversed (mixer -> B -> A)",
            "Parallel (A // B, blend below)",
        ])
        routing_lay.addWidget(self.filter_routing_mode)

        # Blend only matters in Parallel mode -- in either Series mode the chain order alone
        # decides which filter is heard last, so the knob is hidden rather than just disabled.
        self.filter_ab_blend_box = QWidget()
        blend_lay = QHBoxLayout(self.filter_ab_blend_box)
        blend_lay.setContentsMargins(0, 4, 0, 0); blend_lay.setSpacing(6)
        blend_lay.addWidget(QLabel("Blend:"))
        blend_lay.addWidget(QLabel("A"))
        self.filter_ab_blend_dial = PotentiometerKnob("A/B Blend", self)
        self.filter_ab_blend_dial.setRange(0, 100)
        self.filter_ab_blend_dial.setValue(50)
        blend_lay.addWidget(self.filter_ab_blend_dial)
        blend_lay.addWidget(QLabel("B"))
        blend_lay.addStretch()
        routing_lay.addWidget(self.filter_ab_blend_box)
        self.filter_routing_mode.currentIndexChanged.connect(
            lambda idx: self.filter_ab_blend_box.setVisible(idx == 2)
        )
        self.filter_ab_blend_box.setVisible(False)

        routing_box.setLayout(routing_lay)
        filters_column.addWidget(routing_box)

        # Container list definitions for loop compilation connections
        self.cutoff_dials, self.q_dials, self.vcf_poles, self.vcf_types = [], [], [], []

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

        # Filter A Sub-Widget: Normal (Single filter parameters mapped onto cutoff index 0)
        self.filter_a_normal_view = QWidget()
        a_norm_lay = QHBoxLayout(self.filter_a_normal_view)
        a_norm_lay.setContentsMargins(0, 4, 0, 4)

        c_dial_a0 = PotentiometerKnob("FA Freq", self)
        c_dial_a0.setRange(40, 18000); c_dial_a0.setValue(800)
        self.cutoff_dials.append(c_dial_a0)

        q_dial_a0 = PotentiometerKnob("FA Res", self)
        q_dial_a0.setRange(0, 100); q_dial_a0.setValue(20)
        self.q_dials.append(q_dial_a0)

        v_pole_a0 = QComboBox(); v_pole_a0.addItems(["2-Pole (12dB/oct)", "4-Pole (24dB/oct)"])
        self.vcf_poles.append(v_pole_a0)

        v_type_a0 = QComboBox(); v_type_a0.addItems(["None / Off", "Low Pass", "High Pass", "Band Pass"])
        v_type_a0.setCurrentIndex(1)
        self.vcf_types.append(v_type_a0)

        a_norm_lay.addWidget(QLabel("Freq:")); a_norm_lay.addWidget(c_dial_a0)
        a_norm_lay.addWidget(QLabel("Res:")); a_norm_lay.addWidget(q_dial_a0)
        a_norm_lay.addWidget(QLabel("Slope:")); a_norm_lay.addWidget(v_pole_a0)
        a_norm_lay.addWidget(QLabel("Type:")); a_norm_lay.addWidget(v_type_a0)
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
            self.cutoff_dials.append(c_dial_am)

            q_dial_am = PotentiometerKnob(f"F1.M{m_idx} Res", self)
            q_dial_am.setRange(0, 100); q_dial_am.setValue(20)
            self.q_dials.append(q_dial_am)

            v_pole_am = QComboBox(); v_pole_am.addItems(["2-Pole", "4-Pole"])
            self.vcf_poles.append(v_pole_am)

            v_type_am = QComboBox(); v_type_am.addItems(["None / Off", "Low Pass", "High Pass", "Band Pass"])
            v_type_am.setCurrentIndex(1)
            self.vcf_types.append(v_type_am)

            sub_row.addWidget(QLabel(f"Unit {m_idx} Freq:")); sub_row.addWidget(c_dial_am)
            sub_row.addWidget(QLabel("Res:")); sub_row.addWidget(q_dial_am)
            sub_row.addWidget(QLabel("Slope:")); sub_row.addWidget(v_pole_am)
            sub_row.addWidget(QLabel("Type:")); sub_row.addWidget(v_type_am)
            a_mult_outer.addLayout(sub_row)

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
        filter_b_lay = QHBoxLayout()
        filter_b_lay.setSpacing(4); filter_b_lay.setContentsMargins(4, 4, 4, 4)

        c_dial_b = PotentiometerKnob("FB Freq", self)
        c_dial_b.setRange(40, 18000); c_dial_b.setValue(4000)
        self.cutoff_dials.append(c_dial_b)

        q_dial_b = PotentiometerKnob("FB Res", self)
        q_dial_b.setRange(0, 100); q_dial_b.setValue(20)
        self.q_dials.append(q_dial_b)

        v_pole_b = QComboBox(); v_pole_b.addItems(["2-Pole (12dB/oct)", "4-Pole (24dB/oct)"])
        self.vcf_poles.append(v_pole_b)

        v_type_b = QComboBox(); v_type_b.addItems(["None / Off", "Low Pass", "High Pass", "Band Pass"])
        v_type_b.setCurrentIndex(0)  # Off by default to match C default presets cleanly
        self.vcf_types.append(v_type_b)

        filter_b_lay.addWidget(QLabel("Freq:")); filter_b_lay.addWidget(c_dial_b)
        filter_b_lay.addWidget(QLabel("Res:")); filter_b_lay.addWidget(q_dial_b)
        filter_b_lay.addWidget(QLabel("Slope:")); filter_b_lay.addWidget(v_pole_b)
        filter_b_lay.addWidget(QLabel("Type:")); filter_b_lay.addWidget(v_type_b)
        filter_b_box.setLayout(filter_b_lay)
        filters_column.addWidget(filter_b_box)

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
        ]

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

            dial = PotentiometerKnob(f"LFO {x} Rate", self)
            dial.setRange(1, 2000)
            dial.setValue(45)

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
            lfo_idx_for_menu = x - 1
            dp_dial.setContextMenuCallback(lambda pos, i=lfo_idx_for_menu: self._show_mod_source_menu(i, pos))
            self._update_mod_source_tooltip(lfo_idx_for_menu)

            phase_dial = PotentiometerKnob(f"LFO {x} Phase", self)
            phase_dial.setRange(0, 360)
            phase_dial.setValue(0)
            phase_dial.setToolTip("Shifts this LFO's read-phase; modulatable via LFO/EG")
            lfo_row.addWidget(QLabel("Ph:"))
            lfo_row.addWidget(phase_dial)
            self.lfo_phase_dials.append(phase_dial)

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

        self.fx_routing_btn = QPushButton("Series")
        self.fx_routing_btn.setCheckable(True)
        self.fx_routing_btn.setFixedSize(90, 22)
        self.fx_routing_btn.setStyleSheet("font-size: 10px; font-weight: bold;")
        self.fx_routing_btn.setToolTip(
            "Series: mixer -> slot 1 -> slot 2 -> ... -> slot 12 -> output.\n"
            "Parallel: every slot fed the same input independently, all 12 summed and averaged."
        )
        self.fx_routing_btn.toggled.connect(
            lambda checked: self.fx_routing_btn.setText("Parallel" if checked else "Series")
        )
        fx_header_row.addWidget(self.fx_routing_btn)
        fx_outer_lay.addLayout(fx_header_row)

        fx_scroll = QScrollArea()
        fx_scroll.setWidgetResizable(True)
        fx_scroll_content = QWidget()
        fx_scroll_lay = QVBoxLayout(fx_scroll_content)
        fx_scroll_lay.setSpacing(3)
        fx_scroll_lay.setContentsMargins(2, 2, 2, 2)

        self.fx_type_combos, self.fx_mix_dials, self.fx_bypass_buttons = [], [], []
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

        ext_in_box = QGroupBox("External Audio Input")
        ext_in_lay = QHBoxLayout()
        ext_in_lay.setSpacing(8)

        self.ext_in_enable = QCheckBox("Enable")
        ext_in_lay.addWidget(self.ext_in_enable)

        gain_col = QVBoxLayout(); gain_col.setSpacing(0)
        gain_col.addWidget(QLabel("Gain"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.ext_in_gain_dial = PotentiometerKnob("Input Gain", self)
        self.ext_in_gain_dial.setRange(0, 200)
        # Always starts at -inf (silence), never at unity -- an upstream mic pre/interface can be
        # left at any gain, and there must be no chance of a burst hitting the FX bus the instant
        # Enable is checked. The user always has to manually bring this up themselves.
        self.ext_in_gain_dial.setValue(0)
        self.ext_in_gain_dial.value_formatter = lambda v: "-inf dB" if v <= 0 else f"{v}%"
        gain_col.addWidget(self.ext_in_gain_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        ext_in_lay.addLayout(gain_col)

        ext_in_note = QLabel(
            "Routed into the same FX bus as the synth's own voices — passes through all 12 FX slots.\n"
            "Gain always starts at -inf on launch, regardless of what it was last set to — bring it "
            "up manually after Enable, rather than risk a burst from a hot upstream mic pre."
        )
        ext_in_note.setStyleSheet("font-size: 9px; color: #888;")
        ext_in_note.setWordWrap(True)
        ext_in_lay.addWidget(ext_in_note, stretch=1)

        ext_in_box.setLayout(ext_in_lay)
        audio_outer_lay.addWidget(ext_in_box)
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
        self.tab_widget.addTab(self.tab_audio, "Devices")
        self.tab_widget.addTab(self.tab_recording, "Recording")
        self.tab_widget.addTab(self.tab_mapping, "Mapping")

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

    def _show_mod_source_menu(self, idx, global_pos):
        menu = QMenu("Modulation Source", self)
        menu.addSection("Modulation Source")
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
        menu.exec(global_pos)

    def connect_to_backend(self):
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(self.server_address); self.sock.setblocking(False)
            self.mock_mode = False
            print("Successfully linked up to C engine Unix Domain Socket node!")
        except socket.error:
            self.mock_mode = True; self.sock = None
            print("--- WORKING IN MOCK GUI MODE ---")

        # Sample Bank catalog socket: a second, independent connection -- best-effort, same mock-
        # mode convention as the control socket above. Its own outbound-only sender (blocking is
        # fine: these messages are small and only sent on an explicit Mapping tab edit, never on
        # the audio-adjacent poll path the control socket's status reads use).
        try:
            self.zone_catalog_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.zone_catalog_sock.connect(self.zone_catalog_server_address)
        except socket.error:
            self.zone_catalog_sock = None

    # EngineStatus wire format: 5 floats (peak, is_recording, elapsed_seconds, record_error,
    # detected_input_channels) -- must match the EngineStatus struct backend.c sends back over
    # the same socket the control packet goes out on. Used to be a bare 4-byte peak float; grew
    # into this once the Recording tab needed live state back from the engine (is a take
    # actually running, how long, did the last start attempt fail, how many channels does the
    # current input device actually have).
    _STATUS_FORMAT = "6f"
    _STATUS_SIZE = struct.calcsize(_STATUS_FORMAT)

    def listen_for_status_frames(self):
        if self.sock is None or self.mock_mode:
            return
        try:
            # Drain whatever's available -- a stream socket can deliver less (or, coalesced,
            # more) than one struct per recv(), so accumulate across calls like the backend's
            # own poll_control_socket() does for the control packet, rather than assuming a
            # single recv() lines up with a single struct.
            chunk = self.sock.recv(4096)
            if chunk:
                self._status_recv_buf += chunk
        except (socket.error, BlockingIOError):
            pass
        while len(self._status_recv_buf) >= self._STATUS_SIZE:
            peak, is_recording, elapsed_seconds, record_error, detected_channels, tempo_bpm_live = struct.unpack(
                self._STATUS_FORMAT, self._status_recv_buf[:self._STATUS_SIZE]
            )
            self._status_recv_buf = self._status_recv_buf[self._STATUS_SIZE:]
            self.vu_meter.setValue(int(max(0, min(100, peak * 100.0))))
            self._update_recording_status(bool(is_recording), elapsed_seconds, bool(record_error), int(detected_channels))
            self.tempo_live_readout.setText(f"engine: {tempo_bpm_live:.1f} BPM")
    def transmit_knob_packet(self):
        glide_ms = self.glide_dial.value()
        self.glide_readout.setText(f"{glide_ms} ms")
        glide_random_pct = self.glide_random_dial.value()
        self.glide_random_readout.setText(f"{glide_random_pct}%")
        self.tempo_readout.setText(f"{self.tempo_dial.value()} BPM")

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

        # Baseline header list: Maps 13 core parameter elements exactly
        packet_list = [vassign, v1_shape, v2_shape, fm, v1_vol, v2_vol, g_time, m_vol, v1_p, v2_p, v1_fine, v2_pitch, v2_fine]

        # Pack 8 parameters (D, H, A, Dec, S, R, Attack Start, Attack Peak) for all 10 envelope
        # instances (+80 items). Order matches C struct exactly: Osc 1-4 Amp, then EG 1-6.
        all_panels = [self.vco1_amp_eg, self.vco2_amp_eg, self.vco3_amp_eg, self.vco4_amp_eg] + self.envelopes
        for panel in all_panels:
            packet_list.append(float(panel.dials["D"].value()))
            packet_list.append(float(panel.dials["H"].value()))
            packet_list.append(float(panel.dials["A"].value()))
            packet_list.append(float(panel.dials["D_phase"].value()))
            packet_list.append(float(panel.dials["S"].value()))
            packet_list.append(float(panel.dials["R"].value()))
            packet_list.append(float(panel.attack_level_pct))
            packet_list.append(float(panel.attack_peak_pct))

        # --- Overhauled Filter Section Pack Strategy ---
        # Add Filter A/B outer routing mode (+1 item) and its Parallel-mode blend (+1 item)
        packet_list.append(float(self.filter_routing_mode.currentIndex()))
        packet_list.append(float(self.filter_ab_blend_dial.value()))

        # Add Filter A engine mode (+1 item)
        packet_list.append(float(self.filter_a_mode_combo.currentIndex()))

        # Filter A Normal Parameters (+4 items)
        packet_list.append(float(self.cutoff_dials[0].value()))
        packet_list.append(float(self.q_dials[0].value()))
        packet_list.append(float(self.vcf_poles[0].currentIndex()))
        packet_list.append(float(self.vcf_types[0].currentIndex()))
        # Filter A Multi internal topology routing (+1 item)
        packet_list.append(float(self.filter_a_multi_route_combo.currentIndex()))

        # Filter A Multi Engine sub-units 0, 1, and 2 (+12 items)
        for m_idx in range(1, 4):
            packet_list.append(float(self.cutoff_dials[m_idx].value()))
            packet_list.append(float(self.q_dials[m_idx].value()))
            packet_list.append(float(self.vcf_poles[m_idx].currentIndex()))
            packet_list.append(float(self.vcf_types[m_idx].currentIndex()))

        # Filter B Engine Parameters (+4 items)
        # Mapped onto the end of the lists (index 4)
        packet_list.append(float(self.cutoff_dials[4].value()))
        packet_list.append(float(self.q_dials[4].value()))
        packet_list.append(float(self.vcf_poles[4].currentIndex()))
        packet_list.append(float(self.vcf_types[4].currentIndex()))
        # Add active LFO configurations (+112 items, incl. phase offset + mod source CC)
        for idx in range(LFO_COUNT):
            lfo_hz = float(self.lfo_dials[idx].value()) / 10.0
            self.lfo_readouts[idx].setText(f"{lfo_hz:.1f} Hz")
            lfo_dp = float(self.lfo_depth_dials[idx].value()) / 100.0
            packet_list.append(float(self.lfo_combos[idx].currentIndex()))
            packet_list.append(lfo_hz)
            packet_list.append(lfo_dp)
            packet_list.append(float(self.lfo_sync_combos[idx].currentIndex()))
            packet_list.append(float(self.lfo_dest_combos[idx].currentIndex()))
            packet_list.append(float(self.lfo_phase_dials[idx].value()))
            packet_list.append(float(self.lfo_mod_source_cc[idx]))
        # Append the 6 custom EG intensity multipliers (+6 items)
        for idx in range(6):
            packet_list.append(float(self.eg_intensity_dials[idx].value()) / 100.0)

        # Append each assignable EG's own mod destination (+6 items)
        for idx in range(6):
            packet_list.append(float(self.eg_dest_combos[idx].currentIndex()))

        # Append the 10 active envelope mode selection indices (+10 items).
        # Sequence matches C struct exactly: Osc 1-4 Amp, then EG 1-6.
        packet_list.append(float(self.vco1_amp_eg.combo_type.currentIndex()))
        packet_list.append(float(self.vco2_amp_eg.combo_type.currentIndex()))
        packet_list.append(float(self.vco3_amp_eg.combo_type.currentIndex()))
        packet_list.append(float(self.vco4_amp_eg.combo_type.currentIndex()))
        for env in self.envelopes:
            packet_list.append(float(env.combo_type.currentIndex()))
        # Append the 4 Unison Saw shape controls (+4 items)
        packet_list.append(float(self.vco1_unison_detune.value()))
        packet_list.append(float(self.vco1_unison_width.value()))
        packet_list.append(float(self.vco2_unison_detune.value()))
        packet_list.append(float(self.vco2_unison_width.value()))

        # Mono Legato glide time randomization amount (+1 item)
        packet_list.append(float(self.glide_random_dial.value()))

        # Mute flags, decoupled from FM (+2 items)
        packet_list.append(1.0 if self.vco1_mute.isChecked() else 0.0)
        packet_list.append(1.0 if self.vco2_mute.isChecked() else 0.0)

        # Hard Sync mode: 0=Off, 1=Parallel, 2=Serial (+1 item)
        packet_list.append(float(self.hard_sync_combo.currentIndex()))

        # Phase pots, degrees 0-360 (+2 items)
        packet_list.append(float(self.vco1_phase.value()))
        packet_list.append(float(self.vco2_phase.value()))

        # --- 4-Oscillator architecture additions (+10 items) ---
        # Pulse width, Osc 1/2 (+2 items)
        packet_list.append(float(self.vco1_pulse_width.value()))
        packet_list.append(float(self.vco2_pulse_width.value()))

        # Osc 3: vol, pan, mute, pulse-width-or-tilt (+4 items)
        packet_list.append(float(self.vco3_vol.value()) / 100.0)
        packet_list.append(float(self.vco3_pan.value()) / 100.0)
        packet_list.append(1.0 if self.vco3_mute.isChecked() else 0.0)
        packet_list.append(float(self.vco3_pulse_width.value()))

        # Osc 4: vol, pan, mute, pulse-width-or-tilt (+4 items)
        packet_list.append(float(self.vco4_vol.value()) / 100.0)
        packet_list.append(float(self.vco4_pan.value()) / 100.0)
        packet_list.append(1.0 if self.vco4_mute.isChecked() else 0.0)
        packet_list.append(float(self.vco4_pulse_width.value()))

        # --- FX Bus + External Input architecture additions ---
        # This is the 151f contract's first breaking change since the 4-oscillator overhaul --
        # everything above this point is untouched, everything below is new. Order here MUST
        # match SynthSharedControls in backend.c field-for-field: ext_in_enable/gain, then
        # FX_SLOT_COUNT slots (each fx_type, FX_SLOT_PARAM_COUNT params, mix, bypass), then the
        # two fixed-width device name strings, packed last since struct.pack keeps fixed-size
        # string fields at the end of the format string.

        # External Audio Input: enable, gain (+2 items)
        packet_list.append(1.0 if self.ext_in_enable.isChecked() else 0.0)
        packet_list.append(float(self.ext_in_gain_dial.value()) / 100.0)

        # FX Bus: 12 slots * (1 type + 8 params + 1 mix + 1 bypass) = 132 items. A plain dial
        # always sends value/100.0 -- a 0-100-range dial (per FX_TYPE_METADATA) reaches the
        # backend as 0.0-1.0, a -100..100 one as -1.0..1.0. A "choice" param instead sends
        # (selected index / (option count - 1)) -- see FX_TYPE_METADATA's own comment on that
        # convention. Mix itself stays 0-100 raw (backend divides it by 100 on its own side,
        # matching how master_volume-style knobs are packed elsewhere in this function).
        for slot_idx in range(FX_SLOT_COUNT):
            type_idx = self.fx_type_combos[slot_idx].currentIndex()
            packet_list.append(float(type_idx))
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
                packet_list.append(value)
            packet_list.append(float(self.fx_mix_dials[slot_idx].value()))
            packet_list.append(1.0 if self.fx_bypass_buttons[slot_idx].isChecked() else 0.0)

        # FX Bus routing: 0 = Series, 1 = Parallel (+1 item)
        packet_list.append(1.0 if self.fx_routing_btn.isChecked() else 0.0)

        # Pitch Bend range, up/down, semitones (+2 items)
        packet_list.append(float(self.pb_range_up_spin.value()))
        packet_list.append(float(self.pb_range_down_spin.value()))

        # Sustain pedal polarity: 0 = Normal, 1 = Inverted (+1 item)
        packet_list.append(1.0 if self.sustain_polarity_inverted.isChecked() else 0.0)

        # Audio latency, ms (+1 item) -- friendly preset dropdown, see audio_latency_ms in backend.c
        latency_idx = self.audio_latency_combo.currentIndex()
        latency_ms = self._LATENCY_PRESETS[latency_idx][1] if 0 <= latency_idx < len(self._LATENCY_PRESETS) else 25.0
        packet_list.append(float(latency_ms))

        # Live polyphony ceiling (+1 item) -- see active_voice_limit in backend.c
        packet_list.append(float(self.polyphony_spin.value()))

        # Live render block size, samples (+1 item) -- see block_size_frames in backend.c
        bs_idx = self.block_size_combo.currentIndex()
        block_size = self._BLOCK_SIZE_PRESETS[bs_idx] if 0 <= bs_idx < len(self._BLOCK_SIZE_PRESETS) else 128
        packet_list.append(float(block_size))

        # Recording (+19 items: enable, include-synth-output, downmix, 16 per-channel enables)
        packet_list.append(1.0 if self.record_toggle_btn.isChecked() else 0.0)
        packet_list.append(1.0 if self.record_synth_output_cb.isChecked() else 0.0)
        packet_list.append(1.0 if self.record_mode_downmix.isChecked() else 0.0)
        for idx in range(RECORD_MAX_CHANNELS):
            checked = idx < len(self.record_input_channel_cbs) and self.record_input_channel_cbs[idx].isChecked()
            packet_list.append(1.0 if checked else 0.0)

        # Tempo (+34 items): the Tempo knob (tempo_bpm) plus a programmable tempo-map system in
        # the backend -- tempo_marker_count + up to TEMPO_MARKER_MAX (time, bpm) marker pairs,
        # interpolated between (see compute_current_tempo_bpm() in backend.c). No marker editor
        # UI exists yet, so marker_count is always 0 and the arrays always zero -- reserved in
        # the wire layout for a future editor rather than requiring another breaking change then.
        packet_list.append(float(self.tempo_dial.value()))
        packet_list.append(0.0)  # tempo_marker_count
        packet_list.extend([0.0] * TEMPO_MARKER_MAX)  # tempo_marker_time[]
        packet_list.extend([0.0] * TEMPO_MARKER_MAX)  # tempo_marker_bpm[]

        # Sample Bank: NOT part of this packet -- the Mapping tab's zone catalog travels over its
        # own dedicated, event-driven socket instead (see transmit_zone_upsert()/
        # transmit_zone_remove()), sent only when the catalog itself actually changes, not
        # broadcast in full on every unrelated knob turn like everything above. See
        # SAMPLE_SLOT_CAPACITY's comment in backend.c for why.

        # --- Osc 3 identity/tuning (+6 items): shape, fine tune, pitch offset, phase offset,
        # unison detune/width. Must come after the Sample Bank comment above and before Osc 4's
        # identical block, matching backend.c's SynthSharedControls field order exactly. ---
        packet_list.append(float(self.vco3_combo.currentIndex()))
        packet_list.append(float(self.vco3_fine_tune.value()))
        packet_list.append(float(self.vco3_pitch_offset.value()))
        packet_list.append(float(self.vco3_phase.value()))
        packet_list.append(float(self.vco3_unison_detune.value()))
        packet_list.append(float(self.vco3_unison_width.value()))

        # --- Osc 4 identity/tuning (+6 items), mirrors Osc 3's block exactly. ---
        packet_list.append(float(self.vco4_combo.currentIndex()))
        packet_list.append(float(self.vco4_fine_tune.value()))
        packet_list.append(float(self.vco4_pitch_offset.value()))
        packet_list.append(float(self.vco4_phase.value()))
        packet_list.append(float(self.vco4_unison_detune.value()))
        packet_list.append(float(self.vco4_unison_width.value()))

        # --- FM Algorithm system (+8 items): algorithm index, then every depth field in the
        # exact order backend.c declares them. fm_intensity itself was already sent in the
        # baseline header (position 4, the "2->1" depth, unchanged) -- these are only the NEW
        # depth fields plus the algorithm selector. ---
        packet_list.append(float(self.fm_algorithm_combo.currentIndex()))
        packet_list.append(float(self.fm_depth_dials["fm_depth_3_1"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_3_2"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_4_1"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_4_3"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_1_2"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_3_4"].value()) / 100.0)
        packet_list.append(float(self.fm_depth_dials["fm_depth_1_3"].value()) / 100.0)

        # Recording format: 0=WAV, 1=WavPack (+1 item)
        packet_list.append(1.0 if self.record_format_wavpack.isChecked() else 0.0)

        # UNIFIED CONTRACT VERIFICATION -- one term per packet_list.append block above, in order:
        # 13 (baseline) + 80 (10 EGs * 8 params) + 1 (filter_ab_routing_mode) + 1 (filter_ab_blend)
        # + 1 (filter_a_mode) + 4 (filter_a) + 1 (filter_a_multi_routing) + 12 (filter_a_multi[3] * 4)
        # + 4 (filter_b) + 112 (16 LFOs * 7, incl. phase offset + mod_source_cc)
        # + 6 (eg_intensity) + 6 (eg_dest_target) + 10 (eg_mode)
        # + 4 (unison saw width/detune, vco1/vco2 only) + 1 (mono glide randomization)
        # + 2 (vco1/vco2 mute) + 1 (hard sync mode) + 2 (vco1/vco2 phase offset)
        # + 2 (vco1/vco2 pulse width) + 4 (Osc 3: vol/pan/mute/pulse-or-tilt) + 4 (Osc 4: vol/pan/mute/pulse-or-tilt)
        # + 2 (ext in enable/gain) + 132 (12 FX slots * 11) + 1 (fx_routing_mode) + 2 (pb range)
        # + 1 (sustain polarity) + 1 (audio latency) + 1 (polyphony) + 1 (block size)
        # + 19 (recording: enable/synth-output/downmix + 16 channel enables)
        # + 34 (tempo: tempo_bpm + tempo_marker_count + 16 marker times + 16 marker bpms)
        # + 6 (Osc 3 identity) + 6 (Osc 4 identity) + 8 (FM algorithm + 7 new depths)
        # + 1 (record_format) = 486 floats, followed by 4 fixed strings (not floats):
        # output_device, input_device, midi_device (64 bytes each), record_path (256 bytes -- a
        # real filesystem path needs more room than an ALSA device name). The Sample Bank's own
        # file paths travel over the separate catalog socket instead (see transmit_zone_upsert()).
        midi_selection = self.midi_device_combo.currentText()
        midi_wire_value = "auto" if midi_selection == MIDI_AUTO_LABEL else midi_selection
        binary_packet = struct.pack(
            f"486f{AUDIO_DEVICE_NAME_LEN}s{AUDIO_DEVICE_NAME_LEN}s{AUDIO_DEVICE_NAME_LEN}s{RECORD_PATH_LEN}s",
            *packet_list,
            _encode_device_name(self.output_device_combo.currentText()),
            _encode_device_name(self.input_device_combo.currentText()),
            _encode_device_name(midi_wire_value),
            self._current_record_path.encode("utf-8")[:RECORD_PATH_LEN - 1],
        )
        if self.sock is not None:
            try: self.sock.sendall(binary_packet)
            except socket.error: self.sock = None

    # --- Sample Bank catalog socket: a second, dedicated, event-driven Unix socket carrying only
    # ZONE_OP_UPSERT/ZONE_OP_REMOVE messages (see the matching comment block above
    # process_zone_catalog_message() in backend.c). Wire shape: a 5-byte header (1-byte opcode,
    # 4-byte record count, host byte order) followed by `count` fixed-size records -- a
    # ZoneRecordWire per zone for UPSERT, a plain int32 zone_id for REMOVE.
    _ZONE_OP_UPSERT = 1
    _ZONE_OP_REMOVE = 2
    # zone_id (i) + 10 floats (root_note, min_note, max_note, min_velocity, max_velocity,
    # unpitched, loop_start, loop_end, loop_enabled, round_robin_group) + path (SAMPLE_PATH_LEN
    # bytes) -- must match ZoneRecordWire/ZONE_RECORD_WIRE_SIZE in backend.c field-for-field.
    _ZONE_RECORD_FORMAT = f"=i15f{SAMPLE_PATH_LEN}s"  # "=" (no padding) since backend.c parses this field-by-field, not via a matching C struct memcpy

    def _zone_to_wire_record(self, zone):
        is_drum = (zone.key_center == -1)
        root_note = 60.0 if is_drum else float(zone.key_center)  # DRUM sends 60 as a placeholder --
        # sample_slot_unpitched forces playback rate to 1.0 regardless of this value.
        return struct.pack(
            self._ZONE_RECORD_FORMAT,
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
            zone.file_path.encode("utf-8")[:SAMPLE_PATH_LEN - 1].ljust(SAMPLE_PATH_LEN, b"\x00"),
        )

    def transmit_zone_upsert(self, zones):
        """Sends one ZONE_OP_UPSERT message covering exactly the given zones (never the whole
        catalog) -- called from every Mapping tab edit site: drop, drag/resize, delete's sibling
        add, loop-marker drag, root-key change, round-robin-group change."""
        if self.zone_catalog_sock is None or not zones:
            return
        body = b"".join(self._zone_to_wire_record(z) for z in zones)
        header = struct.pack("=Bi", self._ZONE_OP_UPSERT, len(zones))
        try:
            self.zone_catalog_sock.sendall(header + body)
        except socket.error:
            self.zone_catalog_sock = None

    def transmit_zone_remove(self, zone_ids):
        """Sends one ZONE_OP_REMOVE message covering exactly the given zone_ids."""
        if self.zone_catalog_sock is None or not zone_ids:
            return
        body = b"".join(struct.pack("=i", zid) for zid in zone_ids)
        header = struct.pack("=Bi", self._ZONE_OP_REMOVE, len(zone_ids))
        try:
            self.zone_catalog_sock.sendall(header + body)
        except socket.error:
            self.zone_catalog_sock = None

    def closeEvent(self, event):
        if self.sock is not None: self.sock.close()
        if self.zone_catalog_sock is not None: self.zone_catalog_sock.close()
        event.accept()
def runtime_connect_and_bind(gui):
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
    gui.filter_ab_blend_dial.valueChanged.connect(gui.transmit_knob_packet)

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
        for p_dial in gui.fx_param_dials[slot_idx]:
            p_dial.valueChanged.connect(gui.transmit_knob_packet)
        for p_combo in gui.fx_param_combos[slot_idx]:
            p_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
        gui.fx_mix_dials[slot_idx].valueChanged.connect(gui.transmit_knob_packet)
        gui.fx_bypass_buttons[slot_idx].toggled.connect(gui.transmit_knob_packet)
    gui.fx_routing_btn.toggled.connect(gui.transmit_knob_packet)

    # External Audio Input + Audio device selection
    gui.ext_in_enable.toggled.connect(gui.transmit_knob_packet)
    gui.ext_in_gain_dial.valueChanged.connect(gui.transmit_knob_packet)
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
