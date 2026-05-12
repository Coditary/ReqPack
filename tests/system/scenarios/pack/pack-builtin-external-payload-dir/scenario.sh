#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_builtin_pack_project_without_payload_tree "${WORKSPACE_DIR}/project" "external"
    mkdir -p "${WORKSPACE_DIR}/rootfs/opt"
    write_text "${WORKSPACE_DIR}/rootfs/opt/tool.txt" "payload\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q pack ./project --payload-dir ./rootfs --output ./dist/external.rqp --force' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "builtin pack external payload exits zero"
    assert_output_contains "artifact: ${WORKSPACE_DIR}/./dist/external.rqp" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_COPY_DIR}/dist/external.rqp"
}
