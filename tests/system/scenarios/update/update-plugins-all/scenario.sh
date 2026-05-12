#!/usr/bin/env bash

scenario_prepare() {
    write_plugin_bundle "${REGISTRY_SOURCE_DIR}" "moss" "$(base_moss_plugin)"
    write_plugin_bundle "${REGISTRY_SOURCE_DIR}" "quill" "$(base_quill_plugin)"
    write_base_fake_binaries
    create_test_config false true
    rm -rf "${PLUGIN_DIR}/moss" "${PLUGIN_DIR}/quill"
}

scenario_command() {
    printf 'cd %q && rqp --config %q update --all' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "plugin refresh all exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/moss/run.lua"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/quill/run.lua"
    assert_output_contains "moss              OK" "${STDOUT_PATH}"
    assert_output_contains "quill             OK" "${STDOUT_PATH}"
}
