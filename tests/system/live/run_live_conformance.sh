#!/usr/bin/env bash
# ReqPack Plugin Conformance Suite — CLI E2E (7 phases, parametrized via profile .env)
set -euo pipefail

profile="${1:?usage: run_live_conformance.sh /path/to/profile.env}"
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=lib/conformance_common.sh
source "${script_dir}/lib/conformance_common.sh"

# shellcheck disable=SC1090
source "${profile}"

CONF_PLUGIN_DIR="${CONF_PLUGIN_DIR:-/workspace/plugins}"

: "${SYSTEM:?SYSTEM required in profile}"
: "${PLUGIN_ID:=${SYSTEM}}"
: "${PKG_NAME:?PKG_NAME required}"
: "${PKG_LOCAL_V1:?PKG_LOCAL_V1 required}"
: "${PKG_LOCAL_CORRUPT:?PKG_LOCAL_CORRUPT required}"
: "${PKG_BAD_VERSION:?PKG_BAD_VERSION required}"
: "${PKG_REMOTE_VALID:?PKG_REMOTE_VALID required}"
: "${PKG_NONEXISTENT:?PKG_NONEXISTENT required}"
: "${ARCH_VALID:?ARCH_VALID required}"
: "${ARCH_INVALID:?ARCH_INVALID required}"
: "${PKG_VERIFY_CMD:?PKG_VERIFY_CMD required}"

conf_require_tools
rm -f "${CONFORMANCE_REPORT}" "${CONFORMANCE_REPORT}.ndjson"
touch "${CONFORMANCE_REPORT}.ndjson"

conf_log "conformance profile SYSTEM=${SYSTEM} PLUGIN_ID=${PLUGIN_ID} phase=${CONFORMANCE_PHASE}"

set +e

# shellcheck source=run_live_conformance_p1.sh
source "${script_dir}/run_live_conformance_p1.sh"
conf_run_phase1

if [ "${CONFORMANCE_PHASE}" = "p1" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P1
    conf_finalize_report
    conf_log "conformance phase P1 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p2.sh
source "${script_dir}/run_live_conformance_p2.sh"
conf_run_phase2

if [ "${CONFORMANCE_PHASE}" = "p2" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P2
    conf_finalize_report
    conf_log "conformance phase P2 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p3.sh
source "${script_dir}/run_live_conformance_p3.sh"
conf_run_phase3

if [ "${CONFORMANCE_PHASE}" = "p3" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P3
    conf_finalize_report
    conf_log "conformance phase P3 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p4.sh
source "${script_dir}/run_live_conformance_p4.sh"
conf_run_phase4

if [ "${CONFORMANCE_PHASE}" = "p4" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P4
    conf_finalize_report
    conf_log "conformance phase P4 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p5.sh
source "${script_dir}/run_live_conformance_p5.sh"
conf_run_phase5

if [ "${CONFORMANCE_PHASE}" = "p5" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P5
    conf_finalize_report
    conf_log "conformance phase P5 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p6.sh
source "${script_dir}/run_live_conformance_p6.sh"
conf_run_phase6

if [ "${CONFORMANCE_PHASE}" = "p6" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P6
    conf_finalize_report
    conf_log "conformance phase P6 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

# shellcheck source=run_live_conformance_p7.sh
source "${script_dir}/run_live_conformance_p7.sh"
conf_run_phase7

if [ "${CONFORMANCE_PHASE}" = "p7" ]; then
    set -e
    CONFORMANCE_REPORT_PHASE=P7
    conf_finalize_report
    conf_log "conformance phase P7 finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
    if [ "${conf_failed}" -gt 0 ]; then
        exit 1
    fi
    exit 0
fi

set -e
CONFORMANCE_REPORT_PHASE=ALL
conf_finalize_report

conf_log "conformance finished passed=${conf_passed} failed=${conf_failed} skipped=${conf_skipped}"
if [ "${conf_failed}" -gt 0 ]; then
    exit 1
fi
exit 0
