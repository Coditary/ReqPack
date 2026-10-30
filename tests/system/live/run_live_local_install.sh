#!/usr/bin/env bash
set -euo pipefail

system="${1:-dpkg}"
deb_path="${2:-/workspace/live-fixtures/debian/reqpack-live-fixture_1.0_all.deb}"
verify_cmd="${3:-command -v rqplive-fixture}"
remove_name="${4:-reqpack-live-fixture}"

RQP_BIN="${RQP_BIN:-/opt/reqpack/rqp}"
LIVE_CONFIG="${LIVE_CONFIG:-/workspace/config.lua}"

"${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json install "${system}" "${deb_path}"
bash -lc "${verify_cmd}"
"${RQP_BIN}" --config "${LIVE_CONFIG}" -y --json remove "${system}" "${remove_name}"
if bash -lc "${verify_cmd}" >/dev/null 2>&1; then
    printf 'verify failed after remove: %s\n' "${verify_cmd}" >&2
    exit 1
fi
printf '[live-local-install] ok system=%s artifact=%s\n' "${system}" "${deb_path}" >&2
