import csv
import tempfile
import unittest
from pathlib import Path

from scripts.plot_benchmark_results import load_metrics, nearest_rank, write_latency_plot, write_throughput_plot


class BenchmarkPlotTest(unittest.TestCase):
    def test_metrics_use_scheduled_arrival_and_skip_undispatched_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "records.csv"
            with path.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=["scheduled_arrival_ms", "completion_ms", "error"])
                writer.writeheader()
                writer.writerow({"scheduled_arrival_ms": "10", "completion_ms": "30", "error": ""})
                writer.writerow({"scheduled_arrival_ms": "20", "completion_ms": "50", "error": ""})
                writer.writerow({"scheduled_arrival_ms": "30", "completion_ms": "100", "error": "request not dispatched: pending queue is full"})
            latencies, throughput = load_metrics(path)
        self.assertEqual(latencies, [20.0, 30.0])
        self.assertAlmostEqual(throughput, 40.0)

    def test_empty_latency_samples_and_nearest_rank(self):
        self.assertIsNone(nearest_rank([], 0.95))
        self.assertEqual(nearest_rank([9, 1, 5, 3], 0.50), 3)
        self.assertEqual(nearest_rank([9, 1, 5, 3], 0.95), 9)

    def test_burst_and_single_request_have_no_throughput_rate(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, rows in (
                ("burst", [(0, 10), (0, 20), (0, 30)]),
                ("single", [(0, 10)]),
            ):
                path = Path(directory) / f"{name}.csv"
                with path.open("w", newline="", encoding="utf-8") as output:
                    writer = csv.DictWriter(output, fieldnames=["scheduled_arrival_ms", "completion_ms", "error"])
                    writer.writeheader()
                    for scheduled, completion in rows:
                        writer.writerow({"scheduled_arrival_ms": scheduled, "completion_ms": completion, "error": ""})
                _, throughput = load_metrics(path)
                self.assertIsNone(throughput)

    def test_legacy_csv_without_error_column_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "legacy.csv"
            path.write_text("scheduled_arrival_ms,completion_ms\n0,10\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "missing CSV columns: error"):
                load_metrics(path)

    def test_svg_outputs_label_backend_types_and_metric_units(self):
        with tempfile.TemporaryDirectory() as directory:
            latency = Path(directory) / "latency.svg"
            throughput = Path(directory) / "throughput.svg"
            write_latency_plot(latency, [[1.0, 2.0, 3.0], [2.0, 4.0, 6.0]])
            write_throughput_plot(throughput, [12.0, 6.0])
            latency_svg = latency.read_text(encoding="utf-8")
            throughput_svg = throughput.read_text(encoding="utf-8")
        self.assertIn("Synthetic", latency_svg)
        self.assertIn("ONNX Runtime CPU", latency_svg)
        self.assertIn("Latency (ms)", latency_svg)
        self.assertIn("Synthetic", throughput_svg)
        self.assertIn("ONNX Runtime CPU", throughput_svg)
        self.assertIn("Requests per second", throughput_svg)


if __name__ == "__main__":
    unittest.main()
