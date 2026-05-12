#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_text "${WORKSPACE_DIR}/file.mosspkg" 'fixture-moss-local\n'
}

scenario_command() {
    printf 'cd %q && rqp --config %q install ./file.mosspkg' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "mosspkg install exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/plugins/moss/state/local.txt"
    assert_output_contains "INSTALL: moss:local" "${STDOUT_PATH}"
}
