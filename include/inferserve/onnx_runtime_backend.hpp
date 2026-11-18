#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include <inferserve/backend.hpp>

namespace inferserve {

// Minimal fixed-shape, float32 ONNX backend. This phase intentionally supports
// one request per inference call; general batch packing is handled separately.
class OnnxRuntimeBackend final : public Backend {
public:
    explicit OnnxRuntimeBackend(std::string model_path) : model_path_(std::move(model_path)) {}

    void load() override
    {
        try {
            auto env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "inferserve");
            Ort::SessionOptions options;
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

            input_name_ = input_name.get();
            output_name_ = output_name.get();
            input_shape_ = std::move(input_shape);
            output_shape_ = std::move(output_shape);
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
        if (batch.size() != 1) {
            throw std::invalid_argument("ONNX Runtime backend supports batch size one");
        }
        const Data& input = batch.front();
        if (input.size() != element_count(input_shape_)) {
            throw std::invalid_argument("input data size does not match the model input shape");
        }

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memory_info, const_cast<float*>(input.data()), input.size(), input_shape_.data(), input_shape_.size());
        const char* input_names[] = {input_name_.c_str()};
        const char* output_names[] = {output_name_.c_str()};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        const float* output_data = outputs.front().GetTensorData<float>();
        const std::size_t output_size = element_count(output_shape_);
        return {Data(output_data, output_data + output_size)};
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 1; }
    [[nodiscard]] std::string_view name() const noexcept override { return "onnxruntime"; }
    [[nodiscard]] const std::vector<int64_t>& input_shape() const noexcept { return input_shape_; }
    [[nodiscard]] const std::vector<int64_t>& output_shape() const noexcept { return output_shape_; }

private:
    static void validate_shape(const std::vector<int64_t>& shape, const char* label)
    {
        if (shape.empty() || shape.front() != 1) {
            throw std::runtime_error(std::string(label) + " must have a fixed batch dimension of one");
        }
        for (const int64_t dimension : shape) {
            if (dimension <= 0) {
                throw std::runtime_error(std::string(label) + " dimensions must be fixed and positive");
            }
        }
    }

    static std::size_t element_count(const std::vector<int64_t>& shape)
    {
        std::size_t count = 1;
        for (const int64_t dimension : shape) {
            count *= static_cast<std::size_t>(dimension);
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
};

}  // namespace inferserve
