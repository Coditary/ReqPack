#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove --stdin' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'quill:zeno\n'
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "stdin single line remove exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
