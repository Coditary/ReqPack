#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_text "${WORKSPACE_DIR}/file.anvilpkg" 'fixture-anvil-zst\n'
    zstd -q -f "${WORKSPACE_DIR}/file.anvilpkg" -o "${WORKSPACE_DIR}/file.anvilpkg.zst"
}

scenario_command() {
    printf 'cd %q && rqp --config %q install ./file.anvilpkg.zst' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "anvilpkg.zst install exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/anvil/state/local.txt"
    assert_output_contains "INSTALL: anvil:local" "${STDOUT_PATH}"
}
