#include "closed_loop_executor.hpp"
#include "open_loop_executor.hpp"
#include "csv_export.hpp"
#include "dispatch_lag.hpp"
#include "traffic_pattern.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
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

bool parse_csv_output_option(int argc, char* argv[], int positional_argc, std::string_view& output_path)
{
    if (argc == positional_argc) {
        output_path = {};
        return true;
    }
    if (argc == positional_argc + 2 && std::string_view(argv[positional_argc]) == "--csv") {
        output_path = argv[positional_argc + 1];
        return !output_path.empty();
    }
    return false;
}

void print_usage()
{
    std::cerr << "Usage:\n"
                 "  inferserve_load_generator fixed-rate <count> <rate-per-second> [--csv <output-file>]\n"
                 "  inferserve_load_generator burst <count> [--csv <output-file>]\n"
                 "  inferserve_load_generator poisson <count> <rate-per-second> <seed> [--csv <output-file>]\n"
                 "  inferserve_load_generator closed-loop <count> [--csv <output-file>]\n";
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
        std::string_view csv_output_path;
        std::cout << std::fixed << std::setprecision(3);
        std::mutex output_mutex;
        const auto simulate_request = [&output_mutex, closed_loop = pattern == "closed-loop"](
                                          std::size_t index, double scheduled_seconds) {
            const std::lock_guard lock(output_mutex);
            if (closed_loop) {
                std::cout << "closed-loop simulated request ";
            } else {
                std::cout << "simulated request ";
            }
            std::cout << index << " scheduled at " << scheduled_seconds << " seconds\n";
        };
        std::vector<inferserve::load_generator::RequestTimestamps> records;

        if (pattern == "closed-loop") {
            if (!parse_csv_output_option(argc, argv, 3, csv_output_path)) {
                print_usage();
                return 1;
            }
            records = inferserve::load_generator::execute_closed_loop(count, simulate_request);
        } else if (pattern == "fixed-rate") {
            if (!parse_csv_output_option(argc, argv, 4, csv_output_path)) {
                print_usage();
                return 1;
            }
            double rate = 0.0;
            if (!parse_rate(std::string_view(argv[3]), rate)) {
                std::cerr << "rate-per-second must be a number\n";
                return 1;
            }
            arrivals = inferserve::load_generator::fixed_rate_arrivals(count, rate);
        } else if (pattern == "burst") {
            if (!parse_csv_output_option(argc, argv, 3, csv_output_path)) {
                print_usage();
                return 1;
            }
            arrivals = inferserve::load_generator::burst_arrivals(count);
        } else if (pattern == "poisson") {
            if (!parse_csv_output_option(argc, argv, 5, csv_output_path)) {
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
            arrivals = inferserve::load_generator::poisson_arrivals(count, rate, seed);
        } else {
            print_usage();
            return 1;
        }

        if (pattern != "closed-loop") {
            records = inferserve::load_generator::execute_open_loop(arrivals, simulate_request);
        }

        if (!csv_output_path.empty()) {
            inferserve::load_generator::export_records_csv(std::string(csv_output_path), records);
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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
