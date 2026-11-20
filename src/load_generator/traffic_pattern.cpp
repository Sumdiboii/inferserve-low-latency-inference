#include "traffic_pattern.hpp"

#include <cmath>
#include <random>
#include <stdexcept>

namespace inferserve::load_generator {
namespace {

void validate_rate(double rate_per_second)
{
    if (!std::isfinite(rate_per_second) || rate_per_second <= 0.0) {
        throw std::invalid_argument("arrival rate must be a finite positive number");
    }
}

}  // namespace

std::vector<double> fixed_rate_arrivals(std::size_t count, double rate_per_second)
{
    validate_rate(rate_per_second);
    const double interval = 1.0 / rate_per_second;
    std::vector<double> arrivals;
    arrivals.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        arrivals.push_back(static_cast<double>(index) * interval);
    }
    return arrivals;
}

std::vector<double> burst_arrivals(std::size_t count)
{
    return std::vector<double>(count, 0.0);
}

std::vector<double> poisson_arrivals(std::size_t count, double rate_per_second, std::uint64_t seed)
{
    validate_rate(rate_per_second);
    std::mt19937_64 generator(seed);
    std::exponential_distribution<double> inter_arrival(rate_per_second);

    std::vector<double> arrivals;
    arrivals.reserve(count);
    double scheduled_time = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        scheduled_time += inter_arrival(generator);
        arrivals.push_back(scheduled_time);
    }
    return arrivals;
}

}  // namespace inferserve::load_generator
