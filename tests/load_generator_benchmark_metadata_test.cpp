#include "benchmark_metadata.hpp"

#include <gtest/gtest.h>

#include <sstream>

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
}

}  // namespace
