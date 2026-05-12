#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_plugin_bundle "${PLUGIN_DIR}" "amber" "$(base_quill_plugin)"
    write_fake_pm "quillctl" '
cmd="$1"
shift || true
mkdir -p "${CALLS_DIR}"
case "${cmd}" in
  update)
    printf "quill update %s\n" "$*" >> "${CALLS_DIR}/quill.log"
    exit 1
    ;;
esac
'
}

scenario_command() {
    printf 'cd %q && rqp --config %q update amber:zeno moss:luma --stop-on-first-failure' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    status="$(read_status)"
    if [ "${status}" = "0" ]; then
        printf 'expected stop-on-first-failure update to fail\n' >&2
        exit 1
    fi
    assert_file_missing "${WORKSPACE_COPY_DIR}/calls/moss.log"
}
