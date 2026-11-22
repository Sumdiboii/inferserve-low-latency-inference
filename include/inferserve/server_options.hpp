#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace inferserve {

enum class ServerBackend {
    Synthetic,
    OnnxRuntime,
};

struct ServerOptions {
    std::uint16_t port = 8080;
    ServerBackend backend = ServerBackend::Synthetic;
    std::string model_path;
    std::size_t max_batch_size = 32;
    bool help = false;
};

bool parse_server_options(int argc, char* argv[], ServerOptions& options, std::string& error);

}  // namespace inferserve
