#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom all known systems exits zero"
    assert_output_contains "SYSTEM" "${STDOUT_PATH}"
    assert_output_contains "NAME" "${STDOUT_PATH}"
    assert_output_contains "VERSION" "${STDOUT_PATH}"
    assert_output_contains "moss" "${STDOUT_PATH}"
    assert_output_contains "quill" "${STDOUT_PATH}"
    assert_output_contains "anvil" "${STDOUT_PATH}"
    assert_output_contains "glyph" "${STDOUT_PATH}"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_contains "orbit" "${STDOUT_PATH}"
}
