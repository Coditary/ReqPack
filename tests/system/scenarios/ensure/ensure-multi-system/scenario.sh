#!/usr/bin/env bash

scenario_prepare() {
    base_prepare_ensure_fixture
}

scenario_command() {
    printf 'cd %q && rqp --config %q ensure moss anvil' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "ensure multi system exits zero"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/quill.log"
    assert_file_exists "${WORKSPACE_COPY_DIR}/calls/glyph.log"
    assert_output_contains "ENSURE:" "${STDOUT_PATH}"
    assert_output_contains "quill:zeno" "${STDOUT_PATH}"
    assert_output_contains "glyph:fable" "${STDOUT_PATH}"
}
