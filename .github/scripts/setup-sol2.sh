#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <include-prefix>" >&2
    exit 1
fi

include_prefix="$1"
sol_dir="${include_prefix%/}/sol"
sol_version="${SOL2_VERSION:-v3.3.0}"
base_url="https://github.com/ThePhD/sol2/releases/download/${sol_version}"
cache_root="${SOL2_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/reqpack/sol2/${sol_version}}"
cache_sol_dir="${cache_root%/}/sol"

install -d "$sol_dir" "$cache_sol_dir"

for header in sol.hpp config.hpp forward.hpp; do
    if [ ! -f "$cache_sol_dir/$header" ]; then
        tmp_file="$cache_sol_dir/$header.tmp"
        curl -fsSL "${base_url}/$header" -o "$tmp_file"
        mv "$tmp_file" "$cache_sol_dir/$header"
    fi
    install -m 0644 "$cache_sol_dir/$header" "$sol_dir/$header"
done
