#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    create_test_config true
    write_builtin_pack_project "${WORKSPACE_DIR}/project"
    write_text "${WORKSPACE_DIR}/dist/demo.rqp" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack ./project --output ./dist/demo.rqp' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'n\n'
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected existing pack output file prompt to abort\n' >&2
        exit 1
    fi
    assert_output_contains "already exists. Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_output_contains "pack aborted" "${STDOUT_PATH}"
    assert_output_contains "keep-me" "${WORKSPACE_DIR}/dist/demo.rqp"
}
