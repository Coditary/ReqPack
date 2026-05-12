#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "demo" "$(base_demo_plugin_test_plugin)"
    write_plugin_test_case "${WORKSPACE_DIR}/cases/install_pass.lua" "$(plugin_test_case_install_pass)"
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --plugin demo --case ./cases/install_pass.lua --report ./report.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "test-plugin case report exits zero"
    assert_output_contains "[PASS] install success" "${STDOUT_PATH}"
    assert_output_contains "Cases: 1, Passed: 1, Failed: 0" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/report.json"
    assert_output_contains '"plugin": "demo"' "${WORKSPACE_DIR}/report.json"
    assert_output_contains '"success": true' "${WORKSPACE_DIR}/report.json"
    assert_output_contains '"stdout": ["ok\n"]' "${WORKSPACE_DIR}/report.json"
}
