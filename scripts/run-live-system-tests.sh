#!/usr/bin/env bash
# Thin wrapper: live matrix orchestration lives in `rqp live run` (C++).
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"

rqp_bin="${REQPACK_RQP_BIN:-}"
if [ -z "${rqp_bin}" ]; then
    if [ -x "${repo_root}/build/rqp" ]; then
        rqp_bin="${repo_root}/build/rqp"
    elif [ -x "${repo_root}/build/ReqPack/rqp" ]; then
        rqp_bin="${repo_root}/build/ReqPack/rqp"
    else
        rqp_bin="$(command -v rqp)"
    fi
fi

export REQPACK_REPO_ROOT="${REQPACK_REPO_ROOT:-${repo_root}}"
exec "${rqp_bin}" live run --repo-root "${REQPACK_REPO_ROOT}" "$@"
