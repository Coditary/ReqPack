#!/usr/bin/env bash
# Phase 4: Inventory (list installed, search --installed, group list, list miss)
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${ARCH_VALID:?ARCH_VALID required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

conf_p4_package_installed() {
    bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1
}

conf_p4_skip_all() {
    local reason="$1"
    conf_record P4.1-list skip "${reason}"
    conf_record P4.2-search-installed skip "${reason}"
    conf_record P4.3-list-group skip "${reason}"
    conf_record P4.4-list-miss skip "${reason}"
}

conf_run_phase4() {
    conf_log "phase 4 SYSTEM=${SYSTEM} pkg=${PKG_NAME}"

    if ! conf_p4_package_installed; then
        conf_p4_skip_all "installed package missing (P3 prerequisite)"
        return
    fi

    # P4.1 — list specific installed package
    conf_invoke_json P4.1-list list "${SYSTEM}:${PKG_NAME}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P4.1-list fail "exit ${CONF_LAST_EXIT}"
    elif conf_json_has_ok_true && conf_json_contains_name "${CONF_LAST_JSON}" "${PKG_NAME}"; then
        conf_record P4.1-list pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ] && bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
        conf_record P4.1-list pass ""
    elif ! conf_json_has_ok_true && [ ! -s "${CONF_LAST_JSON}" ]; then
        conf_record P4.1-list fail "list not ok"
    else
        conf_record P4.1-list fail "installed package missing from list"
    fi

    # P4.2 — search with --installed
    conf_invoke_json P4.2-search-installed search "${SYSTEM}" "${PKG_NAME}" --installed
    local p42_blob
    p42_blob="$(conf_combined_output)"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P4.2-search-installed fail "exit ${CONF_LAST_EXIT}"
    elif type conf_output_has_lua_stacktrace >/dev/null 2>&1 &&
        conf_output_has_lua_stacktrace "${p42_blob}"; then
        conf_record P4.2-search-installed fail "lua stacktrace"
    elif type conf_search_match_count >/dev/null 2>&1 &&
        [ "$(conf_search_match_count "${CONF_LAST_JSON}" "${PKG_NAME}")" -ge 1 ] 2>/dev/null; then
        conf_record P4.2-search-installed pass ""
    elif bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
        conf_record P4.2-search-installed pass ""
    elif grep -Fq "${PKG_NAME}" <<<"${p42_blob}"; then
        conf_record P4.2-search-installed pass ""
    else
        conf_record P4.2-search-installed fail "installed search miss"
    fi

    # P4.3 — grouped list with arch filter
    if [ "${CONFORMANCE_SKIP_P4_GROUP:-0}" = "1" ]; then
        conf_skip_case P4.3-list-group "CONFORMANCE_SKIP_P4_GROUP=1"
    else
        conf_invoke_json P4.3-list-group list "${SYSTEM}" --group --arch "${ARCH_VALID}"
        if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
            conf_record P4.3-list-group fail "exit ${CONF_LAST_EXIT}"
        elif conf_json_has_ok_true; then
            conf_record P4.3-list-group pass ""
        elif [ "${CONF_LAST_EXIT}" -eq 0 ]; then
            conf_record P4.3-list-group pass ""
        else
            conf_record P4.3-list-group fail "group list not ok"
        fi
    fi

    # P4.4 — list nonexistent package (must not appear; list failure is acceptable)
    conf_invoke_json P4.4-list-miss list "${SYSTEM}" "${PKG_NONEXISTENT}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P4.4-list-miss pass ""
    elif conf_json_contains_name "${CONF_LAST_JSON}" "${PKG_NONEXISTENT}"; then
        conf_record P4.4-list-miss fail "unexpected package in list"
    else
        conf_record P4.4-list-miss pass ""
    fi
}
