#pragma once

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>

#include <inferserve/backend.hpp>

namespace inferserve {

struct SyntheticBackendConfig {
    std::chrono::milliseconds fixed_ms{1};
    std::chrono::milliseconds per_item_ms{1};
    std::size_t max_batch_size{8};
};

class SyntheticBackend final : public Backend {
public:
    explicit SyntheticBackend(SyntheticBackendConfig config = {}) : config_(config)
    {
        if (config_.fixed_ms.count() < 0 || config_.per_item_ms.count() < 0 ||
            config_.max_batch_size == 0) {
            throw std::invalid_argument("invalid synthetic backend configuration");
        }
    }

    SyntheticBackend(
        std::size_t max_batch_size,
        std::chrono::milliseconds fixed_cost,
        std::chrono::milliseconds per_item_cost)
        : SyntheticBackend(SyntheticBackendConfig{fixed_cost, per_item_cost, max_batch_size})
    {
    }

    void load() override {}

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        const auto cost = config_.fixed_ms + config_.per_item_ms * batch.size();
        std::this_thread::sleep_for(cost);

        InferenceResults results;
        results.reserve(batch.size());
        for (const Data& input : batch) {
            // This is synthetic work, not model inference: return the input length as one value.
            results.push_back(Data{static_cast<float>(input.size())});
        }
        return results;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return config_.max_batch_size; }

    [[nodiscard]] std::string_view name() const noexcept override { return "synthetic"; }

private:
    const SyntheticBackendConfig config_;
};

}  // namespace inferserve
