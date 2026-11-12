cmake_minimum_required(VERSION 3.25)

project(InferServe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INFERSERVE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(INFERSERVE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(INFERSERVE_ENABLE_ASAN AND INFERSERVE_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()

add_executable(inferserve src/main.cpp)
set_target_properties(inferserve PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

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

    target_compile_options(inferserve PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(inferserve PRIVATE -fsanitize=address)
endif()

if(INFERSERVE_ENABLE_TSAN)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_cxx_compiler_flag("-fsanitize=thread" INFERSERVE_SUPPORTS_TSAN)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT INFERSERVE_SUPPORTS_TSAN)
        message(FATAL_ERROR "The selected compiler does not support ThreadSanitizer")
    endif()

    target_compile_options(inferserve PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(inferserve PRIVATE -fsanitize=thread)
endif()
