#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q info moss luma' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "info package exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "moss luma fixture" "${STDOUT_PATH}"
}
