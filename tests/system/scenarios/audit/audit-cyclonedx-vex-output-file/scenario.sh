#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --format cyclonedx-vex-json --output audit.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit cyclonedx vex export exits zero"
    assert_output_contains "${WORKSPACE_DIR}/audit.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"vulnerabilities"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"id": "CVE-TEST-QUILL-001"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"analysis": {"state": "in_triage"' "${WORKSPACE_DIR}/audit.json"
}
