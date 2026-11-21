#include "benchmark_metadata.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <vector>

namespace {

using inferserve::load_generator::BenchmarkMetadata;
using inferserve::load_generator::write_benchmark_metadata_json;

TEST(BenchmarkMetadata, WritesRunConfigurationAndExplicitUnavailableValues)
{
    BenchmarkMetadata metadata;
    metadata.run_timestamp_utc = "2026-11-21T10:15:30Z";
    metadata.traffic_pattern = "fixed-rate";
    metadata.mode = "open_loop";
    metadata.transport = "http";
    metadata.request_count = 10;
    metadata.warmup_count = 2;
    metadata.duration_seconds = 2.25;
    metadata.configured_rate_per_second = 5.0;
    metadata.scheduled_duration_seconds = 1.8;
    metadata.http_host = "localhost";
    metadata.http_port = 8080;
    metadata.cpu_architecture = "x86_64";
    metadata.cpu_model = "Test CPU";
    metadata.logical_cpu_count = 8;
    metadata.process_cpu_affinity = std::vector<std::size_t>{0, 2, 4};
    metadata.kernel_version = "6.8-test";
    metadata.system_load_start = inferserve::load_generator::LoadAverages{0.25, 0.50, 0.75};
    metadata.system_load_end = inferserve::load_generator::LoadAverages{0.30, 0.55, 0.80};
    metadata.cgroup_cpu_quota_us = 200000;
    metadata.cgroup_cpu_period_us = 100000;
    metadata.cgroup_cpu_limit = 2.0;

    std::ostringstream output;
    write_benchmark_metadata_json(output, metadata);

    const std::string json = output.str();
    EXPECT_NE(json.find("\"traffic_pattern\": \"fixed-rate\""), std::string::npos);
    EXPECT_NE(json.find("\"mode\": \"open_loop\""), std::string::npos);
    EXPECT_NE(json.find("\"transport\": \"http\""), std::string::npos);
    EXPECT_NE(json.find("\"request_count\": 10"), std::string::npos);
    EXPECT_NE(json.find("\"warmup_count\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"configured_rate_per_second\": 5.000000"), std::string::npos);
    EXPECT_NE(json.find("\"http_host\": \"localhost\""), std::string::npos);
    EXPECT_NE(json.find("\"http_port\": 8080"), std::string::npos);
    EXPECT_NE(json.find("\"cpu_architecture\": \"x86_64\""), std::string::npos);
    EXPECT_NE(json.find("\"cpu_model\": \"Test CPU\""), std::string::npos);
    EXPECT_NE(json.find("\"logical_cpu_count\": 8"), std::string::npos);
    EXPECT_NE(json.find("\"process_cpu_affinity\": [0, 2, 4]"), std::string::npos);
    EXPECT_NE(json.find("\"kernel_version\": \"6.8-test\""), std::string::npos);
    EXPECT_NE(json.find("\"system_load_start\": {\"1_minute\": 0.250000"), std::string::npos);
    EXPECT_NE(json.find("\"system_load_end\": {\"1_minute\": 0.300000"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_quota_us\": 200000"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_period_us\": 100000"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_limit\": 2.000000"), std::string::npos);
    EXPECT_NE(json.find("\"backend_identity\": null"), std::string::npos);
    EXPECT_NE(json.find("\"server_configuration\": null"), std::string::npos);
    EXPECT_NE(json.find("\"operating_system\": \"Linux\""), std::string::npos);
}

TEST(BenchmarkMetadata, EscapesStringsAndRepresentsOptionalConfigurationAsNull)
{
    BenchmarkMetadata metadata;
    metadata.traffic_pattern = "custom\"pattern";
    metadata.mode = "open_loop";
    metadata.transport = "simulated";

    std::ostringstream output;
    write_benchmark_metadata_json(output, metadata);

    const std::string json = output.str();
    EXPECT_NE(json.find("\"traffic_pattern\": \"custom\\\"pattern\""), std::string::npos);
    EXPECT_NE(json.find("\"configured_rate_per_second\": null"), std::string::npos);
    EXPECT_NE(json.find("\"scheduled_duration_seconds\": null"), std::string::npos);
    EXPECT_NE(json.find("\"poisson_seed\": null"), std::string::npos);
    EXPECT_NE(json.find("\"concurrency\": null"), std::string::npos);
    EXPECT_NE(json.find("\"http_host\": null"), std::string::npos);
    EXPECT_NE(json.find("\"http_port\": null"), std::string::npos);
    EXPECT_NE(json.find("\"cpu_architecture\": null"), std::string::npos);
    EXPECT_NE(json.find("\"cpu_model\": null"), std::string::npos);
    EXPECT_NE(json.find("\"logical_cpu_count\": null"), std::string::npos);
    EXPECT_NE(json.find("\"process_cpu_affinity\": null"), std::string::npos);
    EXPECT_NE(json.find("\"kernel_version\": null"), std::string::npos);
    EXPECT_NE(json.find("\"system_load_start\": null"), std::string::npos);
    EXPECT_NE(json.find("\"system_load_end\": null"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_quota_us\": null"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_period_us\": null"), std::string::npos);
    EXPECT_NE(json.find("\"cgroup_cpu_limit\": null"), std::string::npos);
}

}  // namespace
