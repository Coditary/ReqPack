#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_builtin_pack_project "${WORKSPACE_DIR}/project"
    write_text "${WORKSPACE_DIR}/dist/demo.rqp" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack ./project --output ./dist/demo.rqp --non-interactive' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected non-interactive pack overwrite to abort\n' >&2
        exit 1
    fi
    assert_output_not_contains "Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_output_contains "output file already exists: ${WORKSPACE_DIR}/./dist/demo.rqp" "${STDOUT_PATH}"
    assert_output_contains "Use --force to overwrite." "${STDOUT_PATH}"
    assert_output_contains "keep-me" "${WORKSPACE_DIR}/dist/demo.rqp"
}
