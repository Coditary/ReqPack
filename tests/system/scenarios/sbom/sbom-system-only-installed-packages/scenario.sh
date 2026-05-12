#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom system-only request exits zero"
    assert_output_contains "quill" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "orbit" "${STDOUT_PATH}"
    assert_output_not_contains "moss" "${STDOUT_PATH}"
    assert_output_not_contains "luma" "${STDOUT_PATH}"
}
