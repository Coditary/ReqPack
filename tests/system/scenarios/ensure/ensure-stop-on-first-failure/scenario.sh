#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_ensure_fixture
    write_fake_pm "quillctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  install)
    printf "quill install %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    exit 1
    ;;
  update)
    printf "quill update %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    ;;
  update-all)
    printf "quill update-all\n" >> "${CALLS_DIR}/quill.log"
    ;;
  remove)
    printf "quill remove %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    ;;
  *)
    printf "unknown quillctl command: %s\n" "${cmd}" >&2
    exit 1
    ;;
esac
'
}

scenario_command() {
    printf 'cd %q && rqp --config %q ensure moss anvil --stop-on-first-failure' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected ensure stop-on-first-failure to fail\n' >&2
        exit 1
    fi
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/glyph.log"
}
