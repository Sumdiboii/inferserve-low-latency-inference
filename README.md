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
Use `--load-generator` or `--output-dir` to override the executable or output
location for one invocation.

The command reports every run as succeeded or failed and exits non-zero if any
run fails or omits its CSV or metadata sidecar.
