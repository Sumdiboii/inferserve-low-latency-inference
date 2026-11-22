#include <gtest/gtest.h>

#include "csv_export.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::write_records_csv;
using inferserve::load_generator::RequestTimestamps;

TEST(CsvExport, WritesStableHeaderValuesAndRequestOrder)
{
    using Clock = std::chrono::steady_clock;
    const auto origin = Clock::time_point{};
    const std::vector<RequestTimestamps> records{
        {9, origin, origin + 10ms, origin + 12ms, origin + 15ms},
        {2, origin, origin + 20ms, origin + 25ms, origin + 30ms},
    };
    std::ostringstream output;

    write_records_csv(output, records);

    EXPECT_EQ(output.str(),
              "request_index,scheduled_arrival_ms,actual_send_ms,completion_ms,dispatch_lag_ms,http_status,success,error,warmup,dispatched,terminal_failure\n"
              "9,10.000,12.000,15.000,2.000,,true,,false,true,false\n"
              "2,20.000,25.000,30.000,5.000,,true,,false,true,false\n");
}

TEST(CsvExport, IncludesHttpStatusAndEscapesFailureMessage)
{
    using Clock = std::chrono::steady_clock;
    const auto origin = Clock::time_point{};
    std::vector<RequestTimestamps> records{
        {0, origin, origin, origin, origin, {false, 503, "HTTP 503: {\"error\":\"busy, retry\"}"}},
    };
    std::ostringstream output;

    write_records_csv(output, records);

    EXPECT_NE(output.str().find("0,0.000,0.000,0.000,0.000,503,false,\"HTTP 503: {\"\"error\"\":\"\"busy, retry\"\"}\""),
              std::string::npos);
}

TEST(CsvExport, MarksWarmupPrefixAndPreservesEveryRow)
{
    using Clock = std::chrono::steady_clock;
    const auto origin = Clock::time_point{};
    const std::vector<RequestTimestamps> records{
        {0, origin, origin, origin + 1ms, origin + 5ms},
        {1, origin, origin + 10ms, origin + 12ms, origin + 20ms},
        {2, origin, origin + 20ms, origin + 22ms, origin + 30ms},
    };
    std::ostringstream output;

    write_records_csv(output, records, 1);

    EXPECT_NE(output.str().find("0,0.000"), std::string::npos);
    EXPECT_NE(output.str().find(",true,true,false\n"), std::string::npos);
    EXPECT_NE(output.str().find(",true,true,false\n"), std::string::npos);
    EXPECT_NE(output.str().find(",false,true,false\n"), std::string::npos);
    EXPECT_NE(output.str().find("2,20.000"), std::string::npos);
}

}  // namespace
