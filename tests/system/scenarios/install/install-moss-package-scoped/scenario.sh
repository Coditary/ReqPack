#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q install moss:luma' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "moss package scoped exits zero"
    assert_output_contains "INSTALL: moss:luma" "${STDOUT_PATH}"
    assert_output_contains "moss install luma" "${WORKSPACE_COPY_DIR}/calls/moss.log"
}
