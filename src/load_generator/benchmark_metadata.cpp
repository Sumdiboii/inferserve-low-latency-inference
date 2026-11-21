#include "benchmark_metadata.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

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

void write_load_averages(std::ostream& output, const std::optional<LoadAverages>& value)
    {
        if (!value.has_value()) {
            output << "null";
            return;
        }
        output << "{\"1_minute\": " << value->one_minute
               << ", \"5_minutes\": " << value->five_minutes
               << ", \"15_minutes\": " << value->fifteen_minutes << '}';
    }

    void write_affinity(std::ostream& output, const std::optional<std::vector<std::size_t>>& value)
    {
        if (!value.has_value()) {
            output << "null";
            return;
        }
        output << '[';
        for (std::size_t index = 0; index < value->size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << (*value)[index];
        }
        output << ']';
    }

    std::optional<std::string> read_text_file(const std::string& path)
    {
        std::ifstream input(path);
        if (!input) {
            return std::nullopt;
        }
        std::string value;
        std::getline(input, value);
        return input ? std::optional<std::string>(value) : std::nullopt;
    }

    std::optional<LoadAverages> read_load_averages()
    {
    #if defined(__linux__)
        double load[3]{};
        if (getloadavg(load, 3) == 3) {
            return LoadAverages{load[0], load[1], load[2]};
        }
    #endif
        return std::nullopt;
    }

    void collect_cgroup_cpu_limit(BenchmarkMetadata& metadata)
    {
    #if defined(__linux__)
        if (const auto cpu_max = read_text_file("/sys/fs/cgroup/cpu.max")) {
            std::istringstream input(*cpu_max);
            std::string quota;
            std::uint64_t period = 0;
            input >> quota;
            input >> period;
            if (quota != "max" && period != 0) {
                std::uint64_t quota_us = 0;
                if (std::from_chars(quota.data(), quota.data() + quota.size(), quota_us).ec == std::errc{} &&
                    quota_us != 0) {
                    metadata.cgroup_cpu_quota_us = quota_us;
                    metadata.cgroup_cpu_period_us = period;
                    metadata.cgroup_cpu_limit = static_cast<double>(quota_us) / static_cast<double>(period);
                    return;
                }
            }
        }

        const auto quota = read_text_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        const auto period = read_text_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        if (quota && period) {
            std::int64_t quota_us = 0;
            std::uint64_t period_us = 0;
            if (std::from_chars(quota->data(), quota->data() + quota->size(), quota_us).ec == std::errc{} &&
                std::from_chars(period->data(), period->data() + period->size(), period_us).ec == std::errc{} &&
                quota_us > 0 && period_us != 0) {
                metadata.cgroup_cpu_quota_us = static_cast<std::uint64_t>(quota_us);
                metadata.cgroup_cpu_period_us = period_us;
                metadata.cgroup_cpu_limit =
                    static_cast<double>(quota_us) / static_cast<double>(period_us);
            }
        }
    #endif
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

void collect_static_environment(BenchmarkMetadata& metadata)
{
#if defined(__linux__)
    utsname uname_info{};
    if (uname(&uname_info) == 0) {
        metadata.cpu_architecture = std::string(uname_info.machine);
        metadata.kernel_version = std::string(uname_info.release);
    }

    const long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0) {
        metadata.logical_cpu_count = static_cast<std::size_t>(cpu_count);
    }

    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        std::vector<std::size_t> cpus;
        for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &affinity)) {
                cpus.push_back(cpu);
            }
        }
        metadata.process_cpu_affinity = cpus;
    }

    std::ifstream cpu_info("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpu_info, line)) {
        const std::string_view marker = "model name";
        if (line.starts_with(marker) && line.find(':') != std::string::npos) {
            std::string model = line.substr(line.find(':') + 1);
            const std::size_t first = model.find_first_not_of(" \t");
            if (first != std::string::npos) {
                model.erase(0, first);
                metadata.cpu_model = model;
            }
            break;
        }
    }
#endif
    collect_cgroup_cpu_limit(metadata);
}

void collect_benchmark_environment_start(BenchmarkMetadata& metadata)
{
    collect_static_environment(metadata);
    metadata.system_load_start = read_load_averages();
}

void collect_benchmark_environment_end(BenchmarkMetadata& metadata)
{
    metadata.system_load_end = read_load_averages();
}

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
    output << ",\n  \"cpu_architecture\": ";
    if (metadata.cpu_architecture.has_value()) {
        write_json_string(output, *metadata.cpu_architecture);
    } else {
        output << "null";
    }
    output << ",\n  \"cpu_model\": ";
    if (metadata.cpu_model.has_value()) {
        write_json_string(output, *metadata.cpu_model);
    } else {
        output << "null";
    }
    output << ",\n  \"logical_cpu_count\": ";
    write_optional(output, metadata.logical_cpu_count);
    output << ",\n  \"process_cpu_affinity\": ";
    write_affinity(output, metadata.process_cpu_affinity);
    output << ",\n  \"kernel_version\": ";
    if (metadata.kernel_version.has_value()) {
        write_json_string(output, *metadata.kernel_version);
    } else {
        output << "null";
    }
    output << ",\n  \"system_load_start\": ";
    write_load_averages(output, metadata.system_load_start);
    output << ",\n  \"system_load_end\": ";
    write_load_averages(output, metadata.system_load_end);
    output << ",\n  \"cgroup_cpu_quota_us\": ";
    write_optional(output, metadata.cgroup_cpu_quota_us);
    output << ",\n  \"cgroup_cpu_period_us\": ";
    write_optional(output, metadata.cgroup_cpu_period_us);
    output << ",\n  \"cgroup_cpu_limit\": ";
    write_optional(output, metadata.cgroup_cpu_limit);
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
