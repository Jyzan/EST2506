from collections import Counter, defaultdict

import matplotlib
from PyQt5.QtWidgets import QWidget, QVBoxLayout
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

matplotlib.rcParams["font.family"] = "Times New Roman"


class ChartWidget(QWidget):
    def __init__(self):
        super().__init__()
        self.canvas = None
        self._init_canvas()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.canvas)

    def _init_canvas(self):
        self.figure = Figure(figsize=(6, 9))
        self.canvas = FigureCanvas(self.figure)

    @staticmethod
    def _style_axis(ax):
        ax.grid(True, color="#cccccc", linewidth=0.6, alpha=0.5)
        ax.set_axisbelow(True)
        for spine in ax.spines.values():
            spine.set_linewidth(1.8)

    def _replace_canvas(self):
        old = self.canvas
        self._init_canvas()
        self.layout().replaceWidget(old, self.canvas)
        old.deleteLater()

    def update_from_rows(self, rows):
        self._replace_canvas()
        ax1 = self.figure.add_subplot(311)
        ax2 = self.figure.add_subplot(312)
        ax3 = self.figure.add_subplot(313)
        axes = [ax1, ax2, ax3]
        for ax in axes:
            self._style_axis(ax)

        if not rows:
            for ax in axes:
                ax.text(0.5, 0.5, "No data", ha="center", va="center")
                ax.set_xticks([])
                ax.set_yticks([])
            self.figure.tight_layout(h_pad=2.5)
            self.canvas.draw()
            return

        key_counts = Counter()
        alarm_points = []
        sync_points = []
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

        # 图表 1 闹钟触发时间分布
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
            ax.set_xticklabels(dates, rotation=0, ha="center", fontsize=8)
            ax.set_xlim(-0.5, len(dates) - 0.5)
        else:
            ax.text(0.5, 0.5, "No alarm events", ha="center", va="center", transform=ax.transAxes)
            ax.set_xticks([])
            ax.set_yticks([])

        # 图表 2 NTP 对时误差
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
            # 日期较少时收窄柱宽 并把横轴留白拉宽 避免单根柱子显得过粗
            bar_w = 0.5 if len(dates) >= 4 else 0.28
            ax.bar(list(xs), ys, width=bar_w, color="#22c55e", edgecolor="#0f5132")
            ax.set_xticks(list(xs))
            ax.set_xticklabels(dates, rotation=0, ha="center", fontsize=8)
            pad = 0.5 if len(dates) >= 4 else 1.0
            ax.set_xlim(-pad, len(dates) - 1 + pad)
            ax.axhline(0, color="#888888", linewidth=0.8)
        else:
            ax.text(0.5, 0.5, "No sync events", ha="center", va="center", transform=ax.transAxes)
            ax.set_xticks([])
            ax.set_yticks([])

        # 图表 3 按键热度
        ax = axes[2]
        ax.set_title("Key press heat")
        ax.set_xlabel("Count")
        if key_counts:
            names = [n for n, _ in key_counts.most_common()]
            values = [key_counts[n] for n in names]
            ypos = range(len(names))
            # 按键较少时收窄柱高 并把纵轴留白拉宽 避免单根柱子显得过粗
            bar_h = 0.6 if len(names) >= 4 else 0.35
            ax.barh(list(ypos), values, height=bar_h, color="#3b82f6", edgecolor="#1e3a8a")
            ax.set_yticks(list(ypos))
            ax.set_yticklabels(names)
            pad = 0.5 if len(names) >= 4 else 1.0
            ax.set_ylim(-pad, len(names) - 1 + pad)
            ax.invert_yaxis()
            ax.set_xlim(0, max(values) * 1.15 + 1)
        else:
            ax.text(0.5, 0.5, "No key events", ha="center", va="center", transform=ax.transAxes)
            ax.set_xticks([])
            ax.set_yticks([])

        self.figure.tight_layout(h_pad=2.5)
        self.canvas.draw()
