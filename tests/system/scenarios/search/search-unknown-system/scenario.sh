#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q search unknown foo' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected unknown system search to fail\n' >&2
        exit 1
    fi
    assert_output_contains "Unknown search system" "${STDOUT_PATH}"
}
