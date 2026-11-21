#pragma once

#include "open_loop_executor.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace inferserve::load_generator {

void write_records_csv(std::ostream& output, const std::vector<RequestTimestamps>& records,
                       std::size_t warmup_count = 0);
void export_records_csv(const std::string& output_path, const std::vector<RequestTimestamps>& records,
                        std::size_t warmup_count = 0);

}  // namespace inferserve::load_generator
