#!/usr/bin/env bash
# Phase 1 only: Plugin bootstrap (.rqp install) and system readiness (plugin info, host, ensure).
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PLUGIN_ID:=${SYSTEM}}"
: "${PKG_RQP_ARTIFACT:?PKG_RQP_ARTIFACT required}"

CONF_LAST_EXIT=0
CONF_LAST_JSON=""
CONF_LAST_STDERR=""

conf_invoke_json() {
    local label="$1"
    shift
    CONF_LAST_JSON="${CONFORMANCE_REPORT_DIR}/${label}.json"
    CONF_LAST_STDERR="${CONF_LAST_JSON}.stderr"
    set +e
    conf_rqp "$@" --json >"${CONF_LAST_JSON}" 2>"${CONF_LAST_STDERR}"
    CONF_LAST_EXIT=$?
    set -e
}

conf_json_has_ok_true() {
    [ -s "${CONF_LAST_JSON}" ] && jq -e '.ok == true' "${CONF_LAST_JSON}" >/dev/null 2>&1
}

conf_json_has_ok_false() {
    [ -s "${CONF_LAST_JSON}" ] && jq -e '.ok == false' "${CONF_LAST_JSON}" >/dev/null 2>&1
}

conf_combined_output() {
    cat "${CONF_LAST_JSON}" 2>/dev/null || true
    cat "${CONF_LAST_STDERR}" 2>/dev/null || true
}

conf_text_matches_error_hint() {
    conf_combined_output | grep -Eiq 'unknown|not found|missing|plugin|system|unregistered|failed'
}

conf_plugin_materialized() {
    local plugin_root="${CONF_PLUGIN_DIR:-/workspace/plugins}"
    local candidates=(
        "${plugin_root}/${PLUGIN_ID}/run.lua"
        "${plugin_root}/${SYSTEM}/run.lua"
        "${plugin_root}/${PLUGIN_ID}/bundle/run.lua"
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        if [ -f "${candidate}" ]; then
            return 0
        fi
    done
    return 1
}

conf_run_phase1() {
    conf_log "phase 1 SYSTEM=${SYSTEM} PLUGIN_ID=${PLUGIN_ID}"

    mkdir -p "${CONF_PLUGIN_DIR:-/workspace/plugins}"

    # Host metadata must be fresh before installing .rqp packages (debian → dpkg mapping).
    set +e
    conf_rqp host refresh >"${CONFORMANCE_REPORT_DIR}/P1.0-host-refresh-pre.json.stdout" 2>"${CONFORMANCE_REPORT_DIR}/P1.0-host-refresh-pre.json.stderr"
    local pre_host_exit=$?
    set -e
    if [ "${pre_host_exit}" -ne 0 ]; then
        conf_record P1.1-install-rqp fail "host refresh before install exit ${pre_host_exit}"
        conf_record P1.2-plugin-info skip "host refresh failed"
        conf_record P1.3-host-refresh skip "host refresh failed"
        conf_record P1.3-ensure-dry skip "host refresh failed"
        conf_record P1.3-ensure skip "host refresh failed"
        conf_record P1.4-unknown-system skip "host refresh failed"
        return
    fi

    conf_dpkg_apt_update

    if [ "${SYSTEM}" = "nix" ] && [ "${NIX_CONFORMANCE_SKIP_PREWARM:-0}" != "1" ]; then
        set +e
        nix flake metadata nixpkgs \
            >"${CONFORMANCE_REPORT_DIR}/P1.0-nix-flake.stdout" \
            2>"${CONFORMANCE_REPORT_DIR}/P1.0-nix-flake.stderr"
        set -e
    fi

    # P1.1 — install .rqp plugin package
    conf_invoke_json P1.1-install-rqp install rqp "${PKG_RQP_ARTIFACT}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P1.1-install-rqp fail "exit ${CONF_LAST_EXIT}"
    elif ! conf_json_has_ok_true; then
        conf_record P1.1-install-rqp fail "json ok!=true"
    else
        conf_record P1.1-install-rqp pass ""
    fi

    # P1.2 — plugin wrapper metadata via `plugin info <system>`
    conf_invoke_json P1.2-plugin-info plugin info "${SYSTEM}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P1.2-plugin-info fail "exit ${CONF_LAST_EXIT}"
    elif [ -s "${CONF_LAST_JSON}" ] && jq -e . "${CONF_LAST_JSON}" >/dev/null 2>&1 &&
        { jq -e --arg id "${PLUGIN_ID}" --arg sys "${SYSTEM}" '
        (.name // .packageName // .items[0].name // empty) as $n |
        ($n == $id or $n == $sys or (.system // .items[0].system // empty) == $sys)
      ' "${CONF_LAST_JSON}" >/dev/null 2>&1 ||
          grep -Fq "${PLUGIN_ID}" "${CONF_LAST_JSON}" ||
          grep -Fq "${SYSTEM}" "${CONF_LAST_JSON}"; }; then
        conf_record P1.2-plugin-info pass ""
    else
        conf_invoke_text P1.2-plugin-info-text plugin info "${SYSTEM}"
        local p12_blob
        p12_blob="$(conf_combined_output)"
        if grep -Fq "${PLUGIN_ID}" <<<"${p12_blob}" ||
            grep -Fq "${SYSTEM}" <<<"${p12_blob}" ||
            grep -Eiq 'package manager|plugin' <<<"${p12_blob}"; then
            conf_record P1.2-plugin-info pass ""
        else
            conf_record P1.2-plugin-info fail "missing wrapper metadata"
        fi
    fi

    # P1.3 — host refresh + ensure (dry-run then apply)
    set +e
    conf_rqp host refresh >"${CONFORMANCE_REPORT_DIR}/P1.3-host-refresh.stdout" 2>"${CONFORMANCE_REPORT_DIR}/P1.3-host-refresh.stderr"
    local host_exit=$?
    set -e
    if [ "${host_exit}" -ne 0 ]; then
        conf_record P1.3-host-refresh fail "host refresh exit ${host_exit}"
    else
        conf_record P1.3-host-refresh pass ""
    fi

    conf_invoke_json P1.3-ensure-dry ensure "${SYSTEM}" --dry-run
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P1.3-ensure-dry fail "ensure dry-run failed"
    elif ! jq -e '.dryRun == true' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        conf_record P1.3-ensure-dry fail "json dryRun!=true"
    else
        conf_record P1.3-ensure-dry pass ""
    fi

    conf_invoke_json P1.3-ensure ensure "${SYSTEM}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P1.3-ensure fail "ensure failed"
    else
        conf_record P1.3-ensure pass ""
    fi

    if [ "${SYSTEM}" = "nix" ] && [ "${NIX_CONFORMANCE_REQUIRES_PLUGIN_BOOTSTRAP:-0}" = "1" ]; then
        if ! command -v nix >/dev/null 2>&1 &&
            ! test -x "${HOME}/.nix-profile/bin/nix" &&
            ! test -x "/nix/var/nix/profiles/default/bin/nix"; then
            conf_record P1.5-nix-cli fail "nix missing after ensure (plugin bootstrap did not install Nix)"
        else
            conf_record P1.5-nix-cli pass ""
        fi
        conf_invoke_text P1.6-rqp-search search "${SYSTEM}" hello
        if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
            conf_record P1.6-rqp-search fail "search exit ${CONF_LAST_EXIT}"
        elif grep -Fq "hello" <<<"$(conf_combined_output)"; then
            conf_record P1.6-rqp-search pass ""
        else
            conf_record P1.6-rqp-search fail "rqp search nix hello returned no hello results"
        fi
    fi

    # P1.4 — unknown system must fail with readable error
    conf_invoke_json P1.4-unknown-system ensure unknown-pm-xyz
    if [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        conf_record P1.4-unknown-system fail "expected failure for unknown system"
    elif conf_json_has_ok_false; then
        conf_record P1.4-unknown-system pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] && conf_text_matches_error_hint; then
        conf_record P1.4-unknown-system pass ""
    elif [ -s "${CONF_LAST_JSON}" ] && ! conf_json_has_ok_false; then
        conf_record P1.4-unknown-system fail "expected json ok:false"
    elif ! conf_text_matches_error_hint; then
        conf_record P1.4-unknown-system fail "missing error hint in output"
    else
        conf_record P1.4-unknown-system pass ""
    fi
}
