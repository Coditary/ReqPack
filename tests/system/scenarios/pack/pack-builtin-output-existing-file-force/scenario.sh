#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_builtin_pack_project "${WORKSPACE_DIR}/project"
    write_text "${WORKSPACE_DIR}/dist/demo.rqp" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack ./project --output ./dist/demo.rqp --force' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "builtin pack force overwrite exits zero"
    assert_output_contains "artifact: ${WORKSPACE_DIR}/./dist/demo.rqp" "${STDOUT_PATH}"
    assert_output_not_contains "Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/dist/demo.rqp"
    assert_output_not_contains "keep-me" "${WORKSPACE_DIR}/dist/demo.rqp"
}
