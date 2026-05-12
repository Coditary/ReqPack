#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_ensure_fixture
    mkdir -p "${WORKSPACE_DIR}/plugins/quill/state"
    write_text "${WORKSPACE_DIR}/plugins/quill/state/installed.txt" $'zeno\n'
}

scenario_command() {
    printf 'cd %q && rqp --config %q ensure moss' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "ensure already satisfied exits zero"
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
