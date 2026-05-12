#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "moss-and-quill"
    mkdir -p "${WORKSPACE_DIR}/project"
    write_manifest "${WORKSPACE_DIR}/project/reqpack.lua" "    { system = 'moss', name = 'luma' },
    { system = 'quill', name = 'zeno' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit --format json --output explicit-audit.json ./project/reqpack.lua' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit manifest explicit file export exits zero"
    assert_output_contains "${WORKSPACE_DIR}/explicit-audit.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"packages"' "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"findings"' "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"name": "luma"' "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"name": "zeno"' "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"id": "CVE-TEST-MOSS-001"' "${WORKSPACE_DIR}/explicit-audit.json"
    assert_output_contains '"id": "CVE-TEST-QUILL-001"' "${WORKSPACE_DIR}/explicit-audit.json"
}
