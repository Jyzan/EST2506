import csv
import datetime as dt
from pathlib import Path


class EventStore:
    HEADER = ("timestamp", "type", "data")

    def __init__(self, path="events.csv"):
        self.path = Path(path)
        if not self.path.exists():
            with self.path.open("w", newline="", encoding="utf-8-sig") as file:
                csv.writer(file).writerow(self.HEADER)

    def append(self, event_type: str, data: str):
        with self.path.open("a", newline="", encoding="utf-8-sig") as file:
            csv.writer(file).writerow([
                dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                event_type,
                data,
            ])

    def rows(self):
        if not self.path.exists():
            return []
        with self.path.open("r", newline="", encoding="utf-8-sig") as file:
            return list(csv.DictReader(file))
