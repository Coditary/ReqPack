#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove moss:luma quill:zeno' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "multi system scoped remove exits zero"
    assert_output_contains "REMOVE: moss:luma" "${STDOUT_PATH}"
    assert_output_contains "quill:zeno" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/moss.log"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
