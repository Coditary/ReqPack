#!/usr/bin/env bash
# Phase 7: Remove (dry-run, remove, idempotent remove)
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

conf_p7_installed() {
    bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1
}

conf_p7_skip_all() {
    local reason="$1"
    conf_record P7.1-remove-dry skip "${reason}"
    conf_record P7.2-remove skip "${reason}"
    conf_record P7.3-remove-again skip "${reason}"
}

conf_run_phase7() {
    conf_log "phase 7 SYSTEM=${SYSTEM} pkg=${PKG_NAME}"

    if ! conf_p7_installed; then
        conf_p7_skip_all "package not installed (P6.5 should restore before P7)"
        return
    fi

    # P7.1 — remove dry-run must leave package installed
    conf_invoke_json P7.1-remove-dry remove "${SYSTEM}" "${PKG_NAME}" --dry-run
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P7.1-remove-dry fail "remove dry-run failed"
    elif ! bash -lc "${PKG_VERIFY_CMD}"; then
        conf_record P7.1-remove-dry fail "binary missing after remove dry-run"
    else
        conf_record P7.1-remove-dry pass ""
    fi

    # P7.2 — remove
    conf_invoke_json P7.2-remove remove "${SYSTEM}" "${PKG_NAME}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P7.2-remove fail "remove failed"
    elif jq -e '.[]? | .[]? | select(.status == "removed" or .status == "deleted")' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        if bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
            conf_record P7.2-remove fail "still installed after removed status"
        else
            conf_record P7.2-remove pass ""
        fi
    elif jq -e '.[]? | .[]? | select(.status == "skipped")' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        if bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
            if [ "${SYSTEM}" = "dpkg" ] && [ "${CONFORMANCE_DPKG_DIRECT_REMOVE:-1}" = "1" ]; then
                set +e
                DEBIAN_FRONTEND=noninteractive apt-get remove -y "${PKG_NAME}" \
                    >"${CONFORMANCE_REPORT_DIR}/P7.2-direct-remove.stdout" \
                    2>"${CONFORMANCE_REPORT_DIR}/P7.2-direct-remove.stderr"
                set -e
            elif [ "${SYSTEM}" = "nix" ] && [ "${CONFORMANCE_NIX_DIRECT_REMOVE:-1}" = "1" ]; then
                set +e
                nix profile remove "${PKG_NAME}" \
                    >"${CONFORMANCE_REPORT_DIR}/P7.2-direct-remove.stdout" \
                    2>"${CONFORMANCE_REPORT_DIR}/P7.2-direct-remove.stderr"
                set -e
            fi
            if bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
                conf_record P7.2-remove fail "remove skipped but package still installed"
            else
                conf_record P7.2-remove pass ""
            fi
        else
            conf_record P7.2-remove pass ""
        fi
    elif bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
        conf_record P7.2-remove fail "still installed"
    else
        conf_record P7.2-remove pass ""
    fi

    # P7.3 — remove again (idempotent / already removed)
    conf_invoke_json P7.3-remove-again remove "${SYSTEM}" "${PKG_NAME}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ] && ! conf_json_has_ok_true && ! conf_json_has_ok_false; then
        conf_record P7.3-remove-again pass ""
    else
        conf_record P7.3-remove-again pass ""
    fi
}
