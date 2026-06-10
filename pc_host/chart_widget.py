from collections import Counter, defaultdict

import matplotlib
from PyQt5.QtWidgets import QWidget, QVBoxLayout
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

matplotlib.rcParams["font.family"] = "Times New Roman"


class ChartWidget(QWidget):
    def __init__(self):
        super().__init__()
        self.figure = Figure(figsize=(6, 9), tight_layout=True)
        self.canvas = FigureCanvas(self.figure)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.canvas)

    @staticmethod
    def _style_axis(ax):
        ax.grid(True, color="#cccccc", linewidth=0.6, alpha=0.5)
        ax.set_axisbelow(True)
        for spine in ax.spines.values():
            spine.set_linewidth(1.8)

    def update_from_rows(self, rows):
        self.figure.clear()
        axes = [self.figure.add_subplot(311), self.figure.add_subplot(312), self.figure.add_subplot(313)]
        for ax in axes:
            self._style_axis(ax)

        if not rows:
            for ax in axes:
                ax.text(0.5, 0.5, "No data", ha="center", va="center")
                ax.set_xticks([])
                ax.set_yticks([])
            self.canvas.draw_idle()
            return

        key_counts = Counter()
        alarm_points = []   # (date string, trigger hour-of-day)
        sync_points = []    # (date string, delta ms)
        for row in rows:
            typ = row.get("type", "")
            data = row.get("data", "")
            stamp = row.get("timestamp", "")
            date = stamp[:10]
            if typ == "KEY":
                key_counts[data] += 1
            elif typ == "ALARM":
                try:
                    hour = int(stamp[11:13]) + int(stamp[14:16]) / 60.0
                    alarm_points.append((date, hour))
                except Exception:
                    pass
            elif typ == "SYNC":
                try:
                    sync_points.append((date, int(data.split()[-1])))
                except Exception:
                    pass

        # 1) Alarm trigger time distribution: X = date, Y = trigger hour-of-day.
        ax = axes[0]
        ax.set_title("Alarm trigger time")
        ax.set_xlabel("Date")
        ax.set_ylabel("Hour of day")
        ax.set_ylim(0, 24)
        ax.set_yticks(range(0, 25, 6))
        if alarm_points:
            dates = sorted({d for d, _ in alarm_points})
            index = {d: i for i, d in enumerate(dates)}
            xs = [index[d] for d, _ in alarm_points]
            ys = [h for _, h in alarm_points]
            ax.plot(xs, ys, marker="o", linestyle="-", color="#ef4444", linewidth=1.6)
            ax.set_xticks(range(len(dates)))
            ax.set_xticklabels(dates, rotation=30, ha="right", fontsize=8)
            ax.set_xlim(-0.5, len(dates) - 0.5)
        else:
            ax.text(0.5, 0.5, "No alarm events", ha="center", va="center", transform=ax.transAxes)

        # 2) NTP delta: X = date, Y = delta ms (mean per day).
        ax = axes[1]
        ax.set_title("NTP delta (ms)")
        ax.set_xlabel("Date")
        ax.set_ylabel("delta ms")
        if sync_points:
            by_date = defaultdict(list)
            for d, val in sync_points:
                by_date[d].append(val)
            dates = sorted(by_date)
            xs = range(len(dates))
            ys = [sum(by_date[d]) / len(by_date[d]) for d in dates]
            ax.bar(list(xs), ys, width=0.5, color="#22c55e", edgecolor="#0f5132")
            ax.set_xticks(list(xs))
            ax.set_xticklabels(dates, rotation=30, ha="right", fontsize=8)
            ax.set_xlim(-0.5, len(dates) - 0.5)
            ax.axhline(0, color="#888888", linewidth=0.8)
        else:
            ax.text(0.5, 0.5, "No sync events", ha="center", va="center", transform=ax.transAxes)

        # 3) Key heat: total presses per key as horizontal bars.
        ax = axes[2]
        ax.set_title("Key press heat")
        ax.set_xlabel("Count")
        if key_counts:
            names = [n for n, _ in key_counts.most_common()]
            values = [key_counts[n] for n in names]
            ypos = range(len(names))
            ax.barh(list(ypos), values, height=0.6, color="#3b82f6", edgecolor="#1e3a8a")
            ax.set_yticks(list(ypos))
            ax.set_yticklabels(names)
            ax.invert_yaxis()
            ax.set_xlim(0, max(values) * 1.15 + 1)
        else:
            ax.text(0.5, 0.5, "No key events", ha="center", va="center", transform=ax.transAxes)
        self.canvas.draw_idle()
