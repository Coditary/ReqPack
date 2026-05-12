#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${XDG_CACHE_HOME}/reqpack/host"
    write_text "${XDG_CACHE_HOME}/reqpack/host/info.v1.json" "{ invalid json }\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q host refresh' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    local cache_path="${WORKSPACE_COPY_DIR}/xdg-cache/reqpack/host/info.v1.json"
    assert_equals "0" "$(read_status)" "host refresh exits zero"
    assert_output_contains "host refresh: cache updated" "${STDOUT_PATH}"
    assert_output_contains "host os: linux" "${STDOUT_PATH}"
    assert_output_contains "host arch: x86_64" "${STDOUT_PATH}"
    assert_output_contains "host cache: ${WORKSPACE_DIR}/xdg-cache/reqpack/host/info.v1.json" "${STDOUT_PATH}"
    assert_file_exists "${cache_path}"
    assert_output_contains '"refreshReason":"manual-live-probe"' "${cache_path}"
    assert_output_contains '"osFamily":"linux"' "${cache_path}"
    assert_output_contains '"arch":"x86_64"' "${cache_path}"
    assert_output_not_contains "{ invalid json }" "${cache_path}"
}
