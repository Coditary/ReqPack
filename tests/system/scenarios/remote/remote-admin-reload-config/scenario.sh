#!/usr/bin/env bash

scenario_mode="remote_multi_container"

shared_updated_users_path() {
    shared_write_path updated-remote.lua
}

shared_updated_config_path() {
    shared_write_path updated-config.lua
}

shared_server_config_path() {
    shared_write_path server-config.lua
}

shared_server_xdg_config_home() {
    shared_write_path xdg-config
}

shared_server_users_path() {
    printf '%s/reqpack/remote.lua' "$(shared_server_xdg_config_home)"
}

write_shared_server_config() {
    local readonly_value="$1"
    write_text "$(shared_server_config_path)" "return {
  security = {
    autoFetch = false,
    enabled = true,
    osvRefreshMode = \"manual\",
    osvDatabasePath = \"${SERVER_WORKSPACE_DIR}/osv-db\",
    onUnsafe = \"continue\",
    promptOnUnsafe = false,
    severityThreshold = \"critical\",
    scoreThreshold = 0.0,
  },
  execution = {
    useTransactionDb = false,
    deleteCommittedTransactions = false,
    checkVirtualFileSystemWrite = false,
    transactionDatabasePath = \"${SERVER_WORKSPACE_DIR}/transactions\",
  },
  planner = {
    autoDownloadMissingPlugins = false,
    autoDownloadMissingDependencies = false,
  },
  registry = {
    pluginDirectory = \"${SERVER_WORKSPACE_DIR}/plugins\",
    databasePath = \"${SERVER_WORKSPACE_DIR}/registry-db\",
    remoteUrl = \"\",
    autoLoadPlugins = true,
    shutDownPluginsOnExit = true,
  },
  interaction = {
    interactive = false,
  },
  rqp = {
    statePath = \"${SERVER_WORKSPACE_DIR}/rqp-state\",
  },
  remote = {
    readonly = ${readonly_value},
  },
}
"
}

write_shared_server_users_initial() {
    write_text "$(shared_server_users_path)" "return {
  users = {
    root = { token = 'admin-token', isAdmin = true },
  },
}
"
}

write_shared_server_users_updated() {
    write_text "$(shared_updated_users_path)" "return {
  users = {
    root = { token = 'new-admin-token', isAdmin = true },
  },
}
"
}

write_updated_server_config() {
    write_text "$(shared_updated_config_path)" "return {
  security = {
    autoFetch = false,
    enabled = true,
    osvRefreshMode = \"manual\",
    osvDatabasePath = \"${SERVER_WORKSPACE_DIR}/osv-db\",
    onUnsafe = \"continue\",
    promptOnUnsafe = false,
    severityThreshold = \"critical\",
    scoreThreshold = 0.0,
  },
  execution = {
    useTransactionDb = false,
    deleteCommittedTransactions = false,
    checkVirtualFileSystemWrite = false,
    transactionDatabasePath = \"${SERVER_WORKSPACE_DIR}/transactions\",
  },
  planner = {
    autoDownloadMissingPlugins = false,
    autoDownloadMissingDependencies = false,
  },
  registry = {
    pluginDirectory = \"${SERVER_WORKSPACE_DIR}/plugins\",
    databasePath = \"${SERVER_WORKSPACE_DIR}/registry-db\",
    remoteUrl = \"\",
    autoLoadPlugins = true,
    shutDownPluginsOnExit = true,
  },
  interaction = {
    interactive = false,
  },
  rqp = {
    statePath = \"${SERVER_WORKSPACE_DIR}/rqp-state\",
  },
  remote = {
    readonly = true,
  },
}
"
}

scenario_prepare() {
    base_prepare

    if [ "${SYSTEM_TEST_ROLE}" = "server" ]; then
        write_shared_server_config false
        write_shared_server_users_initial
    fi

    if [ "${SYSTEM_TEST_ROLE}" = "client" ]; then
        write_remote_profiles "${SYSTEM_TEST_ROOT}" "$(cat <<EOF
return {
  profiles = {
    admin = {
      url = 'tcp://${REMOTE_SERVER_HOST}:${REMOTE_SERVER_PORT}',
      token = 'admin-token',
    },
    newadmin = {
      url = 'tcp://${REMOTE_SERVER_HOST}:${REMOTE_SERVER_PORT}',
      token = 'new-admin-token',
    },
  },
}
EOF
)"

        write_shared_server_users_updated
        write_updated_server_config
    fi
}

scenario_server_command() {
    printf 'cd %q && env XDG_CONFIG_HOME=%q rqp --config %q serve --remote --bind 0.0.0.0 --port %q' \
        "${WORKSPACE_DIR}" "$(shared_server_xdg_config_home)" "$(shared_server_config_path)" "${REMOTE_SERVER_PORT}"
}

scenario_client_command() {
    local body
    body="cd $(shell_quote "${WORKSPACE_DIR}") && cp $(shell_quote "$(shared_updated_users_path)") $(shell_quote "$(shared_server_users_path)") && cp $(shell_quote "$(shared_updated_config_path)") $(shell_quote "$(shared_server_config_path)") && set +e && rqp --config $(shell_quote "${CONFIG_PATH}") remote admin reload-config > $(shell_quote "${WORKSPACE_DIR}/reload.out") 2>&1; reload_status=\$?; rqp --config $(shell_quote "${CONFIG_PATH}") remote admin list moss >> $(shell_quote "${WORKSPACE_DIR}/reload.out") 2>&1; stale_status=\$?; rqp --config $(shell_quote "${CONFIG_PATH}") remote newadmin install moss luma >> $(shell_quote "${WORKSPACE_DIR}/reload.out") 2>&1; readonly_status=\$?; rqp --config $(shell_quote "${CONFIG_PATH}") remote newadmin shutdown >/dev/null; set -e; cat $(shell_quote "${WORKSPACE_DIR}/reload.out"); test \$reload_status -eq 0 && test \$stale_status -ne 0 && test \$readonly_status -ne 0"
    command_with_wait_prefix "${body}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "remote reload-config scenario exits zero"
    assert_output_contains "authentication failed" "${STDOUT_PATH}"
    assert_output_contains "readonly" "${STDOUT_PATH}"
    assert_file_missing "${SERVER_WORKSPACE_DIR}/plugins/moss/state/installed.txt"
}
