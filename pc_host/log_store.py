"""事件持久化存储 以 CSV 格式记录闹钟 对时 编辑 按键四类事件"""
import csv
import datetime as dt
from pathlib import Path


class EventStore:
    """CSV 事件存储 自动补齐 UTF-8 BOM 表头 支持追加和全量读取"""
    HEADER = ("timestamp", "type", "data")

    def __init__(self, path="events.csv"):
        self.path = Path(path)
        self._ensure_header()

    def _ensure_header(self):
        # 文件缺失 或为空 或首行非表头时补写表头 防止 DictReader 把首条数据误当字段名
        if self.path.exists():
            with self.path.open("r", newline="", encoding="utf-8-sig") as file:
                first = file.readline().strip()
            if first.split(",")[:3] == list(self.HEADER):
                return
            body = self.path.read_bytes()
            bom = b"\xef\xbb\xbf"
            if body.startswith(bom):
                body = body[len(bom):]
            header = (",".join(self.HEADER) + "\r\n").encode("utf-8")
            self.path.write_bytes(bom + header + body)
            return
        with self.path.open("w", newline="", encoding="utf-8-sig") as file:
            csv.writer(file).writerow(self.HEADER)

    def append(self, event_type: str, data: str):
        with self.path.open("a", newline="", encoding="utf-8-sig") as file:
            csv.writer(file).writerow([
                # 按文档 C8 规定 时间戳为秒级 不含毫秒
                dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                event_type,
                data,
            ])

    def rows(self):
        if not self.path.exists():
            return []
        # 读取前补齐表头 防止历史无表头文件被错位解析导致图表读空
        self._ensure_header()
        with self.path.open("r", newline="", encoding="utf-8-sig") as file:
            return list(csv.DictReader(file))
