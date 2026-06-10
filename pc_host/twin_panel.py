from PyQt5.QtCore import Qt, QRectF, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QPainter, QBrush
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QPushButton, QLabel, QSizePolicy

from protocol import display_key_name, normalize_key_name


class SevenSegmentDigit(QWidget):
    SEGMENTS = {
        "a": (0.22, 0.06, 0.56, 0.08),
        "b": (0.78, 0.13, 0.08, 0.33),
        "c": (0.78, 0.54, 0.08, 0.33),
        "d": (0.22, 0.87, 0.56, 0.08),
        "e": (0.14, 0.54, 0.08, 0.33),
        "f": (0.14, 0.13, 0.08, 0.33),
        "g": (0.22, 0.47, 0.56, 0.08),
    }
    MAP = {
        "0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd",
        "4": "fgbc", "5": "afgcd", "6": "afgecd", "7": "abc",
        "8": "abcdefg", "9": "abfgcd", "A": "abcefg", "B": "fgecd",
        "C": "afed", "D": "bgecd", "E": "afged", "F": "afge",
        "G": "afecd", "H": "fbceg", "I": "e", "J": "bcd",
        "K": "fbedg", "L": "fed", "M": "agec", "N": "egc",
        "O": "cdeg", "P": "abfeg", "Q": "abcfg", "R": "eg",
        "S": "fgc", "T": "fged", "U": "bcdef", "V": "fbg",
        "W": "fbgd", "X": "fbec", "Y": "fbgcd", "Z": "agd",
        "-": "g", "_": "d", " ": ""
    }

    def __init__(self):
        super().__init__()
        self.char = " "
        self.dp = False
        self.blink = False
        self._blink_visible = True
        self._blink_timer = QTimer(self)
        self._blink_timer.timeout.connect(self._toggle_blink)
        self.setFixedSize(38, 70)
        self.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)

    def set_char(self, ch: str):
        self.char = (ch or " ")[0].upper()
        self.update()

    def set_dp(self, on: bool):
        self.dp = on
        self.update()

    def set_blink(self, on: bool, period_ms=500):
        self.blink = on
        self._blink_visible = True
        if on:
            self._blink_timer.start(period_ms)
        else:
            self._blink_timer.stop()
        self.update()

    def _toggle_blink(self):
        self._blink_visible = not self._blink_visible
        self.update()

    def set_value(self, ch: str, dp: bool):
        self.set_char(ch)
        self.set_dp(dp)

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        active = QColor("#FF3030")
        inactive = QColor("#220000")
        p.fillRect(self.rect(), QColor("#050505"))
        on = "" if (self.blink and not self._blink_visible) else self.MAP.get(self.char, "")
        for name, rect in self.SEGMENTS.items():
            x, y, rw, rh = rect
            p.setBrush(QBrush(active if name in on else inactive))
            p.setPen(Qt.NoPen)
            p.drawRoundedRect(QRectF(x * w, y * h, rw * w, rh * h), 3, 3)
        p.setBrush(QBrush(active if self.dp else inactive))
        p.drawEllipse(QRectF(0.82 * w, 0.87 * h, 0.12 * w, 0.08 * h))


class LedIndicator(QWidget):
    """圆形渐变 LED 指示灯：圆点(QSS qradialgradient) + 下方标号与注释"""

    def __init__(self, index: int, hint: str):
        super().__init__()
        self.on = False
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(2)
        v.setAlignment(Qt.AlignCenter)

        self._dot = QLabel()
        self._dot.setObjectName("led_off")
        self._dot.setFixedSize(30, 30)
        v.addWidget(self._dot, 0, Qt.AlignCenter)

        caption = QLabel(f"L{index + 1}\n{hint}")
        caption.setAlignment(Qt.AlignCenter)
        caption.setStyleSheet("font-size: 13px; color: #d1d5db;")
        v.addWidget(caption)
        self.setToolTip(hint)

    def set_on(self, on: bool):
        on = bool(on)
        if on == self.on:
            return
        self.on = on
        self._dot.setObjectName("led_on" if on else "led_off")
        self._dot.style().unpolish(self._dot)
        self._dot.style().polish(self._dot)


class TwinPanel(QWidget):
    key_clicked = pyqtSignal(str)
    key_long = pyqtSignal(str)

    LED_HINTS = ["HB", "ALM", "EDIT", "RX/TX", "SUN", "RAI/SNO", "HOT", "NTP"]
    KEY_LAYOUT = [
        ("FUNC", "FUNC"), ("SHFT", "SHIFT"), ("ADD", "ADD"), ("SAVE", "SAVE"),
        ("DISP", "DISP"), ("SPEED", "SPEED"), ("USR1", "USER1"), ("USR2", "USER2"),
        ("FORMAT", "FORMAT"), ("EXT", "EXT"),
    ]

    def __init__(self):
        super().__init__()
        self.digits = []
        self.leds = []
        self.buttons = {}
        self._pressed = {}
        self._long_sent = {}
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)

        digit_row = QHBoxLayout()
        digit_row.setSpacing(4)
        digit_row.setAlignment(Qt.AlignCenter)
        for _ in range(8):
            digit = SevenSegmentDigit()
            self.digits.append(digit)
            digit_row.addWidget(digit)
        layout.addLayout(digit_row)

        led_row = QHBoxLayout()
        led_row.setSpacing(7)
        led_row.setAlignment(Qt.AlignCenter)
        for i, hint in enumerate(self.LED_HINTS):
            led = LedIndicator(i, hint)
            self.leds.append(led)
            led_row.addWidget(led)
        layout.addLayout(led_row)

        key_grid = QGridLayout()
        key_grid.setHorizontalSpacing(6)
        key_grid.setVerticalSpacing(6)
        for idx, (label, name) in enumerate(self.KEY_LAYOUT):
            btn = QPushButton(label)
            btn.setMinimumHeight(26)
            btn.setMinimumWidth(0)
            btn.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            btn.setProperty("pulse", False)
            btn.pressed.connect(lambda n=name: self._on_pressed(n))
            btn.released.connect(lambda n=name: self._on_released(n))
            self.buttons[name] = btn
            key_grid.addWidget(btn, idx // 4, idx % 4)
        layout.addLayout(key_grid)

        layout.addStretch(1)

    def update_digits(self, text: str, dp_hex: int):
        for i, digit in enumerate(self.digits):
            digit.set_value(text[i] if i < len(text) else " ", bool(dp_hex & (1 << i)))

    def update_leds(self, value: int):
        for i, led in enumerate(self.leds):
            led.set_on(bool(value & (1 << i)))

    def pulse_key(self, name: str):
        key = normalize_key_name(name)
        btn = self.buttons.get(key)
        if not btn:
            return
        btn.setProperty("pulse", True)
        btn.style().unpolish(btn)
        btn.style().polish(btn)
        QTimer.singleShot(200, lambda b=btn: self._clear_pulse(b))

    def _clear_pulse(self, btn):
        btn.setProperty("pulse", False)
        btn.style().unpolish(btn)
        btn.style().polish(btn)

    def _on_pressed(self, name: str):
        self._pressed[name] = True
        self._long_sent[name] = False
        QTimer.singleShot(850, lambda n=name: self._emit_long(n))

    def _on_released(self, name: str):
        if self._pressed.get(name) and not self._long_sent.get(name):
            self.key_clicked.emit(name)
        self._pressed[name] = False

    def _emit_long(self, name: str):
        if self._pressed.get(name):
            self._long_sent[name] = True
            self.key_long.emit(name)
