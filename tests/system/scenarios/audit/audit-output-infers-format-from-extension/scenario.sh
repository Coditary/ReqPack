#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --output audit.sarif' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit output infers sarif extension exits zero"
    assert_output_contains "${WORKSPACE_DIR}/audit.sarif" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/audit.sarif"
    assert_output_contains '"version": "2.1.0"' "${WORKSPACE_DIR}/audit.sarif"
    assert_output_contains '"runs"' "${WORKSPACE_DIR}/audit.sarif"
    assert_output_contains '"ruleId": "CVE-TEST-QUILL-001"' "${WORKSPACE_DIR}/audit.sarif"
}
