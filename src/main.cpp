#include <inferserve/http_server.hpp>
#include <inferserve/request_admission.hpp>

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

int main(int argc, char* argv[])
{
    std::uint16_t port = 8080;
    if (argc > 2) {
        std::cerr << "Usage: inferserve [port]\n";
        return 1;
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        unsigned int parsed_port = 0;
        const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), parsed_port);
        if (error != std::errc{} || end != argument.data() + argument.size() || parsed_port > 65535) {
            std::cerr << "Port must be an integer between 0 and 65535\n";
            return 1;
        }
        port = static_cast<std::uint16_t>(parsed_port);
    }

    try {
        inferserve::RequestAdmission admission(1024);
        inferserve::HttpServer server(admission, port);
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
