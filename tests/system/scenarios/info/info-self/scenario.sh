#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q info' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "info self exits zero"
    assert_output_contains "ReqPack" "${STDOUT_PATH}"
    assert_output_contains "Version" "${STDOUT_PATH}"
}
