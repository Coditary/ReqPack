#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <build-dir> [cmake-arg ...]" >&2
    exit 1
fi

build_dir="$1"
shift

cmake -Wno-dev -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build_dir" --parallel --target ReqPack core_unit_tests core_integration_tests exec_rules_unit_tests exec_rules_integration_tests
ctest --test-dir "$build_dir" --output-on-failure
