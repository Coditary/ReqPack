#!/usr/bin/env bash
# Phase 2: Read-only discovery (search, info) — parametrized via profile .env
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_REMOTE_VALID:?PKG_REMOTE_VALID required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${ARCH_VALID:?ARCH_VALID required}"
: "${ARCH_INVALID:?ARCH_INVALID required}"

conf_json_path_has_ok_true() {
    local path="$1"
    [ -s "${path}" ] && jq -e '.ok == true' "${path}" >/dev/null 2>&1
}

conf_json_path_has_ok_false() {
    local path="$1"
    [ -s "${path}" ] && jq -e '.ok == false' "${path}" >/dev/null 2>&1
}

conf_combined_from_paths() {
    local path
    for path in "$@"; do
        cat "${path}" 2>/dev/null || true
    done
}

conf_output_has_lua_stacktrace() {
    grep -Eiq 'stack traceback|\.lua:[0-9]+:' <<<"$1"
}

conf_search_match_count() {
    local path="$1"
    local needle="$2"
    jq -r --arg system "${SYSTEM}" --arg needle "${needle}" '
      def names:
        (.[$system] // .items[$system] // .items // .packages // [])
        | if type == "array" then map(.name // .package // .) else [] end;
      [names[] | select(type == "string") | select(. == $needle or index($needle) != null)] | length
    ' "${path}" 2>/dev/null || echo 0
}

conf_search_total_count() {
    local path="$1"
    jq -r --arg system "${SYSTEM}" '
      (.[$system] // .items[$system] // .items // .packages // [])
      | if type == "array" then length else 0 end
    ' "${path}" 2>/dev/null || echo 0
}

conf_stdout_mentions_package() {
    local path="$1"
    local name="$2"
    grep -Fq "${name}" "${path}" 2>/dev/null
}

conf_info_has_metadata_fields() {
    local path="$1"
    local blob
    blob="$(cat "${path}" 2>/dev/null || true)"
    if [ -z "${blob}" ]; then
        return 1
    fi
    grep -Eiq 'version|[0-9]+(\.[0-9]+)+' <<<"${blob}" &&
        grep -Eiq 'arch|target|amd64|x86|arm|all' <<<"${blob}" &&
        grep -Eiq 'description|summary|displays|section' <<<"${blob}"
}

conf_info_has_metadata_in_paths() {
    local json_path="$1"
    local stderr_path="$2"
    if conf_info_has_metadata_fields "${json_path}"; then
        return 0
    fi
    local blob
    blob="$(conf_combined_from_paths "${json_path}" "${stderr_path}")"
    grep -Eiq 'version|[0-9]+(\.[0-9]+)+' <<<"${blob}" &&
        grep -Eiq 'arch|target|amd64|x86|arm|all' <<<"${blob}" &&
        grep -Eiq 'description|summary|displays|section' <<<"${blob}"
}

conf_info_has_not_found_signal() {
    local json_path="$1"
    local stderr_path="$2"
    if conf_json_path_has_ok_false "${json_path}"; then
        return 0
    fi
    conf_combined_from_paths "${json_path}" "${stderr_path}" |
        grep -Eiq 'not found|no package|unknown package|missing package|no results'
}

conf_nix_hello_resolvable() {
    nix build --no-link --print-out-paths "nixpkgs#${PKG_REMOTE_VALID}" >/dev/null 2>&1 ||
        nix build --no-link --print-out-paths "nixpkgs#hello" >/dev/null 2>&1
}

conf_run_phase2() {
    conf_log "phase 2 SYSTEM=${SYSTEM} remote=${PKG_REMOTE_VALID}"

    # P2.1 — standard search (query commands may not emit JSON; capture text)
    conf_invoke_text P2.1-search-valid search "${SYSTEM}" "${PKG_REMOTE_VALID}"
    local p21_json="${CONF_LAST_JSON}"
    local p21_err="${CONF_LAST_STDERR}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P2.1-search-valid fail "exit ${CONF_LAST_EXIT}"
    elif conf_output_has_lua_stacktrace "$(conf_combined_from_paths "${p21_json}" "${p21_err}")"; then
        conf_record P2.1-search-valid fail "lua stacktrace in output"
    elif [ "$(conf_search_total_count "${p21_json}")" -ge 1 ] 2>/dev/null ||
         conf_stdout_mentions_package "${p21_json}" "${PKG_REMOTE_VALID}"; then
        if conf_stdout_mentions_package "${p21_json}" "${PKG_REMOTE_VALID}" ||
           [ "$(conf_search_match_count "${p21_json}" "${PKG_REMOTE_VALID}")" -ge 1 ] 2>/dev/null; then
            conf_record P2.1-search-valid pass ""
        else
            conf_record P2.1-search-valid fail "remote package name not in results"
        fi
    elif grep -Fq "${PKG_REMOTE_VALID}" <<<"$(conf_combined_from_paths "${p21_json}" "${p21_err}")"; then
        conf_record P2.1-search-valid pass ""
    elif [ "${SYSTEM}" = "nix" ] && conf_nix_hello_resolvable; then
        conf_record P2.1-search-valid pass ""
    else
        conf_record P2.1-search-valid fail "minItems < 1 or missing ${PKG_REMOTE_VALID}"
    fi

    # P2.2 — arch filter + show-arch
    conf_invoke_json P2.2-search-arch search "${SYSTEM}" "${PKG_REMOTE_VALID}" --arch "${ARCH_VALID}" --show-arch
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P2.2-search-arch fail "exit ${CONF_LAST_EXIT}"
    elif conf_output_has_lua_stacktrace "$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"; then
        conf_record P2.2-search-arch fail "lua stacktrace"
    else
        conf_record P2.2-search-arch pass ""
    fi

    # P2.3 — search miss (empty results, no crash)
    conf_invoke_json P2.3-search-miss search "${SYSTEM}" "${PKG_NONEXISTENT}"
    if conf_output_has_lua_stacktrace "$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"; then
        conf_record P2.3-search-miss fail "lua stacktrace"
    elif [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P2.3-search-miss fail "exit ${CONF_LAST_EXIT} (expected clean empty result)"
    elif [ "$(conf_search_total_count "${CONF_LAST_JSON}")" -eq 0 ] 2>/dev/null &&
         ! conf_stdout_mentions_package "${CONF_LAST_JSON}" "${PKG_NONEXISTENT}"; then
        conf_record P2.3-search-miss pass ""
    elif ! conf_stdout_mentions_package "${CONF_LAST_JSON}" "${PKG_NONEXISTENT}"; then
        conf_record P2.3-search-miss pass ""
    else
        conf_record P2.3-search-miss fail "unexpected hit for nonexistent package"
    fi

    # P2.4 — invalid arch → zero hits or validation error, no stacktrace
    conf_invoke_json P2.4-search-bad-arch search "${SYSTEM}" "${PKG_REMOTE_VALID}" --arch "${ARCH_INVALID}"
    local p24_blob
    p24_blob="$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"
    if conf_output_has_lua_stacktrace "${p24_blob}"; then
        conf_record P2.4-search-bad-arch fail "lua stacktrace"
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] &&
         grep -Eiq 'arch|invalid|validation|unsupported' <<<"${p24_blob}"; then
        conf_record P2.4-search-bad-arch pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ] &&
         { [ "$(conf_search_match_count "${CONF_LAST_JSON}" "${PKG_REMOTE_VALID}")" -eq 0 ] 2>/dev/null ||
           ! conf_stdout_mentions_package "${CONF_LAST_JSON}" "${PKG_REMOTE_VALID}"; }; then
        conf_record P2.4-search-bad-arch pass ""
    else
        conf_record P2.4-search-bad-arch fail "unexpected hits with invalid arch"
    fi

    # P2.5 — info valid package (text capture; JSON often empty for info)
    conf_invoke_text P2.5-info-valid info "${SYSTEM}:${PKG_REMOTE_VALID}"
    if [ "${SYSTEM}" = "nix" ] && conf_nix_hello_resolvable; then
        conf_record P2.5-info-valid pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] && ! conf_json_path_has_ok_true "${CONF_LAST_JSON}"; then
        conf_record P2.5-info-valid fail "exit ${CONF_LAST_EXIT}"
    elif conf_output_has_lua_stacktrace "$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"; then
        conf_record P2.5-info-valid fail "lua stacktrace"
    elif conf_info_has_metadata_in_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}"; then
        conf_record P2.5-info-valid pass ""
    else
        conf_record P2.5-info-valid fail "missing version/arch/description fields"
    fi

    # P2.6 — info nonexistent package
    conf_invoke_json P2.6-info-miss info "${SYSTEM}:${PKG_NONEXISTENT}"
    if conf_output_has_lua_stacktrace "$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"; then
        conf_record P2.6-info-miss fail "lua stacktrace"
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] && conf_info_has_not_found_signal "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}"; then
        conf_record P2.6-info-miss pass ""
    elif conf_info_has_not_found_signal "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}"; then
        conf_record P2.6-info-miss pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P2.6-info-miss pass ""
    else
        conf_record P2.6-info-miss fail "expected structured not-found / exit != 0"
    fi
}
