#include <inferserve/server_options.hpp>

#include <charconv>
#include <string_view>
#include <system_error>

namespace inferserve {

bool parse_server_options(int argc, char* argv[], ServerOptions& options, std::string& error)
{
    bool has_port = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--backend") {
            if (++index >= argc) {
                error = "--backend requires a value";
                return false;
            }
            const std::string_view value(argv[index]);
            if (value == "synthetic") {
                options.backend = ServerBackend::Synthetic;
            } else if (value == "onnx") {
                options.backend = ServerBackend::OnnxRuntime;
            } else {
                error = "backend must be 'synthetic' or 'onnx'";
                return false;
            }
        } else if (argument == "--model") {
            if (++index >= argc) {
                error = "--model requires a path";
                return false;
            }
            options.model_path = argv[index];
        } else if (argument == "--max-batch-size") {
            if (++index >= argc) {
                error = "--max-batch-size requires a value";
                return false;
            }
            unsigned int max_batch_size = 0;
            const std::string_view value(argv[index]);
            const auto [end, parse_error] =
                std::from_chars(value.data(), value.data() + value.size(), max_batch_size);
            if (parse_error != std::errc{} ||
                end != value.data() + value.size() ||
                max_batch_size == 0) {
                error = "max batch size must be a positive integer";
                return false;
            }
            options.max_batch_size = max_batch_size;
        } else if (!argument.empty() && argument.front() != '-' && !has_port) {
            unsigned int port = 0;
            const auto [end, parse_error] =
                std::from_chars(argument.data(), argument.data() + argument.size(), port);
            if (parse_error != std::errc{} || end != argument.data() + argument.size() ||
                port > 65535) {
                error = "port must be an integer between 0 and 65535";
                return false;
            }
            options.port = static_cast<std::uint16_t>(port);
            has_port = true;
        } else {
            error = "unknown argument: " + std::string(argument);
            return false;
        }
    }

    if (options.help) {
        return true;
    }
    if (options.backend == ServerBackend::OnnxRuntime && options.model_path.empty()) {
        error = "--backend onnx requires --model <path>";
        return false;
    }
    if (options.backend == ServerBackend::Synthetic && !options.model_path.empty()) {
        error = "--model is only valid with --backend onnx";
        return false;
    }
    return true;
}

}  // namespace inferserve
