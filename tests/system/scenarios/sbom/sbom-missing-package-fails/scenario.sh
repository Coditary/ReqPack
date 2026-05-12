#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill missing --format json --output sbom.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected sbom missing package to fail\n' >&2
        exit 1
    fi
    assert_output_contains "sbom missing package: quill:missing" "${STDOUT_PATH}"
    assert_file_missing "${WORKSPACE_DIR}/sbom.json"
}
