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
