#!/usr/bin/env bash
# Phase 3: Installation & error tolerance (install positive/negative)
set -euo pipefail

: "${SYSTEM:?SYSTEM required}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_LOCAL_V1:?PKG_LOCAL_V1 required}"
: "${PKG_LOCAL_CORRUPT:?PKG_LOCAL_CORRUPT required}"
: "${PKG_REMOTE_VALID:?PKG_REMOTE_VALID required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${PKG_BAD_VERSION:?PKG_BAD_VERSION required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

CONF_P3_LOCAL_V1="${PKG_LOCAL_V1}"

conf_p3_local_ready() {
    [ -e "${CONF_P3_LOCAL_V1}" ] || [ -d "${CONF_P3_LOCAL_V1}" ]
}

conf_p3_prepare_local_v1() {
    CONF_P3_LOCAL_V1="${PKG_LOCAL_V1}"
    if [ "${SYSTEM}" = "nix" ]; then
        if conf_p3_local_ready; then
            return 0
        fi
        local build_out="${CONFORMANCE_REPORT_DIR}/P3.0-nix-build.stdout"
        local build_err="${CONFORMANCE_REPORT_DIR}/P3.0-nix-build.stderr"
        set +e
        nix build --no-link --print-out-paths "nixpkgs#${PKG_NAME}" >"${build_out}" 2>"${build_err}"
        local build_exit=$?
        set -e
        if [ "${build_exit}" -ne 0 ]; then
            conf_log "nix build bootstrap failed for nixpkgs#${PKG_NAME}"
            return 1
        fi
        CONF_P3_LOCAL_V1="$(tail -1 "${build_out}")"
        if [ -z "${CONF_P3_LOCAL_V1}" ] || [ ! -e "${CONF_P3_LOCAL_V1}" ]; then
            return 1
        fi
        mkdir -p "$(dirname "${PKG_LOCAL_V1}")"
        ln -sfn "${CONF_P3_LOCAL_V1}" "${PKG_LOCAL_V1}" 2>/dev/null || true
    fi
    conf_p3_local_ready
}

conf_json_has_skipped_item() {
    local path="$1"
    jq -e '
      [.. | objects | select(.status? == "skipped") ] | length > 0
    ' "${path}" >/dev/null 2>&1
}

conf_pkg_installed() {
    if [ "${SYSTEM}" = "dpkg" ]; then
        dpkg-query -W -f='${Status}' "${PKG_NAME}" 2>/dev/null | grep -q 'install ok installed'
        return
    fi
    if [ "${SYSTEM}" = "nix" ]; then
        if bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1; then
            return 0
        fi
        nix profile list 2>/dev/null | grep -Fq "${PKG_NAME}"
        return
    fi
    bash -lc "${PKG_VERIFY_CMD}" >/dev/null 2>&1
}

conf_run_phase3() {
    conf_log "phase 3 SYSTEM=${SYSTEM} pkg=${PKG_NAME}"

    if ! conf_p3_prepare_local_v1; then
        conf_record P3.1-dry-run skip "local fixture unavailable (${PKG_LOCAL_V1})"
        conf_record P3.2-install-v1 skip "local fixture unavailable"
        conf_record P3.3-idempotent skip "local fixture unavailable"
        conf_record P3.4-install-miss skip "blocked by missing local bootstrap"
        conf_record P3.5-bad-version skip "blocked by missing local bootstrap"
        conf_record P3.6-corrupt skip "blocked by missing local bootstrap"
        conf_record P3.7-mixed-targets skip "blocked by missing local bootstrap"
        return
    fi

    # P3.1 — dry-run must not install
    conf_invoke_json P3.1-dry-run install "${SYSTEM}" "${CONF_P3_LOCAL_V1}" --dry-run
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P3.1-dry-run fail "dry-run failed"
    elif ! jq -e '.dryRun == true' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        conf_record P3.1-dry-run fail "dry-run json missing dryRun:true"
    elif conf_pkg_installed; then
        conf_record P3.1-dry-run fail "package installed after dry-run"
    elif jq -e '.[]? | .[]? | select(.status? == "installed")' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        conf_record P3.1-dry-run fail "dry-run reported installed status"
    else
        conf_record P3.1-dry-run pass ""
    fi

    # P3.2 — local install v1 + verify command
    conf_invoke_json P3.2-install-v1 install "${SYSTEM}" "${CONF_P3_LOCAL_V1}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ] || ! conf_json_has_ok_true; then
        conf_record P3.2-install-v1 fail "install local v1 failed"
    elif ! bash -lc "${PKG_VERIFY_CMD}"; then
        conf_record P3.2-install-v1 fail "verify command failed"
    else
        conf_record P3.2-install-v1 pass ""
    fi

    # P3.3 — idempotent install by package name
    conf_invoke_json P3.3-idempotent install "${SYSTEM}" "${PKG_NAME}"
    if [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P3.3-idempotent fail "exit ${CONF_LAST_EXIT}"
    elif ! conf_json_has_ok_true; then
        conf_record P3.3-idempotent fail "json ok!=true"
    elif conf_json_has_skipped_item "${CONF_LAST_JSON}"; then
        conf_record P3.3-idempotent pass ""
    elif jq -e '.ok == true' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        conf_record P3.3-idempotent pass ""
    else
        conf_record P3.3-idempotent fail "expected skip or ok for already installed"
    fi

    # P3.4 — nonexistent package
    conf_invoke_json P3.4-install-miss install "${SYSTEM}" "${PKG_NONEXISTENT}"
    if [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        conf_record P3.4-install-miss fail "expected failure"
    elif conf_json_has_ok_false || [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P3.4-install-miss pass ""
    else
        conf_record P3.4-install-miss fail "missing error signal"
    fi

    # P3.5 — manifest bad version + --on-unresolved abort
    local manifest_dir="/workspace/conformance-manifest"
    mkdir -p "${manifest_dir}"
    cat >"${manifest_dir}/reqpack.lua" <<EOF
return {
  packages = {
    { system = "${SYSTEM}", name = "${PKG_REMOTE_VALID}", version = "${PKG_BAD_VERSION}" },
  },
}
EOF
    conf_invoke_json P3.5-bad-version install "${manifest_dir}" --on-unresolved abort
    if [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        if jq -e --arg ver "${PKG_BAD_VERSION}" '
            [.. | objects | select(.version? == $ver and (.status? == "skipped" or .status? == "failed"))] | length > 0
          ' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
            conf_record P3.5-bad-version pass ""
        else
            conf_record P3.5-bad-version fail "expected unresolved version failure"
        fi
    elif conf_json_has_ok_false || [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P3.5-bad-version pass ""
    elif jq -e --arg ver "${PKG_BAD_VERSION}" '
        [.. | objects | select(.version? == $ver and (.status? == "skipped" or .status? == "failed"))] | length > 0
      ' "${CONF_LAST_JSON}" >/dev/null 2>&1; then
        conf_record P3.5-bad-version pass ""
    else
        conf_record P3.5-bad-version fail "missing failure for bad version"
    fi

    # P3.6 — corrupt local artifact
    conf_invoke_json P3.6-corrupt install "${SYSTEM}" "${PKG_LOCAL_CORRUPT}"
    if [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        conf_record P3.6-corrupt fail "expected corrupt install failure"
    elif conf_json_has_ok_false || [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P3.6-corrupt pass ""
    else
        conf_record P3.6-corrupt fail "missing corrupt install error"
    fi

    # P3.7 — mixed local path + remote name
    conf_invoke_json P3.7-mixed-targets install "${SYSTEM}" "${CONF_P3_LOCAL_V1}" "${PKG_REMOTE_VALID}"
    if [ "${CONF_LAST_EXIT}" -eq 0 ] && conf_json_has_ok_true; then
        conf_record P3.7-mixed-targets fail "expected mixed target rejection"
    elif conf_json_has_ok_false || [ "${CONF_LAST_EXIT}" -ne 0 ]; then
        conf_record P3.7-mixed-targets pass ""
    else
        conf_record P3.7-mixed-targets fail "missing mixed-target error"
    fi
}
