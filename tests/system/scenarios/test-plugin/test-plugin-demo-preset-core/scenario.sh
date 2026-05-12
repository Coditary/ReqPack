#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "demo" "$(base_demo_plugin_test_plugin)"
    write_plugin_test_preset_cases "${PLUGIN_DIR}" "demo" \
        "list.lua" "$(plugin_test_case_demo_list_preset)"
    write_plugin_test_case "${WORKSPACE_DIR}/cases/search.lua" "$(plugin_test_case_search_pass)"
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --plugin demo --preset core --case ./cases/search.lua' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "test-plugin demo preset exits zero"
    assert_output_contains "[PASS] core list preset" "${STDOUT_PATH}"
    assert_output_contains "[PASS] search artifact and payload" "${STDOUT_PATH}"
    assert_output_contains "Cases: 2, Passed: 2, Failed: 0" "${STDOUT_PATH}"
}
