#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    printf 'usage: %s <repo-root> <scenario-path> <output-dir>\n' "$0" >&2
    exit 1
fi

repo_root="$1"
scenario_path="$2"
output_dir="$3"
provided_system_test_root="${SYSTEM_TEST_ROOT:-}"

export SYSTEM_TEST_SCENARIO="${scenario_path}"
export SYSTEM_TEST_NAME="$(basename "$(dirname "${scenario_path}")")"
export SYSTEM_TEST_ROOT="${SYSTEM_TEST_ROOT:-$(mktemp -d)}"
export SYSTEM_TEST_OUTPUT_DIR="${output_dir}"
system_test_root_was_provided="false"
if [ -n "${provided_system_test_root}" ]; then
    system_test_root_was_provided="true"
fi
cleanup() {
    mkdir -p "${SYSTEM_TEST_OUTPUT_DIR}/workspace-copy"
    cp -a "${SYSTEM_TEST_ROOT}/." "${SYSTEM_TEST_OUTPUT_DIR}/workspace-copy/" 2>/dev/null || true
    if [ "${system_test_root_was_provided}" != "true" ]; then
        rm -rf "${SYSTEM_TEST_ROOT}"
    fi
}
trap cleanup EXIT INT TERM HUP

mkdir -p "${SYSTEM_TEST_OUTPUT_DIR}"
mkdir -p "${SYSTEM_TEST_ROOT}"

# shellcheck source=/dev/null
source "${repo_root}/tests/system/local/lib/system_test_lib.sh"
# shellcheck source=/dev/null
source "${scenario_path}"

scenario_prepare
run_scenario_command
if [ "${SYSTEM_TEST_ROLE:-single}" = "server" ] && declare -f scenario_server_assert >/dev/null 2>&1; then
    scenario_server_assert
elif [ "${SYSTEM_TEST_ROLE:-single}" = "server" ] && declare -f scenario_server_command >/dev/null 2>&1; then
    :
elif [ "${SYSTEM_TEST_ROLE:-single}" = "client" ] && declare -f scenario_client_assert >/dev/null 2>&1; then
    scenario_client_assert
else
    scenario_assert
fi
