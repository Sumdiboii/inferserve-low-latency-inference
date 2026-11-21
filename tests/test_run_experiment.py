import json
import os
import tempfile
import unittest
from pathlib import Path

from scripts.run_experiment import (
    aggregate_configuration,
    build_load_generator_command,
    run_experiment,
)


class ExperimentRunnerTest(unittest.TestCase):
    def make_fake_load_generator(self, directory: Path) -> Path:
        executable = directory / "fake_load_generator.py"
        executable.write_text(
            """#!/usr/bin/env python3
import sys
from pathlib import Path

args = sys.argv[1:]
csv_index = args.index("--csv")
csv_path = Path(args[csv_index + 1])

if "fail-run" in str(csv_path):
    print("intentional load-generator failure", file=sys.stderr)
    raise SystemExit(7)

csv_path.parent.mkdir(parents=True, exist_ok=True)
csv_path.write_text(
    "request_index,scheduled_arrival_ms,completion_ms,error,warmup\\n"
    "0,0,10,,false\\n"
    "1,100,120,,false\\n",
    encoding="utf-8",
)
Path(str(csv_path) + ".metadata.json").write_text(
    '{"traffic_pattern": "burst"}\\n',
    encoding="utf-8",
)
""",
            encoding="utf-8",
        )
        executable.chmod(executable.stat().st_mode | 0o111)
        return executable

    def test_two_runs_and_failed_run_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = self.make_fake_load_generator(root)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [
                        {"name": "burst-one", "pattern": "burst", "count": 3},
                        {"name": "fixed-two", "pattern": "fixed-rate", "count": 4, "rate": 2},
                        {"name": "fail-run", "pattern": "burst", "count": 1},
                    ],
                }),
                encoding="utf-8",
            )
            results, all_succeeded = run_experiment(config)

            self.assertFalse(all_succeeded)
            self.assertEqual(
                [result["status"] for result in results],
                ["succeeded", "succeeded", "succeeded",
                 "succeeded", "succeeded", "succeeded",
                 "failed", "failed", "failed"],
            )
            self.assertEqual(results[6]["exit_code"], 7)
            self.assertIn("intentional load-generator failure", results[6]["detail"])
            for name in ("burst-one", "fixed-two"):
                for repetition in range(1, 4):
                    run_dir = root / "results" / name / f"run-{repetition}"
                    self.assertTrue((run_dir / "requests.csv").is_file())
                    self.assertTrue((run_dir / "requests.csv.metadata.json").is_file())

    def test_success_without_artifacts_is_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "fake.py"
            executable.write_text("#!/usr/bin/env python3\nraise SystemExit(0)\n", encoding="utf-8")
            executable.chmod(executable.stat().st_mode | 0o111)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [{"name": "missing-artifacts", "pattern": "burst", "count": 1}],
                }),
                encoding="utf-8",
            )
            results, all_succeeded = run_experiment(config)
            self.assertFalse(all_succeeded)
            self.assertEqual(results[0]["status"], "failed")
            self.assertIn("missing-artifacts", results[0]["detail"])

    def test_stale_artifacts_cannot_satisfy_success_check(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "fake.py"
            executable.write_text("#!/usr/bin/env python3\nraise SystemExit(0)\n", encoding="utf-8")
            executable.chmod(executable.stat().st_mode | 0o111)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [{"name": "stale-artifacts", "pattern": "burst", "count": 1}],
                }),
                encoding="utf-8",
            )
            run_dir = root / "results" / "stale-artifacts" / "run-1"
            run_dir.mkdir(parents=True)
            (run_dir / "requests.csv").write_text("old csv\n", encoding="utf-8")
            (run_dir / "requests.csv.metadata.json").write_text("old metadata\n", encoding="utf-8")

            results, all_succeeded = run_experiment(config)

            self.assertFalse(all_succeeded)
            self.assertEqual(results[0]["status"], "failed")
            self.assertIn("missing artifacts", results[0]["detail"])
            self.assertFalse((run_dir / "requests.csv").exists())
            self.assertFalse((run_dir / "requests.csv.metadata.json").exists())

    def test_directory_creation_failure_does_not_stop_later_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = self.make_fake_load_generator(root)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [
                        {"name": "blocked-run", "pattern": "burst", "count": 1},
                        {"name": "later-run", "pattern": "burst", "count": 1},
                    ],
                }),
                encoding="utf-8",
            )
            output_dir = root / "results"
            output_dir.mkdir()
            (output_dir / "blocked-run").write_text("blocks directory creation", encoding="utf-8")

            results, all_succeeded = run_experiment(config)

            self.assertFalse(all_succeeded)
            self.assertEqual(
                [result["status"] for result in results],
                ["failed", "failed", "failed", "succeeded", "succeeded", "succeeded"],
            )
            self.assertTrue(results[0]["detail"])
            for repetition in range(1, 4):
                run_dir = output_dir / "later-run" / f"run-{repetition}"
                self.assertTrue((run_dir / "requests.csv").is_file())
                self.assertTrue((run_dir / "requests.csv.metadata.json").is_file())

    def test_aggregates_three_successful_runs_as_median_min_max(self):
        run_results = [
            {
                "status": "succeeded",
                "latency_percentiles": {"p50": 10.0, "p95": 20.0, "p99": 30.0},
                "throughput_requests_per_second": 100.0,
            },
            {
                "status": "succeeded",
                "latency_percentiles": {"p50": 20.0, "p95": 30.0, "p99": 40.0},
                "throughput_requests_per_second": 120.0,
            },
            {
                "status": "succeeded",
                "latency_percentiles": {"p50": 15.0, "p95": 25.0, "p99": 35.0},
                "throughput_requests_per_second": 110.0,
            },
        ]

        summary = aggregate_configuration("three-runs", run_results)

        self.assertEqual(summary["status"], "complete")
        self.assertEqual(summary["successful_runs"], 3)
        self.assertEqual(summary["aggregate"]["latency_ms"]["p50"], {
            "median": 15.0,
            "minimum": 10.0,
            "maximum": 20.0,
            "sample_count": 3,
        })
        self.assertEqual(summary["aggregate"]["throughput_requests_per_second"], {
            "median": 110.0,
            "minimum": 100.0,
            "maximum": 120.0,
            "sample_count": 3,
        })

    def test_incomplete_aggregation_uses_only_successful_runs(self):
        run_results = [
            {
                "status": "succeeded",
                "latency_percentiles": {"p50": 10.0, "p95": 20.0, "p99": 30.0},
                "throughput_requests_per_second": 100.0,
            },
            {"status": "failed", "latency_percentiles": {}, "throughput_requests_per_second": None},
            {
                "status": "succeeded",
                "latency_percentiles": {"p50": 30.0, "p95": 40.0, "p99": 50.0},
                "throughput_requests_per_second": 140.0,
            },
        ]

        summary = aggregate_configuration("two-success", run_results)

        self.assertEqual(summary["status"], "incomplete")
        self.assertEqual(summary["successful_runs"], 2)
        self.assertEqual(summary["aggregate"]["latency_ms"]["p50"]["median"], 20.0)
        self.assertEqual(summary["aggregate"]["throughput_requests_per_second"]["median"], 120.0)

    def test_machine_summary_artifact_is_written(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = self.make_fake_load_generator(root)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [{"name": "summary-run", "pattern": "burst", "count": 3}],
                }),
                encoding="utf-8",
            )

            _, all_succeeded = run_experiment(config)

            self.assertTrue(all_succeeded)
            summary_path = root / "results" / "experiment-summary.json"
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "complete")
            self.assertEqual(summary["successful_runs"], 3)
            self.assertEqual(summary["configurations"][0]["status"], "complete")

    def test_header_only_csv_is_marked_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "fake.py"
            executable.write_text(
                """#!/usr/bin/env python3
import sys
from pathlib import Path
args = sys.argv[1:]
csv_path = Path(args[args.index("--csv") + 1])
csv_path.parent.mkdir(parents=True, exist_ok=True)
csv_path.write_text(
    "request_index,scheduled_arrival_ms,completion_ms,error,warmup\\n",
    encoding="utf-8",
)
Path(str(csv_path) + ".metadata.json").write_text(
    '{"traffic_pattern": "burst"}\\n',
    encoding="utf-8",
)
""",
                encoding="utf-8",
            )
            executable.chmod(executable.stat().st_mode | 0o111)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [{"name": "header-only", "pattern": "burst", "count": 1}],
                }),
                encoding="utf-8",
            )

            results, all_succeeded = run_experiment(config)

            self.assertFalse(all_succeeded)
            self.assertEqual([result["status"] for result in results], ["failed"] * 3)
            self.assertIn("CSV contains no request records", results[0]["detail"])
            summary = json.loads((root / "results" / "experiment-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "incomplete")
            self.assertEqual(summary["configurations"][0]["successful_runs"], 0)

    def test_malformed_metadata_json_is_marked_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "fake.py"
            executable.write_text(
                """#!/usr/bin/env python3
import sys
from pathlib import Path
args = sys.argv[1:]
csv_path = Path(args[args.index("--csv") + 1])
csv_path.parent.mkdir(parents=True, exist_ok=True)
csv_path.write_text(
    "request_index,scheduled_arrival_ms,completion_ms,error,warmup\\n"
    "0,0,10,,false\\n",
    encoding="utf-8",
)
Path(str(csv_path) + ".metadata.json").write_text("{not-json\\n", encoding="utf-8")
""",
                encoding="utf-8",
            )
            executable.chmod(executable.stat().st_mode | 0o111)
            config = root / "experiment.json"
            config.write_text(
                json.dumps({
                    "load_generator": executable.name,
                    "output_dir": "results",
                    "runs": [{"name": "bad-metadata", "pattern": "burst", "count": 1}],
                }),
                encoding="utf-8",
            )

            results, all_succeeded = run_experiment(config)

            self.assertFalse(all_succeeded)
            self.assertEqual([result["status"] for result in results], ["failed"] * 3)
            self.assertIn("invalid successful run artifacts", results[0]["detail"])
            summary = json.loads((root / "results" / "experiment-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "incomplete")
            self.assertEqual(summary["configurations"][0]["successful_runs"], 0)

    def test_build_command_supports_all_existing_patterns(self):
        load_generator = Path("/tmp/inferserve_load_generator")
        csv_path = Path("/tmp/requests.csv")
        fixed = build_load_generator_command(
            load_generator,
            {"name": "fixed", "pattern": "fixed-rate", "count": 5, "rate": 2.5},
            csv_path,
        )
        poisson = build_load_generator_command(
            load_generator,
            {"name": "poisson", "pattern": "poisson", "count": 5, "rate": 3, "seed": 42},
            csv_path,
        )
        closed = build_load_generator_command(
            load_generator,
            {"name": "closed", "pattern": "closed-loop", "count": 5},
            csv_path,
        )
        self.assertEqual(fixed[:5], [str(load_generator), "fixed-rate", "5", "2.5", "--warmup-count"])
        self.assertEqual(poisson[1:5], ["poisson", "5", "3.0", "42"])
        self.assertEqual(closed[1:3], ["closed-loop", "5"])

    def test_warmup_count_is_forwarded_and_validated(self):
        load_generator = Path("/tmp/inferserve_load_generator")
        csv_path = Path("/tmp/requests.csv")
        command = build_load_generator_command(
            load_generator,
            {"name": "warmup", "pattern": "fixed-rate", "count": 5, "rate": 2, "warmup_count": 2},
            csv_path,
        )
        self.assertIn("--warmup-count", command)
        self.assertEqual(command[command.index("--warmup-count") + 1], "2")
        with self.assertRaisesRegex(ValueError, "less than"):
            build_load_generator_command(
                load_generator,
                {"name": "invalid", "pattern": "burst", "count": 3, "warmup_count": 3},
                csv_path,
            )


if __name__ == "__main__":
    unittest.main()
