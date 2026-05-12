#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
runner="${script_dir}/run-local-system-tests.sh"

if [ "$#" -eq 0 ]; then
    exec bash "${runner}"
fi

failed=()
for scenario in "$@"; do
    printf '[system-tests] suite serial scenario: %s\n' "${scenario}"
    set +e
    bash "${runner}" --scenario "${scenario}"
    status=$?
    set -e
    if [ "${status}" -ne 0 ]; then
        failed+=("${scenario}")
    fi
done

if [ "${#failed[@]}" -ne 0 ]; then
    printf 'failed scenarios:\n' >&2
    for scenario in "${failed[@]}"; do
        printf '  %s\n' "${scenario}" >&2
    done
    exit 1
fi

printf 'all requested scenarios passed\n'
