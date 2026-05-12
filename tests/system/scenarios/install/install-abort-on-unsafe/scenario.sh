#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_osv_overlay_for_quill "${WORKSPACE_DIR}/osv-overlay.json" critical 9.8 1.0.0
    create_test_config false false "${WORKSPACE_DIR}/osv-overlay.json" abort critical 0.0 false
}

scenario_command() {
    printf 'cd %q && rqp --config %q install quill:zeno@1.0.0 --abort-on-unsafe' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected abort-on-unsafe to fail\n' >&2
        exit 1
    fi
    assert_output_contains "execution blocked by security policy" "${STDOUT_PATH}"
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/quill.log"
}
