#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q info moss' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "info plugin exits zero"
    assert_output_contains "moss" "${STDOUT_PATH}"
    assert_output_contains "moss plugin fixture" "${STDOUT_PATH}"
}
