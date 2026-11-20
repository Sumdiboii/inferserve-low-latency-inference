#include "traffic_pattern.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
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

void print_usage()
{
    std::cerr << "Usage:\n"
                 "  inferserve_load_generator fixed-rate <count> <rate-per-second>\n"
                 "  inferserve_load_generator burst <count>\n"
                 "  inferserve_load_generator poisson <count> <rate-per-second> <seed>\n";
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
        if (pattern == "fixed-rate" && argc == 4) {
            double rate = 0.0;
            if (!parse_rate(std::string_view(argv[3]), rate)) {
                std::cerr << "rate-per-second must be a number\n";
                return 1;
            }
            arrivals = inferserve::load_generator::fixed_rate_arrivals(count, rate);
        } else if (pattern == "burst" && argc == 3) {
            arrivals = inferserve::load_generator::burst_arrivals(count);
        } else if (pattern == "poisson" && argc == 5) {
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

        std::cout << std::fixed << std::setprecision(9);
        for (const double time_seconds : arrivals) {
            std::cout << time_seconds << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
