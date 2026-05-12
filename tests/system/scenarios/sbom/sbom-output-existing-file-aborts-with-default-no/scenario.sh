#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    create_test_config true
    write_text "${WORKSPACE_DIR}/sbom.json" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --output sbom.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'n\n'
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected existing sbom output file prompt to abort\n' >&2
        exit 1
    fi
    assert_output_contains "already exists. Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_output_contains "aborted." "${STDOUT_PATH}"
    assert_output_contains "keep-me" "${WORKSPACE_DIR}/sbom.json"
    assert_output_not_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/sbom.json"
}
