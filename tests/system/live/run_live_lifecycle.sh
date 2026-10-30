#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <system> <package> [verify-command]" >&2
    echo "env: RQP_BIN, LIVE_CONFIG, LIVE_REPORT_DIR" >&2
    exit 1
fi

system="$1"
package="$2"
verify_cmd="${3:-command -v ${package}}"

RQP_BIN="${RQP_BIN:-/opt/reqpack/rqp}"
LIVE_CONFIG="${LIVE_CONFIG:-/workspace/config.lua}"
LIVE_REPORT_DIR="${LIVE_REPORT_DIR:-/workspace/reports}"

mkdir -p "${LIVE_REPORT_DIR}"

run_step() {
    local name="$1"
    shift
    local out="${LIVE_REPORT_DIR}/${name}.stdout.json"
    local err="${LIVE_REPORT_DIR}/${name}.stderr.txt"
    printf '[live-lifecycle] %s\n' "${name}" >&2
    set +e
    "$@" >"${out}" 2>"${err}"
    local status=$?
    set -e
    printf '%s\n' "${status}" >"${LIVE_REPORT_DIR}/${name}.exit"
    if [ "${status}" -ne 0 ]; then
        printf 'step failed: %s (exit %s)\n' "${name}" "${status}" >&2
        cat "${err}" >&2
        exit "${status}"
    fi
}

assert_installed() {
    if ! bash -lc "${verify_cmd}" >/dev/null 2>&1; then
        printf 'verify failed after install: %s\n' "${verify_cmd}" >&2
        exit 1
    fi
}

assert_not_installed() {
    if bash -lc "${verify_cmd}" >/dev/null 2>&1; then
        printf 'verify failed after remove (still present): %s\n' "${verify_cmd}" >&2
        exit 1
    fi
}

run_step ensure "${RQP_BIN}" --config "${LIVE_CONFIG}" -y plugin ensure "${system}"
run_step search "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json search "${system}" "${package}"
run_step install "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json install "${system}" "${package}"
assert_installed
run_step list "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json list "${system}" "${package}"
run_step info "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json info "${system}" "${package}"
set +e
run_step outdated "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json outdated "${system}"
set -e
run_step remove "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json remove "${system}" "${package}"
assert_not_installed
run_step list_after_remove "${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json list "${system}" "${package}"

printf '[live-lifecycle] ok system=%s package=%s\n' "${system}" "${package}"
