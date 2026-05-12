#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "moss-and-quill"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit moss:luma quill:zeno' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected multi-system audit to report findings\n' >&2
        exit 1
    fi
    assert_output_contains "moss" "${STDOUT_PATH}"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "CVE-TEST-MOSS-001" "${STDOUT_PATH}"
    assert_output_contains "quill" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "CVE-TEST-QUILL-001" "${STDOUT_PATH}"
    assert_output_not_contains "orbit" "${STDOUT_PATH}"
    assert_output_not_contains "fable" "${STDOUT_PATH}"
}
