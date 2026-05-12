#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only" true
    write_text "${WORKSPACE_DIR}/audit.json" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --format json --output audit.json --force' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit output force overwrite exits zero"
    assert_output_contains "${WORKSPACE_DIR}/audit.json" "${STDOUT_PATH}"
    assert_output_not_contains "Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"summary"' "${WORKSPACE_DIR}/audit.json"
    assert_output_contains '"id": "CVE-TEST-QUILL-001"' "${WORKSPACE_DIR}/audit.json"
    assert_output_not_contains "keep-me" "${WORKSPACE_DIR}/audit.json"
}
