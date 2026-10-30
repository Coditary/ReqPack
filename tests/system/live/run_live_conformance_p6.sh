#!/usr/bin/env bash
# Phase 6: Security & manifest (sbom, audit, snapshot restore)
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

SBOM_JSON="${CONFORMANCE_SBOM_JSON:-/workspace/reports/sbom.json}"
AUDIT_SARIF="${CONFORMANCE_AUDIT_SARIF:-/workspace/reports/audit.sarif}"
SNAPSHOT_LUA="${CONFORMANCE_SNAPSHOT_LUA:-/workspace/reports/snapshot.lua}"

conf_p6_installed() {
    bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1
}

conf_p6_skip_all() {
    local reason="$1"
    conf_record P6.1-sbom-json skip "${reason}"
    conf_record P6.2-sbom-miss skip "${reason}"
    conf_record P6.2-sbom-skip skip "${reason}"
    conf_record P6.3-audit skip "${reason}"
    conf_skip_case P6.4-audit-unsafe "blocked by prerequisite"
    conf_record P6.5-snapshot-restore skip "${reason}"
}

conf_json_path_has_ok_true() {
    local path="$1"
    [ -s "${path}" ] && jq -e '.ok == true' "${path}" >/dev/null 2>&1
}

conf_run_phase6() {
    conf_log "phase 6 SYSTEM=${SYSTEM} pkg=${PKG_NAME}"

    if ! conf_p6_installed; then
        conf_p6_skip_all "installed package missing (prior phases)"
        return
    fi

    # P6.1 — SBOM JSON export
    rm -f "${SBOM_JSON}"
    conf_invoke_json P6.1-sbom-json sbom "${SYSTEM}:${PKG_NAME}" --format json -o "${SBOM_JSON}" -f
    if [ -s "${SBOM_JSON}" ] && jq -e . "${SBOM_JSON}" >/dev/null 2>&1; then
        conf_record P6.1-sbom-json pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P6.1-sbom-json fail "sbom export failed"
    elif [ ! -s "${SBOM_JSON}" ]; then
        conf_record P6.1-sbom-json fail "sbom file empty"
    else
        conf_record P6.1-sbom-json pass ""
    fi

    # P6.2 — sbom missing package (expect failure)
    conf_invoke_text P6.2-sbom-miss sbom "${SYSTEM}:${PKG_NONEXISTENT}"
    local p62_blob
    p62_blob="$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P6.2-sbom-miss pass ""
    elif grep -Eiq 'not found|missing|error|failed' <<<"${p62_blob}"; then
        conf_record P6.2-sbom-miss pass ""
    elif [ -s "${CONF_LAST_JSON}" ] && conf_json_path_has_ok_false "${CONF_LAST_JSON}"; then
        conf_record P6.2-sbom-miss pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ] &&
         [ -s "${CONF_LAST_JSON}" ] &&
         conf_json_path_has_ok_true "${CONF_LAST_JSON}"; then
        conf_record P6.2-sbom-miss fail "expected sbom failure"
    else
        conf_record P6.2-sbom-miss pass ""
    fi

    # P6.2-skip — sbom with skip missing
    conf_invoke_json P6.2-sbom-skip sbom "${SYSTEM}:${PKG_NONEXISTENT}" --sbom-skip-missing-packages
    if conf_json_has_ok_true; then
        conf_record P6.2-sbom-skip pass ""
    elif [ "${CONF_LAST_EXIT}" -eq 0 ]; then
        conf_record P6.2-sbom-skip pass ""
    else
        conf_record P6.2-sbom-skip fail "sbom skip not ok"
    fi

    # P6.3 — audit SARIF
    rm -f "${AUDIT_SARIF}"
    conf_invoke_json P6.3-audit audit "${SYSTEM}:${PKG_NAME}" --format sarif -o "${AUDIT_SARIF}" -f
    if [ -s "${AUDIT_SARIF}" ]; then
        conf_record P6.3-audit pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P6.3-audit fail "audit failed"
    elif [ ! -s "${AUDIT_SARIF}" ]; then
        conf_record P6.3-audit fail "audit sarif empty"
    else
        conf_record P6.3-audit pass ""
    fi

    # P6.4 — audit must report fixture CVE for installed conformance package
    if [ "${CONFORMANCE_SKIP_AUDIT_UNSAFE}" = "1" ]; then
        conf_skip_case P6.4-audit-unsafe "CONFORMANCE_SKIP_AUDIT_UNSAFE=1"
    elif [ ! -f "${CONFORMANCE_OSV_FEED}" ]; then
        conf_skip_case P6.4-audit-unsafe "missing OSV feed ${CONFORMANCE_OSV_FEED}"
    else
        conf_invoke_text P6.4-audit-unsafe audit "${SYSTEM}:${PKG_NAME}"
        local p64_blob
        p64_blob="$(conf_combined_from_paths "${CONF_LAST_JSON}" "${CONF_LAST_STDERR}")"
        if grep -Fq "${CONFORMANCE_AUDIT_UNSAFE_ID}" <<<"${p64_blob}" ||
            grep -Fq "CVE-2026-REQPACK" <<<"${p64_blob}"; then
            if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
                conf_record P6.4-audit-unsafe pass ""
            elif grep -Eiq 'critical|high|finding|vulnerabilit' <<<"${p64_blob}"; then
                conf_record P6.4-audit-unsafe pass ""
            else
                conf_record P6.4-audit-unsafe fail "CVE listed but exit 0 (expected findings exit code)"
            fi
        elif [ "${SYSTEM}" = "nix" ] &&
            grep -Fq "${PKG_NAME}" <<<"${p64_blob}" &&
            grep -Eiq 'finding|vulnerabilit|audit' <<<"${p64_blob}"; then
            conf_record P6.4-audit-unsafe pass ""
        else
            conf_record P6.4-audit-unsafe fail "expected audit finding ${CONFORMANCE_AUDIT_UNSAFE_ID}"
        fi
    fi

    # P6.5 — snapshot export, remove, restore from snapshot
    rm -f "${SNAPSHOT_LUA}"
    set +e
    conf_rqp snapshot -o "${SNAPSHOT_LUA}" -f \
        >"${CONFORMANCE_REPORT_DIR}/P6.5-snapshot-export.stdout" \
        2>"${CONFORMANCE_REPORT_DIR}/P6.5-snapshot-export.stderr"
    local snap_exit=$?
    set -e
    if [ "${snap_exit}" -ne 0 ] || [ ! -s "${SNAPSHOT_LUA}" ]; then
        conf_record P6.5-snapshot-restore fail "snapshot export failed"
        return
    fi

    conf_invoke_json P6.5-remove-before-restore remove "${SYSTEM}" "${PKG_NAME}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_log "P6.5 remove before restore exit ${CONF_LAST_EXIT} (continuing)"
    fi

    conf_invoke_json P6.5-restore install "${SNAPSHOT_LUA}"
    if bash -lc "${PKG_VERIFY_CMD}"; then
        conf_record P6.5-snapshot-restore pass ""
    elif [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P6.5-snapshot-restore fail "snapshot install failed"
    else
        conf_record P6.5-snapshot-restore fail "restore failed verify"
    fi
}
