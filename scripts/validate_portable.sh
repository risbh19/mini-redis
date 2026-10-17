#!/usr/bin/env bash
# Local optional tooling: extracted Ubuntu packages in .tools/root (never committed).
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
export LD_LIBRARY_PATH="$root/.tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
clang="$root/.tools/root/usr/lib/llvm-21/bin/clang++"
formatter="$root/.tools/root/usr/lib/llvm-21/bin/clang-format"
case "${1:-format}" in
  format)
    find src tests/unit tests/fuzz bench -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0 | xargs -0 "$formatter" -i
    ;;
  clang)
    cmake -S . -B build-clang -DCMAKE_CXX_COMPILER="$clang" -DCMAKE_BUILD_TYPE=Release -DBUILD_FUZZER=ON -DBUILD_BENCHMARKS=ON
    cmake --build build-clang -j 4
    ctest --test-dir build-clang --output-on-failure -j 4
    ;;
  asan)
    cmake -S . -B build-asan -DCMAKE_CXX_COMPILER="$clang" -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
    cmake --build build-asan -j 4
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure -j 4
    ;;
  tsan)
    cmake -S . -B build-tsan -DCMAKE_CXX_COMPILER="$clang" -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
    cmake --build build-tsan -j 4
    TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure -j 4
    ;;
  *) echo 'Use format|clang|asan|tsan' >&2; exit 1 ;;
esac
