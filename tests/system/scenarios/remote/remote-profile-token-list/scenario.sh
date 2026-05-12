#!/usr/bin/env bash

scenario_mode="remote_multi_container"

scenario_prepare() {
    base_prepare

    if [ "${SYSTEM_TEST_ROLE}" = "server" ]; then
        write_remote_users "${SYSTEM_TEST_ROOT}" "$(remote_users_token_admin dev secret root admin-token)"
    fi

    if [ "${SYSTEM_TEST_ROLE}" = "client" ]; then
        write_remote_profiles "${SYSTEM_TEST_ROOT}" "$(cat <<EOF
return {
  profiles = {
    dev = {
      url = 'tcp://${REMOTE_SERVER_HOST}:${REMOTE_SERVER_PORT}',
      token = 'secret',
    },
    admin = {
      url = 'tcp://${REMOTE_SERVER_HOST}:${REMOTE_SERVER_PORT}',
      token = 'admin-token',
    },
  },
}
EOF
)"
    fi
}

scenario_server_command() {
    printf 'cd %q && rqp --config %q serve --remote --bind 0.0.0.0 --port %q' \
        "${WORKSPACE_DIR}" "${CONFIG_PATH}" "${REMOTE_SERVER_PORT}"
}

scenario_client_command() {
    local body
    body="cd $(shell_quote "${WORKSPACE_DIR}") && rqp --config $(shell_quote "${CONFIG_PATH}") remote dev list moss && rqp --config $(shell_quote "${CONFIG_PATH}") remote admin shutdown >/dev/null"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote token list exits zero"
    assert_output_contains "luma" "${STDOUT_PATH}"
    assert_output_contains "1.0.0" "${STDOUT_PATH}"
}
