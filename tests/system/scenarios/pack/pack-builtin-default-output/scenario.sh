#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_builtin_pack_project "${WORKSPACE_DIR}/project"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack ./project' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "builtin pack default output exits zero"
    assert_output_contains "PACK:" "${STDOUT_PATH}"
    assert_output_contains "rqp" "${STDOUT_PATH}"
    assert_output_contains "demo" "${STDOUT_PATH}"
    assert_output_contains "1.2.3-2+r1" "${STDOUT_PATH}"
    assert_output_contains "artifact: ${WORKSPACE_DIR}/./demo.rqp" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_COPY_DIR}/demo.rqp"
}
