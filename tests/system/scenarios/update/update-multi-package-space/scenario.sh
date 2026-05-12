#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q update quill zeno orbit@1.0.0' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "multi package space update exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_output_contains "UPDATE:" "${STDOUT_PATH}"
    assert_output_contains "quill:orbit@1.0.0" "${STDOUT_PATH}"
    assert_output_contains "quill:zeno" "${STDOUT_PATH}"
}
