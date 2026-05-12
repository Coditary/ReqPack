#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only"
    write_text "${PLUGIN_DIR}/quill/state/installed.txt" "zeno@1.0.0
"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected audit quill to report findings\n' >&2
        exit 1
    fi
    assert_output_contains "SYSTEM" "${STDOUT_PATH}"
    assert_output_contains "NAME" "${STDOUT_PATH}"
    assert_output_contains "FINDING" "${STDOUT_PATH}"
    assert_output_contains "SEVERITY" "${STDOUT_PATH}"
    assert_output_contains "quill" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "CVE-TEST-QUILL-001" "${STDOUT_PATH}"
    assert_output_not_contains "luma" "${STDOUT_PATH}"
    assert_output_not_contains "version unavailable" "${STDOUT_PATH}"
}
