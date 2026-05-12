#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q serve --stdin' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'install moss luma\ninstall "broken\nlist moss\n'
}

scenario_assert() {
    assert_equals "1" "$(read_status)" "serve stdin exits nonzero after parse error"
    assert_file_contains 'luma' "${PLUGIN_DIR}/moss/state/installed.txt"
    assert_output_contains 'stdin line 2: invalid command syntax' "${STDOUT_PATH}"
    assert_output_contains 'LIST:' "${STDOUT_PATH}"
    assert_output_contains '1.0.0' "${STDOUT_PATH}"
}
