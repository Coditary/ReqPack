#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    mkdir -p "${WORKSPACE_DIR}/manifests/app"
    write_manifest "${WORKSPACE_DIR}/manifests/app/reqpack.lua" "    { system = 'moss', name = 'luma' },
    { system = 'quill', name = 'zeno' },"
}

scenario_command() {
    printf 'cd %q && rqp --config %q outdated ./manifests/app' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "outdated manifest explicit dir exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "zeno" "${STDOUT_PATH}"
    assert_output_not_contains "fable" "${STDOUT_PATH}"
}
