#pragma once

#include "open_loop_executor.hpp"

#include <cstdint>
#include <string_view>

namespace inferserve::load_generator {

[[nodiscard]] RequestResult send_infer_request(std::string_view host, std::uint16_t port);

}  // namespace inferserve::load_generator
