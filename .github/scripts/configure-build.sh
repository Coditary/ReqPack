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

if command -v go >/dev/null 2>&1; then
    cmake_args+=(
        "-DREQPACK_GO_EXECUTABLE=$(command -v go)"
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

if [ "$target_family" = "windows" ]; then
    : "${MSYSTEM_PREFIX:?MSYSTEM_PREFIX must be set for Windows builds}"
    cmake_args+=(
        -G Ninja
        -DCMAKE_MAKE_PROGRAM="${MSYSTEM_PREFIX}/bin/ninja"
        -DCMAKE_C_COMPILER="${MSYSTEM_PREFIX}/bin/cc"
        -DCMAKE_CXX_COMPILER="${MSYSTEM_PREFIX}/bin/c++"
        -DCMAKE_PREFIX_PATH="${MSYSTEM_PREFIX}"
    )
fi

cmake -Wno-dev -S . -B "$build_dir" "${cmake_args[@]}"
# Force single-job builds in CI. Bare --parallel with Unix Makefiles becomes
# gmake -j (unbounded), which can OOM GitHub-hosted runners on this project.
cmake --build "$build_dir" --parallel 1 --target ReqPack reqpack_test_targets
