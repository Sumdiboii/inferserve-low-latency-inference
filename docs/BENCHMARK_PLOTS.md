# Benchmark plots

Create two SVG plots from per-request load-generator CSV files:

```sh
python3 scripts/plot_benchmark_results.py \
  --synthetic synthetic.csv \
  --onnx onnx-runtime-cpu.csv \
  --output-dir plots
```

When `--csv results.csv` is supplied to the load generator, it also writes
`results.csv.metadata.json` with the run configuration and available local
build/runtime information. It includes pattern, open/closed-loop mode, transport,
request count, actual execution duration, scheduled duration (open-loop), target
rate and Poisson seed where applicable, closed-loop concurrency, HTTP destination,
UTC start time, compiler, C++ standard, and OS. `backend_identity` and
`server_configuration` are `null` because the client cannot reliably discover
them; null rate, seed, concurrency, or destination values mean not applicable or
not configured for that run.

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
CSV files containing a `warmup` column exclude rows marked `true` from latency
and throughput calculations. Legacy CSV files without the `warmup` column are
treated as fully measured runs.

Run its focused parser and plot-output tests with:

```sh
python3 -m unittest tests/test_plot_benchmark_results.py
```
