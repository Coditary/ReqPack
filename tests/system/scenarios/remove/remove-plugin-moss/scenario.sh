#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "moss" "$(base_moss_plugin)" "glyph:fable"
}

scenario_command() {
    printf 'cd %q && rqp --config %q remove moss' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "plugin remove exits zero"
    assert_file_missing "${WORKSPACE_COPY_DIR}/plugins/moss"
    assert_output_contains "glyph remove fable" "${WORKSPACE_COPY_DIR}/calls/glyph.log"
}
