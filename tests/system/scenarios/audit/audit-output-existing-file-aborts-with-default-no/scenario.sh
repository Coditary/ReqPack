#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_audit_fixture "quill-only" true
    write_text "${WORKSPACE_DIR}/audit.json" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit quill:zeno --format json --output audit.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'n\n'
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected existing audit output file prompt to abort\n' >&2
        exit 1
    fi
    assert_output_contains "already exists. Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_output_contains "aborted." "${STDOUT_PATH}"
    assert_output_contains "keep-me" "${WORKSPACE_DIR}/audit.json"
    assert_output_not_contains '"summary"' "${WORKSPACE_DIR}/audit.json"
}
