#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --help' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "test-plugin help exits zero"
    assert_output_contains "Run hermetic plugin conformance cases" "${STDOUT_PATH}"
    assert_output_contains "--plugin <value>" "${STDOUT_PATH}"
    assert_output_contains "Known presets: core" "${STDOUT_PATH}"
}
