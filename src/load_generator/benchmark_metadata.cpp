#include "benchmark_metadata.hpp"

#include <fstream>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string_view>

namespace inferserve::load_generator {
namespace {

void write_json_string(std::ostream& output, std::string_view value)
{
    output << '"';
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char hex_digits[] = "0123456789abcdef";
                    output << "\\u00" << hex_digits[character >> 4] << hex_digits[character & 0x0f];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
}

template <typename Value>
void write_optional(std::ostream& output, const std::optional<Value>& value)
{
    if (value.has_value()) {
        output << *value;
    } else {
        output << "null";
    }
}

const char* compiler_name()
{
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unavailable";
#endif
}

const char* operating_system()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__unix__)
    return "Unix";
#else
    return "unavailable";
#endif
}

}  // namespace

void write_benchmark_metadata_json(std::ostream& output, const BenchmarkMetadata& metadata)
{
    const auto original_flags = output.flags();
    const auto original_precision = output.precision();
    output << std::fixed << std::setprecision(6);
    output << "{\n  \"run_timestamp_utc\": ";
    write_json_string(output, metadata.run_timestamp_utc);
    output << ",\n  \"traffic_pattern\": ";
    write_json_string(output, metadata.traffic_pattern);
    output << ",\n  \"mode\": ";
    write_json_string(output, metadata.mode);
    output << ",\n  \"transport\": ";
    write_json_string(output, metadata.transport);
    output << ",\n  \"request_count\": " << metadata.request_count
           << ",\n  \"warmup_count\": " << metadata.warmup_count
           << ",\n  \"duration_seconds\": " << metadata.duration_seconds
           << ",\n  \"configured_rate_per_second\": ";
    write_optional(output, metadata.configured_rate_per_second);
    output << ",\n  \"scheduled_duration_seconds\": ";
    write_optional(output, metadata.scheduled_duration_seconds);
    output << ",\n  \"poisson_seed\": ";
    write_optional(output, metadata.poisson_seed);
    output << ",\n  \"concurrency\": ";
    write_optional(output, metadata.concurrency);
    output << ",\n  \"http_host\": ";
    if (metadata.http_host.empty()) {
        output << "null";
    } else {
        write_json_string(output, metadata.http_host);
    }
    output << ",\n  \"http_port\": ";
    write_optional(output, metadata.http_port);
    output << ",\n  \"backend_identity\": null,\n  \"server_configuration\": null,\n  \"build\": {\n"
           << "    \"compiler\": ";
    write_json_string(output, compiler_name());
    output << ",\n    \"cxx_standard\": " << __cplusplus << ",\n    \"operating_system\": ";
    write_json_string(output, operating_system());
    output << "\n  }\n}\n";
    output.flags(original_flags);
    output.precision(original_precision);
    if (!output) {
        throw std::runtime_error("failed to write benchmark metadata JSON");
    }
}

void export_benchmark_metadata_json(const std::string& csv_path, const BenchmarkMetadata& metadata)
{
    const std::string metadata_path = csv_path + ".metadata.json";
    std::ofstream output(metadata_path);
    if (!output) {
        throw std::runtime_error("could not open benchmark metadata file: " + metadata_path);
    }
    write_benchmark_metadata_json(output, metadata);
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to write benchmark metadata file: " + metadata_path);
    }
}

}  // namespace inferserve::load_generator
