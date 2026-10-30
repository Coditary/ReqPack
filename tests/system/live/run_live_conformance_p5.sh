#!/usr/bin/env bash
# Phase 5: Update (outdated, install v2, update miss)
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

conf_p5_installed() {
    bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1
}

conf_p5_skip_all() {
    local reason="$1"
    conf_record P5.1-outdated skip "${reason}"
    conf_record P5.2-update-v2 skip "${reason}"
    conf_record P5.3-update-miss skip "${reason}"
}

conf_p5_local_v2_ready() {
    [ -n "${PKG_LOCAL_V2:-}" ] && [ -f "${PKG_LOCAL_V2}" ]
}

conf_run_phase5() {
    conf_log "phase 5 SYSTEM=${SYSTEM} pkg=${PKG_NAME}"

    if ! conf_p5_installed; then
        conf_p5_skip_all "installed package missing (P3/P4 prerequisite)"
        return
    fi

    # P5.1 — outdated query
    conf_invoke_json P5.1-outdated outdated "${SYSTEM}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P5.1-outdated fail "exit ${CONF_LAST_EXIT}"
    elif conf_json_has_ok_true; then
        conf_record P5.1-outdated pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ]; then
        conf_record P5.1-outdated pass ""
    else
        conf_record P5.1-outdated fail "outdated not ok"
    fi

    # P5.2 — install local v2 (update path)
    if [ "${CONFORMANCE_SKIP_P5_UPDATE_V2:-0}" = "1" ]; then
        conf_skip_case P5.2-update-v2 "CONFORMANCE_SKIP_P5_UPDATE_V2=1"
    elif ! conf_p5_local_v2_ready; then
        conf_skip_case P5.2-update-v2 "PKG_LOCAL_V2 not available"
    else
        conf_invoke_json P5.2-update-v2 install "${SYSTEM}" "${PKG_LOCAL_V2}"
        if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
            conf_record P5.2-update-v2 fail "install v2 failed"
        elif ! bash -lc "${PKG_VERIFY_CMD}"; then
            conf_record P5.2-update-v2 fail "verify after v2 failed"
        else
            if [ -n "${PKG_VERSION_V2:-}" ] && [ -n "${PKG_VERIFY_VERSION_CMD:-}" ]; then
                if bash -lc "${PKG_VERIFY_VERSION_CMD}"; then
                    conf_record P5.2-update-v2 pass ""
                else
                    conf_record P5.2-update-v2 fail "version not ${PKG_VERSION_V2} after v2 install"
                fi
            else
                conf_record P5.2-update-v2 pass ""
            fi
        fi
    fi

    # P5.3 — update nonexistent package
    conf_invoke_json P5.3-update-miss update "${SYSTEM}" "${PKG_NONEXISTENT}"
    if [ "${SYSTEM}" = "nix" ] &&
        jq -e --arg n "${PKG_NONEXISTENT}" '.nix[]? | select(.name == $n and .status == "updated")' "${CONF_LAST_JSON}" >/dev/null 2>&1 &&
        ! nix profile list 2>/dev/null | grep -Fq "${PKG_NONEXISTENT}"; then
        conf_record P5.3-update-miss pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        conf_record P5.3-update-miss fail "expected update failure"
    elif conf_json_has_ok_false || [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P5.3-update-miss pass ""
    else
        conf_record P5.3-update-miss fail "missing error signal"
    fi
}
