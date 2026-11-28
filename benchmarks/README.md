# Raw ONNX Runtime batch latency

Build and run the standalone benchmark with the Debug preset:

```sh
cmake --build --preset debug --target inferserve_onnx_benchmark
./build/debug/inferserve_onnx_benchmark
```

It loads the existing dynamic-batch Identity fixture before timing, warms up
each batch size with 50 calls, then measures 500 calls for batch sizes 1 and 3.
Each sample is the wall-clock duration of one `OnnxRuntimeBackend::infer()`
call; model construction and session loading are outside the timed region. The
reported mean, p50, and p95 are per-call latency. Batch-3 latency is for one
whole three-request batch inference call, not the latency of an individual
request. The output checksum consumes all returned values.

This is a raw backend microbenchmark, not a serving-system benchmark: it excludes
HTTP, queues, dynamic batching, worker scheduling, load generation, and
end-to-end request latency. Results depend on the CPU, model, and ONNX Runtime
configuration. A single run is not evidence that batching is faster.

## Local verification run

A fresh Debug configure/build on 2026-11-28 completed, and the benchmark ran
successfully in the current Linux environment. Its measured output was:

| Batch size | Mean (ms) | p50 (ms) | p95 (ms) |
| ---: | ---: | ---: | ---: |
| 1 | 0.004801 | 0.004748 | 0.004914 |
| 3 | 0.005228 | 0.005197 | 0.005346 |

Output checksum: `3850.000000`. These are one-run local measurements; the
container did not expose CPU-identification data, so they are a smoke result,
not a portable performance baseline or a basis for a batching claim. The clean
Debug build's full CTest run discovered and passed all 119 tests, including the
HTTP/ONNX integration and Python runner/parser tests.
