import queue
import time

import serial
import serial.tools.list_ports
from PyQt5.QtCore import QThread, pyqtSignal


class SerialWorker(QThread):
    line_received = pyqtSignal(str)
    connection_changed = pyqtSignal(bool)
    latency_updated = pyqtSignal(int)
    error = pyqtSignal(str)

    # 连续两条命令之间的最小发送间隔(秒)。NTP 对时等场景会一次入队
    # DATE/TIME/NTP 多条命令, 若背靠背写出, MCU 在粘包边界易丢字节
    # (表现为 ERROR RANGE/SYNTAX)。逐条限速发送给 MCU 留出整行处理时间。
    TX_MIN_GAP = 0.02

    def __init__(self):
        super().__init__()
        self.port = ""
        self.baud = 115200
        self._running = False
        self._ser = None
        self._tx = queue.Queue()
        self._last_tx = 0.0

    @staticmethod
    def list_ports():
        return list(serial.tools.list_ports.comports())

    def open(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.start()

    def close(self):
        self._running = False
        self.wait(1500)

    def write_line(self, line: str):
        self._tx.put(line.rstrip("\r\n") + "\r\n")

    def run(self):
        self._running = True
        try:
            self._ser = serial.Serial(
                self.port,
                self.baud,
                timeout=0.1,
                write_timeout=0.4,
                dsrdtr=False,
                rtscts=False,
                xonxoff=False,
            )
            self._ser.setDTR(False)
            self._ser.setRTS(False)
            self.connection_changed.emit(True)
        except Exception as exc:
            self.error.emit(f"无法打开串口 {self.port}: {exc}")
            self.connection_changed.emit(False)
            return

        buf = bytearray()
        while self._running:
            try:
                # 逐条限速发送: 每轮最多写一条, 且与上一条间隔 >= TX_MIN_GAP,
                # 避免多条命令背靠背挤进 MCU 接收链路导致粘包丢字节。
                if not self._tx.empty() and (time.time() - self._last_tx) >= self.TX_MIN_GAP:
                    self._ser.write(self._tx.get_nowait().encode("ascii", errors="ignore"))
                    self._last_tx = time.time()
                chunk = self._ser.read(128)
                if chunk:
                    buf.extend(chunk)
                    while b"\n" in buf:
                        line, _, rest = buf.partition(b"\n")
                        buf = bytearray(rest)
                        text = line.decode("ascii", errors="replace").strip()
                        if text:
                            self.line_received.emit(text)
                else:
                    time.sleep(0.005)
            except Exception as exc:
                self.error.emit(f"串口通信异常: {exc}")
                break

        try:
            if self._ser:
                self._ser.close()
        except Exception:
            pass
        self.connection_changed.emit(False)
