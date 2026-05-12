#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${WORKSPACE_DIR}/project"
    write_manifest "${WORKSPACE_DIR}/project/reqpack.lua" "    { system = 'moss', name = 'luma' },
    { system = 'quill', name = 'orbit', version = '1.0.0' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q update ./project/reqpack.lua' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "manifest file update exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/moss.log"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_output_contains "UPDATE:" "${STDOUT_PATH}"
    assert_output_contains "moss:luma" "${STDOUT_PATH}"
    assert_output_contains "quill:orbit@1.0.0" "${STDOUT_PATH}"
}
