#include <inferserve/server_runtime.hpp>
#include <inferserve/onnx_runtime_backend.hpp>
#include <inferserve/server_options.hpp>
#include <inferserve/synthetic_backend.hpp>

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void request_shutdown(int)
{
    shutdown_requested = 1;
}

}  // namespace

static_assert(__cplusplus >= 202002L);

namespace {

void print_usage()
{
    std::cerr << "Usage:\n"
                 "  inferserve [port] [--backend synthetic|onnx] [--model <path>] [--max-batch-size <N>]\n"
                 "  inferserve --backend onnx --model <path> [--max-batch-size <N>] [port]\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    inferserve::ServerOptions options;
    std::string error;
    if (!inferserve::parse_server_options(argc, argv, options, error)) {
        std::cerr << error << '\n';
        print_usage();
        return 1;
    }
    if (options.help) {
        print_usage();
        return 0;
    }

    try {
        inferserve::SyntheticBackend synthetic_backend;
        inferserve::OnnxRuntimeBackend onnx_backend(options.model_path);
        inferserve::Backend& backend = options.backend == inferserve::ServerBackend::OnnxRuntime
                                           ? static_cast<inferserve::Backend&>(onnx_backend)
                                           : static_cast<inferserve::Backend&>(synthetic_backend);
        inferserve::ServerRuntime server(backend, options.port, options.max_batch_size);
        if (std::signal(SIGINT, request_shutdown) == SIG_ERR ||
            std::signal(SIGTERM, request_shutdown) == SIG_ERR) {
            std::cerr << "Failed to register shutdown signal handlers\n";
            return 1;
        }
        std::cout << "InferServe listening on port " << server.port() << '\n';
        server.run(&shutdown_requested);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
