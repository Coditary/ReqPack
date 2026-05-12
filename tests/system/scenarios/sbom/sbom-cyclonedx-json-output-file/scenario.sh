#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno --format cyclonedx-json --output sbom.cdx.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom explicit cyclonedx output exits zero"
    assert_output_contains "${WORKSPACE_DIR}/sbom.cdx.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/sbom.cdx.json"
    assert_output_contains '"bomFormat": "CycloneDX"' "${WORKSPACE_DIR}/sbom.cdx.json"
    assert_output_contains '"components"' "${WORKSPACE_DIR}/sbom.cdx.json"
}
