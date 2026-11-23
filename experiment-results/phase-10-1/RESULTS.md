# Phase 10.1: Batching Off vs. Batching On

All four configurations used fixed-rate open-loop traffic at 500 RPS, 1,100
total requests, 100 warmup requests, and 1,000 measured requests. Each
configuration ran three repetitions.

| Configuration | p50 ms (median, range) | p95 ms (median, range) | p99 ms (median, range) | Throughput req/s (median, range) |
|---|---:|---:|---:|---:|
| Synthetic off | 535.617 (456.915–552.631) | 942.180 (847.081–975.322) | 977.601 (882.080–1008.575) | 335.169 (332.068–346.382) |
| Synthetic on | 529.725 (520.266–557.214) | 967.007 (921.722–982.708) | 1002.369 (954.846–1022.638) | 332.983 (329.523–337.440) |
| ONNX off | 1.048 (0.879–1.133) | 14.159 (13.950–17.140) | 21.601 (21.210–29.655) | 500.244 (497.642–500.246) |
| ONNX on | 1.084 (0.905–1.191) | 12.293 (11.951–13.038) | 17.056 (15.444–17.783) | 500.220 (500.177–500.282) |

Every run completed all 1,000 measured requests successfully. There were no
failed, rejected, terminal-failure, or undispatched measured requests.

The 500 RPS Synthetic runs were unstable and saturated below the offered
rate: throughput was approximately 332–346 req/s and tail latency was roughly
850–1,023 ms. Synthetic batching-off and batching-on produced overlapping
run ranges and no clear batching benefit.

The ONNX runs were stable and sustained approximately 500 req/s in all runs.
Batching-on slightly improved p95/p99 median tail latency, with overlapping
individual-run ranges. Differences are small and should not be interpreted as
a large or proven batching effect.

Actual batch aggregation cannot be verified from the existing artifacts:
the runtime records a dispatched-batch-size distribution internally, but
those metrics and per-request batch size are not exported in the CSV or
metadata. The max-batch-size setting is therefore an enabled/disabled
experiment setup, not direct evidence that batches larger than one formed.
