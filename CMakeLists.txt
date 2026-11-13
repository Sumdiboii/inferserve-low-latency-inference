cmake_minimum_required(VERSION 3.25)

project(InferServe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INFERSERVE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(INFERSERVE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
set(INFERSERVE_ONNXRUNTIME_ROOT "" CACHE PATH "Root of a locally installed ONNX Runtime CPU package")

if(INFERSERVE_ENABLE_ASAN AND INFERSERVE_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()

add_executable(inferserve src/main.cpp)
set_target_properties(inferserve PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(inferserve PRIVATE include)

add_library(inferserve_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(inferserve_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
elseif(MSVC)
    target_compile_options(inferserve_warnings INTERFACE /W4 /WX)
else()
    message(FATAL_ERROR "Unsupported compiler for InferServe warning policy: ${CMAKE_CXX_COMPILER_ID}")
endif()

target_link_libraries(inferserve PRIVATE inferserve_warnings)

if(INFERSERVE_ONNXRUNTIME_ROOT)
    set(_inferserve_onnxruntime_root "${INFERSERVE_ONNXRUNTIME_ROOT}")
else()
    set(_inferserve_onnxruntime_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-linux-x64-1.20.1")
endif()
find_path(INFERSERVE_ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h HINTS "${_inferserve_onnxruntime_root}/include" NO_DEFAULT_PATH)
if(EXISTS "${_inferserve_onnxruntime_root}/lib/libonnxruntime.so.1.20.1")
    set(INFERSERVE_ONNXRUNTIME_LIBRARY "${_inferserve_onnxruntime_root}/lib/libonnxruntime.so.1.20.1")
endif()
if(INFERSERVE_ONNXRUNTIME_INCLUDE_DIR AND INFERSERVE_ONNXRUNTIME_LIBRARY)
    add_library(inferserve_onnxruntime UNKNOWN IMPORTED)
    set_target_properties(inferserve_onnxruntime PROPERTIES IMPORTED_LOCATION "${INFERSERVE_ONNXRUNTIME_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${INFERSERVE_ONNXRUNTIME_INCLUDE_DIR}")
    message(STATUS "Found ONNX Runtime: ${INFERSERVE_ONNXRUNTIME_LIBRARY}")
else()
    message(STATUS "ONNX Runtime not found; inference backend remains unavailable")
endif()

add_library(inferserve_sanitizers INTERFACE)
target_link_libraries(inferserve PRIVATE inferserve_sanitizers)

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(inferserve_tests tests/smoke_test.cpp)
    target_include_directories(inferserve_tests PRIVATE include)
    target_link_libraries(inferserve_tests PRIVATE
        GTest::gtest_main
        inferserve_warnings
        inferserve_sanitizers
    )

    include(GoogleTest)
    gtest_discover_tests(inferserve_tests)
endif()

if(INFERSERVE_ENABLE_ASAN OR INFERSERVE_ENABLE_TSAN)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Sanitizer presets are supported only on Linux")
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizer presets require GCC or Clang")
    endif()

    include(CheckCXXCompilerFlag)
endif()

if(INFERSERVE_ENABLE_ASAN)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
    check_cxx_compiler_flag("-fsanitize=address" INFERSERVE_SUPPORTS_ASAN)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT INFERSERVE_SUPPORTS_ASAN)
        message(FATAL_ERROR "The selected compiler does not support AddressSanitizer")
    endif()

    target_compile_options(inferserve_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(inferserve_sanitizers INTERFACE -fsanitize=address)
endif()

if(INFERSERVE_ENABLE_TSAN)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_cxx_compiler_flag("-fsanitize=thread" INFERSERVE_SUPPORTS_TSAN)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT INFERSERVE_SUPPORTS_TSAN)
        message(FATAL_ERROR "The selected compiler does not support ThreadSanitizer")
    endif()

    target_compile_options(inferserve_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(inferserve_sanitizers INTERFACE -fsanitize=thread)
endif()
