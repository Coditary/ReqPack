#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_self_update_fixture "${WORKSPACE_DIR}/self-update-fixture" "v9.9.9" "x86_64-linux"
    append_self_update_config \
        "file://${WORKSPACE_DIR}/self-update-fixture" \
        "https://github.com/coditary/reqpack.git" \
        "v9.9.9" \
        "${WORKSPACE_DIR}/self-update/bin" \
        "${WORKSPACE_DIR}/self-update/current/rqp"
}

scenario_command() {
    printf 'cd %q && rqp --config %q update' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "self update exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/self-update/bin/rqp-v9.9.9-x86_64-linux/rqp"
    assert_file_exists "${WORKSPACE_COPY_DIR}/self-update/current/rqp"
    assert_output_contains "now on release v9.9.9" "${STDOUT_PATH}"
}
