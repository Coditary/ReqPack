#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
runner="${script_dir}/run-live-system-tests.sh"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
live_root="${repo_root}/tests/system/live"
matrix_file="${live_root}/matrix.tsv"

# shellcheck source=tests/system/live/lib/live_common.sh
source "${live_root}/lib/live_common.sh"

jobs="${LIVE_SYSTEM_TEST_JOBS:-2}"
quick="false"
extra_args=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --quick)
            quick="true"
            extra_args+=("--quick")
            shift
            ;;
        --jobs)
            jobs="$2"
            shift 2
            ;;
        *)
            extra_args+=("$1")
            shift
            ;;
    esac
done

ids=()
while IFS='|' read -r id _ _ _ _ _ _ enabled; do
    [[ "${id}" =~ ^# ]] && continue
    [ -z "${id}" ] && continue
    [ "${enabled}" != "1" ] && continue
    if [ "${quick}" = "true" ] && [ "${id}" != "debian-dpkg" ]; then
        continue
    fi
    ids+=("${id}")
done <"${matrix_file}"

if [ "${#ids[@]}" -eq 0 ]; then
    printf 'no enabled matrix rows\n' >&2
    exit 1
fi

work_root="$(bash "${runner}" "${extra_args[@]}" --prepare-only)"
# shellcheck disable=SC1090
source "${work_root}/env.sh"

export LIVE_RUN_ID="$(basename "${work_root}")"
live_init_container_tracking "${work_root}/containers.registry" "${LIVE_RUN_ID}"
export LIVE_CONTAINER_REGISTRY_APPEND=1
trap live_cleanup_containers EXIT INT TERM HUP

printf '[live-system-tests] parallel jobs=%s work_root=%s rows=%s\n' "${jobs}" "${work_root}" "${#ids[@]}"

run_row() {
    bash "${runner}" "${extra_args[@]}" --work-root "${work_root}" --skip-images "$1"
}

export -f run_row
export runner work_root extra_args

if [ "${jobs}" -le 1 ]; then
    for id in "${ids[@]}"; do
        run_row "${id}"
    done
else
    printf '%s\n' "${ids[@]}" | xargs -P "${jobs}" -I {} bash -c 'run_row "$1"' _ {}
fi

failed=()
for id in "${ids[@]}"; do
    status_file="${work_root}/reports/${id}.status"
    if [ ! -f "${status_file}" ] || [ "$(tr -d '[:space:]' < "${status_file}")" != "0" ]; then
        failed+=("${id}")
    fi
done

if [ "${#failed[@]}" -ne 0 ]; then
    printf 'parallel live tests failed:\n' >&2
    for id in "${failed[@]}"; do
        printf '  %s\n' "${id}" >&2
    done
    exit 1
fi

printf 'all parallel live rows passed\n'
