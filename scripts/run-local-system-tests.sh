#!/usr/bin/env bash
set -euo pipefail

repo_root="$(pwd)"
scenario_filter=""
keep_failed_workdir="false"
run_id=""
build_jobs="${REQPACK_LOCAL_BUILD_JOBS:-${LOCAL_SYSTEM_TEST_BUILD_JOBS:-}}"
container_repo_root="/repo"
container_output_dir="/test-output"
container_control_dir="/test-control"
container_runtime_root="/test-runtime-root"
container_shared_rw_root="/test-shared"

validate_run_id() {
    local value="$1"
    [[ "${value}" =~ ^[A-Za-z0-9._-]+$ ]]
}

validate_jobs() {
    local value="$1"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]]
}

generate_run_id() {
    printf '%s-%s-%s\n' "$(date +%Y%m%d-%H%M%S)" "$$" "${RANDOM}"
}

image_tag_suffix() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --scenario)
            if [ "$#" -lt 2 ]; then
                printf '%s requires a value\n' "$1" >&2
                exit 1
            fi
            scenario_filter="$2"
            shift 2
            ;;
        --keep-failed-workdir)
            keep_failed_workdir="true"
            shift
            ;;
        --run-id)
            if [ "$#" -lt 2 ]; then
                printf '%s requires a value\n' "$1" >&2
                exit 1
            fi
            run_id="$2"
            shift 2
            ;;
        *)
            printf 'unknown argument: %s\n' "$1" >&2
            exit 1
            ;;
    esac
done

system_tests_root="${repo_root}/build/system-tests-local"
runs_root="${system_tests_root}/runs"
bundle_dir=""
local_build_dir="${bundle_dir}/build"
bundle_path="${bundle_dir}/rqp-local-linux.tar.gz"
image_context_dir="${bundle_dir}/image-context"
image_tag=""
results_dir="${bundle_dir}/results"
label_suffix=""
remote_label_suffix=""

mkdir -p "${system_tests_root}" "${runs_root}"

if [ -n "${run_id}" ]; then
    if ! validate_run_id "${run_id}"; then
        printf 'invalid run id: %s\n' "${run_id}" >&2
        printf 'allowed characters: letters, digits, dot, underscore, dash\n' >&2
        exit 1
    fi
    bundle_dir="${runs_root}/${run_id}"
    if ! mkdir "${bundle_dir}" 2>/dev/null; then
        printf 'run id already exists: %s\n' "${run_id}" >&2
        printf 'choose unique --run-id for concurrent or repeated runs\n' >&2
        exit 1
    fi
else
    while :; do
        run_id="$(generate_run_id)"
        bundle_dir="${runs_root}/${run_id}"
        if mkdir "${bundle_dir}" 2>/dev/null; then
            break
        fi
    done
fi

local_build_dir="${bundle_dir}/build"
bundle_path="${bundle_dir}/rqp-local-linux.tar.gz"
image_context_dir="${bundle_dir}/image-context"
image_tag="reqpack-local-system-tests:$(image_tag_suffix "${run_id}")"
results_dir="${bundle_dir}/results"

mkdir -p "${results_dir}" "${local_build_dir}"

printf '[system-tests] run root: %s\n' "${bundle_dir}"
printf '[system-tests] image tag: %s\n' "${image_tag}"

if [ -n "${build_jobs}" ] && ! validate_jobs "${build_jobs}"; then
    printf 'invalid build jobs value: %s\n' "${build_jobs}" >&2
    printf 'set REQPACK_LOCAL_BUILD_JOBS or LOCAL_SYSTEM_TEST_BUILD_JOBS to positive integer\n' >&2
    exit 1
fi

printf '[system-tests] build jobs: %s\n' "${build_jobs:-auto}"

if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
    label_suffix=",Z"
    remote_label_suffix=",z"
fi

cmake -S "${repo_root}" -B "${local_build_dir}" -DCMAKE_BUILD_TYPE=Release
if [ -n "${build_jobs}" ]; then
    cmake --build "${local_build_dir}" --parallel "${build_jobs}" --target ReqPack
else
    cmake --build "${local_build_dir}" --parallel --target ReqPack
fi

bash "${repo_root}/scripts/package_release_bundle.sh" "${local_build_dir}/rqp" "${bundle_path}" linux

mkdir -p "${image_context_dir}"
cp "${repo_root}/tests/system/local/Containerfile" "${image_context_dir}/Containerfile"
cp "${bundle_path}" "${image_context_dir}/rqp.tar.gz"

podman build -t "${image_tag}" "${image_context_dir}" >/dev/null

container_runner_command="tmpdir=\"\$(mktemp -d)\" && cp \"${container_control_dir}/run-scenario.sh\" \"\${tmpdir}/run-scenario.sh\" && cp \"${container_control_dir}/scenario.sh\" \"\${tmpdir}/scenario.sh\" && bash \"\${tmpdir}/run-scenario.sh\" \"${container_repo_root}\" \"\${tmpdir}/scenario.sh\" \"${container_output_dir}\""

scenario_uses_remote_multi_container() {
    local scenario="$1"
    grep -Eq '^[[:space:]]*scenario_mode=["'"'"']remote_multi_container["'"'"']' "${scenario}"
}

run_single_container_scenario() {
    local scenario="$1"
    local output_dir="$2"

    rm -rf "${output_dir}"
    mkdir -p "${output_dir}"
    cp "${scenario}" "${output_dir}/scenario.sh"
    cp "${repo_root}/tests/system/local/run-scenario.sh" "${output_dir}/run-scenario.sh"

    set +e
    podman run --rm \
        --network=none \
        -v "${repo_root}:${container_repo_root}:ro${label_suffix}" \
        -v "${output_dir}:${container_output_dir}:rw${label_suffix}" \
        "${image_tag}" \
        bash -lc "tmpdir=\"\$(mktemp -d)\" && cp \"${container_output_dir}/run-scenario.sh\" \"\${tmpdir}/run-scenario.sh\" && cp \"${container_output_dir}/scenario.sh\" \"\${tmpdir}/scenario.sh\" && bash \"\${tmpdir}/run-scenario.sh\" \"${container_repo_root}\" \"\${tmpdir}/scenario.sh\" \"${container_output_dir}\""
    local status=$?
    set -e

    return "${status}"
}

run_remote_multi_container_scenario() {
    local scenario="$1"
    local name="$2"
    local output_dir="$3"
    local control_dir="${output_dir}/control"
    local runtime_root="${output_dir}/runtime"
    local server_output_dir="${output_dir}/server"
    local client_output_dir="${output_dir}/client"
    local shared_rw_dir="${output_dir}/shared"
    local network_name="reqpack-st-${name}-$$-${RANDOM}"
    local server_container_name="${network_name}-server"
    local client_container_name="${network_name}-client"
    local server_status="not-started"
    local client_status=1
    local server_started="false"
    local network_created="false"
    local timed_out="false"
    local client_runtime_dir="${output_dir}/runtime/client"
    local server_runtime_dir="${output_dir}/runtime/server"

    rm -rf "${output_dir}"
    mkdir -p "${control_dir}" "${runtime_root}/server" "${runtime_root}/client" "${server_output_dir}" "${client_output_dir}" "${shared_rw_dir}"
    cp "${scenario}" "${control_dir}/scenario.sh"
    cp "${repo_root}/tests/system/local/run-scenario.sh" "${control_dir}/run-scenario.sh"

    if ! podman network create "${network_name}" >/dev/null; then
        printf 'failed to create test network: %s\n' "${network_name}" >&2
        return 1
    fi
    network_created="true"

    if ! podman run -d \
        --name "${server_container_name}" \
        --hostname server \
        --network "${network_name}" \
        -e SYSTEM_TEST_ROLE=server \
        -e SYSTEM_TEST_ROOT="${container_runtime_root}/server" \
        -e SYSTEM_TEST_SHARED_ROOT="${container_runtime_root}" \
        -e SYSTEM_TEST_SHARED_RW_ROOT="${container_shared_rw_root}" \
        -e SYSTEM_TEST_SERVER_ROOT="${container_runtime_root}/server" \
        -e SYSTEM_TEST_CLIENT_ROOT="${container_runtime_root}/client" \
        -e REMOTE_SERVER_HOST=server \
        -e REMOTE_SERVER_PORT=4545 \
        -v "${repo_root}:${container_repo_root}:ro${remote_label_suffix}" \
        -v "${control_dir}:${container_control_dir}:ro${remote_label_suffix}" \
        -v "${runtime_root}:${container_runtime_root}:rw${remote_label_suffix}" \
        -v "${shared_rw_dir}:${container_shared_rw_root}:rw${remote_label_suffix}" \
        -v "${server_output_dir}:${container_output_dir}:rw${remote_label_suffix}" \
        "${image_tag}" \
        bash -lc "${container_runner_command}" >/dev/null; then
        if [ "${network_created}" = "true" ]; then
            podman network rm "${network_name}" >/dev/null 2>&1 || true
        fi
        printf 'failed to start server container: %s\n' "${server_container_name}" >&2
        return 1
    fi
    server_started="true"

    set +e
    podman run --rm \
        --name "${client_container_name}" \
        --hostname client \
        --network "${network_name}" \
        -e SYSTEM_TEST_ROLE=client \
        -e SYSTEM_TEST_ROOT="${container_runtime_root}/client" \
        -e SYSTEM_TEST_SHARED_ROOT="${container_runtime_root}" \
        -e SYSTEM_TEST_SHARED_RW_ROOT="${container_shared_rw_root}" \
        -e SYSTEM_TEST_SERVER_ROOT="${container_runtime_root}/server" \
        -e SYSTEM_TEST_CLIENT_ROOT="${container_runtime_root}/client" \
        -e REMOTE_SERVER_HOST=server \
        -e REMOTE_SERVER_PORT=4545 \
        -v "${repo_root}:${container_repo_root}:ro${remote_label_suffix}" \
        -v "${control_dir}:${container_control_dir}:ro${remote_label_suffix}" \
        -v "${client_runtime_dir}:${container_runtime_root}/client:rw${remote_label_suffix}" \
        -v "${server_runtime_dir}:${container_runtime_root}/server:ro${remote_label_suffix}" \
        -v "${shared_rw_dir}:${container_shared_rw_root}:rw${remote_label_suffix}" \
        -v "${client_output_dir}:${container_output_dir}:rw${remote_label_suffix}" \
        "${image_tag}" \
        bash -lc "${container_runner_command}"
    client_status=$?
    set -e
    printf '%s\n' "${client_status}" > "${client_output_dir}/container-exit.txt"

    if [ "${server_started}" = "true" ]; then
        set +e
        timeout 15s podman wait "${server_container_name}" > "${server_output_dir}/container-exit.txt"
        local wait_status=$?
        set -e

        if [ "${wait_status}" -eq 0 ]; then
            server_status="$(tr -d '[:space:]' < "${server_output_dir}/container-exit.txt")"
        else
            timed_out="true"
            server_status="timeout"
            printf 'server container did not exit within timeout\n' > "${server_output_dir}/runner-error.txt"
            set +e
            podman stop -t 1 "${server_container_name}" >/dev/null 2>&1
            podman wait "${server_container_name}" >/dev/null 2>&1
            set -e
        fi

        set +e
        podman logs "${server_container_name}" > "${server_output_dir}/container.log" 2>&1
        podman rm -f "${server_container_name}" >/dev/null 2>&1
        set -e
    fi

    if [ "${network_created}" = "true" ]; then
        podman network rm "${network_name}" >/dev/null 2>&1 || true
    fi

    if [ "${timed_out}" = "false" ] && [ "${client_status}" -eq 0 ] && [ "${server_status}" = "0" ]; then
        return 0
    fi

    return 1
}

mapfile -t scenarios < <(printf '%s\n' "${repo_root}"/tests/system/scenarios/*/*/scenario.sh)

if [ -n "${scenario_filter}" ]; then
    filtered=()
    for scenario in "${scenarios[@]}"; do
        name="$(basename "$(dirname "${scenario}")")"
        if [ "${name}" = "${scenario_filter}" ]; then
            filtered+=("${scenario}")
        fi
    done
    scenarios=("${filtered[@]}")
fi

if [ "${#scenarios[@]}" -eq 0 ]; then
    printf 'no scenarios found\n' >&2
    exit 1
fi

failed=0
for scenario in "${scenarios[@]}"; do
    name="$(basename "$(dirname "${scenario}")")"
    output_dir="${results_dir}/${name}"
    printf '[system-tests] %s\n' "${name}"
    if scenario_uses_remote_multi_container "${scenario}"; then
        set +e
        run_remote_multi_container_scenario "${scenario}" "${name}" "${output_dir}"
        status=$?
        set -e
    else
        set +e
        run_single_container_scenario "${scenario}" "${output_dir}"
        status=$?
        set -e
    fi
    if [ "${status}" -eq 0 ]; then
        printf '[pass] %s\n' "${name}"
    else
        printf '[fail] %s\n' "${name}"
        printf 'artifacts: %s\n' "${output_dir}"
        failed=$((failed + 1))
        if [ "${keep_failed_workdir}" != "true" ]; then
            :
        fi
    fi
done

if [ "${failed}" -ne 0 ]; then
    printf 'failed scenarios: %s\n' "${failed}" >&2
    exit 1
fi

printf 'all scenarios passed\n'
