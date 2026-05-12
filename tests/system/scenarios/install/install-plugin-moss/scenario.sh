#!/usr/bin/env bash

scenario_prepare() {
    write_plugin_bundle "${REGISTRY_SOURCE_DIR}" "moss" "$(base_moss_plugin)"
    write_base_fake_binaries
    create_test_config false true
    rm -rf "${PLUGIN_DIR}/moss"
}

scenario_command() {
    printf 'cd %q && rqp --config %q install moss' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "plugin bootstrap exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/moss/run.lua"
}
