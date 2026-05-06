#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <build-dir> [cmake-arg ...]" >&2
    exit 1
fi

build_dir="$1"
shift

cmake -Wno-dev -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build_dir" --parallel --target ReqPack reqpack_test_targets

if ctest --test-dir "$build_dir" -N 2>&1 | grep -F '_NOT_BUILT-' >/dev/null; then
    echo "error: discovered tests contain _NOT_BUILT_ placeholders" >&2
    exit 1
fi

ctest --test-dir "$build_dir" --output-on-failure
