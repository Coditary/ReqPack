#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <build-dir> <output-archive>" >&2
    exit 1
fi

build_dir="$1"
output_archive="$2"

[ -d "$build_dir" ] || {
    echo "build directory not found: $build_dir" >&2
    exit 1
}

mkdir -p "$(dirname "$output_archive")"
tar -C "$build_dir" -czf "$output_archive" .
