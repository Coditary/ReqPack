#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${WORKSPACE_DIR}/project"
    write_manifest "${WORKSPACE_DIR}/project/reqpack.lua" "    { system = 'moss', name = 'luma' },
    { system = 'quill', name = 'zeno' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q outdated .' "${WORKSPACE_DIR}/project" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "outdated manifest current dir exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_not_contains "fable" "${STDOUT_PATH}"
}
