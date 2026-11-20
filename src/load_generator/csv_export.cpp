#include "csv_export.hpp"

#include "dispatch_lag.hpp"

#include <fstream>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>

namespace inferserve::load_generator {
namespace {

double milliseconds_since(std::chrono::steady_clock::time_point timestamp,
                          std::chrono::steady_clock::time_point generator_start)
{
    return std::chrono::duration<double, std::milli>(timestamp - generator_start).count();
}

}  // namespace

void write_records_csv(std::ostream& output, const std::vector<RequestTimestamps>& records)
{
    const auto dispatch_lags = calculate_dispatch_lags(records);
    const auto original_flags = output.flags();
    const auto original_precision = output.precision();

    output << "request_index,scheduled_arrival_ms,actual_send_ms,completion_ms,dispatch_lag_ms\n";
    output << std::fixed << std::setprecision(3);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        output << record.request_index << ',' << milliseconds_since(record.scheduled_arrival, record.generator_start)
               << ',' << milliseconds_since(record.actual_send, record.generator_start) << ','
               << milliseconds_since(record.completion, record.generator_start) << ','
               << dispatch_lags[index].milliseconds << '\n';
    }

    output.flags(original_flags);
    output.precision(original_precision);
    if (!output) {
        throw std::runtime_error("failed to write CSV records");
    }
}

void export_records_csv(const std::string& output_path, const std::vector<RequestTimestamps>& records)
{
    std::ofstream output(output_path);
    if (!output) {
        throw std::runtime_error("could not open CSV output file: " + output_path);
    }
    write_records_csv(output, records);
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to write CSV output file: " + output_path);
    }
}

}  // namespace inferserve::load_generator
