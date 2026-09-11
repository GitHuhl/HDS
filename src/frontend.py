import sys
import socket
import struct
import math
import random
import subprocess
from PyQt6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QLabel, QComboBox, QGroupBox, QRadioButton, QTabWidget,
                             QSlider, QPushButton, QProgressBar, QSpinBox, QLineEdit, QCheckBox,
                             QScrollArea)
from PyQt6.QtGui import QPainter, QPainterPath, QColor, QPen, QBrush, QWheelEvent, QCursor
from PyQt6.QtCore import Qt, QTimer, QPointF, pyqtSignal

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

# Device name byte width must match AUDIO_DEVICE_NAME_LEN in backend.c exactly.
AUDIO_DEVICE_NAME_LEN = 64
# FX bus geometry must match FX_SLOT_COUNT / FX_SLOT_PARAM_COUNT in backend.c exactly.
FX_SLOT_COUNT = 12
FX_SLOT_PARAM_COUNT = 8

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
]

def _list_alsa_devices(list_cmd):
    """Best-effort enumeration of ALSA PCM device names via `aplay -L` / `arecord -L`. Falls
    back to just ["default"] if the tool isn't installed, times out, or returns nothing -- the
    combo box is still editable text either way, so "default" is always a safe starting point."""
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

        painter.drawPath(path)
class PotentiometerKnob(QWidget):
    """Custom vector-drawn potentiometer style dial with drag, scrollwheel, click reset,
    a high-visibility pop-up readout, and a 500ms hover delay trigger to view values passively."""
    valueChanged = pyqtSignal(int)

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

        self._fade_timer = QTimer(self)
        self._fade_timer.setSingleShot(True)
        self._fade_timer.timeout.connect(self._hide_popup)

        self._hover_timer = QTimer(self)
        self._hover_timer.setSingleShot(True)
        self._hover_timer.timeout.connect(self._show_popup)

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
            self._right_press_pos = None
            self._right_dragged = False
            self._drag_button = None
            self._restore_cursor()
            self._hide_popup()
            event.accept()

    def wheelEvent(self, event: QWheelEvent):
        self._hover_timer.stop()
        degrees = event.angleDelta().y() / 8
        steps = degrees / 15
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
        from PyQt6.QtWidgets import QToolTip
        global_pos = self.mapToGlobal(QPointF(self.width() / 2, -15).toPoint())
        display_value = self.value_formatter(self._current_value) if self.value_formatter else str(self._current_value)
        QToolTip.showText(global_pos, f"{self._label_hint}: {display_value}", self)

    def _hide_popup(self):
        from PyQt6.QtWidgets import QToolTip
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
    valuesChanged = pyqtSignal(int, int, int, int, int, int, int, int)

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
            self.delay = max(0.005, (clamped_x - anchor_x) / scale_x)
        elif tag == "attack":
            self.attack = max(0.005, (clamped_x - anchor_x) / scale_x)
            self.attack_peak = level_frac
        elif tag == "hold":
            self.hold = max(0.005, (clamped_x - anchor_x) / scale_x)
        elif tag == "decay":
            self.decay = max(0.005, (clamped_x - anchor_x) / scale_x)
            self.sustain = level_frac
        elif tag == "sustain":
            self.sustain = level_frac
        elif tag == "release":
            self.release = max(0.005, (w - margin - clamped_x) / scale_x)

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
    valuesChanged = pyqtSignal(int, int, int, int, int, int, int, int)
    typeChanged = pyqtSignal(int)

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
        self.columns["A"], self.dials["A"], self.a_readout = self._make_dial_column("A", attack_range_ms, 10, " ms")
        self.columns["D_phase"], self.dials["D_phase"], self.d_readout = self._make_dial_column("Dec", decay_range_ms, 150, " ms")
        self.columns["S"], self.dials["S"], self.s_readout = self._make_dial_column("S", (0, 100), 80, "%")
        self.columns["R"], self.dials["R"], self.r_readout = self._make_dial_column("R", release_range_ms, 300, " ms")

        outer.addLayout(self.get_dial_row())
        self.setLayout(outer)

        for dial in self.dials.values():
            dial.valueChanged.connect(self._on_dial_moved)
        self.graph.valuesChanged.connect(self._on_graph_dragged)
        self.combo_type.currentIndexChanged.connect(self._on_type_dropdown_swapped)

        self._on_type_dropdown_swapped(0)

    def get_dial_row(self):
        return self.dial_row

    def _make_dial_column(self, label_text, value_range, default_value, suffix):
        box_container = QWidget()
        box = QVBoxLayout(box_container); box.setContentsMargins(0, 0, 0, 0); box.setSpacing(0)
        lbl = QLabel(label_text); lbl.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl.setStyleSheet("font-size: 9px; font-weight: bold;")
        box.addWidget(lbl)

        dial = PotentiometerKnob(f"{label_text}", self)
        dial.setRange(*value_range); dial.setValue(default_value)
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
class SynthGui(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("YourSynth Custom Potentiometer Console")
        self.setGeometry(30, 30, 1350, 680)  # Height reduced from 950 to 680 since window footprint is optimized

        self.server_address = '/tmp/synth.sock'
        self.sock = None
        self.mock_mode = False
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
        self.tab_global = QWidget()
        self.tab_play = QWidget()  # Renamed from tab_oscillators to tab_play
        self.tab_filters = QWidget()
        self.tab_lfos = QWidget()
        self.tab_egs = QWidget()
        self.tab_fx = QWidget()
        self.tab_audio = QWidget()

        # ----------------------------------------------------
        # INDEPENDENT COMPONENT: GENERAL SYSTEM SHELL
        # ----------------------------------------------------
        global_outer_lay = QVBoxLayout(self.tab_global)
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

        self.hard_sync_checkbox = QCheckBox("Hard Sync")
        self.hard_sync_checkbox.setToolTip("Osc 2 resets to phase 0 every time Osc 1 completes a cycle")
        global_layout.addWidget(self.hard_sync_checkbox)
        global_layout.addSpacing(4)

        global_layout.addWidget(QLabel("FM Depth"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.fm_dial = PotentiometerKnob("FM Depth", self)
        self.fm_dial.setRange(0, 200); self.fm_dial.setValue(0)
        global_layout.addWidget(self.fm_dial, alignment=Qt.AlignmentFlag.AlignCenter)

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

        routing_box = QGroupBox("Filter Routing Matrix")
        routing_lay = QVBoxLayout(); routing_lay.setSpacing(2); routing_lay.setContentsMargins(4, 4, 4, 4)

        header_row = QHBoxLayout()
        header_row.addWidget(QLabel("Signal Mode:"))
        header_row.addStretch()
        routing_lay.addLayout(header_row)

        self.filter_routing_mode = QComboBox()
        self.filter_routing_mode.addItems(["Series (mixer -> A -> B)", "Parallel (A // B summed 50/50)"])
        routing_lay.addWidget(self.filter_routing_mode)
        routing_box.setLayout(routing_lay)
        global_layout.addWidget(routing_box)
        global_layout.addStretch()

        global_group.setLayout(global_layout)
        global_outer_lay.addWidget(global_group)

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

        outer_layout.addWidget(master_strip_widget, stretch=1)
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
        self.vco1_combo.addItems(["Saw", "Square", "Triangle", "Sine", "Unison Saw", "Noise", "Pulse"])
        v1_strip.addWidget(self.vco1_combo)

        # Pulse Width is a direct sub-parameter of the Shape choice (only meaningful when Shape
        # is Pulse) -- kept right under Shape, not down by Volume, so it reads as part of "what
        # this oscillator's wave looks like" rather than a mixing control.
        self.vco1_pulse_box = QWidget()
        v1_pw_lay = QVBoxLayout(self.vco1_pulse_box)
        v1_pw_lay.setContentsMargins(0, 2, 0, 0)
        v1_pw_lay.addWidget(QLabel("Pulse Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco1_pulse_width = PotentiometerKnob("O1 Pulse Width", self)
        self.vco1_pulse_width.setRange(2, 98)
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
        self.vco2_combo.addItems(["Saw", "Square", "Triangle", "Sine", "Unison Saw", "Noise", "Pulse"])
        self.vco2_combo.setCurrentIndex(3)
        v2_strip.addWidget(self.vco2_combo)

        # Pulse Width is a direct sub-parameter of the Shape choice (only meaningful when Shape
        # is Pulse) -- kept right under Shape, not down by Volume, so it reads as part of "what
        # this oscillator's wave looks like" rather than a mixing control.
        self.vco2_pulse_box = QWidget()
        v2_pw_lay = QVBoxLayout(self.vco2_pulse_box)
        v2_pw_lay.setContentsMargins(0, 2, 0, 0)
        v2_pw_lay.addWidget(QLabel("Pulse Width"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.vco2_pulse_width = PotentiometerKnob("O2 Pulse Width", self)
        self.vco2_pulse_width.setRange(2, 98)
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

        # Noise Strip Configuration
        noise_strip = QVBoxLayout()
        noise_strip.setSpacing(4)
        noise_strip.addWidget(QLabel("<b>Noise</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.noise_vis = WaveformVisualizer()
        self.noise_vis.set_waveform(5, is_lfo=False)
        noise_strip.addWidget(self.noise_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        noise_tilt_lay = QVBoxLayout()
        noise_tilt_lay.addWidget(QLabel("Tilt"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.noise_tilt = PotentiometerKnob("Noise Tilt", self)
        self.noise_tilt.setRange(-100, 100)
        self.noise_tilt.setValue(0)
        noise_tilt_lay.addWidget(self.noise_tilt, alignment=Qt.AlignmentFlag.AlignCenter)
        noise_strip.addLayout(noise_tilt_lay)
        noise_strip.addStretch()

        noise_vol_layout = QVBoxLayout()
        noise_vol_layout.setSpacing(2)
        noise_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.noise_vol = ResetSlider(Qt.Orientation.Vertical, 0)
        self.noise_vol.setRange(0, 100)
        self.noise_vol.setFixedHeight(80)
        noise_vol_layout.addWidget(self.noise_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.noise_mute = QPushButton("M")
        self.noise_mute.setCheckable(True)
        self.noise_mute.setFixedSize(20, 20)
        self.noise_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        noise_vol_layout.addWidget(self.noise_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.noise_pan = PotentiometerKnob("Noise Pan", self)
        self.noise_pan.setRange(-100, 100)
        self.noise_pan.setValue(0)
        noise_vol_layout.addWidget(self.noise_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_pn = QLabel("Pan"); lbl_pn.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_pn.setStyleSheet("font-size: 9px;")
        noise_vol_layout.addWidget(lbl_pn)
        noise_strip.addLayout(noise_vol_layout)
        mixer_lay.addLayout(noise_strip)
        # Sub Strip Configuration
        sub_strip = QVBoxLayout()
        sub_strip.setSpacing(4)
        sub_strip.addWidget(QLabel("<b>Sub</b>"), alignment=Qt.AlignmentFlag.AlignCenter)

        self.sub_vis = WaveformVisualizer()
        sub_strip.addWidget(self.sub_vis, alignment=Qt.AlignmentFlag.AlignCenter)

        self.sub_combo = QComboBox()
        self.sub_combo.addItems(["Saw", "Square", "Triangle", "Sine"])
        self.sub_combo.setCurrentIndex(1)
        sub_strip.addWidget(self.sub_combo)
        sub_strip.addStretch()

        sub_vol_layout = QVBoxLayout()
        sub_vol_layout.setSpacing(2)
        sub_vol_layout.addWidget(QLabel("Vol:"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.sub_vol = ResetSlider(Qt.Orientation.Vertical, 0)
        self.sub_vol.setRange(0, 100)
        self.sub_vol.setFixedHeight(80)
        sub_vol_layout.addWidget(self.sub_vol, alignment=Qt.AlignmentFlag.AlignCenter)

        self.sub_mute = QPushButton("M")
        self.sub_mute.setCheckable(True)
        self.sub_mute.setFixedSize(20, 20)
        self.sub_mute.setStyleSheet("QPushButton:checked { background-color: red; color: white; }")
        sub_vol_layout.addWidget(self.sub_mute, alignment=Qt.AlignmentFlag.AlignCenter)

        self.sub_pan = PotentiometerKnob("Sub Pan", self)
        self.sub_pan.setRange(-100, 100)
        self.sub_pan.setValue(0)
        sub_vol_layout.addWidget(self.sub_pan, alignment=Qt.AlignmentFlag.AlignCenter)
        lbl_ps = QLabel("Pan"); lbl_ps.setAlignment(Qt.AlignmentFlag.AlignCenter); lbl_ps.setStyleSheet("font-size: 9px;")
        sub_vol_layout.addWidget(lbl_ps)
        sub_strip.addLayout(sub_vol_layout)
        mixer_lay.addLayout(sub_strip)

        mixer_group.setLayout(mixer_lay)
        osc_outer_lay.addWidget(mixer_group, stretch=4)
        # Right Column Group: Side-by-Side Amp EGs Horizontal Layout Overhaul
        amp_eg_group = QGroupBox("Dedicated Amplitude Envelopes")
        amp_eg_lay = QHBoxLayout()  # Overhauled layout: Horizontal alignment blocks
        amp_eg_lay.setContentsMargins(4, 6, 4, 6)
        amp_eg_lay.setSpacing(6)

        self.vco1_amp_eg = ADSRPanel("Osc 1 Amp EG", (5, 1000), (5, 2000), (10, 2000))
        self.vco2_amp_eg = ADSRPanel("Osc 2 Amp EG", (5, 1000), (5, 2000), (10, 2000))
        self.noise_amp_eg = ADSRPanel("Noise Amp EG", (5, 1000), (5, 2000), (10, 2000))

        self.btn_sync_amp_eg = QPushButton("1\u21922")
        self.btn_sync_amp_eg.setFixedSize(30, 16)
        self.btn_sync_amp_eg.setStyleSheet("font-size: 8px; font-weight: bold;")
        self.btn_sync_amp_eg.setToolTip("Copy Osc 1 Amp EG settings to Osc 2 Amp EG")
        self.vco1_amp_eg.title_layout.addWidget(self.btn_sync_amp_eg)
        self.btn_sync_amp_eg.clicked.connect(lambda: self.vco2_amp_eg.set_state(self.vco1_amp_eg.get_state()))

        # Osc 2 and Noise have no sync button of their own -- reserve the exact same footprint
        # in their title rows anyway so all three panels stay the same height/alignment instead
        # of only Osc 1's row being taller.
        for panel in (self.vco2_amp_eg, self.noise_amp_eg):
            sync_btn_spacer = QWidget()
            sync_btn_spacer.setFixedSize(30, 16)
            _retain_hidden(sync_btn_spacer)
            sync_btn_spacer.setVisible(False)
            panel.title_layout.addWidget(sync_btn_spacer)

        amp_eg_lay.addWidget(self.vco1_amp_eg)
        amp_eg_lay.addWidget(self.vco2_amp_eg)
        amp_eg_lay.addWidget(self.noise_amp_eg)
        amp_eg_group.setLayout(amp_eg_lay)

        osc_outer_lay.addWidget(amp_eg_group, stretch=6)
        tab_osc_root_lay.addWidget(self.osc_prog_widget)

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

        self.osc_prog_radio.toggled.connect(self.osc_prog_widget.setVisible)
        self.osc_combi_radio.toggled.connect(self.osc_combi_widget.setVisible)
        # ----------------------------------------------------
        # FILTERS TAB CONTENT (2-FILTER OVERHAUL)
        # ----------------------------------------------------
        filters_outer_lay = QVBoxLayout(self.tab_filters)
        filters_column = QVBoxLayout()
        filters_column.setSpacing(6)

        # Container list definitions for loop compilation connections
        self.cutoff_dials, self.q_dials, self.vcf_poles, self.vcf_types = [], [], [], []

        # --- FILTER A VIEW PANEL ---
        filter_a_box = QGroupBox("VCF Filter A")
        filter_a_lay = QVBoxLayout()
        filter_a_lay.setSpacing(2); filter_a_lay.setContentsMargins(4, 4, 4, 4)

        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("<b>Engine Mode:</b>"))
        self.filter_a_mode_combo = QComboBox()
        self.filter_a_mode_combo.addItems(["Normal (Single Filter Core)", "Multi (Legacy 3-Filter Cascade)"])
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

        # Filter A Sub-Widget: Legacy Multi (Deploys 3 distinct blocks inside a sub-grid)
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
            "LFO 1 Phase", "LFO 2 Phase", "LFO 3 Phase", "LFO 4 Phase"
        ]

        for x in range(1, 5):
            lfo_row = QHBoxLayout()
            lfo_row.setSpacing(4)
            lfo_row.addWidget(QLabel(f"<b>LFO {x}</b>"))

            combo = QComboBox()
            combo.addItems(["Tri", "Sine", "Saw", "Sq"])
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

        lfo_group.setLayout(lfo_layout)
        lfo_outer_lay.addWidget(lfo_group)
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
        self.btn_randomize_eg.setToolTip("Randomize all 4 assignable EGs")
        eg_header_row.addWidget(self.btn_randomize_eg)
        eg_layout.addLayout(eg_header_row)
        self.btn_randomize_eg.clicked.connect(self.randomize_assignable_egs)

        self.envelopes, self.eg_dest_combos, self.eg_intensity_dials = [], [], []

        for x in range(1, 5):
            panel = ADSRPanel(f"EG {x}", (5, 1000), (5, 2000), (10, 2000))
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

        eg_group.setLayout(eg_layout)
        eg_outer_lay.addWidget(eg_group)

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

        ext_in_box = QGroupBox("External Audio Input")
        ext_in_lay = QHBoxLayout()
        ext_in_lay.setSpacing(8)

        self.ext_in_enable = QCheckBox("Enable")
        ext_in_lay.addWidget(self.ext_in_enable)

        gain_col = QVBoxLayout(); gain_col.setSpacing(0)
        gain_col.addWidget(QLabel("Gain"), alignment=Qt.AlignmentFlag.AlignCenter)
        self.ext_in_gain_dial = PotentiometerKnob("Input Gain", self)
        self.ext_in_gain_dial.setRange(0, 200)
        self.ext_in_gain_dial.setValue(100)
        gain_col.addWidget(self.ext_in_gain_dial, alignment=Qt.AlignmentFlag.AlignCenter)
        ext_in_lay.addLayout(gain_col)

        ext_in_note = QLabel("Routed into the same FX bus as the synth's own voices — passes through all 12 FX slots.")
        ext_in_note.setStyleSheet("font-size: 9px; color: #888;")
        ext_in_note.setWordWrap(True)
        ext_in_lay.addWidget(ext_in_note, stretch=1)

        ext_in_box.setLayout(ext_in_lay)
        audio_outer_lay.addWidget(ext_in_box)
        audio_outer_lay.addStretch()

        # ----------------------------------------------------
        # ASSEMBLE AND ANCHOR TOUCH DECK TO ROOT LAYOUT
        # ----------------------------------------------------
        self.tab_widget.addTab(self.tab_global, "Global")
        self.tab_widget.addTab(self.tab_play, "Play")
        self.tab_widget.addTab(self.tab_filters, "Filters")
        self.tab_widget.addTab(self.tab_lfos, "LFOs")
        self.tab_widget.addTab(self.tab_egs, "Envelopes")
        self.tab_widget.addTab(self.tab_fx, "FX")
        self.tab_widget.addTab(self.tab_audio, "Audio")

        outer_layout.addWidget(self.tab_widget, stretch=5)
        self.setLayout(outer_layout)

        # Connect visualizer hooks to drive static icon drawings
        self.vco1_combo.currentIndexChanged.connect(lambda idx: self.vco1_vis.set_waveform(idx, is_lfo=False))
        self.vco2_combo.currentIndexChanged.connect(lambda idx: self.vco2_vis.set_waveform(idx, is_lfo=False))
        self.vco1_vis.set_waveform(self.vco1_combo.currentIndex(), is_lfo=False)
        self.vco2_vis.set_waveform(self.vco2_combo.currentIndex(), is_lfo=False)
        self.sub_combo.currentIndexChanged.connect(lambda idx: self.sub_vis.set_waveform(idx, is_lfo=False))
        self.sub_vis.set_waveform(self.sub_combo.currentIndex(), is_lfo=False)

        UNISON_SAW_INDEX = 4
        PULSE_INDEX = 6
        self.vco1_combo.currentIndexChanged.connect(lambda idx: self.vco1_unison_box.setVisible(idx == UNISON_SAW_INDEX))
        self.vco2_combo.currentIndexChanged.connect(lambda idx: self.vco2_unison_box.setVisible(idx == UNISON_SAW_INDEX))
        self.vco1_unison_box.setVisible(self.vco1_combo.currentIndex() == UNISON_SAW_INDEX)
        self.vco2_unison_box.setVisible(self.vco2_combo.currentIndex() == UNISON_SAW_INDEX)

        self.vco1_combo.currentIndexChanged.connect(lambda idx: self.vco1_pulse_box.setVisible(idx == PULSE_INDEX))
        self.vco2_combo.currentIndexChanged.connect(lambda idx: self.vco2_pulse_box.setVisible(idx == PULSE_INDEX))
        self.vco1_pulse_box.setVisible(self.vco1_combo.currentIndex() == PULSE_INDEX)
        self.vco2_pulse_box.setVisible(self.vco2_combo.currentIndex() == PULSE_INDEX)
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

    def refresh_audio_devices(self):
        """Re-scans ALSA device names, preserving whatever's currently selected/typed if it's
        still present (or just keeping the typed text if it isn't a scanned name at all)."""
        for combo, list_cmd in ((self.output_device_combo, ["aplay", "-L"]), (self.input_device_combo, ["arecord", "-L"])):
            current_text = combo.currentText()
            combo.blockSignals(True)
            combo.clear()
            combo.addItems(_list_alsa_devices(list_cmd))
            combo.setCurrentText(current_text)
            combo.blockSignals(False)

    def connect_to_backend(self):
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(self.server_address); self.sock.setblocking(False)
            self.mock_mode = False
            print("Successfully linked up to C engine Unix Domain Socket node!")
        except socket.error:
            self.mock_mode = True; self.sock = None
            print("--- WORKING IN MOCK GUI MODE ---")

    def listen_for_vu_frames(self):
        if self.sock is not None and not self.mock_mode:
            try:
                data = self.sock.recv(4)
                if len(data) == 4:
                    vu_peak_value = struct.unpack("f", data)
                    self.vu_meter.setValue(int(max(0, min(100, vu_peak_value[0] * 100.0))))
            except (socket.error, BlockingIOError): pass
    def transmit_knob_packet(self):
        glide_ms = self.glide_dial.value()
        self.glide_readout.setText(f"{glide_ms} ms")
        glide_random_pct = self.glide_random_dial.value()
        self.glide_random_readout.setText(f"{glide_random_pct}%")

        vassign = 1.0 if self.poly_radio.isChecked() else 0.0
        v1_shape = float(self.vco1_combo.currentIndex())
        v2_shape = float(self.vco2_combo.currentIndex())
        fm = float(self.fm_dial.value()) / 100.0

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

        # Pack 8 parameters (D, H, A, Dec, S, R, Attack Start, Attack Peak) for all 7 envelope instances (+56 items)
        # Order matches C struct exactly: Osc 1 Amp, Osc 2 Amp, Noise Amp, then EG 1-4.
        all_panels = [self.vco1_amp_eg, self.vco2_amp_eg, self.noise_amp_eg] + self.envelopes
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
        # Add Filter A/B outer routing mode (+1 item)
        packet_list.append(float(self.filter_routing_mode.currentIndex()))

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
        # Add active LFO configurations (+24 items, incl. phase offset)
        for idx in range(4):
            lfo_hz = float(self.lfo_dials[idx].value()) / 10.0
            self.lfo_readouts[idx].setText(f"{lfo_hz:.1f} Hz")
            lfo_dp = float(self.lfo_depth_dials[idx].value()) / 100.0
            packet_list.append(float(self.lfo_combos[idx].currentIndex()))
            packet_list.append(lfo_hz)
            packet_list.append(lfo_dp)
            packet_list.append(float(self.lfo_sync_combos[idx].currentIndex()))
            packet_list.append(float(self.lfo_dest_combos[idx].currentIndex()))
            packet_list.append(float(self.lfo_phase_dials[idx].value()))
        # Append the 4 custom EG intensity multipliers (+4 items)
        for idx in range(4):
            packet_list.append(float(self.eg_intensity_dials[idx].value()) / 100.0)

        # Append each assignable EG's own mod destination (+4 items)
        for idx in range(4):
            packet_list.append(float(self.eg_dest_combos[idx].currentIndex()))

        # Append the 7 active envelope mode selection indices (+7 items).
        # Sequence matches C struct exactly: Osc 1 Amp, Osc 2 Amp, Noise Amp, then EG 1-4.
        packet_list.append(float(self.vco1_amp_eg.combo_type.currentIndex()))
        packet_list.append(float(self.vco2_amp_eg.combo_type.currentIndex()))
        packet_list.append(float(self.noise_amp_eg.combo_type.currentIndex()))
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

        # Hard Sync (+1 item)
        packet_list.append(1.0 if self.hard_sync_checkbox.isChecked() else 0.0)

        # Phase pots, degrees 0-360 (+2 items)
        packet_list.append(float(self.vco1_phase.value()))
        packet_list.append(float(self.vco2_phase.value()))

        # --- 4-Oscillator architecture additions (+10 items) ---
        # Pulse width, Osc 1/2 (+2 items)
        packet_list.append(float(self.vco1_pulse_width.value()))
        packet_list.append(float(self.vco2_pulse_width.value()))

        # Dedicated Noise Oscillator: vol, pan, mute, tilt (+4 items)
        packet_list.append(float(self.noise_vol.value()) / 100.0)
        packet_list.append(float(self.noise_pan.value()) / 100.0)
        packet_list.append(1.0 if self.noise_mute.isChecked() else 0.0)
        packet_list.append(float(self.noise_tilt.value()))

        # Sub Oscillator: shape, vol, pan, mute (+4 items)
        packet_list.append(float(self.sub_combo.currentIndex()))
        packet_list.append(float(self.sub_vol.value()) / 100.0)
        packet_list.append(float(self.sub_pan.value()) / 100.0)
        packet_list.append(1.0 if self.sub_mute.isChecked() else 0.0)

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

        # UNIFIED CONTRACT VERIFICATION
        # 151 (baseline through 4-osc additions) + 2 (ext in) + 132 (12 FX slots * 11)
        # + 1 (fx_routing_mode) = 286 floats, followed by 2 fixed 64-byte device name strings
        # (not floats).
        # 13 + 56 + 1 + 1 + 4 + 1 + 12 + 4 + 24 + 4 + 4 + 7 + 4 + 1 + 2 + 1 + 2 + 10 + 2 + 132 + 1 = 286
        binary_packet = struct.pack(
            f"286f{AUDIO_DEVICE_NAME_LEN}s{AUDIO_DEVICE_NAME_LEN}s",
            *packet_list,
            _encode_device_name(self.output_device_combo.currentText()),
            _encode_device_name(self.input_device_combo.currentText()),
        )
        if self.sock is not None:
            try: self.sock.sendall(binary_packet)
            except socket.error: self.sock = None

    def closeEvent(self, event):
        if self.sock is not None: self.sock.close()
        event.accept()
def runtime_connect_and_bind(gui):
    # Core global mode switches
    gui.poly_radio.toggled.connect(gui.transmit_knob_packet)
    gui.mono_radio.toggled.connect(gui.transmit_knob_packet)
    gui.hard_sync_checkbox.toggled.connect(gui.transmit_knob_packet)
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
    gui.fm_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.glide_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.glide_random_dial.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_pulse_width.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pulse_width.valueChanged.connect(gui.transmit_knob_packet)

    # Dedicated Noise Oscillator
    gui.noise_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.noise_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.noise_mute.toggled.connect(gui.transmit_knob_packet)
    gui.noise_tilt.valueChanged.connect(gui.transmit_knob_packet)

    # Sub Oscillator
    gui.sub_combo.currentIndexChanged.connect(gui.transmit_knob_packet)
    gui.sub_vol.valueChanged.connect(gui.transmit_knob_packet)
    gui.sub_pan.valueChanged.connect(gui.transmit_knob_packet)
    gui.sub_mute.toggled.connect(gui.transmit_knob_packet)

    # Detuning, pitch transposition, and basic phases
    gui.vco1_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_pitch_offset.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_fine_tune.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco1_phase.valueChanged.connect(gui.transmit_knob_packet)
    gui.vco2_phase.valueChanged.connect(gui.transmit_knob_packet)
    gui.filter_routing_mode.currentIndexChanged.connect(gui.transmit_knob_packet)

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
    all_egs = [gui.vco1_amp_eg, gui.vco2_amp_eg, gui.noise_amp_eg] + gui.envelopes
    for eg in all_egs:
        eg.valuesChanged.connect(gui.transmit_knob_packet)
        eg.combo_type.currentIndexChanged.connect(gui.transmit_knob_packet)

    for drop in gui.eg_dest_combos:
        drop.currentIndexChanged.connect(gui.transmit_knob_packet)
    for dial in gui.eg_intensity_dials:
        dial.valueChanged.connect(gui.transmit_knob_packet)

    # Assignable LFO engines and small static vector visualizations
    for idx in range(4):
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

    # Initialize separate telemetry thread loop for VU meter updates
    gui.vu_timer = QTimer()
    gui.vu_timer.timeout.connect(gui.listen_for_vu_frames)
    gui.vu_timer.start(25)

    # Broadcast early configuration packet on boot
    gui.transmit_knob_packet()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    main_window = SynthGui()
    runtime_connect_and_bind(main_window)
    main_window.show()
    sys.exit(app.exec())
