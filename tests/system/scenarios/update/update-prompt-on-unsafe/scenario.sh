#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_osv_overlay_for_quill "${WORKSPACE_DIR}/osv-overlay.json" critical 9.8 1.0.0
    create_test_config true false "${WORKSPACE_DIR}/osv-overlay.json" prompt critical 0.0 true
}

scenario_command() {
    printf 'cd %q && rqp --config %q update quill --prompt-on-unsafe' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_stdin() {
    printf 'y\n'
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "prompt on unsafe update exits zero"
    assert_output_contains "unsafe findings require confirmation:" "${STDOUT_PATH}"
    assert_output_contains "Continue? [y/N]" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
