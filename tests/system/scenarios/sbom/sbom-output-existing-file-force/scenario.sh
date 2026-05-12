#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
    write_text "${WORKSPACE_DIR}/sbom.json" "keep-me\n"
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --output sbom.json --force' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom output force overwrite exits zero"
    assert_output_contains "${WORKSPACE_DIR}/sbom.json" "${STDOUT_PATH}"
    assert_output_not_contains "Overwrite? [y/N]" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/sbom.json"
    assert_output_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/sbom.json"
    assert_output_not_contains "keep-me" "${WORKSPACE_DIR}/sbom.json"
}
