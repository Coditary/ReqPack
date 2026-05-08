#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <build-dir>" >&2
    exit 1
fi

build_dir="$1"

if ctest --test-dir "$build_dir" -N 2>&1 | grep -F '_NOT_BUILT_' >/dev/null; then
    echo "error: discovered tests contain _NOT_BUILT_ placeholders" >&2
    exit 1
fi

ctest --test-dir "$build_dir" --output-on-failure
