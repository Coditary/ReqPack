#!/usr/bin/env bash
set -euo pipefail

system="${1:-dpkg}"
RQP_BIN="${RQP_BIN:-/opt/reqpack/rqp}"
LIVE_CONFIG="${LIVE_CONFIG:-/workspace/config.lua}"
PLUGIN_DIR="${PLUGIN_DIR:-/workspace/plugins/${system}}"
REPORT="${LIVE_PLUGIN_REPORT:-/workspace/reports/plugin-live.json}"

mkdir -p "$(dirname "${REPORT}")"

"${RQP_BIN}" --config "${LIVE_CONFIG}" -y test-plugin \
    --plugin "${PLUGIN_DIR}" \
    --live \
    --report "${REPORT}"

grep -F '"failed": 0' "${REPORT}" >/dev/null
printf '[live-plugin-test] ok system=%s\n' "${system}" >&2
