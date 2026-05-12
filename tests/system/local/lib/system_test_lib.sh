#!/usr/bin/env bash
set -euo pipefail

: "${SYSTEM_TEST_ROOT:?missing SYSTEM_TEST_ROOT}"
: "${SYSTEM_TEST_SCENARIO:?missing SYSTEM_TEST_SCENARIO}"
: "${SYSTEM_TEST_NAME:?missing SYSTEM_TEST_NAME}"
: "${SYSTEM_TEST_OUTPUT_DIR:?missing SYSTEM_TEST_OUTPUT_DIR}"

SYSTEM_TEST_ROLE="${SYSTEM_TEST_ROLE:-single}"
SYSTEM_TEST_SHARED_ROOT="${SYSTEM_TEST_SHARED_ROOT:-${SYSTEM_TEST_ROOT}}"
SYSTEM_TEST_SHARED_RW_ROOT="${SYSTEM_TEST_SHARED_RW_ROOT:-${SYSTEM_TEST_ROOT}}"
SYSTEM_TEST_SERVER_ROOT="${SYSTEM_TEST_SERVER_ROOT:-${SYSTEM_TEST_ROOT}}"
SYSTEM_TEST_CLIENT_ROOT="${SYSTEM_TEST_CLIENT_ROOT:-${SYSTEM_TEST_ROOT}}"

WORKSPACE_DIR="${SYSTEM_TEST_ROOT}/workspace"
PLUGIN_DIR="${WORKSPACE_DIR}/plugins"
REGISTRY_SOURCE_DIR="${WORKSPACE_DIR}/registry-sources"
FAKE_BIN_DIR="${WORKSPACE_DIR}/fake-bin"
ARTIFACT_DIR="${WORKSPACE_DIR}/artifacts"
CALLS_DIR="${WORKSPACE_DIR}/calls"
REPORT_DIR="${WORKSPACE_DIR}/reports"
CONFIG_PATH="${WORKSPACE_DIR}/config.lua"
STDOUT_PATH="${SYSTEM_TEST_OUTPUT_DIR}/stdout.txt"
STDERR_PATH="${SYSTEM_TEST_OUTPUT_DIR}/stderr.txt"
STATUS_PATH="${SYSTEM_TEST_OUTPUT_DIR}/status.txt"
COMMAND_PATH="${SYSTEM_TEST_OUTPUT_DIR}/command.txt"
STDIN_PATH="${SYSTEM_TEST_OUTPUT_DIR}/stdin.txt"
WORKSPACE_COPY_DIR="${WORKSPACE_DIR}"
WORKSPACE_ARTIFACT_DIR="${SYSTEM_TEST_OUTPUT_DIR}/workspace-copy/workspace"
SERVER_RUNTIME_DIR="${SYSTEM_TEST_SERVER_ROOT}"
CLIENT_RUNTIME_DIR="${SYSTEM_TEST_CLIENT_ROOT}"
SERVER_WORKSPACE_DIR="${SERVER_RUNTIME_DIR}/workspace"
CLIENT_WORKSPACE_DIR="${CLIENT_RUNTIME_DIR}/workspace"
SERVER_CONFIG_PATH="${SERVER_WORKSPACE_DIR}/config.lua"
CLIENT_CONFIG_PATH="${CLIENT_WORKSPACE_DIR}/config.lua"
REMOTE_SERVER_HOST="${REMOTE_SERVER_HOST:-127.0.0.1}"
REMOTE_SERVER_PORT="${REMOTE_SERVER_PORT:-4545}"

mkdir -p "${WORKSPACE_DIR}" "${PLUGIN_DIR}" "${REGISTRY_SOURCE_DIR}" "${FAKE_BIN_DIR}" "${ARTIFACT_DIR}" "${CALLS_DIR}" "${REPORT_DIR}" "${SYSTEM_TEST_OUTPUT_DIR}"

export HOME="${WORKSPACE_DIR}/home"
export XDG_CACHE_HOME="${WORKSPACE_DIR}/xdg-cache"
export XDG_CONFIG_HOME="${WORKSPACE_DIR}/xdg-config"
export XDG_DATA_HOME="${WORKSPACE_DIR}/xdg-data"
export PATH="${FAKE_BIN_DIR}:/usr/local/bin:/usr/bin:/bin"
export WORKSPACE_DIR PLUGIN_DIR REGISTRY_SOURCE_DIR FAKE_BIN_DIR ARTIFACT_DIR CALLS_DIR REPORT_DIR CONFIG_PATH
export SYSTEM_TEST_ROLE SYSTEM_TEST_SHARED_ROOT SYSTEM_TEST_SHARED_RW_ROOT SYSTEM_TEST_SERVER_ROOT SYSTEM_TEST_CLIENT_ROOT
export SERVER_RUNTIME_DIR CLIENT_RUNTIME_DIR SERVER_WORKSPACE_DIR CLIENT_WORKSPACE_DIR SERVER_CONFIG_PATH CLIENT_CONFIG_PATH
export REMOTE_SERVER_HOST REMOTE_SERVER_PORT

mkdir -p "${HOME}" "${XDG_CACHE_HOME}" "${XDG_CONFIG_HOME}" "${XDG_DATA_HOME}"

scenario_log() {
    printf '[system-test] %s\n' "$*"
}

write_text() {
    local path="$1"
    shift
    mkdir -p "$(dirname "${path}")"
    printf '%s' "$*" > "${path}"
}

append_text() {
    local path="$1"
    shift
    mkdir -p "$(dirname "${path}")"
    printf '%s' "$*" >> "${path}"
}

assert_equals() {
    local expected="$1"
    local actual="$2"
    local message="$3"
    if [ "${expected}" != "${actual}" ]; then
        printf 'assert_equals failed: %s\nexpected: %s\nactual:   %s\n' "${message}" "${expected}" "${actual}" >&2
        exit 1
    fi
}

assert_file_exists() {
    local path="$1"
    if [ ! -e "${path}" ]; then
        printf 'missing file: %s\n' "${path}" >&2
        exit 1
    fi
}

assert_file_missing() {
    local path="$1"
    if [ -e "${path}" ]; then
        printf 'unexpected file: %s\n' "${path}" >&2
        exit 1
    fi
}

assert_file_contains() {
    local needle="$1"
    local path="$2"
    assert_file_exists "${path}"
    assert_output_contains "${needle}" "${path}"
}

assert_output_contains() {
    local needle="$1"
    local path="$2"
    if ! grep -F -- "${needle}" "${path}" >/dev/null 2>&1; then
        printf 'output missing token: %s\nfile: %s\n' "${needle}" "${path}" >&2
        exit 1
    fi
}

assert_output_not_contains() {
    local needle="$1"
    local path="$2"
    if grep -F -- "${needle}" "${path}" >/dev/null 2>&1; then
        printf 'output unexpectedly contains token: %s\nfile: %s\n' "${needle}" "${path}" >&2
        exit 1
    fi
}

read_status() {
    tr -d '[:space:]' < "${STATUS_PATH}"
}

role_home_dir() {
    local root="$1"
    printf '%s/workspace/home' "${root}"
}

role_xdg_config_home() {
    local root="$1"
    printf '%s/workspace/xdg-config' "${root}"
}

role_xdg_data_home() {
    local root="$1"
    printf '%s/workspace/xdg-data' "${root}"
}

role_xdg_cache_home() {
    local root="$1"
    printf '%s/workspace/xdg-cache' "${root}"
}

role_reqpack_config_dir() {
    local root="$1"
    printf '%s/reqpack' "$(role_xdg_config_home "${root}")"
}

shared_write_path() {
    local name="$1"
    printf '%s/%s' "${SYSTEM_TEST_SHARED_RW_ROOT}" "${name}"
}

shell_quote() {
    printf '%q' "$1"
}

command_with_wait_prefix() {
    local body="$1"
    printf 'wait_for_remote_server %q %q && %s' \
        "${REMOTE_SERVER_HOST}" \
        "${REMOTE_SERVER_PORT}" \
        "${body}"
}

socket_open() {
    local fd_var="$1"
    local host="$2"
    local port="$3"
    local fd
    exec {fd}<>"/dev/tcp/${host}/${port}"
    printf -v "${fd_var}" '%s' "${fd}"
}

socket_close() {
    local fd="$1"
    eval "exec ${fd}<&-"
    eval "exec ${fd}>&-"
}

socket_send_text() {
    local fd="$1"
    shift
    local payload="$*"
    eval "printf '%s' \"\${payload}\" >&${fd}"
}

socket_send_line() {
    local fd="$1"
    shift
    local payload="$*"
    eval "printf '%s\\n' \"\${payload}\" >&${fd}"
}

socket_read_line() {
    local fd="$1"
    local line=""
    if ! eval "IFS= read -r -u ${fd} line"; then
        if [ -z "${line}" ]; then
            return 1
        fi
    fi
    printf '%s' "${line}"
}

socket_read_bytes() {
    local fd="$1"
    local count="$2"
    eval "dd bs=1 count=${count} status=none <&${fd}" || return 1
}

SOCKET_RESPONSE_STATUS=""
SOCKET_RESPONSE_BODY=""

socket_read_text_response() {
    local fd="$1"
    local header
    header="$(socket_read_line "${fd}")" || return 1
    SOCKET_RESPONSE_STATUS="${header%% *}"
    local length="${header#* }"
    local temp
    temp="$(mktemp)"
    if ! eval "dd bs=1 count=${length} status=none <&${fd}" > "${temp}"; then
        rm -f "${temp}"
        return 1
    fi
    SOCKET_RESPONSE_BODY="$(<"${temp}")"
    rm -f "${temp}"
}

socket_read_json_response() {
    local fd="$1"
    SOCKET_RESPONSE_STATUS=""
    SOCKET_RESPONSE_BODY="$(socket_read_line "${fd}")" || return 1
}

socket_auth_token() {
    local fd="$1"
    local token="$2"
    socket_send_line "${fd}" "auth token ${token}"
    socket_read_text_response "${fd}"
}

socket_auth_basic() {
    local fd="$1"
    local username="$2"
    local password="$3"
    socket_send_line "${fd}" "auth basic ${username} ${password}"
    socket_read_text_response "${fd}"
}

wait_for_remote_server() {
    local host="${1:-${REMOTE_SERVER_HOST}}"
    local port="${2:-${REMOTE_SERVER_PORT}}"
    local attempts="${3:-200}"
    local delay="${4:-0.1}"
    local i=0
    while [ "${i}" -lt "${attempts}" ]; do
        if exec 3<>"/dev/tcp/${host}/${port}"; then
            exec 3<&-
            exec 3>&-
            return 0
        fi
        i=$((i + 1))
        sleep "${delay}"
    done
    printf 'remote server did not become ready: %s:%s\n' "${host}" "${port}" >&2
    exit 1
}

export -f shell_quote
export -f command_with_wait_prefix
export -f wait_for_remote_server
export -f socket_open
export -f socket_close
export -f socket_send_text
export -f socket_send_line
export -f socket_read_line
export -f socket_read_bytes
export -f socket_read_text_response
export -f socket_read_json_response
export -f socket_auth_token
export -f socket_auth_basic

write_remote_profiles() {
    local role_root="$1"
    local content="$2"
    write_text "$(role_reqpack_config_dir "${role_root}")/remote.lua" "${content}"
}

write_remote_users() {
    local role_root="$1"
    local content="$2"
    write_remote_profiles "${role_root}" "${content}"
}

remote_profile_token() {
    local name="$1"
    local token="$2"
    cat <<EOF
return {
  profiles = {
    ${name} = {
      url = 'tcp://${REMOTE_SERVER_HOST}:${REMOTE_SERVER_PORT}',
      token = '${token}',
    },
  },
}
EOF
}

remote_profile_basic() {
    local name="$1"
    local username="$2"
    local password="$3"
    cat <<EOF
return {
  profiles = {
    ${name} = {
      host = '${REMOTE_SERVER_HOST}',
      port = ${REMOTE_SERVER_PORT},
      username = '${username}',
      password = '${password}',
    },
  },
}
EOF
}

remote_profile_json_token() {
    local name="$1"
    local token="$2"
    cat <<EOF
return {
  profiles = {
    ${name} = {
      host = '${REMOTE_SERVER_HOST}',
      port = ${REMOTE_SERVER_PORT},
      protocol = 'json',
      token = '${token}',
    },
  },
}
EOF
}

remote_users_token_admin() {
    local user_id="$1"
    local token="$2"
    local admin_id="$3"
    local admin_token="$4"
    cat <<EOF
return {
  users = {
    ${user_id} = { token = '${token}' },
    ${admin_id} = { token = '${admin_token}', isAdmin = true },
  },
}
EOF
}

remote_users_basic() {
    local user_id="$1"
    local username="$2"
    local password="$3"
    cat <<EOF
return {
  users = {
    ${user_id} = { username = '${username}', password = '${password}' },
  },
}
EOF
}

run_role_command() {
    local command="$1"
    local stdin_func="${2:-}"
    printf '%s\n' "${command}" > "${COMMAND_PATH}"
    local status=0
    if [ -n "${stdin_func}" ] && declare -f "${stdin_func}" >/dev/null 2>&1; then
        "${stdin_func}" > "${STDIN_PATH}"
        set +e
        bash -lc "${command}" < "${STDIN_PATH}" > "${STDOUT_PATH}" 2> "${STDERR_PATH}"
        status=$?
        set -e
    else
        : > "${STDIN_PATH}"
        set +e
        bash -lc "${command}" > "${STDOUT_PATH}" 2> "${STDERR_PATH}"
        status=$?
        set -e
    fi
    printf '%s\n' "${status}" > "${STATUS_PATH}"
}

render_registry_sources() {
    local output="$1"
    if [ ! -d "${REGISTRY_SOURCE_DIR}" ]; then
        return
    fi
    local entries=()
    local source_dir
    for source_dir in "${REGISTRY_SOURCE_DIR}"/*; do
        [ -d "${source_dir}" ] || continue
        local name
        name="$(basename "${source_dir}")"
        entries+=("      ${name} = { source = '${source_dir}', description = '${name} fixture source' },")
    done
    if [ "${#entries[@]}" -eq 0 ]; then
        return
    fi
    printf '    sources = {\n' >> "${output}"
    local entry
    for entry in "${entries[@]}"; do
        printf '%s\n' "${entry}" >> "${output}"
    done
    printf '    },\n' >> "${output}"
}

create_test_config() {
    local interaction_interactive="${1:-false}"
    local auto_download_plugins="${2:-false}"
    local osv_overlay_path="${3:-}"
    local on_unsafe="${4:-continue}"
    local severity_threshold="${5:-critical}"
    local score_threshold="${6:-0.0}"
    local prompt_on_unsafe="${7:-false}"
    local osv_ecosystem_map_entries="${8:-}"

    mkdir -p "${WORKSPACE_DIR}"
    : > "${CONFIG_PATH}"
    printf 'return {\n' >> "${CONFIG_PATH}"
    printf '  security = {\n' >> "${CONFIG_PATH}"
    printf '    autoFetch = false,\n' >> "${CONFIG_PATH}"
    printf '    enabled = true,\n' >> "${CONFIG_PATH}"
    printf '    osvRefreshMode = "manual",\n' >> "${CONFIG_PATH}"
    printf '    osvDatabasePath = "%s",\n' "${WORKSPACE_DIR}/osv-db" >> "${CONFIG_PATH}"
    if [ -n "${osv_overlay_path}" ]; then
        printf '    osvOverlayPath = "%s",\n' "${osv_overlay_path}" >> "${CONFIG_PATH}"
    fi
    printf '    onUnsafe = "%s",\n' "${on_unsafe}" >> "${CONFIG_PATH}"
    printf '    promptOnUnsafe = %s,\n' "${prompt_on_unsafe}" >> "${CONFIG_PATH}"
    printf '    severityThreshold = "%s",\n' "${severity_threshold}" >> "${CONFIG_PATH}"
    printf '    scoreThreshold = %s,\n' "${score_threshold}" >> "${CONFIG_PATH}"
    if [ -n "${osv_ecosystem_map_entries}" ]; then
        printf '    osvEcosystemMap = {\n' >> "${CONFIG_PATH}"
        printf '%s\n' "${osv_ecosystem_map_entries}" >> "${CONFIG_PATH}"
        printf '    },\n' >> "${CONFIG_PATH}"
    fi
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '  execution = {\n' >> "${CONFIG_PATH}"
    printf '    useTransactionDb = false,\n' >> "${CONFIG_PATH}"
    printf '    deleteCommittedTransactions = false,\n' >> "${CONFIG_PATH}"
    printf '    checkVirtualFileSystemWrite = false,\n' >> "${CONFIG_PATH}"
    printf '    transactionDatabasePath = "%s",\n' "${WORKSPACE_DIR}/transactions" >> "${CONFIG_PATH}"
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '  planner = {\n' >> "${CONFIG_PATH}"
    printf '    autoDownloadMissingPlugins = %s,\n' "${auto_download_plugins}" >> "${CONFIG_PATH}"
    printf '    autoDownloadMissingDependencies = false,\n' >> "${CONFIG_PATH}"
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '  registry = {\n' >> "${CONFIG_PATH}"
    printf '    pluginDirectory = "%s",\n' "${PLUGIN_DIR}" >> "${CONFIG_PATH}"
    printf '    databasePath = "%s",\n' "${WORKSPACE_DIR}/registry-db" >> "${CONFIG_PATH}"
    printf '    remoteUrl = "",\n' >> "${CONFIG_PATH}"
    printf '    autoLoadPlugins = true,\n' >> "${CONFIG_PATH}"
    printf '    shutDownPluginsOnExit = true,\n' >> "${CONFIG_PATH}"
    render_registry_sources "${CONFIG_PATH}"
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '  interaction = {\n' >> "${CONFIG_PATH}"
    printf '    interactive = %s,\n' "${interaction_interactive}" >> "${CONFIG_PATH}"
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '  rqp = {\n' >> "${CONFIG_PATH}"
    printf '    statePath = "%s",\n' "${WORKSPACE_DIR}/rqp-state" >> "${CONFIG_PATH}"
    printf '  },\n' >> "${CONFIG_PATH}"
    printf '}\n' >> "${CONFIG_PATH}"
}

append_self_update_config() {
    local release_api_base_url="$1"
    local repo_url="$2"
    local release_tag="$3"
    local binary_directory="${4:-${WORKSPACE_DIR}/self-update/bin}"
    local link_path="${5:-${WORKSPACE_DIR}/self-update/current/rqp}"

    local temp_path="${CONFIG_PATH}.tmp"
    sed '$d' "${CONFIG_PATH}" > "${temp_path}"
    printf '  selfUpdate = {\n' >> "${temp_path}"
    printf '    releaseApiBaseUrl = "%s",\n' "${release_api_base_url}" >> "${temp_path}"
    printf '    repoUrl = "%s",\n' "${repo_url}" >> "${temp_path}"
    printf '    releaseTag = "%s",\n' "${release_tag}" >> "${temp_path}"
    printf '    binaryDirectory = "%s",\n' "${binary_directory}" >> "${temp_path}"
    printf '    linkPath = "%s",\n' "${link_path}" >> "${temp_path}"
    printf '  },\n' >> "${temp_path}"
    printf '}\n' >> "${temp_path}"
    mv "${temp_path}" "${CONFIG_PATH}"
}

write_plugin_bundle() {
    local root="$1"
    local name="$2"
    local run_lua="$3"
    shift 3
    local dependency_specs=("$@")
    mkdir -p "${root}/${name}/scripts"
    local manifest='return {
  apiVersion = 1,
  depends = {'
    local spec
    if [ "${#dependency_specs[@]}" -gt 0 ]; then
        manifest+=$'\n'
        for spec in "${dependency_specs[@]}"; do
            manifest+="    \"${spec}\","
            manifest+=$'\n'
        done
        manifest+="  "
    fi
    manifest+=$'}\n}\n'
    write_text "${root}/${name}/metadata.json" "{
  \"formatVersion\": 1,
  \"name\": \"${name}\",
  \"version\": \"1.0.0\",
  \"summary\": \"${name} plugin\",
  \"description\": \"${name} plugin bundle\",
  \"license\": \"MIT\"
}
"
    write_text "${root}/${name}/reqpack.lua" "${manifest}"
    write_text "${root}/${name}/run.lua" "${run_lua}"
    write_text "${root}/${name}/scripts/install.lua" "return true
"
    write_text "${root}/${name}/scripts/remove.lua" "return true
"
}

base_demo_pack_plugin() {
    cat <<'EOF'
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pack", "system-test" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.outdated(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0", system = REQPACK_PLUGIN_ID } end
function plugin.pack(context, projectPath, outputPath, flags)
  local artifact = outputPath
  if artifact == nil or artifact == "" then
    artifact = projectPath .. "/dist/demo.pkg"
  end
  context.exec.run("mkdir -p '" .. artifact:match("(.+)/[^/]+$") .. "' && printf '%s' 'native-artifact' > '" .. artifact .. "'")
  context.artifacts.register(artifact)
  return true
end
function plugin.shutdown() return true end

return plugin
EOF
}

base_demo_plugin_test_plugin() {
    cat <<'EOF'
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "test", "system-test" } end
function plugin.getMissingPackages(packages) return packages or {} end

function plugin.install(context, packages)
  context.tx.begin_step("install")
  local result = context.exec.run("fake-pm install " .. packages[1].name)
  if not result.success then
    context.tx.failed("install failed")
    return false
  end
  context.events.installed({ name = packages[1].name })
  context.tx.success()
  return true
end

function plugin.installLocal(context, path)
  local result = context.exec.run("fake-pm install-local " .. path)
  return result.success
end

function plugin.remove(context, packages)
  local result = context.exec.run("fake-pm remove " .. packages[1].name)
  if result.success then
    context.events.deleted({ name = packages[1].name })
  end
  return result.success
end

function plugin.update(context, packages)
  local result = context.exec.run("fake-pm update " .. packages[1].name)
  if result.success then
    context.events.updated({ name = packages[1].name })
  end
  return result.success
end

function plugin.list(context)
  local result = reqpack.exec.run("fake-pm list")
  if not result.success then
    return {}
  end
  return {
    { name = "alpha", version = "1.2.3", description = result.stdout },
  }
end

function plugin.search(context, prompt)
  local result = context.exec.run("fake-pm search " .. prompt)
  if not result.success then
    return {}
  end
  context.artifacts.register({ kind = "search-cache", path = "/tmp/search-cache.json" })
  return {
    { name = prompt, version = "9.9.9", description = result.stdout },
  }
end

function plugin.info(context, packageName)
  local result = context.exec.run("fake-pm info " .. packageName)
  if not result.success then
    return {}
  end
  return { name = packageName, version = "4.5.6", description = result.stdout }
end

function plugin.shutdown() return true end

return plugin
EOF
}

write_plugin_test_case() {
    local path="$1"
    local content="$2"
    write_text "${path}" "${content}"
}

write_plugin_test_preset_cases() {
    local plugin_root="$1"
    local plugin_name="$2"
    shift 2
    local preset_dir="${plugin_root}/${plugin_name}/.reqpack-test/core"
    mkdir -p "${preset_dir}"
    while [ "$#" -gt 1 ]; do
        local file_name="$1"
        local content="$2"
        write_plugin_test_case "${preset_dir}/${file_name}" "${content}"
        shift 2
    done
}

plugin_test_case_install_pass() {
    cat <<'EOF'
return {
  name = "install success",
  request = {
    action = "install",
    system = "demo",
    packages = {
      { name = "curl", version = "8.0" }
    }
  },
  fakeExec = {
    {
      match = "fake-pm install curl",
      exitCode = 0,
      stdout = "ok\n",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm install curl" },
    stdout = { "ok\n" },
    events = { "installed", "success" },
    eventPayloads = {
      installed = "{name=curl}",
      success = "ok",
    },
  }
}
EOF
}

plugin_test_case_install_fail() {
    cat <<'EOF'
return {
  name = "install expectation mismatch",
  request = {
    action = "install",
    system = "demo",
    packages = {
      { name = "curl" }
    }
  },
  fakeExec = {
    {
      match = "fake-pm install curl",
      exitCode = 1,
      stdout = "",
      stderr = "boom",
      success = false,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm install curl" },
  }
}
EOF
}

plugin_test_case_search_pass() {
    cat <<'EOF'
return {
  name = "search artifact and payload",
  request = {
    action = "search",
    system = "demo",
    prompt = "delta"
  },
  fakeExec = {
    {
      match = "fake-pm search delta",
      exitCode = 0,
      stdout = "delta line",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm search delta" },
    stdout = { "delta line" },
    artifacts = { "{kind=search-cache, path=/tmp/search-cache.json}" },
    resultCount = 1,
    resultName = "delta",
    resultVersion = "9.9.9",
  }
}
EOF
}

plugin_test_case_demo_list_preset() {
    cat <<'EOF'
return {
  name = "core list preset",
  request = {
    action = "list",
    system = "demo"
  },
  fakeExec = {
    {
      match = "fake-pm list",
      exitCode = 0,
      stdout = "alpha line",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm list" },
    resultCount = 1,
    resultName = "alpha",
  }
}
EOF
}

plugin_test_case_moss_list_preset() {
    cat <<'EOF'
return {
  name = "core moss list",
  request = {
    action = "list",
    system = "moss"
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 2,
    resultName = "luma",
    resultVersion = "1.0.0",
  }
}
EOF
}

plugin_test_case_moss_info_preset() {
    cat <<'EOF'
return {
  name = "core moss info",
  request = {
    action = "info",
    system = "moss",
    prompt = "luma"
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 1,
    resultName = "luma",
    resultVersion = "1.0.0",
  }
}
EOF
}

plugin_test_case_quill_list_preset() {
    cat <<'EOF'
return {
  name = "core quill list",
  request = {
    action = "list",
    system = "quill"
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 2,
    resultName = "zeno",
    resultVersion = "1.0.0",
  }
}
EOF
}

plugin_test_case_quill_search_preset() {
    cat <<'EOF'
return {
  name = "core quill search",
  request = {
    action = "search",
    system = "quill",
    prompt = "orbit"
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 1,
    resultName = "orbit",
    resultVersion = "1.0.0",
  }
}
EOF
}

write_fake_pm() {
    local name="$1"
    local body="$2"
    local path="${FAKE_BIN_DIR}/${name}"
    write_text "${path}" "#!/usr/bin/env bash
set -euo pipefail
${body}
"
    chmod +x "${path}"
}

base_moss_plugin() {
    cat <<'EOF'
plugin = {}
plugin.fileExtensions = { ".mosspkg" }

local function lower(value)
  return string.lower(value or "")
end

local function contains(haystack, needle)
  return string.find(lower(haystack), lower(needle), 1, true) ~= nil
end

local function installed_specs(state_dir)
  local path = state_dir .. "/installed.txt"
  local file = io.open(path, "r")
  if file == nil then
    return {}
  end
  local specs = {}
  for line in file:lines() do
    specs[lower(line)] = true
    local at = string.find(line, "@", 1, true)
    if at ~= nil and at > 1 then
      specs[lower(string.sub(line, 1, at - 1))] = true
    end
  end
  file:close()
  return specs
end

local function filter_search_results(items, prompt)
  local query = prompt or ""
  if query == "" then
    return items
  end
  local result = {}
  for _, item in ipairs(items) do
    if contains(item.name, query) or contains(item.description, query) then
      table.insert(result, item)
    end
  end
  return result
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getSecurityMetadata()
  return {
    osvEcosystem = "moss-osv",
    purlType = "generic",
    versionComparatorProfile = "lexicographic",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "system-test" } end
function plugin.getMissingPackages(packages)
  local installed = installed_specs(REQPACK_PLUGIN_DIR .. "/state")
  local result = {}
  for _, package in ipairs(packages) do
    local spec = lower(package.name)
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. lower(package.version)
    end
    if not installed[spec] then
      table.insert(result, {
        system = package.system,
        name = package.name,
        version = package.version,
        sourcePath = package.sourcePath,
        localTarget = package.localTarget,
        flags = package.flags,
      })
    end
  end
  return result
end
function plugin.install(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("mossctl install '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/installed.txt'")
  end
  return true
end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  local result = context.exec.run("mossctl install-local '" .. path .. "'")
  if not result.success then
    return false
  end
  context.exec.run("printf '%s\n' '" .. path .. "' > '" .. state .. "/local.txt'")
  return true
end
function plugin.remove(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("mossctl remove '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/removed.txt'")
  end
  return true
end
function plugin.update(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  if packages == nil or #packages == 0 then
    local result = context.exec.run("mossctl update-all")
    if not result.success then
      return false
    end
    context.exec.run("printf 'update-all\n' >> '" .. state .. "/update-all.txt'")
    return true
  end
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("mossctl update '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/updated.txt'")
  end
  return true
end
function plugin.list(context)
  return {
    { name = "luma", version = "1.0.0", architecture = "x86_64", packageType = "pm", description = "moss runtime fixture" },
    { name = "fable", version = "1.0.0", architecture = "aarch64", packageType = "doc", description = "moss docs fixture" },
  }
end
function plugin.outdated(context)
  return {
    { name = "luma", version = "1.0.0", latestVersion = "1.2.0", architecture = "x86_64", packageType = "pm", description = "moss runtime fixture" },
    { name = "fable", version = "1.0.0", latestVersion = "1.1.0", architecture = "aarch64", packageType = "doc", description = "moss docs fixture" },
  }
end
function plugin.search(context, prompt)
  return filter_search_results({
    {
      name = "test",
      version = "1.0.0",
      architecture = "x86_64",
      packageType = "pm",
      description = "test package manager fixture",
    },
    {
      name = "test-doc",
      version = "1.0.0",
      architecture = "x86_64",
      packageType = "doc",
      description = "test docs fixture",
    },
    {
      name = "prompt-match",
      version = "2.0.0",
      architecture = "noarch",
      packageType = "pm",
      description = "das ist mein prompt",
    },
    {
      name = "arm-test",
      version = "1.0.0",
      architecture = "aarch64",
      packageType = "pm",
      description = "test package manager fixture arm",
    },
  }, prompt)
end
function plugin.info(context, package)
  if package == nil or package == "" then
    return { name = "moss", version = "1.0.0", description = "moss plugin fixture", packageType = "plugin", system = "moss" }
  end
  return { name = package, version = "1.0.0", description = "moss luma fixture", architecture = "x86_64", packageType = "pm", system = "moss" }
end
function plugin.shutdown() return true end

return plugin
EOF
}

base_anvil_plugin() {
    cat <<'EOF'
plugin = {}
plugin.fileExtensions = { ".anvilpkg", ".anvilpkg.zst" }

local function lower(value)
  return string.lower(value or "")
end

local function contains(haystack, needle)
  return string.find(lower(haystack), lower(needle), 1, true) ~= nil
end

local function installed_specs(state_dir)
  local path = state_dir .. "/installed.txt"
  local file = io.open(path, "r")
  if file == nil then
    return {}
  end
  local specs = {}
  for line in file:lines() do
    specs[lower(line)] = true
    local at = string.find(line, "@", 1, true)
    if at ~= nil and at > 1 then
      specs[lower(string.sub(line, 1, at - 1))] = true
    end
  end
  file:close()
  return specs
end

local function filter_search_results(items, prompt)
  local query = prompt or ""
  if query == "" then
    return items
  end
  local result = {}
  for _, item in ipairs(items) do
    if contains(item.name, query) or contains(item.description, query) then
      table.insert(result, item)
    end
  end
  return result
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "system-test" } end
function plugin.getMissingPackages(packages)
  local installed = installed_specs(REQPACK_PLUGIN_DIR .. "/state")
  local result = {}
  for _, package in ipairs(packages) do
    local spec = lower(package.name)
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. lower(package.version)
    end
    if not installed[spec] then
      table.insert(result, {
        system = package.system,
        name = package.name,
        version = package.version,
        sourcePath = package.sourcePath,
        localTarget = package.localTarget,
        flags = package.flags,
      })
    end
  end
  return result
end
function plugin.install(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("anvilctl install '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/installed.txt'")
  end
  return true
end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  local result = context.exec.run("anvilctl install-local '" .. path .. "'")
  if not result.success then
    return false
  end
  context.exec.run("printf '%s\n' '" .. path .. "' > '" .. state .. "/local.txt'")
  return true
end
function plugin.remove(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("anvilctl remove '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/removed.txt'")
  end
  return true
end
function plugin.update(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  if packages == nil or #packages == 0 then
    local result = context.exec.run("anvilctl update-all")
    if not result.success then
      return false
    end
    context.exec.run("printf 'update-all\n' >> '" .. state .. "/update-all.txt'")
    return true
  end
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("anvilctl update '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/updated.txt'")
  end
  return true
end
function plugin.list(context)
  return {
    { name = "fable", version = "1.0.0", architecture = "x86_64", packageType = "pm", description = "anvil fixture" },
  }
end
function plugin.outdated(context)
  return {
    { name = "fable", version = "1.0.0", latestVersion = "1.3.0", architecture = "x86_64", packageType = "pm", description = "anvil fixture" },
  }
end
function plugin.search(context, prompt)
  return filter_search_results({
    { name = "forge", version = "1.0.0", architecture = "x86_64", packageType = "pm", description = "anvil forge fixture" },
  }, prompt)
end
function plugin.info(context, package)
  if package == nil or package == "" then
    return { name = "anvil", version = "1.0.0", description = "anvil plugin fixture", packageType = "plugin", system = "anvil" }
  end
  return { name = package, version = "1.0.0", description = "anvil fixture", packageType = "pm", system = "anvil" }
end
function plugin.shutdown() return true end

return plugin
EOF
}

base_quill_plugin() {
    cat <<'EOF'
plugin = {}

local function lower(value)
  return string.lower(value or "")
end

local function contains(haystack, needle)
  return string.find(lower(haystack), lower(needle), 1, true) ~= nil
end

local function installed_specs(state_dir)
  local path = state_dir .. "/installed.txt"
  local file = io.open(path, "r")
  if file == nil then
    return {}
  end
  local specs = {}
  for line in file:lines() do
    specs[lower(line)] = true
    local at = string.find(line, "@", 1, true)
    if at ~= nil and at > 1 then
      specs[lower(string.sub(line, 1, at - 1))] = true
    end
  end
  file:close()
  return specs
end

local function filter_search_results(items, prompt)
  local query = prompt or ""
  if query == "" then
    return items
  end
  local result = {}
  for _, item in ipairs(items) do
    if contains(item.name, query) or contains(item.description, query) then
      table.insert(result, item)
    end
  end
  return result
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getSecurityMetadata()
  return {
    osvEcosystem = "quill-osv",
    purlType = "generic",
    versionComparatorProfile = "lexicographic",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "system-test" } end
function plugin.getMissingPackages(packages)
  local installed = installed_specs(REQPACK_PLUGIN_DIR .. "/state")
  local result = {}
  for _, package in ipairs(packages) do
    local spec = lower(package.name)
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. lower(package.version)
    end
    if not installed[spec] then
      table.insert(result, {
        system = package.system,
        name = package.name,
        version = package.version,
        sourcePath = package.sourcePath,
        localTarget = package.localTarget,
        flags = package.flags,
      })
    end
  end
  return result
end
function plugin.install(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("quillctl install '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/installed.txt'")
  end
  return true
end
function plugin.installLocal(context, path)
  return false
end
function plugin.remove(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("quillctl remove '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/removed.txt'")
  end
  return true
end
function plugin.update(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  if packages == nil or #packages == 0 then
    local result = context.exec.run("quillctl update-all")
    if not result.success then
      return false
    end
    context.exec.run("printf 'update-all\n' >> '" .. state .. "/update-all.txt'")
    return true
  end
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("quillctl update '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/updated.txt'")
  end
  return true
end
function plugin.list(context)
  return {
    { name = "zeno", version = "1.0.0", architecture = "x86_64", packageType = "pm", description = "quill fixture" },
    { name = "orbit", version = "1.0.0", architecture = "noarch", packageType = "doc", description = "quill fixture" },
  }
end
function plugin.outdated(context)
  return {
    { name = "zeno", version = "1.0.0", latestVersion = "2.0.0", architecture = "x86_64", packageType = "pm", description = "quill fixture" },
  }
end
function plugin.search(context, prompt)
  return filter_search_results({
    { name = "zeno", version = "1.0.0", architecture = "x86_64", packageType = "pm", description = "quill zeno fixture" },
    { name = "orbit", version = "1.0.0", architecture = "noarch", packageType = "doc", description = "quill orbit docs fixture" },
  }, prompt)
end
function plugin.info(context, package)
  if package == nil or package == "" then
    return { name = "quill", version = "1.0.0", description = "quill plugin fixture", packageType = "plugin", system = "quill" }
  end
  if package == "zeno" then
    return { name = "zeno", version = "1.0.0", description = "quill zeno fixture", architecture = "x86_64", packageType = "pm", system = "quill" }
  end
  if package == "orbit" then
    return { name = "orbit", version = "1.0.0", description = "quill fixture", architecture = "noarch", packageType = "doc", system = "quill" }
  end
  return {}
end
function plugin.shutdown() return true end

return plugin
EOF
}

base_glyph_plugin() {
    cat <<'EOF'
plugin = {}

local function lower(value)
  return string.lower(value or "")
end

local function contains(haystack, needle)
  return string.find(lower(haystack), lower(needle), 1, true) ~= nil
end

local function installed_specs(state_dir)
  local path = state_dir .. "/installed.txt"
  local file = io.open(path, "r")
  if file == nil then
    return {}
  end
  local specs = {}
  for line in file:lines() do
    specs[lower(line)] = true
    local at = string.find(line, "@", 1, true)
    if at ~= nil and at > 1 then
      specs[lower(string.sub(line, 1, at - 1))] = true
    end
  end
  file:close()
  return specs
end

local function filter_search_results(items, prompt)
  local query = prompt or ""
  if query == "" then
    return items
  end
  local result = {}
  for _, item in ipairs(items) do
    if contains(item.name, query) or contains(item.description, query) then
      table.insert(result, item)
    end
  end
  return result
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "system-test" } end
function plugin.getMissingPackages(packages)
  local installed = installed_specs(REQPACK_PLUGIN_DIR .. "/state")
  local result = {}
  for _, package in ipairs(packages) do
    local spec = lower(package.name)
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. lower(package.version)
    end
    if not installed[spec] then
      table.insert(result, {
        system = package.system,
        name = package.name,
        version = package.version,
        sourcePath = package.sourcePath,
        localTarget = package.localTarget,
        flags = package.flags,
      })
    end
  end
  return result
end
function plugin.install(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("glyphctl install '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/installed.txt'")
  end
  return true
end
function plugin.installLocal(context, path)
  return false
end
function plugin.remove(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("glyphctl remove '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/removed.txt'")
  end
  return true
end
function plugin.update(context, packages)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  if packages == nil or #packages == 0 then
    local result = context.exec.run("glyphctl update-all")
    if not result.success then
      return false
    end
    context.exec.run("printf 'update-all\n' >> '" .. state .. "/update-all.txt'")
    return true
  end
  for _, package in ipairs(packages) do
    local spec = package.name
    if package.version ~= nil and package.version ~= "" then
      spec = spec .. "@" .. package.version
    end
    local result = context.exec.run("glyphctl update '" .. spec .. "'")
    if not result.success then
      return false
    end
    context.exec.run("printf '%s\n' '" .. spec .. "' >> '" .. state .. "/updated.txt'")
  end
  return true
end
function plugin.list(context)
  return {
    { name = "fable", version = "1.0.0", architecture = "noarch", packageType = "pm", description = "glyph fixture" },
  }
end
function plugin.outdated(context)
  return {
    { name = "fable", version = "1.0.0", latestVersion = "1.4.0", architecture = "noarch", packageType = "pm", description = "glyph fixture" },
  }
end
function plugin.search(context, prompt)
  return filter_search_results({
    { name = "fable", version = "1.0.0", architecture = "noarch", packageType = "pm", description = "glyph fable fixture" },
  }, prompt)
end
function plugin.info(context, package)
  if package == nil or package == "" then
    return { name = "glyph", version = "1.0.0", description = "glyph plugin fixture", packageType = "plugin", system = "glyph" }
  end
  return { name = package, version = "1.0.0", description = "glyph fixture", packageType = "pm", system = "glyph" }
end
function plugin.shutdown() return true end

return plugin
EOF
}

write_base_plugins() {
    write_plugin_bundle "${PLUGIN_DIR}" "moss" "$(base_moss_plugin)"
    write_plugin_bundle "${PLUGIN_DIR}" "anvil" "$(base_anvil_plugin)"
    write_plugin_bundle "${PLUGIN_DIR}" "quill" "$(base_quill_plugin)"
    write_plugin_bundle "${PLUGIN_DIR}" "glyph" "$(base_glyph_plugin)"
}

write_ensure_fixture_plugins() {
    write_plugin_bundle "${PLUGIN_DIR}" "moss" "$(base_moss_plugin)" "quill:zeno"
    write_plugin_bundle "${PLUGIN_DIR}" "anvil" "$(base_anvil_plugin)" "glyph:fable"
    write_plugin_bundle "${PLUGIN_DIR}" "quill" "$(base_quill_plugin)"
    write_plugin_bundle "${PLUGIN_DIR}" "glyph" "$(base_glyph_plugin)"
}

write_base_fake_binaries() {
    write_fake_pm "mossctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "moss install %s\n" "$*" >> "${CALLS_DIR}/moss.log"
    ;;
  update)
    printf "moss update %s\n" "$*" >> "${CALLS_DIR}/moss.log"
    ;;
  update-all)
    printf "moss update-all\n" >> "${CALLS_DIR}/moss.log"
    ;;
  remove)
    printf "moss remove %s\n" "$*" >> "${CALLS_DIR}/moss.log"
    ;;
  install-local)
    printf "moss local %s\n" "$*" >> "${CALLS_DIR}/moss.log"
    ;;
  *)
    printf "unknown mossctl command: %s\n" "${cmd}" >&2
    exit 1
    ;;
esac
'
    write_fake_pm "anvilctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "anvil install %s\n" "$*" >> "${CALLS_DIR}/anvil.log"
    ;;
  install-local)
    printf "anvil local %s\n" "$*" >> "${CALLS_DIR}/anvil.log"
    ;;
  update)
    printf "anvil update %s\n" "$*" >> "${CALLS_DIR}/anvil.log"
    ;;
  update-all)
    printf "anvil update-all\n" >> "${CALLS_DIR}/anvil.log"
    ;;
  remove)
    printf "anvil remove %s\n" "$*" >> "${CALLS_DIR}/anvil.log"
    ;;
  *)
    printf "unknown anvilctl command: %s\n" "${cmd}" >&2
    exit 1
    ;;
esac
'
    write_fake_pm "quillctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "quill install %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    ;;
  update)
    printf "quill update %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    ;;
  update-all)
    printf "quill update-all\n" >> "${CALLS_DIR}/quill.log"
    ;;
  remove)
    printf "quill remove %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    ;;
  *)
    printf "unknown quillctl command: %s\n" "${cmd}" >&2
    exit 1
    ;;
esac
'
    write_fake_pm "glyphctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "glyph install %s\n" "$*" >> "${CALLS_DIR}/glyph.log"
    ;;
  update)
    printf "glyph update %s\n" "$*" >> "${CALLS_DIR}/glyph.log"
    ;;
  update-all)
    printf "glyph update-all\n" >> "${CALLS_DIR}/glyph.log"
    ;;
  remove)
    printf "glyph remove %s\n" "$*" >> "${CALLS_DIR}/glyph.log"
    ;;
  *)
    printf "unknown glyphctl command: %s\n" "${cmd}" >&2
    exit 1
    ;;
esac
'
}

write_manifest() {
    local path="$1"
    local entries="$2"
    write_text "${path}" "return {
  packages = {
${entries}
  }
}
"
}

write_osv_overlay_for_quill() {
    local path="$1"
    local severity="$2"
    local score="$3"
    local version="$4"
    write_text "${path}" "[
  {
    \"id\": \"CVE-TEST-QUILL-001\",
    \"modified\": \"2026-01-01T00:00:00Z\",
    \"summary\": \"quill zeno test advisory\",
    \"severity\": [{\"type\": \"CVSS_V3\", \"score\": \"${score}\"}],
    \"database_specific\": { \"severity\": \"${severity}\" },
    \"affected\": [{
      \"package\": {\"ecosystem\": \"quill-osv\", \"name\": \"zeno\"},
      \"versions\": [\"${version}\"]
    }]
  }
]
"
}

write_osv_overlay_for_audit() {
    local path="$1"
    local mode="${2:-quill-only}"
    case "${mode}" in
        empty)
            write_text "${path}" "[]
"
            ;;
        quill-only)
            write_text "${path}" "[
  {
    \"id\": \"CVE-TEST-QUILL-001\",
    \"modified\": \"2026-01-01T00:00:00Z\",
    \"summary\": \"quill zeno audit advisory\",
    \"severity\": [{\"type\": \"CVSS_V3\", \"score\": \"8.8\"}],
    \"database_specific\": { \"severity\": \"high\" },
    \"affected\": [{
      \"package\": {\"ecosystem\": \"quill-osv\", \"name\": \"zeno\"},
      \"versions\": [\"1.0.0\"]
    }]
  }
]
"
            ;;
        moss-and-quill)
            write_text "${path}" "[
  {
    \"id\": \"CVE-TEST-MOSS-001\",
    \"modified\": \"2026-01-01T00:00:00Z\",
    \"summary\": \"moss luma audit advisory\",
    \"severity\": [{\"type\": \"CVSS_V3\", \"score\": \"6.5\"}],
    \"database_specific\": { \"severity\": \"medium\" },
    \"affected\": [{
      \"package\": {\"ecosystem\": \"moss-osv\", \"name\": \"luma\"},
      \"versions\": [\"1.0.0\"]
    }]
  },
  {
    \"id\": \"CVE-TEST-QUILL-001\",
    \"modified\": \"2026-01-01T00:00:00Z\",
    \"summary\": \"quill zeno audit advisory\",
    \"severity\": [{\"type\": \"CVSS_V3\", \"score\": \"8.8\"}],
    \"database_specific\": { \"severity\": \"high\" },
    \"affected\": [{
      \"package\": {\"ecosystem\": \"quill-osv\", \"name\": \"zeno\"},
      \"versions\": [\"1.0.0\"]
    }]
  }
]
"
            ;;
        *)
            printf 'unknown audit overlay mode: %s\n' "${mode}" >&2
            exit 1
            ;;
    esac
}

base_prepare_audit_fixture() {
    local overlay_mode="${1:-quill-only}"
    local interactive="${2:-false}"
    local osv_ecosystem_map_entries="${3:-}"
    local overlay_path="${WORKSPACE_DIR}/audit-overlay.json"
    write_base_plugins
    write_base_fake_binaries
    write_osv_overlay_for_audit "${overlay_path}" "${overlay_mode}"
    create_test_config "${interactive}" false "${overlay_path}" continue critical 0.0 false "${osv_ecosystem_map_entries}"
}

seed_snapshot_history() {
    local seed_stdout="${SYSTEM_TEST_OUTPUT_DIR}/snapshot-seed-stdout.txt"
    local seed_stderr="${SYSTEM_TEST_OUTPUT_DIR}/snapshot-seed-stderr.txt"

    if ! rqp --config "${CONFIG_PATH}" install moss:luma quill:zeno --non-interactive >"${seed_stdout}" 2>"${seed_stderr}"; then
        printf 'failed to seed snapshot history\nstdout: %s\nstderr: %s\n' "${seed_stdout}" "${seed_stderr}" >&2
        exit 1
    fi
}

base_prepare_snapshot_fixture() {
    base_prepare
    seed_snapshot_history
}

write_builtin_pack_project() {
    local project_root="$1"
    local package_name="${2:-demo}"
    local package_version="${3:-1.2.3}"
    local package_release="${4:-2}"
    local package_revision="${5:-1}"
    local install_marker="${6:-${WORKSPACE_DIR}/pack-installed.txt}"

    mkdir -p "${project_root}/scripts" "${project_root}/payload-tree/share"
    write_text "${project_root}/metadata.json" "{
  \"formatVersion\": 1,
  \"name\": \"${package_name}\",
  \"version\": \"${package_version}\",
  \"release\": ${package_release},
  \"revision\": ${package_revision},
  \"summary\": \"${package_name}\",
  \"description\": \"${package_name} package\",
  \"license\": \"MIT\",
  \"vendor\": \"ReqPack System Tests\",
  \"maintainerEmail\": \"tests@example.org\",
  \"url\": \"https://example.test/${package_name}.rqp\"
}
"
    write_text "${project_root}/reqpack.lua" "return {
  apiVersion = 1,
  hooks = {
    install = \"scripts/install.lua\"
  }
}
"
    write_text "${project_root}/scripts/install.lua" "context.fs.copy(context.paths.payloadDir .. '/share/hello.txt', '${install_marker}')
return true
"
    write_text "${project_root}/payload-tree/share/hello.txt" "hello\n"
}

write_builtin_pack_project_without_payload_tree() {
    local project_root="$1"
    local package_name="${2:-external}"

    mkdir -p "${project_root}/scripts"
    write_text "${project_root}/metadata.json" "{
  \"formatVersion\": 1,
  \"name\": \"${package_name}\",
  \"version\": \"1.0.0\",
  \"release\": 1,
  \"revision\": 0,
  \"summary\": \"${package_name}\",
  \"description\": \"${package_name} package\",
  \"license\": \"MIT\",
  \"vendor\": \"ReqPack System Tests\",
  \"maintainerEmail\": \"tests@example.org\",
  \"url\": \"https://example.test/${package_name}.rqp\"
}
"
    write_text "${project_root}/reqpack.lua" "return {
  apiVersion = 1,
  hooks = {
    install = \"scripts/install.lua\"
  }
}
"
    write_text "${project_root}/scripts/install.lua" "return true
"
}

write_self_update_fixture() {
    local fixture_root="$1"
    local tag="${2:-v9.9.9}"
    local target="${3:-x86_64-linux}"
    local asset_name="rqp-${tag}-${target}.tar.gz"
    local release_dir="${fixture_root}/repos/coditary/reqpack/releases/tags"
    local asset_dir="${fixture_root}/assets/${tag}"
    local bundle_dir="${fixture_root}/bundle/${tag}"

    mkdir -p "${release_dir}" "${asset_dir}" "${bundle_dir}"
    write_text "${bundle_dir}/rqp" "#!/usr/bin/env bash
set -euo pipefail
printf 'self-update fixture ${tag}\n'
"
    chmod +x "${bundle_dir}/rqp"
    tar -czf "${asset_dir}/${asset_name}" -C "${bundle_dir}" rqp
    write_text "${release_dir}/${tag}" "{
  \"tag_name\": \"${tag}\",
  \"assets\": [
    {
      \"name\": \"${asset_name}\",
      \"browser_download_url\": \"file://${asset_dir}/${asset_name}\"
    }
  ]
}
"
}

run_scenario_command() {
    if [ "${SYSTEM_TEST_ROLE}" = "server" ] && declare -f scenario_server_command >/dev/null 2>&1; then
        run_role_command "$(scenario_server_command)" scenario_server_stdin
        return
    fi
    if [ "${SYSTEM_TEST_ROLE}" = "client" ] && declare -f scenario_client_command >/dev/null 2>&1; then
        run_role_command "$(scenario_client_command)" scenario_client_stdin
        return
    fi

    run_role_command "$(scenario_command)" scenario_stdin
}

base_prepare() {
    write_base_plugins
    write_base_fake_binaries
    create_test_config
}

base_prepare_ensure_fixture() {
    write_ensure_fixture_plugins
    write_base_fake_binaries
    create_test_config
}
