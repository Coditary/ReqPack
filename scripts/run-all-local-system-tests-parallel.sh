#!/usr/bin/env bash
set -euo pipefail

detect_jobs() {
    command -v nproc >/dev/null 2>&1 && nproc && return
    sysctl -n hw.ncpu 2>/dev/null && return
    printf '1\n'
}

format_duration() {
    local total_seconds="$1"
    local hours=$((total_seconds / 3600))
    local minutes=$(((total_seconds % 3600) / 60))
    local seconds=$((total_seconds % 60))

    if [ "${hours}" -gt 0 ]; then
        printf '%02d:%02d:%02d\n' "${hours}" "${minutes}" "${seconds}"
        return
    fi

    printf '%02d:%02d\n' "${minutes}" "${seconds}"
}

print_progress_summary() {
    local completed="$1"
    local total="$2"
    local passed="$3"
    local failed="$4"
    local running="$5"
    local started_at="$6"
    local now
    local elapsed
    local queued=$((total - completed - running))

    now="$(date +%s)"
    elapsed="$(format_duration "$((now - started_at))")"
    printf '[system-tests] progress: %s/%s done | pass=%s fail=%s running=%s queued=%s | elapsed=%s\n' \
        "${completed}" "${total}" "${passed}" "${failed}" "${running}" "${queued}" "${elapsed}"
}

SCAN_COMPLETED=0
SCAN_PASSED=0
SCAN_FAILED=0
SCAN_RUNNING=0

scan_parallel_statuses() {
    local log_dir="$1"
    local total="$2"
    local -n seen_started_ref="$3"
    local -n seen_finished_ref="$4"
    local scenario
    local status_file
    local status

    SCAN_COMPLETED=0
    SCAN_PASSED=0
    SCAN_FAILED=0
    SCAN_RUNNING=0

    for scenario in "${scenarios[@]}"; do
        status_file="${status_dir}/${scenario}.status"
        if [ ! -f "${status_file}" ]; then
            continue
        fi

        status="$(tr -d '[:space:]' < "${status_file}")"
        case "${status}" in
            running)
                SCAN_RUNNING=$((SCAN_RUNNING + 1))
                if [ -z "${seen_started_ref[${scenario}]+x}" ]; then
                    seen_started_ref["${scenario}"]="1"
                    printf '[system-tests] [start] %s\n' "${scenario}"
                fi
                ;;
            ok)
                SCAN_COMPLETED=$((SCAN_COMPLETED + 1))
                SCAN_PASSED=$((SCAN_PASSED + 1))
                if [ -z "${seen_finished_ref[${scenario}]+x}" ]; then
                    seen_finished_ref["${scenario}"]="ok"
                    printf '[system-tests] [%s/%s] PASS %s\n' "${SCAN_COMPLETED}" "${total}" "${scenario}"
                fi
                ;;
            fail)
                SCAN_COMPLETED=$((SCAN_COMPLETED + 1))
                SCAN_FAILED=$((SCAN_FAILED + 1))
                if [ -z "${seen_finished_ref[${scenario}]+x}" ]; then
                    seen_finished_ref["${scenario}"]="fail"
                    printf '[system-tests] [%s/%s] FAIL %s (%s)\n' \
                        "${SCAN_COMPLETED}" "${total}" "${scenario}" "${log_dir}/${scenario}.log"
                fi
                ;;
        esac
    done
}

monitor_parallel_progress() {
    local worker_pid="$1"
    local log_dir="$2"
    local total="$3"
    local started_at="$4"
    local heartbeat_interval=10
    local last_summary_at=0
    local last_completed=-1
    local last_passed=-1
    local last_failed=-1
    local last_running=-1
    local now
    local -A seen_started=()
    local -A seen_finished=()

    while :; do
        scan_parallel_statuses "${log_dir}" "${total}" seen_started seen_finished
        now="$(date +%s)"

        if [ "${SCAN_COMPLETED}" -ne "${last_completed}" ] ||
           [ "${SCAN_PASSED}" -ne "${last_passed}" ] ||
           [ "${SCAN_FAILED}" -ne "${last_failed}" ] ||
           [ "${SCAN_RUNNING}" -ne "${last_running}" ] ||
           [ $((now - last_summary_at)) -ge "${heartbeat_interval}" ]; then
            print_progress_summary "${SCAN_COMPLETED}" "${total}" "${SCAN_PASSED}" "${SCAN_FAILED}" "${SCAN_RUNNING}" "${started_at}"
            last_summary_at="${now}"
            last_completed="${SCAN_COMPLETED}"
            last_passed="${SCAN_PASSED}"
            last_failed="${SCAN_FAILED}"
            last_running="${SCAN_RUNNING}"
        fi

        if ! kill -0 "${worker_pid}" >/dev/null 2>&1; then
            scan_parallel_statuses "${log_dir}" "${total}" seen_started seen_finished
            print_progress_summary "${SCAN_COMPLETED}" "${total}" "${SCAN_PASSED}" "${SCAN_FAILED}" "${SCAN_RUNNING}" "${started_at}"
            return
        fi

        sleep 1
    done
}

run_parallel_workers() {
    local log_dir="$1"
    local suite_id="$2"
    local runner="$3"
    local status_dir="$4"

    printf '%s\n' "${scenarios[@]}" | xargs -P "${jobs}" -I{} bash -c '
scenario="$1"
log_dir="$2"
suite_id="$3"
runner="$4"
status_dir="$5"
log_path="${log_dir}/${scenario}.log"
run_id="${suite_id}-${scenario}"
printf "running\n" > "${status_dir}/${scenario}.status"
if bash "${runner}" --run-id "${run_id}" --scenario "${scenario}" > "${log_path}" 2>&1; then
    printf "ok\n" > "${status_dir}/${scenario}.status"
else
    printf "fail\n" > "${status_dir}/${scenario}.status"
fi
' _ {} "${log_dir}" "${suite_id}" "${runner}" "${status_dir}"
}

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
runner="${script_dir}/run-local-system-tests.sh"
jobs="${JOBS:-$(detect_jobs)}"
build_jobs="${REQPACK_LOCAL_BUILD_JOBS:-${LOCAL_SYSTEM_TEST_BUILD_JOBS:-auto}}"

case "${jobs}" in
    ''|*[!0-9]*)
        printf 'invalid JOBS value: %s\n' "${jobs}" >&2
        exit 1
        ;;
esac
if [ "${jobs}" -le 0 ]; then
    printf 'JOBS must be greater than 0\n' >&2
    exit 1
fi

discover_scenarios() {
    if [ "$#" -gt 0 ]; then
        printf '%s\n' "$@"
        return
    fi

    local scenario_path
    local scenario_name
    shopt -s nullglob
    for scenario_path in "${repo_root}"/tests/system/scenarios/*/*/scenario.sh; do
        scenario_name="$(basename "$(dirname "${scenario_path}")")"
        printf '%s\n' "${scenario_name}"
    done | sort -u
}

mapfile -t scenarios < <(discover_scenarios "$@")

if [ "${#scenarios[@]}" -eq 0 ]; then
    printf 'no scenarios found\n' >&2
    exit 1
fi

suite_id="parallel-$(date +%Y%m%d-%H%M%S)-$$-${RANDOM}"
log_dir="${repo_root}/build/system-tests-local/parallel-logs/${suite_id}"
status_dir="$(mktemp -d)"
cleanup() {
    rm -rf "${status_dir}"
}
trap cleanup EXIT INT TERM HUP

mkdir -p "${log_dir}"

printf '[system-tests] suite id: %s\n' "${suite_id}"
printf '[system-tests] log dir: %s\n' "${log_dir}"
printf '[system-tests] parallel jobs: %s\n' "${jobs}"
printf '[system-tests] build jobs per runner: %s\n' "${build_jobs}"

started_at="$(date +%s)"
set +e
run_parallel_workers "${log_dir}" "${suite_id}" "${runner}" "${status_dir}" &
workers_pid=$!
monitor_parallel_progress "${workers_pid}" "${log_dir}" "${#scenarios[@]}" "${started_at}"
wait "${workers_pid}"
xargs_status=$?
set -e

failed=()
for scenario in "${scenarios[@]}"; do
    status_file="${status_dir}/${scenario}.status"
    if [ ! -f "${status_file}" ] || [ "$(tr -d '[:space:]' < "${status_file}")" != "ok" ]; then
        failed+=("${scenario}")
    fi
done

if [ "${#failed[@]}" -ne 0 ]; then
    printf 'failed scenarios:\n' >&2
    for scenario in "${failed[@]}"; do
        printf '  %s (%s)\n' "${scenario}" "${log_dir}/${scenario}.log" >&2
    done
    if [ "${xargs_status}" -ne 0 ]; then
        printf 'parallel runner exit code: %s\n' "${xargs_status}" >&2
    fi
    exit 1
fi

printf 'all requested scenarios passed\n'
