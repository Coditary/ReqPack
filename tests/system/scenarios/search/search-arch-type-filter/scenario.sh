#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q search moss test --arch x86_64 --type pm' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "search arch type filter exits zero"
    assert_output_contains "test" "${STDOUT_PATH}"
    assert_output_not_contains "test-doc" "${STDOUT_PATH}"
    assert_output_not_contains "arm-test" "${STDOUT_PATH}"
}
