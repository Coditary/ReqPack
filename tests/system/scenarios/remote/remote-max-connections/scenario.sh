#!/usr/bin/env bash

scenario_mode="remote_multi_container"

scenario_prepare() {
    base_prepare

    if [ "${SYSTEM_TEST_ROLE}" = "server" ]; then
        write_remote_users "${SYSTEM_TEST_ROOT}" "$(cat <<EOF
return {
  users = {
    root = { token = 'admin-token', isAdmin = true },
  },
}
EOF
)"
    fi
}

scenario_server_command() {
    printf 'cd %q && rqp --config %q serve --remote --bind 0.0.0.0 --port %q --max-connections 1' \
        "${WORKSPACE_DIR}" "${CONFIG_PATH}" "${REMOTE_SERVER_PORT}"
}

scenario_client_command() {
    local body
    body="$(cat <<EOF
cd $(shell_quote "${WORKSPACE_DIR}") &&
socket_open admin_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_auth_token \$admin_fd admin-token &&
test \"\$SOCKET_RESPONSE_STATUS\" = \"OK\" &&
socket_open second_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_read_text_response \$second_fd &&
printf '%s\n' \"\$SOCKET_RESPONSE_BODY\" > $(shell_quote "${WORKSPACE_DIR}/max-connections.out") &&
test \"\$SOCKET_RESPONSE_STATUS\" = \"ERR\" &&
socket_send_line \$admin_fd shutdown &&
socket_read_text_response \$admin_fd &&
test \"\$SOCKET_RESPONSE_STATUS\" = \"OK\" &&
cat $(shell_quote "${WORKSPACE_DIR}/max-connections.out") &&
socket_close \$second_fd &&
socket_close \$admin_fd
EOF
)"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote max connections exits zero"
    assert_file_contains "max" "${WORKSPACE_DIR}/max-connections.out"
    assert_file_contains "connections" "${WORKSPACE_DIR}/max-connections.out"
    assert_file_contains "reached" "${WORKSPACE_DIR}/max-connections.out"
}
