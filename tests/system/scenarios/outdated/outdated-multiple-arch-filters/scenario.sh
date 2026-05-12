#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q outdated moss --arch x86_64 --arch aarch64' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "outdated multiple arch filters exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "fable" "${STDOUT_PATH}"
}
