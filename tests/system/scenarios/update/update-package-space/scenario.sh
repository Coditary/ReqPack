#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q update moss luma' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "package space update exits zero"
    assert_output_contains "UPDATE: moss:luma" "${STDOUT_PATH}"
    assert_output_contains "moss update luma" "${WORKSPACE_COPY_DIR}/calls/moss.log"
}
