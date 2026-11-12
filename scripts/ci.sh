#!/usr/bin/env sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cmake_command=${CMAKE_COMMAND:-cmake}
ctest_command=${CTEST_COMMAND:-ctest}

cd "$repository_dir"

printf '%s\n' 'Configuring Debug build...'
"$cmake_command" --preset debug

printf '%s\n' 'Building Debug build...'
"$cmake_command" --build --preset debug

printf '%s\n' 'Running Debug tests...'
"$ctest_command" --preset debug --output-on-failure
