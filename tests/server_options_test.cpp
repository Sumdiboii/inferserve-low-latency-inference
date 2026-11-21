#include <inferserve/server_options.hpp>

#include <gtest/gtest.h>

namespace {

using inferserve::parse_server_options;
using inferserve::ServerBackend;
using inferserve::ServerOptions;

TEST(ServerOptions, DefaultsToSyntheticBackendAndExistingPort)
{
    char executable[] = "inferserve";
    char* argv[] = {executable};
    ServerOptions options;
    std::string error;

    ASSERT_TRUE(parse_server_options(1, argv, options, error));
    EXPECT_EQ(options.backend, ServerBackend::Synthetic);
    EXPECT_EQ(options.port, 8080);
    EXPECT_TRUE(options.model_path.empty());
}

TEST(ServerOptions, PreservesPositionalPort)
{
    char executable[] = "inferserve";
    char port[] = "9090";
    char* argv[] = {executable, port};
    ServerOptions options;
    std::string error;

    ASSERT_TRUE(parse_server_options(2, argv, options, error));
    EXPECT_EQ(options.port, 9090);
    EXPECT_EQ(options.backend, ServerBackend::Synthetic);
}

TEST(ServerOptions, ParsesOnnxBackendWithModel)
{
    char executable[] = "inferserve";
    char backend[] = "--backend";
    char onnx[] = "onnx";
    char model[] = "--model";
    char path[] = "tests/data/identity_dynamic_batch_2.onnx";
    char* argv[] = {executable, backend, onnx, model, path};
    ServerOptions options;
    std::string error;

    ASSERT_TRUE(parse_server_options(5, argv, options, error));
    EXPECT_EQ(options.backend, ServerBackend::OnnxRuntime);
    EXPECT_EQ(options.model_path, "tests/data/identity_dynamic_batch_2.onnx");
}

TEST(ServerOptions, RejectsInvalidBackendAndMissingOnnxModel)
{
    char executable[] = "inferserve";
    char backend[] = "--backend";
    char invalid[] = "invalid";
    char* invalid_argv[] = {executable, backend, invalid};
    ServerOptions options;
    std::string error;

    EXPECT_FALSE(parse_server_options(3, invalid_argv, options, error));
    EXPECT_NE(error.find("synthetic"), std::string::npos);

    char onnx[] = "onnx";
    char* missing_model_argv[] = {executable, backend, onnx};
    options = ServerOptions{};
    EXPECT_FALSE(parse_server_options(3, missing_model_argv, options, error));
    EXPECT_NE(error.find("--model"), std::string::npos);
}

}  // namespace
