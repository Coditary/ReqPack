#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_fake_pm "mossctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "start moss %s\n" "$*" >> "${CALLS_DIR}/parallel.log"
    sleep 1
    printf "end moss %s\n" "$*" >> "${CALLS_DIR}/parallel.log"
    ;;
  install-local)
    printf "moss local %s\n" "$*" >> "${CALLS_DIR}/moss.log"
    ;;
esac
'
    write_fake_pm "quillctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "start quill %s\n" "$*" >> "${CALLS_DIR}/parallel.log"
    sleep 1
    printf "end quill %s\n" "$*" >> "${CALLS_DIR}/parallel.log"
    ;;
esac
'
}

scenario_command() {
    printf 'cd %q && rqp --config %q install quill:zeno moss:luma --jobs 2' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "jobs 2 exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/parallel.log"
    assert_output_contains "INSTALL:" "${STDOUT_PATH}"
    assert_output_contains "quill:zeno" "${STDOUT_PATH}"
    assert_output_contains "moss:luma" "${STDOUT_PATH}"
}
