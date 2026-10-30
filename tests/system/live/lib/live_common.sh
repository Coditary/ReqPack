#!/usr/bin/env bash
set -euo pipefail

live_common_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
system_lib_root="$(CDPATH= cd -- "${live_common_root}/../../lib" && pwd)"
# shellcheck source=../../lib/container_runtime.sh
source "${system_lib_root}/container_runtime.sh"

live_log() {
    printf '[live-test] %s\n' "$*" >&2
}

live_repo_root() {
    if [ -n "${REQPACK_REPO_ROOT:-}" ]; then
        printf '%s' "${REQPACK_REPO_ROOT}"
        return
    fi
    CDPATH= cd -- "${live_common_root}/../../../../.." && pwd
}

live_plugins_root() {
    if [ -n "${REQPACK_PLUGINS_ROOT:-}" ]; then
        printf '%s' "${REQPACK_PLUGINS_ROOT}"
        return
    fi
    local repo
    repo="$(live_repo_root)"
    if [ -d "${repo}/../reqpack-plugins" ]; then
        printf '%s' "${repo}/../reqpack-plugins"
        return
    fi
    printf '%s' "${repo}/reqpack-plugins"
}

live_create_config() {
    local workspace_dir="$1"
    local plugin_dir="$2"
    local config_path="${workspace_dir}/config.lua"
    mkdir -p "${workspace_dir}"
    cat >"${config_path}" <<EOF
return {
  security = {
    autoFetch = false,
    enabled = false,
  },
  execution = {
    useTransactionDb = false,
    deleteCommittedTransactions = false,
    checkVirtualFileSystemWrite = false,
    transactionDatabasePath = "${workspace_dir}/transactions",
  },
  planner = {
    autoDownloadMissingPlugins = false,
    autoDownloadMissingDependencies = false,
  },
  registry = {
    pluginDirectory = "${plugin_dir}",
    databasePath = "${workspace_dir}/registry-db",
    autoLoadPlugins = true,
    shutDownPluginsOnExit = true,
  },
  interaction = {
    interactive = false,
  },
}
EOF
    printf '%s' "${config_path}"
}

live_stage_plugin() {
    local plugins_root="$1"
    local plugin_relpath="$2"
    local plugin_dir="$3"
    local src="${plugins_root}/${plugin_relpath}"
    if [ ! -d "${src}" ]; then
        printf 'missing plugin source: %s\n' "${src}" >&2
        return 1
    fi
    rm -rf "${plugin_dir}"
    mkdir -p "$(dirname "${plugin_dir}")"
    cp -a "${src}" "${plugin_dir}"
}

live_extract_linux_bundle() {
    local archive="$1"
    local dest="$2"
    mkdir -p "${dest}"
    tar -xzf "${archive}" -C "${dest}"
}

live_json_expect_success() {
    local path="$1"
    local label="$2"
    if [ ! -s "${path}" ]; then
        printf '%s: empty output file %s\n' "${label}" "${path}" >&2
        return 1
    fi
    if grep -Eq '"success"[[:space:]]*:[[:space:]]*true|"status"[[:space:]]*:[[:space:]]*"ok"' "${path}"; then
        return 0
    fi
    if grep -Eq '"failed"[[:space:]]*:[[:space:]]*0' "${path}"; then
        return 0
    fi
    if grep -Eq '"exitCode"[[:space:]]*:[[:space:]]*0' "${path}"; then
        return 0
    fi
    printf '%s: output does not look successful (%s)\n' "${label}" "${path}" >&2
    cat "${path}" >&2
    return 1
}

live_container_registry="${LIVE_CONTAINER_REGISTRY:-}"
live_run_id="${LIVE_RUN_ID:-}"

live_init_container_tracking() {
    local registry_path="$1"
    local run_id="$2"
    live_container_registry="${registry_path}"
    live_run_id="${run_id}"
    : >"${live_container_registry}"
}

live_register_container() {
    local name="$1"
    [ -n "${live_container_registry}" ] || return 0
    printf '%s\n' "${name}" >>"${live_container_registry}"
}

live_cleanup_containers() {
    [ -n "${live_container_registry}" ] || return 0
    [ -f "${live_container_registry}" ] || return 0
    container_detect_engine || return 0
    local name
    while IFS= read -r name; do
        [ -z "${name}" ] && continue
        "${CONTAINER_BIN}" rm -f "${name}" >/dev/null 2>&1 || true
    done <"${live_container_registry}"
}

live_container_name() {
    local row_id="$1"
    printf 'rqp-live-%s-%s' "${live_run_id}" "${row_id}"
}

live_run_container() {
    local row_id="$1"
    shift
    local name
    name="$(live_container_name "${row_id}")"
    live_register_container "${name}"
    set +e
    "${CONTAINER_BIN}" run --name "${name}" "$@"
    local status=$?
    set -e
    "${CONTAINER_BIN}" rm -f "${name}" >/dev/null 2>&1 || true
    return "${status}"
}
