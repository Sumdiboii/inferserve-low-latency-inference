#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include <inferserve/backend.hpp>

namespace inferserve {

// Float32 ONNX backend for models with fixed per-request dimensions and either
// a fixed or dynamic leading batch dimension.
class OnnxRuntimeBackend final : public Backend {
public:
    explicit OnnxRuntimeBackend(std::string model_path) : model_path_(std::move(model_path)) {}

    void load() override
    {
        try {
            auto env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "inferserve");
            Ort::SessionOptions options;
            // InferServe parallelizes requests through its worker pool, so limit
            // per-inference intra-op parallelism initially to reduce oversubscription.
            options.SetIntraOpNumThreads(1);
            options.SetInterOpNumThreads(1);
            options.SetExecutionMode(ORT_SEQUENTIAL);
            auto session = std::make_unique<Ort::Session>(*env, model_path_.c_str(), options);

            if (session->GetInputCount() != 1 || session->GetOutputCount() != 1) {
                throw std::runtime_error("model must have exactly one input and one output");
            }

            Ort::AllocatorWithDefaultOptions allocator;
            auto input_name = session->GetInputNameAllocated(0, allocator);
            auto output_name = session->GetOutputNameAllocated(0, allocator);
            auto input_type = session->GetInputTypeInfo(0);
            auto output_type = session->GetOutputTypeInfo(0);
            auto input_info = input_type.GetTensorTypeAndShapeInfo();
            auto output_info = output_type.GetTensorTypeAndShapeInfo();
            if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                throw std::runtime_error(
                    "model input and output must use float32 tensors (found element types " +
                    std::to_string(input_info.GetElementType()) + " and " +
                    std::to_string(output_info.GetElementType()) + ")");
            }

            std::vector<int64_t> input_shape = input_info.GetShape();
            std::vector<int64_t> output_shape = output_info.GetShape();
            validate_shape(input_shape, "input");
            validate_shape(output_shape, "output");
            const std::size_t max_batch_size = input_shape.front() == -1
                ? static_cast<std::size_t>(std::numeric_limits<int64_t>::max())
                : static_cast<std::size_t>(input_shape.front());

            input_name_ = input_name.get();
            output_name_ = output_name.get();
            input_shape_ = std::move(input_shape);
            output_shape_ = std::move(output_shape);
            max_batch_size_ = max_batch_size;
            session_ = std::move(session);
            env_ = std::move(env);
        } catch (const Ort::Exception& error) {
            throw std::runtime_error("failed to load ONNX model '" + model_path_ + "': " + error.what());
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("invalid ONNX model '" + model_path_ + "': " + error.what());
        }
    }

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        if (!session_) {
            throw std::logic_error("ONNX Runtime backend must be loaded before inference");
        }
        if (batch.empty() || batch.size() > max_batch_size_) {
            throw std::invalid_argument("batch size is empty or exceeds the model's supported batch size");
        }
        if (input_shape_.front() > 0 &&
            batch.size() != static_cast<std::size_t>(input_shape_.front())) {
            throw std::invalid_argument("batch size does not match the model's fixed batch dimension");
        }

        const std::size_t input_elements = elements_per_request(input_shape_);
        if (batch.size() > std::vector<float>{}.max_size() / input_elements) {
            throw std::invalid_argument("batch input tensor is too large");
        }
        std::vector<float> packed_input;
        packed_input.reserve(batch.size() * input_elements);
        for (const Data& input : batch) {
            if (input.size() != input_elements) {
                throw std::invalid_argument("input data size does not match the model input shape");
            }
            packed_input.insert(packed_input.end(), input.begin(), input.end());
        }

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> actual_input_shape = input_shape_;
        actual_input_shape.front() = static_cast<int64_t>(batch.size());
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory_info, packed_input.data(), packed_input.size(),
            actual_input_shape.data(), actual_input_shape.size());
        const char* input_names[] = {input_name_.c_str()};
        const char* output_names[] = {output_name_.c_str()};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        if (outputs.size() != 1 || !outputs.front().IsTensor()) {
            throw std::runtime_error("ONNX model did not return one tensor output");
        }

        const auto output_info = outputs.front().GetTensorTypeAndShapeInfo();
        std::vector<int64_t> actual_output_shape = output_shape_;
        actual_output_shape.front() = static_cast<int64_t>(batch.size());
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            output_info.GetShape() != actual_output_shape) {
            throw std::runtime_error("ONNX model returned an unexpected output tensor type or shape");
        }

        const std::size_t output_elements = elements_per_request(output_shape_);
        if (batch.size() > std::numeric_limits<std::size_t>::max() / output_elements ||
            output_info.GetElementCount() != batch.size() * output_elements) {
            throw std::runtime_error("ONNX model returned an unexpected output tensor element count");
        }
        const float* output_data = outputs.front().GetTensorData<float>();
        InferenceResults results;
        results.reserve(batch.size());
        for (std::size_t index = 0; index < batch.size(); ++index) {
            const float* first = output_data + index * output_elements;
            results.emplace_back(first, first + output_elements);
        }
        return results;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return max_batch_size_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "onnxruntime"; }
    [[nodiscard]] const std::vector<int64_t>& input_shape() const noexcept { return input_shape_; }
    [[nodiscard]] const std::vector<int64_t>& output_shape() const noexcept { return output_shape_; }

private:
    static void validate_shape(const std::vector<int64_t>& shape, const char* label)
    {
        if (shape.empty() || shape.front() == 0 || shape.front() < -1) {
            throw std::runtime_error(std::string(label) + " must have a positive or dynamic leading batch dimension");
        }
        for (std::size_t index = 1; index < shape.size(); ++index) {
            if (shape[index] <= 0) {
                throw std::runtime_error(std::string(label) + " non-batch dimensions must be fixed and positive");
            }
        }
    }

    static std::size_t elements_per_request(const std::vector<int64_t>& shape)
    {
        std::size_t count = 1;
        for (std::size_t index = 1; index < shape.size(); ++index) {
            const std::size_t dimension = static_cast<std::size_t>(shape[index]);
            if (count > std::numeric_limits<std::size_t>::max() / dimension) {
                throw std::runtime_error("model tensor shape is too large");
            }
            count *= dimension;
        }
        return count;
    }

    std::string model_path_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::string input_name_;
    std::string output_name_;
    std::vector<int64_t> input_shape_;
    std::vector<int64_t> output_shape_;
    std::size_t max_batch_size_ = 1;
};

}  // namespace inferserve
