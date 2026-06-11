import time

from PyQt5.QtCore import Qt, QRectF, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QPainter, QBrush, QPen, QFont
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QPushButton, QLabel, QSizePolicy, QFrame

from protocol import display_key_name, normalize_key_name


class HoldButton(QPushButton):
    """Button that distinguishes short clicks from long presses via mouse events.
    Uses elapsed time on release, avoiding QTimer unreliability while held."""
    hold_triggered = pyqtSignal()
    short_clicked = pyqtSignal()

    def __init__(self, text, parent=None):
        super().__init__(text, parent)
        self._press_time = 0.0

    def _set_pulse(self, on):
        self.setProperty("pulse", on)
        self.style().unpolish(self)
        self.style().polish(self)

    def mousePressEvent(self, event):
        self._press_time = time.time()
        self._set_pulse(True)
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        elapsed = time.time() - self._press_time
        self._set_pulse(False)
        super().mouseReleaseEvent(event)
        if elapsed >= 0.8:
            self.hold_triggered.emit()
        else:
            self.short_clicked.emit()


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
        "-": "g", "_": "d", "~": "abfg", " ": ""
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
        ("DISP", "DISP"), ("SPEED", "SPEED"), ("FORMAT", "FORMAT"), ("EXT", "EXT"),
    ]
    GPIO_KEYS = [("USR1", "USER1"), ("USR2", "USER2")]

    def __init__(self):
        super().__init__()
        self.digits = []
        self.leds = []
        self.buttons = {}
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
            btn = HoldButton(label)
            btn.setMinimumHeight(26)
            btn.setMinimumWidth(0)
            btn.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            btn.setProperty("pulse", False)
            btn.short_clicked.connect(lambda n=name: self.key_clicked.emit(n))
            btn.hold_triggered.connect(lambda n=name: self.key_long.emit(n))
            self.buttons[name] = btn
            key_grid.addWidget(btn, idx // 4, idx % 4)
        layout.addLayout(key_grid)

        # GPIO USER1 USER2 with visual separation from I2C keys
        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        sep.setStyleSheet("QFrame { color: #374151; }")
        layout.addWidget(sep)

        gpio_row = QHBoxLayout()
        gpio_row.setSpacing(6)
        for label, name in self.GPIO_KEYS:
            btn = HoldButton(label)
            btn.setMinimumHeight(26)
            btn.setMinimumWidth(0)
            btn.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            btn.setProperty("pulse", False)
            btn.setStyleSheet("QPushButton { background: #1a2740; border: 1px solid #3b82f6; border-radius: 4px; padding: 6px 8px; }"
                             "QPushButton:hover { background: #243860; }"
                             "QPushButton[pulse=\"true\"] { background: #2563eb; border-color: #93c5fd; }")
            btn.short_clicked.connect(lambda n=name: self.key_clicked.emit(n))
            btn.hold_triggered.connect(lambda n=name: self.key_long.emit(n))
            self.buttons[name] = btn
            gpio_row.addWidget(btn)
        layout.addLayout(gpio_row)

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


class CountdownRing(QWidget):
    """情景倒计时进度环: 圆弧进度 + 中心剩余 MM:SS + 状态文字。

    与 S800 板 *EVT:CD 状态联动, 进度按 remain/total 渲染。
    """

    STATE_TEXT = {
        "IDLE": "空闲", "EDIT": "板上编辑中", "RUN": "运行中",
        "PAUSE": "已暂停", "DONE": "时间到!",
    }

    def __init__(self):
        super().__init__()
        self.state = "IDLE"
        self.remain = 0
        self.total = 1
        self.scene = 0
        self.setMinimumSize(180, 180)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    def update_state(self, state: str, remain: int, total: int, scene: int):
        self.state = state
        self.remain = max(0, remain)
        self.total = max(1, total)
        self.scene = scene
        self.update()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        side = min(w, h) - 16
        x = (w - side) / 2
        y = (h - side) / 2
        rect = QRectF(x, y, side, side)

        # 底环
        p.setPen(QPen(QColor("#374151"), 12, Qt.SolidLine, Qt.RoundCap))
        p.drawArc(rect, 0, 360 * 16)

        # 进度弧: 剩余占比, 从 12 点钟方向顺时针递减
        frac = self.remain / self.total if self.total else 0
        if self.state in ("RUN", "PAUSE", "EDIT"):
            color = "#22c55e" if self.state == "RUN" else ("#fbbf24" if self.state == "PAUSE" else "#60a5fa")
            span = int(-360 * frac * 16)
            p.setPen(QPen(QColor(color), 12, Qt.SolidLine, Qt.RoundCap))
            p.drawArc(rect, 90 * 16, span)
        elif self.state == "DONE":
            p.setPen(QPen(QColor("#ef4444"), 12, Qt.SolidLine, Qt.RoundCap))
            p.drawArc(rect, 0, 360 * 16)

        # 中心剩余时间 MM:SS
        mm, ss = self.remain // 60, self.remain % 60
        p.setPen(QColor("#e5e7eb"))
        big = QFont()
        big.setPointSize(max(14, int(side / 6)))
        big.setBold(True)
        p.setFont(big)
        p.drawText(rect, Qt.AlignCenter, f"{mm:02d}:{ss:02d}")

        # 状态文字
        small = QFont()
        small.setPointSize(11)
        p.setFont(small)
        p.setPen(QColor("#9ca3af"))
        label = self.STATE_TEXT.get(self.state, self.state)
        p.drawText(QRectF(x, y + side * 0.66, side, 24), Qt.AlignCenter, label)
