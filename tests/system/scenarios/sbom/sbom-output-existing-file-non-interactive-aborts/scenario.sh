#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_text "${WORKSPACE_DIR}/sbom.json" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --non-interactive --output sbom.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected non-interactive sbom overwrite to abort\n' >&2
        exit 1
    fi
    assert_output_not_contains "Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_output_not_contains "aborted." "${STDOUT_PATH}"
    assert_output_contains "keep-me" "${WORKSPACE_DIR}/sbom.json"
    assert_output_not_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/sbom.json"
}
