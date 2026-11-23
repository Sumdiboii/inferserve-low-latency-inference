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
The sidecar also records CPU architecture and model, logical CPU count, process
CPU affinity, kernel version, system load at the start and end of the run,
and cgroup CPU quota/period/limit where available on Linux. Affinity applies only
to the load generator process. CPU and load fields are `null` when collection is
not reliable; no CPU pinning or load-based rejection is performed.

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
The HTTP load generator opens a TCP connection for each HTTP exchange and closes
it afterward. In particular, this includes the `/infer` admission request and
each `/results/{request_id}` polling request. Thus, reported HTTP request latency
includes connection establishment and teardown effects, as well as
completion-polling detection delay; it is client-observed end-to-end latency,
not pure inference execution latency.

## Metric interpretation

The latency plot uses scheduled-arrival-to-client-observed-completion for
successful dispatched measured requests. It does not report backend execution
time or a server-only queueing interval. Throughput is the number of those
successful dispatched measured requests divided by the generator's measured
completion window. Warmup rows are excluded from both calculations. Failure,
HTTP rejection, terminal failure, and undispatched requests are retained in the
CSV and should be reported separately rather than counted as successful
throughput.

`dispatch_lag_ms` is the load generator's actual-send time minus scheduled
arrival time. It is a client-side scheduling/transport indicator and is not a
direct measurement of server queue delay. Open-loop arrivals continue to be
scheduled independently of completions; closed-loop runs wait for one
completion before issuing the next request, so their effective rate changes as
latency changes. This is the coordinated-omission mechanism discussed in
[`ARCHITECTURE.md`](./ARCHITECTURE.md) and Phase 10.4, not a rate-matched
throughput comparison.

The Phase 10 artifacts do not export batch-size distributions or per-request
batch membership. The plots therefore cannot verify batch aggregation, backend
execution time, or a causal saturation threshold. In particular, Phase 10.3's
higher-rate latency variability should not be read as proof of a throughput
plateau.

New summaries produced by `scripts/run_experiment.py` contain absolute `csv`
and `metadata` paths because the runner resolves the output directory. Those
paths are metadata only; plotting still consumes the CSV files supplied on the
command line. Checked-in summaries may use relative paths for portability.

Run its focused parser and plot-output tests with:

```sh
python3 -m unittest tests/test_plot_benchmark_results.py
```
