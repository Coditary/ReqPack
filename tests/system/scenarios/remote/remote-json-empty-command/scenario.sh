#!/usr/bin/env bash

scenario_mode="remote_multi_container"

scenario_prepare() {
    base_prepare

    if [ "${SYSTEM_TEST_ROLE}" = "server" ]; then
        write_remote_users "${SYSTEM_TEST_ROOT}" "$(cat <<EOF
return {
  users = {
    dev = { token = 'secret' },
    root = { token = 'admin-token', isAdmin = true },
  },
}
EOF
)"
    fi
}

scenario_server_command() {
    printf 'cd %q && rqp --config %q serve --remote --json --bind 0.0.0.0 --port %q' \
        "${WORKSPACE_DIR}" "${CONFIG_PATH}" "${REMOTE_SERVER_PORT}"
}

scenario_client_command() {
    local body
    body="$(cat <<EOF
cd $(shell_quote "${WORKSPACE_DIR}") &&
socket_open client_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_send_line \$client_fd '{"token":"secret","command":""}' &&
socket_read_json_response \$client_fd &&
printf '%s\n' \"\$SOCKET_RESPONSE_BODY\" > $(shell_quote "${WORKSPACE_DIR}/empty-json.out") &&
socket_close \$client_fd &&
socket_open admin_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_send_line \$admin_fd '{"token":"admin-token","command":"shutdown"}' &&
socket_read_json_response \$admin_fd &&
socket_close \$admin_fd &&
cat $(shell_quote "${WORKSPACE_DIR}/empty-json.out")
EOF
)"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote empty json exits zero"
    assert_file_contains '"ok":true' "${WORKSPACE_DIR}/empty-json.out"
    assert_file_contains '"output":' "${WORKSPACE_DIR}/empty-json.out"
}
