#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_test_preset_cases "${PLUGIN_DIR}" "quill" \
        "list.lua" "$(plugin_test_case_quill_list_preset)" \
        "search.lua" "$(plugin_test_case_quill_search_preset)"
}

scenario_command() {
    printf 'cd %q && rqp --config %q test-plugin --plugin quill --preset core' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "test-plugin quill preset exits zero"
    assert_output_contains "[PASS] core quill list" "${STDOUT_PATH}"
    assert_output_contains "[PASS] core quill search" "${STDOUT_PATH}"
    assert_output_contains "Cases: 2, Passed: 2, Failed: 0" "${STDOUT_PATH}"
}
