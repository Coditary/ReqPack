#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove quill:zeno moss:luma --jobs-max' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remove jobs max exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/moss.log"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
