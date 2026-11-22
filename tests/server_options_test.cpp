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
    EXPECT_EQ(options.max_batch_size, 32U);
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

TEST(ServerOptions, ParsesExperimentMaxBatchSizes)
{
    char executable[] = "inferserve";
    char option[] = "--max-batch-size";
    char disabled[] = "1";
    char* disabled_argv[] = {executable, option, disabled};
    ServerOptions options;
    std::string error;

    ASSERT_TRUE(parse_server_options(3, disabled_argv, options, error));
    EXPECT_EQ(options.max_batch_size, 1U);

    char enabled[] = "8";
    char* enabled_argv[] = {executable, option, enabled};
    ASSERT_TRUE(parse_server_options(3, enabled_argv, options, error));
    EXPECT_EQ(options.max_batch_size, 8U);
}

TEST(ServerOptions, RejectsInvalidMaxBatchSize)
{
    char executable[] = "inferserve";
    char option[] = "--max-batch-size";
    char zero[] = "0";
    char* zero_argv[] = {executable, option, zero};
    ServerOptions options;
    std::string error;

    EXPECT_FALSE(parse_server_options(3, zero_argv, options, error));
    EXPECT_NE(error.find("positive integer"), std::string::npos);

    char* missing_argv[] = {executable, option};
    EXPECT_FALSE(parse_server_options(2, missing_argv, options, error));
    EXPECT_NE(error.find("requires a value"), std::string::npos);
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
