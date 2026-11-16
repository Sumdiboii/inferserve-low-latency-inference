#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include <inferserve/core_types.hpp>

namespace inferserve {

using Batch = std::vector<Data>;
using InferenceResults = std::vector<Data>;

class Backend {
public:
    virtual ~Backend() = default;

    virtual void load() = 0;
    [[nodiscard]] virtual InferenceResults infer(const Batch& batch) = 0;
    [[nodiscard]] virtual std::size_t max_batch_size() const = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

}  // namespace inferserve
