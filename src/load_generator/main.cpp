#include "closed_loop_executor.hpp"
#include "open_loop_executor.hpp"
#include "benchmark_metadata.hpp"
#include "csv_export.hpp"
#include "dispatch_lag.hpp"
#include "http_transport.hpp"
#include "performance_stats.hpp"
#include "traffic_pattern.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_rate(std::string_view text, double& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

struct Options {
    std::string_view csv_output_path;
    std::string_view http_host;
    std::uint16_t http_port = 0;
};

bool parse_options(int argc, char* argv[], int positional_argc, Options& options)
{
    int argument = positional_argc;
    while (argument < argc) {
        const std::string_view option(argv[argument++]);
        if (option == "--csv" && argument < argc && options.csv_output_path.empty()) {
            options.csv_output_path = argv[argument++];
            if (options.csv_output_path.empty()) {
                return false;
            }
        } else if (option == "--http" && argument + 1 < argc && options.http_host.empty()) {
            options.http_host = argv[argument++];
            unsigned int port = 0;
            if (options.http_host.empty() || !parse_integer(std::string_view(argv[argument++]), port) ||
                port == 0 || port > 65535) {
                return false;
            }
            options.http_port = static_cast<std::uint16_t>(port);
        } else {
            return false;
        }
    }
    return true;
}

void print_usage()
{
    std::cerr << "Usage:\n"
                 "  inferserve_load_generator fixed-rate <count> <rate-per-second> [--http <host> <port>] [--csv <file>]\n"
                 "  inferserve_load_generator burst <count> [--http <host> <port>] [--csv <file>]\n"
                 "  inferserve_load_generator poisson <count> <rate-per-second> <seed> [--http <host> <port>] [--csv <file>]\n"
                 "  inferserve_load_generator closed-loop <count> [--http <host> <port>] [--csv <file>]\n";
}

std::string utc_timestamp()
{
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &timestamp);
#else
    gmtime_r(&timestamp, &utc_time);
#endif
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void print_rate(std::string_view label, const std::optional<double>& rate)
{
    std::cout << label << ": ";
    if (rate.has_value()) {
        std::cout << *rate << " requests/s\n";
    } else {
        std::cout << "n/a\n";
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        print_usage();
        return 1;
    }

    std::size_t count = 0;
    if (!parse_integer(std::string_view(argv[2]), count)) {
        std::cerr << "count must be a non-negative integer\n";
        return 1;
    }

    try {
        std::vector<double> arrivals;
        const std::string_view pattern(argv[1]);
        Options options;
        std::optional<double> configured_rate;
        std::optional<std::uint64_t> poisson_seed;
        std::optional<std::size_t> concurrency;
        const bool closed_loop = pattern == "closed-loop";
        std::cout << std::fixed << std::setprecision(3);
        std::mutex output_mutex;
        const auto execute_request = [&options, &output_mutex, pattern = std::string(pattern)](
                                         std::size_t index, double scheduled_seconds) {
            if (!options.http_host.empty()) {
                return inferserve::load_generator::send_infer_request(options.http_host, options.http_port);
            }
            const std::lock_guard lock(output_mutex);
            std::cout << (pattern == "closed-loop" ? "closed-loop simulated request " : "simulated request ")
                      << index << " scheduled at " << scheduled_seconds << " seconds\n";
            return inferserve::load_generator::RequestResult{};
        };
        std::vector<inferserve::load_generator::RequestTimestamps> records;

        if (closed_loop) {
            if (!parse_options(argc, argv, 3, options)) {
                print_usage();
                return 1;
            }
            concurrency = 1;
        } else if (pattern == "fixed-rate") {
            if (!parse_options(argc, argv, 4, options)) {
                print_usage();
                return 1;
            }
            double rate = 0.0;
            if (!parse_rate(std::string_view(argv[3]), rate)) {
                std::cerr << "rate-per-second must be a number\n";
                return 1;
            }
            configured_rate = rate;
            arrivals = inferserve::load_generator::fixed_rate_arrivals(count, rate);
        } else if (pattern == "burst") {
            if (!parse_options(argc, argv, 3, options)) {
                print_usage();
                return 1;
            }
            arrivals = inferserve::load_generator::burst_arrivals(count);
        } else if (pattern == "poisson") {
            if (!parse_options(argc, argv, 5, options)) {
                print_usage();
                return 1;
            }
            double rate = 0.0;
            std::uint64_t seed = 0;
            if (!parse_rate(std::string_view(argv[3]), rate) ||
                !parse_integer(std::string_view(argv[4]), seed)) {
                std::cerr << "poisson requires a numeric rate and non-negative integer seed\n";
                return 1;
            }
            configured_rate = rate;
            poisson_seed = seed;
            arrivals = inferserve::load_generator::poisson_arrivals(count, rate, seed);
        } else {
            print_usage();
            return 1;
        }

        inferserve::load_generator::BenchmarkMetadata metadata;
        metadata.run_timestamp_utc = utc_timestamp();
        metadata.traffic_pattern = std::string(pattern);
        metadata.mode = closed_loop ? "closed_loop" : "open_loop";
        metadata.transport = options.http_host.empty() ? "simulated" : "http";
        metadata.request_count = count;
        metadata.configured_rate_per_second = configured_rate;
        metadata.scheduled_duration_seconds = closed_loop
                                                 ? std::nullopt
                                                 : std::optional<double>(arrivals.empty() ? 0.0 : arrivals.back());
        metadata.poisson_seed = poisson_seed;
        metadata.concurrency = concurrency;
        if (!options.http_host.empty()) {
            metadata.http_host = std::string(options.http_host);
            metadata.http_port = options.http_port;
        }

        const auto run_started = std::chrono::steady_clock::now();
        if (closed_loop) {
            records = inferserve::load_generator::execute_closed_loop_with_results(count, execute_request);
        } else {
            records = inferserve::load_generator::execute_open_loop_with_results(arrivals, execute_request);
        }
        metadata.duration_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started).count();

        bool has_failed_requests = false;
        for (const auto& record : records) {
            if (record.result.http_status != 0) {
                std::lock_guard lock(output_mutex);
                std::cout << "request " << record.request_index << " HTTP " << record.result.http_status << '\n';
            }
            if (!record.result.success) {
                has_failed_requests = true;
                std::lock_guard lock(output_mutex);
                std::cerr << "request " << record.request_index << " failed: " << record.result.error << '\n';
            }
        }

        if (!options.csv_output_path.empty()) {
            inferserve::load_generator::export_records_csv(std::string(options.csv_output_path), records);
            inferserve::load_generator::export_benchmark_metadata_json(std::string(options.csv_output_path), metadata);
        }
        const auto dispatch_lags = inferserve::load_generator::calculate_dispatch_lags(records);
        for (const auto& dispatch_lag : dispatch_lags) {
            std::cout << "request " << dispatch_lag.request_index << " dispatch lag: " << dispatch_lag.milliseconds
                      << " ms\n";
        }
        if (!dispatch_lags.empty()) {
            const auto statistics = inferserve::load_generator::summarize_dispatch_lags(dispatch_lags);
            std::cout << "dispatch lag statistics (ms): mean=" << statistics.mean_ms << " p50=" << statistics.p50_ms
                      << " p95=" << statistics.p95_ms << '\n';
        }

        const auto performance = inferserve::load_generator::calculate_performance_statistics(records);
        std::cout << "performance statistics:\n";
        if (performance.latency_ms.has_value()) {
            std::cout << "request latency from scheduled arrival (ms): p50=" << performance.latency_ms->p50_ms
                      << " p95=" << performance.latency_ms->p95_ms << " p99=" << performance.latency_ms->p99_ms
                      << " samples=" << performance.latency_ms->sample_count << '\n';
        } else {
            std::cout << "request latency from scheduled arrival (ms): n/a\n";
        }
        print_rate("throughput (completed dispatched requests)", performance.throughput_requests_per_second);
        print_rate("offered rate (scheduled requests)", performance.offered_requests_per_second);
        print_rate("HTTP acknowledgement rate (successful 2xx; 202 means admitted, not inference complete)",
                   performance.http_acknowledgements_per_second);
        std::cout << "request failures: failed=" << performance.failures.failed
                  << " rejected=" << performance.failures.rejected
                  << " undispatched=" << performance.failures.undispatched << '\n';
        if (has_failed_requests) {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
