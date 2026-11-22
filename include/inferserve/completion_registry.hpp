#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <inferserve/core_types.hpp>

namespace inferserve {

enum class CompletionState {
    Pending,
    Terminal,
};

struct CompletionRecord {
    CompletionState state = CompletionState::Pending;
    std::optional<Status> status;
    std::optional<TimePoint> completed_at;
    std::string error;
};

class CompletionRegistry {
public:
    CompletionRegistry(
        std::size_t terminal_capacity,
        std::chrono::steady_clock::duration terminal_ttl)
        : terminal_capacity_(terminal_capacity), terminal_ttl_(terminal_ttl)
    {
    }

    void record_pending(RequestId request_id);
    void record_terminal(RequestId request_id, Status status, std::string error = {});
    [[nodiscard]] std::optional<CompletionRecord> get(RequestId request_id);
    void evict_expired();

private:
    struct TerminalEntry {
        RequestId request_id;
        CompletionRecord record;
        TimePoint completed_at;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, CompletionRecord> pending_;
    std::unordered_map<std::uint64_t, CompletionRecord> terminal_;
    std::deque<TerminalEntry> terminal_order_;
    std::size_t terminal_capacity_;
    std::chrono::steady_clock::duration terminal_ttl_;
};

}  // namespace inferserve
