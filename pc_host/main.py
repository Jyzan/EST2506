import datetime as dt
import sys
import time
from dataclasses import dataclass

from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer
from PyQt5.QtGui import QColor, QTextCharFormat, QTextCursor
from PyQt5.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFileDialog, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton,
    QRadioButton, QScrollArea, QSizePolicy, QSpinBox, QSplitter, QStatusBar,
    QTabWidget, QVBoxLayout, QWidget, QButtonGroup, QLineEdit
)

from astral_helper import current_daynight
from chart_widget import ChartWidget
from log_store import EventStore
from ntp_helper import fetch_network_time
from protocol import (
    is_heartbeat, parse_disp_event, parse_led_event,
    parse_cd_event, parse_cd_status_payload
)
from serial_worker import SerialWorker
from twin_panel import TwinPanel, CountdownRing
from ui_main_window import Ui_MainWindow
from weather_helper import fetch_shanghai_weather


DEFAULT_PORT = "COM9"
KEY_NAMES = ["FUNC", "SHIFT", "ADD", "SAVE", "DISP", "SPEED", "FORMAT", "EXT", "USER1", "USER2"]


@dataclass
class AppState:
    connected: bool = False
    online: bool = False
    fmt: str = "LEFT"
    mode: str = "DAY"
    alarm: str = "OFF"
    latency_ms: int = 0


class NetworkWorker(QThread):
    done = pyqtSignal(str, object)
    failed = pyqtSignal(str, str)

    def __init__(self, kind: str):
        super().__init__()
        self.kind = kind

    def run(self):
        try:
            if self.kind == "ntp":
                self.done.emit(self.kind, fetch_network_time())
            elif self.kind == "weather":
                self.done.emit(self.kind, fetch_shanghai_weather())
        except Exception as exc:
            self.failed.emit(self.kind, str(exc))


class TtsSpeaker:
    """离线语音播报封装: 惰性导入 pyttsx3, 后台线程朗读不阻塞 GUI。

    库缺失或初始化失败时静默降级(available=False), 调用方仅记日志不崩溃。
    """

    def __init__(self):
        self.available = False
        self._engine = None
        self._lock = None
        self._sapi_queue = None
        try:
            import importlib.util
            import queue
            import threading
            # 仅探测依赖是否可导入。pyttsx3 在部分 Windows SAPI 默认语音损坏时
            # 会初始化失败，因此保留 win32com 直连 SAPI 作为备用通道。
            has_win32com = importlib.util.find_spec("win32com") is not None
            has_pyttsx3 = importlib.util.find_spec("pyttsx3") is not None
            self._lock = threading.Lock()
            if has_win32com:
                self._sapi_queue = queue.Queue()
                threading.Thread(target=self._sapi_worker, daemon=True).start()
                self.available = True
            elif has_pyttsx3:
                self.available = True
        except Exception:
            self.available = False

    def _ensure_engine(self):
        if self._engine is not None:
            return self._engine
        import pyttsx3   # 惰性导入, 缺库则抛出由 say() 捕获
        self._engine = pyttsx3.init()
        return self._engine

    def say(self, text: str) -> bool:
        """后台线程朗读 text。成功调度返回 True, 失败(库缺失等)返回 False。"""
        if not self.available:
            return False
        if self._sapi_queue is not None:
            try:
                self._sapi_queue.put_nowait(str(text))
                return True
            except Exception:
                return False

        import threading

        def _run():
            try:
                with self._lock:
                    engine = self._ensure_engine()
                    engine.say(text)
                    engine.runAndWait()
            except Exception:
                # 引擎不可用 标记降级 避免反复重试
                self.available = False

        try:
            threading.Thread(target=_run, daemon=True).start()
            return True
        except Exception:
            return False

    def _sapi_worker(self):
        """单独线程内初始化并复用 Windows SAPI，避免每个数字重建语音对象。"""
        try:
            import pythoncom
            from win32com.client import Dispatch
        except Exception:
            self.available = False
            return

        pythoncom.CoInitialize()
        try:
            speaker = Dispatch("SAPI.SpVoice")
            voices = speaker.GetVoices()
            chosen = None
            for i in range(voices.Count):
                token = voices.Item(i)
                name = token.GetDescription()
                if "Zira" in name or "English" in name:
                    chosen = token
                    break
                if chosen is None:
                    chosen = token
            if chosen is not None:
                speaker.Voice = chosen
            speaker.Rate = 4
            speaker.Volume = 100
            while True:
                text = self._sapi_queue.get()
                while not self._sapi_queue.empty():
                    text = self._sapi_queue.get_nowait()
                try:
                    # Async + purge: countdown speech must track the latest second,
                    # not wait behind older utterances.
                    speaker.Speak(str(text), 3)
                except Exception:
                    self.available = False
        except Exception:
            self.available = False
        finally:
            pythoncom.CoUninitialize()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("S800 Smart Clock Host - 524442910013")
        self.resize(1380, 820)
        self.setMinimumSize(1280, 720)
        self.state = AppState()
        self.worker = None
        self.net_worker = None
        self.last_ping_time = None
        self.last_pong_time = 0.0
        self.store = EventStore("events.csv")
        self.log_lines = 0
        self.tts = TtsSpeaker()
        self.cd_state = "IDLE"
        self.cd_scene = 0          # 跟踪当前情景(可能被板上 EXT 编辑改变)
        self.cd_last_spoken = -1   # 上次语音播报的剩余秒 避免重复念
        self._cd_ring_timer = QTimer(self)
        self._cd_ring_timer.timeout.connect(self._cd_ring_tick)
        self._pending_daynight = False

        self.build_ui()
        self.refresh_ports()

        self.heartbeat_timer = QTimer(self)
        self.heartbeat_timer.timeout.connect(self.heartbeat_tick)

        self.weather_timer = QTimer(self)
        self.weather_timer.timeout.connect(self.fetch_weather)
        self.weather_timer.start(30 * 60 * 1000)

        self.daynight_timer = QTimer(self)
        self.daynight_timer.timeout.connect(self.auto_daynight)
        self.daynight_timer.start(60 * 1000)

        QTimer.singleShot(1000, self.fetch_weather)
        QTimer.singleShot(1500, self.auto_daynight)

    def build_ui(self):
        self.ui = Ui_MainWindow()
        self.ui.setupUi(self)
        self.setWindowTitle("S800 Smart Clock Host - 524442910013")
        main = self.ui.content_layout

        self.conn_led = self.ui.conn_led
        self.port_combo = self.ui.port_combo
        self.baud_combo = self.ui.baud_combo
        self.baud_combo.addItems(["9600", "57600", "115200"])
        self.baud_combo.setCurrentText("115200")
        self.connect_btn = self.ui.connect_btn
        self.latency_label = self.ui.latency_label
        self.ui.refresh_btn.clicked.connect(self.refresh_ports)
        self.connect_btn.clicked.connect(self.toggle_connect)
        self.ui.ping_btn.clicked.connect(self.ping)

        splitter = QSplitter()
        mirror_box = QGroupBox("MCU 镜像")
        mirror_layout = QVBoxLayout(mirror_box)
        self.twin = TwinPanel()
        self.twin.key_clicked.connect(self.on_virtual_key)
        self.twin.key_long.connect(self.on_virtual_long_key)
        mirror_layout.addWidget(self.twin)
        splitter.addWidget(mirror_box)
        splitter.addWidget(self.build_control_panel())
        splitter.addWidget(self.build_log_panel())
        splitter.setSizes([320, 660, 400])
        splitter.setStretchFactor(1, 1)
        main.addWidget(splitter, 1)

        self.status = self.statusBar()
        self.update_status()
        self.update_connection_led()

    def button(self, text, slot):
        btn = QPushButton(text)
        btn.clicked.connect(slot)
        return btn

    def build_control_panel(self):
        tabs = QTabWidget()
        tabs.addTab(self.build_control_scroll(), "控制")
        tabs.addTab(self.build_data_tab(), "数据")
        self.control_tabs = tabs
        return tabs

    def build_control_scroll(self):
        container = QWidget()
        col = QVBoxLayout(container)
        col.setContentsMargins(4, 4, 8, 4)
        col.setSpacing(12)
        col.addWidget(self.section("核心功能", self.build_core_tab()))
        col.addWidget(self.section("扩展功能", self.build_extension_tab()))
        col.addWidget(self.section("协议", self.build_protocol_tab()))
        col.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        scroll.setWidget(container)
        # 给竖直滚动条预留空间, 避免滚动按钮被内容遮盖; 不再用 sizeHint 撑宽整列
        scroll.setMinimumWidth(360)
        return scroll

    def section(self, title, inner):
        box = QGroupBox(title)
        lay = QVBoxLayout(box)
        lay.setContentsMargins(8, 8, 8, 8)
        lay.addWidget(inner)
        return box

    def build_core_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setSpacing(10)

        self.year_spin = self.field_spin(2000, 2099, dt.date.today().year)
        self.month_spin = self.field_spin(1, 12, dt.date.today().month)
        self.day_spin = self.field_spin(1, 31, dt.date.today().day)
        self.hour_spin = self.field_spin(0, 23, dt.datetime.now().hour)
        self.min_spin = self.field_spin(0, 59, dt.datetime.now().minute)
        self.sec_spin = self.field_spin(0, 59, dt.datetime.now().second)
        self.alarm_h_spin = self.field_spin(0, 23, 12)
        self.alarm_m_spin = self.field_spin(0, 59, 0)
        self.alarm_s_spin = self.field_spin(0, 59, 0)
        self.date_combo = QComboBox()
        self.date_combo.addItems(["YEAR MONTH DATE", "YEAR DATE", "MONTH DATE", "YEAR", "MONTH", "DATE"])
        self.date_combo.setMinimumWidth(110)

        time_box = QGroupBox("时间 / 日期 / 闹钟")
        box_layout = QVBoxLayout(time_box)
        box_layout.setSpacing(6)

        # Date: fields row, then full-width action row.
        date_fields = QHBoxLayout()
        date_fields.addWidget(self.row_label("日期"))
        date_fields.addWidget(self.year_spin)
        date_fields.addWidget(self.month_spin)
        date_fields.addWidget(self.day_spin)
        date_fields.addWidget(self.date_combo, 1)
        box_layout.addLayout(date_fields)
        date_actions = QHBoxLayout()
        date_actions.addWidget(self.wide_button("设置日期", self.set_date))
        date_actions.addWidget(self.wide_button("获取日期", lambda: self.send_command("*GET:DATE")))
        box_layout.addLayout(date_actions)

        time_fields = QHBoxLayout()
        time_fields.addWidget(self.row_label("时间"))
        time_fields.addWidget(self.hour_spin)
        time_fields.addWidget(self.min_spin)
        time_fields.addWidget(self.sec_spin)
        time_fields.addStretch(1)
        box_layout.addLayout(time_fields)
        time_actions = QHBoxLayout()
        time_actions.addWidget(self.wide_button("设置时间", self.set_time))
        time_actions.addWidget(self.wide_button("获取时间", lambda: self.send_command("*GET:TIME")))
        box_layout.addLayout(time_actions)

        alarm_fields = QHBoxLayout()
        alarm_fields.addWidget(self.row_label("闹钟"))
        alarm_fields.addWidget(self.alarm_h_spin)
        alarm_fields.addWidget(self.alarm_m_spin)
        alarm_fields.addWidget(self.alarm_s_spin)
        alarm_fields.addStretch(1)
        box_layout.addLayout(alarm_fields)
        alarm_actions = QHBoxLayout()
        alarm_actions.addWidget(self.wide_button("设置闹钟", self.set_alarm))
        alarm_actions.addWidget(self.wide_button("获取闹钟", lambda: self.send_command("*GET:ALARM")))
        alarm_actions.addWidget(self.wide_button("关闭闹钟", lambda: self.send_command("*SET:ALARM OFF")))
        box_layout.addLayout(alarm_actions)
        layout.addWidget(time_box)

        cmd_box = QGroupBox("显示 / 消息 / 蜂鸣器 / LED")
        grid = QGridLayout(cmd_box)
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(8)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 2)
        grid.setColumnMinimumWidth(2, 84)

        self.display_group = QButtonGroup(self)
        self.disp_on = QRadioButton("ON")
        self.disp_off = QRadioButton("OFF")
        self.disp_on.setChecked(True)
        self.display_group.addButton(self.disp_on)
        self.display_group.addButton(self.disp_off)
        self.disp_on.clicked.connect(lambda: self.send_command("*SET:DISPLAY ON"))
        self.disp_off.clicked.connect(lambda: self.send_command("*SET:DISPLAY OFF"))
        grid.addWidget(self.disp_on, 0, 0)
        grid.addWidget(self.disp_off, 0, 1)
        grid.addWidget(self.wide_button("获取显示", lambda: self.send_command("*GET:DISPLAY")), 0, 2)

        self.format_group = QButtonGroup(self)
        self.fmt_left = QRadioButton("LEFT")
        self.fmt_right = QRadioButton("RIGHT")
        self.fmt_left.setChecked(True)
        self.format_group.addButton(self.fmt_left)
        self.format_group.addButton(self.fmt_right)
        self.fmt_left.clicked.connect(lambda: self.send_command("*SET:FORMAT LEFT"))
        self.fmt_right.clicked.connect(lambda: self.send_command("*SET:FORMAT RIGHT"))
        grid.addWidget(self.fmt_left, 1, 0)
        grid.addWidget(self.fmt_right, 1, 1)
        grid.addWidget(self.wide_button("获取格式", lambda: self.send_command("*GET:FORMAT")), 1, 2)

        grid.addWidget(QLabel("消息"), 2, 0)
        self.msg_edit = QLineEdit("Hello Clock")
        self.msg_edit.setMaxLength(32)
        grid.addWidget(self.msg_edit, 2, 1)
        grid.addWidget(self.wide_button("发送消息", self.send_msg), 2, 2)

        # 范围放宽到 0-9999, 以便演示越界(<10 或 >5000)时 MCU 回应 ERROR RANGE.
        self.beep_spin = self.spin(0, 9999, 500)
        self.led_edit = QLineEdit("5A")
        grid.addWidget(QLabel("蜂鸣(ms)"), 3, 0)
        grid.addWidget(self.beep_spin, 3, 1)
        grid.addWidget(self.wide_button("蜂鸣", lambda: self.send_command(f"*SET:BEEP {self.beep_spin.value()}")), 3, 2)
        grid.addWidget(QLabel("LED(十六进制)"), 4, 0)
        grid.addWidget(self.led_edit, 4, 1)
        grid.addWidget(self.wide_button("设置 LED", lambda: self.send_command(f"*SET:LED {self.led_edit.text()}")), 4, 2)
        grid.addWidget(self.wide_button("复位", self.reset_mcu), 5, 2)
        layout.addWidget(cmd_box)
        layout.addStretch(1)
        return w

    def build_extension_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)
        net_box = QGroupBox("网络 / 天气 / 昼夜")
        net_layout = QVBoxLayout(net_box)
        self.sun_label = QLabel("日出/日落: --")
        self.mode_label = QLabel("当前模式: --")
        self.auto_mode_check = QCheckBox("自动昼夜")
        self.auto_mode_check.setChecked(True)

        # 上半区: 天气卡片(左) + 控制按钮组(右), 按钮组与卡片在竖直方向居中对齐.
        top_row = QHBoxLayout()
        top_row.addWidget(self.build_weather_card())
        btn_box = QWidget()
        btn_grid = QGridLayout(btn_box)
        btn_grid.setContentsMargins(0, 0, 0, 0)
        btn_grid.addWidget(self.button("NTP 对时", self.ntp_sync), 0, 0)
        btn_grid.addWidget(self.button("立即更新", self.fetch_weather), 0, 1)
        btn_grid.addWidget(self.auto_mode_check, 1, 0)
        btn_grid.addWidget(self.button("应用昼夜", self.apply_daynight), 1, 1)
        btn_grid.addWidget(self.button("强制白天", lambda: self.force_mode("DAY")), 2, 0)
        btn_grid.addWidget(self.button("强制夜间", lambda: self.force_mode("NIGHT")), 2, 1)
        top_row.addWidget(btn_box, 1, Qt.AlignVCenter)
        net_layout.addLayout(top_row)

        # 下半区: 日出/日落(左) 与 当前模式(右) 处于同一水平线.
        info_row = QHBoxLayout()
        info_row.addWidget(self.sun_label)
        info_row.addStretch(1)
        info_row.addWidget(self.mode_label)
        net_layout.addLayout(info_row)
        layout.addWidget(net_box)

        layout.addWidget(self.build_countdown_box())
        layout.addStretch(1)
        return w

    def build_countdown_box(self):
        box = QGroupBox("情景倒计时")
        outer = QHBoxLayout(box)

        # 左: 进度环
        self.countdown_ring = CountdownRing()
        outer.addWidget(self.countdown_ring, 1)

        # 右: 控制
        ctrl = QVBoxLayout()

        dur_row = QHBoxLayout()
        dur_row.addWidget(QLabel("时长"))
        self.cd_min_spin = self.spin(0, 59, 5)
        self.cd_sec_spin = self.spin(0, 59, 0)
        dur_row.addWidget(self.cd_min_spin)
        dur_row.addWidget(QLabel("分"))
        dur_row.addWidget(self.cd_sec_spin)
        dur_row.addWidget(QLabel("秒"))
        dur_row.addStretch(1)
        ctrl.addLayout(dur_row)

        scene_row = QHBoxLayout()
        scene_row.addWidget(QLabel("情景"))
        self.cd_scene_combo = QComboBox()
        self.cd_scene_combo.addItems(["滚动文本", "闪烁庆祝", "静默"])
        self.cd_scene_combo.currentIndexChanged.connect(
            lambda i: self.send_command(f"*SET:COUNTDOWN SCENE {i}"))
        scene_row.addWidget(self.cd_scene_combo, 1)
        ctrl.addLayout(scene_row)

        msg_row = QHBoxLayout()
        msg_row.addWidget(QLabel("文本"))
        self.cd_msg_edit = QLineEdit("CONGRATULATIONS")
        self.cd_msg_edit.setMaxLength(32)
        msg_row.addWidget(self.cd_msg_edit, 1)
        msg_row.addWidget(self.button("下发", self.cd_send_msg))
        ctrl.addLayout(msg_row)

        btn_row = QHBoxLayout()
        btn_row.addWidget(self.button("开始", self.cd_start))
        self.cd_pause_btn = self.button("暂停", self.cd_toggle_pause)
        btn_row.addWidget(self.cd_pause_btn)
        btn_row.addWidget(self.button("停止", lambda: self.send_command("*SET:COUNTDOWN STOP")))
        ctrl.addLayout(btn_row)

        self.cd_voice_check = QCheckBox("语音播报")
        self.cd_voice_check.setChecked(True)
        if not self.tts.available:
            self.cd_voice_check.setEnabled(False)
            self.cd_voice_check.setText("语音播报 (不可用)")
        ctrl.addWidget(self.cd_voice_check)
        ctrl.addStretch(1)

        outer.addLayout(ctrl, 1)
        return box

    def cd_send_msg(self):
        self.cd_scene_combo.blockSignals(True)
        self.cd_scene_combo.setCurrentIndex(0)
        self.cd_scene_combo.blockSignals(False)
        self.cd_scene = 0
        self.send_command("*SET:COUNTDOWN SCENE 0")
        self.send_command(f"*SET:COUNTDOWN MSG {self.cd_msg_edit.text()}")

    def cd_start(self):
        total = self.cd_min_spin.value() * 60 + self.cd_sec_spin.value()
        if total <= 0:
            QMessageBox.warning(self, "倒计时", "时长必须大于 0 秒。")
            return
        self.cd_last_spoken = -1
        scene = self.cd_scene_combo.currentIndex()
        self.cd_scene = scene
        self.send_command(f"*SET:COUNTDOWN SCENE {scene}")
        if scene == 0:
            self.send_command(f"*SET:COUNTDOWN MSG {self.cd_msg_edit.text()}")
        self.send_command(f"*SET:COUNTDOWN TIME {total}")
        self.send_command("*SET:COUNTDOWN START")

    def cd_toggle_pause(self):
        if self.cd_state == "RUN":
            self.send_command("*SET:COUNTDOWN PAUSE")
        elif self.cd_state == "PAUSE":
            self.send_command("*SET:COUNTDOWN RESUME")

    WEATHER_ICONS = {
        "SUN": "☀", "CLD": "⛅", "OVC": "☁",
        "RAI": "\U0001F327", "SNO": "❄", "FOG": "\U0001F32B",
    }

    def build_weather_card(self):
        card = QGroupBox("天气卡片")
        card.setObjectName("weather_card")
        card.setMinimumWidth(190)
        v = QVBoxLayout(card)
        v.setSpacing(4)
        self.weather_city = QLabel("上海 Shanghai")
        self.weather_city.setAlignment(Qt.AlignCenter)
        self.weather_icon = QLabel("--")
        self.weather_icon.setAlignment(Qt.AlignCenter)
        self.weather_icon.setStyleSheet("font-size: 40px;")
        self.weather_temp = QLabel("--°C")
        self.weather_temp.setAlignment(Qt.AlignCenter)
        self.weather_temp.setStyleSheet("font-size: 34px; font-weight: bold; color: #fbbf24;")
        self.weather_desc = QLabel("--")
        self.weather_desc.setAlignment(Qt.AlignCenter)
        self.weather_updated = QLabel("更新时间: --")
        self.weather_updated.setAlignment(Qt.AlignCenter)
        self.weather_updated.setStyleSheet("color: #9ca3af; font-size: 11px;")
        for widget in (self.weather_city, self.weather_icon, self.weather_temp,
                       self.weather_desc, self.weather_updated):
            v.addWidget(widget)
        return card

    def build_protocol_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)
        self.combo = QComboBox()
        self.combo.addItems([
            "*SET:DATE YEAR MONTH DATE 2026 06 01",
            "*SET:DATE YEAR DATE 2026 01",
            "*SET:DATE MONTH DATE 06 01",
            "*SET:TIME HOUR MIN SEC 12 30 45",
            "*SET:TIME HOUR SEC 12 45",
            "*SET:WEA 25 SUN",
            "*NTP SYNC",
            "*GET:TIME",
        ])
        layout.addWidget(self.combo)
        layout.addWidget(self.button("发送选中", lambda: self.send_command(self.combo.currentText())))
        # 缩写演示: 子命令 DISPlay→DISP, 参数 MINute→MIN / SECond→SEC 均为合法缩写
        layout.addWidget(self.button("缩写演示", lambda: self.send_command("*SET:DISP ON")))
        layout.addWidget(self.button("大小写混合演示", lambda: self.send_command("*sEt:FoRmAt rIgHt")))
        self.raw_edit = QLineEdit("*PING")
        layout.addWidget(self.raw_edit)
        layout.addWidget(self.button("发送原始指令", lambda: self.send_command(self.raw_edit.text())))
        layout.addStretch(1)
        return w

    def build_data_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)
        self.chart = ChartWidget()
        layout.addWidget(self.chart, 1)
        row = QHBoxLayout()
        row.addWidget(self.button("刷新图表", self.refresh_chart))
        row.addWidget(self.button("导出 CSV", self.export_csv))
        layout.addLayout(row)
        self.refresh_chart()
        return w

    def build_log_panel(self):
        box = QGroupBox("日志")
        box.setMinimumWidth(300)
        layout = QVBoxLayout(box)
        self.show_heartbeat = QCheckBox("显示心跳")
        self.show_heartbeat.setChecked(False)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(1000)
        layout.addWidget(self.show_heartbeat)
        layout.addWidget(self.log)
        row = QHBoxLayout()
        row.addWidget(self.button("导出", self.export_log))
        row.addWidget(self.button("清空", self.log.clear))
        layout.addLayout(row)
        return box

    def spin(self, minimum, maximum, value):
        box = QSpinBox()
        box.setRange(minimum, maximum)
        box.setValue(value)
        box.setSizePolicy(QSizePolicy.Minimum, QSizePolicy.Fixed)
        return box

    def row_label(self, text):
        label = QLabel(text)
        label.setFixedWidth(48)
        return label

    def field_spin(self, minimum, maximum, value):
        box = self.spin(minimum, maximum, value)
        box.setFixedWidth(64)
        return box

    def wide_button(self, text, slot):
        btn = self.button(text, slot)
        btn.setMinimumWidth(64)
        btn.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        return btn

    def refresh_ports(self):
        current = self.port_combo.currentText() or DEFAULT_PORT
        self.port_combo.clear()
        ports = SerialWorker.list_ports()
        devices = [p.device for p in ports]
        preferred = next((p.device for p in ports if "USB" in p.description.upper() or "CH340" in p.description.upper()), None)
        if DEFAULT_PORT not in devices:
            devices.insert(0, DEFAULT_PORT)
        self.port_combo.addItems(devices)
        self.port_combo.setCurrentText(preferred or (current if current in devices else devices[0]))

    def toggle_connect(self):
        if self.worker and self.worker.isRunning():
            self.worker.close()
            return
        self.worker = SerialWorker()
        self.worker.line_received.connect(self.on_received)
        self.worker.connection_changed.connect(self.on_serial_state)
        self.worker.latency_updated.connect(self.update_latency)
        self.worker.error.connect(self.on_serial_error)
        self.worker.open(self.port_combo.currentText(), int(self.baud_combo.currentText()))

    def on_serial_state(self, connected):
        self.state.connected = connected
        self.state.online = connected
        self.connect_btn.setText("关闭" if connected else "打开")
        self.add_log("SYS", "已打开" if connected else "已关闭")
        if connected:
            self.set_controls_enabled(True)
            self.heartbeat_timer.start(1000)
            self.last_pong_time = time.time()
            self.send_command("*GET:FORMAT")
            self.send_command("*GET:ALARM")
            self.send_command("*GET:COUNTDOWN")
        else:
            self.heartbeat_timer.stop()
            self._cd_ring_timer.stop()
            self.latency_label.setText("延迟: -- ms")
        self.update_connection_led()
        self.update_status()

    def heartbeat_tick(self):
        if not self.state.connected:
            return
        if self.last_pong_time and (time.time() - self.last_pong_time) > 3.0:
            self.state.online = False
            self.update_connection_led()
            self.update_status()
        self.ping()

    def send_command(self, line: str):
        line = line.strip()
        if not line:
            return
        if any(ord(ch) > 127 for ch in line):
            QMessageBox.warning(self, "仅限 ASCII", "协议文本必须为 ASCII 字符。")
            return
        self.add_log("TX", f"-> {line}")
        if self.worker and self.worker.isRunning():
            self.worker.write_line(line)
            self._request_state_after_set(line)
        else:
            self.add_log("ERR", "not connected", popup=False)

    def _request_state_after_set(self, line: str):
        # 这些 *SET 命令 MCU 只回 OK(无数据), 故发送后补一次对应 *GET, 使单选
        # 按钮与状态栏即时反映新状态(无论命令来自协议 Tab/原始框/单选/虚拟键).
        # *SET:KEY FORMAT 由 on_virtual_key 单独补查询; *SET:MODE 走 *EVT:MODE.
        norm = line.upper().replace(" ", "").replace("\t", "")
        if norm.startswith("*GET"):
            return
        if norm.startswith("*SET:FORMAT"):
            self.send_command("*GET:FORMAT")
        elif norm.startswith("*SET:DISP"):
            self.send_command("*GET:DISPLAY")
        elif norm.startswith("*SET:ALARM"):
            self.send_command("*GET:ALARM")

    def on_virtual_key(self, name: str):
        # 短按 USER1 = 请求 PC 对时(FAQ Q9): 由 PC 直接发起 NTP 流程,
        # 不下发 *SET:KEY USER1(那会触发板上的 NTP 状态短显, 属长按职责).
        if name == "USER1":
            self.ntp_sync()
            return
        self.send_command(f"*SET:KEY {name}")
        # *SET:KEY 不回 *EVT:KEY(防环回), 故 FORMAT 切换后主动查询以更新状态栏
        # (_request_state_after_set 只认 *SET:FORMAT, 不认 *SET:KEY FORMAT).
        if name == "FORMAT":
            self.send_command("*GET:FORMAT")

    def on_virtual_long_key(self, name: str):
        if name == "FUNC":
            self.send_command("*SET:KEY SAVE")
        elif name == "ADD":
            self.send_command("*SET:KEY ADD")
            QTimer.singleShot(200, lambda: self.send_command("*SET:KEY ADD"))
            QTimer.singleShot(400, lambda: self.send_command("*SET:KEY ADD"))
        elif name == "USER1":
            # 长按 USER1 = 板上 NTP 状态短显(FAQ Q13), 通过 *SET:KEY USER1 触发.
            self.send_command("*SET:KEY USER1")
        else:
            self.on_virtual_key(name)

    def on_received(self, line: str):
        kind = "EVT" if line.startswith("*EVT:") else ("ERR" if line.startswith("ERROR") else "RX")
        if self.show_heartbeat.isChecked() or not is_heartbeat(line):
            self.add_log(kind, f"<- {line}")
        try:
            self.handle_protocol_line(line)
        except Exception as exc:
            self.add_log("ERR", f"[PARSE ERR] {line} ({exc})")
        self.update_status()

    def handle_protocol_line(self, line: str):
        if line.startswith("*PONG"):
            self.last_pong_time = time.time()
            self.state.online = True
            if self.last_ping_time:
                self.worker.latency_updated.emit(int((time.time() - self.last_ping_time) * 1000))
            self.update_connection_led()
            return
        if line.startswith("*EVT:DISP"):
            text, dp = parse_disp_event(line)
            self.twin.update_digits(text, dp)
            self.store.append("DISP", text.strip())
            return
        if line.startswith("*EVT:LED"):
            self.twin.update_leds(parse_led_event(line))
            self.store.append("LED", line.split()[-1])
            return
        if line.startswith("*EVT:CD"):
            self.handle_cd_event(line)
            return
        if line.startswith("*EVT:KEY"):
            name = line.split()[-1]
            self.twin.pulse_key(name)
            self.store.append("KEY", name)
            if name == "USER1":
                self.ntp_sync()
            elif name in ("FORMAT", "K7"):
                self.send_command("*GET:FORMAT")
            return
        if line.startswith("*EVT:EDIT"):
            self.store.append("EDIT", line[len("*EVT:EDIT "):].strip())
            self.refresh_chart()
            return
        if line.startswith("*EVT:ALARM"):
            self.state.alarm = "ON" if line.strip() == "*EVT:ALARM" else "OFF"
            self.store.append("ALARM", line)
            if line.strip() == "*EVT:ALARM":
                QMessageBox.information(self, "闹钟", "S800 闹钟正在响铃。")
            return
        if line.startswith("*EVT:MODE"):
            if "NIGHT" in line:
                self.state.mode = "NIGHT"
            elif "DAY" in line:
                self.state.mode = "DAY"
            self.update_status()
            return
        if line.startswith("ERROR"):
            self.status.showMessage(line)
            return
        if line.startswith("OK"):
            self.handle_ok(line)

    def handle_ok(self, line: str):
        parts = line.split(maxsplit=1)
        if len(parts) < 2:
            return
        payload = parts[1].strip()
        upper = payload.upper()
        cd_status = parse_cd_status_payload(payload)
        if cd_status:
            self.apply_cd_status(*cd_status)
            return
        # FORMAT 应答: RIGHT 模式下 MCU 会把 LEFT/RIGHT 逆序成 TFEL/THGIR.
        # 此时 PC 可能尚未同步 FORMAT(首连引导), 故两种朝向都识别.
        if upper in ("LEFT", "TFEL"):
            self.state.fmt = "LEFT"
            self.fmt_left.setChecked(True)
            self.update_status()
            return
        if upper in ("RIGHT", "THGIR"):
            self.state.fmt = "RIGHT"
            self.fmt_right.setChecked(True)
            self.update_status()
            return
        # 其它 *GET 应答: 文档规定 RIGHT 模式下整串逆序, 按当前已知 FORMAT 还原.
        if self.state.fmt == "RIGHT":
            payload = payload[::-1]
        # TIME 应答 HH.MM.SS 用于昼夜判断
        if self._pending_daynight:
            parts_dot = payload.split(".")
            if len(parts_dot) == 3 and all(len(p) == 2 and p.isdigit() for p in parts_dot):
                self._pending_daynight = False
                self._do_apply_daynight(int(parts_dot[0]), int(parts_dot[1]))
        tokens = payload.split()
        if len(tokens) == 1 and len(tokens[0]) == 2 and all(c in "0123456789ABCDEF" for c in tokens[0].upper()):
            self.twin.update_leds(int(tokens[0], 16))
        elif len(tokens) == 1 and tokens[0].upper() in ("ON", "OFF"):
            # 单 token 的 ON/OFF 是 *GET:DISPLAY 应答, 更新显示单选按钮(非闹钟状态).
            on = tokens[0].upper() == "ON"
            (self.disp_on if on else self.disp_off).setChecked(True)
        elif len(tokens) >= 2 and tokens[-1].upper() in ("ON", "OFF"):
            # 仅闹钟应答形如 "HH.MM.SS ON/OFF"(>=2 token); 单 token 的 ON/OFF 属
            # *GET:DISPLAY, 不能用来更新闹钟状态, 否则首连查询显示会误置闹钟为 ON.
            self.state.alarm = tokens[-1].upper()
            self.update_status()

    def _cd_ring_tick(self):
        """兜底递减: MCU 的 1Hz STATE 丢失时才临时推进一次。"""
        remain = max(0, self.countdown_ring.remain - 1)
        self.countdown_ring.update_state("RUN", remain, self.countdown_ring.total, self.cd_scene)
        # 本地语音倒数
        if 1 <= remain <= 5 and remain != self.cd_last_spoken:
            self.cd_last_spoken = remain
            if self.cd_voice_check.isChecked():
                self.tts.say(str(remain))

    def handle_cd_event(self, line: str):
        body = line[len("*EVT:CD"):].strip()
        if body == "DONE":
            self._cd_ring_timer.stop()
            self.cd_state = "DONE"
            self.store.append("CD", "DONE")
            self.countdown_ring.update_state("DONE", 0, self.countdown_ring.total, self.cd_scene)
            if self.cd_voice_check.isChecked():
                if self.cd_scene == 0:
                    self.tts.say(self.cd_msg_edit.text().lower())
                else:
                    self.tts.say("time is up")
            self.cd_last_spoken = -1
            return
        parsed = parse_cd_event(line)
        if not parsed:
            return
        self.apply_cd_status(*parsed)

    def apply_cd_status(self, state: str, remain: int, total: int, scene: int):
        self.cd_state = state
        self.cd_scene = scene
        if scene != self.cd_scene_combo.currentIndex():
            self.cd_scene_combo.blockSignals(True)
            self.cd_scene_combo.setCurrentIndex(scene)
            self.cd_scene_combo.blockSignals(False)
        self.countdown_ring.update_state(state, remain, total, scene)
        self.cd_pause_btn.setText("继续" if state == "PAUSE" else "暂停")
        # RUN 态以 MCU 每秒 STATE 为主; 本地定时器只作 1.2s 无事件兜底。
        if state == "RUN":
            if 1 <= remain <= 5 and remain != self.cd_last_spoken:
                self.cd_last_spoken = remain
                if self.cd_voice_check.isChecked():
                    self.tts.say(str(remain))
            elif remain > 5:
                self.cd_last_spoken = remain
            self._cd_ring_timer.start(1200)
        else:
            self._cd_ring_timer.stop()

    def add_log(self, kind: str, text: str, popup=False):
        colors = {"TX": "#0064C8", "RX": "#009600", "EVT": "#8A2BE2", "ERR": "#C80000", "SYS": "#888888"}
        # 仅当用户停留在底部时才自动跟随; 拖动到上方查看历史时保持当前位置.
        scrollbar = self.log.verticalScrollBar()
        at_bottom = scrollbar.value() >= scrollbar.maximum() - 4
        prev_value = scrollbar.value()
        fmt = QTextCharFormat()
        fmt.setForeground(QColor(colors.get(kind, "#DDDDDD")))
        stamp = dt.datetime.now().strftime("[%H:%M:%S.%f]")[:-3] + "]"
        cursor = QTextCursor(self.log.document())
        cursor.movePosition(QTextCursor.End)
        cursor.setCharFormat(fmt)
        prefix = "" if self.log.document().isEmpty() else "\n"
        cursor.insertText(f"{prefix}{stamp} {kind} {text}")
        if at_bottom:
            scrollbar.setValue(scrollbar.maximum())
        else:
            scrollbar.setValue(min(prev_value, scrollbar.maximum()))
        if popup and kind == "ERR":
            QMessageBox.warning(self, "错误", text)

    def ping(self):
        if not (self.worker and self.worker.isRunning()):
            return
        self.last_ping_time = time.time()
        self.send_command("*PING")

    def set_date(self):
        fields = self.date_combo.currentText().split()
        values = {"YEAR": f"{self.year_spin.value():04d}", "MONTH": f"{self.month_spin.value():02d}", "DATE": f"{self.day_spin.value():02d}"}
        self.send_command(f"*SET:DATE {' '.join(fields)} {' '.join(values[f] for f in fields)}")

    def set_time(self):
        self.send_command(f"*SET:TIME HOUR MIN SEC {self.hour_spin.value():02d} {self.min_spin.value():02d} {self.sec_spin.value():02d}")

    def set_alarm(self):
        self.send_command(f"*SET:ALARM HOUR MIN SEC {self.alarm_h_spin.value():02d} {self.alarm_m_spin.value():02d} {self.alarm_s_spin.value():02d}")

    def send_msg(self):
        self.send_command(f"*SET:MSG {self.msg_edit.text()}")

    def reset_mcu(self):
        if QMessageBox.question(self, "复位", "复位 MCU 的时钟/日期/闹钟状态？") == QMessageBox.Yes:
            self.send_command("*RST")

    def ntp_sync(self):
        if self.net_worker and self.net_worker.isRunning():
            return
        self.net_worker = NetworkWorker("ntp")
        self.net_worker.done.connect(self.on_network_done)
        self.net_worker.failed.connect(self.on_network_failed)
        self.net_worker.start()

    def fetch_weather(self):
        if self.net_worker and self.net_worker.isRunning():
            return
        self.net_worker = NetworkWorker("weather")
        self.net_worker.done.connect(self.on_network_done)
        self.net_worker.failed.connect(self.on_network_failed)
        self.net_worker.start()

    def on_network_done(self, kind, data):
        if kind == "ntp":
            now, delta_ms, server = data
            self.send_command(f"*SET:DATE YEAR MONTH DATE {now.year:04d} {now.month:02d} {now.day:02d}")
            self.send_command(f"*SET:TIME HOUR MIN SEC {now.hour:02d} {now.minute:02d} {now.second:02d}")
            self.send_command("*NTP SYNC")
            self.store.append("SYNC", f"delta {delta_ms}")
            self.add_log("SYS", f"NTP sync {server} delta {delta_ms} ms")
        elif kind == "weather":
            temp, cond, desc = data
            self.weather_icon.setText(self.WEATHER_ICONS.get(cond, "?"))
            self.weather_temp.setText(f"{temp}°C")
            self.weather_desc.setText(f"{desc} ({cond})")
            self.weather_updated.setText(f"更新时间: {dt.datetime.now():%H:%M:%S}")
            self.send_command(f"*SET:WEA {temp} {cond}")
            self.store.append("WEATHER", f"{temp} {cond}")
            self.add_log("SYS", f"weather {temp}C {cond}")
        self.refresh_chart()

    def on_network_failed(self, kind, message):
        self.add_log("ERR", f"{kind} failed: {message}", popup=True)

    def force_mode(self, mode: str):
        self.auto_mode_check.setChecked(False)
        self.send_command(f"*SET:MODE {mode}")

    def apply_daynight(self):
        self._pending_daynight = True
        self.send_command("*GET:TIME")

    def _do_apply_daynight(self, hour: int, minute: int):
        try:
            mode, sunrise, sunset = current_daynight(hour=hour, minute=minute)
            self.sun_label.setText(f"日升 / 日落: {sunrise:%H:%M} / {sunset:%H:%M}")
            self.send_command(f"*SET:MODE {mode}")
        except Exception as exc:
            self.add_log("ERR", f"day/night failed: {exc}")

    def auto_daynight(self):
        if not self.auto_mode_check.isChecked():
            return
        self.apply_daynight()

    def refresh_chart(self):
        self.chart.update_from_rows(self.store.rows())

    def export_csv(self):
        path, _ = QFileDialog.getSaveFileName(self, "导出 CSV", "events.csv", "CSV (*.csv)")
        if not path:
            return
        rows = self.store.rows()
        with open(path, "w", encoding="utf-8-sig") as file:
            file.write("timestamp,type,data\n")
            for row in rows:
                file.write(f"{row.get('timestamp','')},{row.get('type','')},{row.get('data','')}\n")

    def export_log(self):
        path, _ = QFileDialog.getSaveFileName(self, "导出日志", "s800_log.txt", "Text (*.txt)")
        if path:
            with open(path, "w", encoding="utf-8") as file:
                file.write(self.log.toPlainText())

    def update_connection_led(self):
        self.conn_led.setObjectName("conn_on" if self.state.connected and self.state.online else "conn_off")
        self.conn_led.style().unpolish(self.conn_led)
        self.conn_led.style().polish(self.conn_led)

    def update_latency(self, ms: int):
        self.state.latency_ms = ms
        self.latency_label.setText(f"延迟: {ms} ms")
        self.update_status()

    def on_serial_error(self, msg: str):
        self.add_log("ERR", msg, popup=True)
        # 串口被占用 / 打开失败: 禁用控制面板与镜像按键, 直到重新连接成功
        if "open" in msg.lower() or "occupied" in msg.lower() or "denied" in msg.lower():
            self.set_controls_enabled(False)
            self.status.showMessage(msg)

    def set_controls_enabled(self, enabled: bool):
        if hasattr(self, "control_tabs"):
            self.control_tabs.setEnabled(enabled)
        self.twin.setEnabled(enabled)

    def update_status(self):
        self.mode_label.setText(f"当前模式: {self.state.mode}")
        self.status.showMessage(
            f"{'已连接' if self.state.connected else '未连接'} | "
            f"{'在线' if self.state.online else '离线'} | "
            f"格式 {self.state.fmt} | 模式 {self.state.mode} | "
            f"闹钟 {self.state.alarm} | 延迟 {self.state.latency_ms} ms"
        )

    def closeEvent(self, event):
        if self.worker and self.worker.isRunning():
            self.worker.close()
        event.accept()


def install_excepthook():
    import traceback

    def hook(exc_type, exc, tb):
        message = "".join(traceback.format_exception(exc_type, exc, tb))
        sys.stderr.write(message)
        try:
            QMessageBox.critical(None, "意外错误", str(exc))
        except Exception:
            pass

    sys.excepthook = hook


def main():
    app = QApplication(sys.argv)
    install_excepthook()
    app.setStyleSheet("""
        QWidget { background: #111827; color: #e5e7eb; font-size: 13px; }
        QGroupBox { border: 1px solid #374151; border-radius: 6px; margin-top: 8px; padding: 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QPushButton { background: #1f2937; border: 1px solid #4b5563; border-radius: 4px; padding: 6px 8px; }
        QPushButton:hover { background: #374151; }
        QPushButton[pulse="true"] { background: #2563eb; border-color: #93c5fd; }
        QLineEdit, QComboBox, QSpinBox, QPlainTextEdit { background: #0b1220; border: 1px solid #374151; border-radius: 4px; padding: 4px; }
        QTabWidget::pane { border: 1px solid #374151; }
        QTabBar::tab { background: #1f2937; padding: 7px 10px; border: 1px solid #374151; }
        QTabBar::tab:selected { background: #374151; }
        QLabel#led_on {
            background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, stop:0 #FFFF80, stop:1 #FFC800);
            border-radius: 15px; border: 1px solid #888; color: #111827;
        }
        QLabel#led_off {
            background: #333; border-radius: 15px; border: 1px solid #888; color: #d1d5db;
        }
        QLabel#conn_on { background: #22c55e; border-radius: 9px; border: 1px solid #86efac; }
        QLabel#conn_off { background: #4b5563; border-radius: 9px; border: 1px solid #9ca3af; }
        QScrollBar:vertical { background: #0b1220; width: 14px; margin: 0; }
        QScrollBar::handle:vertical { background: #4b5563; border-radius: 5px; min-height: 24px; }
        QScrollBar::handle:vertical:hover { background: #6b7280; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    """)
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
