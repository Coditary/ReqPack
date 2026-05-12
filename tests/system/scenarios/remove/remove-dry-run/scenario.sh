#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove quill:zeno --dry-run' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remove dry-run exits zero"
    assert_output_contains "REMOVE: quill:zeno" "${STDOUT_PATH}"
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
