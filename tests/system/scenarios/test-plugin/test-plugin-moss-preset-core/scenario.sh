#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_test_preset_cases "${PLUGIN_DIR}" "moss" \
        "list.lua" "$(plugin_test_case_moss_list_preset)" \
        "info.lua" "$(plugin_test_case_moss_info_preset)"
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --plugin moss --preset core' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "test-plugin moss preset exits zero"
    assert_output_contains "[PASS] core moss list" "${STDOUT_PATH}"
    assert_output_contains "[PASS] core moss info" "${STDOUT_PATH}"
    assert_output_contains "Cases: 2, Passed: 2, Failed: 0" "${STDOUT_PATH}"
}
