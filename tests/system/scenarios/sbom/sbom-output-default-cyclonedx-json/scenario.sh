#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --output sbom.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom default file output exits zero"
    assert_output_contains "${WORKSPACE_DIR}/sbom.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/sbom.json"
    assert_output_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/sbom.json"
    assert_output_contains '"components"' "${WORKSPACE_DIR}/sbom.json"
    assert_output_contains '"name": "zeno"' "${WORKSPACE_DIR}/sbom.json"
}
