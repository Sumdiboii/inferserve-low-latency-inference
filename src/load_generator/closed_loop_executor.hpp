#pragma once

#include "open_loop_executor.hpp"

namespace inferserve::load_generator {

[[nodiscard]] std::vector<RequestTimestamps> execute_closed_loop(
    std::size_t request_count,
    const std::function<void(std::size_t, double)>& execute_request);

}  // namespace inferserve::load_generator
