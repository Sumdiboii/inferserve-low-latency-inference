#include <inferserve/completion_registry.hpp>

#include <algorithm>
#include <utility>

namespace inferserve {

void CompletionRegistry::record_pending(RequestId request_id)
{
    const std::lock_guard lock(mutex_);
    evict_expired();
    terminal_.erase(request_id.value());
    const auto existing = std::find_if(
        terminal_order_.begin(),
        terminal_order_.end(),
        [request_id](const TerminalEntry& entry) {
            return entry.request_id == request_id;
        });
    if (existing != terminal_order_.end()) {
        terminal_order_.erase(existing);
    }
    pending_[request_id.value()] = CompletionRecord{};
}

void CompletionRegistry::record_terminal(RequestId request_id, Status status, std::string error)
{
    const std::lock_guard lock(mutex_);
    evict_expired();
    pending_.erase(request_id.value());

    const TimePoint now = MonotonicClock{}.now();
    CompletionRecord record;
    record.state = CompletionState::Terminal;
    record.status = status;
    record.completed_at = now;
    record.error = std::move(error);
    if (terminal_.find(request_id.value()) != terminal_.end()) {
        terminal_[request_id.value()] = record;
        const auto existing = std::find_if(
            terminal_order_.begin(),
            terminal_order_.end(),
            [request_id](const TerminalEntry& entry) {
                return entry.request_id == request_id;
            });
        if (existing != terminal_order_.end()) {
            terminal_order_.erase(existing);
        }
        terminal_order_.push_back(TerminalEntry{request_id, record, now});
        return;
    }
    terminal_[request_id.value()] = record;
    terminal_order_.push_back(TerminalEntry{request_id, record, now});

    while (terminal_.size() > terminal_capacity_) {
        const auto oldest = terminal_order_.front();
        terminal_order_.pop_front();
        terminal_.erase(oldest.request_id.value());
    }
}

std::optional<CompletionRecord> CompletionRegistry::get(RequestId request_id)
{
    const std::lock_guard lock(mutex_);
    evict_expired();
    const auto pending = pending_.find(request_id.value());
    if (pending != pending_.end()) {
        return pending->second;
    }
    const auto terminal = terminal_.find(request_id.value());
    if (terminal != terminal_.end()) {
        return terminal->second;
    }
    return std::nullopt;
}

void CompletionRegistry::evict_expired()
{
    const TimePoint now = MonotonicClock{}.now();
    while (!terminal_order_.empty() &&
           now - terminal_order_.front().completed_at >= terminal_ttl_) {
        const auto oldest = terminal_order_.front();
        terminal_order_.pop_front();
        terminal_.erase(oldest.request_id.value());
    }
}

}  // namespace inferserve
