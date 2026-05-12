#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${WORKSPACE_DIR}/dir"
    write_manifest "${WORKSPACE_DIR}/dir/reqpack.lua" "    { system = 'moss', name = 'luma' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove ./dir' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "manifest explicit dir remove exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/moss.log"
    assert_output_contains "REMOVE: moss:luma" "${STDOUT_PATH}"
}
