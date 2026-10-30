#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
export REQPACK_REPO_ROOT="${repo_root}"

live_root="${repo_root}/tests/system/live"
# shellcheck source=tests/system/live/lib/live_common.sh
source "${live_root}/lib/live_common.sh"

bundle_archive=""
skip_build="false"
skip_images="false"
prepare_only="false"
quick="false"
work_root=""
matrix_file="${live_root}/matrix.tsv"
build_jobs="${REQPACK_LOCAL_BUILD_JOBS:-${LOCAL_SYSTEM_TEST_BUILD_JOBS:-}}"
filter_ids=()

usage() {
    cat <<EOF
usage: $(basename "$0") [options] [matrix-id ...]

Runs live package-manager lifecycle checks inside distro containers (Podman or Docker).

Options:
  --bundle PATH       Use existing linux release tarball
  --work-root PATH    Reuse prepared work directory (implies --skip-build)
  --prepare-only      Build bundle and cached distro images, then exit
  --skip-build        Do not compile rqp (requires --bundle or --work-root)
  --skip-images       Do not rebuild cached distro base images
  --quick             Run only debian-dpkg
  --matrix FILE       Alternate matrix TSV (default: tests/system/live/matrix.tsv)
  -h, --help          Show this help

Environment:
  CONTAINER_BIN         Force podman or docker
  REQPACK_PLUGINS_ROOT  Path to reqpack-plugins checkout

Exit codes:
  0 success
  1 failure
  77 skipped (no container engine)
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bundle)
            bundle_archive="$2"
            skip_build="true"
            shift 2
            ;;
        --work-root)
            work_root="$2"
            skip_build="true"
            shift 2
            ;;
        --prepare-only)
            prepare_only="true"
            shift
            ;;
        --skip-build)
            skip_build="true"
            shift
            ;;
        --skip-images)
            skip_images="true"
            shift
            ;;
        --quick)
            quick="true"
            shift
            ;;
        --matrix)
            matrix_file="$2"
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --*)
            printf 'unknown option: %s\n' "$1" >&2
            usage >&2
            exit 1
            ;;
        *)
            filter_ids+=("$1")
            shift
            ;;
    esac
done

if ! container_detect_engine; then
    live_log 'skip: neither podman nor docker available'
    exit 77
fi

live_log "container engine: ${CONTAINER_BIN}"

plugins_root="$(live_plugins_root)"
if [ ! -d "${plugins_root}" ]; then
    printf 'reqpack-plugins not found at %s\n' "${plugins_root}" >&2
    printf 'Set REQPACK_PLUGINS_ROOT to the plugins monorepo checkout.\n' >&2
    exit 1
fi

if [ -z "${work_root}" ]; then
    work_root="${repo_root}/build/live-system-tests/$(date +%Y%m%d-%H%M%S)-$$"
fi

live_run_id="${LIVE_RUN_ID:-$(basename "${work_root}")}"
live_container_registry_path="${work_root}/containers.registry"
if [ "${prepare_only}" != "true" ]; then
    if [ "${LIVE_CONTAINER_REGISTRY_APPEND:-0}" = "1" ]; then
        live_container_registry="${live_container_registry_path}"
    else
        live_init_container_tracking "${live_container_registry_path}" "${live_run_id}"
        trap live_cleanup_containers EXIT INT TERM HUP
    fi
fi

bundle_dir="${work_root}/bundle"
reports_root="${work_root}/reports"
mkdir -p "${bundle_dir}" "${reports_root}"

if [ -f "${work_root}/bundle.ready" ] && [ "${skip_build}" = "true" ]; then
    live_log "reusing prepared bundle at ${bundle_dir}"
elif [ -n "${bundle_archive}" ]; then
    [ -f "${bundle_archive}" ] || {
        printf 'bundle missing: %s\n' "${bundle_archive}" >&2
        exit 1
    }
    live_extract_linux_bundle "${bundle_archive}" "${bundle_dir}"
    touch "${work_root}/bundle.ready"
else
    if [ "${skip_build}" = "true" ]; then
        printf '--skip-build requires --bundle or --work-root with prepared bundle\n' >&2
        exit 1
    fi
    local_build_dir="${work_root}/build"
    mkdir -p "${local_build_dir}"
    live_log "building rqp bundle under ${local_build_dir}"
    cmake -S "${repo_root}" -B "${local_build_dir}" -DCMAKE_BUILD_TYPE=Release >&2
    if [ -n "${build_jobs}" ]; then
        cmake --build "${local_build_dir}" --parallel "${build_jobs}" --target ReqPack >&2
    else
        cmake --build "${local_build_dir}" --parallel --target ReqPack >&2
    fi
    bundle_archive="${work_root}/rqp-linux.tar.gz"
    bash "${repo_root}/scripts/package_release_bundle.sh" "${local_build_dir}/rqp" "${bundle_archive}" linux >&2
    live_extract_linux_bundle "${bundle_archive}" "${bundle_dir}"
    touch "${work_root}/bundle.ready"
fi

lifecycle_script="${live_root}/run_live_lifecycle.sh"
chmod +x "${lifecycle_script}"

image_context_for_tag() {
    local tag="$1"
    case "${tag}" in
        reqpack-live-test:debian-bookworm) printf '%s' "${live_root}/images/debian-bookworm" ;;
        reqpack-live-test:fedora-43) printf '%s' "${live_root}/images/fedora-43" ;;
        reqpack-live-test:archlinux) printf '%s' "${live_root}/images/archlinux" ;;
        reqpack-live-test:node-22-bookworm) printf '%s' "${live_root}/images/node-22-bookworm" ;;
        *)
            printf 'unknown live image tag: %s\n' "${tag}" >&2
            return 1
            ;;
    esac
}

ensure_live_images() {
    if [ "${skip_images}" = "true" ]; then
        return 0
    fi
    local tags=(
        reqpack-live-test:debian-bookworm
        reqpack-live-test:fedora-43
        reqpack-live-test:archlinux
        reqpack-live-test:node-22-bookworm
    )
    local tag context
    for tag in "${tags[@]}"; do
        context="$(image_context_for_tag "${tag}")"
        live_log "building cached base image ${tag}"
        container_build -t "${tag}" -f "${context}/Containerfile" "${context}" >/dev/null
    done
    touch "${work_root}/images.ready"
}

ensure_live_images

{
    printf 'LIVE_WORK_ROOT=%q\n' "${work_root}"
    printf 'LIVE_BUNDLE_DIR=%q\n' "${bundle_dir}"
} >"${work_root}/env.sh"

if [ "${prepare_only}" = "true" ]; then
    live_log "prepare complete: ${work_root}"
    printf '%s\n' "${work_root}"
    exit 0
fi

row_selected() {
    local id="$1"
    if [ "${quick}" = "true" ] && [ "${id}" != "debian-dpkg" ]; then
        return 1
    fi
    if [ "${#filter_ids[@]}" -eq 0 ]; then
        return 0
    fi
    local wanted
    for wanted in "${filter_ids[@]}"; do
        if [ "${wanted}" = "${id}" ]; then
            return 0
        fi
    done
    return 1
}

fixtures_root="${live_root}/fixtures"

resolve_live_image() {
    local tag="$1"
    if [ -n "${LIVE_IMAGE_REGISTRY:-}" ]; then
        local suffix="${tag#reqpack-live-test:}"
        printf '%s/reqpack-live-test:%s' "${LIVE_IMAGE_REGISTRY}" "${suffix}"
        return
    fi
    printf '%s' "${tag}"
}

run_matrix_row() {
    local id="$1"
    local image="$2"
    local system="$3"
    local plugin_relpath="$4"
    local package="$5"
    local verify_cmd="$6"
    local mode="${7:-repo}"

    local row_dir="${work_root}/rows/${id}"
    local workspace_dir="${row_dir}/workspace"
    local plugin_dir="${workspace_dir}/plugins/${system}"
    local scenario_dir="${row_dir}/scenario"
    local report_dir="${workspace_dir}/reports"
    mkdir -p "${scenario_dir}" "${report_dir}"

    live_stage_plugin "${plugins_root}" "${plugin_relpath}" "${plugin_dir}"
    live_create_config "${workspace_dir}" "${workspace_dir}/plugins" >/dev/null

    local container_image
    container_image="$(resolve_live_image "${image}")"

    local -a volume_args=()
    volume_args+=(-v "$(container_volume_ro "${bundle_dir}" /opt/reqpack)")
    volume_args+=(-v "$(container_volume_rw "${workspace_dir}" /workspace)")
    if [ "${mode}" = "local" ] || [ "${mode}" = "plugin-live" ]; then
        volume_args+=(-v "$(container_volume_ro "${fixtures_root}" /workspace/live-fixtures)")
    fi

    local inner_cmd=""
    case "${mode}" in
        repo)
            cp "${lifecycle_script}" "${scenario_dir}/run_live_lifecycle.sh"
            chmod +x "${scenario_dir}/run_live_lifecycle.sh"
            volume_args+=(-v "$(container_volume_ro "${scenario_dir}" /workspace/test)")
            inner_cmd="export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua LIVE_REPORT_DIR=/workspace/reports; bash /workspace/test/run_live_lifecycle.sh '${system}' '${package}' '${verify_cmd}'"
            ;;
        local)
            cp "${live_root}/run_live_local_install.sh" "${scenario_dir}/run_live_local_install.sh"
            chmod +x "${scenario_dir}/run_live_local_install.sh"
            volume_args+=(-v "$(container_volume_ro "${scenario_dir}" /workspace/test)")
            local fixture_rel="${package%%:*}"
            local remove_name="${package#*:}"
            if [ "${remove_name}" = "${package}" ]; then
                remove_name="reqpack-live-fixture"
            fi
            inner_cmd="export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua; bash /workspace/test/run_live_local_install.sh '${system}' '/workspace/live-fixtures/${fixture_rel}' '${verify_cmd}' '${remove_name}'"
            ;;
        plugin-live)
            cp "${live_root}/run_live_plugin_test.sh" "${scenario_dir}/run_live_plugin_test.sh"
            chmod +x "${scenario_dir}/run_live_plugin_test.sh"
            volume_args+=(-v "$(container_volume_ro "${scenario_dir}" /workspace/test)")
            inner_cmd="export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua PLUGIN_DIR=/workspace/plugins/${system}; bash /workspace/test/run_live_plugin_test.sh '${system}'"
            ;;
        *)
            printf 'unknown live test mode: %s\n' "${mode}" >&2
            return 1
            ;;
    esac

    live_log "row ${id}: ${container_image} system=${system} mode=${mode}"

    set +e
    live_run_container "${id}" \
        "${volume_args[@]}" \
        "${container_image}" \
        bash -lc "${inner_cmd}"
    local status=$?
    set -e

    printf '%s\n' "${status}" >"${reports_root}/${id}.status"
    if [ "${status}" -eq 0 ]; then
        live_log "PASS ${id}"
        return 0
    fi

    live_log "FAIL ${id} (exit ${status})"
    return "${status}"
}

failed=()
while IFS='|' read -r id image system plugin_relpath package verify_cmd mode enabled; do
    [[ "${id}" =~ ^# ]] && continue
    [ -z "${id}" ] && continue
    [ "${enabled}" != "1" ] && continue
    mode="${mode:-repo}"
    if ! row_selected "${id}"; then
        continue
    fi
    if ! run_matrix_row "${id}" "${image}" "${system}" "${plugin_relpath}" "${package}" "${verify_cmd}" "${mode}"; then
        failed+=("${id}")
    fi
done <"${matrix_file}"

if [ "${#failed[@]}" -ne 0 ]; then
    printf 'live system tests failed:\n' >&2
    for id in "${failed[@]}"; do
        printf '  %s (artifacts under %s/rows/%s)\n' "${id}" "${work_root}" "${id}" >&2
    done
    exit 1
fi

live_log "all live matrix rows passed (work root: ${work_root})"
