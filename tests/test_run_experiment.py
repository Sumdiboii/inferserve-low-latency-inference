import json
import os
import tempfile
import unittest
from pathlib import Path

from scripts.run_experiment import build_load_generator_command, run_experiment


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
csv_path.write_text("request_index,error\\n0,\\n", encoding="utf-8")
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
            self.assertEqual([result["status"] for result in results],
                             ["succeeded", "succeeded", "failed"])
            self.assertEqual(results[2]["exit_code"], 7)
            self.assertIn("intentional load-generator failure", results[2]["detail"])
            for name in ("burst-one", "fixed-two"):
                run_dir = root / "results" / name
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
            run_dir = root / "results" / "stale-artifacts"
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
            self.assertEqual([result["status"] for result in results], ["failed", "succeeded"])
            self.assertTrue(results[0]["detail"])
            self.assertTrue((output_dir / "later-run" / "requests.csv").is_file())
            self.assertTrue((output_dir / "later-run" / "requests.csv.metadata.json").is_file())

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
