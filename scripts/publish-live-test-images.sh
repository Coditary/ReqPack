#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
live_root="${repo_root}/tests/system/live"

# shellcheck source=tests/system/lib/container_runtime.sh
source "${repo_root}/tests/system/lib/container_runtime.sh"
container_require_engine

registry="${LIVE_IMAGE_REGISTRY:-ghcr.io/coditary}"
tag_prefix="${LIVE_IMAGE_TAG_PREFIX:-reqpack-live-test}"

contexts=(
    debian-bookworm
    fedora-43
    archlinux
    node-22-bookworm
)

for name in "${contexts[@]}"; do
    context="${live_root}/images/${name}"
    image="${registry}/${tag_prefix}:${name}"
    printf '[publish-live-images] building %s\n' "${image}" >&2
    container_build -t "${image}" -f "${context}/Containerfile" "${context}"
    if [ "${LIVE_IMAGE_PUSH:-0}" = "1" ]; then
        "${CONTAINER_BIN}" push "${image}"
    fi
done

printf 'published tags under %s/%s:*\n' "${registry}" "${tag_prefix}"
