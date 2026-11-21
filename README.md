# InferServe

## Repeatable benchmark experiments

Run a small set of load-generator configurations from JSON:

```sh
python3 scripts/run_experiment.py experiments/example.json
```

The runner invokes `build/debug/inferserve_load_generator` for every run and
writes predictable artifacts under `experiment-results/<run-name>/`:

- `requests.csv`
- `requests.csv.metadata.json`

An example configuration is in `experiments/example.json`. Each run requires a
unique `name`, `pattern`, and `count`. Fixed-rate runs require `rate`;
Poisson runs require `rate` and `seed`. Optional `http` objects set a host and
port. An optional `warmup_count` marks that many leading requests as warmup;
it defaults to `0` and must be less than `count`. Warmup requests execute as
part of the same load-generator invocation and are excluded from measured
statistics and plots, but remain in the CSV.
The metadata sidecar records available Linux environment details such as CPU
architecture/model, logical CPU count, load-generator CPU affinity, kernel
version, start/end load averages, and cgroup CPU limit.
Each named configuration is executed exactly three times. Individual results
are preserved under `experiment-results/<run-name>/run-1` through `run-3`, and
`experiment-results/experiment-summary.json` contains median, minimum, and
maximum latency percentiles and throughput across successful runs. A summary
is marked incomplete when fewer than three runs succeed.
Use `--load-generator` or `--output-dir` to override the executable or output
location for one invocation.

## HTTP server backends

The HTTP server uses the Synthetic backend by default:

```sh
build/debug/inferserve [port]
```

To use the existing ONNX Runtime CPU backend, select it and provide an ONNX
model:

```sh
build/debug/inferserve --backend onnx --model tests/data/identity_dynamic_batch_2.onnx [port]
```

The ONNX backend requires an existing float32 model with one input and one
output. The model's leading batch dimension may be fixed or dynamic. A missing
model or invalid model shape fails startup through the existing backend loader.

The command reports every run as succeeded or failed and exits non-zero if any
run fails or omits its CSV or metadata sidecar.
