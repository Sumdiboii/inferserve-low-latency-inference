"""Create Phase 10.2 comparison charts from the runner's summary JSON."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
summary = json.loads((ROOT / "experiment-summary.json").read_text())
configs = summary["configurations"]
colors = ("#2878b5", "#e1812c", "#55a868")


def text(x, y, value, anchor="middle", size=12):
    escaped = str(value).replace("&", "&amp;").replace("<", "&lt;")
    return f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-family="sans-serif" font-size="{size}">{escaped}</text>'


def start(title, width, height):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">', '<rect width="100%" height="100%" fill="white"/>', text(width / 2, 30, title, size=18)]


def latency_plot():
    width, height = 760, 450
    left, top, chart_w, chart_h = 80, 60, 650, 300
    metrics = ("p50", "p95", "p99")
    values = [[c["aggregate"]["latency_ms"][m]["median"] for m in metrics] for c in configs]
    maximum = max(max(row) for row in values) * 1.12
    parts = start("Completion latency by maximum batching delay", width, height)
    parts.append(text(20, top + chart_h / 2, "Latency (ms)", size=13))
    for tick in range(5):
        v = maximum * tick / 4
        y = top + chart_h * (1 - tick / 4)
        parts.extend([f'<line x1="{left}" y1="{y}" x2="{left + chart_w}" y2="{y}" stroke="#ddd"/>', text(left - 10, y + 4, f"{v:.0f}", "end", 11)])
    group = chart_w / len(metrics)
    bw = 36
    for j, metric in enumerate(metrics):
        center = left + group * (j + 0.5)
        parts.append(text(center, top + chart_h + 24, metric))
        for i, config in enumerate(configs):
            v = values[i][j]
            x = center + (i - 1) * 42 - bw / 2
            bh = chart_h * v / maximum
            y = top + chart_h - bh
            parts.extend([f'<rect x="{x}" y="{y}" width="{bw}" height="{bh}" fill="{colors[i]}"/>', text(x + bw / 2, y - 5, f"{v:.1f}", size=10)])
    for i, config in enumerate(configs):
        x = 190 + i * 150
        parts.extend([f'<rect x="{x}" y="405" width="13" height="13" fill="{colors[i]}"/>', text(x + 20, 416, config["name"], "start", 11)])
    parts.append("</svg>")
    (ROOT / "latency_percentiles.svg").write_text("\n".join(parts) + "\n")


def throughput_plot():
    width, height = 620, 420
    left, top, chart_w, chart_h = 80, 60, 500, 270
    values = [c["aggregate"]["throughput_requests_per_second"]["median"] for c in configs]
    maximum = max(values) * 1.12
    parts = start("Completed-request throughput by maximum batching delay", width, height)
    for tick in range(5):
        v = maximum * tick / 4
        y = top + chart_h * (1 - tick / 4)
        parts.extend([f'<line x1="{left}" y1="{y}" x2="{left + chart_w}" y2="{y}" stroke="#ddd"/>', text(left - 10, y + 4, f"{v:.0f}", "end", 11)])
    for i, (config, value) in enumerate(zip(configs, values)):
        center = left + chart_w * (i + 0.5) / len(configs)
        bh = chart_h * value / maximum
        y = top + chart_h - bh
        parts.extend([f'<rect x="{center - 48}" y="{y}" width="96" height="{bh}" fill="{colors[i]}"/>', text(center, y - 6, f"{value:.1f}"), text(center, top + chart_h + 22, config["name"], size=11)])
    parts.extend([text(width / 2, 385, "Completed requests / second (runner estimate)", size=12), "</svg>"])
    (ROOT / "throughput.svg").write_text("\n".join(parts) + "\n")


latency_plot()
throughput_plot()
