#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q install moss luma@1.0.0' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "moss package version space exits zero"
    assert_output_contains "INSTALL: moss:luma@1.0.0" "${STDOUT_PATH}"
    assert_output_contains "moss install luma@1.0.0" "${WORKSPACE_COPY_DIR}/calls/moss.log"
}
