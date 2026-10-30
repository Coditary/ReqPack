#!/usr/bin/env bash
set -euo pipefail

CONFORMANCE_REPORT="${CONFORMANCE_REPORT:-/workspace/reports/conformance.json}"
CONFORMANCE_REPORT_DIR="$(dirname "${CONFORMANCE_REPORT}")"
mkdir -p "${CONFORMANCE_REPORT_DIR}"

RQP_BIN="${RQP_BIN:-/opt/reqpack/rqp}"
LIVE_CONFIG="${LIVE_CONFIG:-/workspace/config.lua}"
CONFORMANCE_PHASE="${CONFORMANCE_PHASE:-all}"
CONFORMANCE_SKIP_AUDIT_UNSAFE="${CONFORMANCE_SKIP_AUDIT_UNSAFE:-0}"
CONFORMANCE_AUDIT_UNSAFE_ID="${CONFORMANCE_AUDIT_UNSAFE_ID:-CVE-2026-REQPACK-CONFORMANCE}"
CONFORMANCE_OSV_FEED="${CONFORMANCE_OSV_FEED:-/workspace/live-fixtures/security/conformance-osv-feed.json}"

conf_passed=0
conf_failed=0
conf_skipped=0

conf_log() {
    printf '[conformance] %s\n' "$*" >&2
}

conf_record() {
    local id="$1"
    local status="$2"
    local detail="${3:-}"
    printf '{"id":"%s","status":"%s","detail":%s}\n' "${id}" "${status}" "$(jq -Rn --arg t "${detail}" '$t')" >>"${CONFORMANCE_REPORT}.ndjson"
    case "${status}" in
        pass) conf_passed=$((conf_passed + 1)) ;;
        fail) conf_failed=$((conf_failed + 1)) ;;
        skip) conf_skipped=$((conf_skipped + 1)) ;;
    esac
}

conf_require_tools() {
    command -v jq >/dev/null 2>&1 || {
        conf_log "jq is required in the conformance container image"
        exit 1
    }
}

conf_rqp() {
    "${RQP_BIN}" --config "${LIVE_CONFIG}" -y "$@"
}

conf_json_file() {
    local label="$1"
    shift
    local path="${CONFORMANCE_REPORT_DIR}/${label}.json"
    set +e
    conf_rqp "$@" --json >"${path}" 2>"${path}.stderr"
    local exit_code=$?
    set -e
    printf '%s\n' "${path}"
    return "${exit_code}"
}

conf_json_ok() {
    local path="$1"
    [ -s "${path}" ] && jq -e '.ok == true' "${path}" >/dev/null 2>&1
}

conf_json_items_count() {
    local path="$1"
    jq -r --arg system "${SYSTEM}" '.[$system] // .items[$system] // [] | length' "${path}" 2>/dev/null || echo 0
}

conf_json_contains_name() {
    local path="$1"
    local name="$2"
    jq -e --arg system "${SYSTEM}" --arg name "${name}" '
      (.[$system] // .items[$system] // []) | map(.name) | index($name) != null
    ' "${path}" >/dev/null 2>&1
}

conf_json_version_for_name() {
    local path="$1"
    local name="$2"
    jq -r --arg system "${SYSTEM}" --arg name "${name}" '
      (.[$system] // .items[$system] // []) | map(select(.name == $name)) | .[0].version // empty
    ' "${path}"
}

conf_dpkg_apt_update() {
    if [ "${SYSTEM}" != "dpkg" ]; then
        return 0
    fi
    if [ "${CONFORMANCE_SKIP_APT_UPDATE:-0}" = "1" ]; then
        return 0
    fi
    set +e
    apt-get update -qq \
        >"${CONFORMANCE_REPORT_DIR}/apt-update.stdout" \
        2>"${CONFORMANCE_REPORT_DIR}/apt-update.stderr"
    local apt_exit=$?
    set -e
    if [ "${apt_exit}" -ne 0 ]; then
        conf_log "apt-get update failed (exit ${apt_exit}); remote search/outdated may fail"
    fi
    return 0
}

conf_run_case() {
    local id="$1"
    shift
    if ! "$@"; then
        conf_record "${id}" fail "assertion failed"
        return 1
    fi
    conf_record "${id}" pass ""
    return 0
}

conf_skip_case() {
    local id="$1"
    local reason="$2"
    conf_record "${id}" skip "${reason}"
}

conf_finalize_report() {
    local phase_label="${CONFORMANCE_REPORT_PHASE:-}"
    if [ -z "${phase_label}" ]; then
        case "${CONFORMANCE_PHASE}" in
            p1) phase_label="P1" ;;
            p2) phase_label="P2" ;;
            p3) phase_label="P3" ;;
            p4) phase_label="P4" ;;
            p5) phase_label="P5" ;;
            p6) phase_label="P6" ;;
            p7) phase_label="P7" ;;
            all) phase_label="ALL" ;;
            *)
                phase_label="${CONFORMANCE_PHASE}"
                ;;
        esac
    fi
    {
        printf '{'
        printf '"phase":"%s",' "${phase_label}"
        printf '"system":"%s",' "${SYSTEM}"
        printf '"passed":%s,' "${conf_passed}"
        printf '"failed":%s,' "${conf_failed}"
        printf '"skipped":%s,' "${conf_skipped}"
        printf '"cases":['
        if [ -f "${CONFORMANCE_REPORT}.ndjson" ]; then
            paste -sd, "${CONFORMANCE_REPORT}.ndjson"
        fi
        printf ']}'
    } >"${CONFORMANCE_REPORT}"
    rm -f "${CONFORMANCE_REPORT}.ndjson"
}

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

conf_invoke_text() {
    local label="$1"
    shift
    CONF_LAST_JSON="${CONFORMANCE_REPORT_DIR}/${label}.out"
    CONF_LAST_STDERR="${CONF_LAST_JSON}.stderr"
    set +e
    conf_rqp "$@" >"${CONF_LAST_JSON}" 2>"${CONF_LAST_STDERR}"
    CONF_LAST_EXIT=$?
    set -e
}

CONF_LAST_EXIT=0
CONF_LAST_JSON=""
CONF_LAST_STDERR=""
