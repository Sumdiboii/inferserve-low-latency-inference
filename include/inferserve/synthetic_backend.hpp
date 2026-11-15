#pragma once

#include <chrono>
#include <cstddef>
#include <thread>

#include <inferserve/backend.hpp>

namespace inferserve {

class SyntheticBackend final : public Backend {
public:
    SyntheticBackend(
        std::size_t max_batch_size,
        std::chrono::milliseconds fixed_cost,
        std::chrono::milliseconds per_item_cost)
        : max_batch_size_(max_batch_size), fixed_cost_(fixed_cost), per_item_cost_(per_item_cost)
    {
    }

    void load() override {}

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        const auto cost = fixed_cost_ + per_item_cost_ * batch.size();
        std::this_thread::sleep_for(cost);

        InferenceResults results;
        results.reserve(batch.size());
        for (const Data& input : batch) {
            // This is synthetic work, not model inference: return the input length as one value.
            results.push_back(Data{static_cast<float>(input.size())});
        }
        return results;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return max_batch_size_; }

    [[nodiscard]] std::string_view name() const noexcept override { return "synthetic"; }

private:
    const std::size_t max_batch_size_;
    const std::chrono::milliseconds fixed_cost_;
    const std::chrono::milliseconds per_item_cost_;
};

}  // namespace inferserve
