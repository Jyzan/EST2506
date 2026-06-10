from PyQt5 import QtCore, QtWidgets


class Ui_MainWindow(object):
    def setupUi(self, MainWindow):
        MainWindow.setObjectName("MainWindow")
        MainWindow.resize(1380, 820)
        MainWindow.setMinimumSize(QtCore.QSize(1280, 720))

        self.centralwidget = QtWidgets.QWidget(MainWindow)
        self.centralwidget.setObjectName("centralwidget")
        self.root_layout = QtWidgets.QVBoxLayout(self.centralwidget)
        self.root_layout.setObjectName("root_layout")

        # --- C1 connection bar ---
        self.topbar = QtWidgets.QHBoxLayout()
        self.topbar.setObjectName("topbar")
        self.conn_led = QtWidgets.QLabel(self.centralwidget)
        self.conn_led.setObjectName("conn_led")
        self.conn_led.setFixedSize(QtCore.QSize(18, 18))
        self.label_port = QtWidgets.QLabel("端口", self.centralwidget)
        self.port_combo = QtWidgets.QComboBox(self.centralwidget)
        self.port_combo.setObjectName("port_combo")
        self.port_combo.setMinimumWidth(120)
        self.label_baud = QtWidgets.QLabel("波特率", self.centralwidget)
        self.baud_combo = QtWidgets.QComboBox(self.centralwidget)
        self.baud_combo.setObjectName("baud_combo")
        self.refresh_btn = QtWidgets.QPushButton("刷新", self.centralwidget)
        self.refresh_btn.setObjectName("refresh_btn")
        self.connect_btn = QtWidgets.QPushButton("打开", self.centralwidget)
        self.connect_btn.setObjectName("connect_btn")
        self.ping_btn = QtWidgets.QPushButton("PING", self.centralwidget)
        self.ping_btn.setObjectName("ping_btn")
        self.latency_label = QtWidgets.QLabel("延迟: -- ms", self.centralwidget)
        self.latency_label.setObjectName("latency_label")
        self.latency_label.setMinimumWidth(110)
        for widget in (self.conn_led, self.label_port, self.port_combo,
                       self.label_baud, self.baud_combo, self.refresh_btn,
                       self.connect_btn, self.ping_btn, self.latency_label):
            self.topbar.addWidget(widget)
        self.topbar.addStretch(1)
        self.root_layout.addLayout(self.topbar)

        # --- content host: main.py adds the three-pane splitter here ---
        self.content_layout = QtWidgets.QVBoxLayout()
        self.content_layout.setObjectName("content_layout")
        self.root_layout.addLayout(self.content_layout, 1)

        MainWindow.setCentralWidget(self.centralwidget)
        self.statusbar = QtWidgets.QStatusBar(MainWindow)
        self.statusbar.setObjectName("statusbar")
        MainWindow.setStatusBar(self.statusbar)
        self.retranslateUi(MainWindow)
        QtCore.QMetaObject.connectSlotsByName(MainWindow)

    def retranslateUi(self, MainWindow):
        MainWindow.setWindowTitle("S800 Smart Clock Host")
