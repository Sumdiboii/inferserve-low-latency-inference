#!/usr/bin/env python3
"""Plot latency percentiles and completed-request throughput from load-generator CSVs.

Usage: python3 scripts/plot_benchmark_results.py --synthetic synthetic.csv --onnx onnx.csv
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Iterable


BACKENDS = ("Synthetic", "ONNX Runtime CPU")
COLORS = ("#2878b5", "#e1812c")
REQUIRED_COLUMNS = {"scheduled_arrival_ms", "completion_ms", "error"}


def load_metrics(path: Path) -> tuple[list[float], float | None]:
    """Return scheduled-arrival latency samples and completed-dispatches/second."""
    latencies: list[float] = []
    completion_times: list[float] = []
    scheduled_arrivals: set[float] = set()
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None or not REQUIRED_COLUMNS.issubset(reader.fieldnames):
            missing = sorted(REQUIRED_COLUMNS.difference(reader.fieldnames or []))
            raise ValueError(f"{path}: missing CSV columns: {', '.join(missing)}")
        has_warmup_column = "warmup" in (reader.fieldnames or [])
        for line_number, row in enumerate(reader, start=2):
            if has_warmup_column and (row["warmup"] or "").strip().lower() == "true":
                continue
            try:
                scheduled = float(row["scheduled_arrival_ms"])
                completion = float(row["completion_ms"])
            except (TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line_number}: invalid timestamp") from error

            scheduled_arrivals.add(scheduled)
            # The executor records queue-overflow arrivals with an explicit error and
            # dispatched=false; CSV does not otherwise have a dispatched column.
            error_text = row["error"] or ""
            dispatched = not error_text.startswith("request not dispatched:")
            if dispatched and completion >= scheduled:
                latencies.append(completion - scheduled)
                completion_times.append(completion)

    throughput = None
    # A one-request run has no sample window, and a same-time burst measures drain
    # time rather than sustained throughput. Leave both undefined instead of
    # presenting a rate that invites comparison with steady offered-load runs.
    if len(scheduled_arrivals) > 1 and completion_times:
        if has_warmup_column:
            earliest_measured = min(scheduled_arrivals)
            window_ms = max(completion_times) - earliest_measured
            if window_ms > 0.0:
                throughput = len(completion_times) * 1000.0 / window_ms
        elif max(completion_times) > 0.0:
            throughput = len(completion_times) * 1000.0 / max(completion_times)
    return latencies, throughput


def nearest_rank(values: Iterable[float], percentile: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _svg_start(title: str, width: int, height: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{width / 2:g}" y="32" text-anchor="middle" font-family="sans-serif" font-size="20">{_escape(title)}</text>',
    ]


def _text(x: float, y: float, value: str, *, anchor: str = "middle", size: int = 13) -> str:
    return (f'<text x="{x:g}" y="{y:g}" text-anchor="{anchor}" font-family="sans-serif" '
            f'font-size="{size}">{_escape(value)}</text>')


def write_latency_plot(path: Path, samples: list[list[float]]) -> None:
    width, height = 720, 440
    left, right, top, bottom = 82, 24, 62, 96
    chart_width, chart_height = width - left - right, height - top - bottom
    percentiles = (0.50, 0.95, 0.99)
    values = [[nearest_rank(sample, p) for p in percentiles] for sample in samples]
    present = [value for series in values for value in series if value is not None]
    maximum = max(present, default=1.0) or 1.0
    maximum *= 1.15
    parts = _svg_start("Request latency by backend", width, height)
    parts.append(_text(22, top + chart_height / 2, "Latency (ms)", size=14))
    parts[-1] = parts[-1].replace('text-anchor="middle"', 'text-anchor="middle" transform="rotate(-90 22 ' + f'{top + chart_height / 2:g}' + ')"')
    for tick in range(5):
        value = maximum * tick / 4
        y = top + chart_height * (1 - tick / 4)
        parts.append(f'<line x1="{left}" y1="{y:g}" x2="{left + chart_width}" y2="{y:g}" stroke="#dddddd"/>')
        parts.append(_text(left - 10, y + 4, f"{value:.1f}", anchor="end", size=11))
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + chart_height}" stroke="#333333"/>')
    parts.append(f'<line x1="{left}" y1="{top + chart_height}" x2="{left + chart_width}" y2="{top + chart_height}" stroke="#333333"/>')
    group_width = chart_width / len(percentiles)
    bar_width = 46
    for p_index, percentile in enumerate(percentiles):
        center = left + group_width * (p_index + 0.5)
        parts.append(_text(center, top + chart_height + 24, f"p{int(percentile * 100)}"))
        for backend_index, series in enumerate(values):
            value = series[p_index]
            if value is None:
                continue
            x = center + (backend_index - 0.5) * (bar_width + 5) - bar_width / 2
            bar_height = chart_height * value / maximum
            y = top + chart_height - bar_height
            parts.append(f'<rect x="{x:g}" y="{y:g}" width="{bar_width}" height="{bar_height:g}" fill="{COLORS[backend_index]}"/>')
            parts.append(_text(x + bar_width / 2, y - 5, f"{value:.2f}", size=10))
    legend_x = left + 105
    for index, backend in enumerate(BACKENDS):
        x = legend_x + index * 210
        parts.append(f'<rect x="{x}" y="{height - 39}" width="14" height="14" fill="{COLORS[index]}"/>')
        parts.append(_text(x + 21, height - 27, backend, anchor="start", size=12))
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def write_throughput_plot(path: Path, rates: list[float | None]) -> None:
    width, height = 560, 400
    left, right, top, bottom = 82, 30, 62, 86
    chart_width, chart_height = width - left - right, height - top - bottom
    maximum = max((rate for rate in rates if rate is not None), default=1.0) or 1.0
    maximum *= 1.15
    parts = _svg_start("Completed-dispatch throughput by backend", width, height)
    parts.append(_text(22, top + chart_height / 2, "Requests per second", size=14))
    parts[-1] = parts[-1].replace('text-anchor="middle"', 'text-anchor="middle" transform="rotate(-90 22 ' + f'{top + chart_height / 2:g}' + ')"')
    for tick in range(5):
        value = maximum * tick / 4
        y = top + chart_height * (1 - tick / 4)
        parts.append(f'<line x1="{left}" y1="{y:g}" x2="{left + chart_width}" y2="{y:g}" stroke="#dddddd"/>')
        parts.append(_text(left - 10, y + 4, f"{value:.1f}", anchor="end", size=11))
    parts.append(f'<line x1="{left}" y1="{top + chart_height}" x2="{left + chart_width}" y2="{top + chart_height}" stroke="#333333"/>')
    for index, (backend, rate) in enumerate(zip(BACKENDS, rates)):
        center = left + chart_width * (index + 0.5) / len(BACKENDS)
        if rate is not None:
            bar_height = chart_height * rate / maximum
            y = top + chart_height - bar_height
            parts.append(f'<rect x="{center - 45:g}" y="{y:g}" width="90" height="{bar_height:g}" fill="{COLORS[index]}"/>')
            parts.append(_text(center, y - 6, f"{rate:.2f}"))
        else:
            parts.append(_text(center, top + chart_height / 2, "n/a"))
        parts.append(_text(center, top + chart_height + 25, backend))
    parts.append(_text(width / 2, height - 23, "Dispatched requests completed by generator / elapsed seconds", size=11))
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synthetic", required=True, type=Path, help="Synthetic backend request-record CSV")
    parser.add_argument("--onnx", required=True, type=Path, help="ONNX Runtime CPU request-record CSV")
    parser.add_argument("--output-dir", type=Path, default=Path("plots"), help="Directory for SVG plots (default: plots)")
    arguments = parser.parse_args()
    try:
        synthetic = load_metrics(arguments.synthetic)
        onnx = load_metrics(arguments.onnx)
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        write_latency_plot(arguments.output_dir / "latency_percentiles.svg", [synthetic[0], onnx[0]])
        write_throughput_plot(arguments.output_dir / "throughput.svg", [synthetic[1], onnx[1]])
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Wrote SVG plots to {arguments.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
