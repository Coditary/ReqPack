#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "empty"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --format json --output audit.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit no findings export exits zero"
    assert_output_contains "${WORKSPACE_DIR}/audit.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"findingCount": 0' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"packageCount": 1' "${WORKSPACE_DIR}/audit.json"
    assert_output_not_contains 'CVE-TEST-' "${WORKSPACE_DIR}/audit.json"
}
