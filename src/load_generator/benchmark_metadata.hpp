#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

namespace inferserve::load_generator {

struct BenchmarkMetadata {
    std::string run_timestamp_utc;
    std::string traffic_pattern;
    std::string mode;
    std::string transport;
    std::size_t request_count = 0;
    double duration_seconds = 0.0;
    std::optional<double> configured_rate_per_second;
    std::optional<double> scheduled_duration_seconds;
    std::optional<std::uint64_t> poisson_seed;
    std::optional<std::size_t> concurrency;
    std::string http_host;
    std::optional<std::uint16_t> http_port;
};

void write_benchmark_metadata_json(std::ostream& output, const BenchmarkMetadata& metadata);
void export_benchmark_metadata_json(const std::string& csv_path, const BenchmarkMetadata& metadata);

}  // namespace inferserve::load_generator
