#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q outdated' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "outdated all exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "1.2.0" "${STDOUT_PATH}"
    assert_output_contains "2.0.0" "${STDOUT_PATH}"
}
