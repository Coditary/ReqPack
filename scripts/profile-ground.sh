#!/usr/bin/env bash
# Interactive profiling ground for ReqPack (rqp).
# Builds a profile-instrumented binary when needed, runs realistic workloads,
# and writes perf or gprof hotspot reports under build/profile/profile-data/ground/.
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "${repo_root}"

PROFILE_BUILD_DIR="${REQPACK_PROFILE_BUILD_DIR:-build/profile}"
PROFILE_DATA_DIR="${PROFILE_BUILD_DIR}/profile-data/ground"
RQP_BIN="${REQPACK_PROFILE_RQP_BIN:-${PROFILE_BUILD_DIR}/rqp}"
CORE_UNIT_BIN="${PROFILE_BUILD_DIR}/core_unit_tests"
CORE_INTEGRATION_BIN="${PROFILE_BUILD_DIR}/core_integration_tests"
PYTHON="${PYTHON:-python3}"

PROFILER="auto"
SCENARIO=""
ITERATIONS=1
NO_BUILD="false"
KEEP_WORKSPACE="false"
INTERACTIVE="true"
USE_SYSTEM_ENV="${REQPACK_PROFILE_USE_SYSTEM:-true}"
DRY_RUN="false"

WORKSPACE=""
CONFIG_PATH=""
LAST_RUN_ID=""
LAST_REPORT_TXT=""

usage() {
    cat <<'EOF'
Usage: profile-ground.sh [options]

Interactive profiling harness for ReqPack. Profiles build/profile/rqp with perf,
gprof, or time and prints hotspot summaries via tests/profile_summary.py.

By default this uses your normal ReqPack config/plugins (~/.config/reqpack, XDG
paths). Hermetic fixture scenarios opt in with --hermetic.

Options:
  --build                 Build profile target before running
  --no-build              Skip build step
  --system                Use your real ReqPack config and XDG paths (default)
  --hermetic              Use isolated temp workspace with test fixtures
  --dry-run               Append --dry-run to install scenarios
  --profiler auto|perf|gprof|time
  --scenario <name>       Run one scenario and exit
  --iterations <n>        Repeat workload n times (default: 1)
  --keep-workspace        Keep hermetic temp workspace on exit
  -h, --help              Show this help

Scenarios:
  install-ycallr-cli      rqp install ycallr-cli (real user setup)
  install-ycallr-cli-dry-run
                          rqp install ycallr-cli --dry-run
  help                    rqp --help
  list                    rqp list dnf (hermetic)
  install-dry-run         rqp install dnf curl --dry-run (hermetic)
  test-plugin             rqp test-plugin demo preset (hermetic)
  security-unit           core_unit_tests [unit][security]
  execution-integration   core_integration_tests [integration][executor]
  integration-all         core_integration_tests (slow)
  custom                  custom rqp args (interactive only)

Examples:
  make profile-ground
  bash scripts/profile-ground.sh --scenario install-ycallr-cli --profiler perf
  bash scripts/profile-ground.sh --scenario install-ycallr-cli-dry-run --iterations 3
  bash scripts/profile-ground.sh --hermetic --scenario install-dry-run

Reports:
  build/profile/profile-data/ground/<run-id>/
EOF
}

log() {
    printf '[profile-ground] %s\n' "$*"
}

die() {
    printf '[profile-ground] error: %s\n' "$*" >&2
    exit 1
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --build)
                NO_BUILD="false"
                shift
                ;;
            --no-build)
                NO_BUILD="true"
                shift
                ;;
            --system)
                USE_SYSTEM_ENV="true"
                shift
                ;;
            --hermetic)
                USE_SYSTEM_ENV="false"
                shift
                ;;
            --dry-run)
                DRY_RUN="true"
                shift
                ;;
            --profiler)
                [ "$#" -ge 2 ] || die "--profiler requires a value"
                PROFILER="$2"
                shift 2
                ;;
            --scenario)
                [ "$#" -ge 2 ] || die "--scenario requires a value"
                SCENARIO="$2"
                INTERACTIVE="false"
                shift 2
                ;;
            --iterations)
                [ "$#" -ge 2 ] || die "--iterations requires a value"
                ITERATIONS="$2"
                shift 2
                ;;
            --keep-workspace)
                KEEP_WORKSPACE="true"
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "unknown argument: $1"
                ;;
        esac
    done
}

scenario_uses_hermetic() {
    case "$1" in
        list|install-dry-run|test-plugin) return 0 ;;
        *) return 1 ;;
    esac
}

detect_profiler() {
    if [ "${PROFILER}" != "auto" ]; then
        return 0
    fi
    if command -v perf >/dev/null 2>&1; then
        PROFILER="perf"
    elif command -v gprof >/dev/null 2>&1; then
        PROFILER="gprof"
    else
        PROFILER="time"
    fi
}

ensure_profile_build() {
    if [ "${NO_BUILD}" = "true" ]; then
        return 0
    fi

    log "building profile target in ${PROFILE_BUILD_DIR} (this may take a while)"
    cmake -S . -B "${PROFILE_BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DREQPACK_ENABLE_PROFILING=ON
    cmake --build "${PROFILE_BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 2)" \
        --target ReqPack core_unit_tests core_integration_tests reqpack_test_targets
}

require_binary() {
    local path="$1"
    local label="$2"
    [ -x "${path}" ] || die "missing ${label}: ${path}. Run with --build or make profile-build"
}

cleanup_workspace() {
    if [ "${KEEP_WORKSPACE}" = "true" ] || [ -z "${WORKSPACE}" ]; then
        return 0
    fi
    rm -rf "${WORKSPACE}"
}

setup_hermetic_workspace() {
    WORKSPACE="$(mktemp -d "${TMPDIR:-/tmp}/reqpack-profile-XXXXXX")"
    trap cleanup_workspace EXIT INT TERM HUP

    local plugin_root="${WORKSPACE}/plugins"
    local dnf_plugin="${repo_root}/tests/fixtures/plugin-bundles/dnf"
    local demo_plugin="${repo_root}/tests/system/fixtures/demo-plugin"
    [ -d "${dnf_plugin}" ] || die "dnf plugin fixture missing: ${dnf_plugin}"
    [ -d "${demo_plugin}" ] || die "demo plugin fixture missing: ${demo_plugin}"

    mkdir -p "${plugin_root}/dnf" "${plugin_root}/demo-plugin"
    cp -R "${dnf_plugin}/." "${plugin_root}/dnf/"
    cp -R "${demo_plugin}/." "${plugin_root}/demo-plugin/"

    CONFIG_PATH="$(bash "${repo_root}/.github/scripts/create-system-test-config.sh" "${WORKSPACE}" "${plugin_root}")"

    export HOME="${WORKSPACE}/home"
    export XDG_CACHE_HOME="${WORKSPACE}/xdg-cache"
    export XDG_CONFIG_HOME="${WORKSPACE}/xdg-config"
    export XDG_DATA_HOME="${WORKSPACE}/xdg-data"
    mkdir -p "${HOME}" "${XDG_CACHE_HOME}" "${XDG_CONFIG_HOME}" "${XDG_DATA_HOME}"
}

setup_system_environment() {
    WORKSPACE="(system: ${HOME:-~})"
    CONFIG_PATH=""
    unset GMON_OUT_PREFIX || true
}

prepare_environment_for_scenario() {
    local name="$1"
    if [ "${USE_SYSTEM_ENV}" = "true" ] && ! scenario_uses_hermetic "${name}"; then
        setup_system_environment
        return 0
    fi
    setup_hermetic_workspace
}

rqp_base_args() {
    local -n out=$1
    out=()
    if [ -n "${CONFIG_PATH}" ]; then
        out+=(--config "${CONFIG_PATH}")
    fi
    out+=(--non-interactive)
}

new_run_dir() {
    local slug="$1"
    LAST_RUN_ID="$(date +%Y%m%d-%H%M%S)-${slug}-$$"
    mkdir -p "${PROFILE_DATA_DIR}/${LAST_RUN_ID}"
    printf '%s\n' "${PROFILE_DATA_DIR}/${LAST_RUN_ID}"
}

run_timed_only() {
    local run_dir="$1"
    shift
    local log_path="${run_dir}/command.log"
    local time_path="${run_dir}/time.txt"

    {
        printf '$ '
        printf '%q ' "$@"
        printf '\n\n'
    } > "${log_path}"

    if /usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%MKB' -o "${time_path}" "$@"; then
        printf 'exit=0\n' >> "${log_path}"
    else
        local status=$?
        printf 'exit=%s\n' "${status}" >> "${log_path}"
        cat "${time_path}" >> "${log_path}" || true
        return "${status}"
    fi

    cat "${time_path}" >> "${log_path}"
    LAST_REPORT_TXT="${log_path}"
    log "timing report: ${log_path}"
    cat "${time_path}"
}

run_with_gprof() {
    local run_dir="$1"
    local binary="$2"
    shift 2
    local prefix="${run_dir}/gmon"
    local report_path="${run_dir}/hotspots.gprof.txt"

    GMON_OUT_PREFIX="${prefix}" run_timed_only "${run_dir}" "${binary}" "$@" || true

    shopt -s nullglob
    local gmon_files=("${prefix}"*)
    shopt -u nullglob
    [ "${#gmon_files[@]}" -gt 0 ] || die "gprof produced no GMON output; was the binary built with -DREQPACK_ENABLE_PROFILING=ON?"

    gprof "${binary}" "${gmon_files[@]}" > "${report_path}"
    LAST_REPORT_TXT="${report_path}"
    log "gprof report: ${report_path}"
    "${PYTHON}" "${repo_root}/tests/profile_summary.py" gprof "${report_path}" || true
}

run_with_perf() {
    local run_dir="$1"
    local binary="$2"
    shift 2
    local perf_data="${run_dir}/sample.perf.data"
    local perf_log="${run_dir}/perf-record.log"
    local report_path="${run_dir}/hotspots.perf-report.txt"

    {
        printf '$ perf record -- '
        printf '%q ' "${binary}" "$@"
        printf '\n'
    } > "${run_dir}/command.log"

    perf record --no-inherit --call-graph dwarf \
        --output "${perf_data}" \
        -- "${binary}" "$@" > "${perf_log}" 2>&1 || {
            cat "${perf_log}" >&2
            die "perf record failed"
        }

    perf report --stdio --no-children \
        --sort comm,dso,symbol \
        --percent-limit 0.5 \
        --dsos "$(basename "${binary}")" \
        -i "${perf_data}" > "${report_path}"

    LAST_REPORT_TXT="${report_path}"
    log "perf report: ${report_path}"
    "${PYTHON}" "${repo_root}/tests/profile_summary.py" perf "${report_path}" || true
}

profile_command() {
    local slug="$1"
    local binary="$2"
    shift 2
    local run_dir
    run_dir="$(new_run_dir "${slug}")"

    log "scenario=${slug} profiler=${PROFILER} iterations=${ITERATIONS}"
    log "binary=${binary}"
    log "workspace=${WORKSPACE}"
    log "config=${CONFIG_PATH:-<default user config>}"
    log "output=${run_dir}"

    local iteration=1
    while [ "${iteration}" -le "${ITERATIONS}" ]; do
        if [ "${ITERATIONS}" -gt 1 ]; then
            log "iteration ${iteration}/${ITERATIONS}"
        fi
        case "${PROFILER}" in
            perf)
                run_with_perf "${run_dir}" "${binary}" "$@"
                ;;
            gprof)
                run_with_gprof "${run_dir}" "${binary}" "$@"
                ;;
            time)
                run_timed_only "${run_dir}" "${binary}" "$@"
                ;;
            *)
                die "unsupported profiler: ${PROFILER}"
                ;;
        esac
        iteration=$((iteration + 1))
    done

    printf '\nFull report: %s\n' "${LAST_REPORT_TXT}"
}

run_install_ycallr_cli() {
    local slug="$1"
    local dry_run="$2"
    local base_args=()
    rqp_base_args base_args
    if [ "${dry_run}" = "true" ]; then
        base_args+=(--dry-run)
    fi
    profile_command "${slug}" "${RQP_BIN}" \
        "${base_args[@]}" \
        install ycallr-cli
}

run_scenario() {
    local name="$1"
    prepare_environment_for_scenario "${name}"

    case "${name}" in
        install-ycallr-cli)
            run_install_ycallr_cli "install-ycallr-cli" "false"
            ;;
        install-ycallr-cli-dry-run)
            run_install_ycallr_cli "install-ycallr-cli-dry-run" "true"
            ;;
        help)
            profile_command "help" "${RQP_BIN}" --help
            ;;
        list)
            local base_args=()
            rqp_base_args base_args
            profile_command "list" "${RQP_BIN}" "${base_args[@]}" list dnf
            ;;
        install-dry-run)
            local base_args=()
            rqp_base_args base_args
            base_args+=(--dry-run)
            profile_command "install-dry-run" "${RQP_BIN}" \
                "${base_args[@]}" \
                install dnf curl
            ;;
        test-plugin)
            local base_args=()
            rqp_base_args base_args
            profile_command "test-plugin" "${RQP_BIN}" \
                "${base_args[@]}" \
                test-plugin \
                --plugin "${WORKSPACE}/plugins/demo-plugin" \
                --preset core \
                --report "${WORKSPACE}/plugin-test-report.json"
            ;;
        security-unit)
            require_binary "${CORE_UNIT_BIN}" "core_unit_tests"
            profile_command "security-unit" "${CORE_UNIT_BIN}" "[unit][security]"
            ;;
        execution-integration)
            require_binary "${CORE_INTEGRATION_BIN}" "core_integration_tests"
            profile_command "execution-integration" "${CORE_INTEGRATION_BIN}" "[integration][executor]"
            ;;
        integration-all)
            require_binary "${CORE_INTEGRATION_BIN}" "core_integration_tests"
            profile_command "integration-all" "${CORE_INTEGRATION_BIN}"
            ;;
        custom)
            [ "${INTERACTIVE}" = "true" ] || die "--scenario custom requires interactive mode"
            printf 'Enter rqp arguments (without the rqp binary name): '
            read -r custom_args
            # shellcheck disable=SC2206
            local argv=( ${custom_args} )
            local base_args=()
            rqp_base_args base_args
            profile_command "custom" "${RQP_BIN}" "${base_args[@]}" "${argv[@]}"
            ;;
        *)
            die "unknown scenario: ${name}"
            ;;
    esac
}

print_menu() {
    cat <<EOF

=== ReqPack Profile Ground ===
rqp binary:     ${RQP_BIN}
profiler:       ${PROFILER}
environment:    $( [ "${USE_SYSTEM_ENV}" = "true" ] && printf 'system (your config/plugins)' || printf 'hermetic fixtures' )
report root:    ${PROFILE_DATA_DIR}

 1) install ycallr-cli                 (real install, your setup)
 2) install ycallr-cli --dry-run
 3) custom rqp command
 4) help / startup baseline
 5) security unit tests
 6) executor integration tests
 7) hermetic: install dnf curl dry-run
 8) hermetic: test-plugin demo preset
 9) repeat last scenario
 r) change profiler (current: ${PROFILER})
 s) toggle environment (current: $( [ "${USE_SYSTEM_ENV}" = "true" ] && echo system || echo hermetic ))
 b) rebuild profile binaries
 q) quit

EOF
}

interactive_loop() {
    local last_scenario=""
    while true; do
        print_menu
        printf 'Choice: '
        read -r choice
        case "${choice}" in
            1) last_scenario="install-ycallr-cli"; run_scenario install-ycallr-cli ;;
            2) last_scenario="install-ycallr-cli-dry-run"; run_scenario install-ycallr-cli-dry-run ;;
            3) run_scenario custom; last_scenario="custom" ;;
            4) last_scenario="help"; run_scenario help ;;
            5) last_scenario="security-unit"; run_scenario security-unit ;;
            6) last_scenario="execution-integration"; run_scenario execution-integration ;;
            7) USE_SYSTEM_ENV="false"; last_scenario="install-dry-run"; run_scenario install-dry-run ;;
            8) USE_SYSTEM_ENV="false"; last_scenario="test-plugin"; run_scenario test-plugin ;;
            9)
                [ -n "${last_scenario}" ] || { log "nothing to repeat yet"; continue; }
                [ "${last_scenario}" = "custom" ] && { log "repeat is not supported for custom commands"; continue; }
                run_scenario "${last_scenario}"
                ;;
            r)
                printf 'Profiler (auto|perf|gprof|time): '
                read -r selected
                PROFILER="${selected:-${PROFILER}}"
                detect_profiler
                ;;
            s)
                if [ "${USE_SYSTEM_ENV}" = "true" ]; then
                    USE_SYSTEM_ENV="false"
                else
                    USE_SYSTEM_ENV="true"
                fi
                log "environment: $( [ "${USE_SYSTEM_ENV}" = "true" ] && echo system || echo hermetic )"
                ;;
            b)
                NO_BUILD="false"
                ensure_profile_build
                require_binary "${RQP_BIN}" "profiled rqp"
                ;;
            q|Q) break ;;
            *) log "unknown choice: ${choice}" ;;
        esac
    done
}

main() {
    parse_args "$@"
    detect_profiler

    if [ "${NO_BUILD}" = "false" ]; then
        ensure_profile_build
    fi

    require_binary "${RQP_BIN}" "profiled rqp"
    mkdir -p "${PROFILE_DATA_DIR}"

    if [ "${INTERACTIVE}" = "true" ]; then
        log "profiler backend: ${PROFILER}"
        interactive_loop
        return 0
    fi

    [ -n "${SCENARIO}" ] || die "non-interactive mode requires --scenario"
    run_scenario "${SCENARIO}"
}

main "$@"
