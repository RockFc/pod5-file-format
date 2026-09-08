#!/usr/bin/env bash
# 编译并运行 pod5_unit_tests 中的 mytest 用例（参考 cytools/quick_test_ccf5_copmression.sh）
#
# 用法:
#   ./quick_test_mytest.sh                         # 默认跑 [mytest3]
#   ./quick_test_mytest.sh "[mytest3]"
#   ./quick_test_mytest.sh "[mytest4]"
#   ./quick_test_mytest.sh "[mytest3],[mytest4]"
#   ./quick_test_mytest.sh "[mytest1],[mytest2],[mytest3]"
#
# 可选环境变量:
#   BUILD_DIR=build                  # cmake 构建目录（相对仓库根）
#   POD5_DEPS_LIB=...                # 覆盖默认 ~/.conda/envs/pod5-deps/lib
#   JOBS=$(nproc)                    # 并行编译核数

set -euo pipefail

script_abs=$(readlink -f "$0")
script_dir=$(dirname "$script_abs")
cd "$script_dir"

build_dir="${BUILD_DIR:-build}"
bin="$script_dir/$build_dir/c++/test/pod5_unit_tests"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

deps_lib="${POD5_DEPS_LIB:-$HOME/.conda/envs/pod5-deps/lib}"
if [[ ! -d "$deps_lib" ]]; then
  deps_lib=""
fi

filter="${1:-[mytest3]}"
if [[ $# -gt 0 ]]; then
  shift
fi

if [[ ! -d "$build_dir" ]]; then
  echo "error: build dir '$build_dir' not found; run ./cmake_cmd.sh first" >&2
  exit 1
fi

echo "[1/2] compile pod5_unit_tests (-j$jobs) ..."
cmake --build "$build_dir" --target pod5_unit_tests -j"$jobs"

if [[ ! -x "$bin" ]]; then
  echo "error: binary not found: $bin" >&2
  exit 1
fi

# 在二进制所在目录运行，使 mytest3/mytest4 里 ../../../test_data 相对路径正确
# （与 docs 约定一致：cd build/c++/test && ./pod5_unit_tests ...）
bin_dir=$(dirname "$bin")
echo "[2/2] run (cwd=$bin_dir) ./$(basename "$bin") \"$filter\" $*"
if [[ -n "$deps_lib" ]]; then
  export LD_LIBRARY_PATH="$deps_lib:${LD_LIBRARY_PATH:-}"
fi
cd "$bin_dir"
./"$(basename "$bin")" "$filter" "$@"
