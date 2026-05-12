#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q outdated moss --arch noarch --type doc' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "outdated no results exits zero"
    assert_output_contains "No results" "${STDOUT_PATH}"
    assert_output_not_contains "luma" "${STDOUT_PATH}"
    assert_output_not_contains "fable" "${STDOUT_PATH}"
}
