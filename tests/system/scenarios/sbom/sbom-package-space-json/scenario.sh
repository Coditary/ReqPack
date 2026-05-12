#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --format json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom package json exits zero"
    assert_output_contains '"packages"' "${STDOUT_PATH}"
    assert_output_contains '"dependencies"' "${STDOUT_PATH}"
    assert_output_contains '"system": "quill"' "${STDOUT_PATH}"
    assert_output_contains '"name": "zeno"' "${STDOUT_PATH}"
    assert_output_contains '"version": "1.0.0"' "${STDOUT_PATH}"
}
