#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <workspace-dir> <plugin-dir>" >&2
    exit 1
fi

workspace_dir="$1"
plugin_dir="$2"
config_path="$workspace_dir/config.lua"

mkdir -p "$workspace_dir"

cat > "$config_path" <<EOF
return {
  security = {
    autoFetch = false,
    enabled = false,
  },
  execution = {
    useTransactionDb = false,
    deleteCommittedTransactions = false,
    checkVirtualFileSystemWrite = false,
    transactionDatabasePath = "$workspace_dir/transactions",
  },
  planner = {
    autoDownloadMissingPlugins = false,
    autoDownloadMissingDependencies = false,
  },
  registry = {
    pluginDirectory = "$plugin_dir",
    databasePath = "$workspace_dir/registry-db",
    autoLoadPlugins = true,
    shutDownPluginsOnExit = true,
  },
  interaction = {
    interactive = false,
  },
}
EOF

printf '%s\n' "$config_path"
