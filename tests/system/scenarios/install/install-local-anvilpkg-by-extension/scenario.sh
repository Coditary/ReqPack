#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_text "${WORKSPACE_DIR}/file.anvilpkg" 'fixture-anvil-local\n'
}

scenario_command() {
    printf 'cd %q && rqp --config %q install ./file.anvilpkg' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "anvilpkg install exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/anvil/state/local.txt"
    assert_output_contains "INSTALL: anvil:local" "${STDOUT_PATH}"
}
