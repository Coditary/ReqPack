#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill zeno' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected audit quill zeno to report findings\n' >&2
        exit 1
    fi
    assert_output_contains "quill" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "CVE-TEST-QUILL-001" "${STDOUT_PATH}"
    assert_output_not_contains "orbit" "${STDOUT_PATH}"
    assert_output_not_contains "version unavailable" "${STDOUT_PATH}"
}
