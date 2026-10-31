#!/usr/bin/env bash
# Verify a linux release tarball runs on debian:bookworm-slim (glibc 2.36).
set -euo pipefail

if [ "$#" -ne 1 ]; then
    printf 'usage: %s <bundle-archive>\n' "$0" >&2
    exit 1
fi

bundle_archive="$1"
image="${REQPACK_SMOKE_IMAGE:-debian:bookworm-slim}"

[ -f "$bundle_archive" ] || {
    printf 'bundle missing: %s\n' "$bundle_archive" >&2
    exit 1
}

engine="${CONTAINER_ENGINE:-}"
if [ -z "$engine" ]; then
    if command -v docker >/dev/null 2>&1; then
        engine=docker
    elif command -v podman >/dev/null 2>&1; then
        engine=podman
    else
        printf 'Need docker or podman\n' >&2
        exit 1
    fi
fi

bundle_archive="$(CDPATH= cd -- "$(dirname "$bundle_archive")" && pwd)/$(basename "$bundle_archive")"

"${engine}" run --rm \
    --security-opt label=disable \
    -v "${bundle_archive}:/rqp.tgz:ro" \
    "${image}" \
    bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates tar
mkdir -p /opt/rqp
tar -xzf /rqp.tgz -C /opt/rqp
/opt/rqp --help >/dev/null
/opt/rqp version >/dev/null
'

printf 'Smoke OK on %s: %s\n' "$image" "$bundle_archive"
