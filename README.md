# inferserve-low-latency-inference
A C++20 inference server built around tail latency. Incoming requests are queued, batched dynamically, and run on a GPU through ONNX Runtime or TensorRT. Ships with an open-loop load generator that reports throughput and p50/p95/p99 latency under steady, bursty, and overloaded traffic.
