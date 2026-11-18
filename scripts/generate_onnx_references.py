#!/usr/bin/env python3
"""Generate deterministic Python reference data for the Identity ONNX fixture."""

from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "tests" / "data" / "identity_python_reference.txt"


def main() -> None:
    examples = {
        "batch_one": np.array([[3.5, -2.0]], dtype=np.float32),
        "batch_three": np.array(
            [[1.25, -4.0], [9.5, 2.0], [-7.0, 3.75]], dtype=np.float32
        ),
    }

    with OUTPUT.open("w", encoding="utf-8", newline="\n") as reference_file:
        for name, inputs in examples.items():
            # The model is Identity, so Python's independent reference is a copy.
            expected = inputs.copy()
            for index, input_row in enumerate(inputs):
                expected_row = expected[index]
                values = (*input_row, *expected_row)
                reference_file.write(
                    name + " " + " ".join(format(float(value), ".9g") for value in values) + "\n"
                )


if __name__ == "__main__":
    main()
