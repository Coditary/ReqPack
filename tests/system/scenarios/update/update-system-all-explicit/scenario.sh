#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q update quill --all' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "system all explicit exits zero"
    assert_output_contains "UPDATE: quill" "${STDOUT_PATH}"
    assert_output_contains "update all packages" "${STDOUT_PATH}"
    assert_output_contains "quill update-all" "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
