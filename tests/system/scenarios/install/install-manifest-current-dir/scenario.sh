#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${WORKSPACE_DIR}/project"
    write_manifest "${WORKSPACE_DIR}/project/reqpack.lua" "    { system = 'moss', name = 'luma' },
    { system = 'quill', name = 'zeno', version = '1.0.0' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q install .' "${WORKSPACE_DIR}/project" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "manifest current dir exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/moss.log"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_output_contains "INSTALL: moss:luma" "${STDOUT_PATH}"
    assert_output_contains "quill:zeno@1.0.0" "${STDOUT_PATH}"
}
