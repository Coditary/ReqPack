#!/usr/bin/env bash

scenario_mode="remote_multi_container"

scenario_prepare() {
    base_prepare

    if [ "${SYSTEM_TEST_ROLE}" = "server" ]; then
        write_remote_users "${SYSTEM_TEST_ROOT}" "$(cat <<EOF
return {
  users = {
    alice = { token = 'user-token' },
    root = { token = 'admin-token', isAdmin = true },
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
    body="$(cat <<EOF
cd $(shell_quote "${WORKSPACE_DIR}") &&
socket_open user_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_auth_token \$user_fd user-token &&
test "\$SOCKET_RESPONSE_STATUS" = "OK" &&
socket_send_line \$user_fd connections count &&
socket_read_text_response \$user_fd &&
printf '%s\n' "\$SOCKET_RESPONSE_BODY" > $(shell_quote "${WORKSPACE_DIR}/user-count.out") &&
test "\$SOCKET_RESPONSE_STATUS" = "ERR" &&
socket_open admin_fd ${REMOTE_SERVER_HOST} ${REMOTE_SERVER_PORT} &&
socket_auth_token \$admin_fd admin-token &&
test "\$SOCKET_RESPONSE_STATUS" = "OK" &&
socket_send_line \$admin_fd connections count &&
socket_read_text_response \$admin_fd &&
printf '%s\n' "\$SOCKET_RESPONSE_BODY" > $(shell_quote "${WORKSPACE_DIR}/admin-count.out") &&
test "\$SOCKET_RESPONSE_STATUS" = "OK" &&
socket_send_line \$admin_fd connections list &&
socket_read_text_response \$admin_fd &&
printf '%s\n' "\$SOCKET_RESPONSE_BODY" > $(shell_quote "${WORKSPACE_DIR}/admin-list.out") &&
test "\$SOCKET_RESPONSE_STATUS" = "OK" &&
socket_send_line \$user_fd exit &&
socket_read_text_response \$user_fd &&
socket_send_line \$admin_fd shutdown &&
socket_read_text_response \$admin_fd &&
test "\$SOCKET_RESPONSE_STATUS" = "OK" &&
cat $(shell_quote "${WORKSPACE_DIR}/user-count.out") $(shell_quote "${WORKSPACE_DIR}/admin-count.out") $(shell_quote "${WORKSPACE_DIR}/admin-list.out") &&
socket_close \$user_fd &&
socket_close \$admin_fd
EOF
)"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote admin connections exits zero"
    assert_file_contains "admin privileges" "${WORKSPACE_DIR}/user-count.out"
    assert_file_contains "Active Connections" "${WORKSPACE_DIR}/admin-count.out"
    assert_file_contains "2" "${WORKSPACE_DIR}/admin-count.out"
    assert_file_contains "alice" "${WORKSPACE_DIR}/admin-list.out"
    assert_file_contains "root" "${WORKSPACE_DIR}/admin-list.out"
    assert_file_contains "true" "${WORKSPACE_DIR}/admin-list.out"
}
