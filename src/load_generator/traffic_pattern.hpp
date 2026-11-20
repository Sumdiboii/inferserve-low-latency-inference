#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace inferserve::load_generator {

[[nodiscard]] std::vector<double> fixed_rate_arrivals(std::size_t count, double rate_per_second);
[[nodiscard]] std::vector<double> burst_arrivals(std::size_t count);
[[nodiscard]] std::vector<double> poisson_arrivals(std::size_t count, double rate_per_second, std::uint64_t seed);

}  // namespace inferserve::load_generator
