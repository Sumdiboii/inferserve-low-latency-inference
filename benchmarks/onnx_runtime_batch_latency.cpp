#include <inferserve/onnx_runtime_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

using inferserve::Batch;
using inferserve::Data;
using inferserve::InferenceResults;
using inferserve::OnnxRuntimeBackend;

constexpr std::size_t kWarmupIterations = 50;
constexpr std::size_t kMeasurementIterations = 500;

struct LatencySummary {
    double mean_ms;
    double p50_ms;
    double p95_ms;
};

double consume(const InferenceResults& results)
{
    double checksum = 0.0;
    for (const Data& output : results) {
        for (float value : output) {
            checksum += static_cast<double>(value);
        }
    }
    return checksum;
}

double percentile(const std::vector<double>& sorted_samples, double fraction)
{
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * sorted_samples.size()));
    return sorted_samples[std::max<std::size_t>(rank, 1U) - 1U];
}

LatencySummary measure(OnnxRuntimeBackend& backend, const Batch& batch, double& checksum)
{
    for (std::size_t iteration = 0; iteration < kWarmupIterations; ++iteration) {
        const InferenceResults results = backend.infer(batch);
        if (results.size() != batch.size()) {
            throw std::runtime_error("backend returned an unexpected warmup result count");
        }
        checksum += consume(results);
    }

    std::vector<double> samples_ms;
    samples_ms.reserve(kMeasurementIterations);
    for (std::size_t iteration = 0; iteration < kMeasurementIterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const InferenceResults results = backend.infer(batch);
        const auto finish = std::chrono::steady_clock::now();
        if (results.size() != batch.size()) {
            throw std::runtime_error("backend returned an unexpected result count");
        }
        samples_ms.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
        checksum += consume(results);
    }

    const double mean_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0) /
        static_cast<double>(samples_ms.size());
    std::sort(samples_ms.begin(), samples_ms.end());
    return LatencySummary{mean_ms, percentile(samples_ms, 0.50), percentile(samples_ms, 0.95)};
}

}  // namespace

int main()
{
    try {
        OnnxRuntimeBackend backend(INFERSERVE_BENCHMARK_DYNAMIC_BATCH_MODEL_PATH);
        backend.load();

        const std::vector<Batch> batches{
            Batch{Data{3.5F, -2.0F}},
            Batch{Data{1.25F, -4.0F}, Data{9.5F, 2.0F}, Data{-7.0F, 3.75F}},
        };

        std::vector<LatencySummary> summaries;
        double checksum = 0.0;
        for (const Batch& batch : batches) {
            summaries.push_back(measure(backend, batch, checksum));
        }

        std::cout << "Raw ONNX Runtime backend call latency (" << kMeasurementIterations
                  << " iterations, " << kWarmupIterations << " warmup calls per batch size)\n";
        std::cout << std::left << std::setw(12) << "Batch size" << std::right << std::setw(14) << "Mean (ms)"
                  << std::setw(14) << "p50 (ms)" << std::setw(14) << "p95 (ms)" << '\n';
        std::cout << std::fixed << std::setprecision(6);
        for (std::size_t index = 0; index < batches.size(); ++index) {
            const LatencySummary& summary = summaries[index];
            std::cout << std::left << std::setw(12) << batches[index].size() << std::right << std::setw(14)
                      << summary.mean_ms << std::setw(14) << summary.p50_ms << std::setw(14) << summary.p95_ms
                      << '\n';
        }
        std::cout << "Output checksum: " << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ONNX Runtime batch latency benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
