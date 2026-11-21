#!/usr/bin/env python3
"""Repeatably run load-generator benchmark configurations."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


NAME_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*$")
PATTERNS = {"fixed-rate", "burst", "poisson", "closed-loop"}


def resolve_path(value: str, base: Path) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (base / path).resolve()


def require_string(run: dict[str, Any], key: str, run_name: str) -> str:
    value = run.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"run '{run_name}': '{key}' must be a non-empty string")
    return value


def require_integer(run: dict[str, Any], key: str, run_name: str) -> int:
    value = run.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"run '{run_name}': '{key}' must be a non-negative integer")
    return value


def require_number(run: dict[str, Any], key: str, run_name: str) -> float:
    value = run.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"run '{run_name}': '{key}' must be a number")
    result = float(value)
    if result < 0:
        raise ValueError(f"run '{run_name}': '{key}' must be non-negative")
    return result


def build_load_generator_command(
    load_generator: Path,
    run: dict[str, Any],
    csv_path: Path,
) -> list[str]:
    name = require_string(run, "name", "<unnamed>")
    pattern = require_string(run, "pattern", name)
    if pattern not in PATTERNS:
        raise ValueError(
            f"run '{name}': unsupported pattern '{pattern}'; "
            f"expected one of {', '.join(sorted(PATTERNS))}"
        )

    count = require_integer(run, "count", name)
    command = [str(load_generator), pattern, str(count)]

    if pattern == "fixed-rate":
        command.append(str(require_number(run, "rate", name)))
    elif pattern == "poisson":
        command.append(str(require_number(run, "rate", name)))
        command.append(str(require_integer(run, "seed", name)))

    http = run.get("http")
    if http is not None:
        if not isinstance(http, dict):
            raise ValueError(f"run '{name}': 'http' must be an object")
        host = require_string(http, "host", name)
        port = http.get("port")
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            raise ValueError(f"run '{name}': http port must be an integer from 1 through 65535")
        command.extend(["--http", host, str(port)])

    command.extend(["--csv", str(csv_path)])
    return command


def load_config(config_path: Path) -> tuple[dict[str, Any], list[dict[str, Any]], Path]:
    with config_path.open("r", encoding="utf-8") as source:
        config = json.load(source)
    if not isinstance(config, dict):
        raise ValueError("experiment configuration must be a JSON object")

    runs = config.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("experiment configuration must contain a non-empty 'runs' array")

    base = config_path.parent
    names: set[str] = set()
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("each run must be a JSON object")
        name = require_string(run, "name", "<unnamed>")
        if not NAME_PATTERN.fullmatch(name):
            raise ValueError(
                f"run '{name}': name must contain only letters, digits, '-' or '_' "
                "and start with a letter or digit"
            )
        if name in names:
            raise ValueError(f"duplicate run name '{name}'")
        names.add(name)

    return config, runs, base


def run_experiment(
    config_path: Path,
    load_generator_override: Path | None = None,
    output_dir_override: Path | None = None,
) -> tuple[list[dict[str, Any]], bool]:
    config, runs, config_base = load_config(config_path)
    repository_root = Path(__file__).resolve().parent.parent

    load_generator_value = (
        str(load_generator_override)
        if load_generator_override is not None
        else config.get("load_generator", "build/debug/inferserve_load_generator")
    )
    if not isinstance(load_generator_value, str) or not load_generator_value:
        raise ValueError("'load_generator' must be a non-empty string")
    if load_generator_override is not None:
        load_generator = resolve_path(load_generator_value, Path.cwd())
    else:
        load_generator = resolve_path(load_generator_value, config_base)

    output_value = config.get("output_dir", "experiment-results")
    if not isinstance(output_value, str) or not output_value:
        raise ValueError("'output_dir' must be a non-empty string")
    if output_dir_override is not None:
        output_dir = resolve_path(str(output_dir_override), Path.cwd())
    else:
        output_dir = resolve_path(output_value, config_base)

    if not load_generator.is_file():
        raise FileNotFoundError(f"load generator executable not found: {load_generator}")

    results: list[dict[str, Any]] = []
    all_succeeded = True
    for run in runs:
        name = require_string(run, "name", "<unnamed>")
        run_dir = output_dir / name
        csv_path = run_dir / "requests.csv"
        metadata_path = Path(str(csv_path) + ".metadata.json")

        try:
            run_dir.mkdir(parents=True, exist_ok=True)
            for artifact in (csv_path, metadata_path):
                if artifact.exists():
                    artifact.unlink()
            command = build_load_generator_command(load_generator, run, csv_path)
            completed = subprocess.run(
                command,
                cwd=repository_root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            missing = [
                str(path)
                for path in (csv_path, metadata_path)
                if not path.is_file()
            ]
            if completed.returncode == 0 and not missing:
                status = "succeeded"
            else:
                status = "failed"
                all_succeeded = False
        except (ValueError, OSError) as error:
            completed = None
            missing = []
            status = "failed"
            all_succeeded = False
            detail = str(error)

        result: dict[str, Any] = {
            "name": name,
            "status": status,
            "csv": str(csv_path),
            "metadata": str(metadata_path),
        }
        if completed is not None:
            result["exit_code"] = completed.returncode
            if status == "failed":
                detail = completed.stderr.strip()
                if not detail:
                    detail = f"missing artifacts: {', '.join(missing)}" if missing else "load generator failed"
        result["detail"] = detail if status == "failed" else ""
        results.append(result)

    return results, all_succeeded


def print_results(results: list[dict[str, Any]]) -> None:
    for result in results:
        label = "SUCCEEDED" if result["status"] == "succeeded" else "FAILED"
        print(f"[{label}] {result['name']}")
        print(f"  CSV:      {result['csv']}")
        print(f"  Metadata: {result['metadata']}")
        if result["status"] == "failed":
            exit_code = result.get("exit_code")
            print(f"  Exit code: {exit_code if exit_code is not None else 'n/a'}")
            if result["detail"]:
                print(f"  Error: {result['detail']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path, help="JSON experiment configuration")
    parser.add_argument("--load-generator", type=Path, help="Override the load-generator executable")
    parser.add_argument("--output-dir", type=Path, help="Override the configured output directory")
    arguments = parser.parse_args()

    config_path = arguments.config.resolve()
    try:
        results, all_succeeded = run_experiment(
            config_path,
            arguments.load_generator,
            arguments.output_dir,
        )
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 1

    print_results(results)
    return 0 if all_succeeded else 1


if __name__ == "__main__":
    raise SystemExit(main())
