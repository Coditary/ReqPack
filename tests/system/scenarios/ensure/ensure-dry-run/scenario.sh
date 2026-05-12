#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_ensure_fixture
}

scenario_command() {
    printf 'cd %q && rqp --config %q ensure moss --dry-run' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "ensure dry run exits zero"
    assert_output_contains "ENSURE: quill:zeno" "${STDOUT_PATH}"
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_file_missing "${WORKSPACE_COPY_DIR}/plugins/quill/state/installed.txt"
}
