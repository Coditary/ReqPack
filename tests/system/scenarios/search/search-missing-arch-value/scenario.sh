#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q search moss test --arch' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected missing arch value to fail\n' >&2
        exit 1
    fi
}
