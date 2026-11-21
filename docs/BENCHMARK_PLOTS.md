# Benchmark plots

Create two SVG plots from per-request load-generator CSV files:

```sh
python3 scripts/plot_benchmark_results.py \
  --synthetic synthetic.csv \
  --onnx onnx-runtime-cpu.csv \
  --output-dir plots
```

The script uses only the Python standard library. It writes `latency_percentiles.svg`
(p50, p95, and p99 scheduled-arrival-to-completion latency in milliseconds) and
`throughput.svg` (dispatched requests completed by the generator per second). The
input labels are supplied by the two command-line options; the CSV format itself
does not record the backend type. Use files captured from the matching backend
runs. The script cannot verify backend identity or whether a file is a smoke run;
smoke-test plots are useful for checking the workflow, not as benchmark evidence.
Same-time burst inputs and single-request inputs show throughput as `n/a`, since
they do not define a sustained arrival window. The throughput plot describes
generator-side request completion; an HTTP 202 response means admission, not
inference completion. Legacy CSV files without the `error` column are rejected
because they cannot identify requests that were not dispatched.

Run its focused parser and plot-output tests with:

```sh
python3 -m unittest tests/test_plot_benchmark_results.py
```
