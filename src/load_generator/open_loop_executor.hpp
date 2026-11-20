#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace inferserve::load_generator {

void execute_open_loop(const std::vector<double>& scheduled_arrivals_seconds,
                       const std::function<void(std::size_t, double)>& execute_request);

}  // namespace inferserve::load_generator
