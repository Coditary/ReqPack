#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --format json --output audit.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit json export exits zero"
    assert_output_contains "${WORKSPACE_DIR}/audit.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"packages"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"findings"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"id": "CVE-TEST-QUILL-001"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"displayName": "zeno"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"ref": "quill:zeno@1.0.0"' "${WORKSPACE_DIR}/audit.json"
}
