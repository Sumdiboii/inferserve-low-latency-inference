#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace inferserve::load_generator {

struct LoadAverages {
    double one_minute;
    double five_minutes;
    double fifteen_minutes;
};

struct BenchmarkMetadata {
    std::string run_timestamp_utc;
    std::string traffic_pattern;
    std::string mode;
    std::string transport;
    std::size_t request_count = 0;
    std::size_t warmup_count = 0;
    double duration_seconds = 0.0;
    std::optional<double> configured_rate_per_second;
    std::optional<double> scheduled_duration_seconds;
    std::optional<std::uint64_t> poisson_seed;
    std::optional<std::size_t> concurrency;
    std::string http_host;
    std::optional<std::uint16_t> http_port;
    std::optional<std::string> cpu_architecture;
    std::optional<std::string> cpu_model;
    std::optional<std::size_t> logical_cpu_count;
    std::optional<std::vector<std::size_t>> process_cpu_affinity;
    std::optional<std::string> kernel_version;
    std::optional<LoadAverages> system_load_start;
    std::optional<LoadAverages> system_load_end;
    std::optional<std::uint64_t> cgroup_cpu_quota_us;
    std::optional<std::uint64_t> cgroup_cpu_period_us;
    std::optional<double> cgroup_cpu_limit;
};

void write_benchmark_metadata_json(std::ostream& output, const BenchmarkMetadata& metadata);
void export_benchmark_metadata_json(const std::string& csv_path, const BenchmarkMetadata& metadata);
void collect_benchmark_environment_start(BenchmarkMetadata& metadata);
void collect_benchmark_environment_end(BenchmarkMetadata& metadata);

}  // namespace inferserve::load_generator
