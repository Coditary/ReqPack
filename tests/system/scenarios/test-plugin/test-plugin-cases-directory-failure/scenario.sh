#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "demo" "$(base_demo_plugin_test_plugin)"
    write_plugin_test_case "${WORKSPACE_DIR}/cases/a-pass.lua" "$(plugin_test_case_install_pass)"
    write_plugin_test_case "${WORKSPACE_DIR}/cases/b-fail.lua" "$(plugin_test_case_install_fail)"
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --plugin demo --cases ./cases' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected test-plugin cases directory failure to return non-zero\n' >&2
        exit 1
    fi
    assert_output_contains "[PASS] install success" "${STDOUT_PATH}"
    assert_output_contains "[FAIL] install expectation mismatch" "${STDOUT_PATH}"
    assert_output_contains "expected success=true" "${STDOUT_PATH}"
    assert_output_contains "Cases: 2, Passed: 1, Failed: 1" "${STDOUT_PATH}"
}
