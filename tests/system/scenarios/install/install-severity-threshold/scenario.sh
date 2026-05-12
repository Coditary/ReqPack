#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_osv_overlay_for_quill "${WORKSPACE_DIR}/osv-overlay.json" medium 6.5 1.0.0
    create_test_config false false "${WORKSPACE_DIR}/osv-overlay.json" abort critical 0.0 false
}

scenario_command() {
    printf 'cd %q && rqp --config %q install quill:zeno@1.0.0 --severity-threshold high' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "severity-threshold exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
