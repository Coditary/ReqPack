#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <build-dir> <target-family> [cmake-arg ...]" >&2
    exit 1
fi

build_dir="$1"
target_family="$2"
shift 2

cmake_args=(
    -DCMAKE_BUILD_TYPE=Release
    "$@"
)

if [ -n "${RELEASE_ID:-}" ]; then
    cmake_args+=(
        "-DREQPACK_RELEASE_ID=${RELEASE_ID}"
    )
fi

if [ "$target_family" = "linux" ]; then
    cmake_args+=(
        -DBUILD_SHARED_LIBS=OFF
        -DLink_Static=ON
        -DREQPACK_LINK_STATIC_LUA=ON
        -DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++\ -static-libgcc
    )
fi

if [ "$target_family" = "macos" ]; then
    cmake_args+=(
        -DREQPACK_LINK_STATIC_LUA=ON
    )
fi

cmake -Wno-dev -S . -B "$build_dir" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel --target ReqPack reqpack_test_targets
