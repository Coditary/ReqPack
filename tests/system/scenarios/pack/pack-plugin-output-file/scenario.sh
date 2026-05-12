#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "demo" "$(base_demo_pack_plugin)"
    mkdir -p "${WORKSPACE_DIR}/native-project"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack demo ./native-project --output ./dist/demo.pkg --force' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "plugin pack output exits zero"
    assert_output_contains "PACK:" "${STDOUT_PATH}"
    assert_output_contains "demo" "${STDOUT_PATH}"
    assert_output_contains "artifact: ./dist/demo.pkg" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/dist/demo.pkg"
    assert_output_contains "native-artifact" "${WORKSPACE_DIR}/dist/demo.pkg"
}
