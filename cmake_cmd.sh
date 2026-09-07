#!/usr/bin/env bash
# Build POD5 from source (DEV.md):
#   git submodule update --init --recursive
#   generate version files
#   mkdir build && cd build && cmake .. && make -j
#
# Usage:
#   ./cmake_cmd.sh
#   ./cmake_cmd.sh -DCMAKE_BUILD_TYPE=Debug
# Default: -DPOD5_DISABLE_TESTS=OFF (builds pod5_unit_tests incl. c_api_tests.cpp)
# Run tests: cd build && ctest --output-on-failure
#   or run the binary: build/Release/bin/pod5_unit_tests
#
# Optional env:
#   PYTHON=...                # override default /softs/anaconda3/bin/python3
#   CMAKE_PREFIX_PATH=...

set -euo pipefail

cd "$(dirname -- "${BASH_SOURCE[0]}")"

PYTHON="${PYTHON:-/softs/anaconda3/bin/python3}"

git submodule update --init --recursive

# setuptools_scm CLI only prints; force write _version.py then POD5Version.cmake
"$PYTHON" -c "from setuptools_scm import get_version; print(get_version(write_to='_version.py'))"
"$PYTHON" ./pod5_make_version.py

mkdir -p build
cd build
# c_api_tests.cpp 等单测在 POD5_DISABLE_TESTS=OFF 时编译为 pod5_unit_tests
cmake -DPOD5_DISABLE_TESTS=OFF "$@" ..
make -j
