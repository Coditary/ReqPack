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
        write_text "${WORKSPACE_DIR}/sample.mosspkg" 'payload-content'
    fi
}

scenario_server_command() {
    printf 'cd %q && rqp --config %q serve --remote --bind 0.0.0.0 --port %q' \
        "${WORKSPACE_DIR}" "${CONFIG_PATH}" "${REMOTE_SERVER_PORT}"
}

scenario_client_command() {
    local body
    body="cd $(shell_quote "${WORKSPACE_DIR}") && rqp --config $(shell_quote "${CONFIG_PATH}") remote dev install moss $(shell_quote "${WORKSPACE_DIR}/sample.mosspkg") && rqp --config $(shell_quote "${CONFIG_PATH}") remote admin shutdown >/dev/null"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote text upload exits zero"
    assert_file_exists "${SERVER_WORKSPACE_DIR}/plugins/moss/state/local.txt"
    assert_output_contains "sample.mosspkg" "${SERVER_WORKSPACE_DIR}/plugins/moss/state/local.txt"
}
