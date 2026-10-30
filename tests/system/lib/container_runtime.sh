#!/usr/bin/env bash
# Shared Podman/Docker helpers for ReqPack system and live container tests.
set -euo pipefail

CONTAINER_BIN="${CONTAINER_BIN:-}"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-}"

CONTAINER_RUN_ARGS=(--rm)

container_detect_engine() {
    if [ -n "${CONTAINER_BIN}" ] && command -v "${CONTAINER_BIN}" >/dev/null 2>&1; then
        case "${CONTAINER_BIN}" in
            podman) CONTAINER_ENGINE=podman ;;
            docker) CONTAINER_ENGINE=docker ;;
            *)
                printf 'unsupported CONTAINER_BIN: %s\n' "${CONTAINER_BIN}" >&2
                return 1
                ;;
        esac
        return 0
    fi

    if command -v podman >/dev/null 2>&1; then
        CONTAINER_BIN=podman
        CONTAINER_ENGINE=podman
        return 0
    fi

    if command -v docker >/dev/null 2>&1; then
        CONTAINER_BIN=docker
        CONTAINER_ENGINE=docker
        return 0
    fi

    return 1
}

container_require_engine() {
    if ! container_detect_engine; then
        printf 'Error: neither podman nor docker found in PATH.\n' >&2
        printf 'Install podman or docker, or set CONTAINER_BIN explicitly.\n' >&2
        exit 1
    fi
}

container_selinux_ro_suffix() {
    if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
        printf '%s' ',Z'
    fi
}

container_selinux_rw_suffix() {
    if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
        printf '%s' ',z'
    fi
}

container_volume_ro() {
    local host_path="$1"
    local container_path="$2"
    printf '%s:%s:ro%s' "${host_path}" "${container_path}" "$(container_selinux_ro_suffix)"
}

container_volume_rw() {
    local host_path="$1"
    local container_path="$2"
    local suffix
    suffix="$(container_selinux_rw_suffix)"
    if [ -z "${suffix}" ]; then
        suffix="$(container_selinux_ro_suffix)"
    fi
    printf '%s:%s:rw%s' "${host_path}" "${container_path}" "${suffix}"
}

container_build() {
    container_require_engine
    "${CONTAINER_BIN}" build "$@"
}

container_run() {
    container_require_engine
    "${CONTAINER_BIN}" run "${CONTAINER_RUN_ARGS[@]}" "$@"
}

container_network_create() {
    container_require_engine
    "${CONTAINER_BIN}" network create "$@"
}

container_network_rm() {
    container_require_engine
    "${CONTAINER_BIN}" network rm "$@" || true
}

container_run_detached() {
    container_require_engine
    "${CONTAINER_BIN}" run "$@"
}

container_wait() {
    container_require_engine
    "${CONTAINER_BIN}" wait "$@"
}

container_stop() {
    container_require_engine
    "${CONTAINER_BIN}" stop "$@"
}

container_logs() {
    container_require_engine
    "${CONTAINER_BIN}" logs "$@"
}

container_rm() {
    container_require_engine
    "${CONTAINER_BIN}" rm "$@"
}
